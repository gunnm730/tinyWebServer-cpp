// CLI 参数解析实现
// 使用 POSIX getopt() 解析命令行选项

#include "config.h"

#include <cstdlib>
#include <unistd.h>

void ServerConfig::parse_arg(int argc, char *argv[])
{
    int opt;
    // getopt 格式串: 所有选项后带冒号表示需要参数
    while ((opt = getopt(argc, argv, "p:l:m:o:s:t:c:a:")) != -1)
    {
        switch (opt)
        {
        case 'p':
            port = std::atoi(optarg);
            break;
        case 'l':
            log_write = std::atoi(optarg);
            break;
        case 'm':
            trig_mode = std::atoi(optarg);
            break;
        case 'o':
            opt_linger = std::atoi(optarg);
            break;
        case 's':
            sql_num = std::atoi(optarg);
            break;
        case 't':
            thread_num = std::atoi(optarg);
            break;
        case 'c':
            close_log = std::atoi(optarg);
            break;
        case 'a':
            actor_model = std::atoi(optarg);
            break;
        }
    }

    // 从组合模式推导独立的 listen/conn 触发模式
    switch (trig_mode)
    {
    case 0:
        listen_trig_mode = 0;
        conn_trig_mode = 0;
        break;
    case 1:
        listen_trig_mode = 0;
        conn_trig_mode = 1;
        break;
    case 2:
        listen_trig_mode = 1;
        conn_trig_mode = 0;
        break;
    case 3:
        listen_trig_mode = 1;
        conn_trig_mode = 1;
        break;
    }
}
