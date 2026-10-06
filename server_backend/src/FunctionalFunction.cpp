
#include "FunctionalFunction.h"
#include <cerrno>
#include <limits>
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

    // 把会话里的账号字符串解析为uint32_t，账号非法（非数字/为0/溢出）返回false
    bool ParseAccount(const std::string& text, uint32_t& account)
    {
        try
        {
            const unsigned long long parsed = std::stoull(text);
            if (parsed == 0 || parsed > std::numeric_limits<uint32_t>::max())
            {
                return false;
            }
            account = static_cast<uint32_t>(parsed);
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
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
    if (!session->login_state)
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
            {"serverId", std::to_string(m.messageId)},
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

void GetAccount(json& res, mysqlconn& conn)
{
    uint32_t newAccount = 0;
    std::cout << "[GetAccount业务] 开始获取新账号" << std::endl;
    if (!conn.callGetAccount(newAccount))
    {
        std::cerr << "[GetAccount业务] 数据库获取失败，newAccount="
                  << newAccount << std::endl;
        res["type"] = "connect_for_register_response";
        res["code"] = 500;
        res["message"] = "获取新账号失败";
        res["success"] = false;
        return;
    }
    res["type"] = "connect_for_register_response";
    res["code"] = 0;
    res["message"] = "";
    res["success"] = true;
    res["accountId"] = std::to_string(newAccount);
    std::cout << "[GetAccount业务] 准备返回: " << res.dump() << std::endl;
}

void Register(json& res, uint32_t newAccount, const std::string& pwd, mysqlconn& conn)
{
    // 账号和密码在 HandleRegister 已从请求包取好并转换完成，这里只做业务校验和落库
    res["type"] = "register_response";

    if (newAccount == 0)
    {
        res["code"] = 400;
        res["message"] = "账号格式非法";
        res["success"] = false;
        return;
    }

    if (pwd.empty() || pwd.size() > 15)
    {
        res["code"] = 400;
        res["message"] = "密码长度非法";
        res["success"] = false;
        return;
    }

    if (!conn.callRegister(newAccount, pwd))
    {
        res["code"] = 500;
        res["message"] = "注册失败";
        res["success"] = false;
        return;
    }

    res["code"] = 0;
    res["message"] = "";
    res["success"] = true;
}

void ModifyPwd(json& res, uint32_t account, const std::string& password, mysqlconn& conn)
{
    res["type"] = "modify_password_response";

    // 账号和密码已由 HandleModifyPwd 从请求包解析并转换完成
    if (account == 0)
    {
        res["code"] = 400;
        res["message"] = "账号格式非法";
        res["success"] = false;
        return;
    }

    if (password.empty() || password.size() > 15)
    {
        res["code"] = 400;
        res["message"] = "密码长度非法";
        res["success"] = false;
        return;
    }

    int retCode = -1;
    if (!conn.callModifyPwd(account, password, retCode))
    {
        res["code"] = 500;
        res["message"] = "修改密码失败";
        res["success"] = false;
        return;
    }

    if (retCode == 1)
    {
        res["code"] = 0;
        res["message"] = "";
        res["success"] = true;
    }
    else if (retCode == 0)
    {
        res["code"] = 404;
        res["message"] = "账号不存在";
        res["success"] = false;
    }
    else
    {
        // retCode = -1 是存储过程 SQLEXCEPTION 处理器写入的返回码，
        // 说明过程内部SQL执行失败（例如字段名/表名与过程不一致）
        res["code"] = 500;
        res["message"] = "数据库修改密码失败, retcode=" + std::to_string(retCode);
        res["success"] = false;
        std::cerr << "[ModifyPwd业务] 非预期返回码 retCode=" << retCode << std::endl;
    }
}

void AddFriend(json& res, uint32_t applyId, uint32_t targetId, const std::string& applyMsg,
               const std::string& sendTime, SessionPtr targetSession, mysqlconn& conn)
{
    res["type"] = "friend_request_response";

    // 本地先拦一次，减少无意义的数据库往返
    if (applyId == targetId)
    {
        res["code"] = 400;
        res["message"] = "不能添加自己为好友";
        res["success"] = false;
        return;
    }

    int retCode = -1;
    uint32_t requestId = 0;
    std::string applyName;
    if (!conn.callAddFriend(applyId, targetId, applyMsg, retCode, requestId, applyName))
    {
        res["code"] = 500;
        res["message"] = "好友申请失败";
        res["success"] = false;
        return;
    }

    switch (retCode)
    {
        case 1:
            break;   // 申请成功，继续走推送
        case 2:
            res["code"] = 400;
            res["message"] = "已存在待处理的好友申请";
            res["success"] = false;
            return;
        case 3:
            res["code"] = 400;
            res["message"] = "对方已经是你的好友";
            res["success"] = false;
            return;
        case 4:
            res["code"] = 400;
            res["message"] = "不能添加自己为好友";
            res["success"] = false;
            return;
        default:
            res["code"] = 500;
            res["message"] = "好友申请失败, retcode=" + std::to_string(retCode);
            res["success"] = false;
            return;
    }

    // 目标在线时实时推一条friend_request，name为申请人资料（头像暂无字段，用空串占位）
    if (targetSession != nullptr && targetSession->fd >= 0)
    {
        // applyName由AddFriend过程联表user带出，过程未补时为空串；
        // remark 带上申请人的留言，与请求端 friend_request 的字段含义保持一致
        // targetName为被申请人昵称：AddFriend过程目前无结果集/无OUT参数回传，暂用空串占位，
        // 与LoadNewFriend推送的字段结构保持完全一致（客户端可按 name/targetName 对称解析）
        json push = {
            {"type", "friend_request"},
            {"requestId", std::to_string(requestId)},
            {"accountId", std::to_string(applyId)},
            {"targetId", std::to_string(targetId)},
            {"name", applyName},
            {"targetName", ""},
            {"avatar", ""},
            {"remark", applyMsg},
            {"sendTime", sendTime},
        };
        const std::string buf = push.dump() + "\n";
        if (!SendAll(targetSession->fd, buf))
        {
            // 推送失败不影响申请已落库的结果，客户端可稍后拉取
            std::cerr << "[AddFriend业务] 好友申请推送失败, targetId=" << targetId << std::endl;
        }
    }

    // 协议要求该响应为纯状态，无数据字段
    res["code"] = 0;
    res["message"] = "";
    res["success"] = true;
}

void LoadNewFriend(json& res, SessionPtr session, mysqlconn& conn)
{
    res["type"] = "pull_friend_request_response";

    uint32_t account = 0;
    if (!ParseAccount(session->account, account))
    {
        res["code"] = 400;
        res["message"] = "账号格式非法";
        res["success"] = false;
        return;
    }

    std::vector<FriendApplyInfo> applies;
    int retCode = 0;
    if (!conn.callLoadNewFriend(account, applies, retCode))
    {
        res["code"] = 500;
        res["message"] = "加载好友申请失败, retcode=" + std::to_string(retCode);
        res["success"] = false;
        return;
    }

    // 逐条重推friend_request，推完再回带count的状态包
    std::size_t pushed = 0;
    for (const auto& info : applies)
    {
        // 昵称与留言由LoadNewFriend过程联表user直接带出，无需再单查
        json one = {
            {"type", "friend_request"},
            {"requestId", std::to_string(info.requestId)},
            {"accountId", std::to_string(info.applyId)},
            {"targetId", std::to_string(info.targetId)},
            {"name", info.applyName},
            {"targetName", info.targetName},
            {"avatar", ""},
            {"remark", info.applyMsg},
            {"sendTime", info.sendTime},
        };
        const std::string buf = one.dump() + "\n";
        if (!SendAll(session->fd, buf))
        {
            std::cerr << "[LoadNewFriend业务] 好友申请推送失败, requestId="
                      << info.requestId << std::endl;
            break;
        }
        ++pushed;
    }

    res["code"] = 0;
    res["message"] = (retCode == 2 || applies.empty()) ? "暂无新的好友申请" : "";
    res["success"] = true;
    res["count"] = pushed;
}

void AcceptFriend(json& res, uint32_t requestId, uint32_t account, mysqlconn& conn)
{
    res["type"] = "accept_friend_request_response";

    std::cout << "[AcceptFriend业务] 入参 requestId=" << requestId
              << ", account=" << account
              << ", 响应包type=" << res["type"].get<std::string>() << std::endl;

    int retCode = -1;
    FriendBriefInfo newFriend;
    if (!conn.callAcceptFriend(requestId, account, retCode, newFriend))
    {
        res["code"] = 500;
        res["message"] = "同意好友申请失败";
        res["success"] = false;
        std::cerr << "[AcceptFriend业务] 调用过程失败(返回false)，发出包type="
                  << res["type"].get<std::string>() << " code=500" << std::endl;
        return;
    }

    // AgreeRequest只回1/2/-1三种：2把"不存在/已处理/无权限"合并成一种，无独立的"已处理"码
    if (retCode == 2)
    {
        res["code"] = 400;
        res["message"] = "该申请不存在、已处理或无权处理";
        res["success"] = false;
        std::cerr << "[AcceptFriend业务] retcode=2，发出包type="
                  << res["type"].get<std::string>() << " code=400" << std::endl;
        return;
    }
    if (retCode != 1)
    {
        res["code"] = 500;
        res["message"] = "同意好友申请失败, retcode=" + std::to_string(retCode);
        res["success"] = false;
        std::cerr << "[AcceptFriend业务] retcode=" << retCode << " 异常，发出包type="
                  << res["type"].get<std::string>() << " code=500" << std::endl;
        return;
    }

    // 新好友资料由AgreeRequest末尾的结果集直接带回，昵称/备注都无需二次查询
    res["code"] = 0;
    res["message"] = "";
    res["success"] = true;
    res["accountId"] = std::to_string(newFriend.accountId);
    res["name"] = newFriend.name;
    res["avatar"] = "";
    res["remark"] = newFriend.remark;

    std::cout << "[AcceptFriend业务] 发出包type=" << res["type"].get<std::string>()
              << "，完整内容=" << res.dump() << std::endl;
}

void RejectFriend(json& res, uint32_t requestId, uint32_t account, mysqlconn& conn)
{
    res["type"] = "reject_friend_request_response";

    int retCode = -1;
    if (!conn.callRejectFriend(requestId, account, retCode))
    {
        res["code"] = 500;
        res["message"] = "拒绝好友申请失败";
        res["success"] = false;
        return;
    }

    if (retCode == 2)
    {
        res["code"] = 400;
        res["message"] = "该申请不存在、已处理或无权处理";
        res["success"] = false;
        return;
    }
    if (retCode != 1)
    {
        res["code"] = 500;
        res["message"] = "拒绝好友申请失败, retcode=" + std::to_string(retCode);
        res["success"] = false;
        return;
    }

    // 纯状态响应
    res["code"] = 0;
    res["message"] = "";
    res["success"] = true;
}

void DeleteFriend(json& res, uint32_t ownId, uint32_t targetId, SessionPtr targetSession, mysqlconn& conn)
{
    res["type"] = "delete_friend_response";

    int retCode = -1;
    if (!conn.callDeleteFriend(ownId, targetId, retCode))
    {
        res["code"] = 500;
        res["message"] = "删除好友失败";
        res["success"] = false;
        return;
    }

    if (retCode == 2)
    {
        res["code"] = 400;
        res["message"] = "该好友关系不存在";
        res["success"] = false;
        return;
    }
    if (retCode != 1)
    {
        res["code"] = 500;
        res["message"] = "删除好友失败, retcode=" + std::to_string(retCode);
        res["success"] = false;
        return;
    }

    res["code"] = 0;
    res["message"] = "";
    res["success"] = true;
    // targetId原样带回，客户端靠它定位本地要删的记录
    res["targetId"] = std::to_string(targetId);

    // 被删者在线时实时推一条 friend_deleted：协议里该包只有 targetId 一个字段，无success、无ACK。
    // 注意这里推出去的 targetId 语义是"【接收方】本地要移除的那个联系人"，
    // 也就是删除的发起者 ownId（被删者自己当然不用被告知自己的账号）。
    // 这与 delete_friend_cache_response 里 deletedFriends[].targetId 的含义完全一致
    if (targetSession != nullptr && targetSession->fd >= 0)
    {
        json push = {
            {"type", "friend_deleted"},
            {"targetId", std::to_string(ownId)},
        };
        const std::string buf = push.dump() + "\n";
        if (!SendAll(targetSession->fd, buf))
        {
            // 推送失败不影响服务端已删除的结果：墓碑已落库，对方下次上线拉取时仍会补上
            std::cerr << "[DeleteFriend业务] friend_deleted推送失败, 被删者=" << targetId << std::endl;
        }
        else
        {
            std::cout << "[DeleteFriend业务] 已向被删者" << targetId
                      << "推送friend_deleted, 内容=" << buf << std::endl;

            // 关键：DeleteFriend过程不管对方在不在线都会落墓碑，
            // 而friend_deleted按协议是"无ACK"的，客户端收到后不会回执，
            // 这条墓碑就会永远残留，导致对方下次上线重复拉到已删过的联系人。
            // 既然已经实时送达，就当场把这条墓碑清掉，让墓碑只服务于"离线补投"。
            int clearCode = -1;
            if (!conn.callClearDeleteCache(ownId, targetId, clearCode) || clearCode == -1)
            {
                // 清不掉也不算删除失败：客户端本地已收到推送完成删除，
                // 只是它下次上线会多拉到一条重复墓碑，属可接受的降级
                std::cerr << "[DeleteFriend业务] 在线推送后清除墓碑失败, sendId=" << ownId
                          << ", targetId=" << targetId << ", retcode=" << clearCode << std::endl;
            }
            else
            {
                std::cout << "[DeleteFriend业务] 在线推送成功，已同步清除墓碑, sendId=" << ownId
                          << ", targetId=" << targetId << ", retcode=" << clearCode << std::endl;
            }
        }
    }
    else
    {
        std::cout << "[DeleteFriend业务] 被删者" << targetId
                  << "不在线，仅落墓碑，等其上线拉取" << std::endl;
    }
}

void ModifyMark(json& res, uint32_t account, uint32_t targetId, const std::string& newMark, mysqlconn& conn)
{
    res["type"] = "set_remark_response";

    int retCode = -1;
    if (!conn.callModifyMark(account, targetId, newMark, retCode))
    {
        res["code"] = 500;
        res["message"] = "修改备注失败";
        res["success"] = false;
        return;
    }

    if (retCode == 2)
    {
        res["code"] = 400;
        res["message"] = "对方不是你的好友";
        res["success"] = false;
        return;
    }
    if (retCode != 1)
    {
        res["code"] = 500;
        res["message"] = "修改备注失败, retcode=" + std::to_string(retCode);
        res["success"] = false;
        return;
    }

    res["code"] = 0;
    res["message"] = "";
    res["success"] = true;
    // 故意不回remark，内容由客户端自己记着
    res["targetId"] = std::to_string(targetId);
}

void LoadOldFriend(json& res, SessionPtr session, mysqlconn& conn)
{
    res["type"] = "pull_server_contacts_response";

    uint32_t account = 0;
    if (!ParseAccount(session->account, account))
    {
        res["code"] = 400;
        res["message"] = "账号格式非法";
        res["success"] = false;
        return;
    }

    std::vector<FriendBriefInfo> contacts;
    int retCode = 0;
    if (!conn.callLoadOldFriend(account, contacts, retCode))
    {
        res["code"] = 500;
        res["message"] = "加载好友列表失败, retcode=" + std::to_string(retCode);
        res["success"] = false;
        return;
    }

    json list = json::array();
    for (const auto& info : contacts)
    {
        // 昵称与备注都由LoadOldFriend过程联表user直接带出，无需逐条再查
        list.push_back({
            {"accountId", std::to_string(info.accountId)},
            {"name", info.name},
            {"avatar", ""},
            {"remark", info.remark},
        });
    }

    res["code"] = 0;
    res["message"] = contacts.empty() ? "暂无好友" : "";
    res["success"] = true;
    res["contacts"] = list;
}

void SearchUser(json& res, uint32_t selfAccount, uint32_t searchAccount, mysqlconn& conn)
{
    res["type"] = "search_request_response";

    // 搜自己没有意义，直接拦掉，避免前端拿到自己还以为搜到了别人
    if (searchAccount == selfAccount)
    {
        res["code"] = 400;
        res["message"] = "不能搜索自己";
        res["success"] = false;
        return;
    }

    // SearchUser过程参数就是"被查找的账号"，按它精确匹配并已联表带出昵称
    std::cout << "[SearchUser业务] self=" << selfAccount
              << ", searchAccount=" << searchAccount << std::endl;

    std::vector<UserBriefInfo> users;
    int retCode = 0;
    if (!conn.callSearchUser(searchAccount, users, retCode))
    {
        res["code"] = 500;
        res["message"] = "搜索用户失败, retcode=" + std::to_string(retCode);
        res["success"] = false;
        return;
    }

    json list = json::array();
    for (const auto& info : users)
    {
        list.push_back({
            {"accountId", std::to_string(info.userId)},
            {"name", info.userName},
            {"avatar", ""},
            {"remark", ""},
        });
    }

    res["code"] = 0;
    res["message"] = list.empty() ? "未找到用户" : "";
    res["success"] = true;
    res["users"] = list;
}

void ModifyName(json& res, uint32_t account, const std::string& newName, mysqlconn& conn)
{
    res["type"] = "modify_name_response";

    // user.user_name 为 VARCHAR(255)，超长直接拦掉，避免数据库截断或报错
    if (newName.empty() || newName.size() > 255)
    {
        res["code"] = 400;
        res["message"] = "用户名长度非法";
        res["success"] = false;
        return;
    }

    int retCode = -1;
    if (!conn.callModifyName(account, newName, retCode))
    {
        res["code"] = 500;
        res["message"] = "修改用户名失败";
        res["success"] = false;
        return;
    }

    if (retCode == 2)
    {
        res["code"] = 400;
        res["message"] = "用户不存在或未正式注册";
        res["success"] = false;
        return;
    }
    if (retCode != 1)
    {
        res["code"] = 500;
        res["message"] = "修改用户名失败, retcode=" + std::to_string(retCode);
        res["success"] = false;
        return;
    }

    res["code"] = 0;
    res["message"] = "";
    res["success"] = true;
}

void PullDeleteFriendCache(json& res, SessionPtr session, mysqlconn& conn)
{
    res["type"] = "delete_friend_cache_response";

    uint32_t account = 0;
    if (!ParseAccount(session->account, account))
    {
        res["code"] = 400;
        res["message"] = "账号格式非法";
        res["success"] = false;
        return;
    }

    // 过程用本账号去匹配 target_id，返回的是"谁删了我"
    std::vector<DelFriendEventInfo> events;
    int retCode = 0;
    if (!conn.callLoadDelFriendEvent(account, events, retCode))
    {
        res["code"] = 500;
        res["message"] = "拉取删除墓碑失败, retcode=" + std::to_string(retCode);
        res["success"] = false;
        return;
    }

    json list = json::array();
    for (const auto& event : events)
    {
        // send_id 才是本人本地要移除的那个联系人（发起删除的人），
        // 协议里这个字段仍叫 targetId，与 friend_deleted 保持一致，都是"我要删掉的联系人"
        // 协议不带时间字段（LoadDelFriendEvent 也没有时间列），客户端只需按 targetId 删本地缓存
        list.push_back({
            {"targetId", std::to_string(event.sendId)},
        });
    }

    res["code"] = 0;
    res["message"] = events.empty() ? "暂无待同步的删除记录" : "";
    res["success"] = true;
    res["deletedFriends"] = list;

    std::cout << "[PullDeleteFriendCache业务] account=" << account
              << ", 墓碑数=" << events.size() << std::endl;
}

void AckDeleteFriendCache(json& res, SessionPtr session, const std::vector<uint32_t>& deleterIds, mysqlconn& conn)
{
    res["type"] = "ack_delete_friend_cache_response";

    // 本人就是墓碑里的被删者，直接用登录会话里的账号，不采信客户端传来的 accountId
    uint32_t account = 0;
    if (!ParseAccount(session->account, account))
    {
        res["code"] = 400;
        res["message"] = "账号格式非法";
        res["success"] = false;
        return;
    }

    // 客户端没有要清的墓碑时直接成功返回，不必空跑存储过程
    if (deleterIds.empty())
    {
        res["code"] = 0;
        res["message"] = "";
        res["success"] = true;
        return;
    }

    // 过程 Clear_delete_cache 一次只清一条（send_id + target_id），
    // 客户端是批量回执，所以这里逐条调用；
    // retcode=1 清掉了，retcode=2 表示这条本来就不存在（重复回执或已被清理），都不算失败
    std::size_t cleared = 0;
    for (const uint32_t deleterId : deleterIds)
    {
        int retCode = -1;
        if (!conn.callClearDeleteCache(deleterId, account, retCode))
        {
            res["code"] = 500;
            res["message"] = "清除删除墓碑失败";
            res["success"] = false;
            return;
        }
        if (retCode == 1)
        {
            ++cleared;
        }
        else if (retCode != 2)
        {
            res["code"] = 500;
            res["message"] = "清除删除墓碑失败, retcode=" + std::to_string(retCode)
                           + ", deleterId=" + std::to_string(deleterId);
            res["success"] = false;
            return;
        }
    }

    res["code"] = 0;
    res["message"] = "";
    res["success"] = true;

    std::cout << "[AckDeleteFriendCache业务] account=" << account
              << ", 回执数=" << deleterIds.size()
              << ", 实际清除=" << cleared << std::endl;
}


