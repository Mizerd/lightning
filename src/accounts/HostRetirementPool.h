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

class HostSignInRetirements
{
public:
    static HostSignInRetirements &process();

    void begin(const QString &userId);
    void end(const QString &userId);
    bool contains(const QString &userId) const;
    int count(const QString &userId) const;

    struct UnsavedHandback {
        QString userId;
        QString deviceId;
        QByteArray expectedDigest;
        QList<QByteArray> acceptedDigests;
        SessionTokenPair tokens;
    };
    void parkUnsaved(const UnsavedHandback &handback);
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

    static constexpr int kDestroyWaitMs = 500;

    explicit HostRetirementPool(RustBridgeApi *api, QObject *parent = nullptr);
    ~HostRetirementPool() override;

    void setLiveHostCount(int hosts);
    int maxThreadCount() const;

    struct RetireJob {
        void *handle = nullptr;
        QString slug;
        bool collectFinalTokens = false;
    };
    struct RetireOutcome {
        bool storeClosed = false;
        QString shutdownReport;
        QString destroyReport;
        bool collectedFinalTokens = false;
        SessionTokenPair finalTokens;
        bool rotatedWhileClosing = false;
        qint64 shutdownMs = 0;
        qint64 destroyMs = 0;
    };
    void retire(const RetireJob &job, std::function<void(const RetireOutcome &)> done);

    void signOut(void *handle, bool oauth, std::function<void(qint64 elapsedMs)> done);

    void runBlocking(void *handle, std::function<void()> work, std::function<void()> done = {});

    bool waitForDone(int budgetMs);
    void deliverFinishedNow();
    int activeJobs() const;

Q_SIGNALS:
    void idle();

private:
    struct Finished;
    struct Shared;
    void submit(void *handle, std::function<std::function<void()>()> work);
    void finish(const std::shared_ptr<Finished> &job);
    void jobStarted();
    void jobFinished();

    RustBridgeApi *m_api;
    std::unique_ptr<QThreadPool> m_threads;
    int m_active = 0;
    // Everything a worker touches, owned jointly with the workers so a job
    // left running past the destructor reaches nothing freed.
    std::shared_ptr<Shared> m_shared;
};

}
