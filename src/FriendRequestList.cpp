#include "FriendRequestList.h"
#include <QStackedWidget>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QPushButton>
#include <QDateTime>

namespace {
// 尺寸常量集中在这里：造卡片、算 item 高度两处都要用，引用同一份，改一处就够
constexpr int kHeaderHeight = 50;      // 与 ContactWindow 左栏标题栏同高，两栏顶边对齐
constexpr int kCardHeight   = 68;      // 白卡片本身的高度（不含卡片之间的空隙）
constexpr int kCardGap      = 10;      // 卡片之间的空隙
constexpr int kCardMargin   = 28;      // 卡片离列表左右边缘的留白（照 QQ：卡片不贴边）
constexpr int kAvatarSize   = 40;
constexpr int kButtonWidth  = 58;
constexpr int kButtonHeight = 26;

// 卡片本体用 QFrame 而不是裸 QWidget：QWidget 想让它支持 QSS 背景必须重写 paintEvent
//（或加 WA_StyledBackground），QFrame 天生就吃 background/border-radius
const char* kCardStyle = R"(
    QFrame#requestCard {
        background-color: white;
        border-radius: 8px;
    }
    QFrame#requestCard:hover {
        background-color: #fafafa;
    }
)";

// 同意：主按钮，配色沿用登录页那套蓝色
const char* kAcceptButtonStyle = R"(
    QPushButton {
        background-color: #4a90d9;
        color: white;
        font-size: 12px;
        border: none;
        border-radius: 6px;
    }
    QPushButton:hover    { background-color: #3a80c9; }
    QPushButton:pressed  { background-color: #2f70b5; }
)";

// 拒绝：次要按钮，浅灰底不抢眼
const char* kRejectButtonStyle = R"(
    QPushButton {
        background-color: #f2f2f2;
        color: #666666;
        font-size: 12px;
        border: 1px solid #e0e0e0;
        border-radius: 6px;
    }
    QPushButton:hover    { background-color: #e8e8e8; }
    QPushButton:pressed  { background-color: #dcdcdc; }
)";

// 申请日期照 QQ 写成 yyyy/MM/dd（sendTime 是 ISODate；解析不出就原样显示，别把时间吞掉）
QString formatSendDate(const QString& isoTime)
{
    const QDateTime time = QDateTime::fromString(isoTime, Qt::ISODate);
    if (!time.isValid()) {
        return isoTime;
    }
    return time.toString("yyyy/MM/dd");
}

// 卡片里那些小字统一用这个字号/颜色，免得每处各写一遍对不上
const char* kMetaLabelStyle = "font-size: 12px; color: #999;";
}  // namespace

FriendRequestList::FriendRequestList(QWidget *parent) : QWidget(parent)
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ===== 顶部标题栏（与 ContactWindow 左栏的"联系人"标题栏同款）=====
    QWidget* headerBar = new QWidget(this);
    headerBar->setObjectName("friendRequestHeaderBar");
    headerBar->setFixedHeight(kHeaderHeight);
    headerBar->setStyleSheet(R"(
        #friendRequestHeaderBar {
            background-color: white;
            border-bottom: 1px solid #e0e0e0;
        }
    )");

    QHBoxLayout* headerLayout = new QHBoxLayout(headerBar);
    headerLayout->setContentsMargins(18, 0, 18, 0);
    headerLayout->setSpacing(0);

    QLabel* headerLabel = new QLabel("新朋友", headerBar);
    headerLabel->setStyleSheet("color: black; font-size: 16px; font-weight: 600;");
    headerLayout->addWidget(headerLabel);
    headerLayout->addStretch();

    layout->addWidget(headerBar);

    // ===== 操作失败提示条 =====
    // 默认 hide()：没出错时它就完全不占位，不会在标题栏下面留一道空条
    m_tipLabel = new QLabel(this);
    m_tipLabel->setFixedHeight(28);
    m_tipLabel->setStyleSheet(R"(
        QLabel {
            background-color: rgba(224, 91, 91, 0.10);
            color: rgba(224, 91, 91, 0.9);
            font-size: 12px;
            padding-left: 18px;
        }
    )");
    m_tipLabel->hide();
    layout->addWidget(m_tipLabel);

    // ===== 提示页 / 列表页 =====
    m_stack = new QStackedWidget(this);

    m_placeholderLabel = new QLabel(m_stack);
    m_placeholderLabel->setAlignment(Qt::AlignCenter);
    m_placeholderLabel->setWordWrap(true);
    m_placeholderLabel->setStyleSheet("color: #999; font-size: 13px; background-color: #f5f5f5;");
    m_stack->addWidget(m_placeholderLabel);

    m_listWidget = new QListWidget(m_stack);
    m_listWidget->setFocusPolicy(Qt::NoFocus);                            // 不抢焦点
    m_listWidget->setFrameShape(QFrame::NoFrame);                         // 关掉 QFrame 原生边框
    m_listWidget->setSelectionMode(QAbstractItemView::NoSelection);       // 卡片自带按钮，条目本身不需要选中态
    m_listWidget->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    // 列表底是浅灰，条目自身全透明——白卡片的圆角/留白都画在 itemWidget 里那层 QFrame 上。
    // 若这里给 ::item 画白底，卡片外的"留白区"也会变白，就没有 QQ 那种浮起来的感觉了
    m_listWidget->setStyleSheet(R"(
        QListWidget {
            border: none;
            background-color: #f5f5f5;
            outline: none;
            padding: 6px 0;
        }
        QListWidget::item {
            background-color: transparent;
            border: none;
            outline: none;
        }
        QListWidget::item:hover,
        QListWidget::item:selected {
            background-color: transparent;
            border: none;
            outline: none;
        }
    )");
    m_stack->addWidget(m_listWidget);

    layout->addWidget(m_stack, 1);

    // 默认停在提示页：还没点过"新朋友"，列表就是空的
    showPlaceholder("暂无新的朋友申请");
}

