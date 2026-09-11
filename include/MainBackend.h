#ifndef MAINBACKEND_H
#define MAINBACKEND_H

#include <QObject>
#include <QString>
#include <QThread>
#include <qstringview.h>
#include "LoginBackend.h"
#include "ChatBackend.h"
#include "DatabaseManager.h"
#include "TcpClient.h"
#include "FeatureStructs.h"  // 共享结构体：ContactInfo / MessageInfo / OutgoingMessage

class MainBackend : public QObject
{
    Q_OBJECT
public:
    MainBackend(QObject* parent = nullptr);
    ~MainBackend();

    ChatBackend* getChatBackend();
    TcpClient* m_tcpClient;
    QString m_userid;
    QString m_password;  
    bool m_loginCompleted; // 登录是否已完成，避免重复登录和无限重连
    DatabaseManager* m_databaseManager;

    static bool s_loggedIn;  // 全局登录状态（定义在 MainBackend.cpp），TCP 断线重连时判断要不要自动重连

    // 获取当前登录账号ID（登录成功后 m_userid 由登录流程写入，UI 通过它拿真实账号）
    QString currentUserId() const { return m_userid; }


signals:
    void loginSuccess(const QString& accountId);
    void loginFailed();
    void registerSuccess();
    void registerFailed();
    void loginWaiting();  // 登录等待中信号
    void loginTimeout();  // 登录连接超时信号
    void modifyPwdSuccess();  // 修改密码成功（忘记密码页提交后服务器确认）
    void modifyPwdFailed();   // 修改密码失败（服务器拒绝/解析失败/TCP错误）
    void modifyPwdTimeout();  // 修改密码连接超时
    void modifyPwdWaiting();  // 修改密码等待中信号
    void sendWaiting();  // 发送等待信号
    void messageSendSuccess(const QString& tempId, const QString& serverId);  // 发送成功（tempId=本地临时消息ID，serverId=服务器分配的ID）
    void messageSendFailed(const QString& tempId, const QString& serverId);  // 发送失败（未连接/服务器拒绝/超时等），UI变红色感叹号

    // === 聊天相关信号（由 ChatBackend 转发）===
    void contactsLoaded(const QList<ContactInfo>& contacts);
    void messagesLoaded(const QList<MessageInfo>& messages);
    void messageReceived(const MessageInfo& message);                // 接收成功（对方发来的新消息，UI显示+存库+回ACK）
    void messageReceiveFailed(const QString& serverId);              // 接收失败（携带服务器消息ID，回ACK让服务器重发）

    // === 数据库异步请求信号（主线程发出 → 后台DB线程执行）===
    void dbSaveMessageRequested(const MessageInfo& message);
    void dbSetAccountRequested(const QString& accountId);
    void dbUpdateMessageIDRequested(const QString& contactId, const QString& oldId, const QString& newId);

    // === 数据库结果信号（后台DB线程回传 → 主线程）===
    void messageSaved(bool success);
    void messageIdUpdated(bool success);
    void databaseInitialized(bool success);

public slots:
    void login(const QString& username, const QString& password);
    void registerUser(const QString& username, const QString& password);
    // 修改密码入口（忘记密码页提交后调用）：未登录态走 TCP，设置功能枚举后连接服务器
    void modifyPwd(const QString& account, const QString& newPassword);
    void JsonParsing(const QByteArray packet);
    void sendMessage(const MessageInfo& message);

    // === 聊天相关接口（转发到 ChatBackend）===
    void loadContacts();
    void loadMessages(const QString& contactId);
    void sendFile(const QString& contactId, const QString& filePath);
    // 输入内容记忆功能
    void saveInputContent(const QString& contactId, const QString& content);
    QString getInputContent(const QString& contactId);
    void clearInputContent(const QString& contactId);
    // 断开会话：直接关闭当前 TCP 连接（转发到 TcpClient）
    void disconnectSession();

    // === 数据库异步接口（只发请求信号，不阻塞主线程）===
    void saveMessage(const MessageInfo& message);
    void setCurrentAccountId(const QString& accountId);
    void onDbMessageIdUpdated(const QString& contactId, const QString& oldId, const QString& newId);
    

private slots:
    // 后台数据库线程的结果回传（在【主线程】执行）
    void onDbMessageSaved(bool success);
    void onDbInitialized(bool success);
    // 收到对方消息：回ACK + 存库 + 转发给UI
    void onMessageReceived(const MessageInfo& message);
    // 接收失败：发拉取请求 + 转发给UI
    void onMessageReceiveFailed(const QString& serverId);

    // === TcpClient 共享信号统一路由（胶水层，分发给各后端）===
    void onTcpConnected();
    void onTcpDisconnected();
    void onTcpError(QAbstractSocket::SocketError error);
    void onTcpConnectionTimeout();
  

private:
    LoginBackend* m_loginBackend;
    ChatBackend* m_chatBackend;
    QThread* m_dbThread;  // 后台数据库线程

    // 注册登录后端（登录/修改密码）的信号连接，从构造函数抽出，避免构造函数越堆越长
    void setupLoginConnections();
    // 未登录态流程（登录/注册/修改密码）结束时统一收尾：清掉功能标记，回到"空闲"态
    void resetFeature();
    // 当前 TCP 连接所服务的功能（未登录态：登录 / 修改密码），
    // onTcpConnected/Error/Timeout 据此把 TcpClient 的信号路由到对应后端的功能函数
    LoginFeature m_currentFeature = LoginFeature::None;
};

#endif // MAINBACKEND_H
