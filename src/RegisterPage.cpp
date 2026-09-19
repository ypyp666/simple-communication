#include "RegisterPage.h"
#include "GlassCard.h"   // 毛玻璃卡片（忘记密码页/注册页共用的真·模糊背景）
#include "MainBackend.h" // 页面显示时要直连后端发起 TCP 连接（信号对槽，见构造函数）
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QFrame>
#include <QSizePolicy>
#include <QIcon>
#include <QPropertyAnimation>   // 动画头文件：注册失败提示的按钮抖动（与忘记密码页同一套）
#include <QApplication>   // qApp：全局焦点信号，用来给输入框容器切换聚焦描边
#include <QPainter>       // 水印倒影要自绘（QLabel 只会正着画字，画不出倒影）
#include <QFontMetrics>
#include <QLinearGradient>
#include <QPaintEvent>
#include <QTransform>
#include <QGraphicsScene>        // 高斯模糊，与 GlassCard::blurPixmap 同一套做法
#include <QGraphicsPixmapItem>
#include <QGraphicsBlurEffect>
#include "MessageStatusIndicator.h"


namespace {

// 密码位数下限：不足 5 位时提交按钮保持置灰（与忘记密码页同一规则）
constexpr int kMinPasswordLength = 5;

// ========== 水印 + 倒影（头像两侧 "WEL/你好"、"COME/新用户"）==========
// QLabel 只会老老实实把文字正着画出来，给不出"倒影"。所以这里自绘一个轻量控件，
// 一行 paintEvent 里画两块：上面是正片文字（直接画在控件上，清晰锐利），下面紧接着一份镜像倒影。
// 倒影四步：
//   ① 把两行文字先按设备分辨率渲染成一张透明底图（只作为倒影的母版）
//   ② 母版上下翻转 → 得到倒影（原图最下面那行会紧贴着正文出现，符合真实反射的观感）
//   ③ 翻转后的图压扁到 50% 再高斯模糊（模糊只作用在倒影上，做法照搬 GlassCard）
//   ④ 盖上自上而下的渐变蒙版：贴着文字处还看得见，越往下越淡直到完全透明
class WatermarkReflection : public QWidget
{
public:
    // text：两行水印（用 '\n' 分行，避免 raw string 把引号当成正文）；
    // align：整体贴左还是贴右（对应头像左右两侧的收边方向）；colWidth：固定列宽
    WatermarkReflection(const QString& text, Qt::Alignment align, int colWidth,
                        QWidget* parent = nullptr)
        : QWidget(parent), m_text(text), m_align(align)
    {
        // 原 QLabel 水印是 40px 加粗。但窗口是固定 400x500 的硬约束，而头部这块的高度
        // = 正文(2 行) + 倒影 = 3 倍行高：40px 字号下实测要吃掉 ~142px（近三成页面高度），
        // 强度条一显示就顶不住，缺额被布局分摊到密码行 → 两个密码框被压扁。
        // 收到 34px 后头部降到 ~113px，省出来的纵向空间正好还给卡片。
        m_font = font();
        m_font.setPixelSize(34);
        m_font.setBold(true);

        // 自己量文字尺寸：按 '\n' 分行，取最宽一行作正文宽度，行高 × 行数作正文高度。
        // （QLabel 的 sizeHint 拿不到"两行文字各自的宽度"，自绘就得自己算）
        const QStringList lines = m_text.split('\n');
        const QFontMetrics fm(m_font);
        m_lineHeight = fm.height();
        m_textWidth = 0;
        for (const QString& line : lines)
            m_textWidth = qMax(m_textWidth, fm.horizontalAdvance(line));
        m_textHeight = m_lineHeight * lines.size();
        m_reflHeight = qRound(m_textHeight * 0.4);   // 倒影取正文 40% 高：够像"反射"，又给固定高度的窗口省出纵向空间

        // 高度 = 正文 + 倒影（网格行高由它决定）；宽度固定，与左右两列一致。
        // 另外：纯装饰控件，别去抢鼠标事件
        setFixedSize(colWidth, m_textHeight + m_reflHeight);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);

        // 贴左还是贴右：左水印右对齐（右边缘收在头像左侧），右水印左对齐
        const int offsetX = (m_align & Qt::AlignRight) ? width() - m_textWidth : 0;

        // ① 正片：直接在控件上写字。控件绘制走的是设备分辨率，高 DPI 下笔画依旧锐利；
        //    正片全程不经过 QPixmap，也就不会被"放大 + 平滑"糊掉——模糊只留给下面的倒影。
        p.setFont(m_font);
        p.setPen(m_textColor);
        drawWatermark(p, offsetX);

        // ② 倒影：唯一模糊的一层，只算一次（paintEvent 会被反复调用，不能每次都重新渲染 + 模糊）
        if (m_reflection.isNull())
            buildReflection();
        p.drawPixmap(QRect(offsetX, m_textHeight, m_textWidth, m_reflHeight), m_reflection);
    }

private:
    // 两行水印按行排出来（正片与"倒影母版"共用这段排版逻辑）
    void drawWatermark(QPainter& p, int offsetX) const
    {
        const QStringList lines = m_text.split('\n');
        const Qt::Alignment hAlign =
            (m_align & Qt::AlignRight) ? Qt::AlignRight : Qt::AlignLeft;
        for (int i = 0; i < lines.size(); ++i)
            p.drawText(QRect(offsetX, i * m_lineHeight, m_textWidth, m_lineHeight),
                       hAlign, lines[i]);
    }

