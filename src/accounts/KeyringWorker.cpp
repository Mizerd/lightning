#include "accounts/KeyringWorker.h"

#include "app/SettingsManager.h"
#include "storage/AppDataPaths.h"
#include "storage/InMemorySecretStore.h"
#include "storage/InsecureFallbackSecretStore.h"
#include "storage/PortableSecretStore.h"
#include "storage/SecretStore.h"
#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix/MediaStoreKey.h"
#endif

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QSemaphore>
#include <QSet>
#include <QThread>

#include <utility>

Q_LOGGING_CATEGORY(lcKeyringWorker, "lightning.accounts.keyring")

namespace lightning::accounts {

namespace {

// SettingsManager's item names (kSecretAccessToken & co in SettingsManager.cpp,
// file-local there). HostSettingsScanTest fails if they ever drift apart.
constexpr auto kSecretAccessToken = "accessToken";
constexpr auto kSecretRefreshToken = "refreshToken";
constexpr auto kSecretOAuthClientId = "oauthClientId";

// Drops this reference to a secret. Not a scrub: QString is implicitly
// shared, so fill() would detach and overwrite only a private copy while
// every other holder keeps the original bytes (review L14).
void forget(QString &value)
{
    value.clear();
}

// The worker's TokenStore: its own SecretStore, and the GUI thread's snapshot
// of the account record. Same item names and write order as
// SettingsManager::updateSessionTokens (refresh first, the access token that
// names the pair last), without its legacy-copy branch: a host only runs for
// an account whose items are this install's own and current
// (SettingsManager::keyringItemsDirectlyReadableFor). SENSITIVE.
class WorkerTokenStore final : public TokenStore
{
public:
    WorkerTokenStore(SecretStore *store, QString userId, QVariantMap record,
                     std::function<bool()> forbidden = {})
        : m_store(store), m_userId(std::move(userId)), m_record(std::move(record))
        , m_forbidden(std::move(forbidden)) {}
    QVariantMap accountRecord(const QString &userId) const override
    {
        return userId == m_userId ? m_record : QVariantMap();
    }
    QString accessTokenFor(const QString &userId) const override
    {
        if (!m_store || userId.isEmpty())
            return {};
        return m_store->readSecret(userId, QLatin1String(kSecretAccessToken));
    }
    bool updateSessionTokens(const QString &userId, const QString &access,
                             const QString &refresh) override
    {
        if (!m_store || userId.isEmpty() || access.isEmpty())
            return false;
        // Checked before EACH item: a removal that forbade this account's
        // writes while this job waited on the thread stops it here.
        if (m_forbidden && m_forbidden())
            return false;
        bool ok = m_store->storeSecret(userId, QLatin1String(kSecretRefreshToken), refresh);
        if (m_forbidden && m_forbidden())
            return false;
        ok = ok && m_store->storeSecret(userId, QLatin1String(kSecretAccessToken), access);
        return ok;
    }

private:
    SecretStore *m_store;
    QString m_userId;
    QVariantMap m_record;
    std::function<bool()> m_forbidden;
};

// GuiQueued: SettingsManager's own calls, one for one, as the foreground's
// SettingsTokenStore does.
class SettingsBackedTokenStore final : public TokenStore
{
public:
    explicit SettingsBackedTokenStore(SettingsManager *settings) : m_settings(settings) {}
    QVariantMap accountRecord(const QString &userId) const override
    {
        return m_settings->accountRecord(userId);
    }
    QString accessTokenFor(const QString &userId) const override
    {
        return m_settings->accessTokenFor(userId);
    }
    bool updateSessionTokens(const QString &userId, const QString &access,
                             const QString &refresh) override
    {
        return m_settings->updateSessionTokens(userId, access, refresh);
    }

private:
    SettingsManager *m_settings;
};

KeyringWorker::WriteResult runWrite(TokenStore *store, const KeyringWorker::WriteRequest &w)
{
    KeyringWorker::WriteResult out;
    if (w.kind == KeyringWorker::WriteKind::Live) {
        const QString owner = custody::tokenOwnerFor(*store, {w.userId}, w.deviceId);
        // The pair being written IS the session's current one (custody reads
        // final_session_tokens() live), so the rotated-past check is moot.
        out.live = custody::writeLive(store, owner, w.tokens.accessToken, w.tokens);
        out.outcome = out.live == custody::LiveWrite::Written
            ? KeyringWorker::WriteOutcome::Written
            : (out.live == custody::LiveWrite::WriteFailed
                   ? KeyringWorker::WriteOutcome::Failed
                   : KeyringWorker::WriteOutcome::Skipped);
        return out;
    }
    // The keyring may hold the pair of one of this host's own live writes that
    // were still queued at retirement (FIFO: they ran just before this). That
    // is still "what this host last wrote", so it is the pair to compare
    // against; anything else (a foreground sign-in) refuses as before.
    const QByteArray expected =
        custody::handbackExpectedDigest(store, w.userId, w.expectedDigest, w.acceptedDigests);
    const custody::HandbackEntry entry{w.userId, w.deviceId, expected, w.tokens};
    // No foreground session of this device can be running: the pool never
    // hosts the foreground account, and a store claim keeps one store to one
    // handle.
    out.handback = custody::writeHandback(store, entry, false);
    out.outcome = out.handback == custody::Handback::Written
        ? KeyringWorker::WriteOutcome::Written
        : (out.handback == custody::Handback::WriteFailed
               ? KeyringWorker::WriteOutcome::Failed
               : KeyringWorker::WriteOutcome::Skipped);
    return out;
}

} // namespace

// forbidWritesFor(): shared with the runner, read on the worker thread.
struct KeyringForbidden {
    mutable QMutex mutex;
    QSet<QString> users;
    bool contains(const QString &userId) const
    {
        QMutexLocker lock(&mutex);
        return users.contains(userId);
    }
    void insert(const QString &userId)
    {
        QMutexLocker lock(&mutex);
        users.insert(userId);
    }
    void remove(const QString &userId)
    {
        QMutexLocker lock(&mutex);
        users.remove(userId);
    }
};

// What a worker job answered, and a semaphore released once it is filled (the
// quit path waits on it with the event loop stopped).
struct KeyringJobSlot {
    QSemaphore done;
    KeyringWorker::ReadResult read;
    KeyringWorker::WriteResult write;
};

// Lives on the worker thread and owns the worker's SecretStore there.
class KeyringWorkerRunner : public QObject
{
public:
    std::function<std::unique_ptr<SecretStore>()> factory;
    QString installScope;
    // Each account's recorded keyring backend (SettingsManager::
    // keyringBackendFor()), as the GUI thread read it when the job was
    // queued. Worker thread only. The store asks for it on every use
    // (SecretStore::setRecordedBackendLookup), as the GUI store asks
    // SettingsManager, so a store with two backends (TwoBackendSecretStore,
    // 356668cb) reads and writes a background account where its record says
    // its sign-in is: never a clean miss in the other backend, which a host
    // would take for a lost sign-in. Declared before the store, which asks it,
    // so it outlives the store.
    QHash<QString, QString> recordedBackends;
    std::unique_ptr<SecretStore> store;
    std::shared_ptr<KeyringForbidden> forbidden;

