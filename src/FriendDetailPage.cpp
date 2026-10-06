#include "FriendDetailPage.h"
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>

namespace {
// 尺寸常量集中在这里。除弹簧以外的东西都是固定尺寸/固定高度，
// 这样"固定总高"才立得住：窗口变高变矮，受影响的只有弹簧那一块
constexpr int kPageMargin    = 16;   // 整页四周留白
constexpr int kModifyBtnW    = 76;
constexpr int kModifyBtnH    = 28;
constexpr int kAvatarSize    = 72;   // 头像正方形边长（圆角取一半就是正圆）
constexpr int kInfoRowH      = 24;   // 每行资料的高度
constexpr int kInfoRowGap    = 8;    // 行与行之间的空隙
constexpr int kTopGap        = 12;   // 顶部按钮行 → 头像 的距离
constexpr int kAvatarGap     = 20;   // 头像 → 三行资料 的距离
constexpr int kButtonW       = 96;   // 底部两个按钮的宽度（固定，不给拉长）
constexpr int kButtonH       = 34;   // 底部两个按钮的高度
constexpr int kButtonGap     = 12;
constexpr int kHintH         = 20;   // 改备注失败提示行的高度（平时留空，占位高度固定）

// 三行共用的一套字：前缀（"用户名："）和内容同一字号，只靠颜色区分
const char* kFieldLabelStyle = "font-size: 14px; color: #333;";
const char* kFieldPrefixStyle = "font-size: 14px; color: #999;";

// 右上角"修改备注 / 保存备注"：幽灵按钮，默认灰描边，悬浮转蓝
const char* kModifyBtnStyle = R"(
    QPushButton {
        background-color: white;
        color: #666;
        font-size: 12px;
        border: 1px solid #d0d0d0;
        border-radius: 6px;
    }
    QPushButton:hover {
        color: #4a90d9;
        border: 1px solid #4a90d9;
    }
    QPushButton:pressed {
        background-color: #f0f6fc;
    }
)";

// 「发送消息」：主按钮，蓝色，与登录页 / 搜索结果列表同一个色号
const char* kSendBtnStyle = R"(
    QPushButton {
        background-color: #4a90d9;
        color: white;
        font-size: 13px;
        border: none;
        border-radius: 6px;
    }
    QPushButton:hover   { background-color: #3a80c9; }
    QPushButton:pressed { background-color: #2f70b5; }
)";

// 「删除好友」：沿用项目里那套柔和红（淡红内里 + 半透明红框 + 红字），
// 不做成大红实心块——这是个需要"想一下"的危险操作，不适合做得太抢眼好按
const char* kDeleteBtnStyle = R"(
    QPushButton {
        background-color: rgba(224, 91, 91, 0.10);
        color: rgba(224, 91, 91, 0.95);
        font-size: 13px;
        border: 1px solid rgba(224, 91, 91, 0.45);
        border-radius: 6px;
    }
    QPushButton:hover {
        background-color: rgba(224, 91, 91, 0.18);
    }
    QPushButton:pressed {
        background-color: rgba(224, 91, 91, 0.26);
    }
)";

// 备注那行：看的时候要"像一句普通文字"，改的时候才该有输入框的样子。
// 所以做成两张表，由 enterRemarkEditMode() 来回切
const char* kRemarkReadStyle = R"(
    QLineEdit {
        border: none;
        background-color: transparent;
        font-size: 14px;
        color: #333;
    }
)";
const char* kRemarkEditStyle = R"(
    QLineEdit {
        border: 1px solid #4a90d9;
        background-color: white;
        border-radius: 6px;
        padding: 0 8px;
        font-size: 14px;
        color: #333;
    }
)";

// 改备注失败的红字提示：沿用项目里那套柔和红，只做文字不做弹窗
const char* kHintStyle = "font-size: 12px; color: rgba(224, 91, 91, 0.95);";
}