    // 只算倒影：正片不走 pixmap，所以不存在"正片被放大变糊"的问题
    void buildReflection()
    {
        // 高 DPI 下必须按设备分辨率出图：否则倒影会被拉伸，糊成一块灰
        const qreal dpr = devicePixelRatioF();

        // ① 水印渲染成透明底图（按 dpr 放大后再画，等于按设备分辨率出图），只用于生成倒影
        QPixmap base(qRound(m_textWidth * dpr), qRound(m_textHeight * dpr));
        base.fill(Qt::transparent);
        {
            QPainter bp(&base);
            bp.setRenderHint(QPainter::Antialiasing);
            bp.setFont(m_font);
            bp.setPen(m_textColor);
            bp.scale(dpr, dpr);   // 之后照搬逻辑坐标即可
            drawWatermark(bp, 0);
        }

        // ② 上下翻转（scale(1,-1)）→ ③ 压扁到一半 → ④ 轻模糊
        // 模糊半径收小到 2（再乘 dpr 抵消分辨率）：倒影要看得清笔画，但不能清零——
        // 这层模糊就是"毛玻璃反光面"的质感来源
        const QPixmap flipped = base.transformed(QTransform().scale(1, -1));
        m_reflection = flipped.scaled(qRound(m_textWidth * dpr), qRound(m_reflHeight * dpr),
                                      Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        m_reflection = blurred(m_reflection, 2.0 * dpr);

        // ⑤ 渐变蒙版：DestinationIn 只保留"蒙版有颜色的部分"，蒙版越透明 → 倒影越淡，
        // 于是自上而下自然消失。
        // 注意倒影的最终不透明度 = 水印色 alpha × 这里的蒙版 alpha（蒙版是乘上去的），
        // 所以正片 alpha 从 160 提到 230 后，倒影会跟着深 44%——顶端差点跟正片一样黑，
        // 那就成了"重影"，反而更像糊。这里把顶端 230→190、中段 90→70 抵掉这次增益，
        // 让倒影维持原来的观感：看得见、但明确比正片淡一档，模糊只留给它。
        {
            QPainter mp(&m_reflection);
            mp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            QLinearGradient g(0, 0, 0, m_reflection.height());
            g.setColorAt(0.0, QColor(255, 255, 255, 190));
            g.setColorAt(0.6, QColor(255, 255, 255, 70));
            g.setColorAt(1.0, QColor(255, 255, 255, 0));
            mp.fillRect(m_reflection.rect(), g);
        }
    }

    // 高斯模糊：照搬 GlassCard::blurPixmap（QGraphicsScene + QGraphicsBlurEffect）
    static QPixmap blurred(const QPixmap& src, qreal radius)
    {
        if (radius <= 0.0 || src.isNull())
            return src;
        QGraphicsScene scene;
        QGraphicsPixmapItem item(src);
        auto* eff = new QGraphicsBlurEffect;
        eff->setBlurRadius(radius);
        eff->setBlurHints(QGraphicsBlurEffect::QualityHint);
        item.setGraphicsEffect(eff);   // effect 由 item 接管，不用手动 delete
        scene.addItem(&item);
        QPixmap out(src.size());
        out.fill(Qt::transparent);
        QPainter p(&out);
        scene.render(&p, QRectF(out.rect()), QRectF(src.rect()));
        return out;
    }

    QString m_text;
    Qt::Alignment m_align;
    QFont m_font;
    int m_lineHeight = 0;    // 单行行高
    int m_textWidth = 0;     // 正文宽（最宽一行）
    int m_textHeight = 0;    // 正文总高
    int m_reflHeight = 0;    // 倒影高（正文总高的 40%）
    // 水印色：淡蓝（色相不变，只调不透明度）。
    // 这里原来 alpha=56（≈0.22）：笔画与浅背景只差 ~24 级灰阶，40px 大字的抗锯齿边
    // 在这么低的对比下等于看不见，整行字就"糊成一团晕"——注意此时笔画边缘其实只有
    // 1 个像素（实测），空间上并不模糊，是"看不清"被眼睛读成了"模糊"。
    // 提到 110（≈0.43）后仍是同一个淡蓝，但笔画立住了、边缘看得见。
    // 实测仍偏"发虚"：110 时笔画灰度 ~194、背景 243，只差 49 级，30 多像素的大字在这种
    // 低对比下依然读成"晕"。再提到 160（≈0.63）→ 笔画灰度降到 ~137，与背景拉开 ~106 级，
    // 边缘（始终只有 1 像素过渡，空间上从未模糊）才真正"立"住。
    // 160 复测：笔画灰度实测 173、背景 244 —— 仍只差 71 级（0.299R+0.587G+0.114B 算的，
    // 与 0.63*131+0.37*243≈173 的合成公式完全对得上，说明"发虚"确实只是对比度不够）。
    // 所以这次不挤牙膏，直接把对比度拉到接近实心：alpha=230（≈0.90）→ 笔画灰度 ~142，
    // 与背景差 ~102 级。正片依旧是蓝色、依旧不是 255 的实色，但已经不可能被读成"糊"。
    QColor m_textColor = QColor(74, 144, 217, 230);
    QPixmap m_reflection;    // 倒影（缓存；正片直接画在控件上，不需要缓存）
};

}   // namespace

/*
RegisterPage 目前是"骨架"状态：控件已建好、信号已接好，各业务流程槽函数均为空实现，
业务逻辑（页内校验、等待动画、服务器拒绝 / 网络连不上的区分、结果提示）待补。
补逻辑时的参照物是 ForgotPasswordPage：两者页面结构、等待动画、错误反馈几乎同构。
*/

RegisterPage::RegisterPage(MainBackend* backend, QWidget *parent)
    : QWidget(parent)
    , m_backend(backend)
{
    // 等待动画定时器：与忘记密码页一致，500ms 走一格点；
    // 这里只创建不启动，进等待态（onRegisterWaiting）时才 start
    m_registerAnimTimer = new QTimer(this);
    m_registerAnimTimer->setInterval(500);

    setupUI();

    // 页面一被显示（QStackedWidget 切到本页）就发 needAccountFromServer，
    // 由这里直接连到后端把 TCP 连接建起来——与 LoginPage 把 loginAquiard
    // 直连 MainBackend::login 是同一套写法，UI 不碰 TcpClient，只发信号
    connect(this, &RegisterPage::needAccountFromServer,
            m_backend, &MainBackend::prepareRegisterConnection);

    // 注册第二段（提交）：填好密码后 onSubmitClicked 校验通过 emit registerAquiard，
    // 直接绑到 MainBackend::registerUser 转发出去，中间不再有保存参数/重连的中间层
    connect(this, &RegisterPage::registerAquiard,
            m_backend, &MainBackend::registerUser);

    // 注册各阶段结果：后端经 TCP 拿到后回传，本页据此提示/切页
    connect(m_backend, &MainBackend::registerWaiting, this, &RegisterPage::onRegisterWaiting);
    connect(m_backend, &MainBackend::registerSuccess, this, &RegisterPage::onRegisterSuccess);
    connect(m_backend, &MainBackend::registerFailed, this, &RegisterPage::onRegisterFailed);
    // 网络层连不上（服务器没跑/断网）与"服务器拒绝"分开：信号带 reason，槽用不到，靠参数少的槽自动丢弃
    connect(m_backend, &MainBackend::registerNetworkError, this, &RegisterPage::onRegisterNetworkError);
    connect(m_backend, &MainBackend::registerTimeout, this, &RegisterPage::onRegisterTimeout);
    // 取号（进页即拉起 TCP 的连接阶段）各阶段结果：与上面的提交阶段分开，
    // 本页只用来驱动账号行指示器（骨架，槽内逻辑待补）
    connect(m_backend, &MainBackend::connectForRegisterWaiting, this, &RegisterPage::onConnectForRegisterWaiting);
    connect(m_backend, &MainBackend::connectForRegisterSuccess, this, &RegisterPage::onConnectForRegisterSuccess);
    connect(m_backend, &MainBackend::connectForRegisterFailed, this, &RegisterPage::onConnectForRegisterFailed);
    connect(m_backend, &MainBackend::connectForRegisterTimeout, this, &RegisterPage::onConnectForRegisterTimeout);
    // 等待动画定时器 → 刷新按钮上的省略号（与忘记密码页同一套）
    connect(m_registerAnimTimer, &QTimer::timeout, this, &RegisterPage::updateSubmitButtonAnimation);
}

