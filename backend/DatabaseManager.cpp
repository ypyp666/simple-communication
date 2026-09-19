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
    return createMessageTable();
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

// ========== 联系人备注 ==========

bool DatabaseManager::setContactRemark(const QString& contactId, const QString& remark)
{
    if (!m_messageDatabase.isOpen()) {
        return false;
    }
    
    QSqlQuery query(m_messageDatabase);
    query.prepare(R"(
        UPDATE messages SET remark = ?
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
    
    QSqlQuery query(m_messageDatabase);
    query.prepare(R"(
        SELECT remark FROM messages
        WHERE contact_id = ? AND remark IS NOT NULL AND remark != ''
        LIMIT 1
    )");
    
    query.bindValue(0, contactId);
    
    if (!query.exec() || !query.next()) {
        return "";
    }
    
    return query.value(0).toString();
}
