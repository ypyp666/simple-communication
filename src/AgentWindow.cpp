#include "AgentWindow.h"
#include "ChatArea.h"
#include "MessageList.h"

#include <QPushButton>
#include <QPropertyAnimation>
#include <QCursor>
#include <QEvent>

namespace {
constexpr int kHotZoneWidth = 16;   // 左侧感应条宽度（只覆盖窄窄一条，不挡聊天区操作）
constexpr int kHandleWidth = 14;    // 手柄宽度
constexpr int kHandleHeight = 44;   // 手柄高度
constexpr int kDrawerWidth = 240;   // 抽屉（会话列表）宽度
constexpr int kDrawerAnimMs = 220;  // 抽屉滑入/滑出时长
constexpr int kHandleGap = 4;       // 手柄与抽屉右边缘之间留的缝
}

// 智能体页：整页垂直布局，主体直接把聊天页那套 ChatArea 搬过来
//（顶部标题条 + 消息滚动区 + 输入框），只是标题换成"小C"、把默认隐藏的输入框打开。
// 在此之上叠了三个"浮层"：左边缘感应条、">"手柄、会话列表抽屉
AgentWindow::AgentWindow(QWidget *parent) : QWidget(parent)
{
    mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    chatArea = new ChatArea(this);
    chatArea->setContactName("小C");   // 复用 ChatArea 顶部的标题条当作本页的标题框
    chatArea->setInputVisible(true);   // 输入框默认是隐藏的（聊天页"没选联系人"的状态），这里要打开
    mainLayout->addWidget(chatArea);

    setupOverlays();
}

AgentWindow::~AgentWindow()
{
}

// ===== 浮层的创建：注意三个控件都 setParent(this) 但都没进布局 =====
// 进了布局就会被自动排版，没法"浮在别的控件上面"；只有手动 setGeometry 才能叠在聊天区上
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

    sessionList = new MessageList(drawer);
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
        return QWidget::eventFilter(watched, event);
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
}

void AgentWindow::layoutOverlays()
{
    const QRect area = chatArea->geometry();
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