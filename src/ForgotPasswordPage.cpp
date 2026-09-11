#include "ForgotPasswordPage.h"
#include "MainBackend.h"
#include <QApplication>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QMessageBox>
#include <QPropertyAnimation>//动画头文件
#include <qboxlayout.h>
#include <QSizePolicy>
#include <qlineedit.h>
#include <qnamespace.h>
#include "GlassCard.h"   // 卡片局部毛玻璃（已提取为独立文件，忘记密码页/注册页共用）

ForgotPasswordPage::ForgotPasswordPage(MainBackend* backend, QWidget *parent)
    : QWidget(parent), m_backend(backend)
{
    // 修改密码等待动画定时器必须在这里 new 出来：
    // 此前只声明了指针没创建，是野指针，setupUI 里 connect 它时
    // Qt 一解引用读 staticMetaObject 就段错误（调用堆栈停在 qobject.h 的 connectImpl）
    m_modifyPwdAnimTimer = new QTimer(this);
    m_modifyPwdAnimTimer->setInterval(500);  // 动画每 500ms 跳一格

    setupUI();
}

void ForgotPasswordPage::setupUI()
{
    // ========== 忘记密码页自己的根布局 ==========
    // 间距全部用显式 addSpacing 控制（不用布局默认 spacing），
    // 多余空间全部交给首尾两个弹性垫片（stretch 因子 2:3，内容整体居中、重心略偏上）——
    // 这样窗口富余的高度不会渗进卡片把输入行拉开（此前"留白太多"的根因）
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setSpacing(0);
    layout->setContentsMargins(20, 28, 20, 28);  // 左右边距再收窄到20，卡片更宽→三个输入框同步拉长
    layout->addStretch(2);

    // 页标题
    QLabel* titleLabel = new QLabel("找回密码", this);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setStyleSheet("QLabel { color: #333333; font-size: 20px; font-weight: bold; }");
    layout->addWidget(titleLabel);
    layout->addSpacing(14);

    // 卡片本体换成真模糊版：GlassCard 自绘（截屏+高斯模糊垫底，见文件顶部类定义）
    fCardFrame = new GlassCard(this);
    // 不再固定300高度：窗口固定500高，装不下"卡片300+按钮+间距"会压缩所有弹性间隙，
    // 导致卡片和下方按钮贴死；改成让卡片贴合内容自适应高度
    // 原生 StyledPanel/Sunken 与 QSS border 会互相冲突（note.txt 记录的坑），纯 QSS 即可
    // 毛玻璃质感卡片：半透明白色背景透出底下的渐变，边缘一圈半透明白色描边模拟玻璃反光
    // （真正的"背景模糊"需要 Windows DWM/Acrylic 平台 API，QSS 不支持 backdrop-filter，
    //   这里用"半透明 + 高光描边"的伪毛玻璃近似，效果已足够接近）
    /*    fCardFrame = new QFrame(this);
    fCardFrame->setObjectName("fCardFrame");
    // 选择器必须写 #fCardFrame 而不是 QFrame：QLabel 也是 QFrame 的子类，
    // 写 QFrame 会连带命中卡片里所有标签，把文字也画上白底边框（上一版"文字带边框"的根因）
    // 所以这里用 #fCardFrame 选择器，只命中卡片本身，不包含内部所有子控件
    fCardFrame->setStyleSheet(R"(#fCardFrame{
        color:#808080;
        background-color: rgba(255, 255, 255, 0.55);
        border-radius: 12px;
        border: 1px solid rgba(255, 255, 255, 0.85);
        outline: none;
    })");*/
    // —— 注：上面这段是旧"伪玻璃 QSS 方案"的记录，代码已由 GlassCard::paintEvent 取代：
    //    圆角/白雾/描边现在都在 GlassCard 里自绘，旧 QSS 选择器不再需要。
    //    但"选择器必须写 #对象名 而不是 QFrame"这个坑的结论依然成立（QLabel 是 QFrame
    //    的子类，写 QFrame 会连带命中卡片里的标签，把文字画上白底边框）。
    //    下面 pwdBox 用的 #pwdBox 选择器就是同一套避坑约定。
    // 卡片垂直方向固定尺寸：QFrame 默认可伸缩，否则窗口富余高度会渗进卡片把它拉高，
    // 卡片一拉高，内部三行输入框就被行距撑开，松松散散（截图"丑"的根因）
    fCardFrame->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addWidget(fCardFrame);
    layout->addSpacing(18);

    QVBoxLayout* FrameLayout= new QVBoxLayout(fCardFrame);
    FrameLayout->setContentsMargins(18, 22, 18, 22);//卡片内边距：左右收窄→输入框更宽
    FrameLayout->setSpacing(14);//卡片内输入框行距

    // ===== 本页复用的统一样式（标签/输入框三行各一份，抽出来一处改全局生效） =====
    // 标签：常规字重小字号，颜色柔和，不做视觉主角
    const QString labelStyle = "QLabel { color: #666666; font-size: 14px; }";
    // 注：原 editStyle（输入框自带边框+聚焦描边）已删除——密码/确认两行的输入框
    // 都嵌在容器（pwdBox/confirmBox）里，边框由容器统一画（避免"框中框"拼缝），
    // 聚焦高亮由容器在 focusChanged 里切蓝，QSS 自带 :focus 边框不再需要

    // 三行表单统一用 QGridLayout：列0=标签（统一80px右对齐），列1=输入框（拉伸填满）。
    // 三个输入框物理上在同一列，等长是布局机制保证的，彻底摆脱此前"各自容器宽度不一"的问题
    QGridLayout* grid = new QGridLayout();
    grid->setContentsMargins(0, 0, 0, 0);//网格整体内边距
    grid->setHorizontalSpacing(18);   // 标签与输入框之间的间距
    grid->setVerticalSpacing(14);     // 三行之间的行距
    grid->setColumnStretch(0, 0);     // 标签列：固定宽度不拉伸
    //权重0优先使用控件自身 sizeHint 建议宽度，不瓜分多余空间**，适合标签文本，标签多大，列就多宽。
    grid->setColumnStretch(1, 1);     // 输入框列：拉伸填满剩余宽度
    //窗口有多余空余宽度，全部交给这一列。输入框就会自动撑满剩下全部区域。
    grid->setColumnStretch(2, 0);     // 按钮列：固定宽度不拉伸

    // ===== 账号行 =====
    // 账号只读展示（从登录页带过来），整体调灰与可编辑的密码行区分开
    QLabel* fAccountLabel = new QLabel("账号:", this);
    fAccountLabel->setStyleSheet("QLabel { color: #999999; font-size: 14px; }");
    fAccountLabel->setFixedWidth(80);                        // 标签统一等宽
    fAccountLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);  // 文字在固定宽度内右对齐
    grid->addWidget(fAccountLabel, 0, 0);

    fAccountEdit = new QLineEdit(this);
    fAccountEdit->setPlaceholderText("请输入账号");
    fAccountEdit->setFixedHeight(42);
    // 禁用态样式：底色更灰、文字变浅灰、描边更淡，视觉上明确"不可编辑"
    fAccountEdit->setStyleSheet(R"(
        QLineEdit {
            border-radius: 8px;
            border: 1px solid rgba(0, 0, 0, 0.05);
            padding-left: 12px;
            font-size: 14px;
            background-color: rgba(255, 255, 255, 0.45);
            color: #999999;
        }
    )");
    fAccountEdit->setEnabled(false);
    grid->addWidget(fAccountEdit, 0, 1);

    // ===== 密码行 =====
    QLabel* fNewPwdLabel = new QLabel("密码:", this);
    fNewPwdLabel->setStyleSheet(labelStyle);
    fNewPwdLabel->setFixedWidth(80);                          // 标签统一等宽
    fNewPwdLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);  // 文字在固定宽度内右对齐
    grid->addWidget(fNewPwdLabel, 1, 0);

    fNewPwdEdit = new QLineEdit(this);
    fNewPwdEdit->setPlaceholderText("请输入新密码");
    fNewPwdEdit->setFixedHeight(40);
    fNewPwdEdit->setMaxLength(15);               // 密码最多15位（与强度分级上限一致）
    fNewPwdEdit->setEchoMode(QLineEdit::Password);
    // 输入框本身不带边框：外框统一由容器 pwdBox 画，
    // 否则"输入框右边框 + 紧贴的按钮"之间永远隔一条拼缝线（本次边框异常的根源）
    fNewPwdEdit->setStyleSheet(R"(
        QLineEdit {
            border: none;
            background: transparent;
            padding-left: 12px;
            font-size: 14px;
            color: #333333;
        }
    )");

    fNewPwdToggleBtn = new QPushButton(this);
    fNewPwdToggleBtn->setFixedSize(40, 40);
    fNewPwdToggleBtn->setFocusPolicy(Qt::NoFocus);
    fNewPwdToggleBtn->setStyleSheet(R"(
        QPushButton {
            border: none;
            background: transparent;
            padding: 0;
            cursor: pointer;
            border-radius: 0 7px 7px 0;   /* 贴合容器内径 8-1 的右角 */
        }
        QPushButton:hover {
            background-color: #f5f5f5;
        }
    )");
    fNewPwdToggleBtn->setIcon(QIcon(":/res/icon/eyes_show.svg"));
    fNewPwdToggleBtn->setVisible(false);

    // "输入组"容器：一只 QFrame 统一画边框和底色，输入框+按钮都嵌在里面
    // 聚焦时描边变蓝的切换逻辑在下方信号区（QSS 不支持 :focus-within，得用焦点信号补）
    QFrame* pwdBox = new QFrame(this);
    pwdBox->setObjectName("pwdBox");   // 选择器必须点名：QFrame 家族会误伤其他控件（卡片那次的坑）
    pwdBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    //水平方向：Expanding → 尽可能占满 Grid 给这一列全部宽度    垂直方向：Fixed → 高度固定，不会自动拉伸

    auto pwdBoxStyle = [](bool focused) {
        return QString(R"(
            #pwdBox {
                border: 1px solid %1;
                border-radius: 8px;
                background-color: rgba(255, 255, 255, 0.75);
            }
        )").arg(focused ? "#4a90d9" : "rgba(0, 0, 0, 0.08)");
    };
    pwdBox->setStyleSheet(pwdBoxStyle(false));

    QHBoxLayout* pwdRow = new QHBoxLayout(pwdBox);
    pwdRow->setContentsMargins(1, 1, 1, 1);   // 让出容器 1px 边框的宽度
    pwdRow->setSpacing(0);                    // 输入框↔按钮 0 间距
    pwdRow->addWidget(fNewPwdEdit);
    pwdRow->addWidget(fNewPwdToggleBtn);
    grid->addWidget(pwdBox, 1, 1);

    // 强度条：跨两列放在密码行正下方；第一行 = 四杠+等级（居中），第二行 = 两行提示文案（居中）
    m_strengthWidget = new QWidget(this);
    QVBoxLayout* strengthCol = new QVBoxLayout(m_strengthWidget);
    strengthCol->setContentsMargins(0, 0, 0, 0);
    strengthCol->setSpacing(6);

    QHBoxLayout* barsRow = new QHBoxLayout();
    barsRow->setSpacing(6);
    barsRow->addStretch();                       // 左侧弹性（与右侧对称 → 杠组整体居中）
    for (int i = 0; i < 4; ++i) {
        QFrame* bar = new QFrame(this);
        bar->setFixedSize(24, 4);                // 固定宽24，细条紧凑
        m_strengthBars[i] = bar;
        bar->setStyleSheet("QFrame { background: #dfe6f0; border-radius: 2px; border: none; }");
        barsRow->addWidget(bar);
    }
    m_strengthLabel = new QLabel(this);          // 等级文字，默认空（未输入）
    m_strengthLabel->setStyleSheet("color: #9aa4b5; font-size: 12px;");
    barsRow->addWidget(m_strengthLabel);
    barsRow->addStretch();                       // 右侧弹性
    strengthCol->addLayout(barsRow);

    // 提示文案：逗号后换行成两行、水平居中（QLabel 默认高度贴文字，两行后自然拉高）
    m_symbolHint = new QLabel("密码由大小写字母、数字和符号组成，\n符号仅支持 ! ? - .", this);
    m_symbolHint->setStyleSheet("color: #9aa4b5; font-size: 10px;");
    m_symbolHint->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    strengthCol->addWidget(m_symbolHint);

    m_strengthWidget->setVisible(false);         // 初始隐藏：没输密码时不打扰
    grid->addWidget(m_strengthWidget, 2, 0, 1, 2);

    // ===== 确认密码行 =====
    QLabel* fConfirmLabel = new QLabel("确认密码:", this);
    fConfirmLabel->setStyleSheet(labelStyle);
    fConfirmLabel->setFixedWidth(80);                          // 标签统一等宽
    fConfirmLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);  // 文字在固定宽度内右对齐
    grid->addWidget(fConfirmLabel, 3, 0);

    fConfirmEdit = new QLineEdit(this);
    fConfirmEdit->setPlaceholderText("请再次输入新密码");
    fConfirmEdit->setFixedHeight(42);
    fConfirmEdit->setMaxLength(15);              // 与"密码"一致，最多15位
    fConfirmEdit->setEchoMode(QLineEdit::Password);
    // 输入框本身不带边框：外框统一由容器 confirmBox 画（与 pwdBox 同一套方案），
    // 否则聚焦时"容器边框 + 输入框自带聚焦蓝框"会嵌套出框中框
    fConfirmEdit->setStyleSheet(R"(
        QLineEdit {
            border: none;
            background: transparent;
            padding-left: 12px;
            font-size: 14px;
            color: #333333;
        }
    )");
    
    fConfirmToggleBtn=new QPushButton(this);
    fConfirmToggleBtn->setFixedSize(40, 40);
    fConfirmToggleBtn->setFocusPolicy(Qt::NoFocus);
    fConfirmToggleBtn->setStyleSheet(R"(
        QPushButton {
            border: none;
            background: transparent;
            padding: 0;
            cursor: pointer;
            border-radius: 0 7px 7px 0;   /* 贴合容器内径 8-1 的右角 */
        }
        QPushButton:hover {
            background-color: #f5f5f5;
        }
    )");
    fConfirmToggleBtn->setIcon(QIcon(":/res/icon/eyes_show.svg"));
    fConfirmToggleBtn->setVisible(false);


    QFrame* confirmBox = new QFrame(this);
    confirmBox->setObjectName("confirmBox");
    confirmBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    //水平方向：Expanding → 尽可能占满 Grid 给这一列全部宽度    垂直方向：Fixed → 高度固定，不会自动拉伸
    auto confirmBoxStyle = [](bool focused) {
        return QString(R"(
            #confirmBox {
                border: 1px solid %1;
                border-radius: 8px;
                background-color: rgba(255, 255, 255, 0.75);
            }
        )").arg(focused ? "#4a90d9" : "rgba(0, 0, 0, 0.08)");
    };
    confirmBox->setStyleSheet(confirmBoxStyle(false));

    QHBoxLayout* confirmRow = new QHBoxLayout(confirmBox);
    confirmRow->setContentsMargins(1, 1, 1, 1);   // 让出容器 1px 边框的宽度
    confirmRow->setSpacing(0);                    // 输入框↔按钮 0 间距
    confirmRow->addWidget(fConfirmEdit);
    confirmRow->addWidget(fConfirmToggleBtn);
    // 注意：这里不加 addStretch——QLineEdit 和弹性垫片都是 Expanding，
    // 会平分多余宽度把输入框挤窄（右侧留空），与 pwdRow 保持一致让输入框占满

    grid->addWidget(confirmBox, 3, 1);

    FrameLayout->addLayout(grid);

    // 提交按钮（先做个占位效果，具体逻辑后面再接后端）
    fSubmitBtn = new QPushButton("确认修改", this);
    fSubmitBtn->setFixedHeight(42);
    fSubmitBtn->setStyleSheet(R"(
        QPushButton {
            border-radius: 8px;
            background-color: #7f91a3ff;
            color: white;
            font-size: 15px;
            font-weight: bold;
            border: none;
        }
        QPushButton:hover {
            background-color: #3a80c9;
        }
        QPushButton:pressed {  /*表示按钮被按下*/
            background-color: #2f70b5;
        }
    )");
    fSubmitBtn->setEnabled(false);
    layout->addWidget(fSubmitBtn);
    layout->addSpacing(6);  // 主按钮和次要的返回按钮贴紧凑，形成一组操作

    // 错误提示行：常驻占一行（16px），布局不跳动；默认无文字，密码不一致时才显示红字
    m_pwdMismatchHint = new QLabel(this);
    m_pwdMismatchHint->setFixedHeight(16);
    m_pwdMismatchHint->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    m_pwdMismatchHint->setStyleSheet("QLabel { color: rgba(224, 91, 91, 0.6); font-size: 12px; font-weight: bold; }");
    layout->addWidget(m_pwdMismatchHint);

    // 按钮左右抖动动画：QPropertyAnimation 动画 pos 属性，keyValueAt 设四次左右偏移，
    // 基准位置（按钮静止处）在第一次出错时才捕获——那时按钮一定已被布局摆到位
    //pos是按钮的位置属性，pos.x()是x轴的偏移量，pos.y()是y轴的偏移量，这个是 Qt 元对象系统（MOC）的**属性名字符串**。
    m_shakeAnim = new QPropertyAnimation(fSubmitBtn, "pos", this);
    //动画控制器对象，管理一整套动画：关键帧、时长、停止、启动、finished 信号
    /*- `stop()`：立刻掐断正在跑的抖动；`setKeyValueAt()`：重新设置这一轮抖动的轨迹；`start()`：跑一遍；`finished`信号只需要 connect 绑定一次，不用每次重复 connect。*/ 
    m_shakeAnim->setDuration(320);

    // 返回登录按钮（点击通知容器切回登录页）
    fBackBtn = new QPushButton("返回登录", this);
    fBackBtn->setStyleSheet(R"(
        QPushButton {
            color: #4a90d9;
            font-size: 14px;
            border: none;
            background: transparent;
            text-align: center;
        }
        QPushButton:hover {
            color: #3a80c9;
        }
    )");
    fBackBtn->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(fBackBtn);
    layout->addStretch(3);  // 尾部弹性垫片：吃掉窗口富余高度（与首部 2:3，内容居中偏上）

    // ========== 信号连接统一放这里 ==========
    // 全部控件创建完毕后再集中 connect，不插在控件创建中间，便于查找和维护
    // 密码输入变化 → 刷新强度条
    connect(fNewPwdEdit, &QLineEdit::textChanged, this, &ForgotPasswordPage::updatePasswordStrength);
    // 焦点进入/离开密码框 → 容器 pwdBox 描边在灰/蓝之间切换（QSS 没有 :focus-within，用全局焦点信号补）
    connect(qApp, &QApplication::focusChanged, this, [=](QWidget*, QWidget* now) {
        pwdBox->setStyleSheet(pwdBoxStyle(now == fNewPwdEdit));
        confirmBox->setStyleSheet(confirmBoxStyle(now == fConfirmEdit));
    });//qApp 是 Qt 的一个宏，指向整个程序唯一的应用对象实例
    // 两个容器（pwdBox/confirmBox）同一套"子控件聚焦→父容器描边变蓝"的逻辑，
    // 焦点落在哪个输入框，就点亮对应容器；焦点在别处则都恢复灰
    // 确认修改 → 统一提交槽函数（页内校验，通过后 emit ModifyPwdAquird 给主后端）
    // 注意：这里必须连 onSubmitClicked，之前残留的 lambda 弹窗会顶掉新逻辑
    connect(fSubmitBtn, &QPushButton::clicked, this, &ForgotPasswordPage::onSubmitClicked);
    // 修改密码请求（onSubmitClicked 校验通过后 emit）→ 主后端 modifyPwd 走 TCP
    connect(this, &ForgotPasswordPage::ModifyPwdAquird, m_backend, &MainBackend::modifyPwd);
    // 任一密码框有输入变化 → 清掉错误提示红字
    connect(fNewPwdEdit, &QLineEdit::textChanged, m_pwdMismatchHint, &QLabel::clear);
    connect(fConfirmEdit, &QLineEdit::textChanged, m_pwdMismatchHint, &QLabel::clear);
    // 抖动动画结束 → 恢复按钮正常样式（updateSubmitButtonState 按当前状态重设正确样式）
    connect(m_shakeAnim, &QPropertyAnimation::finished, this, &ForgotPasswordPage::updateSubmitButtonState);
    connect(fNewPwdEdit, &QLineEdit::textChanged, this, &ForgotPasswordPage::updateSubmitButtonState);
    connect(fConfirmEdit, &QLineEdit::textChanged, this, &ForgotPasswordPage::updateSubmitButtonState);
    // 返回登录 → 通知容器切页
    connect(fBackBtn, &QPushButton::clicked, this, &ForgotPasswordPage::backToLoginRequested);
    // 眼睛可见性按钮 → 切换密码明文/密文：两个按钮复用同一个 togglePasswordVisibility，
    // 闭包各自绑定自己的输入框和按钮，比 sender()/objectName 映射更直观、编译期可查
    connect(fNewPwdToggleBtn, &QPushButton::clicked, this,
            [this] { togglePasswordVisibility(fNewPwdEdit, fNewPwdToggleBtn); });
    connect(fConfirmToggleBtn, &QPushButton::clicked, this,
            [this] { togglePasswordVisibility(fConfirmEdit, fConfirmToggleBtn); });
    // 输入框内容变化 → 同步"空密码时隐藏眼睛按钮"（眼睛按钮只在有内容时才显示）
    connect(fNewPwdEdit, &QLineEdit::textChanged, this,
            [this] { togglePasswordBtnVisibility(fNewPwdEdit, fNewPwdToggleBtn); });
    connect(fConfirmEdit, &QLineEdit::textChanged, this,
            [this] { togglePasswordBtnVisibility(fConfirmEdit, fConfirmToggleBtn); });
    connect(m_backend, &MainBackend::modifyPwdWaiting, this, &ForgotPasswordPage::onModifyPwdWaiting);
    connect(m_backend, &MainBackend::modifyPwdSuccess, this, &ForgotPasswordPage::onModifyPwdSuccess);
    connect(m_backend, &MainBackend::modifyPwdFailed, this, &ForgotPasswordPage::onModifyPwdFailed);
    connect(m_backend, &MainBackend::modifyPwdTimeout, this, &ForgotPasswordPage::onModifyPwdTimeout);
    connect(m_modifyPwdAnimTimer, &QTimer::timeout, this, &ForgotPasswordPage::updateSubmitButtonAnimation);
}

