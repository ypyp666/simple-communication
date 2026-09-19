#include "MainBackend.h"
#include <QDebug>
#include <QJsonObject>
#include <QJsonDocument>
#include <qstringview.h>


bool MainBackend::s_loggedIn = false;  // 全局登录状态的静态定义

MainBackend::MainBackend(QObject* parent)
    : QObject(parent)
{
    // ===== 数据库后台线程初始化 =====
    // 注册跨线程信号槽需要传递的自定义类型（不注册，QueuedConnection 无法传递）
    qRegisterMetaType<MessageInfo>();//告诉Qt，MessageInfo是一个自定义类型，需要注册一下
    qRegisterMetaType<QList<MessageInfo>>();//告诉Qt，QList<MessageInfo>是一个自定义类型，需要注册一下

    // 1. 创建后台数据库线程
    m_dbThread = new QThread(this);
    m_dbThread->setObjectName("DbWorkerThread");

    // 2. 把 DatabaseManager 单例整个搬到后台线程"住下"
    //    moveToThread 必须发生在 connect 之前！
    //    之后它的所有槽函数都会在后台线程执行，数据库连接也都在后台线程创建，
    //    主线程只管发请求信号，DB 再慢也不卡 UI
    m_databaseManager = &DatabaseManager::instance();
    m_databaseManager->moveToThread(m_dbThread);

    // 3. 启动线程（started 事件循环跑起来后，排队过来的请求才会被处理）
    m_dbThread->start();

    // 4. 请求信号（主线程）→ DatabaseManager 槽（后台线程）：跨线程自动 QueuedConnection
    connect(this, &MainBackend::dbSaveMessageRequested,
            m_databaseManager, &DatabaseManager::saveMessage);
    connect(this, &MainBackend::dbSetAccountRequested,
            m_databaseManager, &DatabaseManager::setCurrentAccountId);
    connect(this, &MainBackend::dbUpdateMessageIDRequested,
            m_databaseManager, &DatabaseManager::updateMessageId);
    // 分页读消息请求：contactId+page 排队到后台 DB 线程执行（QSqlDatabase 不能跨线程直调）
    connect(this, &MainBackend::dbLoadMessagesPageRequested,
            m_databaseManager, &DatabaseManager::loadMessagesPage);


    // 5. 结果信号（后台线程发出）→ 本类槽（主线程）：跨线程自动 QueuedConnection
    connect(m_databaseManager, &DatabaseManager::messageSaved,
            this, &MainBackend::onDbMessageSaved);
    // 分页查询结果：后台线程查完排队回主线程，转发给 UI
    connect(m_databaseManager, &DatabaseManager::messagesPageLoaded,
            this, &MainBackend::onDbMessagesPageLoaded);
    connect(m_databaseManager, &DatabaseManager::databaseInitialized,
                    this, &MainBackend::onDbInitialized);
    connect(m_databaseManager, &DatabaseManager::messageIdUpdated,
            this, [=](bool success){
                if(!success) {
                     qDebug() << "更新消息ID失败";
                }
            });
    // 创建登录后端和聊天后端
    m_tcpClient = new TcpClient(this);
    m_loginBackend = new LoginBackend(this,m_tcpClient);
    m_chatBackend = new ChatBackend(this, m_tcpClient);
    
    connect(m_tcpClient, &TcpClient::dataReceived, this, &MainBackend::JsonParsing);
    // TcpClient 共享信号统一路由到主后端，再分发给需要的后端（各后端不再直连）
    connect(m_tcpClient, &TcpClient::connected, this, &MainBackend::onTcpConnected);
    connect(m_tcpClient, &TcpClient::disconnected, this, &MainBackend::onTcpDisconnected);
    connect(m_tcpClient, &TcpClient::errorOccurred, this, &MainBackend::onTcpError);
    connect(m_tcpClient, &TcpClient::connectionTimeout, this, &MainBackend::onTcpConnectionTimeout);
    // 转发 ChatBackend 的发送结果信号（成功/失败都带tempId，UI据此切换气泡状态）
    connect(m_chatBackend, &ChatBackend::sendSuccess,
            this, &MainBackend::messageSendSuccess);
    connect(m_chatBackend, &ChatBackend::sendFailed,
            this, &MainBackend::messageSendFailed);
    // 注册登录后端（登录/修改密码）的信号连接，抽成独立方法，避免构造函数越堆越长
    setupLoginConnections();
    // 转发 ChatBackend 的聊天相关信号
    connect(m_chatBackend, &ChatBackend::contactsLoaded,
            this, &MainBackend::contactsLoaded);
    connect(m_chatBackend, &ChatBackend::newMessageReceived,
            this, &MainBackend::onMessageReceived);
    connect(m_chatBackend, &ChatBackend::messageReceiveFailed,
            this, &MainBackend::onMessageReceiveFailed);
}

