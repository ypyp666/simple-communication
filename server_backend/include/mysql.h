#include <mysql/mysql.h>
#include <string>
#include <cstdint>
#include <cstring>
#include <vector>
#include"json.hpp"
#ifndef UNTITLED_MYSQL_H
#define UNTITLED_MYSQL_H

// 单条消息的完整信息，供 callLoadMessage 返回结果集用
struct MessageInfo {
    uint32_t messageId;   // 消息ID
    uint32_t senderId;    // 发送者ID
    uint32_t targetId;    // 目标ID
    std::string sendTime;  // 发送时间
    std::string content;  //文本内容
};


class mysqlconn {
private:
    MYSQL* mysql;         // mysql核心句柄,是整个数据库会话的总容器。
    //程序启动初始化 → 全程复用执行所有 SQL → 程序退出 / 不用时关闭销毁。所有数据库操作都要靠这个mysql指针传参：
    MYSQL_RES* res;       // 查询结果集
    MYSQL_ROW row;         // 单行数据（字符串数组指针）,本质是char**类型指针
public:
    mysqlconn();
    ~mysqlconn(); // 对外接口1：建立数据库连接（只在main初始化一次/线程内单独调用）
    bool connect(const std::string& host, int port,
                 const std::string& user, const std::string& pwd,
                 const std::string& dbname);

    // 对外接口2：增/删/改统一入口 insert update delete
    bool execUpdate(const std::string& sql);

    // 对外接口3：查询select，返回结果集给上层遍历
    MYSQL_RES* execQuery(const std::string& sql);

    // 对外接口4：释放查询结果内存
    void freeResult();

    // 对外接口5：关闭数据库连接，static无成员访问
    static void close(mysqlconn& conn);

    //对外接口6
    bool callLoginFunc(int account,  std::string& pwd, int& retCode);

    //对外接口7：修改密码数据库过程
    bool callModifyPwd(uint32_t account, const std::string& pwd, int& retCode);

    //对外接口8：注册数据库过程占位
    bool callRegister(uint32_t account, const std::string& pwd);

    //对外接口9：获取注册账号数据库函数占位
    bool callGetAccount(uint32_t& newAccount);

    //对外接口10数据库消息存储
    bool callMessage(nlohmann::json& rsp, uint32_t& outMessageId);

    //对外接口11删除消息缓存
    bool callDeleteMessage(unsigned int messageId);

    //对外接口12加载消息缓存（retcode为过程返回码，消息数据通过outMessages带回）
    bool callLoadMessage(int targetID, std::vector<MessageInfo>& outMessages, int& retcode);

};


#endif //UNTITLED_MYSQL_H
