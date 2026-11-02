#include "AgentWindow.h"
#include "AgentChatArea.h"
#include "AgentSessionList.h"
#include "MainBackend.h"

#include <QPushButton>
#include <QPropertyAnimation>
#include <QCursor>
#include <QEvent>
#include <QLabel>
#include <QTimer>

namespace {
constexpr int kHotZoneWidth = 16;   // 左侧感应条宽度（只覆盖窄窄一条，不挡聊天区操作）
constexpr int kHandleWidth = 14;    // 手柄宽度
constexpr int kHandleHeight = 44;   // 手柄高度
constexpr int kDrawerWidth = 240;   // 抽屉（会话列表）宽度
constexpr int kDrawerAnimMs = 220;  // 抽屉滑入/滑出时长
constexpr int kHandleGap = 4;       // 手柄与抽屉右边缘之间留的缝
}

// 智能体页：整页垂直布局，主体用智能体专属聊天区 AgentChatArea
//（默认空态 + 居中「新建会话」按钮，点开后才是消息列表 + 输入框）。
// 在此之上叠了三个"浮层"：左边缘感应条、">"手柄、会话列表抽屉
AgentWindow::AgentWindow(QWidget *parent, MainBackend* backend) : QWidget(parent), backend(backend)
{
    mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    chatArea = new AgentChatArea(this);
    chatArea->setContactName("小C");   // 顶部标题条
    mainLayout->addWidget(chatArea);

    setupOverlays();

    // 智能体这条链：UI 只认主后端，真正的存库 / 发 TCP 全由 MainBackend 往下路由。
    // ① 点「新建会话」→ 本地只生成 thread_id 并发服务器；服务器确认成功后才落库（见 ⑧）
    //    （之前是"本地先落库再发服务器"，现已改成服务器优先，失败时本地什么都不用回滚）
    connect(chatArea, &AgentChatArea::sessionCreateRequested,
            this, &AgentWindow::onSessionCreateRequested);
    // ② 用户发消息 → 服务器分配消息 ID → 落库成功 → 才发 agent_request（主后端串这条链）
    connect(chatArea, &AgentChatArea::sendMessage, this,
            [this](const QString& sessionId, const QString& content) {
        if (this->backend) {
            this->backend->sendAgentMessage(sessionId, content);
        }
    });
    // ⑦ 结束本轮（点 stop / 切会话 / 离开页面）→ 主后端发 agent_stop 并清掉落库消息 ID
    connect(chatArea, &AgentChatArea::stopAnswer,
            this, &AgentWindow::onStopAnswerRequested);
    // ③ 本地库读回的会话列表 → 抽屉；④ 抽屉里点某个会话 → 切换当前会话
    // ⑤ 点笔改名 → 主后端写库；⑥ 点叉删除 → 主后端删库（连同该会话的所有消息）
    connect(sessionList, &AgentSessionList::sessionSelected,
            this, &AgentWindow::onSessionSelected);
    connect(sessionList, &AgentSessionList::renameRequested,
            this, &AgentWindow::onSessionRenameRequested);
    connect(sessionList, &AgentSessionList::deleteRequested,
            this, &AgentWindow::onSessionDeleteRequested);

    if (backend) {
        connect(backend, &MainBackend::agentSessionsLoaded,
                this, &AgentWindow::onAgentSessionsLoaded);
        // 服务器逐段推回的回答 → 拼进同一个 AI 气泡
        connect(backend, &MainBackend::agentMessageReceived,
                chatArea, &AgentChatArea::receiveAgentChunk);
        connect(backend, &MainBackend::agentRequestFailed,
                chatArea, &AgentChatArea::receiveAgentFailure);
        // 会话历史消息回读 → 重建一屏气泡（DB 层已拼好分块，这里只管按序上屏）
        connect(backend, &MainBackend::agentMessagesLoaded,
                chatArea, &AgentChatArea::showHistory);
        // ⑧ 新建会话：服务器确认才切页；超时/失败留在原页面 + 提示
        connect(backend, &MainBackend::agentSessionCreateSucceeded,
                this, &AgentWindow::onSessionCreateSucceeded);
        connect(backend, &MainBackend::agentSessionCreateFailed,
                this, &AgentWindow::onSessionCreateFailed);
        // ⑨ 改名 / 删除：失败时本地库没动过，界面把名字回滚 / 只弹提示
        connect(backend, &MainBackend::agentSessionRenameFailed,
                this, &AgentWindow::onSessionRenameFailed);
        connect(backend, &MainBackend::agentSessionDeleteSucceeded,
                this, &AgentWindow::onSessionDeleteSucceeded);
        connect(backend, &MainBackend::agentSessionDeleteFailed,
                this, &AgentWindow::onSessionDeleteFailed);
        // 首次进来就把已有会话列表拉到抽屉里（异步，回包后再渲染）
        backend->loadAgentSessions();
    }
}

