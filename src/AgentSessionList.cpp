#include "AgentSessionList.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QIcon>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QEnterEvent>
#include <QResizeEvent>
#include <QKeyEvent>

namespace {
constexpr int kRowHeight = 40;      // 行高：比按钮高一点，留出上下呼吸感
constexpr int kRowSpacing = 4;      // 行与行之间的缝
constexpr int kRowRadius = 8;       // 行本身也是圆角块
constexpr int kIconSize = 16;       // 图标绘制尺寸
constexpr int kBtnSize = 24;        // 按钮命中区域（比图标大一圈，好点）
constexpr int kTextMarginLeft = 12;  // 名字左边距
}

AgentSessionRow::AgentSessionRow(const QString& sessionId, const QString& title, QWidget* parent)
    : QWidget(parent), m_sessionId(sessionId), m_title(title), m_confirmedTitle(title)
{
    setObjectName("agentSessionRow");
    // 同 AgentMessageItem 的气泡：普通 QWidget 想用 QSS 画底色/圆角必须开这个属性
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(kRowHeight);
    setCursor(Qt::PointingHandCursor);

    QHBoxLayout* rowLayout = new QHBoxLayout(this);
    rowLayout->setContentsMargins(kTextMarginLeft, 0, 8, 0);
    rowLayout->setSpacing(4);

    // 名字：SizePolicy 用 Ignored 的水平策略，让长名字不去撑宽抽屉（宽度由布局分，
    // 超出部分自己负责省略）
    m_nameLabel = new QLabel(this);
    m_nameLabel->setStyleSheet("background-color: transparent; color: #333333;");
    // 字号用代码定（不写进 QSS）：省略号是按 QFontMetrics 量的，
    // 而 QSS 里的 font-size 要等 polish 之后才生效，构造期量出来的会是个错的字号
    QFont nameFont = m_nameLabel->font();
    nameFont.setPixelSize(14);
    m_nameLabel->setFont(nameFont);
    m_nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_nameLabel->setToolTip(m_title);
    rowLayout->addWidget(m_nameLabel, 1);

    // 就地改名输入框：和名字标签占同一个布局位置，平时隐藏、点笔时才显示
    m_nameEdit = new QLineEdit(title, this);
    m_nameEdit->setStyleSheet(R"(
        QLineEdit {
            background-color: white;
            border: 1px solid #4a90d9;
            border-radius: 4px;
            padding: 2px 6px;
            color: #333333;
            font-size: 14px;
        }
    )");
    m_nameEdit->hide();
    rowLayout->addWidget(m_nameEdit, 1);
    m_nameEdit->installEventFilter(this);

    // 重命名按钮：modify.svg（用户已放进 res/icon）
    m_renameBtn = new QPushButton(this);
    m_renameBtn->setFixedSize(kBtnSize, kBtnSize);
    m_renameBtn->setIcon(QIcon(":/res/icon/modify.svg"));
    m_renameBtn->setIconSize(QSize(kIconSize, kIconSize));
    m_renameBtn->setCursor(Qt::PointingHandCursor);
    m_renameBtn->setFocusPolicy(Qt::NoFocus);
    m_renameBtn->setToolTip("重命名");
    m_renameBtn->setStyleSheet(R"(
        QPushButton {
            border: none;
            border-radius: 4px;
            background-color: transparent;
        }
        QPushButton:hover {
            background-color: #e4e9ef;
        }
        QPushButton:pressed {
            background-color: #d8e0e8;
        }
    )");
    rowLayout->addWidget(m_renameBtn);

    // 删除按钮：同一个 QIcon 挂两套图 —— 平时灰叉，鼠标悬停在按钮上自动换成红叉。
    // QPushButton 绘制图标时，"鼠标悬浮"用的就是 QIcon::Active 这一档，
    // 所以不用额外写 enter/leave 去手动换图
    QIcon deleteIcon;
    deleteIcon.addFile(":/res/icon/delete.svg", QSize(kIconSize, kIconSize), QIcon::Normal);
    deleteIcon.addFile(":/res/icon/delete_hover.svg", QSize(kIconSize, kIconSize), QIcon::Active);
    m_deleteBtn = new QPushButton(this);
    m_deleteBtn->setFixedSize(kBtnSize, kBtnSize);
    m_deleteBtn->setIcon(deleteIcon);
    m_deleteBtn->setIconSize(QSize(kIconSize, kIconSize));
    m_deleteBtn->setCursor(Qt::PointingHandCursor);
    m_deleteBtn->setFocusPolicy(Qt::NoFocus);
    m_deleteBtn->setToolTip("删除会话");
    m_deleteBtn->setStyleSheet(R"(
        QPushButton {
            border: none;
            border-radius: 4px;
            background-color: transparent;
        }
        QPushButton:hover {
            background-color: #e4e9ef;
        }
        QPushButton:pressed {
            background-color: #d8e0e8;
        }
    )");
    rowLayout->addWidget(m_deleteBtn);

    connect(m_renameBtn, &QPushButton::clicked, this, &AgentSessionRow::startRename);
    connect(m_deleteBtn, &QPushButton::clicked, this, [this]() {
        emit deleteRequested(m_sessionId);
    });
    connect(m_nameEdit, &QLineEdit::returnPressed, this, &AgentSessionRow::commitRename);

    applyRowStyle();
}

