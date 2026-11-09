#ifndef UNTITLED_AGENT_TCP_H
#define UNTITLED_AGENT_TCP_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "OnlineSessionManager.h"

// ===== 流式增量回调 =====
// 智能体每吐出一段文字（SSE 的 token 事件）就调一次，参数是这一段增量文本。
// 返回 true  = 继续接收；
// 返回 false = 立刻中止本次传输（客户端已经断线，或者用户刚点了停止）。
//
// 用 std::function 而不是模板参数，是为了让 CallAgent 继续当普通静态成员函数、
// 定义留在 .cpp 里，不用把 curl 的头文件污染到每个调用方。
using AgentDeltaSink = std::function<bool(const std::string&)>;

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
    // 正式消息ID：由服务器在 agent_message_create 里调 Create_agent_message 分配，
    // 就是 agent_message.message_id（自增主键）。客户端先按它本地落库，
    // 再带着它发 agent_request；服务器收到 agent_request 时按这个号写入用户提问。
    // 为什么不能让客户端自己生成、也不能在内存里造号：
    //   本地库和服务器库要靠这一个号对上同一行，两边各生成一套就永远对不上，
    //   用户提问也就没法落库（没有那一行可写）。
    // 智能体的回答不入库：服务器不做消息持久化，回答只实时推给客户端。
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

    // ===== HITL 审批续跑（新增）=====
    // 为 true 时这一轮不是「新提问」，而是把用户对上一轮审批的决定交回智能体，
    // 走的是 /approve 而不是 /chat，prompt 字段这一轮不用。
    // 为什么把两种请求塞进同一个任务结构、同一个队列：
    //   续跑同样是一次几百毫秒到几十秒的 HTTP 长连接，同样要占一个 worker、
    //   同样要受队列限流和 agent_stop 约束。新开一套线程池只会多出一份同样的代码。
    bool        approveResume = false;
    bool        approved      = false;
    std::string approvalNote;        // 用户拒绝时可能带一句理由，批准时为空

    // 智能体确认过的 thread_id。续跑必须原样交回去，不能重新拼：
    // 智能体可能在自己的那份上加过后缀，重新拼一个就对不上原来那条对话线，
    // 挂在 interrupt() 上的状态也就找不回来了。
    std::string agentThreadId;
};

// ===== 一次审批请求（HITL）=====
// 智能体跑到需要用户点头才能往下走的那一步，会先发 approval_required，
// 然后这一轮就停在 interrupt() 上。服务器不能替用户决定，
// 所以必须把这一轮挂起、把内容原样转给客户端，等客户端回执了再续跑。
//
// 这里只搬字段、不做任何解释：summary / steps / command 具体长什么样由
// table 层决定怎么组包，agent_tcp 不去理解业务语义。
struct AgentApproval
{
    std::string threadId;   // 智能体侧的 thread_id（“会话ID + n + 账号”），续跑时原样交回去
    std::string summary;    // 要用户确认的那句话
    std::string steps;      // JSON 数组原文；智能体没给就是 "[]"
    std::string command;    // JSON 对象原文；智能体没给就是 "{}"
    // 「给谁、做什么」的两列表，形如 [{"label":"对象","value":"老李（账号 1001）"}]。
    // 为什么单独一个字段而不是让客户端从 command 里拼：command 里只有数字账号，
    // 人名是智能体才有的信息（它查过好友表）。客户端拿不到名字就只能给用户看
    // 一串数字，那审批框等于没告诉他发给谁。
    std::string detail;     // JSON 数组原文；智能体没给就是 "[]"
};

