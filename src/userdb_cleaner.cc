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
}

#if defined(_WIN32) || defined(_WIN64)
/**
 * 执行 WeaselDeployer 命令
 */
bool execute_weasel_deployer(const std::string& argument) {
  // 获取共享数据目录（程序目录）
  char shared_data_dir[1024] = {0};
  rime_get_api()->get_shared_data_dir_s(shared_data_dir, sizeof(shared_data_dir));
  
  fs::path deployer_path = fs::path(shared_data_dir) / "WeaselDeployer.exe";
  
  if (!fs::exists(deployer_path)) {
    LOG(ERROR) << "WeaselDeployer.exe not found at: " << deployer_path.string();
    return false;
  }
  
  std::string command = "\"" + deployer_path.string() + "\" " + argument;
  LOG(INFO) << "Executing: " << command;
  
  int result = std::system(command.c_str());
  if (result != 0) {
    LOG(ERROR) << "WeaselDeployer execution failed with code: " << result;
    return false;
  }
  
  LOG(INFO) << "WeaselDeployer executed successfully: " << argument;
  return true;
}
#endif

/**
 * 获取目录下所有的 .userdb 文件夹
 */
std::vector<fs::path> get_userdb_folders(const fs::path& dir) {
  std::vector<fs::path> result;
  if (!fs::exists(dir)) {
    LOG(INFO) << "No .userdb folders found in directory: " << dir.string();
    return result;
  }
  if (!fs::is_directory(dir)) {
    return result;
  }
  
  int folder_count = 0;
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
          result.push_back(path);
          folder_count++;
        }
      }
    } catch (const fs::filesystem_error& e) {
      LOG(ERROR) << "Failed to get .userdb folders. Error: " << e.what();
    }
  }
  LOG(INFO) << "Found " << folder_count << " .userdb folders";
  return result;
}

/**
 * 清理用户目录下的 .userdb 文件夹
 * @return 删除的文件数量（仅用于日志，不计入词条统计）
 */
int clean_userdb_folders() {
  // 使用 get_user_data_dir_s 获取用户数据目录
  char user_data_dir[1024] = {0};
  rime_get_api()->get_user_data_dir_s(user_data_dir, sizeof(user_data_dir));
  
  LOG(INFO) << "Cleaning userdb folders in: " << user_data_dir;
  
  auto folders = get_userdb_folders(user_data_dir);
  int deleted_files_count = 0;
  
  if (!folders.empty()) {
    for (const auto& folder : folders) {
      LOG(INFO) << "Processing folder: " << folder.string();
      for (const auto& entry : fs::directory_iterator(folder)) {
        try {
          LOG(INFO) << "Deleting file: " << entry.path().string();
          fs::remove(entry.path());
          deleted_files_count++;
        } catch (const fs::filesystem_error& e) {
          LOG(ERROR) << "Failed to delete '" << entry.path().string() << "'. Error: " << e.what();
        }
      }
    }
  }
  
  LOG(INFO) << "Cleaned " << deleted_files_count << " files from userdb folders";
  return deleted_files_count;
}

/**
 * 从行中提取词条文本（如 "便便"）
 */
std::string extract_entry_text(const std::string& line) {
  // 格式示例: biàn biàn 	便便	c=1 d=0.00687406 t=31469
  // 查找制表符分隔的部分
  size_t first_tab = line.find('\t');
  if (first_tab == std::string::npos) {
    return line; // 如果没有制表符，返回整行
  }
  
  size_t second_tab = line.find('\t', first_tab + 1);
  if (second_tab == std::string::npos) {
    // 只有一个制表符，取后面的部分
    return line.substr(first_tab + 1);
  }
  
  // 有两个制表符，取中间的部分
  return line.substr(first_tab + 1, second_tab - first_tab - 1);
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
 * 获取 sync 目录下所有的 .userdb.txt 文件
 */
std::vector<fs::path> get_userdb_files() {
  std::vector<fs::path> result;

  // 使用 get_sync_dir_s 获取 sync 目录
  char sync_dir[1024] = {0};
  rime_get_api()->get_sync_dir_s(sync_dir, sizeof(sync_dir));
  
  fs::path sync_path(sync_dir);
  LOG(INFO) << "Scanning for userdb files in: " << sync_path.string();

  if (!fs::exists(sync_path) || !fs::is_directory(sync_path)) {
    LOG(ERROR) << "Sync directory does not exist: " << sync_path.string();
    return result;
  }

  int file_count = 0;
  for (const auto& entry : fs::directory_iterator(sync_path)) {
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
          result.push_back(path);
          file_count++;
        }
      }
    } catch (const fs::filesystem_error& e) {
      LOG(ERROR) << "Failed to get .userdb.txt files. Error: " << e.what();
    }
  }
  
  LOG(INFO) << "Found " << file_count << " .userdb.txt files";
  return result;
}

