#ifndef UNTITLED_AGENT_TCP_H
#define UNTITLED_AGENT_TCP_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
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
    uint64_t    requestId = 0;      // 服务器内部编号，只用于日志排查（客户端ID一律由客户端生成）
    std::string account;            // 发起人账号，打日志用，也是取消键的一半
    std::string threadId;           // 客户端生成的会话ID，原样回显；也是取消键的另一半
    // 正式消息ID：由服务器在 agent_message_create 里分配，客户端先本地落库、
    // 再带着它发 agent_request。服务器只负责原样带回（agent_response 里的 serverId），
    // 不参与任何判断，也不解析它的内容。
    // 为什么不让客户端自己生成：客户端的ID没法保证跨设备/重装后不重复，
    // 服务器分配才能保证全局单调，万一以后要做分页或去重，才有依据。
    std::string messageId;
    std::string prompt;             // 用户问题
    std::weak_ptr<Session> session; // 结果往哪儿推

    // 取消标志：agent_tcp::Cancel 把它置真之后，
    // 在排队的任务直接丢弃，在跑的任务由 curl 进度回调打断。
    // 用 shared_ptr 而不是裸成员，是因为任务在队列里和在途表里各有一份副本，
    // 必须是同一个对象才能共享同一个取消位。
    std::shared_ptr<std::atomic<bool>> cancelled;

    // ===== ack 门（新增）=====
    // agent_request_ack 是客户端启动「5 分钟静默看门狗」的开关，必须先于本轮任何
    // agent_response 抵达。但这两条包是两条独立路径发出去的：
    //     ack      —— 客户端线程里 HandleAgentRequest 自己写 socket
    //     response —— worker 线程自己写 socket
    // worker 一旦比 handle 那条路径跑得快（mock 尤其快，几微秒就出结果），
    // 客户端就会先收到回答、后启动看门狗，甚至把这条回答当成“上一轮迟到包”丢掉。
    // 所以 worker 在推第一包之前必须等这一位变成 true。
    // 为什么用 shared_ptr 而不是裸 atomic<bool>：
    //   1) 任务在队列和 m_inFlight 里各有一份副本，必须是同一个对象才能共享同一个位；
    //   2) 裸 atomic 不可拷贝，会把 AgentTask 变成不可拷贝类型，而现有容器都靠拷贝入表。
    std::shared_ptr<std::atomic<bool>> ackSent;
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

    // 取消某条会话正在进行的回答（对应 agent_stop）。
    // 返回 true 表示确实找到了排队中/在途的那一轮，并已置上取消位。
    //
    // 为什么键是 (account, threadId) 而不是 requestId：
    //   会话ID由客户端生成，客户端只知道自己那条会话，不知道服务器的内部编号；
    //   协议又约定同一会话同一时刻只有一轮回答，所以这两个字段足以唯一定位。
    // 为什么只能取消"自己的"：account 只从登录会话取，客户端传的 userId 一概不采信，
    //   否则任何人拿一个会话ID就能打断别人的回答。
    bool Cancel(const std::string& account, const std::string& threadId);

    // 分配一个正式消息ID（对应 agent_message_create）。
    // 形如“13 位毫秒时间戳 + 3 位序号”，长度固定 16 位、全字符：
    //   客户端要拿它当主键落库，服务器要拿它上线排序、排查日志，两家都靠它对应同一行。
    // 用时间戳前缀而不是纯自增，是为了让ID本身带上“先后关系”（会话内直接按字符串排就是消息顺序），
    // 重启后也不会从0重来、和旧消息撞号。
    // 后面拼序号是为了防同一毫秒并发分配时重复。
    static std::string AllocateMessageId();

    // ===== 智能体侧 thread_id 的拼装与还原 =====
    // 分隔符就一个字母 'n'。为什么敢用这么普通的字符：
    //   两边的值（会话ID、账号）都是纯数字，数字串里永远不会出现 'n'，
    //   所以第一个 'n' 就是唯一分界点，既不用转义，也不用扫完整串。
    // 拼和拆必须用同一个常量，别在两处各写一个字面量
    static constexpr char kAgentThreadSep = 'n';

    // 把“客户端会话ID + 账号”拼成智能体侧唯一的 thread_id。
    // 为什么必须拼：langchain 的 thread 是**全局唯一**的，
    // 而我们的会话ID 只是“账号内的编号”（客户端自己生成，联合主键的一部分）。
    // 不拼的话，甲用户的会话 1 和乙用户的会话 1 在 langchain 眼里是同一条 thread，
    // 上下文会串到别人的对话里 —— 既是数据泄漏，也是答非所问。
    static std::string MakeAgentThreadId(const std::string& threadId, const std::string& userId);

    // 上面那个函数的逆运算：从智能体回传的 thread_id 里拆回 (会话ID, 账号)。
    // 拆失败（没分隔符 / 分隔符在首尾）返回 false，两个出参保持原样不动。
    static bool SplitAgentThreadId(const std::string& agentThreadId,
                                   std::string& threadId, std::string& userId);

    // 真正和智能体通信的入口：一次 HTTP POST + 读完整个 SSE 响应。
    // 成功返回拼好的完整回答；失败返回空串，失败原因写进 outError。
    //
    // 注意字段映射：智能体服务（FastAPI）的请求体是 {"message","thread_id"}，
    // 其中 thread_id 是 langchain 那边的**全局唯一**线程标识，为空则由智能体新开一个。
    // ❗所以本参数不能直接传客户端的会话ID，必须传 MakeAgentThreadId() 拼好的值，
    //   否则不同用户的同名会话会在智能体那边变成同一条对话线。
    // 若 outThreadId 非空，会把智能体回传的 thread_id 带出来（做上下文续接时要用）。
    //
    // cancel 非空时注册 curl 进度回调：回调一旦读到取消位就返回非0，
    // curl_easy_perform 立刻以 CURLE_ABORTED_BY_CALLBACK 结束。
    // 这是唯一能提前打断"已经连上、正在收流"的请求的手段（超时只能干等）。
    static std::string CallAgent(const std::string& prompt,
                                 const std::string& threadId,
                                 const AgentEndpoint& endpoint,
                                 std::string& outError,
                                 std::string* outThreadId = nullptr,
                                 const std::atomic<bool>* cancel = nullptr);

    // 纯本地模拟，不依赖网络。压测队列 / 智能体还没起来时用
    static std::string MockAgent(const std::string& prompt, const std::string& agentId);

