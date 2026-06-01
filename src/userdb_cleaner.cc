// userdb_cleaner.cc
#include <rime/common.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/schema.h>
#include <rime_api.h>

// 跨平台必需的头文件
#include <chrono>
#include <charconv>      // std::from_chars
#include <filesystem>    // std::filesystem
#include <fstream>       // std::ifstream, std::ofstream
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>
#include <cstdlib>       // std::system

#ifdef _WIN32
#include <windows.h>
#endif

#include "lib/detached_thread_manager.hpp"
#include "userdb_cleaner.hpp"

// Rime 头文件
#include <rime/service.h>
#include <rime/lever/deployment_tasks.h>  // 包含 UserDictSync

namespace fs = std::filesystem;

// 常量定义 — 避免魔法字符串
namespace {
constexpr const char* kUserdbSuffix = ".userdb";
constexpr const char* kUserdbTxtSuffix = ".userdb.txt";
constexpr size_t kUserdbSuffixLen = 7;
constexpr size_t kUserdbTxtSuffixLen = 11;
constexpr const char* kCFieldPrefix = "c=";
constexpr size_t kCFieldPrefixLen = 2;

// 检查字符串是否以指定后缀结尾（避免 substr 拷贝）
inline bool has_suffix(const std::string& str, const char* suffix, size_t suffix_len) {
  size_t len = str.size();
  return len > suffix_len && str.compare(len - suffix_len, suffix_len, suffix) == 0;
}
}  // namespace

