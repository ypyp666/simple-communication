//
// Created by admin_yxz on 2026/7/7.
//

#include "../include/mysql.h"
#include "mysql.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <chrono>

mysqlconn::mysqlconn()
{
    mysql = nullptr;
    res = nullptr;
    row = nullptr;
}

mysqlconn::~mysqlconn()
{
    freeResult();
    if (mysql)
    {
        mysql_close(mysql);
    }
}

bool mysqlconn::connect(const std::string& host, int port,
                        const std::string& user, const std::string& pwd,
                        const std::string& dbname)
{
    // 初始化mysql句柄
    mysql = mysql_init(nullptr);
    if (!mysql)
    {
        std::cerr << "mysql初始化失败：" << mysql_error(mysql) << std::endl;
        return false;
    }

    // 建立TCP连接
    if (!mysql_real_connect(mysql,
        host.c_str(),
        user.c_str(),
        pwd.c_str(),
        dbname.c_str(),
        port,
        nullptr,
        0))
        /*
         .c_str()是给转成c的字符
    参数 1 mysql：连接句柄，没有它无法标识本次数据库会话，必须传；
    参数 2：host含义：MySQL 服务的 IP / 域名地址
    参数 3：user含义：数据库登录账号，string 变量转 C 字符串。
    参数 4：pwd含义：数据库账号对应的登录密码。
    参数 5 dbname（默认库）：可以传 nullptr，代表连接后不自动选中数据库，后续手动 mysql_select_db 切换，不能删掉这个位置；
    参数 6 port：默认 3306，直接写数字 3306 即可，不能空缺；
    参数 7 unix_socket：Windows / 远程 TCP 连接统一填 nullptr，留空位置；
    参数 8 client_flag：不需要扩展功能就填 0，不能省略。这个可以支持执行多个语句
         */
    {
        std::cerr << "数据库连接失败：" << mysql_error(mysql) << std::endl;
        return false;
    }

    // 设置utf8中文编码
    mysql_set_character_set(mysql, "utf8mb4");
    std::cout << "数据库连接成功" << std::endl;
    return true;
}

bool mysqlconn::execUpdate(const std::string& sql)
{//这个sql用字符串存执行语句，把字符串传给sql然后mysql再解析字符串去执行
    if (!mysql)
    {
        std::cerr << "数据库未连接，无法执行SQL" << std::endl;
        return false;
    }
    int ret = mysql_query(mysql, sql.c_str());
    if (ret != 0)
    {
        std::cerr << "增删改执行失败：" << mysql_error(mysql) << " SQL=" << sql << std::endl;
        return false;
    }
    return true;
}

MYSQL_RES* mysqlconn::execQuery(const std::string& sql)
{
    if (!mysql)
    {
        std::cerr << "数据库未连接，无法查询" << std::endl;
        return nullptr;
    }
    // 先释放上次残留结果集
    freeResult();
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        std::cerr << "查询失败：" << mysql_error(mysql) << " SQL=" << sql << std::endl;
        return nullptr;
    }
    res = mysql_store_result(mysql);
    return res;
}

void mysqlconn::freeResult()
{
    if (res)
    {
        mysql_free_result(res);
        res = nullptr;
    }
}

