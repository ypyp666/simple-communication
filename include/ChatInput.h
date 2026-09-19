#ifndef CHATINPUT_H
#define CHATINPUT_H

#include <QWidget>
#include <QGridLayout>
#include <QFrame>
#include <QTextEdit>
#include <QPushButton>
#include <QFileDialog>
#include <QPropertyAnimation>

class ChatInput : public QWidget
{
    Q_OBJECT
public:
    explicit ChatInput(QWidget *parent = nullptr);

signals:
    void sendMessage(const QString& content);
    void sendFile(const QString& filePath);

protected:
    // 键盘交互全在这里：回车键要在"文本被插进去之前"拦下来判断是发送还是换行，
    // 而 QTextEdit 没有"按键按下"信号可连，只能给 inputEdit 装事件过滤器（installEventFilter）
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onSendClicked();
    void onFileClicked();
    void onEnterPressed();
    // 按当前文本的实际行数调整输入框高度（textChanged 触发）：
    // 只改输入框自己这一行，功能按钮行/发送按钮行的高度不受影响。
    // 高度用动画过渡（不是 setFixedHeight 一步到位），避免每多一行整块框硬跳
    void adjustHeight();

private:
    // 网格布局：0 行=功能按钮行（固定高），1 行=输入框（唯一伸缩行），2 行=发送按钮行（固定高）。
    // 输入框变高时整个 FrameBox 向上扩，底边不动（ChatArea 里 scrollArea 有 stretch=1，
    // 增出来的高度全部从聊天记录区扣，发送按钮那一行不会被挤压）
    QGridLayout* layout;
    QFrame* boxFrame;          // 三块内容共用的圆角框（功能行/输入框/发送行都在它里面）
    QTextEdit* inputEdit;
    QPushButton* fileButton;
    QPushButton* sendButton;

    // 高度平滑过渡用。一个 QPropertyAnimation 只能动一个属性，而"定高"必须
    // minimumHeight 和 maximumHeight 同步改，所以要两条同时跑：
    // 起止值、时长、曲线完全一致 → 输入框在动画期间始终是定高状态，
    // 只是这个定高值被逐帧推过去，整块 FrameBox 跟着平滑向上长
    QPropertyAnimation* heightAnimMin;
    QPropertyAnimation* heightAnimMax;

    // 输入法是否正在"组字"（拼音/候选词还没上屏）。
    // 中文输入法里回车是"确认候选词"，跟"发送"是两码事，必须区分开：
    // 组字期间按回车直接发送，会把半截拼音发出去
    bool m_imeComposing = false;
};

#endif // CHATINPUT_H
