#include "SearchResultList.h"
#include <QStackedWidget>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QPushButton>

namespace {
// 尺寸常量集中在这里：造卡片、算 item 高度两处都要用，引用同一份，改一处就够
//（与 FriendRequestList 同款比例，两个列表看起来才是一套东西）
constexpr int kCardHeight   = 68;      // 白卡片本身的高度（不含卡片之间的空隙）
constexpr int kCardGap      = 10;      // 卡片之间的空隙
constexpr int kCardMargin   = 28;      // 卡片离列表左右边缘的留白（照 QQ：卡片不贴边）
constexpr int kAvatarSize   = 40;
constexpr int kButtonWidth  = 58;
constexpr int kButtonHeight = 26;

// 卡片本体用 QFrame 而不是裸 QWidget：QWidget 想让它支持 QSS 背景必须重写 paintEvent
//（或加 WA_StyledBackground），QFrame 天生就吃 background/border-radius
const char* kCardStyle = R"(
    QFrame#searchResultCard {
        background-color: white;
        border-radius: 8px;
    }
    QFrame#searchResultCard:hover {
        background-color: #fafafa;
    }
)";

// 添加：主按钮，配色沿用登录页 / 好友申请页那套蓝色
const char* kAddButtonStyle = R"(
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

// 账号ID那行小字统一用这个字号/颜色，免得别处再各写一遍对不上
const char* kIdLabelStyle = "font-size: 12px; color: #999;";
}  // namespace

SearchResultList::SearchResultList(QWidget *parent) : QWidget(parent)
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

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

    // 默认停在提示页：还没搜过，列表就是空的
    showPlaceholder("输入账号或昵称搜索用户");
}

void SearchResultList::setLoading()
{
    // 每次重新搜索都从零开始 —— 上一次的结果留着只会和新结果混在一起
    m_results.clear();
    rebuildList();
    showPlaceholder("正在搜索…");
}

void SearchResultList::setResults(const QList<ContactInfo>& results)
{
    m_results = results;
    if (m_results.isEmpty()) {
        showPlaceholder("没有找到相关用户");
        return;
    }
    rebuildList();
    m_stack->setCurrentWidget(m_listWidget);
}

void SearchResultList::setError(const QString& message)
{
    // 失败就是失败：不显示空列表，否则用户会以为"真的没人叫这个名字"。
    // 顺手清掉旧结果，避免失败文案和上一次的结果同时挂在界面上
    m_results.clear();
    rebuildList();
    showPlaceholder(message.isEmpty() ? QStringLiteral("搜索失败，请重试") : message);
}

void SearchResultList::rebuildList()
{
    m_listWidget->clear();

    for (const ContactInfo& result : m_results) {
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
        card->setObjectName("searchResultCard");
        card->setStyleSheet(kCardStyle);
        containerLayout->addWidget(card);

        QHBoxLayout* cardLayout = new QHBoxLayout(card);
        cardLayout->setContentsMargins(14, 0, 14, 0);
        cardLayout->setSpacing(12);

        // ---- 头像：圆形、名字首字（和会话列表 / 好友申请列表同一个套路） ----
        QLabel* avatarLabel = new QLabel(result.name.left(1), card);
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

        // ---- 中间两行文字：用户名 / 账号ID ----
        QVBoxLayout* textLayout = new QVBoxLayout();
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(4);

        QLabel* nameLabel = new QLabel(result.name, card);
        nameLabel->setStyleSheet("font-size: 14px; font-weight: 600; color: #4a90d9;");
        textLayout->addWidget(nameLabel);

        QLabel* idLabel = new QLabel(result.id, card);
        idLabel->setStyleSheet(kIdLabelStyle);
        textLayout->addWidget(idLabel);

        cardLayout->addLayout(textLayout, 1);

        // ---- 右侧：「添加」按钮 ----
        QPushButton* addBtn = new QPushButton("添加", card);
        addBtn->setFixedSize(kButtonWidth, kButtonHeight);
        addBtn->setCursor(Qt::PointingHandCursor);
        addBtn->setFocusPolicy(Qt::NoFocus);   // 不参与 Tab 焦点循环
        addBtn->setStyleSheet(kAddButtonStyle);

        // 一层 lambda 把整条资料带出去就够（弹"申请加好友"小窗要用名字/头像），不再往外抽具名函数
        const ContactInfo contact = result;
        connect(addBtn, &QPushButton::clicked, this,
                [this, contact]() { emit addRequested(contact); });

        cardLayout->addWidget(addBtn);

        m_listWidget->setItemWidget(item, container);
    }
}

void SearchResultList::showPlaceholder(const QString& text)
{
    m_placeholderLabel->setText(text);
    m_stack->setCurrentWidget(m_placeholderLabel);
}