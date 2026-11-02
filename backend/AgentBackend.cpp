#include "AgentBackend.h"
#include "TcpClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QTimer>

namespace {
// 会话三件套的等待上限：超过就按"服务器没回"处理（界面那边据此回滚 + 提示）
constexpr int kSessionOpTimeoutMs = 5000;
// 流式输出的静默上限：5 分钟没有任何输出才判超时。
// 给得这么宽是因为智能体先"思考"再吐字，思考阶段本来就没有任何回包
constexpr int kStreamTimeoutMs = 5 * 60 * 1000;
}

AgentBackend::AgentBackend(QObject* parent, TcpClient* tcpClient)
    : QObject(parent), m_tcpClient(tcpClient)
{
    // 看门狗：单发定时器，收到确认包才启动，每段输出重置一次（见 onTcpDataReceived）
    m_streamWatchdog = new QTimer(this);
    m_streamWatchdog->setSingleShot(true);
    connect(m_streamWatchdog, &QTimer::timeout, this, &AgentBackend::onStreamWatchdogTimeout);
}

void AgentBackend::setUserId(const QString& userId)
{
    m_userid = userId;
}

void AgentBackend::sendAgentRequest(const QString& sessionId, const QString& messageId, const QString& prompt)
{
    if (m_tcpClient == nullptr) {
        return;
    }
    // 一个会话同时只跑一轮问答：上一轮还没收到 finished/失败，就不再受理新请求
    if (m_inFlightSessions.find(sessionId) != m_inFlightSessions.end()) {
        emit agentRequestFailed(sessionId, "当前会话正在回答中，请等待本轮结束");
        return;
    }
    if (!m_tcpClient->isConnected()) {
        emit agentRequestFailed(sessionId, "网络未连接");
        return;
    }

    QJsonObject request;
    request["type"] = "agent_request";
    request["prompt"] = prompt;
    request["threadId"] = sessionId;
    request["userId"] = m_userid;
    // 带上服务器刚分配的消息 ID（本地已按它空创建过一条消息）
    request["messageId"] = messageId;

    // 先占坑再发：进入"本会话在飞"状态，直到本轮结束才删除
    m_inFlightSessions.emplace(sessionId, prompt);//就地构造元素进入哈希表
    // 回包不带会话字段，只能靠这里记下的 sessionId 把回答对回发起它的会话
    //（漏了这一句，回答的 sessionId 是空串，UI 和落库都会当成脏包丢掉）
    m_pendingSessionId = sessionId;

    QByteArray packet = QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n";
    qDebug() << "发送智能体请求：" << packet;
    m_tcpClient->sendData(packet);
}

// 停止本轮：agent_stop 告诉服务器中断生成（服务端按 threadId 定位那一轮），
// 本地不等服务器回执，立刻把"在飞"标记放掉——界面上按钮已经回到"发送"，
// 用户马上就能重新提问，不该被一条还没回来的停止回执卡住
void AgentBackend::stopAgentRequest(const QString& sessionId)
{
    if (sessionId.isEmpty()) {
        return;
    }

    if (m_tcpClient != nullptr && m_tcpClient->isConnected()) {
        QJsonObject request;
        request["type"] = "agent_stop";
        request["threadId"] = sessionId;
        request["userId"] = m_userid;

        QByteArray packet = QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n";
        qDebug() << "发送智能体停止包：" << packet;
        m_tcpClient->sendData(packet);
    }

    // 停掉的那个会话才算"本轮结束"：pending 要一起清，否则停止前已经在路上的
    // 片段还会被当成有效回答继续拼进气泡、继续落库
    if (m_pendingSessionId == sessionId) {
        m_pendingSessionId.clear();
        m_streamWatchdog->stop();   // 用户主动停，看门狗也一起收工
    }
    m_inFlightSessions.erase(sessionId);
}

// ===== 会话新建 =====
// 三件套各写各的：发包 / 超时 / 回执 三个函数只服务一件事，不共用状态表
void AgentBackend::sendAgentSessionCreate(const QString& sessionId, const QString& title)
{
    if (sessionId.isEmpty() || m_createSessionTimers.contains(sessionId)) {
        return;   // 没 ID；或者这次创建还在等回执（连点直接忽略）
    }

    if (m_tcpClient == nullptr || !m_tcpClient->isConnected()) {
        // 没连上就不用干等 5 秒：直接按失败上抛，界面立刻点亮按钮
        emit agentSessionCreateFailed(sessionId, QStringLiteral("网络未连接"));
        return;
    }

    QJsonObject request;
    request["type"] = "agent_session_create";
    request["sessionId"] = sessionId;   // 会话 ID 由客户端生成，服务器按它建
    request["userId"] = m_userid;
    request["title"] = title;           // 新建默认"新会话"

    // 回执包不带 title，成功要落库的那个名字先记下来
    m_createSessionTitles.insert(sessionId, title);

    QTimer* timer = new QTimer(this);
    timer->setSingleShot(true);
    m_createSessionTimers.insert(sessionId, timer);
    connect(timer, &QTimer::timeout, this, [this, sessionId]() {
        onCreateSessionTimeout(sessionId);
    });
    timer->start(kSessionOpTimeoutMs);

    QByteArray packet = QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n";
    qDebug() << "发送智能体会话新建包：" << packet;
    m_tcpClient->sendData(packet);
}

