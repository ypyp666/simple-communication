#ifndef AGENTWINDOW_H
#define AGENTWINDOW_H

#pragma once
#include <QWidget>
#include <QVBoxLayout>
#include <QList>

#include "FeatureStructs.h"   // ConversationInfo（会话列表数据）

class AgentChatArea;
class AgentSessionList;
class QPushButton;
class QPropertyAnimation;
class QHideEvent;
class QLabel;
class QTimer;
class MainBackend;   // 前置声明：只需指针，无需完整定义

class AgentWindow : public QWidget {
    Q_OBJECT
public:
    AgentWindow(QWidget *parent = nullptr, MainBackend* backend = nullptr);
    ~AgentWindow();

protected:
    // 浮层没进布局、靠绝对定位，所以用事件过滤器盯两件事：
    //   ① 感应条(hotZone)的 Enter/Leave → 决定手柄显隐
    //   ② 聊天区尺寸变化（首次布局、窗口缩放、切页）→ 重新贴边摆放浮层
    bool eventFilter(QObject* watched, QEvent* event) override;
    // 离开智能体页（切到别的页面 / 窗口被藏起来）：本轮回答直接结束。
    // 按钮是复用的，"回答中"状态必须跟着页面走，不能带到别的页面去
    void hideEvent(QHideEvent* event) override;

private slots:
    void onToggleDrawer();        // 点击手柄：拉出 / 收起抽屉
    void onDrawerAnimFinished();  // 动画结束：收起时把抽屉彻底藏掉、恢复感应条
    void updateHandlePosition();  // 手柄 x 跟随抽屉右边缘（动画每一帧都会被调）
    // 抽屉里的会话列表回到（本地库读 / 新建会话后重拉）→ 喂给 AgentSessionList
    void onAgentSessionsLoaded(const QList<ConversationInfo>& sessions);
    // 点击抽屉里某个会话：切到该会话（本会话的消息暂不回转，只切当前会话 ID）
    void onSessionSelected(const QString& sessionId, const QString& sessionName);
    // 行内点笔改名：交给主后端 → 后台 DB 线程写库，写完自动重拉列表
    void onSessionRenameRequested(const QString& sessionId, const QString& title);
    // 行内点叉删除：先交给主后端发服务器；确认成功后才删本地、当前会话才回空态
    void onSessionDeleteRequested(const QString& sessionId);
    // 聊天区要求结束本轮（点 stop / 切会话 / 离开页面）：交给主后端发 agent_stop
    void onStopAnswerRequested(const QString& sessionId);
    // === 会话新建的两段（本地先落库 → 服务器回执）===
    void onSessionCreateRequested(const QString& sessionId, const QString& title);
    void onSessionCreateSucceeded(const QString& sessionId, const QString& title);
    void onSessionCreateFailed(const QString& message);
    // === 会话改名 / 删除的失败分支（成功分支由主后端直接写库 + 重拉列表）===
    void onSessionRenameFailed(const QString& sessionId, const QString& message);
    void onSessionDeleteSucceeded(const QString& sessionId);
    void onSessionDeleteFailed(const QString& sessionId, const QString& message);
    void onToastTimeout();

private:
    void setupOverlays();      // 创建 感应条 / 手柄 / 抽屉 三个浮层
    void layoutOverlays();     // 按聊天区的几何重新摆放它们
    void updateHandleVisible();// 手柄显隐的唯一判断入口（把所有状态分支收敛到这一处）
    bool mouseInsideHotZone() const;
    // 底部小提示条：建/改名/删失败时弹出来，5 秒后自己消失（不用点确定）
    void showToast(const QString& text);

    QVBoxLayout* mainLayout;
    AgentChatArea* chatArea;
    QWidget* hotZone;                 // 聊天区左边缘的透明感应条：只判断"鼠标进没进来"
    QPushButton* handleBtn;           // ">" 手柄（黑色半透明细框）
    QWidget* drawer;                  // 会话列表抽屉（平时躲在左侧外面，点手柄滑出来）
    AgentSessionList* sessionList;    // 抽屉里的会话列表（名字 + 改名/删除按钮，行圆角）
    QPropertyAnimation* drawerAnim;   // 抽屉滑入/滑出动画
    bool m_drawerOpen = false;        // 抽屉当前是拉出还是收起
    MainBackend* backend;             // 主后端（智能体会话/消息都只经过它）
    // 正在等服务器确认的"新建会话"的本地 ID：回包时靠它决定要不要跳进新页面
    //（期间用户点了别的会话就清掉——用户已经走了，别再把页面抢回来）
    QString m_pendingCreateSessionId;
    QLabel* m_toast = nullptr;        // 右下角小提示（创建/改名/删除失败时弹 5 秒）
    QTimer* m_toastTimer = nullptr;
};

#endif // AGENTWINDOW_H