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

    // uid's host began retiring on `storePath`. Re-arming keeps the handback
    // state already noted.
    void arm(const QString &userId, const QString &storePath);
    // The retirement's handback settled: written, refused, or nothing to hand
    // back. The latch still waits for any_held.
    void noteHandbackDone(const QString &userId);
    bool isArmed(const QString &userId) const;
    int armedCount() const;

    // done(true) once uid's latch is free (at once, asynchronously, when it is
    // not armed); done(false) when the budget runs out first. Never called
    // inside await() itself.
    quint64 await(const QString &userId, int budgetMs, std::function<void(bool free)> done);
    void cancel(quint64 ticket);
    // Forget everything and answer nobody (runtime flag-off after drain).
    void clear();

    // One poll now (tests; the timer calls the same).
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

} // namespace lightning::accounts
