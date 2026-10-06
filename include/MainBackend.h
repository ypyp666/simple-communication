#ifndef MAINBACKEND_H
#define MAINBACKEND_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <qstringview.h>
#include "LoginBackend.h"
#include "ChatBackend.h"
#include "ContactBackend.h"
#include "DatabaseManager.h"
#include "TcpClient.h"
#include "FeatureStructs.h"  // 共享结构体：ConversationInfo / ContactInfo / MessageInfo / OutgoingMessage

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
    // 本轮离开主窗口的原因是"被踢回登录页"还是"用户自己关的窗"：
    // main.cpp 的登录循环据此决定"再开一次登录窗"还是"退出程序"
    bool m_kickedToLogin = false;
    DatabaseManager* m_databaseManager;

    static bool s_loggedIn;  // 全局登录状态（定义在 MainBackend.cpp），TCP 断线重连时判断要不要自动重连

    // 获取当前登录账号ID（登录成功后 m_userid 由登录流程写入，UI 通过它拿真实账号）
    QString currentUserId() const { return m_userid; }


signals:
    void loginSuccess(const QString& accountId);
    void loginFailed();
    void loginNetworkError(const QString& reason);  // 网络层连不上（服务器没跑/断网），与密码错误分开
    void loginWaiting();  // 登录等待中信号
    void loginTimeout();  // 登录连接超时信号
    // 令牌失效（服务器重启 / 令牌过期）：这次会话补不回来了，主窗口该退场、回登录页重新登录。
    // 由 main.cpp 的登录循环接住——本类只负责把状态清干净 + 喊一声（见 logout()）
    void sessionKicked();
    void registerSuccess();
    void registerFailed();
    void registerNetworkError(const QString& reason);  // 注册时网络层连不上（服务器没跑/断网），与服务器拒绝分开
    void registerWaiting();  // 注册等待中信号
    void registerTimeout();  // 注册连接超时
    void connectForRegisterSuccess(const QString& account);  // 注册取号连接成功，服务器下发的账号 ID 随信号带回
    void connectForRegisterFailed();
    void connectForRegisterTimeout();
    void connectForRegisterWaiting();
    void modifyPwdSuccess();  // 修改密码成功（忘记密码页提交后服务器确认）
    void modifyPwdFailed();   // 修改密码失败（仅服务器明确拒绝，如账号不存在）
    void modifyPwdNetworkError(const QString& reason);  // 修改密码时网络层连不上（服务器没跑/断网），与服务器拒绝分开
    void modifyPwdTimeout();  // 修改密码连接超时
    void modifyPwdWaiting();  // 修改密码等待中信号
    void sendWaiting();  // 发送等待信号
    void messageSendSuccess(const QString& tempId, const QString& serverId);  // 发送成功（tempId=本地临时消息ID，serverId=服务器分配的ID）
    void messageSendFailed(const QString& tempId, const QString& serverId);  // 发送失败（未连接/服务器拒绝/超时等），UI变红色感叹号

    // === 会话列表信号（本地库读，主后端直接转发给 UI）===
    // 会话列表（ConversationInfo）→ 喂聊天页左侧 MessageList。
    // 旧名 contactsLoaded 携带的其实是会话列表，和"通讯录"只差一个词极易看错，
    // 现正名为 conversationsLoaded（与 DatabaseManager::conversationsLoaded 同名同义）
    void conversationsLoaded(const QList<ConversationInfo>& conversations);
    // === 通讯录信号（本地库读，主后端直接转发给 UI）===
    // 通讯录（ContactInfo）→ 喂联系人页 ContactList。
    // 与会话列表是两套数据（类型不同、来源不同），各走各的信号，UI 侧各接各的
    void contactsLoaded(const QList<ContactInfo>& contacts);

    // === 好友申请信号（喂"新朋友"页）===
    // friendRequestReceived：收到"一条"申请记录（别人新发来的推送 / 我拉取时服务器逐条重推都走它）。
    //   服务器不下一整个数组，而是逐个元素发（参考拉取离线消息），所以这里是"单条"信号。
    //   申请不做本地持久化——本地不存申请，要看列表就向服务器拉一次（服务器有就有），
    //   这里只负责把服务器送来的这条原样上给 UI
    void friendRequestReceived(const FriendRequestInfo& request);
    // 一次拉取的首尾（纯转发，无副作用 → 直接信号对信号接，不另设具名槽）：
    // 列表内容不在这两个信号里——已经逐条走 friendRequestReceived 送过了，
    // 它们只管"收尾"：成功且一条都没收到 → UI 显示空态；失败 → UI 显示错误文案而不是空列表
    void friendRequestsPulled();
    void friendRequestsPullFailed(const QString& message);
    // 同意 / 拒绝某条申请的结果。两条都带 requestId，UI 靠它把那条从内存列表里摘掉。
    // ContactBackend 的回包里【不带】这个 ID（申请不落库，ID 没有落点，见 ContactBackend.h 的说明），
    // 所以由主后端在"发出请求时"记下、回包时随信号带回
    // （m_pendingAcceptRequestId / m_pendingRejectRequestId）
    void friendRequestAccepted(const QString& requestId);
    void friendRequestAcceptFailed(const QString& requestId, const QString& message);
    void friendRequestRejected(const QString& requestId);
    void friendRequestRejectFailed(const QString& requestId, const QString& message);

    // === 好友操作的结果（服务器回包，转发自 ContactBackend）===
    // 删好友：只有 deleteFriendSuccess 才代表"服务器已确认"，主后端在这时才删本地库并重拉列表；
    // 失败只上抛给 UI 提示（本地不动）。命名与 ContactBackend 同名信号一致
    void friendDeleted(const QString& contactId);
    void friendDeleteFailed(const QString& contactId, const QString& message);
    // 改备注的结果：只有 setContactRemarkSuccess 才代表"服务器已确认"，主后端在这时
    // 才把新备注写本地库，并把改后的值上抛给详情页刷新显示；失败只上抛提示（本地不动）
    void setContactRemarkSuccess(const QString& contactId, const QString& remark);
    void setContactRemarkFailed(const QString& contactId, const QString& message);
    // 发好友申请的结果：只有 success 代表"服务器已受理"。服务器回包不带对方 ID，
    // 所以是纯状态信号（不带数据），UI（搜索页）据此给「添加」一个成功/失败的反馈
    void sendFriendRequestSuccess();
    void sendFriendRequestFailed(const QString& message);

    // === 搜索用户的结果（服务器回包，转发自 ContactBackend）===
    // 纯转发：结果只上搜索页，本地不落库、也不动通讯录——搜到不等于已经是好友
    void searchSuccess(const QList<ContactInfo>& results);
    void searchFailed(const QString& message);

    // 分页查询结果转发（后台DB线程查完 → 这里 → UI）。
    // page=1 是最新一页；hasMore=false 表示历史已翻到底，UI 不用再监听滚顶
    void messagesPageLoaded(const QString& contactId, int page,
                            const QList<MessageInfo>& messages, bool hasMore);
    void messageReceived(const MessageInfo& message);                // 接收成功（对方发来的新消息，UI显示+存库+回ACK）
    void messageReceiveFailed(const QString& serverId);              // 接收失败（携带服务器消息ID，回ACK让服务器重发）

    // === 数据库异步请求信号（主线程发出 → 后台DB线程执行）===
    void dbSaveMessageRequested(const MessageInfo& message);
    void dbSetAccountRequested(const QString& accountId);
    void dbUpdateMessageIDRequested(const QString& contactId, const QString& oldId, const QString& newId);
    // 分页读消息请求：contactId+page 交给 DatabaseManager 在后台线程查 SQLite
    //（不能在主线程直调查询——QSqlDatabase 连接只允许在创建它的线程里使用）
    void dbLoadMessagesPageRequested(const QString& contactId, int page, int pageSize);
    // 本地库直读请求：联系人 / 会话列表（无参数）。查完的结果由 DatabaseManager 发信号回来，
    // 主后端用 onDbContactsLoaded / onDbConversationsLoaded 具名槽收口后再上到 UI
    // （会话列表直接转发；通讯录会先经 ContactBackend 排序，再由它发信号上来。
    //   与消息侧 onDbMessagesPageLoaded 同一套路）。同样是"不能在主线程直调查询"那条规矩
    void dbLoadContactsRequested();
    void dbLoadConversationsRequested();
    // 本地库写入请求：会话 / 联系人 的增删改（主线程发出 → 后台DB线程执行，与上面的读请求对称）。
    // 注意：DatabaseManager 这几个写函数是 bool 返回值、没有结果信号——跨线程 return 取不回，
    // 所以这里只负责"把活派给 DB 线程"，UI 拿不到成功与否的回执
    void dbSaveConversationRequested(const ConversationInfo& conversation);
    // 会话快照的另外两个写口（与 dbSaveConversationRequested 同一套路，只派活给 DB 线程）：
    //   收发消息后更新最后消息/时间 + 未读 +1；
    //   打开会话时按 messages 里最新一条重算快照（一条 SQL 子查询搞定，UI 不用先查再写）
    void dbUpdateConversationMessageRequested(const QString& contactId, const QString& lastMessage,
                                              const QDateTime& lastTime, bool increaseUnread);
    void dbRefreshConversationSnapshotRequested(const QString& contactId);
    // 打开会话：未读清零
    void dbClearConversationUnreadRequested(const QString& contactId);
    void dbDeleteConversationRequested(const QString& conversationId);
    void dbSaveContactRequested(const ContactInfo& contact);
    void dbDeleteContactRequested(const QString& contactId);
    void dbSetContactRemarkRequested(const QString& contactId, const QString& remark);

    // === 数据库结果信号（后台DB线程回传 → 主线程）===
    void messageSaved(bool success);
    void messageIdUpdated(bool success);
    void databaseInitialized(bool success);

