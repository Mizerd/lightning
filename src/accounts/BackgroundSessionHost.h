// One background account's live session (live accounts, Labs; design
// §3.3.1): a thin host around its own Background Rust handle, drained on the
// GUI thread by BackgroundDrainer.
//
// Every drain pass, within its share of the tick's budget:
//  1. bg_poll (atomic loads, no allocation). When dirty, bg_take: the counts
//     table becomes this account's badge summary through the SAME rule the
//     foreground list uses (roomunread::countsAsUnread / mentionCount, with
//     roomNotificationModeFor(uid, room)); never ported to Rust. Rooms whose
//     unread state flipped are told to NotificationManager, which withdraws
//     their cards when read elsewhere.
//  2. When the session-state version moved: the sticky slot (login result,
//     initial sync, room-list state, sync errors, the ignored list, the
//     sign-out result).
//  3. The queue, at most 64 events: timeline_event and event_decrypted through
//     NotificationContextBuilder into processEvent/processDecrypted with this
//     account's own context; event_redacted withdraws; a verification request
//     raises a labelled card and its cancellation withdraws it; anything else
//     is counted and dropped without being parsed.
//  4. When the token generation moved or the session was revoked: custody,
//     handed to the pool (onCustody) for the keyring worker or revocation.
//
// I8, binding and source-scanned (HostSettingsScanTest): nothing here reads or
// writes the ACTIVE account's state. Uid-explicit settings reads only; no
// client, no media bridge, no keyring. The card's avatar stays empty: the
// foreground's media bridge would fetch with another account's token.
//
// Logs carry acct=<safeUserSlug> and redactId() room/event ids only.
#pragma once

#include "accounts/RustBridgeApi.h"
#include "accounts/SessionScheduler.h"
#include "notifications/NotificationManager.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

class AccountActivityModel;
class SettingsManager;

namespace lightning::accounts {

class BackgroundSessionHost : public QObject
{
    Q_OBJECT
public:
    static constexpr int kMaxEventsPerPass = 64;

    struct Deps {
        RustBridgeApi *api = nullptr;
        SettingsManager *settings = nullptr;
        NotificationManager *notifications = nullptr;
        AccountActivityModel *activity = nullptr;
        std::function<qint64()> monotonicMs;
        // SENSITIVE: called with custody whenever the token generation moved
        // or the session was revoked. Never log the snapshot.
        std::function<void(BackgroundSessionHost *, const CustodySnapshot &)> onCustody;
    };
    // Who this host is, recorded at start from the account record and the
    // recorded store location; never re-derived later.
    struct Identity {
        QString userId;
        QString deviceId;
        QString homeserver;
        QString storePath;
        QString slug;     // safeUserSlug(userId), for logs
        QString label;    // the account's display name, else its user id
        bool oauth = false;
    };
    enum class Phase : quint8 { Starting, Running, Reconnecting, Stopping };
    enum class FailureKind : quint8 {
        Transient,              // a network or server failure: backoff
        StoreInUse,             // store_in_use: Store busy, no retry
        EncryptionDataMissing   // crypto_account_missing / _mismatch: no retry, nothing created
    };
    Q_ENUM(FailureKind)

    BackgroundSessionHost(Deps deps, Identity identity, void *handle, quint64 storeSerial,
                          quint64 epoch, QObject *parent = nullptr);
    ~BackgroundSessionHost() override;

    const Identity &identity() const { return m_identity; }
    void *handle() const { return m_handle; }
    quint64 storeSerial() const { return m_storeSerial; }
    quint64 epoch() const { return m_epoch; }
    Phase phase() const { return m_phase; }
    void setPhase(Phase phase) { m_phase = phase; }
    bool initialSyncDone() const { return m_initialSyncDone; }

    // The handle leaves this host for the retirement pool; nothing reaches it
    // from here afterwards.
    void *takeHandle();

    // One pass. Returns true when it did more than poll (for the drain stats).
    bool drainOnce();
    // The token step alone (bg_poll, then custody when the generation moved
    // or the session was revoked): what a drainer tick runs for a host its
    // budget did not reach, so a waiting rotation is answered every tick.
    void drainTokensOnly();

