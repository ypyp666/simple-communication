#include "DatabaseManager.h"
#include "MainBackend.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QDir>
#include <QFile>
#include <QUuid>
#include <QCryptographicHash>
#include <QDebug>
#include <QCoreApplication>
#include <algorithm>  // std::reverse（分页查询结果新→旧反转成正序）

DatabaseManager::DatabaseManager(QObject* parent, const QString& accountId)
    : QObject(parent)
    , m_currentAccountId(accountId)
    , m_dbPath("")
    , m_masterKey("ChatApp2026")
{
    //SQLite 没有服务，代码必须告诉程序：.db 文件存在电脑硬盘哪个位置。
    /*
    原生 SQLite 本身不支持数据库文件加密！这个密钥不是用来加密整个 db 文件，而是你业务层预留的密码加密工具。
    绝对不能直接明文把用户密码存入数据库！别人只要打开你的 .db 文件，直接看到所有人明文密码。
    */
}

DatabaseManager::~DatabaseManager()
{
    if (m_database.isOpen()) {
        m_database.close();
    }
    if (m_messageDatabase.isOpen()) {
        m_messageDatabase.close();
    }
}

DatabaseManager& DatabaseManager::instance()
{
    static DatabaseManager instance;
    /*第一次调用 instance() 的时候，才会执行 DatabaseManager 构造函数，创建对象；
    后续再次调用这个函数，不会再次构造，直接复用同一个对象；
    对象生命周期：整个程序运行全程有效，程序正常退出时自动调用析构*/
    return instance;
}

// ========== 路径方法 ==========

QString DatabaseManager::getDatabasePath() const
{
    QString appDir = QCoreApplication::applicationDirPath();//QDir::currentPath() = 工作目录 ≠ exe 所在目录
    // 获取exe程序本体所在目录，不受启动方式影响
    /*QDir 是 Qt 专门用来操作目录、路径、文件系统的工具类。
    能干这些事：拼接跨平台安全路径（Windows / 和 \ 自动处理，不用自己写斜杠）
    判断文件夹是否存在，创建文件夹
    遍历目录、文件改名、删除文件夹等
    可以简单理解：C++ 标准没有统一跨平台目录工具，Qt 把文件目录操作封装成了 QDir。 */
    QString dbDir = QDir(appDir).filePath("database"); // 2. 安全拼接路径：工作目录 + database文件夹
    
    QDir dir;
    if (!dir.exists(dbDir)) //判断 database 文件夹是否存在
    {
        dir.mkpath(dbDir);
        //递归创建目录（多级文件夹都能创建），区别：mkdir() 只能创建一级；mkpath() 推荐日常使用。
    }
    
    return QDir(dbDir).filePath("chat.db");
}

QString DatabaseManager::getMessageDatabasePath(const QString& accountId) const
{
    QString appDir = QCoreApplication::applicationDirPath();
    QString dbDir = QDir(appDir).filePath("database");
    
    QDir dir;
    if (!dir.exists(dbDir)) {
        dir.mkpath(dbDir);
    }
    
    // 每个账号一个独立的数据库文件
    // 对 accountId 进行清理，确保文件名安全
    QString safeAccountId = accountId;
    safeAccountId.replace("/","_").replace("\\","_").replace(":","_")
                .replace("*","_").replace("?","_").replace("\"","_")
                .replace("<","_").replace(">","_").replace("|","_");
    QString userDir=QDir(dbDir).filePath(safeAccountId);
    if (!dir.exists(userDir))
    {
        dir.mkpath(userDir);
    }

    return QDir(userDir).filePath(QString("messages_%1.db").arg(safeAccountId));
}

// ========== 加密方法 ==========

QString DatabaseManager::encryptPassword(const QString& password) const
{
    // 使用 XOR 加密 + Base64 编码实现可逆加密
    QByteArray key = m_masterKey.toUtf8();
    QByteArray data = password.toUtf8();
    
    // XOR 加密
    for (int i = 0; i < data.size(); i++) {
        data[i] = data[i] ^ key[i % key.size()];
    }
    
    // Base64 编码，便于存储
    return QString(data.toBase64());
}

QString DatabaseManager::decryptPassword(const QString& encrypted) const
{
    // Base64 解码 + XOR 解密
    QByteArray key = m_masterKey.toUtf8();
    QByteArray data = QByteArray::fromBase64(encrypted.toUtf8());
    
    // XOR 解密
    for (int i = 0; i < data.size(); i++) {
        data[i] = data[i] ^ key[i % key.size()];
    }
    
    return QString(data);
}

// ========== 公共库初始化（chat.db，只存本地账号）==========

