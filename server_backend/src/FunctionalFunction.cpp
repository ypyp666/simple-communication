
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



// ============================================================================
// 智能体会话管理（thread 表）业务层
// ============================================================================
// 三条共同约定：
//   1) 会话ID 由客户端生成，服务器只负责存储，回包必须原样回显；
//   2) agent_session_response 即使是失败包也要带 sessionId —— 客户端拿它认领
//      这次操作，收到不带ID的包会直接丢弃，表现成"5秒后超时失败"；
//   3) 数据库过程的 retcode：1=成功 2=业务性失败（已存在/不存在）-1=过程内部异常。
//      业务性失败不能回 500，要让客户端知道"确实是这个会话有问题"，而不是服务器抽风。

namespace
{
    // 会话名兜底：客户端可能传空串，库里是 VARCHAR 非空列，
    // 存进空串会让界面上出现一条没有名字的会话，点进去都不知道是哪个话题
    std::string NormalizeThreadTitle(const std::string& title)
    {
        return title.empty() ? std::string("新会话") : title;
    }

    // 会话三件套的失败回包（op = create / rename / delete）
    void FillSessionFail(json& res, const std::string& op, const std::string& sessionId,
                         int code, const std::string& message)
    {
        res["type"]      = "agent_session_response";
        res["op"]        = op;
        res["sessionId"] = sessionId;   // 失败也必须回显，否则客户端无法认领这次操作
        res["code"]      = code;
        res["message"]   = message;
        res["success"]   = false;
    }

    // 会话三件套的成功回包
    void FillSessionOk(json& res, const std::string& op, const std::string& sessionId)
    {
        res["type"]      = "agent_session_response";
        res["op"]        = op;
        res["sessionId"] = sessionId;
        res["code"]      = 0;
        res["message"]   = "";
        res["success"]   = true;
    }

    // 会话ID：协议层传下来的是一串字符串（客户端生成的原串，要原样回显），
    // 但库里 thread 表的主键是 INT，进 SQL 前必须转成整数。
    // 转不了就直接判非法参数（回 400），绝不能静默当成 0 ——
    // 那样所有非法 ID 都会折到同一行上，改一个会话名能改掉别人的。
    bool ParseSessionId(const std::string& sessionId, uint32_t& out)
    {
        if (sessionId.empty() || sessionId.size() > 10)
        {
            return false;
        }

        uint64_t value = 0;
        for (const char ch : sessionId)
        {
            if (ch < '0' || ch > '9')
            {
                return false;
            }
            value = value * 10 + static_cast<uint64_t>(ch - '0');
            if (value > 0xFFFFFFFFULL)
            {
                return false;
            }
        }

        // 0 在库里当“无效ID”用，不允许对外出现
        if (value == 0)
        {
            return false;
        }

        out = static_cast<uint32_t>(value);
        return true;
    }
}

void CreateAgentSession(json& res, uint32_t userId, const std::string& sessionId,
                        const std::string& title, mysqlconn& conn)
{
    // 会话ID 在协议层是字符串（还得原样回显给客户端），进库前必须先转成 INT，
    // 转不了就是客户端给错了，属于参数问题（400）而不是服务器故障（500）
    uint32_t threadId = 0;
    if (userId == 0 || !ParseSessionId(sessionId, threadId))
    {
        FillSessionFail(res, "create", sessionId, 400, "会话参数非法");
        return;
    }

    const std::string threadName = NormalizeThreadTitle(title);

    int retCode = -1;
    if (!conn.callAddThread(userId, threadId, threadName, retCode))
    {
        FillSessionFail(res, "create", sessionId, 500, "服务器异常，新建会话失败");
        return;
    }

    switch (retCode)
    {
        case 1:
            FillSessionOk(res, "create", sessionId);
            break;
        case 2:
            // 客户端生成的ID撞车：只在同一账号于两台设备上同时新建时才会出现。
            // 属于业务性失败，客户端应换个ID重试，而不是弹"服务器错误"
            FillSessionFail(res, "create", sessionId, 409, "会话已存在");
            break;
        default:
            FillSessionFail(res, "create", sessionId, 500,
                            "服务器异常，retcode=" + std::to_string(retCode));
            break;
    }

    std::cout << "[CreateAgentSession业务] userId=" << userId
              << ", sessionId=" << sessionId
              << ", title=" << threadName
              << ", retCode=" << retCode << std::endl;
}

void RenameAgentSession(json& res, uint32_t userId, const std::string& sessionId,
                        const std::string& title, mysqlconn& conn)
{
    // 会话ID 在协议层是字符串（还得原样回显给客户端），进库前必须先转成 INT，
    // 转不了就是客户端给错了，属于参数问题（400）而不是服务器故障（500）
    uint32_t threadId = 0;
    if (userId == 0 || !ParseSessionId(sessionId, threadId))
    {
        FillSessionFail(res, "rename", sessionId, 400, "会话参数非法");
        return;
    }

    const std::string threadName = NormalizeThreadTitle(title);

    int retCode = -1;
    if (!conn.callChangeThreadName(userId, threadId, threadName, retCode))
    {
        FillSessionFail(res, "rename", sessionId, 500, "服务器异常，重命名会话失败");
        return;
    }

    switch (retCode)
    {
        case 1:
            FillSessionOk(res, "rename", sessionId);
            break;
        case 2:
            // 会话不存在：多半是本地已经删了、服务器上也被别的设备删了，
            // 客户端收到后应该把这条本地记录一并清掉
            FillSessionFail(res, "rename", sessionId, 404, "会话不存在");
            break;
        default:
            FillSessionFail(res, "rename", sessionId, 500,
                            "服务器异常，retcode=" + std::to_string(retCode));
            break;
    }

    std::cout << "[RenameAgentSession业务] userId=" << userId
              << ", sessionId=" << sessionId
              << ", title=" << threadName
              << ", retCode=" << retCode << std::endl;
}