// 填充账号输入框（登录页点"忘记密码"时把当前输入的账号带过来）
void ForgotPasswordPage::setAccount(const QString& account)
{
    if (fAccountEdit) {
        fAccountEdit->setText(account);
    }
}

// 密码强度检测：点击"确认修改"前也复用这套分级，保证前端提示与提交校验一致。
// 分级（区分大小写，符号仅允许 ! ? - . ，密码最多15位）：
//   0 级：未输入 → 四杠全灭
//   1 弱(红/1杠)：只有数字/字母/符号中的一种
//   2 中(黄/2杠)：任意两种组合（数字+字母、字母+符号、符号+数字）
//   3 强(绿/3杠)：数字+字母+符号都有（不区分大小写）
//   4 极强(蓝/4杠)：数字+字母+符号都有，且同时含大写和小写字母
void ForgotPasswordPage::updatePasswordStrength(const QString& pwd)
{
    // 空密码：隐藏整块强度条（用户要求"输入密码之后才开始显示"）
    const bool empty = pwd.isEmpty();
    m_strengthWidget->setVisible(!empty);
    if (empty) {
        return;
    }

    // 逐字符分类；非法状态记入成员变量 m_hasIllegal，供提交校验等后续逻辑使用
    bool hasDigit = false, hasLower = false, hasUpper = false, hasSymbol = false;
    m_hasIllegal = false;
    for (QChar c : pwd) {
        if (c.isDigit())          hasDigit  = true;
        else if (c.isLower())     hasLower  = true;
        else if (c.isUpper())     hasUpper  = true;
        else if (c == '!' || c == '?' || c == '-' || c == '.') hasSymbol = true;
        else                      m_hasIllegal = true;  // 出现合法集之外的字符
    }
    const bool hasAlpha = hasLower || hasUpper;  // 大小写统一算"字母类"
    const int categoryCount = (hasDigit ? 1 : 0) + (hasAlpha ? 1 : 0) + (hasSymbol ? 1 : 0);

    int level = 0;
    QString color, text;
    if (pwd.isEmpty()) {
        level = 0;  // 未输入：全灭，无文字
    } else if (hasDigit && hasAlpha && hasSymbol && hasLower && hasUpper) {
        level = 4; color = "#4a90d9"; text = "极强";  // 蓝
    } else if (hasDigit && hasAlpha && hasSymbol) {
        level = 3; color = "#43a047"; text = "强";    // 绿
    } else if (categoryCount == 2) {
        level = 2; color = "#f0b429"; text = "中";    // 黄
    } else {
        level = 1; color = "#e05b5b"; text = "弱";    // 红
    }

    // 点亮前 level 根杠，其余保持极浅灰熄灭
    const QString dimColor = "#dfe6f0";
    for (int i = 0; i < 4; ++i) {
        const QString barColor = (i < level) ? color : dimColor;
        m_strengthBars[i]->setStyleSheet(
            QString("QFrame { background: %1; border-radius: 2px; border: none; }").arg(barColor));
    }

    // 等级文字：无输入则留空，有输入则显示等级且颜色随强度变化
    m_strengthLabel->setText(text);
    m_strengthLabel->setStyleSheet(
        QString("color: %1; font-size: 12px; font-weight: bold;").arg(level == 0 ? "#9aa4b5" : color));

    // 合法符号提示：出现非法字符时变红提醒，否则恢复灰色（两行文案与初始文本一致，
    // 每次输入都会 setText 覆盖，文案必须写在这里才会被看到）
    // 注意：提交按钮的启用/禁用不在这一段做，统一交给 updateSubmitButtonState()，
    // 避免两处逻辑互相覆盖
    if (m_hasIllegal) {
        m_symbolHint->setText("含不允许的字符，\n符号仅支持 ! ? - .");
        m_symbolHint->setStyleSheet("color: #e05b5b; font-size: 10px;");
    } else {
        m_symbolHint->setText("密码由大小写字母、数字和符号组成，\n符号仅支持 ! ? - .");
        m_symbolHint->setStyleSheet("color: #9aa4b5; font-size: 10px;");
    }
}

