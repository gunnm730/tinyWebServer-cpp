// 日志系统 (header-only)
//
// 核心组件:
//   BlockQueue<T> — 线程安全阻塞队列, 基于 std::deque + mutex + condition_variable
//   LogSystem     — 日志单例, 支持同步/异步两种写入模式
//
// 同步模式: write_log() 直接写文件, 返回前已落盘
// 异步模式: write_log() 将消息推入队列, 后台线程 pop 并写文件
//   队列满时自动降级为同步写入(不丢日志)
//
// 文件滚动策略:
//   按天: 每天一个文件, 文件名格式 YYYY_MM_DD_ServerLog
//   按行: 超出 max_lines 行后新建计数文件 (行号 % max_lines == 0 时滚动)
//   两种条件独立触发, 取先到者

#ifndef LOG_SYSTEM_H
#define LOG_SYSTEM_H

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

// 线程安全阻塞队列
// 模板参数 T 必须可移动构造 (push T&& 使用 std::move)
// pop(T&, ms_timeout) 支持超时等待
template <class T>
class BlockQueue
{
public:
    explicit BlockQueue(size_t max_size = 1000)
        : max_size_(max_size)
    {
        if (max_size == 0)
            throw std::invalid_argument("max_size must be > 0");
    }

    void clear()
    {
        std::unique_lock lock(mtx_);
        queue_.clear();
    }

    bool empty() const
    {
        std::unique_lock lock(mtx_);
        return queue_.empty();
    }

    bool full() const
    {
        std::unique_lock lock(mtx_);
        return queue_.size() >= max_size_;
    }

    size_t size() const
    {
        std::unique_lock lock(mtx_);
        return queue_.size();
    }

    size_t max_size() const { return max_size_; }

    // 推送元素(左值版本)
    bool push(const T &item)
    {
        std::unique_lock lock(mtx_);
        if (queue_.size() >= max_size_)
            return false;
        queue_.push_back(item);
        cv_.notify_one();
        return true;
    }

    // 推送元素(右值版本), 避免拷贝
    bool push(T &&item)
    {
        std::unique_lock lock(mtx_);
        if (queue_.size() >= max_size_)
            return false;
        queue_.push_back(std::forward<T>(item));
        cv_.notify_one();
        return true;
    }

