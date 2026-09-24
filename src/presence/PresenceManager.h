#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include "matrix/MatrixClient.h"

class SettingsManager;

// Matrix presence. Sliding Sync delivers no presence events, so presence is
// a bounded polling loop over the users some visible surface has watch()ed,
// with a short debounced burst for newly watched users, and a latch that
// stops polling a server that refuses presence for everyone.
//
// Unknown renders as nothing: a failed or pending lookup, an unsupported
// backend and a presence-disabled server never become a fabricated
// "offline". A transient failure keeps the last known state.
// stateFor()/infoFor() are pure reads, safe in QML bindings; re-read on
// revisionChanged.
//
// Own presence is published by a periodic keep-alive PUT (online, or
// unavailable after a while in the background), gated by the global "share
// presence" setting. Disabling it publishes one final offline.
class PresenceManager : public QObject
{
    Q_OBJECT

    // Backend capability only. Gates the Settings publication card, so it
    // ignores the read-side refusal latch: publication may still run.
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    // supported AND the server has not refused presence reads.
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    // Bumped whenever any cached presence changes.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    // Bumped when a session ends. PresenceDot re-registers its watch on this
    // edge, because clearSession() drops the watched set so one account's
    // list is never polled against the next account's homeserver.
    Q_PROPERTY(int sessionEpoch READ sessionEpoch NOTIFY sessionEpochChanged)
    // True only when presence is known not to be answered: the backend
    // cannot do it, or this server refused it for every user. Everything
    // else stays unknown and renders nothing. Kept explicit rather than
    // derived in QML from `supported && !active`.
    Q_PROPERTY(bool unavailable READ unavailable NOTIFY unavailableChanged)
    // The account's own status. `ownStatusText` is published as the
    // status_msg (emoji first). `ownStatusExpiresAtMs` is 0 for no expiry,
    // else a wall-clock ms deadline; expiry is local and does not federate.
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

    // Ref-counted: a delegate watches on creation and unwatches on
    // destruction. Only watched users are polled.
    Q_INVOKABLE void watch(const QString &userId);
    Q_INVOKABLE void unwatch(const QString &userId);
    // A new remote event is a freshness hint: re-poll a watched sender
    // promptly. Only the server answer changes the rendered state.
    void noteActivity(const QString &userId);
    // Live typing from a watched user contradicts a cached "offline" (a
    // server with presence off answers "offline" for everyone rather than
    // refusing). The claim is withdrawn to unknown, never promoted to
    // "online". "unavailable" is not withdrawn: typing while away is normal.
    void noteTyping(const QString &userId);

    // "online" / "unavailable" / "offline", or "" when unknown.
    Q_INVOKABLE QString stateFor(const QString &userId) const;
    // { state, currentlyActive, lastActiveAgoMs } with the age adjusted to
    // now. lastActiveAgoMs is -1 when the server sent none. Empty when
    // unknown.
    Q_INVOKABLE QVariantMap infoFor(const QString &userId) const;
    // A peer's status text as the last poll reported it ("" = none/unknown).
    Q_INVOKABLE QString statusMessageFor(const QString &userId) const;
    QString ownStatusText() const;
    QString ownStatusEmoji() const { return m_ownStatusEmoji; }
    qint64 ownStatusExpiresAtMs() const { return m_ownStatusExpiresAtMs; }
    Q_INVOKABLE void setOwnStatus(const QString &emoji, const QString &text,
                                  qint64 expiresAtMs);
    Q_INVOKABLE void clearOwnStatus();

    // Test/embedding seam; the ctor also tracks QGuiApplication state.
    void setApplicationActive(bool active);
    // Test seams: the real idle threshold and keep-alive interval are too
    // long for a test.
    void setIdleThresholdForTest(qint64 ms) { m_idleAfterMs = ms; }
    void setPublishIntervalForTest(int ms)
    {
        m_publishJitter = false;   // deterministic cadence for the suite
        m_publishTimer.setInterval(ms);
    }
    void setMinPublishGapForTest(int ms) { m_minPublishGapMs = ms; }
    /// Own-presence PUTs sent and rejected since process start. Monotonic
    /// and deliberately not reset by clearSession(), so a rate can be taken
    /// as the difference of two readings across sessions.
    Q_INVOKABLE qint64 publishAttempts() const { return m_publishAttempts; }
    Q_INVOKABLE qint64 publishRejections() const { return m_publishRejections; }
    /// Retry depth of the current rejection chain, 0 when none is armed.
    /// Exposed because the single restartable retry timer makes the chain
    /// unobservable through publish counts.
    int retryChainForTest() const { return m_retryChain; }
    /// The cap the retry chain plateaus at, so tests assert the real bound.
    static constexpr int maxRetryChain() { return kMaxRetryChain; }
    // The real typing-evidence window is 35 s. Not reset by clearSession():
    // it is a harness value, not session state.
    void setTypingEvidenceWindowForTest(qint64 ms) { m_typingWindowMs = ms; }

Q_SIGNALS:
    void supportedChanged();
    void activeChanged();
    void unavailableChanged();
    void ownStatusChanged();
    void revisionChanged();
    void sessionEpochChanged();

private:
    struct Entry {
        QString state;
        bool currentlyActive = false;
        qint64 lastActiveAgoMs = -1;
        qint64 receivedAtMs = 0;
        QString statusMsg;
    };

