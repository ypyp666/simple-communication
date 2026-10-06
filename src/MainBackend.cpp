#include "MainBackend.h"
#include <QDebug>
#include <QJsonObject>
#include <QJsonDocument>
#include <QTimer>
#include <qstringview.h>


bool MainBackend::s_loggedIn = false;  // 全局登录状态的静态定义

MainBackend::MainBackend(QObject* parent)
    : QObject(parent)
{
    // ===== 数据库后台线程初始化 =====
    // 注册跨线程信号槽需要传递的自定义类型（不注册，QueuedConnection 无法传递）
    qRegisterMetaType<MessageInfo>();//告诉Qt，MessageInfo是一个自定义类型，需要注册一下
    qRegisterMetaType<QList<MessageInfo>>();//告诉Qt，QList<MessageInfo>是一个自定义类型，需要注册一下
    // 联系人 / 会话列表也要跨线程回传（DB 结果信号带 QList<ContactInfo> / QList<ConversationInfo>），
    // 不注册 QueuedConnection 传不过去
    qRegisterMetaType<QList<ContactInfo>>();
    qRegisterMetaType<QList<ConversationInfo>>();
    // 写侧请求信号要跨线程传单个结构体（dbSaveContactRequested / dbSaveConversationRequested），
    // 单条也要注册
    qRegisterMetaType<ContactInfo>();
    qRegisterMetaType<ConversationInfo>();

    
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
    // 本地库直读请求：联系人 / 会话列表，同样排队到后台 DB 线程（两个函数不带参数，
    // 查完的结果由下面的结果信号带回来——返回值跨线程取不到）
    connect(this, &MainBackend::dbLoadContactsRequested,
            m_databaseManager, &DatabaseManager::loadContacts);
    connect(this, &MainBackend::dbLoadConversationsRequested,
            m_databaseManager, &DatabaseManager::loadConversations);
    // 本地库写入请求：会话 / 联系人 的增删改，同样排队到后台 DB 线程执行。
    // 注意 DB 这几个写函数没有结果信号，发出去就完事（要回执得先在 DatabaseManager 补信号）
    connect(this, &MainBackend::dbSaveConversationRequested,
            m_databaseManager, &DatabaseManager::saveConversation);
    connect(this, &MainBackend::dbUpdateConversationMessageRequested,
            m_databaseManager, &DatabaseManager::updateConversationMessage);
    connect(this, &MainBackend::dbRefreshConversationSnapshotRequested,
            m_databaseManager, &DatabaseManager::refreshConversationSnapshot);
    connect(this, &MainBackend::dbClearConversationUnreadRequested,
            m_databaseManager, &DatabaseManager::clearConversationUnread);
    connect(this, &MainBackend::dbDeleteConversationRequested,
            m_databaseManager, &DatabaseManager::deleteConversation);
    connect(this, &MainBackend::dbSaveContactRequested,
            m_databaseManager, &DatabaseManager::saveContact);
    connect(this, &MainBackend::dbDeleteContactRequested,
            m_databaseManager, &DatabaseManager::deleteContact);
    connect(this, &MainBackend::dbSetContactRemarkRequested,
            m_databaseManager, &DatabaseManager::setContactRemark);


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
    m_contactBackend = new ContactBackend(this, m_tcpClient);   // 通讯录后端（共用同一个 TcpClient 发好友请求）
    
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
    // 本地库读结果 → 主后端具名槽收口 → 上 UI。
    // 会话列表由主后端直接转发（业务后端不参与）；通讯录多一步：
    // 先转给 ContactBackend 排序，再把排好序的结果上到 UI。
    // DB 在后台线程，这里跨线程自动 QueuedConnection
    connect(m_databaseManager, &DatabaseManager::conversationsLoaded,
            this, &MainBackend::onDbConversationsLoaded);
    connect(m_databaseManager, &DatabaseManager::contactsLoaded,
            this, &MainBackend::onDbContactsLoaded);
    // 通讯录的"出口"在 ContactBackend：它排完序发 contactsLoaded，
    // 这里信号接信号，原样转成 MainBackend::contactsLoaded 给 UI（喂 ContactList）
    connect(m_contactBackend, &ContactBackend::contactsLoaded,
            this, &MainBackend::contactsLoaded);
    // 删好友有副作用（要动本地库），不能信号对信号直连，落到具名槽里：
    // 服务器确认成功 → 删本地库 + 重拉通讯录（本地这时才真删）；
    // 失败纯粹是通知 UI，没有副作用，直接转发
    connect(m_contactBackend, &ContactBackend::deleteFriendSuccess,
            this, &MainBackend::onFriendDeleteSuccess);
    connect(m_contactBackend, &ContactBackend::deleteFriendFailed,
            this, &MainBackend::friendDeleteFailed);
    // 改备注有副作用（要动本地库），同样不能信号对信号直连，落到具名槽：
    // 服务器确认成功 → 写本地库 + 上抛新值给详情页刷新；失败只上抛提示（本地不动）
    connect(m_contactBackend, &ContactBackend::setRemarkSuccess,
            this, &MainBackend::onSetRemarkSuccess);
    connect(m_contactBackend, &ContactBackend::setRemarkFailed,
            this, &MainBackend::onSetRemarkFailed);
    // 拉取服务器好友列表成功：有副作用（要写本地 contacts 表），落具名槽——
    // 服务器权威数据逐条覆盖本地，再重拉刷新列表（见 onPullServerContactsSuccess）
    connect(m_contactBackend, &ContactBackend::pullServerContactsSuccess,
            this, &MainBackend::onPullServerContactsSuccess);
    // 离线删除好友缓存（墓碑）：服务器下发的"我该删掉的联系人"要落到本地库，有副作用，落具名槽。
    // 槽里按"先删、回ACK、再重拉服务器列表"串行处理（见 onDeleteFriendCacheReceived）
    connect(m_contactBackend, &ContactBackend::deleteFriendCacheReceived,
            this, &MainBackend::onDeleteFriendCacheReceived);
    // 在线被删推送：同样是服务器侧的删除，落具名槽共用同一套本地删除逻辑
    connect(m_contactBackend, &ContactBackend::friendDeletedByPeer,
            this, &MainBackend::onFriendDeletedByPeer);
    // 同意好友申请成功：也要动本地库（把服务器回下来的新好友写进 contacts 表），同样落具名槽
    connect(m_contactBackend, &ContactBackend::acceptFriendRequestSuccess,
            this, &MainBackend::onAcceptFriendRequestSuccess);
    // 好友申请：服务器逐条下发，本地不存申请（要看列表就向服务器拉一次），
    // 所以没有任何副作用，信号对信号直接转发给 UI 即可
    connect(m_contactBackend, &ContactBackend::friendRequestReceived,
            this, &MainBackend::friendRequestReceived);
    // 一次拉取的首尾：同样纯转发（不落库、不动本地库），信号对信号接。
    // UI 靠这两条决定"收尾成空态"还是"显示失败文案"（注意不是把失败当空列表）
    connect(m_contactBackend, &ContactBackend::pullFriendRequestSuccess,
            this, &MainBackend::friendRequestsPulled);
    connect(m_contactBackend, &ContactBackend::pullFriendRequestFailed,
            this, &MainBackend::friendRequestsPullFailed);
    // 同意 / 拒绝失败：要把"是哪一条"补上（服务器回包不带 requestId），必须落具名槽
    connect(m_contactBackend, &ContactBackend::acceptFriendRequestFailed,
            this, &MainBackend::onFriendRequestAcceptFailed);
    connect(m_contactBackend, &ContactBackend::rejectFriendRequestSuccess,
            this, &MainBackend::onFriendRequestRejected);
    connect(m_contactBackend, &ContactBackend::rejectFriendRequestFailed,
            this, &MainBackend::onFriendRequestRejectFailed);
    // 搜索用户的结果：只是给搜索页看（搜到 ≠ 已是好友，本地不入表），没有任何副作用，纯转发
    connect(m_contactBackend, &ContactBackend::searchSuccess,
            this, &MainBackend::searchSuccess);
    connect(m_contactBackend, &ContactBackend::searchFailed,
            this, &MainBackend::searchFailed);
    // 发好友申请的结果：同样是纯状态（不落库、不回填列表），信号对信号直接转发给搜索页
    connect(m_contactBackend, &ContactBackend::sendFriendRequestSuccess,
            this, &MainBackend::sendFriendRequestSuccess);
    connect(m_contactBackend, &ContactBackend::sendFriendRequestFailed,
            this, &MainBackend::sendFriendRequestFailed);
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
                // 把当前账号分发给各功能后端：每个后端自己留一份，谁要用谁直接用
                m_loginBackend->setUserId(m_userid);   // 登录后端（登录/注册/改密流程用）
                m_chatBackend->setUserId(m_userid);    // 聊天后端（拉取重试需要）
                m_contactBackend->setUserId(m_userid); // 好友后端（好友请求要带申请人ID）
                emit loginSuccess(m_userid);
                // 登录成功 → 开启断线自愈 + 心跳检验（TcpClient 里每 3 秒一次的定时器）。
                // 自愈打开后，这次会话期间链路断了由 TcpClient 退避重连一直试到连上
                m_tcpClient->enableBackoff();
                m_tcpClient->StartExamine();
                // 登录成功即拉取服务器缓存中未确认的消息（首次登录：TCP连上→登录成功→拉取）
                m_chatBackend->sendPullRequest(m_userid);
                // 登录成功先拉"离线删除好友缓存"（墓碑），不直接拉好友列表：
                // 必须先按墓碑把本地该删的好友删掉，再由墓碑槽里串行触发 pullServerContacts，
                // 保证顺序是"先处理删、再处理拉"（否则先拉列表会把待删的好友又写回来）
                m_contactBackend->pullDeleteFriendCache();
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

    // --- 断线重连的补登录（重连成功后自动走，不经过用户）---
    connect(m_loginBackend, &LoginBackend::reconnectLoginSuccess,
            this, [this](){
                // 令牌过了，会话算真正回来：先把断线期间该补的补上（与登录成功同一套路），
                // 再把心跳重新开起来——退避期间它被停掉了，不重启这条链路就没人看着了
                qDebug() << "MainBackend: 会话已恢复，开始补拉";
                m_chatBackend->sendPullRequest(m_userid);
                m_contactBackend->pullDeleteFriendCache();
                m_tcpClient->StartExamine();
            });
    connect(m_loginBackend, &LoginBackend::reconnectLoginFailed,
            this, [this](){
                // 令牌被拒（过期 / 服务器重启后令牌表空了）：会话救不回来。
                // 清干净状态、喊一声，由 main.cpp 把用户送回登录页重新登录。
                // 队列延迟：此刻还在"收包"的调用栈里，直接断开会重入 socket 的事件处理
                qDebug() << "MainBackend: 重连登录被拒，请用户重新登录";
                QTimer::singleShot(0, this, &MainBackend::kickToLogin);
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
    m_kickedToLogin = false;  // 新的一轮登录开始，清掉"被踢"标记
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

// 拉取会话列表（喂聊天页左侧 MessageList）：这里只"单纯发"，把请求丢给后台 DB 线程；
// DB 查完由 DatabaseManager::conversationsLoaded 回信号 → onDbConversationsLoaded 收口后上到 UI
//
// 合并节流：发消息 / 收消息 / 切会话这些动作，一轮里可能连着调好几次本函数
// （比如切会话要"清未读"和"补会话"各拉一次），而每拉一次 UI 就要把整个列表清空重建一遍。
// 所以这里不立刻发请求，只立一个"已预约"标记，等本轮事件循环转回来（0ms 定时器）再发一趟。
// 有标记就直接返回，重复的调用被合并掉——省的是"整表重建 UI 列表"的次数，不是数据库查询
void MainBackend::loadConversations()
{
    if (m_conversationsLoadPending) {
        return;   // 本轮已经约过了，这次并进去
    }
    m_conversationsLoadPending = true;
    QTimer::singleShot(0, this, &MainBackend::onCoalescedLoadConversations);
}

// 合并后真正去拉：清掉标记再发请求。清标记必须在这里（而不是发请求之后），
// 否则这期间新来的 loadConversations 会被误当成"已预约"而丢掉，UI 就再也刷不新了
void MainBackend::onCoalescedLoadConversations()
{
    m_conversationsLoadPending = false;
    emit dbLoadConversationsRequested();
}

// 拉取通讯录（喂联系人页 ContactList）：同样只发请求；
// DB 查完由 DatabaseManager::contactsLoaded 回信号 → onDbContactsLoaded 收口后上到 UI
// 与会话列表彻底分开：两个列表的数据源、信号、类型都不一样，互不干扰
void MainBackend::loadContacts()
{
    emit dbLoadContactsRequested();
}

// === 好友申请（"新朋友"页）===
// 拉申请列表：只发请求。服务器不下一整个数组，而是把每条申请当成独立的 friend_request
// 包逐条送回来（参考拉取离线消息）；收到的每一条都由 ContactBackend::friendRequestReceived
// → 本类同名信号 → UI 侧那个内存列表累积。本地【不落库】，所以这里没有任何 DB 操作
void MainBackend::pullFriendRequests()
{
    m_contactBackend->pullFriendRequest(m_userid);
}

// 搜索用户（网络请求）：把关键字原样交给好友后端发 search_request。
// 结果不定时回来，由 ContactBackend::searchSuccess / searchFailed 经本类同名信号上到搜索页
void MainBackend::searchUser(const QString& keyword)
{
    m_contactBackend->sendSearchRequest(keyword);
}

// 加好友（网络请求）：搜索页"申请加好友"小窗点发送走这里。
// 与好友申请同一条线——本地【不落库】，等对方同意后服务器才会把新好友资料回下来
void MainBackend::sendFriendRequest(const QString& userId, const QString& message)
{
    m_contactBackend->sendFriendRequest(userId, message);
}

// 同意某条申请：先把 requestId 记下来——服务器回包【不带】这个 ID，
// 而 UI 要靠它把那条从内存列表里摘掉（见 onAcceptFriendRequestSuccess / onFriendRequestAcceptFailed）
void MainBackend::acceptFriendRequest(const QString& requestId)
{
    m_pendingAcceptRequestId = requestId;
    m_contactBackend->acceptFriendRequest(requestId);
}

// 拒绝某条申请：与同意同一套路，只是换了个请求 type
void MainBackend::rejectFriendRequest(const QString& requestId)
{
    m_pendingRejectRequestId = requestId;
    m_contactBackend->rejectFriendRequest(requestId);
}

// 删除好友（网络请求）：只是把"删"这件事请给服务器，本地一动不动。
// 服务器回 delete_friend_response 且 success=true 时，才由 onFriendDeleteSuccess 真正删本地
void MainBackend::deleteFriend(const QString& contactId)
{
    m_contactBackend->deleteFriend(contactId);
}

// 会话 / 联系人 的增删改：与上面的读请求一样只"单纯发"——把 SQL 派给后台 DB 线程，主线程不阻塞。
// 注意这几个写函数没有结果信号，写完不会自动刷新列表；UI 要在写完后自己再调一次
// loadConversations() / loadContacts() 才能看到最新数据
void MainBackend::saveConversation(const ConversationInfo& conversation)
{
    emit dbSaveConversationRequested(conversation);
}

// 打开会话时刷新会话快照：最后消息 / 时间按 messages 里最新一条重算，一条 SQL 在 DB 线程算完，
// UI 侧不用"先查 messages 再回写 conversations"跑两趟
void MainBackend::refreshConversationSnapshot(const QString& contactId)
{
    emit dbRefreshConversationSnapshotRequested(contactId);
}

// 收发消息后更新会话快照（最后消息 / 时间 / 未读），供左侧列表显示与排序
void MainBackend::updateConversationMessage(const QString& contactId, const QString& lastMessage,
                                            const QDateTime& lastTime, bool increaseUnread)
{
    emit dbUpdateConversationMessageRequested(contactId, lastMessage, lastTime, increaseUnread);
}

// 打开会话：未读清零
void MainBackend::clearConversationUnread(const QString& contactId)
{
    emit dbClearConversationUnreadRequested(contactId);
}

void MainBackend::deleteConversation(const QString& conversationId)
{
    emit dbDeleteConversationRequested(conversationId);
}

void MainBackend::saveContact(const ContactInfo& contact)
{
    emit dbSaveContactRequested(contact);
}

void MainBackend::deleteContact(const QString& contactId)
{
    emit dbDeleteContactRequested(contactId);
}

// 改好友备注（网络请求）：只是把"改备注"这件事请给服务器，本地一动不动。
// 服务器回 set_remark_response 且 success=true 时，才由 onSetRemarkSuccess 真正改本地。
// "谁发谁记"：把 (contactId, remark) 记下来——服务器回包只保证带回 targetId，
// 不保证回显 remark，而写库/刷新显示都需要"改成什么"
void MainBackend::setContactRemark(const QString& contactId, const QString& remark)
{
    m_pendingRemarkContactId = contactId;
    m_pendingRemark = remark;
    m_contactBackend->sendSetRemarkRequest(contactId, remark);
}

// 服务器确认改备注成功（主线程）：本地这时才真改。
// 顺序：先写本地库（把新备注落到 contacts/messages 表），再把改后的值上抛给详情页刷新显示。
// 回包若没带 targetId（服务器只回 success），用发出时记下的那个兜底。
// 同一时刻只可能有一条在飞（点一次保存才发一次请求），pending 不会积压
void MainBackend::onSetRemarkSuccess(const QString& targetId)
{
    const QString contactId = targetId.isEmpty() ? m_pendingRemarkContactId : targetId;
    emit dbSetContactRemarkRequested(contactId, m_pendingRemark);  // 写本地库
    loadContacts();                                                // 重拉通讯录：让内存里的联系人资料（含备注）跟上
    emit setContactRemarkSuccess(contactId, m_pendingRemark);      // 通知详情页把旧值刷成新值
    m_pendingRemarkContactId.clear();
    m_pendingRemark.clear();
}

// 改备注失败：回包不带 remark（也不一定带 targetId），把发出时记下的补上再上抛，
// UI 才知道改的是哪位好友。本地没有任何副作用要处理（备注不动，还是改之前的值）
void MainBackend::onSetRemarkFailed(const QString& targetId, const QString& message)
{
    const QString contactId = targetId.isEmpty() ? m_pendingRemarkContactId : targetId;
    emit setContactRemarkFailed(contactId, message);
    m_pendingRemarkContactId.clear();
    m_pendingRemark.clear();
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

// 后台DB线程查完会话列表（主线程）：原样转发给 UI（喂 ChatWindow 左侧 MessageList）。
// 与消息侧 onDbMessagesPageLoaded 同一套路——本地库读的唯一出口是主后端，业务后端不参与
void MainBackend::onDbConversationsLoaded(const QList<ConversationInfo>& conversations)
{
    emit conversationsLoaded(conversations);
}

// 后台DB线程查完通讯录（主线程）：转给 ContactBackend 排序并留一份 m_contacts，
// 排好序的结果由 ContactBackend::contactsLoaded → 本类同名信号上到 UI（喂联系人页 ContactList）。
// 与会话列表互不干扰（会话列表没有排序规则，主后端直接转发）
void MainBackend::onDbContactsLoaded(const QList<ContactInfo>& contacts)
{
    m_contactBackend->onDbContactsLoaded(contacts);
}

// 服务器确认删好友成功（主线程）：本地这时才真删。
// 顺序不能反——先发删库请求、再发重拉请求，两条都是排队到同一个 DB 线程的 QueuedConnection，
// 按发出顺序执行，所以查回来的一定是删干净之后的列表
void MainBackend::onFriendDeleteSuccess(const QString& targetId)
{
    emit dbDeleteContactRequested(targetId);  // 删本地库
    loadContacts();                           // 重拉通讯录 → ContactList 自动刷新
    emit friendDeleted(targetId);             // 通知 UI：主界面接去删掉该好友的会话（见 MainWindow ⑩）
}

// 服务器侧的"删好友"落到本地：离线墓碑与在线被删推送共用这一份逻辑，
// 保证两条路径的本地行为完全一致——逐条删 contacts 表 + 上抛 friendDeleted 让 UI 删对应会话。
// 注意 contacts 与 conversations 必须一起删，只删联系人会留下"好友没了、会话还在"的孤儿会话
void MainBackend::applyServerSideFriendDeletion(const QStringList& contactIds)
{
    for (const QString& contactId : contactIds) {
        emit dbDeleteContactRequested(contactId);  // 删通讯录里那一条（contacts 表）
        emit friendDeleted(contactId);             // UI 接去删该好友的会话（conversations，见 MainWindow ⑩）
    }
    // 全部删完后再读一次通讯录：读请求排在所有删请求之后，同一 DB 线程按发出顺序执行，
    // 查回来的一定是删干净后的列表。放在循环外只读一次，避免 N 条墓碑触发 N 次重拉
    if (!contactIds.isEmpty()) {
        loadContacts();
    }
}

// 离线删除好友缓存拉取完成（主线程）：这是我离线期间被别人删掉的那些好友（墓碑）。
// 三步严格串行，顺序不能乱：
//   1) 先按墓碑删本地（contacts + 会话）——本地好友是持久化的，服务器删了本地不删就会残留
//   2) 再回 ACK 给服务器，让它清掉已处理的墓碑，否则每次上线都会重推同一条
//   3) 最后才重拉服务器好友列表做权威对账。这一步顺带覆盖"删了又加回"的边界：
//      若其实还是好友，会被服务器列表重新写回本地（所以顺序必须是"先处理删、再处理拉"）
void MainBackend::onDeleteFriendCacheReceived(const QStringList& targetIds)
{
    applyServerSideFriendDeletion(targetIds);
    if (!targetIds.isEmpty()) {
        m_contactBackend->sendDeleteFriendCacheAck(targetIds);
    }
    m_contactBackend->pullServerContacts();
}

// 在线被删推送（主线程）：对方在线删我，服务器实时推来单条 targetId（值是发起删除的人）。语义与墓碑一致，
// 共用 applyServerSideFriendDeletion；但【不回 ACK、不重拉】——在线推送没有墓碑要清，
// 也不必为一次实时删除再跟服务器对一次账
void MainBackend::onFriendDeletedByPeer(const QString& targetId)
{
    if (targetId.isEmpty()) {
        return;  // 脏包：没有 targetId 无从删起，直接丢弃
    }
    applyServerSideFriendDeletion(QStringList{ targetId });
}

// 服务器拉回全量好友列表（主线程）：服务器是好友资料的权威来源，拉到就逐条覆盖本地 contacts 表。
// saveContact（写）/ loadContacts（读）都只是"发请求信号"，两条请求排队到同一个 DB 线程，
// 按发出顺序执行——所以这里先把 N 条写请求全发出去，再发一次读请求，
// 查回来的一定是刚写完的列表（与 onFriendDeleteSuccess 的"先删后读"是同一套路）
void MainBackend::onPullServerContactsSuccess(const QList<ContactInfo>& contacts)
{
    for (const ContactInfo& contact : contacts) {
        saveContact(contact);  // 单条 upsert：已在库里的被服务器最新资料覆盖，新的直接插入
    }
    loadContacts();            // 重拉通讯录 → ContactBackend 排序 → ContactList 刷新
}

// 同意好友申请成功（主线程）：服务器那边好友关系已经建好，并把新好友资料一起回了过来。
// 本地要做的就是"把新好友写进好友表（contacts）"，两条请求排队到同一个 DB 线程按序执行：
//   1) 服务器已确认关系，新好友直接落库（INSERT OR REPLACE，不用再对账）
//   2) 重拉通讯录 → ContactList 刷新出新好友
// 注意不是"删申请记录"：申请不在本地存（要看申请列表就向服务器拉一次）；
// 删除好友是另一回事——那是等服务器确认后删本地 contacts 表里的那条（见 onFriendDeleteSuccess）
void MainBackend::onAcceptFriendRequestSuccess(const ContactInfo& newFriend)
{
    saveContact(newFriend);     // 1) 新好友入库
    loadContacts();             // 2) 重拉通讯录刷新列表
    // 3) 上抛"哪一条被同意了"：UI 据此把那条从内存列表里摘掉。
    //    申请不落库，本地没有别的痕迹要清——它不是"删申请记录"，服务器那边这条申请已经处理完了
    emit friendRequestAccepted(m_pendingAcceptRequestId);
    m_pendingAcceptRequestId.clear();
}

// 同意失败：回包不带 requestId，把发出时记下的那个补上再上抛，UI 才知道是哪一条失败了。
// 本地没有任何副作用要处理（申请不落库），纯粹是通知
void MainBackend::onFriendRequestAcceptFailed(const QString& message)
{
    emit friendRequestAcceptFailed(m_pendingAcceptRequestId, message);
    m_pendingAcceptRequestId.clear();
}

// 拒绝成功：与同意成功同一套路——只有"哪一条"这一个信息要带给 UI，
// 本地不落库所以没有额外清理（拒绝不会产生新好友，也不碰 contacts 表）
void MainBackend::onFriendRequestRejected()
{
    emit friendRequestRejected(m_pendingRejectRequestId);
    m_pendingRejectRequestId.clear();
}

void MainBackend::onFriendRequestRejectFailed(const QString& message)
{
    emit friendRequestRejectFailed(m_pendingRejectRequestId, message);
    m_pendingRejectRequestId.clear();
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

// 登出：当前登录账号作废，把主后端和三个功能后端各自那份 ID 都清成空白，
// 再断开 TCP、关掉断线自愈。
// 两类调用者：用户主动登出（UI 入口还没接）、以及"令牌失效被踢回登录页"（见 reconnectLoginFailed）
void MainBackend::logout()
{
    m_userid.clear();
    m_loginBackend->clearUserId();      // 含会话令牌——登出即作废
    m_chatBackend->clearUserId();
    m_contactBackend->clearUserId();
    s_loggedIn = false;  // 先清登录标记再断线：同步触发的 disconnected 不该再走"已登录"分支
    m_tcpClient->disableBackoff();       // 会话结束了，不用再替用户退避重连
    m_tcpClient->disconnectFromServer(); // 主动断开旧连接（重新登录时再连）
}

// 踢回登录页：把会话清干净（logout 会断 TCP、关自愈），打上"被踢"标记，
// 再喊一声——main.cpp 的登录循环收到 sessionKicked 后销毁主窗口、重新开登录窗
void MainBackend::kickToLogin()
{
    logout();
    m_kickedToLogin = true;
    emit sessionKicked();
}

// ===== TcpClient 共享信号统一路由（胶水层） =====
// TCP连接成功：按"登录后/修改密码/登录"三种场景分发
void MainBackend::onTcpConnected()
{
    if (s_loggedIn) {
        // 已登录过（断线重连场景）：先把登录态补回来！
        // 服务端的登录态是"每个 TCP 连接一份"（新连接默认未登录），光把 TCP 连上
        // 不等于会话回来了——必须拿登录时下发的令牌补一次登录，补上了才谈得上补拉，
        // 否则那些拉取请求会被服务器按"未登录"回绝
        if (m_loginBackend->hasToken()) {
            m_loginBackend->sendReconnectLoginRequest();
        } else {
            // 手里没令牌（服务器没下发 / 流程异常）：会话根本补不回来，
            // 不硬撑，直接走"请重新登录"那条路。
            // 队列延迟：此刻还在 connected 信号的调用栈里，直接断开会重入 socket 的事件处理
            qDebug() << "MainBackend: 重连成功但手里没有令牌，无法恢复会话";
            QTimer::singleShot(0, this, &MainBackend::kickToLogin);
        }
        return;
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
    // 重连不在这里做：已登录时 TcpClient 自己会排退避（enableBackoff 已打开），
    // 由它按 1/2/4/8/16 秒的档位一直试——比在这里立即重连一次要稳
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
    } else if (type == "reconnect_login_response") {
        // 断线重连的补登录结果（重连成功后自动发出），交给登录后端解析成
        // reconnectLoginSuccess（会话恢复，继续补拉）/ reconnectLoginFailed（令牌失效，回登录页）
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
    else if (type == "friend_request_response"
          || type == "friend_request"                 // 服务器推送：别人发给我的好友申请
          || type == "pull_friend_request_response"
          || type == "accept_friend_request_response"
          || type == "reject_friend_request_response"
          || type == "pull_server_contacts_response"
          || type == "search_request_response"
          || type == "delete_friend_response"
          || type == "set_remark_response"
          || type == "delete_friend_cache_response"  // 离线墓碑拉取回包
          || type == "friend_deleted") {             // 在线被删推送（别人删我，服务器实时推）
        // 好友相关的回包统一交给好友后端，由它内部按 type 再分发（主后端只做粗路由）
        m_contactBackend->onTcpDataReceived(packet);
    }
}