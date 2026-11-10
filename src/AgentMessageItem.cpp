#include "AgentMessageItem.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTextEdit>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextOption>
#include <QFontMetrics>
#include <QTimer>
#include <QtMath>

AgentMessageItem::AgentMessageItem(const QString& content, bool isSelf, QWidget *parent)
    : QWidget(parent)
{
    setupUI(isSelf);

    // 先定字号，再塞文本：setPlainText 是"按当前字体插入"的，反过来的话
    // 已经进去的文字仍带着旧字号，后面量宽度就量不准了
    QFont textFont = m_contentEdit->font();
    textFont.setPixelSize(14);
    m_contentEdit->setFont(textFont);
    m_contentEdit->setPlainText(content);

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

    QFontMetrics fm(m_contentEdit->font());
    int textWidth = 0;
    const QStringList textLines = m_contentEdit->toPlainText().split('\n');
    for (const QString& line : textLines) {
        textWidth = qMax(textWidth, fm.horizontalAdvance(line));
    }

    const int maxTextWidth = 780;   // + 气泡左右内边距 10×2 → 气泡最大宽度 800
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

void AgentMessageItem::appendStreamChunk(const QString& chunk)
{
    if (m_contentEdit == nullptr || chunk.isEmpty()) {
        return;
    }

    // 不用 QTextEdit::append：它会新起一个段落，增量包会被拆成一行行
    // 光标移到末尾直接 insertText，文字看起来就是连续长出来的
    QTextCursor cursor(m_contentEdit->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(chunk);

    scheduleResize();
}

void AgentMessageItem::setHitlActions(const QStringList& actions)
{
    // TODO(HITL)：本期只预留接口，不实现按钮。
    // 后续接入时：按 actions 往 m_hitlBar 的布局里创建按钮并连接回调，再 m_hitlBar->show()
    m_pendingActions = actions;
}
