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

public:
    explicit PresenceManager(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void setSettings(SettingsManager *settings);

    bool supported() const;
    bool active() const;
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

    // "online" / "unavailable" / "offline", or "" when unknown.
    Q_INVOKABLE QString stateFor(const QString &userId) const;
    // { state, currentlyActive, lastActiveAgoMs } with the age adjusted to
    // NOW (the server age plus time since the answer arrived), so QML can
    // format "last active …" without its own clock bookkeeping.
    // lastActiveAgoMs is -1 when the server sent none. Empty map when
    // unknown.
    Q_INVOKABLE QVariantMap infoFor(const QString &userId) const;

    // Test/embedding seam; the ctor also tracks QGuiApplication state when
    // one exists.
    void setApplicationActive(bool active);
    // Test seams: the real idle threshold is minutes and the real
    // keep-alive interval is 4 minutes — the idle/publish contracts are
    // untestable at those scales (review L7).
    void setIdleThresholdForTest(qint64 ms) { m_idleAfterMs = ms; }
    void setPublishIntervalForTest(int ms) { m_publishTimer.setInterval(ms); }

Q_SIGNALS:
    void supportedChanged();
    void activeChanged();
    void revisionChanged();
    void sessionEpochChanged();

private:
    struct Entry {
        QString state;
        bool currentlyActive = false;
        qint64 lastActiveAgoMs = -1;
        qint64 receivedAtMs = 0;
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

    void pollRound(const QStringList &userIds);
    void scheduledPollRound();
    void burstRound();
    void applyBatch(quint64 opId, const QVariantList &entries);
    void publishTick(bool force);
    void handleConnectionState(MatrixClient::ConnectionState state);
    void clearSession();
    int desiredOwnState() const;
    bool publishEnabled() const;

    MatrixClient *m_client = nullptr;
    SettingsManager *m_settings = nullptr;

    QHash<QString, int> m_watched;
    QSet<QString> m_burstPending;
    QHash<QString, Entry> m_cache;
    QSet<quint64> m_inFlight;
    QStringList m_pollOrder;
    int m_pollCursor = 0;

    QTimer m_pollTimer;
    QTimer m_burstTimer;
    QTimer m_publishTimer;
    QElapsedTimer m_clock;

    quint64 m_nextOpId = 1;
    int m_revision = 0;
    int m_sessionEpoch = 0;
    int m_forbiddenBatches = 0;
    bool m_serverRefused = false;
    bool m_appActive = true;
    // When focus was LOST (only meaningful while m_appActive is false).
    qint64 m_inactiveSinceMs = 0;
    qint64 m_idleAfterMs = kIdleAfterMs;
    int m_lastPublished = -1;
    // The user disabled sharing while the session was not live; the final
    // offline is owed and flushed on the next Syncing edge (review M3).
    bool m_pendingFinalOffline = false;
    bool m_syncing = false;
};
