#pragma once
#include <QWidget>
#include <QListWidget>
#include <QList>
#include "FeatureStructs.h"   // 引入 FriendRequestInfo（服务器逐条下发的在线数据）

class QLabel;
class QStackedWidget;

// 好友请求列表（"新朋友"页），样式照 QQ 的"好友通知"来：
//   顶部一条标题栏（"新朋友"）
//   下面整块浅灰底，一列白色圆角卡片，卡片里是
//     圆形头像 + 名字(蓝)/说明(灰)/日期(灰) + 第二行"留言：xxx"，右侧挂操作
//
// 条目结构是从聊天页的会话列表（MessageList）搬过来改的——
// 会话列表是"头像 + 名字/时间 + 最后一条消息 + 未读红点"，
// 这里换成"头像 + 名字/日期 + 留言 + 右侧同意/拒绝"，骨架一模一样。
//
// 与 ContactList 最大的不同：本类的数据【只存在内存】，绝不落库。
// 申请是服务器上的在线数据（服务器有就有，没有就没有），拉下来堆在 m_requests 里直接渲染。
//
// 三个状态用 QStackedWidget 切换：
//   页0 = 一句居中提示（"正在加载…" / "暂无新的朋友申请" / 拉取失败文案）
//   页1 = 真正的 QListWidget
// 空列表时若直接显示空白列表，用户分不清"没申请"和"坏了"。
class FriendRequestList : public QWidget
{
    Q_OBJECT
public:
    explicit FriendRequestList(QWidget *parent = nullptr);

    // 当前登录账号：用来区分一条申请的方向（别人发来的 → 给按钮；我发出的 → 只显示状态词）
    void setCurrentAccountId(const QString& accountId);

    // === 一次拉取的三个状态（由 MainWindow 按主后端的结果信号调用）===
    void setLoading();                        // 开始拉取：清空并显示"正在加载…"
    void setError(const QString& message);    // 拉取失败：显示错误文案（不能显示空列表冒充"没有申请"）
    void finishLoading();                     // 拉取完成：有内容就出列表，没内容就出空态

    // 服务器逐条下发入口。按 requestId 去重：服务器每次 pull 都会把全部申请重推一遍，
    // 用户也可以反复点"新朋友"，不去重就会越滚越多
    void addRequest(const FriendRequestInfo& request);
    // 同意 / 拒绝成功后把这条从内存列表里摘掉（服务器那边关系已处理完，本地不再显示）
    void removeRequest(const QString& requestId);

    // 同意 / 拒绝失败：在标题栏下方挂一条红字提示（项目里一律不用 QMessageBox 弹窗）
    void showOperationError(const QString& message);

signals:
    // 卡片右侧的「同意」/「拒绝」被点。参数是那条申请的服务器记录ID
    // （不是对方账号ID：服务器要靠它定位"处理的是哪一条申请"）
    void acceptRequested(const QString& requestId);
    void rejectRequested(const QString& requestId);

private:
    void rebuildList();                          // 按 m_requests 重渲染列表（条目量小，整体重画最省心）
    void showPlaceholder(const QString& text);   // 切到提示页
    int  indexOfRequest(const QString& requestId) const;  // 内存表里按 requestId 找下标（-1 表示没有）

    QLabel* m_tipLabel = nullptr;                // 操作失败提示条（默认隐藏，不占视觉）
    QStackedWidget* m_stack = nullptr;
    QLabel* m_placeholderLabel = nullptr;        // 提示页里那句居中文字
    QListWidget* m_listWidget = nullptr;
    QList<FriendRequestInfo> m_requests;         // 申请的内存缓存（本类自己持有，不落库）
    QString m_currentAccountId;                  // 当前登录账号，用于判断申请方向
};