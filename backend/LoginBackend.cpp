#include "LoginBackend.h"
#include <QJsonObject>
#include <QJsonDocument>
#include <QDebug>

namespace {
// "服务器响应超时"时长：请求已经发出去、连接也还通着，但服务器迟迟不回包。
// 与 TcpClient 的 kConnectTimeoutMs（30 秒，只管"TCP 连不连得上"，连上就停）是两个阶段，
// 所以必须各用各的计时器：连上之后服务器要是装死，只有这张表能把流程从等待态里拽出来
const int kResponseTimeoutMs = 10000;
}

LoginBackend::LoginBackend(QObject* parent, TcpClient* tcpclient)
    : QObject(parent), m_tcpClient(tcpclient), m_loginCompleted(false), m_ownTcpClient(false),
      m_responseTimeoutTimer(nullptr)
{
    // 使用传入的引用参数初始化TCP客户端
    // 注意：TcpClient 的信号槽统一由主后端（MainBackend）路由分发，
    // 这里不再直连信号，避免多个后端重复监听共享信号
    qDebug() << "LoginBackend: m_tcpClient=" << m_tcpClient;

    // "服务器响应超时"计时器：单次触发，每次发包时启动、收到对应响应就停。
    // 放本类而不是 TcpClient，是因为 TcpClient 那张表连上就停了，
    // 覆盖不到"连上了但服务器不回包"这一段——不加这个，UI 会一直卡在"正在登录..."
    m_responseTimeoutTimer = new QTimer(this);
    m_responseTimeoutTimer->setSingleShot(true);
    connect(m_responseTimeoutTimer, &QTimer::timeout, this, &LoginBackend::onResponseTimeout);
}

LoginBackend::~LoginBackend()
{
    // 只有自己创建的 TcpClient 才由这里负责处理
    // 借用的指针归 parent 管理：MainBackend 析构时 Qt 按创建顺序先销毁 TcpClient，
    // 轮到本析构函数时 m_tcpClient 已是悬垂指针，解引用是 UB，所以借用情况下一律不碰
    if (m_ownTcpClient && m_tcpClient) {
        m_tcpClient->disconnectFromServer();
        delete m_tcpClient;
        m_tcpClient = nullptr;
    }
}

void LoginBackend::startLogin(const QString& accountId, const QString& password)
{
    // 保存登录凭证
    m_accountId = accountId;
    m_password = password;
    m_loginCompleted = false;
    
    // 连接服务器（异步操作）：具体连哪个 IP 由 TcpClient 的地址路由表决定
    m_tcpClient->connectToServer();
    emit loginWaiting();
}

void LoginBackend::ModifyPwdAquird(const QString& account, const QString& newPassword)
{
    // 保存修改密码所需参数（连接建立后由 MainBackend 路由回来调 sendModifyPwdRequest 发包）
    m_accountId = account;
    m_newPassword = newPassword;
    m_loginCompleted = false;

    // 连接服务器（异步）。修改密码无需登录，但同样走这条共享 TCP 连接。
    // 注意：不能在连接建立前 sendData，否则数据会因 socket 未连接而直接丢弃，
    // 所以请求的发送延迟到 onTcpConnected 路由之后由 sendModifyPwdRequest 完成
    m_tcpClient->connectToServer();
    emit modifyPwdWaiting();
}

// 注册第一段（取号）：注册页一显示就把 TCP 拉起来，账号 ID 由服务器在连接后下发。
// 与 ModifyPwdAquird 的区别是本函数不保存任何参数、也不 emit 等待信号——
// 用户还没填密码，这里只是"把连接准备好"，真正的提交在第二段 sendRegisterRequest
void LoginBackend::connectForRegister()
{
    m_tcpClient->connectToServer();
    emit connectForRegisterWaiting();
}

