#include "tcp_server.h"
#include <cerrno>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "json_shift.h"   // 你的 JSON 解析和业务转发
#include "mysql.h"        // 你的数据库连接
#include "OnlineSessionManager.h"

namespace
{
    bool SendAll(int fd, const std::string& data)
    {
        std::size_t sent = 0;
        while (sent < data.size())
        {
            const ssize_t size = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (size < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                return false;
            }
            if (size == 0)
            {
                return false;
            }
            sent += static_cast<std::size_t>(size);
        }
        return true;
    }
}

// ---------- 构造/析构 ----------
TcpServer::TcpServer()
    : listen_fd_(-1), port_(0), backlog_(5), running_(false) {}

TcpServer::~TcpServer() {
    Stop();
}

// ---------- 启动 ----------
bool TcpServer::Start(int port, int backlog) {
    port_ = port;
    backlog_ = backlog;

    if (!InitSocket()) {
        return false;
    }

    running_ = true;
    std::cout << "TCP 服务启动，监听端口：" << port_ << std::endl;

    AcceptLoop();   // 阻塞在此，直到 Stop() 被调用（或出错）
    return true;
}

// ---------- 停止 ----------
void TcpServer::Stop() {
    running_ = false;
    if (listen_fd_ != -1) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
}

// ---------- 初始化 socket ----------
bool TcpServer::InitSocket() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        perror("socket 创建失败");
        return false;
    }

    // 端口复用
    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind 失败");
        close(listen_fd_);
        return false;
    }

    if (listen(listen_fd_, backlog_) < 0) {
        perror("listen 失败");
        close(listen_fd_);
        return false;
    }

    return true;
}

// ---------- 主循环 ----------
void TcpServer::AcceptLoop() {
    while (running_) {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client_fd < 0) {
            if (!running_) {
                break;   // Stop() 关闭了 listen_fd，导致 accept 失败，正常退出
            }
            perror("accept 失败");
            continue;
        }

        // 开启保活
        int alive = 1;
        setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &alive, sizeof(alive));

        ClientArg* arg = new ClientArg;
        arg->client_fd = client_fd;
        arg->client_ip = inet_ntoa(client_addr.sin_addr);

        // 创建分离线程
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        int ret = pthread_create(&tid, &attr, ClientWork, arg);
        if (ret != 0) {
            perror("pthread_create 失败");
            close(client_fd);
            delete arg;
        } else {
            std::cout << "[新连接] IP: " << arg->client_ip << std::endl;
        }
        pthread_attr_destroy(&attr);
    }
}

// ---------- 线程工作函数（原 client_work） ----------
void* TcpServer::ClientWork(void* arg) {
    ClientArg* data = static_cast<ClientArg*>(arg);
    int fd = data->client_fd;
    std::string ip = data->client_ip;
    std::queue<char>& cache = data->msg_cache;
    auto sess = std::make_shared<Session>();
    sess->clientIp= ip;
    sess->fd = fd;

    mysqlconn mysqlconnect;
    if (!mysqlconnect.connect("192.168.20.128", 3306, "dbuser", "My-dbuser-123", "User"))
    {
        // 连不上库（例如达到 dbuser 的 max_user_connections 上限）时绝对不能继续往下走：
        // 此时句柄虽然非空但并没有真正连上，任何一次 mysql_real_escape_string / mysql_query
        // 都会在 libmysqlclient 内部直接段错误，把整个服务进程连同其他在线用户一起带崩。
        // 这里先回一个明确的错误包再断开，客户端才不会只看到连接被神秘掐断。
        std::cerr << "[数据库] 连接失败，拒绝该客户端 IP: " << ip << std::endl;
        const std::string busy = nlohmann::json{
            {"type", "error_response"},
            {"code", 503},
            {"message", "服务器繁忙，请稍后重试"},
            {"success", false}
        }.dump() + "\n";
        SendAll(fd, busy);
        close(fd);
        delete data;
        return nullptr;
    }

    constexpr int BUF_MAX = 4096;
    constexpr int CACHE_LIMIT = 4096;
    char recv_buf[BUF_MAX] = {0};
    bool offline = false;
    std::string single_json;

    while (true) {
        memset(recv_buf, 0, sizeof(recv_buf));
        ssize_t recv_size = recv(fd, recv_buf, BUF_MAX, 0);
        if (recv_size <= 0) {
            offline = true;
            break;
        }

        // 限流
        while (cache.size() >= CACHE_LIMIT) {
            usleep(1000); // 简单等待，避免空转
        }

        for (int i = 0; i < recv_size; ++i) {
            cache.push(recv_buf[i]);
        }

        while (!cache.empty()) {
            char ch = cache.front();
            cache.pop();
            if (ch == '\n') {
                std::cout << "[" << ip << "] 完整JSON: " << single_json << std::endl;
                std::string rsp = single_json + "\n";
                std::string reply = JsonParsing(rsp, mysqlconnect,sess);
                // 打印真正发出去的报文（reply 末尾自带换行，这里不再额外补）
                std::cout << "[" << ip << "] 发送JSON: " << reply;
                if (!SendAll(fd, reply))
                {
                    std::cout<<'\n'<<std::endl;
                    offline = true;
                    break;
                }
                single_json.clear();
            } else {
                single_json += ch;
            }
        }
    }

    // 下线：先摘掉本连接的登录态，再按连接校验从在线列表移除。
    sess->login_state = false;
    if (!sess->account.empty()) {
        // 带连接校验：若该账号已经在新连接上重登/重连，字典里存的已不是本连接，
        // 这次清理就不该动它，否则会把刚接上来的新会话误删。
        OnlineSessionManager::Instance().removeSession(sess->account, sess);
    }
    std::cout << "[客户端下线] IP: " << ip << std::endl;
    close(fd);
    delete data;
    return nullptr;
}
