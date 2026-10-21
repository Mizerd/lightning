// A scripted RustBridgeApi for the live-accounts tests (contract §5.1).
//
// Every entry counts its calls by name (calls("createBackground"),
// totalCalls()), so the flag-off gate can assert that nothing reached the
// table. Handles are FakeHandle objects the fake owns; a test scripts what
// each one answers: queued events, the bg_take payload, the sticky session
// state, custody, the destroy report. Blocking entries honour an injectable
// retirement delay, and a "Starting" hold keeps a restore from reporting
// login_ok until the test releases it.
//
// Thread-safe: HostRetirementPool workers call the blocking entries while the
// test thread scripts and inspects. The delay sleeps outside the lock.
//
// The real destroy FREES the client, which this fake cannot do; it counts
// what would have been a use-after-free instead (review H1): a
// destroyAndReport() that began while another blocking call on the same
// handle had not returned (destroyOverlaps()), and any entry reached on a
// handle already destroyed (callsAfterDestroy()).
#pragma once

#include "accounts/RustBridgeApi.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QPair>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <memory>
#include <vector>

namespace lightning::accounts::testing {

struct FakeHandle {
    quint64 serial = 0;
    QString storePath;
    QJsonObject profile;
    QQueue<QString> events;
    QQueue<QString> commands;
    BgStatus status;
    QByteArray take;                       // next bg_take answer
    QJsonObject sticky;                    // session state "state" object
    QByteArray custody;                    // next take_custody answer (JSON)
    QString finalTokens;                   // final_session_tokens answer (JSON)
    QString ledgerLatest;                  // token_ledger_latest answer (JSON), read after destroy
    // sessionTokensPersisted() answers, in order: (generation, saved).
    QList<QPair<quint64, bool>> persistedAnswers;
    int ledgerRetains = 0;                 // retained and not yet released
    QStringList log;                       // entry names, in call order
    QStringList localRules;
    bool restored = false;
    bool oauth = false;
    QString restoredAccess;                // what restore() was given (tests only)
    QString restoredRefresh;
    bool syncing = false;
    bool sendQueueEnabled = true;
    bool shutdown = false;
    bool destroyed = false;
    bool holdStarting = false;
    int blockingInFlight = 0;              // blocking entries running now
    QByteArray mediaKey;
    bool mediaAdmitEncrypted = false;
};

class FakeRustBridge final : public RustBridgeApi
{
public:
    // ---- scripting -----------------------------------------------------------
    void setAvailable(bool on) { QMutexLocker l(&m_mutex); m_available = on; }
    void setCreateFails(bool fail) { QMutexLocker l(&m_mutex); m_createFails = fail; }
    // Every new handle starts held: restore() dispatches and login_ok waits for
    // releaseStarting().
    void setHoldStarting(bool hold) { QMutexLocker l(&m_mutex); m_holdStarting = hold; }
    void setAutoLoginOk(bool on) { QMutexLocker l(&m_mutex); m_autoLoginOk = on; }
    // Sleeps inside shutdownTasks() and destroyAndReport() (each half).
    void setRetireDelayMs(int ms) { QMutexLocker l(&m_mutex); m_retireDelayMs = ms; }
    void setLogoutDelayMs(int ms) { QMutexLocker l(&m_mutex); m_logoutDelayMs = ms; }
    // Sleeps inside stopSync().
    void setStopSyncDelayMs(int ms) { QMutexLocker l(&m_mutex); m_stopSyncDelayMs = ms; }
    // What destroyAndReport() answers; "store_closed=true ..." by default.
    void setDestroyReport(const QString &r) { QMutexLocker l(&m_mutex); m_destroyReport = r; }
    // logged_out "result" a server sign-out reports ("ok", "already_invalid",
    // "failed"); empty: no logged_out at all (a hang).
    void setLogoutResult(const QString &r) { QMutexLocker l(&m_mutex); m_logoutResult = r; }
    // storeAnyHeld() verdict for a path (default 0, free).
    void setAnyHeld(const QString &storePath, int verdict)
    {
        QMutexLocker l(&m_mutex);
        m_anyHeld.insert(storePath, verdict);
    }