// 取号阶段连接出错（连接被拒/断网等）：对应 UI 账号行的红色指示器，点击可重试。
// 这里没有"服务器拒绝"的概念（还没提交任何东西），所以只发 connectForRegisterFailed
void LoginBackend::onConnectForRegisterError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error);
    qDebug() << "注册取号连接出错:" << m_tcpClient->errorString();
    // 同登录：连接废了就不会再有响应，停表防止稍后误报 connectForRegisterTimeout
    stopResponseTimer();
    emit connectForRegisterFailed();
}

// 取号阶段连接超时：与出错同一处理（UI 侧两个槽都切红色指示器）
void LoginBackend::onConnectForRegisterTimeout()
{
    qDebug() << "注册取号连接超时（30秒）";
    emit connectForRegisterTimeout();
}

void LoginBackend::sendLoginRequest()
{
    // 构建登录数据包（JSON格式）
    QJsonObject loginData;
    loginData["type"] = "login";
    loginData["account"] = m_accountId;
    loginData["password"] = m_password;
    
    // 序列化JSON
    QJsonDocument doc(loginData);
    QByteArray jsonData = doc.toJson(QJsonDocument::Compact);
     /*
    toJson () 核心功能：序列化（对象 → 字节流）
    将 doc 中封装的 JSON 语法树，转换成连续的二进制字节数组 QByteArray，这是 TCP 能发送的合法数据格式。
    Compact	压缩紧凑格式，删除所有换行、空格、制表符,Indented	带换行缩进格式化，方便人阅读调试
    */

    // 添加换行符作为包分隔符（解决TCP粘包问题）
    QByteArray packet = jsonData + '\n';
    
    // 发送数据包
    m_tcpClient->sendData(packet);
    
    qDebug() << "发送登录请求:" << jsonData;
    // 请求发出去了：开表等 login_response。连上但服务器不回包时靠它兜底，
    // 否则 UI 会永远停在"正在登录..."（TcpClient 的连接超时表此时已经停了）
    startResponseTimer("login");
}

void LoginBackend::sendModifyPwdRequest()
{
    // 构建修改密码数据包（JSON格式），账号/新密码来自 ModifyPwdAquird 保存的成员
    QJsonObject modifyPwdData;
    modifyPwdData["type"] = "modify_password";
    modifyPwdData["account"] = m_accountId;
    modifyPwdData["new_password"] = m_newPassword;
    
    // 序列化JSON
    QJsonDocument doc(modifyPwdData);
    QByteArray jsonData = doc.toJson(QJsonDocument::Compact);
    
    // 添加换行符作为包分隔符（解决TCP粘包问题）
    QByteArray packet = jsonData + '\n';
    
    // 发送数据包
    m_tcpClient->sendData(packet);
    
    qDebug() << "发送修改密码请求:" << jsonData;
    // 开表等 modify_password_response（同登录：连上之后的等待由这张表兜底）
    startResponseTimer("modify_password");
}

// 注册第二段（提交）：填好密码后由 UI 直接触发，账号/密码从参数进来，不再经中间层保存。
// 组包规则与 sendModifyPwdRequest 一致：JSON 紧凑序列化 + '\n' 分包后 sendData

void LoginBackend::sendconnectForRegister()
{
    // 构建注册数据包（JSON格式）：账号是取号阶段服务器下发、已填进账号框的那个，密码由用户输入
    QJsonObject registerData;
    registerData["type"] = "connect_for_register";
    // 序列化JSON（Compact = 压缩紧凑格式，去掉多余空白）
    QJsonDocument doc(registerData);
    QByteArray jsonData = doc.toJson(QJsonDocument::Compact);
    // 添加换行符作为包分隔符（解决TCP粘包问题）
    QByteArray packet = jsonData + '\n';
    // 发送数据包
    m_tcpClient->sendData(packet);
    qDebug() << "发送注册取号请求:" << jsonData;
    // 开表等 connect_for_register_response：服务器连上了却不下发账号时，
    // 由它把账号行的转圈指示器切成红色可重试
    startResponseTimer("connect_for_register");
}

