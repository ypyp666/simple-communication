#include "MessageList.h"
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QMenu>
#include <algorithm>

namespace
{
// 会话列表的排序规则：最后一条消息的时间越新，排得越靠上（倒序）。
// 排序键取 ConversationInfo::lastTime —— 会话快照里"最后一条消息"的时间，
// 拿它排正好对上 IM 的通用习惯：刚聊过的会话顶到最上面。
// 抽成具名函数（而不是就地写 lambda）是为了让下面 std::sort 那一行一眼能看懂在比什么
bool isNewerConversation(const ConversationInfo& a, const ConversationInfo& b)
{
    return a.lastTime > b.lastTime;
}

// 右键菜单样式。抽成具名函数，不在 onContextMenuRequested 里堆一长串字符串。
// 三个要点（与 ContactList 的菜单保持同一套视觉）：
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
}

MessageList::MessageList(QWidget *parent) : QWidget(parent)
{
    //QListWidget 继承自 QFrame ，默认会绘制一个 框架边框 ，我们不需要，所以用 setFrameShape(QFrame::NoFrame) 来关闭 。
    layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    listWidget = new QListWidget(this);
    listWidget->setFocusPolicy(Qt::NoFocus);// 关闭列表框的焦点，防止点击列表框时触发信号
    listWidget->setSelectionMode(QAbstractItemView::SingleSelection);// 单选模式
    listWidget->setFrameShape(QFrame::NoFrame);
    /*Qt自带的列表显示控件自带垂直滚动条
    自带点击选中，自带双击事件，不用你自己画，不用算坐标，直接 addItem 就能加内容，创建一个列表对象，this 表示它的父控件是 MessageList（左侧会话列表面板）
    */
    //Qt的样式表有 优先级规则 ，如果子控件没有显式设置样式，会继承父控件的样式或使用系统默认样式
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
    // ==================== 会话列表 QListWidget 样式 ====================
    // QListWidget         : 列表整体样式（无边框、浅灰背景）
    // QListWidget::item   : 单个会话条目（高70px、内边距、无边框）
    // item:hover          : 鼠标悬浮时背景变深灰
    // item:selected       : 选中时背景变蓝色（更深）
    // 效果：干净、现代、易点击、视觉清晰的会话列表
    // =====================================================================
    layout->addWidget(listWidget);

    connect(listWidget, &QListWidget::itemClicked, this, &MessageList::onItemClicked);//把点击事件的信号发给 onItemClicked 方法处理
    //itemClicked : 点击列表项时触发，包含点击的项和点击的位置信息，QListWidget独有信号，直接发送被点击的行的指针

    listWidget->setContextMenuPolicy(Qt::CustomContextMenu);//把右键事件作为信号打包发出去
    //Qt 底层已经把右键事件统一打包成"文本菜单事件"，按 contextMenuPolicy 分流；
    //选 CustomContextMenu 才会把它转成 customContextMenuRequested 信号，交给我们自己弹菜单
    connect(listWidget, &QListWidget::customContextMenuRequested, this, &MessageList::onContextMenuRequested);

}

void MessageList::setConversations(const QList<ConversationInfo>& conversations)//把后端给的会话列表渲染出来（数据是 ConversationInfo，其 lastMessage/lastTime/unreadCount 就是会话内容）
{
    m_conversations = conversations;   // 先存下来，真正画的时候还要和名字兜底合在一起用
    rebuildItems();
}

// 名字兜底：通讯录（ContactInfo 自带 name）来了就存成"ID→名字"的映射，再重画一遍。
// 为什么需要它：conversations 表里没有名字列，会话列表的名字靠查库时 LEFT JOIN contacts 补；
// 联系人还没进 contacts 表时那个名字就是空的（截图上"没名字的那条会话"正是这种情况）。
// 这里用内存里现成的通讯录名字顶上，免得为了一个名字再多查一次库
void MessageList::setContactNames(const QList<ContactInfo>& contacts)
{
    for (const auto& contact : contacts) {
        m_contactNames.insert(contact.id, contact.name);
    }
    rebuildItems();
}

// 这一行最终显示的名字：会话自带的（LEFT JOIN 查出来的）优先，为空才退回通讯录名字
QString MessageList::displayNameOf(const ConversationInfo& conversation) const
{
    if (!conversation.name.isEmpty()) {
        return conversation.name;
    }
    return m_contactNames.value(conversation.id);
}

