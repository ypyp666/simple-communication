#include "ChatArea.h"
#include "MessageItem.h"
#include "ChatInput.h"
#include <QTimer>
#include <QScrollBar>

ChatArea::ChatArea(QWidget *parent) : QWidget(parent)
{
    mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    headerLabelWidget = new QWidget(this);
    headerLabelWidget->setStyleSheet(R"(
        background-color: white;
        border-radius: 5px;
        box-shadow: 0 4px 12px rgba(0, 0, 0, 0.3);
    )");
    
    headerLayout = new QHBoxLayout(headerLabelWidget);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(0);

    headerLabel = new QLabel("请选择一个联系人开始聊天", this);
    headerLabel->setStyleSheet(R"(
        background-color: transparent;
        color: black;
        padding: 15px 20px;
        font-size: 16px;
        font-weight: 600;
    )");

    headerLayout->addWidget(headerLabel);

    mainLayout->addWidget(headerLabelWidget);

    scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet(R"(
        QScrollArea {
            border: none;
            background-color: #e8eef3;
        }
        QScrollBar:vertical {
            width: 0px;
            background-color: transparent;
        }
        QScrollBar::handle:vertical {
            background-color: transparent;
            border-radius: 3px;
        }
        QScrollArea:hover QScrollBar:vertical {
            width: 6px;
        }
        QScrollArea:hover QScrollBar::handle:vertical {
            background-color: #c0c0c0;
        }
        QScrollArea:focus QScrollBar:vertical {
            width: 6px;
        }
        QScrollArea:focus QScrollBar::handle:vertical {
            background-color: #c0c0c0;
        }
    )");
    
    // ===== "输入框只向上扩"的机关，就在下面这两次 addWidget =====
    // 目标效果：输入框长高时【顶边抬升、底边贴死窗口底】（整体向上长），而不是向下顶。
    // 靠两件事，缺一不可：
    //   ① 顺序：scrollArea 在前、chatInput 在最后
    //      → chatInput 的底边锚在布局底部（= 窗口底），这个锚点不动
    //   ② stretch：scrollArea = 1（弹性块）、chatInput = 0（刚性块，默认）
    //      → 富余/紧张的空间都归 scrollArea 吸收
    // 机制：输入框从 4 行变 8 行时，"想要"的高度 +N，而窗口总高固定，
    //       这 N 只能从唯一弹性的 scrollArea 身上扣 → scrollArea 矮 N，
    //       chatInput 顶边上移 N、底边不动 → 视觉上就是"只向上扩"。
    // 反例：若把 chatInput 放在最前面（顶部），就变成"顶边不动、底边下移"= 向下扩。
    // 配套（都在 ChatInput 内部）：setMaximumHeight(225) 是高度上限，到顶后改框内滚动；
    //       框内三行的 rowStretch 保证只有输入框那行伸缩，上下两行不乱挤。
    mainLayout->addWidget(scrollArea, 1);

    messagesWidget = new QWidget();
    messagesLayout = new QVBoxLayout(messagesWidget);
    messagesLayout->setContentsMargins(0, 10, 0, 10);
    messagesLayout->setSpacing(0);
    messagesLayout->addStretch(1);

    scrollArea->setWidget(messagesWidget);

    // 创建输入框，作为聊天区域的一部分
    chatInput = new ChatInput(this);
    chatInput->setStyleSheet("background-color: white; border-top: 1px solid #e0e0e0;");
    chatInput->hide();  // 默认隐藏
    mainLayout->addWidget(chatInput);

    // 连接输入框信号到 ChatArea 信号
    connect(chatInput, &ChatInput::sendMessage, this, &ChatArea::sendMessage);
    connect(chatInput, &ChatInput::sendFile, this, &ChatArea::sendFile);

    // 滚动条到顶 → 请求加载更早一页历史消息。
    // m_loadingOlder 防重复：一次翻页期间滚动条可能多次停在顶上（插入前内容少时本来就在顶），
    // 不挡的话同一页会连发好几遍请求
    connect(scrollArea->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (value <= 0 && !m_loadingOlder) {//value小于等于0就是说明已经顶到顶了
            emit loadOlderMessages();
        }
    });
}

// 单条消息 → [气泡+时间标签] 对。追加（atTop=false，插在 stretch 前）和
// 前插（atTop=true，插在布局最前面）共用这一份，时间标签保持在气泡下方，顺序与原来一致
void ChatArea::appendMessageWidgets(const MessageInfo& message, bool atTop, int& frontIndex)
{
    MessageItem* item = new MessageItem(message, this);
    m_messageItems.insert(message.id, item);  // 记录 消息ID→Item，供发送状态切换使用
    // 消息项内点击失败感叹号 → ChatArea 转发重发请求
    connect(item, &MessageItem::retryRequested, this, &ChatArea::retrySend);

    QLabel* timeLabel = new QLabel(message.sendTime.toString("HH:mm"), this);
    timeLabel->setStyleSheet("color: #999; font-size: 11px;");
    timeLabel->setAlignment(Qt::AlignCenter);

    if (atTop) {
        // 前插：气泡插到本页的当前位置，时间标签紧跟其后（在气泡下方），页内按旧→新往后排，
        // 布局末尾的 stretch 不受影响，原有内容整体顺延
        messagesLayout->insertWidget(frontIndex, item);
        messagesLayout->insertWidget(frontIndex + 1, timeLabel);
        frontIndex += 2;  // 直接改调用方那个变量：气泡+时间标签占两个坑，下一对往后排
        return;
    }

    // 追加：都插在 stretch 之前（count()-1 是 stretch 的位置），追加到最新消息后面
    messagesLayout->insertWidget(messagesLayout->count() - 1, item);
    messagesLayout->insertWidget(messagesLayout->count() - 1, timeLabel);
}

