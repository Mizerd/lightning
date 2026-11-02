#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include <functional>

#include "matrix/MatrixClient.h"

class SettingsManager;

class PresenceManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(int sessionEpoch READ sessionEpoch NOTIFY sessionEpochChanged)
    Q_PROPERTY(bool unavailable READ unavailable NOTIFY unavailableChanged)
    Q_PROPERTY(QString ownStatusText READ ownStatusText NOTIFY ownStatusChanged)
    Q_PROPERTY(QString ownStatusEmoji READ ownStatusEmoji NOTIFY ownStatusChanged)
    Q_PROPERTY(qint64 ownStatusExpiresAtMs READ ownStatusExpiresAtMs
                   NOTIFY ownStatusChanged)

public:
    explicit PresenceManager(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void setSettings(SettingsManager *settings);

    bool supported() const;
    bool active() const;
    bool unavailable() const;
    int revision() const { return m_revision; }
    int sessionEpoch() const { return m_sessionEpoch; }

    Q_INVOKABLE void watch(const QString &userId);
    Q_INVOKABLE void unwatch(const QString &userId);
    void noteActivity(const QString &userId);
    void noteTyping(const QString &userId);

    Q_INVOKABLE QString stateFor(const QString &userId) const;
    Q_INVOKABLE QVariantMap infoFor(const QString &userId) const;
    Q_INVOKABLE QString statusMessageFor(const QString &userId) const;
    QString ownStatusText() const;
    QString ownStatusEmoji() const { return m_ownStatusEmoji; }
    qint64 ownStatusExpiresAtMs() const { return m_ownStatusExpiresAtMs; }
    Q_INVOKABLE void setOwnStatus(const QString &emoji, const QString &text,
                                  qint64 expiresAtMs);
    Q_INVOKABLE void clearOwnStatus();
    void setGameActivity(const QString &gameName);
    QString gameActivity() const { return m_gameName; }
    void setMusicActivity(const QString &statusText, bool publishNow = false);
    QString musicActivity() const { return m_musicText; }
    bool withdrawActivityLine(bool *withdrewNow = nullptr);
    bool activityLineWithdrawn() const { return m_overlaysWithdrawn; }
    void restoreActivityLine();
    void whenFarewellSettled(int budgetMs, QObject *context, std::function<void()> then);
    bool settleFarewell(int budgetMs);
    bool farewellPending() const { return m_farewellPending; }
    static constexpr int kFarewellBudgetMs = 3000;
    static constexpr int kSignOutFarewellBudgetMs = 11000;
    int publishesInFlight() const { return m_publishesInFlight; }
    bool sessionLive() const { return m_hasSession; }
    QString effectiveStatusText() const;

    void setApplicationActive(bool active);
    void setIdleThresholdForTest(qint64 ms) { m_idleAfterMs = ms; }
    void setPublishIntervalForTest(int ms)
    {
        m_publishJitter = false;
        m_publishTimer.setInterval(ms);
    }
    void setMinPublishGapForTest(int ms) { m_minPublishGapMs = ms; }
    Q_INVOKABLE qint64 publishAttempts() const { return m_publishAttempts; }
    Q_INVOKABLE qint64 publishRejections() const { return m_publishRejections; }
    int retryChainForTest() const { return m_retryChain; }
    static constexpr int maxRetryChain() { return kMaxRetryChain; }
    void setTypingEvidenceWindowForTest(qint64 ms) { m_typingWindowMs = ms; }

Q_SIGNALS:
    void supportedChanged();
    void activeChanged();
    void unavailableChanged();
    void ownStatusChanged();
    void revisionChanged();
    void sessionEpochChanged();
    void sessionLiveChanged();
    void farewellSettled();

private:
    struct Entry {
        QString state;
        bool currentlyActive = false;
        qint64 lastActiveAgoMs = -1;
        qint64 receivedAtMs = 0;
        QString statusMsg;
    };

    static constexpr int kPollIntervalMs = 30000;
    static constexpr int kBurstDelayMs = 400;
    static constexpr qint64 kFreshWatchMs = 10000;
    static constexpr int kBatchCap = 40;
    // Synapse expires a published "online" 33-63 s after the last activity
    // (SYNC_ONLINE_TIMEOUT plus granularity), and simplified sliding sync
    // has no `set_presence` parameter, so this PUT is the only thing keeping
    // the account online. Must stay under 30 s.
    static constexpr int kPublishIntervalMs = 25 * 1000;
    static constexpr int kMinPublishGapMs = 10 * 1000;
    static constexpr int kPublishJitterMs = 4 * 1000;
    // `rc_presence` is per user (default 0.1/s, burst 1). Jitter spreads a
    // user's devices apart but does not lower their aggregate rate, so with
    // several devices signed in many ticks are rejected, and a device that
    // waits a full period after each rejection can be starved indefinitely.
    // A rejection therefore arms a short one-shot retry, using the server's
    // hint when given, clamped so a hint of 0 cannot busy-loop and a huge one
    // cannot park the keep-alive.
    //
    // What matters is the gap between accepted publishes per account, not
    // per client: presence is per user, so a starved client is invisible
    // while a sibling gets through. publishAttempts() and the
    // `presence-publish` trace measure the offered rate; the rejection ratio
    // alone cannot distinguish many publishers from a flapping connection.
    // See docs/round-history.md (2026-09-19).
    static constexpr int kRetryAfterFloorMs = 1500;
    static constexpr int kRetryAfterCeilingMs = 20 * 1000;
    static constexpr int kRetryAfterUnknownMs = 4 * 1000;
    static constexpr int kMaxRetryChain = 4;
    static constexpr qint64 kIdleAfterMs = 10 * 60 * 1000;
    // Two consecutive all-forbidden batches latch "presence disabled" for
    // the session. A batch counts only with at least this many distinct
    // users, so one user's 403 cannot blind presence for everyone.
    static constexpr int kForbiddenLatchThreshold = 2;
    static constexpr int kForbiddenLatchMinBatch = 2;
    static constexpr qint64 kTypingEvidenceMs = 35000;

    void pollRound(const char *kind, const QStringList &userIds);
    void traceRound(const char *kind, const char *reason, int batch,
                    quint64 opId) const;
    void scheduledPollRound();
    void burstRound();
    void applyBatch(quint64 opId, const QVariantList &entries);
    // `afterRejection` bypasses kMinPublishGapMs: that window suppresses a
    // duplicate of an accepted state, and after a rejection nothing was
    // accepted.
    void publishTick(bool force, bool afterRejection = false);
    void onPublishRejected(const QString &category, qint64 retryAfterMs);
    void handleConnectionState(MatrixClient::ConnectionState state);
    void clearSession();
    int desiredOwnState() const;
    bool publishEnabled() const;
    void loadOwnStatusIfNeeded();
    void persistOwnStatus();
    void armStatusExpiry();
    bool m_ownStatusLoaded = false;
    bool m_statusTimerWired = false;
    QString m_ownStatusEmoji;
    QString m_ownStatusPlainText;
    QString m_gameName;
    QString m_musicText;
    void finishFarewell();
    void sendPresence(int state, const QString &statusMsg);
    void sendOffline();
    void onPublishAccepted();
    void settleFarewellIfQuiet();
    int m_publishesInFlight = 0;
    bool m_farewell429Unresolved = false;
    // A PUT was in flight at the withdrawal: the server may apply it AFTER the
    // farewell, so the farewell is sent once more after the last earlier
    // answer (inside the same budget). Cleared by any farewell PUT sent while
    // nothing else is in flight.
    bool m_farewellResendNeeded = false;
    bool m_overlaysWithdrawn = false;
    QString m_withdrawnGame;
    QString m_withdrawnMusic;
    bool m_farewellPending = false;
    bool m_farewellRetried = false;
    int m_farewellBudgetMs = kFarewellBudgetMs;
    QElapsedTimer m_farewellClock;
    bool m_hasSession = false;
    bool m_statusDirty = false;
    qint64 m_ownStatusExpiresAtMs = 0;
    QTimer m_statusExpiryTimer;
    QString ownPublishedState() const;
    bool isOwnUser(const QString &userId) const;
    bool typingContradicts(const QString &userId) const;
    void pruneTypingEvidence();

    MatrixClient *m_client = nullptr;
    SettingsManager *m_settings = nullptr;

    QHash<QString, int> m_watched;
    QSet<QString> m_burstPending;
    QHash<QString, Entry> m_cache;
    QSet<quint64> m_inFlight;
    QStringList m_pollOrder;
    int m_pollCursor = 0;

    QHash<QString, qint64> m_typingSince;

    QTimer m_pollTimer;
    QTimer m_burstTimer;
    QTimer m_publishTimer;
    QTimer m_publishRetryTimer;
    int m_retryChain = 0;
    qint64 m_publishAttempts = 0;
    qint64 m_publishRejections = 0;
    QTimer m_typingTimer;
    QElapsedTimer m_clock;
    qint64 m_lastPublishAtMs = -1;
    int m_minPublishGapMs = kMinPublishGapMs;
    bool m_publishJitter = true;

    quint64 m_nextOpId = 1;
    int m_revision = 0;
    int m_sessionEpoch = 0;
    int m_forbiddenBatches = 0;
    bool m_serverRefused = false;
    bool m_traceEnabled = false;
    const char *m_lastSkipReason = nullptr;
    bool m_appActive = true;
    qint64 m_inactiveSinceMs = 0;
    qint64 m_idleAfterMs = kIdleAfterMs;
    qint64 m_typingWindowMs = kTypingEvidenceMs;
    int m_lastPublished = -1;
    bool m_pendingFinalOffline = false;
    bool m_syncing = false;
};