AgentWindow::~AgentWindow()
{
}

// ===== 浮层的创建：注意三个控件都 setParent(this) 但都没进布局 =====
// 进了布局就会被自动排版，没法"浮在别的控件上面"；只有手动 setGeometry 才能叠在聊天区上
//因为悬浮不进布局所以就需要我们自己管控坐标，不能用布局管理器来排版
void AgentWindow::setupOverlays()
{
    // 感应条：盖在聊天区左边缘的透明窄条，自己不画任何东西，只负责"鼠标进没进来"。
    // 为什么不给 ChatArea 装 MouseMove 过滤器：鼠标在聊天区的子控件（滚动区、消息气泡）
    // 上面时，move 事件未必冒泡到父级；而 Enter/Leave 一定会发给鼠标最上层的那个控件
    hotZone = new QWidget(this);
    hotZone->setCursor(Qt::PointingHandCursor);
    hotZone->installEventFilter(this);

    // 手柄：黑色半透明细框 + 一个 ">"
    handleBtn = new QPushButton(">", this);
    handleBtn->setFixedSize(kHandleWidth, kHandleHeight);
    handleBtn->setCursor(Qt::PointingHandCursor);
    handleBtn->setFocusPolicy(Qt::NoFocus);
    handleBtn->setStyleSheet(R"(
        QPushButton {
            border: none;
            border-radius: 4px;
            background-color: rgba(0, 0, 0, 0.28);
            color: white;
            font-size: 12px;
            font-weight: 600;
        }
        QPushButton:hover {
            background-color: rgba(0, 0, 0, 0.48);
        }
        QPushButton:pressed {
            background-color: rgba(0, 0, 0, 0.62);
        }
    )");
    handleBtn->hide();//默认隐藏：鼠标不悬浮就不该出现

    // 抽屉：会话列表。收起时整个停在聊天区左边外面（x 为负，被父控件裁掉 = 看不见），
    // 点手柄后靠动画把 x 推回 0，视觉上就是从左侧滑出来
    drawer = new QWidget(this);
    drawer->setObjectName("agentDrawer");
    drawer->setStyleSheet(R"(
        #agentDrawer {
            background-color: white;
            border-right: 1px solid #e0e0e0;
        }
    )");

    sessionList = new AgentSessionList(drawer);
    QVBoxLayout* drawerLayout = new QVBoxLayout(drawer);
    drawerLayout->setContentsMargins(0, 0, 0, 0);
    drawerLayout->setSpacing(0);
    drawerLayout->addWidget(sessionList);
    drawer->hide();

    drawerAnim = new QPropertyAnimation(drawer, "pos", this);//动画直接动抽屉的 pos 属性
    drawerAnim->setDuration(kDrawerAnimMs);
    drawerAnim->setEasingCurve(QEasingCurve::OutCubic);//先快后慢，滑出来更自然

    // ===== 信号连接 =====
    connect(handleBtn, &QPushButton::clicked, this, &AgentWindow::onToggleDrawer);
    // 动画每改一次抽屉位置，手柄就跟着挪到抽屉右边缘（不用自己写逐帧逻辑）
    connect(drawerAnim, &QPropertyAnimation::valueChanged, this, &AgentWindow::updateHandlePosition);
    connect(drawerAnim, &QPropertyAnimation::finished, this, &AgentWindow::onDrawerAnimFinished);
    // 聊天区尺寸一变（首次布局、窗口缩放、切换页面）就重新贴边摆放浮层
    chatArea->installEventFilter(this);

    layoutOverlays();
}