MainBackend::~MainBackend()
{
    // 退出后台数据库线程，防止程序退出时线程还在跑
    if (m_dbThread) {
        m_dbThread->quit();      // 通知事件循环退出
        m_dbThread->wait(3000);  // 等待线程真正结束
    }
    // 析构时自动清理子对象（通过 Qt 的父子机制）
}

// ===== 登录后端信号连接（登录 / 修改密码）=====
// 统一规则，后续接入注册、登出照此办理：
//   1) 纯转发（无副作用）→ 直接"信号对信号"连接，一行即可，不需要 lambda
//   2) 有副作用的 → 用 lambda，其中"清功能标记"这步复用 resetFeature()
void MainBackend::setupLoginConnections()
{
    // --- 登录 ---
    connect(m_loginBackend, &LoginBackend::loginSuccess,
            this, [this](){
                s_loggedIn = true;                   // 更新全局登录状态
                resetFeature();                      // 登录流程结束，清掉功能标记
                m_chatBackend->setUserId(m_userid);  // 同步登录账号给聊天后端（拉取重试需要）
                emit loginSuccess(m_userid);
                // 登录成功即拉取服务器缓存中未确认的消息（首次登录：TCP连上→登录成功→拉取）
                m_chatBackend->sendPullRequest(m_userid);
            });
    connect(m_loginBackend, &LoginBackend::loginFailed,
            this, [this](){
                s_loggedIn = false;                  // 登录失败，重置全局登录状态
                resetFeature();
                emit loginFailed();
            });
    // 网络层连不上（服务器没跑/断网）：与"密码错误"分开，UI 提示语不同，但同样是流程终态
    connect(m_loginBackend, &LoginBackend::loginNetworkError,
            this, [this](const QString& reason){
                s_loggedIn = false;
                resetFeature();
                emit loginNetworkError(reason);
            });
    // 纯转发：等待信号没有任何副作用，信号对信号连接即可
    connect(m_loginBackend, &LoginBackend::loginWaiting, this, &MainBackend::loginWaiting);
    connect(m_loginBackend, &LoginBackend::loginTimeout,
            this, [this](){
                resetFeature();
                emit loginTimeout();
            });

    // --- 修改密码（忘记密码页提交后）---
    // 修改密码流程无论成败都是终态，转发前先清掉功能标记，
    // 防止残留值把后续 TCP 信号误路由到修改密码
    connect(m_loginBackend, &LoginBackend::modifyPwdSuccess,
            this, [this](){
                resetFeature();
                emit modifyPwdSuccess();
            });
    connect(m_loginBackend, &LoginBackend::modifyPwdFailed,
            this, [this](){
                resetFeature();
                emit modifyPwdFailed();
            });
    // 修改密码时网络层连不上：与"服务器拒绝"（modifyPwdFailed）分开，提示语不同，同样是终态
    connect(m_loginBackend, &LoginBackend::modifyPwdNetworkError,
            this, [this](const QString& reason){
                resetFeature();
                emit modifyPwdNetworkError(reason);
            });
    connect(m_loginBackend, &LoginBackend::modifyPwdTimeout,
            this, [this](){
                resetFeature();
                emit modifyPwdTimeout();
            });
    connect(m_loginBackend, &LoginBackend::modifyPwdWaiting, this, &MainBackend::modifyPwdWaiting);

    // --- 注册（注册页）---
    // 与修改密码同一套规则：注册无论成败都是终态，转发前先清功能标记
    // 取号成功：连接已经建好，后面就进入"提交注册"阶段了，功能标记随之从
    // ConnectForRegister 切到 Register——否则提交阶段这条连接上的错误/超时
    // 会被当成取号阶段的问题（报 connectForRegisterFailed 而不是 registerNetworkError）
    connect(m_loginBackend, &LoginBackend::connectForRegisterSuccess,
            this, [this](const QString& account){
                m_currentFeature = LoginFeature::Register;
                emit connectForRegisterSuccess(account);
            });
    // 取号（连接期）失败/超时同样是终态：转发前先清功能标记；
    // 等待信号无副作用，信号对信号直连即可
    connect(m_loginBackend, &LoginBackend::connectForRegisterFailed,
            this, [this](){
                resetFeature();
                emit connectForRegisterFailed();
            });
    connect(m_loginBackend, &LoginBackend::connectForRegisterTimeout,
            this, [this](){
                resetFeature();
                emit connectForRegisterTimeout();
            });
    connect(m_loginBackend, &LoginBackend::connectForRegisterWaiting, this, &MainBackend::connectForRegisterWaiting);
    connect(m_loginBackend, &LoginBackend::registerSuccess,
            this, [this](){
                resetFeature();
                emit registerSuccess();
            });
    connect(m_loginBackend, &LoginBackend::registerFailed,
            this, [this](){
                resetFeature();
                emit registerFailed();
            });
    // 注册时网络层连不上：与"服务器拒绝"（registerFailed）分开，提示语不同，同样是终态
    connect(m_loginBackend, &LoginBackend::registerNetworkError,
            this, [this](const QString& reason){
                resetFeature();
                emit registerNetworkError(reason);
            });
    connect(m_loginBackend, &LoginBackend::registerTimeout,
            this, [this](){
                resetFeature();
                emit registerTimeout();
            });
    // 纯转发：等待信号没有任何副作用，信号对信号连接即可
    connect(m_loginBackend, &LoginBackend::registerWaiting, this, &MainBackend::registerWaiting);
}

