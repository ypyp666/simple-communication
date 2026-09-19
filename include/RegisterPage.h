#ifndef REGISTERPAGE_H
#define REGISTERPAGE_H

#include <QWidget>
#include <QString>

class QLineEdit;
class QLabel;
class QPushButton;
class QTimer;
class QFrame;
class QPropertyAnimation;   // 前置声明：注册失败提示的抖动动画（成员指针即可）
class MainBackend;   // 前置声明：注册要走后端 TCP，头文件只存指针即可
class MessageStatusIndicator;   // 前置声明：账号行的"取号"状态指示器（成员指针即可，不用完整定义）
/*
RegisterPage（注册页）
独立的注册页面组件，放进 LoginWindow 的 QStackedWidget 里作为其中一页。
自己不负责切页，点"返回登录"时发 backToLoginRequested 信号，由容器去切。
*/
/*
注意：提交阶段 UI（校验/错误反馈/结果提示）与取号阶段指示器逻辑均已实现，
参照物是 ForgotPasswordPage：两者的页面结构、等待动画、错误反馈几乎同构。
服务端注册/取号协议未实现前，取号成功返回的是占位流程，提交结果依赖服务器响应。
*/
class RegisterPage : public QWidget
{
    Q_OBJECT
public:
    explicit RegisterPage(MainBackend* backend, QWidget *parent = nullptr);

    // 真正"进入注册页"时调用（由 LoginWindow 切页时显式调，见 LoginWindow.cpp）：
    // 把上次留下的输入复位，并通知后端建 TCP 连接取号。
    // 注意不能挂在 showEvent 里：窗口从最小化还原/重新获得焦点时 Qt 也会补发 showEvent，
    // 那一刻用户并没有离开本页——清空会把已经输入的内容抹掉，还会白发一次取号请求
    void enterPage();

signals:
    void backToLoginRequested();   // 点击"返回登录"
    // 提交注册请求：页内校验通过后发给主后端（MainBackend 接收后转 LoginBackend 走 TCP）
    void registerAquiard(const QString& account, const QString& password);
    // 进入本页时通知后端把 TCP 连接准备起来（由 enterPage 发出，见其声明处注释）。
    // 目前只做到"建立连接"这一步：账号由服务端下发，真正的取号逻辑在服务端；
    // 注册请求等用户点"注册"后再发
    void needAccountFromServer();

private:
    void setupUI();

    // 槽函数：密码强度检测（从忘记密码页整份搬来，保持两页分级与提示一致）
    void updatePasswordStrength(const QString& pwd);

    // 工具函数：切换密码框明文/密文（两个眼睛按钮复用同一份逻辑，照搬忘记密码页）
    void togglePasswordVisibility(QLineEdit* edit, QPushButton* toggleBtn);
    // 工具函数：按输入内容控制眼睛按钮显隐（空密码时隐藏）
    void togglePasswordBtnVisibility(QLineEdit* edit, QPushButton* toggleBtn);

    // 工具函数：当前是否满足"可提交"条件（两框都非空 + 无非法字符 + 密码满 kMinPasswordLength 位）。
    // 这个条件有三处要用（密码框输入变化 / 等待结束恢复 / 抖动动画结束恢复），
    // 收敛成一份，改规则时只改这里，避免三处各写一遍后走偏
    bool isSubmitReady() const;
    // 工具函数：按 isSubmitReady() 的真实结果同步提交按钮（setEnabled 与样式成对）；
    // 三处用到该条件的收尾统一调它，别只切样式——只切样式会让禁用按钮画成亮蓝可点态
    void refreshSubmitButtonState();

    // 注册等待期间统一开关本页交互（等待中禁用输入框/按钮，结束后恢复；
    // regAccountEdit 不参与——它是只读的"服务端下发"展示框，与忘记密码页的账号框同一设定）
    void setInputsEnabled(bool enabled);
    // 槽函数：点击"注册"（页内校验：账号已取到/无非法字符/两次密码一致 → emit registerAquiard）
    void onSubmitClicked();
    // 工具函数：错误反馈（提示行红字 + 按钮红色错误态 + 左右抖动，与忘记密码页同一套）
    void triggerErrorFeedback(const QString& hint);

