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
                                            ▲           └───┬───────┬───┘
                                            │ 批准后回到执行阶段 │       │ 拒绝
                                            └─────────────────┘       │
                                                                      ▼
                                                             ┌────────────────┐
                                                             │  report_expert │──► END
                                                             └────────────────┘
                                                              执行完也回到这里再说一句
```

| 节点 | 职责 | 输出给用户的事件 |
| :-- | :-- | :-- |
| `supervisor` | LLM 意图分类，只决定交给谁，不产出文本 | 无（内部节点） |
| `chat_expert` | 情感/日常聊天专家，需要时调用联网搜索工具 | `token` |
| `imagine_expert` | 图片理解专家，解析画面、文字、表格 | `token` |
| `task_expert` | 任务专家，两段式：**规划**（生成方案并请求审批）→ **执行**（产出指令） | `approval_required` / `task_execute` |
| `examine_expert` | 人类审批（HITL），`interrupt()` 挂起等待前端决定 | `approval_required` / `task_rejected` |
| `report_expert` | 任务链路收尾播报：执行完 / 取消后都用一句人话交代结果 | `token` |

要点：

- `task_expert` 的规划结果是**内部 JSON**，不会作为 `token` 推给用户（否则用户会看到裸 JSON），
  用户只看 `approval_required` 里的 `summary` / `steps`。
- 真正的副作用（删好友、改关系等）由 **Qt 客户端**收到 `task_execute` 后执行，服务端只负责
  决策与留痕。
- 任务链路不会静默结束：批准执行后、用户取消后，都会经过 `report_expert` 说一句
  「干了什么 / 现在到哪一步」。它只能说自己「已下发」或「已取消」，**不会声称执行成功**，
  真实结果由客户端执行后自行反馈。
- 事件顺序固定为：`task_execute` / `task_rejected` → `token`(`report_expert`) → `done`，
  服务端会把播报先攒住，避免客户端先看到「已下发」再收到指令。
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
  "images": ["base64 或 http(s) 图片地址，最多 4 张"],
  "user_id": 1001
}
```

`message` 与 `images` 至少提供一个，否则返回 422。

**`user_id` 是当前登录用户的账号ID**，工具查库时要用（比如查好友必须知道「谁的」好友）。
两条来源，显式的优先：

1. 请求里带 `user_id`（推荐，契约清晰）；
2. 没带就从 `thread_id` 解析 —— 约定 `thread_id = 会话id + "n" + user_id`，
   取最后一个 `n` 右边、且必须是纯数字，否则当作没给。

两条都拿不到时，需要用户身份的工具会返回一条明确的失败文案，**不会**随便猜一个 id 去查库。

### `POST /approve`
对挂起的任务表态。

```json
{ "thread_id": "http-1", "approved": true, "note": "确认删除", "user_id": 1001 }
```

`approved=false` 会终止该任务，不会执行任何操作。`user_id` 规则同 `/chat`。

---

## 3. SSE 事件约定

每个事件形如 `data: {json}\n\n`，心跳是 SSE 注释行 `: ping\n\n`（客户端必须忽略）。
后端每 5 秒发一次心跳，避免模型思考/联网搜索期间的「安静」被客户端误判为超时。

| `event` | 载荷 | 说明 |
| :-- | :-- | :-- |
| `session` | `thread_id` | 流的第一条，前端持久化这个 id |
| `token` | `agent`, `content` | 增量文本，`agent` 为 `chat_expert` / `imagine_expert` / `report_expert` |
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

### 动作白名单在哪加

`action` 的取值不是随便的字符串，必须存在于 `src/main/tool.py` 的 `TASK_ACTIONS` 里，
否则 `normalize_task_command()` 会直接把整条指令作废（返回 `{}`），不会传到客户端。

Qt 端新增一个可执行操作时，要同步做两件事：

1. 在 `tool.py` 的 `TASK_ACTIONS` 加一项：

   ```python
   "动作名": {
       "desc": "说明",
       "params": ["必填参数"],
       "chain": ["search_friend"],   # 可选：执行前必须先调的工具，按顺序
   }
   ```

2. 在 Qt 端的执行器里实现同名分支。

服务端不需要再改别的地方——`TASK_PROMPT` 里的动作清单是由 `list_task_actions()` 现算的，
`chain` 会被自动渲染成「执行前先按顺序调用 …」。工具的详细说明永远只写在工具自己的
docstring 里，**不要为了新工具去改提示词**。

### 工具怎么挂

工具全部写在 `tool.py`，靠文件末尾的三个注册表决定谁能用：

| 注册表 | 使用者 | 现有内容 |
| :-- | :-- | :-- |
| `CHAT_TOOLS` | 聊天专家 | `web_searchquery` |
| `VISION_TOOLS` | 图片理解专家 | 空 |
| `TASK_TOOLS` | 任务专家（规划阶段） | `search_friend` |

任务专家跑的是手动工具循环（`_plan_with_tools`），最多 `MAX_TOOL_ROUNDS` 轮，
最后强制收敛成纯文本 JSON，所以工具再调用也不会把规划结果截断。

### 好友消息恢复：三步链路

目标是「帮某位好友恢复/转发消息」。拆成三个工具，而不是一个大工具：

