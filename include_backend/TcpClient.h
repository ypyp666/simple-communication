#ifndef TCPCLIENT_H
#define TCPCLIENT_H

#include <QObject>
#include <QTcpSocket>
#include <QAbstractSocket>
#include <QByteArray>
#include <QTimer>


class TcpClient : public QObject
{
    Q_OBJECT
public:
    explicit TcpClient(QObject* parent = nullptr);
    ~TcpClient();

    // 按地址路由表依次尝试（表见 TcpClient.cpp 顶部的 kEndpoints），调用方不再传 IP
    void connectToServer();
    void reconnect();  // 断线重连：同样按整张地址表走一遍
    void disconnectFromServer();
    void sendData(const QByteArray& data);
    bool isConnected() const;
    QString errorString() const;
    // 开启心跳检验：登录成功后由 MainBackend 调用，每 3 秒发一个检验包（见 onExamineTimeout）
    void StartExamine();
    // 断线自愈的开关：登录成功开（之后链路断了要一直退避重试到连上），
    // 登出 / 被踢回登录页关——那之后没有会话在等数据，不该在后台空转
    void enableBackoff();
    void disableBackoff();

signals:
    void connected();
    void disconnected();
    void dataReceived(const QByteArray& data);
    void errorOccurred(QAbstractSocket::SocketError error);
    void connectionTimeout();

private slots:
    void onSocketConnected();
    void onSocketDisconnected();
    void onSocketReadyRead();
    void onSocketError(QAbstractSocket::SocketError error);
    void onConnectTimeout();
    void onExamineTimeout();       // 心跳定时器到点：发检验包 + 开等待计时器
    void onExamineWaitTimeout();   // 等待计时器到点：这一秒没等到合法回包，算一次失败
    void onBackoffTimeout();       // 退避等待到点：重走一遍地址表发起重连

private:
    // 按 m_endpointIndex 找下一个可用地址并发起连接；返回 false = 表已翻到底
    bool startNextAttempt();
    // 当前地址失败后换下一个；返回 false = 已经是最后一个候选
    bool tryNextEndpoint();
    // 心跳回包的"就地吃掉"：是心跳回包就返回 true（不再往业务层转），别的包返回 false 照常放行
    bool handleExamineResponse(const QByteArray& packet);
    // 排下一次退避重连。断线/连接失败/心跳判死都从它这一个口进来，
    // 三重前提（已登录 + 非主动断开 + 还没排上）缺一不可，见 .cpp
    void scheduleReconnect();

    QTcpSocket* m_socket;
    QByteArray m_recvBuffer;
    QTimer* m_connectTimeoutTimer;
    QTimer* m_examineTimer;      // 心跳检验定时器（登录成功后启动，每 3 秒一次）
    QTimer* m_examineWaitTimer;  // 心跳等待计时器（发出检验包后只等 1 秒，单次触发）
    QTimer* m_backoffTimer;      // 退避重连计时器（单次触发；档位表见 .cpp 的 kBackoffSteps）
    QString m_host;          // 当前尝试的地址（只用于日志，权威来源是 .cpp 里的地址表）
    // 连续超时次数，同时也是这一次检验包的"检验 id"。
    // 只有在等待时间内收到 id 等于它的回包才清零；每超时一次 +1
    int examineCount;

    int m_port;
    int m_endpointIndex;     // 当前尝试到地址表第几项（见 kEndpoints）
    bool m_routing;          // true = 正在挨个试地址：失败先重试，不立刻上报
    int m_retryIndex;        // 当前退避档位下标（连上后归零，下次断线又从 1 秒起步）
    bool m_backoffEnabled;   // 是否允许断线自愈（已登录 == true，由 MainBackend 开/关）
    // 主动断开闸：disconnectFromServer 会同步触发 disconnected，
    // 不加这道闸，登出时 TcpClient 会把自己的断开当成"意外断线"又排上退避
    bool m_manualDisconnect;
    // 连接流程进行中（connectToServer 起到连上 / 彻底失败止）。
    // connectToServer 里 abort 旧连接会同步抛出 disconnected/errorOccurred，
    // 靠它挡住"新一轮已经开始了、退避却又被自己排上"的重叠
    bool m_connecting;
};

#endif // TCPCLIENT_H
