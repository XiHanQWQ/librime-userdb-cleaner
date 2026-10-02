#include "userdb_clean_job.h"

#include <rime/common.h>
#include <rime/deployer.h>
#include <rime/dict/user_db.h>
#include <rime/registry.h>
#include <rime/service.h>
#include <rime/setup.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace rime {

namespace {

constexpr const char kUserdbDirSuffix[] = ".userdb";
constexpr const char kSnapshotSuffix[] = ".userdb.txt";
constexpr const char kBackupSuffix[] = ".userdb_backup.txt";
constexpr const char kCleanLogFileName[] = "userdb_cleaner.txt";

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

std::optional<int> ExtractCommitCount(std::string_view value) {
  size_t pos = 0;
  while (pos < value.size()) {
    size_t end = value.find(' ', pos);
    if (end == std::string_view::npos)
      end = value.size();
    if (end > pos + 2 && value[pos] == 'c' && value[pos + 1] == '=') {
      int commits = 0;
      const char *first = value.data() + pos + 2;
      const char *last = value.data() + end;
      const auto result = std::from_chars(first, last, commits);
      if (result.ec == std::errc() && result.ptr == last)
        return commits;
    }
    pos = end + 1;
  }
  return std::nullopt;
}

bool ShouldRemoveSnapshotLine(std::string_view raw_line, int threshold,
                              string *word) {
  std::string_view line = raw_line;
  if (!line.empty() && line.back() == '\r')
    line.remove_suffix(1);
  if (line.empty() || line[0] == '#')
    return false;
  const size_t first_tab = line.find('\t');
  if (first_tab == std::string_view::npos)
    return false;
  const size_t second_tab = line.find('\t', first_tab + 1);
  if (second_tab == std::string_view::npos)
    return false;
  const auto commits = ExtractCommitCount(line.substr(second_tab + 1));
  if (!commits || *commits >= threshold)
    return false;
  if (word)
    *word = string(line.substr(first_tab + 1, second_tab - first_tab - 1));
  return true;
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

vector<string> FindUserdbNames(const path &user_data_dir) {
  vector<string> names;
  std::error_code ec;
  if (user_data_dir.empty() ||
      !std::filesystem::is_directory(user_data_dir, ec))
    return names;
  std::filesystem::directory_iterator it(user_data_dir, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code entry_ec;
    if (!it->is_directory(entry_ec) || entry_ec)
      continue;
    const string filename = path(it->path()).filename().to_utf8_string();
    if (auto base = StripSuffix(filename, kUserdbDirSuffix))
      names.push_back(std::move(*base));
  }
  std::sort(names.begin(), names.end());
  return names;
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

// 物理删除本地 .userdb 中 c < threshold 的词条。
//
// librime 的「删除」只是把 c 写成负值（墓碑行），它靠墓碑行跨设备传播删除；
// 但同步是全量导出（UniformBackup 原样遍历本地 LevelDb），墓碑行会被原封
// 写进 *.userdb.txt。于是清理完快照后，下一次同步又从本地导出同一批墓碑行
// ——每次都重复上报同一批词，文件里却始终还在。要真正删干净只能本地物理
// Erase：本地没墓碑 → 导不出 → 快照没有 → 只报一次。
// 代价：没有墓碑行后，其他设备同步来的同名词条会复活（下次再删）；
// 跨设备的删除传播失效。
void CleanLiveUserdbs(const UserdbCleanOptions &options,
                      UserdbCleanStats *stats) {
  UserDb::Component *component = UserDb::Require("userdb");
  if (!component) {
    LOG(ERROR) << "userdb cleaner: userdb component is unavailable.";
    return;
  }
  // 活动会话强持有 .userdb 句柄，而 LevelDb 的 LOCK 文件是互斥的，
  // 不清会话就打不开第二个句柄。与官方 RimeSyncUserData 一致：
  // 先关闭所有会话，前端随后自动重建。
  Service::instance().CleanupAllSessions();
  const Deployer &deployer = Service::instance().deployer();
  for (const string &name : FindUserdbNames(deployer.user_data_dir)) {
    if (!ShouldClean(name, options))
      continue;
    an<Db> db(component->Create(name));
    if (!db) {
      LOG(ERROR) << "userdb cleaner: cannot create user db '" << name << "'.";
      continue;
    }
    if (!db->Open()) {
      LOG(WARNING) << "userdb cleaner: cannot open user db '" << name
                   << "' (locked by another session?).";
      continue;
    }
    if (db->readonly()) {
      LOG(WARNING) << "userdb cleaner: user db '" << name << "' is read only.";
      db->Close();
      continue;
    }
    // 先收集再删除：QueryAll 的游标遍历期间不改动 db。
    vector<string> doomed_keys;
    {
      an<DbAccessor> accessor = db->QueryAll();
      string key, value;
      while (accessor && accessor->GetNextRecord(&key, &value)) {
        const auto commits = ExtractCommitCount(value);
        if (!commits || *commits >= options.clean_threshold)
          continue;
        doomed_keys.push_back(key);
      }
    }
    int removed = 0;
    for (const string &key : doomed_keys) {
      if (!db->Erase(key)) {
        LOG(WARNING) << "userdb cleaner: cannot erase an entry from '" << name
                     << "'.";
        continue;
      }
      ++removed;
      // 删除的词条全部记下来：日志要写全量，弹窗自己只取前 kMaxDisplayedWords 条。
      const size_t tab = key.find('\t');
      stats->deleted_words.push_back(tab == string::npos ? key
                                                         : key.substr(tab + 1));
    }
    db->Close();
    if (removed == 0)
      continue;
    stats->dict_entries_removed += removed;
    stats->deleted_dicts.push_back(name);
    LOG(INFO) << "userdb cleaner: erased " << removed << " entries from '"
              << name << "'.";
  }
}

void CleanSnapshotFile(const path &file,
                       const UserdbCleanOptions &options,
                       UserdbCleanStats *stats) {
  // MinGW 上 rime::path 不是 std::filesystem::path，统一走 UTF-8 字符串。
  std::ifstream in(file.to_utf8_string(), std::ios::binary);
  if (!in.is_open()) {
    LOG(ERROR) << "userdb cleaner: cannot open snapshot file: " << file;
    return;
  }

  // 边读边写到临时文件，避免把整个快照缓冲在内存里。
  path temp(file);
  temp += path(".tmp");
  std::ofstream out(temp.to_utf8_string(), std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    LOG(ERROR) << "userdb cleaner: cannot write " << temp;
    return;
  }

  vector<string> removed_words;
  string line;
  int removed = 0;
  while (std::getline(in, line)) {
    string word;
    if (ShouldRemoveSnapshotLine(line, options.clean_threshold, &word)) {
      ++removed;
      removed_words.push_back(std::move(word));
    } else {
      out << line << '\n';
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

  // 备份由流程第②步的旧快照改名完成，这里不再复制，避免覆盖它。
  std::filesystem::rename(temp, file, ec);
  if (ec) {
    LOG(ERROR) << "userdb cleaner: cannot replace " << file << ": "
               << ec.message();
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    return;
  }

  stats->snapshot_rows_removed += removed;
  stats->deleted_files.push_back(file.filename().to_utf8_string());
  for (string &word : removed_words)
    stats->deleted_words.push_back(std::move(word));
  LOG(INFO) << "userdb cleaner: removed " << removed << " rows from '"
            << file.filename().to_utf8_string() << "'.";
}

void CleanSnapshots(const path &sync_dir, const UserdbCleanOptions &options,
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
    auto dict_name = StripSuffix(filename, kSnapshotSuffix);
    if (!dict_name || !ShouldClean(*dict_name, options))
      continue;
    ++stats->snapshots_processed;
    CleanSnapshotFile(file, options, stats);
  }
}

// 流程第②步：把旧快照改名保留为 *.userdb_backup.txt（同名覆盖旧备份）。
// 之后的「重建快照同步」找不到旧 .userdb.txt，不再合并旧数据，
// 而是在清理后的本地词典上重新生成快照，从源头避免旧快照复活已清理词条。
void BackupSnapshots(const path &sync_dir, const UserdbCleanOptions &options,
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
    auto dict_name = StripSuffix(filename, kSnapshotSuffix);
    if (!dict_name || !ShouldClean(*dict_name, options))
      continue;
    const path backup = file.parent_path() / path(*dict_name + kBackupSuffix);
    std::error_code rename_ec;
    std::filesystem::rename(file, backup, rename_ec);
    if (rename_ec) {
      LOG(WARNING) << "userdb cleaner: cannot rename '" << filename
                   << "': " << rename_ec.message();
      continue;
    }
    ++stats->snapshots_renamed;
    LOG(INFO) << "userdb cleaner: moved snapshot '" << filename
              << "' to backup.";
  }
}

void AppendCleanLog(const path &sync_dir, const UserdbCleanOptions &options,
                    const UserdbCleanStats &stats) {
  if (stats.dict_entries_removed == 0 && stats.snapshot_rows_removed == 0 &&
      stats.snapshots_renamed == 0 && stats.rebuild_sync_ok &&
      stats.final_sync_ok)
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
      << stats.deleted_files.size() << " snapshot file(s), moved "
      << stats.snapshots_renamed << " snapshot(s) to backup; threshold = "
      << options.clean_threshold << "; elapsed = " << stats.elapsed.count()
      << " ms; rebuild-sync " << (stats.rebuild_sync_ok ? "ok" : "FAILED")
      << ", final-sync " << (stats.final_sync_ok ? "ok" : "FAILED") << ":";
  size_t column = 0;
  for (const string &word : stats.deleted_words) {
    if (column++ % 10 == 0)
      out << "\n  ";
    out << "[ " << word << " ]";
  }
  out << "\n\n";
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
  const path sync_dir = ResolveSyncDir();
  // ① 关会话 + 物理删本地 c < threshold 的词条
  //  → ② 旧快照改名备份 → ③ 重建快照同步（由干净的本地导出新快照）
  //  → ④ 兜底清理快照中的残留行 → ⑤ 收尾同步（合并快照再导出）
  CleanLiveUserdbs(opts, &stats);
  BackupSnapshots(sync_dir, opts, &stats);
  stats.rebuild_sync_ok = RunUserDictSync();
  CleanSnapshots(sync_dir, opts, &stats);
  stats.final_sync_ok = RunUserDictSync();
  stats.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  AppendCleanLog(sync_dir, opts, stats);
  LOG(INFO) << "userdb cleaner: scanned " << stats.snapshots_processed
            << " snapshot file(s), moved " << stats.snapshots_renamed
            << " to backup, removed " << stats.dict_entries_removed
            << " entries from " << stats.deleted_dicts.size()
            << " userdb(s) and " << stats.snapshot_rows_removed
            << " snapshot row(s) in " << stats.elapsed.count()
            << " ms; rebuild-sync " << (stats.rebuild_sync_ok ? "ok" : "failed")
            << ", final-sync " << (stats.final_sync_ok ? "ok" : "failed") << ".";
  return stats;
}

} // namespace rime