FriendDetailPage::FriendDetailPage(QWidget *parent) : QWidget(parent)
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(kPageMargin, kPageMargin, kPageMargin, kPageMargin);
    // 间距统一设 0，需要留白的地方用 addSpacing() 显式给固定值。
    // 这样"每段留白多少"在代码里是看得见的数字，不会被布局的自动间距偷偷改掉
    mainLayout->setSpacing(0);

    // ===== 第一行：右上角的「修改备注」=====
    m_modifyRemarkBtn = new QPushButton("修改备注", this);
    m_modifyRemarkBtn->setFixedSize(kModifyBtnW, kModifyBtnH);
    m_modifyRemarkBtn->setCursor(Qt::PointingHandCursor);
    m_modifyRemarkBtn->setFocusPolicy(Qt::NoFocus);//不参与 Tab 焦点循环
    m_modifyRemarkBtn->setStyleSheet(kModifyBtnStyle);

    QHBoxLayout* topLayout = new QHBoxLayout();
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(0);
    topLayout->addStretch();//弹簧：把按钮顶到右边去
    topLayout->addWidget(m_modifyRemarkBtn);
    mainLayout->addLayout(topLayout);

    // ===== 头像（居中）=====
    mainLayout->addSpacing(kTopGap);

    m_avatarLabel = new QLabel(this);
    m_avatarLabel->setFixedSize(kAvatarSize, kAvatarSize);
    m_avatarLabel->setAlignment(Qt::AlignCenter);
    m_avatarLabel->setStyleSheet(QString(R"(
        QLabel {
            border-radius: %1px;
            background-color: #67c23a;
            color: white;
            font-size: 28px;
            font-weight: 600;
        }
    )").arg(kAvatarSize / 2));
    // 第二个参数 0 = 不给拉伸权重，第三个参数 = 水平居中；
    // 只要不给权重，这一项就永远不会被拉高
    mainLayout->addWidget(m_avatarLabel, 0, Qt::AlignHCenter);

    // ===== 三行资料（整页左对齐；头顶那个头像单独居中，两者互不影响）=====
    mainLayout->addSpacing(kAvatarGap);

    m_nameLabel = new QLabel(this);
    m_nameLabel->setFixedHeight(kInfoRowH);
    m_nameLabel->setStyleSheet(kFieldLabelStyle);
    mainLayout->addWidget(m_nameLabel);

    mainLayout->addSpacing(kInfoRowGap);

    m_idLabel = new QLabel(this);
    m_idLabel->setFixedHeight(kInfoRowH);
    m_idLabel->setStyleSheet(kFieldLabelStyle);
    mainLayout->addWidget(m_idLabel);

    mainLayout->addSpacing(kInfoRowGap);

    // 备注行：前缀是个单独的 QLabel，右边才是可编辑的内容。
    // 分开写是因为"备注："三个字不能被编辑（否则用户能把前缀删掉），
    // 只有内容该进 QLineEdit
    QWidget* remarkRow = new QWidget(this);
    remarkRow->setFixedHeight(kInfoRowH);
    QHBoxLayout* remarkLayout = new QHBoxLayout(remarkRow);
    remarkLayout->setContentsMargins(0, 0, 0, 0);
    remarkLayout->setSpacing(0);

    QLabel* remarkPrefix = new QLabel("备注：", remarkRow);
    remarkPrefix->setStyleSheet(kFieldPrefixStyle);
    remarkLayout->addWidget(remarkPrefix);

    m_remarkEdit = new QLineEdit(remarkRow);
    m_remarkEdit->setPlaceholderText("未设置");
    m_remarkEdit->setStyleSheet(kRemarkReadStyle);
    remarkLayout->addWidget(m_remarkEdit, 1);

    mainLayout->addWidget(remarkRow);

    // 改备注失败时的红字提示。固定高度、平时留空（不用 hide），
    // 这样出提示时不会把上面的内容顶动——版面总高依然固定，伸缩的只有下面那根弹簧
    m_hintLabel = new QLabel(this);
    m_hintLabel->setFixedHeight(kHintH);
    m_hintLabel->setStyleSheet(kHintStyle);
    mainLayout->addWidget(m_hintLabel);

    // ===== 弹簧：只这一块会伸缩，把下面两个按钮压到底部 =====
    mainLayout->addStretch();

    // ===== 底部两个按钮 =====
    m_sendMsgBtn = new QPushButton("发送消息", this);
    m_sendMsgBtn->setFixedSize(kButtonW, kButtonH);
    m_sendMsgBtn->setCursor(Qt::PointingHandCursor);
    m_sendMsgBtn->setFocusPolicy(Qt::NoFocus);
    m_sendMsgBtn->setStyleSheet(kSendBtnStyle);

    m_deleteFriendBtn = new QPushButton("删除好友", this);
    m_deleteFriendBtn->setFixedSize(kButtonW, kButtonH);
    m_deleteFriendBtn->setCursor(Qt::PointingHandCursor);
    m_deleteFriendBtn->setFocusPolicy(Qt::NoFocus);
    m_deleteFriendBtn->setStyleSheet(kDeleteBtnStyle);

    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(kButtonGap);
    // 三个弹簧把两个按钮"散开"摆在这一行里：弹簧 发送消息 弹簧 删除好友 弹簧。
    // 按钮都是固定尺寸、不给拉伸权重，所以长度只由 kButtonW 说了算，不会被拉长
    buttonLayout->addStretch();
    buttonLayout->addWidget(m_sendMsgBtn);
    buttonLayout->addStretch();
    buttonLayout->addWidget(m_deleteFriendBtn);
    buttonLayout->addStretch();
    mainLayout->addLayout(buttonLayout);

    // ===== 信号连接 =====
    connect(m_modifyRemarkBtn, &QPushButton::clicked, this, &FriendDetailPage::onModifyRemarkClicked);

    // 一层 lambda 只为带个参数出去：把 m_contactId 交给信号。
    // 空 ID 说明还没人被选中（比如 clearContact 之后），此时按了也不该发信号
    connect(m_sendMsgBtn, &QPushButton::clicked, this, [this]() {
        if (!m_contactId.isEmpty()) {
            emit sendMessageRequested(m_contactId, m_contactName);
        }
    });
    connect(m_deleteFriendBtn, &QPushButton::clicked, this, [this]() {
        if (!m_contactId.isEmpty()) {
            emit deleteFriendRequested(m_contactId);
        }
    });

    enterRemarkEditMode(false);//开机默认是"看备注"状态
    clearContact();
}

