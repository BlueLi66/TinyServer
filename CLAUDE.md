# Linux 高性能服务器项目

## 项目概述
从零构建的 C++ HTTP Web 服务器，使用 Linux epoll + 线程池 + 非阻塞 I/O。
零第三方依赖，仅使用 POSIX/Linux 系统级 API。（MySQL C API 除外）

## 项目结构
```
include/
├── Timer.h          ← 最小堆定时器（小根堆）
├── WebServer.h      ← 主服务器类 + HTTP FSM 枚举
├── ThreadPool.h     ← 通用线程池（mutex + condition_variable）
├── Logger.h         ← 异步日志系统（双缓冲 + 后台刷盘）
└── SqlConnPool.h    ← 数据库连接池（RAII + sem_t + MySQL）
src/
├── main.cpp         ← 入口，端口 8080
├── WebServer.cpp    ← 服务器实现（~460行）
├── Logger.cpp       ← 异步日志系统实现（~132行）
├── SqlConnPool.cpp  ← 数据库连接池实现（~80行）
└── epoll_demo.cpp   ← 早期 epoll 学习 demo
resources/
└── index.html       ← 静态页面
```

## 已完成模块（14项）
1. epoll 边缘触发 (ET) + 非阻塞 I/O 事件循环
2. HTTP/1.1 GET 请求解析（两级 FSM：主状态机 + 行状态机）
3. 静态文件服务（200/404）
4. Keep-Alive 长连接 + HTTP 管线化
5. 堆定时器（HeapTimer）管理空闲连接超时
6. 线程池（ThreadPool）多线程并发处理
7. EPOLLONESHOT 保证一个 fd 只被一个线程处理
8. clients_ 用 vector 预分配，fd 直接做下标
9. POST 请求解析（STATE_BODY + Content-Length 提取）
10. Content-Type 自动识别（按后缀返回正确 MIME 类型）
11. 非阻塞写 + EPOLLOUT（大文件分片发送 + HandleWrite）
12. 错误页面（400 Bad Request、405 Method Not Allowed）
13. 异步日志系统（双缓冲 + 后台线程刷盘 + 宏封装）
14. 数据库连接池（RAII + POSIX 信号量 + MySQL C API）

### 数据库连接池架构
- 设计: `SqlConnPool` 单例 + `RaiiConn` RAII 守卫
- 获取: `GetConn()` → `sem_wait()` 阻塞等待空闲连接 → 持锁取队列头
- 归还: `FreeConn()` → 持锁 push 回队列 → `sem_post()` 唤醒等待者
- RAII: `RaiiConn` 构造时借连接，析构时自动归还，异常安全
- 信号量: `sem_t sem_`，值 = 空闲连接数，`sem_wait`/`sem_post` 在锁外
- 锁: `std::mutex mtx_` 只保护 `conn_queue_` 队列操作，不包信号量
- 单例: Meyers' Singleton（C++11 线程安全），跟 Logger 同模式

### 日志系统架构
- 设计: 双缓冲 (`cur_buf_` + `flush_buf_`) + 后台刷盘线程 `FlushLoop()`
- 写入: `Log()` 持锁将日志行 push 到 `cur_buf_`，满了通知后台线程
- 刷盘: `FlushLoop()` 持锁 swap 两个缓冲区，放锁后写 `flush_buf_` 到磁盘
- 触发: 缓冲区满立即刷 / 超时（默认 3 秒）定时刷
- 未初始化兜底: `Log()` 退化为 `stderr` 输出
- 宏: `LOG_DEBUG/INFO/WARN/ERROR(msg)` — `do-while(0) ` 安全封装
- 日志覆盖: 初始化、启动、新连接、请求处理(方法+URL+状态码)、连接关闭、各类错误

### 加锁架构
- 锁类型: `std::recursive_mutex mtx_`
- 保护: `clients_` 向量 + `timer_` 堆
- 位置: HandleClient / CloseConn / accept循环 / timer_.tick
- epoll_fd_ 不需要锁（内核线程安全）

### 写循环与EPOLLOUT
- `HttpConnection` 扩展: `write_buffer`、`write_offset`、`keep_alive_`
- `HandleClient` Phase 3: 非阻塞写循环 + EAGAIN 保存 + 注册 EPOLLOUT
- `HandleWrite()`: EPOLLOUT 触发时继续写
- `ResetEpollOneshot(fd, write_mode)`: 正常只注册 EPOLLIN，写不完才加 EPOLLOUT

### 性能优化记录
- 初始压测 (8线程): QPS 3,743, 超时 1,011
- 定时器 tick 降频 (每500ms): QPS 4,488 (+20%), 超时 0
- 进一步优化方向: SO_REUSEPORT 多线程 accept

## 知识点笔记
详见 [NOTES.md](NOTES.md) — 31 个专题，7 大分类。

## 待完成
- [x] 异步日志系统（双缓冲 + 后台线程刷盘）
- [x] 数据库连接池（RAII + 信号量 + MySQL）
- [x] Makefile（增量编译 + clean 清理）
- [x] Webbench 压测 + 性能优化（第一轮）
- [ ] README + GitHub
