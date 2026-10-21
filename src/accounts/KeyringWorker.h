// Every keyring call a background host makes (live accounts, Labs): reading
// its sign-in and media key before a start, writing a token rotation, and the
// handback at retirement. One thread, a per-account FIFO, a 3 s read and a
// 10 s write timeout; never the GUI thread for a store that can block
// (libsecret, Credential Manager, Keychain are synchronous). The foreground's
// own GUI-thread keyring calls are today's and do not come here.
//
// Two modes, chosen from the GUI's SecretStore:
//  * WorkerThread: the worker owns its OWN SecretStore instance, made by the
//    factory ON the worker thread, with the GUI store's install scope. Used
//    for every store that keeps nothing in process (the OS keyrings).
//  * GuiQueued: the portable store, the plaintext fallback and the in-memory
//    store keep their secrets in this process, so a second instance would
//    read stale values. Calls go through SettingsManager on the GUI thread,
//    by a queued call: never inside a drainer tick.
//
// §6, binding: a read that failed or timed out is "cannot tell" (locked),
// never "no account"; nothing is deleted here, ever; a failed write keeps the
// pair in Rust (final_session_tokens() cannot lose it) and the caller pauses.
//
// Every WRITE's callback runs exactly once, cancelled or not (a retirement
// waits on its handback to close the store's record and free its latch), and
// a write the store is still inside is never forgotten: writeInFlightFor()
// stays true until the store call returned, even after a timeout or a cancel,
// so a removal never reports the store free while a token can still land.
//
// SENSITIVE: requests and results carry tokens and a media key. Nothing here
// logs a value, a digest, or a path; slugs and outcomes only.
#pragma once

#include "accounts/SessionTokenCustody.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include <functional>
#include <memory>

class QThread;
class SecretStore;
class SettingsManager;

namespace lightning::accounts {

class KeyringWorkerRunner;
struct KeyringJobSlot;
struct KeyringForbidden;

class KeyringWorker : public QObject
{
    Q_OBJECT
public:
    enum class Mode : quint8 { GuiQueued, WorkerThread };
    static constexpr int kReadTimeoutMs = 3000;
    static constexpr int kWriteTimeoutMs = 10000;

    struct Deps {
        SettingsManager *settings = nullptr;    // GuiQueued calls, record snapshots, the
                                                // written-elsewhere note; GUI thread only
        SecretStore *guiSecretStore = nullptr;  // the mode and the install scope
        std::function<std::unique_ptr<SecretStore>()> secretStoreFactory;   // worker thread
    };

    explicit KeyringWorker(Deps deps, QObject *parent = nullptr);
    ~KeyringWorker() override;   // never blocks past a short join; a wedged call is left to exit

    // The process-local cached stores (portable, plaintext fallback, in-memory)
    // and a missing factory give GuiQueued; everything else WorkerThread.
    static Mode modeFor(const SecretStore *guiStore, bool haveFactory);
    Mode mode() const { return m_mode; }

    struct ReadRequest {
        QString userId;
        bool oauth = false;        // also the OAuth client id
        bool mediaKey = false;     // also the media store key (Rust builds; never created here)
    };
    struct ReadResult {
        bool ok = false;           // every read answered
        bool locked = false;       // a read failed, timed out, or the worker is wedged
        bool timedOut = false;
        // SENSITIVE.
        QString accessToken;
        QString refreshToken;
        QString oauthClientId;
        QByteArray mediaKey;
        bool mediaAdmitEncrypted = false;
    };
    quint64 read(const ReadRequest &request, std::function<void(const ReadResult &)> done);

    enum class WriteKind : quint8 { Live, Handback };
    struct WriteRequest {
        QString userId;
        QString deviceId;            // the host's device; the record must name it
        WriteKind kind = WriteKind::Live;
        SessionTokenPair tokens;     // SENSITIVE
        QByteArray expectedDigest;   // Handback: CAS against the persisted pair
        // Handback: also accepted as "what this host last wrote" when the
        // keyring holds one of them when the handback runs: the digests of
        // this host's own live writes still queued at retirement (review L8).
        QList<QByteArray> acceptedDigests;
    };
    // Cancelled: cancelFor() or forbidWritesFor() dropped it; the store may or
    // may not have been written if it was already running (writeInFlightFor()
    // says when that call returned).
    enum class WriteOutcome : quint8 { Written, Failed, TimedOut, Skipped, Cancelled };
    struct WriteResult {
        WriteOutcome outcome = WriteOutcome::Failed;
        custody::LiveWrite live = custody::LiveWrite::WriteFailed;
        custody::Handback handback = custody::Handback::WriteFailed;
    };
    quint64 write(const WriteRequest &request, std::function<void(const WriteResult &)> done);

