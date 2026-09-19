#include "ChatWindow.h"
#include "MainBackend.h"
#include <QFrame>
#include <QDateTime>
#include <QFileInfo>

// ========================= Qt 布局核心笔记 =========================
// 1. 布局方向极其直观：
//    QHBoxLayout 水平布局 = 子控件从左到右排列
//    QVBoxLayout 垂直布局 = 子控件从上到下排列
//    addWidget顺序 = 界面显示顺序

// 2. 自适应布局精髓（最强大、最省心）：
//    固定控件尺寸 + 其余自动占满剩余空间
//    例：左侧联系人 setFixedWidth(280) → 右侧聊天面板自动拉伸填满窗口
//    窗口缩放时，布局自动适配，控件不会变形、错乱

// 3. 布局使用黄金流程：
//    ① 创建布局（水平/垂直）
//    ② addWidget 添加子控件
//    ③ 固定需要固定的宽/高
//    ④ 其余控件自动拉伸适配
//    ⑤ 无需手动计算坐标、分辨率、比例

// 4. 优势：
//    比Web前端的Grid/Flex更简单直观
//    真正的自动响应式，一行代码实现多设备适配
// ====================================================================

ChatWindow::ChatWindow(QWidget *parent, MainBackend* backendPtr) : QWidget(parent), backend(backendPtr)
{
    mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);//设置布局的内边距上下左右为0
    mainLayout->setSpacing(0);

    contactList = new ContactList(this);
    contactList->setFixedWidth(280);
    contactList->setStyleSheet(R"(/*用了R“()”后在括号内可以让你能够按照CSS/HTML一样写样式不用转义字符串*/
    ContactList {
        background-color: rgba(245, 245, 245, 0.43);
        border-right: 1px solid rgba(224, 224, 224, 0.58);/*设置右边分割线*/
        border-radius: 0px;/*圆角*/
    }
)");
    mainLayout->addWidget(contactList);

    // QFrame：带边框/面板的基础容器（QWidget子类）
    // 核心用途：1. 给控件加边框/背景/3D效果；2. 视觉分组（包一组控件）；3. 快速做水平/垂直分隔线
    // 常用组合：setFrameStyle(形状|阴影) + setLineWidth(边框宽度)
    // 子类包括QLabel、QTextEdit等，是Qt界面美化/分组的基础控件
    //在这里使用QFrame创建一个垂直分隔线
    // 在 MainWindow.cpp 中
    QFrame* divider = new QFrame(this);
    divider->setFrameShape(QFrame::VLine);  // 垂直分隔线
    divider->setStyleSheet("color: #e0e0e0;");
    mainLayout->addWidget(divider);

    chatPanel = new QWidget(this);
    chatPanelLayout = new QVBoxLayout(chatPanel);
    chatPanelLayout->setContentsMargins(0, 0, 0, 0);
    chatPanelLayout->setSpacing(0);
    mainLayout->addWidget(chatPanel,1);//把聊天面板添加到水平布局中，有三个参数，第一个是控件，第二个是权重，第三个是对齐方式
    //权重：子控件在布局中的权重，权重越大，子控件的宽度就越大（弹簧系数）
    //对齐方式：子控件在布局中的对齐方式，这里设置为居中对齐

    chatArea = new ChatArea(this);
    chatPanelLayout->addWidget(chatArea);

    // 主后端对象由外部传入（main.cpp 创建全局实例），ChatWindow 不负责创建/销毁
    if (!backend) {
        qWarning("ChatWindow: backend is null!");
    }

    // 初始化当前账号ID（从主后端获取登录成功的真实账号，不再硬编码"self"）
    currentAccountId = backend->currentUserId();
    backend->setCurrentAccountId(currentAccountId);  // 异步：后台线程打开对应账号消息库，不阻塞UI

// ===================== 信号槽连接：整个聊天软件的"神经网络" =====================
// 规则：connect( 信号发送者, 发送的信号, 信号接收者, 处理的槽函数 )
// 作用：让前端UI与后端逻辑自动联动，实现解耦、自动响应

/*connect(
    谁发出信号 ,    // A：信号发送者（对象指针）
    发什么信号 ,    // B：具体信号（函数地址）
    谁来接收处理 ,  // C：信号接收者（通常是 this，主窗口）
    用哪个函数处理  // D：槽函数（你自己写的 private slots）
);
这个广播机制是用于本地通信的也就是使用桌面开发本地前端 UI和后端逻辑处理是在一个程序运行的
如果要转服务器
// Qt 信号槽：仅用于【同一程序内部】通信，不能跨机器/跨服务器
// 前后端接口（sendMessage/contactSelected等）：【通用不变】
// 未来连接服务器：
//   1. 前端UI、信号槽、MainWindow完全不用改
//   2. 只需要把 ChatBackend 从本地逻辑 → 改成 TCP/网络通信
//   3. 接口、函数、信号槽连接方式 100% 复用
*/