// 槽函数：刷新提交按钮可用性。
// 启用条件 = 密码非空 + 确认框非空 + 无非法字符（m_hasIllegal 由 updatePasswordStrength 维护）。
// 密码清空时 updatePasswordStrength 提前返回、m_hasIllegal 保留旧值，但"密码非空"
// 这一条件本身就拦截了空密码提交，所以不影响正确性
void ForgotPasswordPage::updateSubmitButtonState()
{
    const bool bothFilled = !fNewPwdEdit->text().isEmpty() && !fConfirmEdit->text().isEmpty();
    if(bothFilled && !m_hasIllegal)
    {
      fSubmitBtn->setEnabled(true);
      fSubmitBtn->setStyleSheet(R"(
        QPushButton {
            border-radius: 8px;
            background-color: #4a90d9;
            color: white;
            font-size: 15px;
            font-weight: bold;
            border: none;
        }
        QPushButton:hover {
            background-color: #3a80c9;
        }
        QPushButton:pressed {  /*表示按钮被按下*/
            background-color: #2f70b5;
        }
    )");
    }
    else
    {
    fSubmitBtn->setEnabled(false);
    fSubmitBtn->setStyleSheet(R"(
        QPushButton {
            border-radius: 8px;
            background-color: #7f91a3ff;
            color: white;
            font-size: 15px;
            font-weight: bold;
            border: none;
        }
        QPushButton:hover {
            background-color: #3a80c9;
        }
        QPushButton:pressed {  /*表示按钮被按下*/
            background-color: #2f70b5;
        }
    )");
    }
}

