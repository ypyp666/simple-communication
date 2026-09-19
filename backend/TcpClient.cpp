#include "TcpClient.h"
#include <QDebug>

// 服务器地址路由表：按顺序尝试，前一个连不上就自动换下一个。
// 开发时开着虚拟机 → 命中第一项；上线时关掉虚拟机 → 自动落到云服务器那一项。
// 这样切环境不用改代码、也不用改配置，客户端自己会挑能连上的那个。
namespace//匿名命名空间：让里面所有的变量 / 结构体，只在当前这一个 `.cpp` 文件内部可见，其他源文件完全访问不到，相当于文件级私有 
{
struct Endpoint {
    const char* host;
    int port;
};
const Endpoint kEndpoints[] = {
    { "192.168.20.128", 8899 },  // 1. 开发用：本机虚拟机里的服务端（优先）
    { "47.93.77.173",8899 },  // 2. 线上用：云服务器公网 IP（待补，先占位）
};
const int kEndpointCount = int(sizeof(kEndpoints) / sizeof(kEndpoints[0]));

// 整个"挨个试地址"过程的总超时：起点启动一次，中途换地址不重置。
// 只要在这个时限内两个地址连上任意一个就算成功。
const int kConnectTimeoutMs = 30000;
}

TcpClient::TcpClient(QObject* parent)
    : QObject(parent), m_port(0), m_endpointIndex(0), m_routing(false)
{
    m_socket = new QTcpSocket(this);
    // 创建 TCP 套接字

    m_connectTimeoutTimer = new QTimer(this);
    m_connectTimeoutTimer->setSingleShot(true);//设置为单次触发，避免重复触发
    
    // 连接信号槽
    connect(m_socket, &QTcpSocket::connected, this, &TcpClient::onSocketConnected);
    // 连接成功后,触发时机：connectToHost() 发起 TCP 握手，三次握手全部完成，成功连上服务端时触发
    connect(m_socket, &QTcpSocket::disconnected, this, &TcpClient::onSocketDisconnected);
    //触发时机：连接关闭时触发
    connect(m_socket, &QTcpSocket::readyRead, this, &TcpClient::onSocketReadyRead);
    //触发时机：操作系统内核缓冲区有服务端发来的数据，Qt 通知你可以读取。
    connect(m_socket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::errorOccurred),
            this, &TcpClient::onSocketError);
    //触发时机：套接字发生错误时触发，专门捕获所有网络错误，参数会传入错误枚举
    /*
   细讲QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::errorOccurred)这个信号
   可变参数模板是c++11的新特性，用于处理不同参数类型，QOverload是一个模板类，可以绑定到不同参数类型的信号槽。of()方法用于指定参数类型。
    QAbstractSocket里有多个重载版本的errorOccurred信号，用于处理套接字错误。
    直接绑定会有二义性，需要指定参数类型。用of()方法来过滤出我们需要的信号。
   */
    connect(m_connectTimeoutTimer, &QTimer::timeout, this, &TcpClient::onConnectTimeout);
 /*
    向操作系统申请一块 TCP 通信资源；
    创建 Qt 对象，提供信号槽（readyRead、connected、disconnected）异步回调，不卡 UI；
    绑定父对象 MainBackend，生命周期和全局后端同步，程序退出自动释放连接。
    在 Qt 层创建一个管理 TCP 逻辑的对象；
    调用操作系统 socket() 系统调用，在内核分配一个空的套接字文件描述符（fd）；
    初始化收发缓冲区、绑定信号槽。
    */}

TcpClient::~TcpClient()
{
    disconnectFromServer();
         /*
        优雅关闭接口：1.本地发送 FIN 报文，启动四次挥手；2.不会立刻销毁 fd，会先把本地缓冲区剩余数据发送完毕；3.socket 进入 ClosingState（正在关闭）。
        */

        // 确保套接字已关闭，释放资源
        m_socket->deleteLater();
     
     
}

