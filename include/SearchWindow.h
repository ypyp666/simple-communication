#pragma once
#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QListWidget>
#include <QScrollBar>
#include <QString>
#include <QVBoxLayout>

class SearchWindow : public QWidget { 
    Q_OBJECT
public:
    SearchWindow(QWidget *parent = nullptr);
    ~SearchWindow();
};
