# =============================================================================
# CMake 简介
# =============================================================================
# CMake 是一个构建系统生成器（Build System Generator）
# 它不是编译器，而是"包工头"——负责协调整个项目的编译流程
#
# 工作流程：
#   1. CMake 读取 CMakeLists.txt（配置文件）
#   2. 分析项目结构、文件依赖、库依赖
#   3. 生成平台特定的构建文件（Windows 生成 .sln/.mak，Linux 生成 Makefile）
#   4. 调用编译器（g++/cl.exe）执行真正的编译工作
#
# 为什么需要 CMake？
#   - 项目文件很多时，手动敲编译命令不现实
#   - CMake 自动处理文件依赖、编译顺序、链接库等问题
#   - 一次编写，多平台构建（Windows/Linux/Mac）
# =============================================================================

# 设置最低 CMake 版本要求
cmake_minimum_required(VERSION 3.16)

# 导出编译命令，供 VSCode IntelliSense 使用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 定义项目名称（自动创建 PROJECT_NAME 变量）
project(ChatUI)

# =============================================================================
# C++ 标准设置
# =============================================================================
# CMAKE_CXX_STANDARD: 指定 C++ 标准版本（17 = C++17）
# CMAKE_CXX_STANDARD_REQUIRED: 强制要求该标准，如果编译器不支持则报错
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =============================================================================
# Qt 自动化工具（Meta-Object Compiler）
# =============================================================================
# CMake 自动化运行 Qt 的元对象编译器（MOC）、资源编译器（RCC）、UI 编译器（UIC）
#
# AUTOMOC: 自动处理 Q_OBJECT 宏，生成元对象代码
# AUTORCC: 自动处理 .qrc 资源文件
# AUTOUIC: 自动处理 .ui 文件（Qt Designer 设计的界面）
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# =============================================================================
# 查找 Qt 库
# =============================================================================
# find_package: 查找并加载外部项目配置
# REQUIRED: 表示必须找到，找不到则 CMake 失败
# COMPONENTS: 需要查找的 Qt 模块
#   - Widgets: GUI 组件（按钮、窗口、布局等）
#   - Core: 核心功能（信号槽、事件循环、容器等）
    #   - Gui: 图形用户界面基础（绘图、字体等）
find_package(Qt6 REQUIRED COMPONENTS Widgets Core Gui Sql Svg Network)

# 将 include 目录添加到编译器的头文件搜索路径
# 这样 #include "xxx.h" 才能找到我们自定义的头文件
include_directories(include)
include_directories(include_backend)



# =============================================================================
# 定义可执行文件
# =============================================================================
# add_executable: 创建一个可执行目标
# 语法: add_executable(目标名称 [STATIC|SHARED|MODULE] [WIN32] source1 source2 ...)
#
# 所有 .cpp 源文件和 .h 头文件都需要列在这里
# CMake 会分析文件依赖，自动确定编译顺序
add_executable(ChatUI
    src/main.cpp                 # 程序入口
    src/MainWindow.cpp           # 主窗口实现
    src/ChatWindow.cpp           # 聊天窗口实现
    src/SearchWindow.cpp         # 搜索窗口窗口实现
    src/AgentWindow.cpp          # 代理窗口窗口实现
    src/ContactWindow.cpp        # 联系人窗口实现
    src/ContactList.cpp          # 联系人列表实现
    src/ChatArea.cpp             # 聊天区域实现
    src/MessageList.cpp          # 消息列表实现
    src/MessageItem.cpp          # 消息项实现
    src/MessageStatusIndicator.cpp # 消息状态指示器实现（发送中旋转圆圈/失败感叹号）
    src/ChatInput.cpp            # 输入框实现
    src/StatusBar.cpp            # 状态栏实现
    src/LoginWindow.cpp          # 登录窗口容器（QStackedWidget 装三个页面）
    src/LoginPage.cpp            # 登录页实现
    src/RegisterPage.cpp         # 注册页实现
    src/ForgotPasswordPage.cpp   # 忘记密码页实现
    src/GlassCard.cpp            # 局部毛玻璃卡片（忘记密码页/注册页共用）
    src/MainBackend.cpp        # 主后端实现
    resources.qrc             # Qt 资源文件（图标）

    backend/DatabaseManager.cpp       # 数据库管理实现
    backend/ChatBackend.cpp          # 后端接口实现
    backend/TcpClient.cpp          # TCP客户端实现
    backend/LoginBackend.cpp       # 登录后端实现


    # 头文件（让 IDE 能够找到定义）
    include/MainWindow.h
    include/SearchWindow.h
    include/ChatWindow.h
    include/ContactList.h
    include/ChatArea.h
    include/MessageList.h   
    include/MessageItem.h
    include/MessageStatusIndicator.h
    include/ChatInput.h
    include/StatusBar.h
    include/LoginWindow.h
    include/LoginPage.h
    include/RegisterPage.h
    include/ForgotPasswordPage.h
    include/GlassCard.h
    include/MainBackend.h
    include/ContactWindow.h
    include/AgentWindow.h



    include_backend/ChatBackend.h
    include_backend/DatabaseManager.h
    include_backend/TcpClient.h
    include_backend/LoginBackend.h
    include_backend/FeatureStructs.h

    
  
)

# =============================================================================
# 链接 Qt 库
# =============================================================================
# target_link_libraries: 指定目标文件需要链接的库
# 语法: target_link_libraries(目标 [PRIVATE|PUBLIC|INTERFACE] lib1 lib2 ...)
#
# PRIVATE: 这些库仅在编译该目标时需要，不影响其他依赖该目标的库
# PUBLIC: 链接信息既用于编译该目标，也传递给依赖它的目标
# INTERFACE: 仅传递给依赖它的目标，不用于编译该目标
#
# Qt6::Widgets 等是 find_package(Qt6 ...) 导入的 Qt 组件
target_link_libraries(ChatUI PRIVATE
    Qt6::Widgets     # GUI 组件库
    Qt6::Core       # 核心库
    Qt6::Gui        # 图形基础库
    Qt6::Sql        # SQL数据库库
    Qt6::Svg        # SVG图形库
    Qt6::Network    # 网络库（TCP/UDP）
)
