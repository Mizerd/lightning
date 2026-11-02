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
        SettingsManager *settings = nullptr;
        SecretStore *guiSecretStore = nullptr;
        std::function<std::unique_ptr<SecretStore>()> secretStoreFactory;
    };

    explicit KeyringWorker(Deps deps, QObject *parent = nullptr);
    ~KeyringWorker() override;

    static Mode modeFor(const SecretStore *guiStore, bool haveFactory);
    Mode mode() const { return m_mode; }

    struct ReadRequest {
        QString userId;
        bool oauth = false;
        bool mediaKey = false;
    };
    struct ReadResult {
        bool ok = false;
        bool locked = false;
        bool timedOut = false;
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
        QString deviceId;
        WriteKind kind = WriteKind::Live;
        SessionTokenPair tokens;
        QByteArray expectedDigest;
        QList<QByteArray> acceptedDigests;
    };
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
    void allowWritesFor(const QString &userId);
    // A write for uid is queued, running, or still inside the store after a
    // timeout or a cancel. False only when no token of uid can still land.
    bool writeInFlightFor(const QString &userId) const;
    int pendingJobs() const;
    bool wedged() const { return m_wedged; }

    bool waitForWrites(int budgetMs);

    void setTimeoutsForTest(int readMs, int writeMs);

Q_SIGNALS:
    void keyringLocked(const QString &userId);
    void keyringReadable();

private:
    struct Job {
        quint64 id = 0;
        QString userId;
        bool isWrite = false;
        ReadRequest read;
        WriteRequest write;
        QVariantMap recordSnapshot;
        QString recordedBackend;
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
    QThread *m_thread = nullptr;
    KeyringWorkerRunner *m_runner = nullptr;
    QHash<QString, QQueue<Job>> m_queues;
    QStringList m_order;
    int m_next = 0;
    bool m_hasRunning = false;
    Job m_running;
    bool m_runningDiscarded = false;
    quint64 m_strandedJobId = 0;
    QString m_strandedUserId;
    bool m_strandedIsWrite = false;
    int m_cancelledDeliveries = 0;
    std::shared_ptr<KeyringForbidden> m_forbidden;
    std::shared_ptr<KeyringJobSlot> m_runningSlot;
    QTimer m_timeout;
    bool m_wedged = false;
    quint64 m_nextJobId = 1;
    int m_readTimeoutMs = kReadTimeoutMs;
    int m_writeTimeoutMs = kWriteTimeoutMs;
};

}
