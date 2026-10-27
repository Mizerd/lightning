#pragma once

#include <QObject>
#include <QThread>

namespace lightning::media {

// The worker and thread have independent lifetimes. A timed-out join must
// leave the QThread QObject alive until the OS thread emits finished; deleting
// it while still running would crash. Callers own and delete it only on true.
inline bool quitAndJoinWorkerForShutdown(QThread *thread, int budgetMs)
{
    if (!thread)
        return true;
    thread->quit();
    if (thread->wait(budgetMs))
        return true;
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    return false;
}

} // namespace lightning::media
