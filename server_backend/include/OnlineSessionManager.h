#pragma once
#include <mutex>
#include <unordered_map>
#include <string>
#include <memory>

struct Session
{
    std::string account;    // 登录账号，未登录为空
    int fd = -1;            // socket文件描述符
    std::string clientIp;   // 客户端IP地址
    // 登录态：只有登录成功才会置true。
    // 除登录/断线重连/注册/获取账号/修改密码外，其余业务一律先看它。
    // 同账号被新连接顶替时，旧连接的它会立即被置回false。
    bool login_state=false;
};
using SessionPtr = std::shared_ptr<Session>;

class OnlineSessionManager {
    public:
    static OnlineSessionManager& Instance();

    OnlineSessionManager(const OnlineSessionManager&) = delete;
    OnlineSessionManager& operator=(const OnlineSessionManager&) = delete;
    // 添加在线会话（登录成功调用）
    void addSession(const std::string& account, SessionPtr sess);

    // 根据账号查找会话（发消息时用）
    SessionPtr findSession(const std::string& account);

    // 用户下线，移除会话
    void removeSession(const std::string& account);

    // 带连接校验的下线移除：只有字典里该账号当前登记的会话就是 sess 时才移除。
    // 断线重连时旧线程的退出往往晚于新连接登记，若照旧按账号直接删，
    // 会把刚重连上来的新会话一并删掉，用户就会"刚连上就掉登录态"。
    void removeSession(const std::string& account, const SessionPtr& sess);

    // 该账号当前登记的会话是否就是 sess：登录态校验的第二道关，
    // 用来识别"同账号已被新连接顶替后，旧连接还要拿老会话继续操作"
    bool isCurrentSession(const std::string& account, const SessionPtr& sess);

    // ===== 临时令牌（16位随机，键=账号id，值=令牌）=====
    // 正常登录：生成一个新的16位随机令牌并落表；键存在就覆盖（旧令牌立即作废），不存在就新建
    std::string issueNewToken(const std::string& account);

    // 断线重连：能沿用就沿用已下发的令牌（重连不该把客户端手里的令牌作废），
    // 只有服务端没有该账号令牌时（例如服务重启过）才新建
    std::string reuseOrIssueToken(const std::string& account);

    // 查询账号当前令牌，不存在返回空串（重连时拿客户端带来的令牌逐字比对用）
    std::string findToken(const std::string& account);

    // 清除账号的临时令牌（登出时调用，避免旧令牌还能被拿来重连）
    void removeToken(const std::string& account);

    ~OnlineSessionManager()=default;

    private:
    OnlineSessionManager()=default;

    std::mutex m_mtx;//互斥锁对象，一个 mutex 同一时间只允许一个线程拿到锁；别的线程再来拿锁，就会卡住阻塞，直到别人释放锁。
    // key:账号 value:会话智能指针
    std::unordered_map<std::string, SessionPtr> m_onlineMap;
    // key:账号id value:16位临时令牌（登录成功后下发，断线重连时沿用）
    std::unordered_map<std::string, std::string> m_tokenMap;

};