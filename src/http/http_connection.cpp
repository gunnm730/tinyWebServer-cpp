// HTTP 连接处理模块实现
//
// 完整请求处理流程:
//   1. read_once()       — 从 socket 读取数据到 std::array 读缓冲区
//   2. process_read()    — 驱动状态机解析请求
//      ├ parse_line()    — 从状态机: 逐行提取 (原位 \0 分割)
//      ├ parse_request_line() — 解析请求行 (Method / URL / Version)
//      ├ parse_headers()      — 解析请求头 (Connection / Content-Length / Host)
//      ├ parse_content()      — 解析请求体 (POST 数据)
//      └ do_request()         — 路由: 静态文件 / CGI 注册登录
//   3. process_write()   — 构建 HTTP 响应
//      ├ build_file_response()    — 200 OK + mmap 文件数据 (双 iovec)
//      └ build_error_response()   — 4xx/5xx 错误页 (单 iovec)
//   4. write()           — writev 分散/聚集发送
//
// CGI (注册/登录) 流程:
//   POST /2X → 登录校验 (查 users_ map)
//   POST /3X → 注册 (INSERT 数据库 + 更新 users_ map)
//
// 文件服务路由:
//   /0 → register.html
//   /1 → log.html
//   /5 → picture.html
//   /6 → video.html
//   /7 → fans.html

#include "http_connection.h"

#include <memory>
#include <sstream>

// ==================== 静态成员定义 ====================

int HttpConnection::epollfd_ = -1;
std::atomic<int> HttpConnection::user_count_{0};
std::unordered_map<std::string, std::string> HttpConnection::users_;
std::mutex HttpConnection::users_mutex_;

// ==================== 全局用户数据加载 ====================

/** 从 MySQL 加载所有用户名密码到静态 users_ map
 *  启动时调用一次, 后续查询直接查内存, 无需重复查库 */
void HttpConnection::load_users(ConnectionPool &pool)
{
    try
    {
        ConnGuard guard(pool);
        std::unique_ptr<sql::Statement> stmt(guard->createStatement());
        std::unique_ptr<sql::ResultSet> res(
            stmt->executeQuery("SELECT username,passwd FROM user"));

        std::lock_guard lock(users_mutex_);
        users_.clear();
        while (res->next())
        {
            users_[res->getString("username")] = res->getString("passwd");
        }
    }
    catch (const sql::SQLException &e)
    {
        LOG_ERROR(std::string("load_users SQL error: ") + e.what());
    }
}

// ==================== 连接生命周期 ====================

/** 关闭连接: 从 epoll 移除, 关闭 socket, 减少连接计数 */
void HttpConnection::close_conn(bool real_close)
{
    if (real_close && sockfd_ != -1)
    {
        EpollUtils::remove_fd(epollfd_, sockfd_);
        sockfd_ = -1;
        --user_count_;
    }
}

/** 初始化新连接: 注册 epoll 事件, 重置解析状态 */
void HttpConnection::init(int sockfd, const sockaddr_in &addr,
                          const std::string &root, int trig_mode, int close_log)
{
    sockfd_ = sockfd;
    addr_ = addr;
    doc_root_ = root;
    trig_mode_ = trig_mode;
    close_log_ = close_log;

    EpollUtils::add_fd(epollfd_, sockfd_, true, trig_mode_);
    ++user_count_;

    reset();
}

/** 重置解析状态 (keep-alive 复用连接时调用) */
void HttpConnection::reset()
{
    read_idx_ = 0;
    checked_idx_ = 0;
    start_line_ = 0;
    write_idx_ = 0;
    check_state_ = CheckState::RequestLine;
    keep_alive_ = false;
    method_ = Method::GET;
    url_.clear();
    version_.clear();
    host_.clear();
    real_file_.clear();
    content_length_ = 0;
    is_cgi_ = 0;
    post_body_.clear();
    bytes_to_send_ = 0;
    bytes_have_sent_ = 0;
    io_vec_count_ = 0;
    io_vec_[0] = {nullptr, 0};
    io_vec_[1] = {nullptr, 0};
    mmap_file_.release();
    std::memset(read_buf_.data(), 0, read_buf_.size());
    std::memset(write_buf_.data(), 0, write_buf_.size());
    timer_flag = 0;
    improv = 0;
    m_state = 0;
}

// ==================== HTTP 请求读取 ====================

