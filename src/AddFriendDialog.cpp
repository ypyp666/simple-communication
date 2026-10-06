#include "AddFriendDialog.h"
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QColor>
#include <QMouseEvent>
#include <QShowEvent>

namespace {
constexpr int kCardWidth  = 300;   // 卡片（内容区）宽度
constexpr int kShadowPad  = 14;    // 卡片外给阴影留的一圈透明边距
constexpr int kAvatarSize = 40;
constexpr int kMaxMessage = 30;    // 留言上限 30 字（录入阶段由 setMaxLength 卡住）

// 卡片本体：白底圆角 + 一圈极浅描边。
// 注意根窗口是 WA_TranslucentBackground（见构造函数），卡片外那圈才会是透明，
// 圆角才不会被窗口的方形底衬露出来
const char* kCardStyle = R"(
    QFrame#addFriendCard {
        background-color: white;
        border: 1px solid #e6e6e6;
        border-radius: 10px;
    }
)";

// 留言输入框：与详情页备注框同一套观感（灰描边，聚焦变蓝）
const char* kMessageEditStyle = R"(
    QLineEdit {
        border: 1px solid #e0e0e0;
        border-radius: 8px;
        padding: 0 10px;
        font-size: 13px;
        color: #333;
    }
    QLineEdit:focus {
        border: 1px solid #4a90d9;
    }
)";

// 发送：主按钮，沿用项目蓝色
const char* kSendBtnStyle = R"(
    QPushButton {
        background-color: #4a90d9;
        color: white;
        font-size: 13px;
        border: none;
        border-radius: 8px;
    }
    QPushButton:hover   { background-color: #3a80c9; }
    QPushButton:pressed { background-color: #2f70b5; }
)";

// 取消：幽灵按钮（白底灰描边），与详情页"修改备注"同款
const char* kCancelBtnStyle = R"(
    QPushButton {
        background-color: white;
        color: #666;
        font-size: 13px;
        border: 1px solid #d9d9d9;
        border-radius: 8px;
    }
    QPushButton:hover   { background-color: #f5f5f5; }
    QPushButton:pressed { background-color: #ececec; }
)";
}

AddFriendDialog::AddFriendDialog(const QString& contactId, const QString& contactName, QWidget* parent)
    : QDialog(parent), m_contactId(contactId)
{
    // 无边框 + 透明底：圆角卡片靠这俩属性才能"浮"在桌面上而不是被方形窗口底衬框住
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);

    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(kShadowPad, kShadowPad, kShadowPad, kShadowPad);
    rootLayout->setSpacing(0);

    QFrame* card = new QFrame(this);
    card->setObjectName("addFriendCard");
    card->setStyleSheet(kCardStyle);
    card->setFixedWidth(kCardWidth);
    rootLayout->addWidget(card);

    // 一圈柔和投影，让它看起来像浮在上面的小菜单
    QGraphicsDropShadowEffect* shadow = new QGraphicsDropShadowEffect(card);
    shadow->setBlurRadius(24);
    shadow->setColor(QColor(0, 0, 0, 40));
    shadow->setOffset(0, 4);
    card->setGraphicsEffect(shadow);

    QVBoxLayout* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(18, 18, 18, 16);
    cardLayout->setSpacing(0);

    // ---- 第一块：头像 + 名字 ----
    QHBoxLayout* headLayout = new QHBoxLayout();
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->setSpacing(12);

    QLabel* avatarLabel = new QLabel(contactName.left(1), card);
    avatarLabel->setFixedSize(kAvatarSize, kAvatarSize);
    avatarLabel->setAlignment(Qt::AlignCenter);
    // 圆形头像（半径 = 边长一半）+ 名字首字，与搜索结果卡片同一套路
    avatarLabel->setStyleSheet(QString(R"(
        QLabel {
            border-radius: %1px;
            background-color: #67c23a;
            color: white;
            font-size: 16px;
            font-weight: 600;
        }
    )").arg(kAvatarSize / 2));
    headLayout->addWidget(avatarLabel);

    QLabel* nameLabel = new QLabel(contactName, card);
    nameLabel->setStyleSheet("font-size: 15px; font-weight: 600; color: #333;");
    headLayout->addWidget(nameLabel, 1);

    cardLayout->addLayout(headLayout);
    cardLayout->addSpacing(16);

    // ---- 第二块：留言输入框 + 字数计数 ----
    m_messageEdit = new QLineEdit(card);
    m_messageEdit->setPlaceholderText("请输入留言（选填）");
    m_messageEdit->setMaxLength(kMaxMessage);   // 录入阶段就卡住，敲不进第 31 个字
    m_messageEdit->setFixedHeight(36);
    m_messageEdit->setStyleSheet(kMessageEditStyle);
    cardLayout->addWidget(m_messageEdit);

    cardLayout->addSpacing(6);
    m_counterLabel = new QLabel(QString("0/%1").arg(kMaxMessage), card);
    m_counterLabel->setAlignment(Qt::AlignRight);
    m_counterLabel->setStyleSheet("font-size: 11px; color: #bbb;");
    cardLayout->addWidget(m_counterLabel);

    cardLayout->addSpacing(14);

    // ---- 第三块：发送 / 取消（靠右）----
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(10);
    buttonLayout->addStretch();

    QPushButton* sendBtn = new QPushButton("发送", card);
    sendBtn->setFixedSize(76, 32);
    sendBtn->setCursor(Qt::PointingHandCursor);
    sendBtn->setFocusPolicy(Qt::NoFocus);
    sendBtn->setStyleSheet(kSendBtnStyle);
    buttonLayout->addWidget(sendBtn);

    QPushButton* cancelBtn = new QPushButton("取消", card);
    cancelBtn->setFixedSize(76, 32);
    cancelBtn->setCursor(Qt::PointingHandCursor);
    cancelBtn->setFocusPolicy(Qt::NoFocus);
    cancelBtn->setStyleSheet(kCancelBtnStyle);
    buttonLayout->addWidget(cancelBtn);

    cardLayout->addLayout(buttonLayout);

    // ---- 信号 ----
    // 字数计数跟着输入实时更新（上限已由 setMaxLength 保证，这里只负责显示）
    connect(m_messageEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_counterLabel->setText(QString("%1/%2").arg(text.length()).arg(kMaxMessage));
    });
    connect(sendBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);

    m_messageEdit->setFocus();
}

QString AddFriendDialog::message() const
{
    return m_messageEdit->text().trimmed();
}

// 无边框窗口 Qt 不保证摆到父窗口中间，这里自己算一次
void AddFriendDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    if (QWidget* parent = parentWidget()) {
        
        const QPoint center = parent->window()->geometry().center();
        move(center.x() - width() / 2, center.y() - height() / 2);
    }
}

void AddFriendDialog::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
    }
    QDialog::mousePressEvent(event);
}

void AddFriendDialog::mouseMoveEvent(QMouseEvent* event)
{
    if (event->buttons() & Qt::LeftButton) {
        move(event->globalPosition().toPoint() - m_dragOffset);
    }
    QDialog::mouseMoveEvent(event);
}