#include "userdb_clean_job.h"
#include "userdb_notify.h"

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
#include <thread>
#include <utility>
#include <vector>

namespace rime {

namespace {

constexpr const char kUserdbDirSuffix[] = ".userdb";
constexpr const char kSnapshotSuffix[] = ".userdb.txt";
constexpr const char kSnapshotTempSuffix[] = ".userdb.txt.tmp";
constexpr const char kBackupSuffix[] = ".userdb_backup.txt";
constexpr const char kCleanLogFileName[] = "userdb_cleaner.txt";
constexpr const char kLockFileName[] = ".userdb_cleaner.lock";
constexpr const char kLevelDbClass[] = "userdb";
constexpr const char kPlainUserdbClass[] = "plain_userdb";

// 锁文件超过该秒数未被更新即视为陈锁（持有方异常退出且未清理）。
constexpr int kLockStaleSeconds = 600;

bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// 剥离后缀；要求剥离后仍有剩余内容，否则返回 nullopt。
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

// 对应 UserDbValue::Pack() 的字段格式（src/rime/dict/user_db.cc:22）：
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
        // 部分标准库尚未提供浮点数版本的 from_chars，此处改用 strtod。
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

// 复用 librime 自身的可见性判定：UserDictionary::CreateDictEntry 内部完成了
// 完整判断——commits < 0（删除标记）、tick == 0 豁免淘汰判定、tick != 0 且
// 衰减后 dee <= kDiscardThreshold 等（见 src/rime/dict/user_dictionary.cc），
// 返回空即表示该行不会再作为候选输出。插件因此无需复刻其阈值常量与衰减
// 公式，librime 后续调整判据时本插件的判定行为自动保持一致。
bool IsInvisibleToLibrime(std::string_view key, std::string_view value,
                          uint64_t present_tick) {
  return !UserDictionary::CreateDictEntry(string(key), string(value),
                                          present_tick, 1.0, 0.0, nullptr);
}

enum class RowAction {
  keep,
  remove,
  // t = 0 的短语行（源自 table/stabledb，如 custom_phrase）。
  skip_phrase,
  // 无法解析出 c/d/t 的行：保守保留，并由调用方计数上报。
  unparsed,
};

// 单行处置判定。本地 .userdb 与同步快照必须采用同一套判据
// （threshold 与 present_tick），否则本地删除后快照仍保留该行，
// 下一轮同步又会将词条合并回来。
RowAction ClassifyRow(std::string_view key, std::string_view value,
                      int threshold, uint64_t present_tick) {
  const DbRowValue row = ParseDbRowValue(value);
  // c/d/t 缺失时无法套用 librime 的判据（其 Unpack 同样会失败），
  // 保守保留并计数上报。
  if (!row.has_commits || !row.has_dee || !row.has_tick)
    return RowAction::unparsed;
  // tick == 0 表示从 table/stabledb 导入的短语（如 custom_phrase）：
  // librime 对这类行从不执行衰减淘汰，此处同样一律保留。
  if (row.tick == 0)
    return RowAction::skip_phrase;
  if (row.commits < threshold)
    return RowAction::remove;
  // c 不足以判定时，以 librime 自身的可见性判定兜底：已被 librime 判定为
  // 不可见的行（c 可能仍 >= threshold）同样删除。
  if (IsInvisibleToLibrime(key, value, present_tick))
    return RowAction::remove;
  return RowAction::keep;
}

// 快照行的 key 与 value 以制表符分隔，词条名位于第一个制表符之后。
string WordFromKey(std::string_view key) {
  const size_t tab = key.find('\t');
  return tab == std::string_view::npos ? string(key) : string(key.substr(tab + 1));
}

// 去重记录：同一词条可能先经本地物理删除、又经快照清理再删一次
// （本机快照内的一份），只统计一次。
void RecordDeletedWord(UserdbCleanStats *stats, string word) {
  if (stats->recorded_words.insert(word).second)
    stats->deleted_words.push_back(std::move(word));
}

// 排除名单判定：名单中的词典被跳过，其余词典全部清理。
// 名单为空时表示清理所有词典。
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

// 本地词典有两种落盘形式（见 src/rime/dict/dict_module.cc:30-31）：
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

// 进程内同步：合并 sync 目录下其他设备的快照，并导出本机词典。
// IME 进程默认仅加载 kDefaultModules（core/dict/gears），而 user_dict_sync
// 任务注册于 levers 模块，需先补齐模块加载。
bool RunUserDictSync() {
  LoadModules(kDeployerModules);
  // 活动会话的 UserDictionary 持有 .userdb 句柄，而 UserDictManager 会
  // 另行创建句柄执行 Open：Windows 上 LevelDb 的 LOCK 互斥，若不先清理
  // 会话，同步必然失败（日志表现为 "LOCK: 另一个程序正在使用此文件"）。
  // 与官方 RimeSyncUserData 保持一致：先关闭所有会话，由前端随后重建。
  Service::instance().CleanupAllSessions();
  Deployer &deployer = Service::instance().deployer();
  // installation_update 负责读取 installation.yaml 并恢复 user_id；
  // 否则快照会写入 sync_dir 根目录，无法与既有安装目录对应。
  const bool ok_install = deployer.RunTask("installation_update");
  const bool ok_sync = deployer.RunTask("user_dict_sync");
  if (!ok_install || !ok_sync) {
    LOG(ERROR) << "userdb cleaner: sync failed (installation_update="
               << (ok_install ? "ok" : "failed") << ", user_dict_sync="
               << (ok_sync ? "ok" : "failed") << ").";
  }
  return ok_install && ok_sync;
}

// 打开本地词典。正常情况下维护线程上已无活动会话，首次尝试即可打开；
// 若触发按键事件的收尾尚未结束（对应会话的 LOCK 尚未释放），则清理会话
// 并稍候重试。维护线程上不存在 RimeProcessKey 的栈引用，此处的
// CleanupAllSessions 能够真正销毁残留会话。
bool OpenUserdb(an<Db> &db) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (attempt > 0) {
      Service::instance().CleanupAllSessions();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (db->Open())
      return true;
  }
  return false;
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

// 流程第 ③ 步：物理删除本地词典中的无用词条，并将各词典的 /tick 记入
// tick_map，供第 ④ 步清理快照时使用同一 present_tick 判定。
//
// librime 的「删除」仅将 c 写为负值（墓碑行），并依赖墓碑行跨设备传播删除；
// 但同步为全量导出（UniformBackup 原样遍历本地 LevelDb），墓碑行会被原样
// 写入 *.userdb.txt——清理完快照后下一次同步又将其导回，同一批词条每轮
// 重复上报，且文件中始终残留。要彻底删除只能对本地执行物理 Erase：
// 本地无墓碑 → 导出不含 → 快照不含 → 仅上报一次。
// 代价：失去墓碑行后，其他设备同步来的同名词条会复活（下次再删），
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
    // 先收集后删除：避免在 QueryAll 游标遍历期间修改数据库。
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
        // 在日志中逐条记录被删除的词条（含完整 key：码 + 词），便于核对。
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

// 快照头部格式："#@/tick\t41822"。
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
  // 空行与以 "#" 开头的行属于文件头/元数据，一律保留。
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

// 流程第 ④ 步：清理单个快照文件。local_tick / has_local_tick 为本机该词典
// 的 /tick：本地与快照必须使用同一 present_tick 判定，否则本地删除后快照
// 仍保留该行。本机 tick 不可得（词典无法打开或从未同步）时，回退使用
// 快照文件头中的 #@/tick。
void CleanSnapshotFile(const path &file, const UserdbCleanOptions &options,
                       uint64_t local_tick, bool has_local_tick,
                       UserdbCleanStats *stats) {
  // MinGW 下 rime::path 并非 std::filesystem::path，统一经由 UTF-8 字符串
  // 访问文件。
  std::ifstream in(file.to_utf8_string(), std::ios::binary);
  if (!in.is_open()) {
    LOG(ERROR) << "userdb cleaner: cannot open snapshot file: " << file;
    return;
  }

  const string filename = file.filename().to_utf8_string();
  // 流式读写临时文件，避免将整个快照缓冲在内存中。
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

  // 备份由流程第 ② 步完成（仅备份本机快照），此处直接执行原子替换。
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
    // 清除上次运行崩溃或中断残留的临时文件（云盘同步目录中最易堆积）。
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

// 流程第 ② 步：将本机快照复制为 *.userdb_backup.txt（同名覆盖旧备份）。
// 仅复制、不移动：本机快照保留在原位，收尾同步仍需使用；
// 且仅备份本机设备目录——其他设备的快照由其自身负责，不向其他设备
// 目录写入文件。
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

// 日志文件只追加不截断，不设体积上限与轮转。
void AppendCleanLog(const path &sync_dir, const UserdbCleanOptions &options,
                    const UserdbCleanStats &stats) {
  const bool nothing_changed = stats.dict_entries_removed == 0 &&
                               stats.snapshot_rows_removed == 0 &&
                               stats.snapshots_backed_up == 0;
  // 未产生任何变更且两次同步均成功时不记录。
  if (nothing_changed && stats.pre_sync_ok && stats.post_sync_ok)
    return;
  std::error_code ec;
  if (sync_dir.empty() || !std::filesystem::is_directory(sync_dir, ec))
    return;
  const path log_file = sync_dir / path(kCleanLogFileName);
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

// 负责删除锁文件的 RAII 封装。析构不得内联在返回锁路径的函数中：
// 临时对象析构会立即删除刚获取的锁。
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
    return false;  // 无法读取时间戳时按锁仍有效处理，不抢占他人的锁。
  return std::filesystem::file_time_type::clock::now() - last >
         std::chrono::seconds(kLockStaleSeconds);
}

// 尽力而为的运行锁：同一 sync 目录同时只允许一个清理任务运行。
// 云盘同步存在延迟、mtime 也未必保留，因此跨设备无法实现真正互斥，
// 仅用于尽量避免两端同时改写同一批快照。获取失败时返回空 path。
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
    // 无法写入锁文件时放弃互斥，不影响清理本身。
    LOG(WARNING) << "userdb cleaner: cannot write lock file " << lock_file;
    return {};
  }
  out << LogTimestamp() << " userdb_cleaner lock\n";
  out.close();
  return lock_file;
}

// 结果弹窗为模态对话框，在独立线程中显示，避免阻塞维护工作线程：
// "deploy success" 通知须待任务返回后才会发出，不应被弹窗延迟。
void NotifyCleanResult(const UserdbCleanStats &stats,
                       const UserdbCleanOptions &opts) {
  const bool full_display = opts.full_information_display;
  std::thread([stats, full_display]() {
    NotifyUserdbCleanResult(stats, full_display);
  }).detach();
}

} // namespace

