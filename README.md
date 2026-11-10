# ChatApllication — Qt 桌面即时通讯客户端（v1.0）

基于 **Qt 6 + C++17** 开发的跨平台桌面即时通讯客户端，完整打通了登录鉴权、好友体系、消息收发与本地持久化，并内嵌了一个带**人机审批**的 AI 智能助手（服务端为 Python + LangGraph）。

本项目定位为**可扩展的聊天应用框架**：客户端、C++ 服务端、Python 智能体三层解耦，新增功能沿用「信号 → `MainBackend` 路由 → 分发」的模式即可接入。

---

## 核心能力与技术点

- **多线程数据库架构**：数据库读写跑在独立 `QThread` 后台线程，通过 `moveToThread` + 跨线程 `QueuedConnection` 实现，UI 线程零阻塞。
- **信号槽解耦设计**：UI 与后端职责分离，统一经 `MainBackend` 胶水层做信号路由；`TcpClient` 收到的 TCP 数据按协议类型分发给各后端模块，降低耦合、便于新增模块。
- **网络通信协议**：TCP + JSON 帧，以 `\n` 作为包分隔符解决**粘包/半包**问题；一条连接复用登录、聊天、好友、智能体四类业务。
- **内嵌 AI 智能体**：多会话切换 / 重命名、流式回答（30ms 合流 + Markdown 渲染）、Supervisor 多专家路由；涉及改数据的操作走**人机审批（HITL）**，用户点「同意」后才真正执行。
- **好友体系**：账号搜索、30 字留言申请、同意 / 拒绝、备注、删除（服务器确认成功后才删本地）。
- **消息可靠性**：发送时用 `tempId` 关联服务器确认，失败支持重发；断线自动重连，离线消息由服务器缓存经 `repost`/`pull_response` 协议补齐并回 `ACK`。
- **本地优先写入**：自己发出的消息先落本地库（临时 ID）→ 服务器回正式 ID → 替换本地主键，断网或重启都不会丢。
- **本地数据**：SQLite 持久化，按账号分库（`messages_<account>.db`），消息采用 `(contact_id, id)` 复合主键，支持分页加载。

---

## 技术栈

| 层面 | 选型 |
|------|------|
| 客户端语言 | C++17 |
| UI 框架 | Qt 6.11 (Core / Widgets / Network / Sql / Svg) |
| 客户端数据库 | SQLite（Qt Sql 封装，按账号分库） |
| 通信 | TCP + JSON（`\n` 分帧） |
| 服务端 | C++17，自研 TCP 服务（`8899`） |
| 智能体 | Python + FastAPI + LangGraph（`9000`，SSE 流式 + SQLite checkpointer） |
| 智能体业务库 | MySQL（存储过程查好友 / 聊天记录） |
| 构建 | CMake + MinGW 13.1.0 |

---

## 界面截图

### 登录与账号

**登录页** — 账号 / 密码登录，二者均非空时才可点击登录按钮

![登录页](screenshots/login.png)

**登录中** — 异步 TCP 登录 + 动态等待动画，30 秒无响应触发超时检测

![登录中](screenshots/logining.png)

**注册页**

![注册页](screenshots/regist.png)

**修改密码页**

![修改密码页](screenshots/modifyPwd.png)

### 消息收发

**消息气泡** — 接收（左）/ 发送（右）分列显示，带时间戳；会话列表显示未读红点

![消息气泡](screenshots/message1.png)

**会话消息记录** — 切换联系人后各自的独立记录，输入内容按联系人分别记忆

![会话消息记录](screenshots/message2.png)

**双端互通 · 发送** — 两个客户端同时在线，一端发送

![双端发送](screenshots/send1.png)

**双端互通 · 接收** — 另一端实时收到，消息落库后可回看

![双端接收](screenshots/send2.png)

### 添加好友

**搜索并申请** — 搜索页输入对方账号，结果卡片显示头像 / 昵称 / 账号；点「添加」不直接发请求，先弹出申请小窗填写留言（限 30 字，带实时字数），确认后才发送

![搜索并申请](screenshots/addFriend1.png)

**申请已发送** — 发送成功后在搜索框下方给出绿色提示「好友申请已发送，等待对方验证」