bool DatabaseManager::initDatabase()
{
    m_dbPath = getDatabasePath();// 1. 计算数据库文件完整路径，存入成员变量SQLite 核心：一切操作依赖磁盘 db 文件路径，必须先拿到路径。
    const QString connName = "ChatSqliteConn";
    if (QSqlDatabase::contains(connName))// 2. 处理QtSql数据库连接.只保护【同一次软件运行期间，避免重复创建连接】
     {//QSqlDatabase 对象本身只是一个轻量句柄，真正的数据库连接存在 Qt 内部全局哈希表里，用【连接名字】作为唯一 Key。
        m_database = QSqlDatabase::database(connName);
        /*QSqlDatabase 不能直接长期保存对象拷贝！Qt 内部靠【连接名字】全局注册表管理连接
         qt_sql_default_connection"：Qt 默认无名连接的内置名称
         connName：自定义连接名，避免与默认连接冲突
         addDatabase("驱动名","连接名")：向 Qt 全局连接池注册一条数据库连接*/
    } else {
        m_database = QSqlDatabase::addDatabase("QSQLITE",connName);
        /*QSqlDatabase 连接绑定【创建它的线程】
        也就是第一次执行addDatabase这条代码所在的线程。
        后续所有QSqlQuery操作，必须在同一个线程执行，跨线程操作直接 Qt 报错。
        现状：如果你在主线程调用 initDatabase，所有数据库查询都只能主线程执行；大量查询会阻塞 UI。 */
        m_database.setDatabaseName(m_dbPath);
    }
    
    if (!m_database.open()) // 3. 打开数据库，真正触碰磁盘如果 m_dbPath 指向的 chat.db 不存在 → SQLite 自动新建空白 db 文件,文件存在 → 直接打开现有数据库
     {
        qDebug() << "Failed to open database:" << m_database.lastError().text();
        emit databaseInitialized(false);
        return false;
    }
    else {
        qDebug() << "Database opened successfully";
        QSqlQuery pragmaQuery(m_database);
        pragmaQuery.exec("PRAGMA foreign_keys = ON;");//打开外键约束，因为sqlite默认外键是关闭的
    }
    qDebug() << "Database opened successfully:" << m_dbPath;
    
    // 公共库只创建 LocalUser 表
    return createLocalUserTable();
}

bool DatabaseManager::isDatabaseOpen() const
{
    return m_database.isOpen();
}

bool DatabaseManager::isMessageDatabaseOpen() const
{
    return m_messageDatabase.isOpen();
}

// ========== 建表方法 ==========

bool DatabaseManager::createLocalUserTable()
{
    QSqlQuery query(m_database);
    //基于已经打开的数据库连接，创建查询执行器，所有 SQL 语句都通过这个对象发送给 SQLite。
    QString sql = R"(
        CREATE TABLE IF NOT EXISTS LocalUser (
            account_id TEXT PRIMARY KEY,
            password TEXT NOT NULL,
            last_login TEXT,
            remark TEXT
        )
    )";
    /*
    SQLite 采用类型亲和性 (Type Affinity)，它只有 5 种底层存储类别：
    NULL、INTEGER、REAL、TEXT、BLOB
    TEXT:凡是亲和类型标记为 TEXT 的字段，数据库会优先尝试把存入的数据转换成 UTF-8 字符串保存；
    可以存放任意长度文本（理论上限 2GB，日常完全够用）；
    INTEGER PRIMARY KEY：特殊规则，支持自增；
    TEXT PRIMARY KEY：不会自动自增，必须由程序主动传入值 
    INTEGER 存入 123，是数字*/
    
    if (!query.exec(sql)) //把建表语句发送给 SQLite 执行。
    {
        qDebug() << "Failed to create LocalUser table:" << query.lastError().text();
        return false;
    }
    
    query.exec("CREATE INDEX IF NOT EXISTS idx_localuser_account ON LocalUser(account_id)");
    
    return true;
}

bool DatabaseManager::createMessageTable()
{
    // 在分库（m_messageDatabase）中创建，不含 account_id，不含外键约束
    QSqlQuery query(m_messageDatabase);
    
    QString sql = R"(
        CREATE TABLE IF NOT EXISTS messages (
            contact_id TEXT NOT NULL,
            id TEXT,
            sender_id TEXT NOT NULL,
            target_id TEXT NOT NULL,
            content TEXT,
            file_name TEXT,
            file_path TEXT,
            file_size INTEGER DEFAULT 0,
            send_time TEXT NOT NULL,
            remark TEXT,
            is_self INTEGER DEFAULT 0, --判断是否是我发的决定这个消息是在左侧还是右侧
            is_read INTEGER DEFAULT 0, --对方发来的消息我是否已读（0未读 1已读），自己发的无意义
            is_file INTEGER DEFAULT 0,
            is_offline INTEGER DEFAULT 0,
            PRIMARY KEY (contact_id, id)
        )
    )";
    
    if (!query.exec(sql)) {
        qDebug() << "Failed to create messages table:" << query.lastError().text();
        return false;
    }
    
    query.exec("CREATE INDEX IF NOT EXISTS idx_messages_time ON messages(send_time)");
    
    return true;
}

bool DatabaseManager::createContactTable()
{
    // 同在分库（m_messageDatabase）中创建。
    // 这里不写 account_id：库文件本身就叫 messages_<account_id>.db，
    // 一个账号一个文件，天然按账号隔离，列里再存一遍是冗余。
    QSqlQuery query(m_messageDatabase);
    QString sql = R"(
        CREATE TABLE IF NOT EXISTS contacts (
            contact_id TEXT PRIMARY KEY,
            name TEXT NOT NULL,
            avatar TEXT,
            remark TEXT
        )
    )";
    // 注意：TEXT PRIMARY KEY 不自增，联系人 ID 由服务器下发或程序自己填
    if (!query.exec(sql)) {
        qDebug() << "Failed to create contacts table:" << query.lastError().text();
        return false;
    }
    query.exec("CREATE INDEX IF NOT EXISTS idx_contacts_name ON contacts(name)");
    
    return true;
}

