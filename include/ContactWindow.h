#pragma once
#include <QWidget>
#include <QObject>
#include <QString>

class ContactList;
class ContactDetailArea;

// 联系人页：只负责摆控件，不做任何信号接线。
// 全项目的接线规矩是"两条轴各一个统一入口"：要驱动 UI 的从主 UI（MainWindow）进，
// 要动后端的从主后端（MainBackend）进、由它路由一层到业务后端。
// 所以本页的对外链路都收口在 MainWindow::initPages 末尾 —— 这样追一条完整
// 信号链路只需看"主 UI 接线处 + 主后端路由处"，不用顺着各页面的 emit 名一层层往上拼。
//
// 布局：左栏（280）是通讯录 ContactList；右栏整块交给 ContactDetailArea ——
// 那块区域是【好友详情】和【好友请求列表】共用的，切换逻辑收在 ContactDetailArea 自己内部。
class ContactWindow : public QWidget
{
    Q_OBJECT
public:
    explicit ContactWindow(QWidget *parent = nullptr);
    ~ContactWindow();

    // 供 MainWindow 统一接线用。本窗口不转发子控件的信号，
    // 也不再持有后端指针（它自己不接线）
    ContactList* contactList() const { return m_contactList; }
    ContactDetailArea* detailArea() const { return m_detailArea; }

private:
    ContactList* m_contactList;        // 左栏通讯录（"新朋友"行 + 分割线 + "我的好友"折叠组）
    ContactDetailArea* m_detailArea;   // 右栏共用区域（好友详情 / 好友请求列表）
};