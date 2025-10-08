#include <rime/common.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/schema.h>
#include <rime_api.h>

#if defined(_WIN32) || defined(_WIN64)
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <windows.h>
#include <vector>
#include <cstdlib>  // 用于 system 函数
#endif

#include "lib/detached_thread_manager.hpp"
#include "userdb_cleaner.hpp"

namespace fs = std::filesystem;

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
}

#if defined(_WIN32) || defined(_WIN64)
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
        
        // 处理 Windows 路径转义问题
        #if defined(_WIN32) || defined(_WIN64)
        // 如果路径包含转义字符，需要处理
        std::string processed_path = custom_sync_dir;
        // 替换双反斜杠为单反斜杠（处理转义情况）
        size_t pos = 0;
        while ((pos = processed_path.find("\\\\", pos)) != std::string::npos) {
          processed_path.replace(pos, 2, "\\");
          pos += 1;
        }
        sync_path = fs::path(processed_path);
        #endif
        
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
 * 检查是否需要清理指定的userdb
 */
bool should_clean_userdb(const std::string& db_name, const std::vector<std::string>& cleanup_list) {
  // 如果清理列表为空，则清理所有
  if (cleanup_list.empty()) {
    return true;
  }
  
  // 检查是否在清理列表中
  for (const auto& allowed_db : cleanup_list) {
    if (db_name == allowed_db) {
      return true;
    }
  }
  
  return false;
}

/**
 * 从路径中提取userdb名称
 */
std::string extract_userdb_name(const fs::path& path) {
  std::string filename = path.filename().string();
  
  // 处理 .userdb 文件夹
  if (fs::is_directory(path)) {
    const std::string suffix = ".userdb";
    if (filename.length() > suffix.length() && 
        filename.substr(filename.length() - suffix.length()) == suffix) {
      return filename.substr(0, filename.length() - suffix.length());
    }
  }
  
  // 处理 .userdb.txt 文件
  const std::string suffix = ".userdb.txt";
  if (filename.length() > suffix.length() && 
      filename.substr(filename.length() - suffix.length()) == suffix) {
    return filename.substr(0, filename.length() - suffix.length());
  }
  
  return filename; // 返回原始文件名
}

/**
 * 获取目录下所有的 .userdb 文件夹（根据清理列表过滤）
 */
