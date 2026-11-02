#pragma once

#include "accounts/BackgroundSessionHost.h"
#include "accounts/LiveSessionTicket.h"
#include "models/AccountActivityModel.h"

#include <QDeadlineTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>

class AccountManager;
class NotificationManager;
class SecretStore;
class SettingsManager;

namespace lightning::accounts {

class BackgroundDrainer;
class BackgroundSessionHost;
class HostRetirementPool;
class KeyringWorker;
class RetirementLatches;
class RevocationProofs;
class RustBridgeApi;
class SessionScheduler;
class StoreOwnershipRegistry;
struct CustodySnapshot;

class SessionPool : public QObject
{
    Q_OBJECT
public:
    struct Deps {
        SettingsManager *settings = nullptr;
        AccountManager *accounts = nullptr;
        NotificationManager *notifications = nullptr;
        AccountActivityModel *activity = nullptr;
        RevocationProofs *proofs = nullptr;
        StoreOwnershipRegistry *registry = nullptr;
        RustBridgeApi *api = nullptr;
        SecretStore *guiSecretStore = nullptr;
        std::function<std::unique_ptr<SecretStore>()> secretStoreFactory;
        qint64 appStartMs = 0;
        std::function<qint64()> monotonicMs;
        std::function<qint64()> suspendAwareMs;
    };
    explicit SessionPool(Deps deps, QObject *parent = nullptr);
    ~SessionPool() override;

    void start();

    // Foreground facts (the pool never reads m_client or active-account state).
    void setForegroundUserId(const QString &uid);
    void noteForegroundInitialSyncDone();
    void noteForegroundHomeserverReachable(const QString &homeserverUrl);

    enum class PinResult : quint8 { Ok, RefusedCap, RefusedNoRecord };
    PinResult setKeepRunning(const QString &uid, bool on);

    // Phase 1 click path: free uid's store for the foreground. Retires the host (persist=true;
    // a Starting host is retired after login_ok/login_failed), then awaits the latch. done() runs
    // on the GUI thread, asynchronously, within kStoreCloseBudgetMs. A non-live uid -> Free at once
    // (still async).
    enum class ReleaseResult : quint8 { Free, StoreBusy, Cancelled };
    quint64 releaseForForeground(const QString &uid, std::function<void(ReleaseResult)> done);
    void cancelRelease(quint64 ticket);
    std::optional<LiveSessionTicket> takeForAdoption(const QString &uid);
    bool canAdopt(const QString &uid) const;
    // A release that ANSWERED (Free or StoreBusy) whose switch never brought
    // uid to the front (refused on the spot, or the store stayed busy): uid
    // is the pool's again, so its pin can restart it. Without this, uid stayed
    // in the "never restarted until the switch lands" set for the rest of the
    // process (la1-A N1/N2). No-op while a release for uid is still pending,
    // or for the foreground account; touches nothing else (no failure marks
    // cleared, no state flash).
    void abandonRelease(const QString &uid);

    void probeSignIn(const QString &uid, std::function<void(bool lost)> done);

    enum class ServerSignOut : quint8 { Ok, AlreadyInvalid, Failed, TimedOut, NotLive };
    struct RemovalResult { bool storeFree = false; ServerSignOut serverSignOut = ServerSignOut::NotLive; };
    void retireForRemoval(const QString &uid, std::function<void(RemovalResult)> done);

    bool holdsStore(const QString &uid) const;
    // holdsStore(), or a keyring write of uid's sign-in not yet returned (a
    // parked pair being written, a live write stuck after its host retired):
    // the foreground must not read that sign-in yet, so a switch to it goes
    // through releaseForForeground() (§18 round 3, R3-1).
    bool holdsSignIn(const QString &uid) const;
    AccountActivityModel::LiveState liveState(const QString &uid) const;
    int liveHostCount() const;
    quint64 epochFor(const QString &uid) const;

    void beginQuit();
    bool waitForQuit(int budgetMs);
    static constexpr int kQuitSliceMs = 20;
    static constexpr int kShutdownPollMs = 250;
    static constexpr qint64 kShutdownStartingBudgetMs = 60000;
    void shutdownAsync(std::function<void()> done);

    static constexpr int kStoreCloseBudgetMs = 15000;
    static constexpr int kServerSignOutBudgetMs = 15000;
    static constexpr int kAdmitIntervalMs = 1000;
    SessionScheduler *schedulerForTest() const { return m_scheduler; }
    KeyringWorker *keyringForTest() const { return m_keyring; }
    bool idleTimersRunningForTest() const { return m_timersRunning; }
    BackgroundDrainer *drainerForTest() const { return m_drainer; }
    RetirementLatches *latchesForTest() const { return m_latches; }
    HostRetirementPool *retirementForTest() const { return m_retirement; }
    BackgroundSessionHost *hostForTest(const QString &uid) const { return m_hosts.value(uid); }
    void admitNowForTest() { admit(); }
    void setTokenWriteRetryMsForTest(int ms) { m_writeRetryBaseMs = ms; }

Q_SIGNALS:
    void liveHostCountChanged(int hosts);
    void hostSignInExpired(const QString &uid);
    void hostNeedsAttention(const QString &uid, int liveState);

private:
    using LiveState = AccountActivityModel::LiveState;

