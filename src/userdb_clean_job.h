#ifndef USERDB_CLEAN_JOB_H_
#define USERDB_CLEAN_JOB_H_

#include <chrono>
#include <set>
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
  int snapshots_backed_up = 0;
  // t = 0 的短语行（table/stabledb，如 custom_phrase）：按 librime 语义不动。
  int skipped_phrase_rows = 0;
  // 解析不出 c= 字段的行：保守保留，但要报出来，免得格式变了却静默失效。
  int unparsed_rows = 0;
  bool lock_failed = false;
  // 清理前同步：合并 sync 下所有设备目录的快照进本地词典，再导出本机词典
  bool pre_sync_ok = false;
  // 清理后同步：合并已清理的快照并再次导出，保证快照与本地状态一致
  bool post_sync_ok = false;
  std::vector<std::string> deleted_dicts;
  std::vector<std::string> deleted_files;
  // 已去重的删除词条：同一个词先被本地物理删、又被快照清理删时只记一次。
  std::vector<std::string> deleted_words;
  std::set<std::string> recorded_words;
  std::chrono::milliseconds elapsed{0};
};

UserdbCleanStats RunUserdbClean(const UserdbCleanOptions &options);

} // namespace rime

#endif // USERDB_CLEAN_JOB_H_
