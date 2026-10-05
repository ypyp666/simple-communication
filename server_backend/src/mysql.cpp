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

bool mysqlconn::callModifyPwd(uint32_t account, const std::string& pwd, int& retCode)
{
    retCode = -1;
    if (!mysql)
    {
        std::cerr << "数据库未连接" << std::endl;
        return false;
    }
    if (account == 0 || pwd.empty() || pwd.size() > 15)
    {
        std::cerr << "修改密码参数无效" << std::endl;
        return false;
    }

    // 密码来自客户端，进入SQL前必须转义，不能依赖前端校验。
    std::string escapedPwd(pwd.size() * 2 + 1, '\0');
    const unsigned long escapedLength = mysql_real_escape_string(
        mysql, escapedPwd.data(), pwd.data(), static_cast<unsigned long>(pwd.size()));
    escapedPwd.resize(escapedLength);

    const std::string sql = "CALL ModifyPwd(" + std::to_string(account)
                          + ", '" + escapedPwd + "', @retcode)";
    std::cout << "执行修改密码存储过程，account=" << account << std::endl;

    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        std::cerr << "调用修改密码过程失败: " << mysql_error(mysql) << std::endl;
        return false;
    }

    // CALL可能返回多个结果集，必须全部消费后才能查询OUT参数。
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
        std::cerr << "清理修改密码过程结果失败: " << mysql_error(mysql) << std::endl;
        return false;
    }

    if (mysql_query(mysql, "SELECT @retcode") != 0)
    {
        std::cerr << "读取修改密码结果失败: " << mysql_error(mysql) << std::endl;
        return false;
    }

    MYSQL_RES* result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        std::cerr << "读取修改密码结果集失败: " << mysql_error(mysql) << std::endl;
        return false;
    }

    MYSQL_ROW resultRow = mysql_fetch_row(result);
    if (resultRow == nullptr)
    {
        std::cerr << "[ModifyPwd] OUT结果集没有数据行" << std::endl;
        mysql_free_result(result);
        return false;
    }
    if (resultRow[0] == nullptr)
    {
        // @retcode 为 NULL 说明过程没有执行到任何 SET retcode 的分支，
        // 属于异常情况，不能当成默认值 -1 静默放过
        std::cerr << "[ModifyPwd] @retcode为NULL，过程未设置返回码" << std::endl;
        mysql_free_result(result);
        return false;
    }

    std::cout << "[ModifyPwd] 原始返回码=" << resultRow[0] << std::endl;
    try
    {
        retCode = std::stoi(resultRow[0]);
    }
    catch (const std::exception& e)
    {
        std::cerr << "[ModifyPwd] 返回码格式错误: " << e.what() << std::endl;
        mysql_free_result(result);
        return false;
    }
    mysql_free_result(result);

    std::cout << "[ModifyPwd] 解析后 retCode=" << retCode << std::endl;
    // 只要成功读到返回码就算调用成功，业务结果交给 retCode 表达，
    // 否则上层按 retCode 分支的判断会失效（0=账号不存在永远走不到）
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