| 步骤 | 工具 | 状态 |
| :-- | :-- | :-- |
| 1 | `search_friend` 查好友账号 | 已实现 |
| 2 | `get_chat_history` 拉聊天记录 | 待实现（`tool.py` 里有约定说明） |
| 3 | `forward_message` 把内容交给服务器转发 | 待实现，有副作用、必须走审批 |

拆开的理由：第 1、2 步只读，可以放心让模型自己调；第 3 步会真的把消息发出去，
必须挂在 `examine_expert` 后面。合成一个大工具的话整条链都得审批，
而且失败时只剩一句「失败」，分不清是没查到人还是没发出去。

`search_friend` 走存储过程 `Agent_search_user(in_user_id, in_target_name, out retcode)`：

- `retcode = 1` 查到，返回 `(target_id, user_name, mark)` 若干行；查到多人时
  工具会把候选列出来并要求模型先问用户，**不允许模型自己挑一个**。
- `retcode = 2` 没这个人。
- `retcode = -1` SQL 出错。

业务库账号只被授予存储过程的 `EXECUTE` 权限，**没有表的 `SELECT` 权限**，
所以工具里一律走 `CALL`，不要改成自己拼 `SELECT`。

---

## 4. Qt 客户端接入要点

1. 用 `QNetworkAccessManager` 发 POST，连接 `readyRead`，把数据追加进缓冲区。
2. 按 `\n\n` 切分事件块，`data: ` 之后的部分解析成 JSON；以 `:` 开头的行直接丢弃。
3. 收到 `session` 保存 `thread_id`，后续同会话请求都要带上。
4. 收到 `approval_required` 弹出确认对话框，用户点「同意」→ `POST /approve {approved:true}`，
   点「取消」→ `POST /approve {approved:false}`。
5. 收到 `task_execute` 才真正改动本地数据。
6. 执行完不用自己拼提示文案：后面会跟着 `token`（`agent="report_expert"`）的播报，
   直接用普通气泡渲染，末尾以 `done` 收尾。
7. 不要设置过短的低速超时；正常静默最长可能持续数十秒，靠 `: ping` 保活。

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
| `AGENT_DB_HOST` / `AGENT_DB_PORT` | 业务库地址，默认 `192.168.20.128:3306` |
| `AGENT_DB_USER` / `AGENT_DB_PASSWORD` | 业务库账号，**只需要存储过程 EXECUTE 权限**，必填 |
| `AGENT_DB_NAME` | 业务库名，默认 `User` |
| `AGENT_DB_CONNECT_TIMEOUT` | 连库超时秒数，默认 5，别设太大否则会卡住对话 |

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
    │   └── tool.py          # 所有工具 + 注册表(CHAT_TOOLS/...) + 动作白名单(TASK_ACTIONS)
    └── resources/           # 运行时数据（SQLite 会话库等，不入库）
```

## 8. 代码风格约定

- **所有工具都写在 `tool.py`**，靠文件末尾的 `CHAT_TOOLS` / `VISION_TOOLS` / `TASK_TOOLS`
  注册表暴露，`agent.py` 只负责挑哪个列表给哪个专家，不写工具实现。
- **工具的说明写在工具自己的 docstring 里，不要写进系统提示词。**
  模型的「工具描述」就是这段 docstring，一个工具能做什么、什么时候该调、返回什么、
  在链条里排第几、有什么限制，全部写在这里。提示词里只留一条通用规则
  （「细节看工具描述」），不点具体工具名。

  原因：工具一多，每个工具都在提示词里占一段，上下文很快被撑爆；
  而且同样的规则写两份，改一处漏一处就会互相打架。

  如果一个动作在输出指令前必须先调几个工具把数据准备好，把工具名按顺序写进
  `TASK_ACTIONS` 的 `chain` 字段即可，`list_task_actions()` 会自动渲染成
  「执行前先按顺序调用 …」，**同样不需要改提示词**。

- **不使用复杂语法糖**：不用列表/生成器推导式、不用三元表达式堆叠、不用 `**` 解包拼字典、
  不用链式 `or` 取默认值。统一写成显式 `for` 循环 + `if/else` 分支，宁可多几行。
- **工具一律写成 `async def`。** 下游的 `requests`、`pymysql` 都是同步阻塞库，
  直接在事件循环里等它们，一个用户的慢查询/慢搜索会把所有会话一起冻住——
  SSE 心跳发不出去，别人的 token 也不流了。

  做法固定：同步逻辑原样写成普通函数，工具里用 `await asyncio.to_thread(...)`
  丢进线程池，事件循环只负责等，等的过程中还能处理别的会话。

  ```python
  def call_search_user(own_id, target_name):   # 同步逻辑，别在事件循环里直接调
      ...

  @tool
  async def search_friend(target_name: str, config: RunnableConfig) -> str:
      retcode, rows = await asyncio.to_thread(call_search_user, own_id, target_name)
  ```

  注意 `asyncio.to_thread` 只是不占事件循环，底层操作本身取消不掉：
  超时要靠 `connect_timeout` / `timeout=` 兜底，不要指望 `wait_for` 能救。
- 一个工具只干一件事，参数拍平，不要嵌套结构。
- 没写完的工具不要加 `@tool`、不要放进注册表，模型看不到就调不到，也就不会乱承诺。
