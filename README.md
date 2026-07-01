# Linux 高性能 HTTP 服务器

从零构建的 C++ HTTP/1.1 Web 服务器，零第三方依赖（MySQL 除外），仅使用 Linux 系统级 API。

## 特性

- **epoll 边缘触发 (ET)** + 非阻塞 I/O — 高并发事件驱动
- **两级 FSM** 解析 HTTP 请求（主状态机 + 行状态机）
- **线程池** 多线程并发处理（8 线程，mutex + condition_variable）
- **EPOLLONESHOT** 保证同一 fd 只被一个线程处理
- **Keep-Alive** 长连接 + HTTP 管线化
- **非阻塞写 + EPOLLOUT** — 大文件分片发送，写不完不丢数据
- **小根堆定时器** — O(log n) 管理空闲连接超时
- **异步日志系统** — 双缓冲 + 后台线程刷盘
- **数据库连接池** — RAII + POSIX 信号量 + MySQL
- **静态文件服务** — 自动识别 Content-Type

## 架构

```
                    ┌──────────────────┐
                    │    epoll_wait    │  ← 单线程事件循环
                    │  (ET + ONESHOT)  │
                    └────────┬─────────┘
                             │ 分发 fd
                    ┌────────▼─────────┐
                    │    ThreadPool    │  ← 8 工作线程
                    └────────┬─────────┘
                             │
          ┌──────────────────┼──────────────────┐
          ▼                  ▼                  ▼
   HandleClient         HandleWrite        SqlConnPool
   (HTTP FSM)          (EPOLLOUT续写)      (数据库连接池)
```

## 项目结构

```
include/
├── WebServer.h      ← 主服务器类 + HTTP FSM 枚举 + HttpConnection
├── ThreadPool.h     ← 通用线程池（mutex + condition_variable）
├── Timer.h          ← 小根堆定时器（最小堆）
├── Logger.h         ← 异步日志系统（双缓冲 + 后台刷盘）
└── SqlConnPool.h    ← 数据库连接池（RAII + sem_t + MySQL）

src/
├── main.cpp         ← 入口（端口 8080）
├── WebServer.cpp    ← 服务器核心实现（~470 行）
├── Logger.cpp       ← 日志系统实现（~132 行）
├── SqlConnPool.cpp  ← 连接池实现（~80 行）
└── epoll_demo.cpp   ← 早期 epoll 学习 demo

resources/
└── index.html       ← 静态页面

Makefile             ← 增量编译
```

## 快速开始

### 环境要求
- Linux（Ubuntu 24.04）
- g++（C++14）
- MySQL 8.0（可选，连接池需要）

### 编译

```bash
# 安装依赖（仅 MySQL 开发库）
sudo apt install libmysqlclient-dev

# 编译
make

# 启动
./server
```

### 配置数据库（可选）

```sql
CREATE DATABASE mydb;
CREATE USER 'webserver'@'localhost' IDENTIFIED BY '123456';
GRANT ALL PRIVILEGES ON mydb.* TO 'webserver'@'localhost';
FLUSH PRIVILEGES;
```

修改 `src/main.cpp` 中的连接池参数。

### 测试

```bash
# 静态文件
curl http://127.0.0.1:8080/index.html

# 压测
wrk -t4 -c100 -d30s http://127.0.0.1:8080/index.html
```

## 性能

wrk 压测（4 线程 / 100 并发 / 30 秒）：

| 指标 | 数值 |
|------|------|
| QPS | **4,488** |
| 平均延迟 | 32.95ms |
| 超时 | 0 |

硬件：WSL2 Ubuntu 24.04，无特殊优化。

## 技术栈

| 分类 | 技术 |
|------|------|
| I/O 模型 | epoll ET + 非阻塞 I/O |
| 并发模型 | 半同步/半异步（Reactor 变体） |
| HTTP 解析 | 两级有限状态机 |
| 定时器 | 小根堆（HeapTimer） |
| 线程同步 | mutex / recursive_mutex / condition_variable / sem_t |
| 日志 | 异步双缓冲 |
| 数据库 | MySQL C API + 连接池（RAII） |

## 待完成

- [ ] SO_REUSEPORT 多线程 accept
- [ ] 用户登录注册 API
- [ ] 配置文件支持

## 参考资料

项目配套笔记 [NOTES.md](NOTES.md) — 31 个知识点专题，覆盖 I/O、HTTP、并发、C++、数据库、性能优化。
