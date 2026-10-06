#include "MainWindow.h"
#include <QCloseEvent>
#include "ChatWindow.h"
#include "StatusBar.h"
#include "ContactWindow.h"
#include "ContactList.h"
#include "ContactDetailArea.h"
#include "FriendDetailPage.h"
#include "FriendRequestList.h"
#include "SearchWindow.h"
#include "AgentWindow.h"

MainWindow::MainWindow(QWidget *parent, MainBackend* backend) : QMainWindow(parent)
{
    m_backend = backend;
    setWindowTitle("CCEarth");
    setMinimumSize(900, 600);
    resize(1000, 700);

    initUI();
    initPages();
}

MainWindow::~MainWindow()
{
}

// 用户关闭主窗口：通知 main.cpp 的登录循环（它会判断是退出程序还是回登录页），
// 再交给基类走默认关闭流程
void MainWindow::closeEvent(QCloseEvent* event)
{
    emit closed();
    QMainWindow::closeEvent(event);
}

void MainWindow::initUI()
{
    centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    mainLayout = new QHBoxLayout(centralWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    // 提升为成员：联系人页双击打开会话时要靠它切换导航选中态（并间接触发换页）
    m_statusBar = new StatusBar(this);
    mainLayout->addWidget(m_statusBar);
    connect(m_statusBar, &StatusBar::m_changePage, this, &MainWindow::switchToPage);
}

void MainWindow::initPages()
{
    pageStack = new QStackedWidget(this);
    mainLayout->addWidget(pageStack);

    // 添加页面（需要后端数据的页面传入主后端对象；联系人页只摆控件，不接线也不持后端）
    m_chatWindow = new ChatWindow(this, m_backend);
    m_contactWindow = new ContactWindow(this);
    SearchWindow* searchWindow = new SearchWindow(this);
    m_searchWindow = searchWindow;   // 页面栈里的指针之外再留一份成员：切页面时要清它的搜索框
    AgentWindow* agentWindow = new AgentWindow(this);
    pageStack->addWidget(m_chatWindow);
    pageStack->addWidget(m_contactWindow);
    pageStack->addWidget(searchWindow);
    pageStack->addWidget(agentWindow);


    // 默认显示聊天页面
    pageStack->setCurrentIndex(CHAT_PAGE_INDEX);

    // ========== 信号连接（统一收口在这里，等所有页面都建好再接）==========
    // 两条轴各有一个统一入口，和登录页那三个页面（LoginPage / RegisterPage /
    // ForgotPasswordPage 都只认 MainBackend）是同一套规矩：
    //   · 要驱动 UI 的   → 统一从主 UI（本类）进，再由本类分发到具体页面
    //   · 要动后端的     → 统一从主后端（MainBackend）进，由它路由一层到业务后端
    //                      （LoginBackend / ChatBackend），页面永远不直接碰业务后端
    // 注意"页面不许接线"是错的：页面可以接线、也可以持有 MainBackend*，
    // 唯一的约束是它只许认主后端。目的是追一条链路只看"主 UI 接线处 + 主后端路由处"
    // 两个地方就够，不用顺着各页面的 emit 名一层层往上拼

    // ① 联系人页 → 聊天页：双击条目 / 右键"打开会话"
    //    这里不是"信号→信号直连"，而是落到具名槽 onContactOpened：
    //    断点打得进、日志插得下，以后要加逻辑（如记录最近访问）有地方落
    connect(m_contactWindow->contactList(), &ContactList::contactOpened,
            this, &MainWindow::onContactOpened);
    // ①.1 联系人页单击条目 → 右栏显示好友详情（只显示，不切页、不开会话）。
    //     配套的实现在 onContactSelectedInList 里（切页 + 灌数据是一件事的两半）
    connect(m_contactWindow->contactList(), &ContactList::contactSelected,
            this, &MainWindow::onContactSelectedInList);
    // ② 联系人页 → 删除好友：要动后端，先落到具名槽，再由它转给 MainBackend，
    //    最终由 MainBackend 路由到 ContactBackend（发 delete_friend 给服务器）
    connect(m_contactWindow->contactList(), &ContactList::deleteContactRequested,
            this, &MainWindow::onDeleteContactRequested);
    // ③ 跨页面最后一跳只走信号槽，不出现 m_chatWindow->openConversation() 这种直接调用
    connect(this, &MainWindow::openConversationRequested,
            m_chatWindow, &ChatWindow::openConversation);

    // ④ 后端 → 联系人列表：数据从主后端出来（MainBackend 已把 ContactBackend::contactsLoaded
    //    收上来重新发射），页面侧只认主后端，不认识 ContactBackend。
    //    注意：会话列表走的是 MainBackend::conversationsLoaded（喂 ChatWindow），两条线各接各的
    if (m_backend) {
        // 这里本该直连 ContactList::setContacts，之所以落成具名槽：
        // loadContacts() 是发请求给后台 DB 线程，回包是 QueuedConnection，
        // 会在 MainWindow 构造完、事件循环跑起来之后才到——所以左边两份列表
        // （通讯录 + 会话列表名字）都得等这份回包到了才能填，不能构造期间直接灌
        connect(m_backend, &MainBackend::contactsLoaded,
                this, &MainWindow::onContactsLoaded);
    }

    // ⑤ 联系人页 → "新朋友"：点左栏"新朋友"行。纯 UI 那半（右栏共用区切到请求列表）在具名槽里做，
    //    要动后端的那半（拉申请列表）由同一个槽转给 MainBackend
    connect(m_contactWindow->contactList(), &ContactList::newFriendRequested,
            this, &MainWindow::onNewFriendRequested);
    // ⑥ 请求列表卡片上的「同意」/「拒绝」 → 具名槽 → MainBackend 发请求给服务器
    connect(m_contactWindow->detailArea()->friendRequestList(), &FriendRequestList::acceptRequested,
            this, &MainWindow::onFriendAcceptRequested);
    connect(m_contactWindow->detailArea()->friendRequestList(), &FriendRequestList::rejectRequested,
            this, &MainWindow::onFriendRejectRequested);

    // ⑦ 搜索页 ⇄ 主后端：搜索页与其它页面同一套规矩——只认主后端，不认识 ContactBackend。
    //    回车 / 点"搜索" → 发请求；结果 / 失败 → 回填结果列表；结果卡片的「添加」→ 发好友申请。
    //    四条都是纯转发（搜索结果不落库），所以不落具名槽，直接连到方法上
    if (m_backend) {
        connect(m_searchWindow, &SearchWindow::searchRequested,
                m_backend, &MainBackend::searchUser);
        connect(m_searchWindow, &SearchWindow::addFriendRequested,
                m_backend, &MainBackend::sendFriendRequest);
        connect(m_backend, &MainBackend::searchSuccess,
                m_searchWindow, &SearchWindow::setSearchResults);
        connect(m_backend, &MainBackend::searchFailed,
                m_searchWindow, &SearchWindow::setSearchFailed);
        // ⑦.1 好友申请的服务器回包 → 搜索页顶部一句成功/失败反馈。
        //      回包是纯状态（不带数据），所以拆成两条"信号对方法"直连，不套 lambda
        connect(m_backend, &MainBackend::sendFriendRequestSuccess,
                m_searchWindow, &SearchWindow::showAddFriendSuccess);
        connect(m_backend, &MainBackend::sendFriendRequestFailed,
                m_searchWindow, &SearchWindow::showAddFriendFailed);
    }

    // ⑧ 后端 → 请求列表：申请由服务器逐条下发（不是一个数组），所以是"收一条加一条"；
    //    拉取首尾两条只管收尾——成功后一条都没有就显示空态，失败显示错误文案而不是空列表。
    //    这三条都是纯转发（列表自己管内存、不落库），照 ④ 的先例直接连到控件方法上
    if (m_backend) {
        FriendRequestList* requestList = m_contactWindow->detailArea()->friendRequestList();

        connect(m_backend, &MainBackend::friendRequestReceived,
                requestList, &FriendRequestList::addRequest);
        connect(m_backend, &MainBackend::friendRequestsPulled,
                requestList, &FriendRequestList::finishLoading);
        connect(m_backend, &MainBackend::friendRequestsPullFailed,
                requestList, &FriendRequestList::setError);
        // 同意 / 拒绝的结果要"从内存列表里摘掉那条"，落到具名槽里
        connect(m_backend, &MainBackend::friendRequestAccepted,
                this, &MainWindow::onFriendRequestAccepted);
        connect(m_backend, &MainBackend::friendRequestRejected,
                this, &MainWindow::onFriendRequestRejected);
        connect(m_backend, &MainBackend::friendRequestAcceptFailed,
                this, &MainWindow::onFriendRequestAcceptFailed);
        connect(m_backend, &MainBackend::friendRequestRejectFailed,
                this, &MainWindow::onFriendRequestRejectFailed);

        // 当前登录账号注入请求列表：用来区分"别人发来的"（卡片给同意/拒绝）和"我发出的"（只给状态词）
        requestList->setCurrentAccountId(m_backend->currentUserId());
    }

    // ⑨ 好友详情页（右栏）：发送消息是纯 UI 链路，改备注才要经主后端。
    FriendDetailPage* detailPage = m_contactWindow->detailArea()->friendDetailPage();
    // 点「发送消息」= 双击条目那条路：切聊天页 + 打开会话，直接复用 onContactOpened
    connect(detailPage, &FriendDetailPage::sendMessageRequested,
            this, &MainWindow::onContactOpened);
    // 点「删除好友」：与右键菜单"删除好友"是同一个动作（都等服务器确认成功才删本地），
    // 所以复用同一个主后端入口 deleteFriend
    connect(detailPage, &FriendDetailPage::deleteFriendRequested,
            this, &MainWindow::onFriendDetailDeleteRequested);
    // 改备注走"先发服务器、服务器确认成功再改本地"那条线：
    // 保存备注 → 主后端发 set_remark 给服务器；服务器回包的结果再回到详情页刷新显示 / 提示失败
    if (m_backend) {
        connect(detailPage, &FriendDetailPage::remarkSaved,
                m_backend, &MainBackend::setContactRemark);
        connect(m_backend, &MainBackend::setContactRemarkSuccess,
                detailPage, &FriendDetailPage::onRemarkSaveSuccess);
        connect(m_backend, &MainBackend::setContactRemarkFailed,
                detailPage, &FriendDetailPage::onRemarkSaveFailed);
    }

    // ⑩ 删好友成功 → 该好友的会话也要一起删：直接复用聊天页现成的"删除会话"槽
    //    （它会删本地会话快照 + 若正打开则清聊天区 + 重拉会话列表），不再另写一套收尾逻辑。
    //    链路：删除好友确认成功 → MainBackend::friendDeleted(contactId) → 这里 → ChatWindow::onDeleteConversation
    if (m_backend) {
        connect(m_backend, &MainBackend::friendDeleted,
                m_chatWindow, &ChatWindow::onDeleteConversation);
    }

    // ⑪ 状态栏红点：两条独立的数据源各点亮一个导航按钮。
    //    · 会话列表回包 → 汇总未读数 → "消息"按钮
    //    · 收到一条好友申请 → 只算"别人发来的" → "联系人"按钮
    //    两条都落具名槽（不是信号直连 setBadge）：红点数字要经过"求和 / 去重"加工，
    //    不是原样透传
    if (m_backend) {
        connect(m_backend, &MainBackend::conversationsLoaded,
                this, &MainWindow::onConversationsLoaded);
        connect(m_backend, &MainBackend::friendRequestReceived,
                this, &MainWindow::onFriendRequestReceived);
        // 拉取收尾（成功 / 失败都算收尾）：撤掉红点用的"拉取窗口"标记
        connect(m_backend, &MainBackend::friendRequestsPulled,
                this, &MainWindow::onFriendRequestPullFinished);
        connect(m_backend, &MainBackend::friendRequestsPullFailed,
                this, &MainWindow::onFriendRequestPullFinished);
    }

    // 所有页面建好、信号接好之后再拉数据。
    // 不能放在各页面构造函数里：接收页面建得更晚就收不到信号（列表空白）。
    // 注意这两个 load 都是"发请求"而不是"同步填数据"——通讯录那条约 DB 后台线程
    // 绕一圈才回来（QueuedConnection），所以构造期间别指望列表里已经有东西
    if (m_backend) {
        m_backend->loadConversations();  // 会话列表 → ChatWindow
        m_backend->loadContacts();       // 通讯录   → ContactWindow
    }
}

// 后端通讯录回包（DB 线程 QueuedConnection 回来的）→ 喂给左栏 ContactList，
// 同时把同一份名单给会话列表做名字兜底。之所以落成具名槽而不是信号直连 setContacts：
// loadContacts() 是发请求给后台 DB 线程，回包要等事件循环跑起来才到，
// 所以构造期间列表一定是空的，得等这份回包到了左栏才有内容
void MainWindow::onContactsLoaded(const QList<ContactInfo>& contacts)
{
    // 这一份通讯录数据两处用：
    //   ① 通讯录列表（左栏"我的好友"）——正常渲染；
    //   ② 会话列表的名字兜底——conversations 表里没有名字列，会话列表的名字靠查库时
    //      LEFT JOIN contacts 补；联系人还没入 contacts 表时那个名字是空的，
    //      用这份内存名字顶上，免得为了一个名字再查一次库
    QList<ContactInfo> list = contacts;

    // ===== 【调试用假数据：本地库/服务器都没好友时，补两位写死的好友把左栏跑通】=====
    // 联调阶段已注释，走真实数据；以后要单独跑 UI 看效果时取消注释即可
    // if (list.isEmpty()) {
    //     ContactInfo zhangSan;
    //     zhangSan.id = "12345";
    //     zhangSan.name = "张三";
    //     list.append(zhangSan);

    //     ContactInfo admin;
    //     admin.id = "10010";
    //     admin.name = "管理员大人";
    //     list.append(admin);
    // }

    m_contacts = list;   // 存一份：单击好友时右栏要拿它填详情（信号只带 id + 名字，缺备注）

    m_contactWindow->contactList()->setContacts(list);
    m_chatWindow->conversationList()->setContactNames(list);
}

// 单击通讯录里的好友 → 右栏切到"好友详情"并把这位好友的资料摆上去。
// "切页"和"灌数据"必须一起做，少一个就会出现"切过去了但内容还是上一个人"
// 或者"内容对了却没切页"
void MainWindow::onContactSelectedInList(const QString& contactId, const QString& contactName)
{
    // 从缓存里捞完整资料（联系人信号只带 id + 名字，备注得从这儿拿）。
    // 捞不到就退化成"用信号里的 id + 名字凑一个"——宁可少一行备注，也别显示空页面
    ContactInfo info;
    for (const ContactInfo& contact : m_contacts) {
        if (contact.id == contactId) {
            info = contact;
            break;
        }
    }
    if (info.id.isEmpty()) {
        info.id = contactId;
        info.name = contactName;
    }

    m_contactWindow->detailArea()->showFriendDetail();
    m_contactWindow->detailArea()->friendDetailPage()->setContact(info);
}

// 联系人页双击条目 / 右键"打开会话" → 切到聊天页并打开该联系人会话
void MainWindow::onContactOpened(const QString& contactId, const QString& contactName)
{
    if (!m_chatWindow || !m_statusBar) {
        return;
    }
    // setCurrentIndex 内部既切导航按钮的选中态，又 emit m_changePage → switchToPage 换页面栈
    m_statusBar->setCurrentIndex(CHAT_PAGE_INDEX);
    // 后半程（打开会话）交给信号，不去直接调 m_chatWindow 的方法
    emit openConversationRequested(contactId, contactName);
}

// 右键"删除好友" → 让主后端发删除请求给服务器。
// 这里只"请求删除"，本地列表一动不动：等服务器回 delete_friend_response 且成功，
// 才由 MainBackend::onFriendDeleteSuccess 删本地库 + 重拉通讯录，列表随之刷新
void MainWindow::onDeleteContactRequested(const QString& contactId, const QString& contactName)
{
    Q_UNUSED(contactName);  // 名字留给以后弹"确认删除 XXX？"用，当前直接发请求
    if (m_backend) {
        m_backend->deleteFriend(contactId);
    }
}

// 详情页「删除好友」按钮 → 走的是与右键菜单同一条路。
// 这里同样只"请求删除"，本地列表一动不动：等服务器回 delete_friend_response 且成功，
// 才由 MainBackend::onFriendDeleteSuccess 删本地库 + 重拉通讯录，列表随之刷新
void MainWindow::onFriendDetailDeleteRequested(const QString& contactId)
{
    if (m_backend) {
        m_backend->deleteFriend(contactId);
    }
}

// 点左栏"新朋友"：右栏共用区域切到请求列表 → 列表置加载态 → 向服务器拉一次。
// 顺序有意为之：先把界面切好再发请求，服务器逐条回包时列表已经在了，不会漏渲染
void MainWindow::onNewFriendRequested()
{
    // 右栏那块共用区域切到"好友请求列表"，好友详情自动被屏蔽
    m_contactWindow->detailArea()->showFriendRequests();

    // 进"新朋友"页 = 用户已经看到这些申请了 → 状态栏"联系人"按钮的红点清零。
    // 必须放在下面 pull 之前：拉取会让服务器把全部申请重推一遍，先标记已查看，
    // 重推的旧申请才不会被又点成红点
    markFriendRequestsSeen();
    // 开一次"拉取窗口"：回包前收到的申请（就是这次重推）一律算已查看，
    // 免得用户没等回包就切走时旧申请又被点成红点
    m_friendRequestPullActive = true;

    FriendRequestList* list = m_contactWindow->detailArea()->friendRequestList();
    list->setLoading();

    // ===== 【调试用假数据：服务器端好友申请还没落地时，手工造三条喂进列表看 UI】=====
    // 联调阶段已注释，改走下面的真实拉取；以后要单独跑 UI 看效果时取消注释即可
    // if (m_backend) {
    //     const QString me = m_backend->currentUserId();
    //
    //     FriendRequestInfo incoming1;
    //     incoming1.requestId = "req-1001";
    //     incoming1.accountId = "10086";              // 别人发来的：accountId 是对方账号
    //     incoming1.targetId  = me;
    //     incoming1.name      = "林小满";
    //     incoming1.remark    = "我是隔壁工位的林小满";
    //     incoming1.sendTime  = "2026-10-01T09:12:00";
    //     list->addRequest(incoming1);
    //
    //     FriendRequestInfo incoming2;
    //     incoming2.requestId = "req-1002";
    //     incoming2.accountId = "10010";
    //     incoming2.targetId  = me;
    //     incoming2.name      = "周行";               // 附言故意留空，看默认文案
    //     incoming2.sendTime  = "2026-09-30T21:40:00";
    //     list->addRequest(incoming2);
    //
    //     FriendRequestInfo outgoing;
    //     outgoing.requestId = "req-1003";
    //     outgoing.accountId = me;                    // 我发出的：卡片右侧只出"等待验证"状态词，不给按钮
    //     outgoing.targetId  = "10099";
    //     outgoing.name      = "陈稚";
    //     outgoing.remark    = "一起打球的那位";
    //     outgoing.sendTime  = "2026-09-29T18:05:00";
    //     list->addRequest(outgoing);
    // }

    // 真实拉取：向服务器要"与我相关的全部申请"，服务器逐条 friend_request 推回来，
    // 由 FriendRequestList::addRequest 增量灌进列表（按 requestId 去重）
    if (m_backend) {
        m_backend->pullFriendRequests();
    }
}

// 卡片上的「同意」：交给主后端发请求
void MainWindow::onFriendAcceptRequested(const QString& requestId)
{
    if (!m_backend) {
        return;
    }
    m_backend->acceptFriendRequest(requestId);
}

// 卡片上的「拒绝」：与同意同一套路
void MainWindow::onFriendRejectRequested(const QString& requestId)
{
    if (!m_backend) {
        return;
    }
    m_backend->rejectFriendRequest(requestId);
}

// 同意成功：服务器那边好友关系已建好（通讯录会由主后端自动刷新），
// 本地这边只要把那条申请从内存列表摘掉——申请不落库，没有别的痕迹要清
void MainWindow::onFriendRequestAccepted(const QString& requestId)
{
    m_contactWindow->detailArea()->friendRequestList()->removeRequest(requestId);
    // 这条申请已被处理：从红点里摘掉，并记入"已查看"，服务器重推时不再点数
    m_unseenFriendRequestIds.remove(requestId);
    m_seenFriendRequestIds.insert(requestId);
    refreshFriendRequestBadge();
}

// 拒绝成功：同上
void MainWindow::onFriendRequestRejected(const QString& requestId)
{
    m_contactWindow->detailArea()->friendRequestList()->removeRequest(requestId);
    m_unseenFriendRequestIds.remove(requestId);
    m_seenFriendRequestIds.insert(requestId);
    refreshFriendRequestBadge();
}

// 同意失败：那条申请还在列表里，卡片也不动，只在标题栏下方挂一条红字提示
void MainWindow::onFriendRequestAcceptFailed(const QString& requestId, const QString& message)
{
    Q_UNUSED(requestId);   // 失败只需要"提示"，不需要按 ID 做什么
    m_contactWindow->detailArea()->friendRequestList()->showOperationError(
        message.isEmpty() ? QStringLiteral("同意好友申请失败") : message);
}

// 拒绝失败：同同意失败
void MainWindow::onFriendRequestRejectFailed(const QString& requestId, const QString& message)
{
    Q_UNUSED(requestId);
    m_contactWindow->detailArea()->friendRequestList()->showOperationError(
        message.isEmpty() ? QStringLiteral("拒绝好友申请失败") : message);
}

// ===================== 状态栏红点 =====================

// 会话列表回包 → 把每条会话的未读数加起来，点亮"消息"导航按钮的红点。
// 未读清零走的是"打开会话 → clearConversationUnread → 重拉列表"这条既有链路，
// 所以这里只认 conversationsLoaded 一份数据源，不用再单独接收消息信号
void MainWindow::onConversationsLoaded(const QList<ConversationInfo>& conversations)
{
    int unread = 0;
    for (const ConversationInfo& conversation : conversations) {
        unread += conversation.unreadCount;
    }
    m_statusBar->setBadge(CHAT_PAGE_INDEX, unread);
}

// 收到一条好友申请 → 只统计"别人发给我的"。
// 服务器把申请推给两边（我发出去的也会回推一份），所以必须按方向过滤：
// accountId == 当前登录账号 的是"我发出的"，不该点亮红点
void MainWindow::onFriendRequestReceived(const FriendRequestInfo& request)
{
    const QString accountId = m_backend ? m_backend->currentUserId() : QString();
    if (accountId.isEmpty() || request.accountId == accountId) {
        return;
    }

    const QString requestId = request.requestId;
    // requestId 为空没法去重，宁可不点数；已查看 / 已计过的同样跳过
    if (requestId.isEmpty()
        || m_seenFriendRequestIds.contains(requestId)
        || m_unseenFriendRequestIds.contains(requestId)) {
        return;
    }

    // 用户此刻正看着"新朋友"页，或正处于一次拉取的窗口内 → 这些申请用户马上/刚刚
    // 就在列表里看到了，直接算已查看，不点亮红点
    if (m_friendRequestPullActive || isViewingFriendRequests()) {
        m_seenFriendRequestIds.insert(requestId);
        return;
    }

    m_unseenFriendRequestIds.insert(requestId);
    refreshFriendRequestBadge();
}

// 把当前未查看的申请全部记成"已查看"：进"新朋友"页时调一次，红点随之清零。
// 之所以"移"到 seen 而不是简单清空 unseen：服务器重推同一条申请时要靠 seen 认出来，不能重复点数
void MainWindow::markFriendRequestsSeen()
{
    m_seenFriendRequestIds.unite(m_unseenFriendRequestIds);
    m_unseenFriendRequestIds.clear();
    refreshFriendRequestBadge();
}

// 红点数字 = 尚未查看的申请数（0 时 setBadge 内部会隐藏角标）
void MainWindow::refreshFriendRequestBadge()
{
    m_statusBar->setBadge(STATUS_PAGE_INDEX, m_unseenFriendRequestIds.size());
}

// "正在看新朋友页"的两个条件：当前停在联系人页，且右栏那块共用区正显示申请列表
bool MainWindow::isViewingFriendRequests() const
{
    return pageStack->currentIndex() == STATUS_PAGE_INDEX
        && m_contactWindow->detailArea()->isShowingFriendRequests();
}

// 一次拉取的收尾（成功 / 失败都走这里）：关掉"拉取窗口"，之后来的申请重新按正常规则点数
void MainWindow::onFriendRequestPullFinished()
{
    m_friendRequestPullActive = false;
}

void MainWindow::switchToPage(int index)
{
    if (index < 0 || index >= pageStack->count()) {
        return;
    }
    // 离开搜索页就把搜索框清空：下次再进来是干净的一页，不残留上次搜的词。
    // 判断用"从哪走"（currentIndex）而不是"去哪"：从搜索页走到任何一个别的页面都要清
    if (pageStack->currentIndex() == SEARCH_PAGE_INDEX && index != SEARCH_PAGE_INDEX) {
        m_searchWindow->clearSearch();
    }
    pageStack->setCurrentIndex(index);
}