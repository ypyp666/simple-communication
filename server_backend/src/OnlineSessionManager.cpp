#include "OnlineSessionManager.h"

#include <random>
#include <iostream>

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

void OnlineSessionManager::addSession(const std::string& account, SessionPtr sess)
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
