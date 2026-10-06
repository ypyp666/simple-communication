#ifndef AGENTWINDOW_H
#define AGENTWINDOW_H

#pragma once
#include <QWidget>
#include <QVBoxLayout>

class ChatArea;
class MessageList;
class QPushButton;
class QPropertyAnimation;

class AgentWindow : public QWidget {
    Q_OBJECT
public:
    AgentWindow(QWidget *parent = nullptr);
    ~AgentWindow();

protected:
    // 浮层没进布局、靠绝对定位，所以用事件过滤器盯两件事：
    //   ① 感应条(hotZone)的 Enter/Leave → 决定手柄显隐
    //   ② 聊天区尺寸变化（首次布局、窗口缩放、切页）→ 重新贴边摆放浮层
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onToggleDrawer();        // 点击手柄：拉出 / 收起抽屉
    void onDrawerAnimFinished();  // 动画结束：收起时把抽屉彻底藏掉、恢复感应条
    void updateHandlePosition();  // 手柄 x 跟随抽屉右边缘（动画每一帧都会被调）

private:
    void setupOverlays();      // 创建 感应条 / 手柄 / 抽屉 三个浮层
    void layoutOverlays();     // 按聊天区的几何重新摆放它们
    void updateHandleVisible();// 手柄显隐的唯一判断入口（把所有状态分支收敛到这一处）
    bool mouseInsideHotZone() const;

    QVBoxLayout* mainLayout;
    ChatArea* chatArea;
    QWidget* hotZone;                 // 聊天区左边缘的透明感应条：只判断"鼠标进没进来"
    QPushButton* handleBtn;           // ">" 手柄（黑色半透明细框）
    QWidget* drawer;                  // 会话列表抽屉（平时躲在左侧外面，点手柄滑出来）
    MessageList* sessionList;         // 抽屉里的会话列表（列表数据逻辑后续再补）
    QPropertyAnimation* drawerAnim;   // 抽屉滑入/滑出动画
    bool m_drawerOpen = false;        // 抽屉当前是拉出还是收起
};

#endif // AGENTWINDOW_H