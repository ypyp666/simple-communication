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

---

## 功能概览

**v1.0 已实现：**
- 账号密码登录（异步 TCP + 动态等待动画 + 30 秒超时检测）
- 好友列表与聊天消息气泡（发送中 / 成功 / 失败状态指示）
- 消息失败重发、断线自动重连、离线消息拉取
- 消息本地持久化（多线程异步读写）
- 输入内容按联系人独立记忆

**规划中（Roadmap）：**
- 🔍 消息 / 联系人全文搜索
- 👥 联系人管理（添加 / 删除 / 分组）
- 🤖 内嵌 Agent 智能助手
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