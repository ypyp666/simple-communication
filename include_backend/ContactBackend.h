#ifndef CONTACTBACKEND_H
#define CONTACTBACKEND_H

#include <QObject>
#include <QList>
#include <QStringList>
#include "FeatureStructs.h"   // 引入 ContactInfo（通讯录好友资料）
#include "TcpClient.h"        // 好友请求要走 TCP，但客户端实例由主后端注入，本后端不自己 new

// 联系人后端：只管"通讯录"里的好友资料。
// 与 ChatBackend 的会话列表彻底分开，两边各喂一个列表：
//   ChatBackend    → 会话列表（ConversationInfo：最后一条消息 / 未读数 / 在线状态）→ MessageList
//   ContactBackend → 联系人列表（ContactInfo：id / 名字 / 头像 / 备注）           → ContactList
class ContactBackend : public QObject
{
    Q_OBJECT
public:
    explicit ContactBackend(QObject* parent = nullptr, TcpClient* tcpClient = nullptr);
    // 登录成功后由主后端同步当前登录账号：好友请求要带"申请人=我自己"的 ID，
    // 本后端在 MainBackend 构造时就创建了，那时账号还没输进来，所以只能事后同步
    void setUserId(const QString& userId);
    // 登出：把本后端存的当前账号清成空白（预留的口，真正登出流程接入时由主后端调用）
    void clearUserId();

signals:
    // 通讯录拉取完成（携带 ContactInfo）。注意与 ChatBackend::conversationsLoaded 携带的
    // ConversationInfo 是两套数据；会话侧信号已按 conversations 命名，两边名字不再混淆
    void contactsLoaded(const QList<ContactInfo>& contacts);

