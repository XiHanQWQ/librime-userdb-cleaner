#ifndef USERDB_CLEANER_HPP_
#define USERDB_CLEANER_HPP_

#include <rime/common.h>
#include <rime/processor.h>
#include <rime/config.h>
#include <vector>
#include <string>

namespace rime {

class UserdbCleaner : public Processor {
 public:
  explicit UserdbCleaner(const Ticket& ticket);
  ~UserdbCleaner();

  ProcessResult ProcessKeyEvent(const KeyEvent& key_event) override;

 private:
  void InitializeConfig();
  std::string trigger_input_ = "/del";  // 默认触发输入
  std::vector<std::string> cleanup_userdb_list_;  // 需要清理的userdb列表
  bool full_information_display_ = false;  // 是否显示完整清理信息，默认为false
};

}  // namespace rime
#endif