#include "agent_tcp.h"

#include "json.hpp"

#include <curl/curl.h>

#include <chrono>
#include <iostream>
#include <sstream>

namespace
{
    // libcurl 的响应回调：curl 每收到一块数据就调一次，
    // 我们要把这些碎片拼成完整响应体，所以把 std::string 的地址当 userdata 传进去。
    std::size_t OnWriteBody(char* data, std::size_t size, std::size_t nmemb, void* userdata)
    {
        //四个参数意思分别为：
        // data: 接收到的数据指针
        // size: 每个数据块的大小
        // nmemb: 数据块的数量
        // userdata: 用户自定义数据指针
        const std::size_t total = size * nmemb;
        static_cast<std::string*>(userdata)->append(data, total);
        return total;   // 必须返回实际处理的字节数，返回少了 curl 会认为写失败
    }

    // libcurl 的进度回调：传输过程中被 curl 反复调用（约每秒数次）。
    // 返回非0 = 立刻中止本次传输，curl_easy_perform 会返回 CURLE_ABORTED_BY_CALLBACK。
    //
    // 只是读一个原子bool，绝不能在这里做耗时操作或加锁：
    // 这个回调跑在 curl 自己的传输循环里，卡住它等于卡住整个 worker。
    int OnCancelCheck(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
    {
        const auto* flag = static_cast<const std::atomic<bool>*>(userdata);
        return flag->load(std::memory_order_relaxed) ? 1 : 0;
    }

    // ===== SSE 解析结果 =====
    struct SseResult
    {
        std::string answer;    // 所有 token 拼起来 = 完整回答
        std::string threadId;  // 智能体分配的会话 id，下一轮带上它就有上下文了
        std::string error;     // 智能体主动报的错误
    };

    // 解析 Server-Sent Events 响应体。
    // 智能体的 /chat 是流式的，一行一个事件，形如：
    //     data: {"event":"session","thread_id":"..."}
    //     data: {"event":"meta","agent":"chat_agent"}
    //     data: {"event":"token","content":"你"}      <-- 增量，要拼接
    //     data: {"event":"done","agent":"chat_agent"}
    // 基础版先把整个流收完再拼；第二步做流式推送时，
    // 只要把这段逻辑搬进 OnWriteBody 对每行即时处理即可，其余代码不用动。
    void ParseSseBody(const std::string& body, SseResult& out)
    {
        std::istringstream in(body);
        std::string line;

        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();   // 兼容 CRLF
            }

            // 只关心 data: 行；event:/id:/retry:/: 注释行和空行都跳过
            if (line.rfind("data:", 0) != 0)//从0之后范围找亿美元data
            {
                continue;
            }

            std::string payload = line.substr(5);//去掉前缀 "data:"，剩下的就是 JSON 负载
            const std::size_t first = payload.find_first_not_of(' ');//从 payload 字符串里，找到第一个不是空格的字符的下标，stl函数找到第一个不是xx的字符
            if (first == std::string::npos)//std::string::npos是std::string里定义的一个常量，代表**没有找到目标字符 / 子串
            {
                continue;   // "data:" 后面是空的
            }
            payload = payload.substr(first);//substr(pos)含义：**从下标 pos 开始，截取到字符串末尾SSE协议规范里data:后面允许跟一个空格，很多服务端都会写成 data: 内容。
            //如果我们不做这个裁剪，payload 开头就会多一个空格，传给上层解析 JSON 的时候，偶尔会导致 json 解析失败
            if (payload == "[DONE]")
            {
                break;      // OpenAI 风格的结束标记
            }

            try
            {
                const nlohmann::json event = nlohmann::json::parse(payload);
                const std::string name = event.value("event", std::string());

                if (name == "token")
                {
                    out.answer += event.value("content", std::string());//增量文字片段，拼到`out.answer`（流式输出的一小块文字
                }
                else if (name == "session")
                {
                    out.threadId = event.value("thread_id", std::string());//带回会话 id `thread_id`
                }
                else if (name == "error")//业务层面报错，填入`out.error`
                {
                    out.error = event.value("message", std::string("智能体内部错误"));
                }
                // meta / done 只用来打日志，这里不需要
            }
            catch (const std::exception&)
            {
                // 单行不是合法 JSON 就跳过，不能因为一行脏数据把整个回答丢掉
            }
        }
    }

    // 非流式接口的兜底：直接从 JSON 里挑回答字段。
    // 命中哪个用哪个，都没有就把整个响应体当回答，方便对接最简陋的纯文本接口。
    std::string ExtractAnswer(const std::string& body)
    {
        try
        {
            const nlohmann::json parsed = nlohmann::json::parse(body);
            for (const char* key : {"answer", "content", "reply", "message", "text"})
            {
                if (parsed.contains(key) && parsed[key].is_string())
                {
                    return parsed[key].get<std::string>();//如果 JSON 里有这些字段，就直接返回它们的值
                }
            }
            // OpenAI 风格：{"choices":[{"message":{"content":"..."}}]}
            if (parsed.contains("choices") && parsed["choices"].is_array() &&
                !parsed["choices"].empty())
            {
                const auto& first = parsed["choices"][0];
                if (first.contains("message") && first["message"].contains("content") &&
                    first["message"]["content"].is_string())
                {
                    return first["message"]["content"].get<std::string>();
                }
            }
        }
        catch (const std::exception&)
        {
            // 不是 JSON 就原样返回
        }
        return body;
    }
}

