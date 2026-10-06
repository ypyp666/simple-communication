#ifndef UNTITLED_TCP_SERVER_H
#define UNTITLED_TCP_SERVER_H
#include<iostream>
#include <string>
#include <queue>
#include <pthread.h>

struct ClientArg {
    int client_fd;
    std::string client_ip;
    std::queue<char> msg_cache;
};

class TcpServer {
public:
    TcpServer();
    ~TcpServer();

    // 禁止拷贝
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // 启动服务器（阻塞在 accept 循环，直到调用 Stop）
    bool Start(int port, int backlog = 5);

    // 停止服务器（关闭监听 socket，使 Start 退出循环）
    void Stop();

private:
    int  listen_fd_;
    int  port_;
    int  backlog_;
    bool running_;

    // 初始化 socket、绑定、监听
    bool InitSocket();

    // 处理 accept 循环（在 Start 内部调用）
    void AcceptLoop();

    // 静态线程工作函数（替代全局 client_work）
    static void* ClientWork(void* arg);
};

#endif //UNTITLED_TCP_SERVER_H
