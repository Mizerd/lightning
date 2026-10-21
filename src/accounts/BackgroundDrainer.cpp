#include "accounts/BackgroundDrainer.h"

#include "accounts/BackgroundSessionHost.h"
#include "app/GuiStallTracer.h"

namespace lightning::accounts {

BackgroundDrainer::BackgroundDrainer(QObject *parent)
    : QObject(parent)
{
    m_timer.setInterval(kTickMs);
    connect(&m_timer, &QTimer::timeout, this, &BackgroundDrainer::tick);
}

void BackgroundDrainer::addHost(BackgroundSessionHost *host)
{
    if (!host)
        return;
    for (const auto &existing : std::as_const(m_hosts)) {
        if (existing == host)
            return;
    }
    m_hosts.append(host);
    updateTimer();
}

void BackgroundDrainer::removeHost(BackgroundSessionHost *host)
{
    for (qsizetype i = 0; i < m_hosts.size(); ++i) {
        if (m_hosts.at(i) == host) {
            m_hosts.removeAt(i);
            if (m_next > i)
                --m_next;
            break;
        }
    }
    // Retirement deletes the host later; never drained again meanwhile.
    updateTimer();
}

void BackgroundDrainer::setElapsedForTest(std::function<qint64()> elapsedSinceTickStart)
{
    m_elapsedForTest = std::move(elapsedSinceTickStart);
}

int BackgroundDrainer::tick()
{
    stalltrace::Scope stallScope("bg-drain");
    // Hosts retired since the last tick.
    m_hosts.removeIf([](const QPointer<BackgroundSessionHost> &h) { return h.isNull(); });
    if (m_hosts.isEmpty()) {
        m_next = 0;
        updateTimer();
        return 0;
    }
    QElapsedTimer elapsed;
    elapsed.start();
    const auto spentMs = [&] {
        return m_elapsedForTest ? m_elapsedForTest() : elapsed.elapsed();
    };
    // The pass walks a SNAPSHOT: a host's own signals can retire it (and
    // remove it from m_hosts) mid-pass, which would shift every index after
    // it (review L13). Where the next tick starts is found again by host.
    const QList<QPointer<BackgroundSessionHost>> pass = m_hosts;
    const int count = int(pass.size());
    if (m_next >= count)
        m_next = 0;
    const int start = m_next;
    int drained = 0;
    int stoppedAfter = count - 1;
    for (int i = 0; i < count; ++i) {
        const QPointer<BackgroundSessionHost> host = pass.at((start + i) % count);
        if (host)
            host->drainOnce();
        ++drained;
        if (spentMs() >= kBudgetMs && i + 1 < count) {
            stoppedAfter = i;
            break;
        }
    }
    // Every host the budget did not reach still has its token step this
    // tick: two atomic loads, and custody only when a rotation waits. A
    // refresh waits for that answer (sessiontokens' DRAIN_STALL is 500 ms),
    // so it must never sit behind other accounts' heavy work (§18 round 1,
    // L6). Only the heavy work (takes, the queue) is budgeted.
    for (int i = stoppedAfter + 1; i < count; ++i) {
        const QPointer<BackgroundSessionHost> host = pass.at((start + i) % count);
        if (host)
            host->drainTokensOnly();
    }
    // Round-robin: the next tick starts with the first host after the last
    // one drained (a full pass: the one after this pass's first) that is
    // still here.
    const int nextOffset = stoppedAfter + 1 < count ? stoppedAfter + 1 : 1;
    m_next = 0;
    for (int k = 0; k < count; ++k) {
        const QPointer<BackgroundSessionHost> candidate = pass.at((start + nextOffset + k) % count);
        const qsizetype at = candidate ? m_hosts.indexOf(candidate) : -1;
        if (at >= 0) {
            m_next = int(at);
            break;
        }
    }
    m_lastTickMs = spentMs();
    return drained;
}

void BackgroundDrainer::updateTimer()
{
    if (m_hosts.isEmpty())
        m_timer.stop();
    else if (!m_timer.isActive())
        m_timer.start();
}

} // namespace lightning::accounts
