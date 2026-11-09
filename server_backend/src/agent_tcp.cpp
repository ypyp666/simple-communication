#include "agent_tcp.h"

#include "json.hpp"

#include <curl/curl.h>

#include <chrono>
#include <iostream>
#include <sstream>

namespace
{
    // ===== SSE 解析结果 =====
    struct SseResult
    {
        std::string answer;    // 所有 token 拼起来 = 完整回答
        std::string threadId;  // 智能体分配的会话 id，下一轮带上它就有上下文了
        std::string error;     // 智能体主动报的错误
        bool        finished = false;   // 是否收到过 done / [DONE]

        // ===== HITL 审批（新增）=====
        // 智能体要求用户点头才能往下走时为 true。这一轮通常没有正文，
        // 所以 answer 为空是正常的，不能当「智能体没返回内容」报错。
        bool        approvalRequired = false;
        std::string approvalSummary;   // 要用户确认的那句话
        std::string approvalSteps;     // JSON 数组原文
        std::string approvalCommand;   // JSON 对象原文
    };

    // 一次流式下载的上下文：既是 curl 的 userdata，也是“半行缓冲 + 累积结果”的容器。
    //
    // 为什么必须自己维护 pending：
    //   TCP 是字节流，一次写回调收到的数据可能正好把一行 JSON 劈成两半，
    //   也可能一次塞进来好几行。所以不完整的尾巴只能留着，等下一批字节到了再拼。
    //   这也正是流式能成立的前提 —— 不能像以前那样攒完整包再解析。
    struct StreamContext
    {
        std::string    pending;   // 还没凑成完整一行的尾巴
        std::string    rawBody;   // 完整响应体，只给“不是 SSE”的兜底解析用
        SseResult      sse;
        AgentDeltaSink onDelta;   // 增量回调，可能为空（只要最终结果，不要流）
        // 审批回调，可能为空。注意：不在这里当场回调，
        // 而是等流收了尾再统一交给上层——审批包到达时后面的 done 还没来，
        // 此刻就推给客户端的话，客户端可能先弹框、后收到本轮结束，
        // 两条包先后反了（和 ack 门是同一类问题）。
        AgentApprovalSink onApproval;
    };

