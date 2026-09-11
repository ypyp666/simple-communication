#ifndef GLASSCARD_H
#define GLASSCARD_H

#include <QFrame>
#include <QPixmap>

class QShowEvent;
class QResizeEvent;
class QPaintEvent;

// ============================================================
// GlassCard（局部毛玻璃卡片）——从 ForgotPasswordPage.cpp 提取为独立文件，
// 供忘记密码页 / 注册页等多处复用真·模糊卡片背景。
// 背景：QSS 只能做"半透明"，做不出"把后面内容糊掉"的真毛玻璃；
//       DWM/Acrylic 只能糊整个窗口，做不了单个卡片这种局部区域。
// 思路：paintEvent 里 ①把自己临时隐藏（防镜中镜递归）→
//       ②用 root->render 只截取卡片背后那一小片窗口内容 →
//       ③借 QGraphicsBlurEffect 做高斯模糊 →
//       ④垫到卡片底部，再叠一层半透明白 + 一圈高光描边。
// 缓存：模糊结果存 m_backdrop，只在显示/尺寸变化时重算，
//       避免每次重绘（比如输密码刷新强度条）都整片截屏模糊。
// ============================================================
class GlassCard : public QFrame
{
public:
    explicit GlassCard(QWidget* parent = nullptr);

    // 调整模糊强度：越大背后糊得越狠；0 表示只叠白雾不糊
    void setBlurRadius(qreal radius);

protected:
    // 页面切回本卡（或窗口重新显示）时，背后内容可能变了 → 快照作废
    void showEvent(QShowEvent*) override;
    // 卡片尺寸变化 → 快照作废（下次 paint 重新截）
    void resizeEvent(QResizeEvent*) override;

    void paintEvent(QPaintEvent*) override;

private:
    // 截取背后窗口内容 → 模糊 → 裁出卡片大小，存进 m_backdrop
    void refreshBackdrop();
    // 高斯模糊：借 QtWidgets 自带的 QGraphicsBlurEffect 完成，无需任何额外依赖
    static QPixmap blurPixmap(const QPixmap& src, qreal radius);

    qreal m_blurRadius = 50.0;   // 模糊半径：越大糊得越狠（试验值，可调）
    QPixmap m_backdrop;          // 模糊后景快照缓存；null 表示"过期，需要重算"
};

#endif // GLASSCARD_H