// 槽函数：点击"确认修改"（占位实现，内容暂时不动，后续在这里接后端找回密码逻辑）
void ForgotPasswordPage::onSubmitClicked()
{
    if(fNewPwdEdit->text()!=fConfirmEdit->text())
    {
        // 密码不一致：统一走错误反馈（红字 + 红色按钮 + 抖动动画），
        // 具体渲染抽到 triggerErrorFeedback，点这条路径也方便复用给其它校验
        triggerErrorFeedback("两次输入的密码不一致");
        return;
    }
    else
    {
        // 两次密码一致：向主后端发修改密码请求（账号只读框里的值 + 校验过的新密码）
        emit ModifyPwdAquird(fAccountEdit->text(), fNewPwdEdit->text());
    }
}

// 工具函数：错误反馈（红字提示 + 按钮红色错误态样式 + 左右抖动动画）。
// 抽出便于复用：密码不一致、后续接入后端的提交失败等校验都能调这条路径。
// 动画结束时由 finished → updateSubmitButtonState 按当前输入状态复位按钮样式。
void ForgotPasswordPage::triggerErrorFeedback(const QString& hint)
{
    // 按钮下一行显示红字（常驻占位行，不会引起布局跳动）
    m_pwdMismatchHint->setText(hint);

    // 临时错误态样式：按钮整体染成红色系（淡红内里 + 红字 + 红框），不再是"纯蓝底+红框"的割裂配色；
    // 动画结束由 finished → updateSubmitButtonState 恢复正常样式
    fSubmitBtn->setStyleSheet(R"(
        QPushButton {
            border-radius: 8px;
            background-color: rgba(224, 91, 91, 0.14);   /* 内里染淡红 */
            color: rgba(224, 91, 91, 0.85);              /* "确认修改"文字跟着变红 */
            font-size: 15px;
            font-weight: bold;
            border: 2px solid rgba(224, 91, 91, 0.45);   /* 红框同步调柔 */
        }
        QPushButton:hover  { background-color: rgba(224, 91, 91, 0.20); }
        QPushButton:pressed { background-color: rgba(224, 91, 91, 0.26); }
    )");

    // 左右抖动：以基准位为中心 ±8/±5px 递减摆动，结束时回到基准位
    m_shakeAnim->stop();          // 上一次还在抖就先停，防止连点叠加
    // 基准位置只在第一次出错时捕获，之后锁死不更新：
    // 若每次都拿当前 pos() 当基准，连点时会停在动画中途的偏移坐标上，
    // 误把它当新基准 → 按钮越抖越偏（连点漂移）。首帧此时布局已到位，
    // 真实静止坐标就是基准"家"，后面始终围绕它摆动能确保落到原位
    if (!m_btnRestValid) {        // 只在按钮处于布局位时捕获基准（第一次出错时）
        m_btnRestPos = fSubmitBtn->pos();
        m_btnRestValid = true;
    }
    const QPoint base = m_btnRestPos;
    m_shakeAnim->setStartValue(base);
    m_shakeAnim->setKeyValueAt(0.15, base + QPoint(-8, 0));// 时间15%：向左偏移8px
    m_shakeAnim->setKeyValueAt(0.35, base + QPoint( 8, 0));// 时间35%：向右偏移8px
    m_shakeAnim->setKeyValueAt(0.55, base + QPoint(-5, 0));// 时间55%：向左偏移5px
    m_shakeAnim->setKeyValueAt(0.75, base + QPoint( 5, 0));// 时间75%：向右偏移5px
    m_shakeAnim->setEndValue(base);// 时间100%：回到基准位
    m_shakeAnim->start();
}

