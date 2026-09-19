#ifndef CHATAREA_H
#define CHATAREA_H

#include <QWidget>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QWidget>
#include <QLabel>
#include <QMap>
#include "ChatBackend.h"
#include "MessageStatusIndicator.h"

class ChatInput;
class MessageItem;

class ChatArea : public QWidget
{
    Q_OBJECT
public:
    explicit ChatArea(QWidget *parent = nullptr);
    void setMessages(const QList<MessageInfo>& messages);
    void setContactName(const QString& name);
    // 追加一条消息到聊天区末尾（实时收发的新消息走这里，默认滚到底部）
    void addMessage(const MessageInfo& message, bool scrollToBottom = true);
    // 把一页历史消息插到聊天区顶部（翻页加载更早记录走这里），并保持当前滚动位置
    void prependMessages(const QList<MessageInfo>& messages);
    // 翻页时UI要判断"这次滚顶是用户翻历史还是程序自己滚的"，由外部设置加载中标志
    void setLoadingOlder(bool loading) { m_loadingOlder = loading; }
    void setMessageStatus(const QString& messageId, MessageStatusIndicator::Status status);  // 按消息ID切换发送状态（发送中/失败）
    void clearMessages();
    void clearInput();  // 清空输入框
    void setInputVisible(bool visible);  // 设置输入框可见性
    void setInputContent(const QString& content);  // 设置输入框内容
    QString getInputContent();  // 获取输入框内容

signals:
    void sendMessage(const QString& content);
    void sendFile(const QString& filePath);
    void retrySend(const QString& messageId, LoginFeature feature);  // 用户点击失败感叹号 → 请求重发该消息（带功能枚举供后端路由）
    // 滚动条到顶：用户想看更早的历史，请求加载上一页（是否真的翻到底由后端 hasMore 控制）
    void loadOlderMessages();

private:
    // 单条消息 → [气泡+时间标签] 对，追加和前插共用，避免两份几乎一样的布局代码
    void appendMessageWidgets(const MessageInfo& message, bool atTop);

    QVBoxLayout* mainLayout;
    QVBoxLayout* messagesLayout;
    QScrollArea* scrollArea;
    QWidget* messagesWidget;
    QLabel* headerLabel;
    QWidget* headerLabelWidget;
    QHBoxLayout* headerLayout;
    QLabel* headerOnlineLabel;
    ChatInput* chatInput;  // 输入框作为聊天区域的一部分
    QMap<QString, MessageItem*> m_messageItems;  // 消息ID → 消息气泡项（供按ID切换发送状态）
    bool m_loadingOlder = false;  // 正在加载更早的历史页（防滚顶重复触发）
};

#endif // CHATAREA_H