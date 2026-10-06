#include <mysql/mysql.h>
#include <string>
#include <cstdint>
#include <cstring>
#include <vector>
#include"json.hpp"
#ifndef UNTITLED_MYSQL_H
#define UNTITLED_MYSQL_H

// 单条消息的完整信息，供 callLoadMessage 返回结果集用
struct MessageInfo {
    uint32_t messageId;   // 消息ID
    uint32_t senderId;    // 发送者ID
    uint32_t targetId;    // 目标ID
    std::string sendTime;  // 发送时间
    std::string content;  //文本内容
};

// 好友申请记录，供 callLoadNewFriend 返回结果集用（对应 contact_cache 表）
// 结果集列顺序固定为：
// request_id, account_id, target_id, a_user_name, t_user_name, send_time, status, apply_msg
struct FriendApplyInfo {
    uint32_t requestId;     // 申请记录ID（contact_cache.request_id）
    uint32_t applyId;       // 申请人账号（account_id）
    uint32_t targetId;      // 被申请人账号（target_id）
    std::string applyName;  // 申请人昵称（a_user_name，过程已联表user带出）
    std::string targetName; // 被申请人昵称（t_user_name）
    std::string sendTime;   // 申请时间（send_time）
    int status;             // 申请状态（status，0=待处理）
    std::string applyMsg;   // 申请留言（apply_msg），即好友申请里的备注内容
};

// 好友简要资料：AgreeRequest 与 LoadOldFriend 的结果集共用
// AgreeRequest  回 user_id, user_name 两列（remark 留空，新建好友必无备注）
// LoadOldFriend 回 account_id, name, remark, create_time 四列
struct FriendBriefInfo {
    uint32_t accountId;     // 好友账号
    std::string name;       // 好友昵称（过程已联表user带出，无需二次查询）
    std::string remark;     // 自己对该好友的备注；AgreeRequest 不返回该列
    std::string createTime; // 成为好友时间；AgreeRequest 不返回该列
};

// 用户简要信息，供 callSearchUser 返回结果集用（对应 user 表）
struct UserBriefInfo {
    uint32_t userId;        // 用户账号
    std::string userName;   // 用户名
    std::string createTime; // 注册时间
};

// 删除好友墓碑记录，供 callLoadDelFriendEvent 返回结果集用（对应 delete_friend_cache 表）
// 结果集列顺序固定为：send_id, target_id
// send_id   = 删除操作的【发起者】，也就是“谁发起了删除”
// target_id = 删除操作的【接受者】，也就是“谁被删了”
// 拉取时用自己账号去匹配 target_id 这一列（找“谁删了我”），
// 命中的行里的 send_id 才是本人本地需要移除的那个联系人
struct DelFriendEventInfo {
    uint32_t sendId;   // 删除者；对拉取者来说就是本地要移除的联系人
    uint32_t targetId; // 被删除者；等于发起拉取的账号本人
};


class mysqlconn {
private:
    MYSQL* mysql;         // mysql核心句柄,是整个数据库会话的总容器。
    //程序启动初始化 → 全程复用执行所有 SQL → 程序退出 / 不用时关闭销毁。所有数据库操作都要靠这个mysql指针传参：
    MYSQL_RES* res;       // 查询结果集
    MYSQL_ROW row;         // 单行数据（字符串数组指针）,本质是char**类型指针
public:
    mysqlconn();
    ~mysqlconn(); // 对外接口1：建立数据库连接（只在main初始化一次/线程内单独调用）
    bool connect(const std::string& host, int port,
                 const std::string& user, const std::string& pwd,
                 const std::string& dbname);

    // 对外接口2：增/删/改统一入口 insert update delete
    bool execUpdate(const std::string& sql);

    // 对外接口3：查询select，返回结果集给上层遍历
    MYSQL_RES* execQuery(const std::string& sql);

    // 对外接口4：释放查询结果内存
    void freeResult();

    // 对外接口5：关闭数据库连接，static无成员访问
    static void close(mysqlconn& conn);

    //对外接口6
    bool callLoginFunc(int account,  std::string& pwd, int& retCode);

    //对外接口7：修改密码数据库过程
    bool callModifyPwd(uint32_t account, const std::string& pwd, int& retCode);

