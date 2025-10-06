#include <rime/common.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
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
  
  if (auto schema = engine_->schema()) {
    if (auto config = schema->config()) {
      // 读取触发输入配置
      if (!config->GetString("userdb_cleaner/trigger_input", &trigger_input_)) {
        LOG(INFO) << "userdb_cleaner/trigger_input not set, using default: " << trigger_input_;
      } else {
        LOG(INFO) << "UserdbCleaner trigger_input: " << trigger_input_;
      }
    } else {
      LOG(ERROR) << "Failed to get config in UserdbCleaner";
    }
  } else {
    LOG(ERROR) << "Failed to get schema in UserdbCleaner";
  }
}

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
 */
int clean_userdb_folders() {
  auto user_data_dir = rime_get_api()->get_user_data_dir();
  LOG(INFO) << "Cleaning userdb folders in: " << user_data_dir;
  
  auto folders = get_userdb_folders(user_data_dir);
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
  
  LOG(INFO) << "Cleaned " << deleted_files_count << " files from userdb folders";
  return deleted_files_count;
}

/**
 * 获取 sync 目录下所有的 .userdb.txt 文件
 */
std::vector<fs::path> get_userdb_files() {
  std::vector<fs::path> result;

  auto dir = fs::path(rime_get_api()->get_user_data_dir());
  if (!fs::is_directory(dir) || !fs::exists(dir)) {
    LOG(ERROR) << "User data directory does not exist: " << dir.string();
    return result;
  }
  
  auto inst_file = dir / "installation.yaml";
  auto sync_dir = dir / "sync";
  Config config;
  std::string installation_id;
  
  if (config.LoadFromFile(inst_file)) {
    config.GetString("installation_id", &installation_id);
  } else {
    LOG(ERROR) << "Failed to load installation.yaml";
    return result;
  }

  if (installation_id.empty()) {
    LOG(ERROR) << "Installation ID is empty";
    return result;
  }
  
  sync_dir = sync_dir / installation_id;
  LOG(INFO) << "Scanning for userdb files in: " << sync_dir.string();

  if (!fs::exists(sync_dir) || !fs::is_directory(sync_dir)) {
    LOG(ERROR) << "Sync directory does not exist: " << sync_dir.string();
    return result;
  }

  int file_count = 0;
  for (const auto& entry : fs::directory_iterator(sync_dir)) {
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
 * 清理用户目录 sync 下的 .userdb 文件
 * @return 总共清理的无效词条数量
 */
int clean_userdb_files() {
  auto files = get_userdb_files();
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
  
  LOG(INFO) << "Total deleted invalid entries from userdb files: " << delete_item_count;
  return delete_item_count;
}

/**
 * 发送清理结果通知
 */
void send_clean_msg(const int& delete_item_count) {
#if defined(_WIN32) || defined(_WIN64)
  auto content = L"用户词典共清理  行无效词条";
  std::wstring str = content;
  std::wstringstream wss;
  wss << delete_item_count;
  str.insert(8, wss.str());
  MessageBoxW(NULL, str.c_str(), L"通知", MB_OK);
#elif __APPLE__
#elif __linux__
#endif
}

/**
 * 执行清理任务
 */
void process_clean_task() {
  LOG(INFO) << "Starting userdb cleaning task...";
  
  int folder_deleted_count = clean_userdb_folders();
  int file_deleted_count = clean_userdb_files();
  int total_deleted_count = folder_deleted_count + file_deleted_count;
  
  LOG(INFO) << "Userdb cleaning completed. Total deleted entries: " << total_deleted_count;
  send_clean_msg(total_deleted_count);
}

ProcessResult UserdbCleaner::ProcessKeyEvent(const KeyEvent& key_event) {
#if defined(_WIN32) || defined(_WIN64)
  auto ctx = engine_->context();
  auto input = ctx->input();
  
  DLOG(INFO) << "UserdbCleaner processing input: " << input << ", trigger: " << trigger_input_;
  
  if (input == trigger_input_) {
    ctx->Clear();
    LOG(INFO) << "UserdbCleaner triggered by input: " << trigger_input_;
    
    // 启动一个线程来执行清理任务, 避免系统等待用户关闭窗口导致系统阻塞
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