void AgentBackend::onCreateSessionTimeout(const QString& sessionId)
{
    if (QTimer* timer = m_createSessionTimers.take(sessionId)) {
        timer->deleteLater();
    }
    m_createSessionTitles.remove(sessionId);
    emit agentSessionCreateFailed(sessionId, QStringLiteral("创建会话超时"));
}

void AgentBackend::handleCreateSessionResponse(const QJsonObject& response)
{
    const QString sessionId = response["sessionId"].toString();
    QTimer* timer = sessionId.isEmpty() ? nullptr : m_createSessionTimers.take(sessionId);
    if (timer == nullptr) {
        return;   // 没有在飞的创建（重复包 / 迟到包）：丢掉
    }
    timer->stop();
    timer->deleteLater();

    const QString title = m_createSessionTitles.take(sessionId);
    if (response["success"].toBool()) {
        emit agentSessionCreated(sessionId, title);
        return;
    }

    QString message = response["message"].toString();
    if (message.isEmpty()) {
        message = QStringLiteral("服务器拒绝了创建会话");
    }
    emit agentSessionCreateFailed(sessionId, message);
}

// ===== 会话改名 =====
void AgentBackend::sendAgentSessionRename(const QString& sessionId, const QString& title)
{
    if (sessionId.isEmpty() || m_renameSessionTimers.contains(sessionId)) {
        return;   // 没 ID；或者这次改名还在等回执
    }

    if (m_tcpClient == nullptr || !m_tcpClient->isConnected()) {
        emit agentSessionRenameFailed(sessionId, QStringLiteral("网络未连接"));
        return;
    }

    QJsonObject request;
    request["type"] = "agent_session_rename";
    request["sessionId"] = sessionId;
    request["userId"] = m_userid;
    request["title"] = title;           // 用户输入的新名字

    // 回执包不带 title，成功后写本地库要用的新名字先记下来
    m_renameSessionTitles.insert(sessionId, title);

    QTimer* timer = new QTimer(this);
    timer->setSingleShot(true);
    m_renameSessionTimers.insert(sessionId, timer);
    connect(timer, &QTimer::timeout, this, [this, sessionId]() {
        onRenameSessionTimeout(sessionId);
    });
    timer->start(kSessionOpTimeoutMs);

    QByteArray packet = QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n";
    qDebug() << "发送智能体会话改名包：" << packet;
    m_tcpClient->sendData(packet);
}

void AgentBackend::onRenameSessionTimeout(const QString& sessionId)
{
    if (QTimer* timer = m_renameSessionTimers.take(sessionId)) {
        timer->deleteLater();
    }
    m_renameSessionTitles.remove(sessionId);
    emit agentSessionRenameFailed(sessionId, QStringLiteral("重命名会话超时"));
}

void AgentBackend::handleRenameSessionResponse(const QJsonObject& response)
{
    const QString sessionId = response["sessionId"].toString();
    QTimer* timer = sessionId.isEmpty() ? nullptr : m_renameSessionTimers.take(sessionId);
    if (timer == nullptr) {
        return;
    }
    timer->stop();
    timer->deleteLater();

    const QString title = m_renameSessionTitles.take(sessionId);
    if (response["success"].toBool()) {
        emit agentSessionRenamed(sessionId, title);
        return;
    }

    QString message = response["message"].toString();
    if (message.isEmpty()) {
        message = QStringLiteral("服务器拒绝了重命名");
    }
    emit agentSessionRenameFailed(sessionId, message);
}

