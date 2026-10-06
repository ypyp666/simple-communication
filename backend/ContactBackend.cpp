#include "ContactBackend.h"
#include <QCollator>
#include <QLocale>
#include <algorithm>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDateTime>



namespace {
// 造一个"按拼音排中文名"的比较器。
// 关键点：不需要自己维护汉字→拼音映射表。Windows 下 zh-CN 的系统排序表本身就是拼音序，
// QCollator 直接借系统排序表来比，一个汉字都不用查。
// 抽成具名函数（而不是就地写 lambda）是为了让下面 static 那一行一眼能看懂在干什么
QCollator makePinyinCollator()
{
    QCollator collator(QLocale(QLocale::Chinese, QLocale::China));
    collator.setCaseSensitivity(Qt::CaseInsensitive);  // 中英混排时 alice / Bob 不按大小写分家
    return collator;
}

// 把服务器下发的"一个"好友资料 JSON 转成 ContactInfo。
// 好友列表、搜索结果、同意申请回包里的元素是同一套字段（accountId+name+avatar+remark），
// 抽成具名函数，省得各处各抄一遍同样的取字段
ContactInfo toContactInfo(const QJsonObject& obj)
{
    ContactInfo contact;
    contact.id     = obj["accountId"].toString();
    contact.name   = obj["name"].toString();
    contact.avatar = obj["avatar"].toString();
    contact.remark = obj["remark"].toString();
    return contact;
}

// 数组版：逐个元素走上面的单条解析（好友列表 / 搜索结果用）
QList<ContactInfo> toContactList(const QJsonArray& array)
{
    QList<ContactInfo> contacts;
    for (const QJsonValue& value : array) {
        contacts.append(toContactInfo(value.toObject()));
    }
    return contacts;
}

// 把墓碑回包里的 deletedFriends 数组解析成"本地待删联系人ID"列表。
// 服务端每个元素只有一个字段 targetId，语义是"接收方本地要移除的那个联系人"
// （即发起删除的人；见服务端 PullDeleteFriendCache——它把存储过程的 send_id 填进 targetId）。
// 所以这里直接取 targetId 就是本地要删的那条；仍排掉空值和自己，防脏包把本人删掉。
// （字段名务必与服务端逐字一致：早先客户端误读成 deleterId，导致列表恒空、静默不删）
QStringList toDeletedContactIdList(const QJsonArray& array, const QString& selfId)
{
    QStringList ids;
    for (const QJsonValue& value : array) {
        const QString contactId = value.toObject()["targetId"].toString();
        if (!contactId.isEmpty() && contactId != selfId) {
            ids.append(contactId);
        }
    }
    return ids;
}

// 把服务器下发的"一条"好友申请 JSON 转成 FriendRequestInfo。
// 关键：服务器不会把整个申请列表当成数组一次性下发（参考拉取离线消息的设计），
// 而是把列表里的每个元素当成一个独立的包逐条发过来，所以这里是"解析单条"，不是解析数组。
// 每条都带服务器生成的 requestId，同意时原样回传就能定位是哪条申请
FriendRequestInfo toFriendRequestInfo(const QJsonObject& obj)
{
    FriendRequestInfo request;
    request.requestId = obj["requestId"].toString();  // 服务器申请记录ID，同意时要用
    request.accountId  = obj["accountId"].toString();  // 申请人
    request.targetId   = obj["targetId"].toString();   // 被申请人
    request.targetName = obj["targetName"].toString(); // 目标方昵称（我发出的申请要显示"对方"用它）
    request.name       = obj["name"].toString();
    request.avatar    = obj["avatar"].toString();
    request.remark    = obj["remark"].toString();     // 申请附言
    request.sendTime  = obj["sendTime"].toString();
    return request;
}
}

ContactBackend::ContactBackend(QObject* parent, TcpClient* tcpClient)
    : QObject(parent), m_tcpClient(tcpClient)
{
}

// 登录成功后由主后端同步当前登录账号。好友请求里的"申请人"就是我自己，
// 报文要用这个 ID；本后端创建在主后端构造期，那时账号还没输入，所以只能事后同步
void ContactBackend::setUserId(const QString& userId)
{
    m_userid = userId;
}

// 登出：把当前账号清成空白。预留的口，清掉后好友请求不会再挂上一个已登出的申请人
void ContactBackend::clearUserId()
{
    m_userid.clear();
}

