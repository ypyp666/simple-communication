# 智能体后端（Supervisor 多专家路由 + HITL 审批）

本目录是 Qt 桌面软件内嵌智能体的 Python 服务端。它以 **FastAPI + SSE** 向前端推送流式数据，
内部由 **LangGraph** 编排「路由 → 专家 → 审批」的多智能体图，会话状态由 **SQLite checkpointer**
持久化，因此审批可以跨请求、跨重启续跑。

---

## 1. 架构

```
                                    ┌──────────────┐
                        ┌──────────►│ chat_expert  │──► END
                        │           └──────────────┘   闲聊 / 情绪 / 知识问答
                        │           ┌──────────────┐
   START ──► supervisor ├──────────►│imagine_expert│──► END
             (路由分发)  │           └──────────────┘   带图片消息，视觉理解
                        │           ┌──────────────┐   ┌───────────────┐
                        └──────────►│ task_expert  │──►│examine_expert │
                                     └──────────────┘   │   (HITL)      │
                                            ▲           └───────┬───────┘
                                            │ 批准后回到执行阶段   │
                                            └───────────────────┤
                                                                │ 拒绝 / 超时
                                                                ▼
                                                               END
```

| 节点 | 职责 | 输出给用户的事件 |
| :-- | :-- | :-- |
| `supervisor` | LLM 意图分类，只决定交给谁，不产出文本 | 无（内部节点） |
| `chat_expert` | 情感/日常聊天专家，需要时调用联网搜索工具 | `token` |
| `imagine_expert` | 图片理解专家，解析画面、文字、表格 | `token` |
| `task_expert` | 任务专家，两段式：**规划**（生成方案并请求审批）→ **执行**（产出指令） | `approval_required` / `task_execute` |
| `examine_expert` | 人类审批（HITL），`interrupt()` 挂起等待前端决定 | `approval_required` / `task_rejected` |

要点：

- `task_expert` 的规划结果是**内部 JSON**，不会作为 `token` 推给用户（否则用户会看到裸 JSON），
  用户只看 `approval_required` 里的 `summary` / `steps`。
- 真正的副作用（删好友、改关系等）由 **Qt 客户端**收到 `task_execute` 后执行，服务端只负责
  决策与留痕。
- 审批挂起后即使前端断开、后端重启，只要 `thread_id` 不变，仍可通过 `/approve` 续上。

---

## 2. 接口

### `GET /health`
探活。Qt 启动时轮询，返回 `{"status":"ok"}` 即表示后端可用。

### `POST /chat`
提交一条用户消息，返回 SSE 流。

```json
{
  "message": "帮我把好友张三删掉",
  "thread_id": "会话标识，首次可为空，服务端会自动生成并通过 session 事件返回",
  "images": ["base64 或 http(s) 图片地址，最多 4 张"]
}
```

`message` 与 `images` 至少提供一个，否则返回 422。

### `POST /approve`
对挂起的任务表态。

```json
{ "thread_id": "http-1", "approved": true, "note": "确认删除" }
```

`approved=false` 会终止该任务，不会执行任何操作。

---

## 3. SSE 事件约定

每个事件形如 `data: {json}\n\n`，心跳是 SSE 注释行 `: ping\n\n`（客户端必须忽略）。
后端每 5 秒发一次心跳，避免模型思考/联网搜索期间的「安静」被客户端误判为超时。

| `event` | 载荷 | 说明 |
| :-- | :-- | :-- |
| `session` | `thread_id` | 流的第一条，前端持久化这个 id |
| `token` | `agent`, `content` | 增量文本，`agent` 为 `chat_expert` 或 `imagine_expert` |
| `approval_required` | `summary`, `steps`, `command` | 需要弹确认框，用户选择后调 `/approve` |
| `task_execute` | `command` | 已批准，客户端据此执行真实操作 |
| `task_rejected` | — | 已拒绝，任务终止 |
| `error` | `message` | 异常信息 |
| `done` | `agent` | 本轮结束，前端收尾 |

`command` 结构示例：

```json
{
  "action": "delete_friend",
  "params": { "name": "张三" },
  "risk": "high"
}
```

---

## 4. Qt 客户端接入要点

1. 用 `QNetworkAccessManager` 发 POST，连接 `readyRead`，把数据追加进缓冲区。
2. 按 `\n\n` 切分事件块，`data: ` 之后的部分解析成 JSON；以 `:` 开头的行直接丢弃。
3. 收到 `session` 保存 `thread_id`，后续同会话请求都要带上。
4. 收到 `approval_required` 弹出确认对话框，用户点「同意」→ `POST /approve {approved:true}`，
   点「取消」→ `POST /approve {approved:false}`。
5. 收到 `task_execute` 才真正改动本地数据。
6. 不要设置过短的低速超时；正常静默最长可能持续数十秒，靠 `: ping` 保活。

---

## 5. 配置

复制 `.env.example` 为 `.env` 并填写（`.env` 不要提交）：

| 变量 | 说明 |
| :-- | :-- |
| `DEEPSEEK_API_KEY` | 必填，模型密钥 |
| `DEEPSEEK_BASE_URL` | 必填，网关地址 |
| `DEEPSEEK_SEARCH_URL` | 联网搜索服务地址，缺失时搜索工具会明确返回未配置 |
| `DEEPSEEK_MODEL` | 主模型，默认 `deepseek-flash` |
| `DEEPSEEK_VISION_MODEL` | 图片理解用的模型，留空则复用主模型 |
| `AGENT_HOST` / `AGENT_PORT` | 监听地址，默认 `127.0.0.1:8000`（只本机可连） |

> 注意：本网关**不支持** `response_format=json_schema`，因此路由与任务规划都是
> 「纯文本输出 + 本地解析」，不要改回 `with_structured_output`。

---

## 6. 运行

```bash
uv sync
uv run python run_server.py     # 或 uv run python -m main.server（需先设置 PYTHONPATH=src）
```

仅单进程运行：checkpointer 复用同一条 SQLite 连接，不要开多 worker。

---

## 7. 目录

```
agent/
├── run_server.py            # 入口：把 src/ 加入 sys.path 后启动 uvicorn
├── pyproject.toml / uv.lock
└── src/
    ├── main/
    │   ├── app.py           # FastAPI 路由：/health /chat /approve + SSE 心跳
    │   ├── agent.py         # LangGraph 建图、专家节点、流式事件抽取
    │   ├── config.py        # 路径与环境变量（共享，避免循环导入）
    │   ├── server.py        # uvicorn 启动封装
    │   └── tool.py          # 联网搜索工具
    └── resources/           # 运行时数据（SQLite 会话库等，不入库）
```
