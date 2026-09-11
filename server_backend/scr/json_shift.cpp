#include "json_shift.h"
#include"table.h"

std::string JsonParsing(std::string input,mysqlconn &conn,SessionPtr m_session)
{
    std::cout << "首字符ASCII: " << (int)input[0]
              << " | 总长度: " << input.size() <<" "<<input<< std::endl;
    try
    {
        // 1. 把字符串解析成json结构化对象
        json msg = json::parse(input);

        // 2. 取键值，示例：假设客户端发送 {"type":"login","account":"123","password":"456"}，把键值对应的值取出来
        std::string type = msg.value<std::string>("type", "");
        std::string account = msg.value<std::string>("account", "");
        std::string pwd = msg.value<std::string>("password", "");
        //std::string type = msg["type"];这种形式扩展性差虽然很简单推荐用上面的写法，键值若不存在不会报错

        std::cout << "消息类型：" << type << std::endl;
        std::cout << "账号：" << account << " 密码：" << pwd << std::endl;

        // ========== 根据type分发业务逻辑 ==========
        CmdType Ctype=JsonToCmdType(type);
        std::cout<<1;
        std::cout << "枚举值(int): " << static_cast<int>(Ctype) << std::endl;
        auto it = cmd_table.find(Ctype);
        std::string rsp;
        if(it != cmd_table.end())
        {
            CmdHandler hander = it->second;
            rsp = hander(msg, conn, m_session) + '\n';
        }
        else
        {
            rsp = json{
                {"type", "error_response"},
                {"code", 400},
                {"message", "暂不支持该请求类型"},
                {"success", false}
            }.dump() + "\n";
        }
        return rsp;
        /*if (type == "login")
        {
            // 这里调用你封装的MysqlConn数据库接口做登录查询
            // db.execQuery("select * from user where account='" + account + "' and pwd='" + pwd + "'");

            // 构造返回json
            json rsp;
            rsp["code"] = 200;
            rsp["message"] = "登录成功";
            rsp["data"]["user"] = account;

            // dump() 把json转回字符串，结尾加换行符和你的协议匹配
            std::string send_str = rsp.dump() + "\n";
            std::cout << "回复报文：" << send_str << std::endl;

            // 这里你可以把 send_str 返回出去给线程send，也可以加参数传fd
        }
        else if (type == "register")
        {
            // 注册逻辑，执行insert数据库
            json rsp;
            rsp["code"] = 201;
            rsp["message"] = "注册完成";
            std::string send_str = rsp.dump() + "\n";
        }*/
    }
    // 捕获非法json、缺少key等所有错误
    catch (json::parse_error& e)
    {
        std::cerr << "JSON解析失败：" << e.what() << " 原始报文：" << input << std::endl;
        return json{{"type", "error_response"}, {"code",400}, {"message","JSON格式错误"}, {"success", false}}.dump() + "\n";
    }
    catch (json::out_of_range& e)
    {
        std::cerr << "JSON缺少指定键：" << e.what() << std::endl;
        return json{{"type", "error_response"}, {"code",400}, {"message","请求字段缺失"}, {"success", false}}.dump() + "\n";
    }
    catch (std::exception& e)
    {
        std::cerr << "未知错误：" << e.what() << std::endl;
        return json{{"type", "error_response"}, {"code",500}, {"message","服务内部异常"}, {"success", false}}.dump() + "\n";
    }
    return json{{"type", "error_response"}, {"code",500}, {"message","未知故障"}, {"success", false}}.dump() + "\n";
}
