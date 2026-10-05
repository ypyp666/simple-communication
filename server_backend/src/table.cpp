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
}

CmdType JsonToCmdType(std::string type) {
    if (type=="login") return CmdType::Login;
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

std::string HandleModifyNam(const json& req, mysqlconn& conn, SessionPtr session)
{
    json rsp;
    rsp["type"] = "modify_name_response";

    if (!session->state)
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

    if (!session->state)
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

    if (!session->state)
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

    if (!session->state)
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

    if (!session->state)
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

    if (!session->state)
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

    if (!session->state)
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

    if (!session->state)
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

    if (!session->state)
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

    if (!session->state)
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

    if (!session->state)
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
