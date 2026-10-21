// Where background hosts' blocking FFIs run (live accounts, Labs): their own
// thread pool, sized for the live and retiring hosts (+ 1, at most 6), apart from
// the foreground client's two retirement threads, so a host that takes ten
// seconds to close never delays a foreground switch (R1-H6).
//
// Workers touch no QObject. Every result goes back to the GUI thread by a
// queued call and is dropped if this pool is gone by then.
//
// ONE HANDLE, ONE JOB AT A TIME. Jobs for different handles run in parallel;
// jobs for the same handle run in the order they were submitted, each after
// the previous one RETURNED. mx_rust_destroy_and_report frees the client, and
// stop_sync / logout / oauth_logout hold a reference to it while they wait:
// a destroy that overtook one of them would be a use-after-free (review H1).
//
// Also here, because the pool is what retires them: the process-wide count of
// uids whose HOST retirement has not handed its final tokens back yet.
// RustSdkMatrixClient::settleRetiringSignIn() consults it in Labs (a check,
// never a wait: the handback completes on the GUI thread).
#pragma once

#include "accounts/SessionTokenCustody.h"

#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class QThreadPool;

namespace lightning::accounts {

class RustBridgeApi;

// Process-wide (leaked, like retiredSignIns() in RustSdkMatrixClient.cpp):
// a retirement worker may still finish while static destructors run.
class HostSignInRetirements
{
public:
    static HostSignInRetirements &process();

    void begin(const QString &userId);
    // After the handback settled (written, refused, or nothing to hand back).
    void end(const QString &userId);
    bool contains(const QString &userId) const;
    int count(const QString &userId) const;

    // A retirement's handback the keyring refused or did not answer in time
    // (§18 round 1, MUST 1). The pair is the session's last one, and the
    // keyring still holds an older (possibly consumed) refresh token, so the
    // foreground writes it right before it reads that account's sign-in
    // (RustSdkMatrixClient::persistRetiredSessionTokens), by the same
    // compare-and-swap. One per account, the newest kept. SENSITIVE.
    struct UnsavedHandback {
        QString userId;
        QString deviceId;
        QByteArray expectedDigest;
        // The host's own live writes still queued or running at retirement:
        // the keyring holding one of them is still "what this host last
        // wrote" (custody::handbackExpectedDigest, §18 round 2, R2-1).
        QList<QByteArray> acceptedDigests;
        SessionTokenPair tokens;
    };
    void parkUnsaved(const UnsavedHandback &handback);
    // That account's parked pair (none for an empty id).
    QList<UnsavedHandback> takeUnsaved(const QString &userId);
    QStringList unsavedUserIds() const;
    bool hasUnsaved(const QString &userId) const;
    void clearForTest();

private:
    mutable QMutex m_mutex;
    QHash<QString, int> m_pending;
    QHash<QString, UnsavedHandback> m_unsaved;
};

class HostRetirementPool : public QObject
{
    Q_OBJECT
public:
    static constexpr int kMaxThreads = 6;

    // How long the destructor waits for a job still running before leaving
    // its threads to the process exit (a store close that never returns must
    // not hang the quit past its budget).
    static constexpr int kDestroyWaitMs = 500;

    explicit HostRetirementPool(RustBridgeApi *api, QObject *parent = nullptr);
    // The owner (SessionPool) destroys it only after shutdownAsync() or
    // waitForQuit() drained it. A job still running then is waited for at most
    // kDestroyWaitMs and otherwise left to the process exit, never joined.
    ~HostRetirementPool() override;

    // hosts + 1 threads, at least 1, at most kMaxThreads. `hosts` counts every
    // host that can still submit a blocking call: live AND retiring
    // (SessionPool::resizeRetirementThreads). A cap lowered below the jobs
    // already submitted queues the rest behind one thread (review round 2).
    void setLiveHostCount(int hosts);
    int maxThreadCount() const;

    struct RetireJob {
        void *handle = nullptr;
        QString slug;                       // logs only
        bool collectFinalTokens = false;    // persist=true: read final_session_tokens()
    };
    struct RetireOutcome {
        bool storeClosed = false;
        // Rust's fixed report words; never a path.
        QString shutdownReport;
        QString destroyReport;
        bool collectedFinalTokens = false;
        SessionTokenPair finalTokens;       // SENSITIVE: never log
        // The token ledger, read after the destroy, held a newer pair than
        // final_session_tokens() did: that pair is finalTokens.
        bool rotatedWhileClosing = false;
        qint64 shutdownMs = 0;
        qint64 destroyMs = 0;
    };
    // shutdown_tasks, then (persist only) final_session_tokens and a retained
    // token ledger, then destroy_and_report, then (persist only) the ledger's
    // newest pair, which wins when it differs; in that order, on a worker,
    // after every job already submitted for the same handle returned. `done` runs on this object's
    // thread. Nothing may be submitted for the handle after this.
    void retire(const RetireJob &job, std::function<void(const RetireOutcome &)> done);

    // The server sign-out dispatch: oauth_logout for an OAuth account,
    // logout otherwise (the outcome arrives as the sticky logged_out).
    void signOut(void *handle, bool oauth, std::function<void(qint64 elapsedMs)> done);

    // Any other blocking entry on `handle` (stop_sync after a lost
    // connection, a resume or a failed token write).
    void runBlocking(void *handle, std::function<void()> work, std::function<void()> done = {});

    // Quit only (GUI thread): every job finished within the budget.
    bool waitForDone(int budgetMs);
    // Quit only (GUI thread, event loop blocked): run now the GUI halves of
    // jobs that finished but whose queued delivery has not run. Each half
    // runs exactly once, here or from the event loop.
    void deliverFinishedNow();
    int activeJobs() const;

Q_SIGNALS:
    void idle();

private:
    struct Finished;
    struct Shared;
    // Runs `work` on a worker once every earlier job for `handle` returned;
    // then queues `onGui` (built by the work) for this thread.
    void submit(void *handle, std::function<std::function<void()>()> work);
    void finish(const std::shared_ptr<Finished> &job);
    void jobStarted();
    void jobFinished();

    RustBridgeApi *m_api;
    std::unique_ptr<QThreadPool> m_threads;
    int m_active = 0;   // GUI thread
    // Everything a worker touches, owned jointly with the workers so a job
    // left running past the destructor reaches nothing freed.
    std::shared_ptr<Shared> m_shared;
};

} // namespace lightning::accounts