    // 阻塞弹出, 队列为空时等待
    bool pop(T &item)
    {
        std::unique_lock lock(mtx_);
        cv_.wait(lock, [this] { return !queue_.empty(); });
        item = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    // 带超时的弹出, 超时返回 false
    bool pop(T &item, int ms_timeout)
    {
        std::unique_lock lock(mtx_);
        if (!cv_.wait_for(lock, std::chrono::milliseconds(ms_timeout),
                          [this] { return !queue_.empty(); }))
            return false;
        item = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<T> queue_;
    size_t max_size_;
};

// 日志系统单例
// 初始化后通过宏简洁调用:
//   LOG_INFO("user request received")
//   LOG_ERROR(std::string("err: ") + err.what())
class LogSystem
{
public:
    enum class Level
    {
        DEBUG,
        INFO,
        WARN,
        ERROR
    };

    static LogSystem &instance()
    {
        static LogSystem inst;
        return inst;
    }

    // 初始化日志系统
    // file_path: 日志文件路径(不含日期前缀), 如 ./ServerLog
    // close_log: 0=启用, 1=关闭
    // max_lines: 单个文件最大行数
    // buf_size: 格式化缓冲区大小(预留)
    // max_queue_size: 异步模式队列大小, 0=同步模式
    void init(const std::string &file_path,
              int close_log = 0,
              int max_lines = 5000000,
              int buf_size = 8192,
              size_t max_queue_size = 0)
    {
        close_log_ = close_log;
        max_lines_ = max_lines;
        buf_size_ = buf_size;
        is_async_ = max_queue_size > 0;
        base_path_ = file_path;
        today_ = current_day();

        if (is_async_)
        {
            log_queue_ = std::make_unique<BlockQueue<std::string>>(max_queue_size);
            worker_ = std::thread([this] { async_write_loop(); });
        }

        rotate_file();
    }

    // 写入日志
    // level: 日志级别
    // msg: 已格式化的日志内容(不含时间戳和级别前缀)
    //
    // 异步模式: 入队成功则返回; 入队失败(队列满)降级为同步写
    // 同步模式: 直接写文件
    void write_log(Level level, const std::string &msg)
    {
        if (close_log_)
            return;

        std::string log_msg = format_msg(level, msg);

        if (is_async_)
        {
            if (!log_queue_->push(std::move(log_msg)))
            {
                // 队列满降级: 当前线程直接写文件, 避免丢日志
                std::unique_lock lock(file_mutex_);
                if (ofs_ && ofs_->is_open())
                    (*ofs_) << log_msg << std::endl;
            }
        }
        else
        {
            std::unique_lock lock(file_mutex_);
            rotate_if_needed();
            if (ofs_ && ofs_->is_open())
                (*ofs_) << log_msg << std::endl;
        }
    }

    void flush()
    {
        std::unique_lock lock(file_mutex_);
        if (ofs_)
            ofs_->flush();
    }

    bool is_closed() const { return close_log_; }

private:
    LogSystem() = default;

    // 析构: 等待异步线程退出, 关闭文件
    ~LogSystem()
    {
        if (worker_.joinable())
        {
            {
                std::unique_lock lock(queue_mutex_);
                exit_flag_ = true;
                queue_cv_.notify_all();
            }
            worker_.join();
        }
        if (ofs_)
            ofs_->close();
    }

    LogSystem(const LogSystem &) = delete;
    LogSystem &operator=(const LogSystem &) = delete;

    // 异步写线程主循环
    // 从队列取消息并写入文件, 收到退出信号且队列为空时结束
    void async_write_loop()
    {
        std::string msg;
        while (true)
        {
            {
                std::unique_lock lock(queue_mutex_);
                queue_cv_.wait(lock, [this] {
                    return (log_queue_ && !log_queue_->empty()) || exit_flag_;
                });
                if (exit_flag_ && (!log_queue_ || log_queue_->empty()))
                    break;
            }

            if (log_queue_ && log_queue_->pop(msg))
            {
                std::unique_lock flock(file_mutex_);
                rotate_if_needed();
                if (ofs_ && ofs_->is_open())
                    (*ofs_) << msg << std::endl;
            }
        }
    }

    // 格式化日志消息: "YYYY-MM-DD HH:MM:SS.ms [LEVEL] msg"
    std::string format_msg(Level level, const std::string &msg)
    {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) %
                  1000;
        std::time_t t = std::chrono::system_clock::to_time_t(now);

        std::ostringstream oss;
        oss << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << ms.count()
            << " [" << level_str(level) << "] " << msg;
        return oss.str();
    }

    static std::string level_str(Level level)
    {
        switch (level)
        {
        case Level::DEBUG:
            return "DEBUG";
        case Level::INFO:
            return "INFO";
        case Level::WARN:
            return "WARN";
        case Level::ERROR:
            return "ERROR";
        default:
            return "INFO";
        }
    }

    int current_day()
    {
        auto t = std::time(nullptr);
        return std::localtime(&t)->tm_mday;
    }

    std::string date_prefix()
    {
        auto t = std::time(nullptr);
        auto tm = *std::localtime(&t);
        std::ostringstream oss;
        oss << (tm.tm_year + 1900) << "_"
            << std::setw(2) << std::setfill('0') << (tm.tm_mon + 1) << "_"
            << std::setw(2) << std::setfill('0') << tm.tm_mday << "_";
        return oss.str();
    }

    // 创建新日志文件 (使用当前日期前缀)
    void rotate_file()
    {
        fs::path path(base_path_);
        fs::path dir = path.parent_path();
        std::string fname = path.filename().string();
        std::string full_name = dir.string() + "/" + date_prefix() + fname;

        ofs_ = std::make_unique<std::ofstream>(full_name, std::ios::app);
        line_count_ = 0;
        today_ = current_day();
    }

    // 检测是否需要滚动:
    //   换天 → 新建日期文件
    //   行数超限 → 新建计数文件 (实际日期前缀 + 已有文件名)
    void rotate_if_needed()
    {
        int today = current_day();
        if (today != today_ || line_count_ >= max_lines_)
        {
            if (ofs_)
                ofs_->close();
            rotate_file();
        }
        ++line_count_;
    }

    std::unique_ptr<std::ofstream> ofs_;
    std::mutex file_mutex_;
    std::string base_path_;
    int close_log_ = 0;
    int buf_size_ = 8192;
    int max_lines_ = 5000000;
    int line_count_ = 0;
    int today_ = 0;
    bool is_async_ = false;

    std::unique_ptr<BlockQueue<std::string>> log_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread worker_;
    bool exit_flag_ = false;
};

// 日志宏: 展开为 LogSystem::instance().write_log()
// 注意: 参数为 std::string, 不是 printf 格式串
// 调用方需要提前拼好字符串, 如:
//   LOG_INFO(std::string("fd ") + std::to_string(fd))
#define LOG_DEBUG(msg)  LogSystem::instance().write_log(LogSystem::Level::DEBUG, msg)
#define LOG_INFO(msg)   LogSystem::instance().write_log(LogSystem::Level::INFO, msg)
#define LOG_WARN(msg)   LogSystem::instance().write_log(LogSystem::Level::WARN, msg)
#define LOG_ERROR(msg)  LogSystem::instance().write_log(LogSystem::Level::ERROR, msg)

#endif
