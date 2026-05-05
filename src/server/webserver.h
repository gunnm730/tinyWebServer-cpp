// WebServer 编排器
//
// 生命周期: run() 内部按序执行七步初始化后进入事件循环
//
// 初始化顺序:
//   1. init_log()          — 日志系统
//   2. init_db_pool()      — 数据库连接池 + 加载用户数据
//   3. init_trig_mode()    — 触发模式 (已在 config.parse_arg 中推导)
//   4. init_thread_pool()  — 线程池
//   5. init_listen_socket() — 监听 socket
//   6. init_epoll()        — epoll 实例 + 信号管道
//   7. init_signals()      — 信号注册 + 启动 alarm
//
// 事件循环: epoll_wait → 五种事件分发
//   listenfd     → handle_new_connection()
//   EPOLLERR     → remove_timer() 关闭连接
//   pipefd[0]    → handle_signal() (SIGALRM / SIGTERM)
//   EPOLLIN      → handle_read() 
//   EPOLLOUT     → handle_write()
//   timeout      → EpollUtils.timer_handler()

#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <array>
#include <memory>
#include <string>
#include <vector>

#include <netinet/in.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "../config/config.h"
#include "../http/http_connection.h"
#include "../threadpool/thread_pool.h"
#include "../timer/timer_manager.h"

constexpr int MAX_FD = 65536;            // 最大连接数
constexpr int MAX_EVENT_NUMBER = 10000;  // epoll_wait 单次最大事件数
constexpr int TIMESLOT = 5;              // alarm 间隔 (秒), 超时窗口 = 3 * TIMESLOT

class WebServer
{
public:
    WebServer(const ServerConfig &cfg,
              const std::string &db_user,
              const std::string &db_passwd,
              const std::string &db_name);

    ~WebServer();

    WebServer(const WebServer &) = delete;
    WebServer &operator=(const WebServer &) = delete;

    void run();

private:
    // ==================== 初始化步骤 (按序调用) ====================
    void init_log();
    void init_db_pool();
    void init_thread_pool();
    void init_trig_mode();
    void init_listen_socket();
    void init_epoll();
    void init_signals();

    // ==================== 事件循环 ====================
    void event_loop();
    void handle_new_connection();
    void handle_signal(bool &timeout, bool &stop);
    void handle_read(int sockfd);
    void handle_write(int sockfd);

    // ==================== 定时器管理 ====================
    void add_timer(int connfd, const sockaddr_in &addr);
    void adjust_timer(TimerNode *timer);
    void remove_timer(TimerNode *timer, int sockfd);

    // 配置
    ServerConfig cfg_;
    int listen_trig_mode_ = 0;
    int conn_trig_mode_ = 0;

    // 数据库凭据
    std::string db_user_;
    std::string db_passwd_;
    std::string db_name_;

    // 网站根目录 (cwd + "/root")
    std::string root_;

    // epoll
    int epollfd_ = -1;
    int listenfd_ = -1;
    epoll_event events_[MAX_EVENT_NUMBER]{};

    // 信号管道 (socketpair)
    int pipefd_[2] = {-1, -1};

    // 连接数组 (预分配, 以 fd 为下标)
    std::vector<HttpConnection> users_;
    std::vector<ClientData> users_timer_;

    // 线程池
    std::unique_ptr<ThreadPool<HttpConnection>> pool_;

    // 定时器 + epoll 工具
    EpollUtils epoll_utils_;
};

#endif
