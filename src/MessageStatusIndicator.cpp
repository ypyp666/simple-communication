#include "MessageStatusIndicator.h"
#include <QPainter>
#include <QPen>
#include <QMouseEvent>

MessageStatusIndicator::MessageStatusIndicator(QWidget *parent)
    : QWidget(parent), m_status(None), m_angle(0)
{
    setFixedSize(18, 18);  // 指示器大小

    // 旋转动画：每 50ms 转 30°，360° 一圈循环
    m_timer.setInterval(50);
    connect(&m_timer, &QTimer::timeout, this, [this]() {
        m_angle = (m_angle + 30) % 360;
        update();  // 触发重绘 paintEvent
    });

    hide();  // 默认隐藏，只有发送中/失败才显示
}

void MessageStatusIndicator::setStatus(Status status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;

    if (status == Sending) {
        m_angle = 0;      // 每次进入发送中，从 0° 重新转
        m_timer.start();  // 启动旋转动画
        show();
        unsetCursor();    // 发送中不需要手型
    } else if (status == Failed) {
        m_timer.stop();   // 停止动画，显示静止的红色感叹号
        show();
        setCursor(Qt::PointingHandCursor);  // 失败时可点击重发，悬停显示手型
    } else {
        m_timer.stop();
        hide();
        unsetCursor();
    }
    update();
}

// 设置本指示器所属的功能：点击重试时随 retryClicked 一起发出去，
// 上层按枚举把请求路由到对应后端（气泡=MessageSend，注册页账号行=Register）
void MessageStatusIndicator::setFeature(LoginFeature feature)
{
    m_feature = feature;
}

// 点击红色感叹号 → 触发重发（仅在发送失败状态下有效）
void MessageStatusIndicator::mousePressEvent(QMouseEvent *event)
{
    if (m_status == Failed && event->button() == Qt::LeftButton) {
        emit retryClicked(m_feature);
    }
    QWidget::mousePressEvent(event);
}

void MessageStatusIndicator::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);  // 抗锯齿，让圆圈/感叹号边缘平滑

    const int size = qMin(width(), height());
    // 生成一个正方形区域，保证就算控件宽高不一致，绘图区域也是居中正方形
    QRectF rect((width() - size) / 2.0, (height() - size) / 2.0, size, size);//居中公式

    if (m_status == Sending) {
        // ===== 发送中：旋转小圆圈（一段弧线，缺口让它看起来在转） =====
        // 颜色 = 主题蓝 #4a90d9 压到很低的透明度（alpha 80 ≈ 31%）：
        // 叠在账号行那层浅白底上，只比输入框底色深一点点——看得见在转，
        // 又不会像纯蓝那样把底衬得突兀。想更淡/更明显就调 alpha
        QColor arcColor("#4a90d9");
        arcColor.setAlpha(80);
        QPen pen(arcColor, 2.0, Qt::SolidLine, Qt::RoundCap);
        //QPen是画笔，用于绘制线、圆等图形，`Qt::SolidLine`：实线（还有虚线、点线 Qt::DashLine 等）
        //Qt::RoundCap→ 圆角端点，如果默认是`Qt::FlatCap`：圆弧两头是平切的硬切口，RoundCap：圆弧的首尾端点变成半圆形圆头，让动画看的更顺畅
        painter.setPen(pen);//把画笔交给画家对象
        painter.setBrush(Qt::NoBrush);
        //Brush是画刷：用来填充闭合图形内部（比如画圆的时候填充内部颜色），Qt::NoBrush：关闭填充
        QRectF arcRect = rect.adjusted(2, 2, -2, -2);//向内收缩两个像素
        //如果直接贴着 rect 边缘画圆弧，线条**一半会跑到控件外面，被窗口裁切**，圆弧会缺边。向内缩进 2px
        // drawArc 的角度单位是 1/16 度；留 90° 缺口，更像加载动画
        painter.drawArc(arcRect, m_angle * 16, 270 * 16);
        //第一个参数 arcRect：圆弧的外接正方形。在这个正方形内画椭圆弧；正方形 → 画出来是圆弧（正圆上的一段弧
    } else if (m_status == Failed) {
        // ===== 发送失败：红色圆形底 + 白色感叹号 =====
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#e64545"));
        painter.drawEllipse(rect);

        qreal cx = rect.center().x();
        painter.setPen(QPen(Qt::white, 2.0, Qt::SolidLine, Qt::RoundCap));
        // 感叹号的竖线
        painter.drawLine(QPointF(cx, rect.top() + 4.5), QPointF(cx, rect.center().y() + 1.0));
        // 感叹号下面的圆点
        painter.setBrush(Qt::white);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QPointF(cx, rect.bottom() - 4.0), 1.6, 1.6);
    }
}