// ===== 会话删除 =====
void AgentBackend::sendAgentSessionDelete(const QString& sessionId)
{
    if (sessionId.isEmpty() || m_deleteSessionTimers.contains(sessionId)) {
        return;   // 没 ID；或者这次删除还在等回执
    }

    if (m_tcpClient == nullptr || !m_tcpClient->isConnected()) {
        emit agentSessionDeleteFailed(sessionId, QStringLiteral("网络未连接"));
        return;
    }

    QJsonObject request;
    request["type"] = "agent_session_delete";
    request["sessionId"] = sessionId;   // 删除只认会话 ID，不需要名字
    request["userId"] = m_userid;

    QTimer* timer = new QTimer(this);
    timer->setSingleShot(true);
    m_deleteSessionTimers.insert(sessionId, timer);
    connect(timer, &QTimer::timeout, this, [this, sessionId]() {
        onDeleteSessionTimeout(sessionId);
    });
    timer->start(kSessionOpTimeoutMs);

    QByteArray packet = QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n";
    qDebug() << "发送智能体会话删除包：" << packet;
    m_tcpClient->sendData(packet);
}

void AgentBackend::onDeleteSessionTimeout(const QString& sessionId)
{
    if (QTimer* timer = m_deleteSessionTimers.take(sessionId)) {
        timer->deleteLater();
    }
    emit agentSessionDeleteFailed(sessionId, QStringLiteral("删除会话超时"));
}

void AgentBackend::handleDeleteSessionResponse(const QJsonObject& response)
{
    const QString sessionId = response["sessionId"].toString();
    QTimer* timer = sessionId.isEmpty() ? nullptr : m_deleteSessionTimers.take(sessionId);
    if (timer == nullptr) {
        return;
    }
    timer->stop();
    timer->deleteLater();

    if (response["success"].toBool()) {
        emit agentSessionDeleted(sessionId);
        return;
    }

    QString message = response["message"].toString();
    if (message.isEmpty()) {
        message = QStringLiteral("服务器拒绝了删除会话");
    }
    emit agentSessionDeleteFailed(sessionId, message);
}

// ===== 登录后拉取会话列表 =====
void AgentBackend::pullAgentSessions()
{
    if (m_tcpClient == nullptr || !m_tcpClient->isConnected()) {
        // 登录那一刻链路没通就不拉了：不提示、不重试，等下次登录
        return;
    }

    QJsonObject request;
    request["type"] = "agent_sessions_pull";   // 普通请求包，不带 op
    request["userId"] = m_userid;               // 只带账号，服务器按它查自己的会话表

    QByteArray packet = QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n";
    qDebug() << "发送智能体会话列表拉取包：" << packet;
    m_tcpClient->sendData(packet);
}

void AgentBackend::handleAgentSessionsList(const QJsonObject& response)
{
    // 服务器明确说失败：绝不能当成"服务器上一条会话都没有"，
    // 否则下游核对时会把本地会话全部按"已删除"清掉
    if (response.contains("success") && !response["success"].toBool()) {
        qDebug() << "拉取智能体会话列表失败：" << response["message"].toString();
        return;
    }

    QList<ConversationInfo> sessions;
    const QJsonArray array = response["sessions"].toArray();
    for (const QJsonValue& value : array) {
        const QJsonObject item = value.toObject();

        ConversationInfo info;
        info.id = item["sessionId"].toString();
        info.name = item["title"].toString();
        if (info.id.isEmpty()) {
            continue;   // 没有会话 ID 的脏条目丢掉（核对全靠这个 ID）
        }
        info.isOnline = false;
        info.unreadCount = 0;
        sessions.append(info);
    }

    emit agentSessionsPulled(sessions);
}

// ===== 提问第一步：申请消息 ID =====
void AgentBackend::requestAgentMessageId(const QString& sessionId)
{
    if (sessionId.isEmpty()) {
        return;
    }

    if (m_tcpClient == nullptr || !m_tcpClient->isConnected()) {
        emit agentMessageIdAllocateFailed(sessionId, QStringLiteral("网络未连接"));
        return;
    }

    QJsonObject request;
    request["type"] = "agent_message_create";
    request["sessionId"] = sessionId;
    request["userId"] = m_userid;

    QByteArray packet = QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n";
    qDebug() << "发送智能体消息ID申请包：" << packet;
    m_tcpClient->sendData(packet);
}

void AgentBackend::handleMessageCreated(const QJsonObject& response)
{
    const QString sessionId = response["sessionId"].toString();

    // 服务器明确拒绝：这条问题不发出去，让主后端把发送状态收回去
    if (response.contains("success") && !response["success"].toBool()) {
        QString message = response["message"].toString();
        if (message.isEmpty()) {
            message = QStringLiteral("服务器没有分配消息 ID");
        }
        emit agentMessageIdAllocateFailed(sessionId, message);
        return;
    }

    const QString messageId = response["messageId"].toString();
    if (sessionId.isEmpty() || messageId.isEmpty()) {
        emit agentMessageIdAllocateFailed(sessionId, QStringLiteral("服务器返回的消息 ID 无效"));
        return;
    }

    emit agentMessageIdAllocated(sessionId, messageId);
}

