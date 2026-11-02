#include "accounts/HostRetirementPool.h"

#include "accounts/RustBridgeApi.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QMutexLocker>
#include <QPointer>
#include <QThreadPool>

#include <algorithm>
#include <atomic>

Q_LOGGING_CATEGORY(lcHostRetire, "lightning.accounts.retire")

namespace lightning::accounts {

HostSignInRetirements &HostSignInRetirements::process()
{
    static HostSignInRetirements *registry = new HostSignInRetirements;
    return *registry;
}

void HostSignInRetirements::begin(const QString &userId)
{
    if (userId.isEmpty())
        return;
    QMutexLocker lock(&m_mutex);
    ++m_pending[userId];
}

void HostSignInRetirements::end(const QString &userId)
{
    if (userId.isEmpty())
        return;
    QMutexLocker lock(&m_mutex);
    auto it = m_pending.find(userId);
    if (it == m_pending.end())
        return;
    if (--it.value() <= 0)
        m_pending.erase(it);
}

bool HostSignInRetirements::contains(const QString &userId) const
{
    QMutexLocker lock(&m_mutex);
    return m_pending.contains(userId);
}

int HostSignInRetirements::count(const QString &userId) const
{
    QMutexLocker lock(&m_mutex);
    return m_pending.value(userId);
}

void HostSignInRetirements::parkUnsaved(const UnsavedHandback &handback)
{
    if (handback.userId.isEmpty() || handback.tokens.accessToken.isEmpty())
        return;
    QMutexLocker lock(&m_mutex);
    m_unsaved.insert(handback.userId, handback);
}

QList<HostSignInRetirements::UnsavedHandback>
HostSignInRetirements::takeUnsaved(const QString &userId)
{
    QMutexLocker lock(&m_mutex);
    QList<UnsavedHandback> out;
    if (!userId.isEmpty() && m_unsaved.contains(userId))
        out.append(m_unsaved.take(userId));
    return out;
}

QStringList HostSignInRetirements::unsavedUserIds() const
{
    QMutexLocker lock(&m_mutex);
    return m_unsaved.keys();
}

bool HostSignInRetirements::hasUnsaved(const QString &userId) const
{
    QMutexLocker lock(&m_mutex);
    return m_unsaved.contains(userId);
}

void HostSignInRetirements::clearForTest()
{
    QMutexLocker lock(&m_mutex);
    m_pending.clear();
    m_unsaved.clear();
}

struct HostRetirementPool::Finished {
    std::atomic<bool> delivered{false};
    std::function<void()> onGui;
};

struct HostRetirementPool::Shared {
    QMutex finishedMutex;
    QList<std::shared_ptr<Finished>> finished;
    QMutex chainMutex;
    QHash<void *, QList<std::function<void()>>> chains;
};

HostRetirementPool::HostRetirementPool(RustBridgeApi *api, QObject *parent)
    : QObject(parent)
    , m_api(api)
    , m_threads(std::make_unique<QThreadPool>())
    , m_shared(std::make_shared<Shared>())
{
    m_threads->setMaxThreadCount(2);
}

HostRetirementPool::~HostRetirementPool()
{
    if (!m_threads->waitForDone(kDestroyWaitMs)) {
        qCWarning(lcHostRetire) << "a background host's blocking call is still running; "
                                   "leaving it to the process exit";
        (void)m_threads.release();
    }
}

void HostRetirementPool::setLiveHostCount(int hosts)
{
    m_threads->setMaxThreadCount(std::clamp(hosts + 1, 1, kMaxThreads));
}

int HostRetirementPool::maxThreadCount() const
{
    return m_threads->maxThreadCount();
}

void HostRetirementPool::finish(const std::shared_ptr<Finished> &job)
{
    if (job->delivered.exchange(true))
        return;
    {
        QMutexLocker lock(&m_shared->finishedMutex);
        m_shared->finished.removeOne(job);
    }
    if (job->onGui)
        job->onGui();
    jobFinished();
}

void HostRetirementPool::deliverFinishedNow()
{
    QList<std::shared_ptr<Finished>> ready;
    {
        QMutexLocker lock(&m_shared->finishedMutex);
        ready = m_shared->finished;
    }
    for (const auto &job : std::as_const(ready))
        finish(job);
}

void HostRetirementPool::jobStarted()
{
    ++m_active;
}

void HostRetirementPool::jobFinished()
{
    if (--m_active <= 0) {
        m_active = 0;
        Q_EMIT idle();
    }
}

void HostRetirementPool::submit(void *handle, std::function<std::function<void()>()> work)
{
    jobStarted();
    QPointer<HostRetirementPool> self(this);
    const std::shared_ptr<Shared> shared = m_shared;
    std::function<void()> job = [self, shared, work = std::move(work)] {
        auto finished = std::make_shared<Finished>();
        finished->onGui = work();
        {
            QMutexLocker lock(&shared->finishedMutex);
            shared->finished.append(finished);
        }
        if (QCoreApplication *app = QCoreApplication::instance()) {
            QMetaObject::invokeMethod(app, [self, finished] {
                if (self)
                    self->finish(finished);
            }, Qt::QueuedConnection);
        }
    };
    {
        QMutexLocker lock(&shared->chainMutex);
        const auto it = shared->chains.find(handle);
        if (it != shared->chains.end()) {
            it->append(std::move(job));
            return;
        }
        shared->chains.insert(handle, {});
    }
    m_threads->start([shared, handle, first = std::move(job)]() mutable {
        std::function<void()> current = std::move(first);
        while (current) {
            current();
            QMutexLocker lock(&shared->chainMutex);
            const auto it = shared->chains.find(handle);
            if (it == shared->chains.end() || it->isEmpty()) {
                if (it != shared->chains.end())
                    shared->chains.erase(it);
                current = {};
            } else {
                current = it->takeFirst();
            }
        }
    });
}

void HostRetirementPool::retire(const RetireJob &job,
                                std::function<void(const RetireOutcome &)> done)
{
    if (!job.handle || !m_api)
        return;
    RustBridgeApi *api = m_api;
    submit(job.handle, [api, job, done = std::move(done)]() -> std::function<void()> {
        RetireOutcome outcome;
        QElapsedTimer timer;
        timer.start();
        outcome.shutdownReport = api->shutdownTasks(job.handle);
        outcome.shutdownMs = timer.elapsed();
        if (job.collectFinalTokens) {
            outcome.finalTokens = custody::parseFinalSessionTokens(
                api->finalSessionTokens(job.handle));
            outcome.collectedFinalTokens = true;
        }
        void *ledger = job.collectFinalTokens ? api->tokenLedgerRetain(job.handle) : nullptr;
        timer.restart();
        outcome.destroyReport = api->destroyAndReport(job.handle);
        outcome.destroyMs = timer.elapsed();
        if (ledger) {
            const SessionTokenPair latest =
                custody::parseFinalSessionTokens(api->tokenLedgerLatest(ledger));
            api->tokenLedgerRelease(ledger);
            if (!latest.accessToken.isEmpty()
                && custody::accessTokenDigest(latest.accessToken)
                    != custody::accessTokenDigest(outcome.finalTokens.accessToken)) {
                outcome.finalTokens = latest;
                outcome.rotatedWhileClosing = true;
            }
        }
        outcome.storeClosed =
            outcome.destroyReport.startsWith(QLatin1String("store_closed=true"));
        qCInfo(lcHostRetire) << "background host retired off the GUI thread"
                             << "acct=" << job.slug
                             << outcome.shutdownReport
                             << "shutdown_ms=" << outcome.shutdownMs
                             << "destroy_ms=" << outcome.destroyMs
                             << "rotated_while_closing=" << outcome.rotatedWhileClosing
                             << outcome.destroyReport;
        if (!outcome.storeClosed)
            qCWarning(lcHostRetire) << "a background host retired without confirming its "
                                       "store closed; the store stays busy until restart"
                                    << "acct=" << job.slug;
        return [done, outcome] {
            if (done)
                done(outcome);
        };
    });
}

void HostRetirementPool::signOut(void *handle, bool oauth,
                                 std::function<void(qint64 elapsedMs)> done)
{
    if (!handle || !m_api)
        return;
    RustBridgeApi *api = m_api;
    submit(handle, [api, handle, oauth, done = std::move(done)]() -> std::function<void()> {
        QElapsedTimer timer;
        timer.start();
        // The session type decides the sign-out, as in the foreground's
        // logout(): the wrong one leaves the tokens live on the server. The
        // dispatch answer is not the outcome (logged_out carries it).
        if (oauth)
            (void)api->oauthLogout(handle);
        else
            api->logout(handle);
        const qint64 elapsed = timer.elapsed();
        return [done, elapsed] {
            if (done)
                done(elapsed);
        };
    });
}

void HostRetirementPool::runBlocking(void *handle, std::function<void()> work,
                                     std::function<void()> done)
{
    if (!handle || !work)
        return;
    submit(handle, [work = std::move(work), done = std::move(done)]() -> std::function<void()> {
        work();
        return done;
    });
}

bool HostRetirementPool::waitForDone(int budgetMs)
{
    return m_threads->waitForDone(budgetMs);
}

int HostRetirementPool::activeJobs() const
{
    return m_active;
}

}