    // 处理一整行 SSE。返回 false 表示要中止本次传输。
    bool HandleSseLine(StreamContext& ctx, std::string line)
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();   // 兼容 CRLF
        }

        // 只关心 data: 行；event:/id:/retry:/: 注释行和空行都跳过
        if (line.rfind("data:", 0) != 0)
        {
            return true;
        }

        std::string payload = line.substr(5);
        // SSE 规范里 data: 后面允许跟一个空格，不裁掉的话 JSON 解析会失败
        const std::size_t first = payload.find_first_not_of(' ');
        if (first == std::string::npos)
        {
            return true;   // "data:" 后面是空的
        }
        payload = payload.substr(first);

        if (payload == "[DONE]")
        {
            ctx.sse.finished = true;   // OpenAI 风格的结束标记
            return true;
        }

        try
        {
            const nlohmann::json event = nlohmann::json::parse(payload);
            const std::string name = event.value("event", std::string());

            if (name == "token")
            {
                // 全程唯一一处“边收边转发”：
                // 先把增量拼进总答案（日志和兜底要用），再立刻交给上层推给客户端，
                // 用户看到的就是一个字一个字长出来，而不是等整段流结束后一次性出现。
                const std::string delta = event.value("content", std::string());
                if (!delta.empty())
                {
                    ctx.sse.answer += delta;
                    if (ctx.onDelta && !ctx.onDelta(delta))
                    {
                        // 上层已经不想要了（客户端断线 / 用户停止），
                        // 再把这半截流收完只是白烧带宽和 worker
                        return false;
                    }
                }
            }
            else if (name == "session")
            {
                ctx.sse.threadId = event.value("thread_id", std::string());
            }
            else if (name == "done")
            {
                ctx.sse.finished = true;

                // 智能体把「这一轮停在审批点、还没完」也标在 done.status 里。
                // 正常情况下 approval_required 会先到，这里只是兜底，
                // 免得两条包顺序反了就把待审批误当成已完成。
                if (event.value("status", std::string()) == "pending_approval")
                {
                    ctx.sse.approvalRequired = true;
                }
            }
            else if (name == "error")
            {
                // 智能体自己的业务错误：记下来立刻停，
                // 不能把已经收到的半截回答当成功推出去
                ctx.sse.error = event.value("message", std::string("智能体内部错误"));
                return false;
            }
            else if (name == "approval_required")
            {
                // 智能体要用户点头才能往下走，这一轮就停在 interrupt() 上了。
                //
                // 服务器不替用户做决定，也不在这里弹框：只把内容原样记下来，
                // 等流收了尾再统一交给上层（见 PostAgentStream 的收尾段）。
                // 为什么不在这一行就回调：审批包后面紧跟着本轮结束，
                // 此刻就推给客户端的话，客户端可能先弹框、后收到「这一轮结束」，
                // 两条包先后反了（和 ack 门是同一类问题）。
                //
                // 字段名（summary / steps / command）是智能体那边定的，
                // 服务器不解析内部语义，只保证原样搬运。
                ctx.sse.approvalRequired = true;
                ctx.sse.approvalSummary  = event.value("summary", std::string());

                ctx.sse.approvalSteps = "[]";
                if (event.contains("steps"))
                {
                    ctx.sse.approvalSteps = event["steps"].dump();
                }

                ctx.sse.approvalCommand = "{}";
                if (event.contains("command"))
                {
                    ctx.sse.approvalCommand = event["command"].dump();
                }
            }
            // meta 只用来打日志，这里不需要
        }
        catch (const std::exception&)
        {
            // 单行不是合法 JSON 就跳过，不能因为一行脏数据把整个回答丢掉
        }
        return true;
    }

    // libcurl 的响应回调：curl 每收到一块数据就调一次。
    // 这里不解码整个响应体，而是按行切开、切出一行就立刻解析并回调 —— 流式输出就在这一步成真。
    std::size_t OnWriteBody(char* data, std::size_t size, std::size_t nmemb, void* userdata)
    {
        //四个参数意思分别为：
        // data: 接收到的数据指针
        // size: 每个数据块的大小
        // nmemb: 数据块的数量
        // userdata: 用户自定义数据指针
        const std::size_t total = size * nmemb;
        auto* ctx = static_cast<StreamContext*>(userdata);

        ctx->rawBody.append(data, total);   // 兜底解析（非 SSE 接口）还要用完整包
        ctx->pending.append(data, total);

        std::size_t pos = 0;
        while (true)
        {
            const std::size_t nl = ctx->pending.find('\n', pos);
            if (nl == std::string::npos)
            {
                break;   // 剩下的是半行，留给下一次回调拼
            }
            std::string line = ctx->pending.substr(pos, nl - pos);
            pos = nl + 1;
            if (!HandleSseLine(*ctx, std::move(line)))
            {
                // 返回 0 → curl 认为写失败，立刻以 CURLE_WRITE_ERROR 结束传输。
                // 这是“客户端断线 / 智能体报错”时唯一能及时收手的办法
                return 0;
            }
        }
        ctx->pending.erase(0, pos);   // 丢掉已处理部分，只留尾巴
        return total;                 // 必须返回实际处理的字节数，返回少了 curl 会认为写失败
    }

    // 一个 UTF-8 字符占几个字节。
    // 模拟智能体要按字符切片推送，按字节硬切会把一个汉字劈成两半，客户端显示成乱码
    std::size_t Utf8CharBytes(unsigned char lead)
    {
        if ((lead & 0x80) == 0x00) return 1;
        if ((lead & 0xE0) == 0xC0) return 2;
        if ((lead & 0xF0) == 0xE0) return 3;
        if ((lead & 0xF8) == 0xF0) return 4;
        return 1;   // 非法首字节当单字节处理，别把游标卡死
    }

    // libcurl 的进度回调：传输过程中被 curl 反复调用（约每秒数次）。
    // 返回非0 = 立刻中止本次传输，curl_easy_perform 会返回 CURLE_ABORTED_BY_CALLBACK。
    //
    // 只是读一个原子bool，绝不能在这里做耗时操作或加锁：
    // 这个回调跑在 curl 自己的传输循环里，卡住它等于卡住整个 worker。
    int OnCancelCheck(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
    //libcurl本身是c语言库，做不到像c++那样传递std::atomic<bool>，所以这里的userdata是void*类型的指针，指向一个std::atomic<bool>对象
    {
        const auto* flag = static_cast<const std::atomic<bool>*>(userdata);
        return flag->load(std::memory_order_relaxed) ? 1 : 0;
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

    // 把协议里的字符串消息ID转成库里的 INT 主键。
    // agent_request 已经校验过纯数字，这里只需要处理溢出这一种失败
    bool ParseMessageId(const std::string& text, uint32_t& out)
    {
        if (text.empty())
        {
            return false;
        }
        try
        {
            const unsigned long long parsed = std::stoull(text);
            if (parsed == 0 || parsed > 0xFFFFFFFFULL)
            {
                return false;
            }
            out = static_cast<uint32_t>(parsed);
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
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
        // 挂起表也要一起清：编译期内存里的审批内容已经没人能续跑了，
        // 留着只会让客户端重启后点「同意」时回一个莫名其妙的错误
        m_pendingApproval.clear();
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
        // 没有在途回答，但可能正停在审批点上：用户点停止就是放弃这一轮，
        // 把挂起记录一并清掉。不清的后果是：他过一会儿再点「同意」，
        // 服务器会默默把一个他已经不要的任务又启起来，而且会真的执行副作用。
        const auto pendingIt = m_pendingApproval.find(CancelKey(account, threadId));
        if (pendingIt != m_pendingApproval.end())
        {
            m_pendingApproval.erase(pendingIt);
            std::cout << "[agent] 已放弃待审批任务：account=" << account
                      << " threadId=" << threadId << std::endl;
            return true;
        }

        std::cout << "[agent] 取消请求未命中：account=" << account
                  << " threadId=" << threadId << "（该会话没有在途回答）" << std::endl;
        return false;
    }

    it->second->store(true, std::memory_order_relaxed);
    std::cout << "[agent] 已置取消位：account=" << account
              << " threadId=" << threadId << std::endl;
    return true;
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

        // ===== ack 门 =====
        // agent_response 和 agent_request_ack 是两条独立路径写 socket 的：
        // ack 在 handle 线程、response 在 worker 线程。handle 那边 Submit 之后还要回包、
        // 再交给 ClientWork 写出去，而 worker 一被 notify 就能跑 —— 两边没有任何先后保证。
        // 客户端靠 ack 启动看门狗、靠 response 关看门狗，顺序反了就会把回答当成过期包。
        //
        // 改成流式之后，这个门必须挪到调智能体之前：
        // 第一包 token 可能在 curl_easy_perform 返回之前就到达，如果还等到调用结束才开门，
        // 第一个增量包就会抢在 ack 前面出去。而且此刻 worker 正在 curl 的写回调里，
        // 在那里阻塞等 ack 会把 curl 的传输循环一起卡住，不如在这里先等完。
        // ack 由 HandleAgentRequest 在 Submit 之后立刻发出，实际等待时间基本是微秒级。
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

        // ===== 流式推送的准备 =====
        // weak_ptr::lock() 失败说明用户已经下线、会话对象被销毁了；
        // 成功就整轮持住：每个 token 都要回包，每块都去 lock 一次等于白多做一次原子操作。
        const SessionPtr sess = task.session.lock();
        bool pushedAny = false;   // 是否真的推过增量包

        // 推一条 agent_response。
        // 流式期间每块文字都是 done=false 的增量包，最后再单独发一条 done=true 的收尾包。
        const auto pushResponse = [&](const std::string& delta, bool finished, bool ok,
                                      int code, const std::string& message) -> bool
        {
            if (!sess)
            {
                return false;
            }

            nlohmann::json rsp;
            // 失败也走同一个包类型，只看 success/code/message，
            // 客户端就不用为"出错"再多写一条分支
            rsp["type"] = "agent_response";

            // 会话ID / 消息ID 一律原样回显：客户端靠它们把答案挂回正确的会话和气泡。
            // threadId 是客户端生成的，serverId 是 agent_message_create 里服务器发的，
            // 两个值服务器都不解析、不改写。改名 serverId 是为了对齐协议表
            rsp["threadId"]  = task.threadId;
            rsp["serverId"]  = task.messageId;
            // 服务器内部编号，只供日志排查，客户端可以忽略
            rsp["requestId"] = std::to_string(task.requestId);

            rsp["answer"]   = delta;
            rsp["done"]     = finished;
            rsp["finished"] = finished;   // finished 是旧名字，一并带上做兼容
            rsp["code"]     = code;
            rsp["message"]  = message;
            rsp["success"]  = ok;

            // SendJson 内部持 sendMtx 并检查 closed，
            // 保证不会和该连接自己的心跳应答交错拼出半行 JSON
            return sess->SendJson(rsp.dump() + "\n");
        };

        // 智能体每吐一段字就调这里一次 —— 真正的流式输出就在这一步：
        // 以前是把整个 SSE 收完才推一个包，现在是收到一块推一块。
        const AgentDeltaSink onDelta = [&](const std::string& delta) -> bool
        {
            // 用户刚点了停止：立刻别再吐字，返回 false 让 curl 顺手收手
            if (task.cancelled && task.cancelled->load(std::memory_order_relaxed))
            {
                return false;
            }

            // 推不出去说明客户端已经断线，再把流收完只是白烧带宽和 worker
            if (!pushResponse(delta, false, true, 0, ""))
            {
                return false;
            }

            pushedAny = true;
            return true;
        };

        // ===== 审批回调（HITL）=====
        // 智能体跑到需要用户点头的那一步时，这里被调一次。
        // 只做两件事：把内容存进挂起表（续跑的唯一凭据）+ 转发给客户端。
        // 真正“做什么”由客户端确认，服务器不替用户决定。
        bool approvalPending = false;
        const AgentApprovalSink onApproval = [&](const AgentApproval& approval) -> bool
        {
            approvalPending = true;

            // 先登记再推包，顺序不能反：客户端回执可能比推包还快（同机回环），
            // 反过来就会出现「点了同意却找不到待审批任务」。
            {
                std::lock_guard<std::mutex> lock(m_mtx);
                m_pendingApproval[CancelKey(task.account, task.threadId)] = approval;
            }

            std::cout << "[agent] 本轮停在审批点 requestId=" << task.requestId
                      << " threadId=" << task.threadId << std::endl;

            if (!sess)
            {
                // 客户端已经下线：没人能续跑了，提前收尾不再白占 worker。
                // 挂起记录先留着，重连后还能查到
                return false;
            }

            nlohmann::json rsp;
            rsp["type"]      = "agent_approval_required";
            rsp["threadId"]  = task.threadId;
            rsp["serverId"]  = task.messageId;
            rsp["requestId"] = std::to_string(task.requestId);
            rsp["summary"]   = approval.summary;

            // steps / command 在 agent_tcp 里一直是 JSON 原文（字符串），
            // 为嵌进外层包得先解回来；解不出来就原样当字符串给，
            // 至少客户端还能显示，不至于整包丢掉。
            rsp["steps"] = nlohmann::json::parse(approval.steps, nullptr, false);
            if (rsp["steps"].is_discarded())
            {
                rsp["steps"] = approval.steps;
            }

            rsp["command"] = nlohmann::json::parse(approval.command, nullptr, false);
            if (rsp["command"].is_discarded())
            {
                rsp["command"] = approval.command;
            }

            return sess->SendJson(rsp.dump() + "\n");
        };

        // 智能体可能回一个新的 thread_id（首次对话时），所以这里留一个可变副本
        std::string agentThreadId = agentThread;

        std::string answer;
        if (endpoint.mock)
        {
            // 模拟智能体：先整段算好，再按 UTF-8 字符切片慢慢吐，
            // 这样没有真实智能体时也能验证"边收边推"整条链路。
            // 按字符边界推进，绝不从汉字中间切，否则客户端会看到乱码
            const std::string full = MockAgent(task.prompt, agentThread);
            std::size_t i = 0;
            while (i < full.size())
            {
                if (task.cancelled && task.cancelled->load(std::memory_order_relaxed))
                {
                    break;
                }

                std::size_t end = i;
                for (int n = 0; n < 2 && end < full.size(); ++n)
                {
                    end += Utf8CharBytes(static_cast<unsigned char>(full[end]));
                }
                if (end > full.size())
                {
                    end = full.size();
                }

                const std::string piece = full.substr(i, end - i);
                i = end;
                answer += piece;

                if (!onDelta(piece))
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
            }
        }
        else if (task.approveResume)
        {
            // ===== 审批后续跑 =====
            // 地址换成 /approve（从 /chat 的地址推），thread_id 用智能体当初回传的那个：
            // interrupt() 的状态是挂在那一条 thread 上的，重新拼一个就等于换了一条对话线。
            AgentEndpoint approveEndpoint = endpoint;
            approveEndpoint.url = DeriveApproveUrl(endpoint.url);

            answer = CallAgentApprove(task.agentThreadId, task.account, task.approved,
                                      task.approvalNote, approveEndpoint, error,
                                      &agentThreadId, task.cancelled.get(),
                                      onDelta, onApproval);
        }
        else
        {
            answer = CallAgent(task.prompt, agentThread, task.account, endpoint, error,
                               &agentThreadId, task.cancelled.get(), onDelta, onApproval);
        }

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

        // ===== 回答不入库 =====
        // 服务器不做消息持久化：用户提问在 agent_request 处理时就已经写进 agent_message 了，
        // 智能体的回答只实时推给客户端，由客户端自己落本地库。
        // 以前这里还有一段“把正文回填进占位行”的逻辑，那是把两边的职责搞反了：
        // 服务器写回答、客户端写回答，同一段文字存两遍，还多了一次必然对不上的机会。

        // 被 agent_stop 打断的那一轮不再回包：停止回执已由 Handler 发过，
        // 再推一条迟到的答案会把用户已经停掉的气泡又改回去
        if (task.cancelled && task.cancelled->load(std::memory_order_relaxed))
        {
            std::cout << "[agent] 本轮已被取消，丢弃结果 requestId=" << task.requestId
                      << " threadId=" << task.threadId << std::endl;
            finishTask();
            continue;
        }

        // ===== 收尾包 =====
        // 增量已经随到随推完了，这里只负责告诉客户端"本轮结束"。
        // 成功、失败都必须发一条 done=true：客户端靠它结束"回答中"、关掉看门狗，
        // 漏掉这一条，该会话就会永久锁在"回答中"，只能靠用户手动重开会话。
        if (!sess)
        {
            std::cout << "[agent] 用户已下线，丢弃结果 requestId=" << task.requestId << std::endl;
            finishTask();
            continue;
        }

        // ===== 停在审批点 =====
        // 审批包已经在 onApproval 里推过了，这里只补一条收尾包让客户端关掉看门狗。
        // 绝不能掉到下面 answer.empty() 的分支里去报「智能体没有返回任何内容」——
        // 审批轮本来就没有正文，那不是错误。
        if (approvalPending)
        {
            pushResponse("", true, true, 0, "");
            std::cout << "[agent] 已推送审批请求 requestId=" << task.requestId
                      << " threadId=" << task.threadId << std::endl;
            finishTask();
            continue;
        }

        if (answer.empty())
        {
            // answer 和 error 同时为空（智能体返回 200 但内容里没有可用文本）
            // 也是异常，不能当成功推一个空回答回去
            const std::string message = error.empty() ? "智能体没有返回任何内容" : error;
            pushResponse("", true, false, 502, message);
            std::cout << "[agent] 本轮失败 requestId=" << task.requestId
                      << " 原因=" << message << std::endl;
        }
        else if (!pushedAny)
        {
            // 一个字都没流式推过（不是 SSE，而是普通 JSON 接口）：整段一次性发出去，
            // 行为和改造前完全一致，客户端那边只当收到一个很大的增量包
            pushResponse(answer, true, true, 0, "");
            std::cout << "[agent] 已推送（整包）requestId=" << task.requestId
                      << " threadId=" << task.threadId << std::endl;
        }
        else
        {
            // 增量包都已经发出去了，收尾包只带 done=true，回答内容不再重复发一遍
            pushResponse("", true, true, 0, "");
            std::cout << "[agent] 流式推送完成 requestId=" << task.requestId
                      << " threadId=" << task.threadId
                      << " 字节数=" << answer.size() << std::endl;
        }

        finishTask();
    }
}

std::string agent_tcp::CallAgent(const std::string& prompt,
                                 const std::string& threadId,
                                 const std::string& userId,
                                 const AgentEndpoint& endpoint,
                                 std::string& outError,
                                 std::string* outThreadId,
                                 const std::atomic<bool>* cancel,
                                 const AgentDeltaSink& onDelta,
                                 const AgentApprovalSink& onApproval)
{
    outError.clear();

    // ===== 1. 准备请求体 =====
    // 用 nlohmann::json 拼，绝不要手写字符串拼接：
    // 用户的 prompt 里一旦出现引号、换行、反斜杠，手拼出来的就是非法 JSON，
    // 智能体那边直接 422。
    // thread_id 为空要给 null，不能给空字符串 —— 有些框架会把 "" 当成合法 id 去查历史。
    nlohmann::json body;
    body["message"] = prompt;

    // 当前用户ID 明着带一份：thread_id 里虽然已经拼了账号，但那是两段粘在一起的
    // 复合键，智能体得自己拆才知道「谁在问」。单独给一个 user_id 字段，
    // 智能体侧直接拿去查偏好、做个性化，不用猜分隔符。
    // 值只取自登录会话（task.account），客户端传的 userId 一概不采信。
    body["user_id"] = userId;

    if (threadId.empty())
    {
        body["thread_id"] = nullptr;
    }
    else
    {
        body["thread_id"] = threadId;
    }
    const std::string bodyText = body.dump();

    // ===== 2. 真的发出去 =====
    // 建 handle、设超时、边收边解析这一段和 /approve 完全一样，
    // 统一放在 PostAgentStream 里，这里只负责把 /chat 的请求体拼好。
    return PostAgentStream(endpoint.url, bodyText, endpoint, outError, outThreadId,
                           cancel, onDelta, onApproval);
}

std::string agent_tcp::PostAgentStream(const std::string& url,
                                       const std::string& bodyText,
                                       const AgentEndpoint& endpoint,
                                       std::string& outError,
                                       std::string* outThreadId,
                                       const std::atomic<bool>* cancel,
                                       const AgentDeltaSink& onDelta,
                                       const AgentApprovalSink& onApproval)
{
    outError.clear();

    // ===== 一个请求一个独立的 easy handle =====
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

    // 流式上下文：半行缓冲 + 累积结果 + 增量回调。
    // 注意它必须活到 curl_easy_cleanup 之后 —— 它是 WRITEDATA 指向的对象
    StreamContext ctx;
    ctx.onDelta = onDelta;
    ctx.onApproval = onApproval;
    ctx.rawBody.reserve(4096);

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    // 告诉智能体我们接受流式响应（它的 /chat 就是流式的）
    headers = curl_slist_append(headers, "Accept: text/event-stream");//定义请求头

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());//设置请求的URL
    curl_easy_setopt(curl, CURLOPT_POST, 1L);//设置请求方式为POST，默认是GET
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyText.c_str());//设置POST请求的内容，POST请求体的数据指针
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(bodyText.size()));//设置POST请求的内容长度，POST请求体的数据长度
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);//设置请求头，curl_slist_append返回的链表指针

    // 响应不是直接打印，而是交给 OnWriteBody 按行切、边解析边回调；
    // 它只认识 StreamContext，所以这里传 ctx 的地址（而不是一个 std::string）
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnWriteBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    // 🔴 多线程程序必须打开：不开的话 curl 用 SIGALRM 实现 DNS 超时，
    // 信号会被随机投递给进程里任意线程，可能导致整个服务被干掉
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    // 两级超时，各管一件事：
    //   1) CONNECTTIMEOUT  只管 TCP 握手，防“智能体进程没起来”
    //   2) LOW_SPEED_*     唯一的静默检测，防“连上了但不出字”
    // 故意不设 CURLOPT_TIMEOUT：流式场景下“总耗时”不是错误信号，
    // 设了就会把“回答太长”当故障砍掉。长时间安静已由后端心跳（": ping"）消除，
    // 所以低速超时一个计时器就能同时覆盖“要等很久”和“真的死了”两种情况。
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, endpoint.connectTimeoutSec);//连接阶段超时

    // 低速超时：连续 lowSpeedTimeSec 秒内平均速率低于 lowSpeedLimitBytes 才判死。
    // 注意判断的是"时间窗口内的平均速率"，不是瞬时速率，
    // 所以 1 字节/30 秒的实际含义是 30 秒内累计收到不足约 30 字节才中止。
    // SSE 注释行（": ping"）同样是字节，会重置这个窗口 —— 这正是心跳能救命的原因，
    // 也是 C++ 侧不需要为心跳写任何代码的原因（ParseSseBody 只认 "data:" 行，注释直接跳过）。
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, endpoint.lowSpeedLimitBytes);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,  endpoint.lowSpeedTimeSec);

    // 智能体通常要求 HTTP/1.1，显式指定避免协商到服务端不支持的版本
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

    // 中途取消（agent_stop）：默认 curl 会把进度回调关掉（NOPROGRESS=1），
    // 必须先显式打开，XFERINFOFUNCTION 才会被调用。
    // 回调返回非0 时 curl_easy_perform 立刻收手并返回 CURLE_ABORTED_BY_CALLBACK，
    // 这是唯一能提前打断"已连上、正在收流"的请求的办法（超时只能干等）。
    if (cancel != nullptr)
    {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);//no progress直译：不要进度回调。默认值是1L：libcurl默认关闭进度回调，不会去调用你注册的进度函数。传0L= 关掉这个开关，启用进度回调。
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnCancelCheck);//XFERINFO = transfer info，传输信息回调。作用：告诉 libcurl：每次传输循环的时候，去调用你写好的这个函数OnCancelCheck
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool>*>(cancel));//这个是给上面那个回调函数传参数的，也就是回调里的 `void* userdata`
        //**当 libcurl 调用 OnCancelCheck 的时候，把这个指针当成 userdata 传进去**。就是说，OnCancelCheck 里可以拿到这个 cancel 指针，去检查用户是否请求取消任务。onCancelCheck第一个参数
    }

    // ===== 3. 真的发出去（阻塞当前 worker，但只阻塞它自己）=====
    const CURLcode code = curl_easy_perform(curl);//内部封装了全套的等待、接收、事件回调循环

    long httpStatus = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);// 拿到HTTP状态码，比如200、500

    curl_slist_free_all(headers);// 释放我们之前构造的请求头链表，防止内存泄漏
    curl_easy_cleanup(curl);// 销毁这个curl句柄，释放它内部所有资源

    if (code != CURLE_OK)
    {
        // 智能体主动报错、客户端断线，都会让写回调返回 0 提前结束传输，
        // 对外都表现为一个含糊的 CURLE_WRITE_ERROR，所以要先按它们解释
        if (!ctx.sse.error.empty())
        {
            outError = ctx.sse.error;
            std::cerr << "[agent] 智能体报错：" << outError << std::endl;
            return "";
        }

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
            // 写回调返回 0 的两个来源：用户点了停止（取消位已真），
            // 或者推送时发现客户端断线。两者处理一样，但日志里要能一眼区分开
            if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
            {
                outError = "本轮回答已按用户请求停止";
            }
            else
            {
                outError = "客户端已断开，停止接收智能体回答";
            }
            break;
        case CURLE_COULDNT_RESOLVE_HOST:
            outError = "智能体服务地址无法解析";
            break;
        case CURLE_ABORTED_BY_CALLBACK:
            // 进度回调把它打断的（两次数据之间），和网络故障无关，必须单独区分
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
        std::cerr << "[agent] " << outError << " body=" << ctx.rawBody.substr(0, 200) << std::endl;
        return "";
    }

    // ===== 4. 收尾 =====
    // 正常流里每一行已经在写回调里解析并回调过了，这里只剩“整理结果”和兜底。
    // 若最后一行没有以 '\n' 结尾（有些服务端就这样），它还在 pending 里没收尾，补一刀
    if (!ctx.pending.empty())
    {
        HandleSseLine(ctx, std::move(ctx.pending));
        ctx.pending.clear();
    }

    if (!ctx.sse.error.empty())
    {
        outError = ctx.sse.error;
        return "";
    }

    if (!ctx.sse.answer.empty())
    {
        if (outThreadId != nullptr)
        {
            *outThreadId = ctx.sse.threadId;
        }
        return ctx.sse.answer;
    }

    // 停在审批点：这一轮本来就没有正文，answer 为空是正常的，
    // 不能掉到下面「内容为空」的兜底里去报错。
    // 先把审批内容交给上层（挂起这一轮 + 转给客户端），再正常返回。
    if (ctx.sse.approvalRequired)
    {
        if (outThreadId != nullptr)
        {
            *outThreadId = ctx.sse.threadId;
        }

        if (onApproval)
        {
            AgentApproval approval;
            approval.threadId = ctx.sse.threadId;
            approval.summary  = ctx.sse.approvalSummary;
            approval.steps    = ctx.sse.approvalSteps;
            approval.command  = ctx.sse.approvalCommand;

            if (!onApproval(approval))
            {
                // 上层已经不想要了（客户端断线）：这一轮直接收，不推审批包
                return "";
            }
        }
        return "";
    }

    // 走到这里说明不是 SSE（或者流是空的），退回普通 JSON 解析，
    // 这样将来换成非流式接口也不用改代码
    const std::string plain = ExtractAnswer(ctx.rawBody);
    if (plain.empty() || plain == ctx.rawBody)
    {
        outError = "智能体返回内容为空或格式无法识别";
        std::cerr << "[agent] " << outError << " body=" << ctx.rawBody.substr(0, 200) << std::endl;
        return "";
    }
    return plain;
}

