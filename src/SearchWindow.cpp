#include "SearchWindow.h"
#include "SearchResultList.h"
#include "AddFriendDialog.h"
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QRegularExpressionValidator>//搜索框只允许输入数字（与登录页账号框同一套正则）

namespace {
constexpr int kTitleBarHeight = 50;  // 标题框高度（与其他页面保持一致）
constexpr int kSearchBoxHeight = 36; // 搜索框高度（36 = 圆角半径 18 的两倍，正好是胶囊形）
constexpr int kClearIconSize = 12;   // 叉号图标画多大（按钮本身 20x20）
constexpr int kSearchBtnWidth = 64;  // 右侧「搜索」按钮宽度（高度与搜索框齐平）

// 清除按钮的两个图标状态：默认/悬浮用普通叉号，按下换成红色点击态
//（delect_clicked.svg 里 fill="#d81e06"，就是给"按下去"这一刻用的）
const char* kClearIconNormal  = ":/res/icon/delect.svg";
const char* kClearIconClicked = ":/res/icon/delect_clicked.svg";
}

// 搜索页：整页就是一个垂直布局 —— 第 1 项标题框（"搜索"），
// 第 2 项搜索框（盒子 + 右侧搜索按钮），第 3 项搜索结果列表（吃掉剩下全部高度）
SearchWindow::SearchWindow(QWidget *parent) : QWidget(parent)
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ===== 标题框 =====
    QWidget* titleBar = new QWidget(this);
    titleBar->setObjectName("searchTitleBar");
    titleBar->setFixedHeight(kTitleBarHeight);
    // 用 #id 选择器限定作用范围，避免样式污染子控件
    titleBar->setStyleSheet(R"(
        #searchTitleBar {
            background-color: white;
            border-bottom: 1px solid #e0e0e0;
        }
    )");

    QHBoxLayout* titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(15, 0, 15, 0);
    titleLayout->setSpacing(0);

    QLabel* titleLabel = new QLabel("搜索", titleBar);
    titleLabel->setStyleSheet("color: black; font-size: 16px; font-weight: 600;");
    titleLayout->addWidget(titleLabel);
    titleLayout->addStretch();//弹簧：标题靠左

    mainLayout->addWidget(titleBar);

    // ===== 搜索框（盒子）：左图标 + 中输入框 + 右清除按钮 =====
    QWidget* searchBox = new QWidget(this);
    searchBox->setObjectName("searchBox");
    searchBox->setFixedHeight(kSearchBoxHeight);
    searchBox->setStyleSheet(R"(
        #searchBox {
            background-color: white;
            border: 1px solid #e0e0e0;
            border-radius: 18px;
        }
    )");

    QHBoxLayout* boxLayout = new QHBoxLayout(searchBox);
    boxLayout->setContentsMargins(12, 0, 6, 0);//右侧内边距小一点，让圆形按钮贴近边缘
    boxLayout->setSpacing(8);

    m_searchIcon = new QLabel(searchBox);
    m_searchIcon->setFixedSize(16, 16);
    m_searchIcon->setPixmap(QIcon(":/res/icon/Serch_show.svg").pixmap(16, 16));//先用现成的搜索图标顶着，后续要换再替换
    boxLayout->addWidget(m_searchIcon);

    m_searchEdit = new QLineEdit(searchBox);
    m_searchEdit->setPlaceholderText("搜索用户ID");
    m_searchEdit->setStyleSheet(R"(
        QLineEdit {
            border: none;
            background-color: transparent;
            font-size: 14px;
            color: #333;
        }
    )");
    // 搜索对象就是"用户账号ID"，所以只允许数字，最长 15 位——与登录页账号输入框同一套正则。
    // QRegularExpressionValidator 是在【录入阶段】就拦掉：敲字母根本进不去输入框，
    // 比输完再弹红字提示更省事（本页只搜 ID，没有昵称搜索，所以不需要放行其它字符）
    m_searchEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression("[0-9]{0,15}"), m_searchEdit));
    boxLayout->addWidget(m_searchEdit, 1);//权重 1：输入框吃掉中间所有宽度

    m_clearBtn = new QPushButton(searchBox);
    m_clearBtn->setFixedSize(20, 20);
    m_clearBtn->setCursor(Qt::PointingHandCursor);
    m_clearBtn->setFocusPolicy(Qt::NoFocus);//不参与 Tab 焦点循环
    m_clearBtn->setIcon(QIcon(kClearIconNormal));//默认（含悬浮）就是普通叉号
    m_clearBtn->setIconSize(QSize(kClearIconSize, kClearIconSize));
    // 图标本身不带底色，这里只给一圈圆形的悬浮反馈（按下那一瞬的变色靠换图标，见槽函数）
    m_clearBtn->setStyleSheet(R"(
        QPushButton {
            border: none;
            border-radius: 10px;/*半径 = 边长一半 → 正圆*/
            background-color: transparent;
        }
        QPushButton:hover {
            background-color: rgba(0, 0, 0, 0.10);/*悬浮加深*/
        }
    )");
    m_clearBtn->hide();//默认没内容，不显示
    boxLayout->addWidget(m_clearBtn);

    // ===== 搜索框右侧的「搜索」按钮 =====
    m_searchButton = new QPushButton("搜索", this);
    m_searchButton->setFixedSize(kSearchBtnWidth, kSearchBoxHeight);
    m_searchButton->setCursor(Qt::PointingHandCursor);
    m_searchButton->setFocusPolicy(Qt::NoFocus);//不参与 Tab 焦点循环
    // 四个状态（正常 / 悬停 / 按下 / 禁用）写在同一张表里，避免禁用时颜色对不上
    m_searchButton->setStyleSheet(R"(
        QPushButton {
            background-color: #4a90d9;
            color: white;
            font-size: 13px;
            border: none;
            border-radius: 8px;
        }
        QPushButton:hover {
            background-color: #3a80c9;
        }
        QPushButton:pressed {
            background-color: #2f70b5;
        }
        QPushButton:disabled {
            background-color: #d0d0d0;
            color: #f5f5f5;
        }
    )");

    // 搜索框外面再包一层：盒子与按钮并排，整行留出左右上下的空白
    QWidget* searchWrapper = new QWidget(this);
    QHBoxLayout* wrapperLayout = new QHBoxLayout(searchWrapper);
    wrapperLayout->setContentsMargins(15, 12, 15, 0);
    wrapperLayout->setSpacing(10);
    wrapperLayout->addWidget(searchBox, 1);//权重 1：盒子吃掉按钮以外的宽度
    wrapperLayout->addWidget(m_searchButton);

    mainLayout->addWidget(searchWrapper);

    // ===== 「添加好友」结果提示条 =====
    // 点结果卡片的「添加」后，服务器回包（成功/失败）在这里给一句话。
    // 默认 hide()：QLayout 默认不给隐藏控件留位置，所以不显示时不占高度，
    // 出现时把下面的结果列表挤矮 22px，整页不会跳
    m_addResultLabel = new QLabel(this);
    m_addResultLabel->setFixedHeight(22);
    m_addResultLabel->setContentsMargins(15, 0, 15, 0);
    m_addResultLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    m_addResultLabel->hide();
    mainLayout->addWidget(m_addResultLabel);

    // ===== 搜索结果列表（占满下方剩余高度）=====
    m_resultList = new SearchResultList(this);
    mainLayout->addWidget(m_resultList, 1);

    // ===== 信号连接 =====
    connect(m_clearBtn, &QPushButton::clicked, m_searchEdit, &QLineEdit::clear);
    connect(m_clearBtn, &QPushButton::pressed, this, &SearchWindow::onClearButtonPressed);
    connect(m_clearBtn, &QPushButton::released, this, &SearchWindow::onClearButtonReleased);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &SearchWindow::onSearchTextChanged);
    connect(m_searchButton, &QPushButton::clicked, this, &SearchWindow::onSearchTriggered);
    // 结果列表的「添加」→ 本页先弹小窗收留言，确认后才往外抛：主窗口统一接线，