void LoginBackend::sendRegisterRequest(const QString& account, const QString& password)
{
    // 构建注册数据包（JSON格式）：账号是取号阶段服务器下发、已填进账号框的那个，密码由用户输入
    QJsonObject registerData;
    registerData["type"] = "register";
    registerData["account"] = account;
    registerData["password"] = password;

    // 序列化JSON（Compact = 压缩紧凑格式，去掉多余空白）
    QJsonDocument doc(registerData);
    QByteArray jsonData = doc.toJson(QJsonDocument::Compact);

    // 添加换行符作为包分隔符（解决TCP粘包问题）
    QByteArray packet = jsonData + '\n';

    // 发送数据包
    m_tcpClient->sendData(packet);

    qDebug() << "发送注册请求:" << jsonData;

    // 开表等 register_response：服务器不回包时按 registerTimeout 收尾（解锁页面 + 错误反馈），
    // 不然按钮会一直停在"正在注册..."上锁状态
    startResponseTimer("register");

    // 通知 UI 进入等待态（页面上锁 + "正在注册..."加点动画），
    // 与 startLogin 发 loginWaiting、ModifyPwdAquird 发 modifyPwdWaiting 同一套规则
    emit registerWaiting();
}

void LoginBackend::onTcpConnected()
{
    qDebug() << "TCP连接已建立";
    
    // 连接成功后，发送登录请求
    sendLoginRequest();
}

void LoginBackend::onTcpDisconnected()
{
    qDebug() << "TCP连接已断开";
    // 注意：这里不做自动重连。断线后"要不要重连、按哪个功能重连"由
    // MainBackend::onTcpDisconnected 统一决策（它会恢复功能标记再重连），
    // 本层自己重连的话：① 登录断线会双重重连；② 重连成功后标记已被清空，
    // 会掉进 else 兜底拿残留凭证发登录包
}

void LoginBackend::onTcpDataReceived(const QByteArray& data)
{
    qDebug() << "收到服务器数据:" << data;
    
    // 解析JSON响应
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    
    if (error.error != QJsonParseError::NoError) {
        qDebug() << "JSON解析失败:" << error.errorString();
        emit loginFailed();
        return;
    }
    
    QJsonObject response = doc.object();
    QString type = response["type"].toString();
    
    if (type == "login_response") {
        // 收到本次请求的响应：停掉"服务器响应超时"表（四个分支各自停自己那一张，
        // 收到别的流程的包不该把当前等待的表停掉）
        stopResponseTimer();
        bool success = response["success"].toBool();
        if (success) {
            m_loginCompleted = true;
            emit loginSuccess(m_accountId);
        } else {
            m_loginCompleted = true;
            qDebug() << "登录失败:" << response["message"].toString();
            emit loginFailed();
        }
    } else if (type == "modify_password_response") {
        // 修改密码响应：服务器成功/失败只发对应信号，UI 据此提示并切页
        stopResponseTimer();
        if (response["success"].toBool()) {
            qDebug() << "修改密码成功";
            emit modifyPwdSuccess();
        } else {
            qDebug() << "修改密码失败:" << response["message"].toString();
            emit modifyPwdFailed();
        }
    }
    else if (type == "connect_for_register_response") {
        // 注册取号响应（注册页显示 → 发 connect_for_register 后服务器回的账号下发包）：
        // 成功时账号字段随 connectForRegisterSuccess 带回 UI 填账号框，失败切红色指示器
        stopResponseTimer();
        if (response["success"].toBool()) {
            qDebug() << "注册取号成功";
            auto account = response["accountId"].toString();
            emit connectForRegisterSuccess(account);
        } else {
            qDebug() << "注册取号失败:" << response["message"].toString();
            emit connectForRegisterFailed();
        }
    }
    else if (type == "register_response") {
        // 注册提交响应：与修改密码同一套规则，成功/失败只发对应信号，UI 据此提示并切页
        stopResponseTimer();
        if (response["success"].toBool()) {
            qDebug() << "注册成功";
            emit registerSuccess();
        } else {
            qDebug() << "注册失败:" << response["message"].toString();
            emit registerFailed();
        }
    }
}