// 用当前存下的两份数据（会话快照 + 名字兜底）把列表整个重画一遍。
// setConversations / setContactNames 谁先到都会调它：两边数据到达顺序不确定，
// 谁后到就带上对方已经存好的那份一起重画，界面总能收敛到正确样子
void MessageList::rebuildItems()
{
    // 先按"最后一条消息的时间"倒序排一份本地副本，再渲染，保证最新会话在最上面。
    // 排副本而不排 m_conversations：排序只是显示需要，原始数据保持后端给的顺序，
    // 以后要拿它做别的判断时不会被"顺手排过"影响。
    // 数据库那边其实已经 ORDER BY last_time DESC 查过一遍了，这里再排一次是兜底——
    // 以后本地插入新会话（比如刚加了新好友）时，显示顺序由这一处说了算
    QList<ConversationInfo> sortedConversations = m_conversations;
    std::sort(sortedConversations.begin(), sortedConversations.end(), isNewerConversation);

    listWidget->clear();//先清空旧列表

    for (const auto& conversation : sortedConversations)//遍历每一个会话
    {
        // 最终显示名要用在两处（头像首字、名字标签），先算一次，别算两遍
        const QString displayName = displayNameOf(conversation);

        QListWidgetItem* item = new QListWidgetItem(listWidget);
        item->setData(Qt::UserRole, conversation.id);//给这一行藏一个好友 ID点击这一行时，就能知道是哪个联系人。
        // UserRole : 是槽位编号
        item->setSizeHint(QSize(0, 62));

        QWidget* container = new QWidget();//创建一个容器，用来装所有的子控件
        container->setStyleSheet("background-color: transparent;");
        QHBoxLayout* hLayout = new QHBoxLayout(container);//创建一个水平布局，用来装所有的子控件
        hLayout->setContentsMargins(0, 0, 0, 0);
        hLayout->setSpacing(10);

        QLabel* avatarLabel = new QLabel();//创建一个标签，用来显示头像
        avatarLabel->setText(displayName.left(1));
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
        hLayout->addWidget(avatarLabel);//把头像标签添加到水平布局中

        QWidget* textContainer = new QWidget();//创建一个容器，用来装文本标签
        textContainer->setStyleSheet("background-color: transparent;");
        QVBoxLayout* vLayout = new QVBoxLayout(textContainer);//创建一个垂直布局，用来装文本标签
        vLayout->setContentsMargins(0, 0, 0, 0);
        vLayout->setSpacing(4);

        QWidget* nameRow = new QWidget();
        nameRow->setStyleSheet("background-color: transparent;");
        QHBoxLayout* nameLayout = new QHBoxLayout(nameRow);
        nameLayout->setContentsMargins(0, 0, 0, 0);
        nameLayout->setSpacing(0);

        QLabel* nameLabel = new QLabel(displayName);
        nameLabel->setStyleSheet("font-weight: 600; font-size: 14px; color: #333;");
        nameLabel->setFixedHeight(18);
        nameLayout->addWidget(nameLabel);
        nameLayout->addStretch();

        QString timeStr = conversation.lastTime.toString("HH:mm");
        if (conversation.lastTime.date() != QDate::currentDate())
        {
            timeStr = conversation.lastTime.toString("MM-dd");
        }
        QLabel* timeLabel = new QLabel(timeStr);
        timeLabel->setStyleSheet("font-size: 12px; color: #999;");
        timeLabel->setFixedHeight(14);
        nameLayout->addWidget(timeLabel);

        vLayout->addWidget(nameRow);

        QWidget* messageRow = new QWidget();
        messageRow->setStyleSheet("background-color: transparent;");//背景完全透明
        QHBoxLayout* messageLayout = new QHBoxLayout(messageRow);
        messageLayout->setContentsMargins(0, 0, 0, 0);
        messageLayout->setSpacing(4);

        QLabel* messageLabel = new QLabel(conversation.lastMessage);//创建文本标签,显示最后一条消息
        messageLabel->setStyleSheet("font-size: 12px; color: #8f8f8f;");//字体大小12px,颜色#8f8f8f
        messageLabel->setFixedHeight(16);//固定高度16px
        messageLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);//宽度自适应,高度固定
        messageLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);//左对齐,垂直居中
        messageLabel->setWordWrap(false);//不换行
        messageLabel->setTextInteractionFlags(Qt::NoTextInteraction);//不允许交互
        messageLayout->addWidget(messageLabel);//添加文本标签到布局
        messageLayout->addStretch();

        if (conversation.unreadCount > 0)
        {
            QFrame* badgeFrame = new QFrame();
            badgeFrame->setStyleSheet(R"(
                QFrame {
                    background-color: #f56c6c;
                    border-radius: 9px;
                }
            )");
            badgeFrame->setFixedSize(18, 18);

            QLabel* badgeLabel = new QLabel(QString::number(conversation.unreadCount), badgeFrame);
            badgeLabel->setStyleSheet("color: white; font-size: 11px; font-weight: 600;");
            badgeLabel->setAlignment(Qt::AlignCenter);
            badgeLabel->setFixedSize(18, 18);

            messageLayout->addWidget(badgeFrame);
        }

        vLayout->addWidget(messageRow);

        hLayout->addWidget(textContainer);
        hLayout->addStretch();//添加一个拉伸项(弹簧),将文本容器向左对齐

        listWidget->setItemWidget(item, container);
    }
}