![申请已发送](screenshots/addFriend2.png)

**重复申请拦截** — 对同一账号再点「添加」，提示「已存在待处理的好友申请」，不会重复发请求

![重复申请拦截](screenshots/addFriend3.png)

**未读提醒** — 收到新申请时，左侧通讯录图标出现红色角标

![未读提醒](screenshots/addFriend4.png)

**处理申请** — 联系人页 →「新朋友」列出收到的申请：申请人、申请时间、留言，右侧「同意 / 拒绝」；自己发出的申请则显示「等待验证」

![处理申请](screenshots/addFriend5.png)

**成为好友** — 同意后服务器建好好友关系，双方通讯录互相出现对方；「新朋友」列表回到空态「暂无新的朋友申请」

![成为好友](screenshots/addFriend6.png)

### 联系人管理

**好友详情** — 点通讯录里的好友，右栏展示头像、用户名、用户 ID 与备注；右上角「修改备注」（就地编辑，改完变「保存备注」），底部「发送消息」直达会话、「删除好友」

![好友详情](screenshots/friendDetail.png)

**删除好友** — 详情页「删除好友」与列表右键菜单是同一个动作：先发请求给服务器，确认成功后才删本地联系人（失败则本地不动）；对方那一端同样会把该联系人移除

![删除好友](screenshots/friendDelete.png)

### 智能体会话

**新建会话** — 左侧竖栏第二个图标进入智能助手，会话列表首个标签为「新会话」（右侧铅笔重命名、× 关闭）；输入框为空时「发送」按钮置灰

![新建会话](screenshots/agent1.png)

**重命名会话** — 点铅笔就地编辑标题（例：测试会话1）

![重命名会话](screenshots/agent2.png)

**标题同步** — 改完左侧标签与右侧标题栏同步更新

![标题同步](screenshots/agent3.png)

**空状态新建** — 关闭全部会话后，中间只剩一个蓝色「新建会话」按钮

![空状态新建](screenshots/agent4.png)

**多会话对话** — 每个会话各自独立、可并行开多个，助手自称「小曦」

![多会话对话](screenshots/agent5.png)

**流式长回答 + Markdown 渲染** — 长篇回答逐段流式追加（30ms 合流一批，不逐字重排）；`#` 标题、`**加粗**`、有序 / 无序列表、分隔线都按 Markdown 渲染成富文本，不会把标记符原样显示出来

![流式长回答](screenshots/agent6.png)

**人机审批** — 「帮我给张三发个消息」这类会改动数据的操作不会直接执行，先弹「需要你的确认」卡片：操作 / 对象（张三，账号 12345）/ 内容 / 时间 / 风险（中）+ 拟稿理由，由用户选「拒绝」或「同意」

![人机审批](screenshots/agent7.png)

**确认后执行** — 点「同意」后客户端才真正把消息发给好友，聊天页里能看到发出的原文

![确认后执行](screenshots/agent8.png)

---

## 功能概览

**v1.0 已实现：**
- 账号密码登录（异步 TCP + 动态等待动画 + 30 秒超时检测）
- 好友搜索与添加（账号搜索 → 30 字留言申请 → 对方同意 / 拒绝 → 双向入通讯录）
- 内嵌 Agent 智能助手（多会话切换 / 重命名、流式回答 + Markdown 渲染、人机审批后执行真实操作）
- 好友详情（用户名 / 用户 ID / 备注）、修改备注、删除好友（服务器确认后本地才删）
- 好友列表与聊天消息气泡（发送中 / 成功 / 失败状态指示）
- 消息失败重发、断线自动重连、离线消息拉取
- 消息本地持久化（多线程异步读写）
- 输入内容按联系人独立记忆

**规划中（Roadmap）：**
- 🔍 消息 / 联系人全文搜索
- 👥 联系人分组 / 批量管理
- 文件 / 图片消息传输
- 端到端加密

> Roadmap 为方向性规划，随开发进度调整，不构成交付承诺。

---

## 项目结构

