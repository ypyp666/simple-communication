#ifndef MESSAGESTATUSINDICATOR_H
#define MESSAGESTATUSINDICATOR_H

#include <QWidget>
#include <QTimer>
#include "FeatureStructs.h"  // 复用 LoginFeature 枚举：指示器据此把重试路由到对应后端

// 消息状态指示器（自定义 QWidget）
// 两种状态：
//   Sending：发送中 → 旋转小圆圈动画
//   Failed ：发送失败（超时）→ 红色感叹号
// 直接自绘（paintEvent），不需要任何图片资源
// 可复用：消息气泡（MessageSend）、注册页账号行（Register）等都用同一个控件，
// 用 setFeature 标记"这块指示器属于哪个功能"，点击重试时把枚举一并 emit 出去
class MessageStatusIndicator : public QWidget
{
    Q_OBJECT
public:
    enum Status {
        None = 0,    // 无状态（隐藏）
        Sending = 1, // 发送中（旋转圆圈动画）
        Failed = 2   // 发送失败（红色感叹号）
    };

    explicit MessageStatusIndicator(QWidget *parent = nullptr);

    void setStatus(Status status);  // 切换状态
    void setFeature(LoginFeature feature);  // 设置所属功能（决定重试请求路由到哪个后端）

signals:
    // 发送失败状态下点击红色感叹号 → 请求重发；携带功能枚举供上层按枚举路由到对应后端
    void retryClicked(LoginFeature feature);

protected:
    void paintEvent(QPaintEvent *event) override;  // 重绘自己
    void mousePressEvent(QMouseEvent *event) override;  // 点击感叹号重发

private:
    Status m_status;
    LoginFeature m_feature = LoginFeature::None;  // 本指示器所属功能（复用点各自设置）
    QTimer m_timer;  // 旋转动画定时器
    int m_angle;     // 当前旋转角度（0~359）
};

#endif // MESSAGESTATUSINDICATOR_H
