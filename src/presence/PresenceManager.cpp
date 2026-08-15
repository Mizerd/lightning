#include "PresenceManager.h"

#include <algorithm>

#include <QGuiApplication>
#include <QLoggingCategory>
#include <QVariantList>

#include "app/SettingsManager.h"

Q_LOGGING_CATEGORY(lcPresence, "lightning.presence")

namespace {
// The only states an indicator may render. Anything else from the bridge
// ("unknown", a future value) erases the cache entry — no indicator.
bool isRenderableState(const QString &state)
{
    return state == QLatin1String("online")
        || state == QLatin1String("unavailable")
        || state == QLatin1String("offline");
}
} // namespace

PresenceManager::PresenceManager(QObject *parent)
    : QObject(parent)
{
    m_clock.start();
    m_inactiveSinceMs = m_clock.elapsed();

    m_pollTimer.setInterval(kPollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout,
            this, &PresenceManager::scheduledPollRound);
    m_pollTimer.start();

    m_burstTimer.setSingleShot(true);
    m_burstTimer.setInterval(kBurstDelayMs);
    connect(&m_burstTimer, &QTimer::timeout,
            this, &PresenceManager::burstRound);

    m_publishTimer.setInterval(kPublishIntervalMs);
    connect(&m_publishTimer, &QTimer::timeout,
            this, [this]() { publishTick(true); });
    m_publishTimer.start();

    // Headless tests run without a QGuiApplication; the seam
    // setApplicationActive covers them.
    if (auto *gui = qGuiApp) {
        connect(gui, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState state) {
                    setApplicationActive(state == Qt::ApplicationActive);
                });
        setApplicationActive(gui->applicationState()
                             == Qt::ApplicationActive);
    }
}

void PresenceManager::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    clearSession();
    if (!m_client) {
        Q_EMIT supportedChanged();
        Q_EMIT activeChanged();
        return;
    }
    connect(m_client, &MatrixClient::presenceReceived,
            this, &PresenceManager::applyBatch);
    connect(m_client, &MatrixClient::presencePublishFailed, this,
            [](const QString &category) {
                // Bounded, category-only: publication is fire-and-forget
                // and the next keep-alive tick retries anyway.
                qCDebug(lcPresence) << "own-presence publish failed:"
                                    << category;
            });
    connect(m_client, &MatrixClient::loggedOut,
            this, &PresenceManager::clearSession);
    connect(m_client, &MatrixClient::connectionStateChanged,
            this, &PresenceManager::handleConnectionState);
    Q_EMIT supportedChanged();
    Q_EMIT activeChanged();
}

void PresenceManager::setSettings(SettingsManager *settings)
{
    if (m_settings == settings)
        return;
    if (m_settings)
        disconnect(m_settings, nullptr, this, nullptr);
    m_settings = settings;
    if (!m_settings)
        return;
    connect(m_settings, &SettingsManager::sharePresenceChanged, this,
            [this]() {
                if (!m_client || !m_client->supportsPresence())
                    return;
                if (publishEnabled()) {
                    m_pendingFinalOffline = false;
                    if (m_syncing)
                        publishTick(true);
                    return;
                }
                if (!m_syncing) {
                    // Disabled while the session is not live: the final
                    // offline is OWED, not skipped — it flushes on the
                    // next Syncing edge (review M3; the Settings copy
                    // promises it).
                    m_pendingFinalOffline = true;
                    return;
                }
                if (m_lastPublished != 2) {
                    // One final offline so the account does not linger
                    // online after the user asked not to share presence.
                    m_client->publishPresence(2);
                    m_lastPublished = 2;
                }
            });
}

bool PresenceManager::supported() const
{
    return m_client && m_client->supportsPresence();
}

bool PresenceManager::active() const
{
    return supported() && !m_serverRefused;
}

void PresenceManager::watch(const QString &userId)
{
    if (userId.isEmpty())
        return;
    const int refs = ++m_watched[userId];
    if (refs == 1 && !m_cache.contains(userId) && active()) {
        m_burstPending.insert(userId);
        if (!m_burstTimer.isActive())
            m_burstTimer.start();
    }
}

void PresenceManager::unwatch(const QString &userId)
{
    auto it = m_watched.find(userId);
    if (it == m_watched.end())
        return;
    if (--it.value() <= 0) {
        m_watched.erase(it);
        m_burstPending.remove(userId);
    }
}

