#pragma once
#include <QWidget>

class QStackedWidget;
class FriendRequestList;
class FriendDetailPage;

// 联系人页右栏那块区域的共用容器。
//
// 这块区域被两个页面轮流用：【好友详情】和【好友请求列表】，所以要专门写一个类来管，
// 而不是把两套东西都摊在 ContactWindow 里。内部一个 QStackedWidget，
// 谁显示、谁屏蔽由下面两个方法说了算——同一时刻只可能有一个露脸：
//   好友列表来的信号（点联系人）→ showFriendDetail()   显示好友详情，请求列表收起
//   新朋友来的信号（点"新朋友"）→ showFriendRequests() 显示请求列表，好友详情收起
//
// 本类依旧不接线（沿用 ContactWindow 的规矩），两个方法都由 MainWindow 的具名槽调用。
class ContactDetailArea : public QWidget
{
    Q_OBJECT
public:
    explicit ContactDetailArea(QWidget *parent = nullptr);

    // 供 MainWindow 接线用
    FriendRequestList* friendRequestList() const { return m_friendRequestList; }
    // 好友详情页：灌数据（setContact）和三个按钮的信号都在它身上
    FriendDetailPage* friendDetailPage() const { return m_friendDetailPage; }

    void showEmpty();            // 切到空白页（没点任何好友 / 没进"新朋友"时）
    void showFriendDetail();     // 切到好友详情页
    void showFriendRequests();   // 切到好友请求列表页

    // 当前是否正显示"好友请求列表"。MainWindow 靠它判断"用户是否正看着新朋友页"——
    // 正看着时新来的申请直接算已查看，不点亮状态栏联系人按钮的红点
    bool isShowingFriendRequests() const;

private:
    QStackedWidget* m_stack;
    QWidget* m_emptyPage;                   // 空白页（默认停这，右栏"什么都没有"的样子）
    FriendDetailPage* m_friendDetailPage;   // 好友详情（单独一个类，见 FriendDetailPage.h）
    FriendRequestList* m_friendRequestList;

    // 页码常量，不用裸数字（顺序 = addWidget 的顺序）
    static const int EMPTY_INDEX = 0;
    static const int FRIEND_DETAIL_INDEX = 1;
    static const int FRIEND_REQUEST_INDEX = 2;
};