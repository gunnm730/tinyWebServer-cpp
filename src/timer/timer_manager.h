// 定时器管理器 + epoll 工具集 (header-only)
//
// 功能:
//   1. TimerList — 基于升序双向链表的定时器容器
//      每个客户端连接关联一个 TimerNode, 按过期时间升序排列
//      tick() 从头遍历, 处理所有已到期节点
//   2. EpollUtils — 封装 epoll 添加/删除/修改 + 信号处理 + 定时器总控
//
// 协作流程:
//   WebServer.add_timer()  → new TimerNode, set expire = now + 3*TIMESLOT
//   WebServer.adjust_timer() → 延长 expire, 重排位置
//   WebServer.eventLoop() 收到 SIGALRM → EpollUtils.timer_handler()
//     → TimerList.tick() → 调用回调 (关闭连接)
//     → alarm(TIMESLOT) 重新设闹钟
//
// 连接超时: 3 个 TIMESLOT 内无数据则断开
// TIMESLOT 默认 5 秒, 即超时窗口 15 秒

#ifndef TIMER_MANAGER_H
#define TIMER_MANAGER_H

#include <cstring>
#include <functional>
#include <memory>

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "../log/log_system.h"

struct ClientData;
class TimerNode;

// 超时回调函数类型: std::function 替代 C 函数指针
using TimerCallback = std::function<void(ClientData *)>;

// 客户端连接数据结构
// 每个活跃连接对应一个 ClientData, 记录地址 / fd / 关联定时器
struct ClientData
{
    sockaddr_in address{};  // 客户端地址
    int sockfd = -1;        // 连接 socket fd
    TimerNode *timer = nullptr;  // 指向关联的定时器节点
};

// 定时器节点 (双向链表)
// 按 expire 升序排列, 最早过期的在 head
class TimerNode
{
public:
    TimerNode() = default;

    std::chrono::steady_clock::time_point expire;  // 过期时间点
    TimerCallback cb_func;       // 超时回调 (通常为 cb_func, 关闭连接)
    ClientData *user_data = nullptr;  // 关联的客户端数据
    TimerNode *prev = nullptr;   // 链表前驱
    TimerNode *next = nullptr;   // 链表后继
};

// 升序定时器链表
// 操作复杂度:
//   add_timer: O(n)
//   adjust_timer: O(1)+O(n) (移除后重插)
//   del_timer: O(1)
//   tick: O(k) k=已过期节点数
class TimerList
{
public:
    TimerList() = default;

    // 析构: 遍历释放所有节点
    ~TimerList()
    {
        auto tmp = head_;
        while (tmp)
        {
            head_ = tmp->next;
            delete tmp;
            tmp = head_;
        }
    }

    TimerList(const TimerList &) = delete;
    TimerList &operator=(const TimerList &) = delete;

    // 插入定时器 (按 expire 升序)
    void add_timer(TimerNode *timer)
    {
        if (!timer)
            return;

        if (!head_)
        {
            head_ = tail_ = timer;
            return;
        }

        // 比最早还早, 插入头部
        if (timer->expire < head_->expire)
        {
            timer->next = head_;
            head_->prev = timer;
            head_ = timer;
            return;
        }

        add_timer(timer, head_);
    }

    // 调整定时器: 延长超时后重排位置
    // 如果 expire 未超过下一个节点则不动 (最晚)
    void adjust_timer(TimerNode *timer)
    {
        if (!timer)
            return;

        auto tmp = timer->next;
        if (!tmp || (timer->expire < tmp->expire))
            return;

        // 从当前位置移除, 重新插入
        if (timer == head_)
        {
            head_ = head_->next;
            head_->prev = nullptr;
            timer->next = nullptr;
            add_timer(timer, head_);
        }
        else
        {
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            add_timer(timer, timer->next);
        }
    }

    // 删除定时器: 处理头/尾/中/唯一 四种场景
    void del_timer(TimerNode *timer)
    {
        if (!timer)
            return;

        if (timer == head_ && timer == tail_)
        {
            delete timer;
            head_ = tail_ = nullptr;
            return;
        }

        if (timer == head_)
        {
            head_ = head_->next;
            head_->prev = nullptr;
            delete timer;
            return;
        }

        if (timer == tail_)
        {
            tail_ = tail_->prev;
            tail_->next = nullptr;
            delete timer;
            return;
        }

        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }

