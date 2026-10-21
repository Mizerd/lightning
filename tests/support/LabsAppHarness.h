#pragma once

// Live accounts (Labs), Phase 1: support for WP1-A's app-level suites
// (RemovalOrderTest, RevocationTest, QuitParallelTest, CrossAccountOpenTest,
// FlagOffParityTest).
//
// ScriptedLabsBridge implements lightning::accounts::RustBridgeApi as the
// interface contract states it (la-p1-interfaces §5.1, §3.3, §3.4), and
// nothing more, so these suites depend on the contract rather than on another
// package's fake. Thread-safe: SessionPool calls its blocking methods from
// the host retirement pool.
//
// Never a real credential: every token below is a fixture string.

#include "accounts/HostRetirementPool.h"
#include "accounts/RustBridgeApi.h"
#include "accounts/SessionPool.h"
#include "accounts/StoreOwnershipRegistry.h"
#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "storage/AppDataPaths.h"
#include "storage/InMemorySecretStore.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QSettings>
#include <QThread>
#include <QtTest>

#include <functional>
#include <memory>

namespace labstest {

inline const QString kAlice = QStringLiteral("@alice:one.example");
inline const QString kBob = QStringLiteral("@bob:two.example");
inline const QString kCarol = QStringLiteral("@carol:three.example");
inline const QString kHsOne = QStringLiteral("https://one.example");
inline const QString kHsTwo = QStringLiteral("https://two.example");
inline const QString kHsThree = QStringLiteral("https://three.example");

// A label as titles carry it: isolated (FSI ... PDI).
inline QString iso(const QString &label)
{
    return QChar(0x2068) + label + QChar(0x2069);
}

// How long a background account may take to be admitted: the pool's gate
// (foreground initial sync, a clear lag probe over 2 s) plus its keyring
// reads and restore. Generous: a timeout here is a harness failure.
constexpr int kHostStartTimeoutMs = 20000;

class ScriptedLabsBridge final : public lightning::accounts::RustBridgeApi
{
public:
    struct Call {
        QString name;
        QString userId;
        qint64 atMs = 0;
        bool onGuiThread = false;
    };

    ScriptedLabsBridge() { m_clock.start(); }

    // ── Knobs ──
    void setAvailable(bool available)
    {
        QMutexLocker lock(&m_mutex);
        m_available = available;
    }
    // Every blocking retirement step (stopSync, shutdownTasks,
    // destroyAndReport, logout) sleeps this long, on the caller's thread.
    void setRetireDelayMs(int ms)
    {
        QMutexLocker lock(&m_mutex);
        m_retireDelayMs = ms;
    }
    // The account's store never reports closed (store_any_held stays 1 and
    // destroy reports store_closed=false).
    void setStoreHeldForever(const QString &userId, bool held)
    {
        QMutexLocker lock(&m_mutex);
        if (held)
            m_heldForever.insert(userId);
        else
            m_heldForever.remove(userId);
    }
    // The account's restore never answers: the host stays Starting.
    void setStartingHeld(const QString &userId, bool held)
    {
        QMutexLocker lock(&m_mutex);
        if (held)
            m_startingHeld.insert(userId);
        else
            m_startingHeld.remove(userId);
        for (Handle &h : m_handles) {
            if (h.userId == userId && !held && h.restored)
                ++h.version;
        }
    }
    // The server answered M_UNKNOWN_TOKEN, not a soft logout: a hard
    // revocation by the shared classification.
    void revoke(const QString &userId)
    {
        QMutexLocker lock(&m_mutex);
        m_revoked.insert(userId);
        for (Handle &h : m_handles) {
            if (h.userId == userId)
                ++h.tokenGeneration;
        }
    }
    void setAccessToken(const QString &userId, const QString &token)
    {
        QMutexLocker lock(&m_mutex);
        m_tokens.insert(userId, token);
    }
    // What the server sign-out reports through the session state, as the
    // sticky `logged_out` entry the real bridge sets when logout()/
    // oauthLogout() finishes ("ok", "already_invalid", "failed"). Empty: no
    // answer at all (a hang, so the pool's own sign-out budget decides).
    void setLogoutResult(const QString &result)
    {
        QMutexLocker lock(&m_mutex);
        m_logoutResult = result;
    }
    // Runs inside logout()/oauthLogout(), on the pool's thread, with the
    // account the handle belongs to. For facts that are safe to read from
    // any thread (QFileInfo::exists).
    void setLogoutHook(std::function<void(const QString &userId)> hook)
    {
        QMutexLocker lock(&m_mutex);
        m_logoutHook = std::move(hook);
    }

