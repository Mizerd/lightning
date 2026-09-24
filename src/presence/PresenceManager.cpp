#include "PresenceManager.h"

#include <algorithm>

#include <QGuiApplication>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QVariantList>

#include "app/SettingsManager.h"

namespace {
// Status text is shown where Qt may auto-detect HTML, so markup-like text has
// its angle brackets swapped for single angle quotes. "<3" is untouched.
QString displaySafeStatus(QString text)
{
    static const QRegularExpression tagLike(QStringLiteral("<\\s*/?[A-Za-z!]"));
    if (tagLike.match(text).hasMatch()) {
        text.replace(QLatin1Char('<'), QChar(0x2039));
        text.replace(QLatin1Char('>'), QChar(0x203A));
    }
    return text;
}
} // namespace

Q_LOGGING_CATEGORY(lcPresence, "lightning.presence")

namespace {
// The only states an indicator may render; anything else erases the entry.
bool isRenderableState(const QString &state)
{
    return state == QLatin1String("online")
        || state == QLatin1String("unavailable")
        || state == QLatin1String("offline");
}
} // namespace

PresenceManager::PresenceManager(QObject *parent)
    : QObject(parent)
    // Per instance rather than a function static: tests construct a fresh
    // manager per case.
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
    m_publishRetryTimer.setSingleShot(true);
    connect(&m_publishRetryTimer, &QTimer::timeout,
            this, [this]() { publishTick(true, true); });

    // Headless tests have no QGuiApplication; they use setApplicationActive.
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
            &PresenceManager::onPublishRejected);
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
                    // Not live: the final offline is owed and flushed on
                    // the next Syncing edge.
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
    // No client means nothing has been asked yet, so silence rather than
    // "unavailable".
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
    // Restarting coalesces one sync's events into a single read round.
    m_burstTimer.start();
}

void PresenceManager::noteTyping(const QString &userId)
{
    // The local user's own typing says nothing new. Only watched users are
    // recorded, which keeps the map bounded by what is on screen.
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
    // Typing suggests the poll answer is about to change, so re-poll; a
    // withheld dot is then only a brief gap on a healthy server.
    noteActivity(userId);
}

bool PresenceManager::typingContradicts(const QString &userId) const
{
    const auto it = m_typingSince.constFind(userId);
    if (it == m_typingSince.constEnd())
        return false;
    // Expired but not yet pruned: the read must not depend on the timer.
    if (m_clock.elapsed() - it.value() >= m_typingWindowMs)
        return false;
    const auto cached = m_cache.constFind(userId);
    // Only "offline" is contradicted; "unavailable" is the server's idle
    // heuristic and typing while away is ordinary.
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
        m_typingSince.remove(userId);
    }
}

QString PresenceManager::ownPublishedState() const
{
    // The local user's presence is what this client publishes, so answer it
    // locally: a server with presence disabled echoes "offline" for everyone.
    // Only when publication is enabled and has happened; with sharing off,
    // the server's "offline" is what everyone else sees.
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
    // Typing withdraws a contradicted "offline" to unknown.
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
                // "Active now" is exactly what this client is publishing.
                { QStringLiteral("lastActiveAgoMs"), qint64(0) },
                { QStringLiteral("statusMsg"), ownStatusText() },
            };
        }
    }
    // The withdrawal must reach the card too; empty map means unknown.
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
        { QStringLiteral("statusMsg"), it->statusMsg },
    };
}

void PresenceManager::setApplicationActive(bool activeNow)
{
    if (m_appActive == activeNow)
        return;
    m_appActive = activeNow;
    // The idle clock starts when focus is lost, not when it was gained.
    if (!activeNow)
        m_inactiveSinceMs = m_clock.elapsed();
    // Edge-triggered: publish only if the resulting state changed; the
    // keep-alive tick covers the rest.
    publishTick(false);
}

