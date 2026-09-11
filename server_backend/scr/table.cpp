#include "table.h"
#include"FunctionalFunction.h"

CmdType JsonToCmdType(std::string type) {
    if (type=="login") return CmdType::Login;
    if (type=="logout") return CmdType::Logout;
    if (type=="modifyPwd") return CmdType::ModifyPwd;
    if (type=="modifyNam") return CmdType::ModifyNam;
    if (type=="repost") return CmdType::Repost;
    if (type=="pull_msg") return CmdType::Pull;
    if (type=="receive_ack") return CmdType::Receive;//这个函数的意思是对收到消息确认，删掉对应的消息缓存

    return CmdType::Unknown;
}

std::string HandleLogin(const json& req, mysqlconn& conn, SessionPtr session)
{
    auto account = req.value<std::string>("account", "");
    auto pwd = req.value<std::string>("password", "");
    std::cout <<"登录函数当前处理账号:"<<account<<" "<<"密码："<<pwd<<std::endl;

    int my_account=0;
    json rsp;
    bool status=false;
    try
    {
        my_account = std::stoi(account);
        Login(rsp,my_account,pwd,conn);
        // 登录成功后登记在线会话并标记登录状态
        if (rsp.value<bool>("success", false)) {
            session->account = account;
            session->state = true;
            OnlineSessionManager::Instance().addSession(account, session);
        }

    }
    catch (const std::invalid_argument&)
    {
        // 没有合法数字
        rsp["type"]="login_response";
        rsp["code"] = 400;
        rsp["message"] = "illegal account";
        rsp["success"]=false;
        return rsp.dump();
    }
    catch (const std::out_of_range&)
    {
        // 数字太大/太小溢出
        rsp["type"]="login_response";
        rsp["code"] = 400;
        rsp["message"] = "illegal account";
        rsp["success"]=false;
        return rsp.dump();
    }
    std::cout<<rsp.dump();
    return rsp.dump();
}

std::string HandleRepost(const json& req, mysqlconn& conn,SessionPtr session) {
    json rsp,message;
    message=req;
    auto targetID=req.value<std::string>("targetId", "");
    rsp["tempId"] = req.value<std::string>("tempId", "");
    rsp["serverId"] = "";
    if(!session->state)
    {
        rsp["type"] = "repost_response";
        rsp["code"] = 401;
        rsp["message"] = "当前登录状态异常请重新登录";
        rsp["success"] = false;
        return rsp.dump();
    }

    // 身份字段以登录会话为准，避免客户端伪造发送者
    message["accountId"] = session->account;
    message["sendId"] = session->account;
    SessionPtr fd_session = OnlineSessionManager::Instance().findSession(targetID);

    try
    {
        std::cout<<"当前正在运行消息转发"<<std::endl;
        Repost(rsp,message,fd_session,conn);
    }
    catch(const std::exception& e)
    {
        rsp["type"] = "repost_response";
        rsp["code"] = 500;
        rsp["message"] = "消息处理失败";
        rsp["success"] = false;
        std::cerr << e.what() << '\n';
    }
    return rsp.dump();
}

std::string HandlePull(const json& req, mysqlconn& conn, SessionPtr session)
{
    json rsp;
    try
    {
        Pull(rsp, session, conn);
    }
    catch (const std::exception& e)
    {
        rsp["type"] = "pull_response";
        rsp["code"] = 500;
        rsp["message"] = "数据库调用失败";
        rsp["success"] = false;
        std::cerr << e.what() << '\n';
    }
    return rsp.dump();
}

std::string HandleReceiveACK(const json& req, mysqlconn& conn, SessionPtr session)
{
    json rsp;
    const auto messageID = req.value<std::string>("serverId", "");
    rsp["type"] = "receive_ack";
    rsp["serverId"] = messageID;

    if (!session->state)
    {
        rsp["code"] = 401;
        rsp["message"] = "当前登录状态异常请重新登录";
        rsp["success"] = false;
        return rsp.dump();
    }

    try
    {
        // 前端确认失败时保留缓存，后续 pull_msg 可以再次拉取
        if (!req.value<bool>("success", false))
        {
            rsp["code"] = 0;
            rsp["message"] = "消息保留待重试";
            rsp["success"] = true;
            return rsp.dump();
        }

        DeleteCache(rsp, messageID, conn);
    }
    catch(const std::exception& e)
    {
        rsp["code"] = 500;
        rsp["message"] = "删除失败";
        rsp["success"] = false;
        std::cerr << e.what() << '\n';
    }
    return rsp.dump();
}

std::string HandleRegister(const json& req, mysqlconn& conn, SessionPtr session) {
    return "0";
}
std::string HandleGetInfo(const json& req, mysqlconn& conn, SessionPtr session) {
    return "0";
}
std::string HandleModifyPwd(const json& req, mysqlconn& conn, SessionPtr session) {
    return "0";
}
std::string HandleLogout(const json& req, mysqlconn& conn, SessionPtr session) {
    return "0";
}
std::string HandleUnknown(const json& req, mysqlconn& conn, SessionPtr session) {
    return json{
        {"type", "error_response"},
        {"code", 400},
        {"message", "未知请求类型"},
        {"success", false}
    }.dump();
}
