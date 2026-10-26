// The background accounts of live accounts (Labs; design §3.3, §5, §6,
// contract §5.2). Constructed by AppController only while Labs is active;
// with the flag off nothing here exists.
//
// It owns one BackgroundSessionHost per live background account, the
// drainer, the scheduler, the host retirement pool, the retirement latches
// and the keyring worker, and never reads the foreground client or the
// active account's state: the foreground's facts are told to it.
//
// §6 is binding throughout: an unreadable secret is "keyring locked", never
// "no account"; the store an account uses is the RECORDED one; nothing is
// deleted here, ever (removal deletes in AppController, and only after this
// pool reports the store free); a failed token write pauses and retries.
//
// A KeyringLocked account resumes on KeyringWorker::keyringReadable, which
// SettingsManager::secretBackendChanged drives: the user's "Retry keyring"
// (AppController::retryKeyring() replaces the store) is that edge. A keyring
// merely unlocked in the desktop is NOT detected, exactly as the
// foreground's own locked-keyring state is not (AppController.h). Opening
// the account in the foreground also clears it for that account.
//
// Every late callback (a keyring answer, a blocking call's GUI half) is
// checked against the account's epoch, bumped by every retirement, and
// dropped when it moved or when the account is now the foreground's.
//
// GUI thread only.
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
        SettingsManager *settings = nullptr;          // §4 and Phase 0 *For() reads only
        AccountManager *accounts = nullptr;           // markSessionRevoked only
        NotificationManager *notifications = nullptr; // processEvent, observeRoomUnreadFor only
        AccountActivityModel *activity = nullptr;
        RevocationProofs *proofs = nullptr;
        StoreOwnershipRegistry *registry = nullptr;   // nullptr = process()
        RustBridgeApi *api = nullptr;                 // nullptr = rustBridgeApi()
        SecretStore *guiSecretStore = nullptr;        // settings->secretStore(); mode choice + install scope
        std::function<std::unique_ptr<SecretStore>()> secretStoreFactory; // run ON the worker thread
        qint64 appStartMs = 0;                        // wall ms at AppController construction
        std::function<qint64()> monotonicMs;          // nullptr = QElapsedTimer
        std::function<qint64()> suspendAwareMs;       // nullptr = the platform's (SessionScheduler)
    };
    explicit SessionPool(Deps deps, QObject *parent = nullptr);
    ~SessionPool() override;   // never blocks; destroy only after shutdownAsync() or waitForQuit()

    void start();              // api->labsEnable(); lag probe; admits nothing before the gate (§6.1)

    // Foreground facts (the pool never reads m_client or active-account state).
    void setForegroundUserId(const QString &uid);              // "" while signed out or switching
    void noteForegroundInitialSyncDone();
    void noteForegroundHomeserverReachable(const QString &homeserverUrl);

    enum class PinResult : quint8 { Ok, RefusedCap, RefusedNoRecord };
    PinResult setKeepRunning(const QString &uid, bool on);     // writes setKeepRunningFor, reschedules

    // Phase 1 click path: free uid's store for the foreground. Retires the host (persist=true;
    // a Starting host is retired after login_ok/login_failed), then awaits the latch. done() runs
    // on the GUI thread, asynchronously, within kStoreCloseBudgetMs. A non-live uid -> Free at once
    // (still async).
    enum class ReleaseResult : quint8 { Free, StoreBusy, Cancelled };
    quint64 releaseForForeground(const QString &uid, std::function<void(ReleaseResult)> done);
    void cancelRelease(quint64 ticket);
    // Warm account adoption (master prompt Part 4 §4): hand uid's RUNNING
    // host's live handle to the foreground instead of retiring it. Only when
    // the handover cannot lose or race anything: the host is Running with its
    // initial sync done, not being removed or retired, its store not marked
    // busy, and no keyring write of its sign-in or live token write pending
    // (custody settled, so the keyring holds the pair the handle holds).
    // Otherwise std::nullopt and nothing changed: the caller takes the cold
    // releaseForForeground() path. On success, synchronously and with no I/O:
    // the epoch moves (every late callback of that host is stale), uid is held
    // back from restarting, the host and its book are gone, and the store's
    // ownership record moves Host -> Foreground ON THE SAME SERIAL (one store,
    // one owning handle; nothing re-opens it). The caller owns the handle.
    std::optional<LiveSessionTicket> takeForAdoption(const QString &uid);
    // A release that ANSWERED (Free or StoreBusy) whose switch never brought
    // uid to the front (refused on the spot, or the store stayed busy): uid
    // is the pool's again, so its pin can restart it. Without this, uid stayed
    // in the "never restarted until the switch lands" set for the rest of the
    // process (la1-A N1/N2). No-op while a release for uid is still pending,
    // or for the foreground account; touches nothing else (no failure marks
    // cleared, no state flash).
    void abandonRelease(const QString &uid);

    // The account switcher's question (AccountListModel::SignInProbe): did the
    // keyring ANSWER with no sign-in for uid? On the keyring worker, never the
    // GUI thread; "cannot tell" (locked, timed out, legacy items this install
    // may not read directly, an account being removed) is false. done() runs
    // on the GUI thread on a later turn, or never if the read is cancelled.
    // SENSITIVE: the read's tokens are dropped unlogged.
    void probeSignIn(const QString &uid, std::function<void(bool lost)> done);

    // Removal of a LIVE background account (DECISION 7, §9.2 order steps 3-4): forbid uid's keyring
    // writes (KeyringWorker::forbidWritesFor) and its restart, both until the account is opened in
    // the foreground again after the removal answered with no write in flight (a fresh sign-in;
    // setForegroundUserId lifts them), else until restart; server sign-out (logout or
    // oauthLogout on the HostRetirementPool, bounded 15 s), retire persist=false (deferred if
    // Starting), await the latch AND until no write of uid can still land in the keyring, within
    // the budget; otherwise storeFree=false ("not finished"). Deletes nothing.
    enum class ServerSignOut : quint8 { Ok, AlreadyInvalid, Failed, TimedOut, NotLive };
    struct RemovalResult { bool storeFree = false; ServerSignOut serverSignOut = ServerSignOut::NotLive; };
    void retireForRemoval(const QString &uid, std::function<void(RemovalResult)> done);

    bool holdsStore(const QString &uid) const;   // Starting, Running or Stopping host for uid
    // holdsStore(), or a keyring write of uid's sign-in not yet returned (a
    // parked pair being written, a live write stuck after its host retired):
    // the foreground must not read that sign-in yet, so a switch to it goes
    // through releaseForForeground() (§18 round 3, R3-1).
    bool holdsSignIn(const QString &uid) const;
    AccountActivityModel::LiveState liveState(const QString &uid) const;
    int liveHostCount() const;                    // Starting + Running
    quint64 epochFor(const QString &uid) const;   // bumped on every retire of uid

    void beginQuit();                  // aboutToQuit: retire every host persist=true now, in parallel
    bool waitForQuit(int budgetMs);    // GUI thread, quit only: pool threads, then KeyringWorker writes
    // waitForQuit's slice: how long one pass waits for retirements before it
    // runs their GUI halves and handback writes (§18 round 1, MUST 2).
    static constexpr int kQuitSliceMs = 20;
    // A runtime off: how often it checks it is drained, and after how long a
    // host still restoring is reported (it is waited for, never left).
    static constexpr int kShutdownPollMs = 250;
    static constexpr qint64 kShutdownStartingBudgetMs = 60000;
    void shutdownAsync(std::function<void()> done);   // runtime flag-off

    // ---- tests (WP1-S suites only) -------------------------------------------
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
    // The first token-write retry (30 s in production), doubling to 20x it.
    void setTokenWriteRetryMsForTest(int ms) { m_writeRetryBaseMs = ms; }

