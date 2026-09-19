#include "StatusBar.h"
#include <QVBoxLayout>
#include <QButtonGroup>

// 导航图标资源路径，顺序与 MainWindow 的页面栈保持一致：
// 聊天(ChatWindow) → 联系人(ContactWindow) → 搜索(SearchWindow) → Agent(AgentWindow)
namespace {
struct NavIcon {
    const char* normal;  // 未选中：黑色图标
    const char* active;  // 选中：蓝色图标
};

const NavIcon kNavIcons[] = {
    { ":/res/icon/Message_show.svg", ":/res/icon/Message_clicked.svg" },
    { ":/res/icon/Contact_show.svg", ":/res/icon/Contact_clicked.svg" },
    { ":/res/icon/Serch_show.svg",   ":/res/icon/Serch_clickedd.svg" },
    { ":/res/icon/Agent_show.svg",   ":/res/icon/Agent_clickedd.svg" },
};

constexpr int kNavCount = sizeof(kNavIcons) / sizeof(kNavIcons[0]);
constexpr int kButtonSize = 60;       // 导航按钮边长（正方形，等于状态栏可用宽度）
constexpr int kBarBorderWidth = 1;    // 右侧 1px 分割线
}

StatusBar::StatusBar(QWidget *parent) : QWidget(parent)
{
    // 用 ID 选择器限定作用范围，避免样式污染子控件（QLabel 是 QFrame 子类，选择器易误伤）
    setObjectName("statusBar");
    setStyleSheet(R"(
        #statusBar {
            background-color: #f5f5f5;
            border-right: 1px solid #e0e0e0;
        }
    )");

    // 宽度由按钮边长反推，保证按钮始终是正方形（不依赖外部设置）
    setFixedWidth(kButtonSize  + kBarBorderWidth);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);  // 间距为 0，让按钮的悬停/选中底色连成整块

    QButtonGroup* navGroup = new QButtonGroup(this);
    navGroup->setExclusive(true);  // 四个按钮互斥，同一时刻只有一个"选中"
    /*- 数据驱动、数量可变、要滚动/虚拟化 → 用视图控件（`QListWidget` /`QListView` ）
- 数量固定且少、每个元素是独立交互单元、外观靠控件自身 QSS → 独立控件 + 布局 +`QButtonGroup`*/

    for (int i = 0; i < kNavCount; ++i) {
        QPushButton* btn = new QPushButton(this);
        btn->setFixedSize(kButtonSize, kButtonSize);  // 正方形按钮块，宽度顶满状态栏
        btn->setCheckable(true);
        btn->setFocusPolicy(Qt::NoFocus);  // 不参与 Tab 焦点循环
        btn->setCursor(Qt::PointingHandCursor);
        btn->setIconSize(QSize(26, 26));
        btn->setStyleSheet(R"(
            QPushButton {
                border: none;
                border-radius: 8px;
                background-color: transparent;
            }
            QPushButton:hover {
                background-color: rgba(0, 0, 0, 0.06);
            }
            QPushButton:pressed {
                background-color: rgba(0, 0, 0, 0.14);
            }
            /* 选中态：整个按钮块底色加深，与未选中区分 */
            QPushButton:checked {
                background-color: rgba(0, 0, 0, 0.10);
            }
            QPushButton:checked:hover {
                background-color: rgba(0, 0, 0, 0.14);
            }
        )");

        navGroup->addButton(btn, i);
        m_navButtons.append(btn);
        layout->addWidget(btn);
    }
    layout->addStretch();

    setCurrentIndex(0);  // 默认选中第一项（聊天）

    // ========== 信号连接 ==========
    connect(navGroup, &QButtonGroup::idClicked, this, &StatusBar::setCurrentIndex);
}

void StatusBar::setCurrentIndex(int index)
{
    if (index < 0 || index >= m_navButtons.size()) {
        return;
    }

    for (int i = 0; i < m_navButtons.size(); ++i) {
        m_navButtons[i]->setChecked(i == index);// 设置选中当前索引对应的按钮
        m_navButtons[i]->setIcon(QIcon(i == index ? kNavIcons[i].active : kNavIcons[i].normal));
    }
}