// 1. 后端加载完联系人 → 主窗口显示联系人列表
connect(backend, &MainBackend::contactsLoaded, this, &ChatWindow::onContactsLoaded);

// 2. 后端分页查询完消息 → 主窗口显示聊天记录（page/hasMore 用于翻历史）
connect(backend, &MainBackend::messagesPageLoaded, this, &ChatWindow::onMessagesPageLoaded);

// 2.5 聊天区滚到顶部 → 加载更早一页历史消息
connect(chatArea, &ChatArea::loadOlderMessages, this, &ChatWindow::onLoadOlderMessages);

// 3. 点击左侧联系人 → 主窗口切换当前聊天对象
connect(contactList, &ContactList::contactSelected, this, &ChatWindow::onContactSelected);

// 4. 输入框发送消息 → 主窗口交给后端发送（通过ChatArea转发）
connect(chatArea, &ChatArea::sendMessage, this, &ChatWindow::onSendMessage);

// 5. 输入框发送文件 → 主窗口交给后端处理（通过ChatArea转发）
connect(chatArea, &ChatArea::sendFile, this, &ChatWindow::onSendFile);

// 6. 用户点击失败感叹号 → 重发该消息
connect(chatArea, &ChatArea::retrySend, this, &ChatWindow::onRetrySend);

// 发送状态指示器接线（发送中已直接在 onSendMessage 里触发）：
// 后端发送超时/成功信号（由你编写，需携带本地消息ID，即 onSendMessage 里生成的 "new_msg_..."）
// 接上后即可让消息在超时时变红色感叹号、成功时隐藏旋转圈
// 7. 发送结果：失败→红色感叹号，成功→隐藏旋转动画（tempId即 onSendMessage 里生成的临时ID）
connect(backend, &MainBackend::messageSendFailed, this, &ChatWindow::onMessageSendFailed);
connect(backend, &MainBackend::messageSendSuccess, this, &ChatWindow::onMessageSendSuccess);

// 8. 后端收到对方新消息 → 显示到聊天区
connect(backend, &MainBackend::messageReceived, this, &ChatWindow::onMessageReceived);
// 9. 后端接收失败 → 触发拉取，UI提示（可选）
connect(backend, &MainBackend::messageReceiveFailed, this, &ChatWindow::onMessageReceiveFailed);
// 程序启动 → 立即加载联系人列表
backend->loadContacts();
// ============================================================================
}

ChatWindow::~ChatWindow()
{
    // backend 由外部（main.cpp 全局实例）持有，此处不删除
}

void ChatWindow::onContactsLoaded(const QList<ContactInfo>& contacts)
{
    contactList->setContacts(contacts);//槽函数接收到联系人列表，设置到联系人列表控件
}

void ChatWindow::onMessagesPageLoaded(const QString& contactId, int page,
                                      const QList<MessageInfo>& messages, bool hasMore)
{
    // 结果比对：查询是异步的，快速切换联系人时，上一个联系人的慢结果
    // 可能比新联系人的还晚回来——不是当前联系人的直接丢弃
    if (contactId != currentContactId) {
        return;
    }

    if (page <= 1) {
        // 首屏（最新一页）：清空重放，addMessage 内部会滚到底部
        chatArea->setMessages(messages);
    } else {
        // 翻历史：前插到顶部并保持滚动位置；解除"加载中"标志允许继续往上翻
        chatArea->setLoadingOlder(false);
        chatArea->prependMessages(messages);
    }

    m_currentPage = page;
    m_hasMore = hasMore;   // 后端说没了就到头了，滚顶信号会被 onLoadOlderMessages 挡掉
}

// 聊天区滚到顶部 → 请求更早一页。
// 三个前置条件：有联系人、首屏已经加载过（m_hasMore 才有效）、历史还没翻到底
void ChatWindow::onLoadOlderMessages()
{
    if (currentContactId.isEmpty() || !m_hasMore) {
        return;
    }
    // 置"加载中"：请求往返期间滚动条可能多次到顶，挡住重复请求
    chatArea->setLoadingOlder(true);
    backend->loadMessages(currentContactId, m_currentPage + 1);
}

void ChatWindow::onContactSelected(const QString& contactId, const QString& contactName)
{
    // 切换前保存当前联系人的输入内容
    if (!currentContactId.isEmpty()) {
        QString inputContent = chatArea->getInputContent();
        backend->saveInputContent(currentContactId, inputContent);
    }
    
    currentContactId = contactId;//保存当前好友ID
    currentContactName = contactName;//保存当前好友名称
    chatArea->setContactName(contactName);//设置到聊天区域控件
    chatArea->setInputVisible(true);  // 选择联系人后显示输入框
    
    // 恢复新联系人的输入内容
    QString savedContent = backend->getInputContent(contactId);
    chatArea->setInputContent(savedContent);

    // 分页状态复位：先关 m_hasMore 再发请求——切换瞬间聊天区内容被清空，
    // 滚动条归零会触发 loadOlderMessages，不关的话会拿新联系人的 ID 去翻旧页码
    m_currentPage = 1;
    m_hasMore = false;
    backend->loadMessages(contactId, 1);//加载该联系人最新一页消息（50条），翻历史由滚顶触发
}