bool AgentWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == chatArea && event->type() == QEvent::Resize) {
        layoutOverlays();
        return QWidget::eventFilter(watched, event);//这个的作用是：确保抽屉处于正确跟随的位置
    }

    if (watched == hotZone && (event->type() == QEvent::Enter || event->type() == QEvent::Leave)) {
        // Enter/Leave 都丢给同一个函数判断，不在这里分情况写显隐
        updateHandleVisible();
    }

    return QWidget::eventFilter(watched, event);
}

void AgentWindow::onToggleDrawer()
{
    m_drawerOpen = !m_drawerOpen;

    if (m_drawerOpen) {
        drawer->show();       // 收起状态下抽屉是 hide 的，滑入前先显示出来
        hotZone->hide();      // 感应条让位：抽屉现在盖着它，留着会吃掉左边缘 16px 的点击
    }

    // 每次点击都从"当前实际位置"起跑，中途打断动画也不会跳位置
    drawerAnim->stop();
    drawerAnim->setStartValue(QPoint(drawer->x(), drawer->y()));
    const int endX = m_drawerOpen ? chatArea->x() : chatArea->x() - kDrawerWidth;
    drawerAnim->setEndValue(QPoint(endX, drawer->y()));
    drawerAnim->start();

    updateHandleVisible();
}

void AgentWindow::onDrawerAnimFinished()
{
    if (!m_drawerOpen) {
        drawer->hide();       // 收回到位，彻底藏掉（x 是负的本来就看不见，这里避免残留遮挡）
        hotZone->show();      // 抽屉走了，感应条回来值班
    }
    updateHandleVisible();
}

// 手柄位置 = 抽屉右边缘 + 一条缝：抽屉收起时（x=-240）正好回到聊天区左边缘，
// 拉出后跟着挪到抽屉右侧 —— 既是"把手"，也顺手当收起按钮用
void AgentWindow::updateHandlePosition()
{
    handleBtn->move(drawer->x() + drawer->width() + kHandleGap, handleBtn->y());
    handleBtn->raise();
}

// 手柄显隐的唯一判断入口。三种情况：
//   ① 抽屉拉出 → 常显（它同时是收起按钮）
//   ② 抽屉收起 + 鼠标在感应条区域内 → 显示
//   ③ 抽屉收起 + 鼠标不在 → 隐藏
void AgentWindow::updateHandleVisible()
{
    if (m_drawerOpen) {
        handleBtn->show();
        handleBtn->raise();
        return;
    }

    if (mouseInsideHotZone()) {
        handleBtn->show();
        handleBtn->raise();
    } else {
        handleBtn->hide();
    }
}

// 用全局光标位置反查，而不是用 underMouse()：鼠标从感应条挪到手柄上时，
// Qt 会先给感应条发 Leave，那一刻 underMouse() 还没更新完，会误判成"已离开"→ 手柄一闪一闪
bool AgentWindow::mouseInsideHotZone() const
{
    return hotZone->rect().contains(hotZone->mapFromGlobal(QCursor::pos()));
    //`QCursor::pos()`拿到**屏幕全局坐标**（整个显示器坐标系下鼠标的位置，和任何控件无关）
    //`hotZone->mapFromGlobal(全局点)``mapFromGlobal`：**把屏幕全局坐标 → 转换成 hotZone 自身的局部坐标系**
    //`rect().contains(局部点)`：**判断局部坐标是否在矩形区域内**
    //如果在区域内，返回 true；否则返回 false
    //`hotZone->rect()`hotZone 本地矩形：原点 (0,0)，宽高就是控件自身大小，没有偏移。
    //其实就是判断有没有在感应条区域内，而且会有这样一个bug因为手柄和感应条重叠，如果用leave和enter事件，会误判成"已离开"感应区域
    //手柄被隐藏但是隐藏后，鼠标在感应条区域内，也会误判成"已离开"感应区域，导致手柄一闪一闪
}

