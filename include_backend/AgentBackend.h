#ifndef AGENTBACKEND_H
#define AGENTBACKEND_H

#include <QObject>
#include <QString>
#include <QHash>
#include <unordered_map>
#include "FeatureStructs.h"   // ConversationInfo：拉回来的会话列表（只填 id / name）

class TcpClient;
class QTimer;
class QJsonObject;

// 智能体后端：负责与服务器收发 agent_request / agent_response。
// 请求包只需 type + prompt + agentId（服务器从登录会话里取 account）；
// 回答由 worker 以 agent_response 逐段推回同一条连接（流式，直接拼接）。
class AgentBackend : public QObject
{
    Q_OBJECT
public:
    explicit AgentBackend(QObject* parent = nullptr, TcpClient* tcpClient = nullptr);
    // 登录成功后由主后端同步当前登录账号（预留：协议虽然从 session 取账号，本地仍留一份）
    void setUserId(const QString& userId);

signals:
    // 服务器推回的一段回答。finished=true 表示这一轮回答结束
    void agentMessageReceived(const QString& sessionId, const QString& content, bool finished);
    // 请求失败（未连接 / 服务器业务失败 code!=200 / 队列满 503）
    void agentRequestFailed(const QString& sessionId, const QString& message);
    // === 会话增删改的服务器回执（三件各一对信号，不共用一条）===
    // 新建：title 是这次落库用的会话名（回执包不带 title，是发请求时记下的）
    void agentSessionCreated(const QString& sessionId, const QString& title);
    void agentSessionCreateFailed(const QString& sessionId, const QString& message);
    // 改名：title 是这次要落库的新名字（同上，回执包不带）
    void agentSessionRenamed(const QString& sessionId, const QString& title);
    void agentSessionRenameFailed(const QString& sessionId, const QString& message);
    // 删除：只认会话 ID，成功没有额外数据
    void agentSessionDeleted(const QString& sessionId);
    void agentSessionDeleteFailed(const QString& sessionId, const QString& message);
    // 登录后拉回的服务器会话列表（每项只有 id + name，其它字段无意义）。
    // 只在"服务器明确回了成功"时才发；拉取失败不发，免得被当成"一条都没有"
    void agentSessionsPulled(const QList<ConversationInfo>& sessions);
    // === 提问第一步：服务器分配消息 ID ===
    // 用户点发送后先向服务器要 ID，拿到 ID 才能落库、才能发对话请求
    void agentMessageIdAllocated(const QString& sessionId, const QString& messageId);
    void agentMessageIdAllocateFailed(const QString& sessionId, const QString& message);