    // Catch-up (the scheduler's semaphore): begun by the pool, checked every
    // pass, reported once by catchUpComplete().
    void beginCatchUp();
    bool catchingUp() const { return m_catchUp.active(); }

    // Removal in progress (the account's row and cards are already gone):
    // keep draining, so the sign-out result is read, but raise no card,
    // withdrawal or badge any more.
    void setQuiet(bool quiet) { m_quiet = quiet; }
    bool quiet() const { return m_quiet; }

    // Notification modes changed for this account (or for a room): forget the
    // cached modes and re-push the local-rule set.
    void refreshModes(const QString &roomId = QString());

    // ---- test hooks ----------------------------------------------------------
    NotificationManager::Context lastContextForTest() const { return m_lastContext; }
    int droppedEventsForTest() const { return m_droppedEvents; }
    int notifyEventsForTest() const { return m_notifyEvents; }
    /// event_decrypted events for an id never handed over as an undecryptable
    /// timeline_event, raised as new candidates (review WP1-R N2).
    int lateDecryptedEventsForTest() const { return m_lateDecryptedEvents; }
    QSet<QString> unreadRoomsForTest() const { return m_unreadRooms; }
    QStringList ignoredUsersForTest() const { return QStringList(m_ignored.begin(), m_ignored.end()); }

Q_SIGNALS:
    void loginSucceeded();
    void loginFailed(lightning::accounts::BackgroundSessionHost::FailureKind kind);
    // room_list_sync_state went offline while running.
    void connectionLost();
    void rateLimited(qint64 retryAfterMs);
    void catchUpComplete();
    // The sticky logged_out result (a server sign-out this pool asked for).
    void loggedOut(const QString &result);
    void summaryUpdated();

private:
    void applyTake(const QByteArray &json);
    bool takeCustodyIfMoved(const BgStatus &status);
    void applySessionState(const QByteArray &json);
    void handleQueued(const QString &raw);
    void handleTimelineEvent(const QJsonObject &event, bool decrypted);
    void rememberForwardedUtd(const QString &eventId);
    NotificationManager::Context contextFor(const QString &roomId, const QJsonObject &bg,
                                            const TimelineEvent &event);
    int modeFor(const QString &roomId);
    QString verificationCardKey(const QString &flowId) const;
    bool changed(const QString &type, const QJsonObject &payload);
    qint64 now() const;

    Deps m_deps;
    Identity m_identity;
    void *m_handle;
    quint64 m_storeSerial;
    quint64 m_epoch;
    Phase m_phase = Phase::Starting;
    bool m_quiet = false;

    BgStatus m_status;
    quint64 m_seenSessionVersion = 0;
    bool m_sessionStateEverTaken = false;
    quint64 m_seenTokenGeneration = 0;
    bool m_revokedSeen = false;
    QHash<QString, QJsonObject> m_sticky;   // last payload per sticky type acted on
    bool m_initialSyncDone = false;
    QString m_roomListState;
    QSet<QString> m_ignored;
    CatchUpTracker m_catchUp;

    // The last take's counts table, per room (a room absent from it is zero).
    struct RoomCounts {
        int unread = 0;
        int highlight = 0;
        int flags = 0;
    };
    QHash<QString, RoomCounts> m_rooms;
    int m_invites = 0;
    QStringList m_localRules;
    void recomputeSummary(bool force);
    QSet<QString> m_unreadRooms;
    QHash<QString, int> m_modes;
    bool m_haveSummary = false;
    int m_lastUnreadRooms = -1;
    int m_lastMentions = -1;
    int m_lastInvites = -1;
    bool m_lastLowerBound = false;

    NotificationManager::Context m_lastContext;
    int m_droppedEvents = 0;
    int m_notifyEvents = 0;
    int m_lateDecryptedEvents = 0;
    // Event id -> when it was handed over as an undecryptable timeline_event.
    QHash<QString, qint64> m_forwardedUtds;
    QSet<QString> m_verificationCards;
};

} // namespace lightning::accounts
