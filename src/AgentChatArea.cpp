#include "AgentChatArea.h"
#include "AgentMessageItem.h"
#include "ChatInput.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QDateTime>
#include <QUuid>
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QFontMetrics>
#include <QResizeEvent>

namespace {
// 把黑色 SVG 图标染成指定颜色：先按尺寸渲染成 pixmap，再用 SourceIn 覆盖一层色块，
// 只保留原图的 alpha 通道 → 图标颜色就是我们要的前景色（资源文件本身不用改）
QIcon createColoredIcon(const QString& path, int size, const QColor& color)
{
    QPixmap pixmap = QIcon(path).pixmap(QSize(size, size));
    if (pixmap.isNull()) {
        return QIcon(path);
    }
    QPainter painter(&pixmap);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(pixmap.rect(), color);
    painter.end();
    return QIcon(pixmap);
}
}

AgentChatArea::AgentChatArea(QWidget *parent) : QWidget(parent)
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ========== 标题条（空态/聊天态都显示）==========
    QWidget* headerWidget = new QWidget(this);
    headerWidget->setStyleSheet(R"(
        background-color: white;
        border-radius: 5px;
    )");
    QHBoxLayout* headerLayout = new QHBoxLayout(headerWidget);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(0);

    m_headerLabel = new QLabel(headerWidget);
    m_headerLabel->setStyleSheet(R"(
        background-color: transparent;
        color: black;
        padding: 15px 20px;
        font-size: 16px;
        font-weight: 600;
    )");
    headerLayout->addWidget(m_headerLabel);
    mainLayout->addWidget(headerWidget);

    // ========== 当前会话标题条（紧挨在顶部"小C"标题下面）==========
    // 结构：左边占位（和右侧按钮等宽）+ 中间会话名 + 右边"新建会话"图标按钮。
    // 左边留这个等宽空位是为了让中间的名字在整条里真正居中（否则会被右侧按钮挤偏）
    QWidget* sessionBar = new QWidget(this);
    sessionBar->setObjectName("agentSessionBar");
    // 普通 QWidget 要自己画背景/边框必须开 WA_StyledBackground（同消息气泡那个坑）
    sessionBar->setAttribute(Qt::WA_StyledBackground, true);
    sessionBar->setFixedHeight(40);
    sessionBar->setStyleSheet(R"(
        #agentSessionBar {
            background-color: #ffffff;
            border-bottom: 1px solid #eceff3;
        }
    )");
    QHBoxLayout* sessionBarLayout = new QHBoxLayout(sessionBar);
    sessionBarLayout->setContentsMargins(12, 0, 12, 0);
    sessionBarLayout->setSpacing(8);

    const int kBarBtnSize = 28;   // 右侧按钮与左侧占位的边长
    QWidget* leftPlaceholder = new QWidget(sessionBar);
    leftPlaceholder->setFixedSize(kBarBtnSize, kBarBtnSize);
    leftPlaceholder->setStyleSheet("background-color: transparent;");
    sessionBarLayout->addWidget(leftPlaceholder);

    m_sessionTitleLabel = new QLabel(sessionBar);
    m_sessionTitleLabel->setAlignment(Qt::AlignCenter);
    m_sessionTitleLabel->setStyleSheet("background-color: transparent; color: #333333;");
    // 字号/加粗用代码定（不写 QSS）：省略号是按 QFontMetrics 量的，字号必须先落到字体上
    QFont titleFont = m_sessionTitleLabel->font();
    titleFont.setPixelSize(14);
    titleFont.setBold(true);
    m_sessionTitleLabel->setFont(titleFont);
    // 水平 SizePolicy 用 Ignored：长名字不来撑宽标题条，超出部分交给省略号处理
    m_sessionTitleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    sessionBarLayout->addWidget(m_sessionTitleLabel, 1);

    m_newSessionBtn = new QPushButton(sessionBar);
    m_newSessionBtn->setFixedSize(kBarBtnSize, kBarBtnSize);
    m_newSessionBtn->setCursor(Qt::PointingHandCursor);
    m_newSessionBtn->setFocusPolicy(Qt::NoFocus);
    m_newSessionIcon = createColoredIcon(":/res/icon/session.svg", 18, QColor("#4a90d9"));
    m_newSessionIconDisabled = createColoredIcon(":/res/icon/session.svg", 18, QColor("#b9c9d8"));
    m_newSessionBtn->setIcon(m_newSessionIcon);
    m_newSessionBtn->setIconSize(QSize(18, 18));
    m_newSessionBtn->setToolTip("新建会话");
    m_newSessionBtn->setStyleSheet(R"(
        QPushButton {
            border: none;
            border-radius: 14px;
            background-color: transparent;
        }
        QPushButton:hover {
            background-color: #eef3f8;
        }
        QPushButton:pressed {
            background-color: #e1ebf5;
        }
    )");
    connect(m_newSessionBtn, &QPushButton::clicked, this, &AgentChatArea::requestNewSession);
    sessionBarLayout->addWidget(m_newSessionBtn);

    mainLayout->addWidget(sessionBar);

    m_stack = new QStackedWidget(this);
    mainLayout->addWidget(m_stack, 1);

    // ========== 页 0：空态（居中「新建会话」按钮）==========
    QWidget* emptyPage = new QWidget(m_stack);
    emptyPage->setStyleSheet("background-color: #e8eef3;");
    QVBoxLayout* emptyLayout = new QVBoxLayout(emptyPage);
    emptyLayout->setContentsMargins(0, 0, 0, 0);
    emptyLayout->setSpacing(0);

    // 存成成员：创建请求在飞期间要和标题条右侧那颗一起置灰
    m_emptyNewBtn = new QPushButton("新建会话", emptyPage);
    m_emptyNewBtn->setCursor(Qt::PointingHandCursor);
    m_emptyNewBtn->setFixedSize(168, 44);
    m_emptyNewBtn->setFocusPolicy(Qt::NoFocus);
    // 图标和文字是同一个按钮里的整体：左边"新建"图标，右边"新建会话"
    m_emptyNewBtn->setIcon(createColoredIcon(":/res/icon/session.svg", 20, QColor("#ffffff")));
    m_emptyNewBtn->setIconSize(QSize(20, 20));
    m_emptyNewBtn->setStyleSheet(R"(
        QPushButton {
            border: none;
            /* 高 44 → 半径取 22：左右两端正好是半圆，上下是直线 */
            border-radius: 22px;
            background-color: #4a90d9;
            color: white;
            font-size: 15px;
            font-weight: 600;
            padding: 0 20px;
            text-align: center;
        }
        QPushButton:hover {
            background-color: #3a80c9;
        }
        QPushButton:pressed {
            background-color: #356ea8;
        }
        /* 等服务器回执期间：和标题条右侧那颗一起变灰，让人一眼看出"点了在等" */
        QPushButton:disabled {
            background-color: #b7c8d8;
            color: #ffffff;
        }
    )");
    connect(m_emptyNewBtn, &QPushButton::clicked, this, &AgentChatArea::requestNewSession);

    emptyLayout->addStretch(1);
    emptyLayout->addWidget(m_emptyNewBtn, 0, Qt::AlignHCenter);
    emptyLayout->addStretch(1);
    m_stack->addWidget(emptyPage);

    // ========== 页 1：聊天态（消息滚动区 + 输入框）==========
    QWidget* chatPage = new QWidget(m_stack);
    QVBoxLayout* chatLayout = new QVBoxLayout(chatPage);
    chatLayout->setContentsMargins(0, 0, 0, 0);
    chatLayout->setSpacing(0);

    m_scrollArea = new QScrollArea(chatPage);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setStyleSheet(R"(
        QScrollArea {
            border: none;
            background-color: #e8eef3;
        }
        QScrollBar:vertical {
            width: 0px;
            background-color: transparent;
        }
        QScrollBar::handle:vertical {
            background-color: transparent;
            border-radius: 3px;
        }
        QScrollArea:hover QScrollBar:vertical {
            width: 6px;
        }
        QScrollArea:hover QScrollBar::handle:vertical {
            background-color: #c0c0c0;
        }
        QScrollArea:focus QScrollBar:vertical {
            width: 6px;
        }
        QScrollArea:focus QScrollBar::handle:vertical {
            background-color: #c0c0c0;
        }
    )");

    QWidget* messagesWidget = new QWidget();
    m_messagesLayout = new QVBoxLayout(messagesWidget);
    m_messagesLayout->setContentsMargins(0, 10, 0, 10);
    m_messagesLayout->setSpacing(0);
    m_messagesLayout->addStretch(1);
    m_scrollArea->setWidget(messagesWidget);

    // 输入框只向上长：scrollArea stretch=1 在前、ChatInput stretch=0 在最后
    // （同 ChatArea，底边锚死窗口底，长高的部分全从滚动区扣）
    chatLayout->addWidget(m_scrollArea, 1);

    m_input = new ChatInput(chatPage);
    m_input->setStyleSheet("background-color: white; border-top: 1px solid #e0e0e0;");
    chatLayout->addWidget(m_input, 0);
    connect(m_input, &ChatInput::sendMessage, this, &AgentChatArea::onInputSend);
    // 回答中那颗按钮变成了 stop 图标，点它的语义是"停止本轮"（见 ChatInput::stopRequested）
    connect(m_input, &ChatInput::stopRequested, this, &AgentChatArea::onStopClicked);

    m_stack->addWidget(chatPage);

    // 默认停在空态：由用户点「新建会话」后才展开聊天态
    m_stack->setCurrentIndex(0);
}