void AgentWindow::layoutOverlays()
{
    const QRect area = chatArea->geometry();//geometry()：拿到相对于它的父控件的矩形area.x()、area.y()：左上角，是父控件坐标系下的坐标
    if (area.isEmpty()) {
        return;//布局还没算出来，等下一次 Resize 事件
    }

    // 感应条：贴聊天区左边缘，高度与聊天区一致
    hotZone->setGeometry(area.x(), area.y(), kHotZoneWidth, area.height());
    hotZone->raise();

    // 手柄：垂直居中，x 交给 updateHandlePosition 跟着抽屉走
    handleBtn->move(handleBtn->x(), area.y() + (area.height() - kHandleHeight) / 2);
    if (handleBtn->isVisible()) {
        handleBtn->raise();
    }

    // 抽屉：跟随聊天区高度；x 分两种情况——动画没跑时才直接定位，
    // 正在滑动中就让动画自己管 x，否则会把动画打断成瞬移
    const int drawerX = m_drawerOpen ? area.x() : area.x() - kDrawerWidth;
    if (drawerAnim->state() == QAbstractAnimation::Running) {
        drawer->setGeometry(drawer->x(), area.y(), kDrawerWidth, area.height());
    } else {
        drawer->setGeometry(drawerX, area.y(), kDrawerWidth, area.height());
    }

    updateHandlePosition();
    if (handleBtn->isVisible()) {
        handleBtn->raise();
    }
}

// 智能体会话列表回包（本地库读 / 新建会话后重拉）→ 抽屉里的 AgentSessionList
void AgentWindow::onAgentSessionsLoaded(const QList<ConversationInfo>& sessions)
{
    sessionList->setSessions(sessions);
    // 新建会话后聊天区已经切到新会话，这里把抽屉的选中态跟当前会话对齐
    sessionList->setCurrentSessionId(chatArea->currentSessionId());

    // 标题条上的名字以库里的为准：新建的会话叫"新会话"，改过名的也在这里刷新；
    // 当前会话已经被删掉（列表里查不到）就把标题清空
    QString title;
    const QString currentId = chatArea->currentSessionId();
    for (const ConversationInfo& session : sessions) {
        if (session.id == currentId) {
            title = session.name;
            break;
        }
    }
    chatArea->setSessionTitle(title);
}

// 抽屉里点某个会话：切到它（清掉当前这一屏消息，会话 ID 换成选中的那个），
// 再向本地库拉这个会话最近的历史消息。回包是异步的，靠 showHistory 里
// "sessionId 对不上就丢弃"兜住用户又切走的情况
void AgentWindow::onSessionSelected(const QString& sessionId, const QString& sessionName)
{
    // 用户已经主动切走了：那颗"新建会话"的回包就算成功，也不再抢页面
    m_pendingCreateSessionId.clear();

    chatArea->openSession(sessionId);
    // openSession 里拿不到名字（名字在抽屉的行上），这里补上标题条
    chatArea->setSessionTitle(sessionName);

    if (backend) {
        backend->loadAgentMessages(sessionId);
    }
}

// 改名：只上抛，写库由主后端排到后台 DB 线程（写完它会自动重拉列表，界面随之刷新）
void AgentWindow::onSessionRenameRequested(const QString& sessionId, const QString& title)
{
    if (!backend) {
        return;
    }
    backend->renameAgentSession(sessionId, title);
}

// 删除：先问服务器，服务器确认后主后端才真删本地（消息一旦删掉找不回来，
// 所以不能"本地先删"）。确认前界面什么都不动，等 onSessionDeleteSucceeded
void AgentWindow::onSessionDeleteRequested(const QString& sessionId)
{
    if (!backend) {
        return;
    }
    backend->deleteAgentSession(sessionId);
}

