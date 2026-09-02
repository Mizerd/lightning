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

// v0.7.x Matrix presence (design-handoff follow-up).
//
// Sliding Sync delivers NO presence events, so presence is a bounded
// polling loop, and this class owns the entire policy: WHO is polled
// (exactly the users some visible surface has watch()ed — DM rows, the
// open People list, an open profile popover), HOW OFTEN (one bounded batch
// per round, plus a short debounced burst when a new unknown user appears),
// and WHEN TO STOP (a homeserver that answers forbidden for everyone has
// presence disabled; polling it forever would be noise).
//
// Honesty rules, matching the receipt/facepile precedents:
//   - Unknown is rendered as NOTHING. A failed lookup, a not-yet-looked-up
//     user, an unsupported backend and a presence-disabled server are all
//     indistinguishable "no indicator" — never fabricated offline.
//   - A transient network failure keeps the last known state rather than
//     erasing it; only an authoritative answer replaces an answer.
//   - stateFor()/infoFor() are pure reads, safe in QML bindings; re-read
//     on revisionChanged.
//
// Own-presence publication is the other half: a periodic keep-alive PUT
// (servers expire presence quickly) of online — or unavailable once the
// application has been in the background for a while — gated by the
// application-wide "share presence" privacy setting (global like the
// link-preview switches, not per-account). Disabling the setting
// publishes one final offline so the account does not linger online.
class PresenceManager : public QObject
{
    Q_OBJECT

    // Backend capability alone: true whenever the client CAN do presence.
    // This is what gates the Settings publication card — the card must
    // never disappear while publication can still run (review M1), so it
    // deliberately ignores the read-side refusal latch below.
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    // supported AND the server has not refused presence reads; QML uses it
    // only to skip watch() bookkeeping — an inactive manager already
    // answers "" for every user.
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    // Bumped whenever any cached presence changes. Bindings reference it to
    // re-evaluate stateFor()/infoFor().
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    // Bumped when a session ends (sign-out / account switch). PresenceDot
    // re-registers its watch on this edge, because clearSession() drops
    // the watched set (review M2: one account's watch list must never be
    // polled against the next account's homeserver).
    Q_PROPERTY(int sessionEpoch READ sessionEpoch NOTIFY sessionEpochChanged)
    // The ONLY two conditions under which the client actually KNOWS that
    // presence will not be answered: the backend cannot do presence at
    // all, or this session's server refused it for every user (the latch
    // below). Everything else — an unanswered lookup, a user we have not
    // polled yet, a transient failure — stays UNKNOWN, and unknown must
    // keep rendering nothing. This property exists so the profile popover
    // can say "Presence unavailable" for the two honest cases without
    // QML having to infer them from `supported && !active`, which reads
    // like a coincidence and would silently acquire a third meaning the
    // day either property gains a condition.
    Q_PROPERTY(bool unavailable READ unavailable NOTIFY unavailableChanged)
    // v0.9 (phase 10): the account's own status. `ownStatusText` is what is
    // published as the spec presence status_msg (emoji first, as ordinary
    // characters, so every client sees it); `ownStatusExpiresAtMs` is 0 for
    // no expiry, else a wall-clock ms timestamp after which the status is
    // cleared — on the timer while running, on the next start otherwise.
    // Expiry is a LIGHTNING convenience (it does not federate): the
    // published text simply disappears when the deadline passes.
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

    // Ref-counted visibility: a delegate watches on creation and unwatches
    // on destruction. Watching is idempotent per caller and cheap; only
    // watched users are ever polled.
    Q_INVOKABLE void watch(const QString &userId);
    Q_INVOKABLE void unwatch(const QString &userId);
    // A newly received remote event is a freshness hint, never a presence
    // assertion. Re-poll a watched sender promptly; only the server answer
    // may change the rendered state.
    void noteActivity(const QString &userId);
    // A live typing notification about a WATCHED user. It is the one
    // present-tense, server-forwarded fact this client receives about
    // somebody else, and it CONTRADICTS a cached "offline": a homeserver
    // with presence switched off answers 200 with "offline" for everybody
    // rather than refusing, so the refusal latch never fires and the dot is
    // confidently wrong forever (the same defect ownPublishedState() already
    // records for the local user, which reached nobody else).
    //
    // The contradicted claim is WITHDRAWN, never replaced. The state becomes
    // unknown, and unknown renders nothing — promoting it to "online" would
    // be the same fabrication in the other direction, since typing proves
    // activity and presence is a state the server owns. Only "offline" is
    // withdrawn: "unavailable" is a soft idle heuristic and someone typing
    // while marked away is ordinary, not a contradiction.
    void noteTyping(const QString &userId);

    // "online" / "unavailable" / "offline", or "" when unknown.
    Q_INVOKABLE QString stateFor(const QString &userId) const;
    // { state, currentlyActive, lastActiveAgoMs } with the age adjusted to
    // NOW (the server age plus time since the answer arrived), so QML can
    // format "last active …" without its own clock bookkeeping.
    // lastActiveAgoMs is -1 when the server sent none. Empty map when
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

