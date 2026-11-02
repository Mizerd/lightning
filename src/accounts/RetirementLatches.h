// When a retiring background host's store is free again (live accounts, Labs;
// design §1 I1): only when Rust says nothing in the process holds that store
// any more (mx_rust_store_any_held, which counts reservations and live probes)
// AND that account's token handback has settled. Until both, the latch stays
// armed, for the rest of the process if need be: a store whose close was
// never confirmed is "Store busy (restart to reopen)", never deleted and
// never reopened.
//
// GUI thread only. any_held is a non-blocking FFI; it is polled every 250 ms
// while a latch is armed, and not at all otherwise.
#pragma once

#include <QDeadlineTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

namespace lightning::accounts {

class RustBridgeApi;

class RetirementLatches : public QObject
{
    Q_OBJECT
public:
    static constexpr int kPollMs = 250;

    explicit RetirementLatches(RustBridgeApi *api, QObject *parent = nullptr);

    void arm(const QString &userId, const QString &storePath);
    void noteHandbackDone(const QString &userId);
    bool isArmed(const QString &userId) const;
    int armedCount() const;

    quint64 await(const QString &userId, int budgetMs, std::function<void(bool free)> done);
    void cancel(quint64 ticket);
    void clear();

    void pollNow();

Q_SIGNALS:
    void released(const QString &userId);

private:
    struct Latch {
        QString storePath;
        bool handbackDone = false;
    };
    struct Waiter {
        quint64 ticket = 0;
        QString userId;
        QDeadlineTimer deadline;
        std::function<void(bool)> done;
    };
    void updateTimer();
    void answer(const QString &userId, bool free);

    RustBridgeApi *m_api;
    QHash<QString, Latch> m_latches;
    QList<Waiter> m_waiters;
    QTimer m_poll;
    quint64 m_nextTicket = 1;
};

}