agent_tcp& agent_tcp::Instance()
{
    // C++11 起局部静态变量的初始化是线程安全的，这里不需要额外加锁
    static agent_tcp instance;
    return instance;
}

agent_tcp::~agent_tcp()
{
    Stop();
}

void agent_tcp::Configure(const AgentEndpoint& endpoint)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    m_endpoint = endpoint;
}

void agent_tcp::Start(int workerCount)
{
    std::lock_guard<std::mutex> lock(m_mtx);//`lock_guard` 是一个**栈上临时管理对象**，构造的时候自动调用 `m_mtx.lock()`，出作用域销毁的时候自动调用 `m_mtx.unlock()`。
    if (m_running.load())//原子读
    {
        return;   // 幂等：已经启动过就不重复拉线程
    }

    if (workerCount <= 0)
    {
        workerCount = kDefaultWorkers;
    }

    // ===== 必须在任何线程调用 curl 之前初始化 =====
    // curl_global_init 不是线程安全的，只能在拉 worker 线程之前调一次；
    // 漏了它（或放到多线程里调）会出现随机崩溃 / DNS 解析失败。
    // CURL_GLOBAL_ALL 表示把 SSL、Win32 socket 等全部子系统都初始化好。
    curl_global_init(CURL_GLOBAL_ALL);//libcurl库的全局初始化函数，整个进程只能最多调用一次

    m_running.store(true);//原子写
    m_workers.reserve(static_cast<std::size_t>(workerCount));
    for (int i = 0; i < workerCount; ++i)
    {
        m_workers.emplace_back(&agent_tcp::WorkerLoop, this);//emplace_back：在容器尾部直接构造对象，避免拷贝/移动；&agent_tcp::WorkerLoop：成员函数指针，this：传给成员函数的隐式参数
    }

    std::cout << "[agent] 线程池已启动，worker 数量：" << workerCount
              << "，智能体地址：" << (m_endpoint.mock ? "(本地模拟)" : m_endpoint.url)
              << std::endl;
}

void agent_tcp::Stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (!m_running.load())
        {
            return;
        }
        m_running.store(false);
    }

    // 先把状态改成停止，再叫醒所有 worker，它们醒来后看到 m_running=false 就会退出
    m_cv.notify_all();

    for (std::thread& worker : m_workers)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
    m_workers.clear();

    {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_queue.clear();
        m_inFlight.clear();
        m_cancel.clear();
    }

    // 所有 worker 都已 join，此时不会再有线程用 curl，可以安全清理
    curl_global_cleanup();

    std::cout << "[agent] 线程池已停止" << std::endl;
}

