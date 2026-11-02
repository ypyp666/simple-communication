#include "OnlineSessionManager.h"

#include <random>
#include <iostream>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
    // 生成一个16位的随机临时令牌：
    // 字符集取 0-9 a-z A-Z 共62个（不含容易混淆的符号，方便客户端当成普通字符串用），
    // 用 uniform_int_distribution 而不是 %62，保证每个字符等概率、不出现取模偏移。
    std::string GenerateToken16()
    {
        static const char kAlphabet[] =
            "0123456789"
            "abcdefghijklmnopqrstuvwxyz"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ";// 62 个有效字符，末尾的 '\0' 不参与

        std::random_device rd;// Linux 下取 /dev/urandom，是系统级真随机源
        std::mt19937 gen(rd());
        std::uniform_int_distribution<std::size_t> dist(0, sizeof(kAlphabet) - 2);// 62个字符，下标 0~61

        std::string token;
        token.reserve(16);
        for (int i = 0; i < 16; ++i)
        {
            token.push_back(kAlphabet[dist(gen)]);
        }
        return token;
    }
}

OnlineSessionManager& OnlineSessionManager::Instance()
{
    // C++11及以上线程安全单例
    static OnlineSessionManager manager;
    return manager;
}

// Session 的统一发送出口。
// 注意：这里必须"先拿锁再判 closed"，不能先判后拿锁——
// 否则判完 closed 为 false、还没拿到锁的瞬间，客户端线程可能已经 close(fd) 了。
bool Session::SendJson(const std::string& data)
{
    std::lock_guard<std::mutex> lock(sendMtx);

    if (closed || fd < 0)
    {
        return false;
    }

    std::size_t sent = 0;
    while (sent < data.size())
    {
        // MSG_NOSIGNAL：对端已关闭时返回错误而不是触发 SIGPIPE 把整个进程干掉
        const ssize_t size = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (size < 0)
        {
            if (errno == EINTR)
            {
                continue;   // 被信号打断，不是断线，重试
            }
            closed = true;
            return false;
        }
        if (size == 0)
        {
            closed = true;
            return false;
        }
        sent += static_cast<std::size_t>(size);
    }
    return true;
}

void OnlineSessionManager::addSession(const std::string& account, SessionPtr sess)
{
    // 被顶替的旧会话先存一份：通知必须在锁外发，见下面注释
    SessionPtr kicked;

    {
        std::lock_guard<std::mutex> lock(m_mtx);//构造对象 lock 的瞬间：调用 m_mtx.lock()
        /*如果此时锁是空闲：把 m_mtx 设置为【已锁定】，当前线程拿到锁，代码继续往下跑。
        如果锁已经被别的线程拿着：当前线程直接阻塞在这里，原地等待，直到别的线程解锁。
        花括号 { } 就是锁的作用域。只要还在这个大括号里面，锁就保持持有。*/

        // 同一账号只允许一个有效会话：新连接登录/重连成功时，把旧连接的登录态直接作废。
        // 之所以不直接 erase 旧项：断线重连时旧连接的线程往往还没退出，
        // 它会在自己的收尾处调 removeSession，把新会话误删（详见 removeSession 重载）。
        // 这里只把旧会话的 login_state 置回 false，旧连接再发任何业务都会被拦下。
        auto old = m_onlineMap.find(account);
        if (old != m_onlineMap.end() && old->second && old->second != sess)
        {
            old->second->login_state = false;
            // 只有还活着的连接才值得通知：已经断开的没必要再写一次
            if (!old->second->closed && old->second->fd >= 0)
            {
                kicked = old->second;
            }
            std::cout << "[会话] 账号" << account << " 在新连接登录，旧连接的登录态已作废" << std::endl;
        }

        m_onlineMap[account] = std::move(sess);
        //如果容器里没有这个 account 键：operator[] 会就地默认构造一个空的 value 对象插入 map；
    //步骤：
    //1. m_onlineMap[account] → 找不到key就插入一个默认Session，拿到引用
    //2. std::move(sess) 将sess转为右值
    //3. 调用map里面那个元素的移动赋值，把sess内部资源（句柄、buffer、指针）掠夺过来
    //4. 原来局部变量sess变成被移出后的空有效状态，不要再使用sess
    /*move的作用：资源所有权转移，而不是复制资源。网络 Session 这种对象，本来就只应该有一个实例。如果不用move就会直接拷贝复制*/
    }

    // 通知必须在锁外发。send 在对端接收窗口满时会阻塞（本服务没设 SO_SNDTIMEO），
    // 若在 m_mtx 里做，一个卡死的旧连接就能把全服所有账号的登录/重连全部堵在这里。
    //
    // 只发通知、不在这里 close(fd)：那个 fd 归旧连接的 ClientWork 线程所有，
    // 别的线程去关会和它的 close 撞车（fd 号被复用后会误关新连接）。
    // 旧连接收到这条包后由它自己决定退出，服务端这边 login_state 已置 false，它再发什么都不放行。
    if (kicked)
    {
        static const char kKickedJson[] =
            R"({"type":"kicked","code":409,"message":"该账号已在其他设备登录，当前连接已下线","success":false})"
            "\n";   // 协议要求每条消息以 '\n' 结尾
        if (kicked->SendJson(kKickedJson))
        {
            std::cout << "[会话] 账号" << account << " 的旧连接已收到下线通知" << std::endl;
        }
        else
        {
            std::cout << "[会话] 账号" << account << " 的旧连接已不可写，通知未发出" << std::endl;
        }
    }
}