/** 从状态机: 从缓冲区提取一行 (\r\n 或 \n 分隔)
 *  找到后将 \r\n 替换为 \0\0, 方便后续字符串处理
 *  返回 LINE_OK=完整行, LINE_BAD=格式错误, LINE_OPEN=需更多数据 */
HttpConnection::LineStatus HttpConnection::parse_line()
{
    for (; checked_idx_ < read_idx_; ++checked_idx_)
    {
        char c = read_buf_[checked_idx_];
        if (c == '\r')
        {
            if (checked_idx_ + 1 == read_idx_)
                return LineStatus::Open;
            if (read_buf_[checked_idx_ + 1] == '\n')
            {
                read_buf_[checked_idx_++] = '\0';
                read_buf_[checked_idx_++] = '\0';
                return LineStatus::Ok;
            }
            return LineStatus::Bad;
        }
        if (c == '\n')
        {
            if (checked_idx_ > 1 && read_buf_[checked_idx_ - 1] == '\r')
            {
                read_buf_[checked_idx_ - 1] = '\0';
                read_buf_[checked_idx_++] = '\0';
                return LineStatus::Ok;
            }
            return LineStatus::Bad;
        }
    }
    return LineStatus::Open;
}

/** 从 socket 读取数据到读缓冲区
 *  LT 模式: 只读一次
 *  ET 模式: 循环读到 EAGAIN (必须读完, 否则事件丢失) */
bool HttpConnection::read_once()
{
    if (read_idx_ >= HTTP_READ_BUF_SIZE)
        return false;

    if (trig_mode_ == 0)
    {
        int n = recv(sockfd_, read_buf_.data() + read_idx_,
                     HTTP_READ_BUF_SIZE - read_idx_, 0);
        if (n <= 0)
            return false;
        read_idx_ += n;
        return true;
    }

    while (true)
    {
        int n = recv(sockfd_, read_buf_.data() + read_idx_,
                     HTTP_READ_BUF_SIZE - read_idx_, 0);
        if (n == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return false;
        }
        if (n == 0)
            return false;
        read_idx_ += n;
    }
    return true;
}

// ==================== HTTP 请求解析 ====================

/** 解析请求行 "GET /url HTTP/1.1"
 *  提取 Method / URL / Version
 *  去除 http:// 和 https:// 前缀
 *  默认 "/" → "/judge.html" */
HttpConnection::HttpCode HttpConnection::parse_request_line(char *text)
{
    char *url = strpbrk(text, " \t");
    if (!url)
        return HttpCode::BadRequest;
    *url++ = '\0';

    if (strcasecmp(text, "GET") == 0)
        method_ = Method::GET;
    else if (strcasecmp(text, "POST") == 0)
    {
        method_ = Method::POST;
        is_cgi_ = 1;
    }
    else
        return HttpCode::BadRequest;

    url += strspn(url, " \t");
    char *ver = strpbrk(url, " \t");
    if (!ver)
        return HttpCode::BadRequest;
    *ver++ = '\0';
    ver += strspn(ver, " \t");

    if (strcasecmp(ver, "HTTP/1.1") != 0)
        return HttpCode::BadRequest;

    std::string_view url_view(url);
    if (url_view.substr(0, 7) == "http://")
    {
        url_view.remove_prefix(7);
        auto pos = url_view.find('/');
        if (pos == std::string_view::npos)
            return HttpCode::BadRequest;
        url_view.remove_prefix(pos);
    }
    else if (url_view.substr(0, 8) == "https://")
    {
        url_view.remove_prefix(8);
        auto pos = url_view.find('/');
        if (pos == std::string_view::npos)
            return HttpCode::BadRequest;
        url_view.remove_prefix(pos);
    }

    if (url_view.empty() || url_view[0] != '/')
        return HttpCode::BadRequest;

    url_ = url_view;
    version_ = ver;

    if (url_ == "/")
        url_ = "/judge.html";

    check_state_ = CheckState::Header;
    return HttpCode::NoRequest;
}

/** 解析请求头 (大小写不敏感)
 *  支持:
 *    Connection: keep-alive / close
 *    Content-length: body 字节数
 *    Host: 服务器主机名
 *  空行表示头部结束, 若有 body 则进入 Content 状态 */