bool agent_tcp::Submit(AgentTask task, uint64_t& outRequestId)
{
    outRequestId = 0;

    // 懒启动：万一 main 里忘了 Start，第一次提交时自动拉起来，功能不至于整块失效
    if (!m_running.load())
    {
        Start();
    }

    std::lock_guard<std::mutex> lock(m_mtx);

    if (!m_running.load())
    {
        return false;
    }

    // 队列已满：直接拒掉，让调用方回 503。
    // 不设上限的后果是智能体一慢，任务就无限堆积，内存和延迟一起失控
    if (m_queue.size() >= kMaxQueueSize)
    {
        std::cout << "[agent] 队列已满（" << kMaxQueueSize << "），拒绝新任务" << std::endl;
        return false;
    }

    task.requestId = m_nextId.fetch_add(1, std::memory_order_relaxed);
    outRequestId = task.requestId;

    // 取消位在入队前就登记好：用户在任务还没被 worker 取走时也可能点停止，
    // 只登记"在途"的任务会漏掉排队中的那一轮
    task.cancelled = std::make_shared<std::atomic<bool>>(false);
    // 同一会话又来一轮就覆盖掉旧取消位：协议约定同一会话同时只有一轮回答，
    // 覆盖不会误伤，反而让 agent_stop 永远能找到"最新的那一轮"
    m_cancel[CancelKey(task.account, task.threadId)] = task.cancelled;

    m_queue.push_back(std::move(task));
    m_cv.notify_one();   // 只叫醒一个 worker：一个任务只需要一个人处理

    return true;
}

std::string agent_tcp::CancelKey(const std::string& account, const std::string& threadId)
{
    // 用不可见分隔符拼接，避免账号 "1" + 会话 "23" 和账号 "12" + 会话 "3" 撞成同一个键
    return account + '\x1f' + threadId;
}

bool agent_tcp::Cancel(const std::string& account, const std::string& threadId)
{
    std::lock_guard<std::mutex> lock(m_mtx);

    const auto it = m_cancel.find(CancelKey(account, threadId));
    if (it == m_cancel.end() || !it->second)
    {
        std::cout << "[agent] 取消请求未命中：account=" << account
                  << " threadId=" << threadId << "（该会话没有在途回答）" << std::endl;
        return false;
    }

    it->second->store(true, std::memory_order_relaxed);
    std::cout << "[agent] 已置取消位：account=" << account
              << " threadId=" << threadId << std::endl;
    return true;
}

std::string agent_tcp::AllocateMessageId()
{
    // 序号只在同毫秒内起作用，取模 1000 保证不会把位宽拉长；
    // 真凑到单毫秒 1000 条以上会回绕撞号，但那种量级已经远超当前单机承载
    static std::atomic<uint64_t> seq{0};

    const auto millis = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    const uint64_t tail = seq.fetch_add(1, std::memory_order_relaxed) % 1000;

    // 序号补零对齐：不补零的话 "1699999999999" + "7" 和 "1699999999999" + "70"
    // 只差一位，排序会把 7 排到 70 后面
    std::string id = std::to_string(millis);
    id += std::to_string(1000 + tail).substr(1);   // 1000+tail 必为 4 位，取后 3 位就是补零

    return id;
}

std::string agent_tcp::MakeAgentThreadId(const std::string& threadId, const std::string& userId)
{
    // 任一端为空就不拼：拼出来形如 "12n" 或 "n10010"，智能体那边没意义，
    // 我们拆回来也拿不到完整信息。这种情况属于上游参数出了问题，
    // 原样透传比造一个半截的 key 好 —— 至少日志里能一眼看出是空的
    if (threadId.empty() || userId.empty())
    {
        return threadId;
    }

    return threadId + kAgentThreadSep + userId;
}

bool agent_tcp::SplitAgentThreadId(const std::string& agentThreadId,
                                   std::string& threadId, std::string& userId)
{
    // 用 find 取第一个 'n' 而不是 rfind：两边都是纯数字，
    // 整串里只可能有这一个 'n'，第一个就是唯一分界点
    const std::size_t pos = agentThreadId.find(kAgentThreadSep);

    // pos == 0（以 'n' 开头）或 pos+1 >= size（以 'n' 结尾）都说明两段里有一段是空的，
    // 不是我们拼出来的格式，直接判失败，不要返回半截结果
    if (pos == std::string::npos || pos == 0 || pos + 1 >= agentThreadId.size())
    {
        return false;
    }

    threadId = agentThreadId.substr(0, pos);
    userId   = agentThreadId.substr(pos + 1);
    return true;
}