void AgentSessionRow::setSelected(bool selected)
{
    if (m_selected == selected) {
        return;
    }
    m_selected = selected;
    applyRowStyle();
}

// 悬浮 / 选中两个状态都只影响整行底色，统一在这里刷一次，避免多处 setStyleSheet
void AgentSessionRow::applyRowStyle()
{
    QString backgroundColor = "transparent";
    if (m_selected) {
        backgroundColor = "#e8eef3";      // 当前会话：浅蓝灰
    } else if (m_hovered) {
        backgroundColor = "#f2f5f9";      // 悬浮：更淡的一层
    }
    setStyleSheet(QString("#agentSessionRow { background-color: %1; border-radius: %2px; }")
                      .arg(backgroundColor)
                      .arg(kRowRadius));
}

void AgentSessionRow::updateElidedTitle()
{
    // 用行宽反推名字能用的宽度，而不是读 m_nameLabel->width()：
    // row 的 resizeEvent 可能早于内部布局给标签定尺寸，那时标签宽度还是旧值/0
    const int available = width() - kTextMarginLeft - 8 - 2 * kBtnSize - 2 * 4;
    if (available <= 0) {
        return;
    }
    const QFontMetrics fm(m_nameLabel->font());
    m_nameLabel->setText(fm.elidedText(m_title, Qt::ElideRight, available));
}

void AgentSessionRow::startRename()
{
    m_renaming = true;
    m_nameEdit->setText(m_title);
    m_nameLabel->hide();
    m_nameEdit->show();
    m_nameEdit->setFocus();
    m_nameEdit->selectAll();   // 直接打字就是新名字，不用先删旧名
}

void AgentSessionRow::commitRename()
{
    if (!m_renaming) {
        return;   // Esc 取消后隐藏控件会触发一次 FocusOut，这里拦住，避免又提交一遍
    }
    m_renaming = false;
    m_nameEdit->hide();
    m_nameLabel->show();

    const QString newTitle = m_nameEdit->text().trimmed();
    if (newTitle.isEmpty() || newTitle == m_title) {
        return;   // 空名字 / 没改：当取消处理
    }

    // 先把行上显示改成新名字（乐观显示），但 m_confirmedTitle 保持旧值：
    // 服务器没认这次改名时，revertTitle 还要靠它把显示改回去
    m_title = newTitle;
    m_nameLabel->setToolTip(m_title);
    updateElidedTitle();
    emit renameRequested(m_sessionId, newTitle);
}

// 改名被服务器拒绝 / 超时：把显示回滚成"上一次确认过的名字"。
// 服务器确认成功后本地库会重拉整张列表，行是重建的，那时 m_confirmedTitle 自然刷新
void AgentSessionRow::revertTitle()
{
    m_title = m_confirmedTitle;
    m_nameLabel->setToolTip(m_title);
    updateElidedTitle();
}

