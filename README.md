# TinyWebServer — Modern C++ Rewrite

A lightweight Linux web server using **C++17**, **epoll** (LT/ET), **thread pool**, and **MySQL Connector/C++**.

## Features

- HTTP/1.1 GET & POST request parsing
- Static file serving via mmap + writev (zero-copy)
- User registration and login with MySQL backend
- Dual actor model: Proactor and Reactor
- Synchronous and asynchronous logging with file rotation
- Timer-based idle connection management
- Epoll with LT/ET trigger modes + EPOLLONESHOT

## Build

### Prerequisites

- Linux (epoll, POSIX signals)
- g++ with C++17 support
- MySQL Server with database `qgydb` and table `user(username, passwd)`
- MySQL Connector/C++ (`libmysqlcppconn-dev` on Ubuntu)

```bash
sudo apt install g++ libmysqlcppconn-dev
```

### Build & Run

```bash
# Using make
make
./server

# Or using CMake
mkdir build && cd build
cmake ..
make
./server
```

### CLI Options

```
./server [-p port] [-l LOGWrite] [-m TRIGMode] [-o OPT_LINGER] [-s sql_num] [-t thread_num] [-c close_log] [-a actor_model]
```

| Option | Description | Default |
|--------|-------------|---------|
| `-p` | Port | 9006 |
| `-l` | Log write mode: 0=sync, 1=async | 0 |
| `-m` | Trigger mode: 0=LT+LT, 1=LT+ET, 2=ET+LT, 3=ET+ET | 0 |
| `-o` | SO_LINGER: 0=off, 1=on | 0 |
| `-s` | DB connection pool size | 8 |
| `-t` | Thread pool size | 8 |
| `-c` | Close log: 0=enable, 1=disable | 0 |
| `-a` | Actor model: 0=Proactor, 1=Reactor | 0 |

### Database Setup

```sql
CREATE DATABASE qgydb;
USE qgydb;
CREATE TABLE user(
    username CHAR(50) NULL,
    passwd CHAR(50) NULL
) ENGINE=InnoDB;
```

Edit credentials in `src/main.cpp` (default: `root/root/qgydb`).

## Architecture

```
src/
├── main.cpp                 # Entry point
├── config/                  # CLI argument parsing
├── server/                  # Epoll event loop orchestrator
├── http/                    # HTTP state machine + routing
├── log/                     # Async log singleton (header-only)
├── database/                # MySQL connection pool (header-only)
├── threadpool/              # Thread pool template (header-only)
└── timer/                   # Timer list + epoll utils (header-only)
```

## Original Project

This is a modern C++ rewrite of [qinguoyi/TinyWebServer](https://github.com/qinguoyi/TinyWebServer). The original used POSIX threads, C MySQL API, and raw pointers. This version uses:

- `std::thread` / `std::mutex` / `std::condition_variable` instead of `pthread` / `locker` wrappers
- MySQL Connector/C++ (`sql::Connection`) instead of `mysql_query` C API
- `std::function` callbacks instead of C function pointers
- `std::chrono` instead of `time_t`
- `std::vector` / `std::array` instead of `new[]` / C arrays
- `std::unique_ptr` RAII for resources
- Header-only modules for log, database, thread pool, and timer
