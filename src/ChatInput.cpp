#include "ChatInput.h"
#include <QFontMetrics>
#include <QtMath>
#include <QKeyEvent>
#include <QInputMethodEvent>

ChatInput::ChatInput(QWidget *parent) : QWidget(parent)
{
    // 外层只放一个 FrameBox：功能按钮行 + 输入框 + 发送按钮行 三块全在框里，
    // 输入框长高时是这个框整体向上长（顶边抬升、底边贴死窗口底），不是各块各自挤
    // （"向上长"的机关不在本文件，在父布局 ChatArea::mainLayout：scrollArea stretch=1 在前、
    //   chatInput stretch=0 在最后 → 底边锚死窗口底不动，详见 ChatArea.cpp）
    QVBoxLayout* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(15, 0, 15, 15);   // 左右和底部留边，顶部贴着聊天记录区
    outerLayout->setSpacing(0);

    boxFrame = new QFrame(this);
    boxFrame->setObjectName("inputBox");
    boxFrame->setStyleSheet(R"(
        QFrame#inputBox {
            background-color: white;
            border: 1px solid #e0e0e0;
            border-radius: 12px;
        }
    )");

    // 框内用网格布局（自动布局嵌套多了容易失控，网格的三行一列是死的，不会炸）：
    //   0 行：功能按钮行（附件/历史消息查找等），固定高
    //   1 行：输入框，唯一伸缩行（rowStretch=1），高度由 adjustHeight 按内容调
    //   2 行：发送按钮行，固定高
    layout = new QGridLayout(boxFrame);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setHorizontalSpacing(8);
    layout->setVerticalSpacing(6);

    // ===== 0 行：功能按钮行（固定高，不参与伸缩）=====
    fileButton = new QPushButton("📎", this);
    fileButton->setFixedSize(32, 32);
    fileButton->setStyleSheet(R"(
        QPushButton {
            border: none;
            border-radius: 50%;
            background-color: #f0f0f0;
            font-size: 16px;
        }
        QPushButton:hover {
            background-color: #e0e0e0;
        }
    )");
    layout->addWidget(fileButton, 0, 0, 1, 1, Qt::AlignLeft | Qt::AlignVCenter);
    layout->setRowStretch(0, 0);   // 固定行：给多少空间都不长

    // ===== 1 行：输入框（唯一伸缩行）=====
    inputEdit = new QTextEdit(this);
    inputEdit->setPlaceholderText("输入消息...");
    // 字号在这里定死一处，不写进 QSS：QSS 的 font 要等 polish 才落到 widget 上，
    // 而 adjustHeight() 要拿"排版用的那个字体"去量行距，两边必须完全同源，
    // 一处写 QSS 一处写代码，迟早改一处漏一处
    QFont inputFont = inputEdit->font();
    inputFont.setPixelSize(14);
    inputEdit->setFont(inputFont);
    inputEdit->setStyleSheet(R"(
        QTextEdit {
            /* 下/左/右不要边框，只留上边当"功能按钮行 ↔ 输入区"的分隔线。
               按 note.txt 的坑：QTextEdit 底层也是 QFrame，直接写 border: none 可能不生效，
               要显式写成 0px solid transparent 才能真去掉 */
            border: 0px solid transparent;
            border-top: 1px solid #e0e0e0;
            border-radius: 0;   /* 只画单边时圆角会画歪（缺竖边），必须归零 */
            /* 左右不留 padding，只留上下 6px 的文字呼吸位。
               横向 padding 会让"文档用来排版的宽度"和"控件实际可视宽度"差出一个 padding 的量，
               结果就是长行文字右侧被切掉（最后几个字藏在滚动条底下）。
               左右留白交给 boxFrame 自己的 10px 边距——顺带正好和 📎 按钮左对齐 */
            padding: 6px 0px;
            background-color: white;
        }
        QTextEdit:focus {
            border-top-color: #4a90d9;
            outline: none;
        }
        QTextEdit QScrollBar:vertical {
            width: 6px;
            background-color: transparent;
            border: none;
            margin: 0px;   /* 上下箭头按钮去掉后，不留它们占的空位 */
        }
        /* 上下那两个小箭头按钮（sub-line 在上、add-line 在下）：去掉边框和背景，
           高度设 0 等于把按钮整块移除，滑块就能一直贴到两头 */
        QTextEdit QScrollBar::sub-line:vertical,
        QTextEdit QScrollBar::add-line:vertical {
            height: 0px;
            border: none;
            background: transparent;
        }
        /* 滑块的上下"空白页"（sub-page 上、add-page 下）：保持透明，不遮背景 */
        QTextEdit QScrollBar::sub-page:vertical,
        QTextEdit QScrollBar::add-page:vertical {
            background: transparent;
        }
        /* 滑块平时完全透明（等于看不见），只有鼠标进入输入框区域时才显形 —— 隐藏式滚动条。
           关键：滑块"看不见"不等于"滚不动"。滚动条控件本身一直都在（仍旧占着 6px 宽，
           文档排版宽度不会因此变化），滚轮/键盘滚动走的是 QTextEdit 自己的滚动逻辑，
           跟滑块画成什么颜色毫无关系，所以不显示时滚轮照样能滚 */
        QTextEdit QScrollBar::handle:vertical {
            background-color: transparent;
            border-radius: 3px;                      /* 条宽 6px → 半径 3px，上下正好是圆头 */
            min-height: 24px;
        }
        /* 鼠标移进输入框时滑块显形。两种写法并列：前者覆盖"鼠标在输入区任意位置"，
           后者兜底"鼠标正好落在滚动条上"（此时滚动条是子控件，是不是还算父的 hover
           各家 Qt 版本表现略有差别，两条都写上省得踩版本差异） */
        QTextEdit:hover QScrollBar::handle:vertical,
        QScrollBar:hover::handle:vertical {
            background-color: rgba(0, 0, 0, 0.16);   /* 半透明，比原来的实心 #c0c0c0 轻很多 */
        }
    )");
    inputEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    inputEdit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 断行规则必须在这里设（QSS 只管外观）：用 WrapAnywhere —— 任意位置都能断。
    // 不能用 WrapAtWordBoundaryOrAnywhere：实测它有坑，中文后面紧跟一个"没有空格的
    // 超长英文/数字串"时，Qt 会把断点钉在最后一个中文词边界上，宁可让这一行空出大半行
    // 也不肯钻进英文串里断（实测框宽 620px，整行只用到 206.7px，右边空 413px）；
    // 只有整行都是那串英文时它才肯按"anywhere"断。换成 WrapAnywhere 后每行都会老实填满，
    // 中文、字母之间都能断（代价：英文长单词顶到行尾会被拆开，中文聊天场景可以接受）
    inputEdit->setWordWrapMode(QTextOption::WrapAnywhere);
    // 文档自带 4px 内边距，清零后"文档排版宽高"和控件可视区才能精确对上（MessageItem 同理）
    inputEdit->document()->setDocumentMargin(0);
    layout->addWidget(inputEdit, 1, 0, 1, 2);
    layout->setRowStretch(1, 1);   // 伸缩行：输入框长高，1、2 行位置纹丝不动

    // ===== 2 行：发送按钮行（固定高，靠右）=====
    sendButton = new QPushButton("发送", this);
    sendButton->setFixedSize(60, 30);
    sendButton->setStyleSheet(R"(
        QPushButton {
            border: none;
            border-radius: 15px;
            background-color: #4a90d9;
            color: white;
            font-size: 14px;
            font-weight: 500;
        }
        QPushButton:hover {
            background-color: #3a80c9;
        }
        QPushButton:disabled {
            background-color: #a0a0a0;
        }
    )");
    sendButton->setEnabled(false);
    layout->addWidget(sendButton, 2, 1, 1, 1, Qt::AlignRight | Qt::AlignVCenter);
    layout->setRowStretch(2, 0);   // 固定行

    // 列宽：0 列吃掉横向富余（功能按钮靠左、发送按钮靠右互不打架）
    layout->setColumnStretch(0, 1);
    layout->setColumnStretch(1, 0);

    outerLayout->addWidget(boxFrame);

    // 整体高度上限（"向上扩是有高度限制"）：8+32+6+输入框120上限+6+30+8 的框体
    // 加底部 15 边距 ≈ 225。到达上限后输入框内部出滚动条，聊天记录区不再被压
    setMaximumHeight(225);

    // ===== 输入框高度过渡动画（adjustHeight 里用）=====
    // 两条动画一个动 minimumHeight、一个动 maximumHeight，参数必须完全一致：
    // 只要两者时刻相等，输入框在动画期间就一直是"定高"，不会因为 min < max
    // 被网格的 rowStretch 抢走额外高度（那就又变成瞬间跳了）
    heightAnimMin = new QPropertyAnimation(inputEdit, "minimumHeight", this);
    heightAnimMin->setDuration(150);
    heightAnimMin->setEasingCurve(QEasingCurve::OutCubic);   // 快起慢收，收尾不突兀
    //设置缓动曲线决定动画进度怎么走：OutCubic 是"快起慢收"，InCubic 是"慢起快收"，In 是"慢起"，InOut 是"两头慢中间快"，Out 是"慢收"
    heightAnimMax = new QPropertyAnimation(inputEdit, "maximumHeight", this);
    heightAnimMax->setDuration(150);
    heightAnimMax->setEasingCurve(QEasingCurve::OutCubic);

    // ===== 信号连接统一放这里 =====
    // 键盘交互不走信号：QTextEdit 没有"按键按下"信号，给 inputEdit 挂事件过滤器，
    // 事件先经过 ChatInput::eventFilter 再落到 inputEdit，这才拦得住回车
    inputEdit->installEventFilter(this);
    connect(fileButton, &QPushButton::clicked, this, &ChatInput::onFileClicked);
    connect(sendButton, &QPushButton::clicked, this, &ChatInput::onSendClicked);
    // textChanged 一条连两件事：按内容调输入框高度 + 按内容开关发送按钮。
    // 注意 adjustHeight 之前漏连了，输入框从来不会自动长高（文本一多就被布局硬挤）
    connect(inputEdit, &QTextEdit::textChanged, this, [this]() {
        adjustHeight();
        sendButton->setEnabled(!inputEdit->toPlainText().trimmed().isEmpty());
    });

    // 初始高度按"空内容"算一次，保证最小高度生效
    adjustHeight();
}