public slots:
    void login(const QString& username, const QString& password);
    // 注册第二段（提交）：注册页填好密码后由 registerAquiard 直接绑定过来，转给 LoginBackend 发送
    void registerUser(const QString& username, const QString& password);
    // 注册第一段（取号）：注册页显示时调用，发起 TCP 连接，账号 ID 由服务器在连接后下发
    void prepareRegisterConnection();
    // 修改密码入口（忘记密码页提交后调用）：未登录态走 TCP，设置功能枚举后连接服务器
    void modifyPwd(const QString& account, const QString& newPassword);
    void JsonParsing(const QByteArray packet);
    void sendMessage(const MessageInfo& message);
    // 统一的"重试"入口：按功能枚举把重试请求路由到对应的后端
    //   MessageSend → ChatBackend（聊天消息重发，消息体由 ChatWindow 从待确认表里取出后传入）
    //   Register    → LoginBackend（注册页账号行取号失败重试）
    void onRetryRequested(LoginFeature feature, const QString& id, const MessageInfo& message = MessageInfo());

    // === 聊天相关接口（转发到 ChatBackend）===
    // 拉会话列表（喂 MessageList）。旧名 loadContacts 实际拉的是会话列表（不是通讯录），
    // 正名为 loadConversations，与 DatabaseManager::loadConversations 对齐
    void loadConversations();
    // === 联系人相关接口 ===
    // 拉通讯录（喂 ContactList）。正名为 loadContacts，与 DatabaseManager::loadContacts 对齐
    void loadContacts();
    // === 好友申请相关接口（喂"新朋友"页）===
    // 拉好友申请列表：UI 点"新朋友"时调。只发请求——服务器把每条申请当成独立的
    // friend_request 包逐条送回来（见 ContactBackend::onTcpDataReceived），
    // 本地【不落库】，收到的都堆在 UI 侧那个内存列表里
    void pullFriendRequests();
    // 搜索用户（网络请求）：搜索页回车 / 点"搜索"→ 这里 → ContactBackend 发 search_request。
    // 结果异步回来：成功走 searchSuccess（一整批用户资料），失败走 searchFailed。
    // 与好友申请同属"不落库"的在线数据：搜到只是看看，本地什么都不存
    void searchUser(const QString& keyword);
    // 加好友（网络请求）：搜索页"申请加好友"小窗点发送 → 这里 → ContactBackend 发好友申请。
    // userId=要加的人，message=留言（验证消息，可为空）。本步只是"把申请发出去"，
    // 对方同意后服务器才会把新好友资料回下来（见 onAcceptFriendRequestSuccess）
    void sendFriendRequest(const QString& userId, const QString& message);
    // 同意 / 拒绝某条申请（网络请求）：参数是申请列表项带回来的 requestId。
    // 注意是"哪条申请"的服务器记录ID，不是对方账号ID
    void acceptFriendRequest(const QString& requestId);
    void rejectFriendRequest(const QString& requestId);
    // 删除好友（网络请求）：UI 右键"删除好友"→ 这里 → ContactBackend 发 delete_friend 给服务器。
    // 注意：这一步只是"请求删除"，本地库要等服务器回 deleteFriendSuccess 才真删（见 onFriendDeleteSuccess）
    void deleteFriend(const QString& contactId);
    // 分页加载本地聊天记录：page=1 最新一页（点联系人时调），page 递增往历史翻（滚到顶部时调）。
    // 实际查询在后台 DB 线程，结果经 messagesPageLoaded 信号送回 UI
    void loadMessages(const QString& contactId, int page = 1);
    void sendFile(const QString& contactId, const QString& filePath);
    // 输入内容记忆功能
    void saveInputContent(const QString& contactId, const QString& content);
    QString getInputContent(const QString& contactId);
    void clearInputContent(const QString& contactId);
    // 断开会话：直接关闭当前 TCP 连接（转发到 TcpClient）
    void disconnectSession();
    // 登出：预留的口。当前登录账号作废，通知各功能后端把自己那份 ID 清成空白。
    // 真正的登出流程（断开 TCP、回登录页）还没接，这里只负责"清账号"这一件事
    void logout();

    // === 数据库异步接口（只发请求信号，不阻塞主线程）===
    void saveMessage(const MessageInfo& message);
    void setCurrentAccountId(const QString& accountId);
    void onDbMessageIdUpdated(const QString& contactId, const QString& oldId, const QString& newId);
    // 会话 / 联系人 的增删改入口：UI 调这几个，内部只 emit 对应的 dbXxxRequested，
    // 真正的 SQL 在后台 DB 线程执行（与 saveMessage 同一套路）
    void saveConversation(const ConversationInfo& conversation);
    // 打开某个会话时刷新会话快照：最后消息 / 时间按 messages 里最新一条重算（没有历史就当前时间）。
    // 双击联系人新建会话同样走这里——库里没有这条会话时 DB 层会自动补插一条
    void refreshConversationSnapshot(const QString& contactId);
    // 收发消息后更新会话快照；increaseUnread=false 发消息（未读不变），true 收消息（未读 +1）
    void updateConversationMessage(const QString& contactId, const QString& lastMessage,
                                   const QDateTime& lastTime, bool increaseUnread);
    // 打开某个会话：未读清零
    void clearConversationUnread(const QString& contactId);
    void deleteConversation(const QString& conversationId);
    void saveContact(const ContactInfo& contact);
    void deleteContact(const QString& contactId);
    // 改好友备注（网络请求）：UI 详情页点"保存备注"→ 这里 → ContactBackend 发 set_remark 给服务器。
    // 注意：这一步只是"请求改"，本地库要等服务器回 setRemarkSuccess 才真改（见 onSetRemarkSuccess）
    void setContactRemark(const QString& contactId, const QString& remark);
    