/**
 * 清理用户目录 sync 下的 .userdb 文件
 * @return 总共清理的无效词条数量
 */
int clean_userdb_files() {
  auto files = get_userdb_files();
  int delete_item_count = 0;
  std::vector<std::string> deleted_entries; // 记录被删除的词条
  
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
            // 记录被删除的词条
            std::string entry_text = extract_entry_text(line);
            deleted_entries.push_back(entry_text);
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
  
  // 在日志中记录所有被删除的词条
  if (!deleted_entries.empty()) {
    LOG(INFO) << "Deleted entries details:";
    for (const auto& entry : deleted_entries) {
      LOG(INFO) << "  - " << entry;
    }
  }
  
  LOG(INFO) << "Total deleted invalid entries from userdb files: " << delete_item_count;
  return delete_item_count;
}

/**
 * 发送清理结果通知
 */
void send_clean_msg(const int& delete_item_count) {
#if defined(_WIN32) || defined(_WIN64)
  std::wstringstream wss;
  wss << L"User dictionary cleaning completed.\n";
  wss << L"Deleted " << delete_item_count << L" invalid entries.";
  
  MessageBoxW(NULL, wss.str().c_str(), L"UserDB Cleaner", MB_OK | MB_ICONINFORMATION);
#elif __APPLE__
  // macOS 通知实现
  LOG(INFO) << "User dictionary cleaning completed. Deleted " << delete_item_count << " invalid entries.";
#elif __linux__
  // Linux 通知实现
  LOG(INFO) << "User dictionary cleaning completed. Deleted " << delete_item_count << " invalid entries.";
#endif
}

/**
 * 执行清理任务
 */
void process_clean_task() {
  LOG(INFO) << "Starting userdb cleaning task...";
  
#if defined(_WIN32) || defined(_WIN64)
  // 清理前先执行 deploy 和 sync
  LOG(INFO) << "Executing pre-clean deployment...";
  execute_weasel_deployer("/deploy");
  execute_weasel_deployer("/sync");
#endif
  
  // 清理 .userdb 文件夹（只记录日志，不统计到词条数）
  int folder_deleted_count = clean_userdb_folders();
  
  // 清理 .userdb.txt 文件，统计删除的词条数
  int file_deleted_count = clean_userdb_files();
  
#if defined(_WIN32) || defined(_WIN64)
  // 清理后执行 sync 和 deploy
  LOG(INFO) << "Executing post-clean deployment...";
  execute_weasel_deployer("/sync");
  execute_weasel_deployer("/deploy");
#endif
  
  // 只统计删除的词条数量，不包含文件夹中的文件数量
  LOG(INFO) << "Userdb cleaning completed. Deleted " << file_deleted_count << " invalid entries.";
  send_clean_msg(file_deleted_count);
}

ProcessResult UserdbCleaner::ProcessKeyEvent(const KeyEvent& key_event) {
#if defined(_WIN32) || defined(_WIN64)
  auto ctx = engine_->context();
  auto input = ctx->input();
  
  DLOG(INFO) << "UserdbCleaner processing input: " << input << ", trigger: " << trigger_input_;
  
  if (input == trigger_input_) {
    ctx->Clear();
    LOG(INFO) << "UserdbCleaner triggered by input: " << trigger_input_;
    
    // 启动一个线程来执行清理任务
    DetachedThreadManager manager;
    if (manager.try_start(process_clean_task)) {
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