namespace rime {

UserdbCleaner::UserdbCleaner(const Ticket& ticket) : Processor(ticket) {
  DLOG(INFO) << "UserdbCleaner initialized";
  InitializeConfig();
}

UserdbCleaner::~UserdbCleaner() {
  DLOG(INFO) << "UserdbCleaner destroyed";
}

void UserdbCleaner::InitializeConfig() {
  if (!engine_) {
    LOG(ERROR) << "Engine is null in UserdbCleaner";
    return;
  }
  
  Schema* schema = engine_->schema();
  if (!schema) {
    LOG(ERROR) << "Failed to get schema in UserdbCleaner";
    return;
  }
  
  Config* config = schema->config();
  if (!config) {
    LOG(ERROR) << "Failed to get config in UserdbCleaner";
    return;
  }

  // 读取触发输入配置
  if (!config->GetString("userdb_cleaner/trigger_input", &trigger_input_)) {
    LOG(INFO) << "userdb_cleaner/trigger_input not set, using default: " << trigger_input_;
  } else {
    LOG(INFO) << "UserdbCleaner trigger_input: " << trigger_input_;
  }

  // 读取需要清理的userdb列表
  if (auto list = config->GetList("userdb_cleaner/cleanup_userdb_list")) {
    cleanup_userdb_list_.clear();
    for (size_t i = 0; i < list->size(); ++i) {
      if (auto item = list->GetValueAt(i)) {
        std::string db_name;
        if (item->GetString(&db_name)) {
          cleanup_userdb_list_.push_back(db_name);
          LOG(INFO) << "Added to cleanup list: " << db_name;
        }
      }
    }
    LOG(INFO) << "Cleanup userdb list has " << cleanup_userdb_list_.size() << " items";
  } else {
    LOG(INFO) << "No cleanup_userdb_list specified, will clean all userdb files";
  }

  // 读取是否显示完整信息的配置
  if (!config->GetBool("userdb_cleaner/full_information_display", &full_information_display_)) {
    LOG(INFO) << "userdb_cleaner/full_information_display not set, using default: " << full_information_display_;
  } else {
    LOG(INFO) << "UserdbCleaner full_information_display: " << full_information_display_;
  }

  // 读取清理阈值配置
  if (!config->GetInt("userdb_cleaner/clean_threshold", &clean_threshold_)) {
    LOG(INFO) << "userdb_cleaner/clean_threshold not set, using default: " << clean_threshold_;
  } else {
    LOG(INFO) << "UserdbCleaner clean_threshold: " << clean_threshold_;
  }  
}

#ifdef _WIN32
/**
 * 执行 WeaselDeployer 命令（无窗口模式）
 */
bool execute_weasel_deployer(const std::string& argument) {
  // 获取共享数据目录（程序目录）
  char shared_data_dir[1024] = {0};
  rime_get_api()->get_shared_data_dir_s(shared_data_dir, sizeof(shared_data_dir));
  
  // WeaselDeployer.exe 在共享数据目录的父目录中
  fs::path deployer_path = fs::path(shared_data_dir).parent_path() / "WeaselDeployer.exe";
  
  if (!fs::exists(deployer_path)) {
    LOG(ERROR) << "WeaselDeployer.exe not found at: " << deployer_path.string();
    return false;
  }
  
  // 使用 STARTUPINFO 和 PROCESS_INFORMATION 来隐藏窗口
  STARTUPINFO si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;  // 隐藏窗口
  ZeroMemory(&pi, sizeof(pi));
  
  std::string command = "\"" + deployer_path.string() + "\" " + argument;
  LOG(INFO) << "Executing: " << command;
  
  // 创建进程
  BOOL success = CreateProcess(
    NULL,                           // 应用程序名（使用命令行）
    const_cast<LPSTR>(command.c_str()), // 命令行
    NULL,                           // 进程安全属性
    NULL,                           // 线程安全属性
    FALSE,                          // 句柄继承选项
    0,                              // 创建标志
    NULL,                           // 环境变量
    NULL,                           // 当前目录
    &si,                            // 启动信息
    &pi                             // 进程信息
  );
  
  if (!success) {
    LOG(ERROR) << "CreateProcess failed: " << GetLastError();
    return false;
  }
  
  // 等待进程完成
  WaitForSingleObject(pi.hProcess, INFINITE);
  
  // 关闭进程和线程句柄
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  
  LOG(INFO) << "WeaselDeployer executed successfully: " << argument;
  return true;
}
#endif

/**
 * 获取同步目录
 */
fs::path get_sync_directory() {
  fs::path sync_path;
  
  // 方法1: 使用 get_sync_dir_s API 函数
  char sync_dir[1024] = {0};
  rime_get_api()->get_sync_dir_s(sync_dir, sizeof(sync_dir));
  sync_path = fs::path(sync_dir);
  
  if (fs::exists(sync_path) && fs::is_directory(sync_path)) {
    LOG(INFO) << "Using sync directory from API: " << sync_path.string();
    return sync_path;
  }
  
  LOG(WARNING) << "Sync directory from API does not exist: " << sync_path.string();
  
  // 方法2: 解析 installation.yaml 中的 sync_dir 配置
  char user_data_dir[1024] = {0};
  rime_get_api()->get_user_data_dir_s(user_data_dir, sizeof(user_data_dir));
  fs::path user_path(user_data_dir);
  fs::path inst_file = user_path / "installation.yaml";
  
  if (fs::exists(inst_file)) {
    Config config;
    if (config.LoadFromFile(inst_file)) {
      std::string custom_sync_dir;
      if (config.GetString("sync_dir", &custom_sync_dir)) {
        sync_path = fs::path(custom_sync_dir);
        
        if (fs::exists(sync_path) && fs::is_directory(sync_path)) {
          LOG(INFO) << "Using sync directory from installation.yaml: " << sync_path.string();
          return sync_path;
        } else {
          LOG(WARNING) << "Sync directory from installation.yaml does not exist: " << sync_path.string();
        }
      } else {
        LOG(INFO) << "No sync_dir configuration found in installation.yaml";
      }
    } else {
      LOG(ERROR) << "Failed to load installation.yaml";
    }
  } else {
    LOG(WARNING) << "installation.yaml does not exist: " << inst_file.string();
  }
  
  // 方法3: 使用用户目录下的 sync 目录作为默认值
  sync_path = user_path / "sync";
  if (fs::exists(sync_path) && fs::is_directory(sync_path)) {
    LOG(INFO) << "Using default sync directory: " << sync_path.string();
    return sync_path;
  }
  
  LOG(ERROR) << "No valid sync directory found";
  return sync_path; // 返回默认路径，即使它不存在
}

/**
 * 获取当前时间的中文格式字符串
 */
std::string get_current_time() {
  auto now = std::chrono::system_clock::now();
  auto tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm;
#ifdef _WIN32
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  
  std::ostringstream oss;
  oss << std::setfill('0') 
      << (tm.tm_year + 1900) << "-"
      << std::setw(2) << (tm.tm_mon + 1) << "-"
      << std::setw(2) << tm.tm_mday << " "
      << std::setw(2) << tm.tm_hour << ":"
      << std::setw(2) << tm.tm_min << ":"
      << std::setw(2) << tm.tm_sec;

  return oss.str();
}

/**
 * 备份.userdb.txt文件为.userdb_backup.txt
 */
bool backup_userdb_file(const fs::path& userdb_file) {
  try {
    // 构造备份文件名
    std::string filename = userdb_file.filename().string();
    std::string backup_filename = filename;
    size_t pos = backup_filename.find(kUserdbTxtSuffix);
    if (pos != std::string::npos) {
      backup_filename.replace(pos, kUserdbTxtSuffixLen, ".userdb_backup.txt");
    } else {
      backup_filename += ".backup";
    }
    
    fs::path backup_path = userdb_file.parent_path() / backup_filename;
    
    // 复制文件（覆盖模式）
    fs::copy_file(userdb_file, backup_path, fs::copy_options::overwrite_existing);
    
    LOG(INFO) << "Backed up " << filename << " to " << backup_filename;
    return true;
  } catch (const fs::filesystem_error& e) {
    LOG(ERROR) << "Failed to backup file " << userdb_file.string() << ": " << e.what();
    return false;
  }
}

/**
 * 记录删除的词条到日志文件
 */
void log_deleted_words(const std::vector<std::string>& deleted_words, const fs::path& sync_dir) {
  if (deleted_words.empty()) {
    return;
  }
  
  fs::path log_file = sync_dir / "userdb_cleaner.txt";
  
  try {
    // 以追加模式打开文件，使用UTF-8编码（不带BOM）
    std::ofstream out(log_file, std::ios::app);
    if (!out.is_open()) {
      LOG(ERROR) << "Failed to open log file: " << log_file.string();
      return;
    }
    
    // 写入当前时间
    std::string current_time = get_current_time();
    out << current_time << " Deleted " << deleted_words.size() << " words:\n";
    for (size_t i = 0; i < deleted_words.size(); ++i) {
      if (i > 0 && i % 10 == 0) {
        out << "\n";
      }
      out << "  [ " << deleted_words[i] << " ]";
    }
    out << "\n";
    
    out << "\n"; // 添加空行分隔不同时间的记录
    
    out.close();
    LOG(INFO) << "Logged " << deleted_words.size() << " deleted words to " << log_file.string();
  } catch (const std::exception& e) {
    LOG(ERROR) << "Failed to write to log file: " << e.what();
  }
}

/**
 * 检查是否需要清理指定的userdb
 */
bool should_clean_userdb(const std::string& db_name,
                         const std::unordered_set<std::string>& cleanup_set) {
  // 如果清理集合为空，则清理所有
  return cleanup_set.empty() || cleanup_set.count(db_name) > 0;
}

/**
 * 从路径中提取userdb名称
 */
std::string extract_userdb_name(const fs::path& path) {
  std::string filename = path.filename().string();
  
  if (fs::is_directory(path) && has_suffix(filename, kUserdbSuffix, kUserdbSuffixLen)) {
    return filename.substr(0, filename.size() - kUserdbSuffixLen);
  }
  
  if (has_suffix(filename, kUserdbTxtSuffix, kUserdbTxtSuffixLen)) {
    return filename.substr(0, filename.size() - kUserdbTxtSuffixLen);
  }
  
  return filename;
}

/**
 * 获取目录下所有的 .userdb 文件夹（根据清理列表过滤）
 */
std::vector<fs::path> get_userdb_folders(const fs::path& dir, const std::unordered_set<std::string>& cleanup_set, std::vector<std::string>& cleaned_folders) {
  std::vector<fs::path> result;
  if (!fs::exists(dir)) {
    LOG(INFO) << "No .userdb folders found in directory: " << dir.string();
    return result;
  }
  if (!fs::is_directory(dir)) {
    return result;
  }
  
  int folder_count = 0;
  int filtered_count = 0;
  for (const auto& entry : fs::directory_iterator(dir)) {
    try {
      if (entry.is_directory()) {
        const auto& path = entry.path();
        const std::string folder_name = path.filename().string();
        // 匹配以 .userdb 结尾的文件夹
        if (has_suffix(folder_name, kUserdbSuffix, kUserdbSuffixLen)) {
          std::string db_name = extract_userdb_name(path);
          if (should_clean_userdb(db_name, cleanup_set)) {
            result.push_back(path);
            // 去重添加，并添加后缀
            std::string full_name = db_name + ".userdb";
            if (std::find(cleaned_folders.begin(), cleaned_folders.end(), full_name) == cleaned_folders.end()) {
              cleaned_folders.push_back(full_name);
            }
            folder_count++;
            LOG(INFO) << "Including folder in cleanup: " << folder_name << " (db_name: " << db_name << ")";
          } else {
            filtered_count++;
            LOG(INFO) << "Skipping folder (not in cleanup list): " << folder_name << " (db_name: " << db_name << ")";
          }
        }
      }
    } catch (const fs::filesystem_error& e) {
      LOG(ERROR) << "Failed to get .userdb folders. Error: " << e.what();
    }
  }
  LOG(INFO) << "Found " << folder_count << " .userdb folders (" << filtered_count << " filtered out)";
  return result;
}

/**
 * 清理用户目录下的 .userdb 文件夹
 */
int clean_userdb_folders(const std::unordered_set<std::string>& cleanup_set, std::vector<std::string>& cleaned_folders) {
  // 使用 get_user_data_dir_s 获取用户数据目录
  char user_data_dir[1024] = {0};
  rime_get_api()->get_user_data_dir_s(user_data_dir, sizeof(user_data_dir));
  
  LOG(INFO) << "Cleaning userdb folders in: " << user_data_dir;
  LOG(INFO) << "Cleanup set size: " << cleanup_set.size();
  if (!cleanup_set.empty()) {
    LOG(INFO) << "Cleanup set contents:";
    for (const auto& db : cleanup_set) {
      LOG(INFO) << "  - " << db;
    }
  }
  
  auto folders = get_userdb_folders(user_data_dir, cleanup_set, cleaned_folders);
  int deleted_files_count = 0;
  
  if (!folders.empty()) {
    for (const auto& folder : folders) {
      LOG(INFO) << "Processing folder: " << folder.string();
      for (const auto& entry : fs::directory_iterator(folder)) {
        try {
          fs::remove(entry.path());
          deleted_files_count++;
          LOG(INFO) << "Deleted file: " << entry.path().string();
        } catch (const fs::filesystem_error& e) {
          LOG(ERROR) << "Failed to delete '" << entry.path().string() << "'. Error: " << e.what();
        }
      }
    }
  }
  
  LOG(INFO) << "Cleaned " << deleted_files_count << " files from " << cleaned_folders.size() << " userdb folders";
  return deleted_files_count;
}

/**
 * 递归获取 sync 目录下所有子目录中的 .userdb.txt 文件（根据清理列表过滤）
 */
std::vector<fs::path> get_userdb_files(const std::unordered_set<std::string>& cleanup_set, std::vector<std::string>& cleaned_files) {
  std::vector<fs::path> result;

  // 使用新的同步目录获取方法
  fs::path sync_path = get_sync_directory();
  
  LOG(INFO) << "Scanning for userdb files in: " << sync_path.string();

  if (!fs::exists(sync_path) || !fs::is_directory(sync_path)) {
    LOG(ERROR) << "Sync directory does not exist: " << sync_path.string();
    return result;
  }

  int file_count = 0;
  int filtered_count = 0;
  
  // 递归遍历 sync 目录下的所有子目录
  for (const auto& entry : fs::recursive_directory_iterator(sync_path)) {
    try {
      if (entry.is_regular_file()) {
        const auto& path = entry.path();
        const std::string file_name = path.filename().string();
        // 匹配以 .userdb.txt 结尾的文件
        if (has_suffix(file_name, kUserdbTxtSuffix, kUserdbTxtSuffixLen)) {
          std::string db_name = extract_userdb_name(path);
          if (should_clean_userdb(db_name, cleanup_set)) {
            result.push_back(path);
            // 去重添加，并添加后缀
            std::string full_name = db_name + ".userdb.txt";
            if (std::find(cleaned_files.begin(), cleaned_files.end(), full_name) == cleaned_files.end()) {
              cleaned_files.push_back(full_name);
            }
            file_count++;
            LOG(INFO) << "Including file in cleanup: " << file_name << " (db_name: " << db_name << ")";
          } else {
            filtered_count++;
            LOG(INFO) << "Skipping file (not in cleanup list): " << file_name << " (db_name: " << db_name << ")";
          }
        }
      }
    } catch (const fs::filesystem_error& e) {
      LOG(ERROR) << "Failed to get .userdb.txt files. Error: " << e.what();
    }
  }
  
  LOG(INFO) << "Found " << file_count << " .userdb.txt files in sync directory and subdirectories (" << filtered_count << " filtered out)";
  return result;
}

/**
 * 从行中提取 c 值并解析为整数
 * @return -1 表示未找到 c 字段或解析失败；否则返回解析出的整数值
 */
int parse_c_value(const std::string& line) {
  size_t pos = line.rfind(kCFieldPrefix);
  if (pos == std::string::npos)
    return -1;

  pos += kCFieldPrefixLen;

  size_t end = pos;
  while (end < line.size() &&
         !std::isspace(static_cast<unsigned char>(line[end]))) {
    end++;
  }

  if (end == pos) return -1;  // no value after "c="

  int value = -1;
  std::from_chars(line.data() + pos, line.data() + end, value);
  return value;
}

/**
 * 从词条行中提取词条文本
 * 格式示例: biàn biàn 	便便	c=1 d=0.00687406 t=31469
 * 返回: 便便
 */
std::string extract_word_text(const std::string& line) {
  // 查找第一个制表符
  size_t first_tab = line.find('\t');
  if (first_tab == std::string::npos) {
    return line;  // 没有制表符，返回整行
  }
  
  // 查找第二个制表符
  size_t second_tab = line.find('\t', first_tab + 1);
  if (second_tab == std::string::npos) {
    // 没有第二个制表符，返回第一个制表符后的内容
    return line.substr(first_tab + 1);
  }
  
  // 返回两个制表符之间的内容（词条文本）
  return line.substr(first_tab + 1, second_tab - first_tab - 1);
}

/**
 * 清理用户目录 sync 下的 .userdb 文件
 * @return 总共清理的无效词条数量
 */
int clean_userdb_files(const std::unordered_set<std::string>& cleanup_set, 
                      int clean_threshold,
                      std::vector<std::string>& cleaned_files, 
                      std::vector<std::string>& deleted_words) {
  auto files = get_userdb_files(cleanup_set, cleaned_files);
  int delete_item_count = 0;
  
  if (!files.empty()) {
    std::string line;
    line.reserve(256);

    for (const auto& file : files) {
      LOG(INFO) << "Processing file: " << file.string();
      
      // 备份文件
      if (!backup_userdb_file(file)) {
        LOG(ERROR) << "Failed to backup file: " << file.string();
        // 继续处理，但不记录删除的词条
        continue;
      }
      
      if (fs::exists(file) && fs::is_regular_file(file)) {
        std::ifstream in(file, std::ios::binary);
        std::string temp_file = file.string() + ".cache";
        std::ofstream out(temp_file, std::ios::binary);
        if (!in.is_open() || !out.is_open()) {
          LOG(ERROR) << "Failed to open file: " << file.string();
          continue;
        }

        int file_deleted_count = 0;
        while (std::getline(in, line)) {
          if (line.empty()) continue;
          // 提取并检查 c 值
          int c_value = parse_c_value(line);
          // 如果找不到 c 字段（c_value == -1），或者 c 值 >= 阈值，则保留该行
          if (c_value == -1 || c_value >= clean_threshold) {
            out << line << "\n";
          } else {
            // 记录删除的词条
            std::string word_text = extract_word_text(line);
            deleted_words.push_back(word_text);
            delete_item_count++;
            file_deleted_count++;
          }
        }

        out.flush();
        out.close();
        in.close();

        try {
            fs::remove(file);
            fs::rename(temp_file, file);
        } catch (const fs::filesystem_error& e) {
            LOG(ERROR) << "Failed to replace file " << file.string() << ": " << e.what();
        }
        LOG(INFO) << "File " << file.filename().string() << ": deleted " << file_deleted_count << " invalid entries (threshold: " << clean_threshold << ")";
      }
    }
  }
  
  // 在日志中打印删除的词条详情
  if (!deleted_words.empty()) {
    LOG(INFO) << "Deleted words (" << deleted_words.size() << " items, threshold: " << clean_threshold << "):";
    for (const auto& word : deleted_words) {
      LOG(INFO) << "  - " << word;
    }
  }
  
  LOG(INFO) << "Total deleted invalid entries from userdb files: " << delete_item_count << " (threshold: " << clean_threshold << ")";
  return delete_item_count;
}

// ---------- 跨平台消息显示函数 ----------
// 将 UTF-8 字符串进行转义（用于 shell 命令）
// Shell-escape a UTF-8 string for safe use in POSIX shell commands.
// Strategy: wrap in single quotes; embedded single quotes become '\''.
static std::string escape_for_shell(const std::string& s) {
    std::string result;
    result.reserve(s.size() + 16);
    result += '\'';
    for (char c : s) {
        if (c == '\'')
            result += "'\\''";
        else
            result += c;
    }
    result += '\'';
    return result;
}

// 跨平台显示消息框（UTF-8 输入）
static void show_message_utf8(const std::string& title, const std::string& message, bool is_info) {
#ifdef _WIN32
    // Windows 平台：转换为 UTF-16 并调用 MessageBoxW
    int wlen = MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, nullptr, 0);
    std::wstring wmessage(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, &wmessage[0], wlen);
    wlen = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
    std::wstring wtitle(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, &wtitle[0], wlen);
    MessageBoxW(nullptr, wmessage.c_str(), wtitle.c_str(), MB_OK | (is_info ? MB_ICONINFORMATION : MB_ICONWARNING));
#elif defined(__APPLE__)
    // macOS：使用 osascript 显示对话框
    std::string escaped_title = escape_for_shell(title);
    std::string escaped_message = escape_for_shell(message);
    std::string cmd = "osascript -e 'display dialog \"" + escaped_message +
                      "\" with title \"" + escaped_title +
                      "\" buttons {\"OK\"} default button \"OK\"' 2>/dev/null";
    if (system(cmd.c_str()) != 0) {
        LOG(INFO) << "Failed to show dialog, fallback to log: " << title << " - " << message;
    }
#elif defined(__linux__)
    // Linux：尝试 zenity → kdialog → notify-send → log
    std::string escaped_title = escape_for_shell(title);
    std::string escaped_message = escape_for_shell(message);
    if (system("command -v zenity > /dev/null 2>&1") == 0) {
        std::string cmd = "zenity --info --title=" + escaped_title +
                          " --text=" + escaped_message + " 2>/dev/null";
        if (system(cmd.c_str()) == 0) return;
    }
    if (system("command -v kdialog > /dev/null 2>&1") == 0) {
        std::string cmd = "kdialog --title " + escaped_title +
                          " --msgbox " + escaped_message + " 2>/dev/null";
        if (system(cmd.c_str()) == 0) return;
    }
    if (system("command -v notify-send > /dev/null 2>&1") == 0) {
        std::string cmd = "notify-send " + escaped_title +
                          " " + escaped_message + " 2>/dev/null";
        if (system(cmd.c_str()) == 0) return;
    }
    LOG(INFO) << title << ": " << message;
#endif
}

