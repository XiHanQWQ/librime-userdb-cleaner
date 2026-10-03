#include "userdb_clean_job.h"

#include <rime/common.h>
#include <rime/deployer.h>
#include <rime/dict/user_db.h>
#include <rime/dict/user_dictionary.h>
#include <rime/registry.h>
#include <rime/service.h>
#include <rime/setup.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace rime {

namespace {

constexpr const char kUserdbDirSuffix[] = ".userdb";
constexpr const char kSnapshotSuffix[] = ".userdb.txt";
constexpr const char kSnapshotTempSuffix[] = ".userdb.txt.tmp";
constexpr const char kBackupSuffix[] = ".userdb_backup.txt";
constexpr const char kCleanLogFileName[] = "userdb_cleaner.txt";
constexpr const char kRolledLogSuffix[] = ".1";
constexpr const char kLockFileName[] = ".userdb_cleaner.lock";
constexpr const char kLevelDbClass[] = "userdb";
constexpr const char kPlainUserdbClass[] = "plain_userdb";

// 日志只增不减，超过这个体积就轮转为 userdb_cleaner.txt.1。
constexpr std::uintmax_t kMaxLogBytes = 2 * 1024 * 1024;
// 锁文件超过这个秒数视为陈锁（持有者崩了没人删）。
constexpr int kLockStaleSeconds = 600;

bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// 仅当剥离后仍有内容时返回名称；否则返回 nullopt。
std::optional<string> StripSuffix(std::string_view text,
                                  std::string_view suffix) {
  if (text.size() <= suffix.size() || !EndsWith(text, suffix))
    return std::nullopt;
  return string(text.substr(0, text.size() - suffix.size()));
}

string LogTimestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buffer[24] = {};
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm) == 0)
    return {};
  return buffer;
}

// UserDbValue::Pack() 的字段（src/rime/dict/user_db.cc:22）：
//   "c=2 d=0.744532 t=41664"
struct DbRowValue {
  int commits = 0;
  double dee = 0.0;
  uint64_t tick = 0;
  bool has_commits = false;
  bool has_dee = false;
  bool has_tick = false;
};

DbRowValue ParseDbRowValue(std::string_view value) {
  DbRowValue row;
  size_t pos = 0;
  while (pos < value.size()) {
    size_t end = value.find(' ', pos);
    if (end == std::string_view::npos)
      end = value.size();
    const std::string_view token = value.substr(pos, end - pos);
    const size_t eq = token.find('=');
    if (eq != std::string_view::npos && eq > 0) {
      const std::string_view key = token.substr(0, eq);
      const std::string_view text = token.substr(eq + 1);
      const char *first = text.data();
      const char *last = text.data() + text.size();
      if (key == "c") {
        int commits = 0;
        const auto result = std::from_chars(first, last, commits);
        if (result.ec == std::errc() && result.ptr == last) {
          row.commits = commits;
          row.has_commits = true;
        }
      } else if (key == "t") {
        unsigned long long tick = 0;
        const auto result = std::from_chars(first, last, tick);
        if (result.ec == std::errc() && result.ptr == last) {
          row.tick = static_cast<uint64_t>(tick);
          row.has_tick = true;
        }
      } else if (key == "d") {
        // 浮点的 from_chars 在部分标准库上还没有，用 strtod。
        const string text_str(text);
        char *str_end = nullptr;
        const double dee = std::strtod(text_str.c_str(), &str_end);
        if (str_end != text_str.c_str() &&
            str_end == text_str.c_str() + text_str.size()) {
          row.dee = dee;
          row.has_dee = true;
        }
      }
    }
    pos = end + 1;
  }
  return row;
}

// 直接借 librime 自己的可见性判定：UserDictionary::CreateDictEntry 里做了
// 完整判断——commits < 0、tick == 0 豁免衰减、衰减后 dee <= kDiscardThreshold
// 等等（src/rime/dict/user_dictionary.cc）。返回空就说明这一行输入法永远
// 不会再给出。插件因此不需要抄一份 1e-200 或衰减公式，librime 以后调整
// 判据也自动跟上。
bool IsInvisibleToLibrime(std::string_view key, std::string_view value,
                          uint64_t present_tick) {
  return !UserDictionary::CreateDictEntry(string(key), string(value),
                                          present_tick, 1.0, 0.0, nullptr);
}

