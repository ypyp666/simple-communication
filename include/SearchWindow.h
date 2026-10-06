#pragma once
#include <QWidget>
#include <QVBoxLayout>
#include <QList>
#include "FeatureStructs.h"   // 引入 ContactInfo（搜索结果的载体）

class QLabel;
class QLineEdit;
class QPushButton;
class SearchResultList;

class SearchWindow : public QWidget {
    Q_OBJECT
public:
    SearchWindow(QWidget *parent = nullptr);
    ~SearchWindow();

    // 切走搜索页时由 MainWindow 调用：把搜索框清空，下次进来是干净的一页
    //（清除按钮 / 搜索按钮的可用性由 textChanged 自动跟着更新，这里不用管）
    void clearSearch();

public slots:
    // 主后端回来的搜索结果 / 失败 → 交给下面的结果列表渲染
    void setSearchResults(const QList<ContactInfo>& results);
    void setSearchFailed(const QString& message);
    // 好友申请的服务器回包（纯状态，不带数据）→ 顶部提示条给一句成功/失败反馈。
    // 有意拆成"一成功一失败"两个槽，而不是"bool + 文案"合成一个：
    // 合成一个的话接信号时得套 lambda，拆开后两条 connect 都是"信号对方法"直连，
    // 与本页其它转发（setSearchResults / setSearchFailed）保持一致
    void showAddFriendSuccess();
    void showAddFriendFailed(const QString& message);

signals:
    // 回车 / 点「搜索」按钮：把要搜的词交给主后端发 search_request。
    // 空词不发（按钮在无字时本来就是禁用态，这里再兜一道）
    void searchRequested(const QString& keyword);
    // 结果卡片点「添加」→ 弹"申请加好友"小窗填留言 → 带留言由主窗口转主后端发好友申请
    void addFriendRequested(const QString& userId, const QString& message);

protected:
    // 事件过滤：输入框里按回车（主键盘 Key_Return / 小键盘 Key_Enter）触发搜索
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    // 输入框内容变化 → 控制右侧"清除按钮"显隐 + 搜索按钮可用性（空文本时按钮置灰）
    void onSearchTextChanged(const QString& text);
    // 回车 / 点按钮的统一入口
    void onSearchTriggered();
    // 清除按钮按下 / 松开：只切换图标（悬浮用普通叉号，按下换红色点击态）
    void onClearButtonPressed();
    void onClearButtonReleased();
    // 结果卡片点「添加」：先弹"申请加好友"小窗让用户填留言，确认后再带留言往外发
    void onAddRequested(const ContactInfo& contact);

private:
    void updateSearchButtonState();   // 有字才让搜索按钮可点
    // 提示条的统一渲染：成功绿色 / 失败红色；文案为空时用默认话术
    void showAddResult(bool success, const QString& message);

    QLabel* m_addResultLabel = nullptr;  // 「添加」的结果提示条（默认隐藏，隐藏时不占布局高度）
    QLabel* m_searchIcon;            // 搜索框左侧的放大镜图标
    QLineEdit* m_searchEdit;         // 中间的输入框
    QPushButton* m_clearBtn;         // 搜索框内右侧的圆形清除按钮（有文字才出现）
    QPushButton* m_searchButton;     // 搜索框外面的「搜索」按钮
    SearchResultList* m_resultList;  // 下半部分的结果列表
};