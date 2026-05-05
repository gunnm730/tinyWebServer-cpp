// 服务器配置结构体
// 存储 CLI 参数解析结果,并提供 listen/conn 触发模式的推导

#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <optional>

struct ServerConfig
{
    // 端口号, 默认 9006
    int port = 9006;

    // 日志写入方式: 0=同步, 1=异步
    int log_write = 0;

    // listenfd + connfd 触发模式组合:
    //   0 = LT + LT
    //   1 = LT + ET
    //   2 = ET + LT
    //   3 = ET + ET
    int trig_mode = 0;

    // SO_LINGER 选项: 0=关闭, 1=开启(优雅关闭)
    int opt_linger = 0;

    // 数据库连接池大小, 默认 8
    int sql_num = 8;

    // 线程池线程数, 默认 8
    int thread_num = 8;

    // 日志开关: 0=启用, 1=关闭
    int close_log = 0;

    // 并发模型: 0=Proactor, 1=Reactor
    int actor_model = 0;

    // 以下两个字段由 trig_mode 在 parse_arg() 中推导
    // listenfd 触发模式: 0=LT, 1=ET
    int listen_trig_mode = 0;

    // connfd 触发模式: 0=LT, 1=ET
    int conn_trig_mode = 0;

    // 解析命令行参数, 填充以上字段
    void parse_arg(int argc, char *argv[]);
};

#endif
