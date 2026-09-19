#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QObject>
#include <QString>
#include <QSqlDatabase>
#include <QList>
#include <QDateTime>
#include "ChatBackend.h"

class DatabaseManager : public QObject
{
    Q_OBJECT
public:
    static DatabaseManager& instance();

    bool initDatabase();
    bool isDatabaseOpen() const;
    bool isMessageDatabaseOpen() const;

    // === LocalUser 表操作（公共库 chat.db）===
    bool addLocalUser(const QString& accountId, const QString& password);
    bool updateLocalUser(const QString& accountId, const QString& password);
    bool removeLocalUser(const QString& accountId);
    bool verifyLocalUser(const QString& accountId, const QString& password);
    QString getLocalUserPassword(const QString& accountId);
    QList<QString> getAllLocalUsers();
    bool isLocalUserExists(const QString& accountId);

    // === 当前用户 ===
    QString getCurrentAccountId() const { return m_currentAccountId; }

public slots:
    // ===== 跨线程调用的数据库方法 =====
    // 这些方法会被搬到后台数据库线程执行（见 MainBackend 的 moveToThread）
    // 必须声明为 slot 才能被跨线程信号槽（QueuedConnection）调用
    // 注意：执行位置在后台线程，QSqlDatabase 连接也是在这里创建/使用

    void setCurrentAccountId(const QString& accountId);

    // === 消息操作 ===
    bool saveMessage(const MessageInfo& message);
    bool updateMessageId(const QString& contactId, const QString& oldId, const QString& newId);

    // === 消息库操作（每个账号独立文件 messages_<accountId>.db）===
    bool openMessageDatabase(const QString& accountId);
    void closeMessageDatabase();

    // === 消息操作 ===
    QList<MessageInfo> loadMessages(const QString& contactId);
    QList<MessageInfo> loadAllMessages();
    // 分页加载某个联系人的消息（槽，跨线程信号调用，结果经 messagesPageLoaded 信号送回主线程）。
    // page 从 1 开始，1 = 最新一页（离现在最近的 pageSize 条），page 越大越往历史翻；
    // 单页上限 50 条，查出的列表已反转成时间正序（旧→新），UI 可直接按顺序插入
    void loadMessagesPage(const QString& contactId, int page = 1, int pageSize = 50);
    bool deleteMessage(const QString& messageId);
    void clearAllMessages();

    // === 联系人备注 ===
    bool setContactRemark(const QString& contactId, const QString& remark);
    QString getContactRemark(const QString& contactId);

signals:
    void messageSaved(bool success);
    void messageIdUpdated(bool success);
    void messagesLoaded(const QList<MessageInfo>& messages);
    // 分页查询结果（后台线程发出，主线程的 MainBackend 接收后转发 UI）。
    // hasMore：返回条数 == pageSize 就认为历史还没翻到底（最后一页恰好整页时
    // 会多查一次空页，无害）；UI 据此决定"滚到顶部还拉不拉更早一页"
    void messagesPageLoaded(const QString& contactId, int page,
                            const QList<MessageInfo>& messages, bool hasMore);
    void databaseInitialized(bool success);

private:
    explicit DatabaseManager(QObject *parent = nullptr, const QString& accountId = "");
    ~DatabaseManager();
    DatabaseManager(const DatabaseManager&) = delete;//显式告诉编译器，这个函数被删除，不允许生成、不允许调用。
    DatabaseManager& operator=(const DatabaseManager&) = delete;//编译期直接报错，更早发现问题；

    bool createLocalUserTable();
    bool createMessageTable();

    QString getDatabasePath() const;
    QString getMessageDatabasePath(const QString& accountId) const;

    QString encryptPassword(const QString& password) const;
    QString decryptPassword(const QString& encrypted) const;
    QByteArray generateSalt(int length) const;
    QByteArray rotateBytes(QByteArray data, int shift) const;
    QByteArray shuffleBytes(QByteArray data) const;

    // 公共数据库（存本地账号）
    QSqlDatabase m_database;
    // 消息数据库（每个账号一个独立文件）
    QSqlDatabase m_messageDatabase;

    QString m_currentAccountId;
    QString m_dbPath;
    QString m_masterKey;  // 加密密钥
};

#endif // DATABASEMANAGER_H
