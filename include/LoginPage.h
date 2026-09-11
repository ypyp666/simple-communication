#ifndef LOGINPAGE_H
#define LOGINPAGE_H

#include <QWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTimer>
#include "MainBackend.h"
/*
LoginPage（登录页）
把登录页的 UI 和交互逻辑从 LoginWindow 里独立出来，做成一个单独的页面组件。
LoginWindow 只负责装页面的容器（QStackedWidget），这样"登录页 / 注册页 / 忘记密码页"各管各的。
对外通过信号通知容器该干嘛：
  registerRequested        → 用户点了"注册账号"，请容器切到注册页
  forgotPasswordRequested  → 用户点了"忘记密码"，请容器切到忘记密码页
  loginSuccess / loginAquiard → 转发给外部（登录流程相关）
*/

class QPropertyAnimation;  // 前置声明：登录按钮抖动动画

class LoginPage : public QWidget
{
    Q_OBJECT
public:
    explicit LoginPage(MainBackend* backend, QWidget *parent = nullptr);
    ~LoginPage();

signals:
    void loginSuccess(const QString& accountId, const QString& accountName);
    void loginAquiard(const QString& accountId, const QString& passward);
    void registerRequested();        // 点击"注册账号"
    void forgotPasswordRequested(const QString& account);  // 点击"忘记密码"，携带当前输入的账号

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onLoginClicked();
    void onPasswordToggle();
    void onAvatarHoverEnter();
    void onAvatarHoverLeave();
    void updateLoginButtonState();
    void onLoginWaiting();
    void updateLoginButtonText();
    void stopLoginAnimation();
    void onLoginTimeout();

private:
    void setupUI();
    // 登录请求等待期间统一开关整页交互（所有输入框 + 按钮），防止等待中重复提交或改内容
    void setInputsEnabled(bool enabled);
    // 登录错误提示（登录失败/连接超时共用）：按钮红框抖动 + 下方红字，替代弹窗
    void showLoginError(const QString& text);

    // 头像相关
    QLabel* avatarLabel;
    QPushButton* addAccountBtn;

    // 账号输入相关
    QLineEdit* accountEdit;
    QPushButton*  accountDropdownBtn; // 新增，和passwordToggleBtn对齐

    // 密码输入相关
    QLineEdit* passwordEdit;
    QPushButton* passwordToggleBtn;

    // 按钮相关
    QPushButton* loginBtn;
    QPushButton* forgotPwdBtn;
    QPushButton* registerBtn;

    // 后端相关
    MainBackend* m_backend;

    // 登录动画相关
    QTimer* m_loginAnimationTimer;
    int m_dotsCount;
    QString m_originalLoginText;

    // 当前输入的账号（点"忘记密码"时带到忘记密码页）
    QString m_currentAccount;

    // 登录错误提示相关（同忘记密码页的红框抖动方案）
    QLabel* m_loginErrorHint;      // 登录按钮下方的错误提示行（常驻占一行，出错时红字）
    QPropertyAnimation* m_shakeAnim;  // 登录按钮左右抖动动画
    QPoint m_btnRestPos;           // 按钮静止基准位置（抖动结束回到这里，防连点漂移）
    bool m_btnRestValid = false;   // 基准位置是否已记录
};

#endif // LOGINPAGE_H