// 执行SELECT语句，获取单行int返回值（适配Login函数）
bool mysqlconn::callLoginFunc(int account, std::string& pwd, int& retCode)
{
    retCode = 0;
    if (!mysql)
    {
        std::cerr << "数据库未连接" << std::endl;
        return false;
    }
    // 拼接调用MySQL函数的SQL：SELECT Login(账号,密码)
    // account是int，不加单引号；pwd是字符串，用单引号包裹
    // 1. 转义密码字符串
    std::string escaped_pwd(pwd.size() * 2 + 1, '\0');  // 预设最坏情况每个字符都需要转义，预留’\0‘作结尾，C 语言字符串必须以空字节结尾
    mysql_real_escape_string(mysql, &escaped_pwd[0], pwd.c_str(), pwd.size());
    /*
    mysql官方的转义函数 MYSQL *mysql,        // ① 当前活跃的数据库连接句柄（关键！）
    char *to,            // ② 输出：转义后字符串存放地址（我们的escaped_pwd缓冲区首地址）
    const char *from,    // ③ 输入：原始未转义的密码
    unsigned long length // ④ 原始字符串长度，避免靠\0截断产生安全漏洞
    它会读取当前连接的字符集（utf8/gbk 等），精准识别该字符集下所有会破坏 SQL 的特殊字符；如果传空 / 断开的连接，转义失效，注入防护直接报废。
     */
    escaped_pwd.resize(strlen(escaped_pwd.c_str()));     // 调整到实际长度

    // 2. 安全拼接 SQL
    std::string sql = "SELECT Login(" + std::to_string(account)
                    + ", '" + escaped_pwd + "')";

    std::cout << "执行SQL: " << sql << std::endl;   // 调试用，发布时建议去掉
    //SELECT 函数名() → 这是自定义函数 FUNCTION 的调用语法这是 MySQL 语法硬性区分，不是后端 C++ 限制：    FUNCTION（函数）：有返回值 → 只能 SELECT func()
   //PROCEDURE（存储过程）：无单一返回值 → 只能 CALL proc()
    int res = mysql_query(mysql, sql.c_str());
    if (res != 0)
    {
        std::cerr << "调用函数失败:" << mysql_error(mysql) << std::endl;
        return false;
    }
    // 获取结果集
    /*
    普通增删改(mysql_query)没有返回表格数据，调用函数 / SELECT一定会返回一张虚拟表格，必须用这两句接收
    MySQL 会生成一张临时结果表放到 TCP 缓冲区里：
    这张表一直留在连接缓冲区，你不主动读取释放，下一次执行任何 SQL 都会直接卡死报错。
    任何以 SELECT 开头的语句必须加
     */
    MYSQL_RES* result = mysql_store_result(mysql);//把服务器返回的临时结果表从 TCP 缓冲区全部拉下来存到内存，返回一个 MYSQL_RES* 结果集句柄（不主动收走，下次执行任何 SQL 都会卡死）。
    MYSQL_ROW row = mysql_fetch_row(result);//从结果集里取一行，返回 MYSQL_ROW（一个 char** 数组，row[0] 是第一个字段）。多行就循环调，直到返回 nullptr。
    if (row != nullptr)
    {
        retCode = atoi(row[0]); // 拿到返回码 0/1/2/3,c++11新函数可以把字符串数字转换为整型
    }
    mysql_free_result(result); // 必须释放结果集
    return true;
}