HttpConnection::HttpCode HttpConnection::parse_headers(char *text)
{
    if (text[0] == '\0')
    {
        if (content_length_ != 0)
        {
            check_state_ = CheckState::Content;
            return HttpCode::NoRequest;
        }
        return HttpCode::GetRequest;
    }

    std::string_view line(text);

    // Connection 头部
    if (line.size() >= 11)
    {
        bool match = true;
        for (int i = 0; i < 11; ++i)
            if (std::tolower(line[i]) != std::tolower("Connection:"[i]))
            {
                match = false;
                break;
            }
        if (match)
        {
            auto val = line.substr(11);
            val.remove_prefix(std::min(val.find_first_not_of(" \t"), val.size()));
            if (val == "keep-alive")
                keep_alive_ = true;
            return HttpCode::NoRequest;
        }
    }

    // Content-length 头部
    if (line.size() >= 15)
    {
        bool match = true;
        for (int i = 0; i < 15; ++i)
            if (std::tolower(line[i]) != std::tolower("Content-length:"[i]))
            {
                match = false;
                break;
            }
        if (match)
        {
            auto val = line.substr(15);
            val.remove_prefix(std::min(val.find_first_not_of(" \t"), val.size()));
            content_length_ = std::atol(val.data());
            return HttpCode::NoRequest;
        }
    }

    // Host 头部
    if (line.size() >= 5)
    {
        bool match = true;
        for (int i = 0; i < 5; ++i)
            if (std::tolower(line[i]) != std::tolower("Host:"[i]))
            {
                match = false;
                break;
            }
        if (match)
        {
            auto val = line.substr(5);
            val.remove_prefix(std::min(val.find_first_not_of(" \t"), val.size()));
            host_ = val;
            return HttpCode::NoRequest;
        }
    }

    return HttpCode::NoRequest;
}

/** 解析 POST 请求体: 判断数据是否完整到达 */
HttpConnection::HttpCode HttpConnection::parse_content(char *text)
{
    if (read_idx_ >= static_cast<size_t>(content_length_ + checked_idx_))
    {
        text[content_length_] = '\0';
        post_body_ = text;
        return HttpCode::GetRequest;
    }
    return HttpCode::NoRequest;
}

/** 主状态机: 驱动解析流程
 *  循环调用 parse_line() 取行, 按 check_state_ 分派到对应解析函数
 *  当请求完整 (GetRequest) 时调用 do_request() 路由 */
HttpConnection::HttpCode HttpConnection::process_read()
{
    auto line_status = LineStatus::Ok;
    HttpCode ret = HttpCode::NoRequest;

    while ((check_state_ == CheckState::Content && line_status == LineStatus::Ok) ||
           ((line_status = parse_line()) == LineStatus::Ok))
    {
        char *text = get_line();
        start_line_ = checked_idx_;

        switch (check_state_)
        {
        case CheckState::RequestLine:
        {
            ret = parse_request_line(text);
            if (ret == HttpCode::BadRequest)
                return HttpCode::BadRequest;
            break;
        }
        case CheckState::Header:
        {
            ret = parse_headers(text);
            if (ret == HttpCode::BadRequest)
                return HttpCode::BadRequest;
            if (ret == HttpCode::GetRequest)
                return do_request();
            break;
        }
        case CheckState::Content:
        {
            ret = parse_content(text);
            if (ret == HttpCode::GetRequest)
                return do_request();
            line_status = LineStatus::Open;
            break;
        }
        }
    }
    return HttpCode::NoRequest;
}

// ==================== 请求路由 ====================

/** 路由处理: 根据 URL 路径映射到具体资源
 *
 *  CGI (POST 且 URL 末段以 2 或 3 开头):
 *    /3X → 注册: 查重 → INSERT INTO user → 更新 users_ map
 *    /2X → 登录: 查 users_ map 校验密码
 *    body 格式: "user=xxx&passwd=xxx"
 *
 *  静态页面路由 (末字符):
 *    0 → register.html
 *    1 → log.html
 *    5 → picture.html
 *    6 → video.html
 *    7 → fans.html
 *    其他 → 直接作为文件路径
 *
 *  文件校验: 存在 / 有读权限 / 不是目录 → mmap 映射 */