std::string agent_tcp::DeriveApproveUrl(const std::string& chatUrl)
{
    const std::string suffix = "/chat";
    if (chatUrl.size() >= suffix.size()
        && chatUrl.compare(chatUrl.size() - suffix.size(), suffix.size(), suffix) == 0)
    {
        return chatUrl.substr(0, chatUrl.size() - suffix.size()) + "/approve";
    }

    // 地址不含 /chat（有人会直接配完整端点），退化成同目录下加 /approve。
    // 做成推导而不是新增一个配置项：部署时少填一个字段，就少一处填错的机会。
    if (!chatUrl.empty() && chatUrl.back() == '/')
    {
        return chatUrl + "approve";
    }
    return chatUrl + "/approve";
}

std::string agent_tcp::CallAgentApprove(const std::string& agentThreadId,
                                        const std::string& userId,
                                        bool approved,
                                        const std::string& note,
                                        const AgentEndpoint& endpoint,
                                        std::string& outError,
                                        std::string* outThreadId,
                                        const std::atomic<bool>* cancel,
                                        const AgentDeltaSink& onDelta,
                                        const AgentApprovalSink& onApproval)
{
    outError.clear();

    if (agentThreadId.empty())
    {
        // 空 thread_id 打过去，智能体那边只会在历史里乱找，不如当场拒掉
        outError = "缺少待续跑的会话标识";
        return "";
    }

    // 请求体字段名跟智能体的 /approve 接口表对齐；
    // user_id 只取自登录会话，客户端传的一概不采信，和 /chat 一个口径。
    nlohmann::json body;
    body["thread_id"] = agentThreadId;
    body["approved"]  = approved;
    body["note"]      = note;
    body["user_id"]   = userId;

    const std::string bodyText = body.dump();

    return PostAgentStream(endpoint.url, bodyText, endpoint, outError, outThreadId,
                           cancel, onDelta, onApproval);
}