Q_SIGNALS:
    void liveHostCountChanged(int hosts);
    void hostSignInExpired(const QString &uid);   // after classification, proof, markSessionRevoked, retire
    void hostNeedsAttention(const QString &uid, int liveState); // Stopped, StoreBusy,
                                                  // EncryptionDataMissing, OpenToFinishSetup, KeyringLocked

private:
    using LiveState = AccountActivityModel::LiveState;

    // What a host's account would need to start now.
    enum class Eligibility : quint8 {
        Eligible, Skip, NoRecord, NeedsSignIn, OpenToFinishSetup, KeyringLocked, Refused, Wait
    };
    struct Book {                       // per live uid, beside its host
        QByteArray persistedDigest;     // the pair the keyring holds (last read or written)
        QList<QByteArray> pendingLiveDigests;   // live writes queued or running
        bool writePaused = false;       // a token write failed: sync stopped, send queue off
        int writeAttempts = 0;
        QTimer *writeRetry = nullptr;
        QString slot;
    };
    struct Stopping {                   // retired, latch armed
        QString storePath;
        bool persist = false;
    };
    struct Deferred {                   // a retirement asked of a Starting host
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
        QDeadlineTimer deadline;        // the store wait, from the sign-out's answer
    };

    void schedule();
    void admit();
    Eligibility eligibility(const QString &uid) const;
    void beginStart(const QString &uid);
    void createHost(const QString &uid, const QByteArray &mediaKey, bool admitEncrypted,
                    const QString &access, const QString &refresh, const QString &clientId);
    void abandonStart(const QString &uid, const QString &homeserver);
    // Another handle holds uid's store (admit or a refused claim).
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
    // ledgerGeneration: the rotation the write answers for (0: none waits).
    void writeLiveTokens(const QString &uid, const QString &access, const QString &refresh,
                         quint64 ledgerGeneration = 0);
    // sessionTokensPersisted() for the current host's handle, when a rotation
    // waits (ledgerGeneration > 0).
    void answerRotation(const QString &uid, quint64 ledgerGeneration, bool saved);
    void pauseForWriteFailure(const QString &uid);
    void retryTokenWrite(const QString &uid);
    void handleRevocation(const BackgroundSessionHost::Identity &identity,
                          const CustodySnapshot &custody);
    void finishRevocation(const BackgroundSessionHost::Identity &identity, int outcome);

    // persist: hand the final tokens back. Deferred while the host is Starting.
    void retireHost(const QString &uid, bool persist, bool cancelKeyring = false);
    void finishRetirement(const QString &uid, bool persist, quint64 storeSerial, bool storeClosed);
    void onLatchReleased(const QString &uid);
    void resizeRetirementThreads();   // live + retiring hosts (review round 2)
    void removalSignedOut(const QString &uid, ServerSignOut outcome);
    void removalRetire(const QString &uid);
    void finishRemoval(const QString &uid, bool latchFree);
    // A late callback for uid is still about the same session.
    bool stillCurrent(const QString &uid, quint64 epoch) const;

    void setState(const QString &uid, LiveState state);
    void restartReconnecting();
    void onResume();
    void writeLastLiveAt();
    void checkShutdownDrained();
    void updateIdleTimers();
    // Writes uid's parked handback (HostSignInRetirements) through the
    // keyring worker, by the same compare-and-swap. True while one is being
    // written (admission waits), false when none is parked.
    bool writeParked(const QString &uid);
    // At quit: every parked pair whose account has no write in flight.
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
    bool m_shutdownStartingLogged = false;   // checkShutdownDrained, said once
    bool m_timersRunning = false;            // updateIdleTimers
    bool m_probeStoppedForIdle = false;
    bool signInWriteBusy(const QString &uid) const;
    // A release whose store is free answers once uid's keyring writes have
    // returned, re-checked every kReleaseWritePollMs, or StoreBusy at
    // `deadlineMs` (scheduler clock).
    void settleRelease(quint64 ticket, bool free, qint64 deadlineMs);
    static constexpr int kReleaseWritePollMs = 50;
    QSet<QString> m_writingParked;            // writeParked() in flight
    std::function<void()> m_shutdownDone;
    QTimer m_shutdownPoll;
    qint64 m_shutdownStartedMs = 0;

    QString m_foreground;
    QHash<QString, BackgroundSessionHost *> m_hosts;   // Starting, Running, Reconnecting
    QHash<QString, Book> m_books;
    QHash<QString, QString> m_reading;                 // uid -> homeserver: keyring read before create
    QHash<QString, Stopping> m_stopping;
    QHash<QString, Deferred> m_deferred;
    QHash<QString, quint64> m_epochs;
    QHash<QString, LiveState> m_reported;
    QSet<QString> m_keyringLocked;
    QSet<QString> m_needsRestart;                      // Reconnecting hosts waiting for the semaphore
    QSet<QString> m_stopInFlight;                      // stop_sync running on the pool
    QSet<QString> m_releasedForForeground;             // never restarted until the switch lands
    QSet<QString> m_removing;                          // never restarted until signed in again
    QSet<QString> m_storeBusy;                         // a close never confirmed: until restart
    QHash<QString, qint64> m_claimWaitSince;           // store still held by another handle (log once)
    QHash<quint64, Release> m_releases;
    QHash<QString, Removal> m_removals;
    quint64 m_nextTicket = 1;
    int m_writeRetryBaseMs = 30000;
};

} // namespace lightning::accounts
