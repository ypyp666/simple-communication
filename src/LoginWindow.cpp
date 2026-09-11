#include "LoginWindow.h"
#include <QVBoxLayout>

LoginWindow::LoginWindow(MainBackend* backend, QWidget *parent)
    : QDialog(parent)
{
    // ========== 窗口级设置 ==========
    setWindowTitle("登录");
    setFixedSize(400, 500);
    setModal(true);// 设置为模态对话框，用户只能在登录窗口操作，必须登录后才能进入主界面
    // 固定尺寸窗口默认只显示关闭按钮（×），显式打开最小化提示后，
    // Windows 原生标题栏就会在 × 左边显示最小化按钮（−），最小化到任务栏不受固定尺寸影响
    setWindowFlag(Qt::WindowMinimizeButtonHint, true);

    // 设置窗口圆角和阴影（容器背景，三个页面背景透明，透出这个渐变）
    setStyleSheet(R"(
        LoginWindow {
            border-radius: 12px;
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #f0f5ff, stop:1 #e0e8f5);
        }
    )");

    // ========== 页面容器：QStackedWidget ==========
    // QStackedWidget 是"一摞页面卡片"，同一时刻只显示其中一页，
    // 用 setCurrentIndex(页码) 切换，实现在同一个窗口内"登录页/注册页/忘记密码页"互切，
    // 而不是像登录→主界面那样换一个窗口
    m_stackedWidget = new QStackedWidget(this);
    m_loginPage = new LoginPage(backend, this);      // 登录页
    m_registerPage = new RegisterPage(this);         // 注册页
    m_forgotPage = new ForgotPasswordPage(backend, this);     // 忘记密码页（传 backend，修改密码要走 TCP）

    // 顺序即页码：登录页=0，注册页=1，忘记密码页=2
    m_stackedWidget->addWidget(m_loginPage);
    m_stackedWidget->addWidget(m_registerPage);
    m_stackedWidget->addWidget(m_forgotPage);

    // 窗口根布局：堆叠容器铺满整个窗口
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->addWidget(m_stackedWidget);

    // ========== 页面切换信号 ==========
    // 登录页点"注册账号" → 切到注册页(1)；点"忘记密码" → 切到忘记密码页(2)
    connect(m_loginPage, &LoginPage::registerRequested, this, [=]() {
        m_stackedWidget->setCurrentIndex(1);
    });
    connect(m_loginPage, &LoginPage::forgotPasswordRequested, this, [=](const QString& account) {
        m_forgotPage->setAccount(account);          // 把登录页输入的账号带过去
        m_stackedWidget->setCurrentIndex(2);
    });
    // 注册页/忘记密码页点"返回登录" → 切回登录页(0)
    connect(m_registerPage, &RegisterPage::backToLoginRequested, this, [=]() {
        m_stackedWidget->setCurrentIndex(0);
    });
    connect(m_forgotPage, &ForgotPasswordPage::backToLoginRequested, this, [=]() {
        m_stackedWidget->setCurrentIndex(0);
    });

    // ========== 转发登录页信号，保持对外接口不变 ==========
    // 登录页验证成功后通知容器：转发信号 + 关闭模态窗口放行主界面
    connect(m_loginPage, &LoginPage::loginSuccess, this, [=](const QString& accountId, const QString& accountName){
        emit loginSuccess(accountId, accountName); // 对外转发
        accept();                                  // 结束模态循环
    });
    connect(m_loginPage, &LoginPage::loginAquiard, this, &LoginWindow::loginAquiard);
}

LoginWindow::~LoginWindow()
{
}