void ChatWindow::onSendMessage(const QString& content)
{
    if (currentContactId.isEmpty()) return;

    MessageInfo message;
    message.id = QString::number(QDateTime::currentMSecsSinceEpoch());
    message.accountId = currentAccountId;
    message.contactId = currentContactId;
    message.senderId = currentAccountId;
    message.targetId = currentContactId;
    message.content = content;
    message.sendTime = QDateTime::currentDateTime();
    message.isSelf = true;
    message.isRead = true;   // 自己发的消息，已读状态无意义
    message.isFile = false;
    message.isOffline = false;
    backend->sendMessage(message);  // MainBackend 内部已调用 saveMessage，不再重复
    m_pendingMessages.insert(message.id, message);  // 记录待确认消息，超时后可重发
    chatArea->addMessage(message);
    // 显示发送等待动画（气泡左侧旋转圆圈），后端超时/成功信号到达后再切换
    chatArea->setMessageStatus(message.id, MessageStatusIndicator::Sending);

       
    // 发送成功后清除记忆的输入内容
  
}

// 后端发送失败信号到达 → 对应消息切换为红色感叹号
void ChatWindow::onMessageSendFailed(const QString& messageId, const QString& serverId)
{
    Q_UNUSED(serverId);  // 失败时不更新数据库，只变感叹号
    chatArea->setMessageStatus(messageId, MessageStatusIndicator::Failed);
}

// 后端发送成功信号到达 → 隐藏旋转动画，用服务器ID更新本地消息
void ChatWindow::onMessageSendSuccess(const QString& messageId, const QString& serverId)
{
    // 从缓存取出消息自身的 contactId（不能用 currentContactId，用户可能已切换联系人）
    MessageInfo message = m_pendingMessages.take(messageId);
    backend->onDbMessageIdUpdated(message.contactId, messageId, serverId);
    chatArea->setMessageStatus(messageId, MessageStatusIndicator::None);
    // take 已经移除，不用再 remove
}

// 用户点击失败感叹号 → 把重试请求连同功能枚举交给主后端路由（气泡固定 MessageSend → ChatBackend），
// 并恢复发送中动画
void ChatWindow::onRetrySend(const QString& messageId, LoginFeature feature)
{
    auto it = m_pendingMessages.find(messageId);
    if (it == m_pendingMessages.end()) {
        return;
    }
    backend->onRetryRequested(feature, messageId, it.value());
    chatArea->setMessageStatus(messageId, MessageStatusIndicator::Sending);
}

// 后端收到对方新消息 → 显示到聊天区（回ACK已由 MainBackend 处理）
void ChatWindow::onMessageReceived(const MessageInfo& message)
{
    // 拷贝一份，避免改 const 引用的原始数据；拷贝是值类型深拷贝，安全
    MessageInfo msg = message;
    if (msg.contactId == currentContactId) {
        msg.isRead = true;          // 正在看当前联系人 → 标记已读
        chatArea->addMessage(msg);  // 显示气泡
    }
    // 当前联系人=已读(true)，非当前=未读(false)（留给未读计数用）
    // 用本地已读状态重新存库，覆盖 MainBackend 里 isRead=false 的那次
    backend->saveMessage(msg);
}

// 后端接收失败 → 触发拉取，UI提示（可选，你来补具体提示）
void ChatWindow::onMessageReceiveFailed(const QString& serverId)
{
    Q_UNUSED(serverId);
    // TODO: 提示用户消息接收失败，正在拉取…
}

void ChatWindow::onSendFile(const QString& filePath)
{
    if (currentContactId.isEmpty()) return;

    QFileInfo fileInfo(filePath);

    MessageInfo message;
    message.id = "new_file_" + QString::number(QDateTime::currentMSecsSinceEpoch());
    message.accountId = currentAccountId;
    message.contactId = currentContactId;
    message.senderId = currentAccountId;
    message.targetId = currentContactId;
    message.fileName = fileInfo.fileName();
    message.filePath = filePath;
    message.fileSize = fileInfo.size();
    message.sendTime = QDateTime::currentDateTime();
    message.isSelf = true;
    message.isRead = true;   // 自己发的消息，已读状态无意义
    message.isFile = true;
    message.isOffline = false;

    chatArea->addMessage(message);
    backend->sendFile(currentContactId, filePath);
    backend->saveMessage(message);  // 异步保存到数据库（后台线程执行，不阻塞UI）
}