// 未登录态流程（登录 / 注册 / 修改密码）无论成败都回到"空闲"态：清掉功能标记，
// 避免残留值让 onTcpConnected/onTcpError/onTcpConnectionTimeout 误路由到上一次的功能
void MainBackend::resetFeature()
{
    m_currentFeature = LoginFeature::None;
}

ChatBackend* MainBackend::getChatBackend()
{
    return m_chatBackend;
}

void MainBackend::login(const QString& userId, const QString& password)
{
    m_userid = userId;
    m_password = password;
    s_loggedIn = false;  // 新的登录会话开始，重置全局登录状态
    m_currentFeature = LoginFeature::Login;  // 标记当前连接服务于"登录"
    // 转发调用到 LoginBackend
    m_loginBackend->startLogin(userId, password);
}

void MainBackend::modifyPwd(const QString& account, const QString& newPassword)
{
    s_loggedIn = false;  // 修改密码在未登录态进行
    // 转发到 LoginBackend：保存参数并发起连接，实际请求等 connected 后由路由发。
    // 注意顺序：功能标记必须在连接发起【之后】再设——connectToServer 内部 abort 旧连接时
    // 可能同步触发 disconnected → onTcpDisconnected → resetFeature()，
    // 若标记设在前面会被这一下清掉，等 connected 到达时就掉进 else 兜底被当成登录了。
    // 网络信号（connected/error）都走事件循环，最早也是本函数返回后才派发，这里后设标记绝对安全
    m_loginBackend->ModifyPwdAquird(account, newPassword);
    m_currentFeature = LoginFeature::ModifyPassword; // 标记当前连接服务于"修改密码"
}

void MainBackend::sendMessage(const MessageInfo& message)
{
    OutgoingMessage out;
    out.type = "repost";               // 告诉服务器这是一个转发消息
    out.accountId = message.accountId; // 当前登录账号ID
    out.senderId = message.senderId;
    out.targetId = message.targetId;
    out.content = message.content;
    out.sendTime = message.sendTime;
    out.tempId = message.id;
    emit sendWaiting();
    m_chatBackend->startSendMessage(message.contactId, out);

    saveMessage(message);  // 异步保存到数据库（后台线程执行，不阻塞UI）
}