bool mysqlconn::callGetAccount(uint32_t& newAccount)
{
    newAccount = 0;
    std::cout << "[GetAccount] 开始调用，mysql句柄=" << mysql << std::endl;
    if (!mysql)
    {
        std::cerr << "[GetAccount] 数据库未连接" << std::endl;
        return false;
    }

    // GetAccount 是带两个 OUT 参数的存储过程，必须使用 CALL 调用
    const std::string sql = "CALL GetAccount(@out_account, @retcode)";
    std::cout << "执行SQL: " << sql << std::endl;   // 调试用，发布时建议去掉

    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        std::cerr << "[GetAccount] CALL执行失败: " << mysql_error(mysql) << std::endl;
        return false;
    }
    std::cout << "[GetAccount] CALL执行成功，初始字段数="
              << mysql_field_count(mysql) << std::endl;

    // CALL 的全部结果集必须清空，之后才能查询 OUT 参数
    int nextResult = 0;
    int resultSetCount = 0;
    do
    {
        MYSQL_RES* callResult = mysql_store_result(mysql);
        if (callResult != nullptr)
        {
            ++resultSetCount;
            std::cout << "[GetAccount] 清理第" << resultSetCount
                      << "个CALL结果集，列数=" << mysql_num_fields(callResult)
                      << ", 行数=" << mysql_num_rows(callResult) << std::endl;
            mysql_free_result(callResult);
        }
        else if (mysql_field_count(mysql) != 0)
        {
            std::cerr << "[GetAccount] CALL结果集读取失败: "
                      << mysql_error(mysql) << std::endl;
        }
        nextResult = mysql_next_result(mysql);
        std::cout << "[GetAccount] mysql_next_result返回=" << nextResult << std::endl;
    } while (nextResult == 0);

    if (nextResult > 0)
    {
        std::cerr << "[GetAccount] 清理CALL结果失败: " << mysql_error(mysql) << std::endl;
        return false;
    }

    std::cout << "[GetAccount] CALL结果清理完成，共" << resultSetCount
              << "个结果集，开始读取OUT变量" << std::endl;
    if (mysql_query(mysql, "SELECT @out_account, @retcode") != 0)
    {
        std::cerr << "[GetAccount] SELECT OUT变量失败: " << mysql_error(mysql) << std::endl;
        return false;
    }
    std::cout << "[GetAccount] SELECT OUT变量执行成功，字段数="
              << mysql_field_count(mysql) << std::endl;

    MYSQL_RES* result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        std::cerr << "[GetAccount] OUT变量结果集为空或读取失败: "
                  << mysql_error(mysql) << std::endl;
        return false;
    }

    bool success = false;
    std::cout << "[GetAccount] OUT结果集列数=" << mysql_num_fields(result)
              << ", 行数=" << mysql_num_rows(result) << std::endl;
    MYSQL_ROW row = mysql_fetch_row(result);
    if (row == nullptr)
    {
        std::cerr << "[GetAccount] OUT结果集没有数据行" << std::endl;
    }
    else
    {
        std::cout << "[GetAccount] 原始返回值: out_account="
                  << (row[0] != nullptr ? row[0] : "NULL")
                  << ", retcode=" << (row[1] != nullptr ? row[1] : "NULL")
                  << std::endl;
    }

    if (row != nullptr && row[0] != nullptr && row[1] != nullptr)
    {
        try
        {
            const int retcode = std::stoi(row[1]);
            std::cout << "[GetAccount] 解析 retcode=" << retcode << std::endl;
            if (retcode == 1)
            {
                const unsigned long long account = std::stoull(row[0]);
                std::cout << "[GetAccount] 解析 account=" << account << std::endl;
                if (account > 0 && account <= std::numeric_limits<uint32_t>::max())
                {
                    newAccount = static_cast<uint32_t>(account);
                    success = true;
                }
                else
                {
                    std::cerr << "[GetAccount] account无效或超出uint32_t范围" << std::endl;
                }
            }
            else
            {
                std::cerr << "[GetAccount] 数据库过程返回失败码 retcode="
                          << retcode << std::endl;
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "[GetAccount] 返回值格式错误: " << e.what() << std::endl;
        }
    }
    mysql_free_result(result);

    std::cout << "[GetAccount] 最终结果: success=" << std::boolalpha << success
              << ", newAccount=" << newAccount << std::noboolalpha << std::endl;
    return success;
}

bool mysqlconn::callRegister(uint32_t account, const std::string& pwd)
{
    if(!mysql)
    {
        std::cerr << "数据库未连接" << std::endl;
        return false;
    }

    if (account == 0 || pwd.empty())
    {
        std::cerr << "[Register] 参数无效" << std::endl;
        return false;
    }

    // 密码是用户输入，进SQL前必须转义防注入，不能依赖前端校验
    std::string escapedPwd(pwd.size() * 2 + 1, '\0');

    const unsigned long escapedLength = mysql_real_escape_string(
    mysql,
    escapedPwd.data(),
    pwd.data(),
    static_cast<unsigned long>(pwd.size()));

    escapedPwd.resize(escapedLength);

    // 字符串参数必须用单引号包裹：反引号是标识符(字段名)语法，写反引号会报Unknown column
    const std::string sql = "CALL Register(" + std::to_string(account)
                          + ", '" + escapedPwd + "', @retcode)";
    std::cout << "执行注册存储过程，account=" << account << std::endl;   // 不打印密码

     if (mysql_query(mysql, sql.c_str()) != 0)
    {
        std::cerr << "[Register] CALL执行失败: " << mysql_error(mysql) << std::endl;
        return false;
    }

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
        std::cerr << "[Register] 清理CALL结果失败: " << mysql_error(mysql) << std::endl;
        return false;
    }
 
    if (mysql_query(mysql, "SELECT  @retcode") != 0)
    {
        std::cerr << "Register SELECT OUT变量失败: " << mysql_error(mysql) << std::endl;
        return false;
    }

     MYSQL_RES* result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        std::cerr << "[Register] OUT变量结果集为空或读取失败: "
                  << mysql_error(mysql) << std::endl;
        return false;
    }

    int retcode=0;
    MYSQL_ROW row = mysql_fetch_row(result);
    if (row == nullptr || row[0] == nullptr)
    {
        std::cerr << "[Register] OUT结果集没有数据行" << std::endl;
    }
    else
    {
        try
        {
            // retcode必须从结果集解析，否则永远是初始值0，注册会被误判为失败
            retcode = std::stoi(row[0]);
            std::cout << "[Register] 解析 retcode=" << retcode << std::endl;
        }
        catch (const std::exception& e)
        {
            std::cerr << "[Register] 返回值格式错误: " << e.what() << std::endl;
        }
    }
    mysql_free_result(result);   // 无论成功失败都只释放一次，避免内存泄漏

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

