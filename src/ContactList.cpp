#include "ContactList.h"
#include <QPushButton>
#include <QLabel>
#include <QHBoxLayout>
#include <QMenu>
#include <QPropertyAnimation>
#include <QTransform>
#include <QIcon>

namespace {
// 四段的高度常量集中在这里：构造时设高度、算"列表能占多高"时都要用，
// 引用同一份常量，改一处就够，不会出现两边对不上的情况
constexpr int kItemHeight        = 62;   // 每条联系人行高，与 setContacts 里 setSizeHint 一致
constexpr int kNewFriendHeight   = 44;
constexpr int kDividerHeight     = 1;
constexpr int kGroupHeaderHeight = 34;

// 资源里的 arrow-down.svg 是"朝下"的箭头：
//   0°  = 展开（朝下）
//   -90° = 收起（朝右，即设计稿里那个 ">"）
// 只在这里转一次存成 QPixmap，避免每帧去渲染 SVG
QPixmap rotatedChevron(int degrees)
{
    QPixmap src = QIcon(":/res/icon/arrow-down.svg").pixmap(16, 16);
    if (degrees == 0) {
        return src;
    }
    return src.transformed(QTransform().rotate(degrees), Qt::SmoothTransformation);
}

// 右键菜单样式。抽成具名函数，不在 onContextMenuRequested 里堆一长串字符串。
// 三个要点：
//   1. 菜单里的悬停伪状态是 :selected，写 :hover 不生效
//   2. 不给 QMenu 本体写 border-radius —— 它是顶层窗口，四角外面的像素由窗口背景绘制，
//      会出现异色直角；要真圆角得配 FramelessWindowHint + WA_TranslucentBackground，
//      本项目平直画风的其它控件也都是直角，方角更统一
//   3. 不写 border 时 QSS 会让菜单样式"半退化"，边框和 padding 都要显式给
void applyMenuStyle(QMenu& menu)
{
    menu.setStyleSheet(R"(
        QMenu {
            background-color: #ffffff;
            border: 1px solid #e0e0e0;
            padding: 4px;
            font-size: 13px;
        }
        QMenu::item {
            padding: 7px 28px 7px 14px;
            color: #333333;
            border-radius: 6px;
        }
        QMenu::item:selected {
            background-color: #f5f5f5;
            color: #333333;
        }
    )");
}
}  // namespace

