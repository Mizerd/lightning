#include "PresenceManager.h"

#include <algorithm>

#include <QEventLoop>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QVariantList>

#include "app/SettingsManager.h"
#include "presence/GameDetection.h"

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
}

Q_LOGGING_CATEGORY(lcPresence, "lightning.presence")

namespace {
bool isRenderableState(const QString &state)
{
    return state == QLatin1String("online")
        || state == QLatin1String("unavailable")
        || state == QLatin1String("offline");
}
}

PresenceManager::PresenceManager(QObject *parent)
    : QObject(parent)
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
    connect(m_client, &MatrixClient::presencePublished, this,
            &PresenceManager::onPublishAccepted);
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
                    m_pendingFinalOffline = true;
                    return;
                }
                if (m_lastPublished != 2) {
                    sendOffline();
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
    m_burstTimer.start();
}

void PresenceManager::noteTyping(const QString &userId)
{
    if (userId.isEmpty() || isOwnUser(userId) || !m_watched.contains(userId))
        return;
    const bool wasWithholding = typingContradicts(userId);
    m_typingSince.insert(userId, m_clock.elapsed());
    if (!m_typingTimer.isActive())
        m_typingTimer.start(int(qMax<qint64>(1, m_typingWindowMs)));
    if (typingContradicts(userId) != wasWithholding) {
        ++m_revision;
        Q_EMIT revisionChanged();
    }
    noteActivity(userId);
}

bool PresenceManager::typingContradicts(const QString &userId) const
{
    const auto it = m_typingSince.constFind(userId);
    if (it == m_typingSince.constEnd())
        return false;
    if (m_clock.elapsed() - it.value() >= m_typingWindowMs)
        return false;
    const auto cached = m_cache.constFind(userId);
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
                { QStringLiteral("lastActiveAgoMs"), qint64(0) },
                { QStringLiteral("statusMsg"), effectiveStatusText() },
            };
        }
    }
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
    if (!activeNow)
        m_inactiveSinceMs = m_clock.elapsed();
    publishTick(false);
}

void PresenceManager::scheduledPollRound()
{
    if (!active() || !m_syncing || m_watched.isEmpty()) {
        traceRound("scheduled",
                   !m_client ? "no_client"
                   : !m_client->supportsPresence() ? "unsupported"
                   : m_serverRefused ? "latched"
                   : !m_syncing ? "not_syncing"
                   : "nothing_watched",
                   0, 0);
        return;
    }
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
    traceRound(kind, "dispatched", static_cast<int>(userIds.size()), opId);
    m_client->requestPresence(userIds, opId);
}

void PresenceManager::traceRound(const char *kind, const char *reason,
                                 int batch, quint64 opId) const
{
    if (!m_traceEnabled)
        return;
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
        || !publishEnabled()) {
        if (m_traceEnabled) {
            const char *reason = !m_client ? "no-client"
                : !m_client->supportsPresence() ? "unsupported"
                : !m_syncing ? "not-syncing"
                : "sharing-off";
            if (m_lastSkipReason != reason) {
                m_lastSkipReason = reason;
                qInfo("presence-publish-skipped reason=%s force=%d attempts=%lld "
                      "upMs=%lld",
                      reason, force ? 1 : 0,
                      static_cast<long long>(m_publishAttempts),
                      static_cast<long long>(m_clock.elapsed()));
            }
        }
        return;
    }
    m_lastSkipReason = nullptr;
    const int desired = desiredOwnState();
    if (!force && desired == m_lastPublished)
        return;
    // A forced republish of an unchanged state is dropped inside the rate
    // window: the Syncing edge fires more than once during start-up, and the
    // duplicate PUT is what the server rate-limits. Not after a rejection,
    // where there is no accepted state to duplicate.
    if (!afterRejection && force && !m_statusDirty && desired == m_lastPublished
        && m_lastPublishAtMs >= 0
        && m_clock.elapsed() - m_lastPublishAtMs < m_minPublishGapMs)
        return;
    const int previous = m_lastPublished;
    loadOwnStatusIfNeeded();
    if (!afterRejection) {
        m_publishRetryTimer.stop();
        m_retryChain = 0;
    }
    ++m_publishAttempts;
    m_statusDirty = false;
    sendPresence(desired, effectiveStatusText());
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
    if (m_publishJitter) {
        m_publishTimer.setInterval(kPublishIntervalMs
                                   - QRandomGenerator::global()->bounded(
                                       kPublishJitterMs));
    }
    if (previous != m_lastPublished) {
        ++m_revision;
        Q_EMIT revisionChanged();
    }
}