// 统一的"重试"入口（胶水层）：指示器（MessageStatusIndicator）点击重试时携带功能枚举上来，
// 这里按枚举把请求路由到对应的后端
void MainBackend::onRetryRequested(LoginFeature feature, const QString& id, const MessageInfo& message)
{
    Q_UNUSED(id);
    switch (feature) {
    case LoginFeature::MessageSend:
        // 聊天消息重发：交给 ChatBackend 的发送链路（与首次发送同一条路）
        sendMessage(message);
        break;
    case LoginFeature::ConnectForRegister:
        // 注册页账号行（取号连接阶段）失败重试：重新发起 TCP 连接向服务器取号（交给 LoginBackend）
        prepareRegisterConnection();
        break;
    default:
        // None / Login / ModifyPassword 不走指示器重试，忽略
        break;
    }
}

// ===== 数据库异步接口 =====
// 只发请求信号，实际执行在后台数据库线程，主线程不等待、不阻塞

// 异步保存消息
void MainBackend::saveMessage(const MessageInfo& message)
{
    emit dbSaveMessageRequested(message);
}

// 异步切换当前账号（后台线程打开对应账号的消息库）
void MainBackend::setCurrentAccountId(const QString& accountId)
{
    emit dbSetAccountRequested(accountId);
}

// 后台数据库线程回传：消息保存结果（在主线程执行）
void MainBackend::onDbMessageSaved(bool success)
{
    qDebug() << "数据库保存消息结果:" << success;
    emit messageSaved(success);
}

// 后台数据库线程回传：数据库初始化结果（在主线程执行）
void MainBackend::onDbInitialized(bool success)
{
    qDebug() << "数据库初始化结果:" << success;
    emit databaseInitialized(success);
}

// 收到对方消息：回ACK（胶水转发） + 存库 + 转发给UI
void MainBackend::onMessageReceived(const MessageInfo& message)
{
    // 1. 回ACK的业务逻辑在 ChatBackend，主后端只做胶水转发
    m_chatBackend->sendReceiveAck(message.id, true);

    // 2. 存库由 ChatWindow 统一处理（按已读状态覆盖），主后端不再重复保存
    // 3. 转发给UI显示
    emit messageReceived(message);
}

// 接收失败：发拉取请求（业务在 ChatBackend）+ 转发给UI
void MainBackend::onMessageReceiveFailed(const QString& serverId)
{
    // 1. 拉取请求的业务逻辑在 ChatBackend，主后端只做胶水转发
    m_chatBackend->sendPullRequest(m_userid);  // 拉取发给自己的消息

    // 2. 转发给UI（如果需要提示用户）
    emit messageReceiveFailed(serverId);
}

void MainBackend::onDbMessageIdUpdated(const QString& contactId, const QString& oldId, const QString& newId)
{
    emit dbUpdateMessageIDRequested(contactId, oldId, newId);
}

void MainBackend::loadContacts()
{
    m_chatBackend->loadContacts();
}

// 分页加载本地聊天记录：只发请求信号，实际查询在后台 DB 线程执行，
// 结果从 onDbMessagesPageLoaded 转发回 UI（不能在主线程直调 DatabaseManager 的查询）
void MainBackend::loadMessages(const QString& contactId, int page)
{
    emit dbLoadMessagesPageRequested(contactId, page, 50);  // 单页固定50条（项目约定的分页上限）
}

// 后台DB线程分页查询完成（主线程）：原样转发给 UI。
// UI 拿 contactId 比对"还是不是当前联系人"（快速切换联系人时慢一拍的结果不能盖到新联系人上），
// 拿 page 区分首屏（清空重放）还是翻历史（插到顶部）
void MainBackend::onDbMessagesPageLoaded(const QString& contactId, int page,
                                         const QList<MessageInfo>& messages, bool hasMore)
{
    emit messagesPageLoaded(contactId, page, messages, hasMore);
}

void MainBackend::sendFile(const QString& contactId, const QString& filePath)
{
    m_chatBackend->sendFile(contactId, filePath);
}

