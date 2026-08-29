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
    // Read ONCE, per instance (the LIGHTNING_SCROLL_TRACE pattern in
    // TimelineScrollController). Per instance rather than a function
    // static because the presence-manager suite constructs a fresh
    // manager per case and a static would freeze the first value for the
    // whole process.
    , m_traceEnabled(qEnvironmentVariableIsSet("LIGHTNING_PRESENCE_TRACE"))
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

    m_typingTimer.setSingleShot(true);
    connect(&m_typingTimer, &QTimer::timeout,
            this, &PresenceManager::pruneTypingEvidence);

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
        Q_EMIT unavailableChanged();
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
    Q_EMIT unavailableChanged();
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

bool PresenceManager::unavailable() const
{
    // Having no client at all is not a finding: nothing has been asked of
    // any server yet, so the honest answer is silence, not "unavailable".
    // The two cases below are the only ones the client can actually
    // establish — a backend that cannot do presence, and a server that
    // refused it for every user this session.
    return m_client && (!m_client->supportsPresence() || m_serverRefused);
}

void PresenceManager::watch(const QString &userId)
{
    if (userId.isEmpty())
        return;
    const int refs = ++m_watched[userId];
    const auto cached = m_cache.constFind(userId);
    const bool stale = cached == m_cache.constEnd()
        || m_clock.elapsed() - cached->receivedAtMs >= kFreshWatchMs;
    if (refs == 1 && stale && active()) {
        m_burstPending.insert(userId);
        if (!m_burstTimer.isActive())
            m_burstTimer.start();
    }
}

void PresenceManager::noteActivity(const QString &userId)
{
    if (userId.isEmpty() || !active() || !m_syncing
        || !m_watched.contains(userId))
        return;
    m_burstPending.insert(userId);
    // Restarting coalesces a burst of timeline events from one sync into a
    // single authoritative read round.
    m_burstTimer.start();
}

void PresenceManager::noteTyping(const QString &userId)
{
    // The local user is answered from what THIS client publishes, so its own
    // typing says nothing new. Only watched users are recorded: nothing else
    // is ever polled, so nothing else has a cached claim to contradict, and
    // this keeps the map bounded by what is on screen.
    if (userId.isEmpty() || isOwnUser(userId) || !m_watched.contains(userId))
        return;
    const bool wasWithholding = typingContradicts(userId);
    m_typingSince.insert(userId, m_clock.elapsed());
    if (!m_typingTimer.isActive())
        m_typingTimer.start(int(qMax<qint64>(1, m_typingWindowMs)));
    if (typingContradicts(userId) != wasWithholding) {
        // A dot already drawn repaints only on this signal.
        ++m_revision;
        Q_EMIT revisionChanged();
    }
    // Typing is the strongest hint available that the poll answer is about
    // to change, so ask — which is what makes a withheld dot a brief gap on
    // a healthy server rather than a lasting absence. noteActivity owns the
    // watched/active/syncing guards and the debounce.
    noteActivity(userId);
}

bool PresenceManager::typingContradicts(const QString &userId) const
{
    const auto it = m_typingSince.constFind(userId);
    if (it == m_typingSince.constEnd())
        return false;
    // Expired but not yet pruned: the read must not depend on a timer
    // having run.
    if (m_clock.elapsed() - it.value() >= m_typingWindowMs)
        return false;
    const auto cached = m_cache.constFind(userId);
    // ONLY "offline" is contradicted. "unavailable" is the server's own
    // idle heuristic and typing while marked away is ordinary; an unknown
    // user has no claim to withdraw.
    return cached != m_cache.constEnd()
        && cached->state == QLatin1String("offline");
}

