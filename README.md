# TinyServer — Linux 高性能 HTTP 服务器

[![C++](https://img.shields.io/badge/C%2B%2B-14-blue)](https://isocpp.org/)
[![Linux](https://img.shields.io/badge/Linux-Ubuntu%2024.04-orange)](https://ubuntu.com/)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

从零构建的 C++ HTTP/1.1 Web 服务器，~800 行代码，仅使用 POSIX/Linux 系统级 API（MySQL C API 除外），零框架依赖。

## 为什么写这个项目

现代 Web 开发中，开发者大量使用框架（Spring、Express、Django），底层协议细节被层层封装。这个项目的目的是 **深入理解 HTTP 服务器的工作原理**——从 TCP 字节流到 HTTP 响应，全部手写。

## 核心特性

| 模块 | 实现 | 设计选择 |
|------|------|----------|
| I/O 多路复用 | epoll 边缘触发 (ET) + 非阻塞 I/O | epoll 而非 select/poll：红黑树 + 就绪链表，O(1) 获取就绪 fd，无 1024 上限 |
| HTTP 解析 | 两级有限状态机 | 主状态机（请求行→头部→体）+ 行状态机（\r\n 边界检测），处理 TCP 半包粘包 |
| 并发模型 | 半同步/半异步 Reactor | 主线程 epoll 事件分发 + 8 工作线程处理业务，避免线程 per 连接开销 |
| 线程安全 | EPOLLONESHOT + recursive_mutex | ONESHOT 保证同一 fd 只被一个线程操作；锁保护 clients_ 向量和 timer_ 堆 |
| 长连接 | Keep-Alive + HTTP 管线化 | 一个 TCP 连接处理多个请求，10s 空闲超时自动关闭 |
| 定时器 | 小根堆 (HeapTimer) | 惰性检查：堆顶没到期则下面全安全；O(log n) 插入/删除；支持动态续期 |
| 大文件传输 | 非阻塞写 + EPOLLOUT 续写 | write 循环直到 EAGAIN → 暂存 write_buffer → 注册 EPOLLOUT → 事件驱动续写 |
| 异步日志 | 双缓冲 + 后台线程刷盘 | cur_buf_ 收日志 → swap 到 flush_buf_ → 放锁后写盘；满即刷 / 3s 超时刷 |
| 数据库连接池 | RAII + POSIX 信号量 (sem_t) + MySQL | sem_t 控制空闲连接数，RaiiConn 析构自动归还，GetConn 阻塞等待不返回空 |
| 协议识别 | Content-Type 自动映射 | 按文件后缀查表返回正确 MIME 类型，12 种常见格式，默认 octet-stream |
| 错误处理 | 400/404/405 错误页面 | method 或 url 为空→400，非 GET/POST→405，文件不存在→404 |

## 架构

```
                          ┌─────────────┐
                          │   Client    │
                          └──────┬──────┘
                                 │ TCP
                          ┌──────▼──────┐
                          │  epoll_wait │  主线程事件循环
                          │  ET + ONESHOT│  单线程 accept + 分发
                          └──────┬──────┘
                                 │  dispatch fd
                          ┌──────▼──────┐
                          │  ThreadPool │  8 工作线程
                          │  (mutex+CV) │  mutex + condition_variable
                          └──────┬──────┘
                                 │
              ┌──────────────────┼──────────────────┐
              ▼                  ▼                  ▼
       ┌─────────────┐   ┌─────────────┐   ┌─────────────┐
       │HandleClient │   │ HandleWrite │   │ SqlConnPool │
       │ 读 + 解析   │   │ EPOLLOUT    │   │ RAII + sem  │
       │ + 写响应    │   │ 续写大文件  │   │ 数据库连接  │
       └─────────────┘   └─────────────┘   └─────────────┘
```

### HTTP 请求处理流程

```
Phase 1: 读泵 — 循环 read() 直到 EAGAIN，数据存入 read_buffer
         ↓
Phase 2: FSM 解析 — 从 read_buffer 逐行提取 \r\n
         ├─ REQUEST_LINE → method, url, version
         ├─ HEADERS      → Content-Length, Connection, ...
         └─ BODY         → 按 content_length_ 字节截取
         ↓
Phase 3: 响应生成 — 读文件/构造响应 → 循环 write()
         ├─ 写完了 → Keep-Alive? 回到 Phase 1 : CloseConn
         └─ EAGAIN → 存 write_buffer → 注册 EPOLLOUT → HandleWrite 续写
```

### 数据库连接池流程

```
Init(8) → sem_init(&sem, 0, 8) → 预创建 8 条 MySQL 连接

GetConn()  → sem_wait() 阻塞 → lock → queue.pop() → 返回 MYSQL*
FreeConn() → lock → queue.push() → unlock → sem_post() 唤醒等待者

RaiiConn conn(pool);  // 构造=借连接，析构=还连接，异常安全
```

## 项目结构

```
.
├── include/
│   ├── WebServer.h      # 主服务器类 + HTTP 状态机枚举
│   ├── ThreadPool.h     # 通用线程池（mutex + condition_variable, 83 行）
│   ├── Timer.h          # 小根堆定时器（132 行）
│   ├── Logger.h         # 异步日志系统（双缓冲 + 后台刷盘）
│   └── SqlConnPool.h    # 数据库连接池（RAII + sem_t + MySQL）
├── src/
│   ├── main.cpp         # 入口，端口 8080
│   ├── WebServer.cpp    # 服务器核心实现（~470 行）
│   ├── Logger.cpp       # 日志系统实现（~132 行）
│   ├── SqlConnPool.cpp  # 连接池实现（~80 行）
│   └── epoll_demo.cpp   # 早期 epoll 学习 demo
├── resources/
│   └── index.html       # 静态测试页面
├── Makefile             # 增量编译，make clean 清理
├── NOTES.md             # 31 个知识点专题笔记
└── README.md
```

## 快速开始

### 编译运行

```bash
# 安装 MySQL 开发库（仅连接池需要）
sudo apt install libmysqlclient-dev

# 编译
make

# 启动服务器
./server
```

### 测试

```bash
# 浏览器访问
curl -v http://127.0.0.1:8080/index.html

# POST 请求
curl -X POST -d "hello world" http://127.0.0.1:8080/

# 压力测试
wrk -t4 -c100 -d30s http://127.0.0.1:8080/index.html
```

### 启用数据库连接池

1. 创建数据库和用户：

```sql
CREATE DATABASE mydb;
CREATE USER 'webserver'@'localhost' IDENTIFIED BY '123456';
GRANT ALL PRIVILEGES ON mydb.* TO 'webserver'@'localhost';
FLUSH PRIVILEGES;
```

2. 修改 `src/main.cpp` 取消注释并填入正确参数：

```cpp
SqlConnPool::Instance().Init("127.0.0.1", 3306, "webserver", "123456", "mydb", 8);
```

3. 重新编译运行：

```bash
make clean && make && ./server
```

启动日志中会显示 `数据库连接池初始化完成，连接数: 8`。

### 示例：使用连接池查询

```cpp
#include "SqlConnPool.h"

void handleLogin(const std::string& username, const std::string& password) {
    RaiiConn conn(SqlConnPool::Instance());  // 自动获取连接
    MYSQL* mysql = conn.get();
    
    // 查询用户
    std::string query = "SELECT * FROM users WHERE username='" + username + "'";
    mysql_query(mysql, query.c_str());
    
    MYSQL_RES* result = mysql_store_result(mysql);
    // ... 处理结果 ...
    mysql_free_result(result);
}  // conn 析构，自动归还连接
```

## 性能测试

**环境**：WSL2 Ubuntu 24.04, 16GB RAM, 无特殊系统调优

| 指标 | 优化前 (8线程) | 优化后 (tick降频) |
|------|:-----------:|:-----------:|
| QPS | 3,743 | **4,488** (+20%) |
| 平均延迟 | 43.44ms | **32.95ms** |
| P99 延迟 | — | 482ms |
| 超时数 | 1,011 | **0** |

**关键优化**：定时器 tick 从每次 epoll_wait 返回后执行改为每 500ms 执行一次，主循环锁竞争降低 90%+。

```cpp
// 优化前：每次 epoll_wait 都持锁 tick
timer_.tick(...);

// 优化后：500ms 才 tick 一次
auto now = std::chrono::steady_clock::now();
if (duration_cast<milliseconds>(now - last_tick_).count() >= 500) {
    timer_.tick(...);
    last_tick_ = now;
}
```

## 知识点笔记

项目开发过程中整理了 **31 个知识点专题**，分为 7 大分类：

| 分类 | 专题 |
|------|------|
| I/O 与网络 | epoll vs select/poll, LT vs ET, 阻塞 vs 非阻塞, EPOLLONESHOT |
| TCP & HTTP | Reactor vs Proactor, TCP 三次握手/四次挥手, HTTP/1.1 协议, 错误页面 |
| HTTP 解析 | 两级 FSM, POST & Content-Length, Content-Type & MIME |
| 并发 | 线程池设计, 线程安全 & 数据竞争, 锁类型对比, lambda 表达式 |
| 性能 | 小根堆定时器, 非阻塞 write & EPOLLOUT |
| C++ 特性 | static/RAII/move/=delete/thread/ofstream/Makefile |
| 数据库 | RAII 设计模式, POSIX 信号量 sem_t, 连接池设计, MySQL C API, wrk 压测 |

详见 **[NOTES.md](NOTES.md)**。

## License

MIT