enum class RowAction {
  keep,
  remove,
  // t = 0 的短语行（table/stabledb，如 custom_phrase）。
  skip_phrase,
  // 解析不出 c/d/t 的行：保守保留，但要让调用方报出来。
  unparsed,
};

// 一行的处置。threshold 与 present_tick 对本地 .userdb 和同步快照必须用同一套
// 判据，否则本地删了、快照还留着，下一轮同步又会把它合并回来。
RowAction ClassifyRow(std::string_view key, std::string_view value,
                      int threshold, uint64_t present_tick) {
  const DbRowValue row = ParseDbRowValue(value);
  // c/d/t 缺一个就不敢按 librime 的判据下结论（它的 Unpack 同样会失败），
  // 保守保留并计数上报。
  if (!row.has_commits || !row.has_dee || !row.has_tick)
    return RowAction::unparsed;
  // tick == 0 是从 table/stabledb 导入的短语（如 custom_phrase）：
  // librime 对它们从不做衰减淘汰，这里也一律不动。
  if (row.tick == 0)
    return RowAction::skip_phrase;
  if (row.commits < threshold)
    return RowAction::remove;
  // c 看不出"用不用"时，用 librime 自己的判定兜底：它认为已经不可见的行
  // （c 可能还 >= threshold）同样清掉。
  if (IsInvisibleToLibrime(key, value, present_tick))
    return RowAction::remove;
  return RowAction::keep;
}

// 快照行 key 与 value 之间用制表符分隔，词条名在第一个制表符之后。
string WordFromKey(std::string_view key) {
  const size_t tab = key.find('\t');
  return tab == std::string_view::npos ? string(key) : string(key.substr(tab + 1));
}

// 去重：同一个词可能先被本地物理删、又被快照清理删一次（本机快照里那份）。
void RecordDeletedWord(UserdbCleanStats *stats, string word) {
  if (stats->recorded_words.insert(word).second)
    stats->deleted_words.push_back(std::move(word));
}

// 黑名单模式：exclude_userdb_list 中的词典会被跳过，其余全部清理。
// 列表为空时表示清理所有词典。
bool ShouldClean(const string &dict_name, const UserdbCleanOptions &options) {
  if (options.exclude_userdb_list.empty())
    return true;
  return std::find(options.exclude_userdb_list.begin(),
                   options.exclude_userdb_list.end(),
                   dict_name) == options.exclude_userdb_list.end();
}

struct LocalUserdb {
  string name;
  string db_class;
};

// 本地词典有两种落盘形式（src/rime/dict/dict_module.cc:30-31）：
//   userdb       → <name>.userdb 目录（LevelDb，默认）
//   plain_userdb → <name>.userdb.txt 文件（TextDb）
vector<LocalUserdb> FindLocalUserdbs(const path &user_data_dir) {
  vector<LocalUserdb> dbs;
  std::error_code ec;
  if (user_data_dir.empty() ||
      !std::filesystem::is_directory(user_data_dir, ec))
    return dbs;
  std::filesystem::directory_iterator it(user_data_dir, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    const path file(it->path());
    const string filename = file.filename().to_utf8_string();
    std::error_code dir_ec;
    std::error_code file_ec;
    if (it->is_directory(dir_ec) && !dir_ec) {
      if (auto base = StripSuffix(filename, kUserdbDirSuffix))
        dbs.push_back({std::move(*base), kLevelDbClass});
    } else if (it->is_regular_file(file_ec) && !file_ec) {
      if (auto base = StripSuffix(filename, kSnapshotSuffix))
        dbs.push_back({std::move(*base), kPlainUserdbClass});
    }
  }
  std::sort(dbs.begin(), dbs.end(),
            [](const LocalUserdb &lhs, const LocalUserdb &rhs) {
              return lhs.name < rhs.name;
            });
  return dbs;
}

path ResolveSyncDir() {
  const Deployer &deployer = Service::instance().deployer();
  if (!deployer.sync_dir.empty())
    return deployer.sync_dir;
  return deployer.user_data_dir / "sync";
}

