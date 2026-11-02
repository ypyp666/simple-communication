#ifndef AGENTCHATAREA_H
#define AGENTCHATAREA_H

#include <QWidget>
#include <QMap>
#include <QList>
#include <QString>
#include <QStringList>
#include <QIcon>              // 标题条图标按钮的两档图标（正常 / 置灰）
#include "FeatureStructs.h"   // AgentMessageInfo：历史回读的入参结构体

class QLabel;
class QStackedWidget;
class QScrollArea;
class QVBoxLayout;
class QPushButton;
class QResizeEvent;
class ChatInput;
class AgentMessageItem;

// 智能体专属聊天区：
//   - 默认空态：页面中间一个「新建会话」按钮
//   - 点击后切到聊天态：消息滚动区 + 输入框（复用 ChatInput）
//   - 消息气泡用 AgentMessageItem，预留流式拼接与 HITL 按钮
class AgentChatArea : public QWidget
{
    Q_OBJECT
public:
    explicit AgentChatArea(QWidget* parent = nullptr);

    void setContactName(const QString& name);
    // 发送按钮的"回答中"态：true=换成蓝色圆形 stop 图标，false=回到"发送"两个字。
    // 只是外观，真正的开关由本类在发消息/收到 finished/失败时自己切
    void setSendingState(bool sending);
    // 当前会话标题（标题条中间那行字）：切会话 / 新建 / 改名后由上层同步过来
    void setSessionTitle(const QString& title);
    // 点「新建会话」：只生成 thread_id 并把请求抛给上层（先发服务器，本地还不写库），
    // 期间两颗新建按钮置灰；服务器确认后才进聊天页（见 onSessionCreateSucceeded）
    void requestNewSession();
    // 服务器确认创建成功：本地已入库，这时才真正切进新会话（清屏 + 用服务器确认的名字）
    void onSessionCreateSucceeded(const QString& sessionId, const QString& title);
    // 创建失败/超时：本地没写过东西，留在当前页面，按钮重新点亮
    void onSessionCreateFailed();
    // 新建按钮的"等待中"态：创建请求在飞期间置灰，防止连点建出一堆会话
    void setNewSessionEnabled(bool enabled);
    // 切到指定会话（点抽屉里的会话用）：清屏 + 换成该会话 ID，不再新建
    void openSession(const QString& sessionId);
    // 回到空态（会话被删除 / 无会话可用）：清屏 + 清掉当前会话 ID + 切回「新建会话」页
    void resetToEmpty();
    QString currentSessionId() const { return m_sessionId; }
    // 追加一条消息，返回内部消息 ID（流式/HITL 都靠这个 ID 找到气泡）
    QString addMessage(const QString& content, bool isSelf);
    // 流式预留：把增量片段拼进指定气泡；找不到 ID 就忽略（脏包防御）
    void appendStreamChunk(const QString& messageId, const QString& chunk);
    // HITL 预留：本期只转发给气泡保存，界面不显示按钮
    void setHitlActions(const QString& messageId, const QStringList& actions);
    // 服务器回答逐段到达：首次到达时开一个 AI 气泡，后续都拼进同一个气泡。
    // sessionId 对不上当前会话就忽略（回答属于别的会话）
    void receiveAgentChunk(const QString& sessionId, const QString& content, bool finished);
    // 请求失败：以 AI 消息的形式提示在聊天区里
    void receiveAgentFailure(const QString& sessionId, const QString& message);
    // 主动结束当前这一轮（离开智能体页时由上层调）：会话没在回答就什么都不做。
    // 切会话 / 新建 / 回空态时本类自己也会调（见 stopAnswerIfSending）
    void stopCurrentAnswer();
    // 历史回读结果：库里拼好的整条消息按时间正序，重建成一屏气泡。
    // sessionId 对不上当前会话就整包丢弃（回包总是晚于用户操作，可能已经切走）
    void showHistory(const QString& sessionId, const QList<AgentMessageInfo>& messages);

signals:
    // 用户点发送：只带会话 ID 和内容。消息 ID 由服务器分配，主后端拿到后
    // 先落库、再发 agent_request（气泡上那个 ID 只是本地显示用的）
    void sendMessage(const QString& sessionId, const QString& content);
    // 请求新建会话：把本地生成的 thread_id + 默认名字交给上层（服务器先建，
    // 成功后本地才入库；结果由上层用 onSessionCreateSucceeded/Failed 回调回来）
    void sessionCreateRequested(const QString& sessionId, const QString& title);
    // 要停掉这个会话正在回答的那一轮（主后端据此发 agent_stop + 清掉落库消息 ID）
    void stopAnswer(const QString& sessionId);

protected:
    // 窗口宽度变了：标题条里能放下的字数也变了，重新做一次省略号
    void resizeEvent(QResizeEvent* event) override;

private slots:
    // 输入框发出内容：本地先上屏一条自己的气泡，再把请求抛给上层
    void onInputSend(const QString& content);
    // 输入框在"停止态"下被点：结束本轮（不发新消息，已打的字也不清）
    void onStopClicked();

private:
    // 正在回答就停掉（切会话 / 新建会话 / 回空态 / 离开页面都走这一个口）：
    // 记下旧会话 ID → 清流式状态 + 按钮回"发送" → 喊 stopAnswer 让后端发停止包
    void stopAnswerIfSending();
    void appendMessageWidget(const QString& content, bool isSelf, const QString& id);
    void clearMessages();
    void scrollToBottom();
    QString newMessageId();
    // 按标签当前宽度把会话名做省略（名字太长时不硬裁，末尾给"..."）
    void updateSessionTitleText();

    QLabel* m_headerLabel = nullptr;            // 顶部标题（"小C"）
    QPushButton* m_emptyNewBtn = nullptr;       // 空态页中间那颗「新建会话」按钮（等回执期间一起置灰）
    bool m_createPending = false;               // 新建请求在飞（防止连点，也和按钮置灰互相兜底）
    QLabel* m_sessionTitleLabel = nullptr;      // 标题条中间：当前会话名（居中）
    QPushButton* m_newSessionBtn = nullptr;     // 标题条右侧：新建会话（纯图标按钮）
    // 标题条图标按钮的两档图标：QSS 管不到按钮图标，置灰时只能手动换一版灰图标
    QIcon m_newSessionIcon;                     // 正常态（蓝色）
    QIcon m_newSessionIconDisabled;             // 等回执期间（灰色）
    QString m_sessionTitle;                     // 当前会话名（省略号要按原始名字重新算）
    QStackedWidget* m_stack = nullptr;          // 页 0 = 空态，页 1 = 聊天态
    QScrollArea* m_scrollArea = nullptr;
    QVBoxLayout* m_messagesLayout = nullptr;
    ChatInput* m_input = nullptr;               // 直接复用现有输入框
    QMap<QString, AgentMessageItem*> m_items;   // 消息 ID -> 气泡（流式/HITL 定位用）
    QString m_sessionId;                        // 当前会话 ID（切会话/新建时改变）
    QString m_streamMessageId;                  // 正在接收的 AI 气泡 ID（空=本轮还没开始）
    // 发送按钮当前处于哪种状态：false=发送态（发 agent_request），true=停止态（本轮还在回答）。
    // 同一颗按钮两种语义对应两种包，必须把状态记在这，发的时候才知道该发哪种
    bool m_sending = false;
};

#endif // AGENTCHATAREA_H