    // === 服务器回包的结果信号 ===
    // 与 LoginBackend / ChatBackend 同一套规矩：一个请求配"成功 / 失败"两个信号，
    // 由 onTcpDataReceived 里判 success 字段后二选一发，UI 各接各的、不用自己再判一次
    //（LoginBackend 的 loginSuccess/loginFailed、ChatBackend 的 sendSuccess/sendFailed 都是这个形状）。
    // 目前 UI 侧还没接这些信号（"新朋友"页 / 好友列表还在做），先把接口立好，用得上时直接连
    // 好友申请一律用 FriendRequestInfo 承载（不是 ContactInfo）：
    // 申请自带服务器记录ID requestId，同意时要把这个 ID 原样发回去定位是哪条申请
    void sendFriendRequestSuccess();                                      // 我发出的好友申请，服务器已受理（纯状态，不带列表）
    void sendFriendRequestFailed(const QString& message);                 // 我发出的好友申请，被服务器拒绝
    // 收到"一条"好友申请记录。参考拉取离线消息的设计：服务器不下一整个数组，
    // 而是把列表里的每个元素当成一个独立的包逐条发（别人新发来的推送、我拉取时服务器重推，
    // 都用这个信号）。本地不存申请，收到就上 UI；要看完整列表再发一次 pullFriendRequest 向服务器拉
    void friendRequestReceived(const FriendRequestInfo& request);
    // 整批拉取完成的状态确认（对应离线消息的 pull_response）。内容不在这里——
    // 内容已经逐条走 friendRequestReceived 送过了，这个包只有 success / count
    void pullFriendRequestSuccess();
    void pullFriendRequestFailed(const QString& message);                 // 拉好友申请列表失败
    // 同意某个申请成功：服务器那边好友关系已建好，并把"新好友"的资料回下来
    // （与 pullServerContactsSuccess 同一套 ContactInfo），主后端拿这条直接写本地 contacts 表。
    // 回包里的 requestId 本地用不上——申请不在本地存（要看申请列表就向服务器拉一次），
    // 所以这里只带好友资料，不带那个 ID
    void acceptFriendRequestSuccess(const ContactInfo& newFriend);
    void acceptFriendRequestFailed(const QString& message);               // 同意某个申请失败
    // 拒绝某个申请的结果。形状与 sendFriendRequestSuccess/Failed 一致：只有 success，不带数据。
    // 本地不存申请，"拒绝的是哪一条"由主后端记着（见 MainBackend::m_pendingRejectRequestId）——
    // 服务器回包里那个 requestId 本地同样不需要读
    void rejectFriendRequestSuccess();
    void rejectFriendRequestFailed(const QString& message);
    // 删好友的结果。只有成功这条才意味着"服务器已确认"，本地这时才真删（见 MainBackend 的接法）
    void deleteFriendSuccess(const QString& targetId);
    void deleteFriendFailed(const QString& targetId, const QString& message);
    // 改好友备注的结果。与 deleteFriendSuccess/Failed 同一形状：只有成功这条才意味着
    // "服务器已确认"，本地这时才真改备注（见 MainBackend::onSetRemarkSuccess）。
    // 回包只带回 targetId——改后的备注内容由主后端在发出请求时记着（服务器不保证回显 remark）
    void setRemarkSuccess(const QString& targetId);
    void setRemarkFailed(const QString& targetId, const QString& message);
    // 服务器上的好友列表（拉取结果）。与本地库读出口 contactsLoaded 分开：
    // 一个是"服务器权威数据"，一个是"本地库回显"，混在一起会分不清列表该信谁
    void pullServerContactsSuccess(const QList<ContactInfo>& contacts);
    void pullServerContactsFailed(const QString& message);
    // 离线"删除好友缓存"（墓碑）拉取结果：我离线期间被别人删了好友，服务器把"我该删掉的联系人"
    // 暂存着，上线时下发。服务端按 DB 的 target_id=我 筛，回包元素字段名叫 targetId，
    // 但值放的是发起删除的那个人（即"接收方本地要移除的联系人"）。这里传的就是要删的对方ID。
    // 空列表表示没有待删的（拉取失败也发空列表）。主后端收到后：先删本地 → 回 ACK → 再重拉服务器好友列表
    void deleteFriendCacheReceived(const QStringList& targetIds);
    // 在线被删推送：别人在线删我时服务器实时推这一条（字段同样叫 targetId，值是发起删除的人；
    // 不落墓碑所以没有 ACK）。与上面的墓碑同一语义（都要删掉对方），只是实时/离线两条路径不同
    void friendDeletedByPeer(const QString& targetId);
    void searchSuccess(const QList<ContactInfo>& results);               // 搜索用户成功
    void searchFailed(const QString& message);                           // 搜索用户失败

public slots:
    // 载入通讯录：由主后端把 DatabaseManager 查到的结果转发进来（DB 查完 → 主后端 → 这里）。
    // 存一份到 m_contacts，再向上发 contactsLoaded，UI 侧接主后端的同名信号即可
    void onDbContactsLoaded(const QList<ContactInfo>& contacts);
    // 发好友申请：userId=要加的人，message=留言（验证消息，可为空）。
    // 留言随请求一起上服务器，收包侧按同名 remark 字段读回来（见 toFriendRequestInfo）
    void sendFriendRequest(const QString& userId, const QString& message);
    void pullFriendRequest(const QString& userId);
    // 同意好友申请：参数是"那条申请记录的服务器ID"（来自申请列表项的 requestId），
    // 不是对方账号ID——同意的是某条申请，服务器要靠这个 ID 定位
    void acceptFriendRequest(const QString& requestId);
    // 拒绝好友申请：参数与 acceptFriendRequest 相同，都是那条申请记录的服务器ID
    void rejectFriendRequest(const QString& requestId);
    void deleteFriend(const QString& userId);
    // 改好友备注：先发请求给服务器，服务器改成功回包后本地才真改（见 MainBackend::onSetRemarkSuccess）。
    // 参数：targetId=被改备注的好友ID，remark=改后的备注内容（可空串，表示清空备注）
    void sendSetRemarkRequest(const QString& targetId, const QString& remark);
    void pullServerContacts();
    // 拉取服务器上的"离线删除好友缓存"（墓碑）：上线时与拉好友申请并排的一次拉取。
    // 服务器回 delete_friend_cache_response，命中则走 deleteFriendCacheReceived 上抛给主后端
    void pullDeleteFriendCache();
    // 本地按墓碑删完好友后回执给服务器：服务器据此清掉已处理的墓碑（按 accountId=我 + deleterIds 定位），
    // 否则每次上线都会把同一条墓碑再推一遍（删是幂等的，但属于无效功）。
    // 参数是"发起删除的人"ID 列表（即本地刚删掉的那些对方）
    void sendDeleteFriendCacheAck(const QStringList& deleterIds);
    void sendSearchRequest(const QString& userId);
    // 【收包入口】由主后端 JsonParsing 按 type 路由进来，内部再按响应 type 分发到各分支
    void onTcpDataReceived(const QByteArray& packet);



private:
    // 按姓名拼音给 m_contacts 排序。排序是本类"独有的规则"，只在这一个地方实现，
    // 不散到 UI 里去排——以后增删好友后重排也走它，内存里的顺序永远是真源
    void sortContacts();

    QList<ContactInfo> m_contacts;  // 最近一次从本地库载入的通讯录（已排序）
    TcpClient* m_tcpClient;         // 共享 TCP 客户端（主后端注入，与 ChatBackend 用的是同一个）
    QString m_userid;               // 当前登录账号 ID（登录成功后由主后端 setUserId 写入）
};

#endif // CONTACTBACKEND_H