bool mysqlconn::callMessage(nlohmann::json& rsp, uint32_t& outMessageId)
{
    outMessageId = 0;
     if (!mysql)
    {
        std::cerr << "数据库未连接" << std::endl;
        return false;
    }
    // 拼接调用M

    // JSON先按字符串接收，进入数据库前再转换为INT UNSIGNED对应的数值
    const std::string sendID = rsp.value<std::string>("sendId", "");
    const std::string targetID = rsp.value<std::string>("targetId", "");
    const std::string content = rsp.value<std::string>("content", "");
    const std::string sendTime = rsp.value<std::string>("sendTime", "");

    uint32_t sendIdValue = 0;
    uint32_t targetIdValue = 0;
    try
    {
        const unsigned long long parsedSendId = std::stoull(sendID);
        const unsigned long long parsedTargetId = std::stoull(targetID);
        if (parsedSendId > std::numeric_limits<uint32_t>::max() ||
            parsedTargetId > std::numeric_limits<uint32_t>::max())
        {
            throw std::out_of_range("消息ID超出INT UNSIGNED范围");
        }
        sendIdValue = static_cast<uint32_t>(parsedSendId);
        targetIdValue = static_cast<uint32_t>(parsedTargetId);
    }
    catch (const std::exception& e)
    {
        std::cerr << "消息ID格式错误: " << e.what() << std::endl;
        return false;
    }

    // 变量类型已确定：sendIdValue/targetIdValue为整数，文本和时间为字符串。
    // 后续拼接CALL Message时使用std::to_string(sendIdValue)和std::to_string(targetIdValue)。

    // context 是用户输入，必须转义防SQL注入
    std::string escaped_content(content.size() * 2 + 1, '\0');
    const unsigned long contentLength = mysql_real_escape_string(
        mysql, escaped_content.data(), content.c_str(), content.size());
    escaped_content.resize(contentLength);

    std::string escaped_sendTime(sendTime.size() * 2 + 1, '\0');
    const unsigned long sendTimeLength = mysql_real_escape_string(
        mysql, escaped_sendTime.data(), sendTime.c_str(), sendTime.size());
    escaped_sendTime.resize(sendTimeLength);

    // 拼接调用存储过程的SQL：PROCEDURE只能用CALL调用，字符串字段用单引号包裹
    std::string sql = "CALL Message('" + escaped_content + "'"
                    + ", " + std::to_string(sendIdValue)
                    + ", " + std::to_string(targetIdValue)
                    + ", '" + escaped_sendTime + "', @out_msg_id)";

    std::cout << "执行SQL: " << sql << std::endl;   // 调试用，发布时建议去掉
    std::cout<<escaped_content<<std::endl;

    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        std::cerr << "调用存储过程失败:" << mysql_error(mysql) << std::endl;
        return false;
    }

    // CALL可能产生多个结果集，必须全部清空后才能执行SELECT读取OUT参数通用模板
    /*
     @brief 调用存储过程后，排空全部结果集的标准模板
    【MySQL C API 重要原理】
     1. CALL存储过程可能产生多个结果集数据包，存储过程内部每一条裸SELECT都会生成一份结果集；哪怕过程内部没有写SELECT，CALL本身也会返回状态包，数据包会堆积在TCP网络缓冲区。
     2. MySQL通信协议强制要求：必须把上一条命令所有结果集全部读取并释放完毕，数据库连接才会变为空闲； 如果缓冲区还有未消费的数据包，直接执行下一条mysql_query会报：Commands out of sync。
     3. ⚠️关键区分：
       我们业务需要的OUT输出值保存在MySQL服务端会话变量(@xxx)，不在CALL返回的结果集里！
       mysql_free_result仅仅释放C++客户端内存，不会修改服务端会话变量、不会改动数据库数据。
       排空结果集只是清空网络管道，并不会把我们要的返回值丢掉。
     4. 使用流程：①执行CALL xxx(..., @out_param);②执行本循环，消费、释放全部CALL吐出的结果集；③管道空闲后，再执行 SELECT @out_param; 获取真正需要的返回值。
     注意：存储函数(SELECT func())不需要该循环，函数永远只产生单个结果集。
 */
    int nextResult = 0;
    do
    {
        MYSQL_RES* callResult = mysql_store_result(mysql);
        if (callResult != nullptr)
        {
            mysql_free_result(callResult);
        }
        nextResult = mysql_next_result(mysql);
    } while (nextResult == 0);

    if (nextResult > 0)
    {
        std::cerr << "清理存储过程结果失败:" << mysql_error(mysql) << std::endl;
        return false;
    }

    if (mysql_query(mysql, "SELECT @out_msg_id") != 0)//把这个会话内存变量的值，包装成一行一列的结果集返回给 C++ 后端
    {
        std::cerr << "读取消息ID失败:" << mysql_error(mysql) << std::endl;
        return false;
    }

    MYSQL_RES* result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        std::cerr << "读取消息ID结果集失败:" << mysql_error(mysql) << std::endl;
        return false;
    }

    MYSQL_ROW resultRow = mysql_fetch_row(result);
    if (resultRow != nullptr && resultRow[0] != nullptr)
    {
        outMessageId = static_cast<uint32_t>(std::stoul(resultRow[0]));
    }
    mysql_free_result(result);
    return outMessageId > 0;
}

bool mysqlconn::callDeleteMessage(unsigned int messageId)
{
    int outCode = 0;
    if (!mysql)
    {
        std::cerr << "数据库未连接" << std::endl;
        return false;
    }

    if (messageId <= 0)
    {
        std::cerr << "消息缓存ID无效" << std::endl;
        return false;
    }

    const std::string sql = "CALL DeleteMessageCache("
                          + std::to_string(messageId) + ", @out_code)";

    // 删除缓存偶发失败时最多重试3次（含首次），避免残留缓存导致目标重复收信
    constexpr int maxAttempts = 3;
    for (int attempt = 1; attempt <= maxAttempts; ++attempt)
    {
        outCode = 0;
        std::cout << "执行SQL(第" << attempt << "次): " << sql << std::endl;   // 调试用，发布时建议去掉

        if (mysql_query(mysql, sql.c_str()) != 0)
        {
            std::cerr << "调用删除消息缓存过程失败:" << mysql_error(mysql) << std::endl;
            // 失败时先清空可能残留的结果集，避免重试时卡死
            int pending = 0;
            do
            {
                MYSQL_RES* callResult = mysql_store_result(mysql);
                if (callResult != nullptr)
                {
                    mysql_free_result(callResult);
                }
                pending = mysql_next_result(mysql);
            } while (pending == 0);
            if (attempt < maxAttempts)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            continue;
        }

        // CALL产生的结果集必须全部释放，才能继续读取OUT参数
        int nextResult = 0;
        do
        {
            MYSQL_RES* callResult = mysql_store_result(mysql);
            if (callResult != nullptr)
            {
                mysql_free_result(callResult);
            }
            nextResult = mysql_next_result(mysql);
        } while (nextResult == 0);

        if (nextResult > 0)
        {
            std::cerr << "清理删除过程结果失败:" << mysql_error(mysql) << std::endl;
            continue;
        }

        if (mysql_query(mysql, "SELECT @out_code") != 0)
        {
            std::cerr << "读取删除结果失败:" << mysql_error(mysql) << std::endl;
            continue;
        }

        MYSQL_RES* result = mysql_store_result(mysql);
        if (result == nullptr)
        {
            std::cerr << "读取删除结果集失败:" << mysql_error(mysql) << std::endl;
            continue;
        }

        MYSQL_ROW resultRow = mysql_fetch_row(result);
        if (resultRow != nullptr && resultRow[0] != nullptr)
        {
            outCode = std::stoi(resultRow[0]);
        }
        mysql_free_result(result);

        if (outCode == 1)
        {
            return true;
        }
        std::cerr << "过程删除缓存失败, outCode=" << outCode << std::endl;
    }
    return false;
}

