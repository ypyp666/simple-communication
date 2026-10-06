#pragma once
#include <QWidget>
#include <QListWidget>
#include <QList>
#include "FeatureStructs.h"   // 引入 ContactInfo（搜索结果就是一批用户资料）

class QLabel;
class QStackedWidget;

// 搜索结果列表（搜索页下半部分）：
//   一列白色圆角卡片，卡片里是 圆形头像 + 用户名 + 账号ID，右侧挂一个「添加」按钮
//
// 排版是从"新朋友"页的 FriendRequestList 搬过来改的——那边是
// "头像 + 名字/说明/日期 + 留言 + 右侧同意/拒绝"，这边内容更少，
// 只留"头像 + 名字/ID + 右侧添加"，骨架一模一样
//（外层透明容器负责留白，里面才是白卡片 QFrame；卡片之间靠空隙"浮"起来）。
//
// 与 FriendRequestList 一样：数据只存在内存（服务器搜出来什么就显示什么），绝不落库。
// 搜索是"一问一答"——一次请求回来一整批，所以入口是 setResults（整批替换），
// 不像好友申请那样"逐条下发、收一条加一条"。
//
// 三个状态用 QStackedWidget 切：
//   页0 = 一句居中提示（"输入账号或昵称搜索用户" / "正在搜索…" / "没有找到相关用户" / 失败文案）
//   页1 = 真正的 QListWidget
// 空列表时若直接显示空白列表，用户分不清"没搜到"和"坏了"。
class SearchResultList : public QWidget
{
    Q_OBJECT
public:
    explicit SearchResultList(QWidget *parent = nullptr);

    void setLoading();                                   // 开始搜索：清空并显示"正在搜索…"
    void setResults(const QList<ContactInfo>& results);  // 搜索返回：有内容出列表，没内容出空态
    void setError(const QString& message);               // 搜索失败：显示错误文案（不能拿空列表冒充"没搜到"）

signals:
    // 卡片右侧「添加」被点。带上整条 ContactInfo（不只是 ID）——
    // 弹出的"申请加好友"小窗要拿名字/头像去渲染，只给 ID 还得再回查一遍
    void addRequested(const ContactInfo& contact);

private:
    void rebuildList();                          // 按 m_results 重渲染列表（条目量小，整体重画最省心）
    void showPlaceholder(const QString& text);   // 切到提示页

    QStackedWidget* m_stack = nullptr;
    QLabel* m_placeholderLabel = nullptr;        // 提示页里那句居中文字
    QListWidget* m_listWidget = nullptr;
    QList<ContactInfo> m_results;                // 搜索结果的内存缓存（本类自己持有，不落库）
};