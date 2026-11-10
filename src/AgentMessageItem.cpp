#include "AgentMessageItem.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTextEdit>
#include <QTextDocument>
#include <QTextOption>
#include <QFontMetrics>
#include <QTimer>
#include <QtMath>

namespace {
// 流式合流窗口：这段时间内到达的所有增量只排版一次
constexpr int kFlushIntervalMs = 30;
}

AgentMessageItem::AgentMessageItem(const QString& content, bool isSelf, QWidget *parent)
    : QWidget(parent)
    , m_isSelf(isSelf)
    , m_markdownText(content)
{
    setupUI(isSelf);

    // 先定字号，再塞文本：setPlainText / setMarkdown 都是"按当前字体排版"的，反过来的话
    // 已经进去的文字仍带着旧字号，后面量宽度就量不准了
    QFont textFont = m_contentEdit->font();
    textFont.setPixelSize(14);
    m_contentEdit->setFont(textFont);
    // 正文每次都是整篇重渲染（Markdown 有上下文，不能只插增量），撤销栈只会在
    // 每次 setMarkdown 时白白攒一份文档快照，流式场景直接关掉
    m_contentEdit->setUndoRedoEnabled(false);

    // 合流定时器：服务器是"单字符一包"推过来的，一包一次排版会把主线程压死。
    // 用一个 30ms 的单次定时器把窗口内的增量攒起来，到点统一落屏一次
    m_flushTimer = new QTimer(this);
    m_flushTimer->setSingleShot(true);
    m_flushTimer->setInterval(kFlushIntervalMs);
    connect(m_flushTimer, &QTimer::timeout, this, &AgentMessageItem::onFlushTimeout);

    renderContent();
    scheduleResize();
}