// 进程内同步：合并 sync 目录中其他设备的快照 + 导出本机词典。
// IME 进程默认只加载 kDefaultModules（core/dict/gears），
// 而 user_dict_sync 任务注册在 levers 模块里，需先补齐加载。
bool RunUserDictSync() {
  LoadModules(kDeployerModules);
  // 活动会话的 UserDictionary 强持有 .userdb 句柄，而 UserDictManager
  // 会另建句柄去 Open——Windows 下 LevelDb LOCK 互斥，不清会话则同步
  // 必然失败（日志表现为 "LOCK: 另一个程序正在使用此文件"）。
  // 与官方 RimeSyncUserData 一致：先关闭所有会话，前端随后自动重建。
  Service::instance().CleanupAllSessions();
  Deployer &deployer = Service::instance().deployer();
  // installation_update 负责读取 installation.yaml 恢复 user_id，
  // 否则快照会写到 sync_dir 根目录，与既有安装目录对不上。
  const bool ok_install = deployer.RunTask("installation_update");
  const bool ok_sync = deployer.RunTask("user_dict_sync");
  if (!ok_install || !ok_sync) {
    LOG(ERROR) << "userdb cleaner: sync failed (installation_update="
               << (ok_install ? "ok" : "failed") << ", user_dict_sync="
               << (ok_sync ? "ok" : "failed") << ").";
  }
  return ok_install && ok_sync;
}

// 打开本地词典。活动会话强持有 .userdb 句柄（LevelDb 的 LOCK 文件互斥），
// 打不开时再关闭所有会话重试一次——不然每轮清理都要白打断一次输入。
bool OpenUserdb(an<Db> &db) {
  if (db->Open())
    return true;
  Service::instance().CleanupAllSessions();
  return db->Open();
}

bool ReadTick(Db *db, uint64_t *tick) {
  string value;
  if (!db->MetaFetch("/tick", &value))
    return false;
  unsigned long long parsed = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc() || result.ptr != value.data() + value.size())
    return false;
  *tick = static_cast<uint64_t>(parsed);
  return true;
}

// 流程第③步：物理删除本地词典里的无用词条，并把每个词典的 /tick 记进
// tick_map，供第④步清理快照时用同一个 present_tick 判定。
//
// librime 的「删除」只是把 c 写成负值（墓碑行），它靠墓碑行跨设备传播删除；
// 但同步是全量导出（UniformBackup 原样遍历本地 LevelDb），墓碑行会被原封
// 写进 *.userdb.txt，于是清理完快照后下一次同步又把它导回来——每次都重复
// 上报同一批词、文件里却始终还在。要真正删干净只能本地物理 Erase：
// 本地没墓碑 → 导不出 → 快照没有 → 只报一次。
// 代价：没有墓碑行后，其他设备同步来的同名词条会复活（下次再删）；
// 跨设备的删除传播失效。
void CleanLiveUserdbs(const UserdbCleanOptions &options,
                      UserdbCleanStats *stats,
                      std::map<string, uint64_t> *tick_map) {
  const Deployer &deployer = Service::instance().deployer();
  for (const LocalUserdb &entry : FindLocalUserdbs(deployer.user_data_dir)) {
    if (!ShouldClean(entry.name, options))
      continue;
    UserDb::Component *component = UserDb::Require(entry.db_class);
    if (!component) {
      LOG(ERROR) << "userdb cleaner: db class '" << entry.db_class
                 << "' is unavailable.";
      continue;
    }
    an<Db> db(component->Create(entry.name));
    if (!db) {
      LOG(ERROR) << "userdb cleaner: cannot create user db '" << entry.name
                 << "'.";
      continue;
    }
    if (!OpenUserdb(db)) {
      LOG(WARNING) << "userdb cleaner: cannot open user db '" << entry.name
                   << "'.";
      continue;
    }
    uint64_t present_tick = 0;
    const bool have_tick = ReadTick(db.get(), &present_tick);
    if (have_tick)
      (*tick_map)[entry.name] = present_tick;
    // 先收集再删除：QueryAll 的游标遍历期间不改动 db。
    vector<string> doomed_keys;
    vector<string> doomed_words;
    {
      an<DbAccessor> accessor = db->QueryAll();
      string key, value;
      while (accessor && accessor->GetNextRecord(&key, &value)) {
        switch (ClassifyRow(key, value, options.clean_threshold,
                            present_tick)) {
          case RowAction::remove:
            doomed_keys.push_back(key);
            doomed_words.push_back(WordFromKey(key));
            break;
          case RowAction::skip_phrase:
            ++stats->skipped_phrase_rows;
            break;
          case RowAction::unparsed:
            ++stats->unparsed_rows;
            break;
          case RowAction::keep:
            break;
        }
      }
    }
    if (doomed_keys.empty()) {
      db->Close();
      continue;
    }
    int removed = 0;
    for (size_t i = 0; i < doomed_keys.size(); ++i) {
      const string &key = doomed_keys[i];
      if (db->Erase(key)) {
        ++removed;
        // 日志里逐条打印删掉的词条（连同完整 key：码 + 词），方便核对。
        LOG(INFO) << "userdb cleaner: erased '" << doomed_words[i] << "' from '"
                  << entry.name << "' (" << key << ").";
      } else {
        LOG(WARNING) << "userdb cleaner: cannot erase '" << doomed_words[i]
                     << "' from '" << entry.name << "' (" << key << ").";
      }
    }
    db->Close();
    if (removed == 0)
      continue;
    stats->dict_entries_removed += removed;
    stats->deleted_dicts.push_back(entry.name);
    for (string &word : doomed_words)
      RecordDeletedWord(stats, std::move(word));
    LOG(INFO) << "userdb cleaner: erased " << removed << " entries from '"
              << entry.name << "'.";
  }
}