// 工具函数：切换密码框的明文/密文显示（两个眼睛按钮复用这一份逻辑）。
// 两个输入框的切换动作完全相同，只有"目标是哪个输入框/按钮"不同——
// 所以抽成带参函数，两处 lambda 各传各的控件，逻辑只维护这一处，
// 避免两份重复 lambda 以后要改（比如加动画）得同步改两处
void ForgotPasswordPage::togglePasswordVisibility(QLineEdit* edit, QPushButton* toggleBtn)
{
    if (edit->echoMode() == QLineEdit::Password)   // 当前是隐藏态
    {
        edit->setEchoMode(QLineEdit::Normal);              // 切换成明文
        toggleBtn->setIcon(QIcon(":/res/icon/eyes_clicked.svg"));  // "睁开眼"图标=明文可见
    }
    else                                               // 当前是明文态
    {
        edit->setEchoMode(QLineEdit::Password);               // 切换回隐藏
        toggleBtn->setIcon(QIcon(":/res/icon/eyes_show.svg"));   // "闭上眼"图标=密文隐藏
    }
}

// 工具函数：根据输入框内容控制眼睛按钮的显隐（空密码时不显示眼睛按钮）。
// 因为一旦没内容，切换明文/密文没有意义，直接隐藏按钮让界面更干净；
// 它与 togglePasswordVisibility（切换明文/密文）是两个独立功能，配合使用
void ForgotPasswordPage::togglePasswordBtnVisibility(QLineEdit* edit, QPushButton* toggleBtn)
{
    toggleBtn->setVisible(!edit->text().isEmpty());
}