    // Test/embedding seam; the ctor also tracks QGuiApplication state when
    // one exists.
    void setApplicationActive(bool active);
    // Test seams: the real idle threshold is minutes and the real
    // keep-alive interval is 4 minutes — the idle/publish contracts are
    // untestable at those scales (review L7).
    void setIdleThresholdForTest(qint64 ms) { m_idleAfterMs = ms; }
    void setPublishIntervalForTest(int ms) { m_publishTimer.setInterval(ms); }
    // The real typing-evidence window is 35 s; that it EXPIRES is untestable
    // at that scale. Deliberately not reset by clearSession() — it is a
    // harness value, not session state.
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

    // One polling round every 30 s keeps a visible dot honest without
    // meaningfully loading the server (each round is at most
    // kBatchCap GETs for users that are actually on screen).
    static constexpr int kPollIntervalMs = 30000;
    // Newly watched unknown users are answered quickly (a popover should
    // not wait half a minute), but debounced so a People list materializing
    // 30 delegates asks once, not 30 times.
    static constexpr int kBurstDelayMs = 400;
    static constexpr qint64 kFreshWatchMs = 10000;
    // Mirrors PRESENCE_BATCH_CAP in rust/src/presence.rs.
    static constexpr int kBatchCap = 40;
    // Servers expire presence after a few minutes without activity, so the
    // keep-alive must be comfortably faster than that.
    static constexpr int kPublishIntervalMs = 4 * 60 * 1000;
    // The app being CONTINUOUSLY in the background this long reads as
    // "idle" — measured from the moment focus was lost (review H2: an
    // earlier draft measured from the moment focus was GAINED, so any
    // session focused longer than this published Away the instant the
    // user switched windows).
    static constexpr qint64 kIdleAfterMs = 10 * 60 * 1000;
    // Two consecutive all-forbidden batches latch "this server has
    // presence disabled" for the rest of the session. A batch only counts
    // when it carries at least this many distinct users (review L1: a
    // single user's 403 — a federation edge, an invited-not-joined member
    // — must not blind presence for everyone).
    static constexpr int kForbiddenLatchThreshold = 2;
    static constexpr int kForbiddenLatchMinBatch = 2;
    // How long one typing notification keeps contradicting a cached
    // "offline". Long enough to cover a poll round (30 s) plus its answer,
    // short enough that a stale contradiction cannot outlive the typing it
    // came from by much. Withholding is the conservative direction — it
    // renders nothing — so erring slightly long costs an absent dot, never
    // a wrong one.
    static constexpr qint64 kTypingEvidenceMs = 35000;

    void pollRound(const char *kind, const QStringList &userIds);
    // Opt-in diagnostic (env LIGHTNING_PRESENCE_TRACE, read ONCE at
    // construction — the LIGHTNING_SCROLL_TRACE pattern). One bounded line
    // per polling round decision; applyBatch emits the matching answer
    // line itself. Counts and literal tags only: never a user id, never a
    // display name, never a list. `reason` is always a string literal.
    void traceRound(const char *kind, const char *reason, int batch,
                    quint64 opId) const;
    void scheduledPollRound();
    void burstRound();
    void applyBatch(quint64 opId, const QVariantList &entries);
    void publishTick(bool force);
    void handleConnectionState(MatrixClient::ConnectionState state);
    void clearSession();
    int desiredOwnState() const;
    bool publishEnabled() const;
    // v0.9 own status.
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
    // this client is not publishing and therefore does not know.
    QString ownPublishedState() const;
    bool isOwnUser(const QString &userId) const;
    // True while live typing evidence contradicts what the cache holds for
    // this user. Pure read: stateFor()/infoFor() are QML-binding safe.
    bool typingContradicts(const QString &userId) const;
    // Drops expired evidence and ANNOUNCES it. applyBatch only bumps the
    // revision when a polled VALUE changes, and on a presence-disabled
    // server the cached value is "offline" throughout — so without this the
    // withheld dot would never come back until an unrelated update happened
    // along.
    void pruneTypingEvidence();

    MatrixClient *m_client = nullptr;
    SettingsManager *m_settings = nullptr;

    QHash<QString, int> m_watched;
    QSet<QString> m_burstPending;
    QHash<QString, Entry> m_cache;
    QSet<quint64> m_inFlight;
    QStringList m_pollOrder;
    int m_pollCursor = 0;

    // userId -> m_clock time of the most recent typing notification.
    // Bounded by the WATCHED set (nothing else is ever recorded), which is
    // bounded by what is on screen.
    QHash<QString, qint64> m_typingSince;

    QTimer m_pollTimer;
    QTimer m_burstTimer;
    QTimer m_publishTimer;
    QTimer m_typingTimer;
    QElapsedTimer m_clock;

    quint64 m_nextOpId = 1;
    int m_revision = 0;
    int m_sessionEpoch = 0;
    int m_forbiddenBatches = 0;
    bool m_serverRefused = false;
    // Read once at construction so a test can enable the trace per
    // instance; a function-static would freeze the first value for the
    // whole process.
    bool m_traceEnabled = false;
    bool m_appActive = true;
    // When focus was LOST (only meaningful while m_appActive is false).
    qint64 m_inactiveSinceMs = 0;
    qint64 m_idleAfterMs = kIdleAfterMs;
    qint64 m_typingWindowMs = kTypingEvidenceMs;
    int m_lastPublished = -1;
    // The user disabled sharing while the session was not live; the final
    // offline is owed and flushed on the next Syncing edge (review M3).
    bool m_pendingFinalOffline = false;
    bool m_syncing = false;
};