private slots:
    // 后台数据库线程的结果回传（在【主线程】执行）
    void onDbMessageSaved(bool success);
    // 后台DB线程分页查询完成（在主线程执行）：转发给 UI
    void onDbMessagesPageLoaded(const QString& contactId, int page,
                                const QList<MessageInfo>& messages, bool hasMore);
    // 后台DB线程查完会话列表 / 通讯录（在主线程执行）：原样转发给 UI
    void onDbConversationsLoaded(const QList<ConversationInfo>& conversations);
    // 合并节流后真正去拉会话列表（见 MainBackend::loadConversations 的说明）
    void onCoalescedLoadConversations();
    void onDbContactsLoaded(const QList<ContactInfo>& contacts);
    void onDbInitialized(bool success);
    // 服务器确认删好友成功：本地这时才真删（删库 + 重拉通讯录刷新列表），再上抛 friendDeleted
    void onFriendDeleteSuccess(const QString& targetId);
    // 离线删除好友缓存拉取完成：服务器把"我该删掉的联系人"下发来了（我离线期间被删的那些）。
    // 有副作用（要删本地库），落具名槽：逐条删 contacts/conversations → 回 ACK → 重拉服务器好友列表。
    // 严格串行——ACK 发完才发 pullServerContacts（见 .cpp），保证"先处理删、再处理拉"的顺序
    void onDeleteFriendCacheReceived(const QStringList& targetIds);
    // 在线被删推送：对方在线删我，服务器实时推来单条 targetId（值是发起删除的人）。
    // 与墓碑同一套"服务器侧删除"语义，共用 applyServerSideFriendDeletion，但【不回 ACK、不重拉】——
    // 在线推送没有墓碑要清，也不必为一次实时删除再跟服务器对一次账
    void onFriendDeletedByPeer(const QString& targetId);
    // 服务器确认改备注成功：本地这时才真改（写本地库），再把改后的值上抛给详情页刷新显示。
    // 失败只上抛提示（本地不动）。两者都靠 m_pendingRemarkContactId / m_pendingRemark 记着改的是谁、改成什么
    void onSetRemarkSuccess(const QString& targetId);
    void onSetRemarkFailed(const QString& targetId, const QString& message);
    // 拉取服务器好友列表成功：服务器是好友资料的权威来源，逐条 upsert 进本地 contacts 表
    // （saveContact 内部是 INSERT OR REPLACE），再重拉一次通讯录刷新 ContactList
    void onPullServerContactsSuccess(const QList<ContactInfo>& contacts);
    // 同意好友申请成功：服务器已把好友关系建好，并把新好友资料一起回下来。
    // 本地要做的就是"把新好友写进好友表（contacts）"——不是去删申请记录，申请压根不在本地存
    void onAcceptFriendRequestSuccess(const ContactInfo& newFriend);
    // 同意 / 拒绝失败：回包不带 requestId，用它把 m_pendingXxxRequestId 里那个 ID 补上再上抛，
    // UI 才知道"是哪一条失败了"（同意成功那条在上面的 onAcceptFriendRequestSuccess 里收尾）
    void onFriendRequestAcceptFailed(const QString& message);
    void onFriendRequestRejected();
    void onFriendRequestRejectFailed(const QString& message);
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
    ContactBackend* m_contactBackend;
    QThread* m_dbThread;  // 后台数据库线程

    // 注册登录后端（登录/修改密码）的信号连接，从构造函数抽出，避免构造函数越堆越长
    void setupLoginConnections();
    // 服务器侧的"删好友"落到本地：逐条删 contacts 表，并上抛 friendDeleted 让 UI 删掉对应会话
    // （conversations）。离线墓碑与在线推送都走它，保证两条路径的本地行为完全一致
    void applyServerSideFriendDeletion(const QStringList& contactIds);
    // 未登录态流程（登录/注册/修改密码）结束时统一收尾：清掉功能标记，回到"空闲"态
    void resetFeature();
    // 踢回登录页：清会话（logout）+ 打上"被踢"标记 + 喊一声 sessionKicked。
    // 调用点一律用队列延迟（QTimer::singleShot(0, ...)）——
    // 它内部要断 TCP，而触发它的信号往往正跑在 socket 的事件处理栈里，直接调用会重入
    void kickToLogin();
    // 会话列表重拉是否已"预约"：合并节流用——同一轮里连调多次 loadConversations 只发一趟请求
    bool m_conversationsLoadPending = false;
    // 当前 TCP 连接所服务的功能（未登录态：登录 / 修改密码），
    // onTcpConnected/Error/Timeout 据此把 TcpClient 的信号路由到对应后端的功能函数
    LoginFeature m_currentFeature = LoginFeature::None;

    // "正在同意 / 正在拒绝"的那条申请ID。服务器回包不带这个 ID（申请不落库），
    // 而 UI 要靠它把条目从内存列表里摘掉，所以"谁发谁记"：
    // 本类发出请求时记下，回包时随上抛信号带回，然后立刻清空。
    // 同一时刻只可能有一条在飞——卡片上的同意/拒绝每点一次才发一次请求，
    // 而回包会立刻把 pending 清掉，不会积压出第二条
    QString m_pendingAcceptRequestId;
    QString m_pendingRejectRequestId;

    // "正在改备注"的那位好友 + 改后的内容。服务器回包只保证带回 targetId（不保证回显 remark），
    // 而本地写库、UI 刷新都需要"改成什么"，所以同样"谁发谁记"：发出请求时记下，
    // 回包时用它们写本地并上抛，然后清空。同一时刻只可能有一条在飞（点一次保存才发一次请求）
    QString m_pendingRemarkContactId;
    QString m_pendingRemark;
};

#endif // MAINBACKEND_H
