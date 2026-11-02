#include "json_shift.h"
#include"table.h"
#include <ctime>

namespace
{
    // ===== 未登录操作限流 =====
    // 规则：注册 / 获取注册账号 / 忘记密码 这三类请求会开启一个 10 秒的限流窗口；
    // 窗口内每发生一次未登录操作（含登录、断线重连）计数加一，超过 20 次直接拒包；
    // 窗口过期后，下一次请求把窗口重置为「当前时间 + 10 秒」并把计数归 1；
    // 登录成功后不再计数（login_state 为 true 时直接放行，并在登录成功处清零）。
    constexpr int kUnloginWindowSec = 10;  // 窗口长度（秒）
    constexpr int kUnloginMaxCount  = 20;  // 窗口内允许的最大次数

    // 会开启限流窗口的三类请求
    bool OpensLimitWindow(const std::string& type)
    {
        return type == "register"
            || type == "connect_for_register"
            || type == "modify_password";
    }

    // 纳入限流统计的未登录操作（三类开窗请求 + 登录 + 断线重连）
    bool IsLimitedOp(const std::string& type)
    {
        return OpensLimitWindow(type)
            || type == "login"
            || type == "reconnect_login";
    }

    // 请求类型 -> 回包类型：拒包也要沿用业务本来的回包类型，
    // 客户端才能在 login / 注册 / 取号 / 忘记密码 各自的分支里识别并提示
    std::string ResponseTypeOf(const std::string& type)
    {
        if (type == "login")                return "login_response";
        if (type == "reconnect_login")      return "reconnect_login_response";
        if (type == "register")             return "register_response";
        if (type == "connect_for_register") return "connect_for_register_response";
        if (type == "modify_password")      return "modify_password_response";
        return "error_response";
    }

    // 命中限流返回 true，并把拒包写进 rejectRsp
    bool RejectedByRateLimit(const std::string& type, const SessionPtr& session, std::string& rejectRsp)
    {
        // 已登录不再计数；非受控类型直接放行
        if (!session || session->login_state || !IsLimitedOp(type))
        {
            return false;
        }

        const time_t now = std::time(nullptr);

        // 还没有窗口：只有三类开窗请求才开启；单纯登录不开窗、不计数
        if (session->limit_time == 0)
        {
            if (OpensLimitWindow(type))
            {
                session->limit_time = now + kUnloginWindowSec;
                session->unlogin_count = 1;
            }
            return false;
        }

        // 窗口内：每来一次未登录操作计数加一，超过上限就拒包
        if (now < session->limit_time)
        {
            ++session->unlogin_count;
            if (session->unlogin_count > kUnloginMaxCount)
            {
                json rsp;
                rsp["type"] = ResponseTypeOf(type);
                rsp["code"] = 429;
                rsp["message"] = "您的点击太频繁请稍后再试";
                rsp["success"] = false;
                rejectRsp = rsp.dump() + "\n";
                return true;
            }
            return false;
        }

        // 窗口已过期：本次请求开启新窗口，计数归 1
        session->limit_time = now + kUnloginWindowSec;
        session->unlogin_count = 1;
        return false;
    }
}

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

        // 未登录限流：注册/获取账号/忘记密码开启 10 秒窗口，
        // 窗口内未登录操作（含登录）超过 20 次直接拒包
        std::string limitRsp;
        if (RejectedByRateLimit(type, m_session, limitRsp))
        {
            return limitRsp;
        }

        // ========== 根据type分发业务逻辑 ==========
        CmdType Ctype=JsonToCmdType(type);
        std::cout<<1;
        std::cout << "枚举值(int): " << static_cast<int>(Ctype) << std::endl;
        auto it = cmd_table.find(Ctype);
        std::string rsp;
        if(it != cmd_table.end())
        {
            CmdHandler hander = it->second;
            // 处理函数返回空串 = 这一条请求按协议不需要任何应答（agent_stop 就是这种，
            // 客户端发出即自行解锁，不等回执）。
            // 这里绝对不能无脑补 '\n'：否则对端会收到一行空数据，
            // 它的逐行 JSON 解析会直接报错，反而显得"发完就出问题"
            std::string body = hander(msg, conn, m_session);
            rsp = body.empty() ? std::string() : body + '\n';
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