    SecretStore *ensureStore()
    {
        if (!store && factory) {
            store = factory();
            if (store) {
                store->setInstallScope(installScope);
                store->setRecordedBackendLookup([this](const QString &userId) {
                    return recordedBackends.value(userId.trimmed());
                });
            }
        }
        return store.get();
    }
    void noteRecordedBackend(const QString &userId, const QString &kind)
    {
        if (kind.isEmpty())
            recordedBackends.remove(userId.trimmed());
        else
            recordedBackends.insert(userId.trimmed(), kind);
    }
    void rebuild()
    {
        store.reset();
        ensureStore();
    }

    KeyringWorker::ReadResult read(const KeyringWorker::ReadRequest &r,
                                   const QString &recordedBackend)
    {
        KeyringWorker::ReadResult out;
        noteRecordedBackend(r.userId, recordedBackend);
        SecretStore *s = ensureStore();
        if (!s || !s->isAvailable() || r.userId.isEmpty()) {
            out.locked = true;
            return out;
        }
        // Each read is checked on its own: one can fail after another
        // answered, and an empty answer from a FAILED read is "cannot tell".
        const auto readOne = [&](const char *key, QString *dest) {
            *dest = s->readSecret(r.userId, QLatin1String(key));
            return !s->lastReadFailed();
        };
        const bool ok = readOne(kSecretAccessToken, &out.accessToken)
            && readOne(kSecretRefreshToken, &out.refreshToken)
            && (!r.oauth || readOne(kSecretOAuthClientId, &out.oauthClientId));
#ifdef ENABLE_RUST_SDK_BACKEND
        if (ok && r.mediaKey && !out.accessToken.isEmpty()) {
            namespace msk = matrix::media_store_key;
            // Never a new key from here: a background start only uses one.
            msk::Resolution resolution = msk::resolve(s, r.userId, false);
            out.mediaKey = resolution.key;
            out.mediaAdmitEncrypted = msk::admitsEncryptedRooms(resolution, s->isSecure());
            resolution.key.clear();   // shared with out.mediaKey: dropped, not scrubbed
        }
#endif
        out.ok = ok;
        out.locked = !ok;
        if (!ok) {
            forget(out.accessToken);
            forget(out.refreshToken);
            forget(out.oauthClientId);
        }
        return out;
    }