// 快照头部 "#@/tick\t41822"。
std::optional<uint64_t> ParseSnapshotTick(std::string_view line) {
  constexpr std::string_view kPrefix = "#@/tick";
  if (line.size() <= kPrefix.size() ||
      line.compare(0, kPrefix.size(), kPrefix) != 0)
    return std::nullopt;
  size_t pos = kPrefix.size();
  while (pos < line.size() && (line[pos] == '\t' || line[pos] == ' '))
    ++pos;
  unsigned long long tick = 0;
  const char *first = line.data() + pos;
  const char *last = line.data() + line.size();
  const auto result = std::from_chars(first, last, tick);
  if (result.ec != std::errc())
    return std::nullopt;
  return static_cast<uint64_t>(tick);
}

RowAction ClassifySnapshotLine(std::string_view raw_line, int threshold,
                               uint64_t present_tick, string *word) {
  std::string_view line = raw_line;
  if (!line.empty() && line.back() == '\r')
    line.remove_suffix(1);
  // 空行与 "#" 开头的是文件头/元数据，一律保留。
  if (line.empty() || line[0] == '#')
    return RowAction::keep;
  const size_t first_tab = line.find('\t');
  if (first_tab == std::string_view::npos)
    return RowAction::keep;
  const size_t second_tab = line.find('\t', first_tab + 1);
  if (second_tab == std::string_view::npos)
    return RowAction::keep;
  const RowAction action = ClassifyRow(line.substr(0, second_tab),
                                       line.substr(second_tab + 1), threshold,
                                       present_tick);
  if (action == RowAction::remove && word)
    *word = string(line.substr(first_tab + 1, second_tab - first_tab - 1));
  return action;
}