bool agent_tcp::SubmitApprove(const std::string& account,
                              const std::string& threadId,
                              const std::string& messageId,
                              bool approved,
                              const std::string& note,
                              const SessionPtr& session,
                              const std::shared_ptr<std::atomic<bool>>& ackSent,
                              uint64_t& outRequestId)
{
    outRequestId = 0;

    AgentApproval approval;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        const auto it = m_pendingApproval.find(CancelKey(account, threadId));
        if (it == m_pendingApproval.end())
        {
            return false;
        }

        approval = it->second;

        // 先摘掉再提交：一轮审批只能续跑一次。
        // 不摘的话，客户端重复点「同意」会把执行段跑两遍，而那一段是有副作用的。
        // 同时也把取消表里的旧记录当作已结束，避免 agent_stop 打到已经续跑的新一轮。
        m_pendingApproval.erase(it);
    }

    AgentTask task;
    task.account       = account;
    task.threadId      = threadId;
    task.messageId     = messageId;
    task.session       = session;
    task.ackSent       = ackSent;
    task.approveResume = true;
    task.approved      = approved;
    task.approvalNote  = note;
    task.agentThreadId = approval.threadId;

    if (!Submit(std::move(task), outRequestId))
    {
        // 队列满了：把挂起记录放回去，用户还能再点一次。
        // 不能就这么丢掉 —— 那条记录是智能体 wait 在那里的一轮状态，
        // 丢了就只能从头再问一遍。
        std::lock_guard<std::mutex> lock(m_mtx);
        m_pendingApproval[CancelKey(account, threadId)] = approval;
        return false;
    }

    std::cout << "[agent] 提交审批续跑 account=" << account
              << " threadId=" << threadId
              << " approved=" << (approved ? "true" : "false") << std::endl;
    return true;
}

bool agent_tcp::HasPendingApproval(const std::string& account, const std::string& threadId)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    const auto it = m_pendingApproval.find(CancelKey(account, threadId));
    return it != m_pendingApproval.end();
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