bool DatabaseManager::createConversationTable()
{
    // 会话表：只存"这个对话最后一次长什么样"，真正的消息正文在 messages 表里。
    // 拆出来是因为一个联系人只会有一行会话记录（主键即联系人），
    // 查会话列表时不用去 messages 里按 contact_id 分组取最新一条，直接读这张表。
    QSqlQuery query(m_messageDatabase);
    QString sql = R"(
        CREATE TABLE IF NOT EXISTS conversations (
            contact_id TEXT PRIMARY KEY,
            last_message TEXT,
            last_time TEXT,
            unread_count INTEGER DEFAULT 0,
            is_online INTEGER DEFAULT 0
        )
    )";
    if (!query.exec(sql)) {
        qDebug() << "Failed to create conversations table:" << query.lastError().text();
        return false;
    }
    // 会话列表按最后消息时间倒序排，给 last_time 建索引
    query.exec("CREATE INDEX IF NOT EXISTS idx_conversations_time ON conversations(last_time)");
    
    return true;
}

// ========== 消息库操作 ==========

bool DatabaseManager::openMessageDatabase(const QString& accountId)
{
    if (accountId.isEmpty()) {
        return false;
    }
    
    // 已经打开了同一个库，直接返回
    if (m_messageDatabase.isOpen() && 
        m_messageDatabase.databaseName() == getMessageDatabasePath(accountId)) {
        return true;
    }
    
    // 关闭之前的消息库
    if (m_messageDatabase.isOpen()) {
        m_messageDatabase.close();
    }
    
    QString dbPath = getMessageDatabasePath(accountId);
    QString connName = QString("ChatMsgDb_%1").arg(accountId);//创建消息数据库连接前面的连接时公共数据库
    
    if (QSqlDatabase::contains(connName)) {
        m_messageDatabase = QSqlDatabase::database(connName);
    } else {
        m_messageDatabase = QSqlDatabase::addDatabase("QSQLITE", connName);
        m_messageDatabase.setDatabaseName(dbPath);
    }
    
    if (!m_messageDatabase.open()) {
        qDebug() << "Failed to open message database:" << m_messageDatabase.lastError().text();
        return false;
    }
    
    qDebug() << "Message database opened:" << dbPath;
    // 分库里的三张表在这里统一建好（都带 IF NOT EXISTS，重复执行无副作用）
    if (!createMessageTable()) {
        return false;
    }
    if (!createContactTable()) {
        return false;
    }
    return createConversationTable();
}

void DatabaseManager::closeMessageDatabase()
{
    if (m_messageDatabase.isOpen()) {
        m_messageDatabase.close();
    }
}

// ========== LocalUser 表操作（公共库）==========

bool DatabaseManager::addLocalUser(const QString& accountId, const QString& password)
{
    if (!m_database.isOpen()) {
        return false;
    }
    
    QSqlQuery query(m_database);
    query.prepare(R"(
        INSERT OR REPLACE INTO LocalUser (account_id, password, last_login)
        VALUES (?, ?, ?)
    )");// 4. 预编译SQL语句，INSERT OR UPDATE 是 SQLite 特有语法，用于在插入时如果主键已存在则更新，如果不存在则插入
    
    // 5. 依次绑定三个参数，防止 SQL 注入
    query.bindValue(0, accountId);// 绑定 account_id 参数，账号ID
    query.bindValue(1, encryptPassword(password));// 绑定 password 参数，加密后的密码哈希
    query.bindValue(2, QDateTime::currentDateTime().toString(Qt::ISODate));// 绑定 last_login 参数，当前系统时间，格式为 YYYY-MM-DD
    
    bool success = query.exec();
    if (!success) {
        qDebug() << "Failed to add local user:" << query.lastError().text();
    }
    
    return success;
}

bool DatabaseManager::updateLocalUser(const QString& accountId, const QString& password)
{
    if (!m_database.isOpen()) {
        return false;
    }
    
    QSqlQuery query(m_database);
    query.prepare(R"(
        UPDATE LocalUser SET password = ?, last_login = ?
        WHERE account_id = ?
    )");
    
    query.bindValue(0, encryptPassword(password));
    query.bindValue(1, QDateTime::currentDateTime().toString(Qt::ISODate));
    query.bindValue(2, accountId);
    
    bool success = query.exec();
    if (!success) {
        qDebug() << "Failed to update local user:" << query.lastError().text();
    }
    
    return success;
}

bool DatabaseManager::removeLocalUser(const QString& accountId)
{
    if (!m_database.isOpen()) {
        return false;
    }
    
    QSqlQuery query(m_database);
    
   /* query.prepare("DELETE FROM messages WHERE account_id = ?");
    query.bindValue(0, accountId);
    query.exec();*/
    
    query.prepare("DELETE FROM LocalUser WHERE account_id = ?");
    query.bindValue(0, accountId);
    
    bool success = query.exec();
    if (!success) {
        qDebug() << "Failed to remove local user:" << query.lastError().text();
    }
    
    // 关闭该账号的消息库（如果开着）
    if (m_currentAccountId == accountId) {
        closeMessageDatabase();
    }
    
    // 删除该账号的消息库文件
    QString msgDbPath = getMessageDatabasePath(accountId);
    QFile::remove(msgDbPath);
    
    return success;
}

