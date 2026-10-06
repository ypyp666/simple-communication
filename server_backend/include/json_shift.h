
#ifndef UNTITLED_CHAR_SHIFT_H
#define UNTITLED_CHAR_SHIFT_H
#include<iostream>
#include<string>
#include"json.hpp"
#include"mysql.h"
#include"OnlineSessionManager.h"
using json = nlohmann::json;
std::string JsonParsing(std::string input,mysqlconn &conn,SessionPtr m_session);


class json_shift
{
public:

};


#endif //UNTITLED_CHAR_SHIFT_H
