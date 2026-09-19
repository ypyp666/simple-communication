#include "ChatBackend.h"
#include "TcpClient.h"
#include <QJsonObject>
#include <QJsonDocument>
#include <qstringview.h>

ChatBackend::ChatBackend(QObject *parent,TcpClient* tcpclient) : QObject(parent),m_tcpClient(tcpclient)
{
    m_pullTimer = new QTimer(this);
    //m_pullTimer->setInterval(30000);设置30秒触发一次，每次触发后30秒再触发一次
    m_pullTimer->setSingleShot(true);
    // 注意：TcpClient 的信号槽统一由主后端（MainBackend）路由分发，
    // 这里不再直连信号，避免多个后端重复监听共享信号
    // 断线时发送失败由 UI 红色感叹号提示，用户手动点击重发，不做自动重连补发
    connect(m_pullTimer, &QTimer::timeout, this, &ChatBackend::onPullTimerTimeout);
}

// 登录成功后由主后端同步当前登录账号（拉取重试 onPullTimerTimeout 需要用到 m_userid）
void ChatBackend::setUserId(const QString& userId)
{
    m_userid = userId;
}

void ChatBackend::loadContacts()
{
    QList<ContactInfo> contacts;

    ContactInfo c1;
    c1.id = "12345";
    c1.name = "张三";
    c1.avatar = "";
    c1.lastMessage = "明天一起吃饭？";
    c1.lastTime = QDateTime::currentDateTime().addSecs(-15 * 60);
    c1.isOnline = true;
    c1.unreadCount = 2;
    contacts.append(c1);

    ContactInfo c2;
    c2.id = "10010";
    c2.name = "管理员大人";
    c2.avatar = "";
    c2.lastMessage = "你好，我是管理员";
    c2.lastTime = QDateTime::currentDateTime().addSecs(-10 * 60);
    c2.isOnline = false;
    c2.unreadCount = 0;
    contacts.append(c2);
    emit contactsLoaded(contacts);
}

void ChatBackend::startSendMessage(const QString& contactId, const OutgoingMessage& message)
{
    if (!m_tcpClient->isConnected()) {
        // 未连接：只自动重连，不自动重发这条消息（重发交给用户手动点击）
        qDebug() << "TCP连接未建立，触发自动重连（不重发当前消息）";
        m_tcpClient->reconnect();
        emit sendFailed(message.tempId, "");  // 未连接，拿不到服务器ID，UI显示红色感叹号待用户重发
        return;
    }
    sendMessage(contactId, message);
}

void ChatBackend::sendMessage(const QString& contactId, const OutgoingMessage& message)
{
    QJsonObject messageJson;
    messageJson["type"] = "repost";
    messageJson["accountId"] = message.accountId;
    messageJson["sendId"] = message.senderId;
    messageJson["targetId"] = contactId;
    messageJson["content"] = message.content;
    messageJson["sendTime"] = message.sendTime.toString(Qt::ISODate);  // QDateTime → ISO字符串
    messageJson["tempId"] = message.tempId;  // 临时ID
    QJsonDocument doc(messageJson);
    QByteArray messageJsonStr = doc.toJson(QJsonDocument::Compact);
    qDebug() << "发送消息：" << messageJsonStr;
    QByteArray packet = messageJsonStr + "\n";
    m_tcpClient->sendData(packet);

    // 发送超时兜底：30秒内没等到服务器的 repost_response 就判失败，
    // 防止服务器不回包时气泡永久转圈（回包正常到达时由 onTcpDataReceived 清理定时器）
    QTimer* sendTimer = new QTimer(this);
    sendTimer->setSingleShot(true);
    m_pendingSends.insert(message.tempId, sendTimer);
    connect(sendTimer, &QTimer::timeout, this, [this, tempId = message.tempId]() {
        qDebug() << "发送消息超时，未收到服务器回执 tempId=" << tempId;
        emit sendFailed(tempId, "");  // 超时拿不到服务器ID，UI显示红色感叹号待用户重发
        removeSendTimer(tempId);
    });
    sendTimer->start(30000);
}

void ChatBackend::sendFile(const QString& contactId, const QString& filePath)
{
    Q_UNUSED(contactId);
    Q_UNUSED(filePath);
}

void ChatBackend::markMessagesAsRead(const QString& contactId)
{
    Q_UNUSED(contactId);
}

void ChatBackend::connectToServer()
{
}

