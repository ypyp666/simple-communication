#include "AgentApprovalCard.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

AgentApprovalCard::AgentApprovalCard(const AgentApprovalInfo& info, QWidget* parent)
    : QFrame(parent), m_info(info)
{
    setObjectName("agentApprovalCard");
    // 普通 QWidget / QFrame 的 QSS 背景和圆角默认不绘制，必须开这个属性
    setAttribute(Qt::WA_StyledBackground, true);
    setFrameShape(QFrame::NoFrame);
    setMaximumWidth(560);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(10);

    QLabel* title = new QLabel("需要你的确认", this);
    title->setObjectName("approvalTitle");
    root->addWidget(title);

    if (!info.summary.isEmpty()) {
        QLabel* summary = new QLabel(info.summary, this);
        summary->setObjectName("approvalSummary");
        summary->setWordWrap(true);
        root->addWidget(summary);
    }

    // 审批依据：服务端给什么就按顺序渲染什么，客户端不拼任何中文
    //（以后新增动作类型才不用改 Qt 这边）
    if (!info.detail.isEmpty()) {
        QGridLayout* detailGrid = new QGridLayout();
        detailGrid->setHorizontalSpacing(12);
        detailGrid->setVerticalSpacing(6);
        detailGrid->setColumnStretch(0, 0);
        detailGrid->setColumnStretch(1, 1);
        int row = 0;
        for (const AgentApprovalItem& item : info.detail) {
            QLabel* label = new QLabel(item.label, this);
            label->setObjectName("approvalDetailLabel");
            QLabel* value = new QLabel(item.value, this);
            value->setObjectName("approvalDetailValue");
            value->setWordWrap(true);
            detailGrid->addWidget(label, row, 0, Qt::AlignTop | Qt::AlignRight);
            detailGrid->addWidget(value, row, 1, Qt::AlignTop);
            ++row;
        }
        root->addLayout(detailGrid);
    }

    if (!info.steps.isEmpty()) {
        QString stepsText;
        for (const QString& step : info.steps) {
            stepsText += QString("· %1\n").arg(step);
        }
        QLabel* steps = new QLabel(stepsText.trimmed(), this);
        steps->setObjectName("approvalSteps");
        steps->setWordWrap(true);
        root->addWidget(steps);
    }

    QHBoxLayout* buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(8);
    // 状态放左边（失败原因 / 已提交提示），按钮靠右
    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName("approvalStatus");
    m_statusLabel->setWordWrap(true);
    buttonRow->addWidget(m_statusLabel, 1);

    m_rejectBtn = new QPushButton("拒绝", this);
    m_rejectBtn->setObjectName("approvalReject");
    m_rejectBtn->setCursor(Qt::PointingHandCursor);
    m_rejectBtn->setFocusPolicy(Qt::NoFocus);

    m_approveBtn = new QPushButton("同意", this);
    m_approveBtn->setObjectName("approvalApprove");
    m_approveBtn->setCursor(Qt::PointingHandCursor);
    m_approveBtn->setFocusPolicy(Qt::NoFocus);
    // detail 为空 = 没有可读的审批依据，不允许盲签
    m_approveBtn->setEnabled(!info.detail.isEmpty());
    if (info.detail.isEmpty()) {
        m_approveBtn->setToolTip("服务端没有提供可审阅的内容，无法同意");
    }

    buttonRow->addWidget(m_rejectBtn);
    buttonRow->addWidget(m_approveBtn);
    root->addLayout(buttonRow);

    connect(m_approveBtn, &QPushButton::clicked, this, &AgentApprovalCard::onApproveClicked);
    connect(m_rejectBtn, &QPushButton::clicked, this, &AgentApprovalCard::onRejectClicked);

    setStyleSheet(R"(
        QFrame#agentApprovalCard {
            background-color: #ffffff;
            border: 1px solid #d9e2ec;
            border-radius: 12px;
        }
        QLabel#approvalTitle {
            color: #1f2d3d;
            font-size: 14px;
            font-weight: 600;
        }
        QLabel#approvalSummary {
            color: #1f2d3d;
            font-size: 13px;
        }
        QLabel#approvalDetailLabel {
            color: #8a94a6;
            font-size: 13px;
        }
        QLabel#approvalDetailValue {
            color: #1f2d3d;
            font-size: 13px;
        }
        QLabel#approvalSteps {
            color: #8a94a6;
            font-size: 12px;
        }
        QLabel#approvalStatus {
            color: #8a94a6;
            font-size: 12px;
        }
        QPushButton#approvalApprove {
            background-color: #4a90d9;
            color: #ffffff;
            border: none;
            border-radius: 6px;
            padding: 6px 18px;
            font-size: 13px;
        }
        QPushButton#approvalApprove:hover {
            background-color: #3d7fc4;
        }
        QPushButton#approvalApprove:disabled {
            background-color: #b7c8d8;
        }
        QPushButton#approvalReject {
            background-color: #f2f5f9;
            color: #43506b;
            border: 1px solid #d9e2ec;
            border-radius: 6px;
            padding: 6px 18px;
            font-size: 13px;
        }
        QPushButton#approvalReject:hover {
            background-color: #e8eef5;
        }
        QPushButton#approvalReject:disabled {
            color: #b0b8c6;
            background-color: #f7f9fb;
        }
    )");
}

void AgentApprovalCard::onApproveClicked()
{
    m_approvedChosen = true;
    setSubmitted();
    emit decisionMade(m_info.serverId, true);
}

void AgentApprovalCard::onRejectClicked()
{
    m_approvedChosen = false;
    setSubmitted();
    emit decisionMade(m_info.serverId, false);
}

void AgentApprovalCard::setSubmitted()
{
    m_approveBtn->setEnabled(false);
    m_rejectBtn->setEnabled(false);
    m_statusLabel->setStyleSheet(QString());
    m_statusLabel->setText("已提交，等待服务器确认…");
}

void AgentApprovalCard::showSubmitError(const QString& message)
{
    m_statusLabel->setStyleSheet("color: #c0392b;");
    m_statusLabel->setText(message);
    // 解锁：超时 / 失败后必须还能重试，不能永久禁着
    m_rejectBtn->setEnabled(true);
    m_approveBtn->setEnabled(!m_info.detail.isEmpty());
}
