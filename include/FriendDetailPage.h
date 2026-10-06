#pragma once
#include <QWidget>
#include "FeatureStructs.h"   // 引入 ContactInfo（好友资料：id / name / avatar / remark）

class QLabel;
class QLineEdit;
class QPushButton;

// 好友详情页（联系人页右栏的"页0"）。
//
// 为什么单独做成一个类，而不是继续在 ContactDetailArea 里摊着：
// 这块内容排版不少，以后多半还要往上加东西（分组、签名、来源……），
// 让它自己管自己的布局最省事——外面只需要调 setContact() 灌数据。
//
// 版面从上到下（整页一个垂直布局，除弹簧外所有控件都是固定高度，
// 所以窗口拉高只会把弹簧拉长，不会把某一行撑变形）：
//      [ 修改备注 ]            ← 只有这一行靠右
//          头像（居中）
//          用户名：xxx          ← 不可改，左对齐
//          用户ID：xxx          ← 不可改，左对齐
//          备注：xxx            ← 可改：点右上角按钮进入编辑态，改完再点一次保存，左对齐
//          （红字提示）          ← 改备注失败时挂一条提示，平时留空（固定高度，不出提示时不顶动版面）
//          （弹簧）             ← 只它伸缩，把下面那行按钮压到页面底部
//   弹簧 发送消息 弹簧 删除好友 弹簧  ← 两个固定宽度的小按钮，右边那个是红的
//
// 本类不主动接线（沿用 ContactWindow / ContactDetailArea 的规矩）：动作以信号抛出去，
// 由 MainWindow 统一接到主后端；改备注的结果再由 MainWindow 接回 onRemarkSaveSuccess/Failed。
// 改备注是"先发服务器，服务器确认成功才改本地"：点保存时界面【不】立刻换成新备注，
// 先退回旧值，等服务器回包成功（onRemarkSaveSuccess）才更新显示。
class FriendDetailPage : public QWidget
{
    Q_OBJECT
public:
    explicit FriendDetailPage(QWidget *parent = nullptr);

    // 灌数据：换好友时调用（把本地那份 ContactInfo 摆到界面上）。
    // 顺带把"备注编辑态"复位——换人了还留着上一个人的编辑态是错的
    void setContact(const ContactInfo& contact);

    // 清空：当前没有好友被选中时用，不显示上一个人的残留信息
    void clearContact();

signals:
    // 点「发送消息」：带上对方的账号ID + 名字，由主窗口转给聊天页打开会话。
    // 名字要和双击条目那条路用同一份（会话标题靠它），所以一起抛出去，
    // 主窗口直接复用 onContactOpened 那个槽（切聊天页 + 打开会话，与双击走同一条路）
    void sendMessageRequested(const QString& contactId, const QString& contactName);
    // 备注改完点「保存备注」：带上改后的内容，由主窗口转给主后端发请求给服务器。
    // 此时【还没】改本地、也没改显示——要等服务器确认成功回包（见 onRemarkSaveSuccess）
    void remarkSaved(const QString& contactId, const QString& remark);
    // 点「删除好友」：由主窗口转主后端删本地好友（与通讯录右键"删除好友"同一个后端入口）
    void deleteFriendRequested(const QString& contactId);

public slots:
    // 服务器确认改备注成功：这时才把界面从旧值刷成新值（点保存时故意没立刻改显示）。
    // contactId 与当前显示的好友不一致时忽略——慢一拍的回包不能盖到已经切换的好友上
    void onRemarkSaveSuccess(const QString& contactId, const QString& remark);
    // 改备注失败：备注保持旧值，页面上挂一条红字提示
    void onRemarkSaveFailed(const QString& contactId, const QString& message);

private slots:
    // 右上角那个按钮的两副面孔：没在编辑 → 进编辑态；正在编辑 → 存下来并退出编辑态
    void onModifyRemarkClicked();

private:
    // 切开"看备注 / 改备注"两种状态：只读开不开、样式换不换、按钮文字变不变，都在这里收口
    void enterRemarkEditMode(bool editing);

    QPushButton* m_modifyRemarkBtn;   // 右上角：修改备注 / 保存备注
    QLabel* m_avatarLabel;            // 居中头像
    QLabel* m_nameLabel;              // 用户名：xxx
    QLabel* m_idLabel;                // 用户ID：xxx
    QLineEdit* m_remarkEdit;          // 备注：xxx（前缀是另一个 QLabel，这个只放内容）
    QLabel* m_hintLabel;              // 改备注失败的红字提示（平时为空）
    QPushButton* m_sendMsgBtn;        // 发送消息
    QPushButton* m_deleteFriendBtn;   // 删除好友（红）

    QString m_contactId;              // 当前显示的是谁（改备注 / 删好友都要拿它定位）
    QString m_contactName;            // 当前显示的是谁（"发送消息"打开会话时要用，会话标题靠它）
    bool m_remarkEditing = false;     // 备注是不是正在编辑中
    // 备注的两份值，用来区分"界面上显示的"和"正在等服务器的"：
    //   m_remark        —— 已确认的备注（改成功后由回包更新，也是保存后要退回显示的旧值）
    //   m_pendingRemark —— 已提交、还没等到服务器回包的新备注
    QString m_remark;
    QString m_pendingRemark;
};