void ChatArea::addMessage(const MessageInfo& message, bool scrollToBottom)
{
    int frontIndex = 0;  // 追加路径不走 frontIndex，占位传入
    appendMessageWidgets(message, false, frontIndex);

    if (!scrollToBottom) {
        return;  // 前插历史消息时不滚底（prependMessages 自己维护滚动位置）
    }

    QTimer::singleShot(10, this, [this]() -> void {
        QScrollBar* scrollBar = scrollArea->verticalScrollBar();//获取垂直滚动条
        if (scrollBar == nullptr) {
            return;
        }
        scrollBar->setValue(scrollBar->maximum());//滚动到最底部
    });//添加消息后，滚动到最底部，延迟10ms秒，确保消息添加完成后再滚动
}

// 把一页历史消息插到聊天区顶部（时间正序，旧→新，与 DatabaseManager::loadMessagesPage
// 反转后的顺序一致），并保持用户当前看到的滚动位置：
// 插入前记下"滚动条值 + 最大值"，插入后内容整体变高，把滚动条值加上"变高的增量"，
// 用户视觉上原地不动——否则插完会直接跳到新插入的内容上，像被人拽着往上翻
void ChatArea::prependMessages(const QList<MessageInfo>& messages)
{
    //QcrollBar的value: 滚动条当前位置视口顶边距离内容顶边的像素数
//value概念解释：视口是可视区域，内容是所有消息是一个超长的列表，滚动条值是视口顶部距离内容顶部的像素数，value=0是视口顶部在内容顶部,value=80是视口顶部距离内容顶部向下偏移了80像素
//maximum: 滚动条最大值，滚动条最大值是内容高度减去视口高度，就是可以向下滚动的最大值
//maximum是派生出来的每次布局都会重新计算,value是自由值是我们自己设置的
    QScrollBar* scrollBar = scrollArea->verticalScrollBar();//
    const int oldMax = scrollBar ? scrollBar->maximum() : 0;
    const int oldValue = scrollBar ? scrollBar->value() : 0;

    // 前插位置下标：放在"页"这一层（本函数一次调用 = 一页），局部变量天然每页从 0 开始，
    // 页内按旧→新依次往后排；跨页不会残留——若做成成员变量，第 3 页会被插到第 2 页块的下方，顺序反而全反了
    int frontIndex = 0;
    for (const auto& msg : messages) {
        appendMessageWidgets(msg, true, frontIndex);
    }

    if (scrollBar) {
        // 布局尺寸不是插入后立刻生效的，延迟到下一轮事件循环再校正滚动位置
        QTimer::singleShot(0, this, [this, oldMax, oldValue]() {
            QScrollBar* bar = scrollArea->verticalScrollBar();
            if (bar == nullptr) {
                return;
            }
            bar->setValue(bar->maximum() - oldMax + oldValue);
        });
    }
}
    
   /*
   []() {
    // 滚到底部
};Lambda 写法（现代 C++），[this] = 把当前类的 this 指针捕获进来，
作用：在函数里能访问当前类的成员变量（scrollArea、this、成员函数）
   */

void ChatArea::setMessages(const QList<MessageInfo>& messages)
{
    clearMessages();
    for (const auto& msg : messages) {
        addMessage(msg);
    }
}

// 按消息ID切换发送状态：转发给对应的消息气泡项
void ChatArea::setMessageStatus(const QString& messageId, MessageStatusIndicator::Status status)
{
    auto it = m_messageItems.find(messageId);
    if (it != m_messageItems.end()) {
        it.value()->setStatus(status);
    }
}

void ChatArea::setContactName(const QString& name)
{
    headerLabel->setText(name);
}

void ChatArea::clearMessages()
{
    QLayoutItem* item;
    while ((item = messagesLayout->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }
    m_messageItems.clear();  // 同时清除 消息ID→Item 映射，避免悬挂指针
    messagesLayout->addStretch(1);
}

void ChatArea::clearInput()
{
    if (chatInput) {
        // 清空输入框内容
        QTextEdit* inputEdit = chatInput->findChild<QTextEdit*>();
        if (inputEdit) {
            inputEdit->clear();
        }
    }
}

void ChatArea::setInputVisible(bool visible)
{
    if (chatInput) {
        if (visible) {
            chatInput->show();
        } else {
            chatInput->hide();
        }
    }
}

void ChatArea::setInputContent(const QString& content)
{
    if (chatInput) {
        QTextEdit* inputEdit = chatInput->findChild<QTextEdit*>();
        if (inputEdit) {
            inputEdit->setPlainText(content);
            // 将光标移动到文本末尾
            QTextCursor cursor = inputEdit->textCursor();
            cursor.movePosition(QTextCursor::End);
            inputEdit->setTextCursor(cursor);
        }
    }
}

QString ChatArea::getInputContent()
{
    if (chatInput) {
        QTextEdit* inputEdit = chatInput->findChild<QTextEdit*>();
        if (inputEdit) {
            return inputEdit->toPlainText();
        }
    }
    return "";
}