std::vector<fs::path> get_userdb_folders(const fs::path& dir, const std::vector<std::string>& cleanup_list, std::vector<std::string>& cleaned_folders) {
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
        const std::string suffix = ".userdb";
        const size_t suffix_len = suffix.length();
        const size_t name_len = folder_name.length();
        if (name_len > suffix_len &&
            folder_name.substr(name_len - suffix_len) == suffix) {
          std::string db_name = extract_userdb_name(path);
          if (should_clean_userdb(db_name, cleanup_list)) {
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
int clean_userdb_folders(const std::vector<std::string>& cleanup_list, std::vector<std::string>& cleaned_folders) {
  // 使用 get_user_data_dir_s 获取用户数据目录
  char user_data_dir[1024] = {0};
  rime_get_api()->get_user_data_dir_s(user_data_dir, sizeof(user_data_dir));
  
  LOG(INFO) << "Cleaning userdb folders in: " << user_data_dir;
  LOG(INFO) << "Cleanup list size: " << cleanup_list.size();
  if (!cleanup_list.empty()) {
    LOG(INFO) << "Cleanup list contents:";
    for (const auto& db : cleanup_list) {
      LOG(INFO) << "  - " << db;
    }
  }
  
  auto folders = get_userdb_folders(user_data_dir, cleanup_list, cleaned_folders);
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
std::vector<fs::path> get_userdb_files(const std::vector<std::string>& cleanup_list, std::vector<std::string>& cleaned_files) {
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
        const std::string suffix = ".userdb.txt";
        const size_t suffix_len = suffix.length();
        const size_t name_len = file_name.length();
        if (name_len > suffix_len &&
            file_name.substr(name_len - suffix_len) == suffix) {
          std::string db_name = extract_userdb_name(path);
          if (should_clean_userdb(db_name, cleanup_list)) {
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
 * 从行中提取 c 值并解析
 */
double parse_c_value(const std::string& line) {
  // 从后往前查找"c="
  size_t pos = line.rfind("c=");
  if (pos == std::string::npos)
    return 1.0;  // 未找到 c 字段, 保留该行

  // 移动到c值起始位置 (跳过"c=")
  pos += 2;

  // 查找c值结束位置 (空格/制表符/行尾)
  size_t end = pos;
  while (end < line.size() &&
         !std::isspace(static_cast<unsigned char>(line[end]))) {
    end++;
  }

  double value = -1.0;
  auto [ptr, ec] = std::from_chars(line.data() + pos, line.data() + end, value);

  // 检查解析是否成功
  if (ec != std::errc() || ptr != line.data() + end) {
    try {
      return std::stod(line.substr(pos, end - pos));
    } catch (...) {
      return 1.0;  // 解析失败, 保留该行
    }
  }
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
int clean_userdb_files(const std::vector<std::string>& cleanup_list, std::vector<std::string>& cleaned_files, std::vector<std::string>& deleted_words) {
  auto files = get_userdb_files(cleanup_list, cleaned_files);
  int delete_item_count = 0;
  
  if (!files.empty()) {
    std::string line;
    line.reserve(256);

    for (const auto& file : files) {
      LOG(INFO) << "Processing file: " << file.string();
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
          double c_value = parse_c_value(line);
          // 把 c > 0 的行写入新文件
          if (c_value > 0.0) {
            out << line << "\n";
          } else {
            // 记录删除的词条
            std::string word_text = extract_word_text(line);
            deleted_words.push_back(word_text);
            delete_item_count++;
            file_deleted_count++;
          }
          line.clear();
        }

        out.flush();
        out.close();
        in.close();

        fs::remove(file);
        std::string new_file = file.string();
        fs::rename(temp_file, new_file);
        
        LOG(INFO) << "File " << file.filename().string() << ": deleted " << file_deleted_count << " invalid entries";
      }
    }
  }
  
  // 在日志中打印删除的词条详情
  if (!deleted_words.empty()) {
    LOG(INFO) << "Deleted words (" << deleted_words.size() << " items):";
    for (const auto& word : deleted_words) {
      LOG(INFO) << "  - " << word;
    }
  }
  
  LOG(INFO) << "Total deleted invalid entries from userdb files: " << delete_item_count;
  return delete_item_count;
}

/**
 * 发送清理结果通知
 */
void send_clean_msg(const int& delete_item_count, 
                   const std::vector<std::string>& cleaned_folders,
                   const std::vector<std::string>& cleaned_files,
                   const std::vector<std::string>& deleted_words,
                   bool full_information_display) {
#if defined(_WIN32) || defined(_WIN64)
  // 使用 Unicode 转义序列避免编码问题
  std::wstring title = L"\u7528\u6237\u8bcd\u5178\u6e05\u7406\u5de5\u5177"; // 用户词典清理工具
  std::wstring message;
  
  if (delete_item_count > 0) {
    message = L"\u7528\u6237\u8bcd\u5178\u6e05\u7406\u5b8c\u6210\u3002\n"; // 用户词典清理完成。
    message += L"\u5220\u9664\u4e86 " + std::to_wstring(delete_item_count) + L" \u4e2a\u65e0\u6548\u8bcd\u6761\u3002"; // 删除了 X 个无效词条。
    
    // 如果启用了完整信息显示，则显示详细信息
    if (full_information_display) {
      message += L"\n\n";
      
      // 显示清理的文件夹
      if (!cleaned_folders.empty()) {
        message += L"\u6e05\u7406\u7684 userdb \u6587\u4ef6\u5939:\n"; // 清理的 userdb 文件夹:
        for (size_t i = 0; i < cleaned_folders.size(); ++i) {
          if (i > 0) message += L", ";
          // 将字符串转换为宽字符串
          std::string db_name = cleaned_folders[i];
          int wide_length = MultiByteToWideChar(CP_UTF8, 0, db_name.c_str(), -1, nullptr, 0);
          if (wide_length > 0) {
            std::wstring wide_db_name(wide_length, 0);
            MultiByteToWideChar(CP_UTF8, 0, db_name.c_str(), -1, &wide_db_name[0], wide_length);
            if (!wide_db_name.empty() && wide_db_name.back() == L'\0') {
              wide_db_name.pop_back();
            }
            message += wide_db_name;
          }
        }
        message += L"\n\n";
      }
      
      // 显示清理的文件
      if (!cleaned_files.empty()) {
        message += L"\u6e05\u7406\u7684 userdb.txt \u6587\u4ef6:\n"; // 清理的 userdb.txt 文件:
        for (size_t i = 0; i < cleaned_files.size(); ++i) {
          if (i > 0) message += L", ";
          // 将字符串转换为宽字符串
          std::string db_name = cleaned_files[i];
          int wide_length = MultiByteToWideChar(CP_UTF8, 0, db_name.c_str(), -1, nullptr, 0);
          if (wide_length > 0) {
            std::wstring wide_db_name(wide_length, 0);
            MultiByteToWideChar(CP_UTF8, 0, db_name.c_str(), -1, &wide_db_name[0], wide_length);
            if (!wide_db_name.empty() && wide_db_name.back() == L'\0') {
              wide_db_name.pop_back();
            }
            message += wide_db_name;
          }
        }
        message += L"\n\n";
      }
      
      // 显示删除的词条（每行最多5个，用方括号括起来）
      if (!deleted_words.empty()) {
        message += L"\u5220\u9664\u7684\u8bcd\u6761:\n"; // 删除的词条:
        for (size_t i = 0; i < deleted_words.size(); ++i) {
          if (i > 0) {
            if (i % 5 == 0) {
              message += L"\n"; // 每5个词条换行
            } else {
              message += L", ";
            }
          }
          // 将词条转换为宽字符串并用方括号括起来
          std::string word = deleted_words[i];
          int wide_length = MultiByteToWideChar(CP_UTF8, 0, word.c_str(), -1, nullptr, 0);
          if (wide_length > 0) {
            std::wstring wide_word(wide_length, 0);
            MultiByteToWideChar(CP_UTF8, 0, word.c_str(), -1, &wide_word[0], wide_length);
            if (!wide_word.empty() && wide_word.back() == L'\0') {
              wide_word.pop_back();
            }
            message += L"[" + wide_word + L"]";
          }
        }
      }
    }
  } else {
    message = L"\u7528\u6237\u8bcd\u5178\u6e05\u7406\u5b8c\u6210\u3002\n"; // 用户词典清理完成。
    message += L"\u672a\u627e\u5230\u9700\u8981\u6e05\u7406\u7684\u65e0\u6548\u8bcd\u6761\u3002"; // 未找到需要清理的无效词条。
    
    // 如果启用了完整信息显示，则显示清理的文件信息
    if (full_information_display) {
      message += L"\n\n";
      
      // 即使没有删除词条，也显示清理了哪些文件
      if (!cleaned_folders.empty()) {
        message += L"\u6e05\u7406\u7684 userdb \u6587\u4ef6\u5939:\n"; // 清理的 userdb 文件夹:
        for (size_t i = 0; i < cleaned_folders.size(); ++i) {
          if (i > 0) message += L", ";
          std::string db_name = cleaned_folders[i];
          int wide_length = MultiByteToWideChar(CP_UTF8, 0, db_name.c_str(), -1, nullptr, 0);
          if (wide_length > 0) {
            std::wstring wide_db_name(wide_length, 0);
            MultiByteToWideChar(CP_UTF8, 0, db_name.c_str(), -1, &wide_db_name[0], wide_length);
            if (!wide_db_name.empty() && wide_db_name.back() == L'\0') {
              wide_db_name.pop_back();
            }
            message += wide_db_name;
          }
        }
        message += L"\n\n";
      }
      
      if (!cleaned_files.empty()) {
        message += L"\u6e05\u7406\u7684 userdb.txt \u6587\u4ef6:\n"; // 清理的 userdb.txt 文件:
        for (size_t i = 0; i < cleaned_files.size(); ++i) {
          if (i > 0) message += L", ";
          std::string db_name = cleaned_files[i];
          int wide_length = MultiByteToWideChar(CP_UTF8, 0, db_name.c_str(), -1, nullptr, 0);
          if (wide_length > 0) {
            std::wstring wide_db_name(wide_length, 0);
            MultiByteToWideChar(CP_UTF8, 0, db_name.c_str(), -1, &wide_db_name[0], wide_length);
            if (!wide_db_name.empty() && wide_db_name.back() == L'\0') {
              wide_db_name.pop_back();
            }
            message += wide_db_name;
          }
        }
      }
    }
  }
  
  MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
#elif __APPLE__
  // macOS 实现
  if (delete_item_count > 0) {
    LOG(INFO) << "用户词典清理完成。删除了 " << delete_item_count << " 个无效词条。";
    if (full_information_display) {
      if (!cleaned_folders.empty()) {
        LOG(INFO) << "清理的 userdb 文件夹: " << cleaned_folders.size();
      }
      if (!cleaned_files.empty()) {
        LOG(INFO) << "清理的 userdb.txt 文件: " << cleaned_files.size();
      }
      if (!deleted_words.empty()) {
        LOG(INFO) << "删除的词条数量: " << deleted_words.size();
      }
    }
  } else {
    LOG(INFO) << "用户词典清理完成。未找到需要清理的无效词条。";
  }
#elif __linux__
  // Linux 实现
  if (delete_item_count > 0) {
    LOG(INFO) << "用户词典清理完成。删除了 " << delete_item_count << " 个无效词条。";
    if (full_information_display) {
      if (!cleaned_folders.empty()) {
        LOG(INFO) << "清理的 userdb 文件夹: " << cleaned_folders.size();
      }
      if (!cleaned_files.empty()) {
        LOG(INFO) << "清理的 userdb.txt 文件: " << cleaned_files.size();
      }
      if (!deleted_words.empty()) {
        LOG(INFO) << "删除的词条数量: " << deleted_words.size();
      }
    }
  } else {
    LOG(INFO) << "用户词典清理完成。未找到需要清理的无效词条。";
  }
#endif
}

/**
 * 执行清理任务
 */
void process_clean_task(const std::vector<std::string>& cleanup_list, bool full_information_display) {
  LOG(INFO) << "Starting userdb cleaning task...";
  LOG(INFO) << "Cleanup list contains " << cleanup_list.size() << " items";
  if (!cleanup_list.empty()) {
    LOG(INFO) << "Cleanup list:";
    for (const auto& db : cleanup_list) {
      LOG(INFO) << "  - " << db;
    }
  }
  LOG(INFO) << "Full information display: " << full_information_display;
  
#if defined(_WIN32) || defined(_WIN64)
  // 清理前先执行 sync
  LOG(INFO) << "Executing pre-clean deployment...";
  execute_weasel_deployer("/sync");
#endif
  
  std::vector<std::string> cleaned_folders;
  std::vector<std::string> cleaned_files;
  std::vector<std::string> deleted_words;
  
  int folder_deleted_count = clean_userdb_folders(cleanup_list, cleaned_folders);
  int file_deleted_count = clean_userdb_files(cleanup_list, cleaned_files, deleted_words);
  
  // 通知中只显示删除的词条总数（file_deleted_count）
  int total_notification_count = file_deleted_count;
  
#if defined(_WIN32) || defined(_WIN64)
  // 清理后执行 sync
  LOG(INFO) << "Executing post-clean deployment...";
  execute_weasel_deployer("/sync");
#endif
  
  LOG(INFO) << "Userdb cleaning completed. Total deleted entries: " << file_deleted_count;
  LOG(INFO) << "Cleaned folders: " << cleaned_folders.size();
  LOG(INFO) << "Cleaned files: " << cleaned_files.size();
  LOG(INFO) << "Deleted words: " << deleted_words.size();
  
  send_clean_msg(total_notification_count, cleaned_folders, cleaned_files, deleted_words, full_information_display);
}

ProcessResult UserdbCleaner::ProcessKeyEvent(const KeyEvent& key_event) {
#if defined(_WIN32) || defined(_WIN64)
  auto ctx = engine_->context();
  auto input = ctx->input();
  
  DLOG(INFO) << "UserdbCleaner processing input: " << input << ", trigger: " << trigger_input_;
  
  if (input == trigger_input_) {
    ctx->Clear();
    LOG(INFO) << "UserdbCleaner triggered by input: " << trigger_input_;
    
    // 启动一个线程来执行清理任务，传递清理列表和显示配置
    DetachedThreadManager manager;
    if (manager.try_start([cleanup_list = cleanup_userdb_list_, full_display = full_information_display_]() { 
      process_clean_task(cleanup_list, full_display); 
    })) {
      LOG(INFO) << "UserdbCleaner task started successfully";
      return kAccepted;
    } else {
      LOG(ERROR) << "Failed to start UserdbCleaner task - already running";
    }
  }
#endif
  return kNoop;
}

}  // namespace rime