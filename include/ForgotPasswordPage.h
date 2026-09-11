#ifndef FORGOTPASSWORDPAGE_H
#define FORGOTPASSWORDPAGE_H

#include <QWidget>
#include <QLineEdit>
#include <QFrame>
#include <QLabel>
#include<QTimer>

class QPushButton;  // 前置声明：头文件里只用指针，不需要完整定义
class QPropertyAnimation;
class MainBackend;   // 前置声明：修改密码要走后端 TCP，头文件只存指针即可
/*
ForgotPasswordPage（忘记密码页）
独立的找回密码页面组件，放进 LoginWindow 的 QStackedWidget 里作为其中一页。
自己不负责切页，点"返回登录"时发 backToLoginRequested 信号，由容器去切。
*/

class ForgotPasswordPage : public QWidget
{
    Q_OBJECT
public:
    explicit ForgotPasswordPage(MainBackend* backend, QWidget *parent = nullptr);

    // 设置账号输入框内容（登录页点"忘记密码"时把当前输入的账号带过来）
    void setAccount(const QString& account);

signals:
    void backToLoginRequested();   // 点击"返回登录"
    // 提交修改密码请求：两次密码校验通过后发给主后端（MainBackend 接收后转 LoginBackend 走 TCP）
    void ModifyPwdAquird(const QString& account, const QString& newPassword);

private:
    void setupUI();
    // 密码强度检测：根据新密码内容计算等级，点亮对应杠数并更新颜色/文字
    void updatePasswordStrength(const QString& pwd);
    // 槽函数：刷新提交按钮可用性（密码+确认框都非空且无非法字符才可点）
    void updateSubmitButtonState();
    // 槽函数：刷新提交按钮动画（正在修改密码...）
    void updateSubmitButtonAnimation();
    // 槽函数：点击"确认修改"（占位实现，后续接后端找回密码逻辑）
    void onSubmitClicked();
    // 工具函数：播放错误反馈（红字提示 + 按钮红色错误态样式 + 左右抖动动画），
    // 供提交校验等多处复用；动画结束时由 finished → updateSubmitButtonState 复位样式
    void triggerErrorFeedback(const QString& hint);
    // 工具函数：切换密码框明文/密文（眼睛按钮复用），逻辑只维护这一份
    void togglePasswordVisibility(QLineEdit* edit, QPushButton* toggleBtn);
    // 工具函数：控制眼睛按钮的显隐（空密码时隐藏按钮），与上面是独立功能
    void togglePasswordBtnVisibility(QLineEdit* edit, QPushButton* toggleBtn);
    //槽函数：修改密码等待中信号
    void onModifyPwdWaiting();
    //槽函数：修改密码成功信号
    void onModifyPwdSuccess();
    //槽函数：修改密码失败信号
    void onModifyPwdFailed();
    //槽函数：修改密码连接超时信号
    void onModifyPwdTimeout();

    void setInputsEnabled(bool enabled);

    QLineEdit* fAccountEdit;       // 账号输入框（成员，便于 setAccount 填充）
    QLineEdit* fNewPwdEdit;        // 新密码输入框（成员，连接 textChanged 触发强度检测）
    QLineEdit* fConfirmEdit;       // 确认密码输入框（成员，便于提交时比对两次密码是否一致）
    QPushButton* fNewPwdToggleBtn;     // 密码框的可见性切换按钮（创建与切换逻辑你自己写）
    QPushButton* fConfirmToggleBtn;    // 确认密码框的可见性切换按钮（同上）
    QFrame* fCardFrame;            // 卡片底框（美化用：大框套小框的中间小框）
    QWidget* m_strengthWidget;     // 密码强度条整体容器（细杠+提示；默认隐藏，输入密码才显示）
    QFrame* m_strengthBars[4];     // 四条强度横杠（自左向右依次点亮）
    QLabel* m_strengthLabel;       // 强度等级文字（弱/中/强/极强）
    QLabel* m_symbolHint;          // 合法符号提示（含非法字符时变红提醒）
    bool m_hasIllegal = false;     // 当前密码是否含合法集之外的字符（供提交校验等后续逻辑使用）
    QPushButton* fBackBtn;         // 返回登录按钮（成员，便于后续外部控制显隐/改文案）
    QPushButton* fSubmitBtn;       // 确认修改按钮（成员，便于后续接入提交逻辑时控制状态/文案）
    QLabel* m_pwdMismatchHint;     // 提交按钮下方的错误提示行（常驻占一行，出错时红字）
    QPropertyAnimation* m_shakeAnim;  // 提交按钮左右抖动动画（密码不一致时红框抖动）
    QPoint m_btnRestPos;           // 按钮静止基准位置（抖动结束后回到这里，防连点漂移）
    //QPoint就是QT的简单数据结构就只是存坐标的
    bool m_btnRestValid = false;   // 基准位置是否已记录

    MainBackend* m_backend;        // 主后端（修改密码请求/结果信号都经它走 TCP）
    QString m_originalSubmitText;   // 原始输入的提交按钮文案（登录页点"忘记密码"时用）
    int m_dostcount = 0;
    // 提交按钮点击次数（用于判断是否重复点击）
    QTimer* m_modifyPwdAnimTimer;      // 修改密码等待定时器（用于显示"正在修改密码..."）

};

#endif // FORGOTPASSWORDPAGE_H
