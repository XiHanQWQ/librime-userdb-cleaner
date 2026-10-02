#include "userdb_cleaner.h"

#include <rime/common.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>

#include <atomic>
#include <thread>
#include <utility>

#include "userdb_clean_job.h"
#include "userdb_notify.h"

namespace rime {

namespace {

std::atomic<bool> g_cleaning{false};

struct CleaningGuard {
  ~CleaningGuard() { g_cleaning.store(false, std::memory_order_release); }
};

} // namespace

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
  options.full_information_display = full_information_display_;

  if (g_cleaning.exchange(true, std::memory_order_acq_rel)) {
    LOG(WARNING) << "userdb cleaner: a previous run is still in progress.";
    return kAccepted;
  }

  std::thread([options]() {
    UserdbCleanStats stats;
    {
      // 清理阶段持有 guard；通知阶段不再占用标志，
      // 这样弹窗/zenity 等待不会阻塞下一次触发。
      CleaningGuard guard;
      stats = RunUserdbClean(options);
    }
    NotifyUserdbCleanResult(stats, options.full_information_display);
  }).detach();
  return kAccepted;
}

} // namespace rime