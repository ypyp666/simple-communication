#ifndef ADDFRIENDDIALOG_H
#define ADDFRIENDDIALOG_H

#include <QDialog>
#include <QPoint>

class QLabel;
class QLineEdit;

// "申请加好友"小弹窗：搜索页结果卡片点「添加」后弹出。
// 内容只有三块（比 QQ 那个精简很多，去掉分组/好友权限/备注这些字段）：
//   头像 + 名字
//   留言输入框（限 30 字，选填）
//   发送 / 取消 两个按钮
//
// 做成无边框圆角卡片（像个小菜单，而不是系统风格的消息框），可按住空白处拖动。
// 用法：AddFriendDialog dialog(id, name, this); if (dialog.exec() == Accepted) { dialog.message() ... }
// 点「发送」→ accept()；点「取消」/ 关闭 → reject()。留言取出来由调用方一并转发给服务器
class AddFriendDialog : public QDialog
{
    Q_OBJECT
public:
    AddFriendDialog(const QString& contactId, const QString& contactName, QWidget* parent = nullptr);

    QString contactId() const { return m_contactId; }
    QString message() const;   // 用户填的留言（已去首尾空格）

protected:
    // 出现时居中到父窗口（无边框窗口 Qt 不会自动摆好看）
    void showEvent(QShowEvent* event) override;
    // 无边框窗口没有标题栏可拖：按住卡片空白处就能挪动它
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    QString m_contactId;
    QLineEdit* m_messageEdit = nullptr;
    QLabel* m_counterLabel = nullptr;   // 右下角"已输字数 / 上限"计数
    QPoint m_dragOffset;                // 拖动时鼠标相对窗口左上角的偏移
};

#endif // ADDFRIENDDIALOG_H