void MainBackend::saveInputContent(const QString& contactId, const QString& content)
{
    m_chatBackend->saveInputContent(contactId, content);
}

QString MainBackend::getInputContent(const QString& contactId)
{
    return m_chatBackend->getInputContent(contactId);
}

void MainBackend::clearInputContent(const QString& contactId)
{
    m_chatBackend->clearInputContent(contactId);
}

// 断开会话：直接关闭当前 TCP 连接
void MainBackend::disconnectSession()
{
    if (m_tcpClient) {
        m_tcpClient->disconnectFromServer();
    }
}

// ===== TcpClient 共享信号统一路由（胶水层） =====
// TCP连接成功：按"登录后/修改密码/登录"三种场景分发
void MainBackend::onTcpConnected()
{
    if (s_loggedIn) {
        // 已登录过（断线重连场景）：无需重新登录，直接拉取服务器缓存的未确认消息
        m_chatBackend->sendPullRequest(m_userid);
    } else if (m_currentFeature == LoginFeature::ModifyPassword) {
        // 修改密码场景：连接建立后发送修改密码请求（不能登录，也不能拉取）
        m_loginBackend->sendModifyPwdRequest();
    } else if (m_currentFeature == LoginFeature::ConnectForRegister) {
        // 注册取号场景：连接一建立就由登录后端把账号下发出去（emit connectForRegisterSuccess），
        // 这里同样不能掉进下面的登录兜底——否则连上就发一个空账号的畸形登录包
        m_loginBackend->sendconnectForRegister();
    } else if (m_currentFeature == LoginFeature::Register) {
        // 注册提交场景（取号成功后的复用连接）：这里什么都不发——用户还没填完密码，
        // 注册请求要等点"注册"后才由 onSubmitClicked → registerUser 发出。
        // 若漏掉这个分支会掉进下面的登录兜底，连上就发一个空账号的畸形登录包
    } else {
        // 登录场景：交给登录后端发登录请求，登录成功后再拉取
        m_loginBackend->onTcpConnected();
    }
}

// TCP断开：所有需要感知断线的后端都通知到
void MainBackend::onTcpDisconnected()
{
    // 先记住断线前的功能再清标记：未登录态的断线要自动重连，
    // 重连成功的 connected 事件必须按原功能路由——不恢复标记的话会掉进
    // else 兜底被当成登录，拿上次残留的账号密码发一个登录包
    LoginFeature feature = m_currentFeature;
    resetFeature();
    if (!s_loggedIn) {
        // 按断线前的功能决定要不要重连、以及重连后走哪条路。
        // 只在确实有流程在跑（feature != None）时才重连：
        // 登录页闲置时断网就不该拿残留凭证偷偷发登录包
        switch (feature) {
        case LoginFeature::Login:
            // 登录中断线：重连成功后走登录兜底重发登录请求
            m_currentFeature = LoginFeature::Login;
            m_tcpClient->reconnect();
            break;
        case LoginFeature::ConnectForRegister:
            // 取号中断线：重连成功后重新发 connect_for_register 取号
            m_currentFeature = LoginFeature::ConnectForRegister;
            m_tcpClient->reconnect();
            break;
        case LoginFeature::Register:
            // 提交阶段断线：重连后停在 Register 分支，等用户重新点"注册"
            m_currentFeature = LoginFeature::Register;
            m_tcpClient->reconnect();
            break;
        case LoginFeature::ModifyPassword:
            // 改密中断线：重连成功后重发修改密码请求
            m_currentFeature = LoginFeature::ModifyPassword;
            m_tcpClient->reconnect();
            break;
        default:
            break;  // None：没有流程在跑，不重连
        }
        return;
    }
    m_chatBackend->onTcpDisconnected();
    m_tcpClient->reconnect();
}

