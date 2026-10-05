#ifndef UNTITLED_TABLE_H
#define UNTITLED_TABLE_H
#include"json_shift.h"
#include <unordered_map>
#include <stdexcept>
#include<string>
#include"mysql.h"


enum class  CmdType {
    Login,//登录
    Register,//注册
    Logout,//登出
    ModifyPwd,//修改密码
    ModifyNam,//修改用户名
    Repost,//消息转发
    Unknown,//未知错误
    Pull,//消息拉取
    Receive,//接收消息确认
    GetAccount,//注册页面拿到新账号
    FriendRequest,//发起好友申请
    PullFriendRequest,//拉取待处理好友申请
    AcceptFriendRequest,//同意好友申请
    RejectFriendRequest,//拒绝好友申请
    DeleteFriend,//删除好友
    SetRemark,//修改好友备注
    PullServerContacts,//拉取全部好友
    SearchRequest,//搜索用户
    PullDelFriendCache,//上线拉取离线删除好友墓碑
    AckDelFriendCache,//回执并清除已同步的删除墓碑
};

    CmdType  JsonToCmdType(std::string type);
    using CmdHandler = std::string (*)(const json& req, mysqlconn& conn, SessionPtr session);//using X = 类型;：C++11 引入的类型别名（type alias），替代老式 typedef，可读性更强

    std::string HandleLogin(const json& req, mysqlconn& conn, SessionPtr session);
    std::string HandleRegister(const json& req, mysqlconn& conn, SessionPtr session);
    std::string HandleGetInfo(const json& req, mysqlconn& conn, SessionPtr session);
    std::string HandleModifyPwd(const json& req, mysqlconn& conn, SessionPtr session);
    std::string HandleLogout(const json& req, mysqlconn& conn, SessionPtr session);
    std::string HandleUnknown(const json& req, mysqlconn& conn, SessionPtr session);
    std::string HandlePull(const json& req, mysqlconn& conn, SessionPtr session);
    std::string HandleRepost(const json& req, mysqlconn& conn, SessionPtr session);
    std::string HandleReceiveACK(const json& req, mysqlconn& conn, SessionPtr session);
    std::string HandleGetAccount(const json& req, mysqlconn& conn, SessionPtr session);
    std::string HandleModifyNam(const json& req, mysqlconn& conn, SessionPtr session);
    std::string HandleFriendRequest(const json& req, mysqlconn& conn, SessionPtr session);
    std::string HandlePullFriendRequest(const json& req, mysqlconn& conn, SessionPtr session);
    std::string HandleAcceptFriendRequest(const json& req, mysqlconn& conn, SessionPtr session);
    std::string HandleRejectFriendRequest(const json& req, mysqlconn& conn, SessionPtr session);
    std::string HandleDeleteFriend(const json& req, mysqlconn& conn, SessionPtr session);
    std::string HandleSetRemark(const json& req, mysqlconn& conn, SessionPtr session);
    std::string HandlePullServerContacts(const json& req, mysqlconn& conn, SessionPtr session);
    std::string HandleSearchRequest(const json& req, mysqlconn& conn, SessionPtr session);
    std::string HandlePullDelFriendCache(const json& req, mysqlconn& conn, SessionPtr session);
    std::string HandleAckDelFriendCache(const json& req, mysqlconn& conn, SessionPtr session);
    

inline std::unordered_map<CmdType, CmdHandler> cmd_table
{
    {CmdType::Login,HandleLogin},
    {CmdType::Register, HandleRegister},
    {CmdType::ModifyPwd, HandleModifyPwd},
    {CmdType::Logout, HandleLogout},
    {CmdType::Pull,HandlePull},
    {CmdType::Unknown, HandleUnknown},
    {CmdType::Repost, HandleRepost},
    {CmdType::Receive,HandleReceiveACK},
    {CmdType::GetAccount, HandleGetAccount},
    {CmdType::FriendRequest, HandleFriendRequest},
    {CmdType::PullFriendRequest, HandlePullFriendRequest},
    {CmdType::AcceptFriendRequest, HandleAcceptFriendRequest},
    {CmdType::RejectFriendRequest, HandleRejectFriendRequest},
    {CmdType::DeleteFriend, HandleDeleteFriend},
    {CmdType::SetRemark, HandleSetRemark},
    {CmdType::PullServerContacts, HandlePullServerContacts},
    {CmdType::SearchRequest, HandleSearchRequest},
    {CmdType::ModifyNam, HandleModifyNam},
    {CmdType::PullDelFriendCache, HandlePullDelFriendCache},
    {CmdType::AckDelFriendCache, HandleAckDelFriendCache},

};

#endif //UNTITLED_TABLE_H
