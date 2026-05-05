// WebServer 实现
//
// 构造函数: 预分配 MAX_FD 个连接和定时器槽位, 解析根路径
// 析构函数: 关闭 epoll / listenfd / pipe
// 线程池: 每个 HttpConnection 任务通过 ConnGuard 获取数据库连接

#include "webserver.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>

// ==================== 构造 / 析构 ====================

WebServer::WebServer(const ServerConfig &cfg,
                     const std::string &db_user,
                     const std::string &db_passwd,
                     const std::string &db_name)
    : cfg_(cfg),
      db_user_(db_user),
      db_passwd_(db_passwd),
      db_name_(db_name),
      users_(MAX_FD),
      users_timer_(MAX_FD)
{
    // 根路径 = 当前工作目录 + "/root"
    char buf[200];
    if (getcwd(buf, 200))
    {
        root_ = buf;
        root_ += "/root";
    }
}

WebServer::~WebServer()
{
    close(epollfd_);
    close(listenfd_);
    if (pipefd_[0] != -1)
        close(pipefd_[0]);
    if (pipefd_[1] != -1)
        close(pipefd_[1]);
}

// ==================== 初始化入口 ====================

/** 七步初始化 → 事件循环 */
void WebServer::run()
{
    init_log();
    init_db_pool();
    init_trig_mode();
    init_thread_pool();
    init_listen_socket();
    init_epoll();
    init_signals();
    event_loop();
}

/** 初始化日志系统 (同步/异步) */
void WebServer::init_log()
{
    if (cfg_.close_log)
        return;

    size_t queue_size = (cfg_.log_write == 1) ? 800 : 0;
    LogSystem::instance().init("./ServerLog", cfg_.close_log, 2000, 800000, queue_size);
}

/** 初始化数据库连接池 + 加载用户数据到内存 */
void WebServer::init_db_pool()
{
    ConnectionPool::instance().init("127.0.0.1", db_user_, db_passwd_, db_name_, 3307, cfg_.sql_num);
    HttpConnection::load_users(ConnectionPool::instance());
}

/** 初始化线程池 */
void WebServer::init_thread_pool()
{
    pool_ = std::make_unique<ThreadPool<HttpConnection>>(
        cfg_.actor_model, cfg_.thread_num);
}

/** 触发模式已经在 config.parse_arg() 中推导, 此处直接赋值 */
void WebServer::init_trig_mode()
{
    listen_trig_mode_ = cfg_.listen_trig_mode;
    conn_trig_mode_ = cfg_.conn_trig_mode;
}

/** 创建监听 socket: socket → bind → listen */
void WebServer::init_listen_socket()
{
    listenfd_ = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd_ >= 0);

    struct linger tmp{};
    if (cfg_.opt_linger == 0)
    {
        tmp.l_onoff = 0;
        tmp.l_linger = 1;
    }
    else
    {
        tmp.l_onoff = 1;
        tmp.l_linger = 1;
    }
    setsockopt(listenfd_, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));

    int flag = 1;
    setsockopt(listenfd_, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(cfg_.port);

    assert(bind(listenfd_, (struct sockaddr *)&addr, sizeof(addr)) >= 0);
    assert(listen(listenfd_, 5) >= 0);
}

/** 初始化 epoll: 创建实例 → 注册 listenfd → 创建信号管道 → 设置静态引用 */
void WebServer::init_epoll()
{
    epoll_utils_.init(TIMESLOT);

    epollfd_ = epoll_create(5);
    assert(epollfd_ != -1);

    epoll_utils_.add_fd(epollfd_, listenfd_, false, listen_trig_mode_);
    HttpConnection::epollfd_ = epollfd_;

    // 信号管道: 用于信号处理函数向事件循环发送信号编号
    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd_);
    assert(ret != -1);
    epoll_utils_.set_nonblocking(pipefd_[1]);
    epoll_utils_.add_fd(epollfd_, pipefd_[0], false, 0);

    EpollUtils::u_pipefd = pipefd_;
    EpollUtils::u_epollfd = epollfd_;
}

/** 注册信号处理: SIGPIPE 忽略, SIGALRM/SIGTERM 走管道 */
void WebServer::init_signals()
{
    epoll_utils_.add_sig(SIGPIPE, SIG_IGN);
    epoll_utils_.add_sig(SIGALRM, EpollUtils::sig_handler, false);
    epoll_utils_.add_sig(SIGTERM, EpollUtils::sig_handler, false);

    alarm(TIMESLOT);
}

// ==================== 事件循环 ====================

/** epoll_wait 主循环
 *  事件分类:
 *    listenfd        → 新连接
 *    EPOLLRDHUP/HUP/ERR → 连接异常关闭
 *    pipefd[0]       → 信号 (SIGALRM / SIGTERM)
 *    EPOLLIN         → 可读
 *    EPOLLOUT        → 可写
 *  每次循环末尾处理超时 (SIGALRM 触发) */
void WebServer::event_loop()
{
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server)
    {
        int num = epoll_wait(epollfd_, events_, MAX_EVENT_NUMBER, -1);
        if (num < 0 && errno != EINTR)
        {
            LOG_ERROR(std::string("epoll failure: ") + std::to_string(errno));
            break;
        }

        for (int i = 0; i < num; ++i)
        {
            int sockfd = events_[i].data.fd;

            if (sockfd == listenfd_)
            {
                handle_new_connection();
            }
            else if (events_[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                auto *timer = users_timer_[sockfd].timer;
                remove_timer(timer, sockfd);
            }
            else if (sockfd == pipefd_[0] && (events_[i].events & EPOLLIN))
            {
                handle_signal(timeout, stop_server);
            }
            else if (events_[i].events & EPOLLIN)
            {
                handle_read(sockfd);
            }
            else if (events_[i].events & EPOLLOUT)
            {
                handle_write(sockfd);
            }
        }

        if (timeout)
        {
            epoll_utils_.timer_handler();
            timeout = false;
        }
    }
}

