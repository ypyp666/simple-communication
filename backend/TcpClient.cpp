#include "TcpClient.h"
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

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

// 心跳检验周期：登录成功后每 3 秒触发一次
const int kExamineIntervalMs = 3000;

// 发出检验包后最多等 1 秒，等不到回包就算这次检验失败
const int kExamineWaitMs = 1000;

// 连续超时次数超过它（即第 4 次还没回应）才判定链路已死，交给退避重连
const int kExamineFailLimit = 3;

// 心跳包的报文类型：请求 examine / 回应 examine_response，靠 examineId 配对
const char kExamineTypeProbe[] = "examine";
const char kExamineTypeResponse[] = "examine_response";

// 退避重连的档位表（毫秒）：1 / 2 / 4 / 8 / 16 秒。
// 到最后一档封顶、不再涨——之后一直 16 秒试一次，打到连上为止（用户拍板"永不停"）。
// 只用一个定时器：每次失败只是换一个 interval 重新 start，不需要四个定时器互相停
const int kBackoffSteps[] = { 1000, 2000, 4000, 8000, 16000 };
const int kBackoffStepCount = int(sizeof(kBackoffSteps) / sizeof(kBackoffSteps[0]));
}

TcpClient::TcpClient(QObject* parent)
    : QObject(parent), examineCount(0), m_port(0), m_endpointIndex(0), m_routing(false),
      m_retryIndex(0), m_backoffEnabled(false), m_manualDisconnect(false), m_connecting(false)
{
    m_socket = new QTcpSocket(this);
    // 创建 TCP 套接字

    m_connectTimeoutTimer = new QTimer(this);
    m_connectTimeoutTimer->setSingleShot(true);//设置为单次触发，避免重复触发

    // 心跳检验定时器：周期性触发（不是单次），登录成功后由 StartExamine() 启动
    m_examineTimer = new QTimer(this);
    m_examineTimer->setInterval(kExamineIntervalMs);

    // 心跳等待计时器：每发出一个检验包就起一次，只等 1 秒就响（单次）
    m_examineWaitTimer = new QTimer(this);
    m_examineWaitTimer->setSingleShot(true);
    m_examineWaitTimer->setInterval(kExamineWaitMs);

    // 退避重连计时器：排下一次重连用（单次触发，间隔按档位表换）
    m_backoffTimer = new QTimer(this);
    m_backoffTimer->setSingleShot(true);
    
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
    connect(m_examineTimer, &QTimer::timeout, this, &TcpClient::onExamineTimeout);
    connect(m_examineWaitTimer, &QTimer::timeout, this, &TcpClient::onExamineWaitTimeout);
    connect(m_backoffTimer, &QTimer::timeout, this, &TcpClient::onBackoffTimeout);
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
    // 新的连接流程开始：撤掉主动断开闸、停掉退避表，并立起"流程进行中"。
    // 顺序关键：连接流程的旗必须在 abort 之前立——abort 同步抛出的 disconnected
    // 会被 scheduleReconnect 用这个旗挡回去；否则新旧两轮会互相抢着连
    m_manualDisconnect = false;
    m_backoffTimer->stop();
    m_connecting = true;

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
    // 闸落下 + 停掉退避：接下来那次 disconnected 是"自己安排的"，
    // 不是意外断线，绝不能被退避重连当成断线信号又排一次
    m_manualDisconnect = true;
    m_connecting = false;
    m_backoffTimer->stop();
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
    m_connecting = false;
    m_connectTimeoutTimer->stop();
    // 连上了：退避表作废、档位归零（下次再断线又从 1 秒起步）
    m_backoffTimer->stop();
    m_retryIndex = 0;
    // 心跳检查的连续超时次数同样归零：新连接从第 0 次开始数
    examineCount = 0;
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

    // 先让业务层收到 disconnected（清各自的状态），再排退避——
    // 链路有没有"自愈资格"由 scheduleReconnect 内部判断
    scheduleReconnect();
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

        // 心跳回包是传输层自己的控制包，在这里就地吃掉：
        // 业务层不认识 examine_response，转下去只会被当成未知类型
        if (handleExamineResponse(packet)) {
            continue;
        }

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
    m_connecting = false;   // 地址表翻到底：连接流程到此为止，该交给退避了
    m_connectTimeoutTimer->stop();
    emit errorOccurred(error);
    scheduleReconnect();
}

void TcpClient::onConnectTimeout()
{
    // 总超时到点：不管当前在试哪个地址，都判定为连不上
    m_routing = false;
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->abort();   // 这里同步抛的 disconnected 还被 m_connecting 挡着
    }
    m_connecting = false;
    emit connectionTimeout();
    scheduleReconnect();
}

// 开启心跳检验：登录成功后由 MainBackend 调一次。
// 重复 start 只是把周期重新计时，不会被调用两次就多出两个循环（全程只有这一个定时器对象）
void TcpClient::StartExamine()
{
    m_examineTimer->start(kExamineIntervalMs);
}

