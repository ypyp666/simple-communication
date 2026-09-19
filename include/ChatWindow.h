#ifndef CHATWINDOW_H
#define CHATWINDOW_H

//QWidget是所有Qt界面元素的基类，所有界面元素都必须继承自QWidget，包括QMainWindow
//QWidget 提供了基本的界面元素，比如按钮、文本框、列表框等
//QWidget 还可以自动管理内存，不需要手动释放内存，更适合Qt界面开发

#include <QWidget>
#include <QHBoxLayout>//提供一个水平布局管理器
#include <QVBoxLayout>//提供一个垂直布局管理器
#include <QMap>
#include "ContactList.h"
#include "ChatArea.h"

class MainBackend;//前置声明：头文件只需指针，无需完整定义

class ChatWindow : public QWidget
{
    Q_OBJECT//声明为元对象，用于信号槽机制，是Qt的一个宏定义独特机制
    Q_DISABLE_COPY(ChatWindow)//禁用复制构造函数和赋值运算符
public:
    explicit ChatWindow(QWidget *parent = nullptr, MainBackend* backend = nullptr);//构造函数并且explicit禁止隐式转换
    //parent 是父类指针，QWidget是ChatWindow的父类，QWidget的构造函数的参数是parent指针，用于设置子类的父类
    //不设置的话会报错
    ~ChatWindow();
//qt里最重要的就是槽机制其直观表现就是信号函数被signal宏定义，槽函数被slot宏定义
//信号函数可以被多个槽函数连接，槽函数也可以被多个信号函数连接,相当于广播机制
//信号函数发出广播后，所有连接的槽函数都会被调用，每个槽函数可以处理信号的参数，这个依赖于Qt的元对象管理器QMetaObject
//纯g++编译器看不懂必须借助Qt的元对象管理器QMetaObject来处理信号槽机制，还要通过MOC这个Qt自带的元对象编译器翻译成C++的语法
private slots://slots声明的函数可以接受其他对象发出的信号
    void onContactsLoaded(const QList<ContactInfo>& contacts);//联系人列表加载完成后，更新左侧界面
    // 分页查询结果到达：page==1 清空重放（刚点开联系人），page>1 前插（翻更早历史）
    void onMessagesPageLoaded(const QString& contactId, int page,
                              const QList<MessageInfo>& messages, bool hasMore);
    // 聊天区滚到顶部：请求加载更早一页历史消息
    void onLoadOlderMessages();
    void onContactSelected(const QString& contactId, const QString& contactName);//联系人列表中选择联系人时，更新右侧界面
    void onSendMessage(const QString& content);//发送消息
    void onSendFile(const QString& filePath);//发送文件
    void onMessageSendFailed(const QString& messageId, const QString& serverId);// 后端发送失败信号 → 对应消息变红色感叹号
    void onMessageSendSuccess(const QString& messageId, const QString& serverId);// 后端发送成功信号 → 隐藏发送等待动画，并用服务器ID更新本地消息
    void onRetrySend(const QString& messageId, LoginFeature feature);// 用户点击失败感叹号 → 重发该消息（带功能枚举转主后端路由）
    void onMessageReceiveFailed(const QString& serverId);// 后端接收失败信号 → 提示用户拉取
    void onMessageReceived(const MessageInfo& message);// 后端收到对方新消息 → 显示到聊天区
//QList 是一个模板类，用于存储一个有序的元素集合，每个元素可以是任意类型,其作用相当于vector，但是更方便
//QList 提供了更多的方法，比如添加、删除、查找等
//QList 还可以自动管理内存，不需要手动释放内存，更适合Qt界面开发
//Qstring 是一个模板类，用于存储一个字符串，其作用相当于C++的字符串string，但是更方便，更安全，更易用
//QString 还可以自动管理内存，不需要手动释放内存，更适合Qt界面开发而且使用UTF-8编码适用所有文字和表情包不会出现乱码


private:
    QHBoxLayout* mainLayout;//左右分栏的大布局
    ContactList* contactList;//联系人列表
    QWidget* chatPanel;//聊天面板
    QVBoxLayout* chatPanelLayout;//聊天面板的垂直布局
    ChatArea* chatArea;//聊天区域（包含输入框）
    MainBackend* backend;//主后端对象
    QString currentContactId;//当前选中的联系人id
    QString currentContactName;//当前选中的联系人名称
    QString currentAccountId;//当前登录账号ID
    QMap<QString, MessageInfo> m_pendingMessages;  // 消息ID → 已发送但未确认的消息（超时后可点击感叹号重发）
    // === 本地聊天记录分页状态（当前联系人）===
    int m_currentPage = 1;   // 已加载到的页码（1=最新一页，递增往历史翻）
    bool m_hasMore = false;  // 历史是否还没翻到底（由后端查询结果带回，切联系人前先置 false 防误触发）

};

#endif // CHATWINDOW_H