    KeyringWorker::WriteResult write(const KeyringWorker::WriteRequest &w,
                                     const QVariantMap &record,
                                     const QString &recordedBackend)
    {
        noteRecordedBackend(w.userId, recordedBackend);
        if (forbidden && forbidden->contains(w.userId)) {
            KeyringWorker::WriteResult cancelled;
            cancelled.outcome = KeyringWorker::WriteOutcome::Cancelled;
            return cancelled;
        }
        SecretStore *s = ensureStore();
        if (!s) {
            KeyringWorker::WriteResult failed;
            return failed;
        }
        const std::shared_ptr<KeyringForbidden> ban = forbidden;
        const QString uid = w.userId;
        WorkerTokenStore tokens(s, w.userId, record,
                                [ban, uid] { return ban && ban->contains(uid); });
        KeyringWorker::WriteResult out = runWrite(&tokens, w);
        if (ban && ban->contains(uid))
            out.outcome = KeyringWorker::WriteOutcome::Cancelled;
        return out;
    }
};

KeyringWorker::Mode KeyringWorker::modeFor(const SecretStore *guiStore, bool haveFactory)
{
    if (!guiStore || !haveFactory)
        return Mode::GuiQueued;
    // These keep their secrets in this process (a file read once, QSettings,
    // a hash): a second instance would answer from a stale copy.
    if (dynamic_cast<const PortableSecretStore *>(guiStore)
        || dynamic_cast<const InsecureFallbackSecretStore *>(guiStore)
        || dynamic_cast<const InMemorySecretStore *>(guiStore))
        return Mode::GuiQueued;
    return Mode::WorkerThread;
}

KeyringWorker::KeyringWorker(Deps deps, QObject *parent)
    : QObject(parent)
    , m_deps(std::move(deps))
{
    m_forbidden = std::make_shared<KeyringForbidden>();
    m_mode = modeFor(m_deps.guiSecretStore, bool(m_deps.secretStoreFactory));
    m_installScope = m_deps.guiSecretStore ? m_deps.guiSecretStore->installScope() : QString();
    // A store that can block (an OS keyring) with no factory to make the
    // worker's own instance: every call would run on the GUI thread. Said
    // loudly; AppController always passes the factory (review L4).
    if (m_mode == Mode::GuiQueued && !m_deps.secretStoreFactory && m_deps.guiSecretStore
        && modeFor(m_deps.guiSecretStore, true) == Mode::WorkerThread)
        qCWarning(lcKeyringWorker) << "no keyring factory for a blocking store: background "
                                      "keyring calls will run on the GUI thread";
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, &KeyringWorker::runningTimedOut);
    if (m_deps.settings) {
        connect(m_deps.settings, &SettingsManager::secretBackendChanged, this,
                &KeyringWorker::rebuild);
    }
    qCInfo(lcKeyringWorker) << "keyring worker"
                            << "mode=" << (m_mode == Mode::WorkerThread ? "worker-thread"
                                                                        : "gui-queued");
}