bool DatabaseManager::verifyLocalUser(const QString& accountId, const QString& password)
// 校验本地用户密码是否正确，云端登录成功后调用如果不正确就更新本地密码
{
    if (!m_database.isOpen()) {
        return false;
    }
    
    QSqlQuery query(m_database);
    query.prepare("SELECT password FROM LocalUser WHERE account_id = ?");
    query.bindValue(0, accountId);
    
    if (!query.exec() || !query.next())   // 4. 执行SQL + 游标判断，确保查询成功且有结果
    /*query.exec()成功只代表 SQL 正常运行，不代表查到数据。
    next() 移动游标，尝试读取第一条结果：如果查询成功且有结果，返回 true；否则返回 false。 */
    {
        return false;
    }
    
    QString storedPassword = query.value(0).toString();// 5. 读取数据库里存储的密码哈希
    QString encryptedInput = encryptPassword(password);// 6. 对用户输入密码执行哈希运算，得到加密后的密码哈希
        //校验逻辑：不对密文解密，而是把用户输入密码执行一模一样的哈希运算，对比两段哈希字符串是否一致。
    return storedPassword == encryptedInput;// 7. 哈希字符串直接对比
}

QString DatabaseManager::getLocalUserPassword(const QString& accountId)
// 获取本地用户密码（解密后），用于记住密码功能
{
    if (!m_database.isOpen()) {
        return "";
    }
    
    QSqlQuery query(m_database);
    query.prepare("SELECT password FROM LocalUser WHERE account_id = ?");
    query.bindValue(0, accountId);
    
    if (!query.exec() || !query.next()) {
        return "";
    }
    
    // 返回解密后的密码
    QString encryptedPassword = query.value(0).toString();// 执行SQL之后，游标定位到一行数据
    /*
    1、QSqlQuery::value(int index) 原理:执行 query.exec(sql) → 数据库返回结果集（多行表格）
    query.next() 把游标移动到下一行（非常关键！你代码前面一定调用了 next）
    value(下标)：读取当前游标所在行，第 N 列的数据,下标从 0 开始,SELECT password FROM LocalUser WHERE account_id = ?
    查询返回的结果表只有 1 列，列序号：第 0 列：password,所以写 query.value(0)。
    */
    return decryptPassword(encryptedPassword);
}

QList<QString> DatabaseManager::getAllLocalUsers()
{
    QList<QString> users; // 定义字符串列表，用来存放查询出来的所有账号
    
    if (!m_database.isOpen()) {
        return users;
    }
    
    QSqlQuery query(m_database);// SQL：查询LocalUser表所有account_id，按照last_login【倒序】
    // 最近登录的账号排在最上面
    query.exec("SELECT account_id FROM LocalUser ORDER BY last_login DESC");
    
    while (query.next())// 循环：不断下移游标读取每一行结果 next() 拿到一整行（数据库里的一行，可以理解成一组字段，俗称元组）
    {// 当前行第0列 = account_id，加入列表
        users.append(query.value(0).toString());// value是按行读取读取当前行第0列（account_id），并加入列表
        //append = 在列表的末尾追加一个元素
    }
    
    return users;
}

bool DatabaseManager::isLocalUserExists(const QString& accountId)
{
    if (!m_database.isOpen()) {
        return false;
    }
    
    QSqlQuery query(m_database);
    query.prepare("SELECT COUNT(*) FROM LocalUser WHERE account_id = ?");
    query.bindValue(0, accountId);
    
    if (!query.exec() || !query.next()) {
        return false;//哪怕不存在也会返回0，这一段是用来兜底的防止出现异常情况
    }
    
    return query.value(0).toInt() > 0;// 把 QVariant 里面的数据，转换成 int 整型，判断是否大于0
}

// ========== 当前用户 ==========

void DatabaseManager::setCurrentAccountId(const QString& accountId)
// 设置当前登录账号（不用后面反复获取），更新数据库里当前登录账号的last_login时间，调用时机为登录后
{
    m_currentAccountId = accountId;// 1.内存变量记录：当前登录账号
    
    if (!accountId.isEmpty() && m_database.isOpen()) {
        QSqlQuery query(m_database);
        query.prepare("UPDATE LocalUser SET last_login = ? WHERE account_id = ?");
        // 2. 更新数据库里当前登录账号的last_login时间
        query.bindValue(0, QDateTime::currentDateTime().toString(Qt::ISODate));
        query.bindValue(1, accountId);
        query.exec();
    }
    
    // 自动打开该账号的消息分库
    if (!accountId.isEmpty()) {
        openMessageDatabase(accountId);
    }
}

// ========== 消息操作（分库）==========

bool DatabaseManager::saveMessage(const MessageInfo& message)
{
    if (!m_messageDatabase.isOpen()) {
        qDebug() << "Message database is not open";
        return false;
    }
    
    QSqlQuery query(m_messageDatabase);
    
    QString sql = R"(
        INSERT OR REPLACE INTO messages
        (contact_id, id, sender_id, target_id, content, file_name, file_path, 
         file_size, send_time, remark, is_self, is_read, is_file, is_offline)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";//根据主键 id 查询：数据库不存在这条消息 → INSERT 新增，存在 → REPLACE 替换
    
    query.prepare(sql);
    query.bindValue(0, message.contactId);
    query.bindValue(1, message.id.isEmpty() ? QUuid::createUuid().toString() : message.id);
    query.bindValue(2, message.senderId);
    query.bindValue(3, message.targetId);
    query.bindValue(4, message.content);
    query.bindValue(5, message.fileName);
    query.bindValue(6, message.filePath);
    query.bindValue(7, message.fileSize);
    query.bindValue(8, message.sendTime.toString(Qt::ISODate));
    query.bindValue(9, message.remark);
    query.bindValue(10, message.isSelf ? 1 : 0);
    query.bindValue(11, message.isRead ? 1 : 0);
    query.bindValue(12, message.isFile ? 1 : 0);
    query.bindValue(13, message.isOffline ? 1 : 0);
    
    if (!query.exec()) {
        qDebug() << "Failed to save message:" << query.lastError().text();
        return false;
    }
    
    emit messageSaved(true);
    return true;
}