// 修改密码请求等待期间统一开关本页交互
// 禁用阶段：新密码框、确认框、两个眼睛按钮、提交按钮、返回按钮全锁死，
//          防止等待中重复提交或偷改密码
// 恢复阶段：把"常驻可用"的控件开回来，并调一次 updateSubmitButtonState() 按当前输入
//          重算提交按钮（新密码+确认框都填了才允许点），否则解锁后按钮会一直停在禁用态
// 注意：fAccountEdit 不参与开关——它在 setupUI 里本来就 setEnabled(false)，
//      是"只读展示框"（账号只由登录页 setAccount 带入，不允许手动改），
//      一旦在这里恢复成 true 就把这个设定破坏了
void ForgotPasswordPage::setInputsEnabled(bool enabled)
{
    // 输入框 + 眼睛按钮
    fNewPwdEdit->setEnabled(enabled);
    fConfirmEdit->setEnabled(enabled);
    fNewPwdToggleBtn->setEnabled(enabled);
    fConfirmToggleBtn->setEnabled(enabled);

    // 返回登录按钮
    fBackBtn->setEnabled(enabled);

    if (enabled) {
        // 提交按钮的可用性由输入内容决定，交给它重算
        updateSubmitButtonState();
    } else {
        fSubmitBtn->setEnabled(false);
    }
}

