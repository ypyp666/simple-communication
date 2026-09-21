#include "OnlineSessionManager.h"

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