    // One round every 30 s, at most kBatchCap GETs for on-screen users.
    static constexpr int kPollIntervalMs = 30000;
    // Newly watched unknown users are answered quickly but debounced, so a
    // list materializing many delegates asks once.
    static constexpr int kBurstDelayMs = 400;
    static constexpr qint64 kFreshWatchMs = 10000;
    // Mirrors PRESENCE_BATCH_CAP in rust/src/presence.rs.
    static constexpr int kBatchCap = 40;
    // Synapse expires a published "online" 33-63 s after the last activity
    // (SYNC_ONLINE_TIMEOUT plus granularity), and simplified sliding sync
    // has no `set_presence` parameter, so this PUT is the only thing keeping
    // the account online. Must stay under 30 s.
    static constexpr int kPublishIntervalMs = 25 * 1000;
    // Drop an unchanged-state publish inside this window. A session start
    // flaps through several connection states and each Syncing edge forces a
    // publish; Synapse's `rc_presence` burst of 1 rejects the duplicate.
    // A real state change still publishes immediately.
    static constexpr int kMinPublishGapMs = 10 * 1000;
    // Keep-alive ticks land up to this much early (21-25 s effective); the
    // retry path adds it instead, to spread rejected devices apart.
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
    // The jitter is added after this clamp, so the effective ceiling is
    // 24 s, still inside the expiry floor. The bound mainly guards against
    // client clock skew when converting a RetryAfter::DateTime hint.
    static constexpr int kRetryAfterCeilingMs = 20 * 1000;
    // Wait used when a rejection carries no hint; stays inside the life of
    // the last accepted publish.
    static constexpr int kRetryAfterUnknownMs = 4 * 1000;
    // After this many consecutive retries the chain gives up and waits for
    // the ordinary tick. The chain also multiplies the wait, but only when
    // the server gave no hint (see onPublishRejected).
    static constexpr int kMaxRetryChain = 4;
    // Continuously in the background this long reads as idle, measured from
    // when focus was lost.
    static constexpr qint64 kIdleAfterMs = 10 * 60 * 1000;
    // Two consecutive all-forbidden batches latch "presence disabled" for
    // the session. A batch counts only with at least this many distinct
    // users, so one user's 403 cannot blind presence for everyone.
    static constexpr int kForbiddenLatchThreshold = 2;
    static constexpr int kForbiddenLatchMinBatch = 2;
    // How long one typing notification contradicts a cached "offline":
    // a poll round plus its answer. Erring long only withholds a dot.
    static constexpr qint64 kTypingEvidenceMs = 35000;

    void pollRound(const char *kind, const QStringList &userIds);
    // Opt-in trace (LIGHTNING_PRESENCE_TRACE, read once at construction):
    // one line per polling decision. Counts and literal tags only, never a
    // user id or display name.
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
    // Own status.
    void loadOwnStatusIfNeeded();
    void persistOwnStatus();
    void armStatusExpiry();
    bool m_ownStatusLoaded = false;
    bool m_statusTimerWired = false;
    QString m_ownStatusEmoji;
    QString m_ownStatusPlainText;
    qint64 m_ownStatusExpiresAtMs = 0;
    QTimer m_statusExpiryTimer;
    // "online" / "unavailable" / "offline" for the local user, or "" when
    // this client is not publishing.
    QString ownPublishedState() const;
    bool isOwnUser(const QString &userId) const;
    // True while live typing evidence contradicts the cache. Pure read.
    bool typingContradicts(const QString &userId) const;
    // Drops expired evidence and bumps the revision; applyBatch only does so
    // when a polled value changes, which on a presence-disabled server never
    // happens.
    void pruneTypingEvidence();

    MatrixClient *m_client = nullptr;
    SettingsManager *m_settings = nullptr;

    QHash<QString, int> m_watched;
    QSet<QString> m_burstPending;
    QHash<QString, Entry> m_cache;
    QSet<quint64> m_inFlight;
    QStringList m_pollOrder;
    int m_pollCursor = 0;

    // userId -> m_clock time of the latest typing notification. Bounded by
    // the watched set.
    QHash<QString, qint64> m_typingSince;

    QTimer m_pollTimer;
    QTimer m_burstTimer;
    QTimer m_publishTimer;
    // One-shot, armed only by a rejection, so retries never disturb the
    // ordinary cadence.
    QTimer m_publishRetryTimer;
    int m_retryChain = 0;
    qint64 m_publishAttempts = 0;
    qint64 m_publishRejections = 0;
    QTimer m_typingTimer;
    QElapsedTimer m_clock;
    // m_clock time of the last PUT sent, for kMinPublishGapMs.
    qint64 m_lastPublishAtMs = -1;
    int m_minPublishGapMs = kMinPublishGapMs;
    // Randomize each tick so several clients of one account do not stay
    // aligned and collide against the burst-1 limiter. This fixes collisions
    // only; it cannot help when the aggregate rate exceeds the per-user limit.
    bool m_publishJitter = true;

    quint64 m_nextOpId = 1;
    int m_revision = 0;
    int m_sessionEpoch = 0;
    int m_forbiddenBatches = 0;
    bool m_serverRefused = false;
    // Per instance so a test can enable the trace; a function-static would
    // freeze the first value for the whole process.
    bool m_traceEnabled = false;
    bool m_appActive = true;
    // When focus was lost (only meaningful while m_appActive is false).
    qint64 m_inactiveSinceMs = 0;
    qint64 m_idleAfterMs = kIdleAfterMs;
    qint64 m_typingWindowMs = kTypingEvidenceMs;
    int m_lastPublished = -1;
    // Sharing was disabled while not live; the final offline is flushed on
    // the next Syncing edge.
    bool m_pendingFinalOffline = false;
    bool m_syncing = false;
};