// ===== 流式输出看门狗 =====
void AgentBackend::onStreamWatchdogTimeout()
{
    const QString sessionId = m_pendingSessionId;
    if (sessionId.isEmpty()) {
        return;   // 本轮已经结束（finished / 失败 / 被停），没东西可中止
    }

    // 本地先按"本轮结束"处理：清在飞标记 + 解锁会话，之后漏过来的片段一律丢弃
    m_pendingSessionId.clear();
    m_inFlightSessions.erase(sessionId);

    // 顺手给服务器发停止包，别让它继续在后台算
    if (m_tcpClient != nullptr && m_tcpClient->isConnected()) {
        QJsonObject request;
        request["type"] = "agent_stop";
        request["threadId"] = sessionId;
        request["userId"] = m_userid;

        QByteArray packet = QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n";
        m_tcpClient->sendData(packet);
    }

    emit agentRequestFailed(sessionId, QStringLiteral("智能体长时间没有输出，已中止本轮"));
}

void AgentBackend::onTcpDataReceived(const QByteArray& packet)
{
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(packet, &error);
    if (error.error != QJsonParseError::NoError) {
        qDebug() << "智能体回包 JSON 解析失败:" << error.errorString();
        return;
    }

    QJsonObject response = doc.object();
    const QString responseType = response["type"].toString();

    // 会话三件套的回执：包类型相同，只按 op 字段分派到三件各自的处理函数
    if (responseType == "agent_session_response") {
        const QString op = response["op"].toString();
        if (op == "create") {
            handleCreateSessionResponse(response);
        } else if (op == "rename") {
            handleRenameSessionResponse(response);
        } else if (op == "delete") {
            handleDeleteSessionResponse(response);
        }
        // 不认识的 op：脏包，直接丢掉
        return;
    }

    // 登录后拉取的会话列表：只解析，本地怎么核对（补插 / 删多余的）交给 DB 层
    if (responseType == "agent_sessions_list") {
        handleAgentSessionsList(response);
        return;
    }

    // 申请消息 ID 的回执：拿到 ID 后主后端先落库，落库成功才发对话请求
    if (responseType == "agent_message_created") {
        handleMessageCreated(response);
        return;
    }

    // 对话请求的确认包：服务器已受理，从这一刻起开始防"长时间无输出"
    if (responseType == "agent_request_ack") {
        m_streamWatchdog->start(kStreamTimeoutMs);
        return;
    }

    if (responseType != "agent_response") {
        return;
    }

    // 没有任何请求在飞（还没发过 / 已结束 / 刚被停止）：这之后到的片段一律丢弃。
    // 停止后服务器可能还会漏几段过来，会话 ID 对不回任何地方，拼进去就是脏数据
    if (m_pendingSessionId.isEmpty()) {
        return;
    }

    // 回包不带会话字段，用发出请求时记下的那个会话
    const QString sessionId = m_pendingSessionId;

    // 注意：回包里可能带 serverId，但本地消息 ID 是客户端生成的（本地为主），
    // 不需要再拿服务器 ID 覆盖，直接忽略这个字段

    if (!response["success"].toBool()) {
        QString message = response["message"].toString();
        if (message.isEmpty()) {
            message = "智能体请求失败";
        }
        m_pendingSessionId.clear();   // 失败即本轮结束，不再接收后续片段
        m_inFlightSessions.erase(sessionId);   // 本轮结束，会话解锁
        m_streamWatchdog->stop();     // 本轮失败，看门狗没用了
        emit agentRequestFailed(sessionId, message);
        return;
    }

    // 流式回答：每段文本放在 answer（兼容 content 字段）里，done/finished 标记本轮结束
    QString content = response["answer"].toString();
    if (content.isEmpty()) {
        content = response["content"].toString();
    }
    const bool finished = response["done"].toBool() || response["finished"].toBool();

    // 只要还有输出就重置看门狗：它防的是"长时间静默"，不是"总时长过长"
    m_streamWatchdog->start(kStreamTimeoutMs);

    if (finished) {
        m_pendingSessionId.clear();
        m_inFlightSessions.erase(sessionId);   // 本轮正常结束，会话解锁
        m_streamWatchdog->stop();              // 本轮已结束，看门狗收工
    }
    if (!content.isEmpty() || finished) {
        emit agentMessageReceived(sessionId, content, finished);
    }
}
