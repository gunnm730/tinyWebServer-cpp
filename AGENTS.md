# AGENTS.md — tinyWebServer-cpp

## Build & Run

```bash
make                    # debug build (DEBUG=1 by default, adds -g)
make DEBUG=0            # release build (-O2)
make clean              # removes server binary + .o files
mkdir build && cd build && cmake .. && make   # cmake alternative
./server                # runs on port 9006 by default
```

Flags: `-p port -l logmode -m trigmode -o linger -s sqlpool -t threads -c closelog -a actormodel`

## Project Structure

```
src/main.cpp               — entry, wires config→log→db→webserver
src/config/config.{h,cpp}  — CLI parsing via getopt
src/server/webserver.{h,cpp} — epoll event-loop orchestrator
src/http/http_connection.{h,cpp} — HTTP state machine, mmap+writev, CGI login/register
src/log/log_system.h         — async log singleton (header-only)
src/database/connection_pool.h — MySQL conn pool (header-only)
src/threadpool/thread_pool.h   — thread pool template (header-only)
src/timer/timer_manager.h      — timer list + epoll utils (header-only)
```

## Gotchas

- **`src/timer/timer_manager.h` includes static member definitions and `cb_func()` at file scope** — these must be `inline` (C++17) to avoid multiple-definition linker errors when included by multiple `.cpp` files.
- MySQL credentials are hardcoded in `src/main.cpp` (`root/root/qgydb` by default).
- **`src/database/connection_pool.h`**: `create_connection()` originally ignored the `port` parameter; it now builds a `tcp://host:port` URI string.
- **`src/http/http_connection.cpp:do_request()`**: CGI block (lines 423-470) modified `url_` for redirects but never recalculated `real_file_` — fixed by re-running routing via `goto reroute` after CGI.
- MySQL credentials are hardcoded in `src/main.cpp` (`root/root/qgydb` by default). During dev, port 3307 with `webuser/webpass` was used.
- Log file `ServerLog` is written to CWD with date prefix (e.g., `2026_05_05_ServerLog`).
- No test framework, no lint/format config, no CI.
- Single git commit (`a7c898c` — "Modern C++17 rewrite of TinyWebServer").
- Serving static files requires a `./root/` directory (CWD + "/root"). Without it, all GET requests return 404.

## Dependencies

- Linux (epoll, POSIX signals)
- `g++` with C++17 support
- `libmysqlcppconn-dev` (`sudo apt install g++ libmysqlcppconn-dev`)
- MySQL/MariaDB server with database `qgydb` and table `user(username CHAR(50), passwd CHAR(50))`