// ==================== 事件处理 ====================

/** 处理新连接: accept → 检查上限 → 初始化连接 + 创建定时器
 *  LT 模式: 每次只 accept 一个
 *  ET 模式: while 循环 accept 到 EAGAIN */
void WebServer::handle_new_connection()
{
    struct sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);

    if (listen_trig_mode_ == 0)
    {
        int connfd = accept(listenfd_, (struct sockaddr *)&client_addr, &len);
        if (connfd < 0)
        {
            LOG_ERROR(std::string("accept error: ") + std::to_string(errno));
            return;
        }
        if (HttpConnection::user_count_ >= MAX_FD)
        {
            epoll_utils_.show_error(connfd, "Internal server busy");
            return;
        }
        add_timer(connfd, client_addr);
    }
    else
    {
        while (true)
        {
            int connfd = accept(listenfd_, (struct sockaddr *)&client_addr, &len);
            if (connfd < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                LOG_ERROR(std::string("accept error: ") + std::to_string(errno));
                break;
            }
            if (HttpConnection::user_count_ >= MAX_FD)
            {
                epoll_utils_.show_error(connfd, "Internal server busy");
                break;
            }
            add_timer(connfd, client_addr);
        }
    }
}

/** 处理信号: 从管道读取信号编号
 *  SIGALRM → 设 timeout=true (触发定时器)
 *  SIGTERM → 设 stop_server=true (退出事件循环) */
void WebServer::handle_signal(bool &timeout, bool &stop)
{
    char signals[1024];
    int ret = recv(pipefd_[0], signals, sizeof(signals), 0);
    if (ret <= 0)
        return;

    for (int i = 0; i < ret; ++i)
    {
        switch (signals[i])
        {
        case SIGALRM:
            timeout = true;
            break;
        case SIGTERM:
            stop = true;
            break;
        }
    }
}

/** 处理读事件
 *  Reactor: 入队, 工作线程读数据 (spin-wait improv 同步)
 *  Proactor: 主线程读数据, 然后入队 */
void WebServer::handle_read(int sockfd)
{
    auto *timer = users_timer_[sockfd].timer;

    if (cfg_.actor_model == 1)
    {
        // Reactor: 工作线程负责 I/O
        if (timer)
            adjust_timer(timer);

        pool_->append(&users_[sockfd], 0);

        // 自旋等待工作线程设置 improv (Reactor 同步协议)
        while (true)
        {
            if (users_[sockfd].improv == 1)
            {
                if (users_[sockfd].timer_flag == 1)
                    remove_timer(timer, sockfd);
                users_[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        // Proactor: 主线程读, worker 只处理
        if (users_[sockfd].read_once())
        {
            LOG_INFO(std::string("deal with client: ") + inet_ntoa(users_[sockfd].get_address()->sin_addr));
            pool_->append_p(&users_[sockfd]);
            if (timer)
                adjust_timer(timer);
        }
        else
        {
            remove_timer(timer, sockfd);
        }
    }
}

/** 处理写事件
 *  Reactor: 入队, 工作线程写 (spin-wait improv)
 *  Proactor: 主线程直接写 */
void WebServer::handle_write(int sockfd)
{
    auto *timer = users_timer_[sockfd].timer;

    if (cfg_.actor_model == 1)
    {
        if (timer)
            adjust_timer(timer);

        pool_->append(&users_[sockfd], 1);

        while (true)
        {
            if (users_[sockfd].improv == 1)
            {
                if (users_[sockfd].timer_flag == 1)
                    remove_timer(timer, sockfd);
                users_[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        if (users_[sockfd].write())
        {
            if (timer)
                adjust_timer(timer);
        }
        else
        {
            remove_timer(timer, sockfd);
        }
    }
}

// ==================== 定时器管理 ====================

/** 为新连接创建定时器: 初始化 HttpConnection → 创建 TimerNode → 加入链表
 *  超时时间 = now + 3 * TIMESLOT (默认 15 秒) */
void WebServer::add_timer(int connfd, const sockaddr_in &addr)
{
    users_[connfd].init(connfd, addr, root_, conn_trig_mode_, cfg_.close_log);

    users_timer_[connfd].address = addr;
    users_timer_[connfd].sockfd = connfd;

    auto *timer = new TimerNode;
    timer->user_data = &users_timer_[connfd];
    timer->cb_func = [this](ClientData *data) {
        users_[data->sockfd].close_conn(true);
    };
    timer->expire = std::chrono::steady_clock::now() + std::chrono::seconds(3 * TIMESLOT);

    users_timer_[connfd].timer = timer;
    epoll_utils_.timer_list_.add_timer(timer);
}

/** 延长定时器超时: 有数据活动时调用, 重置 expire 并重排 */
void WebServer::adjust_timer(TimerNode *timer)
{
    if (!timer)
        return;
    timer->expire = std::chrono::steady_clock::now() + std::chrono::seconds(3 * TIMESLOT);
    epoll_utils_.timer_list_.adjust_timer(timer);
}

/** 删除定时器: 执行回调关闭连接, 然后从链表中移除 */
void WebServer::remove_timer(TimerNode *timer, int sockfd)
{
    if (!timer)
        return;
    timer->cb_func(&users_timer_[sockfd]);
    epoll_utils_.timer_list_.del_timer(timer);
}