// TCP错误：按功能枚举分发（登录 / 修改密码 / 聊天三种后端各回各家）
void MainBackend::onTcpError(QAbstractSocket::SocketError error)
{
    if (s_loggedIn) {
        // 会话中途的错误（断线、服务器重启等）交给聊天后端，避免误弹登录失败
        m_chatBackend->onTcpError(error);
    } else if (m_currentFeature == LoginFeature::ModifyPassword) {
        // 修改密码流程中的错误 → 修改密码失败（不是登录失败）
        m_loginBackend->onModifyPwdError(error);
    } else if (m_currentFeature == LoginFeature::ConnectForRegister) {
        // 注册取号时的连接错误 → 账号行指示器变红可重试（不能报"登录失败"）
        m_loginBackend->onConnectForRegisterError(error);
    } else if (m_currentFeature == LoginFeature::Register) {
        // 注册提交阶段的错误 → 走注册提交流程（不能报"登录失败"）
        m_loginBackend->onRegisterError(error);
    } else {
        // 登录流程中的错误 → 登录失败
        m_loginBackend->onTcpError(error);
    }
}

// TCP连接超时：按功能枚举分发
void MainBackend::onTcpConnectionTimeout()
{
    if (s_loggedIn) {
        m_chatBackend->onTcpConnectionTimeout();
    } else if (m_currentFeature == LoginFeature::ModifyPassword) {
        m_loginBackend->onModifyPwdTimeout();
    } else if (m_currentFeature == LoginFeature::ConnectForRegister) {
        m_loginBackend->onConnectForRegisterTimeout();
    } else if (m_currentFeature == LoginFeature::Register) {
        m_loginBackend->onRegisterTimeout();
    } else {
        m_loginBackend->onTcpConnectionTimeout();
    }
}

// 注册第二段（提交）：填好密码后由 RegisterPage 的 registerAquiard 直接绑定到本槽，
// 不做"保存参数 + 重连"那套中间层——第一段（进页面）已经把 TCP 拉起来了，
// 这里只把账号/密码原样转给 LoginBackend 把提交请求发出去
void MainBackend::registerUser(const QString& username, const QString& password)
{
    m_loginBackend->sendRegisterRequest(username, password);
}

// 注册页显示时的取号连接：只连 TCP，不发提交请求，账号由服务器在连接建立后下发。
// 顺序同 modifyPwd：先发起连接，再设功能标记——connectToServer 内部 abort 旧连接时
// 可能同步触发 disconnected → resetFeature() 把标记清掉，设早了等 connected 到达时
// 就会掉进 else 兜底被当成登录（发一个空账号的登录请求）
void MainBackend::prepareRegisterConnection()
{
    s_loggedIn = false;
    m_loginBackend->connectForRegister();
    m_currentFeature = LoginFeature::ConnectForRegister;   // 标记这条连接服务于"注册取号"
}

void MainBackend::JsonParsing(const QByteArray packet)
{
    qDebug() << "收到服务器数据:" << packet;
    
    // 解析JSON响应
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(packet, &error);
    
    if (error.error != QJsonParseError::NoError) {
        qDebug() << "JSON解析失败:" << error.errorString();
       // emit loginFailed();
        return;
    }
    
    QJsonObject response = doc.object();
    QString type = response["type"].toString();
    if (type == "login_response") {
        m_loginBackend->onTcpDataReceived(packet);
    } else if (type == "modify_password_response") {
        // 修改密码响应（忘记密码页提交后），交给登录后端解析成 modifyPwd 结果信号
        m_loginBackend->onTcpDataReceived(packet);
    } else if (type == "connect_for_register_response") {
        // 注册取号响应（注册页显示后服务器回的账号下发包），交给登录后端解析成
        // connectForRegisterSuccess(account)（带服务器分配的账号）/ connectForRegisterFailed
        m_loginBackend->onTcpDataReceived(packet);
    } else if (type == "register_response") {
        // 注册提交响应（点"注册"后服务器回的结果包），交给登录后端解析成 register 结果信号
        m_loginBackend->onTcpDataReceived(packet);
    } else if (type == "repost_response") {
        m_chatBackend->onTcpDataReceived(packet);
    }
    else if (type == "repost") {
        m_chatBackend->onTcpRepost(packet);
    }
    else if (type == "pull_response") {
        // 服务器发完所有缓存消息后发来的拉取确认包，交给聊天后端处理（停止拉取定时器）
        m_chatBackend->onPullResponse(packet);
    }
}