HttpConnection::HttpCode HttpConnection::do_request()
{
    auto slash_pos = url_.rfind('/');
    if (slash_pos == std::string::npos || slash_pos + 1 >= url_.size())
        return HttpCode::BadRequest;

    char route_flag = url_[slash_pos + 1];

    if (is_cgi_ == 1 && (route_flag == '2' || route_flag == '3'))
    {
        std::string page_path = "/" + url_.substr(slash_pos + 2);
        real_file_ = doc_root_ + page_path;

        auto user_pos = post_body_.find("user=");
        auto passwd_pos = post_body_.find("&passwd=");
        if (user_pos == std::string::npos || passwd_pos == std::string::npos)
            return HttpCode::BadRequest;

        std::string name = post_body_.substr(user_pos + 5, passwd_pos - user_pos - 5);
        std::string password = post_body_.substr(passwd_pos + 8);

        if (route_flag == '3')
        {
            // 注册: 用户名不重复则写入数据库
            std::lock_guard lock(users_mutex_);
            if (users_.find(name) == users_.end())
            {
                try
                {
                    ConnGuard guard(ConnectionPool::instance());
                    std::unique_ptr<sql::Statement> stmt(guard->createStatement());
                    std::string sql = "INSERT INTO user(username, passwd) VALUES('";
                    sql += name + "', '" + password + "')";
                    stmt->execute(sql);
                    users_[name] = password;
                    url_ = "/log.html";
                }
                catch (const sql::SQLException &e)
                {
                    LOG_ERROR(std::string("Register SQL error: ") + e.what());
                    url_ = "/registerError.html";
                }
            }
            else
            {
                url_ = "/registerError.html";
            }
        }
        else
        {
            // 登录: 查找用户名密码是否匹配
            std::lock_guard lock(users_mutex_);
            auto it = users_.find(name);
            url_ = (it != users_.end() && it->second == password)
                       ? "/welcome.html"
                       : "/logError.html";
        }

        // CGI 可能修改了 url_ (重定向), 重新计算 real_file_
        slash_pos = url_.rfind('/');
        route_flag = url_[slash_pos + 1];
        goto reroute;
    }

reroute:
    // 静态页面路由 (也被 CGI 重定向后跳转至此)
    switch (route_flag)
    {
    case '0':
        real_file_ = doc_root_ + "/register.html";
        break;
    case '1':
        real_file_ = doc_root_ + "/log.html";
        break;
    case '5':
        real_file_ = doc_root_ + "/picture.html";
        break;
    case '6':
        real_file_ = doc_root_ + "/video.html";
        break;
    case '7':
        real_file_ = doc_root_ + "/fans.html";
        break;
    default:
        real_file_ = doc_root_ + url_;
        break;
    }

    // 文件校验
    if (stat(real_file_.c_str(), &file_stat_) < 0)
        return HttpCode::NoResource;

    if (!(file_stat_.st_mode & S_IROTH))
        return HttpCode::ForbiddenRequest;

    if (S_ISDIR(file_stat_.st_mode))
        return HttpCode::BadRequest;

    if (!mmap_file_.open_file(real_file_))
        return HttpCode::NoResource;

    return HttpCode::FileRequest;
}

// ==================== 响应构建 ====================

/** 构建错误响应 (4xx/5xx)
 *  使用 std::ostringstream 拼装响应头 + body
 *  单 iovec: 只发响应头+错误正文 */
void HttpConnection::build_error_response(HttpCode ret)
{
    std::ostringstream oss;
    std::string body;

    auto add_line = [&](int code, const char *title, const std::string &body_text) {
        body = body_text;
        oss << "HTTP/1.1 " << code << " " << title << "\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n"
            << "Content-Type: text/html\r\n"
            << "\r\n"
            << body;
    };

    switch (ret)
    {
    case HttpCode::InternalError:
        add_line(500, "Internal Error", "Internal Server Error.\n");
        break;
    case HttpCode::BadRequest:
        add_line(400, "Bad Request", "Your request has bad syntax.\n");
        break;
    case HttpCode::ForbiddenRequest:
        add_line(403, "Forbidden", "You do not have permission.\n");
        break;
    case HttpCode::NoResource:
        add_line(404, "Not Found", "The requested file was not found.\n");
        break;
    default:
        add_line(500, "Internal Error", "Unknown error.\n");
        break;
    }

    std::string header_str = oss.str();
    size_t copy_len = std::min(header_str.size(), write_buf_.size() - 1);
    std::memcpy(write_buf_.data(), header_str.data(), copy_len);
    write_idx_ = copy_len;

    io_vec_[0].iov_base = write_buf_.data();
    io_vec_[0].iov_len = write_idx_;
    io_vec_count_ = 1;
    bytes_to_send_ = write_idx_;
}