private:
    agent_tcp() = default;
    ~agent_tcp();
    agent_tcp(const agent_tcp&) = delete;
    agent_tcp& operator=(const agent_tcp&) = delete;

    // worker 循环：取任务 -> 调智能体 -> 推送 -> 出在途表
    void WorkerLoop();

    // 取消键：账号和会话ID拼成一个键，避免为两级键单独写哈希
    static std::string CancelKey(const std::string& account, const std::string& threadId);

    std::mutex              m_mtx;
    std::condition_variable m_cv;// 条件变量：队列空时 worker 睡在上面，Submit 时 notify_one 唤醒一个  
    //条件变量不能单独用，强制绑定一个 mutex；它本身不保存信号：提供内核级、低开销的线程间事件通知原语，用来解决「线程等待某个共享状态满足，直到别的线程修改状态并通知它」这个经典生产者 - 消费者同步问题。
    std::deque<AgentTask>   m_queue;                    // 进任务队列（有界 FIFO）
    std::unordered_map<uint64_t, AgentTask> m_inFlight; // 在途表：已取出、未完成
    // 取消表：(账号|会话ID) -> 该轮的取消标志。Submit 登记，worker 收尾时摘除
    std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>> m_cancel;
    std::vector<std::thread> m_workers;
    std::atomic<bool>       m_running{false};//原子变量：对这个变量的读、写操作，不会出现 “半写、撕裂”，CPU 保证这一次操作是不可分割的，要么做完，要么完全没做，解决的就是cpu缓存问题
    std::atomic<uint64_t>   m_nextId{1};                // 请求编号发号器（1 起，0 作无效值）

    // 智能体地址配置：只在启动前写一次，worker 取任务时读一份副本，
    // 所以用 m_mtx 保护就够，不必为它单独加锁
    AgentEndpoint           m_endpoint;
};

#endif //UNTITLED_AGENT_TCP_H