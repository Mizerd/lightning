#include "threads/ThreadManager.h"

ThreadManager::ThreadManager(QObject *parent)
    : QObject(parent)
{
}

QStringList ThreadManager::threadsInRoom(const QString &) const
{
    return {};
}

int ThreadManager::threadReplyCount(const QString &) const
{
    return 0;
}