ContactList::ContactList(QWidget *parent) : QWidget(parent)
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ===== 第 1 段："新朋友" =====
    // 底色必须显式写白，不能图省事写 transparent：
    // 父级（ContactWindow 的 leftPanel）是浅灰底，透明会让这行跟着发灰，
    // 看上去像"一直处于悬浮态"；而下面的 QListWidget 是纯白，两块白不接壤很扎眼。
    // 悬浮/按下的深色反馈保留，色板与 StatusBar 导航按钮一致（rgba(0,0,0,0.06) / 0.14）
    m_newFriendBtn = new QPushButton("新朋友", this);
    m_newFriendBtn->setFixedHeight(kNewFriendHeight);
    m_newFriendBtn->setCursor(Qt::PointingHandCursor);// 鼠标悬停时显示"手"光标
    m_newFriendBtn->setFocusPolicy(Qt::NoFocus);   // 不参与 Tab 焦点循环
    // QPushButton 文字默认水平居中，text-align: left 才靠左；padding-left 控制离左边的距离。
    // 规范写法是带上 QPushButton 类型选择器，避免裸选择器被作用域共享
    m_newFriendBtn->setStyleSheet(R"(
        QPushButton {
            color: #333;
            font-size: 14px;
            font-weight: 500;
            text-align: left;
            padding-left: 15px;
            border: none;
            background-color: #ffffff;
        }
        QPushButton:hover {
            background-color: rgba(0, 0, 0, 0.06);
        }
        QPushButton:pressed {
            background-color: rgba(0, 0, 0, 0.14);
        }
    )");
    layout->addWidget(m_newFriendBtn);

    // ===== 第 2 段：分割线 =====
    // 用普通 QWidget + #id 选择器，不用 QFrame::HLine —— QFrame 的原生 frame 和 QSS border
    // 是两套独立的绘制系统，混用容易出"边框去不掉"的问题（见 note.txt），
    // 本项目已统一改为纯 QSS 方案，不再调 setFrameStyle。
    // 高度必须显式固定：默认垂直方向可伸缩，窗口富余高度会渗进来把线撑粗
    QWidget* divider = new QWidget(this);
    divider->setObjectName("contactDivider");
    divider->setFixedHeight(kDividerHeight);
    divider->setStyleSheet("#contactDivider { background-color: #f0f0f0; }");
    layout->addWidget(divider);

    // ===== 第 3 段："我的好友"折叠分组标题 =====
    // 不用 QPushButton::setIcon()：QSS 调不了"图标与文字之间的间距"，
    // 所以按钮内部自己塞一个 QHBoxLayout，把箭头和组名当成两个 QLabel 摆
    m_groupHeaderBtn = new QPushButton(this);
    m_groupHeaderBtn->setFixedHeight(kGroupHeaderHeight);
    m_groupHeaderBtn->setCursor(Qt::PointingHandCursor);// 鼠标悬停时显示"手"光标
    m_groupHeaderBtn->setFocusPolicy(Qt::NoFocus);
    // 底色同样显式写白，不能写 transparent（原因见上一条"新朋友"）
    m_groupHeaderBtn->setStyleSheet(R"(
        QPushButton {
            border: none;
            background-color: #ffffff;
        }
        QPushButton:hover {
            background-color: rgba(0, 0, 0, 0.06);
        }
        QPushButton:pressed {
            background-color: rgba(0, 0, 0, 0.14);
        }
    )");

    QHBoxLayout* headerLayout = new QHBoxLayout(m_groupHeaderBtn);
    // 左缩进必须写在这里：QSS 的 padding 作用不到按钮内部的子布局
    headerLayout->setContentsMargins(15, 0, 12, 0);
    headerLayout->setSpacing(6);

    m_chevronExpanded = rotatedChevron(0);// 展开状态的箭头
    m_chevronCollapsed = rotatedChevron(-90);// 折叠状态的箭头

    m_groupChevronLabel = new QLabel(m_groupHeaderBtn);
    m_groupChevronLabel->setFixedSize(16, 16);
    m_groupChevronLabel->setAlignment(Qt::AlignCenter);
    m_groupChevronLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);// 让鼠标事件穿透到按钮，否则 QLabel 会把 hover/pressed 吞掉

    m_groupNameLabel = new QLabel("我的好友", m_groupHeaderBtn);
    m_groupNameLabel->setStyleSheet("color: #666; font-size: 13px; font-weight: 600;");
    m_groupNameLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    headerLayout->addWidget(m_groupChevronLabel);
    headerLayout->addWidget(m_groupNameLabel);
    headerLayout->addStretch();
    layout->addWidget(m_groupHeaderBtn);

    updateGroupChevron();

    // ===== 第 4 段：联系人列表 =====
    // 折叠动画作用在外层 m_listContainer 上，不直接动 listWidget ——
    // 列表每帧都要重排条目、反复显隐滚动条，动画会很抖；
    // 套一层裸 QWidget 动画它的 maximumHeight，由父级裁剪，内部尺寸不用重算
    m_listContainer = new QWidget(this);
    QVBoxLayout* containerLayout = new QVBoxLayout(m_listContainer);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->setSpacing(0);

    //QListWidget 继承自 QFrame，默认会绘制一个框架边框，用 setFrameShape(QFrame::NoFrame) 关掉
    listWidget = new QListWidget(m_listContainer);
    listWidget->setFocusPolicy(Qt::NoFocus);                              // 不抢焦点
    listWidget->setSelectionMode(QAbstractItemView::SingleSelection);     // 单选模式，最多选一条
    listWidget->setFrameShape(QFrame::NoFrame);// 关掉框架边框
    // 这套 QSS 与 MessageList（会话列表）保持一致 —— "画风一致"最省事的保证
    listWidget->setStyleSheet(R"(
        QListWidget {
            border: none;
            background-color: white;
            show-decoration-selected: 0;
            outline: none;
        }
        QListWidget::item {
            height: 62px;
            padding: 6px 12px;
            background-color: white;
            border-top: none;
            border-left: none;
            border-right: none;
            border-bottom: 1px solid #f0f0f0;
            outline: none;
        }
        QListWidget::item:hover {
            background-color: #f5f5f5;
            border-top: none;
            border-left: none;
            border-right: none;
            border-bottom: 1px solid #f0f0f0;
        }
        QListWidget::item:selected {
            background-color: #ebebeb;
            color: #333;
            border-top: none;
            border-left: none;
            border-right: none;
            border-bottom: 1px solid #f0f0f0;
            outline: none;
        }
        QListWidget::item:focus {
            outline: none;
            border-top: none;
            border-left: none;
            border-right: none;
            border-bottom: 1px solid #f0f0f0;
        }
    )");
    containerLayout->addWidget(listWidget);
    // 注意这里没有 stretch：列表高度由 applyListHeight() 按内容算出来，
    // 剩余高度一律交给下一行的弹簧
    layout->addWidget(m_listContainer);

    // 末尾弹簧 —— 整个"不莫名撑开"的解法就在这一行。
    // 布局里必须始终存在一个能吸收富余高度的项：收起列表时列表容器被 hide()，
    // 若此时布局里全是固定高度的行，Qt 会把富余高度平摊给它们（表现为
    // "新朋友"和"我的好友"被拉开、中间大片空白）。有了弹簧，富余高度全归它，
    // ①②③ 永远紧贴顶部，展开/收起只改变列表占的高度。
    layout->addStretch();

    // 折叠动画：动 maximumHeight，160ms，缓动与项目其它动画同一档观感
    m_collapseAnim = new QPropertyAnimation(m_listContainer, "maximumHeight", this);
    m_collapseAnim->setDuration(160);
    m_collapseAnim->setEasingCurve(QEasingCurve::InOutCubic);

    // ========== 信号连接（统一放构造末尾）==========
    connect(m_groupHeaderBtn, &QPushButton::clicked, this, &ContactList::onGroupHeaderClicked);
    // 单击：右栏切到这位好友的详情（只看，不切页、不开会话）。
    // 单击本来靠 SingleSelection 的选中高亮给反馈就够了，现在多一个用途——喂右栏，
    // 所以必须连上 itemClicked 了
    connect(listWidget, &QListWidget::itemClicked, this, &ContactList::onItemClicked);
    // 双击：打开会话（切到聊天页）。双击前 Qt 会先发一次 itemClicked，
    // 也就是"右栏先切到详情、紧接着跳去聊天页"，视觉上没影响
    connect(listWidget, &QListWidget::itemDoubleClicked, this, &ContactList::onItemDoubleClicked);
    // QListWidget 默认不弹上下文菜单，先打开自定义策略
    listWidget->setContextMenuPolicy(Qt::CustomContextMenu);//把右键事件作为信号打包发出去
    //因为qt底层把右键事件给处理了，统一打包成了文本菜单事件按照菜单策略分流contextMenuPolicy，所以要连接customContextMenuRequested信号
    connect(listWidget, &QListWidget::customContextMenuRequested, this, &ContactList::onContextMenuRequested);
    connect(m_collapseAnim, &QPropertyAnimation::finished, this, &ContactList::onCollapseAnimFinished);
    // "新朋友"行：只往上喊一声，切右栏 / 发拉取请求都由 MainWindow 收口（本类不碰后端）
    connect(m_newFriendBtn, &QPushButton::clicked, this, &ContactList::onNewFriendBtnClicked);
}