void AgentChatArea::setContactName(const QString& name)
{
    m_headerLabel->setText(name);
}

void AgentChatArea::setSendingState(bool sending)
{
    // 记下当前是"发送"还是"停止"：后面发请求时要按这个状态决定发哪种包
    m_sending = sending;
    // 按钮实际长在 ChatInput 里，这里只做转发（本类管状态，输入框只管画）
    m_input->setSendingState(sending);
}

// 结束当前这一轮：这是"停止态"的唯一收口，三个触发点都走它——
// ① 用户点 stop 按钮 / ② 切会话（openSession、startNewSession、resetToEmpty 都会先把旧会话停掉）
// ③ 离开智能体页（AgentWindow::hideEvent 调 stopCurrentAnswer）
// 顺序很关键：先取旧会话 ID 再清状态、再喊信号，否则切完会话 signal 里带的就成新会话了
void AgentChatArea::stopAnswerIfSending()
{
    if (!m_sending) {
        return;
    }

    const QString sessionId = m_sessionId;
    m_streamMessageId.clear();
    setSendingState(false);
    emit stopAnswer(sessionId);
}

void AgentChatArea::stopCurrentAnswer()
{
    stopAnswerIfSending();
}

void AgentChatArea::onStopClicked()
{
    // 停止只结束这一轮，输入框里已经打的字原样保留（用户可能只是想改问题了）
    stopAnswerIfSending();
}