bool DatabaseManager::updateMessageId(const QString& contactId, const QString& oldId, const QString& newId)
{
    if (!m_messageDatabase.isOpen()) {
        qDebug() << "Message database is not open";
        return false;
    }

    QSqlQuery query(m_messageDatabase);
    query.prepare(R"(
        UPDATE messages SET id = ?
        WHERE contact_id = ? AND id = ?
    )");
    query.bindValue(0, newId);      // SET id = 新ID（服务器ID）
    query.bindValue(1, contactId);  // WHERE contact_id = 联系人（联合主键第一列）
    query.bindValue(2, oldId);      // WHERE id = 旧ID（临时ID，联合主键第二列）

    if (!query.exec()) {
        qDebug() << "Failed to update message id:" << query.lastError().text();
        return false;
    }
    
    emit messageIdUpdated(true);
    return true;
}

QList<MessageInfo> DatabaseManager::loadMessages(const QString& contactId)
{
    QList<MessageInfo> messages;
    
    if (!m_messageDatabase.isOpen()) {
        qDebug() << "Message database is not open";
        return messages;
    }
    
    QSqlQuery query(m_messageDatabase);
    
    QString sql = R"(
        SELECT * FROM messages
        WHERE contact_id = ?
        ORDER BY send_time ASC
    )";
    
    query.prepare(sql);
    query.bindValue(0, contactId);
    
    if (!query.exec()) {
        qDebug() << "Failed to load messages:" << query.lastError().text();
        return messages;
    }
    
    while (query.next()) {
        MessageInfo msg;
        msg.id = query.value("id").toString();
        msg.contactId = query.value("contact_id").toString();
        msg.accountId = m_currentAccountId;
        msg.senderId = query.value("sender_id").toString();
        msg.targetId = query.value("target_id").toString();
        msg.content = query.value("content").toString();
        msg.fileName = query.value("file_name").toString();
        msg.filePath = query.value("file_path").toString();
        msg.fileSize = query.value("file_size").toLongLong();
        msg.sendTime = QDateTime::fromString(query.value("send_time").toString(), Qt::ISODate);
        msg.remark = query.value("remark").toString();
        msg.isSelf = query.value("is_self").toBool();
        msg.isRead = query.value("is_read").toBool();
        msg.isFile = query.value("is_file").toBool();
        msg.isOffline = query.value("is_offline").toBool();
        messages.append(msg);
    }
    
    emit messagesLoaded(messages);
    return messages;
}

// 分页加载某个联系人的消息（在后台 DB 线程执行，由 MainBackend 的请求信号触发）。
// 翻页方向：page=1 是最新一页。实现上先按 send_time 倒序取"离现在最近的 pageSize 条"，
// 查完再反转成时间正序——这样"第 2 页"自然就是再往前的一段历史，
// 而不像 ASC+OFFSET 那样 page=1 永远是最旧的开头（聊天首屏应该看到最新消息）
void DatabaseManager::loadMessagesPage(const QString& contactId, int page, int pageSize)
{
    QList<MessageInfo> messages;
    bool hasMore = false;

    // 参数保护：页码从1开始，单页1~50条
    if (page < 1) page = 1;
    if (pageSize < 1) pageSize = 50;
    if (pageSize > 50) pageSize = 50;

    if (!m_messageDatabase.isOpen()) {
        qDebug() << "Message database is not open";
        emit messagesPageLoaded(contactId, page, messages, hasMore);
        return;
    }

    QSqlQuery query(m_messageDatabase);

    // 倒序取页：OFFSET (page-1)*pageSize 表示跳过最近的 (page-1) 页，
    // 剩下最靠前的 pageSize 条就是"第 page 页"（比第 page-1 页更早的历史）
    QString sql = R"(
        SELECT * FROM messages
        WHERE contact_id = ?
        ORDER BY send_time DESC
        LIMIT ? OFFSET ?
    )";

    query.prepare(sql);
    query.bindValue(0, contactId);
    query.bindValue(1, pageSize);
    query.bindValue(2, (page - 1) * pageSize);

    if (!query.exec()) {
        qDebug() << "Failed to load messages page:" << query.lastError().text();
        emit messagesPageLoaded(contactId, page, messages, hasMore);
        return;
    }

    while (query.next()) {
        MessageInfo msg;
        msg.id = query.value("id").toString();
        msg.contactId = query.value("contact_id").toString();
        msg.accountId = m_currentAccountId;
        msg.senderId = query.value("sender_id").toString();
        msg.targetId = query.value("target_id").toString();
        msg.content = query.value("content").toString();
        msg.fileName = query.value("file_name").toString();
        msg.filePath = query.value("file_path").toString();
        msg.fileSize = query.value("file_size").toLongLong();
        msg.sendTime = QDateTime::fromString(query.value("send_time").toString(), Qt::ISODate);
        msg.remark = query.value("remark").toString();
        msg.isSelf = query.value("is_self").toBool();
        msg.isRead = query.value("is_read").toBool();
        msg.isFile = query.value("is_file").toBool();
        msg.isOffline = query.value("is_offline").toBool();
        messages.append(msg);
    }

    // 拿满一页说明后面大概率还有更早的；不足一页就是已经翻到头了
    hasMore = (messages.size() == pageSize);

    // 查出来是"新→旧"，反转成"旧→新"（聊天区从上到下的显示顺序），
    // UI 拿到手可以直接按顺序插，不用再关心查询方向
    std::reverse(messages.begin(), messages.end());

    emit messagesPageLoaded(contactId, page, messages, hasMore);
}