void ContactList::setContacts(const QList<ContactInfo>& contacts)
{
    m_contacts = contacts;   // 留一份数据（后续删除 / 重渲染用）
    listWidget->clear();

    for (const auto& contact : contacts)
    {
        QListWidgetItem* item = new QListWidgetItem(listWidget);
        item->setData(Qt::UserRole, contact.id);         // 藏好友 ID，点击后据此知道是谁
        // 名字直接存 item 上（用 Role+1），比遍历控件树去抠 QLabel 文本可靠得多
        item->setData(Qt::UserRole + 1, contact.name);
        item->setSizeHint(QSize(0, kItemHeight));

        QWidget* container = new QWidget();   // 装这一行的所有子控件
        // 必须透明：容器是画在 item 之上的子控件，
        // 一旦有不透明底色就会把 item:hover / item:selected 的背景整块盖住
        container->setStyleSheet("background-color: transparent;");
        QHBoxLayout* hLayout = new QHBoxLayout(container);
        hLayout->setContentsMargins(0, 0, 0, 0);
        hLayout->setSpacing(10);

        QLabel* avatarLabel = new QLabel();
        avatarLabel->setText(contact.name.left(1));   // 没有头像图时用名字首字当头像
        avatarLabel->setStyleSheet(R"(
            QLabel {
                width: 44px;
                height: 44px;
                border-radius: 8px;
                background-color: #67c23a;
                color: white;
                font-size: 18px;
                font-weight: 600;
            }
        )");
        avatarLabel->setAlignment(Qt::AlignCenter);
        hLayout->addWidget(avatarLabel);

        QLabel* nameLabel = new QLabel(contact.name);
        nameLabel->setStyleSheet("font-weight: 600; font-size: 14px; color: #333;");
        hLayout->addWidget(nameLabel);
        hLayout->addStretch();   // 弹簧：名字靠左

        listWidget->setItemWidget(item, container);
    }

    // 条目数变了，列表该占的高度跟着变（0 条时高度 0，不留空档）
    applyListHeight();
}

