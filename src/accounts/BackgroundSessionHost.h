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
    struct Identity {
        QString userId;
        QString deviceId;
        QString homeserver;
        QString storePath;
        QString slug;
        QString label;
        bool oauth = false;
    };
    enum class Phase : quint8 { Starting, Running, Reconnecting, Stopping };
    enum class FailureKind : quint8 {
        Transient,
        StoreInUse,
        EncryptionDataMissing
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

    bool drainOnce();
    void drainTokensOnly();

    void beginCatchUp();
    bool catchingUp() const { return m_catchUp.active(); }

    void setQuiet(bool quiet) { m_quiet = quiet; }
    bool quiet() const { return m_quiet; }

    void refreshModes(const QString &roomId = QString());

    NotificationManager::Context lastContextForTest() const { return m_lastContext; }
    int droppedEventsForTest() const { return m_droppedEvents; }
    int notifyEventsForTest() const { return m_notifyEvents; }
    int lateDecryptedEventsForTest() const { return m_lateDecryptedEvents; }
    QSet<QString> unreadRoomsForTest() const { return m_unreadRooms; }
    QStringList ignoredUsersForTest() const { return QStringList(m_ignored.begin(), m_ignored.end()); }

Q_SIGNALS:
    void loginSucceeded();
    void loginFailed(lightning::accounts::BackgroundSessionHost::FailureKind kind);
    void connectionLost();
    void rateLimited(qint64 retryAfterMs);
    void catchUpComplete();
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
    QHash<QString, QJsonObject> m_sticky;
    bool m_initialSyncDone = false;
    QString m_roomListState;
    QSet<QString> m_ignored;
    CatchUpTracker m_catchUp;

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
    QHash<QString, qint64> m_forwardedUtds;
    QSet<QString> m_verificationCards;
};

}
