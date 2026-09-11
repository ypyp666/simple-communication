#pragma once
#include <QObject>
#include <QString>
#include <QByteArray>
#include <QAbstractSocket>
#include "TcpClient.h"

class LoginBackend : public QObject
{
    Q_OBJECT
public:
    explicit LoginBackend(QObject* parent = nullptr, TcpClient* tcpclient = nullptr);
    ~LoginBackend();
    void onTcpDataReceived(const QByteArray& data);
    void startLogin(const QString& accountId, const QString& password);
    void ModifyPwdAquird(const QString& account, const QString& newPassword);
    // 修改密码：连接建立后发送请求（用 ModifyPwdAquird 保存的账号/新密码）
    // 单独拆出来是因为 connectToServer 是异步的，不能在连接建立前 sendData，得等 connected 信号
    void sendModifyPwdRequest();
    // 修改密码流程的 TCP 错误/超时回调（与登录流程分开，避免误发 loginFailed/loginTimeout）
    void onModifyPwdError(QAbstractSocket::SocketError error);
    void onModifyPwdTimeout();

signals:
    void loginSuccess(const QString& accountId);
    void loginFailed();
    void loginWaiting();
    void loginTimeout();
    void modifyPwdSuccess();   // 修改密码成功
    void modifyPwdFailed();    // 修改密码失败（服务器拒绝 / 解析失败 / TCP 错误）
    void modifyPwdTimeout();   // 修改密码连接超时
    void modifyPwdWaiting();   // 修改密码等待中信号

public slots:
    // TcpClient 的连接状态回调，由 MainBackend 统一路由后直接调用（所以放 public）
    void onTcpConnected();
    void onTcpDisconnected();
    void onTcpError(QAbstractSocket::SocketError error);
    void onTcpConnectionTimeout();


private:
    TcpClient* m_tcpClient;
    QString m_accountId;
    QString m_password;
    QString m_newPassword;  // 修改密码时的新密码（登录密码用 m_password，二者分开存）
    bool m_loginCompleted;
    bool m_ownTcpClient;  // 是否自己创建的TcpClient

    void sendLoginRequest();
};
