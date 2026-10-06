#include "ContactDetailArea.h"
#include "FriendRequestList.h"
#include "FriendDetailPage.h"
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QFrame>

ContactDetailArea::ContactDetailArea(QWidget *parent) : QWidget(parent)
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_stack = new QStackedWidget(this);
    // QStackedWidget 是 QFrame 子类，默认会画一圈原生边框（见 note.txt：QFrame 的 frame
    // 和 QSS 是两套绘制系统），显式关掉才干净
    m_stack->setFrameShape(QFrame::NoFrame);
    // 用 #id 选择器限定范围：裸写声明会连同子控件一起染色
    m_stack->setObjectName("contactDetailStack");
    m_stack->setStyleSheet("#contactDetailStack { background-color: white; }");

    // ===== 页0：空白页（默认停在这一页）=====
    // 刚进联系人页时右栏是空的——没点好友就没有详情可看。
    // 不直接把详情页当"默认页"再靠 clearContact 清空：那样顶点会看到"修改备注"按钮
    // 和空头像框，像是一块没加载完的界面
    m_emptyPage = new QWidget(m_stack);
    m_emptyPage->setStyleSheet("background-color: white;");
    m_stack->addWidget(m_emptyPage);

    // ===== 页1：好友详情 =====
    // 内容不少且以后还要扩，所以单独拆成一个类（FriendDetailPage），这里只负责摆放它
    m_friendDetailPage = new FriendDetailPage(m_stack);
    m_stack->addWidget(m_friendDetailPage);

    // ===== 页2：好友请求列表 =====
    // 自带标题栏的整块宽列表（照 QQ"好友通知"的样子），不再另开右侧详情栏
    m_friendRequestList = new FriendRequestList(m_stack);
    m_stack->addWidget(m_friendRequestList);

    layout->addWidget(m_stack);

    // 默认停在空白页——右栏要等"点了谁"才有内容
    m_stack->setCurrentIndex(EMPTY_INDEX);
}

void ContactDetailArea::showEmpty()
{
    m_stack->setCurrentIndex(EMPTY_INDEX);
}

void ContactDetailArea::showFriendDetail()
{
    m_stack->setCurrentIndex(FRIEND_DETAIL_INDEX);
}

void ContactDetailArea::showFriendRequests()
{
    m_stack->setCurrentIndex(FRIEND_REQUEST_INDEX);
}

bool ContactDetailArea::isShowingFriendRequests() const
{
    return m_stack->currentIndex() == FRIEND_REQUEST_INDEX;
}