/** Build the UTF-8 notification message string (shared by all platforms). */
static std::string build_clean_message_utf8(
    int delete_item_count,
    const std::vector<std::string>& cleaned_folders,
    const std::vector<std::string>& cleaned_files,
    const std::vector<std::string>& deleted_words,
    bool full_information_display) {

    auto join = [](const std::vector<std::string>& items) -> std::string {
        std::string out;
        for (size_t i = 0; i < items.size(); ++i) {
            if (i > 0) out += ", ";
            out += items[i];
        }
        return out;
    };

    std::ostringstream msg;

    if (delete_item_count > 0) {
        msg << u8"用户词典清理完成。\n"
            << u8"删除了 " << delete_item_count << u8" 个无效词条。"
            << u8"\n\n删除的词条已记录到同步目录 userdb_cleaner.txt 文件中。";

        if (full_information_display) {
            msg << u8"\n\n";
            if (!cleaned_folders.empty())
                msg << u8"清理的 userdb 文件夹:\n" << join(cleaned_folders) << u8"\n\n";
            if (!cleaned_files.empty())
                msg << u8"清理的 userdb.txt 文件:\n" << join(cleaned_files) << u8"\n\n";
            if (!deleted_words.empty()) {
                msg << u8"删除的词条:\n";
                for (size_t i = 0; i < deleted_words.size(); ++i) {
                    if (i > 0) {
                        msg << (i % 5 == 0 ? u8"\n" : u8", ");
                    }
                    msg << u8"[ " << deleted_words[i] << u8" ]";
                }
            }
        }
    } else {
        msg << u8"用户词典清理完成。\n未找到需要清理的无效词条。";
        if (full_information_display) {
            msg << u8"\n\n";
            if (!cleaned_folders.empty())
                msg << u8"清理的 userdb 文件夹:\n" << join(cleaned_folders) << u8"\n\n";
            if (!cleaned_files.empty())
                msg << u8"清理的 userdb.txt 文件:\n" << join(cleaned_files);
        }
    }

    return msg.str();
}