    // ── Observations ──
    int calls(const QString &name) const
    {
        QMutexLocker lock(&m_mutex);
        int n = 0;
        for (const Call &c : m_log)
            n += c.name == name ? 1 : 0;
        return n;
    }
    int callsFor(const QString &name, const QString &userId) const
    {
        QMutexLocker lock(&m_mutex);
        int n = 0;
        for (const Call &c : m_log)
            n += (c.name == name && c.userId == userId) ? 1 : 0;
        return n;
    }
    int totalCalls() const
    {
        QMutexLocker lock(&m_mutex);
        return int(m_log.size()) + m_availableCalls;
    }
    QList<Call> log() const
    {
        QMutexLocker lock(&m_mutex);
        return m_log;
    }
    // Index of the first call named `name` for `userId`, or -1.
    int firstIndexOf(const QString &name, const QString &userId) const
    {
        QMutexLocker lock(&m_mutex);
        for (int i = 0; i < m_log.size(); ++i) {
            if (m_log.at(i).name == name && m_log.at(i).userId == userId)
                return i;
        }
        return -1;
    }
    qint64 firstAtMs(const QString &name, const QString &userId) const
    {
        QMutexLocker lock(&m_mutex);
        for (const Call &c : m_log) {
            if (c.name == name && c.userId == userId)
                return c.atMs;
        }
        return -1;
    }
    qint64 nowMs() const { return m_clock.elapsed(); }

