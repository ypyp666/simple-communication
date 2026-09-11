#include "LoginPage.h"
#include <QMessageBox>
#include <QTimer>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QRegularExpressionValidator>
#include <QPropertyAnimation>

LoginPage::LoginPage(MainBackend* backend, QWidget *parent)
    : QWidget(parent)
{
    this->m_backend = backend;

    // 初始化登录动画相关
    m_loginAnimationTimer = new QTimer(this);
    m_dotsCount = 0;

    setupUI();
}

LoginPage::~LoginPage()
{
}

void LoginPage::setupUI()
{
    // 登录页自己的根布局：直接铺在本页面(this)上
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(20);
    mainLayout->setContentsMargins(40, 40, 40, 40);

    // ========== 第一层：头像区域 ==========
    QHBoxLayout* avatarLayout = new QHBoxLayout();
    avatarLayout->setSpacing(10);

    // 头像标签
    avatarLabel = new QLabel(this);
    avatarLabel->setFixedSize(80, 80);
    avatarLabel->setStyleSheet(R"(
        QLabel {
            border-radius: 40px;
            background-color: #4a90d9;
            border: 3px solid white;
        }
    )");
    avatarLabel->installEventFilter(this);// 安装事件过滤器，监听头像点击事件
    //当 avatarLabel 发生任何事件（鼠标进入、离开、点击等），都会先经过 LoginPage::eventFilter() 处理
    // 添加账号按钮（默认隐藏）
    addAccountBtn = new QPushButton("+", this);
    addAccountBtn->setFixedSize(30, 30);
    addAccountBtn->setStyleSheet(R"(
        QPushButton {
            border-radius: 15px;
            background-color: #4a90d9;
            color: white;
            font-size: 18px;
            border: none;
        }
        QPushButton:hover {
            background-color: #3a80c9;
        }
    )");
    addAccountBtn->hide();

    avatarLayout->addStretch();
    avatarLayout->addWidget(avatarLabel);
    avatarLayout->addWidget(addAccountBtn);
    avatarLayout->addStretch();

    mainLayout->addLayout(avatarLayout);

    // ========== 第二层：账号输入 ==========
    QHBoxLayout* accountLayout = new QHBoxLayout();
    accountLayout->setSpacing(0);

    accountEdit = new QLineEdit(this);
    // 账号输入校验：只允许数字，最长10位（QRegularExpressionValidator 在录入阶段就拦截非法字符，
    // 字母/符号根本进不了框；量词 {0,10} 同时限制最大长度）
    accountEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression("[0-9]{0,15}"), accountEdit));
    // 设置占位符（placeholder）：输入框为空时显示灰色的"请输入账号"，一旦输入文字就自动隐藏占位符并显示用户内容，全部删光后占位符自动回来
    // 这是 QLineEdit 的内建行为，由 Qt 内部根据 text() 是否为空自动切换，无需自己写代码
    accountEdit->setPlaceholderText("请输入账号");
    accountEdit->setFixedHeight(45);
    accountEdit->setStyleSheet(R"(
        QLineEdit {
            border-radius: 8px 0 0 8px;
            border: none;
            padding-left: 15px;
            font-size: 14px;
            background-color: white;
        }
        QLineEdit:focus {
            border-color: #4a90d9;
            outline: none;
        }
    )");//  border-radius: 8px 0 0 8px;左上圆角，右上圆角，右下圆角，左下圆角
    //outline 是控件获得焦点时，外围自动出现的高亮虚线 / 实线外框，不属于边框 border，不会占用布局空间，只是视觉提示当前选中了这个输入框、按钮。
    accountEdit->installEventFilter(this);

    // 下拉选择按钮（默认透明）
    accountDropdownBtn = new QPushButton(this);
    accountDropdownBtn->setFixedSize(45, 45);

    accountDropdownBtn->setFocusPolicy(Qt::ClickFocus);
    accountDropdownBtn->setStyleSheet(R"(
        QPushButton {
            border-radius: 0 8px 8px 0;
            border: none;
            background-color: white;
        }
        QPushButton:hover {
            background-color: #f5f5f5;
        }
    )");
   // 资源里放 arrow-down.svg
    accountDropdownBtn->setIcon(QIcon(":/res/icon/arrow-down.svg"));
    accountDropdownBtn->setIconSize(QSize(16, 16));
    accountDropdownBtn->installEventFilter(this);
    accountDropdownBtn->hide();

    accountLayout->addWidget(accountEdit);
    accountLayout->addWidget(accountDropdownBtn);

    mainLayout->addLayout(accountLayout);

    // ========== 第三层：密码输入 ==========
    QHBoxLayout* passwordLayout = new QHBoxLayout();
    passwordLayout->setSpacing(0);

    passwordEdit = new QLineEdit(this);
    // 密码输入校验：允许可见 ASCII 字符（[!-~] 覆盖字母、数字、常用符号），最长20位
    passwordEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression("[!-~]{0,20}"), passwordEdit));
    // 密码框占位符：同账号框，空时显示"请输入密码"，输入后自动被用户文本替代
    passwordEdit->setPlaceholderText("请输入密码");
    passwordEdit->setFixedHeight(45);
    passwordEdit->setEchoMode(QLineEdit::Password);
    passwordEdit->setStyleSheet(R"(
        QLineEdit {
            border-radius: 8px 0 0 8px;
            border: none;
            padding-left: 15px;
            font-size: 14px;
            background-color: white;
        }
        QLineEdit:focus {
            border-color: #4a90d9;
            outline: none;
        }
        QLineEdit:disabled {
            background-color: white;
            color: #999999;
        }
    )");
    passwordEdit->installEventFilter(this);
    passwordEdit->setEnabled(false);  // 初始禁止输入，输入账号后才解锁

    // 密码可见切换按钮（默认透明）
    passwordToggleBtn = new QPushButton(this);
    passwordToggleBtn->setFixedSize(45, 45);
    passwordToggleBtn->setFocusPolicy(Qt::NoFocus);  // 禁止 Tab 键选中
    passwordToggleBtn->setStyleSheet(R"(
        QPushButton {
            border-radius: 0 8px 8px 0;
            background-color: white;
            border: none;
            padding: 0;
            cursor: pointer;
        }
        QPushButton:hover {
            background-color: #f5f5f5;
        }
    )");
    passwordToggleBtn->setIcon(QIcon(":/res/icon/eyes_show.svg"));
    passwordToggleBtn->setIconSize(QSize(14, 14));
    passwordToggleBtn->installEventFilter(this);
    passwordToggleBtn->hide();

    passwordLayout->addWidget(passwordEdit);
    passwordLayout->addWidget(passwordToggleBtn);

    mainLayout->addLayout(passwordLayout);

    // ========== 第四层：登录按钮 ==========
    loginBtn = new QPushButton("登录", this);
    loginBtn->setFixedHeight(45);
    loginBtn->setStyleSheet(R"(
        QPushButton {
            border-radius: 8px;
            background-color:white;
            font-size: 16px;
            font-weight: bold;
            border: none;
        }
        QPushButton:hover {
            background-color: #f5f5f5;
        }
        QPushButton:pressed {
            background-color: #2a70b9;
        }
    )");

    mainLayout->addWidget(loginBtn);

    // 登录错误提示行：常驻占一行（16px），布局不跳动；默认无文字，登录失败/超时时显示红字
    m_loginErrorHint = new QLabel(this);
    m_loginErrorHint->setFixedHeight(16);
    m_loginErrorHint->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    m_loginErrorHint->setStyleSheet("QLabel { color: rgba(224, 91, 91, 0.75); font-size: 12px; font-weight: bold; }");
    mainLayout->addWidget(m_loginErrorHint);

    // 登录按钮左右抖动动画：keyValueAt 设四次左右偏移，基准位置在第一次出错时捕获
    m_shakeAnim = new QPropertyAnimation(loginBtn, "pos", this);
    m_shakeAnim->setDuration(320);
    // 抖动结束 → 恢复按钮正常样式（updateLoginButtonState 按当前账号/密码状态重设正确样式）
    connect(m_shakeAnim, &QPropertyAnimation::finished, this, &LoginPage::updateLoginButtonState);

    // ========== 第五层：忘记密码和注册 ==========
    QHBoxLayout* bottomLayout = new QHBoxLayout();
    bottomLayout->setSpacing(0);

    // ========== 忘记密码按钮 ==========
    // 普通状态：透明无边框，只显示灰色文字（视觉上像文字链接，不像按钮）
    forgotPwdBtn = new QPushButton("忘记密码", this);
    forgotPwdBtn->setEnabled(false);  // 暂时禁用：忘记密码功能未完成，禁止点击
    forgotPwdBtn->setStyleSheet(R"(
        QPushButton {
            color: #666666;                /* 文字颜色：灰色 */
            font-size: 14px;               /* 字号 */
            border: none;                  /* 去掉默认边框，不显示按钮的"盒子"轮廓 */
            background-color: transparent; /* 背景透明，按钮上只剩文字 */
            text-align: left;              /* 文字靠左对齐（配合下方布局的 addStretch 定位） */
        }
        QPushButton:hover {                /* :hover 是伪状态，仅鼠标悬浮期间生效 */
            color: #4a90d9;                /* 悬浮时文字由灰变蓝 */
        }
    )");

    // ========== 注册账号按钮 ==========
    // 与"忘记密码"结构相同，区别：文字默认蓝色（更醒目的引导入口）、靠右对齐
    registerBtn = new QPushButton("注册账号", this);
    registerBtn->setStyleSheet(R"(
        QPushButton {
            color: #4a90d9;            /* 文字颜色：蓝色 */
            font-size: 14px;
            border: none;              /* 去掉默认边框 */
            background: transparent;   /* 背景透明（background 是 background-color 的简写） */
            text-align: right;         /* 文字靠右对齐 */
        }
        QPushButton:hover {
            color: #3a80c9;            /* 悬浮时变深蓝 */
        }
    )");

    bottomLayout->addWidget(forgotPwdBtn);
    bottomLayout->addStretch();
    bottomLayout->addWidget(registerBtn);

    mainLayout->addLayout(bottomLayout);

    // 连接信号槽
    connect(loginBtn, &QPushButton::clicked, this, &LoginPage::onLoginClicked);
    connect(passwordToggleBtn, &QPushButton::clicked, this, &LoginPage::onPasswordToggle);
    connect(accountEdit, &QLineEdit::textChanged, this, &LoginPage::updateLoginButtonState);
    connect(passwordEdit, &QLineEdit::textChanged, this, &LoginPage::updateLoginButtonState);
    // 账号输入变化时同步记录当前账号（点"忘记密码"时带过去）
    connect(accountEdit, &QLineEdit::textChanged, this, [=](const QString& text){
        m_currentAccount = text;//void textChanged(const QString &text);
    });
    connect(this,&LoginPage::loginAquiard,m_backend,&MainBackend::login);
    // 专门监听后端登录结果信号，在这里判断是否登录成功
    connect(m_backend, &MainBackend::loginSuccess, this, [=](const QString& accountId){
        // 服务器验证成功，停止动画并通知容器（容器收到后关闭登录窗口）
        stopLoginAnimation();
        emit loginSuccess(accountId, accountId); // 转发给容器
    });
    connect(m_backend, &MainBackend::loginFailed, this, [=](){
        // 服务器验证失败：停动画 + 按钮红框抖动 + 红字提示（不再用弹窗），窗口保持打开
        stopLoginAnimation();
        showLoginError("账号或密码错误");
    });
    // 任一输入框有变化 → 清掉错误提示红字
    connect(accountEdit, &QLineEdit::textChanged, m_loginErrorHint, &QLabel::clear);
    connect(passwordEdit, &QLineEdit::textChanged, m_loginErrorHint, &QLabel::clear);
    // 监听登录等待信号
    connect(m_backend, &MainBackend::loginWaiting, this, &LoginPage::onLoginWaiting);
    // 监听登录超时信号
    connect(m_backend, &MainBackend::loginTimeout, this, &LoginPage::onLoginTimeout);
    // 登录动画定时器
    connect(m_loginAnimationTimer, &QTimer::timeout, this, &LoginPage::updateLoginButtonText);
    // 初始化登录按钮状态
    updateLoginButtonState();

    // 点击"注册账号"/"忘记密码"：本页面不做切换，只发信号，由容器切页面
    connect(registerBtn, &QPushButton::clicked, this, [=]() {

        emit registerRequested();
    });
    connect(forgotPwdBtn, &QPushButton::clicked, this, [=]() {
        emit forgotPasswordRequested(m_currentAccount);  // 携带当前输入的账号
    });
}

 bool LoginPage::eventFilter(QObject* obj, QEvent* event)//QObject类的原生虚函数，用于拦截事件并进行处理
{
    // 头像悬浮：10秒延迟消失
    if (obj == avatarLabel || obj == addAccountBtn) {
        if (event->type() == QEvent::Enter)//鼠标进入事件
         {
            addAccountBtn->show();
            // 重置10秒计时器
            //QTimer::singleShot(10000, addAccountBtn, &QPushButton::hide);
        } else if (event->type() == QEvent::Leave)//鼠标离开事件
         {
            // 重新启动5秒计时器
            QTimer::singleShot(5000, addAccountBtn, &QPushButton::hide);
        }
    }

    /*
     QPushButton* relatedBtn = (obj == accountEdit)
            ? qobject_cast<QPushButton*>(accountEdit->parent()->findChild<QPushButton*>())
            : passwordToggleBtn;
        这段逻辑保留，逻辑讲解：
        1. 如果obj是accountEdit，那么relatedBtn就是accountDropdownBtn，并且用
        qobject_cast将accountDropdownBtn转换为QPushButton*类型。（qobject_cast是Qt提供的类型转换函数，用于将QObject*安全转换为指定类型的指针）
        accountEdit->parent()拿到accountEdit的父对象，即LoginPage。
        findChild<QPushButton*>()在LoginPage中查找accountDropdownBtn，返回accountDropdownBtn的指针。
        因为accountDropdownBtn是按钮组件并且accountEdit的子对象，所以可以使用findChild()函数来查找它。
        2. 如果obj是passwordEdit，那么relatedBtn就是passwordToggleBtn。
        3. 这段逻辑的目的是根据obj的类型，选择对应的下拉按钮或密码可见切换按钮。
        */
    // 账号输入框区域：悬浮显示下拉按钮
    if (obj == accountEdit || obj == passwordEdit) {
        QPushButton* relatedBtn = (obj == accountEdit)
            ? accountDropdownBtn
            : passwordToggleBtn;

        if (relatedBtn) {
            if (event->type() == QEvent::Enter)//鼠标进入事件
            {
                relatedBtn->setStyleSheet(R"(
                    QPushButton {
                        border-radius: 0 8px 8px 0;
                        border: 1px solid #e0e0e0;
                        border-left: none;
                        background-color: white;
                    }
                    QPushButton:hover {
                        background-color: #f5f5f5;
                    }
                )");
                // 密码按钮：密码为空时不显示，密码非空才悬浮显示；账号下拉按钮则始终显示
                if(relatedBtn == passwordToggleBtn && !passwordEdit->text().isEmpty())
                {
                    relatedBtn->show();
                }
                else if(relatedBtn == accountDropdownBtn)
                {
                    relatedBtn->show();
                }
            } else if (event->type() == QEvent::Leave) {
                relatedBtn->setStyleSheet(R"(
                    QPushButton {
                        border-radius: 0 8px 8px 0;
                        border: 1px solid #e0e0e0;
                        border-left: none;
                        background-color: white;
                    }
                    QPushButton:hover {
                        background-color: #f5f5f5;
                    }
                )");
                // 离开输入框后延时隐藏按钮；若鼠标已移到按钮上则保持显示
                QTimer::singleShot(500, relatedBtn, [relatedBtn]() {
                    if (!relatedBtn->underMouse()) {
                        relatedBtn->hide();
                    }
                });
            }
        }
    }

    // 密码按钮本身悬浮（同样要求密码非空才显示，为空时不显示）
    if (obj == passwordToggleBtn && !passwordEdit->text().isEmpty()) {
        if (event->type() == QEvent::Enter) {
            passwordToggleBtn->setStyleSheet(R"(
                QPushButton {
                    border-radius: 0 8px 8px 0;
                    border: 1px solid #e0e0e0;
                    border-left: none;
                    background-color: white;
                }
                QPushButton:hover {
                    background-color: #f5f5f5;
                }
            )");
            passwordToggleBtn->show();
        } else if (event->type() == QEvent::Leave) {
            passwordToggleBtn->setStyleSheet(R"(
                QPushButton {
                    border-radius: 0 8px 8px 0;
                    border: 1px solid #e0e0e0;
                    border-left: none;
                    background-color: white;
                }
                QPushButton:hover {
                    background-color: #f5f5f5;
                }
            )");
            QTimer::singleShot(500, passwordToggleBtn, &QPushButton::hide);
        }
    }

    if(obj==accountDropdownBtn)
    {
     if(event->type()==QEvent::Enter)//鼠标进入事件
     {
        accountDropdownBtn->show();
     }
     else if(event->type()==QEvent::Leave)//鼠标离开事件
     {
       QTimer::singleShot(500, accountDropdownBtn, &QPushButton::hide);
     }
    }

    // 回车触发登录：在账号框/密码框里按 Enter/Return 直接登录
    if ((obj == accountEdit || obj == passwordEdit) && event->type() == QEvent::KeyPress) {
        if(accountEdit->text().isEmpty() || passwordEdit->text().isEmpty())
        {
            return false;
        }
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            onLoginClicked();
            return true;  // 事件已消费，不再往下传
        }
    }

    return QWidget::eventFilter(obj, event);
}

