#ifndef FEATURESTRUCTS_H
#define FEATURESTRUCTS_H

// =============================================================================
// 功能结构体统一头文件
// 存放前后端共享的数据结构（联系人、消息、待发送报文）
// 任何类需要这些结构体时，直接 #include "FeatureStructs.h" 即可
// =============================================================================

#include <QString>
#include <QDateTime>
#include <QMetaType>

// 联系人信息结构体
struct ContactInfo {
    QString id;
    QString name;
    QString avatar;
    QString lastMessage;
    QDateTime lastTime;
    bool isOnline;
    int unreadCount;
};

// 本地消息结构体（用于本地存储、界面渲染）
struct MessageInfo {
    QString id;            // 消息唯一ID
    QString accountId;     // 当前登录账号ID（用于校验，防止入库错误）
    QString contactId;     // 对话对方ID（联合主键第一列，用于快速查询）
    QString senderId;      // 发送人ID
    QString targetId;      // 接收人ID
    QString content;       // 消息文本内容
    QString fileName;       // 文件名（如果有）
    QString filePath;       // 文件路径（如果有）
    qint64 fileSize;       // 文件大小
    QDateTime sendTime;    // 发送时间
    QString remark;        // 备注（可为空）
    bool isSelf;           // 是否为自己发送
    bool isRead;           // 对方发来的消息是否已读（自己发的无意义）
    bool isFile;           // 是否为文件消息
    bool isOffline;        // 是否为离线消息
};

// 发送给服务器的消息结构体（与本地存储结构不同）
// 消息ID由服务器生成（随服务器自增），故此处不含id字段
struct OutgoingMessage {
    QString type;          // 告诉后端当前json文件的处理类型
    QString accountId;     // 当前登录账号ID（用于校验，防止入库错误）
    QString senderId;      // 当前发送账号ID
    QString targetId;      // 接收账号ID
    QString content;       // 文本内容
    QDateTime sendTime;    // 发送时间
    QString tempId;        //临时ID
};

// =============================================================================
// 功能标识枚举（登录 / 注册 / 修改密码 / 消息发送）
// 登录、注册和"忘记密码（修改密码）"都发生在未登录阶段，都要走同一条共享 TcpClient，
// 但收到 connected / error / timeout 等 TCP 信号时需要路由到不同的功能函数。
// 单靠 s_loggedIn 只能区分"登录前/登录后"，区分不了"登录""注册""修改密码"这几种，
// 所以用这个枚举在 MainBackend 里标记"当前这条连接到底为了哪个功能"。
// 同一个枚举还被复用到"重试"场景（见 MessageStatusIndicator）：气泡重试带 MessageSend、
// 注册页账号行重试带 Register，MainBackend 据此把重试请求路由到对应的后端。
// =============================================================================
enum class LoginFeature {
    None = 0,         // 未发起任何请求（初始态）
    Login,            // 登录
    Register,         // 注册（注册页，无需登录但需 TCP 连接）
    ConnectForRegister,  // 注册"取号/连接"阶段：注册页一显示就拉起 TCP 向服务器索取账号 ID，
                         // 此时还没填密码、提交请求发不出去，独立出来好把这些连接期信号
                         // （connectForRegisterSuccess/Failed/Timeout/Waiting）与提交阶段区分开
    ModifyPassword,   // 修改密码（忘记密码页，无需登录但需 TCP 连接）
    MessageSend,      // 聊天消息发送（已登录态，重试时路由到 ChatBackend）
};

// =============================================================================
// Q_DECLARE_METATYPE：把自定义结构体注册进 Qt 元类型系统
// 作用：跨线程信号槽（QueuedConnection）传递这些类型时，Qt 才能正确序列化
// 不加这个，后台线程发信号带 MessageInfo 会直接编译失败或运行时报错
// =============================================================================
Q_DECLARE_METATYPE(ContactInfo)
Q_DECLARE_METATYPE(MessageInfo)
Q_DECLARE_METATYPE(OutgoingMessage)

#endif // FEATURESTRUCTS_H