void PresenceManager::onPublishRejected(const QString &category,
                                        qint64 retryAfterMs)
{
    ++m_publishRejections;
    m_publishesInFlight = qMax(0, m_publishesInFlight - 1);
    if (m_farewellPending) {
        if (category != QLatin1String("rate_limited")) {
            settleFarewellIfQuiet();
            return;
        }
        m_farewell429Unresolved = true;
        if (m_publishRetryTimer.isActive())
            return;
        const qint64 wait = retryAfterMs > 0 ? qMax<qint64>(retryAfterMs, 200) : 1000;
        const qint64 left = m_farewellBudgetMs - m_farewellClock.elapsed();
        if (!m_farewellRetried && wait < left) {
            m_farewellRetried = true;
            m_publishRetryTimer.start(static_cast<int>(wait));
        } else {
            finishFarewell();
        }
        return;
    }
    if (category != QLatin1String("rate_limited")) {
        qCDebug(lcPresence) << "own-presence publish failed:" << category;
        return;
    }
    if (m_retryChain >= kMaxRetryChain) {
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
        if (m_pendingFinalOffline && m_client
            && m_client->supportsPresence() && !publishEnabled()) {
            sendOffline();
            m_lastPublished = 2;
        }
        m_pendingFinalOffline = false;
        publishTick(true);
        scheduledPollRound();
        if (!m_hasSession) {
            m_hasSession = true;
            Q_EMIT sessionLiveChanged();
        }
    }
}

