#ifndef UNTITLED_FUNCTIONALFUNCTION_H
#define UNTITLED_FUNCTIONALFUNCTION_H
#include<iostream>
#include<string>
#include"mysql.h"
#include "json.hpp"
#include"OnlineSessionManager.h"
using json = nlohmann::json;
void Login(json& res, int account, std::string& pwd, mysqlconn& conn);
void Repost(json& res,json& message,SessionPtr target_session,mysqlconn& conn);
void Pull(json& res, SessionPtr session, mysqlconn& conn);
void DeleteCache(json& res,std::string serverId ,mysqlconn& conn);
void GetAccount(json& res, mysqlconn& conn);
// 账号和密码已由 HandleRegister 从请求包解析并转换好，这里不再碰原始 JSON
void Register(json& res, uint32_t newAccount, const std::string& pwd, mysqlconn& conn);
void ModifyPwd(json& res, uint32_t account, const std::string& password, mysqlconn& conn);

// ===== 好友/联系人功能业务函数 =====

// 发起好友申请：applyId为申请人（以登录会话为准），目标在线时由targetSession实时推送
void AddFriend(json& res, uint32_t applyId, uint32_t targetId, const std::string& applyMsg,
               const std::string& sendTime, SessionPtr targetSession, mysqlconn& conn);

// 拉取自己的待处理好友申请：逐条推送friend_request包后回响应
void LoadNewFriend(json& res, SessionPtr session, mysqlconn& conn);

// 同意好友申请：成功后回新好友资料
void AcceptFriend(json& res, uint32_t requestId, uint32_t account, mysqlconn& conn);

// 拒绝好友申请
void RejectFriend(json& res, uint32_t requestId, uint32_t account, mysqlconn& conn);

// 删除好友（服务端删除双向关系）；被删者在线时由targetSession实时推friend_deleted
void DeleteFriend(json& res, uint32_t ownId, uint32_t targetId, SessionPtr targetSession, mysqlconn& conn);

// 修改好友备注（newMark为用户输入，由数据层负责转义）
void ModifyMark(json& res, uint32_t account, uint32_t targetId, const std::string& newMark, mysqlconn& conn);

// 拉取自己的全部好友
void LoadOldFriend(json& res, SessionPtr session, mysqlconn& conn);

// 搜索用户（searchText暂未参与SQL，保留参数便于后续扩展模糊匹配）
void SearchUser(json& res, uint32_t selfAccount, uint32_t searchAccount, mysqlconn& conn);

// 修改用户名（newName为用户输入，由数据层负责转义）
void ModifyName(json& res, uint32_t account, const std::string& newName, mysqlconn& conn);

// 上线拉取本账号的离线删除墓碑（谁删了我），客户端据此清掉本地残留联系人
void PullDeleteFriendCache(json& res, SessionPtr session, mysqlconn& conn);

// 客户端本地删除完成后的回执，服务端据此清除已同步的墓碑；
// deleterIds为客户端刚在本地删掉的那些联系人账号，也就是墓碑里的发起者(删除者)
void AckDeleteFriendCache(json& res, SessionPtr session, const std::vector<uint32_t>& deleterIds, mysqlconn& conn);

#endif //UNTITLED_FUNCTIONALFUNCTION_H