void ChatBackend::disconnectFromServer()
{
}


// 保存输入框内容
void ChatBackend::saveInputContent(const QString& contactId, const QString& content)
{
    if (!content.isEmpty()) {
        inputCache[contactId] = content;
    } else {
        inputCache.remove(contactId);
    }
}

// 获取输入框内容
QString ChatBackend::getInputContent(const QString& contactId)
{
    return inputCache.value(contactId, "");
}

// 清除输入框内容（发送消息后调用）
void ChatBackend::clearInputContent(const QString& contactId)
{
    inputCache.remove(contactId);
}

void ChatBackend::onTcpDisconnected()
{
    qDebug() << "TCP连接已断开";
}

void ChatBackend::onTcpError(QAbstractSocket::SocketError error)
{
    qDebug() << "TCP连接错误：" << m_tcpClient->errorString();
}

void ChatBackend::onTcpConnectionTimeout()
{
    qDebug() << "TCP连接超时";
}
// 【发消息回执】由 MainBackend::JsonParsing 分发调用（type=repost_response 时走到这）。
// 职责：解析服务器对"我发出的消息"的处理结果（success/tempId/serverId），
//       发 sendSuccess/sendFailed 通知 UI 停止转圈动画、把临时ID替换为服务器ID。
// 注意：这不是收消息！收消息走 onTcpRepost（type=repost）。
void ChatBackend::onTcpDataReceived(const QByteArray& packet)//这个是服务器回执是对发送消息的确认，要更改本地的临时ID为服务器ID
{
    qDebug() << "[ChatBackend::onTcpDataReceived] 收到原始包:" << packet;  // 调试用：完整打印服务器回包
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(packet, &error);
    if (error.error != QJsonParseError::NoError) {
        qDebug() << "JSON解析失败:" << error.errorString();
        emit sendFailed("", "");  // 包损坏拿不到tempId和serverId，UI找不到对应气泡会忽略
        return;
    }
    QJsonObject response = doc.object();
    QString type = response["type"].toString();
    if (type == "repost_response") {
       bool success = response["success"].toBool();
       QString tempId = response["tempId"].toString();    // 服务器原样回传的临时ID
       QString serverId = response["serverId"].toString();  // 服务器分配的消息ID
       qDebug() << "收到发消息确认(sendSuccess) success=" << success
                << "tempId=" << tempId << "serverId=" << serverId;
       if (success) {
           emit sendSuccess(tempId, serverId);
       } else {
           emit sendFailed(tempId, serverId);
       }
       removeSendTimer(tempId);  // 服务器已回执（无论成败），取消超时兜底
    }
}

// 收到某条消息的回执（成功/失败）或自身超时触发后，清理对应的超时定时器
void ChatBackend::removeSendTimer(const QString& tempId)
{
    auto it = m_pendingSends.find(tempId);
    if (it != m_pendingSends.end()) {
        it.value()->stop();
        it.value()->deleteLater();
        m_pendingSends.erase(it);
    }
}
// 【收消息】由 MainBackend::JsonParsing 分发调用（type=repost 时走到这）。
// 职责：解析服务器转发来的他人消息（字段：serverId/sendId/targetId/content/sendTime），
//       组装成 MessageInfo（contactId=发送者，isSelf=false，isRead=false），
//       发 newMessageReceived 交给 MainBackend → UI显示 + 存库 + 回 receive_ack。
void ChatBackend::onTcpRepost(const QByteArray& packet)
{
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(packet, &error);
    if (error.error != QJsonParseError::NoError) {
        qDebug() << "JSON解析失败:" << error.errorString();
        return;
    }
    QJsonObject messageJson = doc.object();
    QString type = messageJson["type"].toString();
    if (type == "repost") {
        MessageInfo message;
        // 协议字段已统一：服务器实时转发(Repost)与拉取重推(Pull)都用 serverId，
        // 客户端不再兼容旧字段名 ID（旧兜底分支为死代码，已删除）
        message.id = messageJson["serverId"].toString();
        if (message.id.isEmpty()) {
            // 拿不到ID：无法存库也无法回ACK（空serverId会让服务器stoull抛异常），
            // 直接丢弃不emit——不回ACK服务器缓存里就留着这条消息，下次拉取/重发还能补救
            qWarning() << "repost包缺少消息ID（serverId为空），丢弃该消息且不回ACK，等待服务器重发:" << packet;
            return;
        }
        // 登记进待确认队列：持有有效ID才允许回receive_ack，同时防止同一ID重复回执
        m_pendingAcks.insert(message.id);
        message.accountId = messageJson["accountId"].toString();
        message.senderId = messageJson["sendId"].toString();
        message.targetId = messageJson["targetId"].toString();
        message.content = messageJson["content"].toString();
        message.sendTime = QDateTime::fromString(messageJson["sendTime"].toString(), Qt::ISODate);
        message.contactId = message.senderId;  // 对方发来的消息，对话对方=发送者
        message.isSelf = false;                // 对方发的，不是自己发的
        message.isRead = false;               // 收到时默认未读
        message.isFile = false;
        message.isOffline = false;
        emit newMessageReceived(message);
    }
}

