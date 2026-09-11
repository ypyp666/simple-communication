
#include "FunctionalFunction.h"
#include <cerrno>
#include <sys/socket.h>

namespace
{
    bool SendAll(int fd, const std::string& data)
    {
        std::size_t sent = 0;
        while (sent < data.size())
        {
            const ssize_t size = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (size < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                return false;
            }
            if (size == 0)
            {
                return false;
            }
            sent += static_cast<std::size_t>(size);
        }
        return true;
    }
}

void Login(json& res,int account,  std::string& pwd, mysqlconn& conn)
{
    int retCode=0 ;
    conn.callLoginFunc(account, pwd,retCode);
    switch (retCode) {
        case 0:
            res["type"]="login_response";
            res["code"] = 400;
            res["message"] = "Data abnormal crash";//数据异常崩溃
            res["success"]=false;
            break;
        case 1:
            res["type"]="login_response";
            res["code"] = 400;
            res["message"] = "Account does not exist";
            res["success"]=false;
            break;
        case 2:
            res["type"]="login_response";
            res["code"] = 400;
            res["message"] = "Incorrect password";
            res["success"]=false;
            break;
        case 3:
            res["type"]="login_response";
            res["code"] = 0;
            res["message"] = "";
            res["success"]=true;
            break;
        default:
            throw std::out_of_range("System error, data anomaly");
            break;
    }

}

void Repost(json& res, json& message, SessionPtr target_session, mysqlconn& conn)
{
    // 目标用户不在线


    // 调用存储过程把消息写入数据库
    uint32_t outMessageId = 0;
    std::cout<<"正在调用数据库消息存储过程"<<std::endl;
    if (!conn.callMessage(message, outMessageId))
    {
        res["type"] = "repost_response";
        res["code"] = 500;
        res["message"] = "消息存储失败";
        res["success"] = false;
        return;
    }
    if(!outMessageId)
    {
        res["type"] = "repost_response";
        res["code"] = 500;
        res["message"] = "消息过程运行错误或者账号异常";
        res["success"] = false;
        return;
    }
    // Qt 前端按字符串读取这些标识，不能直接返回 JSON 数字
    message["serverId"] = std::to_string(outMessageId);

   // 目标用户在线，直接转发；协议要求每条JSON以\n结尾
    std::string forward = message.dump() + "\n";
     if (!(target_session == nullptr))
    {
         if (!SendAll(target_session->fd, forward))
         {
             res["type"] = "repost_response";
             res["code"] = 500;
              res["message"] = "消息转发失败";
             res["success"] = false;
             return;
         }
    }

    res["type"] = "repost_response";
    res["code"] = 0;
    res["message"] = "";
    res["success"] = true;
    res["serverId"] = std::to_string(outMessageId);
}

void Pull(json& res, SessionPtr session, mysqlconn& conn)
{
    // 拉取人是自己，直接复用传入的会话对象，无需findSession
    res["type"] = "pull_response";
    if (!session->state)
    {
        res["code"] = 401;
        res["message"] = "当前登录状态异常请重新登录";
        res["success"] = false;
        return;
    }

    // 拉取的是发给自己的消息，targetID 取自己的账号
    uint32_t targetId = 0;
    try
    {
        targetId = static_cast<uint32_t>(std::stoul(session->account));
    }
    catch (const std::exception& e)
    {
        res["code"] = 400;
        res["message"] = "账号格式非法";
        res["success"] = false;
        return;
    }

    std::vector<MessageInfo> msgs;
    int retcode = 0;
    if (!conn.callLoadMessage(targetId, msgs, retcode) && retcode != 2)
    {
        res["code"] = 500;
        res["message"] = "消息加载失败, retcode=" + std::to_string(retcode);
        res["success"] = false;
        return;
    }

    // 大量消息直接在功能函数里逐条发给拉取人自己的socket
    for (const auto& m : msgs)
    {
        json one = {
            {"type", "repost"},
            {"ID", std::to_string(m.messageId)},
            {"accountId", session->account},
            {"sendId", std::to_string(m.senderId)},
            {"targetId", std::to_string(m.targetId)},
            {"sendTime", m.sendTime},
            {"content", m.content},
            {"isOffline", true},
        };
        std::string buf = one.dump() + "\n";
        std::cout<<buf<<std::endl;
        if (send(session->fd, buf.c_str(), buf.size(), 0) < 0)
        {
            std::cerr << "消息推送失败, msgId=" << m.messageId << std::endl;
            break;
        }
    }

    if (retcode == 1 || retcode == 2)
    {
        // 最后返回确认状态包（含条数）
        res["code"] = 0;
        res["message"] = retcode == 2 ? "未有离线消息" : "";
        res["success"] = true;
        res["count"] = msgs.size();
    }
}

void DeleteCache(json& res,std::string serverId,mysqlconn& conn)
{
    auto messageID=serverId;
    const unsigned int parsedSendId = std::stoull(messageID);
    if(!conn.callDeleteMessage(parsedSendId))
    {
        res["type"]="receive_ack";
        res["code"]=500;
        res["message"]="，数据库调用异常";
        res["success"]=false;
        return;
    }
    res["type"]="receive_ack";
    res["code"]=0;
    res["message"]="";
    res["success"]=true;
}