// ============================================================================
// 好友/联系人功能：公共辅助
// ============================================================================

// 排空CALL产生的全部结果集：MySQL协议要求消费完上一条命令的所有数据包，
// 连接才会变为空闲，否则后续SELECT @变量会报 Commands out of sync
bool mysqlconn::drainCallResults()
{
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
        std::cerr << "清理CALL结果集失败: " << mysql_error(mysql) << std::endl;
        return false;
    }
    return true;
}

// 读取OUT会话变量并解析为int，任何一步异常都返回false，避免上层误判成功
bool mysqlconn::fetchOutParameter(const std::string& varName, int& outValue)
{
    outValue = 0;
    const std::string sql = "SELECT " + varName;
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        std::cerr << "读取会话变量失败: " << mysql_error(mysql) << std::endl;
        return false;
    }

    MYSQL_RES* result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        std::cerr << "读取会话变量结果集失败: " << mysql_error(mysql) << std::endl;
        return false;
    }

    MYSQL_ROW resultRow = mysql_fetch_row(result);
    if (resultRow == nullptr || resultRow[0] == nullptr)
    {
        // 会话变量为NULL说明过程没有走到任何SET retcode分支，属于异常情况
        std::cerr << "会话变量 " << varName << " 为NULL" << std::endl;
        mysql_free_result(result);
        return false;
    }

    try
    {
        outValue = std::stoi(resultRow[0]);
    }
    catch (const std::exception& e)
    {
        std::cerr << "会话变量 " << varName << " 格式错误: " << e.what() << std::endl;
        mysql_free_result(result);
        return false;
    }
    mysql_free_result(result);
    return true;
}

// ============================================================================
// 好友/联系人功能：数据库过程调用
// ============================================================================

