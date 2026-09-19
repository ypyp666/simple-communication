#include <QApplication>
#include <QIcon>
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

    // 先显示登录窗口
    LoginWindow login(&mainBackend);
    if (login.exec() == QDialog::Accepted) //由于登录窗口还没做，所以这里直接返回1
    {
        // 登录成功，显示主窗口
    MainWindow window(nullptr, &mainBackend);
    window.show();
        return app.exec();
    }

    // 登录失败或取消，退出程序
    return 0;
}