void agent_tcp::WorkerLoop()
{
    while (true)
    {
        AgentTask task;
        AgentEndpoint endpoint;
        {
            std::unique_lock<std::mutex> lock(m_mtx);

            // 没活就睡在条件变量上，完全不占 CPU；
            // 直到有人 Submit（notify_one）或 Stop（notify_all）才被唤醒
            m_cv.wait(lock, [this] { return !m_running.load() || !m_queue.empty(); });

            if (!m_running.load())
            {
                return;   // 停机：直接退出，不再取任务
            }

            task = std::move(m_queue.front());
            m_queue.pop_front();

            // 出队即登记在途，方便后续做超时、取消、断线回收
            m_inFlight[task.requestId] = task;

            // 配置只在启动前改，这里拿一份副本，出锁之后用
            endpoint = m_endpoint;
        }

        // 收尾：出在途表 + 摘掉取消位。
        // 摘取消位必须校验是不是同一个 shared_ptr —— 同一会话可能已经提交了新一轮，
        // 无脑 erase 会把新一轮的取消位一起抹掉，那一轮就再也 stop 不掉了
        const auto finishTask = [this, &task]()
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_inFlight.erase(task.requestId);
            const auto it = m_cancel.find(CancelKey(task.account, task.threadId));
            if (it != m_cancel.end() && it->second == task.cancelled)
            {
                m_cancel.erase(it);
            }
        };

        // 排队期间用户就点了停止：不必再打一次智能体，直接收尾。
        // 这里不回包 —— 停止的回执已经由 HandleAgentStop 当场发走了，
        // 再补一条只会让客户端收到两个 done
        if (task.cancelled && task.cancelled->load(std::memory_order_relaxed))
        {
            std::cout << "[agent] 任务出队前已被取消，直接丢弃 requestId=" << task.requestId
                      << " threadId=" << task.threadId << std::endl;
            finishTask();
            continue;
        }

        // ===== 干活区：阻塞时长完全取决于智能体，这里绝不能持有全局锁 =====
        std::cout << "[agent] 开始处理 requestId=" << task.requestId
                  << " account=" << task.account
                  << " threadId=" << task.threadId
                  << " prompt=" << task.prompt << std::endl;

        std::string error;
        // ===== 智能体侧的 thread_id 要拼接，不能直接用会话ID =====
        // langchain 的 thread 是全局唯一的，而我们的会话ID 只是“账号内的编号”：
        // 会话ID 由客户端生成，不同用户完全可能造出同一个号。
        // 直接把会话ID 当 thread_id 发过去，两家的对话会落到同一条 thread 上，
        // 上下文互相穿插 —— 甲用户能看到乙用户的提问，反过来乙的追问也被甲的上下文带偏。
        // 所以按协议拼成 "会话ID + 'n' + 账号"：
        //   • 账号只从登录会话取，客户端传什么都不采信，天然防冒用；
        //   • 拼完之后 (会话, 账号) 这个联合主键就整体进了 thread_id，与库里的语义一致；
        //   • 中间加 'n' 只是分隔符，两边都是纯数字，解包时取第一个 'n' 就够。
        // 这里的拼接是确定性的：不存任何状态，每轮现算现用，
        // 多设备重连、服务重启之后算出来还是同一个 thread，上下文不会断。
        const std::string agentThread = MakeAgentThreadId(task.threadId, task.account);

        // 智能体可能回一个新的 thread_id（首次对话时），所以这里留一个可变副本
        std::string agentThreadId = agentThread;
        const std::string answer = endpoint.mock
            ? MockAgent(task.prompt, agentThread)
            : CallAgent(task.prompt, agentThread, endpoint, error,
                        &agentThreadId, task.cancelled.get());

        if (!agentThreadId.empty())
        {
            // 日志里打的是拼好的全串，排查时直接拿去搜智能体那边的记录就能对上号
            std::cout << "[agent] 智能体 thread_id=" << agentThreadId << std::endl;

            // 智能体回传的值和我们发出去的不一样也不是错（它会补自己的后缀），
            // 但只要拆得开就能确认“会话/账号”两段还在，拆不开说明上游改了格式，得早发现
            std::string backThreadId;
            std::string backUserId;
            if (SplitAgentThreadId(agentThreadId, backThreadId, backUserId))
            {
                if (backThreadId != task.threadId || backUserId != task.account)
                {
                    std::cout << "[agent] 注意：智能体改写了 thread_id 的会话/账号段，"
                              << "threadId=" << backThreadId
                              << " account=" << backUserId << std::endl;
                }
            }
            else
            {
                std::cout << "[agent] 警告：智能体回传的 thread_id 无法解包="
                          << agentThreadId << std::endl;
            }
        }

        // 被 agent_stop 打断的那一轮不再回包：停止回执已由 Handler 发过，
        // 再推一条迟到的答案会把用户已经停掉的气泡又改回去
        if (task.cancelled && task.cancelled->load(std::memory_order_relaxed))
        {
            std::cout << "[agent] 本轮已被取消，丢弃结果 requestId=" << task.requestId
                      << " threadId=" << task.threadId << std::endl;
            finishTask();
            continue;
        }

        // ===== ack 门 =====
        // agent_response 和 agent_request_ack 是两条独立路径写 socket 的：
        // ack 在 handle 线程、response 在 worker 线程。handle 那边 Submit 之后还要回包、
        // 再交给 ClientWork 写出去，而 worker 一被 notify 就能跑 —— 两边没有任何先后保证，
        // mock 这个分支又特别快，几乎必然抢在 ack 前面。
        // 客户端靠 ack 启动看门狗、靠 response 关看门狗，顺序反了就会把回答当成过期包。
        // 所以这里先卡住，等 handle 把 ack 真正写进 socket 后再继续。
        // 上限 3 秒只是兜底：万一对端已经断线、ack 根本发不出去，worker 不能被永久卡住白占线程。
        if (task.ackSent)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!task.ackSent->load(std::memory_order_acquire))
            {
                // 等闸门期间用户又点了停止，就没必要再等了
                if (task.cancelled && task.cancelled->load(std::memory_order_relaxed))
                {
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    std::cout << "[agent] 等待 ack 超时（3s），仍继续推送 requestId="
                              << task.requestId << std::endl;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        // 等闸门期间用户可能又点了停止，推包前再确认一次
        if (task.cancelled && task.cancelled->load(std::memory_order_relaxed))
        {
            std::cout << "[agent] 本轮在等 ack 期间被取消，丢弃结果 requestId="
                      << task.requestId << " threadId=" << task.threadId << std::endl;
            finishTask();
            continue;
        }

        // ===== 推送结果 =====
        // weak_ptr::lock() 失败说明用户已经下线、会话对象被销毁了，结果直接丢弃
        if (const SessionPtr sess = task.session.lock())
        {
            nlohmann::json rsp;
            // 失败也走同一个包类型，只看 success/code/message，
            // 客户端就不用为"出错"再多写一条分支
            rsp["type"] = "agent_response";

            // 会话ID / 消息ID 一律原样回显：客户端靠它们把答案挂回正确的会话和气泡。
            // threadId 是客户端生成的，serverId 是 agent_message_create 里服务器发的，
            // 两个值服务器都不解析、不改写。改名 serverId 是为了对齐协议表
            rsp["threadId"] = task.threadId;
            rsp["serverId"] = task.messageId;
            // 服务器内部编号，只供日志排查，客户端可以忽略
            rsp["requestId"] = std::to_string(task.requestId);

            // done 每轮都必须为 true —— 客户端靠它结束“回答中”状态。
            // 漏掉这一条，该会话就会永久锁在“回答中”，只能靠用户手动重开会话。
            // 成功和失败都要带，所以放在分支外面统一设置；finished 是旧名字，一并带上做兼容
            rsp["done"]     = true;
            rsp["finished"] = true;

            if (answer.empty())
            {
                // answer 和 error 同时为空（智能体返回 200 但内容里没有可用文本）
                // 也是异常，不能当成功推一个空回答回去
                rsp["code"]    = 502;
                rsp["message"] = error.empty() ? "智能体没有返回任何内容" : error;
                rsp["success"] = false;
                rsp["answer"]  = "";
            }
            else
            {
                rsp["code"]    = 0;
                rsp["message"] = "";
                rsp["success"] = true;
                // 顶层 answer 才是协议字段（兼容 content），不要再套一层 data
                rsp["answer"] = answer;
            }

            // SendJson 内部持 sendMtx 并检查 closed，
            // 保证不会和该连接自己的心跳应答交错拼出半行 JSON
            if (sess->SendJson(rsp.dump() + "\n"))
            {
                std::cout << "[agent] 已推送 requestId=" << task.requestId
                          << " threadId=" << task.threadId << std::endl;
            }
            else
            {
                std::cout << "[agent] 推送失败，连接已断开 requestId=" << task.requestId << std::endl;
            }
        }
        else
        {
            std::cout << "[agent] 用户已下线，丢弃结果 requestId=" << task.requestId << std::endl;
        }

        finishTask();
    }
}