void RegisterPage::setupUI()
{
    MessageStatusIndicator* m_statusIndicator = new MessageStatusIndicator(this);
    // ========== 注册页自己的根布局 ==========
    // 与忘记密码页同一套布局骨架：首尾弹性垫片让内容整体居中，
    // 富余高度只落在垫片上，不会渗进卡片把表单行拉开
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setSpacing(0);
    // 上下边距由 28 收到 16：窗口是固定 400x500 的硬约束，而注册页比忘记密码页
    // 多一整块强度条，这 24px 是腾给强度条的（否则内容总高超窗 → 布局压缩 → 强度文案被挤变形）
    layout->setContentsMargins(20, 16, 20, 16);
    layout->addStretch(2);

    // ========== 头部：头像居中 + 左右两半"WELCOME / 你好新用户"透明水印 ==========
    // 拆成三列：[左侧水印] [头像] [右侧水印]，头像正好落在水印中间那道空档里。
    // 不再用一整块文字垫在头像后面——那样左右留白不均，"空格"对不准头像中心
    QGridLayout* headerGrid = new QGridLayout();
    headerGrid->setContentsMargins(0, 0, 0, 0);
    headerGrid->setHorizontalSpacing(6);   // 水印 ↔ 头像之间的空档
    headerGrid->setVerticalSpacing(0);
    // 左右两列固定等宽（见下面 watermarkColWidth）→ 中间列必然落在页面水平中心，
    // 富余的那几 px 全给中间列，头像在中间列里居中
    headerGrid->setColumnStretch(1, 1);

    // 水印列宽按窗口算死：窗口固定 400，根布局左右各 20 边距 → 可用 360，
    // 130 + 6 + 80(头像) + 6 + 130 = 352，还剩 8px；34px 字号下"新用户"排得下还富余
    const int watermarkColWidth = 130;

    // 左半边：右对齐 → "WEL" / "你好" 的右边缘一起收在头像左侧；
    // 控件自带"镜像 + 模糊 + 渐隐"的倒影，见文件顶部 WatermarkReflection
    WatermarkReflection* leftWatermark =
        new WatermarkReflection("WEL\n你好", Qt::AlignRight, watermarkColWidth, this);
    headerGrid->addWidget(leftWatermark, 0, 0);

    // 右半边：左对齐 → "COME" / "新用户" 的左边缘一起从头像右侧开始；同样带倒影
    WatermarkReflection* rightWatermark =
        new WatermarkReflection("COME\n新用户", Qt::AlignLeft, watermarkColWidth, this);
    headerGrid->addWidget(rightWatermark, 0, 2);

    // 头像：与登录页同一尺寸/配色，只占位无逻辑；竖直方向居中于水印两行，
    // 圆心正好落在 "WELCOME" 与 "你好新用户" 之间那道空档上
    avatarLabel = new QLabel(this);
    avatarLabel->setFixedSize(80, 80);
    avatarLabel->setStyleSheet(R"(
        QLabel {
            border-radius: 40px;
            background-color: #4a90d9;
            border: 3px solid white;
        }
    )");
    headerGrid->addWidget(avatarLabel, 0, 1, Qt::AlignCenter);

    layout->addLayout(headerGrid);
    layout->addSpacing(8);      // 头部 ↔ 卡片间距

    // 毛玻璃卡片：与忘记密码页共用的 GlassCard（截屏+高斯模糊垫底）
    GlassCard* card = new GlassCard(this);
    card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addWidget(card);
    layout->addSpacing(12);     // 卡片 ↔ 返回登录间距

    QVBoxLayout* cardLayout = new QVBoxLayout(card);
    // 卡片内边距上下由 22 收到 16：同上，为强度条腾纵向空间（窗口 400x500 是硬约束）
    cardLayout->setContentsMargins(18, 16, 18, 16);
    cardLayout->setSpacing(12);   // 网格 ↔ 注册按钮之间的间距

    // ===== 表单统一用 QGridLayout（照搬忘记密码页）=====
    // 列0 = 侧边标签（统一 80px 右对齐），列1 = 输入框（拉伸填满剩余宽度）。
    // 用网格而非竖直直排，才有"账号: / 密码: / 确认密码:"这些侧边文字，
    // 且三个输入框物理上同列 → 等宽（各自 addWidget 直排时宽度会随内容不一）
    QGridLayout* grid = new QGridLayout();
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(18);   // 标签与输入框之间的间距
    grid->setVerticalSpacing(10);     // 各行之间的行距（含强度条行；收紧 2px 给固定高度腾空间）
    grid->setColumnStretch(0, 0);     // 标签列：按自身建议宽度，不拉伸
    grid->setColumnStretch(1, 1);     // 输入框列：吃掉全部多余宽度
    grid->setColumnStretch(2, 0);     // 预留列：不拉伸（与忘记密码页三列式保持同构）

    // 侧边标签统一样式（三行共用）
    const QString labelStyle = "QLabel { color: #666666; font-size: 14px; }";

    // ===== 账号行 =====
    // 账号由服务器下发（见 needAccountFromServer），不允许用户自定义 → setEnabled(false)，
    // 并配一套"灰底浅字"的禁用态样式，视觉上直接表明这栏不可输入（与忘记密码页账号行同一套）
    QLabel* accountLabel = new QLabel("账号:", card);
    accountLabel->setStyleSheet(labelStyle);
    accountLabel->setFixedWidth(80);                                // 标签统一等宽
    accountLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);  // 固定宽度内右对齐
    grid->addWidget(accountLabel, 0, 0);

    QFrame* accountBox = new QFrame(card);
    accountBox->setObjectName("accountBox");
    accountBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    accountBox->setStyleSheet(R"(
            #accountBox {
                border: 1px solid rgba(0, 0, 0, 0.08);
                border-radius: 8px;
                background-color: rgba(255, 255, 255, 0.72);
            })");
    QHBoxLayout* accountRow = new QHBoxLayout(accountBox);
    accountRow->setContentsMargins(1, 1, 1, 1);
    accountRow->setSpacing(0);

    regAccountEdit = new QLineEdit(accountBox);
    regAccountEdit->setPlaceholderText("账号由服务器分配");
    regAccountEdit->setFixedHeight(42);
    // 整行外观统一由容器 accountBox 负责画。原来自带圆角白底时，右侧留给指示器的
    // 那一小条没有输入框叠加，白底深浅与输入框区域对不上，看上去像账号框右边
    // 另贴了一个独立的白圆圈——指示器"跟账号框分开"的观感就来自这里
    regAccountEdit->setStyleSheet(R"(
        QLineEdit {
            border: none;
            padding-left: 15px;
            font-size: 14px;
            background-color: rgba(255, 255, 255, 0.45);
            color: #999999;
        }
    )");

    m_accountStatusIndicator = new MessageStatusIndicator(accountBox);
    // 账号行这块指示器属于"取号连接"阶段：失败/超时点亮它的红感叹号的是
    // onConnectForRegisterFailed/Timeout，重试也按 ConnectForRegister 枚举路由
    m_accountStatusIndicator->setFeature(LoginFeature::ConnectForRegister);
    connect(m_accountStatusIndicator, &MessageStatusIndicator::retryClicked, this, [this](LoginFeature feature) {
        m_backend->onRetryRequested(feature, regAccountEdit->text());
    });
    regAccountEdit->setEnabled(false);   // 不可输入：账号只能由服务端下发

    accountRow->addWidget(regAccountEdit);
    accountRow->addWidget(m_accountStatusIndicator);

    grid->addWidget(accountBox, 0, 1);
    

    // ===== 密码行 =====
    QLabel* passwordLabel = new QLabel("密码:", card);
    passwordLabel->setStyleSheet(labelStyle);
    passwordLabel->setFixedWidth(80);
    passwordLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    grid->addWidget(passwordLabel, 1, 0);

    regPasswordEdit = new QLineEdit(card);
    regPasswordEdit->setPlaceholderText("请输入密码");
    regPasswordEdit->setFixedHeight(40);   // 40 + 容器上下各 1px 边框 = 42，与账号行等高
    regPasswordEdit->setMaxLength(15);     // 密码最多15位（与忘记密码页、强度分级上限一致）
    regPasswordEdit->setEchoMode(QLineEdit::Password);
    // 输入框本身不带边框：外框统一由容器 pwdBox 画。
    // 否则"输入框右边框 + 紧贴的眼睛按钮"之间会留一条拼缝线（忘记密码页踩过的坑）
    regPasswordEdit->setStyleSheet(R"(
        QLineEdit {
            border: none;
            background: transparent;
            padding-left: 15px;
            font-size: 14px;
            color: #333333;
        }
    )");

    // 眼睛按钮：切换明文/密文（逻辑整份照搬忘记密码页）；空密码时隐藏
    regPwdToggleBtn = new QPushButton(card);
    regPwdToggleBtn->setFixedSize(40, 40);
    regPwdToggleBtn->setFocusPolicy(Qt::NoFocus);    // 不参与键盘焦点，避免抢走输入框焦点
    regPwdToggleBtn->setStyleSheet(R"(
        QPushButton {
            border: none;
            background: transparent;
            padding: 0;
            border-radius: 0 7px 7px 0;   /* 贴合容器内径 8-1 的右角 */
        }
        QPushButton:hover {
            background-color: #f5f5f5;
        }
    )");
    regPwdToggleBtn->setIcon(QIcon(":/res/icon/eyes_show.svg"));
    regPwdToggleBtn->setVisible(false);

    // "输入组"容器：一只 QFrame 统一画边框和底色，输入框 + 眼睛按钮都嵌在里面
    // 聚焦时描边变蓝的切换在下方信号区（QSS 没有 :focus-within，得用焦点信号补）
    QFrame* pwdBox = new QFrame(card);
    pwdBox->setObjectName("pwdBox");   // 选择器必须点名 #pwdBox：QLabel 也是 QFrame 子类，写 QFrame 会误伤
    pwdBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto pwdBoxStyle = [](bool focused) {
        return QString(R"(
            #pwdBox {
                border: 1px solid %1;
                border-radius: 8px;
                background-color: rgba(255, 255, 255, 0.72);
            }
        )").arg(focused ? "#4a90d9" : "rgba(0, 0, 0, 0.08)");
    };
    pwdBox->setStyleSheet(pwdBoxStyle(false));

    QHBoxLayout* pwdRow = new QHBoxLayout(pwdBox);
    pwdRow->setContentsMargins(1, 1, 1, 1);   // 让出容器 1px 边框的宽度
    pwdRow->setSpacing(0);                    // 输入框↔按钮 0 间距（拼缝线的根源就在这）
    pwdRow->addWidget(regPasswordEdit);
    pwdRow->addWidget(regPwdToggleBtn);
    grid->addWidget(pwdBox, 1, 1);

    // ===== 强度条（整份照搬忘记密码页）=====
    // 第一行 = 四杠 + 等级文字（居中），第二行 = 两行提示文案（居中）
    m_strengthWidget = new QWidget(card);
    QVBoxLayout* strengthCol = new QVBoxLayout(m_strengthWidget);
    strengthCol->setContentsMargins(0, 0, 0, 0);
    strengthCol->setSpacing(4);

    QHBoxLayout* barsRow = new QHBoxLayout();
    barsRow->setSpacing(6);
    barsRow->addStretch();                       // 左侧弹性（与右侧对称 → 杠组整体居中）
    for (int i = 0; i < 4; ++i) {
        QFrame* bar = new QFrame(m_strengthWidget);
        bar->setFixedSize(24, 4);                // 固定宽24，细条紧凑
        m_strengthBars[i] = bar;
        bar->setStyleSheet("QFrame { background: #dfe6f0; border-radius: 2px; border: none; }");
        barsRow->addWidget(bar);
    }
    m_strengthLabel = new QLabel(m_strengthWidget);   // 等级文字，默认空（未输入）
    m_strengthLabel->setStyleSheet("color: #9aa4b5; font-size: 12px;");
    barsRow->addWidget(m_strengthLabel);
    barsRow->addStretch();                       // 右侧弹性
    strengthCol->addLayout(barsRow);

    // 提示文案：逗号后换行成两行、水平居中
    m_symbolHint = new QLabel("密码由大小写字母、数字和符号组成，\n符号仅支持 ! ? - .", m_strengthWidget);
    m_symbolHint->setStyleSheet("color: #9aa4b5; font-size: 10px;");
    m_symbolHint->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    strengthCol->addWidget(m_symbolHint);

    m_strengthWidget->setVisible(false);         // 初始隐藏：没输密码时不打扰
    grid->addWidget(m_strengthWidget, 2, 0, 1, 2);   // 跨两列，坐在密码行正下方

    // ===== 确认密码行 =====
    QLabel* confirmLabel = new QLabel("确认密码:", card);
    confirmLabel->setStyleSheet(labelStyle);
    confirmLabel->setFixedWidth(80);
    confirmLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    grid->addWidget(confirmLabel, 3, 0);

    regConfirmEdit = new QLineEdit(card);
    regConfirmEdit->setPlaceholderText("请再次输入密码");
    regConfirmEdit->setFixedHeight(40);   // 同密码行：40 + 容器 2px = 42
    regConfirmEdit->setMaxLength(15);     // 与"密码"一致，最多15位
    regConfirmEdit->setEchoMode(QLineEdit::Password);
    regConfirmEdit->setStyleSheet(R"(
        QLineEdit {
            border: none;
            background: transparent;
            padding-left: 15px;
            font-size: 14px;
            color: #333333;
        }
    )");

    // 确认框的眼睛按钮：与密码行同一套（容器 confirmBox 也同一套方案）
    regConfirmToggleBtn = new QPushButton(card);
    regConfirmToggleBtn->setFixedSize(40, 40);
    regConfirmToggleBtn->setFocusPolicy(Qt::NoFocus);
    regConfirmToggleBtn->setStyleSheet(R"(
        QPushButton {
            border: none;
            background: transparent;
            padding: 0;
            border-radius: 0 7px 7px 0;
        }
        QPushButton:hover {
            background-color: #f5f5f5;
        }
    )");
    regConfirmToggleBtn->setIcon(QIcon(":/res/icon/eyes_show.svg"));
    regConfirmToggleBtn->setVisible(false);

    QFrame* confirmBox = new QFrame(card);
    confirmBox->setObjectName("confirmBox");
    confirmBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto confirmBoxStyle = [](bool focused) {
        return QString(R"(
            #confirmBox {
                border: 1px solid %1;
                border-radius: 8px;
                background-color: rgba(255, 255, 255, 0.72);
            }
        )").arg(focused ? "#4a90d9" : "rgba(0, 0, 0, 0.08)");
    };
    confirmBox->setStyleSheet(confirmBoxStyle(false));

    QHBoxLayout* confirmRow = new QHBoxLayout(confirmBox);
    confirmRow->setContentsMargins(1, 1, 1, 1);   // 让出容器 1px 边框的宽度
    confirmRow->setSpacing(0);
    confirmRow->addWidget(regConfirmEdit);
    confirmRow->addWidget(regConfirmToggleBtn);
    // 注意：这里不加 addStretch——QLineEdit 和弹性垫片都是 Expanding，
    // 会平分多余宽度把输入框挤窄（右侧留空），与 pwdRow 保持一致让输入框占满
    grid->addWidget(confirmBox, 3, 1);

    // 网格整体进卡片，再挂注册按钮
    cardLayout->addLayout(grid);

    // 注册按钮
    regSubmitBtn = new QPushButton("注册", card);
    regSubmitBtn->setFixedHeight(42);
    regSubmitBtn->setStyleSheet(R"(
        QPushButton {
            border-radius: 8px;
            background-color: #7f91a3ff;
            color: white;
            font-size: 16px;
            font-weight: bold;
            border: none;
        }
        QPushButton:hover {
            background-color: #3a80c9;
        }
    )");
    cardLayout->addWidget(regSubmitBtn);

    // 先把按钮原文案记下来兜底：正常路径由 onRegisterWaiting 覆盖（内容同样是"注册"），
    // 但 onRegisterFailed/NetworkError/Timeout 三条收尾都拿它还原文案——万一某条错误路径
    // 没经过 onRegisterWaiting，空串会把按钮文字抹成一片空白（按钮只剩个蓝底，看不出是"注册"）
    m_originalSubmitText = regSubmitBtn->text();

    m_registerMismatchHint = new QLabel(this);
    m_registerMismatchHint->setStyleSheet("QLabel { color: rgba(224, 91, 91, 0.6); font-size: 12px; font-weight: bold; }");
    m_registerMismatchHint->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    m_registerMismatchHint->setFixedHeight(16);
    layout->addWidget(m_registerMismatchHint);

    m_registerMismatchAnim = new QPropertyAnimation(regSubmitBtn, "pos", this);
    m_registerMismatchAnim->setDuration(350);

    // 返回登录按钮（透明文字链接，点击通知容器切回登录页）
    backToLoginBtn = new QPushButton("返回登录", this);
    backToLoginBtn->setStyleSheet(R"(
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
    backToLoginBtn->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(backToLoginBtn);
    layout->addStretch(3);

    // ========== 信号连接统一放这里 ==========
    // 全部控件创建完毕后再集中 connect，不插在控件创建中间，便于查找和维护。
    // 分块顺序与忘记密码页保持一致：页面按钮 → 密码框（眼睛）→ 焦点，同一个信号只连一条。
    // 注册按钮：页内校验 + 发注册请求都收进 onSubmitClicked
    connect(regSubmitBtn, &QPushButton::clicked, this, &RegisterPage::onSubmitClicked);
    connect(backToLoginBtn, &QPushButton::clicked, this, &RegisterPage::backToLoginRequested);

    connect(m_registerMismatchAnim, &QPropertyAnimation::finished, this, [this](){
        // 抖动结束不写死"恢复成可点"，而是按当前真实输入重算：
        // 错误反馈可能在"按钮本来就该禁用"的状态下被触发（例如密码不足 kMinPasswordLength 位
        // 或清空输入后又被校验拦下），这时写死 true 会把禁用按钮画成亮蓝可点态却点不动
        refreshSubmitButtonState();
    });

    // 两个"眼睛"按钮 → 各自切换本行密码框的明文/密文：复用同一份 togglePasswordVisibility，
    // 闭包各绑各的控件（比 sender() 映射更直观、编译期可查）
    connect(regPwdToggleBtn, &QPushButton::clicked, this,
            [this] { togglePasswordVisibility(regPasswordEdit, regPwdToggleBtn); });
    connect(regConfirmToggleBtn, &QPushButton::clicked, this,
            [this] { togglePasswordVisibility(regConfirmEdit, regConfirmToggleBtn); });

    // 两个密码框的 textChanged 各只连一条：原本同一个信号连了两处（一处重算强度、
    // 一处同步眼睛按钮显隐），读起来像两套逻辑、其实是同一件事，合并进一条 lambda
    auto bindPasswordEdit = [this](QLineEdit* edit, QPushButton* toggleBtn, bool needStrength) 
    {
        connect(edit, &QLineEdit::textChanged, this,
                [this, edit, toggleBtn, needStrength](const QString& text) {
            if (needStrength) {
                updatePasswordStrength(text);   // 仅密码行：刷新强度条（空密码时内部整块隐藏）
            }
            togglePasswordBtnVisibility(edit, toggleBtn);   // 空密码时隐藏眼睛按钮
            // 可提交条件与 setInputsEnabled / 抖动动画收尾共用 isSubmitReady()，
            // 改规则（如位数下限）只改那一处
            refreshSubmitButtonState();
        });
    };
    bindPasswordEdit(regPasswordEdit, regPwdToggleBtn, true);
    bindPasswordEdit(regConfirmEdit, regConfirmToggleBtn, false);

    // 焦点进入/离开密码框 → 对应容器描边在灰/蓝之间切换
    //（QSS 没有 :focus-within，只能用全局焦点信号补，与忘记密码页同一方案）
    connect(qApp, &QApplication::focusChanged, this, [=](QWidget*, QWidget* now) {
        pwdBox->setStyleSheet(pwdBoxStyle(now == regPasswordEdit));
        confirmBox->setStyleSheet(confirmBoxStyle(now == regConfirmEdit));
    });
}