void FriendRequestList::setCurrentAccountId(const QString& accountId)
{
    m_currentAccountId = accountId;
    rebuildList();   // 方向（决定右侧给按钮还是给状态词）依赖它，变了就重画一遍
}

void FriendRequestList::setLoading()
{
    // 每次重新拉取都从零开始 —— 服务器会重推全部申请，旧数据留着只会滚出重复项
    m_requests.clear();
    rebuildList();
    m_tipLabel->hide();
    showPlaceholder("正在加载…");
}

void FriendRequestList::setError(const QString& message)
{
    // 失败就是失败：不显示空列表，否则用户会以为"真的没人加我"
    showPlaceholder(message.isEmpty() ? QStringLiteral("好友申请拉取失败") : message);
}

void FriendRequestList::finishLoading()
{
    if (m_requests.isEmpty()) {
        showPlaceholder("暂无新的朋友申请");
        return;
    }
    rebuildList();
    m_stack->setCurrentWidget(m_listWidget);
}

void FriendRequestList::addRequest(const FriendRequestInfo& request)
{
    // 去重：同一条申请（requestId 相同）服务器重推时不再收第二遍。
    // requestId 是服务器生成的，正常不会为空；真为空时没法判断是不是同一条，只能当新条目收下
    if (!request.requestId.isEmpty() && indexOfRequest(request.requestId) >= 0) {
        return;
    }
    m_requests.append(request);
    m_tipLabel->hide();   // 收到新数据说明链路通了，上一次的错误提示可以撤了
    rebuildList();
    m_stack->setCurrentWidget(m_listWidget);
}

void FriendRequestList::removeRequest(const QString& requestId)
{
    const int index = indexOfRequest(requestId);
    if (index < 0) {
        return;
    }
    m_requests.removeAt(index);
    rebuildList();
    if (m_requests.isEmpty()) {
        showPlaceholder("暂无新的朋友申请");
    }
}

void FriendRequestList::showOperationError(const QString& message)
{
    m_tipLabel->setText(message.isEmpty() ? QStringLiteral("操作失败，请重试") : message);
    m_tipLabel->show();
}

int FriendRequestList::indexOfRequest(const QString& requestId) const
{
    for (int i = 0; i < m_requests.size(); ++i) {
        if (m_requests[i].requestId == requestId) {
            return i;
        }
    }
    return -1;
}

