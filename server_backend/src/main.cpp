#include <iostream>
#include <sys/socket.h>//只要你要创建、操作 TCP 套接字，必须包含这个头文件
#include <arpa/inet.h>//IP 地址格式转换专用头文件
#include <unistd.h>//Linux 系统通用工具头文件（系统调用基础）
//网络编程里只用它两个核心功能close()：关闭文件描述符（socket 本质就是文件，客户端断开必须close(fd)）
//进程休眠 sleep()、获取当前进程 ID getpid() 等系统工具函数
#include <cstring>
#include <string>
#include<queue>
#include<pthread.h>
#include"json_shift.h"
#include"mysql.h"
#include <csignal>
#include"tcp_server.h"

// 服务监听端口，和Windows UI统一
constexpr int LISTEN_PORT = 8899;//constexpr编译期常量，性能更好
// 数据缓冲区大小
constexpr int BUF_MAX = 4096;
constexpr int CACHE_LIMIT = 4096;   // 单客户端消息队列上限，限流防内存爆炸
constexpr int LISTEN_BACKLOG = 5;   // listen内核半连接队列长度

TcpServer* g_server = nullptr;


void signal_handler(int) {
    if (g_server) g_server->Stop();
}

int main() {
    TcpServer server;
    g_server = &server;
    signal(SIGINT, signal_handler);   // Ctrl+C
    signal(SIGTERM, signal_handler);  // kill
    server.Start(8899);
    return 0;
}

