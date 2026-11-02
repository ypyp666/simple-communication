#ifndef AGENTSESSIONLIST_H
#define AGENTSESSIONLIST_H

#include <QWidget>
#include <QScrollArea>
#include <QList>
#include <QString>

#include "FeatureStructs.h"   // ConversationInfo（会话数据）

class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

// 抽屉里的一行会话：**不复用聊天页的 MessageList**。
// 聊天页那套有头像、最后一条消息、时间、未读红点，智能体会话不需要这些，
// 这里只留：会话名 + 重命名按钮 + 删除按钮，整行是圆角块。
// 点行 = 选中会话；点笔 = 就地改名（回车/失焦提交，Esc 取消）；点叉 = 请求删除
class AgentSessionRow : public QWidget
{
    Q_OBJECT
public:
    AgentSessionRow(const QString& sessionId, const QString& title, QWidget* parent = nullptr);

    QString sessionId() const { return m_sessionId; }
    QString title() const { return m_title; }
    void setSelected(bool selected);   // 由列表统一维护"当前会话"的选中态
    // 改名被服务器拒绝/超时：把显示回滚成上一次确认过的名字（本地库没改过，只回滚界面）
    void revertTitle();

signals:
    void clicked(const QString& sessionId);
    void renameRequested(const QString& sessionId, const QString& title);
    void deleteRequested(const QString& sessionId);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    // 盯着行内的改名输入框：回车提交、失焦提交、Esc 取消
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void startRename();
    void commitRename();
    void cancelRename();
    void applyRowStyle();               // 按 选中/悬浮 状态重刷圆角底色
    void updateElidedTitle();           // 名字太长按标签实际宽度省略成 "..."

    QString m_sessionId;
    QString m_title;                    // 行上正在显示的（改名的乐观显示也是它）
    QString m_confirmedTitle;           // 服务器确认过的名字，改名失败时回滚到它
    QLabel* m_nameLabel = nullptr;      // 平时显示的名字
    QLineEdit* m_nameEdit = nullptr;    // 改名时顶替名字显示（默认隐藏）
    QPushButton* m_renameBtn = nullptr;
    QPushButton* m_deleteBtn = nullptr;
    bool m_selected = false;
    bool m_hovered = false;
    bool m_renaming = false;            // 防止 Esc 取消触发的失焦又去提交一次
};

// 智能体抽屉的会话列表：滚动区 + 垂直排的行（每行一个 AgentSessionRow）。
// 用滚动区而不是 QListWidget：行内要放可点按钮、还要就地改名的输入框，
// 交给自绘的行控件比跟 item widget 的事件转发较劲省事
class AgentSessionList : public QScrollArea
{
    Q_OBJECT
public:
    explicit AgentSessionList(QWidget* parent = nullptr);

    // 整表刷新（本地库读回 / 新建 / 改名 / 删除后重拉都走它）
    void setSessions(const QList<ConversationInfo>& sessions);
    // 标出当前会话（列表重建后仍保留选中态）
    void setCurrentSessionId(const QString& sessionId);
    // 改名被服务器拒绝/超时：把那一行的显示回滚成旧名字（本地库没动过）
    void revertTitle(const QString& sessionId);

signals:
    void sessionSelected(const QString& sessionId, const QString& sessionName);
    void renameRequested(const QString& sessionId, const QString& title);
    // 删除请求：只上抛给上层，真正删数据由后台 DB 线程做
    void deleteRequested(const QString& sessionId);

private slots:
    void onRowClicked(const QString& sessionId);

private:
    void clearRows();

    QWidget* m_container = nullptr;
    QVBoxLayout* m_rowsLayout = nullptr;
    QList<AgentSessionRow*> m_rows;
    QString m_currentSessionId;
};

#endif // AGENTSESSIONLIST_H