void PresenceManager::scheduledPollRound()
{
    if (!active() || !m_syncing || m_watched.isEmpty()) {
        // Every gate looks the same to the user, so the trace names which
        // one closed.
        traceRound("scheduled",
                   !m_client ? "no_client"
                   : !m_client->supportsPresence() ? "unsupported"
                   : m_serverRefused ? "latched"
                   : !m_syncing ? "not_syncing"
                   : "nothing_watched",
                   0, 0);
        return;
    }
    // Stable rotation so a watched set larger than one batch is covered
    // across rounds.
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
        // A burst queued while not live is discarded, not deferred; the
        // Syncing edge re-polls the whole watched set.
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
        // Everything pending was unwatched before the debounce fired.
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
    // Answers dropped by the lifecycle guard never clear their op id; evict
    // the oldest ids (monotonic) rather than clearing pending rounds.
    while (m_inFlight.size() > 64)
        m_inFlight.remove(*std::min_element(m_inFlight.cbegin(),
                                            m_inFlight.cend()));
    const quint64 opId = m_nextOpId++;
    m_inFlight.insert(opId);
    // Traced before the request; applyBatch traces the answer. A dispatch
    // with no answer is the only evidence of a request the backend dropped
    // silently.
    traceRound(kind, "dispatched", static_cast<int>(userIds.size()), opId);
    m_client->requestPresence(userIds, opId);
}