// 本页面不认识主后端以外的任何东西（所以这里不直连 addFriendRequested，中间要插一道弹窗）
    connect(m_resultList, &SearchResultList::addRequested, this, &SearchWindow::onAddRequested);

    m_searchEdit->installEventFilter(this);//回车触发搜索（见 eventFilter）
    updateSearchButtonState();             //初始是空词 → 按钮置灰
}

SearchWindow::~SearchWindow()
{
}

// 事件过滤：输入框里的回车触发搜索。
// 为什么不用 QLineEdit::returnPressed：这里要的是"主键盘回车 Key_Return 和小键盘回车
// Key_Enter 都认"，并且把按键在这一层吃掉（不再传给输入框）——用事件过滤一处收口，
// 以后要加别的键（比如 Esc 清空输入框）也直接往这里加一个分支就行
bool SearchWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_searchEdit && event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        // Key_Return = 主键盘回车，Key_Enter = 小键盘回车，两个都要
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            onSearchTriggered();
            return true;//事件已处理，不再往下传
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SearchWindow::onSearchTextChanged(const QString& text)
{
    m_clearBtn->setVisible(!text.isEmpty());//有字才显示清除按钮
    updateSearchButtonState();
}

void SearchWindow::updateSearchButtonState()
{
    // 与登录按钮同一套规矩：没内容时置灰不可点。
    // 用 trimmed 判断：只敲了几个空格不该算"有内容"
    m_searchButton->setEnabled(!m_searchEdit->text().trimmed().isEmpty());
}

