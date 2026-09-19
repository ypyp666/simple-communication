#ifndef MESSAGEITEM_H
#define MESSAGEITEM_H

#include <QWidget>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTextEdit>
#include <QTextOption>
#include "ChatBackend.h"
#include "MessageStatusIndicator.h"

class MessageItem : public QWidget
{
    Q_OBJECT
public:
    explicit MessageItem(const MessageInfo& message, QWidget *parent = nullptr);

    // 切换消息发送状态（发送中/失败），用于发送等待动画
    void setStatus(MessageStatusIndicator::Status status);

signals:
    // 用户点击失败感叹号 → 请求重发该消息；携带功能枚举（气泡固定 MessageSend）供后端路由
    void retryRequested(const QString& messageId, LoginFeature feature);

private:
    void setupUI(const MessageInfo& message);

    QString m_messageId;                        // 消息ID（供按ID切换状态）
    MessageStatusIndicator* m_statusIndicator;  // 状态指示器（自己发送的消息才有，气泡左侧）
};

#endif // MESSAGEITEM_H