#ifndef USERDB_CLEANER_H_
#define USERDB_CLEANER_H_

#include <string>
#include <vector>

#include <rime/common.h>
#include <rime/processor.h>

namespace rime {

class UserdbCleaner : public Processor {
public:
  explicit UserdbCleaner(const Ticket &ticket);

  ProcessResult ProcessKeyEvent(const KeyEvent &key_event) override;

private:
  void InitializeConfig();

  std::string trigger_input_ = "/del";
  // 排除名单：名单中的用户词典不参与清理；名单为空时清理全部词典。
  std::vector<std::string> exclude_userdb_list_;
  bool full_information_display_ = false;
  int clean_threshold_ = 0;
  // 自定义 c 值废词判据（c < clean_threshold）的开关，默认不启用。
  bool use_custom_threshold_ = false;
};

} // namespace rime

#endif // USERDB_CLEANER_H_