    FakeHandle *handleFor(const QString &storePath) const
    {
        QMutexLocker l(&m_mutex);
        for (auto it = m_handles.crbegin(); it != m_handles.crend(); ++it) {
            if ((*it)->storePath == storePath)
                return it->get();
        }
        return nullptr;
    }
    FakeHandle *lastHandle() const
    {
        QMutexLocker l(&m_mutex);
        return m_handles.empty() ? nullptr : m_handles.back().get();
    }
    QList<FakeHandle *> handles() const
    {
        QMutexLocker l(&m_mutex);
        QList<FakeHandle *> out;
        for (const auto &h : m_handles)
            out.append(h.get());
        return out;
    }

    void releaseStarting(FakeHandle *h)
    {
        QMutexLocker l(&m_mutex);
        h->holdStarting = false;
        if (h->restored)
            markLoginOkLocked(h);
    }
    void pushEvent(FakeHandle *h, const QJsonObject &event)
    {
        QMutexLocker l(&m_mutex);
        h->events.enqueue(QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)));
        h->status.queuedEvents = quint32(h->events.size());
    }
    // counts tuples [room, unread, highlight, flags]; marks dirty.
    void setCounts(FakeHandle *h, const QJsonArray &counts, int invites = 0, bool baseline = false)
    {
        QMutexLocker l(&m_mutex);
        QJsonObject take{{QStringLiteral("v"), 1},
                         {QStringLiteral("seq"), double(++m_takeSeq)},
                         {QStringLiteral("baseline"), baseline},
                         {QStringLiteral("rescan"), false},
                         {QStringLiteral("counts"), counts},
                         {QStringLiteral("rows"), QJsonArray{}},
                         {QStringLiteral("invites"), invites}};
        h->take = QJsonDocument(take).toJson(QJsonDocument::Compact);
        h->status.dirty = true;
        ++h->status.notableUpdatesTotal;
    }
    void setSticky(FakeHandle *h, const QString &type, const QJsonObject &payload)
    {
        QMutexLocker l(&m_mutex);
        h->sticky.insert(type, payload);
        ++h->status.sessionStateVersion;
    }
    // custody JSON (§3.4); bumps token_generation (and sets revoked when the
    // JSON says so).
    void setCustody(FakeHandle *h, const QJsonObject &custody)
    {
        QMutexLocker l(&m_mutex);
        h->custody = QJsonDocument(custody).toJson(QJsonDocument::Compact);
        ++h->status.tokenGeneration;
        h->status.revoked = custody.value(QStringLiteral("revoked")).toBool(false);
    }
    // What token_ledger_latest() answers once the handle is destroyed: the
    // pair the handle's client last rotated to (a rotation while closing).
    void setLedgerLatest(FakeHandle *h, const QString &access, const QString &refresh)
    {
        QMutexLocker l(&m_mutex);
        h->ledgerLatest = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("access_token"), access},
                                      {QStringLiteral("refresh_token"), refresh}})
                .toJson(QJsonDocument::Compact));
    }
    QList<QPair<quint64, bool>> persistedAnswers(FakeHandle *h) const
    {
        QMutexLocker l(&m_mutex);
        return h->persistedAnswers;
    }
    int ledgerRetains(FakeHandle *h) const { QMutexLocker l(&m_mutex); return h->ledgerRetains; }
    void setFinalTokens(FakeHandle *h, const QString &access, const QString &refresh)
    {
        QMutexLocker l(&m_mutex);
        h->finalTokens = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("access_token"), access},
                                      {QStringLiteral("refresh_token"), refresh}})
                .toJson(QJsonDocument::Compact));
    }

    // ---- inspection -----------------------------------------------------------
    int calls(const QString &name) const { QMutexLocker l(&m_mutex); return m_calls.value(name); }
    int totalCalls() const
    {
        QMutexLocker l(&m_mutex);
        int n = 0;
        for (auto it = m_calls.cbegin(); it != m_calls.cend(); ++it)
            n += it.value();
        return n;
    }
    QStringList log(FakeHandle *h) const { QMutexLocker l(&m_mutex); return h->log; }
    // Threads the blocking entries ran on (never the GUI thread, tests assert).
    QList<QThread *> blockingThreads() const { QMutexLocker l(&m_mutex); return m_blockingThreads; }
    // The most blocking entries ever running at once, over every handle.
    int maxConcurrentBlocking() const { QMutexLocker l(&m_mutex); return m_maxConcurrent; }
    int destroyOverlaps() const { QMutexLocker l(&m_mutex); return m_destroyOverlaps; }
    int callsAfterDestroy() const { QMutexLocker l(&m_mutex); return m_callsAfterDestroy; }

    // ---- RustBridgeApi ----------------------------------------------------------
    bool available() const override { QMutexLocker l(&m_mutex); return m_available; }
    void labsEnable() override { count(QStringLiteral("labsEnable")); }
    void *createBackground(const QString &storePath, const QByteArray &profileJson) override
    {
        count(QStringLiteral("createBackground"));
        QMutexLocker l(&m_mutex);
        if (m_createFails)
            return nullptr;
        auto h = std::make_unique<FakeHandle>();
        h->serial = ++m_nextSerial;
        h->storePath = storePath;
        h->profile = QJsonDocument::fromJson(profileJson).object();
        h->holdStarting = m_holdStarting;
        h->log << QStringLiteral("createBackground");
        h->status.sessionStateVersion = 0;
        FakeHandle *raw = h.get();
        m_handles.push_back(std::move(h));
        return raw;
    }
    QString setMediaStoreKey(void *h, const QByteArray &key, bool admitEncrypted) override
    {
        FakeHandle *f = note(h, QStringLiteral("setMediaStoreKey"));
        QMutexLocker l(&m_mutex);
        f->mediaKey = key;
        f->mediaAdmitEncrypted = admitEncrypted;
        return {};
    }
    QString restore(void *h, const QString &, const QString &, const QString &,
                    const QString &access, const QString &refresh) override
    {
        FakeHandle *f = note(h, QStringLiteral("restore"));
        QMutexLocker l(&m_mutex);
        f->restored = true;
        f->restoredAccess = access;
        f->restoredRefresh = refresh;
        if (!f->holdStarting && m_autoLoginOk)
            markLoginOkLocked(f);
        return {};
    }
    QString oauthRestore(void *h, const QString &, const QString &, const QString &,
                         const QString &, const QString &access,
                         const QString &refresh) override
    {
        FakeHandle *f = note(h, QStringLiteral("oauthRestore"));
        QMutexLocker l(&m_mutex);
        f->restored = true;
        f->oauth = true;
        f->restoredAccess = access;
        f->restoredRefresh = refresh;
        if (!f->holdStarting && m_autoLoginOk)
            markLoginOkLocked(f);
        return {};
    }
    void startSync(void *h) override
    {
        FakeHandle *f = note(h, QStringLiteral("startSync"));
        QMutexLocker l(&m_mutex);
        f->syncing = true;
    }
    bool stopSync(void *h) override
    {
        FakeHandle *f = noteBlocking(h, QStringLiteral("stopSync"));
        int delay = 0;
        {
            QMutexLocker l(&m_mutex);
            delay = m_stopSyncDelayMs;
        }
        if (delay > 0)
            QThread::msleep(ulong(delay));
        QMutexLocker l(&m_mutex);
        const bool was = f->syncing;
        f->syncing = false;
        endBlockingLocked(f);
        return was;
    }
    QString pollEvent(void *h) override
    {
        FakeHandle *f = note(h, QStringLiteral("pollEvent"), /*log=*/false);
        QMutexLocker l(&m_mutex);
        if (f->events.isEmpty())
            return {};
        const QString out = f->events.dequeue();
        f->status.queuedEvents = quint32(f->events.size());
        return out;
    }
    QString pollCommandEvent(void *h) override
    {
        FakeHandle *f = note(h, QStringLiteral("pollCommandEvent"), /*log=*/false);
        QMutexLocker l(&m_mutex);
        return f->commands.isEmpty() ? QString() : f->commands.dequeue();
    }
    bool bgPoll(void *h, BgStatus *out) override
    {
        FakeHandle *f = note(h, QStringLiteral("bgPoll"), /*log=*/false);
        QMutexLocker l(&m_mutex);
        if (!out || f->destroyed)
            return false;
        *out = f->status;
        return true;
    }
    QByteArray bgTake(void *h, quint32) override
    {
        FakeHandle *f = note(h, QStringLiteral("bgTake"), /*log=*/false);
        QMutexLocker l(&m_mutex);
        f->status.dirty = false;
        return f->take;
    }
    QByteArray bgTakeSessionState(void *h) override
    {
        FakeHandle *f = note(h, QStringLiteral("bgTakeSessionState"), /*log=*/false);
        QMutexLocker l(&m_mutex);
        return QJsonDocument(QJsonObject{{QStringLiteral("v"), 1},
                                         {QStringLiteral("version"),
                                          double(f->status.sessionStateVersion)},
                                         {QStringLiteral("state"), f->sticky}})
            .toJson(QJsonDocument::Compact);
    }
    QString bgSetLocalRules(void *h, const QStringList &rooms) override
    {
        FakeHandle *f = note(h, QStringLiteral("bgSetLocalRules"));
        QMutexLocker l(&m_mutex);
        f->localRules = rooms;
        return {};
    }
    QString bgSetSendQueueEnabled(void *h, bool enabled) override
    {
        FakeHandle *f = note(h, QStringLiteral("bgSetSendQueueEnabled"));
        QMutexLocker l(&m_mutex);
        f->sendQueueEnabled = enabled;
        return {};
    }
    CustodySnapshot takeCustody(void *h) override
    {
        FakeHandle *f = note(h, QStringLiteral("takeCustody"));
        QMutexLocker l(&m_mutex);
        return parseCustodyJson(f->custody);
    }
    void sessionTokensPersisted(void *h, quint64 generation, bool saved) override
    {
        FakeHandle *f = note(h, QStringLiteral("sessionTokensPersisted"));
        QMutexLocker l(&m_mutex);
        f->persistedAnswers.append(qMakePair(generation, saved));
    }
    void logout(void *h) override { serverSignOut(h, QStringLiteral("logout")); }
    QString oauthLogout(void *h) override
    {
        serverSignOut(h, QStringLiteral("oauthLogout"));
        return {};
    }
    QString shutdownTasks(void *h) override
    {
        FakeHandle *f = noteBlocking(h, QStringLiteral("shutdownTasks"));
        sleepRetire();
        QMutexLocker l(&m_mutex);
        f->shutdown = true;
        f->syncing = false;
        endBlockingLocked(f);
        return QStringLiteral("shutdown=ok");
    }
    QString finalSessionTokens(void *h) override
    {
        FakeHandle *f = noteBlocking(h, QStringLiteral("finalSessionTokens"));
        QMutexLocker l(&m_mutex);
        endBlockingLocked(f);
        return f->finalTokens;
    }
    QString destroyAndReport(void *h) override
    {
        FakeHandle *f = noteBlocking(h, QStringLiteral("destroyAndReport"));
        {
            // Anything else still inside this handle would now touch a freed
            // client.
            QMutexLocker l(&m_mutex);
            if (f->blockingInFlight > 1)
                ++m_destroyOverlaps;
        }
        sleepRetire();
        QMutexLocker l(&m_mutex);
        f->destroyed = true;
        endBlockingLocked(f);
        return m_destroyReport;
    }
    // The ledger outlives the handle (Rust's is an Arc): reading it after the
    // destroy is legal, so these are deliberately not counted as calls after
    // destroy. The "ledger" is the FakeHandle itself, which the fake owns.
    void *tokenLedgerRetain(void *h) override
    {
        FakeHandle *f = note(h, QStringLiteral("tokenLedgerRetain"));
        QMutexLocker l(&m_mutex);
        ++f->ledgerRetains;
        return f;
    }
    QString tokenLedgerLatest(void *ledger) override
    {
        count(QStringLiteral("tokenLedgerLatest"));
        if (!ledger)
            return {};
        QMutexLocker l(&m_mutex);
        auto *f = static_cast<FakeHandle *>(ledger);
        f->log << QStringLiteral("tokenLedgerLatest");
        return f->ledgerLatest;
    }
    void tokenLedgerRelease(void *ledger) override
    {
        count(QStringLiteral("tokenLedgerRelease"));
        if (!ledger)
            return;
        QMutexLocker l(&m_mutex);
        --static_cast<FakeHandle *>(ledger)->ledgerRetains;
    }
    int storeAnyHeld(const QString &storePath) override
    {
        count(QStringLiteral("storeAnyHeld"));
        QMutexLocker l(&m_mutex);
        // A handle not yet destroyed on that path holds it, as Rust's
        // reservation and live probe do; a scripted verdict overrides.
        if (m_anyHeld.contains(storePath))
            return m_anyHeld.value(storePath);
        for (const auto &h : m_handles) {
            if (h->storePath == storePath && !h->destroyed)
                return 1;
        }
        return 0;
    }
    quint64 handleSerial(void *h) override
    {
        count(QStringLiteral("handleSerial"));
        return h ? static_cast<FakeHandle *>(h)->serial : 0;
    }

