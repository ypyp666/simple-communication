#ifndef UNTITLED_AGENT_TCP_H
#define UNTITLED_AGENT_TCP_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "OnlineSessionManager.h"

// ===== 一次智能体请求 =====
// 基础版：一问一答（非流式），先把「队列 + 线程池 + 安全推送」这条管子跑通。
//
// 队列里为什么不直接存 fd，而是存 weak_ptr<Session>：
//   fd 是内核循环复用的编号。用户断线后 close(fd)，这个号码马上会被分配给新连接，
//   worker 拿着旧号码推送就会把 A 的答案发给 B。
//   用 weak_ptr 则可以同时拿到最新的 fd、账号和 closed 状态，而且不会阻止会话对象释放。
struct AgentTask
{
    uint64_t    requestId = 0;      // 请求编号，回包原样带回（相当于心跳的 examineId）
    std::string account;            // 发起人账号，打日志用
    std::string agentId;            // 智能体标识，预留「一个用户对应多个智能体会话」
    std::string prompt;             // 用户问题
    std::weak_ptr<Session> session; // 结果往哪儿推
};

// ===== 智能体服务端地址 / 超时配置 =====
// 之所以做成可配置，而不是写死一个常量：
//   本地调试、内网部署、正式环境三处的地址一定不一样，
//   写死就得改代码重编，改配置只需要在 main 里调一次 Configure。
struct AgentEndpoint
{
    // 智能体进程的 HTTP 接口。留空则自动降级为本地模拟（不连网也能跑通整条链路）
    std::string url = "http://127.0.0.1:8000/chat";

    // 连接超时：智能体进程挂掉/机器不可达时，别让 worker 卡在 TCP 握手上
    long connectTimeoutSec = 5;

    // 低速超时（主检测手段）：连续 lowSpeedTimeSec 秒内平均速率低于
    // lowSpeedLimitBytes 字节/秒就判定智能体卡死并中止。
    // 取 30 秒而不是 10 秒：这个计时从传输开始就算，覆盖了首 token 的等待时间，
    // 冷启动加载模型 / RAG 检索 / reasoning 模型都可能十几秒不出字。
    long lowSpeedLimitBytes = 1;
    long lowSpeedTimeSec    = 30;

    // 总超时（兜底，不是主检测手段）：从发请求到收完响应的绝对上限。
    // 低速超时只能发现"完全卡死"，发现不了"每 29 秒吐一个字节"的细水长流，
    // 所以要留一个正常回答不可能超过的值，防止这种请求永久占住 worker。
    // 300 秒已经是极宽松的上限，正常长回答几轮都够用。
    long totalTimeoutSec = 300;

    // 强制本地模拟（不真的发 HTTP），用于压测队列和线程池本身
    bool mock = false;
};

// ===== 智能体调用模块 =====
// 只负责：收任务 -> 丢进有界队列 -> 固定几个 worker 取出来干活 -> 把结果推回连接。
// 完全独立于 TcpServer，连接层只多了一个「提交任务」的调用点。
class agent_tcp
{
public:
    // 默认 worker 数量：同时最多能跑几路智能体调用，受 MySQL 连接数上限制约
    static constexpr int    kDefaultWorkers = 3;
    // 进任务队列上限：满了就拒绝，这就是最朴素的背压
    static constexpr size_t kMaxQueueSize   = 100;

    static agent_tcp& Instance();

    // 修改智能体地址与超时（启动前调一次即可）
    void Configure(const AgentEndpoint& endpoint);

    // 启动线程池（幂等，重复调用无副作用）
    void Start(int workerCount = kDefaultWorkers);

    // 停止：唤醒所有 worker 并等它们退出
    void Stop();

    // 提交一个任务。
    // 队列满返回 false —— 调用方直接回 503，
    // 不设上限的话智能体一慢，内存和延迟会一起失控。
    bool Submit(AgentTask task, uint64_t& outRequestId);

    // 真正和智能体通信的入口：一次 HTTP POST + 读完整个 SSE 响应。
    // 成功返回拼好的完整回答；失败返回空串，失败原因写进 outError。
    //
    // 注意字段映射：智能体服务（FastAPI）的请求体是 {"message","thread_id"}，
    // 其中 thread_id 就是"同一个会话"的标识，为空则由智能体新开一个。
    // 所以本参数 threadId 对应客户端的 agentId —— 一个智能体、多条会话线。
    // 若 outThreadId 非空，会把智能体新分配的 thread_id 带出来（第三步做持久化时要用）。
    static std::string CallAgent(const std::string& prompt,
                                 const std::string& threadId,
                                 const AgentEndpoint& endpoint,
                                 std::string& outError,
                                 std::string* outThreadId = nullptr);

    // 纯本地模拟，不依赖网络。压测队列 / 智能体还没起来时用
    static std::string MockAgent(const std::string& prompt, const std::string& agentId);

private:
    agent_tcp() = default;
    ~agent_tcp();
    agent_tcp(const agent_tcp&) = delete;
    agent_tcp& operator=(const agent_tcp&) = delete;

    // worker 循环：取任务 -> 调智能体 -> 推送 -> 出在途表
    void WorkerLoop();

    std::mutex              m_mtx;
    std::condition_variable m_cv;// 条件变量：队列空时 worker 睡在上面，Submit 时 notify_one 唤醒一个  
    //条件变量不能单独用，强制绑定一个 mutex；它本身不保存信号：提供内核级、低开销的线程间事件通知原语，用来解决「线程等待某个共享状态满足，直到别的线程修改状态并通知它」这个经典生产者 - 消费者同步问题。
    std::deque<AgentTask>   m_queue;                    // 进任务队列（有界 FIFO）
    std::unordered_map<uint64_t, AgentTask> m_inFlight; // 在途表：已取出、未完成
    std::vector<std::thread> m_workers;
    std::atomic<bool>       m_running{false};//原子变量：对这个变量的读、写操作，不会出现 “半写、撕裂”，CPU 保证这一次操作是不可分割的，要么做完，要么完全没做，解决的就是cpu缓存问题
    std::atomic<uint64_t>   m_nextId{1};                // 请求编号发号器（1 起，0 作无效值）

    // 智能体地址配置：只在启动前写一次，worker 取任务时读一份副本，
    // 所以用 m_mtx 保护就够，不必为它单独加锁
    AgentEndpoint           m_endpoint;
};

#endif //UNTITLED_AGENT_TCP_H