// 载入通讯录：主后端把本地库查到的结果转进来（DB 查完 → 主后端 → 本后端）。
// 存一份到 m_contacts，排好序，再向上发 contactsLoaded，由主后端同名信号转给 UI
void ContactBackend::onDbContactsLoaded(const QList<ContactInfo>& contacts)
{
    m_contacts = contacts;
    sortContacts();
    emit contactsLoaded(m_contacts);
}

// 拼音排序：逐个比较姓名后原地重排 m_contacts。
// QCollator 构造时要取系统排序表，比较贵，所以做成 static 只建一次、全程复用；
// 排的是成员而不是参数，保证"进内存即有序"——后续增删好友后重排也调这里，
// 不会出现"库里有序、内存里乱序、UI 又收到乱序"的情况
void ContactBackend::sortContacts()
{
    static const QCollator collator = makePinyinCollator();

    std::sort(m_contacts.begin(), m_contacts.end(),
              [](const ContactInfo& a, const ContactInfo& b) {
                  return collator.compare(a.name, b.name) < 0;
              });//返回<0表示a排在b前面
}

// === 以下是一组好友接口（发请求包给服务器）===
// 报文格式统一：type（蛇形小写，服务器按字符串大小写敏感分发）+ 各接口自己的字段；
// 凡是"我这个账号"相关的字段一律叫 accountId（与 login / repost 同名字段，服务器可复用取值）
void ContactBackend::sendFriendRequest(const QString& userId, const QString& message)//发送好友请求，是用户搜索到对应的用户后发送好友请求
{
    QJsonObject requestJson;
    requestJson["type"] = "friend_request";       // 与 login / repost / pull_msg 一样走蛇形小写，服务器按字符串分发
    requestJson["accountId"] = m_userid;          // 申请人 = 当前登录账号（登录成功后由主后端 setUserId 写入）
    requestJson["targetId"] = userId;
    requestJson["remark"] = message;              // 留言（验证消息）：与收包侧 toFriendRequestInfo 读的 remark 同名字段，服务器原样存下发
    requestJson["sendTime"] = QDateTime::currentDateTime().toString(Qt::ISODate);  // 与聊天消息同名字段，服务器可复用解析
    QJsonDocument doc(requestJson);
    QByteArray requestJsonStr = doc.toJson(QJsonDocument::Compact);
    qDebug() << "发送好友请求：" << requestJsonStr;
    QByteArray packet = requestJsonStr + "\n";
    m_tcpClient->sendData(packet);
}

void ContactBackend::pullFriendRequest(const QString& userId)//拉取好友请求包括发送的和接收的，是用户拉取服务器上的所有好友申请
{
    QJsonObject requestJson;
    requestJson["type"] = "pull_friend_request";       // 与 login / repost / pull_msg 一样走蛇形小写，服务器按字符串分发
    requestJson["accountId"] = m_userid;          // 申请人 = 当前登录账号（登录成功后由主后端 setUserId 写入）
    QJsonDocument doc(requestJson);
    QByteArray requestJsonStr = doc.toJson(QJsonDocument::Compact);
    qDebug() << "发送好友请求：" << requestJsonStr;
    QByteArray packet = requestJsonStr + "\n";
    m_tcpClient->sendData(packet);
}

void ContactBackend::acceptFriendRequest(const QString& requestId)//接受好友请求，是用户发送对具体请求的同意响应
{
    QJsonObject requestJson;
    requestJson["type"] = "accept_friend_request";       // 与 login / repost / pull_msg 一样走蛇形小写，服务器按字符串分发
    // 参数就是那条申请记录的服务器ID（来自申请列表项的 requestId）。
    // 流程上此时用户手里已经有申请列表了（发送申请时服务器回的，或主动拉取的），
    // 列表项自带服务器ID，同意时直接原样回传，服务器据此定位是同意哪一条申请
    requestJson["requestId"] = requestId;
    QJsonDocument doc(requestJson);
    QByteArray requestJsonStr = doc.toJson(QJsonDocument::Compact);
    qDebug() << "发送好友请求：" << requestJsonStr;
    QByteArray packet = requestJsonStr + "\n";
    m_tcpClient->sendData(packet);
}