std::string agent_tcp::CallAgent(const std::string& prompt,
                                 const std::string& threadId,
                                 const AgentEndpoint& endpoint,
                                 std::string& outError,
                                 std::string* outThreadId,
                                 const std::atomic<bool>* cancel)
{
    outError.clear();

    // ===== 1. 准备请求体 =====
    // 用 nlohmann::json 拼，绝不要手写字符串拼接：
    // 用户的 prompt 里一旦出现引号、换行、反斜杠，手拼出来的就是非法 JSON，
    // 智能体那边直接 422。
    // thread_id 为空要给 null，不能给空字符串 —— 有些框架会把 "" 当成合法 id 去查历史。
    nlohmann::json body;
    body["message"] = prompt;
    if (threadId.empty())
    {
        body["thread_id"] = nullptr;
    }
    else
    {
        body["thread_id"] = threadId;
    }
    const std::string bodyText = body.dump();

    // ===== 2. 每个请求一个独立的 easy handle =====
    // 不共享 handle：curl 的 easy handle 不是线程安全的，
    // 3 个 worker 共用一个会互相踩。后面要提性能可以按 worker 各留一个长期复用。
    CURL* curl = curl_easy_init();
    /*libcurl 的设计思路：**一次 easy 句柄，对应一次完整的 HTTP 事务（一次请求 + 一次响应）**。
    一次 HTTP 事务天然包含两半：客户端输出：HTTP Request 服务端输入：HTTP Response*/
    if (curl == nullptr)
    {
        outError = "初始化 HTTP 客户端失败";
        return "";
    }

    std::string responseBody;
    responseBody.reserve(4096);

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    // 告诉智能体我们接受流式响应（它的 /chat 就是流式的）
    headers = curl_slist_append(headers, "Accept: text/event-stream");//定义请求头

    curl_easy_setopt(curl, CURLOPT_URL, endpoint.url.c_str());//设置请求的URL
    curl_easy_setopt(curl, CURLOPT_POST, 1L);//设置请求方式为POST，默认是GET
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyText.c_str());//设置POST请求的内容，POST请求体的数据指针
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(bodyText.size()));//设置POST请求的内容长度，POST请求体的数据长度
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);//设置请求头，curl_slist_append返回的链表指针

    // 响应不是直接打印，而是交给 OnWriteBody 拼进 responseBody
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnWriteBody);
    // 传给回调的 userdata，这里是 responseBody 的地址
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);

    // 🔴 多线程程序必须打开：不开的话 curl 用 SIGALRM 实现 DNS 超时，
    // 信号会被随机投递给进程里任意线程，可能导致整个服务被干掉
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    // 三级超时，各管一件事：
    //   1) CONNECTTIMEOUT  只管 TCP 握手，防"智能体进程没起来"
    //   2) LOW_SPEED_*     主检测手段，防"连上了但不出字"，流式场景就该用它
    //   3) TIMEOUT         纯兜底，防"细水长流"式占住 worker，必须远大于正常回答耗时
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, endpoint.connectTimeoutSec);//连接阶段超时

    // 低速超时：连续 lowSpeedTimeSec 秒内平均速率低于 lowSpeedLimitBytes 才判死。
    // 注意判断的是"时间窗口内的平均速率"，不是瞬时速率，
    // 所以 1 字节/30 秒的实际含义是 30 秒内累计收到不足约 30 字节才中止。
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, endpoint.lowSpeedLimitBytes);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,  endpoint.lowSpeedTimeSec);

    // 总时长兜底：宁可设得宽松，也不要让它抢在低速超时前面砍掉正常的长回答
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, endpoint.totalTimeoutSec);//整个http事务超时

    // 智能体通常要求 HTTP/1.1，显式指定避免协商到服务端不支持的版本
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

    // 中途取消（agent_stop）：默认 curl 会把进度回调关掉（NOPROGRESS=1），
    // 必须先显式打开，XFERINFOFUNCTION 才会被调用。
    // 回调返回非0 时 curl_easy_perform 立刻收手并返回 CURLE_ABORTED_BY_CALLBACK，
    // 这是唯一能提前打断"已连上、正在收流"的请求的办法（超时只能干等）。
    if (cancel != nullptr)
    {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnCancelCheck);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool>*>(cancel));
    }

    // ===== 3. 真的发出去（阻塞当前 worker，但只阻塞它自己）=====
    const CURLcode code = curl_easy_perform(curl);//内部封装了全套的等待、接收、事件回调循环

    long httpStatus = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);// 拿到HTTP状态码，比如200、500

    curl_slist_free_all(headers);// 释放我们之前构造的请求头链表，防止内存泄漏
    curl_easy_cleanup(curl);// 销毁这个curl句柄，释放它内部所有资源

    if (code != CURLE_OK)
    {
        // 把 curl 的错误码翻成人话，日志和客户端提示都用得上
        switch (code)
        {
        case CURLE_OPERATION_TIMEDOUT:
            // 低速超时和总兜底超时共用这个错误码，提示语用"长时间无输出"更准确：
            // 只有真卡死（低速超时）才常见，正常长回答被总超时砍掉属配置问题，看日志里的耗时能分辨
            outError = "智能体长时间无输出，已中止";
            break;
        case CURLE_COULDNT_CONNECT:
            outError = "无法连接智能体服务，请确认智能体进程已启动";
            break;
        case CURLE_WRITE_ERROR:
            // 将来做流式推送时，写回调发现客户端断线会返回 0 触发这个码，
            // 那不是智能体的错，日志里要能一眼区分开
            outError = "客户端已断开，停止接收智能体回答";
            break;
        case CURLE_COULDNT_RESOLVE_HOST:
            outError = "智能体服务地址无法解析";
            break;
        case CURLE_ABORTED_BY_CALLBACK:
            // 我们自己置的取消位把它打断的，和网络故障无关，必须单独区分
            outError = "本轮回答已按用户请求停止";
            break;
        case CURLE_GOT_NOTHING:
            outError = "智能体服务未返回任何内容";
            break;
        default:
            outError = std::string("调用智能体失败：") + curl_easy_strerror(code);
            break;
        }
        std::cerr << "[agent] " << outError << " (curl=" << code << ")" << std::endl;
        return "";
    }

    if (httpStatus < 200 || httpStatus >= 300)
    {
        outError = "智能体服务返回异常状态码 " + std::to_string(httpStatus);
        std::cerr << "[agent] " << outError << " body=" << responseBody.substr(0, 200) << std::endl;
        return "";
    }

    // ===== 4. 解析响应 =====
    SseResult sse;
    ParseSseBody(responseBody, sse);

    if (!sse.error.empty())
    {
        outError = sse.error;
        return "";
    }

    if (!sse.answer.empty())
    {
        if (outThreadId != nullptr)
        {
            *outThreadId = sse.threadId;
        }
        return sse.answer;
    }

    // 走到这里说明不是 SSE（或者流是空的），退回普通 JSON 解析，
    // 这样将来换成非流式接口也不用改代码
    const std::string plain = ExtractAnswer(responseBody);
    if (plain.empty() || plain == responseBody)
    {
        outError = "智能体返回内容为空或格式无法识别";
        std::cerr << "[agent] " << outError << " body=" << responseBody.substr(0, 200) << std::endl;
        return "";
    }
    return plain;
}

std::string agent_tcp::MockAgent(const std::string& prompt, const std::string& agentId)
{
    // 纯本地模拟，不依赖网络：
    // 用来单独压测「队列 + 线程池 + 推送」这条管子，
    // 也方便在智能体进程还没起来时先联调客户端。
    // 800ms 是为了模拟真实思考耗时，让你能观察到排队现象。
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    if (agentId.empty())
    {
        return "（模拟智能体）我收到你的问题了：" + prompt;
    }
    return "（模拟智能体 " + agentId + "）我收到你的问题了：" + prompt;
}