private:
    void count(const QString &name)
    {
        QMutexLocker l(&m_mutex);
        ++m_calls[name];
    }
    FakeHandle *note(void *h, const QString &name, bool log = true)
    {
        QMutexLocker l(&m_mutex);
        ++m_calls[name];
        auto *f = static_cast<FakeHandle *>(h);
        if (f->destroyed)
            ++m_callsAfterDestroy;
        if (log)
            f->log << name;
        return f;
    }
    // Every blocking entry ends with endBlockingLocked() on the same handle.
    FakeHandle *noteBlocking(void *h, const QString &name)
    {
        FakeHandle *f = note(h, name);
        QMutexLocker l(&m_mutex);
        m_blockingThreads.append(QThread::currentThread());
        ++f->blockingInFlight;
        ++m_concurrent;
        m_maxConcurrent = std::max(m_maxConcurrent, m_concurrent);
        return f;
    }
    void endBlockingLocked(FakeHandle *f)
    {
        --f->blockingInFlight;
        --m_concurrent;
    }
    void sleepRetire()
    {
        int ms = 0;
        {
            QMutexLocker l(&m_mutex);
            ms = m_retireDelayMs;
        }
        if (ms > 0)
            QThread::msleep(ulong(ms));
    }
    void serverSignOut(void *h, const QString &name)
    {
        FakeHandle *f = noteBlocking(h, name);
        int delay = 0;
        {
            QMutexLocker l(&m_mutex);
            delay = m_logoutDelayMs;
        }
        if (delay > 0)
            QThread::msleep(ulong(delay));
        QMutexLocker l(&m_mutex);
        endBlockingLocked(f);
        if (m_logoutResult.isEmpty())
            return;
        f->sticky.insert(QStringLiteral("logged_out"),
                         QJsonObject{{QStringLiteral("type"), QStringLiteral("logged_out")},
                                     {QStringLiteral("result"), m_logoutResult}});
        ++f->status.sessionStateVersion;
    }
    void markLoginOkLocked(FakeHandle *f)
    {
        f->sticky.insert(QStringLiteral("login_ok"),
                         QJsonObject{{QStringLiteral("type"), QStringLiteral("login_ok")}});
        ++f->status.sessionStateVersion;
    }

    mutable QMutex m_mutex;
    QHash<QString, int> m_calls;
    std::vector<std::unique_ptr<FakeHandle>> m_handles;
    QHash<QString, int> m_anyHeld;
    QList<QThread *> m_blockingThreads;
    QString m_destroyReport = QStringLiteral("store_closed=true waited_ms=0 builds=1 "
                                             "client_alive=false still_open=none "
                                             "runtime_shutdown=completed");
    QString m_logoutResult = QStringLiteral("ok");
    quint64 m_nextSerial = 0;
    quint64 m_takeSeq = 0;
    int m_retireDelayMs = 0;
    int m_logoutDelayMs = 0;
    int m_stopSyncDelayMs = 0;
    int m_concurrent = 0;
    int m_maxConcurrent = 0;
    int m_destroyOverlaps = 0;
    int m_callsAfterDestroy = 0;
    bool m_available = true;
    bool m_createFails = false;
    bool m_holdStarting = false;
    bool m_autoLoginOk = true;
};

} // namespace lightning::accounts::testing