void ContactList::updateGroupChevron()
{
    m_groupChevronLabel->setPixmap(m_groupExpanded ? m_chevronExpanded : m_chevronCollapsed);
    //setPixmap 是一张定死的图片，不能动态改变大小，否则会导致图片模糊或失真
}

// 列表"应该"占多高：内容少就贴着内容，内容多就占满左栏可用高度（超出部分内部滚动）。
// 上限必须减掉上面三行的固定高度，否则列表会连同三行一起超出左栏。
int ContactList::listTargetHeight() const
{
    const int content   = listWidget->count() * kItemHeight;
    const int fixedRows = kNewFriendHeight + kDividerHeight + kGroupHeaderHeight;
    const int available = qMax(0, height() - fixedRows);
    return qMin(content, available);
}

// 把高度钉到目标值。两处都要钉：
//   listWidget  —— 定住内容实际占的高度
//   容器 maximumHeight —— 定住容器高度，不让它按 QListWidget 自己的 sizeHint 撑高
// 动画运行期间不能调这个函数：maximumHeight 那时归动画管，插一脚会打架
void ContactList::applyListHeight()
{
    const int target = listTargetHeight();
    listWidget->setFixedHeight(target);
    m_listContainer->setMaximumHeight(target);
}

void ContactList::resizeEvent(QResizeEvent* event)
{
    //这个是一个事件，当窗口大小改变时触发，用于更新列表高度，目的：
    // 1. 确保列表高度与窗口高度一致，避免滚动条显示不全
    // 2. 避免在动画运行期间更新高度，导致动画异常
    QWidget::resizeEvent(event);
    // 左栏高度变了，列表可占的高度跟着变。
    // 收起态不用算（列表藏着，算了也看不见），动画中更不能算
    if (m_groupExpanded && m_collapseAnim->state() != QAbstractAnimation::Running) {
        applyListHeight();
    }
}

void ContactList::onGroupHeaderClicked()
{
    // 连点保护：上一次动画没跑完就再点，先停掉，否则起止值会串
    m_collapseAnim->stop();

    m_groupExpanded = !m_groupExpanded;
    updateGroupChevron();

    if (m_groupExpanded) {
        // 先把目标高度算好（同时钉住 listWidget 的高度），动画终点直接用它。
        // 不能用本控件的 height() 当终点 —— 那是整个左栏的高度，
        // 会让 maximumHeight 上限被抬到全栏高，动画一次性"撑"到底
        applyListHeight();
        // 隐藏状态下高度恒为 0，不先显示出来动画是"看不见"的
        m_listContainer->setVisible(true);
        m_collapseAnim->setStartValue(0);
        m_collapseAnim->setEndValue(listTargetHeight());
    } else {
        m_collapseAnim->setStartValue(m_listContainer->height());
        m_collapseAnim->setEndValue(0);
    }
    m_collapseAnim->start();
}

void ContactList::onCollapseAnimFinished()
{
    if (m_groupExpanded) {
        // 动画跑完把上限钉回目标高度（= listWidget 的固定高），两者始终一致
        applyListHeight();
    } else {
        m_listContainer->setVisible(false);
    }
}

void ContactList::onNewFriendBtnClicked()
{
    // 纯转发：真正的动作是"切右栏 + 向服务器拉申请列表"，都由 MainWindow 具名槽接手，
    // 本类不认识 MainBackend / ContactBackend
    emit newFriendRequested();
}

// 单击好友条目 → 往上喊一声"选中了谁"，由 MainWindow 决定右栏显示什么。
// 分组头和"新朋友"行都没藏 ID（见 setContacts：只有好友行 setData 了 UserRole），
// 所以 id 为空就直接返回，不会把这两行误当成好友发出去
void ContactList::onItemClicked(QListWidgetItem* item)
{
    if (!item) {
        return;
    }
    const QString contactId = item->data(Qt::UserRole).toString();
    if (contactId.isEmpty()) {
        return;
    }
    emit contactSelected(contactId, item->data(Qt::UserRole + 1).toString());
}

void ContactList::onItemDoubleClicked(QListWidgetItem* item)
{
    if (!item) {
        return;
    }
    emit contactOpened(item->data(Qt::UserRole).toString(),
                       item->data(Qt::UserRole + 1).toString());
}

