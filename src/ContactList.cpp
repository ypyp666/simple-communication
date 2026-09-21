#include "ContactList.h"
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>

ContactList::ContactList(QWidget *parent) : QWidget(parent)
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
    自带点击选中，自带双击事件，不用你自己画，不用算坐标，直接 addItem 就能加内容，创建一个列表对象，this 表示它的父控件是 ContactList（左侧联系人面板）
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
    // ==================== 联系人列表 QListWidget 样式 ====================
    // QListWidget         : 列表整体样式（无边框、浅灰背景）
    // QListWidget::item   : 单个联系人条目（高70px、内边距、无边框）
    // item:hover          : 鼠标悬浮时背景变深灰
    // item:selected       : 选中时背景变蓝色（更深）
    // 效果：干净、现代、易点击、视觉清晰的联系人列表
    // =====================================================================
    layout->addWidget(listWidget);

    connect(listWidget, &QListWidget::itemClicked, this, &ContactList::onItemClicked);//把点击事件的信号发给 onItemClicked 方法处理
    //itemClicked : 点击列表项时触发，包含点击的项和点击的位置信息，QListWidget独有信号，直接发送被点击的行的指针

}

void ContactList::setContacts(const QList<ContactInfo>& contacts)//把后端给的联系人列表渲染出来
{
    listWidget->clear();//先清空旧列表

    for (const auto& contact : contacts)//遍历每一个联系人
    {
        QListWidgetItem* item = new QListWidgetItem(listWidget);
        item->setData(Qt::UserRole, contact.id);//给这一行藏一个好友 ID点击这一行时，就能知道是哪个联系人。
        // UserRole : 是槽位编号
        item->setSizeHint(QSize(0, 62));

        QWidget* container = new QWidget();//创建一个容器，用来装所有的子控件
        container->setStyleSheet("background-color: transparent;");
        QHBoxLayout* hLayout = new QHBoxLayout(container);//创建一个水平布局，用来装所有的子控件
        hLayout->setContentsMargins(0, 0, 0, 0);
        hLayout->setSpacing(10);

        QLabel* avatarLabel = new QLabel();//创建一个标签，用来显示头像
        avatarLabel->setText(contact.name.left(1));
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

        QLabel* nameLabel = new QLabel(contact.name);
        nameLabel->setStyleSheet("font-weight: 600; font-size: 14px; color: #333;");
        nameLabel->setFixedHeight(18);
        nameLayout->addWidget(nameLabel);
        nameLayout->addStretch();

        QString timeStr = contact.lastTime.toString("HH:mm");
        if (contact.lastTime.date() != QDate::currentDate())
        {
            timeStr = contact.lastTime.toString("MM-dd");
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

        QLabel* messageLabel = new QLabel(contact.lastMessage);//创建文本标签,显示最后一条消息
        messageLabel->setStyleSheet("font-size: 12px; color: #8f8f8f;");//字体大小12px,颜色#8f8f8f
        messageLabel->setFixedHeight(16);//固定高度16px
        messageLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);//宽度自适应,高度固定
        messageLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);//左对齐,垂直居中
        messageLabel->setWordWrap(false);//不换行
        messageLabel->setTextInteractionFlags(Qt::NoTextInteraction);//不允许交互   
        messageLayout->addWidget(messageLabel);//添加文本标签到布局
        messageLayout->addStretch();

        if (contact.unreadCount > 0)
        {
            QFrame* badgeFrame = new QFrame();
            badgeFrame->setStyleSheet(R"(
                QFrame {
                    background-color: #f56c6c;
                    border-radius: 9px;
                }
            )");
            badgeFrame->setFixedSize(18, 18);

            QLabel* badgeLabel = new QLabel(QString::number(contact.unreadCount), badgeFrame);
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

void ContactList::onItemClicked(QListWidgetItem* item)//用户点击了列表里的某一行，Qt 自动调用这个函数。
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
    // 一旦结构不符合 setContacts 里造的约定，itemAt 越界只返回 nullptr 而不抛异常，
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