void PresenceManager::clearSession()
{
    m_ownStatusLoaded = false;
    m_ownStatusEmoji.clear();
    m_ownStatusPlainText.clear();
    m_ownStatusExpiresAtMs = 0;
    m_statusExpiryTimer.stop();
    Q_EMIT ownStatusChanged();
    m_cache.clear();
    m_inFlight.clear();
    m_burstPending.clear();
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
    m_gameName.clear();
    m_musicText.clear();
    m_overlaysWithdrawn = false;
    m_withdrawnGame.clear();
    m_withdrawnMusic.clear();
    m_farewellPending = false;
    m_farewell429Unresolved = false;
    m_publishesInFlight = 0;
    const bool hadSession = m_hasSession;
    m_hasSession = false;
    // The watched set names the previous account's contacts; polling it
    // against the next homeserver would leak them. Live PresenceDots
    // re-register on the epoch bump.
    m_watched.clear();
    ++m_sessionEpoch;
    Q_EMIT sessionEpochChanged();
    const bool wasRefused = m_serverRefused;
    m_serverRefused = false;
    ++m_revision;
    Q_EMIT revisionChanged();
    if (wasRefused) {
        Q_EMIT activeChanged();
        Q_EMIT unavailableChanged();
    }
    if (hadSession)
        Q_EMIT sessionLiveChanged();
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

QString PresenceManager::ownStatusText() const
{
    if (m_ownStatusPlainText.isEmpty() && m_ownStatusEmoji.isEmpty())
        return {};
    if (m_ownStatusEmoji.isEmpty())
        return m_ownStatusPlainText;
    if (m_ownStatusPlainText.isEmpty())
        return m_ownStatusEmoji;
    return m_ownStatusEmoji + QLatin1Char(' ') + m_ownStatusPlainText;
}

QString PresenceManager::statusMessageFor(const QString &userId) const
{
    if (isOwnUser(userId))
        return effectiveStatusText();
    const auto it = m_cache.constFind(userId);
    return it == m_cache.constEnd() ? QString() : it->statusMsg;
}

QString PresenceManager::effectiveStatusText() const
{
    const QString own = ownStatusText();
    if (!own.isEmpty())
        return own;
    if (!m_gameName.isEmpty())
        return gamedetect::statusTextFor(m_gameName);
    return m_musicText;
}

void PresenceManager::setMusicActivity(const QString &statusText, bool publishNow)
{
    const QString text = displaySafeStatus(statusText.simplified().left(200));
    if (m_overlaysWithdrawn) {
        m_withdrawnMusic = text;
        return;
    }
    if (text == m_musicText)
        return;
    loadOwnStatusIfNeeded();
    const QString before = effectiveStatusText();
    m_musicText = text;
    if (effectiveStatusText() == before)
        return;
    ++m_revision;
    Q_EMIT revisionChanged();
    Q_EMIT ownStatusChanged();
    if (publishNow && m_client && m_client->supportsPresence() && m_syncing
        && publishEnabled() && m_lastPublished >= 0 && m_lastPublished != 2) {
        m_statusDirty = true;
        publishTick(true);
    }
}

void PresenceManager::setGameActivity(const QString &gameName)
{
    const QString name = displaySafeStatus(gameName.simplified().left(120));
    if (m_overlaysWithdrawn) {
        m_withdrawnGame = name;
        return;
    }
    if (name == m_gameName)
        return;
    loadOwnStatusIfNeeded();
    const QString before = effectiveStatusText();
    m_gameName = name;
    ++m_revision;
    Q_EMIT revisionChanged();
    Q_EMIT ownStatusChanged();
    if (effectiveStatusText() != before && m_client && m_client->supportsPresence()
        && m_syncing && publishEnabled() && m_lastPublished >= 0 && m_lastPublished != 2) {
        m_statusDirty = true;
        publishTick(true);
    }
}

bool PresenceManager::withdrawActivityLine(bool *withdrewNow)
{
    if (withdrewNow)
        *withdrewNow = !m_overlaysWithdrawn;
    loadOwnStatusIfNeeded();
    const QString before = effectiveStatusText();
    if (!m_overlaysWithdrawn) {
        m_withdrawnGame = m_gameName;
        m_withdrawnMusic = m_musicText;
        m_overlaysWithdrawn = true;
    }
    m_gameName.clear();
    m_musicText.clear();
    if (effectiveStatusText() == before)
        return false;
    ++m_revision;
    Q_EMIT revisionChanged();
    Q_EMIT ownStatusChanged();
    if (!m_client || !m_client->supportsPresence() || !m_syncing || !publishEnabled()
        || m_lastPublished < 0 || m_lastPublished == 2)
        return false;
    const qint64 attempts = m_publishAttempts;
    m_farewellPending = true;
    m_farewellRetried = false;
    m_farewell429Unresolved = false;
    m_farewellResendNeeded = m_publishesInFlight > 0;
    m_farewellBudgetMs = kFarewellBudgetMs;
    m_farewellClock.start();
    m_statusDirty = true;
    publishTick(true);
    if (m_publishAttempts == attempts)
        m_farewellPending = false;
    return m_farewellPending;
}

void PresenceManager::restoreActivityLine()
{
    if (!m_overlaysWithdrawn)
        return;
    m_overlaysWithdrawn = false;
    if (m_farewellPending)
        m_publishRetryTimer.stop();
    m_farewellPending = false;
    m_farewellRetried = false;
    m_farewell429Unresolved = false;
    m_farewellResendNeeded = false;
    loadOwnStatusIfNeeded();
    const QString before = effectiveStatusText();
    m_gameName = m_withdrawnGame;
    m_musicText = m_withdrawnMusic;
    m_withdrawnGame.clear();
    m_withdrawnMusic.clear();
    if (effectiveStatusText() == before)
        return;
    ++m_revision;
    Q_EMIT revisionChanged();
    Q_EMIT ownStatusChanged();
    if (m_client && m_client->supportsPresence() && m_syncing && publishEnabled()
        && m_lastPublished >= 0 && m_lastPublished != 2) {
        m_statusDirty = true;
        publishTick(true);
    }
}

void PresenceManager::sendPresence(int state, const QString &statusMsg)
{
    if (m_farewellPending) {
        m_farewell429Unresolved = false;
        if (m_publishesInFlight == 0)
            m_farewellResendNeeded = false;
    }
    ++m_publishesInFlight;
    m_client->publishPresence(state, statusMsg);
}

void PresenceManager::sendOffline()
{
    ++m_publishesInFlight;
    m_client->publishPresence(2);
}

void PresenceManager::onPublishAccepted()
{
    m_publishesInFlight = qMax(0, m_publishesInFlight - 1);
    if (m_farewellPending)
        settleFarewellIfQuiet();
}

void PresenceManager::settleFarewellIfQuiet()
{
    if (m_publishesInFlight != 0 || m_farewell429Unresolved
        || m_publishRetryTimer.isActive())
        return;
    if (m_farewellResendNeeded && m_farewellClock.elapsed() < m_farewellBudgetMs) {
        m_farewellResendNeeded = false;
        const qint64 attempts = m_publishAttempts;
        m_statusDirty = true;
        publishTick(true, true);
        if (m_publishAttempts != attempts)
            return;
    }
    finishFarewell();
}

void PresenceManager::finishFarewell()
{
    if (!m_farewellPending)
        return;
    m_farewellPending = false;
    m_publishRetryTimer.stop();
    Q_EMIT farewellSettled();
}

void PresenceManager::whenFarewellSettled(int budgetMs, QObject *context,
                                          std::function<void()> then)
{
    QObject *owner = context ? context : this;
    if (!m_farewellPending) {
        QMetaObject::invokeMethod(owner, then, Qt::QueuedConnection);
        return;
    }
    m_farewellBudgetMs = budgetMs;
    auto *waiter = new QObject(owner);
    auto done = std::make_shared<bool>(false);
    auto fire = [waiter, done, then, owner]() {
        if (*done)
            return;
        *done = true;
        waiter->deleteLater();
        QMetaObject::invokeMethod(owner, then, Qt::QueuedConnection);
    };
    connect(this, &PresenceManager::farewellSettled, waiter, fire);
    const qint64 left = qMax<qint64>(0, budgetMs - m_farewellClock.elapsed());
    QTimer::singleShot(static_cast<int>(left), waiter, [this, fire] {
        finishFarewell();
        fire();
    });
}

bool PresenceManager::settleFarewell(int budgetMs)
{
    if (!m_farewellPending)
        return true;
    m_farewellBudgetMs = budgetMs;
    QEventLoop loop;
    bool settled = false;
    const auto conn = connect(this, &PresenceManager::farewellSettled, &loop, [&] {
        settled = true;
        loop.quit();
    });
    QTimer bound;
    bound.setSingleShot(true);
    connect(&bound, &QTimer::timeout, &loop, &QEventLoop::quit);
    bound.start(static_cast<int>(qMax<qint64>(0, budgetMs - m_farewellClock.elapsed())));
    if (m_farewellPending)
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    disconnect(conn);
    if (!settled) {
        m_farewellPending = false;
        m_publishRetryTimer.stop();
    }
    return settled;
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
    if (m_client && m_client->supportsPresence() && m_syncing && publishEnabled()
        && m_lastPublished >= 0 && m_lastPublished != 2)
        sendPresence(m_lastPublished, effectiveStatusText());
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
        && publishEnabled() && m_lastPublished >= 0 && m_lastPublished != 2)
        sendPresence(m_lastPublished, effectiveStatusText());
    ++m_revision;
    Q_EMIT revisionChanged();
}