// 当前会话名：存原始名字再按标签宽度做省略；
// 悬停时用 tooltip 给全名（标题条窄的时候长名字也能看全）
void AgentChatArea::setSessionTitle(const QString& title)
{
    m_sessionTitle = title;
    m_sessionTitleLabel->setToolTip(title);
    updateSessionTitleText();
}

void AgentChatArea::updateSessionTitleText()
{
    const int available = m_sessionTitleLabel->width();
    if (available <= 0) {
        // 布局还没给标签定尺寸（首次 show 之前），先把原名挂上，等 resize 再省略
        m_sessionTitleLabel->setText(m_sessionTitle);
        return;
    }
    const QFontMetrics fm(m_sessionTitleLabel->font());
    m_sessionTitleLabel->setText(fm.elidedText(m_sessionTitle, Qt::ElideRight, available));
}

void AgentChatArea::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateSessionTitleText();   // 宽度变了，省略号要跟着重算
}

// 点「新建会话」：一条带回执的链路——本地不太事前落库，先发服务器；
// 这期间按钮置灰、页面不动，服务器 5 秒内确认才"本地入库 + 切进新会话"
//（onSessionCreateSucceeded），超时/失败就留在原页面（onSessionCreateFailed）。
// 这样不会出现"客户端有、服务器没有"的会话
void AgentChatArea::requestNewSession()
{
    if (m_createPending) {
        return;   // 上一件创建请求还没回执，连点直接忽略（按钮也是灰的，双保险）
    }
    m_createPending = true;
    setNewSessionEnabled(false);

    // 会话 ID = threadId，由客户端本地生成（服务器只认 (userId, threadId) 联合主键，
    // 不会下发 ID）；用 UUID 保证唯一，userId 由会话操作包单独携带
    const QString sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    emit sessionCreateRequested(sessionId, QStringLiteral("新会话"));
}