void DeleteAgentSession(json& res, uint32_t userId, const std::string& sessionId, mysqlconn& conn)
{
    // 会话ID 在协议层是字符串（还得原样回显给客户端），进库前必须先转成 INT，
    // 转不了就是客户端给错了，属于参数问题（400）而不是服务器故障（500）
    uint32_t threadId = 0;
    if (userId == 0 || !ParseSessionId(sessionId, threadId))
    {
        FillSessionFail(res, "delete", sessionId, 400, "会话参数非法");
        return;
    }

    int retCode = -1;
    // 连带消息：协议要求"删会话连带删掉它的消息"，但库里目前只有 Delete_thread
    // 这一个过程，没有删智能体消息的接口。现在的做法只能是靠消息表对 thread 表
    // 的外键 ON DELETE CASCADE 自动清；
    // 如果那张表没有建这个外键，就得补一个 Delete_thread_message 之类的过程，
    // 在这里 callDeleteThread 之前先调一次，别指望业务层能替数据库删数据
    if (!conn.callDeleteThread(userId, threadId, retCode))
    {
        FillSessionFail(res, "delete", sessionId, 500, "服务器异常，删除会话失败");
        return;
    }

    switch (retCode)
    {
        case 1:
            FillSessionOk(res, "delete", sessionId);
            break;
        case 2:
            // 本来就不存在：对"删掉它"这个目标来说结果是一样的，
            // 但仍然回 success=false 让客户端知道服务器上没这条记录，好顺手清本地残留
            FillSessionFail(res, "delete", sessionId, 404, "会话不存在");
            break;
        default:
            FillSessionFail(res, "delete", sessionId, 500,
                            "服务器异常，retcode=" + std::to_string(retCode));
            break;
    }

    std::cout << "[DeleteAgentSession业务] userId=" << userId
              << ", sessionId=" << sessionId
              << ", retCode=" << retCode << std::endl;
}

void LoadAgentSessions(json& res, uint32_t userId, mysqlconn& conn)
{
    res["type"] = "agent_sessions_list";

    if (userId == 0)
    {
        res["code"]    = 400;
        res["message"] = "账号格式非法";
        res["success"] = false;
        return;
    }

    std::vector<AgentThreadInfo> threads;
    int retCode = 0;
    if (!conn.callLoadThreads(userId, threads, retCode))
    {
        // 关键：查库失败绝不能回空数组。
        // 客户端把这份列表当成"服务器上到底有哪些会话"的唯一权威，
        // 一旦收到空列表就会把本地会话全部删掉 —— 一次抖动就能清空用户历史。
        // 所以失败时连 sessions 字段都不给，只给 success=false 让它跳过对齐
        res["code"]    = 500;
        res["message"] = "服务器异常，加载会话列表失败";
        res["success"] = false;
        std::cout << "[LoadAgentSessions业务] 加载失败 userId=" << userId
                  << ", retCode=" << retCode << std::endl;
        return;
    }

    // retcode 契约：1=有会话，2=无会话，其余（0 / -1）都是过程内部抛异常。
    // 这一道必须卡死，理由同上：过程报错时结果集是空的，如果当成"没有会话"回
    // success=true + []，客户端会照样清空本地历史 —— 等于把库侧故障放大成用户数据丢失
    if (retCode != 1 && retCode != 2)
    {
        res["code"]    = 500;
        res["message"] = "服务器异常，加载会话列表失败";
        res["success"] = false;
        std::cout << "[LoadAgentSessions业务] 过程异常 userId=" << userId
                  << ", retCode=" << retCode << std::endl;
        return;
    }

    json list = json::array();
    for (const auto& info : threads)
    {
        // 会话项只下发客户端需要的两个字段：ID 和 名称。
        // 库里的 threadId 是 uint32，而协议约定所有ID都以字符串形式走网，
        // 所以这里必须显式转成字符串；直接塞数字客户端会当成 number 解析失败。
        // 另外不能用花括号初始列表直接 push_back：nlohmann 不认那种写法
        json item;
        item["sessionId"] = std::to_string(info.threadId);
        item["title"]     = info.threadName;
        list.push_back(item);
    }

    res["code"]     = 0;
    res["message"]  = threads.empty() ? "暂无会话" : "";
    res["success"]  = true;
    res["sessions"] = list;

    std::cout << "[LoadAgentSessions业务] userId=" << userId
              << ", 会话数=" << threads.size()
              << ", retCode=" << retCode << std::endl;
}