void FriendDetailPage::setContact(const ContactInfo& contact)
{
    m_contactId = contact.id;
    m_contactName = contact.name;   // 发送消息打开会话时要用（会话标题靠它）
    m_remark = contact.remark;      // 这份是"已确认"的备注（改备注成功后才由回包更新它）
    m_pendingRemark.clear();        // 换了人，上一位还没回包的新备注作废

    m_avatarLabel->setText(contact.name.left(1));//头像里放名字首字（与会话列表 / 好友申请列表同一套路）
    m_nameLabel->setText("用户名：" + contact.name);
    m_idLabel->setText("用户ID：" + contact.id);
    m_remarkEdit->setText(contact.remark);
    m_hintLabel->clear();

    // 换好友时务必把编辑态退掉：上一个人的"半截备注"不该留在新好友身上
    enterRemarkEditMode(false);
}

void FriendDetailPage::clearContact()
{
    m_contactId.clear();
    m_contactName.clear();
    m_remark.clear();
    m_pendingRemark.clear();
    m_avatarLabel->clear();
    m_nameLabel->setText("用户名：");
    m_idLabel->setText("用户ID：");
    m_remarkEdit->clear();
    m_hintLabel->clear();
    enterRemarkEditMode(false);
}

void FriendDetailPage::onModifyRemarkClicked()
{
    if (!m_remarkEditing) {
        // 进编辑态：先把上一次的失败提示清掉，光标落到备注上并全选，用户直接敲字就能覆盖
        m_hintLabel->clear();
        enterRemarkEditMode(true);
        m_remarkEdit->setFocus();
        m_remarkEdit->selectAll();
        return;
    }

    // 退出编辑态 = 保存。前后空格去掉（备注末尾多一个空格属于手滑，不是内容）
    const QString remark = m_remarkEdit->text().trimmed();
    enterRemarkEditMode(false);
    m_hintLabel->clear();

    // 保存后【不】把显示换成新值：备注要等服务器确认成功才更新（见 onRemarkSaveSuccess）。
    // 先把输入框退回已确认的旧值，避免"服务器还没答应，界面就显示改好了"
    m_remarkEdit->setText(m_remark);

    if (!m_contactId.isEmpty()) {
        m_pendingRemark = remark;   // 记下"正在等服务器确认"的新备注
        emit remarkSaved(m_contactId, remark);
    }
}

// 服务器确认改备注成功：这时才把界面从旧值刷成新值。
// 回包对应的是不是当前显示的好友要判一下——快速切换好友时，慢一拍的回包不能盖到新人身上
void FriendDetailPage::onRemarkSaveSuccess(const QString& contactId, const QString& remark)
{
    if (contactId != m_contactId) {
        return;
    }
    m_remark = remark;
    m_remarkEdit->setText(remark);
    m_pendingRemark.clear();
    m_hintLabel->clear();
}

// 改备注失败：备注保持旧值（保存时就没动显示），这里只在页面上挂一条红字提示
void FriendDetailPage::onRemarkSaveFailed(const QString& contactId, const QString& message)
{
    if (contactId != m_contactId) {
        return;
    }
    m_pendingRemark.clear();
    m_hintLabel->setText(message.isEmpty() ? "备注修改失败，请重试" : message);
}

void FriendDetailPage::enterRemarkEditMode(bool editing)
{
    m_remarkEditing = editing;
    m_remarkEdit->setReadOnly(!editing);
    m_remarkEdit->setStyleSheet(editing ? kRemarkEditStyle : kRemarkReadStyle);
    // 同一个按钮两副面孔：编辑态下它变成"保存备注"，用户才知道再点一下是保存
    m_modifyRemarkBtn->setText(editing ? "保存备注" : "修改备注");
}