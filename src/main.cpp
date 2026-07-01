#include "../include/WebServer.h"
#include "../include/Logger.h"
#include "../include/SqlConnPool.h"
#include <iostream>
#include <signal.h>

int main() {
    signal(SIGPIPE, SIG_IGN);

    Logger::Instance().Init("server.log");
    SqlConnPool::Instance().Init("127.0.0.1", 3306, "webserver", "123456", "mydb", 8);
    int port = 8080;

    // Create the server object
    WebServer server(port);
    try{
        server.Init();
        server.Start();
    }
    catch(const std::exception& e) {
        std::cerr << "[Fatal Error] Server failed to start: " << e.what() << std::endl;
        return -1;
    }
    return 0;
}