// MySQL Connector/C++ 连接池 (header-only)
//
// ConnectionPool — 单例, 管理多个 sql::Connection
// ConnGuard     — RAII 包装器, 构造时取连接, 析构时自动归还
//
// 关键行为:
//   初始化时预创建 max_conn/2 个连接
//   超出时惰性创建, 但总量不超过 max_conn
//   所有连接耗尽时 get_connection() 阻塞等待
//   使用 std::unique_ptr<sql::Connection> 管理连接生命周期
//   所有 sql::Statement / sql::ResultSet 由调⽤方自行管理

#ifndef CONNECTION_POOL_H
#define CONNECTION_POOL_H

#include <cppconn/connection.h>
#include <cppconn/driver.h>
#include <cppconn/exception.h>
#include <cppconn/resultset.h>
#include <cppconn/statement.h>

#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "../log/log_system.h"

class ConnectionPool
{
public:
    using ConnPtr = std::unique_ptr<sql::Connection>;

    static ConnectionPool &instance()
    {
        static ConnectionPool pool;
        return pool;
    }

    // 初始化连接池
    // host: MySQL 主机地址
    // port: 端口号 (默认 3306)
    // max_conn: 最大连接数 (预创建 max_conn/2)
    void init(const std::string &host,
              const std::string &user,
              const std::string &password,
              const std::string &database,
              unsigned int port = 3306,
              int max_conn = 8)
    {
        host_ = host;
        user_ = user;
        password_ = password;
        database_ = database;
        port_ = port;
        max_conn_ = max_conn;

        try
        {
            driver_ = get_driver_instance();
        }
        catch (const sql::SQLException &e)
        {
            std::string err = std::string("MySQL driver init failed: ") + e.what();
            LOG_ERROR(err);
            throw;
        }

        // 预创建一半连接 (避免启动时全部创建的开销)
        for (int i = 0; i < max_conn_ / 2; ++i)
            pool_.push_back(create_connection());
    }

    // 获取连接: 池中有空闲则直接取, 否则阻塞等待或惰性创建
    ConnPtr get_connection()
    {
        std::unique_lock lock(mtx_);
        while (pool_.empty() && cur_conn_ >= max_conn_)
            cv_.wait(lock);

        if (!pool_.empty())
        {
            auto conn = std::move(pool_.front());
            pool_.pop_front();
            return conn;
        }

        // 未超上限, 惰性创建
        ++cur_conn_;
        return create_connection();
    }

    // 归还连接到池
    void release_connection(ConnPtr conn)
    {
        std::unique_lock lock(mtx_);
        pool_.push_back(std::move(conn));
        cv_.notify_one();
    }

    // 销毁所有连接
    void destroy()
    {
        std::unique_lock lock(mtx_);
        pool_.clear();
        cur_conn_ = 0;
    }

private:
    ConnectionPool() = default;

    ~ConnectionPool() { destroy(); }

    ConnectionPool(const ConnectionPool &) = delete;
    ConnectionPool &operator=(const ConnectionPool &) = delete;

    // 创建一个新的数据库连接并设置默认 schema
    ConnPtr create_connection()
    {
        try
        {
            std::string url = "tcp://" + host_ + ":" + std::to_string(port_);
            ConnPtr conn(driver_->connect(url, user_, password_));
            conn->setSchema(database_);
            return conn;
        }
        catch (const sql::SQLException &e)
        {
            std::string err = std::string("MySQL connect failed: ") + e.what();
            LOG_ERROR(err);
            throw;
        }
    }

    sql::Driver *driver_ = nullptr;
    std::string host_, user_, password_, database_;
    unsigned int port_ = 3306;
    int max_conn_ = 8;
    int cur_conn_ = 0;
    std::list<ConnPtr> pool_;
    std::mutex mtx_;
    std::condition_variable cv_;
};

// RAII 封装: 构造时取连接, 析构时自动归还
// 可通过 operator-> / operator* 直接当 sql::Connection* 使用:
//   ConnGuard guard(pool);
//   auto stmt = guard->createStatement();
//   guard->setSchema("mydb");
class ConnGuard
{
public:
    explicit ConnGuard(ConnectionPool &pool)
        : pool_(pool)
    {
        conn_ = pool_.get_connection();
    }

    ~ConnGuard()
    {
        if (conn_)
            pool_.release_connection(std::move(conn_));
    }

    ConnGuard(const ConnGuard &) = delete;
    ConnGuard &operator=(const ConnGuard &) = delete;

    ConnGuard(ConnGuard &&other) noexcept
        : pool_(other.pool_), conn_(std::move(other.conn_)) {}

    sql::Connection *get() const { return conn_.get(); }
    sql::Connection &operator*() const { return *conn_; }
    sql::Connection *operator->() const { return conn_.get(); }

private:
    ConnectionPool &pool_;
    ConnectionPool::ConnPtr conn_;
};

#endif
