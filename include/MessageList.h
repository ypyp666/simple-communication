#pragma once
#include <QObject>
#include <QList>
#include <QString>


class MessageList : public QObject
{
    Q_OBJECT
public:
    MessageList(QObject *parent = nullptr);
    ~MessageList();
};
