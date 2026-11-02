#include "table.h"
#include"FunctionalFunction.h"
#include "agent_tcp.h"
#include <limits>

namespace
{
    // 把JSON里的字符串ID转成uint32_t，非数字/为0/溢出都判为非法
    bool ParseUint32(const std::string& text, uint32_t& value)
    {
        try
        {
            const unsigned long long parsed = std::stoull(text);
            if (parsed == 0 || parsed > std::numeric_limits<uint32_t>::max())
            {
                return false;
            }
            value = static_cast<uint32_t>(parsed);
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    // 判断整串是否只由数字组成。
    // std::stoull 遇到 "12a3" 会静默截成 12 而不报错，账号场景必须整串都是数字才算合法
    bool IsAllDigits(const std::string& text)
    {
        if (text.empty())
        {
            return false;
        }
        for (const char ch : text)
        {
            if (ch < '0' || ch > '9')
            {
                return false;
            }
        }
        return true;
    }

    // 按UTF-8字符数计数：0x80~0xBF 是续接字节，不单独算一个字符。
    // 数据库里 varchar(255) 的 255 指的是字符而不是字节，用 size() 会误判中文长度
    size_t Utf8Length(const std::string& text)
    {
        size_t count = 0;
        for (const unsigned char ch : text)
        {
            if ((ch & 0xC0) != 0x80)
            {
                ++count;
            }
        }
        return count;
    }

    // 好友/联系人功能统一的未登录响应
    std::string NotLoginResponse(const std::string& type)
    {
        json rsp;
        rsp["type"] = type;
        rsp["code"] = 401;
        rsp["message"] = "当前登录状态异常请重新登录";
        rsp["success"] = false;
        return rsp.dump();
    }

    // 全业务统一的登录态校验（除 登录/断线重连/注册/获取账号/修改密码 外，其余一律先过这一关）：
    // 第一道，看本连接自己记的 login_state；
    // 第二道，再回服务端会话字典确认"该账号当前登记的连接就是本连接"——
    // 同账号在新连接上重登/重连后，旧连接手里那个 session 的 login_state 会被置回 false，
    // 就算没置，字典里也已经换了人，旧连接不会还能接着操作。
    bool IsLoggedIn(const SessionPtr& session)
    {
        if (!session || !session->login_state)
        {
            return false;
        }
        return OnlineSessionManager::Instance().isCurrentSession(session->account, session);
    }
}

CmdType JsonToCmdType(std::string type) {
    if (type=="login") return CmdType::Login;
    if (type=="reconnect_login") return CmdType::ReconnectLogin;
    if (type=="logout") return CmdType::Logout;
    if (type=="modify_password") return CmdType::ModifyPwd;
    if (type=="modify_name") return CmdType::ModifyNam;
    if (type=="modifyNam") return CmdType::ModifyNam;
    if (type=="repost") return CmdType::Repost;
    if (type=="pull_msg") return CmdType::Pull;
    if (type=="receive_ack") return CmdType::Receive;//这个函数的意思是对收到消息确认，删掉对应的消息缓存
    if (type=="register") return CmdType::Register;
    if (type=="connect_for_register") return CmdType::GetAccount;
    if (type=="friend_request") return CmdType::FriendRequest;
    if (type=="pull_friend_request") return CmdType::PullFriendRequest;
    if (type=="accept_friend_request") return CmdType::AcceptFriendRequest;
    if (type=="reject_friend_request") return CmdType::RejectFriendRequest;
    if (type=="delete_friend") return CmdType::DeleteFriend;
    if (type=="set_remark") return CmdType::SetRemark;
    if (type=="pull_server_contacts") return CmdType::PullServerContacts;
    if (type=="search_request") return CmdType::SearchRequest;
    if (type=="pull_delete_friend_cache") return CmdType::PullDelFriendCache;
    if (type=="ack_delete_friend_cache") return CmdType::AckDelFriendCache;
    if (type=="examine") return CmdType::Examine;
    if (type=="agent_request") return CmdType::AgentRequest;
    // 注意这里全部用下划线风格，和 agent_request 保持一致；
    // 大小写不统一是本项目的历史包袱（login 小写、ModifyNam 驼峰），新增的别再添乱
    if (type=="agent_message_create") return CmdType::AgentMessageCreate;
    if (type=="agent_stop") return CmdType::AgentStop;
    if (type=="agent_session_create") return CmdType::AgentSessionCreate;
    if (type=="agent_session_rename") return CmdType::AgentSessionRename;
    if (type=="agent_session_delete") return CmdType::AgentSessionDelete;
    // 请求叫 pull，响应包类型才叫 agent_sessions_list，按协议表的命名分别写死
    if (type=="agent_sessions_pull") return CmdType::AgentSessionsList;
    // 旧名字保底：早期客户端发的是 agent_sessions_list，别让它们掉进 Unknown
    if (type=="agent_sessions_list") return CmdType::AgentSessionsList;

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
            session->login_state = true;
            OnlineSessionManager::Instance().addSession(account, session);
            // 登录成功即停止未登录计数：清空限流窗口
            session->limit_time = 0;
            session->unlogin_count = 0;
            // 正常登录：签发一个全新的16位临时令牌（同账号旧令牌立即作废，键存在即覆盖、不存在即新建），
            // 随登录响应一起下发给客户端
            rsp["token"] = OnlineSessionManager::Instance().issueNewToken(account);
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

// 断线重连登录：与普通登录的区别在于三点——
// 1) 响应类型是 reconnect_login_response，客户端据此走"续接会话"分支；
// 2) 只校验令牌，不查库、不签新令牌：客户端掉线重连时手里就只剩令牌；
// 3) 回包内容只有"成功与否"（success + 失败原因 message）——
//    重连本来就不换令牌，不回 token，客户端手里那份继续用。
std::string HandleReconnectLogin(const json& req, mysqlconn&, SessionPtr session)
{
    const auto account = req.value<std::string>("account", "");
    const auto token = req.value<std::string>("token", "");
    std::cout << "断线重连登录，账号:" << account
              << " 令牌长度:" << token.size() << std::endl;

    json rsp;
    rsp["type"] = "reconnect_login_response";
    rsp["success"] = false;
    rsp["message"] = "";

    // 账号必须是纯数字：它既是会话字典的键，也是令牌表的键，格式错了一律拒掉
    uint32_t my_account = 0;
    if (!ParseUint32(account, my_account))
    {
        rsp["message"] = "illegal account";
        return rsp.dump();
    }

    // 重连只认令牌：与服务端令牌表里存的逐字比对。
    // 表里没有该账号（服务重启过、或已经主动登出）或对不上，一律判失败，
    // 交给客户端主后端把用户送回登录页重新走一次 login
    const std::string stored = OnlineSessionManager::Instance().findToken(account);
    const bool ok = (!stored.empty() && stored == token);
    rsp["success"] = ok;
    rsp["message"] = ok ? "" : "令牌无效或已过期, 请重新登录";

    if (ok)
    {
        // 重连成功：新连接接管该账号（addSession 里会把旧连接的登录态作废），
        // 令牌原样保留，不重新签发
        session->account = account;
        session->login_state = true;
        OnlineSessionManager::Instance().addSession(account, session);
        // 重连成功同样停止未登录计数
        session->limit_time = 0;
        session->unlogin_count = 0;
    }

    std::cout << rsp.dump();
    return rsp.dump();
}

std::string HandleRepost(const json& req, mysqlconn& conn,SessionPtr session) {
    json rsp,message;
    message=req;
    auto targetID=req.value<std::string>("targetId", "");
    rsp["tempId"] = req.value<std::string>("tempId", "");
    rsp["serverId"] = "";
    if(!IsLoggedIn(session))
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
    rsp["type"] = "pull_response";

    if (!IsLoggedIn(session))
    {
        return NotLoginResponse("pull_response");
    }

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

    if (!IsLoggedIn(session))
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

std::string HandleGetAccount(const json& req, mysqlconn& conn, SessionPtr session) {
    json rsp;
    std::cout << "[GetAccount处理] 收到请求: " << req.dump() << std::endl;
    try
    {
        GetAccount(rsp, conn);
    }
    catch (const std::exception& e)
    {
        rsp["type"] = "connect_for_register_response";
        rsp["code"] = 500;
        rsp["message"] = "服务器异常";
        rsp["success"] = false;
        std::cerr << e.what() << '\n';
    }
    std::cout << "[GetAccount处理] 返回响应: " << rsp.dump() << std::endl;
    return rsp.dump();
}

std::string HandleRegister(const json& req, mysqlconn& conn, SessionPtr session) {
    json rsp;
    rsp["type"] = "register_response";
    std::cout << "当前正在处理注册" << std::endl;

    // 客户端注册包字段是 account/password，账号是取号阶段服务器下发的字符串，
    // 在这里统一解析成数值型，避免把 JSON 对象拷贝来拷贝去导致取值失败
    const std::string accountText = req.value<std::string>("account", "");
    const std::string password = req.value<std::string>("password", "");

    uint32_t newAccount = 0;
    try
    {
        const unsigned long long account = std::stoull(accountText);
        if (account == 0 || account > std::numeric_limits<uint32_t>::max())
        {
            throw std::out_of_range("account超出uint32_t范围");
        }
        newAccount = static_cast<uint32_t>(account);
    }
    catch (const std::exception& e)
    {
        rsp["code"] = 400;
        rsp["message"] = "账号格式非法";
        rsp["success"] = false;
        std::cerr << "注册账号转换失败: " << e.what() << std::endl;
        return rsp.dump();
    }

    try {
        Register(rsp, newAccount, password, conn);
    }
    catch (const std::exception& e) {
     rsp["code"] = 500;
     rsp["message"] = "服务器异常";
     rsp["success"] = false;
     std::cerr << e.what() << '\n'; 
    }
    return rsp.dump();
}
std::string HandleGetInfo(const json& req, mysqlconn& conn, SessionPtr session) {
    return "0";
}
std::string HandleModifyPwd(const json& req, mysqlconn& conn, SessionPtr session) {
    json rsp;
    rsp["type"] = "modify_password_response";

    const std::string accountText = req.value<std::string>("account", "");
    const std::string password = req.value<std::string>("new_password", "");

    uint32_t account = 0;
    try
    {
        const unsigned long long parsedAccount = std::stoull(accountText);
        if (parsedAccount == 0 ||
            parsedAccount > std::numeric_limits<uint32_t>::max())
        {
            throw std::out_of_range("account超出uint32_t范围");
        }
        account = static_cast<uint32_t>(parsedAccount);
    }
    catch (const std::exception& e)
    {
        rsp["code"] = 400;
        rsp["message"] = "账号格式非法";
        rsp["success"] = false;
        std::cerr << "修改密码账号转换失败: " << e.what() << std::endl;
        return rsp.dump();
    }

    try
    {
        ModifyPwd(rsp, account, password, conn);
    }
    catch (const std::exception& e)
    {
        rsp["type"] = "modify_password_response";
        rsp["code"] = 500;
        rsp["message"] = "服务器异常";
        rsp["success"] = false;
        std::cerr << "修改密码处理异常: " << e.what() << std::endl;
    }
    return rsp.dump();
}
std::string HandleLogout(const json& req, mysqlconn& conn, SessionPtr session) {
    json rsp;
    rsp["type"] = "logout_response";

    // 登出本身就是在动登录态，所以得先确认当前确实是登录着的
    if (!IsLoggedIn(session))
    {
        return NotLoginResponse("logout_response");
    }

    // 主动登出三件事：作废临时令牌（否则旧令牌还能被拿来重连）、
    // 从在线会话字典里摘掉本连接、清空本连接的登录态。
    // 账号要先存一份，因为下面会把 session->account 清空。
    const std::string account = session->account;
    OnlineSessionManager::Instance().removeToken(account);
    OnlineSessionManager::Instance().removeSession(account, session);
    session->login_state = false;
    session->account.clear();

    rsp["code"] = 0;
    rsp["message"] = "";
    rsp["success"] = true;
    std::cout << "[Logout] 账号" << account << "已登出，临时令牌已作废" << std::endl;
    return rsp.dump();
}
std::string HandleUnknown(const json& req, mysqlconn& conn, SessionPtr session) {
    return json{
        {"type", "error_response"},
        {"code", 400},
        {"message", "未知请求类型"},
        {"success", false}
    }.dump();
}

std::string HandleModifyNam(const json& req, mysqlconn& conn, SessionPtr session)
{
    json rsp;
    rsp["type"] = "modify_name_response";

    if (!IsLoggedIn(session))
    {
        return NotLoginResponse("modify_name_response");
    }

    // 账号以登录会话为准，避免客户端伪造
    uint32_t account = 0;
    if (!ParseUint32(session->account, account))
    {
        rsp["code"] = 400;
        rsp["message"] = "账号格式非法";
        rsp["success"] = false;
        return rsp.dump();
    }

    const std::string newName = req.value<std::string>("new_name", "");

    try
    {
        ModifyName(rsp, account, newName, conn);
    }
    catch (const std::exception& e)
    {
        rsp["code"] = 500;
        rsp["message"] = "服务器异常";
        rsp["success"] = false;
        std::cerr << "修改用户名处理异常: " << e.what() << std::endl;
    }
    return rsp.dump();
}

std::string HandleFriendRequest(const json& req, mysqlconn& conn, SessionPtr session)
{
    json rsp;
    rsp["type"] = "friend_request_response";

    if (!IsLoggedIn(session))
    {
        return NotLoginResponse("friend_request_response");
    }

    // 申请人以登录会话为准，防止客户端伪造accountId
    uint32_t applyId = 0;
    if (!ParseUint32(session->account, applyId))
    {
        rsp["code"] = 400;
        rsp["message"] = "账号格式非法";
        rsp["success"] = false;
        return rsp.dump();
    }

    uint32_t targetId = 0;
    if (!ParseUint32(req.value<std::string>("targetId", ""), targetId))
    {
        rsp["code"] = 400;
        rsp["message"] = "目标账号格式非法";
        rsp["success"] = false;
        return rsp.dump();
    }

    // 申请留言由客户端传入（friend_request 协议里放在 remark 字段），属于用户输入：
    // 转义由数据层负责，这里只做长度校验。客户端完全没带该字段时才回落到默认文案
    std::string applyMsg = "请求添加你为好友";
    if (req.contains("remark"))
    {
        applyMsg = req.value<std::string>("remark", "");
    }
    if (Utf8Length(applyMsg) > 255)   // 过程参数 apply_msg 为 varchar(255)
    {
        rsp["code"] = 400;
        rsp["message"] = "申请留言过长";
        rsp["success"] = false;
        return rsp.dump();
    }

    const std::string sendTime = req.value<std::string>("sendTime", "");

    // 目标在线才有会话可推，离线时只落库，等对方下次拉取
    SessionPtr targetSession = OnlineSessionManager::Instance().findSession(std::to_string(targetId));

    try
    {
        AddFriend(rsp, applyId, targetId, applyMsg, sendTime, targetSession, conn);
    }
    catch (const std::exception& e)
    {
        rsp["code"] = 500;
        rsp["message"] = "服务器异常";
        rsp["success"] = false;
        std::cerr << "好友申请处理异常: " << e.what() << std::endl;
    }
    return rsp.dump();
}

std::string HandlePullFriendRequest(const json& req, mysqlconn& conn, SessionPtr session)
{
    json rsp;
    rsp["type"] = "pull_friend_request_response";

    if (!IsLoggedIn(session))
    {
        return NotLoginResponse("pull_friend_request_response");
    }

    try
    {
        LoadNewFriend(rsp, session, conn);
    }
    catch (const std::exception& e)
    {
        rsp["code"] = 500;
        rsp["message"] = "服务器异常";
        rsp["success"] = false;
        std::cerr << "拉取好友申请处理异常: " << e.what() << std::endl;
    }
    return rsp.dump();
}

std::string HandleAcceptFriendRequest(const json& req, mysqlconn& conn, SessionPtr session)
{
    json rsp;
    rsp["type"] = "accept_friend_request_response";

    if (!IsLoggedIn(session))
    {
        return NotLoginResponse("accept_friend_request_response");
    }

    uint32_t account = 0;
    if (!ParseUint32(session->account, account))
    {
        rsp["code"] = 400;
        rsp["message"] = "账号格式非法";
        rsp["success"] = false;
        return rsp.dump();
    }

    // 协议要求该请求只带requestId
    uint32_t requestId = 0;
    if (!ParseUint32(req.value<std::string>("requestId", ""), requestId))
    {
        rsp["code"] = 400;
        rsp["message"] = "申请ID格式非法";
        rsp["success"] = false;
        return rsp.dump();
    }

    try
    {
        AcceptFriend(rsp, requestId, account, conn);
    }
    catch (const std::exception& e)
    {
        rsp["code"] = 500;
        rsp["message"] = "服务器异常";
        rsp["success"] = false;
        std::cerr << "同意好友申请处理异常: " << e.what() << std::endl;
    }

    // 打印最终要回给客户端的包（含type），排查时可确认发出的报文类型与内容
    const std::string reply = rsp.dump();
    std::cout << "[HandleAcceptFriendRequest] 返回包type=" << rsp["type"].get<std::string>()
              << "，完整内容=" << reply << std::endl;
    return reply;
}

std::string HandleRejectFriendRequest(const json& req, mysqlconn& conn, SessionPtr session)
{
    json rsp;
    rsp["type"] = "reject_friend_request_response";

    if (!IsLoggedIn(session))
    {
        return NotLoginResponse("reject_friend_request_response");
    }

    uint32_t account = 0;
    if (!ParseUint32(session->account, account))
    {
        rsp["code"] = 400;
        rsp["message"] = "账号格式非法";
        rsp["success"] = false;
        return rsp.dump();
    }

    uint32_t requestId = 0;
    if (!ParseUint32(req.value<std::string>("requestId", ""), requestId))
    {
        rsp["code"] = 400;
        rsp["message"] = "申请ID格式非法";
        rsp["success"] = false;
        return rsp.dump();
    }

    try
    {
        RejectFriend(rsp, requestId, account, conn);
    }
    catch (const std::exception& e)
    {
        rsp["code"] = 500;
        rsp["message"] = "服务器异常";
        rsp["success"] = false;
        std::cerr << "拒绝好友申请处理异常: " << e.what() << std::endl;
    }
    return rsp.dump();
}

std::string HandleDeleteFriend(const json& req, mysqlconn& conn, SessionPtr session)
{
    json rsp;
    rsp["type"] = "delete_friend_response";

    if (!IsLoggedIn(session))
    {
        return NotLoginResponse("delete_friend_response");
    }

    uint32_t account = 0;
    if (!ParseUint32(session->account, account))
    {
        rsp["code"] = 400;
        rsp["message"] = "账号格式非法";
        rsp["success"] = false;
        return rsp.dump();
    }

    uint32_t targetId = 0;
    if (!ParseUint32(req.value<std::string>("targetId", ""), targetId))
    {
        rsp["code"] = 400;
        rsp["message"] = "目标账号格式非法";
        rsp["success"] = false;
        return rsp.dump();
    }

    // 被删者在线时才有会话可推实时 friend_deleted，离线时只落墓碑等其上线拉取
    SessionPtr targetSession = OnlineSessionManager::Instance().findSession(std::to_string(targetId));

    try
    {
        DeleteFriend(rsp, account, targetId, targetSession, conn);
    }
    catch (const std::exception& e)
    {
        rsp["code"] = 500;
        rsp["message"] = "服务器异常";
        rsp["success"] = false;
        std::cerr << "删除好友处理异常: " << e.what() << std::endl;
    }
    return rsp.dump();
}

std::string HandleSetRemark(const json& req, mysqlconn& conn, SessionPtr session)
{
    json rsp;
    rsp["type"] = "set_remark_response";

    if (!IsLoggedIn(session))
    {
        return NotLoginResponse("set_remark_response");
    }

    uint32_t account = 0;
    if (!ParseUint32(session->account, account))
    {
        rsp["code"] = 400;
        rsp["message"] = "账号格式非法";
        rsp["success"] = false;
        return rsp.dump();
    }

    uint32_t targetId = 0;
    if (!ParseUint32(req.value<std::string>("targetId", ""), targetId))
    {
        rsp["code"] = 400;
        rsp["message"] = "目标账号格式非法";
        rsp["success"] = false;
        return rsp.dump();
    }

    // remark是用户输入，转义在数据层完成；空串表示清空备注
    const std::string remark = req.value<std::string>("remark", "");

    try
    {
        ModifyMark(rsp, account, targetId, remark, conn);
    }
    catch (const std::exception& e)
    {
        rsp["code"] = 500;
        rsp["message"] = "服务器异常";
        rsp["success"] = false;
        std::cerr << "修改备注处理异常: " << e.what() << std::endl;
    }
    return rsp.dump();
}

std::string HandlePullServerContacts(const json& req, mysqlconn& conn, SessionPtr session)
{
    json rsp;
    rsp["type"] = "pull_server_contacts_response";

    if (!IsLoggedIn(session))
    {
        return NotLoginResponse("pull_server_contacts_response");
    }

    try
    {
        LoadOldFriend(rsp, session, conn);
    }
    catch (const std::exception& e)
    {
        rsp["code"] = 500;
        rsp["message"] = "服务器异常";
        rsp["success"] = false;
        std::cerr << "拉取好友列表处理异常: " << e.what() << std::endl;
    }
    return rsp.dump();
}

std::string HandleSearchRequest(const json& req, mysqlconn& conn, SessionPtr session)
{
    json rsp;
    rsp["type"] = "search_request_response";

    if (!IsLoggedIn(session))
    {
        return NotLoginResponse("search_request_response");
    }

    uint32_t account = 0;
    if (!ParseUint32(session->account, account))
    {
        rsp["code"] = 400;
        rsp["message"] = "账号格式非法";
        rsp["success"] = false;
        return rsp.dump();
    }

    // 搜索按账号精确匹配（UI已用正则限制只能输入数字），不做文本模糊搜索。
    // 这里的 searchText 语义就是"要查找的那个账号"，不是关键字
    const std::string searchText = req.value<std::string>("searchText", "");
    if (!IsAllDigits(searchText))
    {
        rsp["code"] = 400;
        rsp["message"] = "请输入正确的账号";
        rsp["success"] = false;
        return rsp.dump();
    }

    uint32_t searchAccount = 0;
    if (!ParseUint32(searchText, searchAccount))
    {
        rsp["code"] = 400;
        rsp["message"] = "请输入正确的账号";
        rsp["success"] = false;
        return rsp.dump();
    }

    try
    {
        SearchUser(rsp, account, searchAccount, conn);
    }
    catch (const std::exception& e)
    {
        rsp["code"] = 500;
        rsp["message"] = "服务器异常";
        rsp["success"] = false;
        std::cerr << "搜索用户处理异常: " << e.what() << std::endl;
    }
    return rsp.dump();
}

std::string HandlePullDelFriendCache(const json& req, mysqlconn& conn, SessionPtr session)
{
    json rsp;
    rsp["type"] = "delete_friend_cache_response";

    if (!IsLoggedIn(session))
    {
        return NotLoginResponse("delete_friend_cache_response");
    }

    // 协议里虽然带了accountId，但拉的必定是本人的墓碑，所以以登录会话为准，防止伪造他人账号
    try
    {
        PullDeleteFriendCache(rsp, session, conn);
    }
    catch (const std::exception& e)
    {
        rsp["code"] = 500;
        rsp["message"] = "服务器异常";
        rsp["success"] = false;
        std::cerr << "拉取删除墓碑处理异常: " << e.what() << std::endl;
    }
    return rsp.dump();
}

std::string HandleAckDelFriendCache(const json& req, mysqlconn& conn, SessionPtr session)
{
    json rsp;
    rsp["type"] = "ack_delete_friend_cache_response";

    if (!IsLoggedIn(session))
    {
        return NotLoginResponse("ack_delete_friend_cache_response");
    }

    // 协议：accountId 是"目标ID"(被删的自己)，deleterIds 是发起者账号数组(批量)；
    // 目标账号一律以登录会话为准，客户端传的 accountId 只做展示不采信。
    // 兼容旧字段名 targetIds
    const json* idArray = nullptr;
    if (req.contains("deleterIds") && req["deleterIds"].is_array())
    {
        idArray = &req["deleterIds"];
    }
    else if (req.contains("targetIds") && req["targetIds"].is_array())
    {
        idArray = &req["targetIds"];
    }

    // 逐个校验后转成uint32_t；任何一项非法都直接判错，不静默丢弃，否则墓碑会残留清不掉
    std::vector<uint32_t> deleterIds;
    if (idArray != nullptr)
    {
        for (const auto& item : *idArray)
        {
            const std::string text = item.is_string() ? item.get<std::string>() : "";
            uint32_t id = 0;
            if (!ParseUint32(text, id))
            {
                rsp["code"] = 400;
                rsp["message"] = "联系人账号格式非法";
                rsp["success"] = false;
                return rsp.dump();
            }
            deleterIds.push_back(id);
        }
    }

    try
    {
        AckDeleteFriendCache(rsp, session, deleterIds, conn);
    }
    catch (const std::exception& e)
    {
        rsp["code"] = 500;
        rsp["message"] = "服务器异常";
        rsp["success"] = false;
        std::cerr << "清除删除墓碑处理异常: " << e.what() << std::endl;
    }
    return rsp.dump();
}

// 心跳检验：客户端登录后每 3 秒发一次 {"type":"examine","examineId":n}，
// 服务端把 examineId 原样回带，客户端据此确认链路还活着。
// 不查库。但登录态要校验：心跳本身就是建立在"登录成功后的链路"上的，
// 未登录 / 已被新连接顶号时回 401，客户端会当成异常走重连，这正是期望行为。
std::string HandleExamine(const json& req, mysqlconn&, SessionPtr session)
{
    json rsp;
    rsp["type"] = "examine_response";

    if (!IsLoggedIn(session))
    {
        return NotLoginResponse("examine_response");
    }

    // examineId 由客户端自己递增并用来和新旧回包配对，所以必须原样回带。
    // 客户端用 QJsonObject 赋的是整数(number)，这里按原类型回带，
    // 同时兼容字符串写法，避免客户端改动后取值时抛类型错误。
    if (req.contains("examineId") &&
        (req["examineId"].is_number() || req["examineId"].is_string()))
    {
        rsp["examineId"] = req["examineId"];
    }
    else
    {
        // 缺字段或类型不对时回 0，客户端会因为对不上号而算一次超时，属预期行为
        rsp["examineId"] = 0;
    }

    rsp["code"] = 0;
    rsp["message"] = "";
    rsp["success"] = true;

    std::cout << "[Examine心跳] 回带 examineId=" << rsp["examineId"].dump() << std::endl;
    return rsp.dump();
}

// 智能体请求：前端发起一次对话。
//
// 关键点：这里绝不能在客户端线程里直接调智能体——那是至少几百毫秒、多则几十秒的阻塞，
// 而客户端心跳是 3 秒一发、等 1 秒、连续超 3 次就断线重连，
// 把调用写在返回路径上，用户一定会看到"发了条消息然后就掉线"。
//
// 所以这里只做三件事：校验 -> 入队 -> 立刻回 ack，
// 真正的答案稍后由 worker 以 agent_response 推送回同一个连接。
std::string HandleAgentRequest(const json& req, mysqlconn&, SessionPtr session)
{
    if (!IsLoggedIn(session))
    {
        return NotLoginResponse("agent_response");
    }

    // 协议要求三个字段全带：会话ID（客户端生成）、消息ID（agent_message_create 分配的）、
    // 问题正文。先全部取出来，后面每条失败回包都要回显它们，
    // 客户端才能把"这条报错"归到某条会话线、某个气泡上
    const std::string threadId  = req.value<std::string>("threadId", "");
    const std::string messageId = req.value<std::string>("messageId", "");
    const std::string prompt    = req.value<std::string>("prompt", "");

    // 每个失败分支都必须带 done=true：客户端已经把气泡挂成"思考中"，
    // 收不到 done 这条会话就永久卡在"回答中"，用户再也发不出新问题
    auto replyFail = [threadId, messageId](int code, const std::string& message)
    {//lambda函数固定格式:[捕获列表](参数列表) -> 返回类型 { 函数体 }`[]` 捕获列表：**创建 lambda 的时候，就把值 “封包固定下来” 的上下文变量**（环境变量）
        json rsp;
        rsp["type"]     = "agent_response";
        rsp["threadId"] = threadId;
        rsp["serverId"] = messageId;   // 客户端会忽略，带着方便抓包定位
        rsp["answer"]   = "";
        rsp["content"]  = "";
        rsp["code"]     = code;
        rsp["message"]  = message;
        rsp["success"]  = false;
        rsp["done"]     = true;
        rsp["finished"] = true;
        return rsp.dump();
    };

    if (threadId.empty())
    {
        return replyFail(400, "会话ID不能为空");
    }
    // 会话ID 必须是纯数字。这不只是数据校验，还是"智能体 thread_id 拼接"能成立的前提：
    // 发给智能体的是 "会话ID + 'n' + 账号"，只有两段都是纯数字，
    // 字母 'n' 才一定不会出现在任何一段里，解包时取第一个 'n' 才是唯一分界点。
    // 放进来一个带 'n' 的会话ID，拼出来的串就有两个 'n'，还原时会拆错位置。
    // （会话建表那边也是按 INT 存的，这里卡住和库侧语义一致）
    if (!IsAllDigits(threadId))
    {
        return replyFail(400, "会话ID格式非法");
    }
    if (messageId.empty())
    {
        return replyFail(400, "消息ID不能为空");
    }
    // 消息ID 只可能来自服务器自己发的号（纯数字），
    // 这里卡一道既能挡住乱塞的内容，也能在将来落库时直接当整数用
    if (!IsAllDigits(messageId))
    {
        return replyFail(400, "消息ID格式非法");
    }
    if (prompt.empty())
    {
        return replyFail(400, "问题内容不能为空");
    }
    // 长度按"字"算而不是按字节：协议写的是 4000 字，
    // 按字节卡的话中文一千多字就会被误判超长
    if (Utf8Length(prompt) > 4000)
    {
        return replyFail(413, "问题内容过长");
    }

    AgentTask task;
    // 账号只认登录会话，请求里那个 userId 一概不看：
    // 否则改个包就能拿别人的身份往别人的会话里塞消息
    task.account   = session->account;
    task.threadId  = threadId;
    task.messageId = messageId;
    task.prompt    = prompt;
    // 存 weak_ptr：worker 只在推送时短暂持有，不会拖住会话对象的生命周期
    task.session   = session;

    // ===== ack 门 =====
    // 先造好闸门（初值 false）挂到任务上，worker 靠它判断"能不能开始推包"。
    // 为什么必须要有这个闸门：Submit 一返回 worker 就可能开始跑，而 ack 要等本函数
    // 返回后由 ClientWork 才写进 socket —— 两条路径没有任何先后保证。
    // mock 分支几微秒就出结果，几乎必然抢在 ack 前面，客户端就会先看到回答、后启动看门狗，
    // 甚至把这条回答当成"上一轮迟到包"直接丢掉。
    const auto ackSent = std::make_shared<std::atomic<bool>>(false);
    task.ackSent = ackSent;

    uint64_t requestId = 0;
    if (!agent_tcp::Instance().Submit(std::move(task), requestId))
    {
        // 队列已满 = 后端拒不住了，直接告诉客户端稍后再试（背压）。
        // 这里没提交成功，worker 永远不会来等这个闸门，所以直接丢掉就行
        return replyFail(503, "服务器繁忙，请稍后再试");
    }

    // 受理确认。协议里这一包"只需 type"——客户端收到它才启动
    // "5 分钟没有新包就自动收尾"的看门狗，所以只带成功标记，越简单越不会出错。
    // 顺序很重要：必须 Submit 成功后再回 ack。反过来客户端会为一个压根没排上队的
    // 请求启动看门狗，白等 5 分钟再自己发 agent_stop
    json ack;
    ack["type"]    = "agent_request_ack";
    ack["code"]    = 0;
    ack["message"] = "";
    ack["success"] = true;

    std::cout << "[agent] 已受理 requestId=" << requestId
              << " account=" << session->account
              << " threadId=" << threadId
              << " messageId=" << messageId << std::endl;

    // 这一包不走"返回字符串让 ClientWork 发"的老路，而是当场自己写 socket：
    // 只有 ack 真的落进 socket 之后才能放行 worker，而返回值要等本函数返回后
    // 才轮到 ClientWork 去发，中间隔着一次函数返回，放行时机就说不准了。
    // SendJson 内部持 sendMtx 并检查 closed，和 worker 的推送不会拼出半行 JSON。
    if (!session->SendJson(ack.dump() + "\n"))
    {
        std::cout << "[agent] ack 发送失败（连接可能已断）requestId=" << requestId << std::endl;
    }

    // 无论 ack 发没发出去都必须放行：连接断了客户端的看门狗也没意义了，
    // 但 worker 不能因为这一位没人置真而永远卡在闸门上白占一个线程
    ackSent->store(true, std::memory_order_release);

    // 返回空串 = 本条请求的应答已经自己发完了。
    // JsonParsing 见空串就不再组包，ClientWork 也不会重复发送
    return "";
}

// 提问第一步：向服务器申请一条消息ID。
// 客户端拿到 agent_message_created 里的 messageId 才本地落库，再带着它发 agent_request，
// 所以这个包失败时客户端不会继续提问——success=false 必须给出原因。
std::string HandleAgentMessageCreate(const json& req, mysqlconn&, SessionPtr session)
{
    // 这个包里它叫 sessionId，和 agent_request 里的 threadId 是同一个东西
    const std::string sessionId = req.value<std::string>("sessionId", "");

    // 未登录也走自己的组包（不能用 NotLoginResponse）：协议要求
    // agent_message_created 无论成败都回显 sessionId，缺了客户端就丢弃整个包
    if (!IsLoggedIn(session))
    {
        json rsp;
        rsp["type"]      = "agent_message_created";
        rsp["sessionId"] = sessionId;
        rsp["messageId"] = "";
        rsp["code"]      = 401;
        rsp["message"]   = "当前登录状态异常请重新登录";
        rsp["success"]   = false;
        return rsp.dump();
    }

    json rsp;
    rsp["type"] = "agent_message_created";

    // 会话ID 必须是纯数字：库里 thread_id 是 INT，而且发给智能体时
    // 要用它拼 "会话ID + 'n' + 账号"，非数字会让拼接串出现多个 'n' 拆不回来。
    // 卡在这一步（而不是等 agent_request 再报错）是因为此时客户端还没落库，
    // 失败成本最低
    if (sessionId.empty() || sessionId.size() > 10 || !IsAllDigits(sessionId))
    {
        // 失败也要回显 sessionId：客户端靠它匹配在飞的申请，缺了就整个包丢弃
        rsp["sessionId"] = sessionId;
        rsp["messageId"] = "";
        rsp["code"]      = 400;
        rsp["message"]   = "会话ID非法";
        rsp["success"]   = false;
        return rsp.dump();
    }

    // 这里不查库：会话ID 是客户端生成的，服务器只负责存储，
    // "会话还没建就先提问"是允许的，不该在这一步拦
    const std::string messageId = agent_tcp::AllocateMessageId();

    rsp["sessionId"] = sessionId;
    rsp["messageId"] = messageId;
    rsp["code"]      = 0;
    rsp["message"]   = "";
    rsp["success"]   = true;

    std::cout << "[agent] 分配消息ID account=" << session->account
              << " sessionId=" << sessionId
              << " messageId=" << messageId << std::endl;
    return rsp.dump();
}

namespace
{
    // 会话操作（create/rename/delete）的失败包。
    // 三件事必须同时成立：包类型对、op 对、sessionId 原样回显。
    // 少了 sessionId 客户端会直接丢弃这个包，用户看到的就是"操作转圈5秒后失败"
    std::string SessionFailResponse(const std::string& op, const std::string& sessionId,
                                    int code, const std::string& message)
    {
        json rsp;
        rsp["type"]      = "agent_session_response";
        rsp["op"]        = op;
        rsp["sessionId"] = sessionId;
        rsp["code"]      = code;
        rsp["message"]   = message;
        rsp["success"]   = false;
        return rsp.dump();
    }

    // 会话操作公共前置校验：会话ID 非空 + 名称按 UTF-8 字符数不超过 varchar(255)
    // 返回空串表示通过，否则就是要直接回给客户端的原因
    std::string CheckSessionArgs(const std::string& sessionId, const std::string& title)
    {
        if (sessionId.empty())
        {
            return "会话ID不能为空";
        }
        if (Utf8Length(title) > 255)
        {
            return "会话名称过长";
        }
        return "";
    }
}

// 中止当前会话正在进行（或还在排队）的回答。
// 协议规定这一包无回执：客户端发出即自行解锁，不等服务器应答；
// 服务器之后漏发过来的 agent_response 也会被客户端直接丢弃。
// 所以这里故意返回空串——JsonParsing 见到空串就不组包，ClientWork 也不会 send。
std::string HandleAgentStop(const json& req, mysqlconn&, SessionPtr session)
{
    const std::string threadId = req.value<std::string>("threadId", "");

    // 未登录或参数不全也照样不回包：这一条本来就没有应答，
    // 补一个 401 反而会让客户端收到一条它没在等的 agent_response
    if (!IsLoggedIn(session) || threadId.empty())
    {
        std::cout << "[agent_stop] 忽略：未登录或会话ID为空" << std::endl;
        return "";
    }

    // 账号取自登录会话，和 agent_request 入队时用的键是同一个，
    // 这样才能精确命中那条任务
    const bool taken = agent_tcp::Instance().Cancel(session->account, threadId);

    std::cout << "[agent_stop] account=" << session->account
              << " threadId=" << threadId
              << " 结果=" << (taken ? "已中止，worker 不再推送" : "无可中止任务") << std::endl;
    return "";
}

// 新建会话。userId 从请求里读的那份一律丢掉，只用登录会话里的账号：
// 否则谁都能往别人账号下塞会话
std::string HandleAgentSessionCreate(const json& req, mysqlconn& conn, SessionPtr session)
{
    const std::string sessionId = req.value<std::string>("sessionId", "");

    // 未登录也不能用 NotLoginResponse：那个包里没有 sessionId，
    // 而协议要求三件套必须原样回显 sessionId，缺了客户端会整个包丢弃，
    // 用户看到的就是"转圈 5 秒后操作失败"
    if (!IsLoggedIn(session))
    {
        return SessionFailResponse("create", sessionId, 401, "当前登录状态异常请重新登录");
    }

    const std::string title = req.value<std::string>("title", "");   // 空则业务层补"新会话"

    const std::string invalid = CheckSessionArgs(sessionId, title);
    if (!invalid.empty())
    {
        return SessionFailResponse("create", sessionId, 400, invalid);
    }

    uint32_t userId = 0;
    if (!ParseUint32(session->account, userId))
    {
        return SessionFailResponse("create", sessionId, 401, "当前登录状态异常请重新登录");
    }

    json rsp;
    CreateAgentSession(rsp, userId, sessionId, title, conn);
    return rsp.dump();
}

std::string HandleAgentSessionRename(const json& req, mysqlconn& conn, SessionPtr session)
{
    const std::string sessionId = req.value<std::string>("sessionId", "");

    // 同上：未登录也要回显 sessionId，否则客户端匹配不上这条在飞请求
    if (!IsLoggedIn(session))
    {
        return SessionFailResponse("rename", sessionId, 401, "当前登录状态异常请重新登录");
    }

    const std::string title = req.value<std::string>("title", "");

    const std::string invalid = CheckSessionArgs(sessionId, title);
    if (!invalid.empty())
    {
        return SessionFailResponse("rename", sessionId, 400, invalid);
    }

    // 重命名必须真的给个名字，否则会把会话改成"新会话"，等于把用户起的名字弄丢了
    if (title.empty())
    {
        return SessionFailResponse("rename", sessionId, 400, "会话名称不能为空");
    }

    uint32_t userId = 0;
    if (!ParseUint32(session->account, userId))
    {
        return SessionFailResponse("rename", sessionId, 401, "当前登录状态异常请重新登录");
    }

    json rsp;
    RenameAgentSession(rsp, userId, sessionId, title, conn);
    return rsp.dump();
}

std::string HandleAgentSessionDelete(const json& req, mysqlconn& conn, SessionPtr session)
{
    const std::string sessionId = req.value<std::string>("sessionId", "");

    // 同上：未登录也要回显 sessionId
    if (!IsLoggedIn(session))
    {
        return SessionFailResponse("delete", sessionId, 401, "当前登录状态异常请重新登录");
    }

    if (sessionId.empty())
    {
        return SessionFailResponse("delete", sessionId, 400, "会话ID不能为空");
    }

    uint32_t userId = 0;
    if (!ParseUint32(session->account, userId))
    {
        return SessionFailResponse("delete", sessionId, 401, "当前登录状态异常请重新登录");
    }

    json rsp;
    DeleteAgentSession(rsp, userId, sessionId, conn);
    return rsp.dump();
}

// 拉取全部会话列表。这一份是客户端判断"服务器上到底有哪些会话"的唯一依据，
// 所以失败时必须明确 success=false，不能回空列表（否则客户端会把本地会话清空）
std::string HandleAgentSessionsList(const json& req, mysqlconn& conn, SessionPtr session)
{
    if (!IsLoggedIn(session))
    {
        return NotLoginResponse("agent_sessions_list");
    }

    uint32_t userId = 0;
    if (!ParseUint32(session->account, userId))
    {
        json rsp;
        rsp["type"]    = "agent_sessions_list";
        rsp["code"]    = 401;
        rsp["message"] = "当前登录状态异常请重新登录";
        rsp["success"] = false;
        return rsp.dump();
    }

    json rsp;
    LoadAgentSessions(rsp, userId, conn);
    return rsp.dump();
}