SessionPtr OnlineSessionManager::findSession(const std::string& account)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    auto it = m_onlineMap.find(account);
    if (it != m_onlineMap.end())
    {
        return it->second;
    }
    return nullptr;
}

void OnlineSessionManager::removeSession(const std::string& account)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    m_onlineMap.erase(account);
}

void OnlineSessionManager::removeSession(const std::string& account, const SessionPtr& sess)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    auto it = m_onlineMap.find(account);
    // 只有字典里存的确实还是这个连接时才删。
    // 若已经不是了，说明该账号已经在新连接上重登/重连成功，
    // 这里碍于旧线程收尾就无条件按账号删，会把新会话误伤
    if (it != m_onlineMap.end() && it->second == sess)
    {
        m_onlineMap.erase(it);
    }
}

bool OnlineSessionManager::isCurrentSession(const std::string& account, const SessionPtr& sess)
{
    if (account.empty() || !sess)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mtx);
    auto it = m_onlineMap.find(account);
    // 双重确认：账号在字典里，且登记的就是这个连接
    return it != m_onlineMap.end() && it->second == sess;
}

void OnlineSessionManager::removeToken(const std::string& account)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    m_tokenMap.erase(account);
}

std::string OnlineSessionManager::issueNewToken(const std::string& account)
{
    const std::string token = GenerateToken16();
    std::lock_guard<std::mutex> lock(m_mtx);
    // 键存在就覆盖（旧令牌立即作废），不存在就新建：operator[] 恰好就是这个语义
    m_tokenMap[account] = token;
    return token;
}

std::string OnlineSessionManager::reuseOrIssueToken(const std::string& account)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    auto it = m_tokenMap.find(account);
    if (it != m_tokenMap.end() && !it->second.empty())
    {
        // 断线重连：沿用已下发的令牌，保证客户端手里那份仍然有效
        return it->second;
    }
    // 服务端没有该账号的令牌（例如服务重启过、或从未正常登录过）才新建
    const std::string token = GenerateToken16();
    m_tokenMap[account] = token;
    return token;
}

std::string OnlineSessionManager::findToken(const std::string& account)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    auto it = m_tokenMap.find(account);
    if (it != m_tokenMap.end())
    {
        return it->second;
    }
    return std::string();
}
