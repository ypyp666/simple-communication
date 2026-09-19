#ifndef STATUSBAR_H
#define STATUSBAR_H

#include <QWidget>
#include <QPushButton>
#include <QVector>

class StatusBar : public QWidget
{
    Q_OBJECT
public:
    explicit StatusBar(QWidget *parent = nullptr);

public slots:
    // 切换选中的导航项（按钮图标随之黑/蓝切换）
    void setCurrentIndex(int index);

private:
    QVector<QPushButton*> m_navButtons;  // 导航按钮，顺序与 MainWindow 页面栈一致
};

#endif // STATUSBAR_H