/**
 * 发送清理结果通知
 */
void send_clean_msg(int delete_item_count,
                   const std::vector<std::string>& cleaned_folders,
                   const std::vector<std::string>& cleaned_files,
                   const std::vector<std::string>& deleted_words,
                   bool full_information_display) {
#ifdef _WIN32
    std::string utf8_msg = build_clean_message_utf8(delete_item_count, cleaned_folders,
                                                     cleaned_files, deleted_words,
                                                     full_information_display);
    std::string utf8_title = u8"用户词典清理工具";
    
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_msg.c_str(), -1, nullptr, 0);
    std::wstring wmessage(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_msg.c_str(), -1, &wmessage[0], wlen);
    
    int tlen = MultiByteToWideChar(CP_UTF8, 0, utf8_title.c_str(), -1, nullptr, 0);
    std::wstring wtitle(tlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_title.c_str(), -1, &wtitle[0], tlen);
    
    MessageBoxW(nullptr, wmessage.c_str(), wtitle.c_str(), MB_OK | MB_ICONINFORMATION);
#else
    std::string msg = build_clean_message_utf8(delete_item_count, cleaned_folders,
                                                cleaned_files, deleted_words,
                                                full_information_display);
    show_message_utf8(u8"用户词典清理工具", msg, true);
#endif
}

