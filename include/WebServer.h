#pragma once

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <cerrno>
#include <memory>   
#include <mutex>
#include <chrono>
#include "Timer.h"
#include "ThreadPool.h"

// 1. 状态机核心枚举

// 主状态机: 当前正在解析 HTTP 报文的哪个部分
enum ParseState {
    STATE_REQUEST_LINE, // 正在解析请求行 (GET / HTTP/1.1)
    STATE_HEADERS,      // 正在解析请求头 (Host: 127.0.0.1)
    STATE_BODY,         // 正在解析请求体 (POST的数据)  
    STATE_FINISH        // 整个报文解析完成
};

// 从状态机: 当前这一行数据到底读全了没有
enum LineStatus {
    LINE_OK,    // 完美，找到了 \r\n, 读取到完整的一行
    LINE_BAD,   // 报文格式不对
    LINE_OPEN   // 还没遇到 \r\n, 这是个半包(Half Packet), 需要等下一次网络收水
};

// 2. 客人专属档案卡 (HttpConnection)
struct HttpConnection {
    int fd;
    std::string read_buffer;    // 存放从系统抽出来的原始数据
    std::string write_buffer;
    size_t write_offset;
    ParseState state;           // 记录上次解析到了哪一步

    std::string method;
    std::string url;
    std::string version;
    std::string body_;
    size_t content_length_;
    bool keep_alive_;
    std::unordered_map<std::string, std::string> headers;   // 存放所有头部字段

    HttpConnection() : fd(-1), state(STATE_REQUEST_LINE), content_length_(0), keep_alive_(false), write_offset(0) {}

    void Reset() {
        state = STATE_REQUEST_LINE;
        method.clear();
        url.clear();
        version.clear();
        headers.clear();
        body_.clear();
        content_length_ = 0;
        // read_buffer 不能清空, 里面可能已经有了下个请求的半包数据 (Pipeline)
    }
};

// 3. 主类
class WebServer {
public:
    // Constructor and Destructor
    WebServer(int port);
    ~WebServer();

    // Core lifestyle method
    void Init();
    void Start();

private:
    // Helper tools for network and file I/O
    void SetNonBlocking(int fd);
    void HandleClient(int client_fd);
    void CloseConn(int client_fd);
    void ResetEpollOneshot(int fd, bool write_mode = false); 
    void HandleWrite(int client_fd);
    std::string ReadFile(const std::string& path);
    std::string GetMimeType(const std::string& path);
    // Member variables
    int port_;
    int server_fd_;
    int epoll_fd_;
    bool is_running_;
    std::chrono::steady_clock::time_point last_tick_;
    // We can handle up to 1024 events simultaneously per wake-up
    static const int MAX_EVENTS = 1024;
    struct epoll_event events_[MAX_EVENTS];
    
    // key 是 client_fd, value 是客人的专属档案
    std::vector<HttpConnection> clients_;
    HeapTimer timer_;
    std::unique_ptr<ThreadPool> thread_pool_;
    mutable std::recursive_mutex mtx_;
};
