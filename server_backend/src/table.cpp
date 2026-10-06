#include "table.h"
#include"FunctionalFunction.h"
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