// 工具函数：切换密码框的明文/密文显示（两个眼睛按钮复用这一份逻辑，照搬忘记密码页）
void RegisterPage::togglePasswordVisibility(QLineEdit* edit, QPushButton* toggleBtn)
{
    if (edit->echoMode() == QLineEdit::Password) {   // 当前是隐藏态
        edit->setEchoMode(QLineEdit::Normal);                    // 切换成明文
        toggleBtn->setIcon(QIcon(":/res/icon/eyes_clicked.svg"));  // "睁眼"图标 = 明文可见
    } else {                                        // 当前是明文态
        edit->setEchoMode(QLineEdit::Password);                  // 切换回隐藏
        toggleBtn->setIcon(QIcon(":/res/icon/eyes_show.svg"));     // "闭眼"图标 = 密文隐藏
    }
}

// 工具函数：按输入内容控制眼睛按钮显隐（空密码时隐藏——没内容时切换明文/密文没意义）
void RegisterPage::togglePasswordBtnVisibility(QLineEdit* edit, QPushButton* toggleBtn)
{
    toggleBtn->setVisible(!edit->text().isEmpty());
}

// 真正"进入注册页"时调用（LoginWindow 切到本页时显式调，见 LoginWindow.cpp）：
// 先把上次留下的输入状态清回初始态，再通知后端建 TCP 连接取号。
// 账号框 regAccountEdit 不在复位范围：它是只读的"服务端下发"展示框，每次进页由
// connectForRegisterSuccess 重新填，清掉反而会在新号到达前露出一段空白
//
// 为什么不做成 showEvent：窗口从最小化还原、重新获得焦点时 Qt 也会给本页补发一次
// showEvent，可那时用户并没有离开过本页（也没点"返回登录"重进）。挂在那儿会连累两件事：
//   ① 把用户已经敲进去的密码清掉；
//   ② 白发一次 needAccountFromServer，取号流程在 TCP 上重来一遍。
// 第二次取号会搅乱连接状态（旧连接被 abort → 断线重连），随之而来的 TCP 错误按当时的
// 功能标记（取号成功后已切到 Register）被当成"注册提交阶段连不上服务器"，
// 于是注册页在没有任何提交动作的情况下弹出红字，而那行红字收尾时会把按钮文案还原成
// m_originalSubmitText——没提交过就是空串，按钮就变成"一片蓝色、没有字"的死按钮
void RegisterPage::enterPage()
{
    // 密码/确认框清空：本来有内容时 clear() 会发 textChanged，
    // 强度条、眼睛按钮、提交按钮会顺着信号自动复位
    regPasswordEdit->clear();
    regConfirmEdit->clear();

    // 上面两下只在"本来有内容"时才发信号，本来就是空的不发；
    // 所以派生出来的状态再显式复位一遍，保证每次进页都是同一副初始模样
    regPasswordEdit->setEchoMode(QLineEdit::Password);   // 上次点过眼睛要收回密文态
    regConfirmEdit->setEchoMode(QLineEdit::Password);
    regPwdToggleBtn->setIcon(QIcon(":/res/icon/eyes_show.svg"));
    regConfirmToggleBtn->setIcon(QIcon(":/res/icon/eyes_show.svg"));
    regPwdToggleBtn->setVisible(false);                   // 空密码不显示眼睛按钮
    regConfirmToggleBtn->setVisible(false);

    m_hasIllegal = false;                                 // 非法字符标记清零（否则会拦住下次提交）
    m_strengthWidget->setVisible(false);                  // 强度条整块收起
    m_strengthLabel->clear();
    m_symbolHint->setText("密码由大小写字母、数字和符号组成，\n符号仅支持 ! ? - .");
    m_symbolHint->setStyleSheet("color: #9aa4b5; font-size: 10px;");

    // 提交按钮回到初始态：停掉上次可能没停的加点动画，文案与禁用态配色复原
    m_registerAnimTimer->stop();
    m_dostCount = 0;
    regSubmitBtn->setText(tr("注册"));
    regSubmitBtn->setEnabled(false);
    regSubmitBtn->setStyleSheet(R"(
        QPushButton {
            border-radius: 8px;
            background-color: #7f91a3ff;
            color: white;
            font-size: 15px;
            font-weight: bold;
            border: none;
        }
    )");

    emit needAccountFromServer();
}