void PresenceManager::traceRound(const char *kind, const char *reason,
                                 int batch, quint64 opId) const
{
    if (!m_traceEnabled)
        return;
    // qInfo, not a category, so one environment variable is enough for a
    // tester. Counts, booleans and literals only; never a user id or name.
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
    // Trace accounting: every outcome renders as the same absent dot.
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
    // A batch feeds the refusal latch only when broad enough that "everyone
    // forbidden" means the server refuses presence, not one user's quirk.
    // Counted in distinct user ids; a too-small batch neither advances nor
    // resets the count.
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
            cached.statusMsg =
                displaySafeStatus(entry.value(QStringLiteral("statusMsg")).toString());
            cached.receivedAtMs = m_clock.elapsed();
            m_cache.insert(userId, cached);
            changed = true;
            continue;
        }
        const QString category =
            entry.value(QStringLiteral("category")).toString();
        if (category == QLatin1String("forbidden")
            || category == QLatin1String("not_found")) {
            // Authoritative "no presence": drop what we had. Only forbidden
            // counts toward the latch.
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
            // Transient: keep the last known state to avoid flicker.
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
            // The client now knows this server will not answer.
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
        // The state distribution separates refusals (forbidden=N, latches)
        // from a server answering "offline" for everyone (ok=N offline=N,
        // never latches).
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

void PresenceManager::publishTick(bool force, bool afterRejection)
{
    if (!m_client || !m_client->supportsPresence() || !m_syncing
        || !publishEnabled())
        return;
    const int desired = desiredOwnState();
    if (!force && desired == m_lastPublished)
        return;
    // A forced republish of an unchanged state is dropped inside the rate
    // window: the Syncing edge fires more than once during start-up, and the
    // duplicate PUT is what the server rate-limits. Not after a rejection,
    // where there is no accepted state to duplicate.
    if (!afterRejection && force && desired == m_lastPublished
        && m_lastPublishAtMs >= 0
        && m_clock.elapsed() - m_lastPublishAtMs < m_minPublishGapMs)
        return;
    const int previous = m_lastPublished;
    loadOwnStatusIfNeeded();
    // Reset the retry chain where a publish is actually sent, not on every
    // tick, so at most one retry is ever pending.
    if (!afterRejection) {
        m_publishRetryTimer.stop();
        m_retryChain = 0;
    }
    ++m_publishAttempts;
    m_client->publishPresence(desired, ownStatusText());
    // Log the offered rate. ~2.4 attempts/min is one healthy client; near 6
    // means the Syncing-edge path is firing at its 10 s gap.
    if (m_traceEnabled) {
        const qint64 upMs = m_clock.elapsed();
        const double perMin = upMs > 0
                                  ? double(m_publishAttempts) * 60000.0 / double(upMs)
                                  : 0.0;
        qInfo("presence-publish state=%d afterRejection=%d chain=%d "
              "attempts=%lld rejections=%lld upMs=%lld attemptsPerMin=%.2f",
              desired, afterRejection ? 1 : 0, m_retryChain,
              static_cast<long long>(m_publishAttempts),
              static_cast<long long>(m_publishRejections),
              static_cast<long long>(upMs), perMin);
    }
    m_lastPublished = desired;
    m_lastPublishAtMs = m_clock.elapsed();
    // Re-arm with fresh jitter so clients of one account drift apart.
    if (m_publishJitter) {
        m_publishTimer.setInterval(kPublishIntervalMs
                                   - QRandomGenerator::global()->bounded(
                                       kPublishJitterMs));
    }
    // stateFor()/infoFor() answer the local user from m_lastPublished.
    if (previous != m_lastPublished) {
        ++m_revision;
        Q_EMIT revisionChanged();
    }
}

// A rejected publish arms a short retry instead of waiting a full period;
// see the constants in the header.
void PresenceManager::onPublishRejected(const QString &category,
                                        qint64 retryAfterMs)
{
    ++m_publishRejections;
    // Only rate limiting is retried here; anything else is left to the
    // ordinary tick. Note the forbidden latch is on the read path only, so a
    // server that rejects every PUT still receives one per period.
    if (category != QLatin1String("rate_limited")) {
        qCDebug(lcPresence) << "own-presence publish failed:" << category;
        return;
    }
    if (m_retryChain >= kMaxRetryChain) {
        // Give up rather than spin; the next ordinary tick resets the chain.
        qCDebug(lcPresence) << "own-presence rate limited; retry chain"
                            << "exhausted, waiting for the next tick";
        return;
    }
    ++m_retryChain;
    // Use the server's hint, bounded at both ends. A blind retry stays under
    // the expiry so it lands within the life of the last accepted publish.
    int wait = retryAfterMs > 0
                   ? static_cast<int>(
                         qBound(static_cast<qint64>(kRetryAfterFloorMs),
                                retryAfterMs,
                                static_cast<qint64>(kRetryAfterCeilingMs)))
                   : kRetryAfterUnknownMs;
    // Do not multiply a server hint by the chain depth: Synapse's hints
    // already escalate, and doubling them overshoots. Only the blind default
    // backs off.
    if (retryAfterMs <= 0)
        wait = qMin(wait * m_retryChain, kRetryAfterCeilingMs);
    if (m_publishJitter)
        wait += QRandomGenerator::global()->bounded(kPublishJitterMs);
    qCDebug(lcPresence) << "own-presence rate limited; retrying in" << wait
                        << "ms (attempt" << m_retryChain << "of"
                        << kMaxRetryChain << ", server hint"
                        << retryAfterMs << "ms)";
    m_publishRetryTimer.start(wait);
}

void PresenceManager::handleConnectionState(MatrixClient::ConnectionState state)
{
    const bool syncing = state == MatrixClient::Syncing;
    const bool entered = syncing && !m_syncing;
    m_syncing = syncing;
    if (entered) {
        // Flush an owed final offline first; publishTick's publishEnabled()
        // gate would otherwise skip it.
        if (m_pendingFinalOffline && m_client
            && m_client->supportsPresence() && !publishEnabled()) {
            m_client->publishPresence(2);
            m_lastPublished = 2;
        }
        m_pendingFinalOffline = false;
        // The session just became live: publish and refresh every watched
        // dot now.
        publishTick(true);
        scheduledPollRound();
    }
}

void PresenceManager::clearSession()
{
    // The own status is re-read for the next account from ITS settings.
    m_ownStatusLoaded = false;
    m_ownStatusEmoji.clear();
    m_ownStatusPlainText.clear();
    m_ownStatusExpiresAtMs = 0;
    m_statusExpiryTimer.stop();
    Q_EMIT ownStatusChanged();
    m_cache.clear();
    m_inFlight.clear();
    m_burstPending.clear();
    // Typing evidence names the previous account's contacts.
    m_typingSince.clear();
    m_typingTimer.stop();
    m_pollOrder.clear();
    m_pollCursor = 0;
    m_forbiddenBatches = 0;
    m_lastPublished = -1;
    m_lastPublishAtMs = -1;
    // A retry armed for the previous account must not fire after an
    // account switch; it would also bypass kMinPublishGapMs.
    m_publishRetryTimer.stop();
    m_retryChain = 0;
    m_pendingFinalOffline = false;
    m_syncing = false;
    // The watched set names the previous account's contacts; polling it
    // against the next homeserver would leak them. Live PresenceDots
    // re-register on the epoch bump.
    m_watched.clear();
    ++m_sessionEpoch;
    Q_EMIT sessionEpochChanged();
    // The refusal latch is per session.
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

// ---------------------------------------------------------------------------
// Own status message
// ---------------------------------------------------------------------------

QString PresenceManager::ownStatusText() const
{
    if (m_ownStatusPlainText.isEmpty() && m_ownStatusEmoji.isEmpty())
        return {};
    // The emoji rides as ordinary text so every client shows it.
    if (m_ownStatusEmoji.isEmpty())
        return m_ownStatusPlainText;
    if (m_ownStatusPlainText.isEmpty())
        return m_ownStatusEmoji;
    return m_ownStatusEmoji + QLatin1Char(' ') + m_ownStatusPlainText;
}

QString PresenceManager::statusMessageFor(const QString &userId) const
{
    if (isOwnUser(userId))
        return ownStatusText();
    const auto it = m_cache.constFind(userId);
    return it == m_cache.constEnd() ? QString() : it->statusMsg;
}

void PresenceManager::loadOwnStatusIfNeeded()
{
    if (m_ownStatusLoaded)
        return;
    m_ownStatusLoaded = true;
    if (!m_settings)
        return;
    const QVariantMap stored = m_settings->ownPresenceStatus();
    if (stored.isEmpty())
        return;
    const qint64 expires = stored.value(QStringLiteral("expiresAtMs")).toLongLong();
    if (expires > 0 && QDateTime::currentMSecsSinceEpoch() >= expires) {
        // Expired while closed: clear it so it is never republished.
        m_settings->setOwnPresenceStatus({});
        return;
    }
    m_ownStatusEmoji = stored.value(QStringLiteral("emoji")).toString();
    m_ownStatusPlainText = stored.value(QStringLiteral("text")).toString();
    m_ownStatusExpiresAtMs = expires;
    armStatusExpiry();
    Q_EMIT ownStatusChanged();
}

void PresenceManager::persistOwnStatus()
{
    if (!m_settings)
        return;
    if (m_ownStatusEmoji.isEmpty() && m_ownStatusPlainText.isEmpty()) {
        m_settings->setOwnPresenceStatus({});
        return;
    }
    m_settings->setOwnPresenceStatus(QVariantMap{
        { QStringLiteral("emoji"), m_ownStatusEmoji },
        { QStringLiteral("text"), m_ownStatusPlainText },
        { QStringLiteral("expiresAtMs"), m_ownStatusExpiresAtMs },
    });
}

void PresenceManager::armStatusExpiry()
{
    m_statusExpiryTimer.stop();
    if (m_ownStatusExpiresAtMs <= 0)
        return;
    const qint64 remaining =
        m_ownStatusExpiresAtMs - QDateTime::currentMSecsSinceEpoch();
    if (remaining <= 0) {
        clearOwnStatus();
        return;
    }
    // QTimer is 32-bit: a deadline further out than ~24 days is re-armed
    // when it fires, never truncated.
    m_statusExpiryTimer.setSingleShot(true);
    m_statusExpiryTimer.setInterval(
        static_cast<int>(qMin<qint64>(remaining, 24LL * 60 * 60 * 1000)));
    if (!m_statusTimerWired) {
        m_statusTimerWired = true;
        connect(&m_statusExpiryTimer, &QTimer::timeout, this,
                [this] { armStatusExpiry(); });
    }
    m_statusExpiryTimer.start();
}

void PresenceManager::setOwnStatus(const QString &emoji, const QString &text,
                                   qint64 expiresAtMs)
{
    loadOwnStatusIfNeeded();
    // Bounded here as well as in Rust: one status line, not a paragraph.
    const QString cleanText = displaySafeStatus(text.simplified().left(200));
    const QString cleanEmoji = emoji.trimmed().left(8);
    if (cleanText.isEmpty() && cleanEmoji.isEmpty()) {
        clearOwnStatus();
        return;
    }
    m_ownStatusEmoji = cleanEmoji;
    m_ownStatusPlainText = cleanText;
    m_ownStatusExpiresAtMs = expiresAtMs > 0 ? expiresAtMs : 0;
    persistOwnStatus();
    armStatusExpiry();
    Q_EMIT ownStatusChanged();
    // Publish now: the status is part of the presence event.
    if (m_client && m_client->supportsPresence() && m_syncing && publishEnabled()
        && m_lastPublished >= 0)
        m_client->publishPresence(m_lastPublished, ownStatusText());
    ++m_revision;
    Q_EMIT revisionChanged();
}

void PresenceManager::clearOwnStatus()
{
    m_ownStatusLoaded = true;
    const bool had = !m_ownStatusEmoji.isEmpty() || !m_ownStatusPlainText.isEmpty();
    m_ownStatusEmoji.clear();
    m_ownStatusPlainText.clear();
    m_ownStatusExpiresAtMs = 0;
    m_statusExpiryTimer.stop();
    persistOwnStatus();
    Q_EMIT ownStatusChanged();
    if (had && m_client && m_client->supportsPresence() && m_syncing
        && publishEnabled() && m_lastPublished >= 0)
        m_client->publishPresence(m_lastPublished, QString());
    ++m_revision;
    Q_EMIT revisionChanged();
}