/** 构建文件响应 (200 OK)
 *  双 iovec: [0]=响应头, [1]=mmap 文件数据
 *  通过 writev 一次系统调用发送两部分数据 (零拷贝) */
void HttpConnection::build_file_response()
{
    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n"
        << "Content-Length: " << file_stat_.st_size << "\r\n"
        << "Connection: " << (keep_alive_ ? "keep-alive" : "close") << "\r\n"
        << "Content-Type: text/html\r\n"
        << "\r\n";

    std::string header_str = oss.str();
    size_t copy_len = std::min(header_str.size(), write_buf_.size() - 1);
    std::memcpy(write_buf_.data(), header_str.data(), copy_len);
    write_idx_ = copy_len;

    io_vec_[0].iov_base = write_buf_.data();
    io_vec_[0].iov_len = write_idx_;
    io_vec_[1].iov_base = mmap_file_.data();
    io_vec_[1].iov_len = file_stat_.st_size;
    io_vec_count_ = 2;
    bytes_to_send_ = write_idx_ + file_stat_.st_size;
}

/** 根据解析结果调用对应的响应构建函数 */
bool HttpConnection::process_write(HttpCode ret)
{
    switch (ret)
    {
    case HttpCode::FileRequest:
        build_file_response();
        return true;
    case HttpCode::InternalError:
    case HttpCode::BadRequest:
    case HttpCode::ForbiddenRequest:
    case HttpCode::NoResource:
        build_error_response(ret);
        return true;
    default:
        return false;
    }
}

// ==================== 响应发送 ====================

/** 通过 writev 发送 HTTP 响应
 *  支持大文件分片发送 (writev 返回 EAGAIN 时重新注册 EPOLLOUT)
 *  发送完成后:
 *    若 keep-alive → reset() 等待下一个请求
 *    否则 → 关闭连接 */
bool HttpConnection::write()
{
    if (bytes_to_send_ == 0)
    {
        EpollUtils::mod_fd(epollfd_, sockfd_, EPOLLIN, trig_mode_);
        reset();
        return true;
    }

    while (true)
    {
        int n = writev(sockfd_, io_vec_, io_vec_count_);
        if (n < 0)
        {
            if (errno == EAGAIN)
            {
                EpollUtils::mod_fd(epollfd_, sockfd_, EPOLLOUT, trig_mode_);
                return true;
            }
            mmap_file_.release();
            return false;
        }

        bytes_have_sent_ += n;
        bytes_to_send_ -= n;

        // 调整 iovec 指针: 响应头发完则只调整文件 iovec
        if (bytes_have_sent_ >= static_cast<int>(io_vec_[0].iov_len))
        {
            io_vec_[0].iov_len = 0;
            io_vec_[1].iov_base = mmap_file_.data() + (bytes_have_sent_ - write_idx_);
            io_vec_[1].iov_len = bytes_to_send_;
        }
        else
        {
            io_vec_[0].iov_base = write_buf_.data() + bytes_have_sent_;
            io_vec_[0].iov_len = io_vec_[0].iov_len - bytes_have_sent_;
        }

        if (bytes_to_send_ <= 0)
        {
            mmap_file_.release();
            EpollUtils::mod_fd(epollfd_, sockfd_, EPOLLIN, trig_mode_);

            if (keep_alive_)
            {
                reset();
                return true;
            }
            return false;
        }
    }
}

// ==================== 顶层入口 ====================

/** 请求处理入口 (线程池工作线程调用)
 *  1. process_read() 解析 HTTP 请求
 *     若请求不完整 → 重新注册 EPOLLIN 等待更多数据
 *  2. process_write() 构建响应
 *     若失败 → 关闭连接
 *  3. 注册 EPOLLOUT, 等待可写事件触发 write() */
void HttpConnection::process()
{
    HttpCode ret = process_read();
    if (ret == HttpCode::NoRequest)
    {
        EpollUtils::mod_fd(epollfd_, sockfd_, EPOLLIN, trig_mode_);
        return;
    }

    if (!process_write(ret))
    {
        close_conn();
        return;
    }

    EpollUtils::mod_fd(epollfd_, sockfd_, EPOLLOUT, trig_mode_);
}