// 回车 / 点「搜索」按钮的统一入口
void SearchWindow::onSearchTriggered()
{
    // 前后空格不参与搜索（复制粘贴时很容易多带一个空格）
    const QString keyword = m_searchEdit->text().trimmed();
    if (keyword.isEmpty()) {
        return;//空词不搜（按钮这时本来也是禁用态，这里属于兜底）
    }
    m_resultList->setLoading();//先把列表切到"正在搜索…"，等主后端的结果回来再填
    m_addResultLabel->hide();  //上一次那条"申请已发送"提示不该跨到新一轮搜索里
    emit searchRequested(keyword);
}

void SearchWindow::onClearButtonPressed()
{
    m_clearBtn->setIcon(QIcon(kClearIconClicked));
}

void SearchWindow::onClearButtonReleased()
{
    m_clearBtn->setIcon(QIcon(kClearIconNormal));
}

// 结果卡片点「添加」：不直接发申请，先弹"申请加好友"小窗让用户填留言（限 30 字）。
// 点「发送」（Accepted）才带留言往外抛；点「取消」什么都不做——申请不算发出去过
void SearchWindow::onAddRequested(const ContactInfo& contact)
{
    AddFriendDialog dialog(contact.id, contact.name, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    m_addResultLabel->hide();   // 新一轮申请：先把上一条提示收掉，等这次的回包再显示
    emit addFriendRequested(contact.id, dialog.message());
}

void SearchWindow::setSearchResults(const QList<ContactInfo>& results)
{
    m_resultList->setResults(results);
}

void SearchWindow::setSearchFailed(const QString& message)
{
    m_resultList->setError(message);
}

void SearchWindow::showAddFriendSuccess()
{
    showAddResult(true, QString());
}

void SearchWindow::showAddFriendFailed(const QString& message)
{
    showAddResult(false, message);
}

// 提示条的统一渲染：文案为空时用默认话术（服务器回包不带原因时也有话说）。
// 有意不做"几秒后自动消失"：那要引入定时器，还会被快速连点打断；
// 改成留着到下一次搜索或下一次「添加」，行为更可预期
void SearchWindow::showAddResult(bool success, const QString& message)
{
    QString text = message;
    if (text.isEmpty()) {
        text = success ? QStringLiteral("好友申请已发送，等待对方验证")
                       : QStringLiteral("好友申请发送失败");
    }
    // 与登录页 / 详情页同一套配色：成功用柔和绿，失败用柔和的半透明红
    m_addResultLabel->setStyleSheet(success
        ? "color: rgba(103, 194, 58, 0.9); font-size: 12px;"
        : "color: rgba(224, 91, 91, 0.85); font-size: 12px;");
    m_addResultLabel->setText(text);
    m_addResultLabel->setVisible(true);
}

// 切走搜索页时由 MainWindow 调用。
// 只清输入框：已经搜出来的结果列表留着，用户切回来还能接着看；
// 清除按钮的显隐、搜索按钮的置灰都会由 textChanged → onSearchTextChanged 自动跟上
void SearchWindow::clearSearch()
{
    m_searchEdit->clear();
}