// 审批回调：agent_tcp 解析到 approval_required 时调一次，
// 由上层负责“记挂起表 + 推给客户端”。
// 返回 false 表示上层不再需要（客户端已断线），本次传输可以提前收尾。
using AgentApprovalSink = std::function<bool(const AgentApproval&)>;

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
    // 注意字段映射：智能体服务（FastAPI）的请求体是 {"message","user_id","thread_id"}，
    // 其中 thread_id 是 langchain 那边的**全局唯一**线程标识，为空则由智能体新开一个。
    // ❗所以 threadId 参数不能直接传客户端的会话ID，必须传 MakeAgentThreadId() 拼好的值，
    //   否则不同用户的同名会话会在智能体那边变成同一条对话线。
    // userId 是当前登录账号（只取自登录会话），明着带一份给智能体做个性化用，
    //   省得它去猜 threadId 里那个分隔符的语义。
    // 若 outThreadId 非空，会把智能体回传的 thread_id 带出来（做上下文续接时要用）。
    //
    // cancel 非空时注册 curl 进度回调：回调一旦读到取消位就返回非0，
    // curl_easy_perform 立刻以 CURLE_ABORTED_BY_CALLBACK 结束。
    // 这是唯一能提前打断"已经连上、正在收流"的请求的手段（超时只能干等）。
    //
    // onDelta 非空时走真流式：每解析出一段 token 就当场回调一次，由调用方立刻推给客户端，
    // 不再等整个流收完。onDelta 返回 false 会让本次传输提前结束（客户端断线 / 用户停止）。
    // 返回值仍然把完整回答拼好返回，方便日志和"非流式接口"的兜底。
    static std::string CallAgent(const std::string& prompt,
                                 const std::string& threadId,
                                 const std::string& userId,
                                 const AgentEndpoint& endpoint,
                                 std::string& outError,
                                 std::string* outThreadId = nullptr,
                                 const std::atomic<bool>* cancel = nullptr,
                                 const AgentDeltaSink& onDelta = nullptr,
                                 const AgentApprovalSink& onApproval = nullptr);

    // 审批后续跑：把用户对上一轮 approval_required 的决定交回智能体。
    // 对应的 HTTP 入口不是 /chat 而是 /approve（地址从 endpoint.url 推出来）。
    //
    // agentThreadId 必须是**智能体在 approval_required 里回传的那个**，
    // 不能拿客户端会话ID 重新拼：interrupt() 的状态是挂在那一条 thread 上的，
    // 换一个 thread_id 就等于换了一条对话线，图里根本没有可恢复的中断点。
    //
    // 其余参数（onDelta / cancel / onApproval）和 CallAgent 语义完全一致：
    // 批准后的执行段同样会吐 token，同样可能被 agent_stop 打断，
    // 也理论上可能再次要审批（多步任务），所以三条通道都得留着。
    static std::string CallAgentApprove(const std::string& agentThreadId,
                                        const std::string& userId,
                                        bool approved,
                                        const std::string& note,
                                        const AgentEndpoint& endpoint,
                                        std::string& outError,
                                        std::string* outThreadId = nullptr,
                                        const std::atomic<bool>* cancel = nullptr,
                                        const AgentDeltaSink& onDelta = nullptr,
                                        const AgentApprovalSink& onApproval = nullptr);

    // 提交一轮「审批后续跑」。
    // 挂起表里找不到就返回 false —— 说明这条会话没有待审批的任务
    // （客户端重复点了、或者服务重启把内存里的挂起记录丢了），
    // 调用方据此回一个明确的错误，不要默默排队。
    //
    // ackSent 和 Submit 的同名字段是一回事：客户端收到 agent_approve_ack 才启动看门狗，
    // 必须先于任何 agent_response 抵达，所以闸门在外层造、这里只顺手挂上。
    bool SubmitApprove(const std::string& account,
                       const std::string& threadId,
                       const std::string& messageId,
                       bool approved,
                       const std::string& note,
                       const SessionPtr& session,
                       const std::shared_ptr<std::atomic<bool>>& ackSent,
                       uint64_t& outRequestId);

    // 这条会话当前是不是停在审批点上（客户端重连后想恢复审批框时用）。
    // 只读查询，查不到一律当「没有」，不抛异常。
    bool HasPendingApproval(const std::string& account, const std::string& threadId);

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

    // ===== 两个 HTTP 入口共用的那一段（新增）=====
    // /chat 和 /approve 的请求体不同、地址不同，但“建 handle -> 设回调 -> 设超时 ->
    // 边收边解析 -> 收尾”这整段是一模一样的。
    // 抽出来纯粹是为了不把同一段 curl 代码拄两遍（拄两遍的结果一定是只改一边），
    // 行为与原来 CallAgent 里的实现逐行一致。
    static std::string PostAgentStream(const std::string& url,
                                       const std::string& bodyText,
                                       const AgentEndpoint& endpoint,
                                       std::string& outError,
                                       std::string* outThreadId,
                                       const std::atomic<bool>* cancel,
                                       const AgentDeltaSink& onDelta,
                                       const AgentApprovalSink& onApproval);

    // 从 /chat 的地址推 /approve 的地址。
    // 地址配的是完整的 chat 端点（如 http://127.0.0.1:8000/chat），
    // 把最后那段 /chat 换成 /approve 即可；不含 /chat 的地址退化成同目录下加 /approve。
    // 做成推导而不是加一个新配置项：部署时少填一个字段就少一处填错的机会。
    static std::string DeriveApproveUrl(const std::string& chatUrl);

    std::mutex              m_mtx;
    std::condition_variable m_cv;// 条件变量：队列空时 worker 睡在上面，Submit 时 notify_one 唤醒一个  
    //条件变量不能单独用，强制绑定一个 mutex；它本身不保存信号：提供内核级、低开销的线程间事件通知原语，用来解决「线程等待某个共享状态满足，直到别的线程修改状态并通知它」这个经典生产者 - 消费者同步问题。
    std::deque<AgentTask>   m_queue;                    // 进任务队列（有界 FIFO）
    std::unordered_map<uint64_t, AgentTask> m_inFlight; // 在途表：已取出、未完成
    // 取消表：(账号|会话ID) -> 该轮的取消标志。Submit 登记，worker 收尾时摘除
    std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>> m_cancel;

    // 待审批表：(账号|会话ID) -> 那一轮挂在 interrupt() 上的审批内容。
    // 生命周期和取消表不一样，所以必须单独一张表：worker 收尾时**不能**摘它，
    // 因为收尾意味着“这一轮停住了”，而它恰恰是下一步续跑的唯一凭据。
    // 只有 SubmitApprove 成功把它换成新任务时才摘（或者任务被 agent_stop 打断时由
    // HandleAgentStop 一并清掉，那一轮已经放弃了，留着只会让用户点同意时莫名重启一个旧任务）。
    std::unordered_map<std::string, AgentApproval> m_pendingApproval;

    std::vector<std::thread> m_workers;
    std::atomic<bool>       m_running{false};//原子变量：对这个变量的读、写操作，不会出现 “半写、撕裂”，CPU 保证这一次操作是不可分割的，要么做完，要么完全没做，解决的就是cpu缓存问题
    std::atomic<uint64_t>   m_nextId{1};                // 请求编号发号器（1 起，0 作无效值）

    // 智能体地址配置：只在启动前写一次，worker 取任务时读一份副本，
    // 所以用 m_mtx 保护就够，不必为它单独加锁
    AgentEndpoint           m_endpoint;
};

#endif //UNTITLED_AGENT_TCP_H