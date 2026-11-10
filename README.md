# ChatApllication — Qt 桌面即时通讯客户端（v1.0）

基于 **Qt 6 + C++17** 开发的跨平台桌面即时通讯客户端，完整打通了从登录鉴权、好友聊天、消息本地持久化，到断线自动重连与离线消息拉取的闭环流程。

本项目定位为**可扩展的聊天应用框架**。当前 v1.0 已实现核心聊天链路，并在架构上为后续功能（搜索、联系人管理、内嵌 Agent 等）预留了清晰扩展位。

---

## 核心能力与技术点

- **多线程数据库架构**：数据库读写跑在独立 `QThread` 后台线程，通过 `moveToThread` + 跨线程 `QueuedConnection` 实现，UI 线程零阻塞。
- **信号槽解耦设计**：UI 与后端职责分离，统一经 `MainBackend` 胶水层做信号路由；`TcpClient` 收到的 TCP 数据按协议类型分发给各后端模块，降低耦合、便于新增模块。
- **网络通信协议**：TCP + JSON 帧，以 `\n` 作为包分隔符解决**粘包/半包**问题。
- **消息可靠性**：发送时用 `tempId` 关联服务器确认，失败支持重发；断线自动重连，离线消息由服务器缓存经 `repost`/`pull_response` 协议补齐并回 `ACK`。
- **本地数据**：SQLite 持久化，按账号分库（`messages_<account>.db`），消息采用 `(contact_id, id)` 复合主键，支持分页加载。

---

## 技术栈

| 层面 | 选型 |
|------|------|
| 语言 | C++17 |
| UI 框架 | Qt 6.11 (Core / Widgets / Network / Sql / Svg) |
| 数据库 | SQLite（Qt Sql 封装） |
| 通信 | TCP + JSON（`\n` 分帧） |
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

**v1.5 已实现：**
- 账号密码登录（异步 TCP + 动态等待动画 + 30 秒超时检测）
- 好友搜索与添加（账号搜索 → 30 字留言申请 → 对方同意 / 拒绝 → 双向入通讯录）
- 内嵌 Agent 智能助手（多会话切换 / 重命名、流式回答、人机审批后执行真实操作）
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
├── src/                # UI 层（MainWindow / ChatWindow / ContactList / ChatArea / ...）
├── include/            # UI 层头文件
├── backend/            # 后端逻辑（TcpClient / ChatBackend / LoginBackend / DatabaseManager）
├── include_backend/    # 后端头文件与共享数据结构（FeatureStructs.h）
├── res/                # 资源文件（SVG 图标）
└── CMakeLists.txt      # 构建配置
```

---

## 架构设计

```
UI（src/）
   │  信号/槽（通过 MainBackend 胶水层转发）
   ▼
MainBackend（协调中枢：信号统一路由，避免各后端重复监听共享信号）
   ├── LoginBackend    —— 登录鉴权、登录超时检测
   ├── ChatBackend     —— 聊天消息收发、输入记忆、离线拉取
   ├── TcpClient       —— TCP 连接、粘包/半包缓冲处理
   └── DatabaseManager —— SQLite 后台线程持久化（单例）
```

- **扩展方式**：新增功能时，在 `ChatBackend` / `MainBackend` 各加一个模块实例，沿用"信号 → MainBackend 路由 → 分发"的模式即可接入，无需改动 UI 层既有结构。

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

---

## License

本项目为个人学习 / 就业展示项目，仅供学习参考。