    // ── RustBridgeApi ──
    bool available() const override
    {
        QMutexLocker lock(&m_mutex);
        // Counted: T-OFF-1 asserts a flag-off controller never asks.
        ++m_availableCalls;
        return m_available;
    }
    void labsEnable() override { record(QStringLiteral("labsEnable"), nullptr); }
    void *createBackground(const QString &storePath,
                           const QByteArray &profileJson) override
    {
        const QJsonObject profile =
            QJsonDocument::fromJson(profileJson).object();
        const QJsonObject account =
            profile.value(QStringLiteral("require_account")).toObject();
        QMutexLocker lock(&m_mutex);
        Handle h;
        h.id = m_nextHandle++;
        h.store = storePath;
        h.userId = account.value(QStringLiteral("user_id")).toString();
        h.deviceId = account.value(QStringLiteral("device_id")).toString();
        m_handles.insert(h.id, h);
        m_log.append(Call{ QStringLiteral("createBackground"), h.userId,
                           m_clock.elapsed(), onGui() });
        return reinterpret_cast<void *>(h.id);
    }
    QString setMediaStoreKey(void *h, const QByteArray &, bool) override
    {
        record(QStringLiteral("setMediaStoreKey"), h);
        return QString();
    }
    QString restore(void *h, const QString &, const QString &, const QString &,
                    const QString &, const QString &) override
    {
        markRestored(h);
        record(QStringLiteral("restore"), h);
        return QString();
    }
    QString oauthRestore(void *h, const QString &, const QString &,
                         const QString &, const QString &, const QString &,
                         const QString &) override
    {
        markRestored(h);
        record(QStringLiteral("oauthRestore"), h);
        return QString();
    }
    void startSync(void *h) override { record(QStringLiteral("startSync"), h); }
    bool stopSync(void *h) override
    {
        record(QStringLiteral("stopSync"), h);
        delay();
        return true;
    }
    QString pollEvent(void *) override { return QString(); }
    QString pollCommandEvent(void *) override { return QString(); }
    bool bgPoll(void *h, lightning::accounts::BgStatus *out) override
    {
        QMutexLocker lock(&m_mutex);
        const auto it = m_handles.constFind(quintptr(h));
        if (it == m_handles.cend() || !out)
            return false;
        out->sessionStateVersion = it->version;
        out->tokenGeneration = it->tokenGeneration;
        out->notableUpdatesTotal = 0;
        out->queuedEvents = 0;
        out->queuedCommands = 0;
        out->dirty = false;
        out->revoked = m_revoked.contains(it->userId);
        return true;
    }
    QByteArray bgTake(void *, quint32) override
    {
        return QByteArrayLiteral(
            R"({"v":1,"seq":1,"baseline":true,"rescan":false,"counts":[],"rows":[],"invites":0})");
    }
    QByteArray bgTakeSessionState(void *h) override
    {
        QMutexLocker lock(&m_mutex);
        const auto it = m_handles.constFind(quintptr(h));
        QJsonObject state;
        quint64 version = 0;
        if (it != m_handles.cend() && it->restored
            && !m_startingHeld.contains(it->userId)) {
            version = it->version;
            state.insert(QStringLiteral("login_ok"),
                         QJsonObject{ { QStringLiteral("user_id"), it->userId },
                                      { QStringLiteral("device_id"),
                                        it->deviceId } });
            state.insert(QStringLiteral("initial_sync_done"), QJsonObject{});
            state.insert(QStringLiteral("room_list_sync_state"),
                         QJsonObject{ { QStringLiteral("state"),
                                        QStringLiteral("running") } });
        }
        // The server sign-out's answer: sticky, like the bridge's.
        if (it != m_handles.cend() && !it->loggedOutResult.isEmpty()) {
            version = it->version;
            state.insert(QStringLiteral("logged_out"),
                         QJsonObject{ { QStringLiteral("type"),
                                        QStringLiteral("logged_out") },
                                      { QStringLiteral("result"),
                                        it->loggedOutResult } });
        }
        const QJsonObject root{ { QStringLiteral("v"), 1 },
                                { QStringLiteral("version"), qint64(version) },
                                { QStringLiteral("state"), state } };
        return QJsonDocument(root).toJson(QJsonDocument::Compact);
    }
    QString bgSetLocalRules(void *h, const QStringList &) override
    {
        record(QStringLiteral("bgSetLocalRules"), h);
        return QString();
    }
    QString bgSetSendQueueEnabled(void *h, bool) override
    {
        record(QStringLiteral("bgSetSendQueueEnabled"), h);
        return QString();
    }
    lightning::accounts::CustodySnapshot takeCustody(void *h) override
    {
        lightning::accounts::CustodySnapshot out;
        QMutexLocker lock(&m_mutex);
        const auto it = m_handles.constFind(quintptr(h));
        if (it == m_handles.cend())
            return out;
        out.valid = true;
        out.hasSession = true;
        out.accessToken = m_tokens.value(it->userId);
        out.generation = it->tokenGeneration;
        out.revoked = m_revoked.contains(it->userId);
        if (out.revoked)
            out.softLogout = false;
        return out;
    }
    void sessionTokensPersisted(void *h, quint64, bool) override
    {
        record(QStringLiteral("sessionTokensPersisted"), h);
    }
    void logout(void *h) override
    {
        record(QStringLiteral("logout"), h);
        runLogoutHook(h);
        delay();
        markLoggedOut(h);
    }
    QString oauthLogout(void *h) override
    {
        record(QStringLiteral("oauthLogout"), h);
        runLogoutHook(h);
        delay();
        markLoggedOut(h);
        return QString();
    }
    QString shutdownTasks(void *h) override
    {
        record(QStringLiteral("shutdownTasks"), h);
        delay();
        return QStringLiteral("tasks=0");
    }
    QString finalSessionTokens(void *h) override
    {
        record(QStringLiteral("finalSessionTokens"), h);
        QMutexLocker lock(&m_mutex);
        const auto it = m_handles.constFind(quintptr(h));
        if (it == m_handles.cend())
            return QString();
        return QString::fromUtf8(QJsonDocument(QJsonObject{
            { QStringLiteral("access_token"), m_tokens.value(it->userId) },
            { QStringLiteral("refresh_token"), QString() } })
                                     .toJson(QJsonDocument::Compact));
    }
    QString destroyAndReport(void *h) override
    {
        record(QStringLiteral("destroyAndReport"), h);
        delay();
        QMutexLocker lock(&m_mutex);
        const auto it = m_handles.find(quintptr(h));
        if (it == m_handles.end())
            return QStringLiteral("store_closed=true runtime_shutdown=completed");
        const bool held = m_heldForever.contains(it->userId);
        if (!held)
            m_closedStores.insert(it->store);
        it->destroyed = true;
        return held ? QStringLiteral("store_closed=false still_open=fixture "
                                     "runtime_shutdown=completed")
                    : QStringLiteral("store_closed=true runtime_shutdown=completed");
    }
    // No ledger in this fixture: a retirement keeps final_session_tokens'
    // pair (the ledger path has its own cases in HostRetirementPoolTest).
    void *tokenLedgerRetain(void *h) override
    {
        record(QStringLiteral("tokenLedgerRetain"), h);
        return nullptr;
    }
    QString tokenLedgerLatest(void *) override { return QString(); }
    void tokenLedgerRelease(void *) override {}
    int storeAnyHeld(const QString &storePath) override
    {
        QMutexLocker lock(&m_mutex);
        if (storePath.isEmpty())
            return -1;
        for (const Handle &h : std::as_const(m_handles)) {
            if (h.store != storePath)
                continue;
            if (m_heldForever.contains(h.userId) || !h.destroyed)
                return 1;
        }
        return 0;
    }
    quint64 handleSerial(void *h) override { return quint64(quintptr(h)); }

private:
    struct Handle {
        quintptr id = 0;
        QString store;
        QString userId;
        QString deviceId;
        bool restored = false;
        bool destroyed = false;
        // The sticky logged_out result, once the server sign-out answered.
        QString loggedOutResult;
        quint64 version = 0;
        quint64 tokenGeneration = 1;
    };

