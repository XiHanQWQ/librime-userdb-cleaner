#include "userdb_cleaner.h"

#include <rime/common.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/deployer.h>
#include <rime/engine.h>
#include <rime/schema.h>
#include <rime/service.h>

#include <utility>

#include "userdb_clean_job.h"

namespace rime {

UserdbCleaner::UserdbCleaner(const Ticket &ticket) : Processor(ticket) {
  InitializeConfig();
}

void UserdbCleaner::InitializeConfig() {
  Config *config =
      (engine_ && engine_->schema()) ? engine_->schema()->config() : nullptr;
  if (!config) {
    LOG(ERROR) << "userdb cleaner: schema config is unavailable, "
               << "falling back to default settings.";
    return;
  }
  config->GetString("userdb_cleaner/trigger_input", &trigger_input_);
  config->GetBool("userdb_cleaner/full_information_display",
                  &full_information_display_);
  config->GetInt("userdb_cleaner/clean_threshold", &clean_threshold_);
  config->GetBool("userdb_cleaner/use_custom_threshold",
                  &use_custom_threshold_);
  if (auto list = config->GetList("userdb_cleaner/exclude_userdb_list")) {
    exclude_userdb_list_.clear();
    for (size_t i = 0; i < list->size(); ++i) {
      if (auto item = list->GetValueAt(i)) {
        std::string name;
        if (item->GetString(&name) && !name.empty())
          exclude_userdb_list_.push_back(std::move(name));
      }
    }
  }
  LOG(INFO) << "userdb cleaner: trigger '" << trigger_input_
            << "', clean_threshold " << clean_threshold_
            << ", use_custom_threshold " << use_custom_threshold_
            << ", full_information_display " << full_information_display_
            << ", userdb blacklist has " << exclude_userdb_list_.size()
            << " item(s).";
}

ProcessResult UserdbCleaner::ProcessKeyEvent(const KeyEvent &) {
  auto ctx = engine_->context();
  if (!ctx || trigger_input_.empty() || ctx->input() != trigger_input_)
    return kNoop;
  ctx->Clear();
  LOG(INFO) << "userdb cleaner: triggered by '" << trigger_input_ << "'.";

  UserdbCleanOptions options;
  options.exclude_userdb_list = exclude_userdb_list_;
  options.clean_threshold = clean_threshold_;
  options.use_custom_threshold = use_custom_threshold_;
  options.full_information_display = full_information_display_;

  // 调度方式与官方 RimeSyncUserData（src/rime_api_impl.h）保持一致：
  // 清空会话、将清理任务排入 Deployer 队列、进入维护模式。
  // 清理不得在按键事件内同步执行：触发会话由 RimeProcessKey 调用栈上的
  // shared_ptr 持有，只要该调用链尚未返回，会话持有的 .userdb 文件锁
  // 就无法释放，user_dict_sync 将因 LevelDB 文件锁冲突而全部失败；
  // 维护工作线程上不存在该栈引用，会话及其文件锁才能被真正释放。
  // 维护模式期间 Service::disabled() 使全部会话 API 安全失效（按键直接
  // 透传，前端不会访问已销毁的会话）；任务完成后 Deployer 发出
  // "deploy success" 通知，前端据此重建会话。清理逻辑见 UserdbCleanTask::Run。
  Deployer &deployer = Service::instance().deployer();
  Service::instance().CleanupAllSessions();
  deployer.ScheduleTask(New<UserdbCleanTask>(options));
  if (!deployer.StartMaintenance())
    LOG(INFO) << "userdb cleaner: task queued behind running maintenance.";
  return kAccepted;
}

} // namespace rime