    enum class Eligibility : quint8 {
        Eligible, Skip, NoRecord, NeedsSignIn, OpenToFinishSetup, KeyringLocked, Refused, Wait
    };
    struct Book {
        QByteArray persistedDigest;
        QList<QByteArray> pendingLiveDigests;
        bool writePaused = false;
        int writeAttempts = 0;
        QTimer *writeRetry = nullptr;
        QString slot;
    };
    struct Stopping {
        QString storePath;
        bool persist = false;
    };
    struct Deferred {
        bool persist = true;
        bool signOut = false;
        bool cancelKeyring = false;
    };
    struct Release {
        QString uid;
        std::function<void(ReleaseResult)> done;
        quint64 latchTicket = 0;
    };
    struct Removal {
        std::function<void(RemovalResult)> done;
        ServerSignOut signOut = ServerSignOut::NotLive;
        QTimer *timer = nullptr;
        bool signOutDispatched = false;
        bool answered = false;
        QDeadlineTimer deadline;
    };

    const char *adoptionRefusal(const QString &uid) const;
    void schedule();
    void admit();
    Eligibility eligibility(const QString &uid) const;
    void beginStart(const QString &uid);
    void createHost(const QString &uid, const QByteArray &mediaKey, bool admitEncrypted,
                    const QString &access, const QString &refresh, const QString &clientId);
    void abandonStart(const QString &uid, const QString &homeserver);
    void waitForStore(const QString &uid);
    void markStoreBusy(const QString &uid, const QString &homeserver, const char *why);
    QByteArray profileFor(const QString &uid, const QString &deviceId, const QString &slot) const;
    QString freeSlot() const;
    void wireHost(BackgroundSessionHost *host);

    void onLoginSucceeded(BackgroundSessionHost *host);
    void onLoginFailed(BackgroundSessionHost *host, int kind);
    void onConnectionLost(BackgroundSessionHost *host);
    void onCatchUpComplete(BackgroundSessionHost *host);
    void onCustody(BackgroundSessionHost *host, const CustodySnapshot &custody);
    void writeLiveTokens(const QString &uid, const QString &access, const QString &refresh,
                         quint64 ledgerGeneration = 0);
    void answerRotation(const QString &uid, quint64 ledgerGeneration, bool saved);
    void pauseForWriteFailure(const QString &uid);
    void retryTokenWrite(const QString &uid);
    void handleRevocation(const BackgroundSessionHost::Identity &identity,
                          const CustodySnapshot &custody);
    void finishRevocation(const BackgroundSessionHost::Identity &identity, int outcome);

    void retireHost(const QString &uid, bool persist, bool cancelKeyring = false);
    void finishRetirement(const QString &uid, bool persist, quint64 storeSerial, bool storeClosed);
    void onLatchReleased(const QString &uid);
    void resizeRetirementThreads();
    void removalSignedOut(const QString &uid, ServerSignOut outcome);
    void removalRetire(const QString &uid);
    void finishRemoval(const QString &uid, bool latchFree);
    bool stillCurrent(const QString &uid, quint64 epoch) const;

    void setState(const QString &uid, LiveState state);
    void restartReconnecting();
    void onResume();
    void writeLastLiveAt();
    void checkShutdownDrained();
    void updateIdleTimers();
    bool writeParked(const QString &uid);
    void writeParkedForQuit();
    QString homeserverOf(const QString &uid) const;
    static QString normalizeHomeserver(const QString &url);

    Deps m_deps;
    RustBridgeApi *m_api = nullptr;
    StoreOwnershipRegistry *m_registry = nullptr;
    HostRetirementPool *m_retirement = nullptr;
    KeyringWorker *m_keyring = nullptr;
    RetirementLatches *m_latches = nullptr;
    SessionScheduler *m_scheduler = nullptr;
    BackgroundDrainer *m_drainer = nullptr;
    QTimer m_admitTimer;
    QTimer m_lastLiveTimer;
    QTimer m_memoryTimer;
    bool m_started = false;
    bool m_schedulePending = false;
    bool m_quitting = false;
    bool m_shuttingDown = false;
    bool m_shutdownStartingLogged = false;
    bool m_timersRunning = false;
    bool m_probeStoppedForIdle = false;
    bool signInWriteBusy(const QString &uid) const;
    void settleRelease(quint64 ticket, bool free, qint64 deadlineMs);
    static constexpr int kReleaseWritePollMs = 50;
    QSet<QString> m_writingParked;
    std::function<void()> m_shutdownDone;
    QTimer m_shutdownPoll;
    qint64 m_shutdownStartedMs = 0;

    QString m_foreground;
    QHash<QString, BackgroundSessionHost *> m_hosts;
    QHash<QString, Book> m_books;
    QHash<QString, QString> m_reading;
    QHash<QString, Stopping> m_stopping;
    QHash<QString, Deferred> m_deferred;
    QHash<QString, quint64> m_epochs;
    QHash<QString, LiveState> m_reported;
    QSet<QString> m_keyringLocked;
    QSet<QString> m_needsRestart;
    QSet<QString> m_stopInFlight;
    QSet<QString> m_releasedForForeground;
    QSet<QString> m_removing;
    QSet<QString> m_storeBusy;
    QHash<QString, qint64> m_claimWaitSince;
    QHash<quint64, Release> m_releases;
    QHash<QString, Removal> m_removals;
    quint64 m_nextTicket = 1;
    int m_writeRetryBaseMs = 30000;
};

}