QList<MessageInfo> DatabaseManager::loadAllMessages()
{
    QList<MessageInfo> messages;
    
    if (!m_messageDatabase.isOpen()) {
        qDebug() << "Message database is not open";
        return messages;
    }
    
    QSqlQuery query(m_messageDatabase);
    
    if (!query.exec("SELECT * FROM messages ORDER BY send_time ASC")) {
        qDebug() << "Failed to load all messages:" << query.lastError().text();
        return messages;
    }
    
    while (query.next()) {
        MessageInfo msg;
        msg.id = query.value("id").toString();
        msg.contactId = query.value("contact_id").toString();
        msg.accountId = m_currentAccountId;
        msg.senderId = query.value("sender_id").toString();
        msg.targetId = query.value("target_id").toString();
        msg.content = query.value("content").toString();
        msg.fileName = query.value("file_name").toString();
        msg.filePath = query.value("file_path").toString();
        msg.fileSize = query.value("file_size").toLongLong();
        msg.sendTime = QDateTime::fromString(query.value("send_time").toString(), Qt::ISODate);
        msg.remark = query.value("remark").toString();
        msg.isSelf = query.value("is_self").toBool();
        msg.isRead = query.value("is_read").toBool();
        msg.isFile = query.value("is_file").toBool();
        msg.isOffline = query.value("is_offline").toBool();
        messages.append(msg);
    }
    
    emit messagesLoaded(messages);
    return messages;
}

bool DatabaseManager::deleteMessage(const QString& messageId)
{
    if (!m_messageDatabase.isOpen()) {
        return false;
    }
    
    QSqlQuery query(m_messageDatabase);
    query.prepare("DELETE FROM messages WHERE id = ?");
    query.bindValue(0, messageId);
    
    return query.exec();
}

void DatabaseManager::clearAllMessages()
{
    if (!m_messageDatabase.isOpen()) {
        return;
    }
    
    QSqlQuery query(m_messageDatabase);
    query.exec("DELETE FROM messages");
}

// ========== 会话操作（conversations 表：左侧会话列表的快照）==========

QList<ConversationInfo> DatabaseManager::loadConversations()
{
    QList<ConversationInfo> conversations;
    if (!m_messageDatabase.isOpen()) {
        emit conversationsLoaded(conversations);   // 库没打开也要回一声，调用方不会空等
        return conversations;
    }

    QSqlQuery query(m_messageDatabase);
    // conversations 表只存"会话快照"（最后一条消息/时间/未读数/在线态），
    // 名字和头像在 contacts 表里，用 LEFT JOIN 补齐（联系人可能还没入 contacts 表，
    // 用 LEFT JOIN 而不是 INNER JOIN，避免那张表缺行时整条会话被丢掉）。
    // 排序：最新消息在前，正好对上 UI 左侧列表从上到下的顺序
    QString sql = R"(
        SELECT c.contact_id, ct.name, ct.avatar,
               c.last_message, c.last_time, c.unread_count, c.is_online
        FROM conversations c
        LEFT JOIN contacts ct ON ct.contact_id = c.contact_id
        ORDER BY c.last_time DESC
    )";

    if (!query.exec(sql)) {
        qDebug() << "Failed to load conversations:" << query.lastError().text();
        emit conversationsLoaded(conversations);
        return conversations;
    }

    while (query.next()) {
        ConversationInfo info;
        info.id = query.value(0).toString();
        info.name = query.value(1).toString();
        info.avatar = query.value(2).toString();
        info.lastMessage = query.value(3).toString();
        info.lastTime = QDateTime::fromString(query.value(4).toString(), Qt::ISODate);
        info.unreadCount = query.value(5).toInt();
        info.isOnline = query.value(6).toInt() != 0;   // SQLite 无 bool，0/1 转回 bool
        conversations.append(info);
    }

    emit conversationsLoaded(conversations);
    return conversations;
}

bool DatabaseManager::saveConversation(const ConversationInfo& conversation)
{
    if (!m_messageDatabase.isOpen()) {
        qDebug() << "Message database is not open";
        return false;
    }

    QSqlQuery query(m_messageDatabase);
    // contact_id 是主键：INSERT OR REPLACE 天然是"upsert"——
    // 新会话 → 插入；已有会话 → 覆盖成最新快照（与会话列表"只保留最后一条"一致）
    QString sql = R"(
        INSERT OR REPLACE INTO conversations
        (contact_id, last_message, last_time, unread_count, is_online)
        VALUES (?, ?, ?, ?, ?)
    )";

    query.prepare(sql);
    query.bindValue(0, conversation.id);
    query.bindValue(1, conversation.lastMessage);
    query.bindValue(2, conversation.lastTime.toString(Qt::ISODate));
    query.bindValue(3, conversation.unreadCount);
    query.bindValue(4, conversation.isOnline ? 1 : 0);

    if (!query.exec()) {
        qDebug() << "Failed to save conversation:" << query.lastError().text();
        return false;
    }

    return true;
}

