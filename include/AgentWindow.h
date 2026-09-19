#ifndef AGENTWINDOW_H
#define AGENTWINDOW_H 

#pragma once
#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QListWidget>
#include <QScrollBar>
#include <QObject>
#include <QString>
#include <QVBoxLayout>

class AgentWindow : public QWidget { 
    Q_OBJECT
public:
    AgentWindow(QWidget *parent = nullptr);
    ~AgentWindow();
};

#endif