bool mysqlconn::callAddFriend(uint32_t applyId, uint32_t targetId, const std::string& applyMsg,
                              int& retCode, uint32_t& outRequestId, std::string& outApplyName)
{
    retCode = -1;
    outRequestId = 0;
    outApplyName.clear();
    if (!mysql)
    {
        std::cerr << "数据库未连接" << std::endl;
        return false;
    }
    if (applyId == 0 || targetId == 0)
    {
        std::cerr << "好友申请参数无效" << std::endl;
        return false;
    }

    // applyMsg 是客户端传入的申请留言，属于用户输入，进SQL前必须转义防注入
    std::string escapedMsg(applyMsg.size() * 2 + 1, '\0');
    const unsigned long escapedLength = mysql_real_escape_string(
        mysql, escapedMsg.data(), applyMsg.data(), static_cast<unsigned long>(applyMsg.size()));
    escapedMsg.resize(escapedLength);

    const std::string sql = "CALL AddFriend(" + std::to_string(applyId)
                          + ", " + std::to_string(targetId)
                          + ", '" + escapedMsg + "', @retcode)";
    // 打印完整语句，可直接复制到 mysql 客户端里逐步排查
    std::cout << "[AddFriend] 执行SQL: " << sql << std::endl;

    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        std::cerr << "调用好友申请过程失败: " << mysql_error(mysql) << std::endl;
        std::cerr << "[AddFriend] 失败SQL: " << sql << std::endl;
        return false;
    }

    // 逐个消费结果集：AddFriend只往contact_cache写申请，过程默认不回吐结果集。
    // 若将来给AddFriend补上联表user的 SELECT user_id, user_name，这里会自动取到申请人昵称；
    // 在补之前 outApplyName 保持为空串，实时推送的name字段就是空的。
    bool captured = false;
    int nextResult = 0;
    int resultIndex = 0;
    do
    {
        MYSQL_RES* result = mysql_store_result(mysql);
        if (result != nullptr)
        {
            const unsigned int fieldCount = mysql_num_fields(result);
            MYSQL_FIELD* fields = mysql_fetch_fields(result);
            std::cout << "[AddFriend] 结果集#" << resultIndex << " 列数=" << fieldCount << " 列名=";
            for (unsigned int i = 0; i < fieldCount; ++i)
            {
                std::cout << (i == 0 ? "" : ",") << fields[i].name;
            }
            std::cout << std::endl;

            if (!captured && fieldCount >= 2)
            {
                MYSQL_ROW row = mysql_fetch_row(result);
                if (row != nullptr)
                {
                    outApplyName = (row[1] != nullptr) ? row[1] : "";
                    captured = true;
                    std::cout << "[AddFriend] 取到申请人昵称: " << outApplyName << std::endl;
                }
            }
            mysql_free_result(result);
            ++resultIndex;
        }
        nextResult = mysql_next_result(mysql);
    }
    while (nextResult == 0);

    if (nextResult > 0)
    {
        std::cerr << "好友申请过程结果集消费失败: " << mysql_error(mysql) << std::endl;
        return false;
    }

    if (!fetchOutParameter("@retcode", retCode))
    {
        return false;
    }
    std::cout << "[AddFriend] @retcode=" << retCode << std::endl;

    // AddFriend只有OUT retcode，没有返回新申请ID，成功时用同连接的LAST_INSERT_ID取
    if (retCode == 1)
    {
        if (mysql_query(mysql, "SELECT LAST_INSERT_ID()") != 0)
        {
            std::cerr << "读取好友申请ID失败: " << mysql_error(mysql) << std::endl;
            return false;
        }
        MYSQL_RES* idResult = mysql_store_result(mysql);
        if (idResult != nullptr)
        {
            MYSQL_ROW idRow = mysql_fetch_row(idResult);
            if (idRow != nullptr && idRow[0] != nullptr)
            {
                outRequestId = static_cast<uint32_t>(std::stoul(idRow[0]));
            }
            mysql_free_result(idResult);
        }
        std::cout << "[AddFriend] LAST_INSERT_ID=" << outRequestId << std::endl;
    }

    std::cout << "[AddFriend] 汇总 applyId=" << applyId
              << ", targetId=" << targetId
              << ", retCode=" << retCode
              << ", requestId=" << outRequestId
              << ", applyName=" << outApplyName << std::endl;
    return true;
}

bool mysqlconn::callLoadNewFriend(uint32_t account, std::vector<FriendApplyInfo>& outApplies, int& retCode)
{
    outApplies.clear();
    retCode = 0;
    if (!mysql)
    {
        std::cerr << "数据库未连接" << std::endl;
        return false;
    }
    if (account == 0)
    {
        std::cerr << "查询好友申请失败：账号无效" << std::endl;
        return false;
    }

    const std::string sql = "CALL LoadNewFriend(" + std::to_string(account) + ", @retcode)";
    std::cout << "执行SQL: " << sql << std::endl;

    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        std::cerr << "调用加载好友申请过程失败: " << mysql_error(mysql) << std::endl;
        return false;
    }

    // 结果集列顺序：request_id, account_id, target_id, a_user_name, t_user_name, send_time, status, apply_msg
    MYSQL_RES* result = mysql_store_result(mysql);
    if (result != nullptr)
    {
        const unsigned int fieldCount = mysql_num_fields(result);
        if (fieldCount < 8)
        {
            std::cerr << "加载好友申请结果集列数异常: " << fieldCount << "（期望8列）" << std::endl;
            mysql_free_result(result);
            return false;
        }
        MYSQL_ROW row = nullptr;
        while ((row = mysql_fetch_row(result)) != nullptr)
        {
            FriendApplyInfo info;
            info.requestId  = (row[0] != nullptr) ? static_cast<uint32_t>(std::stoul(row[0])) : 0;
            info.applyId    = (row[1] != nullptr) ? static_cast<uint32_t>(std::stoul(row[1])) : 0;
            info.targetId   = (row[2] != nullptr) ? static_cast<uint32_t>(std::stoul(row[2])) : 0;
            info.applyName  = (row[3] != nullptr) ? row[3] : "";
            info.targetName = (row[4] != nullptr) ? row[4] : "";
            info.sendTime   = (row[5] != nullptr) ? row[5] : "";
            info.status     = (row[6] != nullptr) ? std::stoi(row[6]) : 0;
            info.applyMsg   = (row[7] != nullptr) ? row[7] : "";
            outApplies.push_back(info);
        }
        mysql_free_result(result);
    }

    if (!drainCallResults() || !fetchOutParameter("@retcode", retCode))
    {
        return false;
    }

    // retcode 1=查到申请 2=无待处理申请，两者都算调用成功
    return retCode == 1 || retCode == 2;
}