bool UserdbCleanTask::Run(Deployer *) {
  const auto started = std::chrono::steady_clock::now();
  UserdbCleanStats stats;
  UserdbCleanOptions opts = options_;
  // 统一剥离名单项中可能携带的后缀。
  for (string &name : opts.exclude_userdb_list) {
    if (auto stripped = StripSuffix(name, kSnapshotSuffix))
      name = std::move(*stripped);
    if (auto stripped = StripSuffix(name, kUserdbDirSuffix))
      name = std::move(*stripped);
  }

  // 获取运行锁；获取失败则跳过本轮。
  LockGuard lock;
  lock.file = AcquireCleanLock(ResolveSyncDir());
  if (lock.file.empty()) {
    stats.lock_failed = true;
    stats.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    NotifyCleanResult(stats, opts);
    return false;
  }

  // 处理顺序：先合并各设备的词条，再两侧同时清理（本地 .userdb 与
  // sync 目录下所有 *.userdb.txt），最后导出干净快照收尾。
  // ① 清理前同步：合并 sync 目录下所有设备快照 → ② 备份本机快照（复制）
  //  → ③ 物理删除本地无用词条 → ④ 清理所有 *.userdb.txt 中的残留行
  //  → ⑤ 清理后同步：再次导出收尾。
  // 触发按键事件的收尾可能尚未结束：触发会话由 RimeProcessKey 调用栈上的
  // shared_ptr 持有，其 .userdb LOCK 需待事件返回后才会释放。维护线程上
  // 不存在栈引用，此处的 CleanupAllSessions 可真正生效；先短暂等待，
  // 同步失败则重试。
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  for (int attempt = 0;;) {
    Service::instance().CleanupAllSessions();
    stats.pre_sync_ok = RunUserDictSync();
    if (stats.pre_sync_ok || ++attempt >= 3)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  // sync_dir / user_id 需待 installation_update 执行后才有意义：
  // 进程启动初期 Deployer 中这两个字段可能仍为空。
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
  NotifyCleanResult(stats, opts);
  return stats.pre_sync_ok && stats.post_sync_ok && !stats.lock_failed;
}

} // namespace rime