// 只在不存在时插入一条会话（双击联系人页"新建会话"时用）。
// INSERT OR IGNORE：contact_id 是主键，撞键就整条跳过——正是"有了就别动"的语义。
// 这里刻意不用 saveConversation 的 INSERT OR REPLACE：UI 列表可能只是"还没加载出来"
// （比如刚启动、列表还是空的），DB 里这条会话其实早就存在，
// OR REPLACE 会把它的未读数、最后消息摘要一起冲成 0 / 空
bool DatabaseManager::insertConversationIfAbsent(const ConversationInfo& conversation)
{
    if (!m_messageDatabase.isOpen()) {
        qDebug() << "Message database is not open";
        return false;
    }

    QSqlQuery query(m_messageDatabase);
    QString sql = R"(
        INSERT OR IGNORE INTO conversations
        (contact_id, last_message, last_time, unread_count, is_online)
        VALUES (?, ?, ?, ?, ?)
    )";

    query.prepare(sql);
    query.bindValue(0, conversation.id);
    query.bindValue(1, conversation.lastMessage);
    query.bindValue(2, conversation.lastTime.toString(Qt::ISODate));
    query.bindValue(3, conversation.unreadCount);
    query.bindValue(4, conversation.isOnline ? 1 : 0);

    if (!query.exec()) {
        qDebug() << "Failed to insert conversation:" << query.lastError().text();
        return false;
    }

    return true;
}

// 收发消息后更新会话快照：最后消息 / 时间，必要时未读 +1。
// 分两步而不是一条 UPSERT：先当"更新已有会话"试，一行都没改到，就说明表里还没有这条会话，再补插。
// 未读数走 SQL 自增（unread_count + ?），不先查出来再写回——少一次查询，
// 也不会因为中间还夹着别的写入而把计数覆盖丢
bool DatabaseManager::updateConversationMessage(const QString& contactId, const QString& lastMessage,
                                                const QDateTime& lastTime, bool increaseUnread)
{
    if (!m_messageDatabase.isOpen()) {
        qDebug() << "Message database is not open";
        return false;
    }

    const QString timeStr = lastTime.toString(Qt::ISODate);

    QSqlQuery update(m_messageDatabase);
    update.prepare(R"(
        UPDATE conversations
        SET last_message = ?, last_time = ?, unread_count = unread_count + ?
        WHERE contact_id = ?
    )");
    update.bindValue(0, lastMessage);
    update.bindValue(1, timeStr);
    update.bindValue(2, increaseUnread ? 1 : 0);
    update.bindValue(3, contactId);

    if (!update.exec()) {
        qDebug() << "Failed to update conversation:" << update.lastError().text();
        return false;
    }

    if (update.numRowsAffected() > 0) {
        return true;   // 更新到已有会话了，收工
    }

    // 一行都没改到 → 表里还没有这条会话（这条消息就是它俩的第一条）：补插一条
    QSqlQuery insert(m_messageDatabase);
    insert.prepare(R"(
        INSERT INTO conversations
        (contact_id, last_message, last_time, unread_count, is_online)
        VALUES (?, ?, ?, ?, 0)
    )");
    insert.bindValue(0, contactId);
    insert.bindValue(1, lastMessage);
    insert.bindValue(2, timeStr);
    insert.bindValue(3, increaseUnread ? 1 : 0);

    if (!insert.exec()) {
        qDebug() << "Failed to insert conversation:" << insert.lastError().text();
        return false;
    }

    return true;
}