bool mysqlconn::callAcceptFriend(uint32_t requestId, uint32_t account, int& retCode,
                                 FriendBriefInfo& outFriend)
{
    retCode = -1;
    outFriend = FriendBriefInfo{};
    if (!mysql)
    {
        std::cerr << "数据库未连接" << std::endl;
        return false;
    }
    if (requestId == 0 || account == 0)
    {
        std::cerr << "同意好友申请参数无效" << std::endl;
        return false;
    }

    // 过程名为AgreeRequest；成功路径末尾回吐一条2列结果集：user_id, user_name
    // （新好友刚建立关系，mark 必为''，所以过程不带 remark，这里也留空）
    const std::string sql = "CALL AgreeRequest(" + std::to_string(requestId)
                          + ", " + std::to_string(account) + ", @retcode)";
    // 打印完整语句，可直接复制到 mysql 客户端里逐步排查
    std::cout << "[AgreeRequest] 执行SQL: " << sql << std::endl;

    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        std::cerr << "调用同意好友申请过程失败: " << mysql_error(mysql) << std::endl;
        std::cerr << "[AgreeRequest] 失败SQL: " << sql << std::endl;
        return false;
    }

    // 失败路径过程直接LEAVE，不产生任何结果集；成功路径才有那一条结果集，
    // 所以这里逐个消费结果集，遇到带资料的就取走，不能假定结果集一定存在
    bool captured = false;
    int nextResult = 0;
    int resultIndex = 0;
    do
    {
        MYSQL_RES* result = mysql_store_result(mysql);
        if (result != nullptr)
        {
            const unsigned int fieldCount = mysql_num_fields(result);
            MYSQL_FIELD* fields = mysql_fetch_fields(result);
            std::cout << "[AgreeRequest] 结果集#" << resultIndex << " 列数=" << fieldCount << " 列名=";
            for (unsigned int i = 0; i < fieldCount; ++i)
            {
                std::cout << (i == 0 ? "" : ",") << fields[i].name;
            }
            std::cout << std::endl;

            // 逐行打印，把过程实际吐出来的数据全部暴露出来（便于比对列序与取值）
            MYSQL_ROW row = nullptr;
            int rowIndex = 0;
            while ((row = mysql_fetch_row(result)) != nullptr)
            {
                std::cout << "[AgreeRequest] 结果集#" << resultIndex << " 行#" << rowIndex << " : ";
                for (unsigned int i = 0; i < fieldCount; ++i)
                {
                    std::cout << fields[i].name << "="
                              << (row[i] != nullptr ? row[i] : "NULL") << (i + 1 < fieldCount ? ", " : "");
                }
                std::cout << std::endl;
                ++rowIndex;
            }
            // mysql_fetch_row 已把游标走到末尾，需要把结果集内部指针重置后再取第一行
            mysql_data_seek(result, 0);

            // 过程返回 user_id, user_name 两列；若将来补了 remark 成三列也能兼容
            if (!captured && fieldCount >= 2)
            {
                row = mysql_fetch_row(result);
                if (row != nullptr)
                {
                    outFriend.accountId = (row[0] != nullptr) ? static_cast<uint32_t>(std::stoul(row[0])) : 0;
                    outFriend.name      = (row[1] != nullptr) ? row[1] : "";
                    outFriend.remark    = (fieldCount >= 3 && row[2] != nullptr) ? row[2] : "";
                    captured = true;
                    std::cout << "[AgreeRequest] 捕获新好友: accountId=" << outFriend.accountId
                              << ", name=" << outFriend.name
                              << ", remark=" << outFriend.remark << std::endl;
                }
            }
            else if (!captured)
            {
                std::cerr << "[AgreeRequest] 结果集列数不足2列(" << fieldCount
                          << ")，无法取出新好友资料" << std::endl;
            }
            mysql_free_result(result);
            ++resultIndex;
        }
        nextResult = mysql_next_result(mysql);
    }
    while (nextResult == 0);

    if (nextResult > 0)
    {
        std::cerr << "同意好友申请过程结果集消费失败: " << mysql_error(mysql) << std::endl;
        return false;
    }

    if (!fetchOutParameter("@retcode", retCode))
    {
        return false;
    }

    std::cout << "[AgreeRequest] @retcode=" << retCode << std::endl;
    if (!captured)
    {
        std::cout << "[AgreeRequest] 本次无结果集（多为失败路径或用例无数据）" << std::endl;
    }
    std::cout << "[AgreeRequest] 汇总 requestId=" << requestId
              << ", account=" << account
              << ", retCode=" << retCode
              << ", friendId=" << outFriend.accountId
              << ", friendName=" << outFriend.name
              << ", remark=" << outFriend.remark << std::endl;
    return true;
}