void LoginPage::onLoginClicked()
{
    QString account = accountEdit->text();
    QString password = passwordEdit->text();

    if (account.isEmpty() || password.isEmpty()) {
        QMessageBox::warning(this, "提示", "请输入账号和密码");
        return;
    }
    emit loginAquiard(account, password);
    // 模拟登录成功（实际应该调用后端验证）
}

void LoginPage::onPasswordToggle()
{
    if (passwordEdit->echoMode() == QLineEdit::Password)//判断当前输入框回显模式：是否为密码隐藏模式
    {
        passwordEdit->setEchoMode(QLineEdit::Normal);//设置为正常模式
        passwordToggleBtn->setIcon(QIcon(":/res/icon/eyes_clicked.svg"));
    } else
    {
        passwordEdit->setEchoMode(QLineEdit::Password);//设置为密码模式
        passwordToggleBtn->setIcon(QIcon(":/res/icon/eyes_show.svg"));
    }
}

void LoginPage::onAvatarHoverEnter()
{
    addAccountBtn->show();
}

void LoginPage::onAvatarHoverLeave()
{
    // 延迟10秒后隐藏
    QTimer::singleShot(10000, addAccountBtn, &QPushButton::hide);
}

void LoginPage::updateLoginButtonState()
{
    // 密码框联动：账号框有输入才允许输密码
    passwordEdit->setEnabled(!accountEdit->text().isEmpty());

    if(accountEdit->text().size()>=5)
    {
        forgotPwdBtn->setEnabled(true);
    }

    bool canLogin = !accountEdit->text().isEmpty() && !passwordEdit->text().isEmpty();

    if (canLogin) {
        // 可登录状态：蓝色按钮，可点击，有悬浮效果
        loginBtn->setEnabled(true);
        loginBtn->setStyleSheet(R"(
            QPushButton {
                border-radius: 8px;
                background-color: #4a90d9;
                color: white;
                font-size: 16px;
                font-weight: bold;
                border: none;
            }
            QPushButton:hover {
                background-color: #3a80c9;
            }
            QPushButton:pressed {
                background-color: #2a70b9;
            }
        )");
    } else {
        // 不可登录状态：灰色按钮，禁用，无悬浮效果
        loginBtn->setEnabled(false);
        loginBtn->setStyleSheet(R"(
            QPushButton {
                border-radius: 8px;
                background-color: #cccccc;
                color: #999999;
                font-size: 16px;
                font-weight: bold;
                border: none;
            }
        )");
    }
}

