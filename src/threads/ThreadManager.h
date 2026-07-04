#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

// v0.1 placeholder. In v0.5, this exposes threaded reply metadata and
// per-thread timeline models.
class ThreadManager : public QObject
{
    Q_OBJECT
public:
    explicit ThreadManager(QObject *parent = nullptr);

    Q_INVOKABLE QStringList threadsInRoom(const QString &roomId) const;
    Q_INVOKABLE int threadReplyCount(const QString &rootEventId) const;
};
