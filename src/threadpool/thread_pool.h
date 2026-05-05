// 线程池模板 (header-only)
//
// 支持两种并发模型:
//   Proactor (actor_model=0): 主线程完成 read/write, 工作线程只调用 process()
//   Reactor  (actor_model=1): 工作线程完成 read_once()/write(), 然后调用 process()
//
// 生命周期:
//   构造函数: 创建 N 个 std::thread
//   析构函数: 设置 stop_=true, notify_all, join 所有线程
//
// 模板参数 T 需要实现:
//   int m_state         — 0=读, 1=写
//   bool read_once()    — 从 socket 读取数据
//   bool write()        — 向 socket 发送数据
//   void process()      — 处理请求（内部通过 ConnGuard 自行获取 DB 连接）
//   int improv          — 处理完成标志(Reactor 同步用)
//   int timer_flag      — 超时关闭标志

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

template <typename T>
class ThreadPool
{
public:
    // actor_model: 0=Proactor, 1=Reactor
    // thread_number: 工作线程数量
    // max_requests: 任务队列最大长度
    // 注意: DB 连接由 process() 内部通过 ConnGuard 自行获取, 不由线程池管理
    ThreadPool(int actor_model, int thread_number = 8, int max_requests = 10000)
        : actor_model_(actor_model),
          max_requests_(max_requests),
          stop_(false)
    {
        for (int i = 0; i < thread_number; ++i)
        {
            threads_.emplace_back([this] { worker(); });
        }
    }

    // 析构: 通知所有线程退出, 等待它们结束
    ~ThreadPool()
    {
        {
            std::unique_lock lock(queue_mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto &t : threads_)
        {
            if (t.joinable())
                t.join();
        }
    }

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    // Reactor 模式入队: 设置 m_state (0=读, 1=写)
    bool append(T *request, int state)
    {
        std::unique_lock lock(queue_mutex_);
        if (work_queue_.size() >= max_requests_)
            return false;
        request->m_state = state;
        work_queue_.push_back(request);
        cv_.notify_one();
        return true;
    }

    // Proactor 模式入队: 不设置 m_state (数据已由主线程读好)
    bool append_p(T *request)
    {
        std::unique_lock lock(queue_mutex_);
        if (work_queue_.size() >= max_requests_)
            return false;
        work_queue_.push_back(request);
        cv_.notify_one();
        return true;
    }

private:
    // 工作线程的主循环
    void worker()
    {
        while (true)
        {
            T *request = nullptr;
            {
                std::unique_lock lock(queue_mutex_);
                cv_.wait(lock, [this] {
                    return !work_queue_.empty() || stop_;
                });
                if (stop_ && work_queue_.empty())
                    return;
                request = work_queue_.front();
                work_queue_.pop_front();
            }

            if (!request)
                continue;

            if (actor_model_ == 1)
            {
                // Reactor: 工作线程负责 I/O
                if (request->m_state == 0)
                {
                    if (request->read_once())
                    {
                        request->improv = 1;
                        request->process();
                    }
                    else
                    {
                        request->improv = 1;
                        request->timer_flag = 1;
                    }
                }
                else
                {
                    // 写任务: worker 调用 write() 发送响应
                    if (request->write())
                    {
                        request->improv = 1;
                    }
                    else
                    {
                        request->improv = 1;
                        request->timer_flag = 1;
                    }
                }
            }
            else
            {
                // Proactor: 数据已由主线程读好, worker 只需处理
                request->process();
            }
        }
    }

    int actor_model_;
    int max_requests_;
    std::deque<T *> work_queue_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::vector<std::thread> threads_;
    std::atomic<bool> stop_;
};

#endif