/** Execute Rime user dictionary sync using platform-appropriate method. */
static void execute_sync() {
#ifdef _WIN32
    execute_weasel_deployer("/sync");
#else
    Deployer& deployer = rime::Service::instance().deployer();
    UserDictSync sync_task;
    sync_task.Run(&deployer);
#endif
}

/**
 * 执行清理任务
 */
void process_clean_task(const std::vector<std::string>& cleanup_list,
                       bool full_information_display,
                       int clean_threshold) {
  LOG(INFO) << "Starting userdb cleaning task...";
  LOG(INFO) << "Cleanup list contains " << cleanup_list.size() << " items";
  if (!cleanup_list.empty()) {
    LOG(INFO) << "Cleanup list:";
    for (const auto& db : cleanup_list) {
      LOG(INFO) << "  - " << db;
    }
  }
  LOG(INFO) << "Full information display: " << full_information_display;
  LOG(INFO) << "Clean threshold: " << clean_threshold;
  
  // Pre-clean sync
  LOG(INFO) << "Executing pre-clean sync...";
  execute_sync();

  std::unordered_set<std::string> cleanup_set(cleanup_list.begin(), cleanup_list.end());
  
  std::vector<std::string> cleaned_folders;
  std::vector<std::string> cleaned_files;
  std::vector<std::string> deleted_words;
  
  int folder_deleted_count = clean_userdb_folders(cleanup_set, cleaned_folders);
  int file_deleted_count = clean_userdb_files(cleanup_set, clean_threshold, cleaned_files, deleted_words);
  
  // 记录删除的词条到日志文件
  fs::path sync_dir = get_sync_directory();
  log_deleted_words(deleted_words, sync_dir);
  
  // Post-clean sync
  LOG(INFO) << "Executing post-clean sync...";
  execute_sync();
  
  LOG(INFO) << "Userdb cleaning completed. Total deleted entries: " << file_deleted_count;
  LOG(INFO) << "Cleaned folders: " << cleaned_folders.size();
  LOG(INFO) << "Cleaned files: " << cleaned_files.size();
  LOG(INFO) << "Deleted words: " << deleted_words.size();
  
  send_clean_msg(file_deleted_count, cleaned_folders, cleaned_files, deleted_words, full_information_display);
}

ProcessResult UserdbCleaner::ProcessKeyEvent(const KeyEvent& key_event) {
  auto ctx = engine_->context();
  auto input = ctx->input();
  
  DLOG(INFO) << "UserdbCleaner processing input: " << input << ", trigger: " << trigger_input_;
  
  if (input == trigger_input_) {
    ctx->Clear();
    LOG(INFO) << "UserdbCleaner triggered by input: " << trigger_input_;
    
    // 启动一个线程来执行清理任务，传递清理列表、显示配置和清理阈值
    DetachedThreadManager manager;
    if (manager.try_start([cleanup_list = cleanup_userdb_list_, 
                          full_display = full_information_display_,
                          threshold = clean_threshold_]() { 
      process_clean_task(cleanup_list, full_display, threshold); 
    })) {
      LOG(INFO) << "UserdbCleaner task started successfully";
      return kAccepted;
    } else {
      LOG(ERROR) << "Failed to start UserdbCleaner task - already running";
    }
  }
  return kNoop;
}

}  // namespace rime