void FriendRequestList::rebuildList()
{
    m_listWidget->clear();

    for (const FriendRequestInfo& request : m_requests) {
        QListWidgetItem* item = new QListWidgetItem(m_listWidget);
        // item 高度 = 卡片高 + 卡片间空隙。空隙画在下面的 layout margins 里，
        // 所以 item 之间看起来是"浮着的卡片"，而不是一条挨一条的列表行
        item->setSizeHint(QSize(0, kCardHeight + kCardGap));

        // itemWidget 会被 Qt 拉满整个 item 矩形。外层这层容器全透明、只负责留白，
        // 真正的白卡片是里面那个 QFrame
        QWidget* container = new QWidget();
        container->setStyleSheet("background-color: transparent;");
        QHBoxLayout* containerLayout = new QHBoxLayout(container);
        containerLayout->setContentsMargins(kCardMargin, kCardGap / 2, kCardMargin, kCardGap / 2);
        containerLayout->setSpacing(0);

        QFrame* card = new QFrame(container);
        card->setObjectName("requestCard");
        card->setStyleSheet(kCardStyle);
        containerLayout->addWidget(card);

        QHBoxLayout* cardLayout = new QHBoxLayout(card);
        cardLayout->setContentsMargins(14, 0, 14, 0);
        cardLayout->setSpacing(12);

        // 方向：accountId 是我 → 这条是"我发出的"，服务器回的 name 是我自己；
        // 卡片要显示的是"对方"（=目标），所以名字换成目标昵称 targetName，
        // 服务端没给目标名字时退回目标ID（targetId）。别人发来的则照旧用 name
        const bool isMine = !m_currentAccountId.isEmpty() && request.accountId == m_currentAccountId;
        const QString displayName = isMine
            ? (request.targetName.isEmpty() ? request.targetId : request.targetName)
            : request.name;

        // ---- 头像：圆形、名字首字（没有头像图时兜底，和会话列表一个套路） ----
        QLabel* avatarLabel = new QLabel(displayName.left(1), card);
        avatarLabel->setFixedSize(kAvatarSize, kAvatarSize);
        avatarLabel->setAlignment(Qt::AlignCenter);
        avatarLabel->setStyleSheet(QString(R"(
            QLabel {
                border-radius: %1px;
                background-color: #67c23a;
                color: white;
                font-size: 16px;
                font-weight: 600;
            }
        )").arg(kAvatarSize / 2));
        cardLayout->addWidget(avatarLabel);

        // ---- 中间两行文字 ----
        QVBoxLayout* textLayout = new QVBoxLayout();
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(4);

        QHBoxLayout* firstRow = new QHBoxLayout();
        firstRow->setContentsMargins(0, 0, 0, 0);
        firstRow->setSpacing(8);

        QLabel* nameLabel = new QLabel(displayName, card);
        nameLabel->setStyleSheet("font-size: 14px; font-weight: 600; color: #4a90d9;");
        firstRow->addWidget(nameLabel);

        // 名字后面跟一句动作说明，与 QQ"好友通知"一致：别人加我 / 我在等对方
        QLabel* actionLabel = new QLabel(isMine ? "等待对方验证" : "请求添加你为好友", card);
        actionLabel->setStyleSheet(kMetaLabelStyle);
        firstRow->addWidget(actionLabel);

        QLabel* dateLabel = new QLabel(formatSendDate(request.sendTime), card);
        dateLabel->setStyleSheet(kMetaLabelStyle);
        firstRow->addWidget(dateLabel);

        firstRow->addStretch();
        textLayout->addLayout(firstRow);

        // 第二行是留言。附言为空时给 QQ 那句默认文案，留空这一行会显得像没加载出来
        QLabel* remarkLabel = new QLabel(
            QString("留言：%1").arg(request.remark.isEmpty() ? "请求添加对方为好友" : request.remark), card);
        remarkLabel->setStyleSheet("font-size: 12px; color: #8f8f8f;");
        textLayout->addWidget(remarkLabel);

        cardLayout->addLayout(textLayout, 1);

        // ---- 右侧：别人发来的给「同意」「拒绝」，我发出的只给一句状态词 ----
        if (isMine) {
            QLabel* stateLabel = new QLabel("等待验证", card);
            stateLabel->setStyleSheet(kMetaLabelStyle);
            cardLayout->addWidget(stateLabel);
        } else {
            QPushButton* rejectBtn = new QPushButton("拒绝", card);
            rejectBtn->setFixedSize(kButtonWidth, kButtonHeight);
            rejectBtn->setCursor(Qt::PointingHandCursor);
            rejectBtn->setFocusPolicy(Qt::NoFocus);   // 不参与 Tab 焦点循环
            rejectBtn->setStyleSheet(kRejectButtonStyle);

            QPushButton* acceptBtn = new QPushButton("同意", card);
            acceptBtn->setFixedSize(kButtonWidth, kButtonHeight);
            acceptBtn->setCursor(Qt::PointingHandCursor);
            acceptBtn->setFocusPolicy(Qt::NoFocus);
            acceptBtn->setStyleSheet(kAcceptButtonStyle);

            // 一层 lambda 把 requestId 带出去就够，不再往外抽具名函数
            const QString requestId = request.requestId;
            connect(acceptBtn, &QPushButton::clicked, this,
                    [this, requestId]() { emit acceptRequested(requestId); });
            connect(rejectBtn, &QPushButton::clicked, this,
                    [this, requestId]() { emit rejectRequested(requestId); });

            cardLayout->addWidget(rejectBtn);
            cardLayout->addWidget(acceptBtn);
        }

        m_listWidget->setItemWidget(item, container);
    }
}

void FriendRequestList::showPlaceholder(const QString& text)
{
    m_placeholderLabel->setText(text);
    m_stack->setCurrentWidget(m_placeholderLabel);
}