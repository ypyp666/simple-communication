#include "ContactWindow.h"
#include "ContactList.h"
#include "ContactDetailArea.h"
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>

// 布局思路与 ChatWindow 一致：QHBoxLayout 左右分栏，左栏固定宽、右栏自动拉伸填满窗口。
// 区别只在左栏内部：聊天页左栏直接就是会话列表，这里左栏是垂直布局
// —— 第 1 项标题框（"联系人"），第 2 项是联系人分组列表 ContactList
// （"新朋友"行、分割线、"我的好友"折叠分组都收在 ContactList 内部）
ContactWindow::ContactWindow(QWidget *parent) : QWidget(parent)
{
    QHBoxLayout* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ===== 左栏：固定宽 280（和聊天页左栏同宽），内部从上到下垂直排 =====
    QWidget* leftPanel = new QWidget(this);
    leftPanel->setObjectName("contactLeftPanel");
    leftPanel->setFixedWidth(280);
    // 用 #id 选择器限定作用范围，避免样式污染子控件
    leftPanel->setStyleSheet(R"(
        #contactLeftPanel {
            background-color: rgba(245, 245, 245, 0.43);
            border-right: 1px solid rgba(224, 224, 224, 0.58);/*右侧分割线*/
        }
    )");

    QVBoxLayout* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);

    // 标题框（垂直布局第 1 项）：固定高度，不参与拉伸，只显示"联系人"三个字
    QWidget* titleBar = new QWidget(leftPanel);
    titleBar->setObjectName("contactTitleBar");
    titleBar->setFixedHeight(50);
    titleBar->setStyleSheet(R"(
        #contactTitleBar {
            background-color: white;
            border-bottom: 1px solid #e0e0e0;
        }
    )");

    QHBoxLayout* titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(15, 0, 15, 0);
    titleLayout->setSpacing(0);

    QLabel* titleLabel = new QLabel("联系人", titleBar);
    titleLabel->setStyleSheet("color: black; font-size: 16px; font-weight: 600;");
    titleLayout->addWidget(titleLabel);
    titleLayout->addStretch();//弹簧：标题靠左

    leftLayout->addWidget(titleBar);

    // 联系人分组列表（垂直布局第 2 项）：权重 1 = 剩余高度全给它。
    // "新朋友"行、分割线、"我的好友"折叠分组、联系人条目都在 ContactList 内部
    m_contactList = new ContactList(leftPanel);
    leftLayout->addWidget(m_contactList, 1);

    mainLayout->addWidget(leftPanel);

     QFrame* divider = new QFrame(this);
    divider->setFrameShape(QFrame::VLine);  // 垂直分隔线
    divider->setStyleSheet("color: #e0e0e0;");
    mainLayout->addWidget(divider);

    // ===== 右栏：整块交给 ContactDetailArea（权重 1 = 自动占满剩余宽度）=====
    // 这块区域是"好友详情"和"好友请求列表"共用的，谁显示谁屏蔽由该类的
    // showFriendDetail() / showFriendRequests() 决定，本页不掺和
    m_detailArea = new ContactDetailArea(this);
    mainLayout->addWidget(m_detailArea, 1);

    // 本窗口刻意不做任何信号接线：ContactList / ContactDetailArea（及其内部的
    // FriendRequestList）的对外信号，以及后端 contactsLoaded → ContactList::setContacts，
    // 都统一收口在 MainWindow::initPages 末尾（纯 UI 链路走主页面，
    // 需要后端参与的走 MainBackend）。
    // 好处是"谁把会话打开了"只看一个文件就够，不用顺着各页面的转发往上拼；
    // 也不再出现"信号→信号直连"这种没有具名函数承载、断不了点也插不进逻辑的写法
}

ContactWindow::~ContactWindow()
{
}