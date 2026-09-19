#include "MessageItem.h"
#include <QTimer>
#include <QFontMetrics>
#include <QStringList>
#include <QtMath>

MessageItem::MessageItem(const MessageInfo& message, QWidget *parent)
    : QWidget(parent), m_messageId(message.id), m_statusIndicator(nullptr)
{
    setupUI(message);
}

// 切换消息发送状态：转发给状态指示器（自己发送的消息才有效）
void MessageItem::setStatus(MessageStatusIndicator::Status status)
{
    if (m_statusIndicator) {
        m_statusIndicator->setStatus(status);
    }
}

void MessageItem::setupUI(const MessageInfo& message)
{
    QHBoxLayout* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(15, 10, 15, 10);
    mainLayout->setSpacing(10);

    // ========== 头像部分 ==========
    QFrame* avatarFrame = new QFrame(this);
    avatarFrame->setFixedSize(40, 40);
    
    QString avatarBgColor = message.isSelf ? "#4a90d9" : "#67c23a";
    avatarFrame->setStyleSheet(QString(R"(
        QFrame {
            border-radius: 20px;
            background-color: %1;
        }
    )").arg(avatarBgColor));
    
    QLabel* avatarLabel = new QLabel(avatarFrame);
    avatarLabel->setFixedSize(40, 40);
    avatarLabel->setStyleSheet(R"(
        color: white;
        font-size: 16px;
    )");
    avatarLabel->setText(message.isSelf ? "我" : message.senderId.left(1).toUpper());
    avatarLabel->setAlignment(Qt::AlignCenter);

    // ========== 消息气泡部分 ==========
    QWidget* bubbleWidget = new QWidget(this);
    // ✅ 关键1：气泡用垂直布局，后面要加时间标签，水平布局放不下
    QVBoxLayout* bubbleLayout = new QVBoxLayout(bubbleWidget);
    bubbleLayout->setContentsMargins(10, 6, 10, 6);
    bubbleLayout->setSpacing(4);

    QString bubbleColor = message.isSelf ? "#4a90d9" : "#ffffff";
    QString textColor = message.isSelf ? "white" : "#333333";

    if (message.isFile) {
        QLabel* fileLabel = new QLabel(bubbleWidget);
        QString fileSizeStr;
        if (message.fileSize < 1024) {
            fileSizeStr = QString("%1 B").arg(message.fileSize);
        } else if (message.fileSize < 1024 * 1024) {
            fileSizeStr = QString("%1 KB").arg(message.fileSize / 1024);
        } else {
            fileSizeStr = QString("%1 MB").arg(message.fileSize / (1024 * 1024));
        }

        QString html = QString(R"(
            <div style="display: flex; align-items: center; gap: 8px;">
                <span style="font-size: 20px;">📄</span>
                <div>
                    <div style="font-size: 13px; color: %1; font-weight: 500;">%2</div>
                    <div style="font-size: 11px; color: %3;">%4</div>
                </div>
            </div> 
        )").arg(textColor, message.fileName, "#999", fileSizeStr);
        fileLabel->setText(html);
        bubbleLayout->addWidget(fileLabel);
     } else {
        QTextEdit* contentEdit = new QTextEdit(bubbleWidget);
        // 先定字号，再塞文本：setPlainText 是"按当前字体插入"的，反过来的话
        // 已经进去的文字仍带着旧字号，后面量宽度就量不准了
        QFont textFont = contentEdit->font();
        textFont.setPixelSize(14);
        contentEdit->setFont(textFont);
        contentEdit->setPlainText(message.content);
        contentEdit->setStyleSheet(QString(R"(
            QTextEdit {
                color: %1;
                background-color: transparent;
                /* 字号不在这里写：量宽度和排版必须是同一个字体，两处各写一份 14px
                   迟早会改一处漏一处，字号统一在上面用代码定
                   border 必须写成 0px solid transparent（同 ChatInput / note.txt）：
                   QTextEdit 是 QFrame 子类，直接写 border: none 可能不生效，
                   残留的原生边框会让"可视宽度"比"控件宽度"小 2~4px，
                   文字就会"差一点放不下"而提前折行
                   padding 同理，横向 padding 会让排版宽度和可视宽度不一致 */
                border: 0px solid transparent;
                padding: 0px;
            }
        )").arg(textColor));
        
        contentEdit->setReadOnly(true);//设置为只读，防止用户编辑
        contentEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);//隐藏垂直滚动条
        contentEdit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);//隐藏水平滚动条
        // 断行用 WrapAnywhere（任意位置断），理由同 ChatInput：WrapAtWordBoundaryOrAnywhere
        // 在"中文 + 没有空格的超长英文数字串"时会把断点钉在最后一个中文词边界上，
        // 让这一行右边空出一大段（气泡里也是同样的坑）
        contentEdit->setWordWrapMode(QTextOption::WrapAnywhere);//断点选择:一行排满放不下了，就在 当前那个字符后面 直接断，下一个字符挪到下一行；
        /*WrapAtWordBoundaryOrAnywhere` （ 优先在词边界断，实在不行才任意断 ）。它的毛病是：
        遇到「中文 + 紧跟一个没有空格的超长英文/数字串」时，Qt 会把断点 钉死在最后一个中文词边界 上， */

        
        // 关键：自适应宽度逻辑
        /*
        QTextEdit = 显示控件 + 内部有一个 QTextDocument（文档对象）
        你看到的文字、换行、排版，全是 QTextDocument 算出来的
        QTextEdit 只负责 “画出来”，不负责计算最佳宽高
        所以自适应的核心，永远是 操作 QTextDocument
        */ 

        //以下是自适应宽度的详细流程
         /*
         * 自适应宽度总流程（忘了回来看这里）：
         *   1. 定字号：建控件时用代码把 14px 定死（见上面的 setFont），不写进 QSS，
         *      保证"量宽度用的字体"和"排版用的字体"完全同源
         *   2. 造尺子：QFontMetrics fm(contentEdit->font())
         *      —— 只依赖字体、不依赖控件/布局，能在布局之外离线量字宽
         *   3. 量宽：split('\n') 先拆出手动换行的每一行（回车是文字自带的，不算自动折行），
         *      逐行 fm.horizontalAdvance() 再 qMax 取最宽那行
         *      → 得到"完全不自动换行时，这段文字需要多宽"
         *   4. 定排版宽度：qMin(文字真实宽度, 封顶 780) + 1
         *      —— 注意是"在真实宽度和封顶值之间取小"，不是"求最小"：
         *         短文字按自己真实宽度贴字走，长文字被 780 封顶后改为向下折行
         *      —— +1 是防取整余量：量出的是整数，排版用浮点，不给余量会"差一丝就折行"
         *   5. 排版量高：doc->setTextWidth(wrapWidth) 让文档按这个宽度排版，
         *      再 qCeil(doc->size().height()) 取它真实需要的高度
         *   6. 回设：contentEdit 的宽高、bubbleWidget 的宽，全部用同一个 wrapWidth，
         *      保证"量宽 = 设宽 = 排版宽"三者统一（历史上就是三者各说各话才导致提前折行）
         */
         //`QTimer::singleShot` 是个 静态成员函数 （`static` ）
        QTimer::singleShot(10, this, [contentEdit, bubbleWidget]()
         {
            /*QTextEdit 只是个显示器，真正算换行/排版的是它内部的QTextDocument。 后面所有计算都在这货身上做 ，所以先把它取出来
            你想要文字要多宽度但你只能问 QTextDocument 要尺寸，而 document 的尺寸，必须"你先给它宽度"，它才能算出来
            死循环 → 所以宽度不能问它
            */
            QTextDocument* doc = contentEdit->document();
            // 文档默认自带 4px 内边距，不清零的话真正能排下文字的宽度会比下面算出来的少 8px，
            // 表现就是"明明量着放得下，实际却提前换行"
            // （气泡的左右内边距由 bubbleLayout 的 margins 负责，不该让文档再出一份）
            doc->setDocumentMargin(0);

            // 1. 按字体量出"最长一行"在不换行时的宽度。
            //    字体直接取控件当前的——字号已经在建控件时定死，这里再取就是这个字体，
            //    保证"量宽度用的字体"就是"排版用的字体"
            QFontMetrics fm(contentEdit->font());//造尺子。用contentEdit->font() 而不是自己造字体，保证"量宽度的字体"就是"排版的字
           //QFontMetrics它是唯一"不用先知道宽度就能算出宽度"的办法。font()是 QWidget 的成员函数，返回 这个控件当前真正生效的字体
            int textWidth = 0;
            const QStringList textLines = contentEdit->toPlainText().split('\n');//先拆行把用户手动换行的每行的宽度算出来
            for (const QString& line : textLines) {
                textWidth = qMax(textWidth, fm.horizontalAdvance(line));//计算每行的宽度，取最大的作为总宽度
            }

            // 2. 只有超过上限才需要折行，没超过就按文字的实际宽度撑开。
            //    +1 是防取整的余量：量出来是整数（可能比真实 advance 小 0.5px 以内），
            //    而排版用的是浮点 advance，不给余量就会出现"差一丝就折行"
            const int maxTextWidth = 780;   // + 气泡左右内边距 10×2 → 气泡最大宽度 800
            const int wrapWidth = qMin(textWidth, maxTextWidth) + 1;

            // 3. 让文档按这个宽度排版，量出它真实需要的高度
            doc->setTextWidth(wrapWidth);
            const int textHeight = qCeil(doc->size().height());

            // 4. 控件 / 排版宽度 / 气泡三者必须是同一个宽度值：
            //    原来的毛病就是三者各说各话——量宽度用 idealWidth、排版却用 idealWidth - 20、
            //    控件又用 idealWidth，排版的可用宽度比控件整整窄 20px（再加文档自带 8px），
            //    所以盒子明明还放得下，文字却先换行了
            contentEdit->setFixedWidth(wrapWidth);
            contentEdit->setFixedHeight(textHeight);
            bubbleWidget->setFixedWidth(wrapWidth + 20);   // + 气泡左右内边距 10×2
        });
        
        bubbleLayout->addWidget(contentEdit);
    }


    bubbleWidget->setStyleSheet(QString(R"(
        QWidget {
            border-radius: 18px;
            background-color: %1;
        }
    )").arg(bubbleColor));

    // ✅ 关键4：限制气泡最大宽度，不要用样式表的max-width，代码设置更可靠
    bubbleWidget->setMaximumWidth(800);
    bubbleWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    // ========== 布局顺序区分 ==========
    if (message.isSelf) {
        mainLayout->addStretch(1);
        // 自己发送的消息：气泡左侧显示状态指示器（发送中旋转圆圈 / 失败红色感叹号）
        // 历史消息默认隐藏，只有新发的消息才通过 setStatus 显示
        m_statusIndicator = new MessageStatusIndicator(this);
        // 气泡这块指示器属于"消息发送"功能：重试时按 MessageSend 路由到聊天后端
        m_statusIndicator->setFeature(LoginFeature::MessageSend);
        // 点击失败感叹号 → 转发重发请求（带上本消息ID + 功能枚举）
        connect(m_statusIndicator, &MessageStatusIndicator::retryClicked, this, [this](LoginFeature feature) {
            emit retryRequested(m_messageId, feature);
        });
        mainLayout->addWidget(m_statusIndicator, 0, Qt::AlignVCenter);
        mainLayout->addWidget(bubbleWidget, 1);
        mainLayout->addWidget(avatarFrame);
    } else {
        mainLayout->addWidget(avatarFrame);
        mainLayout->addWidget(bubbleWidget, 1);
        mainLayout->addStretch(1);
    }

    // 让整个MessageItem高度自适应内容
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}