// 打开会话时刷新会话快照：把"最后一条消息"重新对齐到 messages 表里真正最新的那条。
// 只写一条 UPDATE，最新消息用相关子查询现取；某条会话还没聊过时子查询返回 NULL，
// 靠 COALESCE 退化成空摘要 / 当前时间（正好对上"有历史就取历史，没有就是当前时间"）。
// 与 updateConversationMessage 同一套"先试 UPDATE，一行没改到再补插"的做法
bool DatabaseManager::refreshConversationSnapshot(const QString& contactId)
{
    if (!m_messageDatabase.isOpen()) {
        qDebug() << "Message database is not open";
        return false;
    }

    // 取"最新一条历史消息"的子查询：send_time 存的是 ISODate 文本，可以直接按字典序倒排；
    // 同一时刻有多条时再按 rowid 兜底（后插入的算更新）
    const QString latestContent = "SELECT content FROM messages WHERE contact_id = ? "
                                  "ORDER BY send_time DESC, rowid DESC LIMIT 1";
    const QString latestTime = "SELECT send_time FROM messages WHERE contact_id = ? "
                               "ORDER BY send_time DESC, rowid DESC LIMIT 1";
    const QString nowStr = QDateTime::currentDateTime().toString(Qt::ISODate);

    QSqlQuery update(m_messageDatabase);
    update.prepare(QString(R"(
        UPDATE conversations
        SET last_message = COALESCE((%1), ''),
            last_time = COALESCE((%2), ?)
        WHERE contact_id = ?
    )").arg(latestContent, latestTime));
    update.bindValue(0, contactId);   // 子查询：最新摘要
    update.bindValue(1, contactId);   // 子查询：最新时间
    update.bindValue(2, nowStr);      // 没有历史消息时的兜底时间
    update.bindValue(3, contactId);   // WHERE

    if (!update.exec()) {
        qDebug() << "Failed to refresh conversation:" << update.lastError().text();
        return false;
    }

    if (update.numRowsAffected() > 0) {
        return true;   // 已有会话，快照已对齐，收工（未读数没动）
    }

    // 一行都没改到 → 表里还没有这条会话（第一次打开）：补插一条。
    // 用 INSERT ... SELECT 让摘要/时间同样从 messages 现取，没有历史就是 空 / 当前时间，
    // 避免"先查一次再拼参数"多跑一趟
    QSqlQuery insert(m_messageDatabase);
    insert.prepare(QString(R"(
        INSERT INTO conversations
        (contact_id, last_message, last_time, unread_count, is_online)
        SELECT ?, COALESCE((%1), ''), COALESCE((%2), ?), 0, 0
    )").arg(latestContent, latestTime));
    insert.bindValue(0, contactId);
    insert.bindValue(1, contactId);
    insert.bindValue(2, contactId);
    insert.bindValue(3, nowStr);

    if (!insert.exec()) {
        qDebug() << "Failed to insert conversation:" << insert.lastError().text();
        return false;
    }

    return true;
}

// 打开会话：未读清零。只改未读一列，最后消息和时间保持原样
bool DatabaseManager::clearConversationUnread(const QString& contactId)
{
    if (!m_messageDatabase.isOpen()) {
        return false;
    }

    QSqlQuery query(m_messageDatabase);
    query.prepare("UPDATE conversations SET unread_count = 0 WHERE contact_id = ?");
    query.bindValue(0, contactId);

    return query.exec();
}

bool DatabaseManager::deleteConversation(const QString& conversationId)
{
    if (!m_messageDatabase.isOpen()) {
        return false;
    }

    QSqlQuery query(m_messageDatabase);
    query.prepare("DELETE FROM conversations WHERE contact_id = ?");
    query.bindValue(0, conversationId);

    return query.exec();
}

// ========== 联系人操作（contacts 表：通讯录好友资料）==========

QList<ContactInfo> DatabaseManager::loadContacts()
{
    QList<ContactInfo> contacts;
    if (!m_messageDatabase.isOpen()) {
        emit contactsLoaded(contacts);   // 库没打开也要回一声，调用方不会空等
        return contacts;
    }

    QSqlQuery query(m_messageDatabase);
    // 通讯录按名字排序（会话列表才按时间，两者是两套数据）
    QString sql = R"(
        SELECT contact_id, name, avatar, remark
        FROM contacts
        ORDER BY name
    )";

    if (!query.exec(sql)) {
        qDebug() << "Failed to load contacts:" << query.lastError().text();
        emit contactsLoaded(contacts);
        return contacts;
    }

    while (query.next()) {
        ContactInfo info;
        info.id = query.value(0).toString();
        info.name = query.value(1).toString();
        info.avatar = query.value(2).toString();
        info.remark = query.value(3).toString();
        contacts.append(info);
    }

    emit contactsLoaded(contacts);
    return contacts;
}

bool DatabaseManager::saveContact(const ContactInfo& contact)
{
    if (!m_messageDatabase.isOpen()) {
        qDebug() << "Message database is not open";
        return false;
    }

    QSqlQuery query(m_messageDatabase);
    // 同上：contact_id 主键 + INSERT OR REPLACE = 新增或更新好友资料
    QString sql = R"(
        INSERT OR REPLACE INTO contacts
        (contact_id, name, avatar, remark)
        VALUES (?, ?, ?, ?)
    )";

    query.prepare(sql);
    query.bindValue(0, contact.id);
    query.bindValue(1, contact.name);
    query.bindValue(2, contact.avatar);
    query.bindValue(3, contact.remark);

    if (!query.exec()) {
        qDebug() << "Failed to save contact:" << query.lastError().text();
        return false;
    }

    return true;
}

bool DatabaseManager::deleteContact(const QString& contactId)
{
    if (!m_messageDatabase.isOpen()) {
        return false;
    }

    QSqlQuery query(m_messageDatabase);
    query.prepare("DELETE FROM contacts WHERE contact_id = ?");
    query.bindValue(0, contactId);

    return query.exec();
}

// ========== 联系人备注 ==========

bool DatabaseManager::setContactRemark(const QString& contactId, const QString& remark)
{
    if (!m_messageDatabase.isOpen()) {
        return false;
    }
    
    // 注意表名是 contacts 不是 messages：备注是"好友资料"的一栏，
    // loadContacts 正是从 contacts.remark 读出来显示的；写进 messages 表的话
    // 下一次重载通讯录就读不到，等于白改
    QSqlQuery query(m_messageDatabase);
    query.prepare(R"(
        UPDATE contacts SET remark = ?
        WHERE contact_id = ?
    )");
    
    query.bindValue(0, remark);
    query.bindValue(1, contactId);
    
    return query.exec();
}

QString DatabaseManager::getContactRemark(const QString& contactId)
{
    if (!m_messageDatabase.isOpen()) {
        return "";
    }
    
    // 与 setContactRemark 同一个表：备注存在 contacts.remark（messages.remark 是消息自己的字段）
    QSqlQuery query(m_messageDatabase);
    query.prepare(R"(
        SELECT remark FROM contacts
        WHERE contact_id = ? AND remark IS NOT NULL AND remark != ''
        LIMIT 1
    )");
    
    query.bindValue(0, contactId);
    
    if (!query.exec() || !query.next()) {
        return "";
    }
    
    return query.value(0).toString();
}