bool mysqlconn::callRejectFriend(uint32_t requestId, uint32_t account, int& retCode)
{
    retCode = -1;
    if (!mysql)
    {
        std::cerr << "数据库未连接" << std::endl;
        return false;
    }
    if (requestId == 0 || account == 0)
    {
        std::cerr << "拒绝好友申请参数无效" << std::endl;
        return false;
    }

    // 过程名为RejectRequest；retcode 1=拒绝成功 2=申请不存在/已处理/无权处理 -1=过程异常
    const std::string sql = "CALL RejectRequest(" + std::to_string(requestId)
                          + ", " + std::to_string(account) + ", @retcode)";
    std::cout << "执行SQL: " << sql << std::endl;

    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        std::cerr << "调用拒绝好友申请过程失败: " << mysql_error(mysql) << std::endl;
        return false;
    }

    if (!drainCallResults() || !fetchOutParameter("@retcode", retCode))
    {
        return false;
    }

    std::cout << "[RejectRequest] retCode=" << retCode << std::endl;
    return true;
}

bool mysqlconn::callDeleteFriend(uint32_t ownId, uint32_t targetId, int& retCode)
{
    retCode = -1;
    if (!mysql)
    {
        std::cerr << "数据库未连接" << std::endl;
        return false;
    }
    if (ownId == 0 || targetId == 0)
    {
        std::cerr << "删除好友参数无效" << std::endl;
        return false;
    }

    const std::string sql = "CALL DeleteFriend(" + std::to_string(ownId)
                          + ", " + std::to_string(targetId) + ", @retcode)";
    std::cout << "执行SQL: " << sql << std::endl;

    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        std::cerr << "调用删除好友过程失败: " << mysql_error(mysql) << std::endl;
        return false;
    }

    if (!drainCallResults() || !fetchOutParameter("@retcode", retCode))
    {
        return false;
    }

    std::cout << "[DeleteFriend] retCode=" << retCode << std::endl;
    return true;
}

bool mysqlconn::callModifyMark(uint32_t account, uint32_t targetId, const std::string& newMark, int& retCode)
{
    retCode = -1;
    if (!mysql)
    {
        std::cerr << "数据库未连接" << std::endl;
        return false;
    }
    if (account == 0 || targetId == 0)
    {
        std::cerr << "修改备注参数无效" << std::endl;
        return false;
    }

    // 备注是用户输入，进SQL前必须转义防注入，不能依赖前端校验
    std::string escapedMark(newMark.size() * 2 + 1, '\0');
    const unsigned long escapedLength = mysql_real_escape_string(
        mysql, escapedMark.data(), newMark.data(), static_cast<unsigned long>(newMark.size()));
    escapedMark.resize(escapedLength);

    const std::string sql = "CALL ModifyMark(" + std::to_string(account)
                          + ", " + std::to_string(targetId)
                          + ", '" + escapedMark + "', @retcode)";
    std::cout << "执行SQL: " << sql << std::endl;

    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        std::cerr << "调用修改备注过程失败: " << mysql_error(mysql) << std::endl;
        return false;
    }

    if (!drainCallResults() || !fetchOutParameter("@retcode", retCode))
    {
        return false;
    }

    std::cout << "[ModifyMark] retCode=" << retCode << std::endl;
    return true;
}

