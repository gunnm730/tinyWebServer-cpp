// 服务器启动入口
//
// 初始化流程:
//   1. 解析 CLI 参数 → ServerConfig
//   2. LogSystem 单例初始化
//   3. ConnectionPool 单例初始化 + 加载用户数据
//   4. 创建 WebServer 编排器
//   5. server.run() 执行七步初始化 + 事件循环
//
// 数据库凭据硬编码在此文件中,部署前应修改

#include "config/config.h"
#include "server/webserver.h"

int main(int argc, char *argv[])
{
    // 数据库连接信息 (部署时修改)
    std::string user = "webuser";
    std::string passwd = "webpass";
    std::string databasename = "qgydb";

    // 解析命令行参数 (-p -l -m -o -s -t -c -a)
    ServerConfig config;
    config.parse_arg(argc, argv);

    // 构建服务器实例并启动
    // run() 内部依次完成: 日志→数据库池→触发模式→线程池→监听socket→epoll→信号注册→事件循环
    WebServer server(config, user, passwd, databasename);
    server.run();

    return 0;
}