void AgentSessionRow::cancelRename()
{
    if (!m_renaming) {
        return;
    }
    m_renaming = false;
    m_nameEdit->hide();
    m_nameLabel->show();
}

bool AgentSessionRow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_nameEdit) {
        if (event->type() == QEvent::FocusOut) {
            commitRename();
        } else if (event->type() == QEvent::KeyPress) {
            QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
            if (keyEvent->key() == Qt::Key_Escape) {
                cancelRename();
                return true;   // 吃掉 Esc，别让它冒泡到抽屉/窗口
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

// 点行 = 选中会话。按钮是子控件，它们接受点击后事件到不了这里，
// 所以点笔/点叉不会误触发"切换会话"
void AgentSessionRow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        emit clicked(m_sessionId);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void AgentSessionRow::enterEvent(QEnterEvent* event)
{
    m_hovered = true;
    applyRowStyle();
    QWidget::enterEvent(event);
}

void AgentSessionRow::leaveEvent(QEvent* event)
{
    m_hovered = false;
    applyRowStyle();
    QWidget::leaveEvent(event);
}

void AgentSessionRow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    // 行宽变了 → 名字能显示的字符数跟着变，重新省略
    updateElidedTitle();
}

AgentSessionList::AgentSessionList(QWidget* parent) : QScrollArea(parent)
{
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 滚动区本身不上色（抽屉是白底），只留一条很细的滚动条
    setStyleSheet(R"(
        QScrollArea {
            border: none;
            background-color: transparent;
        }
        QScrollBar:vertical {
            width: 4px;
            background-color: transparent;
        }
        QScrollBar::handle:vertical {
            background-color: #d0d5db;
            border-radius: 2px;
            min-height: 20px;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0px;
        }
    )");
    viewport()->setAutoFillBackground(false);

    m_container = new QWidget(this);
    m_container->setStyleSheet("background-color: transparent;");
    m_rowsLayout = new QVBoxLayout(m_container);
    m_rowsLayout->setContentsMargins(8, 8, 8, 8);
    m_rowsLayout->setSpacing(kRowSpacing);
    m_rowsLayout->addStretch(1);   // 行少时全部顶到上面，不往下摊
    setWidget(m_container);
}

void AgentSessionList::setSessions(const QList<ConversationInfo>& sessions)
{
    clearRows();

    for (const ConversationInfo& info : sessions) {
        AgentSessionRow* row = new AgentSessionRow(info.id, info.name, m_container);
        connect(row, &AgentSessionRow::clicked, this, &AgentSessionList::onRowClicked);
        connect(row, &AgentSessionRow::renameRequested, this, &AgentSessionList::renameRequested);
        connect(row, &AgentSessionRow::deleteRequested, this, &AgentSessionList::deleteRequested);
        // 插在末尾 stretch 之前
        m_rowsLayout->insertWidget(m_rowsLayout->count() - 1, row);
        m_rows.append(row);
    }

    // 列表重建后把选中态补回去（删别的会话 / 改名重拉时，当前会话不该丢高亮）
    for (AgentSessionRow* row : m_rows) {
        row->setSelected(row->sessionId() == m_currentSessionId);
    }
}

void AgentSessionList::setCurrentSessionId(const QString& sessionId)
{
    m_currentSessionId = sessionId;
    for (AgentSessionRow* row : m_rows) {
        row->setSelected(row->sessionId() == sessionId);
    }
}

void AgentSessionList::revertTitle(const QString& sessionId)
{
    for (AgentSessionRow* row : m_rows) {
        if (row->sessionId() == sessionId) {
            row->revertTitle();
            return;
        }
    }
}

void AgentSessionList::onRowClicked(const QString& sessionId)
{
    setCurrentSessionId(sessionId);

    // 会话名由行自己带着，回给上层用于切会话时的提示（目前上层只用 ID）
    AgentSessionRow* row = qobject_cast<AgentSessionRow*>(sender());
    emit sessionSelected(sessionId, row ? row->title() : QString());
}

void AgentSessionList::clearRows()
{
    for (AgentSessionRow* row : m_rows) {
        m_rowsLayout->removeWidget(row);
        row->deleteLater();
    }
    m_rows.clear();
}
