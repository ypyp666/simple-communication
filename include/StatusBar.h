#ifndef STATUSBAR_H
#define STATUSBAR_H

#include <QWidget>
#include <QPushButton>
#include <QVector>

class QLabel;

class StatusBar : public QWidget
{
    Q_OBJECT
public:
    explicit StatusBar(QWidget *parent = nullptr);

signals:
    // 用户点击导航按钮 → 通知 MainWindow 切换右侧页面栈（信号函数体由 moc 生成，无需实现）
    void m_changePage(int index);

public slots:
    // 切换选中的导航项（按钮图标随之黑/蓝切换）
    void setCurrentIndex(int index);
    // 在指定导航按钮右上角挂一个红色角标（未读消息数 / 好友申请数）。
    // count<=0 时隐藏；超过 99 显示 "99+"，避免角标被撑得过宽
    void setBadge(int index, int count);

private:
    QVector<QPushButton*> m_navButtons;  // 导航按钮，顺序与 MainWindow 页面栈一致
    QVector<QLabel*> m_badges;           // 角标，与 m_navButtons 一一对应（下标同含义）
};

#endif // STATUSBAR_H