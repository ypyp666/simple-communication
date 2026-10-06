#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QSet>
#include"MainBackend.h"

class StatusBar;
class ChatWindow;
class ContactWindow;
class SearchWindow;

/*
 QMainWindow（你的 MainWindow 主窗口）
 专门做主窗口，核心用途：显示聊天界面、状态信息、账号管理等。
 常用布局：水平布局，包含聊天页面、状态页面、账号管理页面等。
 尺寸固定，一般是大窗口；
 不存在阻塞函数，显示直接show()即可。
 不存在模态问题，所有操作都是异步的。
 程序主体长期运行，等待用户交互。
 主窗口关闭后，程序退出。
*/ 

class MainWindow : public QMainWindow
{
    Q_OBJECT
    Q_DISABLE_COPY(MainWindow)
public:
    explicit MainWindow(QWidget *parent = nullptr, MainBackend* backend = nullptr);
    ~MainWindow();
    MainBackend* m_backend;

signals:
    // 页面切换信号
    void pageChanged(int index);

    // 请聊天页切到指定联系人的会话。
    // 跨页面一律走信号槽，不出现"A 页面直接调 B 页面方法"的抄近路
    void openConversationRequested(const QString& contactId, const QString& contactName);

    // 用户关闭主窗口：main.cpp 的"登录窗 ↔ 主窗口"循环靠它收尾
    // （收到它且不是"被踢"= 用户要退出程序）
    void closed();

protected:
    // 点窗口 X 时触发：先发 closed 通知 main.cpp，再照常关闭。
    // "被踢回登录页"不走这里——那条路是直接把窗口销毁（见 main.cpp）
    void closeEvent(QCloseEvent* event) override;

public slots:


private:
    void initUI();
    void initPages();
       // 切换到指定页面
    void switchToPage(int index);
    // 后端通讯录回包 → 喂给左栏 ContactList。
    // 【临时】原本是信号直连 ContactList::setContacts，为了空列表时补假好友才落成具名槽
    void onContactsLoaded(const QList<ContactInfo>& contacts);
    // 单击通讯录好友 → 右栏切"好友详情"并灌数据（切页 + 填内容一起做）
    void onContactSelectedInList(const QString& contactId, const QString& contactName);
    // 联系人页双击 / 右键"打开会话"：先切页并同步导航选中态，再把后半程转给聊天页
    void onContactOpened(const QString& contactId, const QString& contactName);
    // 删除好友：涉及后端数据，必须经 MainBackend；接口未就绪，当前为空实现
    void onDeleteContactRequested(const QString& contactId, const QString& contactName);
    // 详情页「删除好友」按钮：与右键菜单那条是同一个动作（都等服务器确认成功后才删本地库）。
    // 详情页的信号只带 contactId、不带名字，所以没法复用上面那个两参版本
    void onFriendDetailDeleteRequested(const QString& contactId);

    // === "新朋友"（好友申请）===
    // 点左栏"新朋友"行：右栏共用区域切到请求列表、置加载态，再向主后端要一次申请列表
    void onNewFriendRequested();
    // 卡片上的「同意 / 拒绝」 → 交给主后端发请求（本类不碰业务后端）
    void onFriendAcceptRequested(const QString& requestId);
    void onFriendRejectRequested(const QString& requestId);
    // 服务器确认同意 / 拒绝：那条申请从内存列表里摘掉
    void onFriendRequestAccepted(const QString& requestId);
    void onFriendRequestRejected(const QString& requestId);
    // 同意 / 拒绝失败：标题栏下方挂一条红字提示（列表不动，那条申请还在）
    void onFriendRequestAcceptFailed(const QString& requestId, const QString& message);
    void onFriendRequestRejectFailed(const QString& requestId, const QString& message);

    // === 状态栏红点 ===
    // 会话列表回包 → 汇总各会话未读数，点亮"消息"导航按钮的红点
    void onConversationsLoaded(const QList<ConversationInfo>& conversations);
    // 收到一条好友申请 → 只统计"别人发给我的"，累计到"联系人"导航按钮的红点
    void onFriendRequestReceived(const FriendRequestInfo& request);

    // 把当前未查看的好友申请标记为已查看（进"新朋友"页时调用）→ 联系人按钮红点清零
    void markFriendRequestsSeen();
    // 按 m_unseenFriendRequestIds 刷新"联系人"导航按钮的红点数字
    void refreshFriendRequestBadge();
    // 用户此刻是否正停在"新朋友"页：正看着时新来的申请直接算已查看，不点亮红点
    bool isViewingFriendRequests() const;
    // 一次"新朋友"拉取收尾（成功 / 失败都调）：撤掉拉取窗口的"一律算已查看"标记
    void onFriendRequestPullFinished();

    QWidget* centralWidget;
    QHBoxLayout* mainLayout;
    QStackedWidget* pageStack;  // 页面管理器

    // 需要跨函数 / 跨页面用到的子控件，必须提升为成员（原来是局部变量）
    StatusBar* m_statusBar = nullptr;
    ChatWindow* m_chatWindow = nullptr;
    ContactWindow* m_contactWindow = nullptr;
    SearchWindow* m_searchWindow = nullptr;   // 搜索页：切走时要清空它的搜索框，必须留成成员
    // 通讯录缓存：单击好友时右栏要填详情，而 contactSelected 只带 id + 名字（缺备注），
    // 所以把后端回包的那份留着查
    QList<ContactInfo> m_contacts;

    // 好友申请红点的两份状态（按 requestId 去重：服务器每次拉取都会把所有申请重推一遍）：
    //   m_seenFriendRequestIds   —— 已查看 / 已处理过的申请，重推时不再点数
    //   m_unseenFriendRequestIds —— 别人发来、尚未查看的申请，就是红点的数字
    QSet<QString> m_seenFriendRequestIds;
    QSet<QString> m_unseenFriendRequestIds;
    // 一次"新朋友"拉取是否还在飞行中。进页面后服务器会把全部申请重推一遍，
    // 若用户没等回包就切走，"正在看页面"的判断会失效、旧申请会被又点成红点。
    // 所以拉取窗口内收到的申请一律算已查看，回包（成功或失败）才撤标记
    bool m_friendRequestPullActive = false;

    // 页面索引常量（顺序与 StatusBar 的导航按钮一致：聊天 → 联系人 → 搜索 → Agent）
    static const int CHAT_PAGE_INDEX = 0;
    static const int STATUS_PAGE_INDEX = 1;
    static const int SEARCH_PAGE_INDEX = 2;
};

#endif // MAINWINDOW_H