    static bool onGui()
    {
        return QCoreApplication::instance()
            && QThread::currentThread() == QCoreApplication::instance()->thread();
    }
    void record(const QString &name, void *h)
    {
        QMutexLocker lock(&m_mutex);
        const QString user = h ? m_handles.value(quintptr(h)).userId : QString();
        m_log.append(Call{ name, user, m_clock.elapsed(), onGui() });
    }
    void markRestored(void *h)
    {
        QMutexLocker lock(&m_mutex);
        const auto it = m_handles.find(quintptr(h));
        if (it == m_handles.end())
            return;
        it->restored = true;
        it->version = 1;
    }
    // The bridge's sticky logged_out, and the session-state version bump
    // that makes a host read it (bgPoll -> bgTakeSessionState).
    void markLoggedOut(void *h)
    {
        QMutexLocker lock(&m_mutex);
        const auto it = m_handles.find(quintptr(h));
        if (it == m_handles.end() || m_logoutResult.isEmpty())
            return;
        it->loggedOutResult = m_logoutResult;
        ++it->version;
    }
    void delay()
    {
        int ms = 0;
        {
            QMutexLocker lock(&m_mutex);
            ms = m_retireDelayMs;
        }
        if (ms > 0)
            QThread::msleep(ulong(ms));
    }
    void runLogoutHook(void *h)
    {
        std::function<void(const QString &)> hook;
        QString user;
        {
            QMutexLocker lock(&m_mutex);
            hook = m_logoutHook;
            user = m_handles.value(quintptr(h)).userId;
        }
        if (hook)
            hook(user);
    }

