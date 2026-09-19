#pragma once
#include <QObject>
#include <QString>
#include <QByteArray>
#include <QAbstractSocket>
#include <QTimer>
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
    // 注册第一段：注册页一显示就发起 TCP 连接，向服务器索取账号 ID（这就是"注册请求/取号"）。
    // 此时用户还没填密码，提交请求发不出去，所以这里只负责把连接拉起来
    void connectForRegister();
    // 修改密码：连接建立后发送请求（用 ModifyPwdAquird 保存的账号/新密码）
    // 单独拆出来是因为 connectToServer 是异步的，不能在连接建立前 sendData，得等 connected 信号
    void sendModifyPwdRequest();
    // 注册第二段：填好密码后提交。与修改密码不同，这里不再经中间层保存参数、也不重连，
    // 直接接收账号/密码把"提交注册"发出去
    void sendRegisterRequest(const QString& account, const QString& password);
    // 注册取号连接：连接建立后发送请求（用 connectForRegister 函数保存的账号）
    void sendconnectForRegister();
    // 登录流程：连接建立后发送请求（用 startLogin 函数保存的账号/密码）
    void sendLoginRequest();
    // 修改密码流程的 TCP 错误/超时回调（与登录流程分开，避免误发 loginFailed/loginTimeout）
    void onModifyPwdError(QAbstractSocket::SocketError error);
    void onModifyPwdTimeout();
    // 注册【提交阶段】的 TCP 错误/超时回调（与其它流程分开，避免误发 loginFailed）：
    // 错误发 registerNetworkError（网络层），不能发 registerFailed（服务器拒绝）
    void onRegisterError(QAbstractSocket::SocketError error);
    void onRegisterTimeout();
    // 注册【取号阶段】的回调：由 MainBackend 按 LoginFeature::ConnectForRegister 路由过来。
    // 与提交阶段分开是因为两阶段要发的信号不同（connectForRegister* vs register*），
    // 混在一起会让 UI 分不清是"取号连不上"还是"提交被拒"
    void onConnectForRegisterError(QAbstractSocket::SocketError error);
    void onConnectForRegisterTimeout();

signals:
    void loginSuccess(const QString& accountId);
    void loginFailed();        // 仅"服务器明确拒绝"（login_response success=false），即账号不存在/密码错误
    void loginNetworkError(const QString& reason);  // 网络层连不上（连接被拒/断网等），与密码错误分开提示
    void loginWaiting();
    void loginTimeout();
    void connectForRegisterSuccess(const QString& account);  // 注册取号连接成功，服务器下发的账号 ID 随信号带回
    void connectForRegisterFailed();
    void connectForRegisterTimeout();
    void connectForRegisterWaiting();
    void registerSuccess();    // 注册成功
    void registerFailed();     // 仅"服务器明确拒绝"（register_response success=false），如账号已存在
    void registerNetworkError(const QString& reason);  // 网络层连不上（连接被拒/断网等），与服务器拒绝分开提示
    void registerWaiting();    // 注册等待中信号
    void registerTimeout();    // 注册连接超时
    void modifyPwdSuccess();   // 修改密码成功
    void modifyPwdFailed();    // 仅"服务器明确拒绝"（modify_password_response success=false），如账号不存在
    void modifyPwdNetworkError(const QString& reason);  // 网络层连不上（连接被拒/断网等），与服务器拒绝分开提示
    void modifyPwdTimeout();   // 修改密码连接超时
    void modifyPwdWaiting();   // 修改密码等待中信号

public slots:
    // TcpClient 的连接状态回调，由 MainBackend 统一路由后直接调用（所以放 public）
    void onTcpConnected();
    void onTcpDisconnected();
    void onTcpError(QAbstractSocket::SocketError error);
    void onTcpConnectionTimeout();


private slots:
    // "服务器响应超时"到点：连接是通的、请求也发出去了，但服务器迟迟不回包。
    // 与 onTcpConnectionTimeout（TcpClient 连接阶段超时，连不上）是两回事
    void onResponseTimeout();

private:
    // 请求发出后开表：等待服务器回包，超时按 requestType 发对应流程的超时信号
    void startResponseTimer(const QString& requestType);
    // 收到对应响应（或流程已提前失败）后停表
    void stopResponseTimer();

    TcpClient* m_tcpClient;
    QString m_accountId;
    QString m_password;
    QString m_newPassword;  // 修改密码时的新密码（登录密码用 m_password，二者分开存）
    bool m_loginCompleted;
    bool m_ownTcpClient;  // 是否自己创建的TcpClient

    // "服务器响应超时"计时器：与 TcpClient 里那个 30 秒连接超时计时器是两个阶段的东西——
    // TcpClient 的表只管"TCP 连不连得上"，连上就停；这里管"请求发出去了，响应什么时候回"。
    // 同一时刻只会有一个流程在跑（登录/改密/取号/注册四选一，由 MainBackend 的功能标记串起来），
    // 所以共用一个单次计时器够用，不会出现两个请求抢表
    QTimer* m_responseTimeoutTimer;
    QString m_pendingRequest;   // 正在等响应的请求类型（"login"/"modify_password"/...），超时后据此分发信号
};