// 密码强度检测：整份从忘记密码页搬过来，分级规则与提示文案完全一致。
// 分级（区分大小写，符号仅允许 ! ? - . ）：
//   0 级：未输入 → 四杠全灭
//   1 弱(红/1杠)：数字 / 小写 / 大写 / 符号 里只命中一种
//   2 中(黄/2杠)：命中任意两种
//   3 强(绿/3杠)：命中任意三种
//   4 极强(蓝/4杠)：四种全中（数字+小写+大写+符号）
void RegisterPage::updatePasswordStrength(const QString& pwd)
{
    // 空密码：隐藏整块强度条（输入密码之后才开始显示）
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

    // 把"命中了哪些种类"压成 4 位掩码：bit0 数字 / bit1 小写 / bit2 大写 / bit3 符号
    int mask = 0;
    if (hasDigit) mask |= 0x1;
    if (hasLower) mask |= 0x2;
    if (hasUpper) mask |= 0x4;
    if (hasSymbol) mask |= 0x8;

    // 按掩码穷举分级：15 种非空组合全列出来，强度 = 命中的种类数
    int level = 0;
    QString color, text;
    switch (mask) {
    // —— 命中 1 种：弱 ——
    case 0x1: case 0x2: case 0x4: case 0x8:
        level = 1; color = "#e05b5b"; text = "弱";    // 红
        break;
    // —— 命中 2 种：中 ——
    case 0x1|0x2: case 0x1|0x4: case 0x1|0x8:
    case 0x2|0x4: case 0x2|0x8: case 0x4|0x8:
        level = 2; color = "#f0b429"; text = "中";    // 黄
        break;
    // —— 命中 3 种：强 ——
    case 0x1|0x2|0x4: case 0x1|0x2|0x8:
    case 0x1|0x4|0x8: case 0x2|0x4|0x8:
        level = 3; color = "#43a047"; text = "强";    // 绿
        break;
    // —— 4 种全中：极强 ——
    case 0x1|0x2|0x4|0x8:
        level = 4; color = "#4a90d9"; text = "极强";  // 蓝
        break;
    default:
        // mask==0：密码非空但一个合法字符都没有（全是非法字符），按最弱红字提示
        level = 1; color = "#e05b5b"; text = "弱";
        break;
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

    // 合法符号提示：出现非法字符时变红提醒，否则恢复灰色
    if (m_hasIllegal) {
        m_symbolHint->setText("含不允许的字符，\n符号仅支持 ! ? - .");
        m_symbolHint->setStyleSheet("color: #e05b5b; font-size: 10px;");
    } else {
        m_symbolHint->setText("密码由大小写字母、数字和符号组成，\n符号仅支持 ! ? - .");
        m_symbolHint->setStyleSheet("color: #9aa4b5; font-size: 10px;");
    }
}

// 注册等待期间统一开关本页交互
// 禁用阶段：密码框、确认框、两个眼睛按钮、提交按钮、返回按钮全锁死，
//          防止等待中重复提交或偷改输入
// 恢复阶段：把"常驻可用"的控件开回来，并按当前输入重算提交按钮状态，
//          否则解锁后按钮会一直停在禁用态
// 注意：regAccountEdit 不参与开关——它在 setupUI 里本来就 setEnabled(false)，
//      是"只读展示框"（账号由服务器取号下发，不允许手动改），
//      一旦在这里恢复成 true 就把这个设定破坏了
void RegisterPage::setInputsEnabled(bool enabled)
{
    // 输入框 + 眼睛按钮
    regPasswordEdit->setEnabled(enabled);
    regConfirmEdit->setEnabled(enabled);
    regPwdToggleBtn->setEnabled(enabled);
    regConfirmToggleBtn->setEnabled(enabled);

    // 返回登录按钮
    backToLoginBtn->setEnabled(enabled);

    if (enabled) {
        // 恢复时 setEnabled 与样式必须成对更新：updateSubmitButtonState 只切样式，
        // 不调它的话等待前 setEnabled(false) 的禁用状态会残留——按钮画成蓝色可点态
        // 却点不动（与 bindPasswordEdit 里那份是同一个道理）
        refreshSubmitButtonState();
    } else {
        regSubmitBtn->setEnabled(false);
    }
}

// 当前是否满足"可提交"条件：两框都非空 + 无非法字符 + 密码满 kMinPasswordLength 位。
// 注意这里不管账号框——账号由服务端取号下发，取号失败时点按钮由 onSubmitClicked 单独提示
bool RegisterPage::isSubmitReady() const
{
    return !regPasswordEdit->text().isEmpty()
           && !regConfirmEdit->text().isEmpty()
           && !m_hasIllegal
           && regPasswordEdit->text().length() >= kMinPasswordLength;
}

// 按真实可用性同步提交按钮：setEnabled 与样式成对更新。
// 只调 updateSubmitButtonState 不调 setEnabled 会留下"画着可点态却点不动"的按钮；
// 只调 setEnabled 不调 updateSubmitButtonState 则会留下与状态不符的配色
void RegisterPage::refreshSubmitButtonState()
{
    const bool ready = isSubmitReady();
    regSubmitBtn->setEnabled(ready);
    // 禁用态配色与忘记密码页提交按钮保持一致（灰蓝底白字）
    updateSubmitButtonState(ready);
}

// 槽函数：点击"注册"
// 参照 ForgotPasswordPage::onSubmitClicked：页内校验失败走统一错误反馈，
// 全部通过才 emit registerAquiard(账号, 密码) 交给 MainBackend 走 TCP
void RegisterPage::onSubmitClicked()
{
    // 账号还没取到（取号失败/超时后直接点注册）：先提示，不发请求
    if (regAccountEdit->text().isEmpty()) {
        triggerErrorFeedback("账号尚未获取，请等待取号成功后再试");
        return;
    }
    // 密码里出现了合法集之外的字符（强度提示行已经标红提醒过了）
    if (m_hasIllegal) {
        triggerErrorFeedback("密码含有不支持的符号，符号仅支持 ! ? - .");
        return;
    }
    // 两次密码不一致：统一走错误反馈（红字 + 红色按钮 + 抖动动画）
    if (regPasswordEdit->text() != regConfirmEdit->text()) {
        triggerErrorFeedback("两次输入的密码不一致");
        return;
    }
    // 校验通过：账号（服务器下发）+ 密码交给主后端发注册请求
    emit registerAquiard(regAccountEdit->text(), regPasswordEdit->text());
}

// 工具函数：错误反馈（红字提示 + 按钮红色错误态样式 + 左右抖动动画）
// 整份照搬 ForgotPasswordPage::triggerErrorFeedback，只有控件名不同；
// 动画结束时由 finished → updateSubmitButtonState 按当前输入状态复位按钮样式
void RegisterPage::triggerErrorFeedback(const QString& hint)
{
    // 错误提示行统一恢复红色样式：成功分支会把它改成绿色，
    // 这里不还原的话，后面的错误会以绿字显示
    m_registerMismatchHint->setStyleSheet(
        "QLabel { color: rgba(224, 91, 91, 0.6); font-size: 12px; font-weight: bold; }");

    // 按钮下一行显示红字（常驻占位行，不会引起布局跳动）
    m_registerMismatchHint->setText(hint);

    // 临时错误态样式：按钮整体染成红色系（淡红内里 + 红字 + 红框）；
    // 动画结束由 finished → updateSubmitButtonState 恢复正常样式
    regSubmitBtn->setStyleSheet(R"(
        QPushButton {
            border-radius: 8px;
            background-color: rgba(224, 91, 91, 0.14);   /* 内里染淡红 */
            color: rgba(224, 91, 91, 0.85);              /* "注册"文字跟着变红 */
            font-size: 16px;
            font-weight: bold;
            border: 2px solid rgba(224, 91, 91, 0.45);   /* 红框同步调柔 */
        }
        QPushButton:hover  { background-color: rgba(224, 91, 91, 0.20); }
        QPushButton:pressed { background-color: rgba(224, 91, 91, 0.26); }
    )");

    // 左右抖动：以基准位为中心 ±8/±5px 递减摆动，结束时回到基准位
    m_registerMismatchAnim->stop();   // 上一次还在抖就先停，防止连点叠加
    // 基准位置只在第一次出错时捕获，之后锁死不更新：
    // 若每次都拿当前 pos() 当基准，连点时会停在动画中途的偏移坐标上，
    // 误把它当新基准 → 按钮越抖越偏（连点漂移）
    if (!m_btnRestValid) {
        m_btnRestPos = regSubmitBtn->pos();
        m_btnRestValid = true;
    }
    const QPoint base = m_btnRestPos;
    m_registerMismatchAnim->setStartValue(base);
    m_registerMismatchAnim->setKeyValueAt(0.15, base + QPoint(-8, 0));  // 时间15%：向左偏移8px
    m_registerMismatchAnim->setKeyValueAt(0.35, base + QPoint( 8, 0));  // 时间35%：向右偏移8px
    m_registerMismatchAnim->setKeyValueAt(0.55, base + QPoint(-5, 0));  // 时间55%：向左偏移5px
    m_registerMismatchAnim->setKeyValueAt(0.75, base + QPoint( 5, 0));  // 时间75%：向右偏移5px
    m_registerMismatchAnim->setEndValue(base);                          // 时间100%：回到基准位
    m_registerMismatchAnim->start();
}

// 连接/等待中：锁页面 + "正在注册..."加点动画（照搬 ForgotPasswordPage::onModifyPwdWaiting）
void RegisterPage::onRegisterWaiting()
{
    m_originalSubmitText = regSubmitBtn->text();   // 记住原文案（"注册"），等待结束后恢复
    setInputsEnabled(false);                       // 等待中锁页面，防重复提交
    m_dostCount = 0;                               // 点数从零开始数
    regSubmitBtn->setText(tr("正在注册"));
    regSubmitBtn->setStyleSheet(R"(
        QPushButton {
            border-radius: 8px;
            background-color: #999999;
            color: white;
            font-size: 16px;
            font-weight: bold;
            border: none;
        }
    )");
    m_registerAnimTimer->start(500);               // 500ms 一格，timeout 连着下面的加点动画
}

// 注册成功：停等待动画 → 解锁 → 绿色提示 + 清空输入（照搬 ForgotPasswordPage::onModifyPwdSuccess）
void RegisterPage::onRegisterSuccess()
{
    m_registerAnimTimer->stop();                  // 停掉"正在注册..."的加点动画
    setInputsEnabled(true);                       // 解锁页面（内部会重算提交按钮状态）
    regSubmitBtn->setText(m_originalSubmitText);  // 按钮文案恢复成"注册"

    // 注册已成功，密码留在框里没意义也不安全，直接清空。
    // clear() 会触发 textChanged → 强度条自动收起、眼睛按钮自动隐藏、提交按钮自动置灰
    regPasswordEdit->clear();
    regConfirmEdit->clear();

    // 成功提示走 m_registerMismatchHint 这一行（常驻占位不跳动），改为绿色与错误红字区分
    m_registerMismatchHint->setStyleSheet(
        "QLabel { color: rgba(67, 160, 71, 0.9); font-size: 12px; font-weight: bold; }");
    m_registerMismatchHint->setText("注册成功，请返回登录使用新账号");
}

// 仅"服务器明确拒绝"（如账号已存在）：停等待动画 → 复位按钮 → 复用统一错误反馈（红字 + 抖动）
void RegisterPage::onRegisterFailed()
{
    m_registerAnimTimer->stop();
    setInputsEnabled(true);
    regSubmitBtn->setText(m_originalSubmitText);

    // 错误反馈不额外调 updateSubmitButtonState，让按钮的红色错误态保留到抖动结束——
    // 动画 finished 信号会自己把它复位成正常样式，这里紧跟一次复位会把红框瞬间覆盖掉
    triggerErrorFeedback("注册失败，该账号已被注册");
}

// 网络层连不上（服务器没跑/断网）：收尾动作与失败分支相同，
// 只有提示语必须区分开——否则用户会以为密码有问题，对着正确的输入反复重试
void RegisterPage::onRegisterNetworkError()
{
    m_registerAnimTimer->stop();
    setInputsEnabled(true);
    regSubmitBtn->setText(m_originalSubmitText);

    triggerErrorFeedback("无法连接服务器，请检查网络或稍后再试");
}

// 连接超时：与网络错误同一处理，提示语区分开
void RegisterPage::onRegisterTimeout()
{
    m_registerAnimTimer->stop();
    setInputsEnabled(true);
    regSubmitBtn->setText(m_originalSubmitText);

    triggerErrorFeedback("注册连接超时，请检查网络后重试");
}

// 取号连接中：账号行指示器转圈（骨架，逻辑待补）
void RegisterPage::onConnectForRegisterWaiting()
{
    m_accountStatusIndicator->setStatus(MessageStatusIndicator::Status::Sending);
}

// 取号连接成功：服务器下发的账号 ID 由参数进来，填进账号框 + 指示器切成功（骨架，逻辑待补）
void RegisterPage::onConnectForRegisterSuccess(const QString& account)
{
    m_accountStatusIndicator->setStatus(MessageStatusIndicator::Status::None);
    regAccountEdit->setText(account);
}

// 取号连接失败（网络层连不上）：指示器变红感叹号，点击重试（骨架，逻辑待补）
void RegisterPage::onConnectForRegisterFailed()
{
    m_accountStatusIndicator->setStatus(MessageStatusIndicator::Status::Failed);
}

// 取号连接超时：与失败同一处理（骨架，逻辑待补）
void RegisterPage::onConnectForRegisterTimeout()
{
    m_accountStatusIndicator->setStatus(MessageStatusIndicator::Status::Failed);
}

// 槽函数：刷新提交按钮动画（"正在注册"后面的点，500ms 多一个点、循环到 3 个）
// 与忘记密码页同一套路，区别只有两点：文案换成"正在注册"；
// setText 挪到循环外——原版在 for 里每圈都 setText，实际显示的点数永远比计数少 1
void RegisterPage::updateSubmitButtonAnimation()
{
    m_dostCount = (m_dostCount + 1) % 4;
    QString text = tr("正在注册");
    for (int i = 0; i < m_dostCount; ++i) {
        text += ".";
    }
    regSubmitBtn->setText(text);
}

void RegisterPage::updateSubmitButtonState(bool enabled)
{
    if(enabled)
    {
    regSubmitBtn->setStyleSheet(R"(
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
        )");
    }
    else{
    regSubmitBtn->setStyleSheet(R"(
                    QPushButton {
                        border-radius: 8px;
                        background-color: #7f91a3ff;
                        color: white;
                        font-size: 15px;
                        font-weight: bold;
                        border: none;
                    }
                )");
    }
}