# Linux 高性能服务器 — 知识点笔记

> 配合项目代码阅读，每个知识点对应代码中的具体实现。

## 目录

### 第一部分：I/O 与网络
- [一、I/O 多路复用：select → poll → epoll](#一io-多路复用select--poll--epoll)
- [二、epoll 的 LT vs ET](#二epoll-的-lt水平触发vs-et边缘触发)
- [三、阻塞 I/O vs 非阻塞 I/O](#三阻塞-io-vs-非阻塞-io)
- [四、EPOLLONESHOT](#四epolloneshot)

### 第二部分：TCP 与 HTTP 协议
- [五、React vs Proactor vs 半同步半异步](#五reactor-vs-proactor-vs-半同步半异步)
- [六、TCP 连接管理](#六tcp-连接管理)
- [七、HTTP/1.1 协议基础](#七http11-协议基础)
- [八、错误页面](#八错误页面)

### 第三部分：HTTP 解析
- [九、有限状态机（FSM）在 HTTP 解析中的应用](#九有限状态机fsm在-http-解析中的应用)
- [十、POST 请求与 Content-Length](#十post-请求与-content-length)
- [十一、Content-Type 与 MIME 类型](#十一content-type-与-mime-类型)

### 第四部分：并发与线程
- [十二、线程池设计要点](#十二线程池设计要点)
- [十三、线程安全与数据竞争](#十三线程安全与数据竞争)
- [十四、锁的类型](#十四锁的类型)
- [十五、C++ lambda 表达式](#十五c-lambda-表达式)

### 第五部分：性能与优化
- [十六、堆定时器（最小堆）设计](#十六堆定时器最小堆设计)
- [十七、非阻塞 write 与 EPOLLOUT](#十七非阻塞-write-与-epollout)

### 第六部分：C++ 特性
- [十八、C++ 关键特性总结](#十八c-关键特性总结)
- [十九、时间戳格式化](#十九时间戳格式化)
- [二十、宏封装（#define）](#二十宏封装define)
- [二十一、`static` 关键字](#二十一static-关键字)
- [二十二、`std::move` 与移动语义](#二十二stdmove-与移动语义)
- [二十三、`= delete` 禁止默认函数](#二十三-delete-禁止默认函数)
- [二十四、`std::thread` 绑定成员函数](#二十四stdthread-绑定成员函数)
- [二十五、`std::ofstream` 文件操作](#二十五stdofstream-文件操作)
- [二十六、Makefile 基础](#二十六makefile-基础)

### 第七部分：数据库与连接池
- [二十七、RAII 设计模式](#二十七raii-设计模式)
- [二十八、POSIX 信号量（sem_t）](#二十八posix-信号量sem_t)
- [二十九、数据库连接池设计](#二十九数据库连接池设计)
- [三十、MySQL C API 基础](#三十mysql-c-api-基础)
- [三十一、wrk 压测与性能优化](#三十一wrk-压测与性能优化)

---

## 第一部分：I/O 与网络

### 一、I/O 多路复用：select → poll → epoll

| | select | poll | epoll |
|---|---|---|---|
| 数据结构 | fd_set（位图，最大1024） | pollfd 数组（无上限） | 红黑树 + 就绪链表 |
| 注册方式 | 每次调用前设 fd_set | 每次调用前设 pollfd 数组 | epoll_ctl 一次性注册，内核长存 |
| 查找就绪 fd | O(n) 遍历所有 | O(n) 遍历所有 | O(1) 直接取就绪链表 |
| 内核态→用户态 | 每次拷贝整个集合 | 每次拷贝整个数组 | 只拷贝就绪的 fd |
| 适用场景 | 连接数少 | 连接数少 | **高并发（>1000连接）** |

核心区别：select/poll 每次 `wait` 都要把**所有** fd 从用户态传到内核态，返回时还要遍历一遍找就绪的。epoll 用 `epoll_ctl` 一次性注册进内核红黑树，`epoll_wait` 只返回就绪的 fd——**不用全量拷贝和遍历**。

> 📄 `src/WebServer.cpp` — `Init()` 中 `epoll_create1` + `epoll_ctl`，`Start()` 中 `epoll_wait`

---

### 二、epoll 的 LT（水平触发）vs ET（边缘触发）

**快递员类比**：你桌上有一堆没拆的快递。
- LT：快递员反复经过，看见还有没拆的就催你一次
- ET：快递到了通知一声。你不拆是你的事，不会再提醒

| | LT | ET |
|---|---|---|
| 触发条件 | 缓冲区有数据就通知 | 只在"无→有"的瞬间通知一次 |
| 没读完 | 下次 epoll_wait 继续通知 | **不会再通知**，数据丢失 |
| 编程要求 | 可以少读 | 必须循环读到 EAGAIN |
| 适合场景 | 简单不易出错 | 高性能，减少无效通知 |

**项目为什么用 ET**：减少 epoll_wait 返回次数。ET 下一次通知就把数据全读走，配合非阻塞 I/O 无阻塞。

**ET 下必须做的事**：
- fd 设非阻塞（`O_NONBLOCK`）
- `read`/`write` 循环直到 `EAGAIN`

> 📄 `src/WebServer.cpp` — `SetNonBlocking()`，`HandleClient()` 中 Phase 1 读循环

---

### 三、阻塞 I/O vs 非阻塞 I/O

```
阻塞 read(fd, buf, 4096):
  内核只有 100 字节 → 等！→ 等到凑齐 4096 或连接断开才返回

非阻塞 read(fd, buf, 4096):
  内核有 100 字节 → 直接返回 100，不等
  内核为空 → 返回 -1，errno = EAGAIN（不是错误，是"现在没数据"）
```

在 Linux 上 `EAGAIN` == `EWOULDBLOCK`，数值相同。

> 📄 `src/WebServer.cpp` — `HandleClient()` 中 `if (errno == EAGAIN) break;`

---

### 四、EPOLLONESHOT

**问题**：加了线程池后，主线程 dispatch fd=5 给工作线程 A。工作线程 A 还在处理，客户端又发来数据 → epoll 再次通知 fd=5 → 主线程 dispatch 给工作线程 B → **两个线程同时操作同一个 fd** → 数据竞争。

**解决**：注册 fd 时加 `EPOLLONESHOT`。epoll 通知一次后**自动摘除**这个 fd，不再通知。工作线程处理完后主动 `EPOLL_CTL_MOD` 重新注册。

```
epoll 通知 fd=5 → 自动摘除（不通知了）
  → 工作线程处理 HandleClient(5)
    → 处理好后，重新把 fd=5 挂上去
      → 下次数据来了再通知
```

**为什么单线程不需要**：单线程时不存在"两个线程处理同一个 fd"的问题。

> 📄 `src/WebServer.cpp` — `Start()` 中 `EPOLLIN | EPOLLET | EPOLLONESHOT`，`HandleClient()` 末尾 `ResetEpollOneshot()`

---

## 第二部分：TCP 与 HTTP 协议

### 五、Reactor vs Proactor vs 半同步半异步

```
Reactor（主线程同步读）:
  主线程: epoll_wait → 检测可读 → read() 读数据 → 交给工作线程
  工作线程: 解析 + 生成响应

Proactor（OS 异步读）:
  主线程: 发起异步 read → 不管了
  OS: 读完数据 → 通知 → 工作线程直接拿到数据

我们的项目（半同步半异步）:
  主线程: epoll_wait → dispatch(fd) 给线程池
  工作线程: read + parse + write 全包
  ↑ 主线程只管分发 I/O 事件，工作线程做具体 I/O
```

> 📄 `src/WebServer.cpp` — `Start()` 主线程 `AddTask`，`HandleClient()` 在工作线程执行

---

### 六、TCP 连接管理

**三次握手**：
```
客户端                    服务器
  │─── SYN ──────────→│  1. 我想连接
  │←── SYN+ACK ───────│  2. 收到，确认
  │─── ACK ──────────→│  3. 连接建立（accept 返回）
```

**四次挥手**：
```
客户端                    服务器
  │─── FIN ──────────→│  1. 我不发了
  │←── ACK ───────────│  2. 知道了
  │←── FIN ───────────│  3. 我也不发了
  │─── ACK ──────────→│  4. 知道了
  [TIME_WAIT 等 2MSL ≈ 60-120s]
```

**`SO_REUSEADDR`**：服务器重启后端口还在 TIME_WAIT，不加这个参数 `bind()` 会报 "Address already in use"。

**`SOMAXCONN`**：`listen(fd, SOMAXCONN)` 设置 accept 队列最大长度（通常 128）。连接在三握完成后排队等 `accept()`。

> 📄 `src/WebServer.cpp` — `Init()` 中 `setsockopt(SO_REUSEADDR)`、`listen(SOMAXCONN)`

---

### 七、HTTP/1.1 协议基础

**请求格式**：
```
GET /index.html HTTP/1.1\r\n        ← 请求行（方法 URL 版本）
Host: 127.0.0.1\r\n                 ← 头部
Connection: keep-alive\r\n
\r\n                                  ← 空行（头结束标志）
[POST 才有请求体]
```

**响应格式**：
```
HTTP/1.1 200 OK\r\n                  ← 状态行
Content-Type: text/html\r\n          ← 响应头
Content-Length: 128\r\n
Connection: keep-alive\r\n
\r\n                                  ← 空行
<html>...</html>                      ← 响应体
```

**关键规则**：行结束符是 `\r\n`（CRLF）；头部和体之间空行（`\r\n\r\n`）分隔；`Content-Length` 告诉浏览器读多少字节。

**常见状态码**：

| 状态码 | 含义 | 触发条件 |
|---|---|---|
| 200 | OK | 正常 |
| 400 | Bad Request | method 或 url 为空 |
| 404 | Not Found | 文件不存在 |
| 405 | Method Not Allowed | 不是 GET 也不是 POST |

> 📄 `src/WebServer.cpp` — `HandleClient()` Phase 2 FSM 解析，Phase 3 响应生成

---

### 八、错误页面

在 `STATE_FINISH` 响应生成阶段，先判断 method 合法性，再决定返回什么：

```
if method 为空或 url 为空 → 400 Bad Request
else if method != GET && method != POST → 405 Method Not Allowed
else → 正常处理
```

用 `status_code` + `status_text` 统一构造响应行，避免每种错误单独写一遍。

> 📄 `src/WebServer.cpp` — `HandleClient()` Phase 3 响应生成部分

---

## 第三部分：HTTP 解析

### 九、有限状态机（FSM）在 HTTP 解析中的应用

**为什么用 FSM**：TCP 是字节流，一次 `read()` 可能只收到半行数据。

```
第1次 read: "GET /inde"              ← 半行
第2次 read: "x.html HTTP/1.1\r\nHo"  ← 前半行 + 下个头部开头
第3次 read: "st: 127.0.0.1\r\n\r\n"  ← 头部 + 空行
```

**两级 FSM**：

```
主状态机                     从状态机（行检测，由 read_buffer 驱动）
─────────                    ──────────────────────────
REQUEST_LINE → 找到一个 \r\n → 解析 method/url/version → 切到 HEADERS
HEADERS      → 找到一个 \r\n → 空行？→ 切到 BODY 或 FINISH
                              非空？→ 解析 Key:Value 存入 headers
BODY         → 按 content_length_ 字节读取 → 切到 FINISH
FINISH       → 生成响应
```

每次循环：
1. 从 `read_buffer` 找下一个 `\r\n`
2. 找不到 → 半包 → 退出，等下次 read 灌数据
3. 找到 → 取出一行 → 根据主状态处理 → 继续循环

**keep-alive 管线化**：一个 TCP 连接发多个请求。`Reset()` 后回到 `REQUEST_LINE`，继续从 `read_buffer` 剩余数据里解析下一个请求。**read_buffer 不清空**——下个请求的数据可能已经到了。

> 📄 `include/WebServer.h` — `ParseState` 枚举；`src/WebServer.cpp` — `HandleClient()` Phase 2 两层 while 循环

---

### 十、POST 请求与 Content-Length

**GET vs POST**：GET 数据在 URL（`?key=value`），POST 数据在请求体，`Content-Length` 告诉服务器体长。

```
POST /login HTTP/1.1\r\n
Content-Length: 27\r\n
\r\n
username=admin&pwd=123456     ← 27 字节的请求体
```

**FSM 流程**：
```
REQUEST_LINE → HEADERS（解析 Content-Length: 27 → content_length_ = 27）
  → 空行 → content_length_ > 0?
    → 是 → STATE_BODY
    → 否 → STATE_FINISH

STATE_BODY：
  从 read_buffer.substr(0, content_length_) 取 body
  删掉 read_buffer 前 content_length_ 字节
  切到 STATE_FINISH
```

**注意**：空行后面的 body 数据可能已经被 Phase 1 的读泵一次读到 `read_buffer` 里了，STATE_BODY 直接从 buffer 取，不用再 `read()`。

> 📄 `include/WebServer.h` — `HttpConnection` 中 `body_`、`content_length_`；`src/WebServer.cpp` — Phase 2.5

---

### 十一、Content-Type 与 MIME 类型

HTTP 响应必须声明内容类型，否则浏览器无法渲染。现在所有文件都返回 `text/html`，图片/CSS/JS 全都会乱码。

**常见 MIME 表**：

| 后缀 | Content-Type |
|---|---|
| `.html` `.htm` | `text/html` |
| `.css` | `text/css` |
| `.js` | `application/javascript` |
| `.png` | `image/png` |
| `.jpg` `.jpeg` | `image/jpeg` |
| `.gif` | `image/gif` |
| `.ico` | `image/x-icon` |
| `.svg` | `image/svg+xml` |
| `.txt` | `text/plain` |
| `.json` | `application/json` |
| `.pdf` | `application/pdf` |
| 未知 | `application/octet-stream` |

**实现**：`path.find_last_of('.')` 提取后缀 → 查表 → 返回对应 MIME 字符串。没有后缀默认 `text/html`。

> 📄 `src/WebServer.cpp` — `GetMimeType()`

---

## 第四部分：并发与线程

### 十二、线程池设计要点

**核心组件**：
```
任务队列(queue) + 工作线程(vector<thread>) + 互斥锁(mutex) + 条件变量(condition_variable)
```

**工作流程**：
```
主线程:  AddTask(lambda) → queue.push() → cond_.notify_one()
工作线程: cond_.wait() → 唤醒 → queue.pop() → 执行 lambda
空闲线程: cond_.wait() → 没任务就睡
```

**`condition_variable::wait(lock, predicate)` 四步**：
1. 释放锁 + 线程沉睡
2. 被 `notify_one/all` 唤醒
3. 重新获取锁
4. 检查 predicate → true 返回 / false 继续睡（防止虚假唤醒）

**虚假唤醒**：OS 层面可能导致线程莫名其妙被唤醒。必须用 predicate 验证"真的有任务可做吗"。

**析构顺序（不能反过来）**：
1. `stop_ = true`
2. `cond_.notify_all()` — 唤醒所有等待的线程
3. `join()` — 等所有线程退出
4. ⚠️ 先 join 后 notify → 线程永远不醒 → join 永久阻塞

> 📄 `include/ThreadPool.h` — 完整实现（83行）

---

### 十三、线程安全与数据竞争

**数据竞争**：两个线程同时访问同一块内存，至少一个是写操作 → 未定义行为 → 崩溃。

**项目的共享数据**：

| 数据 | 主线程 | 工作线程 | 保护方式 |
|---|---|---|---|
| `clients_[fd]` | accept 写入、tick 清空 | handleClient 读写 | `recursive_mutex` |
| `timer_` | `add()`、`tick()` | `add()`（续期） | `recursive_mutex` |
| `epoll_fd_` | `epoll_ctl` | `epoll_ctl` | **不需要锁**（内核线程安全） |

**为什么 epoll_fd_ 不需锁**：Linux 内核内部用自旋锁保护了 epoll 实例。

**为什么 clients_/timer_ 必须锁**：`std::vector`、`std::string`、`std::unordered_map` 都不是线程安全的——这是我们自己写的代码。

**竞态案例**：
```
工作线程: HandleClient(5) → client.read_buffer += "新数据"
主线程:   timer_.tick() → CloseConn(5) → client.Reset() → read_buffer.clear()
          ↑ 两个线程同时操作一个 string → 内存损坏 → SIGSEGV
```

**加锁位置（4 处）**：HandleClient 入口、CloseConn 入口、accept 循环外包、timer_.tick 外包。

> 📄 `src/WebServer.cpp` — 4 处 `lock_guard`；`include/WebServer.h` — `mutable std::recursive_mutex mtx_`

---

### 十四、锁的类型

| 锁 | 特点 | 适用场景 |
|---|---|---|
| `std::mutex` | 同一线程 lock 两次会死锁 | 无嵌套调用 |
| `std::recursive_mutex` | 同一线程可多次 lock（内部维护计数） | **有嵌套调用** |
| `std::lock_guard<T>` | RAII，构造 lock，析构 unlock | 所有需要锁的作用域 |
| `std::unique_lock<T>` | RAII + 可手动 unlock/lock，配合条件变量 | **`cond_.wait()` 必须用这个** |

`unique_lock` 比 `lock_guard` 多了两个能力：
1. 可以提前 `unlock()`（减小临界区）
2. 可以配合 `condition_variable::wait()`（内部需要反复解锁/加锁）

```cpp
// lock_guard：一锁到底，不能中途解锁 ← Logger::Log() 里用
// unique_lock：可解锁再加锁           ← Logger::FlushLoop() 里用，因为 cond_.wait_for() 内部会解锁

std::unique_lock<std::mutex> lock(mtx_);
cond_.wait_for(lock, timeout, predicate);
// wait_for 内部：解锁 → 睡着 → 被唤醒 → 重新加锁 → 检查 predicate
```

**为什么用 recursive_mutex 而不是 mutex**：

```
HandleClient 加锁 → 内部调 CloseConn → CloseConn 也加锁
                 ↑ 同一线程嵌套拿锁 → 普通 mutex 死锁
```

`recursive_mutex` 维护一个计数和一个线程 ID，同一线程 lock 多次只增加计数，unlock 减计数，减到 0 才真释放。

**锁粒度问题**（当前）：整函数一把锁，I/O 期间也被锁住 → 并发度低。后续优化方向是把锁缩小到只保护 `clients_` 和 `timer_` 的局部操作。

> 📄 `src/WebServer.cpp` — HandleClient、CloseConn 中 `lock_guard`

---

### 十五、C++ lambda 表达式

**语法**：`[捕获列表](参数列表) -> 返回类型 { 函数体 }`

**捕获方式**：

| 写法 | 含义 | 何时用 |
|---|---|---|
| `[x]` | 值捕获，复制一份 x | x 是小变量，lambda 活得更久 |
| `[&x]` | 引用捕获 | x 是大对象，要避免拷贝 |
| `[this]` | 捕获 this 指针 | 要调成员函数 |
| `[=]` | 值捕获所有用到的东西 | 省事 |
| `[&]` | 引用捕获所有 | 省事但有悬垂引用风险 |

**项目中的用法**：

```cpp
[this, current_fd]() { HandleWrite(current_fd); }
```

- `this`：HandleWrite 是成员函数，需要通过 this 调用
- `current_fd`：值捕获。因为 lambda 会被另一个线程执行，栈上的 `current_fd` 可能已经失效

**等价类**：lambda 就是编译器帮你生成一个带 `operator()` 的匿名类，本质语法糖。

> 📄 `src/WebServer.cpp` — `Start()` 中线程池 `AddTask(lambda)`，`timer_.tick(lambda)`

---

## 第五部分：性能与优化

### 十六、堆定时器（最小堆）设计

**为什么用小根堆**：
- 链表：找最小 O(n)
- 红黑树：`std::set`，每个节点单独分配，cache 不友好
- 小根堆：数组存储，堆顶就是最小 O(1)，插入/删除 O(log n)

**`tick` 惰性检查**：
```
while (堆顶过期):
    CloseConn(堆顶.fd)
    pop()
// 堆顶没到期 → 下面一定没到期 → 全部安全，不下滑
```

**`ref_` 哈希表**：`unordered_map<fd, index>` — `add()` 时如果 fd 已存在，O(1) 定位到堆中位置 → 更新 expires → O(log n) 调整。

**续期**：
```
add(fd, timeout):
  fd 已存在 → 更新 expires → siftDown 或 siftUp
  fd 不存在 → push_back → siftUp
```

> 📄 `include/Timer.h` — 完整实现（132行）

---

### 十七、非阻塞 write 与 EPOLLOUT

**问题**：非阻塞 `write()` 可能写不完。

```
response 有 1MB
write(fd, buf, 1MB) → 内核发送缓冲区只有 64KB 空位
→ 实际写入 64KB，返回 65536
→ 剩下的 960KB 丢了？← 不能丢！
```

**解决方案**：

```
1. 循环 write() 直到 remaining == 0
2. 如果 EAGAIN（内核满了）：
   ├─ 剩余数据存入 client.write_buffer
   ├─ 注册 EPOLLOUT 事件
   └─ return（退出等 epoll 通知）
3. epoll 通知 EPOLLOUT → HandleWrite() 从 write_buffer 继续写
4. 写完了 → 回到 EPOLLIN 模式
```

**为什么不能永久注册 EPOLLOUT**：发送缓冲区大部分时间有空位。永久注册会让 epoll_wait 不断返回"可写"，CPU 空转。只在写不完的时候临时加，写完了就摘掉。

**涉及的代码改动**：

| 位置 | 加/改什么 |
|---|---|
| `HttpConnection` | `write_buffer`、`write_offset`、`keep_alive_` |
| `ResetEpollOneshot(fd, write_mode)` | 正常只注册 EPOLLIN，写不完才加 EPOLLOUT |
| `HandleClient` Phase 3 | write 改循环 + EAGAIN 保存 + 注册 EPOLLOUT |
| `HandleWrite` 新函数 | EPOLLOUT 触发时从 write_buffer 继续写 |
| `Start()` | 识别 `EPOLLOUT` 事件 → dispatch 到 HandleWrite |

**`keep_alive_` 为什么跨函数**：HandleClient 里 EAGAIN 时还没发完，不能决定 keep-alive/close。把 `keep_alive_` 存到连接结构体里→ HandleWrite 写完数据后用它判断。

> 📄 `src/WebServer.cpp` — `HandleWrite()` + `HandleClient()` Phase 3

---

## 第六部分：C++ 特性

### 十八、C++ 关键特性总结

| 特性 | 用在哪 | 为什么 |
|---|---|---|
| `std::unique_ptr` | `thread_pool_` | 独占所有权，自动释放 |
| `std::make_unique` | `Init()` 中创建线程池 | 异常安全（C++14） |
| `std::forward` | `ThreadPool::AddTask` | 完美转发，避免拷贝 |
| `emplace_back` | 线程池创建线程 | 原地构造，不拷贝不移动 |
| `RAII` | `lock_guard`、`unique_ptr` | 自动释放，异常安全 |
| `lambda` | `AddTask`、`tick` 回调 | 匿名函数绑定上下文 |
| `mutable` | `recursive_mutex mtx_` | const 函数也能操作锁 |
| `static` | `ReadFile` 缓存 | 函数级全局变量，不在对象间共享 |
| `std::stoi` | Content-Length 解析 | 字符串转整数 |
| `std::string::find` | 找 `\r\n` 找后缀 | 子串查找 |
| `std::string::substr` | 提取行、body | 子串截取 |
| `std::stringstream` | 请求行解析 | 流式提取，自动按空格分割 |
| `std::move` | `push_back(std::move(entry))` | 移动语义，避免深拷贝 O(1) 开销 |
| `= delete` | 单例禁止拷贝 | 显式删除编译器自动生成的函数 |
| `std::thread` + 成员函数 | `&Logger::FlushLoop, this` | 成员函数做线程入口需同时传对象指针 |
| `std::ofstream` | 日志写盘 | `open()` / `<<` / `flush()` / `close()` |
| `static_cast<T>` | `>= max_buf_size_` | C++ 安全类型转换 |

---

### 十九、时间戳格式化

**最终效果**：每条日志前面标记 `[2026-06-29 14:30:15]`。

**三次数据转换**：

```
system_clock::now()     →      time_t (秒数)      →      char* (字符串)
   ↑ time_point                   ↑ 整数                     ↑ "2026-06-29 14:30:15"
   to_time_t()                  strftime()
```

**第一层：获取当前时间点**

```cpp
#include <chrono>
auto now = std::chrono::system_clock::now();
```

| 概念 | 含义 |
|---|---|
| `std::chrono` | C++ 时间库 |
| `system_clock` | 墙上时钟（北京时间），可受系统时间调整影响 |
| `now()` | 返回当前时间点，类型是 `time_point`（人类读不懂） |
| `auto` | 编译器推类型，否则要写一长串 `std::chrono::time_point<...>` |

**第二层：时间点 → 秒数**

```cpp
std::time_t t = std::chrono::system_clock::to_time_t(now);
```

| 概念 | 含义 |
|---|---|
| `std::time_t` | 整数类型，距离 1970年1月1日 的秒数（Unix 时间戳） |
| `to_time_t()` | `time_point` → `time_t` 转换函数 |

此时 `t = 1751903415`，人类还是读不懂。

**第三层：秒数 → 年月日时分秒**

```cpp
#include <ctime>
char buf[32];
std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
std::string time_str = buf;  // "2026-06-29 14:30:15"
```

| 函数/参数 | 含义 |
|---|---|
| `localtime(&t)` | 把 `time_t` 转成 `struct tm`（包含年、月、日、时、分、秒等字段） |
| `strftime(buf, size, format, tm)` | **str**ing **f**ormat **time** — 把 `tm` 按格式模板输出到 `buf` |

**格式符速查**：

| 符号 | 含义 | 示例 |
|---|---|---|
| `%Y` | 四位年份 | `2026` |
| `%m` | 两位月份，01-12 | `06` |
| `%d` | 两位日期，01-31 | `29` |
| `%H` | 24小时制小时，00-23 | `14` |
| `%M` | 分钟，00-59 | `30` |
| `%S` | 秒，00-59 | `15` |

> 📄 `include/Logger.h` — 日志系统 `GetTime()`

---

### 二十、宏封装（`#define`）

**宏是什么**：预处理指令。编译器在编译之前，先把宏名替换成定义的内容——纯文本替换。

```cpp
#define 宏名 替换内容
// 预处理后，所有"宏名"被换成"替换内容"
```

**为什么日志用宏**：

没有宏，每次写日志很啰嗦：

```cpp
Logger::Instance().Log(LogLevel::DEBUG, "客户端 " + std::to_string(fd) + " 断开了");
// ↑ 类名、枚举前缀，一行很长
```

有宏：

```cpp
LOG_DEBUG("客户端 " + std::to_string(fd) + " 断开了");
// ↑ 简洁
```

**带参数的宏**：

```cpp
#define LOG_DEBUG(msg) Logger::Instance().Log(LogLevel::DEBUG, msg)
//          ↑ 参数      ↑ 展开体，msg 被替换成调用时的实参
```

使用时：

```cpp
LOG_DEBUG("hello");  
// 预处理后 → Logger::Instance().Log(LogLevel::DEBUG, "hello");
```

**四种日志等级宏**：

```cpp
#define LOG_DEBUG(msg) Logger::Instance().Log(LogLevel::DEBUG, msg)
#define LOG_INFO(msg)  Logger::Instance().Log(LogLevel::INFO,  msg)
#define LOG_WARN(msg)  Logger::Instance().Log(LogLevel::WARN,  msg)
#define LOG_ERROR(msg) Logger::Instance().Log(LogLevel::ERROR, msg)
```

**宏 vs 函数**：

| | 宏 | 函数 |
|---|---|---|
| 什么时候执行 | 编译前（预处理阶段） | 运行时 |
| 参数类型检查 | ❌ 无，纯文本替换 | ✅ 有 |
| 运行时开销 | 零（编译前已展开） | 有函数调用开销 |
| 调试 | 无法打断点 | 可以打断点 |
| 命名约定 | 全大写 `LOG_INFO` | 小驼峰或下划线 |

**常见陷阱**：宏只是纯文本替换，以下写法有 bug：

```cpp
#define SQUARE(x) x * x

SQUARE(2 + 3)   // → 2 + 3 * 2 + 3 = 2 + 6 + 3 = 11 （不是 25！）
// 正确写法：
#define SQUARE(x) ((x) * (x))
```

**安全宏技巧（`do-while(0)`）**：

```cpp
#define LOG_INFO(msg) \
    do { Logger::Instance().Log(LogLevel::INFO, msg); } while(0)
```

作用：保证宏在任何上下文（if/else、无花括号等）中都能安全使用，且强制要求末尾加分号。

> 📄 `include/Logger.h` — 四个 LOG_ 宏定义

---

### 二十一、`static` 关键字

`static` 在 C++ 里有三种用法，分清楚就永远不会忘。

**用法速查**：

| 位置 | 含义 | 例子 |
|---|---|---|
| 函数里的局部变量 | 只初始化**一次**，函数返回后**变量还在** | `Instance()` 里的 `instance` |
| 类的成员变量 | 归**整个类**，不归某个对象 | `static int count_` |
| 类的成员函数 | 不需要对象就能调，**不能访问 this** | `GetTime()` |

---

**一、`static` 局部变量 — 只初始化一次，永不销毁**

```cpp
// 普通局部变量 — 每次调用都重新来
int count() {
    int n = 0;      // 每次调用都是全新的 n
    n++;
    return n;       // 永远返回 1
}

// static 局部变量 — 第一次初始化，之后保持
int count_static() {
    static int n = 0;   // ← 第一次走到这行才初始化
    n++;                //    第二次调用时，这行"初始化"被跳过
    return n;           // 返回 1, 2, 3, 4 ...
}
```

| | 普通局部变量 | `static` 局部变量 |
|---|---|---|
| 初始化次数 | 每次调用都初始化 | 只初始化一次 |
| 生命周期 | 函数退出 → 销毁 | 函数退出 → **还在** |
| 存储位置 | 栈 | 静态存储区（和全局变量一起） |
| 第二次调用 | 全新的值 | 上次的值还在 |

**单例里的应用**（Meyers' Singleton）：

```cpp
Logger& Logger::Instance() {
    static Logger instance;   // C++11 保证：多线程也只构造一次
    return instance;
}
```

谁调用 `Instance()` 拿到的都是同一个 `instance`。C++11 标准保证多线程安全。

---

**二、`static` 成员变量 — 归整个类，不归某个对象**

```cpp
class Student {
    std::string name_;      // 每个学生有自己的名字
    int age_;               // 每个学生有自己的年龄
    static int total_;      // 所有学生共享一个 total（班级总人数）
};
```

| | 普通成员 | `static` 成员 |
|---|---|---|
| 属于 | 每个对象一份 | 整个类一份 |
| 存在位置 | 对象内部 | 类外部（全局存储区） |
| 怎么访问 | `obj.name_` | `Student::total_` |
| 初始化 | 构造函数里 | 必须在类**外面**单独定义 |

```cpp
// 在 .cpp 里必须写这一行，否则链接报错
int Student::total_ = 0;
```

你的 Logger 没有用 `static` 成员，而是用了单例模式——把唯一的对象存在 `Instance()` 的局部 `static` 变量里。本质一样，写法不同。

---

**三、`static` 成员函数 — 不需要对象就能调**

```cpp
class Logger {
    static std::string GetTime();   // ← static 函数
    void Log(...);                  // ← 普通成员函数
};
```

| | 普通成员函数 | `static` 成员函数 |
|---|---|---|
| 怎么调 | `obj.Log(...)` | `Logger::GetTime()` |
| 有 `this` 指针 | ✅ 有 | ❌ 没有 |
| 能访问成员变量 | ✅ 能 | ❌ 不能（没 this，不知道看谁的） |
| 适用场景 | 需要操作对象状态 | 工具函数，不依赖对象 |

`GetTime()` 只做时间格式化，不碰 `cur_buf_`、`mtx_` 等成员——所以声明成 `static`，不用通过 `Instance()` 就能直接调用：

```cpp
// 在 Log() 里直接调，不需要 Instance()
std::string entry = "[" + GetTime() + "]";   // GetTime() 是 static 的
```

---

**为什么老是忘记**：三种用法的底层思路是一样的——"不属于某个具体对象，属于更大的范围"。

```
static 局部变量  → 不属于"本次调用"，属于"整个函数"
static 成员变量  → 不属于"某个对象"，属于"整个类"
static 成员函数  → 不需要"某个对象"，直接绑在类上
```

> 📄 `include/Logger.h` — `Instance()` 里的 `static Logger instance`；`GetTime()` 声明为 `static`

---

### 二十二、`std::move` 与移动语义

**拷贝 vs 移动**：

```cpp
std::string a = "hello world";            // a 有一块内存存 "hello world"

std::string b = a;                        // 拷贝：b 分配新内存，逐字符复制
//  a 和 b 各自有一份 "hello world"

std::string c = std::move(a);             // 移动：c 直接抢走 a 的内存指针
//  a 变成空字符串，c 拥有原来的 "hello world"
```

| | 拷贝 `b = a` | 移动 `c = std::move(a)` |
|---|---|---|
| 内存分配 | 新分配一块 | 不分配，直接偷 |
| 数据 | 逐字节复制 | 改指针，原数据没动 |
| 原变量 | `a` 不变 | `a` 被掏空（变成空字符串） |
| 开销 | O(n) | O(1) |

**为什么 Logger 里用移动**：

```cpp
void Logger::Log(LogLevel level, const std::string& msg) {
    std::string entry = "[" + GetTime() + "]" + ...;  // 在栈上拼好字符串
    cur_buf_.push_back(std::move(entry));              // 把字符串"搬"进 vector
    // entry 从此不用了 — 所以用 move 避免拷贝
}
```

**一句话**：变量不再用了 → `std::move` 把它搬走，省的复制。

**`std::move` 其实不移动**：它只是一个**类型转换**，把左值转成右值（"可被掠夺"的形态），真正的移动发生在 `push_back` 内部调用移动构造函数时。

> 📄 `src/Logger.cpp` — `Log()` 中 `push_back(std::move(entry))`

---

### 二十三、`= delete` 禁止默认函数

**背景**：C++ 编译器会**自动生成**一些函数：

```cpp
class Logger {
    // 编译器自动生成：
    Logger(const Logger&) = default;             // 拷贝构造函数
    Logger& operator=(const Logger&) = default;   // 拷贝赋值运算符
};
```

你不写也会自动有——这会导致单例被意外拷贝，产生第二个对象。

**解决**：用 `= delete` 明确禁止：

```cpp
class Logger {
    Logger(const Logger&) = delete;            // 不准拷贝构造
    Logger& operator=(const Logger&) = delete;  // 不准拷贝赋值
};

Logger a = Logger::Instance();  // ❌ 编译报错 — 拷贝构造被 delete 了
```

**常见应用**：

| 场景 | 禁止什么 |
|---|---|
| 单例模式 | 拷贝构造 + 拷贝赋值 |
| `std::unique_ptr` | 拷贝构造 + 拷贝赋值（只能 move） |
| 文件句柄类 | 拷贝构造 + 拷贝赋值（不能有两个对象管理同一文件） |

**老式写法**（C++03）：声明为 `private` 且不实现。`= delete` 更简洁，编译报错信息也更清晰。

> 📄 `include/Logger.h` — `Logger(const Logger&) = delete`

---

### 二十四、`std::thread` 绑定成员函数

**普通函数做线程入口很简单**：

```cpp
void func(int a) { ... }
std::thread t(func, 42);           // 传函数名 + 参数
```

**成员函数做线程入口**：需要同时传**函数地址**和**对象地址**：

```cpp
std::thread t(&类名::函数名, 对象指针, 参数...);
//           ^^^^^^^^^^^^^^^  ^^^^^^^^  ^^^^^^^
//           成员函数指针      对象      参数
```

```cpp
flush_thread_ = std::thread(&Logger::FlushLoop, this);
//                          ↑ 要执行的函数           ↑ 操作哪个对象
```

**为什么必须传对象**：成员函数内部用到了 `cur_buf_`、`mtx_`、`file_` 这些成员变量——不告诉它操作哪个对象，它不知道这些变量是谁的。

**等价于**：

```cpp
Logger* me = this;
me->FlushLoop();                  // 主线程同步调用

std::thread(&Logger::FlushLoop, this);  // 另一个线程异步调用
```

**对比 ThreadPool 的写法**：

```cpp
// ThreadPool — lambda 捕获 this
threads_.emplace_back([this]() { ... });

// Logger — 直接传成员函数指针
flush_thread_ = std::thread(&Logger::FlushLoop, this);
```

两者等价，lambda 版是编译器帮封装了一层。直接传成员函数指针更简洁。

> 📄 `src/Logger.cpp` — `Init()` 中 `std::thread(&Logger::FlushLoop, this)`

---

### 二十五、`std::ofstream` 文件操作

**打开文件**：

```cpp
std::ofstream file_;
file_.open("server.log", std::ios::out | std::ios::app);
```

| 标志 | 含义 |
|---|---|
| `std::ios::out` | 写模式 |
| `std::ios::app` | **append**——追加到文件末尾，不覆盖已有内容 |
| `std::ios::trunc` | **truncate**——打开前清空文件（默认行为） |
| `std::ios::in` | 读模式 |

`|` 是位或，组合多个标志。

**open 和 is_open 的区别**：`open()` 是操作，`is_open()` 是查询——返回 `true`/`false` 告诉你文件是否成功打开了。

**写文件**：`<<` 运算符，和 `std::cout` 用法完全一样。

```cpp
file_ << "hello" << 42 << "\n";  // 写进文件
```

**`flush()` vs 不 `flush()`**：

```
file_ << "hello";       // "hello" 还在 C++ 的缓冲区里，没到磁盘
file_ << "world";       // "world" 也追加到缓冲区
                        // 此时程序崩溃 → 数据全部丢失
file_.flush();          // 强制把缓冲区内容推送到磁盘

// 类比：
// file_ << x;    →  把信放进邮筒
// file_.flush(); →  打电话催："现在就来取件！"
```

操作系统和 C++ 库为了性能，都会缓冲写操作。`flush()` 就是"别等了，立刻写盘"。日志系统在关键节点调 `flush()` 确保不丢数据。

**关闭文件**：

```cpp
file_.close();
```

不手动调也行——析构时自动关。但显式调用能检查关闭是否出错。

> 📄 `src/Logger.cpp` — `Init()` 中 `file_.open()`、`file_.flush()`；`Stop()` 中 `file_.close()`

---

### 二十六、Makefile 基础

**Makefile 是什么**：让 `make` 工具自动决定哪些文件需要重新编译。改动了一个 `.cpp`，只重编相关的文件，不用全量重编。

**核心三要素**：目标、依赖、命令。

```makefile
目标: 依赖1 依赖2
	命令              # ← 注意：命令前面必须是 TAB，不能是空格！
```

`make` 的工作逻辑：如果依赖比目标新（被修改过），执行命令重新生成目标。

**项目的 Makefile**（10 行）：

```makefile
# 编译器
CXX = g++
# 编译选项
CXXFLAGS = -std=c++14 -pthread -I include

server: main.o WebServer.o Logger.o
	$(CXX) $(CXXFLAGS) -o server main.o WebServer.o Logger.o

# 模式规则：所有 .o 都由对应的 src/.cpp 编译
%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f server *.o server.log
```

**工作流程**：

```
make
  ├── main.o 不存在 → 执行 %.o 规则: g++ -c src/main.cpp -o main.o
  ├── WebServer.o 不存在 → g++ -c src/WebServer.cpp -o WebServer.o
  ├── Logger.o 不存在 → g++ -c src/Logger.cpp -o Logger.o
  └── 三个 .o 齐了 → g++ -o server main.o WebServer.o Logger.o

make         # 再次运行 → 文件没改，跳过（nothing to be done）
touch src/main.cpp
make         # 只有 main.o 和 server 重编，其他跳过
make clean   # 删掉所有编译产物
```

**变量**：用 `$()` 展开，和 shell 的变量很像。

| 写法 | 含义 |
|---|---|
| `$(CXX)` | 展开变量 CXX → `g++` |
| `$(CXXFLAGS)` | → `-std=c++14 -pthread -I include` |

**自动变量**（模式规则里常用）：

| 符号 | 含义 | 示例（`main.o: src/main.cpp`） |
|---|---|---|
| `$<` | 第一个依赖 | `src/main.cpp` |
| `$@` | 目标 | `main.o` |
| `$^` | 所有依赖 | `main.o WebServer.o Logger.o` |

**为什么命令必须用 TAB**：Makefile 历史遗留语法。目标行不缩进，命令行必须 TAB 缩进。混用空格会报 `*** missing separator`。

**`.PHONY` 是什么**：声明 `clean` 不是要生成的文件名。如果项目目录里恰好有个文件叫 `clean`，`make clean` 会误判为"文件已存在，不用做"。加了 `.PHONY` 就永远执行。

> 📄 `Makefile` — 完整 10 行

---

## 第七部分：数据库与连接池

### 二十七、RAII 设计模式

**全称**：Resource Acquisition Is Initialization（资源获取就是初始化）。

**核心思想**：用 C++ 对象的生命周期管理资源。构造 = 获取，析构 = 释放。

```cpp
// ❌ 没有 RAII — 人肉管理，容易漏
void handleRequest() {
    MYSQL* conn = pool.GetConn();
    if (出错) return;      // ⚠️ 漏了 FreeConn！
    pool.FreeConn(conn);   // 只有正常走到这才还
}

// ✅ RAII — 构造获取，析构释放，绝不遗漏
void handleRequest() {
    RaiiConn conn(pool);   // 构造 → GetConn()
    if (出错) return;      // 析构 → FreeConn() 自动执行 ✅
}
```

**为什么析构一定执行**：C++ 保证栈上对象离开作用域时析构函数被调用，不管是通过 `return`、异常还是大括号结束离开。

**项目里已有的 RAII 例子**：

| 类 | 管理的资源 | 位置 |
|---|---|---|
| `std::lock_guard<T>` | 互斥锁 | ThreadPool、Logger、WebServer |
| `std::unique_lock<T>` | 互斥锁（可手动 unlock） | Logger::FlushLoop |
| `std::unique_ptr<T>` | 堆内存 | WebServer::thread_pool_ |
| `RaiiConn` | MySQL 连接 | SqlConnPool |

**一句话**：把"记得释放"交给编译器，不要靠人记。

> 📄 `include/SqlConnPool.h` — `RaiiConn` 类

---

### 二十八、POSIX 信号量（sem_t）

**信号量是什么**：一个原子计数器，值代表"可用资源数"。

**四个核心函数**：

```c
#include <semaphore.h>

// 1. 初始化
int sem_init(sem_t *sem, int pshared, unsigned int value);
//                              pshared=0: 线程间共享   value: 初始可用数

// 2. P 操作 — "我要占用一个"
int sem_wait(sem_t *sem);
// 信号量 > 0 → 立即减1并返回
// 信号量 = 0 → 阻塞，直到有人 sem_post

// 3. V 操作 — "我腾出一个位置"
int sem_post(sem_t *sem);
// 信号量 +1，唤醒一个在 sem_wait 阻塞的线程

// 4. 销毁
int sem_destroy(sem_t *sem);
```

**连接池里的用法**：

```
Init(8) → sem_init(&sem_, 0, 8)    信号量 = 8

GetConn():  sem_wait(&sem_)   8→7  拿到连接
            sem_wait(&sem_)   7→6  ...
            ...借完8条...
            sem_wait(&sem_)   ⏳ 信号量=0，阻塞等待！

FreeConn(): sem_post(&sem_)   0→1  唤醒等待者
```

**和互斥锁的区别**：

| | mutex | sem_t |
|---|---|---|
| 干什么 | 保护临界区 | 控制资源数量 |
| 值范围 | 0 或 1（二元） | 任意正整数 |
| 谁加谁减 | lock/unlock 同一线程 | wait 和 post 可不同线程 |
| 比喻 | 厕所隔间门锁 | 停车场剩余车位牌 |

**关键规则**：`sem_wait` 和 `sem_post` 必须在锁（mutex）外面！如果在锁里阻塞，别人永远拿不到锁来 post → 死锁。

```cpp
// ✅ GetConn
sem_wait(&sem_);         // 在锁外等
lock(mtx_);
conn = queue.front();    // 拿到信号量了，安全取连接
queue.pop();
unlock(mtx_);

// ✅ FreeConn
lock(mtx_);
queue.push(conn);        // 先还连接
unlock(mtx_);
sem_post(&sem_);         // 在锁外发信号
```

> 📄 `src/SqlConnPool.cpp` — `GetConn()`、`FreeConn()`

---

### 二十九、数据库连接池设计

**为什么需要连接池**：`mysql_real_connect()` 要做 TCP 三次握手 + MySQL 认证，耗时 50ms+。每次请求都建连再关闭 → 高并发下时间全浪费在握手上了。提前建好 N 条连接，请求来了直接拿现成的 → 微秒级。

**架构**：

```
           SqlConnPool (单例)
        ┌──────────────────────┐
        │ sem_t sem_     ← 空闲连接数    │
        │ queue<MYSQL*>  ← 空闲连接排队  │
        │ mutex mtx_     ← 保护队列      │
        │                              │
        │ Init()     ← 预创建 N 条连接   │
        │ GetConn()  ← sem_wait + pop  │
        │ FreeConn() ← push + sem_post │
        └──────────────────────┘
               ↑         ↓
          借出            归还
               ↓         ↑
        ┌──────────────────────┐
        │      RaiiConn        │
        │ MYSQL* conn_         │
        │ SqlConnPool& pool_   │
        │ 析构时自动 FreeConn   │
        └──────────────────────┘
```

**调用流程**：

```cpp
// 启动时初始化一次
SqlConnPool::Instance().Init("127.0.0.1", 3306, "root", "pwd", "mydb", 8);

// 每次请求用 RaiiConn 自动管理
void handleRequest() {
    RaiiConn conn(SqlConnPool::Instance());
    MYSQL* raw = conn.get();
    mysql_query(raw, "SELECT ...");
}  // 自动归还
```

**线程安全分析**：
- `sem_wait` 在锁外：不会持锁阻塞
- `sem_post` 在锁外：不会和被唤醒者抢锁
- 锁只保护队列的 push/pop，不保护信号量

**单例模式（Meyers' Singleton）**：

```cpp
SqlConnPool& SqlConnPool::Instance() {
    static SqlConnPool instance;   // C++11 保证多线程只构造一次
    return instance;
}
```

构造函数 private，拷贝 `= delete`，全局只有一个入口。跟 `Logger` 一模一样。

> 📄 `include/SqlConnPool.h`、`src/SqlConnPool.cpp`

---

### 三十、MySQL C API 基础

**会用到的 4 个函数**：

```c
#include <mysql/mysql.h>

// 1. 创建 MYSQL 对象
MYSQL *mysql_init(MYSQL *mysql);   // 传 NULL → 内部 new，返回指针

// 2. 建立 TCP 连接 + 认证（最耗时！）
MYSQL *mysql_real_connect(MYSQL *mysql,
                           const char *host,      // "127.0.0.1"
                           const char *user,      // "root"
                           const char *passwd,    // 密码
                           const char *db,        // 数据库名
                           unsigned int port,     // 3306
                           const char *unix_socket, // NULL
                           unsigned long clientflag); // 0

// 3. 关闭连接
void mysql_close(MYSQL *mysql);

// 4. 获取错误信息
const char *mysql_error(MYSQL *mysql);
```

**连接的生命周期**：

```
mysql_init(NULL)         ← 创建空壳子
      │
      ▼
mysql_real_connect(...)  ← TCP 握手 + 认证（50ms+，只在 Init 时做）
      │
      ▼
┌─────────────────┐
│  放进 conn_queue_ │  ← 空闲状态，反复借还
│  （零开销复用）    │
└─────────────────┘
      │
      ▼  (ClosePool 时)
mysql_close(conn)        ← 真正释放
```

**编译链接**：`-lmysqlclient` 放在 `.o` 文件**之后**（链接器从左往右处理，库必须在引用它的目标文件后面）。

> 📄 `src/SqlConnPool.cpp` — `Init()`、`ClosePool()`；`Makefile`

---

### 三十一、wrk 压测与性能优化

**wrk 基本用法**：

```bash
wrk -t4 -c100 -d30s http://127.0.0.1:8080/index.html
#   -t4: 4个线程  -c100: 100并发  -d30s: 持续30秒
```

**输出解读**：

| 指标 | 含义 |
|------|------|
| Requests/sec | **QPS**——服务器每秒处理的请求数 |
| Latency (Avg) | 平均响应延迟 |
| Latency (Max) | 最慢的请求花了多久 |
| Socket errors: timeout | 因超时被客户端放弃的请求 |
| Transfer/sec | 吞吐量 |

**本项目的优化过程**：

| 阶段 | QPS | 超时 | 改动 |
|------|-----|------|------|
| 初始 (8线程) | 3,743 | 1,011 | — |
| 16线程 | 3,546 | 949 | 无效——瓶颈不在工作线程数 |
| tick 降频 (500ms) | 4,488 (+20%) | **0** | 减少主循环锁竞争 |

**为什么 tick 降频有效**：原来每次 `epoll_wait` 返回都持锁调 `timer_.tick()`，高并发下每秒上百次抢锁。改成每 500ms tick 一次，锁竞争降 90%+。

```cpp
// 用 steady_clock 控制 tick 频率
auto now = std::chrono::steady_clock::now();
if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick_).count() >= 500) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    timer_.tick( [this](int fd) { CloseConn(fd); } );
    last_tick_ = now;
}
```

**`steady_clock` vs `system_clock`**：`steady_clock` 单调递增，不会因系统时间调整（NTP 对时）而回跳——测时间差必须用它。

**主要瓶颈**：单线程 accept + epoll_wait 分发。进一步优化方向是 SO_REUSEPORT 多线程 accept。

> 📄 `src/WebServer.cpp` — `Start()` tick 逻辑；`include/WebServer.h` — `last_tick_` 成员
