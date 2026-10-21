#include "accounts/RetirementLatches.h"

#include "accounts/RustBridgeApi.h"
#include "storage/AppDataPaths.h"

#include <QLoggingCategory>

#include <utility>

Q_LOGGING_CATEGORY(lcLatches, "lightning.accounts.latch")

namespace lightning::accounts {

RetirementLatches::RetirementLatches(RustBridgeApi *api, QObject *parent)
    : QObject(parent)
    , m_api(api)
{
    m_poll.setInterval(kPollMs);
    connect(&m_poll, &QTimer::timeout, this, &RetirementLatches::pollNow);
}

void RetirementLatches::arm(const QString &userId, const QString &storePath)
{
    if (userId.isEmpty())
        return;
    Latch &latch = m_latches[userId];
    latch.storePath = storePath;
    updateTimer();
}

void RetirementLatches::noteHandbackDone(const QString &userId)
{
    auto it = m_latches.find(userId);
    if (it == m_latches.end())
        return;
    it->handbackDone = true;
    updateTimer();
}

bool RetirementLatches::isArmed(const QString &userId) const
{
    return m_latches.contains(userId);
}

int RetirementLatches::armedCount() const
{
    return int(m_latches.size());
}

quint64 RetirementLatches::await(const QString &userId, int budgetMs,
                                 std::function<void(bool free)> done)
{
    const quint64 ticket = m_nextTicket++;
    m_waiters.append(Waiter{ticket, userId, QDeadlineTimer(budgetMs), std::move(done)});
    if (!m_latches.contains(userId)) {
        // Free now; answered on the next turn so the caller's own state is
        // settled before its callback runs.
        QTimer::singleShot(0, this, [this, userId] { answer(userId, true); });
    }
    updateTimer();
    return ticket;
}

void RetirementLatches::cancel(quint64 ticket)
{
    m_waiters.removeIf([ticket](const Waiter &w) { return w.ticket == ticket; });
    updateTimer();
}

void RetirementLatches::clear()
{
    m_latches.clear();
    m_waiters.clear();
    updateTimer();
}

void RetirementLatches::answer(const QString &userId, bool free)
{
    QList<Waiter> ready;
    for (qsizetype i = 0; i < m_waiters.size();) {
        if (m_waiters.at(i).userId == userId) {
            ready.append(m_waiters.takeAt(i));
            continue;
        }
        ++i;
    }
    for (const Waiter &waiter : std::as_const(ready)) {
        if (waiter.done)
            waiter.done(free);
    }
    updateTimer();
}

void RetirementLatches::pollNow()
{
    // Releases first, so a waiter whose latch frees on the same poll its
    // budget ends is answered "free".
    QStringList freed;
    for (auto it = m_latches.begin(); it != m_latches.end();) {
        // -1 (a path Rust could not resolve) proves nothing: still held.
        if (it->handbackDone && m_api && m_api->storeAnyHeld(it->storePath) == 0) {
            freed.append(it.key());
            it = m_latches.erase(it);
            continue;
        }
        ++it;
    }
    for (const QString &uid : std::as_const(freed)) {
        qCInfo(lcLatches) << "store free again" << "acct=" << matrix::app_data::safeUserSlug(uid);
        Q_EMIT released(uid);
        answer(uid, true);
    }
    QStringList expired;
    for (const Waiter &waiter : std::as_const(m_waiters)) {
        if (waiter.deadline.hasExpired() && m_latches.contains(waiter.userId)
            && !expired.contains(waiter.userId))
            expired.append(waiter.userId);
    }
    for (const QString &uid : std::as_const(expired)) {
        // Only the waiters whose own budget ran out.
        QList<Waiter> late;
        for (qsizetype i = 0; i < m_waiters.size();) {
            if (m_waiters.at(i).userId == uid && m_waiters.at(i).deadline.hasExpired()) {
                late.append(m_waiters.takeAt(i));
                continue;
            }
            ++i;
        }
        qCInfo(lcLatches) << "store still held at the end of a wait"
                          << "acct=" << matrix::app_data::safeUserSlug(uid);
        for (const Waiter &waiter : std::as_const(late)) {
            if (waiter.done)
                waiter.done(false);
        }
    }
    updateTimer();
}

void RetirementLatches::updateTimer()
{
    const bool needed = !m_latches.isEmpty() || !m_waiters.isEmpty();
    if (needed && !m_poll.isActive())
        m_poll.start();
    else if (!needed && m_poll.isActive())
        m_poll.stop();
}

} // namespace lightning::accounts
