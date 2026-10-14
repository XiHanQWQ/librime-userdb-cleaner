#ifndef USERDB_CLEAN_JOB_H_
#define USERDB_CLEAN_JOB_H_

#include <chrono>
#include <set>
#include <string>
#include <vector>

#include <rime/deployer.h>

namespace rime {

// 结果弹窗中最多列出的已删除词条数；日志文件不受此限制，记录全部词条。
constexpr size_t kMaxDisplayedWords = 10;

struct UserdbCleanOptions {
  // 排除名单：名单中的词典将被跳过，其余词典全部清理。
  // 名单为空时表示清理所有词典。
  std::vector<std::string> exclude_userdb_list;
  int clean_threshold = 0;
  // 自定义 c 值废词判据（c < clean_threshold）的开关：false 时仅使用
  // librime 内部判据（UserDictionary::CreateDictEntry 可见性判定）。
  bool use_custom_threshold = false;
  bool full_information_display = false;
};

struct UserdbCleanStats {
  int dict_entries_removed = 0;
  int snapshot_rows_removed = 0;
  int snapshots_processed = 0;
  int snapshots_backed_up = 0;
  // t = 0 的短语行（源自 table/stabledb，如 custom_phrase）：遵循 librime
  // 语义，不做清理。
  int skipped_phrase_rows = 0;
  // 无法解析出 c= 字段的行：保守保留，但必须计数上报，避免词典格式变化后
  // 清理行为静默失效。
  int unparsed_rows = 0;
  bool lock_failed = false;
  // 清理前同步：合并 sync 目录下所有设备快照至本地词典，并导出本机词典。
  bool pre_sync_ok = false;
  // 清理后同步：合并已清理的快照并再次导出，保证发布出去的快照与本地状态
  // 一致。
  bool post_sync_ok = false;
  std::vector<std::string> deleted_dicts;
  std::vector<std::string> deleted_files;
  // 去重后的已删除词条：同一词条先经本地物理删除、又经快照清理删除时
  // 仅记录一次。
  std::vector<std::string> deleted_words;
  std::set<std::string> recorded_words;
  std::chrono::milliseconds elapsed{0};
};

// 清理以部署任务的形式执行：任务入队后由 StartMaintenance() 启动的维护
// 工作线程运行。维护模式期间 Service::disabled() 使全部会话 API 安全失效；
// 工作线程上不存在 RimeProcessKey 的栈引用，活动会话及其持有的 .userdb
// 句柄得以真正销毁，从而避免与前端线程产生数据竞争。
class UserdbCleanTask : public DeploymentTask {
 public:
  explicit UserdbCleanTask(const UserdbCleanOptions &options)
      : options_(options) {}

  bool Run(Deployer *deployer) override;

 private:
  UserdbCleanOptions options_;
};

} // namespace rime

#endif // USERDB_CLEAN_JOB_H_