void PresenceManager::pruneTypingEvidence()
{
    const qint64 now = m_clock.elapsed();
    bool withheldSomething = false;
    qint64 nextDueAt = -1;
    for (auto it = m_typingSince.begin(); it != m_typingSince.end(); ) {
        const qint64 expiresAt = it.value() + m_typingWindowMs;
        if (expiresAt <= now) {
            const auto cached = m_cache.constFind(it.key());
            if (cached != m_cache.constEnd()
                && cached->state == QLatin1String("offline"))
                withheldSomething = true;
            it = m_typingSince.erase(it);
            continue;
        }
        if (nextDueAt < 0 || expiresAt < nextDueAt)
            nextDueAt = expiresAt;
        ++it;
    }
    if (nextDueAt >= 0)
        m_typingTimer.start(int(qMax<qint64>(1, nextDueAt - now)));
    if (withheldSomething) {
        ++m_revision;
        Q_EMIT revisionChanged();
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
        // Nothing renders this user any more, so the evidence has nothing
        // left to withhold from.
        m_typingSince.remove(userId);
    }
}

QString PresenceManager::ownPublishedState() const
{
    // The local user's presence is the one answer this client does not have
    // to ask for: it is publishing it. Asking the server and rendering the
    // echo is a round trip that can only be wrong, and on a homeserver with
    // presence switched off (Synapse's `presence.enabled: false` answers 200
    // with "offline" for everybody rather than refusing) it IS wrong — the
    // user sat in a live session looking at their own profile card reading
    // "Offline". Reported 2026-08-22 with a screenshot.
    //
    // Reported only when publication is actually on and has actually
    // happened: with sharing disabled the server's "offline" is the truth
    // about what everyone else sees, and this must not paper over it.
    if (!m_client || !m_client->supportsPresence() || !publishEnabled()
        || m_lastPublished < 0)
        return {};
    switch (m_lastPublished) {
    case 0:  return QStringLiteral("online");
    case 1:  return QStringLiteral("unavailable");
    default: return QStringLiteral("offline");
    }
}

bool PresenceManager::isOwnUser(const QString &userId) const
{
    return m_client && !userId.isEmpty()
        && m_client->currentUserId() == userId;
}

QString PresenceManager::stateFor(const QString &userId) const
{
    if (isOwnUser(userId)) {
        const QString own = ownPublishedState();
        if (!own.isEmpty())
            return own;
    }
    // A live typing notification withdraws a contradicted "offline"; the
    // answer becomes unknown, which renders nothing.
    if (typingContradicts(userId))
        return {};
    const auto it = m_cache.constFind(userId);
    return it == m_cache.constEnd() ? QString() : it->state;
}

QVariantMap PresenceManager::infoFor(const QString &userId) const
{
    if (isOwnUser(userId)) {
        const QString own = ownPublishedState();
        if (!own.isEmpty()) {
            return QVariantMap{
                { QStringLiteral("state"), own },
                { QStringLiteral("currentlyActive"), m_appActive },
                // Zero, not -1: "active now" is exactly what this client is
                // telling the server, and a fabricated age would be the one
                // part of this answer we did not know.
                { QStringLiteral("lastActiveAgoMs"), qint64(0) },
            };
        }
    }
    // The card formats its whole sentence from this map, so the withdrawal
    // has to reach it too — an empty map is how this class says "unknown".
    if (typingContradicts(userId))
        return {};
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
    if (!active() || !m_syncing || m_watched.isEmpty()) {
        // Every one of these looks identical to the user — no dot at all —
        // so the trace has to name WHICH gate closed. The order matches
        // the conditions above: a null client is not "unsupported", and a
        // latched session is not "not syncing".
        traceRound("scheduled",
                   !m_client ? "no_client"
                   : !m_client->supportsPresence() ? "unsupported"
                   : m_serverRefused ? "latched"
                   : !m_syncing ? "not_syncing"
                   : "nothing_watched",
                   0, 0);
        return;
    }
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
    pollRound("scheduled", round);
}

void PresenceManager::burstRound()
{
    if (!active() || !m_syncing || m_burstPending.isEmpty()) {
        // Note what this drop means and why the scheduled round is the
        // recovery: a burst queued while the session was not live is
        // DISCARDED here, not deferred. The Syncing edge re-polls the
        // whole watched set, so nothing is lost — but the discarded burst
        // is invisible without this line.
        traceRound("burst",
                   !m_client ? "no_client"
                   : !m_client->supportsPresence() ? "unsupported"
                   : m_serverRefused ? "latched"
                   : !m_syncing ? "not_syncing"
                   : "nothing_pending",
                   0, 0);
        m_burstPending.clear();
        return;
    }
    QStringList round;
    for (const QString &userId : std::as_const(m_burstPending)) {
        if (m_watched.contains(userId))
            round.append(userId);
        if (round.size() >= kBatchCap)
            break;
    }
    m_burstPending.clear();
    if (round.isEmpty()) {
        // Everything pending was unwatched again before the debounce
        // fired (a delegate created and destroyed inside 400 ms).
        traceRound("burst", "pending_unwatched", 0, 0);
        return;
    }
    pollRound("burst", round);
}

