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

};

#endif //UNTITLED_TABLE_H