KeyringWorker::~KeyringWorker()
{
    m_queues.clear();
    if (!m_thread)
        return;
    m_thread->quit();
    // A store call that never returns (a hung keyring) must not hang the
    // quit: the thread and its store are left to the process exit.
    if (m_thread->wait(500)) {
        delete m_thread;
    } else {
        qCWarning(lcKeyringWorker) << "the keyring worker is still inside a store call; "
                                      "leaving it to the process exit";
    }
    m_thread = nullptr;
    m_runner = nullptr;
}

void KeyringWorker::ensureThread()
{
    if (m_mode != Mode::WorkerThread || m_thread)
        return;
    m_thread = new QThread;
    m_thread->setObjectName(QStringLiteral("lt-keyring"));
    m_runner = new KeyringWorkerRunner;
    m_runner->factory = m_deps.secretStoreFactory;
    m_runner->installScope = m_installScope;
    m_runner->forbidden = m_forbidden;
    m_runner->moveToThread(m_thread);
    // The runner, and the store it made on that thread, die on that thread.
    connect(m_thread, &QThread::finished, m_runner, &QObject::deleteLater);
    m_thread->start();
}

void KeyringWorker::setTimeoutsForTest(int readMs, int writeMs)
{
    m_readTimeoutMs = readMs;
    m_writeTimeoutMs = writeMs;
}

quint64 KeyringWorker::read(const ReadRequest &request,
                            std::function<void(const ReadResult &)> done)
{
    Job job;
    job.id = m_nextJobId++;
    job.userId = request.userId;
    job.read = request;
    // The account's recorded keyring (GUI thread): the worker's store reads
    // it there, as the GUI's own store does.
    if (m_deps.settings)
        job.recordedBackend = m_deps.settings->keyringBackendFor(request.userId);
    job.onRead = std::move(done);
    const quint64 id = job.id;
    enqueue(std::move(job));
    return id;
}

quint64 KeyringWorker::write(const WriteRequest &request,
                             std::function<void(const WriteResult &)> done)
{
    Job job;
    job.id = m_nextJobId++;
    job.userId = request.userId;
    job.isWrite = true;
    job.write = request;
    // Snapshot on this (the GUI) thread: the worker never touches SettingsManager.
    if (m_deps.settings) {
        job.recordSnapshot = m_deps.settings->accountRecord(request.userId);
        job.recordedBackend = m_deps.settings->keyringBackendFor(request.userId);
    }
    job.onWrite = std::move(done);
    const quint64 id = job.id;
    enqueue(std::move(job));
    return id;
}

void KeyringWorker::enqueue(Job job)
{
    if (job.isWrite && m_forbidden->contains(job.userId)) {
        deliverCancelledLater(job);
        return;
    }
    if (m_wedged) {
        // A store that has not returned in its timeout is a locked one: say so
        // now rather than queue behind it.
        ReadResult read;
        read.locked = true;
        read.timedOut = true;
        WriteResult write;
        write.outcome = WriteOutcome::TimedOut;
        const QString uid = job.userId;
        // Delivered on the next turn, never inside the caller's own stack.
        ++m_cancelledDeliveries;
        QMetaObject::invokeMethod(this, [this, job, read, write] {
            --m_cancelledDeliveries;
            deliver(job, read, write);
        }, Qt::QueuedConnection);
        if (!job.isWrite)
            Q_EMIT keyringLocked(uid);
        return;
    }
    if (!m_queues.contains(job.userId))
        m_order.append(job.userId);
    m_queues[job.userId].enqueue(std::move(job));
    // Always a queued pump: a GuiQueued call never runs inside the caller's
    // stack (a drainer tick), and a worker job never starts from one either.
    QMetaObject::invokeMethod(this, &KeyringWorker::pump, Qt::QueuedConnection);
}

