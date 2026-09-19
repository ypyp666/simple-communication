#ifndef CHATBACKEND_H
#define CHATBACKEND_H

#include <QObject>
#include <QString>
#include <QList>
#include <QDateTime>
#include "TcpClient.h"
#include <QMap>
#include <QSet>
#include <QTimer>
#include "FeatureStructs.h"  // 引入共享结构体：ContactInfo / MessageInfo / OutgoingMessage

class ChatBackend : public QObject
{
    Q_OBJECT
public:
    explicit ChatBackend(QObject *parent = nullptr,TcpClient* tcpclient = nullptr);
    // 登录成功后由主后端同步当前登录账号（拉取重试 onPullTimerTimeout 需要用到 m_userid）
    void setUserId(const QString& userId);

signals:
    void contactsLoaded(const QList<ContactInfo>& contacts);
    void newMessageReceived(const MessageInfo& message);        // 接收成功（携带完整消息，UI显示+存库+回ACK）
    void messageReceiveFailed(const QString& serverId);         // 接收失败（携带服务器消息ID，回ACK让服务器重发）
    void contactStatusChanged(const QString& contactId, bool isOnline);
    void fileTransferProgress(const QString& messageId, int progress);
    void fileTransferCompleted(const QString& messageId, bool success);
    void loginSuccess(const QString& accountId,bool isSuccess);
    void sendFailed(const QString& tempId, const QString& serverId);   // 发送失败（tempId=本地临时消息ID，serverId=服务器ID可为空）
    void sendSuccess(const QString& tempId, const QString& serverId);  // 发送成功（tempId=本地临时消息ID，serverId=服务器分配的ID）




public slots:
    void loadContacts();
    void startSendMessage(const QString& contactId, const OutgoingMessage& message);
    void sendFile(const QString& contactId, const QString& filePath);
    void markMessagesAsRead(const QString& contactId);
    void connectToServer();
    void disconnectFromServer();
    void sendMessage(const QString& contactId, const OutgoingMessage& message);
    // 输入内容记忆功能
    void saveInputContent(const QString& contactId, const QString& content);
    QString getInputContent(const QString& contactId);
    void clearInputContent(const QString& contactId);
    void onTcpDisconnected();
    void onTcpError(QAbstractSocket::SocketError error);
    void onTcpConnectionTimeout();
    // 【发消息回执】处理服务器对"我发出的消息"的确认包（type=repost_response），
    // 不是收消息！解析 success/tempId/serverId 后发 sendSuccess/sendFailed 让 UI 停止转圈动画
    void onTcpDataReceived(const QByteArray& packet);
    // 【收消息】处理服务器转发来的他人消息（type=repost），
    // 解析出完整 MessageInfo 后发 newMessageReceived（UI显示 + 存库 + 回ACK）
    void onTcpRepost(const QByteArray& packet);
    // 回复接收确认：收到消息后回ACK给服务器
    void sendReceiveAck(const QString& serverId, bool success);
    // 发送拉取请求：接收失败时向服务器请求重发未收到的缓存消息
    void sendPullRequest(const QString& targetId);
    void onPullTimerTimeout();//拉取请求超时处理
    void onPullResponse(const QByteArray& packet);//拉取请求确认（服务器发完所有缓存消息后发来，用于停止拉取定时器）

private:
    QMap<QString, QString> inputCache;  // 存储每个联系人的输入内容
    TcpClient* m_tcpClient;
    bool m_loginCompleted;
    QString m_userid;
    QString m_password;
    QTimer* m_pullTimer;
    int pullCount = 0;//拉取请求次数，最多3次
    QMap<QString, QTimer*> m_pendingSends;  // tempId → 发送超时定时器（30秒兜底，防止服务器不回包气泡永久转圈）
    QSet<QString> m_pendingAcks;  // 待回复确认（receive_ack）的消息ID队列：onTcpRepost 收到有效消息时登记，
                                  // sendReceiveAck 回执成功后移除；防止拿空ID/重复ID回ACK（空serverId会让服务器stoull崩溃）

    void removeSendTimer(const QString& tempId);  // 收到回执或超时触发后清理对应定时器
};

#endif // CHATBACKEND_H