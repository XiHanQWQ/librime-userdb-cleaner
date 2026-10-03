#include "userdb_notify.h"

#include <rime/common.h>

#include "userdb_clean_job.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rime {
namespace {

#ifndef _WIN32
std::string ShellQuote(const std::string &text) {
  std::string result = "'";
  for (const char c : text) {
    if (c == '\'')
      result += "'\\''";
    else
      result += c;
  }
  result += '\'';
  return result;
}

std::string AppleScriptQuote(const std::string &text) {
  std::string result;
  for (const char c : text) {
    if (c == '\\' || c == '"')
      result += '\\';
    result += c;
  }
  return result;
}
#endif

std::string JoinLines(const std::vector<std::string> &items) {
  std::string out;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0)
      out += '\n';
    out += items[i];
  }
  return out;
}

std::string FormatWords(const std::vector<std::string> &words, size_t limit) {
  const size_t count = limit ? (std::min)(limit, words.size()) : words.size();
  std::ostringstream out;
  for (size_t i = 0; i < count; ++i) {
    if (i > 0)
      out << (i % 5 == 0 ? "\n" : ", ");
    out << "[ " << words[i] << " ]";
  }
  return out.str();
}

std::string BuildMessage(const UserdbCleanStats &stats, bool full) {
  std::ostringstream msg;
  const int total_removed =
      stats.dict_entries_removed + stats.snapshot_rows_removed;
  if (stats.lock_failed) {
    msg << "用户词典清理已跳过：另一个清理任务正在运行。\n"
        << "锁文件：同步目录下的 .userdb_cleaner.lock，超过 10 分钟视为陈锁"
        << "自动接管。\n耗时：" << stats.elapsed.count() << " 毫秒";
  } else if (total_removed > 0) {
    msg << "用户词典清理完成。\n"
        << "用户词典：移除 " << stats.dict_entries_removed << " 个词条（"
        << stats.deleted_dicts.size() << " 本）\n"
        << "同步快照：删除 " << stats.snapshot_rows_removed << " 行（"
        << stats.deleted_files.size() << " 个文件）\n"
        << "耗时：" << stats.elapsed.count() << " 毫秒\n"
        << "删除的词条已记录到同步目录的 userdb_cleaner.txt 文件中。";
  } else {
    msg << "用户词典清理完成。\n"
        << "未找到需要清理的词条。\n"
        << "耗时：" << stats.elapsed.count() << " 毫秒";
  }
  msg << "\n清理前同步：" << (stats.pre_sync_ok ? "成功" : "失败")
      << "，清理后同步：" << (stats.post_sync_ok ? "成功" : "失败");
  if (stats.skipped_phrase_rows > 0 || stats.unparsed_rows > 0) {
    msg << "\n跳过 " << stats.skipped_phrase_rows << " 行 t=0 短语、"
        << stats.unparsed_rows << " 行无法解析的行（详见日志）";
  }
  if (full) {
    msg << "\n\n已扫描同步快照：" << stats.snapshots_processed << " 个文件";
    if (stats.snapshots_backed_up > 0)
      msg << "\n备份的本机快照：" << stats.snapshots_backed_up << " 个";
    if (!stats.deleted_dicts.empty())
      msg << "\n\n处理的用户词典：\n" << JoinLines(stats.deleted_dicts);
    if (!stats.deleted_files.empty())
      msg << "\n\n处理的快照文件：\n" << JoinLines(stats.deleted_files);
    if (!stats.deleted_words.empty()) {
      msg << "\n\n删除的词条 ( " << stats.deleted_words.size() << "条 )：";
      if (stats.deleted_words.size() > kMaxDisplayedWords)
        msg << "（弹窗只列前 " << kMaxDisplayedWords << " 条）";
      msg << "\n" << FormatWords(stats.deleted_words, kMaxDisplayedWords);
    }
  }
  return msg.str();
}

void ShowMessage(const std::string &title, const std::string &message) {
#ifdef _WIN32
  const int wmessage_len =
      MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, nullptr, 0);
  const int wtitle_len =
      MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
  if (wmessage_len <= 0 || wtitle_len <= 0) {
    LOG(WARNING) << "userdb cleaner: failed to convert message to UTF-16.";
    return;
  }
  std::wstring wmessage(static_cast<size_t>(wmessage_len), L'\0');
  std::wstring wtitle(static_cast<size_t>(wtitle_len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, wmessage.data(),
                      wmessage_len);
  MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, wtitle.data(), wtitle_len);
  // 去掉尾部 '\0'，让 wstring 大小与实际字符数一致。
  wmessage.resize(static_cast<size_t>(wmessage_len - 1));
  wtitle.resize(static_cast<size_t>(wtitle_len - 1));
  MessageBoxW(nullptr, wmessage.c_str(), wtitle.c_str(),
              MB_OK | MB_ICONINFORMATION);
#elif defined(__APPLE__)
  const std::string script = "display dialog \"" + AppleScriptQuote(message) +
                             "\" with title \"" + AppleScriptQuote(title) +
                             "\" buttons {\"OK\"} default button \"OK\"";
  if (std::system(("osascript -e " + ShellQuote(script)).c_str()) != 0)
    LOG(INFO) << "userdb cleaner: " << title << ": " << message;
#elif defined(__linux__)
  const std::string quoted_title = ShellQuote(title);
  const std::string quoted_message = ShellQuote(message);
  if (std::system("command -v zenity > /dev/null 2>&1") == 0 &&
      std::system(("zenity --info --title=" + quoted_title +
                   " --text=" + quoted_message + " 2>/dev/null")
                      .c_str()) == 0)
    return;
  if (std::system("command -v kdialog > /dev/null 2>&1") == 0 &&
      std::system(("kdialog --title " + quoted_title + " --msgbox " +
                   quoted_message + " 2>/dev/null")
                      .c_str()) == 0)
    return;
  if (std::system("command -v notify-send > /dev/null 2>&1") == 0 &&
      std::system(
          ("notify-send " + quoted_title + " " + quoted_message).c_str()) == 0)
    return;
  LOG(INFO) << "userdb cleaner: " << title << ": " << message;
#else
  LOG(INFO) << "userdb cleaner: " << title << ": " << message;
#endif
}

} // namespace

void NotifyUserdbCleanResult(const UserdbCleanStats &stats,
                             bool full_information_display) {
  ShowMessage("用户词典清理工具",
              BuildMessage(stats, full_information_display));
}

} // namespace rime