// 流程第④步：清理一份快照文件。local_tick / has_local_tick 是本机该词典的
// /tick：两边必须用同一个 present_tick 判定，否则本地删了、快照还留着。
// 没有本机 tick（词典打不开或没做过同步）时退回文件头里的 #@/tick。
void CleanSnapshotFile(const path &file, const UserdbCleanOptions &options,
                       uint64_t local_tick, bool has_local_tick,
                       UserdbCleanStats *stats) {
  // MinGW 上 rime::path 不是 std::filesystem::path，统一走 UTF-8 字符串。
  std::ifstream in(file.to_utf8_string(), std::ios::binary);
  if (!in.is_open()) {
    LOG(ERROR) << "userdb cleaner: cannot open snapshot file: " << file;
    return;
  }

  const string filename = file.filename().to_utf8_string();
  // 边读边写到临时文件，避免把整个快照缓冲在内存里。
  path temp(file);
  temp += path(kSnapshotTempSuffix);
  std::ofstream out(temp.to_utf8_string(), std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    LOG(ERROR) << "userdb cleaner: cannot write " << temp;
    return;
  }

  uint64_t present_tick = local_tick;
  bool have_present = has_local_tick;
  vector<string> removed_words;
  string line;
  int removed = 0;
  while (std::getline(in, line)) {
    if (!have_present) {
      if (auto tick = ParseSnapshotTick(line)) {
        present_tick = *tick;
        have_present = true;
      }
    }
    string word;
    switch (ClassifySnapshotLine(line, options.clean_threshold, present_tick,
                                 &word)) {
      case RowAction::remove:
        ++removed;
        // 逐条打印删掉的行（key + value），方便核对。
        LOG(INFO) << "userdb cleaner: removed row '" << line << "' from '"
                  << filename << "'.";
        removed_words.push_back(std::move(word));
        break;
      case RowAction::skip_phrase:
        ++stats->skipped_phrase_rows;
        out << line << '\n';
        break;
      case RowAction::unparsed:
        ++stats->unparsed_rows;
        out << line << '\n';
        break;
      case RowAction::keep:
        out << line << '\n';
        break;
    }
  }
  in.close();

  out.flush();
  const bool write_failed = !out;
  out.close();

  std::error_code ec;
  if (write_failed) {
    LOG(ERROR) << "userdb cleaner: failed while writing " << temp;
    std::filesystem::remove(temp, ec);
    return;
  }
  if (removed == 0) {
    std::filesystem::remove(temp, ec);
    return;
  }

  // 备份由流程第②步完成（只备份本机那份），这里直接原子替换。
  std::filesystem::rename(temp, file, ec);
  if (ec) {
    LOG(ERROR) << "userdb cleaner: cannot replace " << file << ": "
               << ec.message();
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    return;
  }

  stats->snapshot_rows_removed += removed;
  stats->deleted_files.push_back(filename);
  for (string &word : removed_words)
    RecordDeletedWord(stats, std::move(word));
  LOG(INFO) << "userdb cleaner: removed " << removed << " rows from '"
            << filename << "'.";
}

void CleanSnapshots(const path &sync_dir, const UserdbCleanOptions &options,
                    const std::map<string, uint64_t> &tick_map,
                    UserdbCleanStats *stats) {
  std::error_code ec;
  if (sync_dir.empty() || !std::filesystem::is_directory(sync_dir, ec)) {
    LOG(INFO) << "userdb cleaner: sync directory not found: " << sync_dir;
    return;
  }
  std::filesystem::recursive_directory_iterator it(
      sync_dir, std::filesystem::directory_options::skip_permission_denied, ec),
      end;
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code entry_ec;
    if (!it->is_regular_file(entry_ec) || entry_ec)
      continue;
    const path file(it->path());
    const string filename = file.filename().to_utf8_string();
    // 上次崩溃/中断留下的临时文件，顺手清掉（云盘同步目录里最容易堆积）。
    if (EndsWith(filename, kSnapshotTempSuffix)) {
      std::error_code remove_ec;
      std::filesystem::remove(file, remove_ec);
      LOG(INFO) << "userdb cleaner: removed stale temp file '" << filename
                << "'.";
      continue;
    }
    auto dict_name = StripSuffix(filename, kSnapshotSuffix);
    if (!dict_name || !ShouldClean(*dict_name, options))
      continue;
    ++stats->snapshots_processed;
    uint64_t local_tick = 0;
    bool has_local_tick = false;
    if (auto found = tick_map.find(*dict_name); found != tick_map.end()) {
      local_tick = found->second;
      has_local_tick = true;
    }
    CleanSnapshotFile(file, options, local_tick, has_local_tick, stats);
  }
}