```
├── src/                # UI 层（MainWindow / ChatWindow / ContactList / ChatArea /
│                       #         AgentWindow / AgentChatArea / AgentMessageItem / AgentApprovalCard ...）
├── include/            # UI 层头文件
├── backend/            # 后端逻辑（TcpClient / LoginBackend / ChatBackend /
│                       #          ContactBackend / AgentBackend / DatabaseManager）
├── include_backend/    # 后端头文件与共享数据结构（FeatureStructs.h）
├── server_backend/     # 服务端源码（C++，TCP 8899：账号 / 好友 / 消息 / 离线消息）
├── agent/              # 智能体服务端（Python + FastAPI + LangGraph，实现在 simple-communication/agent）
├── res/                # 资源文件（SVG 图标）
├── screenshots/        # README 截图
└── CMakeLists.txt      # 客户端构建配置
```

---

## 架构设计

```
UI（src/）
   │  信号 / 槽
   ▼
MainBackend（协调中枢：TCP 回包按 type 统一路由，本地库副作用在此收口）
   ├── LoginBackend    —— 登录鉴权、登录超时检测
   ├── ChatBackend     —— 聊天消息收发、输入记忆、离线消息拉取
   ├── ContactBackend  —— 好友搜索 / 申请 / 审批 / 删除 / 备注
   ├── AgentBackend    —— 智能体会话、流式回答、人机审批
   ├── TcpClient       —— 一条 TCP 连接（按行切 JSON）、心跳检验、退避重连、多地址路由
   └── DatabaseManager —— SQLite 后台线程（QThread + 信号排队），按账号分库
```

**关键设计**

- **单连接复用**：登录、聊天、好友、智能体共用一条 TCP 连接，收包后按 `type` 字段分发到对应后端；新的功能模块只需要在 `MainBackend` 挂一个实例，沿用"信号 → 路由 → 分发"的模式接入。
- **传输格式**：换行分隔 JSON（NDJSON）。服务端 `server_backend` 监听 `8899`，智能体服务 `agent/` 监听 `9000`，两者通过内部协议对接。
- **本地优先落库**：自己发出的消息先写本地库（临时 ID）→ 服务器回正式 ID → 替换本地主键；智能体代发的消息同样落库并刷新会话列表，因此断网、重启都不会丢。
- **人机审批（HITL）**：涉及改数据的操作不直接执行，链路为 `agent_request` → `agent_approval_required` → `agent_approve` → `agent_approve_ack` → `agent_task_execute`，批准前不产生任何副作用。
- **流式渲染**：同一 `messageId` 的增量累积进同一个气泡；增量先进 30ms 合流缓冲，到点再整篇 Markdown 重渲染（`QTextDocument::setMarkdown`），避免逐字重排卡 UI。
- **数据库**：`build/database/<账号ID>/messages_<账号ID>.db`，表为 `messages` / `contacts` / `conversations` / `agent_sessions` / `agent_messages`（智能体消息按 `chunk_seq` 分块存）。所有 SQL 在 DB 线程执行，主线程只发请求信号。
- **会话 ID 规则**：智能体会话 ID 由客户端生成，必须是纯数字（≤10 位、非 0），服务器按数字解析。


---

## 环境要求与构建

**环境要求：**
- Qt 6.11.1（mingw_64）
- MinGW 13.1.0
- CMake ≥ 3.16

**构建运行：**

```bash
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH=C:/Qt/6.11.1/mingw_64
cmake --build build
./build/ChatUI.exe
```

**运行前置（服务端）：**

客户端只负责界面、协议和本地库，登录 / 好友 / 消息 / 智能体都要有服务端在跑：

- **C++ 服务器**（`server_backend/`，TCP `8899`）：客户端按地址表依次尝试 `192.168.20.128:8899`（开发机虚拟机）→ `47.93.77.173:8899`（云服务器），都不通就一直退避重连。
- **Python 智能体**（`agent/`，监听 `9000`）：先复制 `agent/.env.example` 为 `.env`，填模型 API Key（`DEEPSEEK_*`）和业务库连接（`AGENT_DB_*`，智能体查好友、拉聊天记录都走它）。不配也能启动，但智能体只会闲聊、无法执行真实操作。

---

## License

本项目为个人学习 / 就业展示项目，仅供学习参考。