void AgentChatArea::onSessionCreateSucceeded(const QString& sessionId, const QString& title)
{
    m_createPending = false;
    setNewSessionEnabled(true);

    // 切进新会话：openSession 顺带把上一个会话的"回答中"停掉、清屏、换 ID
    openSession(sessionId);
    setSessionTitle(title.isEmpty() ? QStringLiteral("新会话") : title);
}

void AgentChatArea::onSessionCreateFailed()
{
    // 留在当前页面不动（可能还是空态页）：只把按钮点亮，等用户重试
    m_createPending = false;
    setNewSessionEnabled(true);
}

void AgentChatArea::setNewSessionEnabled(bool enabled)
{
    // 标题条那颗是纯图标按钮：QSS 只能改背景，图标颜色要自己换两档
    m_newSessionBtn->setEnabled(enabled);
    m_newSessionBtn->setIcon(enabled ? m_newSessionIcon : m_newSessionIconDisabled);
    // 空态那颗是文字按钮：按钮自己的 QSS 里有 :disabled 规则，改 enabled 就够了
    if (m_emptyNewBtn != nullptr) {
        m_emptyNewBtn->setEnabled(enabled);
    }
}

void AgentChatArea::openSession(const QString& sessionId)
{
    if (sessionId.isEmpty()) {
        return;
    }
    // 切会话 = 上一轮对话结束：上一个会话的回答不再拼到这一屏，也不该再让服务器白跑
    stopAnswerIfSending();

    m_sessionId = sessionId;
    m_streamMessageId.clear();
    clearMessages();
    m_stack->setCurrentIndex(1);
    // 这里还拿不到会话名（名字在抽屉的行上），先清空避免显示上一个会话的名字，
    // 由 AgentWindow 紧接着调 setSessionTitle 补上
    setSessionTitle(QString());
}

// 回空态：会话被删（或删的正是当前会话）时用。
// 清 ID 很关键——否则以后收到同一会话的流式回答还会被当成"当前会话"拼到屏幕上
void AgentChatArea::resetToEmpty()
{
    // 会话被删 / 无会话可用：先停掉这一轮（停止包要带还没清掉的会话 ID）
    stopAnswerIfSending();

    m_sessionId.clear();
    m_streamMessageId.clear();
    clearMessages();
    setSessionTitle(QString());
    m_stack->setCurrentIndex(0);
}

QString AgentChatArea::addMessage(const QString& content, bool isSelf)
{
    const QString id = newMessageId();
    appendMessageWidget(content, isSelf, id);
    scrollToBottom();
    return id;
}

// 消息 ID 必须是全局唯一的：库里主键是 (session_id, message_id, chunk_seq)，
// 而且 AI 回答是分块写的（问题 3 会把 ID 一起落库）。
// 原来用 ++m_seq 的序号：软件一重启计数从 1 重来，新消息会跟上次运行的旧行撞主键，
// INSERT OR REPLACE 直接把历史覆盖掉 → 改用 UUID
QString AgentChatArea::newMessageId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// 输入框发出内容：先本地把自己的气泡上屏（发送方视觉上立刻有反馈），
// 再把会话 ID + 内容抛给上层——服务器分配消息 ID → 落库 → 才发 agent_request
void AgentChatArea::onInputSend(const QString& content)
{
    if (content.trimmed().isEmpty()) {
        return;
    }
    // 本轮还在回答（按钮=停止态）：这次点击是"停止"语义，不是发新提问。
    // 停止包等服务器协议补上再接；现在先什么都不做，免得把上一轮打断成一次脏请求
    if (m_sending) {
        return;
    }
    // 没有会话就不发：正常流程进不到这一步（空态页没有输入框，
    // 且新建会话要等服务器回执才切页）——留着当脏状态防御
    if (m_sessionId.isEmpty()) {
        return;
    }

    // 发新问题前结束上一条回答的拼接：即使服务器没给 done 标记，
    // 下一次回答也不会被拼进上一轮那个气泡里
    m_streamMessageId.clear();

    // 气泡上那个 ID 只用于本屏渲染；真正落库的消息 ID 由服务器分配（见 MainBackend）
    appendMessageWidget(content, true, newMessageId());
    scrollToBottom();
    // 先切"回答中"再发：发送被拒（同会话在跑 / 未连接）时，失败回调会同步把按钮切回来
    setSendingState(true);
    emit sendMessage(m_sessionId, content);
}