bool mysqlconn::callLoadOldFriend(uint32_t account, std::vector<FriendBriefInfo>& outContacts, int& retCode)
{
    outContacts.clear();
    retCode = 0;
    if (!mysql)
    {
        std::cerr << "数据库未连接" << std::endl;
        return false;
    }
    if (account == 0)
    {
        std::cerr << "查询好友列表失败：账号无效" << std::endl;
        return false;
    }

    const std::string sql = "CALL LoadOldFriend(" + std::to_string(account) + ", @retcode)";
    std::cout << "执行SQL: " << sql << std::endl;

    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        std::cerr << "调用加载好友列表过程失败: " << mysql_error(mysql) << std::endl;
        return false;
    }

    // 结果集列顺序：account_id, name, remark, create_time
    // 昵称与备注都由过程联表user直接带出，dbuser没有SELECT权限，必须依赖过程
    MYSQL_RES* result = mysql_store_result(mysql);
    if (result != nullptr)
    {
        const unsigned int fieldCount = mysql_num_fields(result);
        if (fieldCount < 4)
        {
            std::cerr << "加载好友列表结果集列数异常: " << fieldCount << std::endl;
            mysql_free_result(result);
            return false;
        }
        MYSQL_ROW row = nullptr;
        while ((row = mysql_fetch_row(result)) != nullptr)
        {
            FriendBriefInfo info;
            info.accountId  = (row[0] != nullptr) ? static_cast<uint32_t>(std::stoul(row[0])) : 0;
            info.name       = (row[1] != nullptr) ? row[1] : "";
            info.remark     = (row[2] != nullptr) ? row[2] : "";
            info.createTime = (row[3] != nullptr) ? row[3] : "";
            outContacts.push_back(info);
        }
        mysql_free_result(result);
    }

    if (!drainCallResults() || !fetchOutParameter("@retcode", retCode))
    {
        return false;
    }

    // retcode 1=查到好友 2=无好友，两者都算调用成功
    return retCode == 1 || retCode == 2;
}

bool mysqlconn::callSearchUser(uint32_t account, std::vector<UserBriefInfo>& outUsers, int& retCode)
{
    outUsers.clear();
    retCode = 0;
    if (!mysql)
    {
        std::cerr << "数据库未连接" << std::endl;
        return false;
    }
    if (account == 0)
    {
        std::cerr << "搜索用户失败：账号无效" << std::endl;
        return false;
    }

    const std::string sql = "CALL SearchUser(" + std::to_string(account) + ", @retcode)";
    std::cout << "执行SQL: " << sql << std::endl;

    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        std::cerr << "调用搜索用户过程失败: " << mysql_error(mysql) << std::endl;
        return false;
    }

    // 第一个结果集列顺序：user_id, user_name, create_time
    MYSQL_RES* result = mysql_store_result(mysql);
    if (result != nullptr)
    {
        const unsigned int fieldCount = mysql_num_fields(result);
        if (fieldCount < 3)
        {
            std::cerr << "搜索用户结果集列数异常: " << fieldCount << std::endl;
            mysql_free_result(result);
            return false;
        }
        MYSQL_ROW row = nullptr;
        while ((row = mysql_fetch_row(result)) != nullptr)
        {
            UserBriefInfo info;
            info.userId     = (row[0] != nullptr) ? static_cast<uint32_t>(std::stoul(row[0])) : 0;
            info.userName   = (row[1] != nullptr) ? row[1] : "";
            info.createTime = (row[2] != nullptr) ? row[2] : "";
            outUsers.push_back(info);
        }
        mysql_free_result(result);
    }

    if (!drainCallResults() || !fetchOutParameter("@retcode", retCode))
    {
        return false;
    }

    // retcode 1=查到用户 2=未找到，两者都算调用成功
    return retCode == 1 || retCode == 2;
}