void ChatInput::onSendClicked()
{
    QString content = inputEdit->toPlainText().trimmed();//去掉首尾空格
    if (!content.isEmpty()) {
        emit sendMessage(content);
        inputEdit->clear();
        sendButton->setEnabled(false);
    }
}

void ChatInput::onFileClicked()
{
    QString filePath = QFileDialog::getOpenFileName(this, "选择文件", "", "所有文件 (*.*)");
    if (!filePath.isEmpty()) {
        emit sendFile(filePath);
    }
}

// 事件过滤器：inputEdit 上的所有事件都会先经过这里。
// 返回 true = 事件被吃掉（inputEdit 收不到），返回 false = 放行，inputEdit 照常处理
bool ChatInput::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != inputEdit) {
        return QWidget::eventFilter(watched, event);
    }

    // 输入法事件只用来记录"当前是不是在组字"，事件本身照常放行给 QTextEdit 处理
    if (event->type() == QEvent::InputMethod) {
        QInputMethodEvent* imeEvent = static_cast<QInputMethodEvent*>(event);//把event向下转型
        m_imeComposing = !imeEvent->preeditString().isEmpty();//判断是不是在组字，`preeditString()` 返回 当前还没上屏的组字内
        return QWidget::eventFilter(watched, event);//调用父类的事件Filter方法，放行事件
    }

    if (event->type() != QEvent::KeyPress) {
        return QWidget::eventFilter(watched, event);
    }

    QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);//把event向下转型

    // 主键盘的回车是 Key_Return、小键盘的回车是 Key_Enter，两个都得认
    const bool isEnterKey = keyEvent->key() == Qt::Key_Return
                         || keyEvent->key() == Qt::Key_Enter;
    if (!isEnterKey) {
        return QWidget::eventFilter(watched, event);
    }

    // 1) 组字中的回车归输入法管（选词/上屏），放行 —— 否则会把半截拼音发出去
    if (m_imeComposing) {
        return QWidget::eventFilter(watched, event);
    }

    // 2) 长按回车不重复发送，否则按住不放会刷出一串消息
    if (keyEvent->isAutoRepeat()) //判断是不是重复发的
    {
        return true;
    }

    // 3) Shift + 回车 = 换行：放行，QTextEdit 自己会插入一个换行
    //补一个知识点:每次键盘按下按键时，会触发一个事件，这个事件包含"主键是谁(key)"和"当时按着哪些修饰键(modifiers)"两个信息
    //修饰键（Shift/Ctrl/Alt）是"状态键"：按住期间系统一直维护它的状态；再按另一个键时会产生新事件，事件 = 主键 + 当时的修饰键状态快照
    //所以 Shift+回车 一共两个事件：事件1的主键是 Shift，事件2的主键是回车(带 Shift 状态)；我们处理的是事件2
    /*
     * ── 知识点总结：组合键 / modifiers / 位掩码 ───────────────────────────
     *
     * 【一、键盘事件的机制】
     *  1. 每个物理键按下都会产生一个独立的 KeyPress 事件（Shift 自己也有事件），
     *     并不是"组合键只发一个事件"。
     *  2. 修饰键（Shift / Ctrl / Alt）特殊在它是"状态键"：从按下到松开这段时间，
     *     系统一直维护着"它是按下"这个状态（跟按得久不久无关，是"按住期间"）。
     *  3. 每当产生一个按键事件，系统都会把"此刻的修饰键状态快照"一起打包发出。
     *  4. 所以 Shift+回车 物理上是两个事件：
     *       事件1：key=Key_Shift,  modifiers=ShiftModifier   （Shift 自己的事件）
     *       事件2：key=Key_Return, modifiers=ShiftModifier   （回车事件，带上了 Shift 状态）
     *     我们真正要处理的是事件2，它才是"回车"。
     *
     * 【二、modifiers() 是位掩码】
     *  QKeyEvent 里两个字段互不相关：
     *       key()       → 这次按的主键是谁（Key_Return / Key_A ...）
     *       modifiers() → 当时按住了哪些修饰键（一个位掩码）
     *  修饰键的值都是 2 的幂，一位一个键，互不重叠：
     *       ShiftModifier   = 0x02000000（第25位）
     *       ControlModifier = 0x04000000（第26位）
     *       AltModifier     = 0x08000000（第27位）
     *  同时按多个就是把对应的位"或"起来，例如 Shift+Ctrl = 0x06000000。
     *  用位掩码的原因：修饰键能同时按好几个，普通单值枚举表示不了组合。
     *
     * 【三、为什么用 & 而不是 ==】
     *  & 是"按位与"：只保留两个数同为 1 的位 —— 用来"筛出某一位有没有置上"。
     *       0x06000000 & 0x02000000 = 0x02000000  非0 → 按了 Shift ✅
     *       0x04000000 & 0x02000000 = 0x00000000  为0  → 没按 Shift ❌
     *  if() 里非 0 即 true，所以：
     *       if (keyEvent->modifiers() & Qt::ShiftModifier)   // 含 Shift 即命中
     *  ⚠ 绝不能用 ==：== 要求"不多不少正好相等"。
     *     一旦多按了别的键（如 Shift+Ctrl+回车），modifiers 变成 0x06000000，
     *     此时 == Qt::ShiftModifier 为 false → 换行失效，回车被误当成发送。
     *  规则：判断修饰键一律用 &，永远不用 ==。
     *
     * 【四、主键 vs 修饰键：看"角色"，不看"按下先后"】
     *  主键   = 这次按键操作的【目标键】，自身带动作（打字 / 发送 / 换行）
     *  修饰键 = 【专门用来修饰别人的键】，自身无动作，只为改变主键的含义
     *  ⚠ 按下先后 ≠ 主次！Shift 总是先按，但它只是"服务角色"：
     *     单按 Shift 不打字、不发送、不换行，它存在的意义就是"给下一个键贴标签"。
     *     类比"红色的苹果"：'红色的'先说出口，但中心词是'苹果'。
     *  注意：每个事件各有自己的主键：
     *     按 Shift 产生【事件1】：主键 = Shift（key=Key_Shift）
     *        —— 但它 key 不是回车，在第一关就被放行，我们不处理它
     *     按 Enter 产生【事件2】：主键 = Enter（key=Key_Return），Shift 降级为 modifiers 标记
     *        —— 这才是我们要处理的事件
     *  所以代码"先判 key() 再判 modifiers()" = 先确认主键是回车，再问它带不带 Shift 标记。
     *
     * 【五、放行 ≠ 换行】
     *  eventFilter 里的 `return QWidget::eventFilter(...)`（false）只表示"不拦截"，
     *  它本身【不会】产生 '\n'。换行是控件的"原本事件函数"干的：
     *      QTextEdit 自带 keyPressEvent：收到回车 → 往文档里插入一个 '\n'
     *  两个函数的分工：
     *      ChatInput::eventFilter（我们写的，外挂过滤器）→ 先跑，只决定"拦 / 放"
     *      QTextEdit::keyPressEvent（Qt 自带，控件本能）→ 后跑，按事件类型处理：
     *          收到【回车事件】  → 插入 '\n'（换行）
     *          收到【输入法事件】→ 更新预编辑串（候选），与 '\n' 无关
     *  所以同样是"放行"，结果不同 —— 取决于放行过去的是什么事件。
     *  本段代码正是靠这个分工：
     *      普通回车   → return true  【抢过来】当发送（QTextEdit 收不到，故不插 '\n'）
     *      Shift+回车 → return false 【不抢】还给 QTextEdit，保持它天性的换行
     *  一句话：Shift+回车能换行，本质是"没有去抢 QTextEdit 的默认行为"。
     */
    if (keyEvent->modifiers() & Qt::ShiftModifier) {
        return QWidget::eventFilter(watched, event);
    }

    // 4) 剩下的回车 = 发送。必须返回 true 把事件吃掉，否则 QTextEdit 会在发送之后
    //    再插一个空行（消息发出去了，框里却还留着一行空白）
    onEnterPressed();
    return true;
}