// 服务器回答逐段到达：第一段先开一个 AI 气泡，之后都拼进同一个气泡；
// finished=true 时结束本轮（下次回答再开新气泡）
void AgentChatArea::receiveAgentChunk(const QString& sessionId, const QString& content, bool finished)
{
    if (sessionId != m_sessionId) {
        return;   // 回答属于别的会话（比如用户已经新建/切走），当前页面不显示
    }

    if (m_streamMessageId.isEmpty()) {
        m_streamMessageId = newMessageId();
        appendMessageWidget(QString(), false, m_streamMessageId);
    }
    if (!content.isEmpty()) {
        appendStreamChunk(m_streamMessageId, content);
        scrollToBottom();
    }
    if (finished) {
        m_streamMessageId.clear();
        setSendingState(false);   // 本轮结束，按钮从 stop 图标回到"发送"
    }
}

void AgentChatArea::receiveAgentFailure(const QString& sessionId, const QString& message)
{
    if (sessionId != m_sessionId) {
        return;
    }
    m_streamMessageId.clear();
    setSendingState(false);   // 本轮失败即结束，按钮回"发送"
    addMessage(QString("请求失败：%1").arg(message), false);
}

// 历史回读：DB 层已经把分块按 message_id 拼成整条、并按时间正序排好，
// 这里只需要从上往下补齐气泡（id 直接用库里的消息 ID，后续流式/HITL 也认这个 ID）。
// 空列表直接返回：点亮会话时 openSession 已经清过屏了，此时再清一次反而会
// 把"历史还没回来、用户已经发出的那条"一起清掉
void AgentChatArea::showHistory(const QString& sessionId, const QList<AgentMessageInfo>& messages)
{
    if (sessionId != m_sessionId || messages.isEmpty()) {
        return;
    }

    m_streamMessageId.clear();
    clearMessages();
    for (const AgentMessageInfo& message : messages) {
        appendMessageWidget(message.content, message.isSelf, message.id);
    }
    scrollToBottom();
}

void AgentChatArea::appendMessageWidget(const QString& content, bool isSelf, const QString& id)
{
    AgentMessageItem* item = new AgentMessageItem(content, isSelf, this);
    m_items.insert(id, item);
    // 插在末尾 stretch 之前（count()-1 就是 stretch 的位置）
    m_messagesLayout->insertWidget(m_messagesLayout->count() - 1, item);
}

void AgentChatArea::appendStreamChunk(const QString& messageId, const QString& chunk)
{
    auto it = m_items.find(messageId);
    if (it == m_items.end()) {
        return;   // 气泡已经不在了（清空会话 / ID 不匹配），直接丢弃
    }
    it.value()->appendStreamChunk(chunk);
}

void AgentChatArea::setHitlActions(const QString& messageId, const QStringList& actions)
{
    auto it = m_items.find(messageId);
    if (it == m_items.end()) {
        return;
    }
    it.value()->setHitlActions(actions);
}

void AgentChatArea::clearMessages()
{
    QLayoutItem* layoutItem;
    while ((layoutItem = m_messagesLayout->takeAt(0)) != nullptr) {
        // takeAt是先摘除 widget从布局里拿出去，再用delete删除 layout item
        delete layoutItem->widget();
        delete layoutItem;
    }
    m_items.clear();   // 同步清映射，避免悬挂指针
    m_messagesLayout->addStretch(1);
}

void AgentChatArea::scrollToBottom()
{
    // 延迟到布局算完再滚，否则拿到的 maximum 还是旧值
    QTimer::singleShot(10, this, [this]() {
        QScrollBar* scrollBar = m_scrollArea->verticalScrollBar();
        if (scrollBar == nullptr) {
            return;
        }
        scrollBar->setValue(scrollBar->maximum());
    });
}
