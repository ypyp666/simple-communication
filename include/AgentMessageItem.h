#ifndef AGENTMESSAGEITEM_H
#define AGENTMESSAGEITEM_H

#include <QWidget>
#include <QString>
#include <QStringList>

class QTextEdit;

// 智能体聊天区的单条消息气泡（不带头像，与普通聊天页的 MessageItem 区分开）
// 流式输出：appendStreamChunk 往同一个气泡里增量拼接
// HITL：底部预留一块默认隐藏的操作按钮容器，本期只保存参数不建按钮
class AgentMessageItem : public QWidget
{
    Q_OBJECT
public:
    explicit AgentMessageItem(const QString& content, bool isSelf, QWidget* parent = nullptr);

    // 流式增量：把新到的片段追加到已有文本末尾，并合并重算气泡尺寸
    void appendStreamChunk(const QString& chunk);

    // HITL 预留：本期仅记录动作列表，容器保持隐藏（后续再往里填按钮）
    void setHitlActions(const QStringList& actions);

private:
    void setupUI(bool isSelf);
    // 按文本实际内容重算控件宽高与气泡宽度（从 MessageItem 的自适应算法抽出，
    // 流式拼接后可以反复调用）
    void refreshBubbleSize();
    // 连续多个 chunk 只排一次重算（singleShot 期间到来的都合并掉）
    void scheduleResize();

    QTextEdit* m_contentEdit = nullptr;
    QWidget* m_bubbleWidget = nullptr;
    QWidget* m_hitlBar = nullptr;        // HITL 按钮容器（默认隐藏）
    QStringList m_pendingActions;        // 本期仅保存，不生成按钮
    bool m_resizePending = false;
};

#endif // AGENTMESSAGEITEM_H