/*
Linux 系统有一条核心规则：一切皆文件
网卡、普通文本、管道、socket 网络连接，在内核里全部统一用「文件描述符」管理，文件描述符本质就是一个整数编号。

// 每个客户端独立参数结构体，传给子线程（数据隔离，无全局竞争）
struct ClientArg
{
    int client_fd;
    std:: string client_ip;
    std::queue<char> msg_cache; // 当前客户端专属字节缓存队列
};

void* client_work(void* arg);
int main()
{

    // 1. 创建TCP套接字,监听套接字
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);

    AF_INET 地址族（Address Family：AF_INET = IPv4 互联网协议，AF_INET6 = IPv6、AF_UNIX = 本机进程本地通信（不用网卡）
    SOCK_STREAM 套接字类型：SOCK_STREAM = TCP 流式套接字
    同一个地址族 + 套接字类型下，选用的底层子协议
    当 AF_INET + SOCK_STREAM 组合时，默认协议就是 TCP，填 0 代表自动选用默认 TCP 协议；
    固定搭配：AF_INET + SOCK_STREAM + 0 = TCP，AF_INET + SOCK_DGRAM + 0 = UDP

    if (server_sock < 0)
    {
        perror("socket 创建失败");
        return -1;
    }

    // 端口复用：重启程序不会报端口占用
    int opt_val = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val));
    /*
    server_sock：要配置的套接字文件描述符（你刚创建的 TCP 服务 fd）
    SOL_SOCKET：配置层级，代表「操作套接字本身的通用属性」还有别的层级（比如 IP 层、TCP 层），端口复用属于套接字全局属性，固定填 SOL_SOCKET。
    SO_REUSEADDR：要修改的属性名，字面意思「复用地址 + 端口」
    &opt_val：配置参数的内存地址，告诉内核我们要开启（值为 1）
    sizeof(opt_val)：参数占用字节大小，内核用来读取配置值。

    // 配置服务端地址：监听本机所有网卡 0.0.0.0
    sockaddr_in server_addr{};//Linux 专门存 IPv4 地址 + 端口的结构体；
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);//sin_addr.s_addr：本机 IP 地址（二进制网络序），INADDR_ANY：字面意思「任意本机 IP」
    //你虚拟机有多个网卡（内网 192.168.x.x、lo 本机回环 127.0.0.1），填这个代表所有网卡全部监听，不管客户端从哪个 IP 连进来都能接收。
    //htonl()：host to network long，把主机字节序转网络大端序，IP 必须转。如果你只想让本机自己连、外部虚拟机访问不了，就写 inet_addr("127.0.0.1")；
    server_addr.sin_port = htons(LISTEN_PORT);

    // 2. 绑定端口
    if (bind(server_sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0)
    {
        /*精讲一下这个sizeof(server_addr))，这是c语言的两个结构体，bind只认sockaddr结构体，sockaddr_in转完后传到bind里时结构体的便宜量和大小是完全按照sockaddr来的
        sockaddr_in的自己的一些变量你不告诉bind他找不到因此得把大小传过去才能找到

        perror("bind 绑定端口失败");
        close(server_sock);
        return -1;
    }

     *server_sock：刚才 socket 创建出来的套接字 fd
    reinterpret_cast<sockaddr*>(&server_addr)
    reinterpret_cast<目标类型>(原值) 是 C++ 四种强制转换关键字之一，不是模板、不是类、没有构造函数。
    <> 只是语法符号，用来告诉编译器「你要转换成什么类型」，和模板只是长得像，底层逻辑无关。
    bind 函数通用参数是父结构体 sockaddr*，但我们存地址用的是 IPv4 专用 sockaddr_in，必须强制类型转换，不然编译报错。sockaddr_in继承自sockaddr
    sizeof(server_addr)：告诉 bind 地址结构体占多大内存，内核区分 IPv4/IPv6 用。
    返回值规则：返回≥0：绑定成功。返回 < 0：失败（端口占用、权限不足、IP 错误等），用 perror 打印原因。
    失败处理逻辑：close(server_sock); 立刻关闭套接字，释放资源再退出，防止文件描述符泄漏。


    // 3. 开启监听，等待队列长度5
    if (listen(server_sock, 5) < 0)//参数 1：server_sock 监听套接字参数 2：5 挂起连接队列长度（backlog
    {
        //数字 5 = 队列最多存 5 个待处理连接；队列满了之后新客户端连接直接被内核拒绝。
        perror("listen 监听失败");
        close(server_sock);
        return -1;
    }

    1.当你执行完 listen(server_sock,5) 这一行后，内核就会接管这个 IP + 端口，后台自动做这些事：
    2.网卡收到客户端发来的 SYN 连接请求包；
    3.内核自动回复 SYN+ACK（服务端握手响应报文），不需要你写一行 send；
    4.等待客户端回 ACK；
    5.收到 ACK，三次握手正式完成，内核把这条连接放进已完成连接队列缓存起来。

    std::cout << "TCP服务启动完成，监听端口：" << LISTEN_PORT << std::endl;
    std::cout << "客户端连接地址： :" << LISTEN_PORT << std::endl;
//三次握手响应完全是操作系统内核自动完成的
    // 循环等待客户端接入
    while (true)
    {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);//socklen_t 是系统为套接字地址长度专门定义的标准专用类型
        // 阻塞等待客户端连接
        int client_fd= accept(server_sock, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);


        accept 只做一件事：从内核队列里 “取走已经建好的连接”
        accept 根本不参与握手，它只是一个取连接的函数：
        队列里有建好的连接 → 直接取出，返回 client_sock；
        队列空 → 阻塞卡住，等待内核完成新的握手、往队列里放连接，accept返回的 client_sock（通信套接字，一客户一个）
        当内核完成客户端三次握手、连接存入队列后：
        accept 会在内核里新建一套独立 TCP 连接上下文，生成全新文件描述符返回给你，这就是 client_sock。
        它能干什么：唯一用来和当前这个客户端收发数据：recv()、send()；
        存储这条 TCP 连接完整状态（序列号、滑动窗口、对方 IP 端口等）；
        生命周期：一个客户端对应一个唯一 client_sock；客户端下线 / 会话结束必须 close(client_sock) 释放，否则文件描述符泄露。

        int alive = 1;
        // 开启保活开关
        setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &alive, sizeof(alive));
        if (client_fd< 0)
        {
            perror("accept 接收客户端失败");
            continue;
        }

        // 打印连接客户端IP
        std::string client_ip = inet_ntoa(client_addr.sin_addr);
        std::cout << "\n新客户端接入 IP：" << client_ip << std::endl;

        // 接收客户端发送的数据
        //char recv_buf[BUF_MAX]{};
        //ssize_t recv_len = recv(client_sock, recv_buf, BUF_MAX, 0);

        参数 1：client_sock
        你之前 accept 返回的客户端专属通信套接字。
        明确告诉操作系统：我要读取这条 TCP 连接上的数据，只读取这个客户端发来的内容。
        参数 2：recv_buf
        你上面刚定义好的缓冲区首地址。
        内核读到的数据，会复制到这块内存里，供你程序使用。
        参数 3：BUF_MAX
        本次读取最多拷贝多少字节，不能超过缓冲区数组的大小，防止内存溢出。
        内核不会一次性往数组里塞超过这个数字的字节，保证数组不会越界。
        参数 4：0
        读取标记，填 0 代表阻塞读取模式：
        如果操作系统内核缓冲区里，没有客户端发来的任何数据，当前线程直接卡在 recv 这一行，暂停运行，不占用 CPU，直到客户端发数据过来才会解除阻塞。
        ssize_t:系统专用有符号整数类型，专门用来表示 IO 读写长度，能存正数、0、负数；普通int语义不精准，规范网络代码统一用它。

        ClientArg* arg = new ClientArg;//用new而不是直接创建的原因是直接创建是临时的，new出来的是在堆中的
        arg->client_fd = client_fd;
        arg->client_ip = inet_ntoa(client_addr.sin_addr);

        // 创建分离线程：线程结束自动回收资源，无需主线程join等待
        pthread_t tid;//// 线程ID句柄，用来标识一条线程，操作系统内核每条线程都有独立 TCB（线程控制块），TCB 里存了：寄存器上下文、栈、调度优先级、状态（就绪 / 阻塞 / 运行）。
        //id 是用户态程序用来定位、操作对应 TCB的句柄，相当于线程的 “身份证号”。如果没有 tid，操作系统不知道你要操作哪一条线程：
        pthread_attr_t attr;// 线程属性结构体：存放线程配置（分离状态、栈大小、调度优先级等）只是创建线程时一次性读取的配置模板，仅在 pthread_create 调用的内部系统调用过程中生效。
        pthread_attr_init(&attr); // 初始化属性结构体，填充默认配置
        // 设置分离属性，线程退出自动释放资源
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);// 设置为分离线程，joinable 为汇合线程


        1. 汇合线程（默认，不设置分离）
        线程执行完 return 退出后，内核不会自动销毁 TCB 线程资源（线程栈、tid 标识、退出返回值）；
        资源会一直残留在系统中，产生线程泄漏，直到主线程调用 pthread_join(tid, &ret)；
        pthread_join 两个作用：阻塞等待线程结束 + 释放线程全部内核资源；
        2. 分离线程（PTHREAD_CREATE_DETACHED）
        线程执行完毕退出瞬间，操作系统内核自动释放这条线程所有 TCB 资源；
        完全不需要主线程调用pthread_join，主线程不用保存 tid、不用关心线程生命周期；
        适合服务端长连接场景：主线程只负责创建线程，后续完全不干涉子线程生命周期


        // 启动子线程处理当前客户端
        int ret = pthread_create(&tid, &attr, client_work, arg);//创建线程的系统调用函数
        if (ret != 0)
        {
            perror("pthread_create 创建线程失败");
            close(client_fd);
            delete arg;
        }
        pthread_attr_destroy(&attr); // 销毁线程属性

        std::cout << "[新连接接入] IP：" << arg->client_ip << "，已分配独立子线程" << std::endl;
    }

    close(server_sock);
    return 0;
}

// pthread强制规定：线程函数返回void*，参数void*
void* client_work(void* arg)
{
    // 取出客户端参数
    ClientArg* client_data = static_cast<ClientArg*>(arg);


    static_cast 是 C++ 静态强制转换运算符编译期做类型合法性校验；编译期做类型合法性校验；
    编译期做类型合法性校验；只允许逻辑上有转换关系的类型：数值互转 int/double；
    父子类指针；* ↔ void*（合法安全）；转换时编译器知道两种指针存在合法映射，不会粗暴二进制硬解释。

    int fd = client_data->client_fd;
    std::string ip = client_data->client_ip;
    std::queue<char>& cache = client_data->msg_cache;
    mysqlconn mysqlconnect;
    mysqlconnect.connect("192.168.20.128",3306,"dbuser","My-dbuser-123","User");


    std::cout << "[线程创建] 开始处理客户端：" << ip << std::endl;
    char recv_buf[BUF_MAX] = {0};
    bool is_offline = false;

    // 长连接循环：持续收发，直到客户端断开
    while (true)
    {
        memset(recv_buf, 0, sizeof(recv_buf));
        // 阻塞读取客户端数据
        ssize_t recv_size = recv(fd, recv_buf, BUF_MAX, 0);

        // 两种情况：客户端正常关闭 / 网络异常断开
        if (recv_size <= 0)
        {
            is_offline = true;
            break;
        }

        // 限流：缓存队列达到上限，阻塞等待解析释放空间
        while (cache.size() >= CACHE_LIMIT);

        // 将本次读取到的所有字节压入专属缓存队列
        for (int i = 0; i < recv_size; ++i)
        {
            cache.push(recv_buf[i]);
        }

        // 循环拆分缓存，按分隔符 \n 切割完整JSON消息
        std::string single_json;
        while (!cache.empty())
        {
            char ch = cache.front();
            cache.pop();

            if (ch == '\n')
            {
                // 截取一条完整JSON，执行业务逻辑
                std::cout << "[" << ip << "] 完整JSON消息：" << single_json << std::endl;
                // 组装回复数据
                std::string rsp=single_json + "\n";
                std::string reply;
                //std::string rsp = "服务端已收到消息：" + single_json + "\n";
                reply= JsonParsing(rsp,mysqlconnect);
                send(fd, reply.c_str(), reply.size(), 0);
                single_json.clear();
            }
            else
            {
                single_json += ch;
            }
        }
    }

    // 客户端下线收尾工作
    std::cout << "[客户端下线] IP：" << ip << "，关闭套接字" << std::endl;
    close(fd);
    delete client_data; // 释放堆上分配的客户端参数内存
    return nullptr;
}
*/