#ifndef USERDB_CLEAN_JOB_H_
#define USERDB_CLEAN_JOB_H_

#include <chrono>
#include <string>
#include <vector>

namespace rime {

// 弹窗里最多列出的删除词条数；日志文件不受此限制，记录全部词条。
constexpr size_t kMaxDisplayedWords = 10;

struct UserdbCleanOptions {
  // 黑名单：列表中的词典将被跳过，其余全部清理。
  // 列表为空时表示清理所有词典。
  std::vector<std::string> exclude_userdb_list;
  int clean_threshold = 0;
  bool full_information_display = false;
};

struct UserdbCleanStats {
  int dict_entries_removed = 0;
  int snapshot_rows_removed = 0;
  int snapshots_processed = 0;
  int snapshots_renamed = 0;
  // 重建快照同步：旧快照已改名备份，直接由清理后的本地词典导出全新快照
  bool rebuild_sync_ok = false;
  // 收尾同步：合并刚清理过的快照并再次导出，确认没有词条被合并回来
  bool final_sync_ok = false;
  std::vector<std::string> deleted_dicts;
  std::vector<std::string> deleted_files;
  std::vector<std::string> deleted_words;
  std::chrono::milliseconds elapsed{0};
};

UserdbCleanStats RunUserdbClean(const UserdbCleanOptions &options);

} // namespace rime

#endif // USERDB_CLEAN_JOB_H_