void LoginBackend::onTcpError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error);
    qDebug() << "Socket错误:" << m_tcpClient->errorString();
    // 连接已经废了，这次请求不可能再有响应：先停表，否则 10 秒后还会蹦出一次
    // "登录超时"，把刚提示的"连不上服务器"覆盖掉
    stopResponseTimer();
    // 网络层错误（连接被拒/断网等）不能发 loginFailed：
    // 那会让 UI 显示"账号或密码错误"，用户会误以为密码输错了
    // （典型场景：服务器开着机但后端进程没跑，连接直接被拒）。
    // 单发 loginNetworkError，UI 提示"连不上服务器"
    emit loginNetworkError(m_tcpClient->errorString());
}

void LoginBackend::onTcpConnectionTimeout()
{
    qDebug() << "登录连接超时（30秒）";
    emit loginTimeout();
}

void LoginBackend::onModifyPwdError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error);
    qDebug() << "修改密码连接出错:" << m_tcpClient->errorString();
    // 同登录：连接废了就不会再有响应，停表防止稍后误报 modifyPwdTimeout
    stopResponseTimer();
    // 与登录的 onTcpError 同一套规则：网络层连不上不是"服务器拒绝"，
    // 不能发 modifyPwdFailed（那会让忘记密码页提示"修改失败，请确认账号"，
    // 用户会以为账号错了，反复重试）。单发网络错误，UI 提示"连不上服务器"
    emit modifyPwdNetworkError(m_tcpClient->errorString());
}

void LoginBackend::onModifyPwdTimeout()
{
    qDebug() << "修改密码连接超时（30秒）";
    emit modifyPwdTimeout();
}

// 注册提交阶段的 TCP 错误：照 onModifyPwdError 的规则，
// 网络层连不上要单发 registerNetworkError，不能发 registerFailed
// （后者是"服务器明确拒绝"，UI 会提示账号已存在之类，用户会以为信息填错了）
void LoginBackend::onRegisterError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error);
    qDebug() << "注册连接出错:" << m_tcpClient->errorString();
    // 同登录：连接废了就不会再有响应，停表防止稍后误报 registerTimeout
    stopResponseTimer();
    emit registerNetworkError(m_tcpClient->errorString());
}

// 注册提交阶段的连接超时：照 onModifyPwdTimeout，发 registerTimeout
void LoginBackend::onRegisterTimeout()
{
    qDebug() << "注册连接超时（30秒）";
    emit registerTimeout();
}

// ===== "服务器响应超时"计时器（与 TcpClient 的连接超时是两回事）=====
// 分工：TcpClient 的 kConnectTimeoutMs 管"TCP 连不连得上"（30 秒，连上就停）；
//      这里管"连上了、请求也发了，服务器什么时候回"（kResponseTimeoutMs）。
// 之所以一个计时器就够：同一时刻只跑一个流程（登录/改密/取号/注册四选一，
// 由 MainBackend 的功能标记串起来），不会有两个请求同时等响应抢这张表
void LoginBackend::startResponseTimer(const QString& requestType)
{
    m_pendingRequest = requestType;   // 记下在等谁，超时后据此决定发哪个超时信号
    m_responseTimeoutTimer->start(kResponseTimeoutMs);
}

void LoginBackend::stopResponseTimer()
{
    m_responseTimeoutTimer->stop();
    m_pendingRequest.clear();
}

// 超时到点：按"当前在等哪个请求"发对应流程的超时信号，各页自己提示
// （登录页/忘记密码页/注册页文案不同，所以四个信号必须分开，不能合成一个）
void LoginBackend::onResponseTimeout()
{
    qDebug() << "服务器响应超时，未收到响应:" << m_pendingRequest;
    if (m_pendingRequest == "login") {
        emit loginTimeout();
    } else if (m_pendingRequest == "modify_password") {
        emit modifyPwdTimeout();
    } else if (m_pendingRequest == "connect_for_register") {
        emit connectForRegisterTimeout();
    } else if (m_pendingRequest == "register") {
        emit registerTimeout();
    }
    m_pendingRequest.clear();
}

