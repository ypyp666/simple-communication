#ifndef UNTITLED_FUNCTIONALFUNCTION_H
#define UNTITLED_FUNCTIONALFUNCTION_H
#include<iostream>
#include<string>
#include"mysql.h"
#include "json.hpp"
#include"OnlineSessionManager.h"
using json = nlohmann::json;
void Login(json& res, int account, std::string& pwd, mysqlconn& conn);
void Repost(json& res,json& message,SessionPtr target_session,mysqlconn& conn);
void Pull(json& res, SessionPtr session, mysqlconn& conn);
void DeleteCache(json& res,std::string serverId ,mysqlconn& conn);
void GetAccount(json& res, mysqlconn& conn);
// 账号和密码已由 HandleRegister 从请求包解析并转换好，这里不再碰原始 JSON
void Register(json& res, uint32_t newAccount, const std::string& pwd, mysqlconn& conn);
void ModifyPwd(json& res, uint32_t account, const std::string& password, mysqlconn& conn);

#endif //UNTITLED_FUNCTIONALFUNCTION_H