// 从联系人页双击 / 右键"打开会话"进来时调用：把会话列表里对应的行选中并滚到可见处。
// 只做视觉高亮，不发 contactSelected —— 选中逻辑统一由 ChatWindow::openConversation
// 走 onContactSelected，这里再发一次会让"存草稿 / 拉消息"被重复执行两遍
void MessageList::highlightContact(const QString& contactId)
{
    for (int i = 0; i < listWidget->count(); ++i)
    {
        QListWidgetItem* item = listWidget->item(i);
        if (item->data(Qt::UserRole).toString() == contactId)
        {
            listWidget->setCurrentItem(item);
            listWidget->scrollToItem(item, QAbstractItemView::PositionAtCenter);
            return;   // 列表里 ID 唯一，找到就收工
        }
    }
    // 没找到也不报错：该联系人可能还没出现在会话列表里，聊天照常能打开
}

void MessageList::onItemClicked(QListWidgetItem* item)//用户点击了列表里的某一行，Qt 自动调用这个函数。
{
    QString contactId = item->data(Qt::UserRole).toString();//获取点击的项的用户角色数据(联系人ID)


    QWidget* widget = listWidget->itemWidget(item);//获取点击的项对应的 QWidget
    // 先判空：itemWidget() 在「这一行没调过 setItemWidget」时会返回 nullptr，
    // 而下一行的 widget->layout() 会直接解引用它 —— 空指针解引用当场崩溃。
    // 注意 qobject_cast 救不了这种情况：函数参数要先求值，轮到它时已经崩了
    if (!widget) {
        return;
    }
    // 再校验布局。为什么非要运行时查：onItemClicked 是信号槽回调，item 由 Qt 运行时递进来，
    // 编译期管不了它的内部结构；而下面的代码是「盲走下标」(itemAt(1)/itemAt(0))，
    // 一旦结构不符合 setConversations 里造的约定，itemAt 越界只返回 nullptr 而不抛异常，
    // 紧接着 ->widget() 就又是空指针解引用 → 崩溃。所以校验和取下标必须成对写
    QHBoxLayout* hLayout = qobject_cast<QHBoxLayout*>(widget->layout());//获取 QWidget 的水平布局
    if (!hLayout) {
        return;
    }
    QString contactName = "未知联系人";//默认名字
    if (hLayout && hLayout->count() > 1) //判断布局是否有效，控件至少为两个
    {
        QWidget* textContainer = hLayout->itemAt(1)->widget();//获取文本容器
        //itemAt (index) = 从布局里，拿出「第几个」控件的指针，从0下标开始，这里获取的是文本容器
        QVBoxLayout* vLayout = qobject_cast<QVBoxLayout*>(textContainer->layout());//获取文本容器的垂直布局
        if (vLayout && vLayout->count() > 0)
        {
            QWidget* nameRow = vLayout->itemAt(0)->widget();//获取姓名行
            QHBoxLayout* nameLayout = qobject_cast<QHBoxLayout*>(nameRow->layout());//获取姓名行的水平布局
            if (nameLayout && nameLayout->count() > 0)
            {
                QLabel* nameLabel = qobject_cast<QLabel*>(nameLayout->itemAt(0)->widget());//获取姓名标签
                if (nameLabel) {
                    contactName = nameLabel->text();//获取姓名标签的文本内容
                }
            }
        }
    }

    emit contactSelected(contactId, contactName);//发送联系人选择信号,包含联系人ID和姓名
}

// 右键会话条目 → 弹菜单，目前只有"删除会话"一项
void MessageList::onContextMenuRequested(const QPoint& pos)
{
    // pos 是 viewport 坐标（真正接收鼠标事件的是 viewport），itemAt 正好吃这个坐标系；
    // 落在空白处返回 nullptr，此时不弹菜单
    QListWidgetItem* item = listWidget->itemAt(pos);
    if (!item) {
        return;
    }
    // 先选中，让用户看清这个菜单是冲哪一行来的（setCurrentItem 不会触发 itemClicked）
    listWidget->setCurrentItem(item);

    const QString contactId = item->data(Qt::UserRole).toString();

    QMenu menu(this);
    applyMenuStyle(menu);
    QAction* deleteAction = menu.addAction("删除会话");

    // exec 是阻塞的，返回时菜单已关闭；用返回值判断选中项而不是连 triggered，
    // 免去"在 triggered 回调里删掉正在 exec 的菜单"这类坑。
    // 弹菜单要用屏幕坐标，所以把 viewport 坐标 map 出去
    QAction* chosen = menu.exec(listWidget->viewport()->mapToGlobal(pos));

    if (chosen == deleteAction) {
        emit deleteConversationRequested(contactId);   // 交给 ChatWindow 转主后端删库，本控件不动数据
    }
}