    // ===== 注册流程各阶段槽函数 =====
    void onRegisterWaiting();        // 连接/等待中：锁页面 + "正在注册..."动画
    void onRegisterSuccess();        // 注册成功：绿色提示 + 清空输入 + 解锁
    void onRegisterFailed();         // 仅"服务器明确拒绝"（如账号已存在）
    void onRegisterNetworkError();   // 网络层连不上（服务器没跑/断网），与服务器拒绝分开提示
    void onRegisterTimeout();        // 连接超时
    // ===== "取号"（进页即拉起 TCP 的后端连接阶段）各阶段槽函数（骨架，逻辑待补）=====
    // 与上面的提交阶段分开：这一组只驱动账号行指示器（转圈 / 打勾 / 红感叹号可重试）
    void onConnectForRegisterWaiting();   // 连接中
    void onConnectForRegisterSuccess(const QString& account);   // 连接建立成功，服务器下发的账号 ID 从参数进来
    void onConnectForRegisterFailed();    // 网络层连不上（与"注册被拒"分开提示）
    void onConnectForRegisterTimeout();   // 连接超时
    // 槽函数：刷新提交按钮动画（"正在注册..."后面的点），骨架待补
    void updateSubmitButtonAnimation();
    void updateSubmitButtonState(bool enabled);

    // ===== 成员变量 =====
    MainBackend* m_backend = nullptr;      // 主后端（注册请求/结果信号都经它走 TCP）
    QLabel* avatarLabel = nullptr;         // 顶部头像占位（与登录页同一位置，当前只占位无逻辑）
    QLineEdit* regAccountEdit = nullptr;   // 账号输入框
    // 账号行右侧指示器：取号（连接/下发账号）期间转圈、失败变红感叹号（可点重试）。
    // 提成成员变量才能在取号各阶段槽函数里切状态（局部变量在 setupUI 之外够不着）
    MessageStatusIndicator* m_accountStatusIndicator = nullptr;
    QLineEdit* regPasswordEdit = nullptr;  // 密码输入框
    QLineEdit* regConfirmEdit = nullptr;   // 确认密码输入框
    QPushButton* regSubmitBtn = nullptr;   // 注册按钮
    QPushButton* regPwdToggleBtn = nullptr;      // 密码框"眼睛"按钮（切换明文/密文）
    QPushButton* regConfirmToggleBtn = nullptr;  // 确认密码框"眼睛"按钮
    QPushButton* backToLoginBtn = nullptr; // 返回登录按钮
    // 提示行：错误/成功文案都显示在这一行（骨架：UI 尚未创建，接逻辑时再 new 出来，
    // 先初始化为 nullptr 而不是留空——成员裸指针不初始化就是野指针，connect 时会段错误）
    QLabel* m_registerHint = nullptr;
    QTimer* m_registerAnimTimer = nullptr; // 注册等待动画定时器（构造函数里已创建，尚未启动）
    QString m_originalSubmitText;          // 进入等待前的按钮原文案（等待结束后恢复用）
    int m_dostCount = 0;                   // 动画点数（"正在注册..."的省略号个数）

    // ===== 密码强度条（整份照搬忘记密码页，含：四杠 + 等级文字 + 两行提示文案）=====
    QWidget* m_strengthWidget = nullptr;                     // 整块强度区（空密码时整体隐藏）
    QFrame* m_strengthBars[4] = {nullptr, nullptr, nullptr, nullptr};  // 四根强度杠
    QLabel* m_strengthLabel = nullptr;                       // 等级文字（弱/中/强/极强）
    QLabel* m_symbolHint = nullptr;                          // 两行提示文案（含非法字符时变红）
    bool m_hasIllegal = false;                               // 是否出现合法集之外的字符（供提交校验用）
    QLabel* m_registerMismatchHint = nullptr;             // 注册失败提示（两次密码不一致）
    QPropertyAnimation* m_registerMismatchAnim = nullptr; // 注册失败提示动画（按钮左右抖动）
    QPoint m_btnRestPos;                                   // 抖动基准位（按钮的布局静止坐标）
    bool m_btnRestValid = false;                           // 基准位是否已捕获（只在第一次出错时记一次）
};

#endif // REGISTERPAGE_H