// 流程第②步：把本机那份快照复制一份为 *.userdb_backup.txt（同名覆盖旧备份）。
// 只复制、不移动：本机快照留在原地，收尾同步还要用；
// 也只备份本机设备目录——其他设备的快照由它们自己备份，不往人家目录里写文件。
void BackupSnapshots(const path &own_sync_dir,
                     const UserdbCleanOptions &options,
                     UserdbCleanStats *stats) {
  std::error_code ec;
  if (own_sync_dir.empty() ||
      !std::filesystem::is_directory(own_sync_dir, ec)) {
    LOG(INFO) << "userdb cleaner: own sync directory not found: "
              << own_sync_dir;
    return;
  }
  std::filesystem::directory_iterator it(
      own_sync_dir, std::filesystem::directory_options::skip_permission_denied,
      ec),
      end;
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code entry_ec;
    if (!it->is_regular_file(entry_ec) || entry_ec)
      continue;
    const path file(it->path());
    const string filename = file.filename().to_utf8_string();
    auto dict_name = StripSuffix(filename, kSnapshotSuffix);
    if (!dict_name || !ShouldClean(*dict_name, options))
      continue;
    const path backup = file.parent_path() / path(*dict_name + kBackupSuffix);
    std::error_code copy_ec;
    std::filesystem::copy_file(
        file, backup, std::filesystem::copy_options::overwrite_existing,
        copy_ec);
    if (copy_ec) {
      LOG(WARNING) << "userdb cleaner: cannot back up '" << filename
                   << "': " << copy_ec.message();
      continue;
    }
    ++stats->snapshots_backed_up;
    LOG(INFO) << "userdb cleaner: backed up snapshot '" << filename << "'.";
  }
}

// 日志只增不减：超过 kMaxLogBytes 就轮转一代为 userdb_cleaner.txt.1。
void RotateCleanLog(const path &log_file) {
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(log_file, ec);
  if (ec || size <= kMaxLogBytes)
    return;
  path rolled(log_file);
  rolled += path(kRolledLogSuffix);
  std::filesystem::remove(rolled, ec);
  std::filesystem::rename(log_file, rolled, ec);
}

void AppendCleanLog(const path &sync_dir, const UserdbCleanOptions &options,
                    const UserdbCleanStats &stats) {
  const bool nothing_changed = stats.dict_entries_removed == 0 &&
                               stats.snapshot_rows_removed == 0 &&
                               stats.snapshots_backed_up == 0;
  // 什么都没动、且两次同步都成功就不用记一笔。
  if (nothing_changed && stats.pre_sync_ok && stats.post_sync_ok)
    return;
  std::error_code ec;
  if (sync_dir.empty() || !std::filesystem::is_directory(sync_dir, ec))
    return;
  const path log_file = sync_dir / path(kCleanLogFileName);
  RotateCleanLog(log_file);
  std::ofstream out(log_file.to_utf8_string(), std::ios::app);
  if (!out.is_open()) {
    LOG(ERROR) << "userdb cleaner: cannot open log file: " << log_file;
    return;
  }
  out << LogTimestamp() << " Removed " << stats.dict_entries_removed
      << " entries from " << stats.deleted_dicts.size() << " userdb(s), "
      << stats.snapshot_rows_removed << " rows from "
      << stats.deleted_files.size() << " snapshot file(s), backed up "
      << stats.snapshots_backed_up << " snapshot(s), skipped "
      << stats.skipped_phrase_rows << " phrase row(s) / "
      << stats.unparsed_rows << " unparsed row(s); threshold = "
      << options.clean_threshold << "; elapsed = " << stats.elapsed.count()
      << " ms; pre-sync " << (stats.pre_sync_ok ? "ok" : "FAILED")
      << ", post-sync " << (stats.post_sync_ok ? "ok" : "FAILED") << ":";
  size_t column = 0;
  for (const string &word : stats.deleted_words) {
    if (column++ % 10 == 0)
      out << "\n  ";
    out << "[ " << word << " ]";
  }
  out << "\n\n";
}

// 删锁用的 RAII 壳子。不能把析构放进返回锁路径的函数里：那样临时对象一析构
// 就把刚拿到的锁删了。
struct LockGuard {
  path file;
  ~LockGuard() {
    if (file.empty())
      return;
    std::error_code ec;
    std::filesystem::remove(file, ec);
  }
};