    //对外接口8：注册数据库过程占位
    bool callRegister(uint32_t account, const std::string& pwd);

    //对外接口9：获取注册账号数据库函数占位
    bool callGetAccount(uint32_t& newAccount);

    //对外接口10数据库消息存储
    bool callMessage(nlohmann::json& rsp, uint32_t& outMessageId);

    //对外接口11删除消息缓存
    bool callDeleteMessage(unsigned int messageId);

    //对外接口12加载消息缓存（retcode为过程返回码，消息数据通过outMessages带回）
    bool callLoadMessage(int targetID, std::vector<MessageInfo>& outMessages, int& retcode);

    // ===== 好友/联系人功能相关数据库过程 =====

    //对外接口13：发起好友申请（过程AddFriend）
    //retcode为过程返回码；成功后通过outRequestId带回新申请ID，
    //outApplyName为申请人昵称，依赖AddFriend结果集补出(user_id, user_name)，未补时为空
    bool callAddFriend(uint32_t applyId, uint32_t targetId, const std::string& applyMsg,
                       int& retCode, uint32_t& outRequestId, std::string& outApplyName);

    //对外接口14：加载待处理的好友申请列表（过程LoadNewFriend，结果集已含申请人昵称）
    bool callLoadNewFriend(uint32_t account, std::vector<FriendApplyInfo>& outApplies, int& retCode);

    //对外接口15：同意好友申请（过程AgreeRequest，成功时通过outFriend带回新好友资料）
    bool callAcceptFriend(uint32_t requestId, uint32_t account, int& retCode,
                          FriendBriefInfo& outFriend);

    //对外接口16：拒绝好友申请（过程RejectRequest）
    bool callRejectFriend(uint32_t requestId, uint32_t account, int& retCode);

    //对外接口17：删除双向好友关系
    bool callDeleteFriend(uint32_t ownId, uint32_t targetId, int& retCode);

    //对外接口18：修改好友备注（newMark为用户输入，进入SQL前必须转义）
    bool callModifyMark(uint32_t account, uint32_t targetId, const std::string& newMark, int& retCode);

    //对外接口19：加载自己的好友列表（过程LoadOldFriend，结果集自带昵称与备注）
    bool callLoadOldFriend(uint32_t account, std::vector<FriendBriefInfo>& outContacts, int& retCode);

    //对外接口20：搜索用户（当前过程按accountId精确匹配，searchText暂不参与SQL）
    bool callSearchUser(uint32_t account, std::vector<UserBriefInfo>& outUsers, int& retCode);

    //对外接口21：修改用户名（newName为用户输入，进入SQL前必须转义）
    bool callModifyName(uint32_t account, const std::string& newName, int& retCode);

    //对外接口22：拉取本账号的离线删除墓碑（过程LoadDelFriendEvent）
    //过程内部用入参账号去匹配 delete_friend_cache.target_id，结果集为 send_id, target_id 两列
    bool callLoadDelFriendEvent(uint32_t userId, std::vector<DelFriendEventInfo>& outEvents, int& retCode);

    //对外接口23：清除一条已同步完成的墓碑（过程Clear_delete_cache）
    //入参顺序与过程一致：sendId=墓碑里的发起者(删除者)，targetId=被删者(本人)，
    //过程内部按 WHERE send_id = p_send_id AND target_id = p_target_id 删行，
    //带 target_id 就保证了只能清掉自己的墓碑
    //retcode：1=删除成功，2=没有匹配记录(重复回执或已清理过)，-1=过程内部异常
    bool callClearDeleteCache(uint32_t sendId, uint32_t targetId, int& retCode);

    //注意：本类不做任何直接SELECT。数据库账号dbuser只有EXECUTE权限，
    //直接查表会报ERROR 1142，所有读操作必须走存储过程，因此没有"按账号查昵称"的接口。

private:
    // 排空CALL存储过程产生的全部结果集，为读取OUT会话变量做准备
    bool drainCallResults();

    // 读取CALL之后的OUT会话变量（例如 @retcode）并解析为int
    bool fetchOutParameter(const std::string& varName, int& outValue);
};


#endif //UNTITLED_MYSQL_H