void KeyringWorker::pump()
{
    if (m_hasRunning || m_wedged)
        return;
    // Round-robin over accounts with work; FIFO within each, so a handback
    // write always lands before the same account's next read.
    for (int tried = 0; tried < m_order.size(); ++tried) {
        if (m_next >= m_order.size())
            m_next = 0;
        const QString uid = m_order.at(m_next);
        auto it = m_queues.find(uid);
        if (it == m_queues.end() || it->isEmpty()) {
            m_queues.remove(uid);
            m_order.removeAt(m_next);
            --tried;
            if (m_order.isEmpty())
                return;
            continue;
        }
        Job job = it->dequeue();
        if (it->isEmpty()) {
            m_queues.erase(it);
            m_order.removeAt(m_next);
        } else {
            ++m_next;
        }
        dispatch(std::move(job));
        return;
    }
}

KeyringWorker::ReadResult KeyringWorker::runReadOnGui(const Job &job) const
{
    ReadResult out;
    SettingsManager *settings = m_deps.settings;
    if (!settings || job.userId.isEmpty()) {
        out.locked = true;
        return out;
    }
    // restoreSession()'s reading, for any account: an empty answer counts as
    // "none" only while the backend can answer at all.
    out.accessToken = settings->accessTokenFor(job.userId);
    bool ok = !(out.accessToken.isEmpty() && settings->secretBackendUnavailable());
    if (ok) {
        out.refreshToken = settings->refreshTokenFor(job.userId);
        ok = !(out.refreshToken.isEmpty() && settings->secretBackendUnavailable());
    }
    if (ok && job.read.oauth) {
        out.oauthClientId = settings->oauthClientIdFor(job.userId);
        ok = !(out.oauthClientId.isEmpty() && settings->secretBackendUnavailable());
    }
#ifdef ENABLE_RUST_SDK_BACKEND
    if (ok && job.read.mediaKey && !out.accessToken.isEmpty()) {
        namespace msk = matrix::media_store_key;
        SecretStore *store = settings->secretStore();
        msk::Resolution resolution = msk::resolve(store, job.userId, false);
        out.mediaKey = resolution.key;
        out.mediaAdmitEncrypted =
            msk::admitsEncryptedRooms(resolution, store && store->isSecure());
        resolution.key.clear();   // shared with out.mediaKey: dropped, not scrubbed
    }
#endif
    out.ok = ok;
    out.locked = !ok;
    if (!ok) {
        forget(out.accessToken);
        forget(out.refreshToken);
        forget(out.oauthClientId);
    }
    return out;
}

KeyringWorker::WriteResult KeyringWorker::runWriteOnGui(const Job &job) const
{
    if (!m_deps.settings)
        return WriteResult{};
    if (m_forbidden->contains(job.userId)) {
        WriteResult cancelled;
        cancelled.outcome = WriteOutcome::Cancelled;
        return cancelled;
    }
    SettingsBackedTokenStore store(m_deps.settings);
    return runWrite(&store, job.write);
}