bool mysqlconn::callLoadMessage(int targetID, std::vector<MessageInfo>& outMessages, int& retcode)
{
    outMessages.clear();
    retcode = 0;
    if (!mysql)
    {
        std::cerr << "数据库未连接" << std::endl;
        return false;
    }

    if (targetID <= 0)
    {
        std::cerr << "目标ID无效" << std::endl;
        return false;
    }

    // CALL 存储过程：第一个结果集是消息数据，@out_code 是过程返回码
    const std::string sql = "CALL LoadMessage(" + std::to_string(targetID) + ", @out_code)";
    std::cout << "执行SQL: " << sql << std::endl;   // 调试用，发布时建议去掉

    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        std::cerr << "调用加载消息过程失败:" << mysql_error(mysql) << std::endl;
        return false;
    }

    // 第一个结果集：消息数据，按 消息ID/发送者ID/目标ID/文本内容/发送时间 顺序读取（与当前存储过程字段顺序保持一致）
    MYSQL_RES* result = mysql_store_result(mysql);
    if (result != nullptr)
    {
        MYSQL_ROW row = nullptr;
        while ((row = mysql_fetch_row(result)) != nullptr)
        {
            MessageInfo m;
            m.messageId = (row[0] != nullptr) ? static_cast<uint32_t>(std::stoul(row[0])) : 0;
            m.senderId  = (row[1] != nullptr) ? static_cast<uint32_t>(std::stoul(row[1])) : 0;
            m.targetId  = (row[2] != nullptr) ? static_cast<uint32_t>(std::stoul(row[2])) : 0;
            m.content   = (row[3] != nullptr) ? row[3] : "";
            m.sendTime  = (row[4] != nullptr) ? row[4] : "";
            std::cout<<m.content<<std::endl;
            outMessages.push_back(m);
        }
        mysql_free_result(result);
    }

    // 清空剩余结果集，之后才能读取OUT参数
    int nextResult = 0;
    do
    {
        MYSQL_RES* callResult = mysql_store_result(mysql);
        if (callResult != nullptr)
        {
            mysql_free_result(callResult);
        }
        nextResult = mysql_next_result(mysql);
    } while (nextResult == 0);

    if (nextResult > 0)
    {
        std::cerr << "清理加载过程结果失败:" << mysql_error(mysql) << std::endl;
        return false;
    }

    if (mysql_query(mysql, "SELECT @out_code") != 0)
    {
        std::cerr << "读取加载结果失败:" << mysql_error(mysql) << std::endl;
        return false;
    }

    MYSQL_RES* codeResult = mysql_store_result(mysql);
    if (codeResult == nullptr)
    {
        std::cerr << "读取加载结果集失败:" << mysql_error(mysql) << std::endl;
        return false;
    }

    MYSQL_ROW codeRow = mysql_fetch_row(codeResult);
    if (codeRow != nullptr && codeRow[0] != nullptr)
    {
        retcode = std::stoi(codeRow[0]);
    }
    mysql_free_result(codeResult);

    return retcode == 1;
}

void mysqlconn::close(mysqlconn& conn)
{
    conn.freeResult();
    if (conn.mysql)
    {
        mysql_close(conn.mysql);
        conn.mysql = nullptr;
    }
}
