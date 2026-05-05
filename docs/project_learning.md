# TinyWebServer 项目学习文档

> **TinyWebServer** — 基于 C++17 的 Linux 高并发 Web 服务器，采用 epoll 多路复用 + 线程池 + MySQL 连接池架构。支持 HTTP/1.1 GET/POST 请求解析、静态文件零拷贝服务（mmap + writev）、用户注册/登录 CGI 处理、异步日志、Proactor/Reactor 双并发模型、定时器管理空闲连接。总代码量约 2000 行，header-only 模块设计，是深入学习 Linux 系统编程和 C++ 服务端开发的完整实践项目。

---

## 快速启动

```bash
# 1. 安装依赖
sudo apt install g++ libmysqlcppconn-dev mysql-server

# 2. 配置数据库
sudo mysql -e "
    CREATE DATABASE IF NOT EXISTS qgydb;
    CREATE USER IF NOT EXISTS 'webuser'@'localhost' IDENTIFIED BY 'webpass';
    GRANT ALL PRIVILEGES ON qgydb.* TO 'webuser'@'localhost';
    FLUSH PRIVILEGES;
    USE qgydb;
    CREATE TABLE IF NOT EXISTS user(
        username CHAR(50) NULL,
        passwd CHAR(50) NULL
    ) ENGINE=InnoDB;
"

# 3. 编译
make

# 4. 准备静态文件
mkdir -p root
# （将 judge.html, register.html, log.html, welcome.html 等放入 root/）

# 5. 运行
./server

# 浏览器访问 http://localhost:9006/
```

如需自定义数据库地址或端口，修改 `src/main.cpp`（凭据）和 `src/server/webserver.cpp`（连接地址）。

---

## 目录