void ForgotPasswordPage::onModifyPwdWaiting()
{
    m_originalSubmitText = fSubmitBtn->text();
    setInputsEnabled(false);
    m_dostcount = 0;
    fSubmitBtn->setText(tr("正在修改密码"));
    fSubmitBtn->setStyleSheet(R"(
        QPushButton {
            border-radius: 8px;
            background-color: #999999;
            color: white;
            font-size: 16px;
            font-weight: bold;
            border: none;
        }
    )");
    m_modifyPwdAnimTimer->start(500); // 500ms动画
}

void ForgotPasswordPage::onModifyPwdTimeout()
{ 
    m_modifyPwdAnimTimer->stop();
    setInputsEnabled(true);
    fSubmitBtn->setText(m_originalSubmitText);
    fSubmitBtn->setStyleSheet(R"(
        QPushButton {
            border-radius: 8px;
            background-color: #7f91a3ff;
            color: white;
            font-size: 15px;
            font-weight: bold;
            border: none;
        }
        QPushButton:hover {
            background-color: #3a80c9;
        }
        QPushButton:pressed {  /*表示按钮被按下*/
            background-color: #2f70b5;
        }
    )");
    updateSubmitButtonState();
}

// 修改密码成功（内容待实现：成功反馈 + 状态复位）
void ForgotPasswordPage::onModifyPwdSuccess()
{
}

// 修改密码失败（内容待实现：错误反馈 + 状态复位）
void ForgotPasswordPage::onModifyPwdFailed()
{
}

void ForgotPasswordPage::updateSubmitButtonAnimation()
{
    m_dostcount=(m_dostcount+1)%4;
    QString text="正在修改密码";
    for(int i=0;i<m_dostcount;i++)
    {
        text+=".";
        fSubmitBtn->setText(text);
    }
}

