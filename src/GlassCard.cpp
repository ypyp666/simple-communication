#include "GlassCard.h"

#include <QPainter>
#include <QPainterPath>
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <QGraphicsBlurEffect>
#include <QShowEvent>
#include <QResizeEvent>
#include <QPaintEvent>
#include <QtMath>          // qCeil

GlassCard::GlassCard(QWidget* parent)
    : QFrame(parent)
{
}

// 调整模糊强度：越大背后糊得越狠；0 表示只叠白雾不糊
void GlassCard::setBlurRadius(qreal radius)
{
    m_blurRadius = radius;
    m_backdrop = QPixmap();   // 半径变了，旧快照作废
    update();
}

// 页面切回本卡（或窗口重新显示）时，背后内容可能变了 → 快照作废
void GlassCard::showEvent(QShowEvent*)
{
    m_backdrop = QPixmap();
    update();
}

// 卡片尺寸变化 → 快照作废（下次 paint 重新截）
void GlassCard::resizeEvent(QResizeEvent*)
{
    m_backdrop = QPixmap();
}

void GlassCard::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // 圆角裁剪：四角不画、透出底下原有的渐变，形成圆角玻璃轮廓
    QPainterPath clip;
    clip.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);
    p.setClipPath(clip);

    // 首次绘制时才截屏模糊：构造/布局阶段窗口还没画好，截不到东西
    if (m_backdrop.isNull())
        refreshBackdrop();

    if (!m_backdrop.isNull()) {
        // ①糊掉的后景垫底
        p.drawPixmap(rect(), m_backdrop, m_backdrop.rect());
    } else {
        // 截屏失败兜底：退成半透明白，至少不像"黑窟窿"
        p.fillRect(rect(), QColor(255, 255, 255, 140));
    }

    // ②磨砂"雾感"：再叠一层半透明白，玻璃味更足
    p.fillRect(rect(), QColor(255, 255, 255, 120));
    // ③高光描边：模拟玻璃边缘反光
    p.setPen(QPen(QColor(255, 255, 255, 216), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);
}

// 截取背后窗口内容 → 模糊 → 裁出卡片大小，存进 m_backdrop
void GlassCard::refreshBackdrop()
{
    QWidget* root = window();          // 顶层窗口（LoginWindow），渐变就画在它身上
    if (!root || !isVisible() || size().isEmpty())
        return;

    // 卡片在顶层窗口坐标系里的矩形
    const QRect cardRect(mapTo(root, QPoint(0, 0)), size());
    // 往外扩一圈再截：模糊会在边缘"往窗外采样"，不扩边边缘会出现暗角
    const int pad = qCeil(m_blurRadius * 2);
    const QRect capRect = cardRect.adjusted(-pad, -pad, pad, pad)
                              .intersected(root->rect());

    // 【防镜中镜递归】先把自己藏起来再截窗口：不藏的话截图里带着自己，
    // 自己身上又画"截图里的自己"，画的时候再跑去截图 → 无限递归
    hide();
    QPixmap shot(capRect.size() * root->devicePixelRatioF());
    shot.setDevicePixelRatio(root->devicePixelRatioF());
    shot.fill(Qt::transparent);
    QPainter sp(&shot);
    // render + 源区域：只截卡片背后那一小片，不用整窗 grab
    root->render(&sp, QPoint(0, 0), capRect);
    sp.end();
    show();

    // 整片糊掉，再裁出卡片正对的一块（去掉四周扩边）
    const QPixmap blurred = blurPixmap(shot, m_blurRadius);
    const QPoint off = cardRect.topLeft() - capRect.topLeft();
    m_backdrop = blurred.copy(QRect(off, size()));
}

// 高斯模糊：借 QtWidgets 自带的 QGraphicsBlurEffect 完成，无需任何额外依赖
QPixmap GlassCard::blurPixmap(const QPixmap& src, qreal radius)
{
    if (radius <= 0.0 || src.isNull())
        return src;

    QGraphicsScene scene;
    QGraphicsPixmapItem item(src);
    auto* eff = new QGraphicsBlurEffect;
    eff->setBlurRadius(radius);
    eff->setBlurHints(QGraphicsBlurEffect::QualityHint);
    item.setGraphicsEffect(eff);        // effect 由 item 接管，不用手动 delete
    scene.addItem(&item);

    QPixmap out(src.size());
    out.setDevicePixelRatio(src.devicePixelRatioF());
    out.fill(Qt::transparent);
    QPainter p(&out);
    scene.render(&p, QRectF(out.rect()), QRectF(src.rect()));
    return out;
}