void ContactBackend::rejectFriendRequest(const QString& requestId)//拒绝好友请求，与 acceptFriendRequest 对称
{
    QJsonObject requestJson;
    requestJson["type"] = "reject_friend_request";       // 与 accept_friend_request 对称：都拿 requestId 定位是哪条申请
    requestJson["requestId"] = requestId;
    QJsonDocument doc(requestJson);
    QByteArray requestJsonStr = doc.toJson(QJsonDocument::Compact);
    qDebug() << "发送拒绝好友申请：" << requestJsonStr;
    QByteArray packet = requestJsonStr + "\n";
    m_tcpClient->sendData(packet);
}

void ContactBackend::deleteFriend(const QString& userId)//删除好友，是用户删除服务器上的好友申请，在服务器删除成功后再同步删掉本地好友
{
    QJsonObject requestJson;
    requestJson["type"] = "delete_friend";        // 与 friend_request 对称：accountId=我，targetId=要删掉的那个好友
    requestJson["accountId"] = m_userid;          // 当前登录账号
    requestJson["targetId"] = userId;
    requestJson["sendTime"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    QJsonDocument doc(requestJson);
    QByteArray requestJsonStr = doc.toJson(QJsonDocument::Compact);
    qDebug() << "发送删除好友请求：" << requestJsonStr;
    QByteArray packet = requestJsonStr + "\n";
    m_tcpClient->sendData(packet);
}

void ContactBackend::sendSetRemarkRequest(const QString& targetId, const QString& remark)//改好友备注，先发服务器，服务器改成功回包后本地才真改
{
    QJsonObject requestJson;
    requestJson["type"] = "set_remark";           // 与 delete_friend 同一套：accountId=我，targetId=要备注的那个好友
    requestJson["accountId"] = m_userid;          // 当前登录账号
    requestJson["targetId"] = targetId;
    requestJson["remark"] = remark;               // 改后的备注内容（空串表示清空）
    requestJson["sendTime"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    QJsonDocument doc(requestJson);
    QByteArray requestJsonStr = doc.toJson(QJsonDocument::Compact);
    qDebug() << "发送修改备注请求：" << requestJsonStr;
    QByteArray packet = requestJsonStr + "\n";
    m_tcpClient->sendData(packet);
}

void ContactBackend::pullServerContacts()//拉取服务器上的所有好友
{
    QJsonObject requestJson;
    requestJson["type"] = "pull_server_contacts";       // 与 login / repost / pull_msg 一样走蛇形小写，服务器按字符串分发
    requestJson["accountId"] = m_userid;          // 申请人 = 当前登录账号（登录成功后由主后端 setUserId 写入）
    QJsonDocument doc(requestJson);
    QByteArray requestJsonStr = doc.toJson(QJsonDocument::Compact);
    qDebug() << "拉取服务器上的所有好友：" << requestJsonStr;
    QByteArray packet = requestJsonStr + "\n";
    m_tcpClient->sendData(packet);
}

// 拉取"离线删除好友缓存"（墓碑）：我离线期间被别人删好友，服务器把这些墓碑暂存着，
// 上线时由我拉下来（与拉好友申请并列的第二次拉取）。回包走 delete_friend_cache_response。
// accountId=我：要拉的是"发给我的"墓碑，服务器按它筛
void ContactBackend::pullDeleteFriendCache()
{
    QJsonObject requestJson;
    requestJson["type"] = "pull_delete_friend_cache";
    requestJson["accountId"] = m_userid;
    QJsonDocument doc(requestJson);
    QByteArray requestJsonStr = doc.toJson(QJsonDocument::Compact);
    qDebug() << "拉取离线删除好友缓存：" << requestJsonStr;
    QByteArray packet = requestJsonStr + "\n";
    m_tcpClient->sendData(packet);
}

// 回执：本地已按墓碑把好友删干净，告诉服务器"这些可以清了"。
// 服务器收到后按 accountId(我=被删者) + deleterIds 定位并删掉对应缓存行，
// 下次上线就不再重推；不带 ACK 的话墓碑会一直留着，每次上线都重删一遍（删是幂等的，但属于无效功）。
// 注意字段方向：回的是"发起删除的人"（deleterId），不是"被删的我"——服务器按行里的发起者来清
void ContactBackend::sendDeleteFriendCacheAck(const QStringList& deleterIds)
{
    QJsonObject requestJson;
    requestJson["type"] = "ack_delete_friend_cache";
    requestJson["accountId"] = m_userid;          // 我=被删者，服务器按它圈定"发给我的"那些行
    QJsonArray idsArray;
    for (const QString& id : deleterIds) {
        idsArray.append(id);
    }
    requestJson["deleterIds"] = idsArray;         // 已处理的发起者 ID 列表，服务器按它清墓碑
    QJsonDocument doc(requestJson);
    QByteArray requestJsonStr = doc.toJson(QJsonDocument::Compact);
    qDebug() << "回执离线删除好友缓存：" << requestJsonStr;
    QByteArray packet = requestJsonStr + "\n";
    m_tcpClient->sendData(packet);
}

void ContactBackend::sendSearchRequest(const QString& userId)//发送搜索请求
{
    QJsonObject requestJson;
    requestJson["type"] = "search_request";       // 与 login / repost / pull_msg 一样走蛇形小写，服务器按字符串分发
    requestJson["accountId"] = m_userid;          // 申请人 = 当前登录账号（登录成功后由主后端 setUserId 写入）
    requestJson["searchText"] = userId;
    QJsonDocument doc(requestJson);
    QByteArray requestJsonStr = doc.toJson(QJsonDocument::Compact);
    qDebug() << "发送搜索请求：" << requestJsonStr;
    QByteArray packet = requestJsonStr + "\n";
    m_tcpClient->sendData(packet);
}

// 【收包入口】由 MainBackend::JsonParsing 把"好友相关的包"按 type 路由进来。
// 职责参照 ChatBackend::onTcpDataReceived：解析 JSON → 按响应 type 分发 → 组装数据 → 发结果信号给主后端，
// 主后端再往上给 UI。主后端只做"这个包该给哪个后端"的粗路由，字段级解析都在这里。
//
// 注意：服务器端好友功能还没写，下面各响应 type 是按"请求 type + _response 后缀"假设的
// （与 login → login_response、repost → repost_response 同一套命名）。等服务器定稿后，
// 要回头把各分支里取字段的名字跟服务器逐字核对一遍
void ContactBackend::onTcpDataReceived(const QByteArray& packet)
{
    qDebug() << "[ContactBackend::onTcpDataReceived] 收到原始包:" << packet;  // 调试用：完整打印服务器回包
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(packet, &error);
    if (error.error != QJsonParseError::NoError) {
        qDebug() << "JSON解析失败:" << error.errorString();
        return;  // 包损坏，解析不出任何字段，直接丢弃
    }

    QJsonObject response = doc.object();
    QString type = response["type"].toString();
    bool success = response["success"].toBool();
    QString message = response["message"].toString();  // 错误文案统一用 message（与 login / repost 一致）

    if (type == "friend_request_response") {
        // 我发出去的好友申请的回执：只回"受理 / 拒绝"，跟 repost_response 一样靠 success 判，不带数据。
        // 申请列表不在这里——服务器会把每条申请当成独立的 friend_request 包逐条发（见下面那个分支）
        if (success) {
            emit sendFriendRequestSuccess();
        } else {
            qDebug() << "好友申请被拒:" << message;
            emit sendFriendRequestFailed(message);
        }
    }
    else if (type == "friend_request") {
        // 收到"一条"好友申请记录。设计参考拉取离线消息：服务器不下一整个数组，而是逐个元素发。
        // 这个 type 双向复用（和我发出去的请求同名），走的是服务器→客户端方向，所以不判 success。
        // 覆盖两种情况：别人新发来的申请推送；我 pull 时服务器对缓存申请的重推。
        // 每包都在响应里带服务器申请记录ID（requestId），上一条收一条、本地按 requestId 增量累积
        emit friendRequestReceived(toFriendRequestInfo(response));
    }
    else if (type == "pull_friend_request_response") {
        // 整批拉取的完成确认（对应离线消息的 pull_response）：只有 success / count，
        // 不含申请内容——内容已由前面逐条的 friend_request 包送过了。
        // 必须判 success：以前不看它直接发列表，服务器回失败时会把空列表当成
        // "没有申请"上给 UI，等于把失败默认成了成功
        if (success) {
            qDebug() << "好友申请拉取完成，条数：" << response["count"].toInt();
            emit pullFriendRequestSuccess();
        } else {
            qDebug() << "拉取好友申请失败:" << message;
            emit pullFriendRequestFailed(message);
        }
    }
    else if (type == "accept_friend_request_response") {
        // 同意某个申请的结果。成功后服务器那边好友关系已经建好，并把"新好友"的资料回下来
        // （与 contacts 数组元素同一套字段），主后端拿它直接写本地 contacts 表，不必再跟服务器对账。
        // 回包里还会带 requestId，本地不读——申请不在本地存，那个 ID 没有落点
        if (success) {
            emit acceptFriendRequestSuccess(toContactInfo(response));
        } else {
            qDebug() << "同意好友申请失败:" << message;
            emit acceptFriendRequestFailed(message);
        }
    }
    else if (type == "reject_friend_request_response") {
        // 拒绝某个申请的结果：只有受理 / 不受理，没有数据。
        // 与 accept 那边一样，回包里的 requestId 本地不读——不落库的 ID 没有落点
        if (success) {
            emit rejectFriendRequestSuccess();
        } else {
            qDebug() << "拒绝好友申请失败:" << message;
            emit rejectFriendRequestFailed(message);
        }
    }
    else if (type == "delete_friend_response") {
        // 删好友：只有服务器回"成功"才算数。targetId 原样带回，成功时主后端据此删本地库、
        // 失败时 UI 才知道删的是谁并给出提示
        QString targetId = response["targetId"].toString();
        if (success) {
            emit deleteFriendSuccess(targetId);
        } else {
            qDebug() << "删除好友失败:" << message;
            emit deleteFriendFailed(targetId, message);
        }
    }
    else if (type == "set_remark_response") {
        // 改备注：与 delete_friend_response 同一形状——只有服务器回"成功"才算数。
        // targetId 原样带回，成功时主后端据此把新备注写本地，失败时 UI 才知道改的是哪位好友。
        // 改后的备注内容不从这里取：服务器不保证回显 remark，内容由主后端发出请求时记着
        QString targetId = response["targetId"].toString();
        if (success) {
            emit setRemarkSuccess(targetId);
        } else {
            qDebug() << "修改备注失败:" << message;
            emit setRemarkFailed(targetId, message);
        }
    }
    else if (type == "delete_friend_cache_response") {
        // 离线墓碑：我离线期间被别人删好友，服务器把整条记录暂存着（deleterId + targetId），
        // 上线时按 targetId=我 筛出来下发。注意甄别方向：服务器回的是"谁删了我"，
        // 而本地要删的是"对方"，所以 toDeletedContactIdList 取非自己的那个 ID（详见该函数注释）。
        // 失败时也发空列表：主后端的处理链是"删本地 → 回ACK → 重拉服务器列表"，
        // 拉墓碑失败不该把后面的"重拉好友列表"一起卡死（这次少删的下次上线还能补回来）
        if (success) {
            emit deleteFriendCacheReceived(toDeletedContactIdList(response["deletedFriends"].toArray(), m_userid));
        } else {
            qDebug() << "拉取离线删除好友缓存失败:" << message;
            emit deleteFriendCacheReceived(QStringList());
        }
    }
    else if (type == "friend_deleted") {
        // 在线被删推送：别人在线删我时，服务器实时推这一条（对方那条走 delete_friend_response，
        // 我这条走这里——同一个删除动作，两边各收各的）。推的字段名是 targetId，
        // 值同样是"本地要移除的联系人"（发起删除的那个人），与墓碑回包保持同名字段；
        // 不落墓碑、也就没有 ACK
        emit friendDeletedByPeer(response["targetId"].toString());
    }
    else if (type == "pull_server_contacts_response") {
        // 服务器上的好友列表，走 pullServerContactsSuccess（不是本地库那条 contactsLoaded）。
        // 同样要判 success，失败时不能发空列表冒充"拉到了"
        if (success) {
            emit pullServerContactsSuccess(toContactList(response["contacts"].toArray()));
        } else {
            qDebug() << "拉取服务器好友列表失败:" << message;
            emit pullServerContactsFailed(message);
        }
    }
    else if (type == "search_request_response") {
        // 搜索用户的结果：失败时同样是"搜索失败"，不是"搜到了 0 个人"
        if (success) {
            emit searchSuccess(toContactList(response["users"].toArray()));
        } else {
            qDebug() << "搜索用户失败:" << message;
            emit searchFailed(message);
        }
    }
    else {
        qDebug() << "ContactBackend 收到未识别的包，type=" << type;  // 路由漏了或服务器发了新type
    }
}
