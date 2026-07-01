#include "../include/Logger.h"
#include <iostream>

void Logger::Init(const std::string& log_path, int max_buf_size, int flush_interval_ms) {
    if (is_running_) {
        return;
    }         
    max_buf_size_ = max_buf_size;
    flush_interval_ms_ = flush_interval_ms;
    cur_buf_.reserve(max_buf_size_);
    flush_buf_.reserve(max_buf_size_);

    file_.open(log_path, std::ios::out | std::ios::app);

    if (!file_.is_open()) {
        std::cerr << "[Logger] FATAL: cannot open log file: " << log_path << std::endl;
        return;
    }

    is_running_ = true;
    flush_thread_ = std::thread(&Logger::FlushLoop, this);

    file_ << "[" << GetTime() << "] [INFO]  ===== Logger started =====\n";
    file_.flush();
}

// ═══════════════════════════════════════════════════════════════
//  单例 — Meyers' Singleton，C++11 保证多线程只构造一次
// ═══════════════════════════════════════════════════════════════

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

// ═══════════════════════════════════════════════════════════════
//  Log — 拼一行 → 持锁 → 塞进 cur_buf_ → 满了就通知后台线程
// ═══════════════════════════════════════════════════════════════

void Logger::Log(LogLevel level, const std::string& msg) {
    // 在锁外面拼好字符串，减少临界区长度
    std::string entry = "[" + GetTime() + "]" + LevelStr(level) + " " + msg + "\n";

    std::lock_guard<std::mutex> lock(mtx_);

    // 未初始化时的兜底：直接打到 stderr
    if (!is_running_) {
        std::cerr << entry;
        return;
    }
    // 塞进当前缓冲区，用 move 避免字符串拷贝
    cur_buf_.push_back(std::move(entry));

    // 缓冲区满了就唤醒后台线程，让它赶紧 swap + 刷盘
    if (cur_buf_.size() >= static_cast<size_t>(max_buf_size_)) {
        cond_.notify_one();
    }
}

void Logger::Stop() {
    if (!is_running_) {
        return;
    }
    is_running_ = false;
    cond_.notify_one();
    if (flush_thread_.joinable()) {
        flush_thread_.join();
    }   
    
    if (!cur_buf_.empty()) {
        for (const auto& line : cur_buf_) {
            file_ << line;
        }
        cur_buf_.clear();
    }
    file_ << "[" << GetTime() << "] [INFO] ===== Logger stopped =====\n";
    file_.flush();
    file_.close();
}

Logger::Logger() {
    cur_buf_.reserve(max_buf_size_);
    flush_buf_.reserve(max_buf_size_);
}

Logger::~Logger() {
    Stop();
}
std::string Logger::GetTime() {
    auto now = std::chrono::system_clock::now();
    time_t t = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    std::string time_str = buf;
    return time_str;
}

const char* Logger::LevelStr(LogLevel level) {
    switch (level)
    {
    case DEBUG: return "[DEBUG]";
    case INFO:  return "[INFO] ";
    case WARN:  return "[WARN] ";
    case ERROR: return "[ERROR]";
    default:    return "[UNKN] ";
    }
}

void Logger::FlushLoop() {
    while (is_running_) {
        std::unique_lock<std::mutex> lock(mtx_);
        cond_.wait_for(lock, std::chrono::milliseconds(flush_interval_ms_),[this](){
            return !is_running_ || cur_buf_.size() >= static_cast<size_t>(max_buf_size_);
        }); 
        
        if (!is_running_ && cur_buf_.empty() && flush_buf_.empty()) {
            break;
        }

        if (!cur_buf_.empty()) {
            cur_buf_.swap(flush_buf_);
        }

        lock.unlock();

        for (const auto& line : flush_buf_) {
            file_ << line;
        }
        file_.flush();
        flush_buf_.clear();
    }
}