// 回复接收确认：收到消息后回ACK给服务器（成功=true移除缓存，失败=false让服务器重发）
// 只有登记在 m_pendingAcks 里的有效ID才会真正发出：
// - 空ID直接拦截（历史bug：空serverId发到服务器，DeleteCache里stoull("")抛异常）
// - 队列里没有的ID说明已回执过或从未收到过，拦掉重复ACK
void ChatBackend::sendReceiveAck(const QString& serverId, bool success)
{
    if (serverId.isEmpty()) {
        qWarning() << "receive_ack被拦截：消息ID为空，不发送";
        return;
    }
    if (!m_pendingAcks.contains(serverId)) {
        qWarning() << "receive_ack被拦截：消息" << serverId << "不在待确认队列中（已回执过或未收到过），跳过";
        return;
    }
    m_pendingAcks.remove(serverId);  // 出队：一条消息只回一次ACK

    QJsonObject ack;
    ack["type"] = "receive_ack";
    ack["success"] = success;
    ack["serverId"] = serverId;  // 服务器分配的消息ID
    QJsonDocument doc(ack);
    QByteArray packet = doc.toJson(QJsonDocument::Compact) + "\n";
    qDebug() << "回复接收确认：" << packet;
    m_tcpClient->sendData(packet);
}

// 发送拉取请求：接收失败时向服务器请求重发未收到的缓存消息
void ChatBackend::sendPullRequest(const QString& targetId)
{
    m_pullTimer->start(30000);
    QJsonObject pullReq;
    pullReq["type"] = "pull_msg";
    pullReq["targetId"] = targetId;  // 拉取发给自己的消息
    QJsonDocument doc(pullReq);
    QByteArray packet = doc.toJson(QJsonDocument::Compact) + "\n";
    qDebug() << "发送拉取请求：" << packet;
    m_tcpClient->sendData(packet);
}

void ChatBackend::onPullTimerTimeout()
{ 
    pullCount++;//拉取请求次数增加
    if (pullCount <=3) {//最多3次
        qDebug() << "拉取请求次数增加：" << pullCount;

        sendPullRequest(m_userid);//重新发送拉取请求
    }
    else {
        m_pullTimer->stop();//停止定时器
        m_tcpClient->disconnectFromServer();//断开TCP连接
    }
}

// 拉取请求确认：服务器把该账号所有缓存消息重发完后发来的确认包（仅状态字段：code/error/success/count）
// 【职责】收到确认说明本次拉取已完成：停止定时器并重置次数，避免继续重拉。
// 注意：实际消息内容不在这个包里！每条消息由服务器以独立的 type=repost 包推送，
//       走 onTcpRepost 解析；在这里解析消息字段（ID/Id等）只会读到空值，产生空气泡
void ChatBackend::onPullResponse(const QByteArray& packet)
{
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(packet, &error);
    if (error.error != QJsonParseError::NoError) {
        qDebug() << "JSON解析失败:" << error.errorString();
        return;
    }
    QJsonObject response = doc.object();
    QString type = response["type"].toString();
    if (type == "pull_response") {
        bool success = response["success"].toBool();
        if (success) {
            qDebug() << "拉取请求成功，缓存消息已全部重发完毕，条数：" << response["count"].toInt();
            m_pullTimer->stop();  // 拉取完成，停止30秒重试定时器
            pullCount = 0;        // 重置重试次数
        } else {
            // 服务器 pull_response 失败时错误文案放在 message 字段（与 login/repost_response 一致，协议统一用 message）
            qDebug() << "拉取请求失败：" << response["message"].toString();
        }
    }
}
