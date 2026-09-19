#pragma once
#include <QWidget>
#include <QObject>
#include <QString>

class ContactWindow : public QWidget
{
    Q_OBJECT
public:
    ContactWindow(QWidget *parent = nullptr);
    ~ContactWindow();
};