void PresenceManager::pollRound(const char *kind, const QStringList &userIds)
{
    if (!m_client || userIds.isEmpty()) {
        traceRound(kind, m_client ? "empty_batch" : "no_client", 0, 0);
        return;
    }
    // Answers dropped by the lifecycle guard never clear their op id;
    // bound the set by evicting the OLDEST ids (they are monotonic) —
    // wholesale clearing discarded legitimately pending rounds whose
    // answers were then rejected as stale (review L2).
    while (m_inFlight.size() > 64)
        m_inFlight.remove(*std::min_element(m_inFlight.cbegin(),
                                            m_inFlight.cend()));
    const quint64 opId = m_nextOpId++;
    m_inFlight.insert(opId);
    // Traced BEFORE the request, and applyBatch traces the matching
    // answer. A dispatch line with no answer line is the ONLY evidence of
    // a request the backend swallowed: requestPresence() returns void, its
    // two early-outs (not logged in, no Rust handle) are silent, a
    // synchronous FFI rejection only logs a category-gated warning under
    // lightning.rust, and the Rust task's lifecycle guard returns without
    // enqueuing anything. In every one of those the op id simply stays in
    // m_inFlight until it is evicted.
    traceRound(kind, "dispatched", static_cast<int>(userIds.size()), opId);
    m_client->requestPresence(userIds, opId);
}

void PresenceManager::traceRound(const char *kind, const char *reason,
                                 int batch, quint64 opId) const
{
    if (!m_traceEnabled)
        return;
    // qInfo rather than qCDebug(lcPresence): a diagnostic that needed BOTH
    // an environment variable and QT_LOGGING_RULES would be a trap for a
    // remote tester, and the row-reveal trace in ReverseListProxyModel
    // sets the precedent. Counts, booleans and string literals only —
    // never a user id, never a display name, never a list.
    qInfo("presence-round kind=%s reason=%s watched=%d pending=%d "
          "supported=%d active=%d syncing=%d appActive=%d inFlight=%d "
          "batch=%d op=%llu",
          kind, reason, static_cast<int>(m_watched.size()),
          static_cast<int>(m_burstPending.size()), supported() ? 1 : 0,
          active() ? 1 : 0, m_syncing ? 1 : 0, m_appActive ? 1 : 0,
          static_cast<int>(m_inFlight.size()), batch,
          static_cast<unsigned long long>(opId));
}