    // 遍历过期节点, 执行回调并释放
    void tick()
    {
        if (!head_)
            return;

        auto cur = std::chrono::steady_clock::now();
        auto tmp = head_;

        while (tmp)
        {
            if (cur < tmp->expire)
                break;

            if (tmp->cb_func)
                tmp->cb_func(tmp->user_data);

            head_ = tmp->next;
            if (head_)
                head_->prev = nullptr;
            delete tmp;
            tmp = head_;
        }
    }

private:
    // 私有辅助: 从 lst_head 开始遍历, 找到合适位置插入
    void add_timer(TimerNode *timer, TimerNode *lst_head)
    {
        auto prev = lst_head;
        auto tmp = prev->next;

        while (tmp)
        {
            if (timer->expire < tmp->expire)
            {
                prev->next = timer;
                timer->next = tmp;
                tmp->prev = timer;
                timer->prev = prev;
                return;
            }
            prev = tmp;
            tmp = tmp->next;
        }

        // 插入到末尾
        prev->next = timer;
        timer->prev = prev;
        timer->next = nullptr;
        tail_ = timer;
    }

    TimerNode *head_ = nullptr;
    TimerNode *tail_ = nullptr;
};

// epoll 工具类
// 统一 epoll_fd 的添加/删除/修改操作 + 信号处理 + 定时器总控
// 消除 http_connection.cpp 和 timer 模块中重复的 epoll 辅助函数
class EpollUtils
{
public:
    EpollUtils() = default;

    void init(int timeslot)
    {
        timeslot_ = timeslot;
    }

    // 设置 fd 为非阻塞
    static int set_nonblocking(int fd)
    {
        int old = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, old | O_NONBLOCK);
        return old;
    }

    // 添加 fd 到 epoll 事件表
    // one_shot: 是否开启 EPOLLONESHOT (同一事件只触发一次)
    // trig_mode: 0=LT, 1=ET
    static void add_fd(int epollfd, int fd, bool one_shot, int trig_mode)
    {
        epoll_event ev{};
        ev.data.fd = fd;
        ev.events = EPOLLIN | EPOLLRDHUP;
        if (trig_mode == 1)
            ev.events |= EPOLLET;
        if (one_shot)
            ev.events |= EPOLLONESHOT;
        epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
        set_nonblocking(fd);
    }

    // 从 epoll 移除 fd 并关闭
    static void remove_fd(int epollfd, int fd)
    {
        epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
    }

    // 修改 epoll 事件 (用于 EPOLLONESHOT 后重新注册)
    static void mod_fd(int epollfd, int fd, int ev, int trig_mode)
    {
        epoll_event event{};
        event.data.fd = fd;
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
        if (trig_mode == 1)
            event.events |= EPOLLET;
        epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
    }

    // 信号处理函数: 通过管道向事件循环发送信号编号
    // 可重入: 保存/恢复 errno
    static void sig_handler(int sig)
    {
        int save_errno = errno;
        int msg = sig;
        send(u_pipefd[1], reinterpret_cast<char *>(&msg), 1, 0);
        errno = save_errno;
    }

    // 注册信号处理函数
    void add_sig(int sig, void (*handler)(int), bool restart = true)
    {
        struct sigaction sa{};
        sa.sa_handler = handler;
        if (restart)
            sa.sa_flags |= SA_RESTART;
        sigfillset(&sa.sa_mask);
        assert(sigaction(sig, &sa, nullptr) != -1);
    }

    // 定时处理: 处理到期连接 + 重置闹钟
    void timer_handler()
    {
        timer_list_.tick();
        alarm(timeslot_);
    }

    // 向客户端发送错误信息并关闭连接
    void show_error(int connfd, const char *info)
    {
        send(connfd, info, static_cast<int>(std::strlen(info)), 0);
        close(connfd);
    }

    static int *u_pipefd;    // 信号管道 fd[2]
    static int u_epollfd;    // epoll 实例 fd
    TimerList timer_list_;   // 定时器链表

private:
    int timeslot_ = 0;  // alarm 间隔 (秒)
};

inline int *EpollUtils::u_pipefd = nullptr;
inline int EpollUtils::u_epollfd = 0;

#endif