    // === 人机审批（HITL）四件套，各走各自的信号，不合并 ===
    // 服务器要求用户确认某次操作：UI 弹卡片，用户点同意/拒绝后走 sendAgentApprove
    void agentApprovalRequired(const AgentApprovalInfo& info);
    // 审批回执：serverId 用于对上是哪张卡；success=false 时 message 要显示出来
    //（常见是"没有待审批的任务，可能已被处理或已取消"），不能静默关框
    void agentApproveAcked(const QString& serverId, bool success, const QString& message);
    // 审批通过后服务器下发的可执行指令（不等流结束，先到先执行）
    void agentTaskExecute(const AgentTaskCommand& command);
    // 服务器告知这条指令被拒绝执行（例如用户拒绝了审批）：本地数据什么都不用做
    void agentTaskRejected(const QString& requestId);

public slots:
    // sessionId 只用于本地把回答对回发起它的会话；messageId 是服务器分配的消息 ID
    //（先 requestAgentMessageId 拿到、本地落库成功之后才调这里）；
    // prompt 就是用户输入的问题（服务器限制非空、长度 <= 4000）
    void sendAgentRequest(const QString& sessionId, const QString& messageId, const QString& prompt);
    // 停止本轮回答：给服务器发 agent_stop，同时本地立刻放掉"在飞"标记
    //（按钮已经不再是回答中，用户马上能重新提问；停掉后漏过来的片段一律丢弃）
    void stopAgentRequest(const QString& sessionId);
    // === 会话三件套（新建 / 改名 / 删除）===
    // 本地库一律由主后端在"收到成功回执之后"才动，这里只管服务器往返 + 5 秒超时
    void sendAgentSessionCreate(const QString& sessionId, const QString& title);
    void sendAgentSessionRename(const QString& sessionId, const QString& title);
    void sendAgentSessionDelete(const QString& sessionId);
    // 登录成功后拉服务器会话列表：请求只带 userId，回包是一串 (会话ID, 会话名)
    void pullAgentSessions();
    // 提问第一步：向服务器申请这条消息的 ID（请求只带 sessionId + userId）
    void requestAgentMessageId(const QString& sessionId);
    // 审批回执：threadId / serverId 原样回显服务器下发的审批包，
    // approved=false 时服务端会直接终止任务、不执行任何操作
    void sendAgentApprove(const QString& threadId, const QString& serverId,
                          bool approved, const QString& note);
    void onTcpDataReceived(const QByteArray& packet);

private:
    // 拉取回执：解析数组后整包上抛，本地怎么核对由 DB 层做
    void handleAgentSessionsList(const QJsonObject& response);
    // 申请消息 ID 的回执：解析出 messageId 上抛
    void handleMessageCreated(const QJsonObject& response);
    // 流式输出看门狗超时：5 分钟没有任何输出就按失败中止本轮
    void onStreamWatchdogTimeout();
    // 回执处理：三种操作各一个函数，进门时按 op 字段分派（见 onTcpDataReceived）
    void handleCreateSessionResponse(const QJsonObject& response);
    void handleRenameSessionResponse(const QJsonObject& response);
    void handleDeleteSessionResponse(const QJsonObject& response);
    // 超时处理：三种操作各一个，5 秒没回执就按失败上抛
    void onCreateSessionTimeout(const QString& sessionId);
    void onRenameSessionTimeout(const QString& sessionId);
    void onDeleteSessionTimeout(const QString& sessionId);
    // === 审批四件套的解析（一种包一个函数，进门按 type 分派）===
    void handleApprovalRequired(const QJsonObject& response);
    void handleApproveAck(const QJsonObject& response);
    void handleTaskExecute(const QJsonObject& response);
    // 审批回执超时：解锁卡片按钮并提示，不能让它永久停在"已提交"
    void onApproveTimeout(const QString& serverId);

    // std::unordered_map 默认不认识 QString，补一个基于 qHash 的哈希函数
    // （Qt 没给 QString 提供 std::hash 特化，只能自己带一个）
    struct QStringHash {
        std::size_t operator()(const QString& key) const noexcept
        {//不会抛异常
            return qHash(key);
        }
    };

    TcpClient* m_tcpClient;
    QString m_userid;
    // 同一时刻只允许一个智能体请求在飞：服务器回包不带会话字段，
    // 只能靠"谁发谁记"把回答对回发起时的会话
    QString m_pendingSessionId;
    // 会话 ID -> 本轮正在跑的 prompt。有键 = 该会话正在回答，禁止再发起第二轮；
    // 收到 finished=true 或失败（本轮结束）时把键删掉，会话才能再发
    std::unordered_map<QString, QString, QStringHash> m_inFlightSessions;
    // === 会话三件套的在飞登记（三件各自一套，不混在一个表里）===
    // 新建：键 = sessionId。有键 = 这次创建还在等回执（重复点击直接忽略）
    QHash<QString, QTimer*> m_createSessionTimers;
    // 新建要落库的标题（回执包不带 title，成功时只能从这里取）
    QHash<QString, QString> m_createSessionTitles;
    // 改名：同上，计时器 + 新名字（新名字是成功后写本地库用的）
    QHash<QString, QTimer*> m_renameSessionTimers;
    QHash<QString, QString> m_renameSessionTitles;
    // 删除：只认会话 ID，不需要记名字
    QHash<QString, QTimer*> m_deleteSessionTimers;
    // 流式输出看门狗：收到"请求已受理"确认包后启动，每收到一段输出就重置；
    // 超过 kStreamTimeoutMs 一直没有输出 → 判定卡死，本地中止本轮
    QTimer* m_streamWatchdog = nullptr;
    // === 审批回执的在飞登记 ===
    // 键 = serverId（服务器下发的 messageId）。有键 = 这次审批还在等 agent_approve_ack，
    // 重复点击直接忽略；回执 / 超时后删掉
    QHash<QString, QTimer*> m_approveTimers;
};

#endif // AGENTBACKEND_H