void PresenceManager::applyBatch(quint64 opId, const QVariantList &entries)
{
    if (!m_inFlight.remove(opId)) {
        if (m_traceEnabled) {
            qInfo("presence-batch op=%llu stale=1 entries=%d",
                  static_cast<unsigned long long>(opId),
                  static_cast<int>(entries.size()));
        }
        return;
    }
    bool changed = false;
    // Trace accounting only. Every one of these outcomes renders as the
    // same absent dot, which is exactly why the counts have to be
    // separable in a capture.
    int okCount = 0;
    int forbiddenCount = 0;
    int notFoundCount = 0;
    int transientCount = 0;
    int malformedCount = 0;
    int onlineCount = 0;
    int awayCount = 0;
    int offlineCount = 0;
    int unrenderableCount = 0;
    int erasedCount = 0;
    // A batch feeds the refusal latch only when it is broad enough that
    // "everyone forbidden" plausibly means "the server refuses presence"
    // rather than one user's federation/membership quirk (review L1). A
    // too-small batch neither advances nor resets the latch count.
    //
    // Counted in DISTINCT user ids, which is what kForbiddenLatchMinBatch
    // has always claimed to mean: entries.size() would let one user's
    // repeated 403 look like a broad refusal, and the latch blinds
    // presence for the whole session, so it is the one place worth
    // spending a QSet on. Bounded by the batch cap the bridge enforces.
    QSet<QString> distinctUsers;
    bool allForbidden = !entries.isEmpty();
    for (const QVariant &value : entries) {
        const QVariantMap entry = value.toMap();
        const QString userId = entry.value(QStringLiteral("userId")).toString();
        if (userId.isEmpty()) {
            ++malformedCount;
            allForbidden = false;
            continue;
        }
        distinctUsers.insert(userId);
        if (entry.value(QStringLiteral("ok")).toBool()) {
            ++okCount;
            allForbidden = false;
            const QString state =
                entry.value(QStringLiteral("state")).toString();
            if (!isRenderableState(state)) {
                ++unrenderableCount;
                const int removed = static_cast<int>(m_cache.remove(userId));
                erasedCount += removed;
                changed = removed > 0 || changed;
                continue;
            }
            if (state == QLatin1String("online"))
                ++onlineCount;
            else if (state == QLatin1String("unavailable"))
                ++awayCount;
            else
                ++offlineCount;
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
            const int removed = static_cast<int>(m_cache.remove(userId));
            erasedCount += removed;
            changed = removed > 0 || changed;
            if (category != QLatin1String("forbidden")) {
                ++notFoundCount;
                allForbidden = false;
            } else {
                ++forbiddenCount;
            }
        } else {
            // Transient (network, rate limit): keep the last known state —
            // erasing it would flicker every dot on a flaky connection.
            ++transientCount;
            allForbidden = false;
        }
    }
    bool latchArmedNow = false;
    const bool latchEligible =
        distinctUsers.size() >= kForbiddenLatchMinBatch;
    if (allForbidden && latchEligible) {
        if (++m_forbiddenBatches >= kForbiddenLatchThreshold
            && !m_serverRefused) {
            qCInfo(lcPresence)
                << "server refuses presence for every user; disabling "
                   "presence polling for this session";
            m_serverRefused = true;
            latchArmedNow = true;
            changed = changed || !m_cache.isEmpty();
            m_cache.clear();
            m_inFlight.clear();
            m_burstPending.clear();
            Q_EMIT activeChanged();
            // The one state the UI is allowed to disclose: from here the
            // client KNOWS this session's server will not answer, so the
            // profile popover may say so instead of staying silent.
            Q_EMIT unavailableChanged();
        }
    } else if (!allForbidden) {
        m_forbiddenBatches = 0;
    }
    if (changed) {
        ++m_revision;
        Q_EMIT revisionChanged();
    }
    if (m_traceEnabled) {
        // The state distribution is the field that separates the two
        // shapes a presence-disabled homeserver can take: refusals
        // (forbidden=N, which eventually latches) versus a server that
        // answers 200 with a flat offline for everyone (ok=N offline=N,
        // which never latches and renders a grey dot on every avatar).
        qInfo("presence-batch op=%llu entries=%d ok=%d forbidden=%d "
              "not_found=%d transient=%d malformed=%d online=%d away=%d "
              "offline=%d unrenderable=%d erased=%d latchEligible=%d "
              "forbiddenStreak=%d latchArmed=%d latched=%d",
              static_cast<unsigned long long>(opId),
              static_cast<int>(entries.size()), okCount, forbiddenCount,
              notFoundCount, transientCount, malformedCount, onlineCount,
              awayCount, offlineCount, unrenderableCount, erasedCount,
              latchEligible ? 1 : 0, m_forbiddenBatches,
              latchArmedNow ? 1 : 0, m_serverRefused ? 1 : 0);
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
    const int previous = m_lastPublished;
    m_client->publishPresence(desired);
    m_lastPublished = desired;
    // stateFor()/infoFor() answer the local user from m_lastPublished, so
    // the dot and the profile line only move when the revision does.
    if (previous != m_lastPublished) {
        ++m_revision;
        Q_EMIT revisionChanged();
    }
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
    // Typing evidence names the previous account's contacts, exactly like
    // the watched set below.
    m_typingSince.clear();
    m_typingTimer.stop();
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
    if (wasRefused) {
        Q_EMIT activeChanged();
        Q_EMIT unavailableChanged();
    }
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