bool LockIsStale(const path &lock_file) {
  std::error_code ec;
  const auto last = std::filesystem::last_write_time(lock_file, ec);
  if (ec)
    return false;  // 读不到时间就当它还有效，别抢别人的锁
  return std::filesystem::file_time_type::clock::now() - last >
         std::chrono::seconds(kLockStaleSeconds);
}

// 尽力而为的运行锁：同一个 sync 目录同时只跑一个清理任务。
// 云盘同步有延迟、mtime 也不一定保留，所以跨设备并不能真正互斥，只是尽量
// 避免两端同时改写同一批快照。拿不到锁时返回空 path。
path AcquireCleanLock(const path &sync_dir) {
  std::error_code ec;
  if (sync_dir.empty() || !std::filesystem::is_directory(sync_dir, ec))
    return {};
  const path lock_file = sync_dir / path(kLockFileName);
  if (std::filesystem::exists(lock_file, ec)) {
    if (!LockIsStale(lock_file)) {
      LOG(WARNING) << "userdb cleaner: another clean run holds " << lock_file;
      return {};
    }
    LOG(WARNING) << "userdb cleaner: removing stale lock " << lock_file;
    std::filesystem::remove(lock_file, ec);
  }
  std::ofstream out(lock_file.to_utf8_string(), std::ios::trunc);
  if (!out.is_open()) {
    // 写不了锁文件就不锁了，不影响清理本身。
    LOG(WARNING) << "userdb cleaner: cannot write lock file " << lock_file;
    return {};
  }
  out << LogTimestamp() << " userdb_cleaner lock\n";
  out.close();
  return lock_file;
}

} // namespace

UserdbCleanStats RunUserdbClean(const UserdbCleanOptions &options) {
  const auto started = std::chrono::steady_clock::now();
  UserdbCleanStats stats;
  UserdbCleanOptions opts = options;
  // 统一把列表项里可能带上的后缀剥掉。
  for (string &name : opts.exclude_userdb_list) {
    if (auto stripped = StripSuffix(name, kSnapshotSuffix))
      name = std::move(*stripped);
    if (auto stripped = StripSuffix(name, kUserdbDirSuffix))
      name = std::move(*stripped);
  }

  // 拿运行锁，拿不到就跳过这一轮。
  LockGuard lock;
  lock.file = AcquireCleanLock(ResolveSyncDir());
  if (lock.file.empty()) {
    stats.lock_failed = true;
    stats.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return stats;
  }

  // 顺序：先把各设备的词条同步合并进来，再两边一起清（本地 .userdb +
  // sync 下所有 *.userdb.txt），最后导出一份干净的快照收尾。
  // ① 清理前同步：合并 sync 下所有设备目录的快照 → ② 备份本机快照（复制）
  //  → ③ 物理删本地无用词条 → ④ 清理所有 *.userdb.txt 的残留行
  //  → ⑤ 清理后同步：再导出一次收尾
  stats.pre_sync_ok = RunUserDictSync();
  // sync_dir / user_id 要等 installation_update 跑过才可信：
  // 进程刚起来时 Deployer 里这两个字段可能还是空的。
  const path sync_dir = ResolveSyncDir();
  BackupSnapshots(Service::instance().deployer().user_data_sync_dir(), opts,
                  &stats);
  std::map<string, uint64_t> tick_map;
  CleanLiveUserdbs(opts, &stats, &tick_map);
  CleanSnapshots(sync_dir, opts, tick_map, &stats);
  stats.post_sync_ok = RunUserDictSync();

  stats.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  AppendCleanLog(sync_dir, opts, stats);
  LOG(INFO) << "userdb cleaner: scanned " << stats.snapshots_processed
            << " snapshot file(s), backed up " << stats.snapshots_backed_up
            << " snapshot(s), removed " << stats.dict_entries_removed
            << " entries from " << stats.deleted_dicts.size()
            << " userdb(s) and " << stats.snapshot_rows_removed
            << " snapshot row(s), skipped " << stats.skipped_phrase_rows
            << " phrase row(s) / " << stats.unparsed_rows
            << " unparsed row(s) in " << stats.elapsed.count()
            << " ms; pre-sync " << (stats.pre_sync_ok ? "ok" : "failed")
            << ", post-sync " << (stats.post_sync_ok ? "ok" : "failed") << ".";
  return stats;
}

} // namespace rime