// 登录等待期间统一开关整页交互
// 禁用阶段：输入框 + 全部按钮锁死，防止等待中重复提交或偷改账号密码
// 恢复阶段：输入框和"常驻可用"的按钮直接开回来；loginBtn / passwordEdit / forgotPwdBtn
//          不在这里恢复，交给紧随其后的 updateLoginButtonState() 按当前输入重新判定，
//          否则会把本该禁用的控件误打开（如账号没填满时不允许点忘记密码）
void LoginPage::setInputsEnabled(bool enabled)
{
    // 输入框
    accountEdit->setEnabled(enabled);
    passwordEdit->setEnabled(enabled);

    // 按钮
    passwordToggleBtn->setEnabled(enabled);
    accountDropdownBtn->setEnabled(enabled);
    registerBtn->setEnabled(enabled);
    addAccountBtn->setEnabled(enabled);

    // 状态由输入内容决定的控件：只在禁用阶段直接锁上
    if (!enabled) {
        loginBtn->setEnabled(false);
        forgotPwdBtn->setEnabled(false);
    }
}

void LoginPage::onLoginWaiting()
{
    // 保存原始按钮文本
    m_originalLoginText = loginBtn->text();

    // 锁住整页交互（输入框 + 所有按钮），防止等待期间重复点击或改账号密码
    setInputsEnabled(false);

    // 初始化点的数量
    m_dotsCount = 0;

    // 更新按钮文本为"正在登录中"
    loginBtn->setText("正在登录中");

 
   // 设置登录中的样式（灰色背景）
    loginBtn->setStyleSheet(R"(
        QPushButton {
            border-radius: 8px;
            background-color: #999999;
            color: white;
            font-size: 16px;
            font-weight: bold;
            border: none;
        }
    )");
    // 启动定时器，每500ms更新一次点的数量
    m_loginAnimationTimer->start(500);
}

