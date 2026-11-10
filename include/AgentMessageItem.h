#ifndef AGENTMESSAGEITEM_H
#define AGENTMESSAGEITEM_H

#include <QWidget>
#include <QString>
#include <QStringList>
#include <QTimer>

class QTextEdit;

// 智能体聊天区的单条消息气泡（不带头像，与普通聊天页的 MessageItem 区分开）
// 流式输出：appendStreamChunk 先进缓冲，30ms 合流一次再排版（服务器单字符一包，
//           逐包排版会把主线程压死）
// AI 回答按 Markdown 渲染，用户提问仍是纯文本
// HITL：底部预留一块默认隐藏的操作按钮容器，本期只保存参数不建按钮
class AgentMessageItem : public QWidget
{
    Q_OBJECT
public:
    explicit AgentMessageItem(const QString& content, bool isSelf, QWidget* parent = nullptr);

    // 流式增量：只往缓冲里追加，不立刻排版（等 30ms 合流定时器或 flushPending 触发）
    void appendStreamChunk(const QString& chunk);

    // 把缓冲里剩下的文字立刻落屏（本轮结束/停止时调用，避免最后一段还要等 30ms）
    void flushPending();

    // HITL 预留：本期仅记录动作列表，容器保持隐藏（后续再往里填按钮）
    void setHitlActions(const QStringList& actions);

private slots:
    void onFlushTimeout();

private:
    void setupUI(bool isSelf);
    // 把 m_markdownText 整篇重新渲染进 QTextEdit（AI 走 Markdown，自己走纯文本）
    void renderContent();
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

    bool m_isSelf = false;              // true = 用户提问（纯文本），false = AI 回答（Markdown）
    QString m_markdownText;             // 已落屏的全文（AI 分支重渲染的源文本）
    QString m_pendingText;              // 还没落屏的增量（合流缓冲）
    QTimer* m_flushTimer = nullptr;     // 30ms 合流定时器（单次，收到增量才启动）
};


#endif // AGENTMESSAGEITEM_H