void ChatInput::onEnterPressed()
{
    if (sendButton->isEnabled()) {
        onSendClicked();
    }
}

void ChatInput::adjustHeight()
{
    // 文档排版的实际高度 + QSS 的上下 padding（6px × 2）。
    // 不补 padding 的话文字会被量矮一截，多行时最后一行会被裁掉；
    // 用 qCeil 向上取整，别用 (int) 截断——截断同样是"少算一丝"，最后一行照样被切
    const int docHeight = qCeil(inputEdit->document()->size().height()) + 12;

    // 初始（同时也是最小）高度 = 4 行：行距 × 4 + 上下 padding。
    // 字体直接取控件当前的：字号已经在建控件时定了，取到的就是排版用的那个字体，
    // 不用再"照着 QSS 的字号造一个字体"来量
    const int minHeight = QFontMetrics(inputEdit->font()).lineSpacing() * 4 + 12;

    // 夹在 [4 行, 上限] 之间：空输入框也占 4 行；
    // 上限还是"向上扩的高度限制"——到顶后内容在框内自己滚，框不再长
    const int target = qBound(minHeight, docHeight, 120);

    // 构造期第一次调用时还没设过高度（minimumHeight 是控件默认值 0）：直接一步落位，
    // 不播动画，否则界面一出来会看到输入框从很矮"长"出来
    if (inputEdit->minimumHeight() == 0) {
        inputEdit->setFixedHeight(target);
        return;
    }

    // 同一行内继续打字时高度没变，不用反复重启动画
    const int current = inputEdit->maximumHeight();
    if (current == target) {
        return;
    }

    // 起点取"当前高度"而不是"上次的目标值"：打字快、上一条动画还没跑完就又触发时，
    // 这样是从当前位置接着长，不会先回跳再走
    heightAnimMin->stop();
    heightAnimMax->stop();
    heightAnimMin->setStartValue(current);
    heightAnimMin->setEndValue(target);
    heightAnimMax->setStartValue(current);
    heightAnimMax->setEndValue(target);
    heightAnimMin->start();
    heightAnimMax->start();
}