// 新建第一段：记住这次待确认的会话 ID（用户中途切走会清掉），
// 再把"发服务器等回执"整件活交给主后端（本地库等服务器确认后才写）
void AgentWindow::onSessionCreateRequested(const QString& sessionId, const QString& title)
{
    if (!backend) {
        chatArea->onSessionCreateFailed();   // 没有后端就当场回滚，别让按钮一直灰着
        return;
    }
    m_pendingCreateSessionId = sessionId;
    backend->createAgentSession(sessionId, title);
}

// 新建第二段（服务器确认）：只有这次创建还是"最新那一次"才跳页，
// 用户已经点了别的会话就只把按钮点亮，页面不动
void AgentWindow::onSessionCreateSucceeded(const QString& sessionId, const QString& title)
{
    const bool stillWaiting = (m_pendingCreateSessionId == sessionId);
    m_pendingCreateSessionId.clear();

    if (stillWaiting) {
        chatArea->onSessionCreateSucceeded(sessionId, title);
    } else {
        chatArea->onSessionCreateFailed();   // 只负责把按钮点亮（名字不叫"失败"是因为它只管状态）
    }
}

void AgentWindow::onSessionCreateFailed(const QString& message)
{
    m_pendingCreateSessionId.clear();
    chatArea->onSessionCreateFailed();
    showToast(QString("新建会话失败：%1").arg(message));
}

// 改名失败：本地库没动过，把行上已经乐观改成新名字的显示回滚成旧名字
void AgentWindow::onSessionRenameFailed(const QString& sessionId, const QString& message)
{
    sessionList->revertTitle(sessionId);
    showToast(QString("重命名失败：%1").arg(message));
}

// 删除确认：如果删的正是当前打开的会话，聊天区回空态（不能停在一个已经不存在的会话上）。
// 本地库由主后端删除并重拉列表
void AgentWindow::onSessionDeleteSucceeded(const QString& sessionId)
{
    if (chatArea->currentSessionId() == sessionId) {
        chatArea->resetToEmpty();
    }
}

void AgentWindow::onSessionDeleteFailed(const QString& sessionId, const QString& message)
{
    Q_UNUSED(sessionId);
    showToast(QString("删除会话失败：%1").arg(message));
}

// 小提示：底部居中的深色圆角条，5 秒后自己消失（不打断操作，也不用点确定）
void AgentWindow::showToast(const QString& text)
{
    if (m_toast == nullptr) {
        m_toast = new QLabel(this);
        m_toast->setObjectName("agentToast");
        // 纯提示条：不吃鼠标事件，免得挡住底下的输入框/按钮
        m_toast->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_toast->setStyleSheet(R"(
            QLabel#agentToast {
                background-color: rgba(0, 0, 0, 0.75);
                color: white;
                border-radius: 8px;
                padding: 8px 16px;
                font-size: 13px;
            }
        )");
        m_toastTimer = new QTimer(this);
        m_toastTimer->setSingleShot(true);
        connect(m_toastTimer, &QTimer::timeout, this, &AgentWindow::onToastTimeout);
    }

    m_toast->setText(text);
    m_toast->adjustSize();
    // 底部居中、离底边 70px：不压输入框那颗发送按钮
    m_toast->move((width() - m_toast->width()) / 2, height() - m_toast->height() - 70);
    m_toast->show();
    m_toast->raise();
    m_toastTimer->start(5000);
}

void AgentWindow::onToastTimeout()
{
    if (m_toast != nullptr) {
        m_toast->hide();
    }
}

// 结束本轮：聊天区已经把本地状态（流式气泡 ID、按钮外观）收拾干净了，
// 这里只负责把"停止"这件事交给主后端（发 agent_stop + 清落库消息 ID）
void AgentWindow::onStopAnswerRequested(const QString& sessionId)
{
    if (!backend) {
        return;
    }
    backend->stopAgentMessage(sessionId);
}

// 离开智能体页（pageStack 切走会隐藏本页）：正在回答就先停掉。
// 注意最小化窗口也会触发 hide，这里同样会停——可以接受：
// 界面上已经看不到回答了，继续让服务器白跑没有意义
void AgentWindow::hideEvent(QHideEvent* event)
{
    chatArea->stopCurrentAnswer();
    QWidget::hideEvent(event);
}