void TcpClient::connectToServer()
{
    // 如果已经有连接/正在连接，先掐掉（异步），避免和新连接抢同一个 socket。
    // 注意：这一步放在设置 m_routing 之前——万一 abort 同步抛出错误信号，
    // 也不会被 onSocketError 当成"当前地址失败"而把第一项（虚拟机）跳过去
    m_routing = false;
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->abort();
            // 断开后会触发 onSocketDisconnected 信号
    }

    // 从头开始走地址表：先虚拟机，再云服务器
    m_endpointIndex = 0;
    m_routing = true;

    // 启动30秒总超时计时器：覆盖"挨个试地址"的整个过程，中途换地址不重置
    m_connectTimeoutTimer->start(kConnectTimeoutMs);//全局共享计时，避免重复触发

    startNextAttempt();
}

// 按 m_endpointIndex 找下一个可用地址并发起连接；返回 false = 表已翻到底
bool TcpClient::startNextAttempt()
{
    if (m_endpointIndex < 0 || m_endpointIndex >= kEndpointCount) {
        return false;
    }

    const Endpoint& ep = kEndpoints[m_endpointIndex];
    m_host = ep.host;
    m_port = ep.port;

    qDebug() << "TcpClient: 尝试连接" << m_host << m_port;
       // 连接到服务器
    m_socket->connectToHost(m_host, m_port);
    return true;
}

// 当前地址失败后换下一个；返回 false = 已经是最后一个候选
bool TcpClient::tryNextEndpoint()
{
    ++m_endpointIndex;
    return startNextAttempt();
}

void TcpClient::reconnect()
{
    // 断线重连：整张地址表重走一遍（虚拟机优先，连不上自动落到云服务器）
    connectToServer();
}

void TcpClient::disconnectFromServer()
{
    m_routing = false;   // 主动断开，不再自动换地址
    m_connectTimeoutTimer->stop();
    
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->disconnectFromHost();
        m_socket->waitForDisconnected();
    }
           //waitForDisconnected()主动阻塞等待连接断开 等待套接字关闭，确保所有数据发送完毕

}

void TcpClient::sendData(const QByteArray& data)
{
    if (m_socket->state() == QAbstractSocket::ConnectedState) {
        m_socket->write(data);
        m_socket->flush();
    }
}

bool TcpClient::isConnected() const
{
    return m_socket->state() == QAbstractSocket::ConnectedState;
}

QString TcpClient::errorString() const
{
    return m_socket->errorString();
}

void TcpClient::onSocketConnected()
{
    m_routing = false;   // 已经连上，不再换地址
    m_connectTimeoutTimer->stop();
    emit connected();
}

void TcpClient::onSocketDisconnected()
{
    m_connectTimeoutTimer->stop();
    
    if (m_socket->bytesAvailable() > 0) {// 连接断开时，检查是否还有未读取的数据
        m_recvBuffer.append(m_socket->readAll());// 保留读取所有可读数据
        onSocketReadyRead();
    }
    
    emit disconnected();
}

void TcpClient::onSocketReadyRead()
{
    m_recvBuffer.append(m_socket->readAll());// 将新数据追加到接收缓冲区
    
    while (true) {
         // 查找换行符位置
        int newlinePos = m_recvBuffer.indexOf('\n');
        // 如果没有完整的包，等待下次数据
        if (newlinePos == -1) {
            break;
        }
             // 提取一个完整的数据包（不包含换行符）
        QByteArray packet = m_recvBuffer.left(newlinePos);
            // 从缓冲区移除已处理的数据包
        m_recvBuffer.remove(0, newlinePos + 1);
        
        emit dataReceived(packet);
    }
}

void TcpClient::onSocketError(QAbstractSocket::SocketError error)
{
    // 正在挨个试地址：当前地址失败就换下一个，直到表翻到底才把错误上报。
    // 这样"虚拟机没开"不会被当成致命错误，而是自动落到云服务器
    if (m_routing && tryNextEndpoint()) {
        qDebug() << "TcpClient: 地址不可用，改试下一个" << m_host << m_port;
        return;
    }

    m_routing = false;
    m_connectTimeoutTimer->stop();
    emit errorOccurred(error);
}

void TcpClient::onConnectTimeout()
{
    // 总超时到点：不管当前在试哪个地址，都判定为连不上
    m_routing = false;
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->abort();
    }
    emit connectionTimeout();
}