QString PresenceManager::stateFor(const QString &userId) const
{
    const auto it = m_cache.constFind(userId);
    return it == m_cache.constEnd() ? QString() : it->state;
}

QVariantMap PresenceManager::infoFor(const QString &userId) const
{
    const auto it = m_cache.constFind(userId);
    if (it == m_cache.constEnd())
        return {};
    qint64 age = it->lastActiveAgoMs;
    if (age >= 0)
        age += m_clock.elapsed() - it->receivedAtMs;
    return QVariantMap{
        { QStringLiteral("state"), it->state },
        { QStringLiteral("currentlyActive"), it->currentlyActive },
        { QStringLiteral("lastActiveAgoMs"), age },
    };
}

void PresenceManager::setApplicationActive(bool activeNow)
{
    if (m_appActive == activeNow)
        return;
    m_appActive = activeNow;
    // The idle clock starts when focus is LOST — "continuously in the
    // background for N minutes", never "N minutes since focus was gained"
    // (review H2: the latter consumed the grace during foreground use and
    // published Away the instant the user switched windows).
    if (!activeNow)
        m_inactiveSinceMs = m_clock.elapsed();
    // Edge-triggered: publish immediately only when the resulting state
    // differs from the last published one (the keep-alive tick covers the
    // rest). Returning from a long background stretch flips idle → online
    // right away this path.
    publishTick(false);
}

void PresenceManager::scheduledPollRound()
{
    if (!active() || !m_syncing || m_watched.isEmpty())
        return;
    // Stable rotation over the watched set so a set larger than one batch
    // still refreshes everyone across consecutive rounds.
    m_pollOrder = m_watched.keys();
    std::sort(m_pollOrder.begin(), m_pollOrder.end());
    if (m_pollCursor >= m_pollOrder.size())
        m_pollCursor = 0;
    QStringList round;
    const int count = static_cast<int>(m_pollOrder.size());
    const int take = qMin(kBatchCap, count);
    round.reserve(take);
    for (int i = 0; i < take; ++i)
        round.append(m_pollOrder.at((m_pollCursor + i) % count));
    m_pollCursor = (m_pollCursor + take) % qMax(1, count);
    pollRound(round);
}

void PresenceManager::burstRound()
{
    if (!active() || !m_syncing || m_burstPending.isEmpty()) {
        m_burstPending.clear();
        return;
    }
    QStringList round;
    for (const QString &userId : std::as_const(m_burstPending)) {
        if (m_watched.contains(userId) && !m_cache.contains(userId))
            round.append(userId);
        if (round.size() >= kBatchCap)
            break;
    }
    m_burstPending.clear();
    if (!round.isEmpty())
        pollRound(round);
}

void PresenceManager::pollRound(const QStringList &userIds)
{
    if (!m_client || userIds.isEmpty())
        return;
    // Answers dropped by the lifecycle guard never clear their op id;
    // bound the set by evicting the OLDEST ids (they are monotonic) —
    // wholesale clearing discarded legitimately pending rounds whose
    // answers were then rejected as stale (review L2).
    while (m_inFlight.size() > 64)
        m_inFlight.remove(*std::min_element(m_inFlight.cbegin(),
                                            m_inFlight.cend()));
    const quint64 opId = m_nextOpId++;
    m_inFlight.insert(opId);
    m_client->requestPresence(userIds, opId);
}

