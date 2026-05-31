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
  int clean_threshold_ = 1;  // 清理阈值，c值小于此值的词条将被删除，默认为1
  int tick_threshold_ = 3000;  // tick差值清理阈值，总tick-词条tick大于此值的词条将被删除
  std::string clean_mode_ = "c";  // 清理模式: "c"(按c值), "tick"(按tick差值), "c_then_tick"(先c后tick)
  bool enable_backup_ = true;  // 是否在清理前备份.userdb.txt文件
  bool enable_write_clean_file_ = true;  // 是否将清理后的结果写回文件
};

}  // namespace rime
#endif