1. [项目结构速览](#1-项目结构速览)
2. [基础设施模块简介](#2-基础设施模块简介)
   - 2.1 日志系统 LogSystem
   - 2.2 数据库连接池 ConnectionPool
   - 2.3 线程池 ThreadPool
3. [MySQL 与 MariaDB 的那些事](#3-mysql-与-mariadb-的那些事)
4. [timer_manager.h 深度解析](#4-timermanagerh-深度解析)
   - 4.1 双向链表定时器 TimerList
   - 4.2 epoll 操作封装 EpollUtils
   - 4.3 Linux 信号处理
   - 4.4 信号管道机制
   - 4.5 定时器工作流程
5. [完整工作流程](#5-完整工作流程)
   - 5.1 启动阶段
   - 5.2 事件循环
   - 5.3 新连接处理
   - 5.4 HTTP 请求处理
   - 5.5 处理超时连接
   - 5.6 优雅关闭

---

## 1. 项目结构速览

```
src/
├── main.cpp                     # 入口：解析参数 → 创建服务器 → 启动
├── config/
│   ├── config.h                 # ServerConfig 结构体定义
│   └── config.cpp               # getopt CLI 解析实现
├── server/
│   ├── webserver.h              # WebServer 编排器声明
│   └── webserver.cpp            # 7步初始化 + epoll 事件循环
├── http/
│   ├── http_connection.h        # HttpConnection + MmapFile 声明
│   └── http_connection.cpp      # HTTP 状态机 + CGI + writev 响应
├── log/
│   └── log_system.h             # 日志单例 (header-only)
├── database/
│   └── connection_pool.h        # MySQL 连接池 (header-only)
├── threadpool/
│   └── thread_pool.h            # 线程池模板 (header-only)
└── timer/
    └── timer_manager.h          # 定时器链表 + epoll 工具 (header-only)
```

**仅 4 个 .cpp 文件**，其余模块全在头文件中实现（header-only），设计上减少了编译单元。

---

## 2. 基础设施模块简介

### 2.1 日志系统 LogSystem (`src/log/log_system.h`)

- **单例模式**：`LogSystem::instance()` 返回全局唯一实例
- **同步/异步双模式**：由 `max_queue_size` 决定。`=0` 为同步，>0 为异步
- **异步实现**：`BlockQueue<std::string>` 线程安全阻塞队列 + 后台 `std::thread` 不断 `pop` 写入文件
- **队列满降级**：异步队列满时，当前线程直接同步写文件，不丢日志
- **文件滚动**：按天（`YYYY_MM_DD_ServerLog`）和按行数（`max_lines`）双条件滚动
- **日志宏**：`LOG_INFO(msg)` / `LOG_ERROR(msg)`，参数为 `std::string`

**关键知识点**：`std::deque`、`std::mutex`、`std::condition_variable`、`std::thread`、RAII 锁、单例（Magic Static）、`std::put_time`、`std::filesystem`

### 2.2 数据库连接池 ConnectionPool (`src/database/connection_pool.h`)

- **单例模式**：`ConnectionPool::instance()`
- **预创建 + 惰性创建**：初始化时预创建 `max_conn/2` 个连接，超出时惰性创建
- **阻塞获取**：所有连接耗尽时 `get_connection()` 通过 `condition_variable` 阻塞等待
- **RAII 包装器**：`ConnGuard` 构造时取连接，析构时自动归还到池
- **连接字符串**：通过 `tcp://host:port` URI 格式连接（原代码遗漏了 port 参数，已修复）

**关键知识点**：`sql::Driver`、`sql::Connection`、`std::unique_ptr`、`std::list`、`std::condition_variable` 超时等待、RAII 包装

### 2.3 线程池 ThreadPool (`src/threadpool/thread_pool.h`)

- **模板类**：`ThreadPool<T>`，T 需实现 `read_once()`、`write()`、`process()` 等方法
- **双并发模型**：
  - **Proactor (0)**：主线程完成 `read/write` I/O，工作线程只调用 `process()`
  - **Reactor (1)**：工作线程完成全部 I/O + 处理，主线程只负责事件分发
- **Reactor 同步协议**：主线程通过 `improv` 标志 spin-wait 等待工作线程完成，实现简单同步
- **生命周期**：`append()` 入队 → 工作线程 `pop` → 执行 → 循环

**关键知识点**：`std::thread`、`std::deque`、条件变量 `wait` 带 predicate、RAII 线程管理

---

## 3. MySQL 与 MariaDB 的那些事

### 3.1 它们是什么关系？

这是一个经常让人困惑的问题：

| | MySQL | MariaDB |
|---|---|---|
| **起源** | 1995 年发布，最流行的开源关系型数据库 | 2009 年从 MySQL 分支出来 |
| **作者** | MySQL AB → Sun → Oracle | 原 MySQL 创始人 Monty Widenius |
| **原因** | Oracle 收购 MySQL 后社区担忧开源前景 | 从 MySQL 分叉，保持开源 |
| **兼容性** | — | **API/协议完全兼容 MySQL** |

**结论：MariaDB 是 MySQL 的"增强版替代品"，底层协议完全兼容。同一套客户端库、同样的 SQL 语法、同样的连接协议。**

### 3.2 本项目用的是什么？

本项目**链接的是 MySQL Connector/C++（Oracle 官方驱动）**：

```cpp
// CMakeLists.txt
find_library(MYSQLCPPCONN_LIB mysqlcppconn REQUIRED)
```

代码通过这个库的标准接口操作数据库：
```cpp
#include <cppconn/connection.h>
#include <cppconn/driver.h>
#include <cppconn/statement.h>
sql::Driver *driver = get_driver_instance();
sql::Connection *conn = driver->connect("tcp://127.0.0.1:3307", user, pass);
```

> MySQL Connector/C++ 可以连接任何兼容 MySQL 协议的数据库，包括 MariaDB。

### 3.3 为什么调试时用的是 MariaDB？

系统环境安装的是 **MariaDB 10.6**（`mariadb-server-10.6`），而不是 Oracle MySQL。但两种方式都能工作：

```
程序代码 (mysqlcppconn)
        ↓
MySQL Connector/C++ (Oracle 驱动)
        ↓
MySQL 协议 (TCP 3306 端口)
        ↓
接收端：可以是 Oracle MySQL 或 MariaDB（协议兼容）
```

### 3.4 为什么一开始连接失败了？

调试时遇到 `Access denied for user 'root'@'localhost'`，原因是：

```
MariaDB root 用户的认证方式 = unix_socket 插件
         ↓
只允许 OS root 用户通过 Unix socket 连接
         ↓
普通用户 gunnm 用密码 "root" 连 → 被拒绝
```

这就好比你有一把锁，它只认**指纹（unix_socket）**，但你偏要用**密码（password）**去开。MariaDB 的 root 在 Ubuntu 上默认使用 `unix_socket` 认证，不是传统的密码认证。

### 3.5 最终的解决方案

1. 启动了一个独立的 MariaDB 实例（因为不能 sudo 修改系统 MariaDB 的 root 密码）
2. 创建了专用用户 `webuser`/`webpass`
3. 代码连接 `127.0.0.1:3307`（临时实例的端口）
4. 同时修复了 `connection_pool.h` 中 `port` 参数被忽略的 bug

---

## 4. timer_manager.h 深度解析

这是整个项目中最具 Linux 特色的模块。它包含两个核心组件和一个全局回调函数：

- `TimerList` — 基于双向链表的定时器容器
- `EpollUtils` — epoll 操作 + 信号处理 + 定时器总控
- `cb_func()` — 连接超时时的默认回调

### 4.1 TimerList — 升序双向链表定时器

#### 数据结构：TimerNode（定时器节点）

```cpp
struct TimerNode {
    std::chrono::steady_clock::time_point expire; // 过期时间点
    TimerCallback cb_func;         // 超时回调函数
    ClientData *user_data;         // 关联的客户端连接数据
    TimerNode *prev;               // 前驱指针
    TimerNode *next;               // 后继指针
};
```

每个活跃连接对应一个 `TimerNode`，记录了过期时间（`expire`）、超时要执行的回调（`cb_func`）、以及关联的客户端数据（`user_data`）。最关键的是 `prev` 和 `next` 两个指针——这是双向链表的标志。

#### 为什么用双向链表？

| 操作 | 复杂度 | 原因 |
|------|--------|------|
| `add_timer()` 插入 | O(n) | 需要找到合适位置维持升序 |
| `adjust_timer()` 调整 | O(1)+O(n) | 从原位置删除(O(1))，重新插入(O(n)) |
| `del_timer()` 删除 | O(1) | 双向指针直接操作前后节点 |
| `tick()` 遍历到期节点 | O(k) | k=到期节点数，从头遍历直到未到期 |

**不需要随机访问**，只有顺序插入和头尾删除，所以链表比数组更合适。

#### add_timer() — 按过期时间升序插入

```cpp
void add_timer(TimerNode *timer)
{
    if (!head_) {
        head_ = tail_ = timer;  // 空链表，直接设为头尾
        return;
    }
    if (timer->expire < head_->expire) {
        // 比头节点还早 → 插入头部
        timer->next = head_;
        head_->prev = timer;
        head_ = timer;
        return;
    }
    add_timer(timer, head_);  // 从 head 开始遍历找位置
}
```

#### 为什么 expire 不用 time_t 而用 `steady_clock::time_point`？

```cpp
timer->expire = std::chrono::steady_clock::now() + std::chrono::seconds(3 * TIMESLOT);
```

这里用 `std::chrono::steady_clock` 而不是 `system_clock` 或 `time_t`：

| 时钟类型 | 特点 | 适用场景 |
|---------|------|---------|
| `system_clock` | 系统时间，可能被用户修改/NTP 同步调整 | 显示时间、日志时间戳 |
| `steady_clock` | 单调递增，**永远不会被调整** | 测量时间间隔、超时计算 |
| `high_resolution_clock` | 通常只是 steady_clock 的别名 | 高精度计时 |

**超时计算必须用 steady_clock**。如果用户修改了系统时间（比如 `date -s "2020-01-01"`），`system_clock` 会前后跳跃，导致连接立刻超时或永远不超时。`steady_clock` 不受影响，只计算真实流逝的时间。

#### adjust_timer() — 延长超时时间

```cpp
void adjust_timer(TimerNode *timer)
{
    auto tmp = timer->next;
    if (!tmp || (timer->expire < tmp->expire))
        return;  // 没有下一个节点，或新 expire 仍比下一个早 → 无需移动
    
    // 从链表移除并重新插入
    // ...
    add_timer(timer, head_);
}
```

连接有数据传输时调用 `adjust_timer()`，将 `expire` 设为 `now + 3*TIMESLOT`，然后重排位置。这实现了"连接活动 → 延长超时"的机制。

#### del_timer() — 四种删除场景

```cpp
void del_timer(TimerNode *timer)
{
    if (timer == head_ && timer == tail_) { /* 唯一节点 */ }
    else if (timer == head_) { /* 头节点 */ }
    else if (timer == tail_) { /* 尾节点 */ }
    else { /* 中间节点 */ }
}
```

四种场景各自处理指针关系，体现双向链表删除的典型操作。

#### tick() — 遍历并处理到期节点

```cpp
void tick()
{
    auto cur = std::chrono::steady_clock::now();
    auto tmp = head_;
    while (tmp) {
        if (cur < tmp->expire) break;  // 剩下的节点都未到期
        if (tmp->cb_func)
            tmp->cb_func(tmp->user_data);  // 执行超时回调
        delete tmp;  // 释放节点
        tmp = head_;
    }
}
```

从头遍历链表，遇到 `expire > now` 时停止（链表是升序的，后面的都更晚到期）。对每个到期节点执行回调（关闭连接）。

### 4.2 EpollUtils — epoll 操作封装

`EpollUtils` 封装了 epoll 的常见操作和信号处理功能。

#### epoll 的核心概念

epoll 是 Linux 下高性能 I/O 多路复用的解决方案，解决了 `select`/`poll` 的三大问题：

| | select | poll | epoll |
|---|--------|------|-------|
| 最大连接数 | FD_SETSIZE (1024) | 无限制 | 无限制 |
| 数据结构 | 位图 | 链表 | 红黑树 + 就绪链表 |
| 注册/监听分离 | 否 | 否 | 是（epoll_ctl/epoll_wait） |
| 获取就绪事件 | 遍历全部 fd | 遍历全部 fd | 直接返回就绪事件列表 |
| 工作模式 | LT only | LT only | LT + ET |

**epoll 的三个关键接口：**

```cpp
int epoll_create(int size);    // 创建 epoll 实例，返回 epoll fd
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
```

#### EpollUtils 对 epoll 的封装

**add_fd — 注册文件描述符到 epoll：**

```cpp
static void add_fd(int epollfd, int fd, bool one_shot, int trig_mode)
{
    epoll_event ev{};
    ev.data.fd = fd;
    ev.events = EPOLLIN | EPOLLRDHUP;  // 可读 + 连接关闭/半关闭
    if (trig_mode == 1)
        ev.events |= EPOLLET;          // 边缘触发
    if (one_shot)
        ev.events |= EPOLLONESHOT;     // 事件一次性触发
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
    set_nonblocking(fd);               // 设置非阻塞
}
```

**关键标志位说明：**

| epoll 事件标志 | 含义 |
|---------------|------|
| `EPOLLIN` | 文件描述符可读（有数据到达） |
| `EPOLLOUT` | 文件描述符可写 |
| `EPOLLRDHUP` | 对端关闭连接或半关闭（TCP 的 FIN） |
| `EPOLLHUP` | 挂起（通常表示对端异常断开） |
| `EPOLLERR` | 发生错误 |
| `EPOLLET` | **边缘触发（Edge Triggered）**，只在状态变化时通知 |
| `EPOLLONESHOT` | 事件触发一次后自动从 epoll 中禁用，需重新 `EPOLL_CTL_MOD` 激活 |

**EPOLLONESHOT 的作用：**

这是多线程服务器的关键机制。当一个连接的事件被一个线程处理后，`EPOLLONESHOT` 确保该连接不会同时被其他线程处理。工作线程处理完后再通过 `mod_fd()` 重新注册。

**remove_fd — 从 epoll 移除并关闭：**

```cpp
static void remove_fd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
}
```

**mod_fd — 修改事件监听（用于重新注册 EPOLLONESHOT）：**

```cpp
static void mod_fd(int epollfd, int fd, int ev, int trig_mode)
{
    epoll_event event{};
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    if (trig_mode == 1) event.events |= EPOLLET;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}
```

#### LT 与 ET 模式的区别

| | 水平触发（LT） | 边缘触发（ET） |
|---|---|---|
| **触发条件** | 只要 fd 有数据可读，**每次** epoll_wait 都会返回 | fd 从"无数据"变为"有数据"时**只触发一次** |
| **处理要求** | 可以不读完数据 | **必须一次性读完**（循环到 EAGAIN） |
| **编程难度** | 简单 | 高 |
| **性能** | 较低（可能重复通知） | 高（减少系统调用） |
| **本项目使用** | listenfd: LT, connfd: LT（默认） | 可选 ET + ET（-m 3） |

这体现在 `read_once()` 函数中：

```cpp
// LT 模式：只读一次
if (trig_mode_ == 0) {
    int n = recv(sockfd_, ..., 0);
    if (n <= 0) return false;
    read_idx_ += n;
    return true;
}

// ET 模式：必须循环读到 EAGAIN
while (true) {
    int n = recv(sockfd_, ..., 0);
    if (n == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return false;
    }
    read_idx_ += n;
}
```

### 4.3 Linux 信号处理

#### 信号（Signal）的概念

信号是 Linux/Unix 的**异步事件通知机制**。当某个事件发生时（如用户按 Ctrl+C、定时器到期），内核会向进程发送信号。进程可以选择：
- **默认处理**：通常是终止进程
- **忽略**：`SIG_IGN`
- **自定义处理函数**：通过 `sigaction()` 注册

#### 本项目涉及的信号

| 信号 | 触发条件 | 处理方式 |
|------|---------|---------|
| `SIGPIPE` | 向已关闭的 socket 写数据（RST 状态） | **忽略**（`SIG_IGN`），否则会杀死进程 |
| `SIGALRM` | `alarm()` 定时器到期 | 写入管道通知事件循环（`timer_handler()`） |
| `SIGTERM` | 外部请求终止（如 `kill` 命令） | 写入管道通知事件循环（设置 stop 标志） |

#### sigaction() 函数的详细说明

```cpp
void add_sig(int sig, void (*handler)(int), bool restart = true)
{
    struct sigaction sa{};
    sa.sa_handler = handler;          // 处理函数或 SIG_IGN/SIG_DFL
    if (restart)
        sa.sa_flags |= SA_RESTART;    // 自动重启被中断的系统调用
    sigfillset(&sa.sa_mask);          // 处理此信号时屏蔽所有其他信号
    assert(sigaction(sig, &sa, nullptr) != -1);
}
```

**`sa.sa_flags` 的作用：**

| 标志 | 作用 |
|------|------|
| `SA_RESTART` | 信号处理函数返回后，自动重启被中断的慢速系统调用（如 `accept`、`read`） |
| `SA_NOCLDSTOP` | 仅用于 `SIGCHLD`，子进程停止/继续时不通知父进程 |
| `SA_RESETHAND` | 信号处理函数执行一次后重置为默认处理 |

**`sigfillset(&sa.sa_mask)` 的含义：**

处理信号期间，暂时屏蔽**所有其他信号**，防止信号处理函数重入。这是可重入性保护。

#### 为什么是 sigaction() 而不是 signal()？

```cpp
// 旧的 API（行为因系统而异，不推荐）
void (*signal(int sig, void (*handler)(int)))(int);

// 新的 POSIX 标准 API（行为明确）
int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact);
```

`sigaction()` 提供更精细的控制（屏蔽掩码、标志位），且行为在所有 POSIX 系统上一致。

#### 为什么 SIGPIPE 要忽略？

```cpp
epoll_utils_.add_sig(SIGPIPE, SIG_IGN);
```

当一个 socket 连接已关闭（对端发 FIN 且本端已收到 RST），但程序继续向它 `write` 数据时，内核会发送 `SIGPIPE` 信号。**SIGPIPE 的默认行为是终止进程**。

如果不忽略 SIGPIPE，当服务器试图向一个已经关闭的连接发送响应时，整个服务器进程会被杀死。这显然是灾难性的。

### 4.4 信号管道机制

#### 为什么需要信号管道？

信号处理和事件循环之间存在一个**核心矛盾**：

```
信号处理函数在"信号上下文"中执行
→ 调用哪些函数是安全的？几乎只有 async-signal-safe 函数
→ 不能加锁、不能分配内存、不能调用大多数库函数
→ 但事件循环需要"知道"信号发生了
```

解决方案：**self-pipe trick（自管技巧）**。

#### self-pipe trick 的原理

```cpp
// 1. 在事件循环初始化时创建一对管道
int pipefd_[2];
socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd_);

// 2. 管道的读端注册到 epoll
epoll_utils_.add_fd(epollfd_, pipefd_[0], false, 0);

// 3. 信号处理函数中，向管道写端写入一个字节（信号编号）
static void sig_handler(int sig) {
    int save_errno = errno;           // 保存 errno（可重入性要求）
    int msg = sig;
    send(u_pipefd[1], &msg, 1, 0);    // 唯一的"不安全"操作——但 write/send 是 async-signal-safe
    errno = save_errno;               // 恢复 errno
}

// 4. 事件循环中，pipefd[0] 可读时，读取信号编号
if (sockfd == pipefd_[0] && (events_[i].events & EPOLLIN)) {
    char signals[1024];
    recv(pipefd_[0], signals, sizeof(signals), 0);
    // 处理信号
}
```

#### 为什么信号处理函数中要保存/恢复 errno？

```cpp
int save_errno = errno;  // 保存
// ... 可能修改 errno 的操作 ...
errno = save_errno;      // 恢复
```

信号处理函数可能在任何时刻被执行，包括主程序正在检查 `errno` 的临界时刻。如果信号处理函数修改了 `errno`，主程序的错误检查会被破坏。这是**可重入性（reentrancy）**的基本要求。

#### async-signal-safe 函数

信号处理函数中**只能**调用 async-signal-safe 函数。POSIX 规定的安全函数列表包括 `write()`、`send()`、`read()`、`close()`、`exit()` 等，但**不包含 `malloc()`、`free()`、`printf()`、互斥锁操作等**。

### 4.5 定时器工作流程

```
时间线：
│
├─ t=0    alarm(TIMESLOT) 设置 5 秒闹钟
├─ t=5    SIGALRM → pipe → handle_signal → timeout=true
│         → timer_handler() → tick() → 关闭到期连接 → alarm(5)
├─ t=10   SIGALRM → ... 重复
├─ t=15   SIGALRM → ... 重复
└─ ...
```

每次 `SIGALRM` 触发：
1. `sig_handler()` 向管道写入 `SIGALRM`
2. `event_loop()` 中的 `epoll_wait` 返回，检测到 `pipefd_[0]` 可读
3. `handle_signal()` 从管道读取数据，设置 `timeout = true`
4. 事件循环末尾，如果 `timeout == true`，调用 `epoll_utils_.timer_handler()`
5. `timer_handler()` 调用 `timer_list_.tick()` 遍历并关闭到期连接，然后 `alarm(TIMESLOT)` 重置闹钟

---

## 5. 完整工作流程

### 5.1 启动阶段

```
main()
  │
  ├─ 1. ServerConfig config;   创建配置对象（默认值）
  │      config.parse_arg()    getopt 解析 CLI 参数
  │   2. WebServer server      构造 WebServer
  │      ├─ getcwd + "/root"   设置根路径
  │      ├─ users_(MAX_FD)     预分配 MAX_FD=65536 个连接槽位
  │      └─ users_timer_(MAX_FD) 预分配同等数量的定时器槽位
  │
  └─ 3. server.run()           七步初始化：
        │
        ├─ ① init_log()
        │     LogSystem::instance().init("ServerLog", ...)
        │     创建/打开日志文件，启动异步写线程（如果需要）
        │
        ├─ ② init_db_pool()
        │     ConnectionPool::instance().init("127.0.0.1", ...)
        │        ├─ 初始化 MySQL 驱动
        │        ├─ 预创建 max_conn/2 个数据库连接
        │        └─ 加载用户数据到内存
        │     HttpConnection::load_users()
        │        ├─ SELECT username,passwd FROM user
        │        └─ 存入静态 users_ map
        │
        ├─ ③ init_trig_mode()
        │     将 config 中的触发模式赋值给成员变量
        │
        ├─ ④ init_thread_pool()
        │     创建线程池，启动 N 个工作线程
        │
        ├─ ⑤ init_listen_socket()
        │     socket() → bind() → listen() → SO_LINGER / SO_REUSEADDR
        │
        ├─ ⑥ init_epoll()
        │     ├─ epoll_create()
        │     ├─ add_fd(epollfd, listenfd, ...)  注册监听 socket
        │     ├─ socketpair() → add_fd(pipefd[0]) 创建信号管道
        │     └─ 设置 EpollUtils::u_pipefd / u_epollfd
        │
        ├─ ⑦ init_signals()
        │     ├─ SIGPIPE → 忽略
        │     ├─ SIGALRM → sig_handler → 管道
        │     ├─ SIGTERM → sig_handler → 管道
        │     └─ alarm(TIMESLOT)  首次启动定时器
        │
        └─ event_loop()
              → epoll_wait 主循环
```

### 5.2 事件循环

```
event_loop():
  while (!stop_server):
    epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1)
      │
      ├─ listenfd → handle_new_connection()
      │    ├─ accept() 新连接
      │    ├─ users_[connfd].init() 注册到 epoll (EPOLLONESHOT)
      │    ├─ 创建 TimerNode → add_timer() 插入链表
      │    └─ 连接超时设置为 now + 15s
      │
      ├─ EPOLLRDHUP|EPOLLHUP|EPOLLERR → remove_timer() 关闭
      │
      ├─ pipefd[0] (EPOLLIN) → handle_signal()
      │    ├─ recv() 读取信号编号
      │    ├─ SIGALRM → timeout = true
      │    └─ SIGTERM → stop_server = true
      │
      ├─ EPOLLIN → handle_read(sockfd)
      │    ├─ Reactor: 入队线程池 → 工作线程 read_once()
      │    └─ Proactor: 主线程 read_once() → 入队线程池
      │
      ├─ EPOLLOUT → handle_write(sockfd)
      │    ├─ Reactor: 入队线程池 → 工作线程 write()
      │    └─ Proactor: 主线程直接 write()
      │
      └─ if (timeout):
           epoll_utils_.timer_handler()
             ├─ timer_list_.tick()  关闭到期连接
             └─ alarm(TIMESLOT)     重置闹钟
           timeout = false
```

### 5.3 新连接处理（handle_new_connection）

```
handle_new_connection()
  │
  ├─ LT 模式：accept() 一次
  ├─ ET 模式：while 循环 accept() 直到 EAGAIN
  │
  └─ 对每个新连接：
       ├─ users_[connfd].init(connfd, addr, root, ...)
       │    ├─ 保存地址、触发模式、文档根路径
       │    ├─ add_fd(epollfd, connfd, one_shot=true, trig_mode)
       │    │   注册到 epoll，开启 EPOLLONESHOT
       │    └─ ++user_count_    原子递增连接计数
       │
       ├─ users_timer_[connfd] = {address, sockfd}
       ├─ new TimerNode
       │    ├─ expire = now + 15s
       │    ├─ cb_func = cb_func (关闭连接的回调)
       │    └─ user_data = &users_timer_[connfd]
       │
       └─ add_timer(timer)  插入升序双向链表
```

### 5.4 HTTP 请求处理

这是最复杂的部分，涉及完整的 HTTP 状态机。

#### 阶段一：读取数据

```
read_once()
  ├─ LT: recv() 一次
  └─ ET: 循环 recv() 到 EAGAIN
```

#### 阶段二：对等体处理（Proactor/Reactor）

```
handle_read()  /  线程池 worker()
  │
  └─ read_once() 完成后 → process()
       │
       ├─ process_read()  HTTP 状态机解析
       │    │
       │    ├─ ① parse_line()   从缓冲区取一行（\r\n 分隔）
       │    │    通过 checked_idx_ 顺序扫描，找到 \r\n 后替换为 \0\0
       │    │
       │    ├─ ② parse_request_line()
       │    │    "GET /3register.html HTTP/1.1"
       │    │    → method = POST, url_ = "/3register.html"
       │    │    → is_cgi_ = 1
       │    │
       │    ├─ ③ parse_headers()  逐行解析请求头
       │    │    Connection: → keep_alive_
       │    │    Content-length: → content_length_
       │    │    Host: → host_
       │    │    空行 → 完成头部解析
       │    │
       │    ├─ ④ parse_content()  POST 请求体
       │    │    "user=test&passwd=123"
       │    │
       │    └─ ⑤ do_request()  路由分发
       │         │
       │         ├─ CGI 路由（POST + /2X 或 /3X）
       │         │    ├─ /3X: 注册
       │         │    │    ├─ 查 users_ map 是否已存在
       │         │    │    ├─ INSERT INTO user
       │         │    │    └─ url_ = "/log.html" 或 "/registerError.html"
       │         │    │
       │         │    └─ /2X: 登录
       │         │         ├─ 查 users_ map 匹配密码
       │         │         └─ url_ = "/welcome.html" 或 "/logError.html"
       │         │
       │         └─ 静态文件路由
       │              ├─ route_flag = url_ 最后一个字符
       │              ├─ '0' → register.html
       │              ├─ '1' → log.html
       │              ├─ '5' → picture.html
       │              ├─ '6' → video.html
       │              ├─ '7' → fans.html
       │              └─ 其他 → 直接作为文件名
       │
       └─ process_write(HttpCode)
            │
            ├─ FileRequest → build_file_response()
            │    ├─ HTTP/1.1 200 OK\r\n
            │    ├─ Content-Length: xxx\r\n
            │    ├─ Connection: keep-alive/close\r\n
            │    ├─ Content-Type: text/html\r\n
            │    └─ \r\n
            │    写入 write_buf_（响应头）
            │    iovec[0] = write_buf_  .iov_base
            │    iovec[1] = mmap_file_  .iov_base + .iov_len
            │    io_vec_count_ = 2
            │
            └─ BadRequest/NoResource/...
                 → build_error_response()
                 → 错误页面写入 write_buf_（单 iovec[0]）
```

#### 阶段三：发送响应

```
write()  通过 writev 系统调用发送
  │
  ├─ writev(sockfd, iovec, 2)
  │    ├─ iovec[0] = 响应头（来自 write_buf_）
  │    └─ iovec[1] = 文件内容（来自 mmap）
  │    一次系统调用发送两部分数据（零拷贝）
  │
  ├─ 大文件分片：writev 返回 EAGAIN → mod_fd EPOLLOUT
  │    等待下次可写事件继续发送
  │
  └─ 发送完成：
       ├─ keep-alive → reset() 复用连接，等待下一个请求
       └─ close → 关闭连接
```

#### 状态机流转图

```
parse_request_line()
  → CHECK_STATE_REQUESTLINE → CHECK_STATE_HEADER
       │
       ├─ parse_headers() → 空行
       │    ├─ content_length == 0 → GetRequest → do_request()
       │    └─ content_length > 0 → CHECK_STATE_CONTENT
       │         │
       │         └─ parse_content() → "user=..." 完整读取 → GetRequest → do_request()
       │
       └─ do_request():
            ├─ CGI: 注册/登录 → 设置 url_ 重定向 → 路由 → mmap 文件
            └─ 静态文件: 路由 → stat → mmap 文件
                 ├─ 成功 → FileRequest
                 ├─ 404 → NoResource
                 ├─ 403 → ForbiddenRequest
                 └─ 400 → BadRequest
```

### 5.5 处理超时连接

```
alarm(5 秒) → 5 秒后内核发 SIGALRM
  │
  ├─ sig_handler(SIGALRM)
  │    send(pipefd[1], &SIGALRM, 1, 0)   写入管道
  │
  ├─ epoll_wait 返回 → pipefd[0] 可读
  │    handle_signal() → timeout = true
  │
  └─ 事件循环末尾 → timer_handler()
       ├─ timer_list_.tick()
       │    ├─ cur = steady_clock::now()
       │    ├─ 从 head 遍历: expire > cur? break
       │    ├─ cb_func(user_data) → epoll_ctl DEL → close(fd)
       │    └─ delete timer 释放节点
       └─ alarm(TIMESLOT)  重新设闹钟
```

### 5.6 优雅关闭

```
kill 命令发送 SIGTERM
  │
  ├─ sig_handler(SIGTERM)
  │    send(pipefd[1], &SIGTERM, 1, 0)
  │
  ├─ epoll_wait 返回 → pipefd[0] 可读
  │    handle_signal() → stop_server = true
  │
  └─ while(!stop_server) 循环退出
       run() 返回
       main() 结束
       WebServer 析构:
         ├─ close(epollfd_)
         ├─ close(listenfd_)
         └─ close(pipefd_[0]) / close(pipefd_[1])
```

---

## 附录：涉及的 Linux 系统接口速查

| 接口 | 头文件 | 作用 |
|------|--------|------|
| `epoll_create` | `<sys/epoll.h>` | 创建 epoll 实例 |
| `epoll_ctl` | `<sys/epoll.h>` | 向 epoll 注册/修改/删除 fd |
| `epoll_wait` | `<sys/epoll.h>` | 等待 I/O 事件 |
| `socket` | `<sys/socket.h>` | 创建 socket |
| `bind` | `<sys/socket.h>` | 绑定地址到 socket |
| `listen` | `<sys/socket.h>` | 设置 socket 为监听模式 |
| `accept` | `<sys/socket.h>` | 接受新连接 |
| `connect` | `<sys/socket.h>` | 客户端连接服务器 |
| `recv` | `<sys/socket.h>` | 从 socket 接收数据 |
| `send` | `<sys/socket.h>` | 向 socket 发送数据 |
| `writev` | `<sys/uio.h>` | 分散/聚集 I/O 发送 |
| `setsockopt` | `<sys/socket.h>` | 设置 socket 选项（SO_LINGER、SO_REUSEADDR） |
| `socketpair` | `<sys/socket.h>` | 创建成对 socket（信号管道） |
| `fcntl` | `<fcntl.h>` | 设置非阻塞（F_GETFL/F_SETFL） |
| `mmap` | `<sys/mman.h>` | 文件内存映射 |
| `munmap` | `<sys/mman.h>` | 解除内存映射 |
| `stat` | `<sys/stat.h>` | 获取文件信息 |
| `sigaction` | `<signal.h>` | 注册信号处理函数 |
| `sigfillset` | `<signal.h>` | 设置信号屏蔽集 |
| `alarm` | `<unistd.h>` | 设置定时闹钟 |
| `close` | `<unistd.h>` | 关闭文件描述符 |
| `getcwd` | `<unistd.h>` | 获取当前工作目录 |
| `errno` | `<errno.h>` | 系统调用错误码 |
| `EAGAIN`/`EWOULDBLOCK` | `<errno.h>` | 非阻塞操作"无数据可读" |
