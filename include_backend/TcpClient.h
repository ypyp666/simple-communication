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

private:
    // 按 m_endpointIndex 找下一个可用地址并发起连接；返回 false = 表已翻到底
    bool startNextAttempt();
    // 当前地址失败后换下一个；返回 false = 已经是最后一个候选
    bool tryNextEndpoint();

    QTcpSocket* m_socket;
    QByteArray m_recvBuffer;
    QTimer* m_connectTimeoutTimer;
    QString m_host;          // 当前尝试的地址（只用于日志，权威来源是 .cpp 里的地址表）

    int m_port;
    int m_endpointIndex;     // 当前尝试到地址表第几项（见 kEndpoints）
    bool m_routing;          // true = 正在挨个试地址：失败先重试，不立刻上报
};

#endif // TCPCLIENT_H