    mutable QMutex m_mutex;
    QElapsedTimer m_clock;
    bool m_available = true;
    mutable int m_availableCalls = 0;
    int m_retireDelayMs = 0;
    quintptr m_nextHandle = 0x1000;
    QHash<quintptr, Handle> m_handles;
    QSet<QString> m_heldForever;
    QSet<QString> m_startingHeld;
    QSet<QString> m_revoked;
    QSet<QString> m_closedStores;
    QHash<QString, QString> m_tokens;
    QString m_logoutResult = QStringLiteral("ok");
    QList<Call> m_log;
    std::function<void(const QString &)> m_logoutHook;
};

// Installs the bridge for one scope; the production table comes back after.
class BridgeScope
{
public:
    explicit BridgeScope(lightning::accounts::RustBridgeApi *api)
    {
        lightning::accounts::setRustBridgeApiForTest(api);
    }
    ~BridgeScope() { lightning::accounts::setRustBridgeApiForTest(nullptr); }
    BridgeScope(const BridgeScope &) = delete;
    BridgeScope &operator=(const BridgeScope &) = delete;
};

// LIGHTNING_LABS_LIVE_ACCOUNTS for one scope (read at AppController
// construction). Since 2026-10-20 Labs is offered by default and the variable
// only FORCES it off: `available` true leaves it unset (the shipped default),
// false sets "0".
class LabsEnvScope
{
public:
    explicit LabsEnvScope(bool available)
    {
        if (available)
            qunsetenv("LIGHTNING_LABS_LIVE_ACCOUNTS");
        else
            qputenv("LIGHTNING_LABS_LIVE_ACCOUNTS", "0");
    }
    ~LabsEnvScope() { qunsetenv("LIGHTNING_LABS_LIVE_ACCOUNTS"); }
    LabsEnvScope(const LabsEnvScope &) = delete;
    LabsEnvScope &operator=(const LabsEnvScope &) = delete;
};

// The keyring the controller and its keyring worker share: process-local,
// so the worker goes through SettingsManager on the GUI thread (§5.6).
inline AppController::SecretStoreFactory inMemoryKeyring()
{
    return [](QObject *parent) -> std::unique_ptr<SecretStore> {
        return std::make_unique<InMemorySecretStore>(parent);
    };
}

inline void isolateSettings(const QString &configHome, const QString &dataHome,
                            const char *appName)
{
    qputenv("XDG_CONFIG_HOME", configHome.toUtf8());
    qputenv("XDG_DATA_HOME", dataHome.toUtf8());
    // No daemon: generic notices take the tray path, where they are counted.
    qputenv("DBUS_SESSION_BUS_ADDRESS",
            "unix:path=/nonexistent/lightning-no-daemon");
    QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
    QCoreApplication::setApplicationName(QString::fromLatin1(appName));
}

inline void clearSettings()
{
    QSettings settings;
    settings.clear();
    settings.sync();
    // The process-wide registries outlive a case: a store a previous case
    // left "close unconfirmed" (by design busy until restart) would refuse
    // every later case's host on the same fixture path (integration).
    lightning::accounts::StoreOwnershipRegistry::process().clearForTest();
    lightning::accounts::HostSignInRetirements::process().clearForTest();
}

// Saves the three fixture accounts and signs Alice in on the mock backend.
inline void saveAccountsAndSignInAlice(AppController &app)
{
    SettingsManager *s = app.settings();
    s->saveSession(kHsOne, kAlice, QStringLiteral("ALICEDEV"),
                   QStringLiteral("alice-token-fixture"));
    s->saveSession(kHsTwo, kBob, QStringLiteral("BOBDEV"),
                   QStringLiteral("bob-token-fixture"));
    s->saveSession(kHsThree, kCarol, QStringLiteral("CAROLDEV"),
                   QStringLiteral("carol-token-fixture"));
    app.switchToAccount(kAlice);
    QTRY_VERIFY(!app.accountSwitching());
    QTRY_VERIFY(app.loggedIn());
}

// Pins `pinned`, turns live accounts on, and waits until the pool holds a
// store for each pinned account. False (with a QWARN) on a harness timeout.
inline bool startLiveAccounts(AppController &app, ScriptedLabsBridge &bridge,
                              const QStringList &pinned)
{
    for (const QString &uid : pinned) {
        bridge.setAccessToken(uid, uid == kBob ? QStringLiteral("bob-token-fixture")
                                   : uid == kCarol
                                       ? QStringLiteral("carol-token-fixture")
                                       : QStringLiteral("alice-token-fixture"));
        app.settings()->setKeepRunningFor(uid, true);
    }
    app.settings()->setLabsConcurrentAccounts(true);
    if (!app.liveAccountsActive()) {
        qWarning("live accounts did not turn on");
        return false;
    }
    QElapsedTimer waited;
    waited.start();
    for (const QString &uid : pinned) {
        while (!app.sessionPoolForTest()->holdsStore(uid)) {
            if (waited.elapsed() > kHostStartTimeoutMs) {
                qWarning("a pinned account never became live (harness)");
                return false;
            }
            QTest::qWait(50);
        }
    }
    return true;
}

// The recorded identity of a saved account (its store path included).
inline matrix::app_data::AccountIdentity identityOf(AppController &app,
                                                    const QString &uid)
{
    matrix::app_data::AccountIdentity identity;
    app.settings()->resolveSavedIdentity(uid, &identity);
    return identity;
}

// A file inside the account's store and root, so a deletion is observable.
inline void plantStoreFile(const matrix::app_data::AccountIdentity &identity)
{
    QDir().mkpath(identity.rustStorePath);
    QFile marker(identity.rustStorePath + QStringLiteral("/marker"));
    if (marker.open(QIODevice::WriteOnly))
        marker.write("fixture");
}

} // namespace labstest
