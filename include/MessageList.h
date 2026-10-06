#ifndef MESSAGELIST_H
#define MESSAGELIST_H

#include <QWidget>
#include <QListWidget>
#include <QVBoxLayout>
#include <QHash>
#include "ChatBackend.h"

// 会话（消息）列表：微信左侧那一列——头像 + 名字 + 最后一条消息预览 + 时间 + 未读红点。
// 数据源是 ConversationInfo（它带的 lastMessage/lastTime/unreadCount 就是会话信息），
// 以前误叫 ContactList，真正的通讯录联系人列表交给占位的 ContactList（配合 ContactWindow）
class MessageList : public QWidget
{
    Q_OBJECT
public:
    explicit MessageList(QWidget *parent = nullptr);
    void setConversations(const QList<ConversationInfo>& conversations);
    // 名字兜底：会话表（conversations）本身不存名字，名字靠查询时 LEFT JOIN contacts 补；
    // 库里查不到（联系人还没入表）时这里给的"ID→名字"顶上，省得再查一次库。
    // 与 setConversations 谁先到都行：两边都只是把数据存下，再重画一遍
    void setContactNames(const QList<ContactInfo>& contacts);
    // 从联系人页打开会话时，把对应行选中并滚到可见处（纯视觉，不发 contactSelected）
    void highlightContact(const QString& contactId);

signals:
    void contactSelected(const QString& contactId, const QString& contactName);
    // 右键菜单"删除会话"：本控件只负责报"删哪一条"，真正删库由 ChatWindow 转给主后端。
    // 只带 contactId：会话表主键就是它，删除不需要名字
    void deleteConversationRequested(const QString& contactId);

private slots:
    void onItemClicked(QListWidgetItem* item);
    // 右键条目：弹菜单（目前只有"删除会话"）。空白处不弹
    void onContextMenuRequested(const QPoint& pos);

private:
    // 用当前这两份数据（会话快照 + 名字兜底）把列表整个重画一遍
    void rebuildItems();
    // 这一行最终显示的名字：会话自带的名字优先，为空才用通讯录兜底
    QString displayNameOf(const ConversationInfo& conversation) const;

    QListWidget* listWidget;
    QVBoxLayout* layout;
    QList<ConversationInfo> m_conversations;      // 最近一次 setConversations 的原始数据
    QHash<QString, QString> m_contactNames;       // 联系人 ID → 名字（来自通讯录）
};

#endif // MESSAGELIST_H
