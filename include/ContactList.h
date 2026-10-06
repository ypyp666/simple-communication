#pragma once
#include <QWidget>
#include <QListWidget>
#include <QVBoxLayout>
#include <QPixmap>
#include <QList>
#include "FeatureStructs.h"   // 引入 ContactInfo（通讯录好友资料，与 MessageList 的 ConversationInfo 分开）

class QPushButton;
class QLabel;
class QPropertyAnimation;
class QResizeEvent;

// 联系人（通讯录）列表：左栏从上到下四段
//   ① "新朋友"            固定高，悬浮/按下有底色变化
//   ② 分割线               固定高 1px
//   ③ "我的好友" 分组标题   固定高，点击展开/收起下方列表
//   ④ 联系人 QListWidget    高度由内容决定（内容少就贴着内容，内容多就占满并内部滚动）
//
// 布局末尾永远挂着一个 addStretch()。这是本类不"莫名撑开"的关键：
// 多余高度必须有个明确的归属，否则收起列表后 Qt 会把富余高度平摊给剩下的三个控件，
// 于是"新朋友""我的好友"被拉开、中间出现大片空白。有了弹簧，富余高度全归它，
// ①②③ 永远紧贴顶部，展开/收起只改变 ④ 占的高度。
//
// 注意区分：左侧"头像+最后消息+未读数"的会话列表是 MessageList，本类才是真正的通讯录。
// 本类不依赖 MainBackend，数据由外部调用 setContacts() 喂进来（分层与 MessageList 一致）。
class ContactList : public QWidget
{
    Q_OBJECT
public:
    explicit ContactList(QWidget *parent = nullptr);

    // 后端数据入口：MainWindow 把 MainBackend::contactsLoaded 接到这里。
    // 数据只从主后端出来，本类不碰任何业务后端（ContactBackend 等）
    void setContacts(const QList<ContactInfo>& contacts);

signals:
    // 双击条目 / 右键菜单"打开会话"
    void contactOpened(const QString& contactId, const QString& contactName);
    // 单击好友条目（不切页、不开会话）：右栏靠它切到"好友详情"。
    // 分组头 / "新朋友"行没有藏 ID，不会发这个信号
    void contactSelected(const QString& contactId, const QString& contactName);
    // 右键菜单"删除好友"（由 MainWindow 转给主后端，发删除请求给服务器）
    void deleteContactRequested(const QString& contactId, const QString& contactName);
    // 点击"新朋友"行：由 MainWindow 转给主后端，发 pull_friend_request 向服务器拉申请列表。
    // 本类只负责"喊一声"，不碰后端，也不自己切右栏（切页同样收口在 MainWindow）
    void newFriendRequested();

protected:
    // 左栏高度变了要重算列表能占多高（不重算的话，窗口拉大后列表还是旧高度）
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void onGroupHeaderClicked();                        // 展开 / 收起分组
    void onItemDoubleClicked(QListWidgetItem* item);    // 双击 → contactOpened
    void onItemClicked(QListWidgetItem* item);          // 单击 → contactSelected（右栏切详情）
    void onContextMenuRequested(const QPoint& pos);     // 右键 → 弹出菜单
    void onCollapseAnimFinished();                      // 折叠动画收尾
    void onNewFriendBtnClicked();                       // 点"新朋友"行 → newFriendRequested

private:
    void updateGroupChevron();                          // 按 m_groupExpanded 换箭头朝向
    int  listTargetHeight() const;                      // 列表"应该"占多高
    void applyListHeight();                             // 把上面算出的高度钉到列表和容器上

    QListWidget* listWidget = nullptr;
    QPushButton* m_newFriendBtn = nullptr;              // "新朋友"行
    QPushButton* m_groupHeaderBtn = nullptr;            // "我的好友"分组标题行
    QLabel* m_groupChevronLabel = nullptr;              // 标题行里的箭头
    QLabel* m_groupNameLabel = nullptr;                 // 标题行里的组名
    QWidget* m_listContainer = nullptr;                 // 承载 listWidget 的折叠容器（动画作用对象）
    QPropertyAnimation* m_collapseAnim = nullptr;
    bool m_groupExpanded = true;                        // 默认展开
    QPixmap m_chevronExpanded;                          // 构造时生成一次，不每帧渲染 SVG
    QPixmap m_chevronCollapsed;
    QList<ContactInfo> m_contacts;                      // 留一份数据（后续删除/重渲染用）
};