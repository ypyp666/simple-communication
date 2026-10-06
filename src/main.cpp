#include <QApplication>
#include <QIcon>
#include <QEventLoop>
#include "MainWindow.h"
#include "LoginWindow.h"


int main(int argc, char *argv[])
{
    QApplication app(argc, argv);//Qt应用程序的核心控制类，控制事件循环
    app.setWindowIcon(QIcon(":/res/icon/application.png"));//设置窗口图标
    app.setApplicationName("CCEarth");//设置应用名称
    app.setOrganizationName("CCEarth");//设置组织名称
    app.setApplicationVersion("1.0");//设置应用版本
    MainBackend mainBackend; // 主后端实例

    // 退出时机完全由本函数说了算：关掉最后一个窗口不自动退出。
    // 因为"主窗口关掉"要分两类——用户自己关（退出程序）和令牌失效被踢（回登录页再来一轮）
    app.setQuitOnLastWindowClosed(false);

    // 登录窗 ↔ 主窗口 的循环：正常情况下只走一轮；
    // 只有"重连时令牌失效被踢"（MainBackend::sessionKicked）才会从主窗口回到登录窗
    for (;;) {
        // 先显示登录窗口
        LoginWindow login(&mainBackend);
        if (login.exec() != QDialog::Accepted) {
            return 0;   // 登录窗口被关掉（取消 / 关窗）= 用户要退出程序
        }

        // 登录成功，显示主窗口。每轮都新建：被踢回来重新登录时，
        // 上一个账号的会话列表 / 消息气泡不会串进新会话
        MainWindow* window = new MainWindow(nullptr, &mainBackend);
        window->show();

        // 等两个事件之一：用户关掉主窗口（退出）或 会话被踢回登录页
        QEventLoop loop;
        QObject::connect(window, &MainWindow::closed, &loop, &QEventLoop::quit);
        QObject::connect(&mainBackend, &MainBackend::sessionKicked, &loop, &QEventLoop::quit);
        loop.exec();

        const bool kicked = mainBackend.m_kickedToLogin;
        delete window;   // 旧窗口整个销毁：子控件和它接过的信号一并清掉
        if (!kicked) {
            return 0;    // 用户自己关的窗 = 退出程序
        }
    }
}