bool mysqlconn::callModifyName(uint32_t account, const std::string& newName, int& retCode)
{
    retCode = -1;
    if (!mysql)
    {
        std::cerr << "数据库未连接" << std::endl;
        return false;
    }
    if (account == 0)
    {
        std::cerr << "修改用户名参数无效" << std::endl;
        return false;
    }

    // 用户名是用户输入，进SQL前必须转义防注入
    std::string escapedName(newName.size() * 2 + 1, '\0');
    const unsigned long escapedLength = mysql_real_escape_string(
        mysql, escapedName.data(), newName.data(), static_cast<unsigned long>(newName.size()));
    escapedName.resize(escapedLength);

    const std::string sql = "CALL ModifyName(" + std::to_string(account)
                          + ", '" + escapedName + "', @retcode)";
    std::cout << "执行修改用户名存储过程, account=" << account << std::endl;

    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        std::cerr << "调用修改用户名过程失败: " << mysql_error(mysql) << std::endl;
        return false;
    }

    if (!drainCallResults() || !fetchOutParameter("@retcode", retCode))
    {
        return false;
    }

    std::cout << "[ModifyName] retCode=" << retCode << std::endl;
    return true;
}

bool mysqlconn::callLoadDelFriendEvent(uint32_t userId, std::vector<DelFriendEventInfo>& outEvents, int& retCode)
{
    outEvents.clear();
    retCode = 0;
    if (!mysql)
    {
        std::cerr << "数据库未连接" << std::endl;
        return false;
    }
    if (userId == 0)
    {
        std::cerr << "拉取删除墓碑失败：账号无效" << std::endl;
        return false;
    }

    // 入参是"当前登录账号"，过程内部是 WHERE target_id = user_id，
    // 即查出所有"别人删掉了我"的事件，返回该事件的删除者(send_id)
    const std::string sql = "CALL LoadDelFriendEvent(" + std::to_string(userId) + ", @retcode)";
    std::cout << "[LoadDelFriendEvent] 执行SQL: " << sql << std::endl;

    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        std::cerr << "调用加载删除墓碑过程失败: " << mysql_error(mysql) << std::endl;
        std::cerr << "[LoadDelFriendEvent] 失败SQL: " << sql << std::endl;
        return false;
    }

    // 结果集列顺序：send_id, target_id
    MYSQL_RES* result = mysql_store_result(mysql);
    if (result != nullptr)
    {
        const unsigned int fieldCount = mysql_num_fields(result);
        if (fieldCount < 2)
        {
            std::cerr << "加载删除墓碑结果集列数异常: " << fieldCount << "（期望2列）" << std::endl;
            mysql_free_result(result);
            return false;
        }
        MYSQL_ROW row = nullptr;
        while ((row = mysql_fetch_row(result)) != nullptr)
        {
            DelFriendEventInfo info;
            info.sendId   = (row[0] != nullptr) ? static_cast<uint32_t>(std::stoul(row[0])) : 0;
            info.targetId = (row[1] != nullptr) ? static_cast<uint32_t>(std::stoul(row[1])) : 0;
            outEvents.push_back(info);
        }
        mysql_free_result(result);
    }

    if (!drainCallResults() || !fetchOutParameter("@retcode", retCode))
    {
        return false;
    }

    std::cout << "[LoadDelFriendEvent] account=" << userId
              << ", 墓碑数=" << outEvents.size()
              << ", retCode=" << retCode << std::endl;
    return true;
}

bool mysqlconn::callClearDeleteCache(uint32_t sendId, uint32_t targetId, int& retCode)
{
    retCode = -1;
    if (!mysql)
    {
        std::cerr << "数据库未连接" << std::endl;
        return false;
    }
    if (sendId == 0 || targetId == 0)
    {
        std::cerr << "清除删除墓碑失败：参数无效" << std::endl;
        return false;
    }

    // 过程一次只清一条，入参顺序是 (发起删除的人, 被删的人)，
    // 所以第一个传墓碑里的发起者 sendId，第二个传本人账号 targetId
    const std::string sql = "CALL Clear_delete_cache(" + std::to_string(sendId)
                          + ", " + std::to_string(targetId) + ", @retcode)";
    std::cout << "[Clear_delete_cache] 执行SQL: " << sql << std::endl;

    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        std::cerr << "调用清除删除墓碑过程失败: " << mysql_error(mysql) << std::endl;
        std::cerr << "[Clear_delete_cache] 失败SQL: " << sql << std::endl;
        return false;
    }

    if (!drainCallResults() || !fetchOutParameter("@retcode", retCode))
    {
        return false;
    }

    std::cout << "[Clear_delete_cache] sendId=" << sendId
              << ", targetId=" << targetId
              << ", retCode=" << retCode << std::endl;
    return true;
}