void AgentMessageItem::setupUI(bool isSelf)
{
    QHBoxLayout* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(15, 10, 15, 10);
    mainLayout->setSpacing(0);

    // ========== 消息气泡部分 ==========
    // 气泡用垂直布局：上面是文本，下面是 HITL 按钮容器（以后直接往里加，不用改布局结构）
    m_bubbleWidget = new QWidget(this);
    // 关键：普通 QWidget 默认不绘制 QSS 里的 background / border-radius（Qt 只会给
    // QFrame 等原生支持背景的控件画），开了 WA_StyledBackground 才会用样式表自己画底，
    // 否则窗口里看到的永远是布局默认的方块底色 —— 这就是气泡"圆角不生效"的根因
    m_bubbleWidget->setAttribute(Qt::WA_StyledBackground, true);
    m_bubbleWidget->setObjectName("agentBubble");
    QVBoxLayout* bubbleLayout = new QVBoxLayout(m_bubbleWidget);
    bubbleLayout->setContentsMargins(10, 6, 10, 6);
    bubbleLayout->setSpacing(6);

    QString bubbleColor = isSelf ? "#4a90d9" : "#ffffff";
    QString textColor = isSelf ? "white" : "#333333";

    m_contentEdit = new QTextEdit(m_bubbleWidget);
    // 字号统一在代码里定死（见构造函数）：量宽度和排版必须是同一个字体
    // border 必须写成 0px solid transparent（QTextEdit 是 QFrame 子类，
    // 直接写 border: none 可能不生效，残留边框会让"可视宽度"比"控件宽度"小，
    // 文字就会"差一点放不下"而提前折行）；padding 同理必须清零
    m_contentEdit->setStyleSheet(QString(R"(
        QTextEdit {
            color: %1;
            background-color: transparent;
            border: 0px solid transparent;
            padding: 0px;
        }
    )").arg(textColor));
    m_contentEdit->setReadOnly(true);
    m_contentEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_contentEdit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // WrapAnywhere：一行排满就在当前字符后直接断行（同 MessageItem，避免超长英文串把断点钉在词边界）
    m_contentEdit->setWordWrapMode(QTextOption::WrapAnywhere);
    bubbleLayout->addWidget(m_contentEdit);

    // HITL 预留容器：默认隐藏，隐藏时布局上不占空间
    m_hitlBar = new QWidget(m_bubbleWidget);
    QHBoxLayout* hitlLayout = new QHBoxLayout(m_hitlBar);
    hitlLayout->setContentsMargins(0, 0, 0, 0);
    hitlLayout->setSpacing(8);
    hitlLayout->addStretch(1);
    m_hitlBar->setStyleSheet("background-color: transparent;");
    m_hitlBar->hide();
    bubbleLayout->addWidget(m_hitlBar);

    // 选择器用 #agentBubble（而不是 QWidget）：QWidget 会连子控件（文本、HITL 容器）
    // 一起命中，虽然它们各自写了 transparent 兜着，但没必要让样式外溢，指定对象名最稳
    m_bubbleWidget->setStyleSheet(QString(R"(
        #agentBubble {
            border-radius: 12px;
            background-color: %1;
        }
    )").arg(bubbleColor));
    m_bubbleWidget->setMaximumWidth(800);
    m_bubbleWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    // ========== 左右区分（不带头像）==========
    if (isSelf) {
        mainLayout->addStretch(1);
        mainLayout->addWidget(m_bubbleWidget);
    } else {
        mainLayout->addWidget(m_bubbleWidget);
        mainLayout->addStretch(1);
    }

    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

// 自适应宽高的核心永远在 QTextDocument 上：
//   1. 按字体量出最长一行的宽度（手动换行的每一行都量）
//   2. 与封顶值取小，得到排版宽度（+1 防取整余量）
//   3. 让文档按这个宽度排版，量出真实需要的高度
//   4. 控件宽高、气泡宽全部回设成同一个 wrapWidth，保证"量宽 = 设宽 = 排版宽"
void AgentMessageItem::refreshBubbleSize()
{
    if (m_contentEdit == nullptr || m_bubbleWidget == nullptr) {
        return;
    }

    QTextDocument* doc = m_contentEdit->document();
    // 文档默认自带 4px 内边距，不清零的话真正能排下文字的宽度会比算出来的少 8px
    doc->setDocumentMargin(0);

    int textWidth = 0;
    if (m_isSelf) {
        // 纯文本：手动换行的每一行都要量，取最宽的一行
        QFontMetrics fm(m_contentEdit->font());
        const QStringList textLines = m_contentEdit->toPlainText().split('\n');
        for (const QString& line : textLines) {
            textWidth = qMax(textWidth, fm.horizontalAdvance(line));
        }
    } else {
        // Markdown：标题字号更大、加粗更宽、列表还带缩进，用 QFontMetrics 量纯文本
        // 会算小（渲染后比算出来的宽 → 又提前折行），所以把文档临时设成不限宽，
        // 让 QTextDocument 自己算"最宽一行要多少像素"
        const qreal savedWidth = doc->textWidth();
        doc->setTextWidth(-1);
        textWidth = qCeil(doc->idealWidth());
        doc->setTextWidth(savedWidth);
    }

    // 上限不能写死 780：窗口（或智能体面板）变窄时，消息容器可能还不到 800，
    // 写死会让气泡比可视区域还宽，被 QScrollArea 横向裁掉，底部多出横向滚动条
    // 容器宽 - 左右外边距 15×2 - 气泡内边距 10×2 - 取整余量 10
    int maxTextWidth = 780;   // 宽窗口下的封顶值（+ 气泡左右内边距 10×2 → 气泡最大 800）
    const QWidget* container = this->parentWidget();
    // >200 才认：控件还没被布局算过时宽度是默认值（100），拿它算会把气泡压成窄条
    if (container != nullptr && container->width() > 200) {
        maxTextWidth = qMin(maxTextWidth, container->width() - 60);
    }
    maxTextWidth = qMax(maxTextWidth, 120);   // 窗口极窄时兜底，避免算出负宽度
    const int wrapWidth = qMin(textWidth, maxTextWidth) + 1;

    doc->setTextWidth(wrapWidth);
    const int textHeight = qCeil(doc->size().height());

    m_contentEdit->setFixedWidth(wrapWidth);
    m_contentEdit->setFixedHeight(textHeight);
    m_bubbleWidget->setFixedWidth(wrapWidth + 20);   // + 气泡左右内边距 10×2
}

void AgentMessageItem::scheduleResize()
{
    // 流式时 chunk 可能来得很密，只让第一个排一次任务，期间的增量全部合并
    if (m_resizePending) {
        return;
    }
    m_resizePending = true;
    QTimer::singleShot(10, this, [this]() {
        m_resizePending = false;
        refreshBubbleSize();
    });
}

// 把整篇文本重新渲染进 QTextEdit。
// 用户提问走 setPlainText，AI 回答走 setMarkdown —— 两者都不能只"往末尾插增量"：
// Markdown 的块级语法（``` 代码块、列表连续行）是上下文相关的，边插边解析会把
// 半个代码块渲染成普通段落，所以统一"攒够一次、整篇重排一次"
void AgentMessageItem::renderContent()
{
    if (m_contentEdit == nullptr) {
        return;
    }
    if (m_isSelf) {
        m_contentEdit->setPlainText(m_markdownText);
        return;
    }
    // 用 QTextDocument 的版本而不是 QTextEdit::setMarkdown：后者的重载只有
    // "一个参数"，方言写死；文档版可以显式指定 GitHub 方言（表格、删除线都认）
    m_contentEdit->document()->setMarkdown(m_markdownText, QTextDocument::MarkdownDialectGitHub);
}

void AgentMessageItem::onFlushTimeout()
{
    flushPending();
}

void AgentMessageItem::flushPending()
{
    if (m_pendingText.isEmpty()) {
        return;
    }
    m_markdownText += m_pendingText;
    m_pendingText.clear();
    if (m_flushTimer != nullptr) {
        m_flushTimer->stop();   // 已经手动落屏，定时器不必再补一次
    }
    renderContent();
    // 这里直接量尺寸，不再走 scheduleResize：落屏本身就是 30ms 一次的节流节奏，
    // 再套一层 10ms 延迟只会让最后一段晚一点出来
    refreshBubbleSize();
}

void AgentMessageItem::appendStreamChunk(const QString& chunk)
{
    if (m_contentEdit == nullptr || chunk.isEmpty()) {
        return;
    }

    // 只入缓冲，等定时器统一落屏。判断用 isActive 而不是每次 restart：
    // 服务器持续不断推包时，restart 会把超时一直往后推，文字就永远上不了屏
    m_pendingText += chunk;
    if (m_flushTimer != nullptr && !m_flushTimer->isActive()) {
        m_flushTimer->start();
    }
}

void AgentMessageItem::setHitlActions(const QStringList& actions)
{
    // TODO(HITL)：本期只预留接口，不实现按钮。
    // 后续接入时：按 actions 往 m_hitlBar 的布局里创建按钮并连接回调，再 m_hitlBar->show()
    m_pendingActions = actions;
}