void PresenceManager::applyBatch(quint64 opId, const QVariantList &entries)
{
    if (!m_inFlight.remove(opId))
        return;
    bool changed = false;
    // A batch feeds the refusal latch only when it is broad enough that
    // "everyone forbidden" plausibly means "the server refuses presence"
    // rather than one user's federation/membership quirk (review L1). A
    // too-small batch neither advances nor resets the latch count.
    const bool latchEligible = entries.size() >= kForbiddenLatchMinBatch;
    bool allForbidden = !entries.isEmpty();
    for (const QVariant &value : entries) {
        const QVariantMap entry = value.toMap();
        const QString userId = entry.value(QStringLiteral("userId")).toString();
        if (userId.isEmpty()) {
            allForbidden = false;
            continue;
        }
        if (entry.value(QStringLiteral("ok")).toBool()) {
            allForbidden = false;
            const QString state =
                entry.value(QStringLiteral("state")).toString();
            if (!isRenderableState(state)) {
                changed = m_cache.remove(userId) > 0 || changed;
                continue;
            }
            Entry cached;
            cached.state = state;
            cached.currentlyActive =
                entry.value(QStringLiteral("currentlyActive")).toBool();
            cached.lastActiveAgoMs =
                entry.value(QStringLiteral("lastActiveAgoMs"), -1)
                    .toLongLong();
            cached.receivedAtMs = m_clock.elapsed();
            m_cache.insert(userId, cached);
            changed = true;
            continue;
        }
        const QString category =
            entry.value(QStringLiteral("category")).toString();
        if (category == QLatin1String("forbidden")
            || category == QLatin1String("not_found")) {
            // Authoritative "no presence for this user": drop what we had.
            // Only forbidden counts toward the disabled-server latch.
            changed = m_cache.remove(userId) > 0 || changed;
            if (category != QLatin1String("forbidden"))
                allForbidden = false;
        } else {
            // Transient (network, rate limit): keep the last known state —
            // erasing it would flicker every dot on a flaky connection.
            allForbidden = false;
        }
    }
    if (allForbidden && latchEligible) {
        if (++m_forbiddenBatches >= kForbiddenLatchThreshold
            && !m_serverRefused) {
            qCInfo(lcPresence)
                << "server refuses presence for every user; disabling "
                   "presence polling for this session";
            m_serverRefused = true;
            changed = changed || !m_cache.isEmpty();
            m_cache.clear();
            m_inFlight.clear();
            m_burstPending.clear();
            Q_EMIT activeChanged();
        }
    } else if (!allForbidden) {
        m_forbiddenBatches = 0;
    }
    if (changed) {
        ++m_revision;
        Q_EMIT revisionChanged();
    }
}

void PresenceManager::publishTick(bool force)
{
    if (!m_client || !m_client->supportsPresence() || !m_syncing
        || !publishEnabled())
        return;
    const int desired = desiredOwnState();
    if (!force && desired == m_lastPublished)
        return;
    m_client->publishPresence(desired);
    m_lastPublished = desired;
}

void PresenceManager::handleConnectionState(MatrixClient::ConnectionState state)
{
    const bool syncing = state == MatrixClient::Syncing;
    const bool entered = syncing && !m_syncing;
    m_syncing = syncing;
    if (entered) {
        // An owed final offline (sharing disabled while the session was
        // not live, review M3) is flushed FIRST — publishTick's own
        // publishEnabled() gate would otherwise skip it forever.
        if (m_pendingFinalOffline && m_client
            && m_client->supportsPresence() && !publishEnabled()) {
            m_client->publishPresence(2);
            m_lastPublished = 2;
        }
        m_pendingFinalOffline = false;
        // Edge into Syncing (the notification-mode retry precedent): the
        // session just became live — publish our state and refresh every
        // watched dot without waiting for the next scheduled round.
        publishTick(true);
        scheduledPollRound();
    }
}

void PresenceManager::clearSession()
{
    m_cache.clear();
    m_inFlight.clear();
    m_burstPending.clear();
    m_pollOrder.clear();
    m_pollCursor = 0;
    m_forbiddenBatches = 0;
    m_lastPublished = -1;
    m_pendingFinalOffline = false;
    m_syncing = false;
    // The watched set is DROPPED with the session: it names the previous
    // account's contacts, and polling it against the next account's
    // homeserver would leak who the previous account was looking at
    // (review M2). Live PresenceDot instances re-register on the epoch
    // bump below.
    m_watched.clear();
    ++m_sessionEpoch;
    Q_EMIT sessionEpochChanged();
    // A different account (or the next sign-in) may be on a server that
    // does support presence; the latch is per session.
    const bool wasRefused = m_serverRefused;
    m_serverRefused = false;
    ++m_revision;
    Q_EMIT revisionChanged();
    if (wasRefused)
        Q_EMIT activeChanged();
}

int PresenceManager::desiredOwnState() const
{
    if (m_appActive)
        return 0;
    return (m_clock.elapsed() - m_inactiveSinceMs) >= m_idleAfterMs ? 1 : 0;
}

bool PresenceManager::publishEnabled() const
{
    return m_settings && m_settings->sharePresence();
}
