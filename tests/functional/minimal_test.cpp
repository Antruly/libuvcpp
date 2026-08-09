#include <iostream>
#include "net/uvcpp_tcp_server.h"
int main() {
    std::cout << "hello" << std::endl;
    uvcpp::uvcpp_tcp_server server;
    std::cout << "server created" << std::endl;
    server.bind("127.0.0.1", 12345);
    std::cout << "bound" << std::endl;
    return 0;
}
