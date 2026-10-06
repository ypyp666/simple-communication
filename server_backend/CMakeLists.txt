cmake_minimum_required(VERSION 4.2)
project(backend VERSION 0.1)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 关键：把include目录加入全局头文件搜索路径
include_directories(include)
include_directories(nlohmann)

# 查找线程库（你已经写了，保留）
find_package(Threads REQUIRED)

# MySQL 依赖查找
find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQL REQUIRED mysqlclient)

# 生成即时通信服务器可执行程序
add_executable(im_server
        src/main.cpp
        src/mysql.cpp
        src/json_shift.cpp
        src/table.cpp
        src/FunctionalFunction.cpp
        src/tcp_server.cpp
        src/OnlineSessionManager.cpp

        include/mysql.h
        include/json_shift.h
        include/table.h
        include/FunctionalFunction.h
        include/tcp_server.h
        include/OnlineSessionManager.h

        nlohmann/json.hpp




)

# 链接库统一写在一条，更整洁
target_link_libraries(im_server PRIVATE
        ${MYSQL_LIBRARIES}
        Threads::Threads
)

# MySQL头文件、库目录保留不变
target_include_directories(im_server PRIVATE ${MYSQL_INCLUDE_DIRS})
target_link_directories(im_server PRIVATE ${MYSQL_LIBRARY_DIRS})