void ContactList::onContextMenuRequested(const QPoint& pos)//QPoint 是一个点类，值就相当于是int x,y组成的(x,y)坐标
{
    // pos 是 viewport 坐标（真正接收鼠标事件的是 viewport），itemAt 正好吃这个坐标系；
    // 落在空白处返回 nullptr，此时不弹菜单
    // itemAt = 命中测试：坐标进、元素出。把视口坐标喂进去，返回"这个位置上压着的条目指针"，
    // 空白处返回 nullptr；它不返回坐标也不返回行号（行号要另调 row(item)）
    QListWidgetItem* item = listWidget->itemAt(pos);
    if (!item) {
        return;
    }
    // 先选中，让用户看清这个菜单是冲哪一行来的（setCurrentItem 不会触发 itemClicked）
    listWidget->setCurrentItem(item);

    const QString contactId = item->data(Qt::UserRole).toString();
    const QString contactName = item->data(Qt::UserRole + 1).toString();

    QMenu menu(this);
    applyMenuStyle(menu);
    // 破坏性操作放下面
    QAction* openAction = menu.addAction("打开会话");//QAction 是一个动作类，打包可供用户执行的命令
    QAction* deleteAction = menu.addAction("删除好友");

    // exec 是阻塞的，返回时菜单已关闭；用返回值判断选中项而不是连 triggered，
    // 免去"在 triggered 回调里删除正在 exec 的菜单"这类坑。
    // 弹菜单要用屏幕坐标，所以把 viewport 坐标 map 出去
    QAction* chosen = menu.exec(listWidget->viewport()->mapToGlobal(pos));
    /*
    被点击元素的坐标是根据当前视窗的坐标算的，列表控件的对外展示是依赖视窗,也就是QListWidget->viewort->条目，我们看到的展示的条目就是在视窗上
    因此第一步就是拿到点击的项在视窗的条目拿到对应的坐标（在视窗上）以viewport左上角为顶点开始计算的坐标，而点击的项在视窗的坐标是根据当前视窗的坐标算的，所以要转换为屏幕坐标
    menu很特殊他是跟主窗口平级的它的绘制需要拿到屏幕坐标，因此要转换为屏幕坐标也就是listWidget->viewport()->mapToGlobal(pos)
    */

    if (chosen == openAction) {
        emit contactOpened(contactId, contactName);
    } else if (chosen == deleteAction) {
        emit deleteContactRequested(contactId, contactName);   // 由 MainWindow 转给主后端，发 delete_friend 给服务器
    }
}

// ============================================================
// 知识笔记：双击为什么有现成信号、三击四击五击没有
// ============================================================
// 双击是全平台统一的标准惯用语（双击=打开），Qt 必须支持；而检测它很麻烦——
// 间隔多少毫秒、偏移几个像素算"同一次"，阈值每个系统还不一样（Windows 默认 500ms，
// 用户可在控制面板改）。这些复杂判断被框架层吃掉了：QApplication 计时，
// 第二次点击落在时间窗内就直接发 itemDoubleClicked / MouseButtonDblClick。
//
// 三击以上不是任何平台的桌面惯用语，没有约定语义，所以 Qt 不封装。真要做就用
// 「计数器 + 时间窗 + 位置容差」在 mousePressEvent（或事件过滤器）里手搓：
//   m_clickCount = (上一次点击够快 && 位置够近) ? m_clickCount + 1 : 1;
//   阈值沿用 styleHints()->mouseDoubleClickInterval()（业界通用做法）。
// 另外两个彩蛋：
//   ① QTextEdit/QTextBrowser 三击选整段是内置的，聊天区要做"三击选一段"不用写代码
//   ② Qt 里第 3/4/5 次快击也会被发成又一个 MouseButtonDblClick 事件，
//      在事件里数"连到第几个 DblClick"是另一条偷懒路
/*// 成员：int m_clickCount = 0;  QElapsedTimer m_clickTimer;  QPoint m_clickPos;

void XxxWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        return QWidget::mousePressEvent(event);
    }

    const int interval = QGuiApplication::styleHints()->mouseDoubleClickInterval();
    const bool chained = m_clickTimer.isValid()
                         && m_clickTimer.elapsed() < interval                // 够快
                         && (event->pos() - m_clickPos).manhattanLength() < 10; // 够近

    m_clickCount = chained ? m_clickCount + 1 : 1;
    m_clickTimer.restart();
    m_clickPos = event->pos();

    if (m_clickCount >= 5) {          // 五连击达成，归零防连发
        m_clickCount = 0;
        emit quintupleClicked();
    }
    QWidget::mousePressEvent(event);
}*/



