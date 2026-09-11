#include "RegisterPage.h"
#include "GlassCard.h"   // 毛玻璃卡片（忘记密码页/注册页共用的真·模糊背景）
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QMessageBox>
#include <QSizePolicy>

RegisterPage::RegisterPage(QWidget *parent)
    : QWidget(parent)
{
    setupUI();
}

void RegisterPage::setupUI()
{
    // ========== 注册页自己的根布局 ==========
    // 与忘记密码页同一套布局骨架：首尾弹性垫片（2:3）让内容整体居中、重心略偏上，
    // 富余高度不渗进卡片把表单行拉开
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setSpacing(0);
    layout->setContentsMargins(20, 28, 20, 28);
    layout->addStretch(2);

    // 页标题
    QLabel* titleLabel = new QLabel("注册账号", this);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setStyleSheet("QLabel { color: #333333; font-size: 20px; font-weight: bold; }");
    layout->addWidget(titleLabel);
    layout->addSpacing(14);

    // 毛玻璃卡片：与忘记密码页共用的 GlassCard（截屏+高斯模糊垫底）
    GlassCard* card = new GlassCard(this);
    card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addWidget(card);
    layout->addSpacing(18);

    QVBoxLayout* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(18, 22, 18, 22);
    cardLayout->setSpacing(14);

    // 输入框统一半透明白：透出底下毛玻璃卡片（纯白底会整个盖住卡片，毛玻璃白做）
    const QString editStyle = R"(
        QLineEdit {
            border-radius: 8px;
            border: none;
            padding-left: 15px;
            font-size: 14px;
            background-color: rgba(255, 255, 255, 0.72);
        }
    )";

    // 账号输入框
    QLineEdit* regAccountEdit = new QLineEdit(card);
    regAccountEdit->setPlaceholderText("请输入账号");
    regAccountEdit->setFixedHeight(45);
    regAccountEdit->setStyleSheet(editStyle);
    cardLayout->addWidget(regAccountEdit);

    // 密码输入框
    QLineEdit* regPasswordEdit = new QLineEdit(card);
    regPasswordEdit->setPlaceholderText("请输入密码");
    regPasswordEdit->setFixedHeight(45);
    regPasswordEdit->setEchoMode(QLineEdit::Password);
    regPasswordEdit->setStyleSheet(editStyle);
    cardLayout->addWidget(regPasswordEdit);

    // 确认密码输入框
    QLineEdit* regConfirmEdit = new QLineEdit(card);
    regConfirmEdit->setPlaceholderText("请再次输入密码");
    regConfirmEdit->setFixedHeight(45);
    regConfirmEdit->setEchoMode(QLineEdit::Password);
    regConfirmEdit->setStyleSheet(editStyle);
    cardLayout->addWidget(regConfirmEdit);

    // 注册按钮（先做个占位效果，具体逻辑后面再接后端）
    QPushButton* regSubmitBtn = new QPushButton("注册", card);
    regSubmitBtn->setFixedHeight(45);
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
    cardLayout->addWidget(regSubmitBtn);

    // 返回登录按钮（透明文字链接，点击通知容器切回登录页）
    QPushButton* backToLoginBtn = new QPushButton("返回登录", this);
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
    connect(regSubmitBtn, &QPushButton::clicked, this, [=]() {
        QMessageBox::information(this, "提示", "注册功能开发中");
    });
    connect(backToLoginBtn, &QPushButton::clicked, this, &RegisterPage::backToLoginRequested);
}