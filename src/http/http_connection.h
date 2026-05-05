// HTTP 连接处理模块
//
// 核心职责:
//   1. 使用有限状态机解析 HTTP 请求报文 (GET/POST)
//   2. CGI 处理: 用户注册 (/3X) 和登录 (/2X)
//   3. 静态文件服务: mmap 零拷贝 + writev 分散发送
//
// 状态机流程:
//   CHECK_STATE_REQUESTLINE → CHECK_STATE_HEADER → CHECK_STATE_CONTENT
//   每一步由对应的 parse_*() 处理, 完成后调用 do_request() 进行路由
//
// 线程安全:
//   epollfd_ / user_count_ — 所有连接共享
//   users_ — 全局用户 map (mutex 保护)
//   improv / timer_flag — Reactor 模式下与主线程同步
//
// MmapFile RAII 封装:
//   open_file() 时 mmap, 析构或 release() 时 munmap
//   move-only, 避免意外拷贝导致重复 munmap

#ifndef HTTP_CONNECTION_H
#define HTTP_CONNECTION_H

#include <atomic>
#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "../database/connection_pool.h"
#include "../log/log_system.h"
#include "../timer/timer_manager.h"

constexpr int HTTP_FILENAME_LEN = 200;
constexpr int HTTP_READ_BUF_SIZE = 2048;
constexpr int HTTP_WRITE_BUF_SIZE = 1024;

class MmapFile
{
public:
    MmapFile() = default;

    // 打开文件并映射到内存
    // 返回 false: 文件不存在 / 无权限 / 是目录 / mmap 失败
    bool open_file(const std::string &path)
    {
        struct stat st;
        if (stat(path.c_str(), &st) < 0)
            return false;
        if (!(st.st_mode & S_IROTH) || S_ISDIR(st.st_mode))
            return false;

        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0)
            return false;

        size_ = st.st_size;
        data_ = static_cast<char *>(mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0));
        ::close(fd);

        if (data_ == MAP_FAILED)
        {
            data_ = nullptr;
            size_ = 0;
            return false;
        }
        return true;
    }

    ~MmapFile() { release(); }

    MmapFile(MmapFile &&other) noexcept
        : data_(other.data_), size_(other.size_)
    {
        other.data_ = nullptr;
        other.size_ = 0;
    }

    MmapFile &operator=(MmapFile &&other) noexcept
    {
        if (this != &other)
        {
            release();
            data_ = other.data_;
            size_ = other.size_;
            other.data_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    MmapFile(const MmapFile &) = delete;
    MmapFile &operator=(const MmapFile &) = delete;

    void release()
    {
        if (data_)
        {
            munmap(data_, size_);
            data_ = nullptr;
            size_ = 0;
        }
    }

    char *data() const { return data_; }
    size_t size() const { return size_; }
    explicit operator bool() const { return data_ != nullptr; }

private:
    char *data_ = nullptr;
    size_t size_ = 0;
};

// HTTP 连接类
class HttpConnection
{
public:
    // HTTP 请求方法
    enum class Method
    {
        GET,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };

    // 解析状态机的阶段
    enum class CheckState
    {
        RequestLine,
        Header,
        Content
    };

    // HTTP 请求解析结果
    enum class HttpCode
    {
        NoRequest,        // 请求不完整, 需继续读
        GetRequest,       // 请求已完整获取
        BadRequest,       // 请求语法错误
        NoResource,       // 资源不存在
        ForbiddenRequest, // 资源禁止访问
        FileRequest,      // 文件请求成功
        InternalError,    // 服务器内部错误
        ClosedConnection  // 连接已关闭
    };

    // 从缓冲区取一行的结果
    enum class LineStatus
    {
        Ok,
        Bad,
        Open
    };

    HttpConnection() = default;
    ~HttpConnection() { close_conn(true); }

    HttpConnection(const HttpConnection &) = delete;
    HttpConnection &operator=(const HttpConnection &) = delete;

    // 初始化连接 (WebServer.add_timer 中调用)
    void init(int sockfd, const sockaddr_in &addr,
              const std::string &root, int trig_mode, int close_log);

    void close_conn(bool real_close = true);
    void process();   // 顶层入口: read → parse → write
    bool read_once(); // 读取 socket 数据到读缓冲区
    bool write();     // 通过 writev 发送响应
    sockaddr_in *get_address() { return &addr_; }

    // 线程池直接访问的成员
    int m_state = 0;    // 0=读, 1=写 (Reactor 模式用)
    int timer_flag = 0; // 1=需要关闭连接 (Reactor 模式用)
    int improv = 0;     // 1=处理完成 (Reactor 同步用)

    // 启动时从数据库加载用户数据到全局 map
    static void load_users(ConnectionPool &pool);

    // 所有连接共享
    static int epollfd_;
    static std::atomic<int> user_count_;

private:
    void reset(); // 重置解析状态 (keep-alive 复用连接)

    // HTTP 解析
    HttpCode process_read();
    bool process_write(HttpCode ret);
    LineStatus parse_line();                 // 从状态机: 从缓冲区取出一行
    HttpCode parse_request_line(char *text); // 解析请求行
    HttpCode parse_headers(char *text);      // 解析请求头
    HttpCode parse_content(char *text);      // 解析请求体
    HttpCode do_request();                   // 路由: 文件 / CGI
    char *get_line() { return read_buf_.data() + start_line_; }

    // 响应构建
    void build_error_response(HttpCode ret);
    void build_file_response();

    // ==================== 成员变量 ====================

    // 连接
    int sockfd_ = -1;
    sockaddr_in addr_{};

    // 读缓冲区 (解析过程中原位写 \0 分割行)
    std::array<char, HTTP_READ_BUF_SIZE> read_buf_{};
    size_t read_idx_ = 0;    // 已读入的数据量
    size_t checked_idx_ = 0; // 已解析的位置
    size_t start_line_ = 0;  // 当前行的起始位置

    // 写缓冲区 (存放响应头)
    std::array<char, HTTP_WRITE_BUF_SIZE> write_buf_{};
    size_t write_idx_ = 0;

    // 解析状态
    CheckState check_state_ = CheckState::RequestLine;
    Method method_ = Method::GET;
    std::string url_;         // 请求 URL
    std::string version_;     // HTTP 版本
    std::string host_;        // Host 头部
    std::string real_file_;   // 解析后的文件路径
    long content_length_ = 0; // Content-Length 头部
    bool keep_alive_ = false; // Connection: keep-alive
    int is_cgi_ = 0;          // 0=GET, 1=POST
    std::string post_body_;   // POST 请求体

    // 文件服务
    MmapFile mmap_file_;      // mmap 映射的文件
    struct stat file_stat_{}; // 文件状态

    // writev I/O (分散/聚集发送)
    struct iovec io_vec_[2]{}; // [0]=响应头, [1]=文件数据
    int io_vec_count_ = 0;     // iovec 数量 (1 或 2)
    int bytes_to_send_ = 0;    // 待发送总字节
    int bytes_have_sent_ = 0;  // 已发送字节

    // 配置
    std::string doc_root_; // 网站根目录
    int trig_mode_ = 0;    // 触发模式 (LT/ET)
    int close_log_ = 0;    // 日志开关

    // 全局用户数据库 (所有连接共享)
    static std::unordered_map<std::string, std::string> users_;
    static std::mutex users_mutex_;
};

#endif
