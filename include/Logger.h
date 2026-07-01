#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <fstream>
#include <chrono>
#include <ctime>

// ─── 日志等级 ────────────────────────────────────────────────
enum LogLevel {
    DEBUG = 0,
    INFO  = 1,
    WARN  = 2,
    ERROR = 3
};

// ─── 异步日志器（双缓冲 + 后台线程刷盘）───────────────────────
//
//  生产者（任意线程）→ 往 cur_buf_ 里追加（持锁）
//  消费者（flush_thread）→ 把 cur_buf_ swap 到 flush_buf_（持锁）
//                        → 把 flush_buf_ 写到磁盘（不持锁）
//
//  两种触发刷盘的时机：
//    1. cur_buf_ 满了（>= max_buf_size_）→ 立刻通知消费者
//    2. 超过 flush_interval_ms_ 毫秒没刷 → 超时自动唤醒
//
class Logger {
public:
    static Logger& Instance();       // 单例入口（Meyers' Singleton）

    // 启动后台刷盘线程。重复调用无副作用。
    void Init(const std::string& log_path      = "server.log",
              int max_buf_size                 = 1024,
              int flush_interval_ms            = 3000);

    // 写入一条日志（线程安全）。未 Init 时退化为 stderr 输出。
    void Log(LogLevel level, const std::string& msg);

    // 刷完残留日志 → 停止线程 → 关闭文件
    void Stop();

private:
    Logger();
    ~Logger();
    // 禁止拷贝 — 单例不能出现第二个对象
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // 工具函数
    static std::string GetTime();                 // "[2026-06-29 14:30:15]"
    static const char*  LevelStr(LogLevel level); // "[INFO] "
    void FlushLoop();   // 后台线程入口

    // ── 共享状态（由 mtx_ 保护）──
    std::mutex                mtx_;
    std::condition_variable   cond_;
    std::vector<std::string>  cur_buf_;     // 生产者往这里写
    std::vector<std::string>  flush_buf_;   // 消费者从这里刷盘

    // ── 独占资源 ──
    std::ofstream  file_;
    std::thread    flush_thread_;

    bool is_running_          = false;
    int  max_buf_size_        = 1024;    // 触发立即刷盘的阈值
    int  flush_interval_ms_   = 3000;    // 超时刷盘间隔（毫秒）
};

// ─── 便捷宏 ───────────────────────────────────────────────────
//  do-while(0)：保证宏在 if/else 后面安全展开，且强制末尾加分号。

#define LOG_DEBUG(msg) \
    do { Logger::Instance().Log(DEBUG, msg); } while(0)

#define LOG_INFO(msg) \
    do { Logger::Instance().Log(INFO,  msg); } while(0)

#define LOG_WARN(msg) \
    do { Logger::Instance().Log(WARN,  msg); } while(0)

#define LOG_ERROR(msg) \
    do { Logger::Instance().Log(ERROR, msg); } while(0)