// 心跳定时器到点（每 3 秒一次）：发一个检验包出去，包上带当前次数当"检验 id"，
// 然后只给自己 1 秒的等待时间
void TcpClient::onExamineTimeout()
{
    QJsonObject probe;
    probe["type"] = kExamineTypeProbe;
    probe["examineId"] = examineCount;

    sendData(QJsonDocument(probe).toJson(QJsonDocument::Compact) + '\n');

    // 每发一次重新计时：等回包 1 秒，等不到由 onExamineWaitTimeout 判失败
    m_examineWaitTimer->start(kExamineWaitMs);
}

// 等待计时器到点：这 1 秒没收到"id 对得上"的回包 → 算一次连续超时。
// 次数 +1 同时也是下一次检验包的 id，所以旧回包（id 是过去的数字）不会被认账
void TcpClient::onExamineWaitTimeout()
{
    ++examineCount;

    // 连续超限 → 判定链路已死（典型是 TCP 层还"连着"、对面其实已经不响了）。
    // 主动掐掉这条半死不活的连接，再排退避重连；
    // abort 同步触发的 disconnected 不会造成重复排（scheduleReconnect 有"已排上"的保护）
    if (examineCount > kExamineFailLimit) {
        qDebug() << "TcpClient: 心跳连续超时" << examineCount << "次，判定链路已死，转入退避重连";
        m_examineTimer->stop();
        m_examineWaitTimer->stop();
        if (m_socket->state() != QAbstractSocket::UnconnectedState) {
            m_socket->abort();
        }
        scheduleReconnect();
    }
}

// 心跳回包处理：只有回包里的 examineId 等于当前 examineCount 才认账、把次数清零。
// 小于当前次数的都是迟到的旧回包（那一轮早就判超时了），放它过去只会错误地清零计数
bool TcpClient::handleExamineResponse(const QByteArray& packet)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(packet, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;   // 不是合法 JSON：不是心跳回包，照常转给业务层去判/报错
    }

    const QJsonObject object = doc.object();
    if (object["type"].toString() != kExamineTypeResponse) {
        return false;
    }

    // 只认数字类型的 id：服务器漏带、或带成字符串时 toInt() 会退化成 0，
    // 而 0 本身是合法 id，不挡住就会把一次真失败误判成成功
    const QJsonValue examineId = object["examineId"];
    if (examineId.isDouble() && examineId.toInt() == examineCount)//先问是不是数字再问跟当前确认号码对不对的上
     {
        examineCount = 0;                    // 对上号 = 这次检验成功
        m_examineWaitTimer->stop();          // 不用再等超时了
    }
    return true;   // 是心跳回包，无论 id 对不对都吃掉，不往业务层转
}

// ===== 退避重连 =====
// 断线自愈的开关：登录成功时由 MainBackend 开、登出/被踢时关。
// 开着才允许排退避——"只要没连上就一直发"只在会话进行中成立，登录页 / 登出后不代劳
void TcpClient::enableBackoff()
{
    m_backoffEnabled = true;
    m_retryIndex = 0;   // 新一轮会话：断线从 1 秒档起步
}

void TcpClient::disableBackoff()
{
    m_backoffEnabled = false;
    m_backoffTimer->stop();
    m_retryIndex = 0;
}

// 排下一次退避重连。三个前提缺一不可：
//   ① 会话还在（已登录）——登录页或登出后不该在后台偷偷重连
//   ② 不是主动断开——disconnectFromServer 会同步触发 disconnected，
//      不挡住的话，登出会变成"自己断开→自己排退避→自己重连"的死循环
//   ③ 还没有排上的——断线/错误/超时常常一起来（一次 abort 就可能连发几个信号），只认第一次
void TcpClient::scheduleReconnect()
{
    if (!m_backoffEnabled || m_manualDisconnect || m_backoffTimer->isActive())
     {
        return;
    }
    // 连接流程还在跑（含 connectToServer 里 abort 旧连接同步抛出的信号）：
    // 等这一轮的结果说话，现在排上只会跟它抢着连
    if (m_connecting) {
        return;
    }

    // 心跳的应用层检测先停掉：链路已经不在了，再发检验包只会白攒失败次数
    m_examineTimer->stop();
    m_examineWaitTimer->stop();

    const int interval = kBackoffSteps[m_retryIndex];
    // 最后一档封顶：之后一直用它（16 秒），不再往上翻倍
    if (m_retryIndex < kBackoffStepCount - 1) {
        ++m_retryIndex;
    }
    qDebug() << "TcpClient: 将在" << interval << "毫秒后重连";
    m_backoffTimer->start(interval);
}

// 退避到点：重走一遍地址表（虚拟机优先，连不上自动落到云服务器）。
// 成功会走 onSocketConnected（停表 + 档位归零）；失败又会回到 scheduleReconnect 排下一档
void TcpClient::onBackoffTimeout()
{
    qDebug() << "TcpClient: 退避到点，发起重连";
    connectToServer();
}