    // Drops uid's queued jobs and discards the running one's result. A
    // dropped READ's callback never runs; a dropped WRITE's runs once, on a
    // later turn (never inside this call), with WriteOutcome::Cancelled.
    void cancelFor(const QString &userId);
    // cancelFor(), and from now on every write for uid is Cancelled before it
    // touches the store, including one already handed to the worker thread
    // (its runner checks before each item). A removed account's tokens are
    // never written back (§6), until allowWritesFor() or the process ends.
    void forbidWritesFor(const QString &userId);
    // Lifts forbidWritesFor() for an account signed in afresh. Refused (no
    // change) while writeInFlightFor(uid): the old session's write must not
    // be the one that lands.
    void allowWritesFor(const QString &userId);
    // A write for uid is queued, running, or still inside the store after a
    // timeout or a cancel. False only when no token of uid can still land.
    bool writeInFlightFor(const QString &userId) const;
    int pendingJobs() const;
    bool wedged() const { return m_wedged; }

    // Quit only (GUI thread): runs the queued WRITES now, in order, within
    // the budget. Reads are dropped. True when every write finished.
    bool waitForWrites(int budgetMs);

    void setTimeoutsForTest(int readMs, int writeMs);

Q_SIGNALS:
    // A read for this account could not be answered (failed or timed out).
    void keyringLocked(const QString &userId);
    // The backend changed (SettingsManager::secretBackendChanged) and the
    // worker's store was rebuilt: paused starts may try again.
    void keyringReadable();

private:
    struct Job {
        quint64 id = 0;
        QString userId;
        bool isWrite = false;
        ReadRequest read;
        WriteRequest write;
        QVariantMap recordSnapshot;   // GUI-thread snapshot for the custody checks
        QString recordedBackend;      // GUI-thread keyringBackendFor(userId), "" unknown
        std::function<void(const ReadResult &)> onRead;
        std::function<void(const WriteResult &)> onWrite;
    };

    void enqueue(Job job);
    void pump();
    void dispatch(Job job);
    void finishRunning(quint64 jobId, const ReadResult &read, const WriteResult &write);
    void deliverCancelledLater(const Job &job);
    void runningTimedOut();
    void failQueuedWhileWedged();
    void deliver(const Job &job, const ReadResult &read, const WriteResult &write);
    void rebuild();
    void ensureThread();
    ReadResult runReadOnGui(const Job &job) const;
    WriteResult runWriteOnGui(const Job &job) const;

    Deps m_deps;
    Mode m_mode = Mode::GuiQueued;
    QString m_installScope;
    QThread *m_thread = nullptr;             // owned; leaked only when wedged at destruction
    KeyringWorkerRunner *m_runner = nullptr; // lives on m_thread
    QHash<QString, QQueue<Job>> m_queues;
    QStringList m_order;                     // round-robin over accounts with work
    int m_next = 0;
    bool m_hasRunning = false;
    Job m_running;
    bool m_runningDiscarded = false;
    // A store call that timed out and has not returned yet (its answer will be
    // discarded): whose, and whether it writes. One at most: the worker runs
    // one call at a time and nothing new starts while it is wedged.
    quint64 m_strandedJobId = 0;
    QString m_strandedUserId;
    bool m_strandedIsWrite = false;
    int m_cancelledDeliveries = 0;           // Cancelled callbacks not yet run
    // forbidWritesFor(): read by the runner on the worker thread.
    std::shared_ptr<KeyringForbidden> m_forbidden;
    std::shared_ptr<KeyringJobSlot> m_runningSlot;   // worker mode: the running job's answer
    QTimer m_timeout;
    bool m_wedged = false;
    quint64 m_nextJobId = 1;
    int m_readTimeoutMs = kReadTimeoutMs;
    int m_writeTimeoutMs = kWriteTimeoutMs;
};

} // namespace lightning::accounts
