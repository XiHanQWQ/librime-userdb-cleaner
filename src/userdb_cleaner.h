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
  // 黑名单：不参与清理的词典名。为空表示清理全部。
  std::vector<std::string> exclude_userdb_list_;
  bool full_information_display_ = false;
  int clean_threshold_ = 0;
};

} // namespace rime

#endif // USERDB_CLEANER_H_