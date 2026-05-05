# Linux 命令详解——以 TinyWebServer 调试为例

> 本文通过讲解本项目调试中用到的所有 Linux 命令，帮助你理解命令的结构、管道、重定向等核心概念。

---

## 目录

1. [命令的基本结构](#1-命令的基本结构)
2. [管道 `|` 与链式执行](#2-管道--与链式执行)
3. [I/O 重定向](#3-io-重定向)
4. [后台运行与进程管理](#4-后台运行与进程管理)
5. [MySQL/MariaDB 相关命令](#5-mysqlmariadb-相关命令)
6. [文件与目录操作](#6-文件与目录操作)
7. [网络测试命令](#7-网络测试命令)
8. [系统信息查询](#8-系统信息查询)
9. [综合实战：拆解一条完整命令](#9-综合实战拆解一条完整命令)

---

## 1. 命令的基本结构

绝大多数 Linux 命令遵循这个结构：

```
命令名  [选项]  [参数]
```

示例：

```bash
ls -la /tmp
# └─  └──  └───
# 命令  选项  参数
```

- **命令名**：要执行什么程序（`ls`、`mysql`、`webbench`）
- **选项**：调整命令行为（`-l` 详细格式、`-a` 显示隐藏文件）
- **参数**：命令操作的目标（`/tmp` 目录、`root/` 路径）

选项可以合并：

```bash
ls -l -a       # 分开写
ls -la         # 合并写，效果一样
```

---

## 2. 管道 `|` 与链式执行

### 2.1 管道 —— 把命令串起来

管道 `|` 是把**前一个命令的输出**作为**后一个命令的输入**。

```
命令1  |  命令2
 输出└────→┘输入
```

```bash
# 示例：列出所有进程，然后找出包含 "server" 的行
ps aux | grep server
# ① ps aux   → 输出所有进程（一大堆）
# ② |        → 把①的输出送给②
# ③ grep server → 只保留包含 "server" 的行
```

```bash
# 更完整的过滤：排除 grep 命令本身
ps aux | grep server | grep -v grep
#                          └─ -v = 排除匹配的行
```

### 2.2 链式执行

| 符号 | 含义 | 示例 |
|------|------|------|
| `cmd1 ; cmd2` | 无论成功失败都执行 cmd2 | `cd /tmp ; ls` |
| `cmd1 && cmd2` | **cmd1 成功才**执行 cmd2 | `make && ./server` |
| `cmd1 \|\| cmd2` | **cmd1 失败才**执行 cmd2 | `cd /x || echo "失败"` |

```bash
# && 示例：编译成功才运行
make clean && make && ./server
#       └─ 上一步成功 → 下一步
```

```bash
# 综合：尝试安装，如果已安装则跳过
which webbench 2>/dev/null || sudo apt install -y webbench
# ① which → 查找命令路径
# ② || → 如果①失败（未安装）才执行 apt install
```

### 2.3 命令替换 `$()` 和反引号

```bash
# 查看 mariadb 的版本
dpkg -l | grep mariadb-server
# 想知道 mariadb 的安装路径
dpkg -L mariadb-server-10.6 2>/dev/null | grep bin
```

---

## 3. I/O 重定向

Linux 每个进程有 3 个标准文件流：

| 编号 | 名称 | 含义 |
|------|------|------|
| 0 | stdin | 标准输入（键盘） |
| 1 | stdout | 标准输出（屏幕） |
| 2 | stderr | 标准错误（屏幕） |

### 3.1 基本重定向

```bash
# >   ：重定向 stdout（覆盖）
echo "hello" > file.txt     # 把 "hello" 写入 file.txt（覆盖原内容）

# >>  ：重定向 stdout（追加）
echo "world" >> file.txt    # 追加到 file.txt 末尾

# 2>  ：重定向 stderr
grep root /etc/shadow 2> /dev/null   # 错误信息丢进黑洞

# &>  ：同时重定向 stdout 和 stderr
make &> build.log           # 所有输出都保存到文件
```

### 3.2 魔鬼命令解析

```bash
./server </dev/null >/dev/null 2>&1 &
```

分步拆解：

```
./server                  # ① 运行服务器程序
         < /dev/null      # ② stdin 来自空（没有键盘输入）
                   2>&1   # ④ stderr(2) 重定向到 stdout(1)，即也去 /dev/null
         > /dev/null      # ③ stdout 重定向到空（不输出到屏幕）
                       &  # ⑤ 后台运行
```

更易读的写法（bash 4+ 支持）：

```bash
./server &>/dev/null &
#        └─ 等价于 >/dev/null 2>&1
```

### 3.3 管道与重定向组合

```bash
# 只看 stderr
make 2>&1 >/dev/null | grep error
# ① make 的输出：stdout(1) 和 stderr(2)
# ② 2>&1       ：stderr 合并到 stdout
# ③ >/dev/null ：丢弃 stdout（包含原来的 stdout + 合并来的 stderr）
# 👆 有问题！因为 2>&1 时 stdout 还没重定向

# 正确姿势：
make 2>&1 >/dev/null   # ❌ stderr 追踪到了旧 stdout
make >/dev/null 2>&1   # ✅ 先重定向 stdout，再把 stderr 指向它
```

### 3.4 /dev/null 黑洞

`/dev/null` 是一个特殊的设备文件——写入它的任何数据都会被丢弃，读取它得到空。

```bash
# 丢弃所有输出
./server >/dev/null 2>&1

# 只保留错误信息
./server >/dev/null        # stdout 丢弃，stderr 仍显示

# 检查命令是否存在，不显示任何输出
which webbench 2>/dev/null  # 错误信息隐藏，只返回退出码
```

---

## 4. 后台运行与进程管理

### 4.1 `&` 后台运行

在命令末尾加 `&`，命令会在后台执行，终端立即返回：

```bash
./server &
# [1] 12345     ← 后台作业编号和进程 PID
```

### 4.2 `sleep` 等待

```bash
./server &
sleep 2          # 等待 2 秒，让服务器完成初始化
ss -tlnp | grep 9006  # 然后检查端口是否在监听
```

### 4.3 `pkill` 杀进程

```bash
# 杀死所有名字包含 ./server 的进程
pkill -f "^\./server$"
#       └─ -f = 匹配完整命令行（不只是进程名）
#          "^\./server$" = 正则表达式：
#            ^    开头
#            \.   点（转义，因为 . 在正则里是"任意字符"）
#            $    结尾
```

```bash
# 按 PID 杀死
kill 12345      # 优雅终止
kill -9 12345   # 强制杀死（SIGKILL）
```

### 4.4 `ps` 查看进程

```bash
ps aux
# a = 所有用户
# u = 显示用户列
# x = 包括没有终端的进程

# 与 grep 配合
ps aux | grep server | grep -v grep
```

### 4.5 `setsid` 完全脱离终端

```bash
setsid mysqld --datadir=... &
```

`setsid` 让程序在一个新会话中运行，即使终端关闭也不会杀死它。普通 `&` 后台进程在终端关闭时仍可能收到 SIGHUP 信号。

### 4.6 `timeout` 限时运行

```bash
timeout 2 ./server   # 运行 2 秒后自动终止
timeout 10 webbench -c 50 -t 10 http://...   # 限制总运行时间
```

---

## 5. MySQL/MariaDB 相关命令

### 5.1 安装与创建数据库

```bash
# 安装依赖
sudo apt install g++ libmysqlcppconn-dev mysql-server

# 注意：本机实际装的是 mariadb-server，用法完全相同
```

### 5.2 `mysql` 客户端连接

```bash
# 基本语法
mysql -u 用户名 -p密码 -S socket文件 -e "SQL语句"

# 示例：以 webuser 身份查询数据库列表
mysql -u webuser -pwebpass -S /tmp/mariadb_temp/run/mysql.sock -e "SHOW DATABASES;"
#   -u webuser              = 用户名
#   -pwebpass               = 密码（-p 后直接跟密码，没有空格）
#   -S /path/to/socket      = Unix socket 文件路径
#   -e "SHOW DATABASES;"    = 执行 SQL 后退出
```

**为什么用 `-S` 而不是 `-h`？**

```
-S, --socket     → 通过本地 socket 文件连接（更快）
-h, --host       → 通过网络 TCP 连接（需指定端口 -P）
```

本机开发用 `-S` 更快更安全。远程连接必须用 `-h`。

### 5.3 创建数据库和表的完整 SQL

```bash
mysql -u root -S /tmp/run/mysql.sock -e "
CREATE USER IF NOT EXISTS 'webuser'@'localhost' IDENTIFIED BY 'webpass';
CREATE DATABASE IF NOT EXISTS qgydb;
GRANT ALL PRIVILEGES ON qgydb.* TO 'webuser'@'localhost';
FLUSH PRIVILEGES;
USE qgydb;
CREATE TABLE IF NOT EXISTS user(
    username CHAR(50) NULL,
    passwd CHAR(50) NULL
) ENGINE=InnoDB;
"
#   ① 多行 SQL 用双引号包裹，可以直接在 shell 里写多行
#   ② IF NOT EXISTS → 幂等，重复执行不会报错
#   ③ qgydb.*      → 数据库 qgydb 中所有表
#   ④ ENGINE=InnoDB → 使用 InnoDB 引擎（支持事务、行级锁）
```

### 5.4 `mysql_install_db` 初始化数据目录

```bash
mysql_install_db --datadir=/tmp/mariadb_temp/data \
                 --auth-root-authentication-method=normal
#  --datadir=...    → 指定数据存储目录
#  --auth-root...  → 创建 root 用户时使用密码认证（而不是 unix_socket）
```

### 5.5 `mysqld` 启动数据库服务

```bash
setsid /usr/sbin/mysqld \
  --datadir=/tmp/mariadb_temp/data \
  --socket=/tmp/mariadb_temp/run/mysql.sock \
  --port=3307 \
  --pid-file=/tmp/mariadb_temp/run/mysqld.pid \
  --skip-networking=0 \
  --user=gunnm \
  </dev/null >/dev/null 2>&1 &
```

分步解释：

```
setsid                      ← 在新会话中运行（脱离终端）
  /usr/sbin/mysqld           ← MariaDB 服务器程序
  --datadir=...              ← 数据存储位置
  --socket=...               ← Unix socket 文件路径
  --port=3307                ← 监听端口（避免与系统 3306 冲突）
  --pid-file=...             ← PID 文件路径
  --skip-networking=0        ← 允许 TCP 连接（默认只监听本地 socket）
  --user=gunnm               ← 以 gunnm 用户运行（不是默认的 mysql 用户）
  </dev/null >/dev/null 2>&1 ← 不依赖终端、不输出到终端
  &                          ← 后台运行
```

命令太长怎么读？用 `\` 反斜杠换行：

```bash
# 相当于一行，但更易读
setsid /usr/sbin/mysqld \
  --datadir=/tmp/mariadb_temp/data \
  --socket=/tmp/mariadb_temp/run/mysql.sock
# ^ 反斜杠必须在一行的末尾，后面不能有空格
```

---

## 6. 文件与目录操作

### 6.1 `ls` 列出文件

```bash
ls            # 列出当前目录
ls -l         # 详细格式（权限、大小、修改时间）
ls -a         # 显示隐藏文件（以 . 开头的）
ls -la        # 合并：详细格式 + 隐藏文件
ls /tmp       # 列出指定目录
```

### 6.2 `mkdir` 创建目录

```bash
mkdir root            # 创建单个目录
mkdir -p a/b/c        # 创建多级目录（-p = parents）
mkdir -p /tmp/mariadb_temp/run
```

### 6.3 `rm` 删除

```bash
rm file.txt           # 删除文件
rm -r dir/            # 删除目录（-r = recursive，递归）
rm -rf dir/           # 强制删除（-f = force，不提示）
```

### 6.4 `cat` / `tail` / `wc` 查看文件

```bash
cat file.txt          # 显示整个文件
cat file.txt | tail -5    # 只看最后 5 行
tail -5 file.txt      # ✅ 更高效（不需要 cat + 管道）
tail -f file.txt      # 持续跟踪文件新增内容（实时看日志）
wc -l file.txt        # 统计文件行数
wc -l *.cpp           # 统计所有 .cpp 文件的行数
```

### 6.5 `grep` 搜索文本

```bash
grep "server" file.txt       # 搜索包含 "server" 的行
grep -v grep                 # -v = 排除匹配的行
grep -E "mysql|mariadb"      # -E = 扩展正则，匹配 mysql 或 mariadb
grep -n "server" file.txt    # -n = 显示行号
```

---

## 7. 网络测试命令

### 7.1 `wget` HTTP 请求

```bash
# 基本 GET
wget -q -O - http://localhost:9006/judge.html
#   -q       = quiet（安静模式，不显示进度）
#   -O -     = 输出到 stdout（-，标准输出），而不是保存文件

# 不想看输出，只看 HTTP 状态码
wget -q -O /dev/null -w "%{http_code}" http://localhost:9006/
#   -O /dev/null  = 下载内容丢进黑洞
#   -w "格式"     = 在输出末尾附加指定信息

# POST 请求（发送表单数据）
wget -q -O - --post-data="user=test&passwd=123" \
    http://localhost:9006/3register.html
#   --post-data="k=v&k2=v2"  = POST 请求体（URL 编码格式）

# 调试模式（显示请求和响应的原始内容）
wget -d -O /tmp/out http://localhost:9006/
#   -d = debug，打印完整的 HTTP 请求和响应头部
```

### 7.2 `nc`（netcat）网络测试

`nc` 像是一个网络版瑞士军刀，可以直接连接 TCP 端口：

```bash
# 连接服务器，手动发送 HTTP 请求
printf "GET /judge.html HTTP/1.0\r\n\r\n" | timeout 2 nc 127.0.0.1 9006
# ① printf 构造 HTTP 请求（\r\n 是 HTTP 协议要求的换行）
# ② | 管道送给 nc
# ③ nc 建立 TCP 连接，发送数据，接收响应
# ④ timeout 2 限制 2 秒，防止 nc 不退出
```

拆解：

```
printf "GET /judge.html HTTP/1.0\r\n\r\n"
# 构造 HTTP 请求，\r\n 分别是回车（CR）和换行（LF）
# HTTP 协议要求每行以 \r\n 结尾
# 两个 \r\n 表示头部结束

|     ← 管道：把 printf 的输出送给 nc

timeout 2 nc 127.0.0.1 9006
#   timeout 2        → 限制总时间 2 秒
#   nc               → netcat 网络工具
#   127.0.0.1        → 本机（localhost 的 IP 地址）
#   9006             → 端口号
```

### 7.3 `webbench` 压力测试

```bash
webbench -c 100 -t 10 http://127.0.0.1:9006/judge.html
#   -c 100  = concurrency = 100 个并发客户端
#   -t 10   = time = 持续 10 秒
#   URL     = 测试的目标地址

# 只看结果摘要
webbench -c 50 -t 10 http://... 2>/dev/null | grep -E "Speed|Requests"
#   2>/dev/null  ← 丢弃 stderr（进度信息去了 stderr）
#   grep -E      ← 扩展正则，匹配 Speed 或 Requests 的行
#   "Speed|Requests" ← 正则：包含 Speed 或 Requests 的行
```

输出解读：

```
Speed=906780 pages/min, 7042657 bytes/sec.
Requests: 151130 susceed, 0 failed.
```

- `pages/min` = 每分钟处理的请求数（906,780 ÷ 60 ≈ 15,113 req/s）
- `bytes/sec` = 吞吐量（约 6.7 MB/s）
- `susceed` = 成功数
- `failed` = 失败数（理想值：0）

### 7.4 `ss` 查看网络状态

```bash
ss -tlnp
#   -t = TCP 连接
#   -l = 只显示监听中的（LISTEN）
#   -n = 数字格式（不解析服务名）
#   -p = 显示使用该端口的进程

# 与 grep 配合
ss -tlnp | grep 9006
# 过滤出包含 9006 的行
```

输出示例：

```
LISTEN 0      5            0.0.0.0:9006      0.0.0.0:*    users:(("server",pid=5466,fd=8))
```

- `LISTEN` = 正在监听
- `0.0.0.0:9006` = 监听所有网卡的 9006 端口
- `pid=5466` = 进程 ID
- `fd=8` = 文件描述符编号

### 7.5 Bash 原生 TCP（`/dev/tcp`）

Bash 内建了 TCP 功能，不需要 nc：

```bash
timeout 2 bash -c '
  exec 3<>/dev/tcp/127.0.0.1/9006
  echo -e "GET /judge.html HTTP/1.1\r\nHost: localhost\r\n\r\n" >&3
  cat <&3
'
```

拆解：

```
bash -c '...'       ← 执行一段 bash 脚本

exec 3<>/dev/tcp/127.0.0.1/9006
#  exec 3<>文件      ← 打开文件描述符 3（可读可写）
#  /dev/tcp/ip/port  ← Bash 特殊路径，表示 TCP 连接
#  相当于：建立 TCP 连接到 127.0.0.1:9006

echo -e "..." >&3
#  >&3              ← 写入文件描述符 3（发送 HTTP 请求）
#  -e               ← 启用转义（\r\n 生效）

cat <&3
#  <&3              ← 从文件描述符 3 读取（接收服务器响应）
```

---

## 8. 系统信息查询

### 8.1 `which` 查找命令路径

```bash
which webbench          # 输出 /usr/local/bin/webbench（存在）
which nonexistent       # 输出空，返回非 0（不存在）
```

### 8.2 `dpkg` 包管理

```bash
dpkg -l | grep mariadb
#   -l = list，列出已安装的包

dpkg -L mariadb-server-10.6 | head -10
#   -L = list files，列出包安装的所有文件

dpkg -l | grep -E "mysql|mariadb"
#   grep -E "mysql|mariadb" = 匹配 mysql 或 mariadb
```

### 8.3 `systemctl` / `service` 服务管理

```bash
systemctl is-active mysql     # 检查 MySQL 服务是否运行中
service mysql status          # 同上（旧方式）
```

### 8.4 `ps` / `kill` 快速参考

```bash
ps aux | grep server          # 查看服务器进程
kill 12345                    # 优雅终止 PID 12345
pkill -f "^\./server$"        # 按进程名模式杀死
```

---

## 9. 综合实战：拆解一条完整命令

### 示例 1：启动 MariaDB

```bash
setsid /usr/sbin/mysqld \
  --datadir=/tmp/mariadb_temp/data \
  --socket=/tmp/mariadb_temp/run/mysql.sock \
  --port=3307 \
  --pid-file=/tmp/mariadb_temp/run/mysqld.pid \
  --skip-networking=0 \
  --user=gunnm \
  </dev/null >/dev/null 2>&1 &
```

从左到右理解：

| 部分 | 作用 |
|------|------|
| `setsid` | 新会话，脱离终端 |
| `/usr/sbin/mysqld` | MariaDB 服务器程序 |
| `\` | 反斜杠换行（一行太长，分成多行） |
| `--datadir=...` | 数据存在哪 |
| `--socket=...` | socket 文件放哪 |
| `--port=3307` | 监听 3307 端口（不是默认的 3306） |
| `--pid-file=...` | PID 写哪 |
| `--skip-networking=0` | 允许 TCP 连接 |
| `--user=gunnm` | 以当前用户运行 |
| `</dev/null` | stdin 来自空 |
| `>/dev/null` | 正常输出进黑洞 |
| `2>&1` | 错误输出也进黑洞 |
| `&` | 后台运行 |

### 示例 2：注册用户并验证

```bash
wget -q -O - --post-data="user=demo&passwd=123" \
    http://127.0.0.1:9006/3register.html
```

| 部分 | 作用 |
|------|------|
| `wget` | HTTP 客户端工具 |
| `-q` | 安静模式（不打印下载进度） |
| `-O -` | 输出到 stdout |
| `--post-data="user=demo&passwd=123"` | POST 请求体 |
| `http://.../3register.html` | 目标 URL（3 代表注册） |

### 示例 3：持续追踪日志

```bash
tail -f 2026_05_05_ServerLog
#   -f = follow，文件新增一行就立即显示
#   按 Ctrl+C 退出
```

### 示例 4：压测 + 看结果

```bash
webbench -c 100 -t 10 http://127.0.0.1:9006/judge.html 2>/dev/null | grep -E "Speed|Requests"
```

| 部分 | 作用 |
|------|------|
| `webbench -c 100 -t 10` | 100 并发，持续 10 秒 |
| `2>/dev/null` | 丢弃 stderr（进度） |
| `\|` | 管道送给 grep |
| `grep -E "Speed\|Requests"` | 只保留包含 Speed 或 Requests 的行 |

---

## 附录：命令速查表

| 命令 | 常用选项 | 用途 |
|------|---------|------|
| `ls` | `-l` `-a` `-la` | 列出文件 |
| `mkdir` | `-p` | 创建目录 |
| `rm` | `-r` `-f` | 删除文件/目录 |
| `cat` | | 显示文件内容 |
| `tail` | `-n` `-f` | 查看文件尾部 |
| `grep` | `-v` `-E` `-n` | 搜索文本 |
| `wc` | `-l` | 统计行数 |
| `ps` | `aux` | 查看进程 |
| `pkill` | `-f` | 杀进程 |
| `kill` | `-9` | 杀进程（强） |
| `sleep` | | 等待 N 秒 |
| `timeout` | | 限时运行 |
| `which` | | 查找命令路径 |
| `ss` | `-tlnp` | 查看网络状态 |
| `dpkg` | `-l` `-L` | 包管理 |
| `wget` | `-q` `-O` `--post-data=` `-d` | HTTP 客户端 |
| `nc` | | TCP 连接工具 |
| `webbench` | `-c` `-t` | HTTP 压测 |
| `mysql` | `-u` `-p` `-S` `-e` | MySQL 客户端 |
| `mysqld` | `--datadir` `--socket` `--port` | MySQL 服务器 |