void KeyringWorker::dispatch(Job job)
{
    m_hasRunning = true;
    m_runningDiscarded = false;
    m_running = std::move(job);
    if (m_mode == Mode::GuiQueued) {
        // On this thread, now (pump() itself only ever runs queued).
        ReadResult read;
        WriteResult write;
        if (m_running.isWrite)
            write = runWriteOnGui(m_running);
        else
            read = runReadOnGui(m_running);
        finishRunning(m_running.id, read, write);
        return;
    }
    ensureThread();
    auto slot = std::make_shared<KeyringJobSlot>();
    m_runningSlot = slot;
    m_timeout.start(m_running.isWrite ? m_writeTimeoutMs : m_readTimeoutMs);
    QPointer<KeyringWorker> self(this);
    KeyringWorkerRunner *runner = m_runner;
    const Job copy = m_running;
    QMetaObject::invokeMethod(m_runner, [self, runner, slot, copy] {
        if (copy.isWrite)
            slot->write = runner->write(copy.write, copy.recordSnapshot, copy.recordedBackend);
        else
            slot->read = runner->read(copy.read, copy.recordedBackend);
        slot->done.release();
        QCoreApplication *app = QCoreApplication::instance();
        if (!app)
            return;
        QMetaObject::invokeMethod(app, [self, slot, id = copy.id] {
            if (self)
                self->finishRunning(id, slot->read, slot->write);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void KeyringWorker::finishRunning(quint64 jobId, const ReadResult &read,
                                  const WriteResult &write)
{
    if (!m_hasRunning || m_running.id != jobId) {
        // The answer of a call that already timed out: late, discarded. The
        // store answered, so it is no longer wedged, and nothing of that call
        // can land any more.
        if (jobId != 0 && jobId == m_strandedJobId) {
            if (m_strandedIsWrite && write.outcome == WriteOutcome::Written
                && m_mode == Mode::WorkerThread && m_deps.settings)
                m_deps.settings->noteSessionTokensWrittenElsewhere(m_strandedUserId);
            m_strandedJobId = 0;
            m_strandedUserId.clear();
            m_strandedIsWrite = false;
        }
        if (m_wedged) {
            m_wedged = false;
            qCInfo(lcKeyringWorker) << "the keyring answered again after a timeout";
            QMetaObject::invokeMethod(this, &KeyringWorker::pump, Qt::QueuedConnection);
            // What the wedge answered "locked" (failQueuedWhileWedged) may
            // try again now; without this only a backend change said so, and
            // a paused start stayed paused for the session (§18 round 1,
            // MUST 4).
            Q_EMIT keyringReadable();
        }
        return;
    }
    m_timeout.stop();
    const Job job = std::move(m_running);
    const bool discarded = m_runningDiscarded;
    m_hasRunning = false;
    m_running = Job{};
    m_runningSlot.reset();
    if (!discarded) {
        deliver(job, read, write);
    } else if (job.isWrite) {
        // Cancelled while running: the store call has returned, and its
        // callback still runs once (a retirement waits on it). What landed
        // is noted for the backend all the same.
        if (write.outcome == WriteOutcome::Written && m_mode == Mode::WorkerThread
            && m_deps.settings)
            m_deps.settings->noteSessionTokensWrittenElsewhere(job.userId);
        WriteResult cancelled = write;
        cancelled.outcome = WriteOutcome::Cancelled;
        if (job.onWrite)
            job.onWrite(cancelled);
    }
    QMetaObject::invokeMethod(this, &KeyringWorker::pump, Qt::QueuedConnection);
}

void KeyringWorker::deliverCancelledLater(const Job &job)
{
    if (!job.isWrite)
        return;   // a dropped read's callback never runs
    ++m_cancelledDeliveries;
    QMetaObject::invokeMethod(this, [this, job] {
        --m_cancelledDeliveries;
        WriteResult cancelled;
        cancelled.outcome = WriteOutcome::Cancelled;
        if (job.onWrite)
            job.onWrite(cancelled);
    }, Qt::QueuedConnection);
}

void KeyringWorker::runningTimedOut()
{
    if (!m_hasRunning)
        return;
    const Job job = std::move(m_running);
    const bool discarded = m_runningDiscarded;
    m_hasRunning = false;
    m_running = Job{};
    m_runningSlot.reset();
    // The call is still inside the store; its answer, when it comes, is
    // dropped. Until then every job fails fast as locked, and a write of it
    // is still in flight (writeInFlightFor()).
    m_wedged = true;
    m_strandedJobId = job.id;
    m_strandedUserId = job.userId;
    m_strandedIsWrite = job.isWrite;
    qCWarning(lcKeyringWorker) << "a keyring call did not answer in time"
                               << "acct=" << matrix::app_data::safeUserSlug(job.userId)
                               << "kind=" << (job.isWrite ? "write" : "read");
    ReadResult read;
    read.locked = true;
    read.timedOut = true;
    WriteResult write;
    write.outcome = WriteOutcome::TimedOut;
    if (!discarded) {
        deliver(job, read, write);
    } else if (job.isWrite && job.onWrite) {
        write.outcome = WriteOutcome::Cancelled;
        job.onWrite(write);
    }
    failQueuedWhileWedged();
}

void KeyringWorker::failQueuedWhileWedged()
{
    QHash<QString, QQueue<Job>> queues;
    queues.swap(m_queues);
    m_order.clear();
    m_next = 0;
    for (auto it = queues.begin(); it != queues.end(); ++it) {
        while (!it->isEmpty()) {
            const Job job = it->dequeue();
            ReadResult read;
            read.locked = true;
            read.timedOut = true;
            WriteResult write;
            write.outcome = WriteOutcome::TimedOut;
            deliver(job, read, write);
        }
    }
}

void KeyringWorker::deliver(const Job &job, const ReadResult &read, const WriteResult &write)
{
    if (job.isWrite) {
        // The keyring backend note SettingsManager::updateSessionTokens makes
        // itself; a write it did not make tells it here.
        if (write.outcome == WriteOutcome::Written && m_mode == Mode::WorkerThread
            && m_deps.settings)
            m_deps.settings->noteSessionTokensWrittenElsewhere(job.userId);
        if (job.onWrite)
            job.onWrite(write);
        return;
    }
    if (read.locked)
        Q_EMIT keyringLocked(job.userId);
    if (job.onRead)
        job.onRead(read);
}

void KeyringWorker::cancelFor(const QString &userId)
{
    // A dropped write still answers (Cancelled), on a later turn.
    const QQueue<Job> dropped = m_queues.take(userId);
    for (const Job &job : dropped)
        deliverCancelledLater(job);
    const qsizetype at = m_order.indexOf(userId);
    if (at >= 0) {
        m_order.removeAt(at);
        if (m_next > at)
            --m_next;
    }
    if (m_hasRunning && m_running.userId == userId)
        m_runningDiscarded = true;
}

void KeyringWorker::forbidWritesFor(const QString &userId)
{
    if (userId.isEmpty())
        return;
    m_forbidden->insert(userId);
    cancelFor(userId);
}

void KeyringWorker::allowWritesFor(const QString &userId)
{
    // Only once nothing of the forbidden session can still land: a write
    // still inside the store stays the caller's to wait for.
    if (userId.isEmpty() || writeInFlightFor(userId))
        return;
    m_forbidden->remove(userId);
}

bool KeyringWorker::writeInFlightFor(const QString &userId) const
{
    // Running, discarded or not: the store call has not returned.
    if (m_hasRunning && m_running.userId == userId && m_running.isWrite)
        return true;
    // Timed out and still inside the store.
    if (m_strandedJobId != 0 && m_strandedIsWrite && m_strandedUserId == userId)
        return true;
    const auto it = m_queues.constFind(userId);
    if (it == m_queues.cend())
        return false;
    for (const Job &job : *it) {
        if (job.isWrite)
            return true;
    }
    return false;
}

int KeyringWorker::pendingJobs() const
{
    int n = (m_hasRunning ? 1 : 0) + m_cancelledDeliveries;
    for (auto it = m_queues.cbegin(); it != m_queues.cend(); ++it)
        n += int(it->size());
    return n;
}

bool KeyringWorker::waitForWrites(int budgetMs)
{
    QDeadlineTimer deadline(budgetMs);
    // The job on the worker now, whatever it is: its answer comes first.
    if (m_hasRunning && m_mode == Mode::WorkerThread && m_runningSlot) {
        const std::shared_ptr<KeyringJobSlot> slot = m_runningSlot;
        if (!slot->done.tryAcquire(1, int(deadline.remainingTime())))
            return false;
        // Its queued finishRunning() is still on the event loop and will find
        // no running job; the answer is taken from the slot it filled.
        finishRunning(m_running.id, slot->read, slot->write);
    }
    // Then every queued write, in order; reads are dropped.
    QList<Job> writes;
    for (const QString &uid : std::as_const(m_order)) {
        for (const Job &job : std::as_const(m_queues[uid])) {
            if (job.isWrite)
                writes.append(job);
        }
    }
    m_queues.clear();
    m_order.clear();
    m_next = 0;
    // Writes this call did not reach go back, in order, never dropped: a
    // caller that pumps in slices (SessionPool::waitForQuit) runs them on its
    // next call (§18 round 1, MUST 2).
    const auto requeueFrom = [this, &writes](qsizetype first) {
        for (qsizetype k = first; k < writes.size(); ++k) {
            const Job &job = writes.at(k);
            if (!m_queues.contains(job.userId))
                m_order.append(job.userId);
            m_queues[job.userId].enqueue(job);
        }
    };
    for (qsizetype i = 0; i < writes.size(); ++i) {
        const Job &job = writes.at(i);
        if (deadline.hasExpired()) {
            requeueFrom(i);
            return false;
        }
        if (m_mode == Mode::GuiQueued) {
            deliver(job, ReadResult{}, runWriteOnGui(job));
            continue;
        }
        ensureThread();
        auto slot = std::make_shared<KeyringJobSlot>();
        KeyringWorkerRunner *runner = m_runner;
        QPointer<KeyringWorker> self(this);
        QMetaObject::invokeMethod(m_runner, [self, runner, slot, job] {
            slot->write = runner->write(job.write, job.recordSnapshot, job.recordedBackend);
            slot->done.release();
            // Also answered from the event loop, as dispatch() does, for when
            // the quit gave up on it and the loop runs again; finishRunning()
            // ignores an answer already taken.
            if (QCoreApplication *app = QCoreApplication::instance()) {
                QMetaObject::invokeMethod(app, [self, slot, id = job.id] {
                    if (self)
                        self->finishRunning(id, slot->read, slot->write);
                }, Qt::QueuedConnection);
            }
        }, Qt::QueuedConnection);
        if (!slot->done.tryAcquire(1, int(deadline.remainingTime()))) {
            // Still inside the store: it is the running job now, answered by
            // the next call (or the event loop) from its slot.
            m_hasRunning = true;
            m_runningDiscarded = false;
            m_running = job;
            m_runningSlot = slot;
            requeueFrom(i + 1);
            return false;
        }
        deliver(job, ReadResult{}, slot->write);
    }
    return true;
}

void KeyringWorker::rebuild()
{
    // The backend changed: a store that was locked may answer now.
    SecretStore *gui = m_deps.settings ? m_deps.settings->secretStore() : nullptr;
    if (gui)
        m_deps.guiSecretStore = gui;
    const Mode mode = modeFor(m_deps.guiSecretStore, bool(m_deps.secretStoreFactory));
    m_installScope = m_deps.guiSecretStore ? m_deps.guiSecretStore->installScope() : QString();
    // The worker's own store is the OLD backend's whatever the new mode is:
    // rebuilt for worker mode, dropped otherwise, so a later return to
    // worker mode never reuses it (§18 round 1, L9). On the worker thread,
    // where it was made.
    if (m_runner) {
        KeyringWorkerRunner *runner = m_runner;
        const QString scope = m_installScope;
        const bool worker = mode == Mode::WorkerThread;
        QMetaObject::invokeMethod(m_runner, [runner, scope, worker] {
            runner->installScope = scope;
            if (worker)
                runner->rebuild();
            else
                runner->store.reset();
        }, Qt::QueuedConnection);
    }
    m_mode = mode;
    qCInfo(lcKeyringWorker) << "keyring backend changed; background starts may retry"
                            << "mode=" << (m_mode == Mode::WorkerThread ? "worker-thread"
                                                                        : "gui-queued");
    Q_EMIT keyringReadable();
}

} // namespace lightning::accounts
