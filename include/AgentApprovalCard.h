#ifndef AGENTAPPROVALCARD_H
#define AGENTAPPROVALCARD_H

#include <QFrame>

#include "FeatureStructs.h"   // AgentApprovalInfo

class QLabel;
class QPushButton;

// 审批卡片：服务端下发 agent_approval_required 时插在对话里。
// 三样内容分工固定，不能混着用：
//   summary —— 一句话主内容（模型写的）
//   detail  —— 审批依据，按数组顺序渲染成两列（服务端确定性生成的「操作 / 对象 / 内容 / 时间 / 风险」）
//   steps   —— 分步说明，小字（模型写的）
// detail 为空时禁用「同意」：服务端没生成出可读内容，让用户盲签没有意义
class AgentApprovalCard : public QFrame
{
    Q_OBJECT
public:
    explicit AgentApprovalCard(const AgentApprovalInfo& info, QWidget* parent = nullptr);

    QString serverId() const { return m_info.serverId; }
    QString threadId() const { return m_info.threadId; }
    // 用户当时点的是同意还是拒绝（回执成功后要把这个结果写成一行小字）
    bool approvedChosen() const { return m_approvedChosen; }

    // 回执已经发出去：两颗按钮都禁用，状态改成「已提交，等待服务器确认…」
    void setSubmitted();
    // 回执失败 / 超时：解锁按钮并把原因显示出来——不能永久禁用，
    // 否则用户既关不掉也重试不了
    void showSubmitError(const QString& message);

signals:
    // 只是"用户点了哪个"，不直接改任何本地数据
    void decisionMade(const QString& serverId, bool approved);

private slots:
    void onApproveClicked();
    void onRejectClicked();

private:
    AgentApprovalInfo m_info;
    QPushButton* m_approveBtn = nullptr;
    QPushButton* m_rejectBtn = nullptr;
    QLabel* m_statusLabel = nullptr;
    bool m_approvedChosen = false;
};

#endif // AGENTAPPROVALCARD_H