void LoginPage::updateLoginButtonText()
{
    // 更新点的数量（0->1->2->3->0循环）
    m_dotsCount = (m_dotsCount + 1) % 4;

    // 构建新的按钮文本
    QString text = "正在登录中";
    for (int i = 0; i < m_dotsCount; i++) {
        text += ".";
    }

    // 更新按钮文本
    loginBtn->setText(text);
}

void LoginPage::stopLoginAnimation()
{
    // 停止定时器
    m_loginAnimationTimer->stop();

    // 恢复原始按钮文本
    loginBtn->setText(m_originalLoginText);

    // 先解锁整页交互，再按当前输入内容重算各控件状态
    // （顺序不能反：updateLoginButtonState 会覆盖登录按钮/密码框的状态）
    setInputsEnabled(true);
    updateLoginButtonState();
}

void LoginPage::onLoginTimeout()
{
    // 停止登录动画
    stopLoginAnimation();

    // 按钮红框抖动 + 红字提示（不再用弹窗）
    showLoginError("连接超时，请检查网络设置");
}

// 登录错误提示（登录失败/连接超时共用）：按钮临时套半透明红框并左右抖动，
// 下方常驻提示行显示红字；动画结束由 finished → updateLoginButtonState 恢复正常样式
void LoginPage::showLoginError(const QString& text)
{
    m_loginErrorHint->setText(text);

    // 临时错误态样式：按钮整体染成红色系（淡红内里 + 红字 + 红框），与忘记密码页同款
    loginBtn->setStyleSheet(R"(
        QPushButton {
            border-radius: 8px;
            background-color: rgba(224, 91, 91, 0.14);   /* 内里染淡红 */
            color: rgba(224, 91, 91, 0.85);              /* "登录"文字跟着变红 */
            font-size: 16px;
            font-weight: bold;
            border: 2px solid rgba(224, 91, 91, 0.45);   /* 红框同步调柔 */
        }
        QPushButton:hover  { background-color: rgba(224, 91, 91, 0.20); }
        QPushButton:pressed { background-color: rgba(224, 91, 91, 0.26); }
    )");

    // 左右抖动：以基准位为中心 ±8/±5px 递减摆动，结束时回到基准位
    m_shakeAnim->stop();          // 上一次还在抖就先停，防止连点叠加
    if (!m_btnRestValid) {        // 只在按钮处于布局位时捕获基准（第一次出错时）
        m_btnRestPos = loginBtn->pos();
        m_btnRestValid = true;
    }
    const QPoint base = m_btnRestPos;
    m_shakeAnim->setStartValue(base);
    m_shakeAnim->setKeyValueAt(0.15, base + QPoint(-8, 0));
    m_shakeAnim->setKeyValueAt(0.35, base + QPoint( 8, 0));
    m_shakeAnim->setKeyValueAt(0.55, base + QPoint(-5, 0));
    m_shakeAnim->setKeyValueAt(0.75, base + QPoint( 5, 0));
    m_shakeAnim->setEndValue(base);
    m_shakeAnim->start();
}
