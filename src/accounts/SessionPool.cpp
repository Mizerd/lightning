#include "accounts/SessionPool.h"

#include "accounts/BackgroundDrainer.h"
#include "accounts/BackgroundSessionHost.h"
#include "accounts/HostRetirementPool.h"
#include "accounts/KeyringWorker.h"
#include "accounts/RetirementLatches.h"
#include "accounts/RevocationProofs.h"
#include "accounts/RustBridgeApi.h"
#include "accounts/SessionScheduler.h"
#include "accounts/SessionTokenCustody.h"
#include "accounts/StoreOwnershipRegistry.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "notifications/NotificationManager.h"
#include "storage/AppDataPaths.h"

#include <QDateTime>
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <utility>

Q_LOGGING_CATEGORY(lcSessionPool, "lightning.accounts.pool")

namespace lightning::accounts {

namespace {

constexpr int kMaxSlots = 10;
// A store another handle still holds (the foreground's own retirement after a
// switch away) is waited for for as long as its close may still come; past
// this a line says so, once. Only a close REPORTED unconfirmed is "Store busy"
// (review M6: a slow close that succeeds is not busy).
constexpr qint64 kClaimWaitLogMs = 15000;
// While a removal waits for a keyring write of that account to return.
constexpr int kRemovalWritePollMs = 50;
constexpr int kLastLiveIntervalMs = 10 * 60 * 1000;
constexpr int kMemoryIntervalMs = 10 * 1000;

QString slugOf(const QString &uid)
{
    return matrix::app_data::safeUserSlug(uid);
}

// Drops this reference to a secret. Not a scrub: QString and QByteArray are
// implicitly shared, so fill() would detach and overwrite only a private copy
// while the caller's original keeps the bytes (review L14).
void forget(QString &value)
{
    value.clear();
}

} // namespace

SessionPool::SessionPool(Deps deps, QObject *parent)
    : QObject(parent)
    , m_deps(std::move(deps))
{
    m_api = m_deps.api ? m_deps.api : &rustBridgeApi();
    m_registry = m_deps.registry ? m_deps.registry : &StoreOwnershipRegistry::process();
    m_retirement = new HostRetirementPool(m_api, this);
    m_latches = new RetirementLatches(m_api, this);
    m_scheduler = new SessionScheduler(m_deps.monotonicMs, {}, this);
    if (m_deps.suspendAwareMs)
        m_scheduler->setSuspendAwareClock(m_deps.suspendAwareMs);
    m_drainer = new BackgroundDrainer(this);
    KeyringWorker::Deps keyringDeps;
    keyringDeps.settings = m_deps.settings;
    keyringDeps.guiSecretStore = m_deps.guiSecretStore;
    keyringDeps.secretStoreFactory = m_deps.secretStoreFactory;
    m_keyring = new KeyringWorker(std::move(keyringDeps), this);

    m_scheduler->setCap(SettingsManager::kMaxLiveAccounts);
    m_admitTimer.setInterval(kAdmitIntervalMs);
    connect(&m_admitTimer, &QTimer::timeout, this, &SessionPool::admit);
    m_lastLiveTimer.setInterval(kLastLiveIntervalMs);
    connect(&m_lastLiveTimer, &QTimer::timeout, this, &SessionPool::writeLastLiveAt);
    m_memoryTimer.setInterval(kMemoryIntervalMs);
    connect(&m_memoryTimer, &QTimer::timeout, this,
            [this] { (void)m_scheduler->memoryGuardWouldPause(); });
    m_shutdownPoll.setInterval(kShutdownPollMs);
    connect(&m_shutdownPoll, &QTimer::timeout, this, &SessionPool::checkShutdownDrained);

    connect(m_latches, &RetirementLatches::released, this, &SessionPool::onLatchReleased);
    connect(m_scheduler, &SessionScheduler::admissionMayHaveChanged, this, &SessionPool::schedule);
    connect(m_scheduler, &SessionScheduler::resumeDetected, this, &SessionPool::onResume);
    connect(m_keyring, &KeyringWorker::keyringReadable, this, [this] {
        // A locked keyring pauses starts and never marks an account gone; the
        // backend answering again resumes them (design §6.1.6).
        const QSet<QString> locked = std::exchange(m_keyringLocked, {});
        for (const QString &uid : locked)
            setState(uid, LiveState::NotLive);
        schedule();
    });
    if (m_deps.settings) {
        connect(m_deps.settings, &SettingsManager::keepRunningChanged, this, &SessionPool::schedule);
        connect(m_deps.settings, &SettingsManager::accountsChanged, this, &SessionPool::schedule);
        connect(m_deps.settings, &SettingsManager::roomNotificationModeChanged, this,
                [this](const QString &roomId) {
            for (BackgroundSessionHost *host : std::as_const(m_hosts))
                host->refreshModes(roomId);
        });
    }
}

SessionPool::~SessionPool()
{
    // Never blocks: the owner drained everything first (shutdownAsync or
    // waitForQuit). A Starting host still here keeps its handle until the
    // process exits; it is said once.
    for (BackgroundSessionHost *host : std::as_const(m_hosts)) {
        if (host->handle())
            qCWarning(lcSessionPool) << "pool destroyed with a background account still open"
                                     << "acct=" << host->identity().slug;
    }
}

void SessionPool::start()
{
    if (m_started)
        return;
    m_started = true;
    m_api->labsEnable();
    m_scheduler->noteStarted();
    m_scheduler->startProbe();
    m_admitTimer.start();
    m_lastLiveTimer.start();
    m_memoryTimer.start();
    m_timersRunning = true;
    qCInfo(lcSessionPool) << "live accounts pool started"
                          << "max_live=" << SettingsManager::kMaxLiveAccounts;
    schedule();
}

void SessionPool::updateIdleTimers()
{
    if (!m_started || m_quitting || m_shuttingDown)
        return;
    // Nothing pinned and nothing hosted, starting or stopping: no lag probe,
    // admission tick or memory read wakes the process (§18 round 1, L7).
    const bool busy = !m_hosts.isEmpty() || !m_reading.isEmpty() || !m_stopping.isEmpty()
        || (m_deps.settings && !m_deps.settings->keepRunningUserIds().isEmpty());
    if (busy == m_timersRunning)
        return;
    m_timersRunning = busy;
    if (busy) {
        // Restarted only if idleness stopped it (a test that stopped the
        // probe itself keeps its scripted clock).
        if (std::exchange(m_probeStoppedForIdle, false))
            m_scheduler->startProbe();
        m_admitTimer.start();
        m_lastLiveTimer.start();
        m_memoryTimer.start();
    } else {
        // Only a probe that runs is stopped, and so restarted (a test that
        // stopped it for a scripted clock keeps it stopped).
        m_probeStoppedForIdle = m_scheduler->probeRunning();
        if (m_probeStoppedForIdle)
            m_scheduler->stopProbe();
        m_admitTimer.stop();
        m_lastLiveTimer.stop();
        m_memoryTimer.stop();
    }
}

// ---- foreground facts ----------------------------------------------------------------

void SessionPool::setForegroundUserId(const QString &uid)
{
    if (m_foreground == uid)
        return;
    const QString previous = std::exchange(m_foreground, uid);
    if (!previous.isEmpty() && !m_hosts.contains(previous) && !m_stopping.contains(previous))
        setState(previous, LiveState::NotLive);
    if (!uid.isEmpty()) {
        m_releasedForForeground.remove(uid);
        m_claimWaitSince.remove(uid);
        // Opened in the foreground: whatever refused it here (no sign-in in
        // the keyring, a revocation, missing encryption data, a locked
        // keyring) is the foreground's to fix, and a successful sign-in
        // there clears the proofs and the revoked mark. So the next time it
        // is a background account it is asked again, once (review M5). A
        // store busy until restart stays busy (m_storeBusy is not touched).
        m_scheduler->clearFailures(uid);
        m_keyringLocked.remove(uid);
        // A removed account opened in the foreground again was signed in
        // afresh (its record was gone). The removal's two process-lifetime
        // marks, "never restarted" and "no keyring writes", end here, once
        // that removal has answered and no write of the old session can
        // still land; otherwise they stay until restart (review round 2).
        if (m_removing.contains(uid) && !m_removals.contains(uid)) {
            if (m_keyring->writeInFlightFor(uid)) {
                qCWarning(lcSessionPool) << "a removed account is signed in again while an old "
                                            "keyring write is still in flight; it cannot run in "
                                            "the background until restart"
                                         << "acct=" << slugOf(uid);
            } else {
                m_removing.remove(uid);
                m_keyring->allowWritesFor(uid);
            }
        }
        // A start still reading its keyring is dropped: its read answers
        // into nothing, and createHost() re-checks the foreground besides
        // (§18 round 1, L1).
        if (m_reading.contains(uid)) {
            m_keyring->cancelFor(uid);
            abandonStart(uid, m_reading.value(uid));
        }
        // The click path released it first. If a host is somehow still open
        // on that account, the foreground's store claim refuses; retire it so
        // the next attempt finds the store free.
        if (m_hosts.contains(uid)) {
            qCWarning(lcSessionPool) << "the foreground account still had a background "
                                        "session; retiring it"
                                     << "acct=" << slugOf(uid);
            retireHost(uid, true);
        }
        setState(uid, LiveState::Active);
    }
    schedule();
}

void SessionPool::noteForegroundInitialSyncDone()
{
    m_scheduler->noteForegroundInitialSyncDone();
}

void SessionPool::noteForegroundHomeserverReachable(const QString &homeserverUrl)
{
    m_scheduler->noteHomeserverReachable(normalizeHomeserver(homeserverUrl));
    schedule();
}

// ---- pins ----------------------------------------------------------------------------

SessionPool::PinResult SessionPool::setKeepRunning(const QString &uid, bool on)
{
    if (!m_deps.settings)
        return PinResult::RefusedNoRecord;
    const auto result = m_deps.settings->setKeepRunningFor(uid, on);
    if (result == SettingsManager::KeepRunningResult::RefusedCap)
        return PinResult::RefusedCap;
    if (result == SettingsManager::KeepRunningResult::RefusedNoRecord)
        return PinResult::RefusedNoRecord;
    if (on) {
        // The user asked again: a refusal that gave up on this account may
        // try once more (a store busy until restart stays busy).
        m_scheduler->clearFailures(uid);
        if (m_reported.value(uid) != LiveState::StoreBusy)
            m_reported.remove(uid);
    } else {
        if (m_reading.contains(uid)) {
            m_keyring->cancelFor(uid);
            abandonStart(uid, m_reading.value(uid));
        }
        if (m_hosts.contains(uid))
            retireHost(uid, true);
        else if (!m_stopping.contains(uid) && uid != m_foreground)
            setState(uid, LiveState::NotLive);
    }
    schedule();
    return PinResult::Ok;
}

// ---- scheduling ----------------------------------------------------------------------

void SessionPool::schedule()
{
    if (m_schedulePending || !m_started)
        return;
    m_schedulePending = true;
    QTimer::singleShot(0, this, [this] {
        m_schedulePending = false;
        updateIdleTimers();
        admit();
    });
}

SessionPool::Eligibility SessionPool::eligibility(const QString &uid) const
{
    if (uid.isEmpty() || uid == m_foreground || m_hosts.contains(uid) || m_reading.contains(uid)
        || m_stopping.contains(uid) || m_releasedForForeground.contains(uid)
        || m_removing.contains(uid) || m_latches->isArmed(uid))
        return Eligibility::Skip;
    SettingsManager *settings = m_deps.settings;
    if (!settings || !settings->hasSavedAccount(uid) || !settings->keepRunningFor(uid))
        return Eligibility::NoRecord;
    if (m_storeBusy.contains(uid) || m_scheduler->permanentlyRefused(uid))
        return Eligibility::Refused;
    // Revoked this run (the foreground's or a host's own proof): only a
    // sign-in through the foreground brings it back.
    if ((m_deps.accounts && m_deps.accounts->sessionRevoked(uid))
        || (m_deps.proofs && (m_deps.proofs->hardFor(uid) || m_deps.proofs->softFor(uid))))
        return Eligibility::NeedsSignIn;
    // A legacy shared-keyring account (adopted-unverified) is opened in the
    // foreground once first (design §9.2): its items may be another install's.
    if (!settings->keyringItemsDirectlyReadableFor(uid))
        return Eligibility::OpenToFinishSetup;
    if (m_keyringLocked.contains(uid))
        return Eligibility::KeyringLocked;
    if (m_scheduler->inBackoff(uid))
        return Eligibility::Wait;
    const QString hs = homeserverOf(uid);
    if (m_scheduler->homeserverPaused(hs) || m_scheduler->homeserverStarting(hs))
        return Eligibility::Wait;
    return Eligibility::Eligible;
}

void SessionPool::admit()
{
    if (!m_started || m_quitting || m_shuttingDown || !m_deps.settings)
        return;
    const QStringList pins = m_deps.settings->keepRunningUserIds();
    // Rows for the pins that cannot run, whatever the gate says: they cost no
    // keyring read, file, thread or network.
    for (const QString &uid : pins) {
        switch (eligibility(uid)) {
        case Eligibility::NeedsSignIn:
            setState(uid, LiveState::NeedsSignIn);
            break;
        case Eligibility::OpenToFinishSetup:
            setState(uid, LiveState::OpenToFinishSetup);
            break;
        case Eligibility::KeyringLocked:
            setState(uid, LiveState::KeyringLocked);
            break;
        default:
            break;
        }
    }
    if (!m_scheduler->gateOpen() || !m_scheduler->catchUpFree())
        return;
    // A live host whose sync was stopped (lost connection, resume) catches up
    // before anything new starts.
    restartReconnecting();
    if (!m_scheduler->catchUpFree())
        return;
    for (const QString &uid : pins) {
        if (eligibility(uid) != Eligibility::Eligible)
            continue;
        // Another handle still owns the store (the foreground's own handle of
        // this account, retiring after a switch away): wait without a keyring
        // read for as long as its close may still come. A close reported
        // unconfirmed, or another account's record on this same store, is
        // Store busy for the rest of the process.
        matrix::app_data::AccountIdentity identity;
        if (m_deps.settings->resolveSavedIdentity(uid, &identity)) {
            const auto hold =
                m_registry->holdOn(identity.rustStorePath, identity.effectiveStoreSlug());
            if (hold == StoreOwnershipRegistry::Hold::Held) {
                waitForStore(uid);
                continue;
            }
            if (hold != StoreOwnershipRegistry::Hold::None) {
                markStoreBusy(uid, homeserverOf(uid),
                              hold == StoreOwnershipRegistry::Hold::Duplicate ? "duplicate"
                                                                              : "unconfirmed");
                continue;
            }
        }
        m_claimWaitSince.remove(uid);
        if (!m_scheduler->underCap(liveHostCount(), !m_foreground.isEmpty())
            || !m_scheduler->fdHeadroomFor(liveHostCount() + 1)) {
            setState(uid, LiveState::Paused);
            continue;
        }
        // A pair its last retirement could not save is written first,
        // through the worker, by the same compare-and-swap: the keyring
        // still holds an older pair, possibly a consumed refresh token, and
        // the read below would start the host on it (§18 round 2, R2-2).
        // Re-pin, re-admission after a memory or quota retirement, and a
        // release that ended StoreBusy or abandoned all come through here.
        if (writeParked(uid))
            continue;
        beginStart(uid);
        return;   // one catch-up at a time
    }
}

QString SessionPool::homeserverOf(const QString &uid) const
{
    if (const BackgroundSessionHost *host = m_hosts.value(uid))
        return host->identity().homeserver;
    if (m_reading.contains(uid))
        return m_reading.value(uid);
    matrix::app_data::AccountIdentity identity;
    if (m_deps.settings && m_deps.settings->resolveSavedIdentity(uid, &identity))
        return normalizeHomeserver(identity.homeserver);
    return {};
}

QString SessionPool::normalizeHomeserver(const QString &url)
{
    QString trimmed = url.trimmed();
    if (trimmed.isEmpty())
        return {};
    if (!trimmed.contains(QLatin1String("://")))
        trimmed.prepend(QLatin1String("https://"));
    QUrl parsed(trimmed);
    parsed.setScheme(parsed.scheme().toLower());
    parsed.setHost(parsed.host().toLower());
    QUrl normalized = parsed.adjusted(QUrl::StripTrailingSlash | QUrl::NormalizePathSegments);
    // StripTrailingSlash keeps a lone "/" (Qt strips only while the path is
    // longer than one character), so "https://hs/" and "https://hs" would be
    // two homeservers and the foreground reaching one would never reset the
    // other's backoff (integration, SessionPoolTest).
    if (normalized.path() == QLatin1String("/"))
        normalized.setPath(QString());
    // The scheme's default port names the same server ("https://hs:443" is
    // "https://hs"), by the same reasoning (G5 recheck).
    if ((normalized.scheme() == QLatin1String("https") && normalized.port() == 443)
        || (normalized.scheme() == QLatin1String("http") && normalized.port() == 80))
        normalized.setPort(-1);
    return normalized.toString();
}

void SessionPool::beginStart(const QString &uid)
{
    const QString hs = homeserverOf(uid);
    m_scheduler->acquireCatchUp(uid);
    m_scheduler->noteStarting(hs, uid);
    m_reading.insert(uid, hs);
    // An account removed while NOT live and re-added in this process is
    // still tombstoned in the activity model (m_removing never saw it): lift
    // that at the epoch this host will report at, before its first state
    // (la1-M contract amendment, review round 2 N1). A no-op otherwise.
    if (m_deps.activity && !m_removing.contains(uid) && m_deps.activity->isRemoved(uid)
        && !m_deps.activity->reopenAccount(uid, epochFor(uid))) {
        qCWarning(lcSessionPool) << "an activity row could not be reopened below its floor"
                                 << "acct=" << slugOf(uid);
    }
    setState(uid, LiveState::Starting);
    Q_EMIT liveHostCountChanged(liveHostCount());
    KeyringWorker::ReadRequest request;
    request.userId = uid;
    request.oauth = m_deps.settings->isOAuthAccount(uid);
    request.mediaKey = true;
    qCInfo(lcSessionPool) << "starting a background account" << "acct=" << slugOf(uid);
    // The keyring worker answers on this thread, never inside this call.
    m_keyring->read(request, [this, uid](const KeyringWorker::ReadResult &result) {
        if (!m_reading.contains(uid))
            return;   // cancelled meanwhile (unpinned, released, removed)
        const QString hs = m_reading.value(uid);
        if (result.locked) {
            // §6: "cannot tell" is never "no account". Paused, not failed:
            // the backend's readable edge resumes it.
            m_keyringLocked.insert(uid);
            abandonStart(uid, hs);
            setState(uid, LiveState::KeyringLocked);
            return;
        }
        if (result.accessToken.isEmpty()) {
            // The keyring answered and holds no sign-in for this account: it
            // needs one through the foreground. Nothing is deleted.
            m_scheduler->noteFailure(uid, hs, true);
            abandonStart(uid, hs);
            setState(uid, LiveState::NeedsSignIn);
            return;
        }
        createHost(uid, result.mediaKey, result.mediaAdmitEncrypted, result.accessToken,
                   result.refreshToken, result.oauthClientId);
    });
}

void SessionPool::abandonStart(const QString &uid, const QString &homeserver)
{
    m_reading.remove(uid);
    m_scheduler->noteStartFinished(homeserver, uid);
    m_scheduler->releaseCatchUp(uid);
    Q_EMIT liveHostCountChanged(liveHostCount());
}

void SessionPool::waitForStore(const QString &uid)
{
    // "Stopping": the store is still being closed by another handle. The next
    // admit tick asks again; nothing is read or opened meanwhile.
    setState(uid, LiveState::Stopping);
    const qint64 now = m_scheduler->now();
    const auto it = m_claimWaitSince.constFind(uid);
    if (it == m_claimWaitSince.cend()) {
        m_claimWaitSince.insert(uid, now);
    } else if (it.value() >= 0 && now - it.value() >= kClaimWaitLogMs) {
        qCInfo(lcSessionPool) << "a background account still waits for its store to close"
                              << "acct=" << slugOf(uid)
                              << "waited_ms=" << now - it.value();
        m_claimWaitSince.insert(uid, -1);   // said once
    }
}

void SessionPool::markStoreBusy(const QString &uid, const QString &homeserver, const char *why)
{
    m_claimWaitSince.remove(uid);
    m_storeBusy.insert(uid);
    m_scheduler->noteFailure(uid, homeserver, true);
    qCInfo(lcSessionPool) << "a background account's store is busy until restart"
                          << "acct=" << slugOf(uid) << "why=" << why;
    setState(uid, LiveState::StoreBusy);
}

QString SessionPool::freeSlot() const
{
    QSet<QString> used;
    for (auto it = m_books.cbegin(); it != m_books.cend(); ++it)
        used.insert(it->slot);
    for (int i = 0; i < kMaxSlots; ++i) {
        const QString slot = QStringLiteral("b%1").arg(i);
        if (!used.contains(slot))
            return slot;
    }
    return QStringLiteral("bx");
}

QByteArray SessionPool::profileFor(const QString &uid, const QString &deviceId,
                                   const QString &slot) const
{
    SettingsManager *settings = m_deps.settings;
    // Events older than max(appStart, lastLiveAt) - 30 s were seen, or could
    // have been, by an earlier run: dropped at the source (R2-M5).
    const qint64 floorMs =
        std::max(m_deps.appStartMs, settings->lastLiveAtFor(uid)) - 30000;
    const QJsonObject profile{
        {QStringLiteral("v"), 1},
        {QStringLiteral("slot"), slot},
        {QStringLiteral("require_account"),
         QJsonObject{{QStringLiteral("user_id"), uid}, {QStringLiteral("device_id"), deviceId}}},
        {QStringLiteral("strict_trust"), settings->strictDeviceTrust()},
        // No read receipt, no index-all from a background account (R1-H8).
        {QStringLiteral("receipt_privacy"), 2},
        {QStringLiteral("index_all_hold"), double(4294967295.0)},
        {QStringLiteral("floor_ms"), double(std::max<qint64>(0, floorMs))},
        {QStringLiteral("local_rule_rooms"),
         QJsonArray::fromStringList(settings->locallyDecidedNotificationRoomsFor(uid))},
        {QStringLiteral("max_blocking_threads"), 8},
        {QStringLiteral("sqlite"), QStringLiteral("low_memory")},
        {QStringLiteral("retry_limit"), 3},
        {QStringLiteral("max_retry_time_ms"), 30000},
        {QStringLiteral("watch_cap"), 200},
    };
    return QJsonDocument(profile).toJson(QJsonDocument::Compact);
}

void SessionPool::createHost(const QString &uid, const QByteArray &mediaKeyIn,
                             bool admitEncrypted, const QString &accessIn,
                             const QString &refreshIn, const QString &clientIdIn)
{
    // SENSITIVE: the tokens and the key are scrubbed before this returns.
    QByteArray mediaKey = mediaKeyIn;
    QString access = accessIn;
    QString refresh = refreshIn;
    QString clientId = clientIdIn;
    const auto scrubAll = [&] {
        mediaKey.clear();
        forget(access);
        forget(refresh);
        forget(clientId);
    };
    const QString hs = m_reading.value(uid);
    SettingsManager *settings = m_deps.settings;

    // Opened in the foreground while its keyring read was in flight (§18
    // round 1, L1): the foreground owns that store now; nothing opens here.
    if (uid == m_foreground || m_releasedForForeground.contains(uid)) {
        qCInfo(lcSessionPool) << "a background start was overtaken by the foreground"
                              << "acct=" << slugOf(uid);
        if (m_reading.contains(uid))   // setForegroundUserId() may have ended it
            abandonStart(uid, hs);
        scrubAll();
        return;
    }

    // The RECORDED store, never a path re-derived from the user id (§6).
    matrix::app_data::AccountIdentity identity;
    const QVariantMap record = settings->accountRecord(uid);
    const QString deviceId = record.value(QStringLiteral("deviceId")).toString();
    if (!settings->resolveSavedIdentity(uid, &identity) || deviceId.isEmpty()) {
        qCWarning(lcSessionPool) << "a background account's record does not resolve"
                                 << "acct=" << slugOf(uid);
        m_scheduler->noteFailure(uid, hs, true);
        abandonStart(uid, hs);
        setState(uid, LiveState::Stopped);
        scrubAll();
        return;
    }

    // One store, one owner (I1): atomically, before anything opens it, under
    // the slug every claim of this store passes (the foreground's too): the
    // RECORDED store slug, which differs from the canonical one for a
    // delegated or case-variant account (review M7).
    const QString storeSlug = identity.effectiveStoreSlug();
    quint64 serial = 0;
    quint64 conflict = 0;
    const auto verdict = m_registry->claimOpen(identity.rustStorePath, storeSlug,
                                               StoreOwnershipRegistry::Role::Starting, &serial,
                                               &conflict);
    if (verdict != StoreOwnershipRegistry::ClaimVerdict::Granted) {
        abandonStart(uid, hs);
        scrubAll();
        qCInfo(lcSessionPool) << "background start refused by the store claim"
                              << "acct=" << slugOf(uid)
                              << "verdict=" << StoreOwnershipRegistry::verdictName(verdict)
                              << "conflict_serial=" << conflict;
        // The foreground's own handle on this store may still be closing (a
        // switch away from this account a moment ago): waited for as admit()
        // waits. Only an unconfirmed close or another account's record is
        // busy for good.
        const auto hold = verdict == StoreOwnershipRegistry::ClaimVerdict::RefusedDuplicateRecord
            ? StoreOwnershipRegistry::Hold::Duplicate
            : m_registry->holdOn(identity.rustStorePath, storeSlug);
        if (hold == StoreOwnershipRegistry::Hold::Held)
            waitForStore(uid);
        else if (hold == StoreOwnershipRegistry::Hold::None)
            setState(uid, LiveState::NotLive);   // gone meanwhile: the next tick starts it
        else
            markStoreBusy(uid, hs,
                          hold == StoreOwnershipRegistry::Hold::Duplicate ? "duplicate"
                                                                          : "unconfirmed");
        return;
    }
    m_claimWaitSince.remove(uid);

    Book book;
    book.slot = freeSlot();
    void *handle = m_api->createBackground(identity.rustStorePath,
                                           profileFor(uid, deviceId, book.slot));
    if (!handle) {
        // Nothing was opened: the record goes again.
        m_registry->noteClosed(serial, true);
        m_scheduler->noteFailure(uid, hs, false);
        abandonStart(uid, hs);
        setState(uid, LiveState::Stopped);
        scrubAll();
        return;
    }
    m_api->setMediaStoreKey(handle, mediaKey, admitEncrypted);

    BackgroundSessionHost::Identity hostIdentity;
    hostIdentity.userId = uid;
    hostIdentity.deviceId = deviceId;
    hostIdentity.homeserver = hs;
    hostIdentity.storePath = identity.rustStorePath;
    hostIdentity.slug = slugOf(uid);
    // The display name, else the localpart: never the full MXID (§18 round
    // 1, L5), as AppController::liveAccountLabel().
    const QString displayName =
        record.value(QStringLiteral("displayName")).toString().trimmed();
    QString localpart = uid;
    if (uid.startsWith(QLatin1Char('@'))) {
        const qsizetype colon = uid.indexOf(QLatin1Char(':'));
        localpart = colon > 1 ? uid.mid(1, colon - 1) : uid.mid(1);
    }
    hostIdentity.label = !displayName.isEmpty() ? displayName
                         : !localpart.isEmpty() ? localpart : uid;
    hostIdentity.oauth = settings->isOAuthAccount(uid);

    BackgroundSessionHost::Deps hostDeps;
    hostDeps.api = m_api;
    hostDeps.settings = settings;
    hostDeps.notifications = m_deps.notifications;
    hostDeps.activity = m_deps.activity;
    hostDeps.monotonicMs = m_deps.monotonicMs;
    hostDeps.onCustody = [this](BackgroundSessionHost *host, const CustodySnapshot &custody) {
        onCustody(host, custody);
    };
    auto *host = new BackgroundSessionHost(std::move(hostDeps), hostIdentity, handle, serial,
                                           epochFor(uid), this);
    book.persistedDigest = custody::accessTokenDigest(access);
    m_books.insert(uid, book);
    m_reading.remove(uid);
    m_hosts.insert(uid, host);
    wireHost(host);
    m_drainer->addHost(host);
    resizeRetirementThreads();
    host->beginCatchUp();

    // The restore dispatches; its result is the sticky login_ok/login_failed.
    // The Rust side runs the reservation and the crypto precheck inside it.
    const QString result = hostIdentity.oauth
        ? m_api->oauthRestore(handle, identity.homeserver, uid, deviceId, clientId, access,
                              refresh)
        : m_api->restore(handle, identity.homeserver, uid, deviceId, access, refresh);
    scrubAll();
    if (!result.isEmpty()) {
        qCInfo(lcSessionPool) << "background restore was not dispatched"
                              << "acct=" << slugOf(uid);
        // Nothing restored, so nothing to hand back.
        host->setPhase(BackgroundSessionHost::Phase::Running);
        m_scheduler->noteFailure(uid, hs, false);
        m_scheduler->noteStartFinished(hs, uid);
        m_scheduler->releaseCatchUp(uid);
        retireHost(uid, false);
        setState(uid, LiveState::Stopped);
        return;
    }
    Q_EMIT liveHostCountChanged(liveHostCount());
}

void SessionPool::wireHost(BackgroundSessionHost *host)
{
    connect(host, &BackgroundSessionHost::loginSucceeded, this,
            [this, host] { onLoginSucceeded(host); });
    connect(host, &BackgroundSessionHost::loginFailed, this,
            [this, host](BackgroundSessionHost::FailureKind kind) {
        onLoginFailed(host, int(kind));
    });
    connect(host, &BackgroundSessionHost::connectionLost, this,
            [this, host] { onConnectionLost(host); });
    connect(host, &BackgroundSessionHost::rateLimited, this, [this, host](qint64 retryAfterMs) {
        // A 429 pauses that homeserver for every host (design §6.1.7).
        m_scheduler->pauseHomeserver(host->identity().homeserver,
                                     std::max(SessionScheduler::kHomeserverPauseMs, retryAfterMs));
    });
    connect(host, &BackgroundSessionHost::catchUpComplete, this,
            [this, host] { onCatchUpComplete(host); });
    connect(host, &BackgroundSessionHost::loggedOut, this, [this, host](const QString &result) {
        const QString uid = host->identity().userId;
        if (!m_removals.contains(uid))
            return;
        // Only "ok" signed anything out. "no_session" / "no_active_session"
        // (nothing to sign out from this handle), "failed" and anything
        // unknown leave the server's view of the device unknown (review L12).
        const ServerSignOut outcome = result == QLatin1String("ok")
            ? ServerSignOut::Ok
            : (result == QLatin1String("already_invalid") ? ServerSignOut::AlreadyInvalid
                                                          : ServerSignOut::Failed);
        removalSignedOut(uid, outcome);
    });
}

// ---- host results ----------------------------------------------------------------------

void SessionPool::onLoginSucceeded(BackgroundSessionHost *host)
{
    const QString uid = host->identity().userId;
    m_registry->promote(host->storeSerial(), StoreOwnershipRegistry::Role::Host);
    m_scheduler->noteStartFinished(host->identity().homeserver, uid);
    m_scheduler->noteSuccess(uid);
    // The catch-up semaphore stays held until catchUpComplete().
    setState(uid, LiveState::Running);
    if (m_deferred.contains(uid)) {
        const Deferred deferred = m_deferred.take(uid);
        if (deferred.signOut) {
            removalRetire(uid);   // signs out first, then retires
            return;
        }
        retireHost(uid, deferred.persist, deferred.cancelKeyring);
        return;
    }
    host->refreshModes();
    schedule();
}

void SessionPool::onLoginFailed(BackgroundSessionHost *host, int kindValue)
{
    const auto kind = BackgroundSessionHost::FailureKind(kindValue);
    const QString uid = host->identity().userId;
    const QString hs = host->identity().homeserver;
    m_scheduler->noteStartFinished(hs, uid);
    m_scheduler->releaseCatchUp(uid);
    LiveState state = LiveState::Stopped;
    switch (kind) {
    case BackgroundSessionHost::FailureKind::StoreInUse:
        // Another handle in this process holds the store although the claim
        // said free: never retried until restart (design §6.1.7).
        m_storeBusy.insert(uid);
        m_scheduler->noteFailure(uid, hs, true);
        state = LiveState::StoreBusy;
        break;
    case BackgroundSessionHost::FailureKind::EncryptionDataMissing:
        // Nothing was created (the precheck never opens a missing crypto
        // store); only the foreground's explicit "Open anyway" may mint.
        m_scheduler->noteFailure(uid, hs, true);
        state = LiveState::EncryptionDataMissing;
        break;
    case BackgroundSessionHost::FailureKind::Transient:
        m_scheduler->noteFailure(uid, hs, false);
        break;
    }
    const bool removing = m_deferred.contains(uid) && m_deferred.value(uid).signOut;
    m_deferred.remove(uid);
    // A restore the server refused with M_UNKNOWN_TOKEN is a revocation, not
    // a failure to retry: custody says so (its flag is sticky).
    const CustodySnapshot custody =
        host->handle() ? m_api->takeCustody(host->handle()) : CustodySnapshot{};
    const BackgroundSessionHost::Identity identity = host->identity();
    // Not restored, so the tokens are the ones the keyring holds: nothing to
    // hand back.
    host->setPhase(BackgroundSessionHost::Phase::Running);
    retireHost(uid, false);
    if (removing) {
        removalSignedOut(uid, ServerSignOut::Failed);
        return;
    }
    if (custody.valid && custody.revoked) {
        handleRevocation(identity, custody);
        return;
    }
    setState(uid, state);
}

void SessionPool::onConnectionLost(BackgroundSessionHost *host)
{
    const QString uid = host->identity().userId;
    if (host->phase() != BackgroundSessionHost::Phase::Running || m_removals.contains(uid))
        return;
    // Its catch-up after the outage waits for the semaphore like any other
    // (design §6.1.5), and for its backoff, or for the foreground to reach
    // that homeserver again.
    host->setPhase(BackgroundSessionHost::Phase::Reconnecting);
    m_scheduler->releaseCatchUp(uid);
    m_scheduler->noteFailure(uid, host->identity().homeserver, false);
    m_needsRestart.insert(uid);
    void *handle = host->handle();
    m_stopInFlight.insert(uid);
    // On the handle's own queue: a retirement asked meanwhile runs only after
    // this returned (review H1).
    const quint64 epoch = epochFor(uid);
    m_retirement->runBlocking(handle, [api = m_api, handle] { api->stopSync(handle); },
                              [this, uid, epoch] {
        if (!stillCurrent(uid, epoch))
            return;
        m_stopInFlight.remove(uid);
        schedule();
    });
    qCInfo(lcSessionPool) << "background account lost its connection; it catches up again later"
                          << "acct=" << slugOf(uid);
}

void SessionPool::restartReconnecting()
{
    const QSet<QString> waiting = m_needsRestart;
    for (const QString &uid : waiting) {
        BackgroundSessionHost *host = m_hosts.value(uid);
        if (!host || m_stopInFlight.contains(uid) || m_scheduler->inBackoff(uid)
            || m_scheduler->homeserverPaused(host->identity().homeserver)
            || m_books.value(uid).writePaused)
            continue;
        m_needsRestart.remove(uid);
        m_scheduler->acquireCatchUp(uid);
        host->setPhase(BackgroundSessionHost::Phase::Running);
        m_api->startSync(host->handle());
        host->beginCatchUp();
        return;
    }
}

void SessionPool::onResume()
{
    // Every running host catches up again, one at a time, after the
    // foreground (the scheduler holds the semaphore for it a while).
    for (BackgroundSessionHost *host : std::as_const(m_hosts)) {
        if (host->phase() != BackgroundSessionHost::Phase::Running)
            continue;
        const QString uid = host->identity().userId;
        host->setPhase(BackgroundSessionHost::Phase::Reconnecting);
        m_scheduler->releaseCatchUp(uid);
        m_needsRestart.insert(uid);
        void *handle = host->handle();
        m_stopInFlight.insert(uid);
        const quint64 epoch = epochFor(uid);
        m_retirement->runBlocking(handle, [api = m_api, handle] { api->stopSync(handle); },
                                  [this, uid, epoch] {
            if (!stillCurrent(uid, epoch))
                return;
            m_stopInFlight.remove(uid);
            schedule();
        });
    }
}

void SessionPool::onCatchUpComplete(BackgroundSessionHost *host)
{
    m_scheduler->releaseCatchUp(host->identity().userId);
    schedule();
}

// ---- tokens --------------------------------------------------------------------------------

void SessionPool::onCustody(BackgroundSessionHost *host, const CustodySnapshot &custody)
{
    if (!custody.valid)
        return;
    const QString uid = host->identity().userId;
    if (m_removals.contains(uid)) {
        // Nothing is written for an account being removed (§6); the rotation
        // waiting inside the refresh is released at once, not after its bound.
        answerRotation(uid, custody.ledgerGeneration, false);
        return;
    }
    if (custody.revoked) {
        handleRevocation(host->identity(), custody);
        return;
    }
    // Only a pair that differs from the one the keyring holds is written; the
    // one it already holds is the answer (a rotation this read pairs with
    // nothing newer than what is saved).
    if (!custody::retryWriteNeeded(custody.accessToken, m_books.value(uid).persistedDigest)) {
        answerRotation(uid, custody.ledgerGeneration, !custody.accessToken.isEmpty());
        return;
    }
    writeLiveTokens(uid, custody.accessToken, custody.refreshToken, custody.ledgerGeneration);
}

void SessionPool::answerRotation(const QString &uid, quint64 ledgerGeneration, bool saved)
{
    // 24a485c5's rule, for a background account: matrix-sdk holds the request
    // it retries with the new access token until this answer (bounded by
    // sessiontokens::WAIT_CAP and DRAIN_STALL), so a kill before the keyring
    // write leaves that token unused. Only the CURRENT host's handle is
    // answered: a retired one's wait ended at its shutdown (stop_waiting).
    if (ledgerGeneration == 0)
        return;
    BackgroundSessionHost *host = m_hosts.value(uid);
    if (!host || !host->handle())
        return;
    m_api->sessionTokensPersisted(host->handle(), ledgerGeneration, saved);
}

void SessionPool::writeLiveTokens(const QString &uid, const QString &access,
                                  const QString &refresh, quint64 ledgerGeneration)
{
    const BackgroundSessionHost *host = m_hosts.value(uid);
    if (!host)
        return;
    KeyringWorker::WriteRequest request;
    request.userId = uid;
    request.deviceId = host->identity().deviceId;
    request.kind = KeyringWorker::WriteKind::Live;
    request.tokens = SessionTokenPair{access, refresh};
    const QByteArray digest = custody::accessTokenDigest(access);
    // Until it answers, a retirement's handback accepts this pair as "what the
    // host last wrote" (review L8).
    m_books[uid].pendingLiveDigests.append(digest);
    const quint64 epoch = epochFor(uid);
    m_keyring->write(request, [this, uid, digest, epoch, ledgerGeneration](
                                  const KeyringWorker::WriteResult &result) {
        // A host retired meanwhile, or a new one of this account, is not
        // this write's (review M4): its book is another session's.
        if (!stillCurrent(uid, epoch))
            return;
        auto book = m_books.find(uid);
        if (book == m_books.end())
            return;
        // A write that timed out may still be inside the store and land
        // later: it stays this host's own pair for its handback, parked or
        // not (§18 round 2, R2-1).
        if (result.outcome != KeyringWorker::WriteOutcome::TimedOut)
            book->pendingLiveDigests.removeOne(digest);
        // Answered first, whatever the outcome: only Written says the pair is
        // in the keyring; a refused write releases the wait too, and the
        // pause below keeps the session from rotating further unseen.
        answerRotation(uid, ledgerGeneration,
                       result.outcome == KeyringWorker::WriteOutcome::Written);
        switch (result.outcome) {
        case KeyringWorker::WriteOutcome::Written: {
            book->persistedDigest = digest;
            book->writeAttempts = 0;
            if (book->writeRetry)
                book->writeRetry->stop();
            if (book->writePaused) {
                book->writePaused = false;
                if (BackgroundSessionHost *host = m_hosts.value(uid)) {
                    m_api->bgSetSendQueueEnabled(host->handle(), true);
                    // Its sync restarts through the semaphore like a reconnect.
                    m_needsRestart.insert(uid);
                    schedule();
                }
                qCInfo(lcSessionPool) << "a background account's sign-in is saved again"
                                      << "acct=" << slugOf(uid);
            }
            break;
        }
        case KeyringWorker::WriteOutcome::Skipped:
            // The record is gone or names another device: what was removed
            // stays removed (§6). Nothing to retry.
            qCInfo(lcSessionPool) << "a background token write was not kept: the record "
                                     "is gone or names another device"
                                  << "acct=" << slugOf(uid);
            break;
        case KeyringWorker::WriteOutcome::Failed:
        case KeyringWorker::WriteOutcome::TimedOut:
            pauseForWriteFailure(uid);
            break;
        case KeyringWorker::WriteOutcome::Cancelled:
            // A removal of this account forbade it: the removal's path decides
            // what happens to the pair. (A retirement's cancel bumped the
            // epoch, so its callbacks returned above.) Any other cancel of a
            // live host's write is not expected; it is treated as a failed
            // write, never dropped silently: the rotated pair stays in Rust
            // and sync pauses until a write lands (review round 2).
            if (m_removals.contains(uid) || m_removing.contains(uid))
                break;
            qCWarning(lcSessionPool) << "a background token write was cancelled while its "
                                        "session is live; pausing it"
                                     << "acct=" << slugOf(uid);
            pauseForWriteFailure(uid);
            break;
        }
    });
}

void SessionPool::pauseForWriteFailure(const QString &uid)
{
    auto book = m_books.find(uid);
    BackgroundSessionHost *host = m_hosts.value(uid);
    if (book == m_books.end() || !host)
        return;
    // The pair stays in Rust (final_session_tokens() cannot lose it). Until a
    // write lands, nothing may rotate it further unseen: sync stops and the
    // send queue is disabled. Nothing is retired or deleted (§6).
    if (!book->writePaused) {
        book->writePaused = true;
        m_api->bgSetSendQueueEnabled(host->handle(), false);
        if (host->phase() == BackgroundSessionHost::Phase::Running) {
            host->setPhase(BackgroundSessionHost::Phase::Reconnecting);
            m_scheduler->releaseCatchUp(uid);
            void *handle = host->handle();
            m_stopInFlight.insert(uid);
            const quint64 epoch = epochFor(uid);
            m_retirement->runBlocking(handle, [api = m_api, handle] { api->stopSync(handle); },
                                      [this, uid, epoch] {
                if (stillCurrent(uid, epoch))
                    m_stopInFlight.remove(uid);
            });
        }
        qCWarning(lcSessionPool) << "a background account's rotated sign-in could not be "
                                    "saved; its sync is paused until it is"
                                 << "acct=" << slugOf(uid);
    }
    if (!book->writeRetry) {
        book->writeRetry = new QTimer(this);
        book->writeRetry->setSingleShot(true);
        connect(book->writeRetry, &QTimer::timeout, this, [this, uid] { retryTokenWrite(uid); });
    }
    // 30 s, doubling, at most ten minutes apart (the foreground's rule).
    const int shift = std::min(book->writeAttempts, 5);
    ++book->writeAttempts;
    book->writeRetry->start(std::min(m_writeRetryBaseMs << shift, m_writeRetryBaseMs * 20));
}

void SessionPool::retryTokenWrite(const QString &uid)
{
    BackgroundSessionHost *host = m_hosts.value(uid);
    if (!host || !host->handle() || m_removals.contains(uid))
        return;
    // The session's pair now, which is newer than any the failed write carried.
    const CustodySnapshot custody = m_api->takeCustody(host->handle());
    if (!custody.valid || custody.accessToken.isEmpty()) {
        pauseForWriteFailure(uid);
        return;
    }
    writeLiveTokens(uid, custody.accessToken, custody.refreshToken, custody.ledgerGeneration);
}

void SessionPool::handleRevocation(const BackgroundSessionHost::Identity &identity,
                                   const CustodySnapshot &custody)
{
    const QString uid = identity.userId;
    RevocationEventFacts facts;
    facts.softLogout = custody.softLogout;
    facts.hadRefreshToken = custody.hadRefreshToken;
    facts.oauthSession = m_deps.settings && m_deps.settings->isOAuthAccount(uid);
    if (!revocationNeedsKeyringProbe(facts)) {
        finishRevocation(identity, int(classifyRevocation(facts, false).outcome));
        return;
    }
    // The same probe as the foreground's handler, on the worker: a refresh
    // token the keyring holds (or a keyring that cannot answer) means the
    // session never got it, not that it ended.
    KeyringWorker::ReadRequest request;
    request.userId = uid;
    request.oauth = false;
    const quint64 epoch = epochFor(uid);
    m_keyring->read(request, [this, identity, facts, epoch](const KeyringWorker::ReadResult &result) {
        // Retired meanwhile (a click, an unpin) or opened in the foreground:
        // this probe is about a session that is not ours any more, and the
        // foreground meets its own revocation itself (review M4).
        if (!stillCurrent(identity.userId, epoch))
            return;
        const bool holdsOrCannotTell = result.locked || !result.refreshToken.isEmpty();
        finishRevocation(identity, int(classifyRevocation(facts, holdsOrCannotTell).outcome));
    });
}

bool SessionPool::stillCurrent(const QString &uid, quint64 epoch) const
{
    return epochFor(uid) == epoch && uid != m_foreground;
}

void SessionPool::finishRevocation(const BackgroundSessionHost::Identity &identity,
                                   int outcomeValue)
{
    const QString uid = identity.userId;
    if (m_removals.contains(uid))
        return;
    const auto outcome = RevocationOutcome(outcomeValue);
    const QString deviceId = identity.deviceId;
    if (outcome == RevocationOutcome::KeyringHoldsOrCannotTell) {
        // Not a revocation: the keyring card's case. No proof, no "revoked"
        // mark; the host stops without writing anything back.
        qCInfo(lcSessionPool) << "a background session could not renew its sign-in; the "
                                 "keyring holds one or cannot tell"
                              << "acct=" << slugOf(uid);
        m_keyringLocked.insert(uid);
        retireHost(uid, false, true);
        setState(uid, LiveState::KeyringLocked);
        return;
    }
    // The same classification and the same proof as the foreground, in the
    // per-account map login() consults in Labs (R1-C1).
    if (m_deps.proofs && !deviceId.isEmpty()) {
        if (outcome == RevocationOutcome::Hard) {
            m_deps.proofs->recordHard(uid, deviceId);
        } else if (outcome == RevocationOutcome::Soft) {
            // The homeserver as the RECORD names it (§6: recorded, never
            // re-derived), which is what login()'s soft-proof check compares
            // with the record (§18 round 1, L4).
            matrix::app_data::AccountIdentity recorded;
            if (m_deps.settings && m_deps.settings->resolveSavedIdentity(uid, &recorded))
                m_deps.proofs->recordSoft(uid, recorded.homeserver, deviceId);
        }
    }
    if (m_deps.accounts)
        m_deps.accounts->markSessionRevoked(uid);
    qCInfo(lcSessionPool) << "a background account's sign-in ended"
                          << "acct=" << slugOf(uid)
                          << "outcome=" << (outcome == RevocationOutcome::Hard
                                                ? "hard"
                                                : (outcome == RevocationOutcome::Soft ? "soft"
                                                                                      : "unproven"));
    // Never endRevokedSession, never a move-aside, never a deletion: signing
    // in again finds the proof and takes the foreground's path (§3.3.2).
    m_scheduler->noteFailure(uid, identity.homeserver, true);
    retireHost(uid, false, true);   // a no-op when it already retired (a failed start)
    setState(uid, LiveState::NeedsSignIn);
    Q_EMIT hostSignInExpired(uid);
}

// ---- retirement ------------------------------------------------------------------------------

void SessionPool::retireHost(const QString &uid, bool persist, bool cancelKeyring)
{
    BackgroundSessionHost *host = m_hosts.value(uid);
    if (!host)
        return;
    // A Starting host cannot be retired (R1-C3): its restore can rotate the
    // tokens and send the unsent queue. It retires at its login result.
    if (host->phase() == BackgroundSessionHost::Phase::Starting) {
        Deferred &deferred = m_deferred[uid];
        deferred.persist = deferred.persist && persist;
        deferred.cancelKeyring = deferred.cancelKeyring || cancelKeyring;
        return;
    }
    // A dropped handback still answers (Cancelled), so a retirement waiting
    // on one always finishes (review M1).
    if (cancelKeyring)
        m_keyring->cancelFor(uid);
    ++m_epochs[uid];
    // A stop_sync still on this handle's queue is the retirement's to follow
    // (HostRetirementPool runs the destroy after it returned).
    m_stopInFlight.remove(uid);
    if (persist && m_deps.settings)
        m_deps.settings->setLastLiveAtFor(uid, QDateTime::currentMSecsSinceEpoch());

    const BackgroundSessionHost::Identity identity = host->identity();
    const quint64 serial = host->storeSerial();
    void *handle = host->takeHandle();
    m_drainer->removeHost(host);
    m_hosts.remove(uid);
    host->deleteLater();
    m_needsRestart.remove(uid);
    m_scheduler->releaseCatchUp(uid);
    m_scheduler->noteStartFinished(identity.homeserver, uid);
    Book book = m_books.take(uid);
    if (book.writeRetry) {
        book.writeRetry->stop();
        book.writeRetry->deleteLater();
    }

    m_registry->noteReleasing(serial);
    m_latches->arm(uid, identity.storePath);
    m_stopping.insert(uid, Stopping{identity.storePath, persist});
    if (persist)
        HostSignInRetirements::process().begin(uid);
    setState(uid, LiveState::Stopping);
    // The retiring host still needs its thread: it is in m_stopping now, so
    // the cap does not shrink under the job submitted next (review round 2).
    resizeRetirementThreads();
    Q_EMIT liveHostCountChanged(liveHostCount());

    const QByteArray expected = book.persistedDigest;
    const QList<QByteArray> accepted = book.pendingLiveDigests;
    const QString deviceId = identity.deviceId;
    m_retirement->retire(
        HostRetirementPool::RetireJob{handle, identity.slug, persist},
        [this, uid, persist, serial, expected, accepted, deviceId](
            const HostRetirementPool::RetireOutcome &outcome) {
            // Handback: the pair the session held at the end, when it differs
            // from the one the keyring holds, compared-and-swapped against it.
            if (persist && outcome.collectedFinalTokens
                && custody::needsHandback(outcome.finalTokens.accessToken, expected)) {
                KeyringWorker::WriteRequest request;
                request.userId = uid;
                request.deviceId = deviceId;
                request.kind = KeyringWorker::WriteKind::Handback;
                request.tokens = outcome.finalTokens;
                request.expectedDigest = expected;
                request.acceptedDigests = accepted;
                const bool closed = outcome.storeClosed;
                const SessionTokenPair finalTokens = outcome.finalTokens;
                m_keyring->write(request, [this, uid, persist, serial, closed, deviceId,
                                           expected, accepted, finalTokens](
                                              const KeyringWorker::WriteResult &result) {
                    // Whatever the outcome, cancelled included: the record,
                    // the sign-in count and the latch all wait on this.
                    qCInfo(lcSessionPool) << "background retirement handback"
                                          << "acct=" << slugOf(uid)
                                          << "written="
                                          << (result.outcome == KeyringWorker::WriteOutcome::Written)
                                          << "cancelled="
                                          << (result.outcome == KeyringWorker::WriteOutcome::Cancelled);
                    // Refused or unanswered (not a removal's cancel): the
                    // keyring still holds an older pair. The foreground writes
                    // this one right before it reads the sign-in (MUST 1).
                    if (result.outcome == KeyringWorker::WriteOutcome::Failed
                        || result.outcome == KeyringWorker::WriteOutcome::TimedOut) {
                        HostSignInRetirements::process().parkUnsaved(
                            HostSignInRetirements::UnsavedHandback{uid, deviceId, expected,
                                                                   accepted, finalTokens});
                        qCWarning(lcSessionPool)
                            << "a background account's last sign-in could not be saved at "
                               "retirement; it is kept for the next read"
                            << "acct=" << slugOf(uid);
                    }
                    finishRetirement(uid, persist, serial, closed);
                });
                return;
            }
            finishRetirement(uid, persist, serial, outcome.storeClosed);
        });
}

void SessionPool::finishRetirement(const QString &uid, bool persist, quint64 storeSerial,
                                   bool storeClosed)
{
    // An unconfirmed close keeps the record Retiring and the latch armed for
    // the rest of the process: "Store busy (restart to reopen)".
    m_registry->noteClosed(storeSerial, storeClosed);
    if (!storeClosed)
        m_storeBusy.insert(uid);
    if (persist)
        HostSignInRetirements::process().end(uid);
    m_latches->noteHandbackDone(uid);
    if (!storeClosed) {
        setState(uid, LiveState::StoreBusy);
    }
    checkShutdownDrained();
}

void SessionPool::onLatchReleased(const QString &uid)
{
    m_stopping.remove(uid);
    if (m_reported.value(uid) == LiveState::Stopping)
        setState(uid, uid == m_foreground ? LiveState::Active : LiveState::NotLive);
    resizeRetirementThreads();
    Q_EMIT liveHostCountChanged(liveHostCount());
    schedule();
}

quint64 SessionPool::releaseForForeground(const QString &uid,
                                          std::function<void(ReleaseResult)> done)
{
    const quint64 ticket = m_nextTicket++;
    Release release;
    release.uid = uid;
    release.done = std::move(done);
    // Never restarted behind the switch's back.
    m_releasedForForeground.insert(uid);
    if (m_reading.contains(uid)) {
        // Nothing opened yet: drop the start.
        m_keyring->cancelFor(uid);
        abandonStart(uid, m_reading.value(uid));
        setState(uid, LiveState::NotLive);
    }
    if (m_storeBusy.contains(uid)) {
        m_releases.insert(ticket, release);
        QTimer::singleShot(0, this, [this, ticket] {
            const Release r = m_releases.take(ticket);
            if (r.done)
                r.done(ReleaseResult::StoreBusy);
        });
        return ticket;
    }
    if (BackgroundSessionHost *host = m_hosts.value(uid)) {
        // A Starting host retires only at its login result (R1-C3); its latch
        // is armed now, so the wait below cannot answer "free" before then.
        if (host->phase() == BackgroundSessionHost::Phase::Starting)
            m_latches->arm(uid, host->identity().storePath);
        retireHost(uid, true);
    }
    // Free at once (asynchronously) when nothing holds it; otherwise when the
    // latch frees, or StoreBusy at the end of the budget. The latch frees once
    // the handback SETTLED; Free also needs no write of this account still
    // inside the keyring (a timed-out one can still land after the
    // foreground's read). A handback that settled unsaved is parked for the
    // foreground's own write before its read (MUST 1).
    // A write still running (a parked pair, a stuck live write) is waited for
    // within the same budget (§18 round 3, R3-1).
    const qint64 deadline = m_scheduler->now() + kStoreCloseBudgetMs;
    release.latchTicket = m_latches->await(uid, kStoreCloseBudgetMs,
                                           [this, ticket, deadline](bool free) {
        settleRelease(ticket, free, deadline);
    });
    m_releases.insert(ticket, release);
    return ticket;
}

void SessionPool::settleRelease(quint64 ticket, bool free, qint64 deadlineMs)
{
    const auto it = m_releases.find(ticket);
    if (it == m_releases.end())
        return;
    const bool writeLanding = signInWriteBusy(it->uid);
    if (free && writeLanding && m_scheduler->now() < deadlineMs) {
        QTimer::singleShot(kReleaseWritePollMs, this, [this, ticket, deadlineMs] {
            settleRelease(ticket, true, deadlineMs);
        });
        return;
    }
    const Release r = it.value();
    m_releases.erase(it);
    if (free && writeLanding) {
        qCWarning(lcSessionPool) << "a background account's store is free but a keyring "
                                    "write of it has not returned; not opened yet"
                                 << "acct=" << slugOf(r.uid);
    }
    if (r.done)
        r.done(free && !writeLanding ? ReleaseResult::Free : ReleaseResult::StoreBusy);
}

void SessionPool::cancelRelease(quint64 ticket)
{
    const auto it = m_releases.find(ticket);
    if (it == m_releases.end())
        return;
    const Release release = it.value();
    m_releases.erase(it);
    m_latches->cancel(release.latchTicket);
    m_releasedForForeground.remove(release.uid);
    if (release.done)
        release.done(ReleaseResult::Cancelled);
    schedule();
}

void SessionPool::abandonRelease(const QString &uid)
{
    if (uid.isEmpty() || uid == m_foreground)
        return;
    for (auto it = m_releases.cbegin(); it != m_releases.cend(); ++it) {
        if (it->uid == uid)
            return;   // another click's release still waits for this store
    }
    if (!m_releasedForForeground.remove(uid))
        return;
    qCInfo(lcSessionPool) << "a release for the foreground was abandoned; the account may run "
                             "in the background again"
                          << "acct=" << slugOf(uid);
    // The retirement already left the row NotLive (or StoreBusy, which stays
    // busy until restart): only admission needs to look again.
    schedule();
}

void SessionPool::probeSignIn(const QString &uid, std::function<void(bool lost)> done)
{
    if (!done)
        return;
    const bool readable = m_deps.settings && m_deps.settings->hasSavedAccount(uid)
        && m_deps.settings->keyringItemsDirectlyReadableFor(uid);
    if (!readable || m_removing.contains(uid) || m_keyringLocked.contains(uid)) {
        QTimer::singleShot(0, this, [done = std::move(done)] { done(false); });
        return;
    }
    KeyringWorker::ReadRequest request;
    request.userId = uid;
    // Answered exactly once: by the read, or, when the worker drops it (a
    // cancelFor() of this account, a quit), as "cannot tell" once the read's
    // own timeout has surely passed, so the switcher's probe slot is never
    // leaked (§18 round 1, L3).
    struct Probe {
        std::function<void(bool)> done;
        bool answered = false;
    };
    auto probe = std::make_shared<Probe>();
    probe->done = std::move(done);
    m_keyring->read(request, [probe](const KeyringWorker::ReadResult &result) {
        if (std::exchange(probe->answered, true))
            return;
        // The keyring answered and holds nothing: a lost sign-in. A failed or
        // timed-out read is "cannot tell" (§6), never "no account".
        probe->done(result.ok && !result.locked && result.accessToken.isEmpty());
    });
    QTimer::singleShot(KeyringWorker::kReadTimeoutMs + 2000, this, [probe] {
        if (!std::exchange(probe->answered, true))
            probe->done(false);
    });
}

// ---- removal ----------------------------------------------------------------------------------

void SessionPool::retireForRemoval(const QString &uid, std::function<void(RemovalResult)> done)
{
    m_removing.insert(uid);
    // For the rest of the process no token of this account is written to the
    // keyring from here: AppController deletes its secrets after we answer
    // (§6, B044; review M2). A handback already queued answers Cancelled, so
    // an earlier retirement waiting on it still finishes (review M1).
    m_keyring->forbidWritesFor(uid);
    // A pair its last retirement could not save is never written now: the
    // record and the secrets are about to go (R4-1).
    HostSignInRetirements::process().takeUnsaved(uid);
    if (m_reading.contains(uid))
        abandonStart(uid, m_reading.value(uid));
    Removal removal;
    removal.done = std::move(done);
    BackgroundSessionHost *host = m_hosts.value(uid);
    if (!host) {
        // Not live: only a retirement still in flight can hold the store.
        removal.signOut = ServerSignOut::NotLive;
        m_removals.insert(uid, removal);
        removalSignedOut(uid, ServerSignOut::NotLive);
        return;
    }
    host->setQuiet(true);
    m_removals.insert(uid, removal);
    if (host->phase() == BackgroundSessionHost::Phase::Starting) {
        // Signed out at its login result (or retired at a failure).
        Deferred &deferred = m_deferred[uid];
        deferred.persist = false;
        deferred.signOut = true;
        deferred.cancelKeyring = true;
        return;
    }
    removalRetire(uid);
}

void SessionPool::removalRetire(const QString &uid)
{
    BackgroundSessionHost *host = m_hosts.value(uid);
    auto removal = m_removals.find(uid);
    if (!host || removal == m_removals.end())
        return;
    host->setQuiet(true);
    if (removal->signOutDispatched)
        return;
    removal->signOutDispatched = true;
    // DECISION 7: removing a live account also signs it out on the server,
    // bounded: the result is the sticky logged_out, or a timeout.
    removal->timer = new QTimer(this);
    removal->timer->setSingleShot(true);
    connect(removal->timer, &QTimer::timeout, this,
            [this, uid] { removalSignedOut(uid, ServerSignOut::TimedOut); });
    removal->timer->start(kServerSignOutBudgetMs);
    m_retirement->signOut(host->handle(), host->identity().oauth, {});
}

void SessionPool::removalSignedOut(const QString &uid, ServerSignOut outcome)
{
    auto removal = m_removals.find(uid);
    if (removal == m_removals.end())
        return;
    if (removal->timer) {
        removal->timer->stop();
        removal->timer->deleteLater();
        removal->timer = nullptr;
    }
    // The first answer wins: logged_out, the timeout, or a failed start.
    if (removal->answered)
        return;
    removal->answered = true;
    removal->signOut = outcome;
    removal->deadline = QDeadlineTimer(kStoreCloseBudgetMs);
    // Then retire without a handback: a removal writes nothing back. On the
    // timeout path the sign-out may still be running; the handle's queue
    // runs the retirement after it returned (review H1).
    if (m_hosts.contains(uid))
        retireHost(uid, false, true);
    // And await THIS store's latch, never the pool-wide wait.
    m_latches->await(uid, kStoreCloseBudgetMs,
                     [this, uid](bool free) { finishRemoval(uid, free); });
}

void SessionPool::finishRemoval(const QString &uid, bool latchFree)
{
    const auto it = m_removals.find(uid);
    if (it == m_removals.end())
        return;
    // A keyring write of this account the store is still inside (one that
    // was running when the removal forbade them, or one that timed out
    // earlier) could still land after AppController deletes the secrets: a
    // token at rest with no record. Waited for within the same budget, a
    // parked pair's write included until its answer arrived (R4-1).
    const bool writing = signInWriteBusy(uid);
    if (latchFree && writing && !it->deadline.hasExpired()) {
        QTimer::singleShot(kRemovalWritePollMs, this,
                           [this, uid] { finishRemoval(uid, true); });
        return;
    }
    const Removal r = it.value();
    m_removals.erase(it);
    RemovalResult result;
    result.storeFree = latchFree && !writing && !m_storeBusy.contains(uid);
    result.serverSignOut = r.signOut;
    qCInfo(lcSessionPool) << "background account removal: store"
                          << (result.storeFree ? "free" : "still held")
                          << "keyring_write_in_flight=" << writing
                          << "acct=" << slugOf(uid);
    if (r.done)
        r.done(result);
}

// ---- state ------------------------------------------------------------------------------------

void SessionPool::setState(const QString &uid, LiveState state)
{
    if (uid.isEmpty())
        return;
    const auto it = m_reported.constFind(uid);
    if (it != m_reported.cend() && it.value() == state)
        return;
    m_reported.insert(uid, state);
    // A removed account's row is gone; never re-created from here.
    if (m_deps.activity && !m_removing.contains(uid))
        m_deps.activity->setLiveState(uid, state);
    switch (state) {
    case LiveState::Stopped:
    case LiveState::StoreBusy:
    case LiveState::EncryptionDataMissing:
    case LiveState::OpenToFinishSetup:
    case LiveState::KeyringLocked:
        if (!m_removing.contains(uid))
            Q_EMIT hostNeedsAttention(uid, int(state));
        break;
    default:
        break;
    }
}

bool SessionPool::holdsStore(const QString &uid) const
{
    return m_hosts.contains(uid) || m_stopping.contains(uid);
}

bool SessionPool::signInWriteBusy(const QString &uid) const
{
    return m_writingParked.contains(uid) || m_keyring->writeInFlightFor(uid);
}

bool SessionPool::holdsSignIn(const QString &uid) const
{
    return holdsStore(uid) || signInWriteBusy(uid);
}

AccountActivityModel::LiveState SessionPool::liveState(const QString &uid) const
{
    if (!uid.isEmpty() && uid == m_foreground)
        return LiveState::Active;
    if (m_reading.contains(uid))
        return LiveState::Starting;
    if (const BackgroundSessionHost *host = m_hosts.value(uid)) {
        return host->phase() == BackgroundSessionHost::Phase::Starting ? LiveState::Starting
                                                                      : LiveState::Running;
    }
    // Retiring or not live: what was last reported (Stopping, then StoreBusy,
    // NeedsSignIn, Stopped... as the retirement and its cause decide).
    return m_reported.value(uid, LiveState::NotLive);
}

int SessionPool::liveHostCount() const
{
    return int(m_hosts.size() + m_reading.size());
}

// Threads for every host that can still make a blocking call: the live ones
// AND the retiring ones (m_stopping, until their latch frees). Sizing from the
// live set alone shrank the cap as each host was handed to the pool, so at
// quit QThreadPool queued every retirement past the second and ran them one
// after another on the last thread, against waitForQuit's budget (review
// round 2). Lowering the cap never stops a running job; it only queues.
void SessionPool::resizeRetirementThreads()
{
    m_retirement->setLiveHostCount(liveHostCount() + int(m_stopping.size()));
}

quint64 SessionPool::epochFor(const QString &uid) const
{
    return m_epochs.value(uid);
}

void SessionPool::writeLastLiveAt()
{
    if (!m_deps.settings)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (BackgroundSessionHost *host : std::as_const(m_hosts)) {
        if (host->phase() == BackgroundSessionHost::Phase::Running)
            m_deps.settings->setLastLiveAtFor(host->identity().userId, now);
    }
}

// ---- quit and flag-off ---------------------------------------------------------------------

void SessionPool::beginQuit()
{
    if (m_quitting)
        return;
    m_quitting = true;
    m_admitTimer.stop();
    m_memoryTimer.stop();
    m_lastLiveTimer.stop();
    m_scheduler->stopProbe();
    for (const QString &uid : m_reading.keys()) {
        m_keyring->cancelFor(uid);
        abandonStart(uid, m_reading.value(uid));
    }
    // Nothing more is raised while quitting: a host still restoring is
    // drained by waitForQuit() for its login result only (§18 round 1, L8).
    for (BackgroundSessionHost *host : std::as_const(m_hosts))
        host->setQuiet(true);
    // In parallel, on the host pool, beside the foreground's own retirement:
    // each retired host stays counted (m_stopping), so the cap stays at
    // hosts + 1 while they are submitted one after another.
    const QStringList uids = m_hosts.keys();
    for (const QString &uid : uids)
        retireHost(uid, true);
}

bool SessionPool::waitForQuit(int budgetMs)
{
    QDeadlineTimer deadline(budgetMs);
    // A Starting host retires at its login result: drain only those, here,
    // until the result or the budget.
    const auto anyStarting = [this] {
        for (BackgroundSessionHost *host : std::as_const(m_hosts)) {
            if (host->phase() == BackgroundSessionHost::Phase::Starting)
                return true;
        }
        return false;
    };
    // The other hosts' retirements finish meanwhile: their GUI halves and
    // their handback writes run inside this loop, a slice at a time, so a
    // host that never finishes restoring cannot spend the budget they need
    // (§18 round 1, MUST 2). Nothing is dropped: writes not reached in a
    // slice stay queued for the next one.
    const auto pumpRetirements = [this, &deadline] {
        m_retirement->waitForDone(int(std::min<qint64>(kQuitSliceMs,
                                                       std::max<qint64>(0, deadline.remainingTime()))));
        m_retirement->deliverFinishedNow();
        m_keyring->waitForWrites(int(std::min<qint64>(kQuitSliceMs * 10,
                                                      std::max<qint64>(0, deadline.remainingTime()))));
    };
    while (anyStarting() && !deadline.hasExpired()) {
        const QList<BackgroundSessionHost *> hosts = m_hosts.values();
        for (BackgroundSessionHost *host : hosts) {
            if (host->phase() == BackgroundSessionHost::Phase::Starting)
                host->drainOnce();
        }
        // A login result retired it at once; deferred retirements ran.
        for (const QString &uid : m_hosts.keys()) {
            BackgroundSessionHost *host = m_hosts.value(uid);
            if (host && host->phase() != BackgroundSessionHost::Phase::Starting)
                retireHost(uid, true);
        }
        pumpRetirements();
    }
    for (BackgroundSessionHost *host : std::as_const(m_hosts)) {
        if (host->phase() == BackgroundSessionHost::Phase::Starting)
            qCWarning(lcSessionPool) << "quit before restore finished, tokens current as of "
                                        "last persist"
                                     << "acct=" << host->identity().slug;
    }
    bool ok = m_retirement->waitForDone(int(std::max<qint64>(0, deadline.remainingTime())));
    // Their GUI halves now (the event loop is blocked): the handback writes
    // are queued from there.
    m_retirement->deliverFinishedNow();
    // Pairs earlier retirements could not save, and any these handbacks
    // park: written through the worker too, inside the same budget, twice at
    // most (§18 round 2, R2-3 and L-a). The foreground's own quit writes none.
    for (int round = 0; round < 2 && !deadline.hasExpired(); ++round) {
        writeParkedForQuit();
        ok = m_keyring->waitForWrites(int(std::max<qint64>(0, deadline.remainingTime()))) && ok;
    }
    if (!ok)
        qCWarning(lcSessionPool) << "background accounts did not all close within the quit "
                                    "budget";
    return ok;
}

bool SessionPool::writeParked(const QString &uid)
{
    if (m_writingParked.contains(uid))
        return true;
    const QList<HostSignInRetirements::UnsavedHandback> parked =
        HostSignInRetirements::process().takeUnsaved(uid);
    if (parked.isEmpty())
        return false;
    // SENSITIVE: the request carries the tokens; never log it.
    const HostSignInRetirements::UnsavedHandback unsaved = parked.first();
    KeyringWorker::WriteRequest request;
    request.userId = uid;
    request.deviceId = unsaved.deviceId;
    request.kind = KeyringWorker::WriteKind::Handback;
    request.tokens = unsaved.tokens;
    request.expectedDigest = unsaved.expectedDigest;
    request.acceptedDigests = unsaved.acceptedDigests;
    m_writingParked.insert(uid);
    m_keyring->write(request, [this, uid, unsaved](const KeyringWorker::WriteResult &result) {
        m_writingParked.remove(uid);
        const bool refused = result.outcome == KeyringWorker::WriteOutcome::Failed
            || result.outcome == KeyringWorker::WriteOutcome::TimedOut;
        qCInfo(lcSessionPool) << "wrote a background account's parked sign-in before starting it"
                              << "acct=" << slugOf(uid)
                              << "written=" << (result.outcome == KeyringWorker::WriteOutcome::Written)
                              << "refused=" << refused;
        // Refused or unanswered: kept, and the account waits for the next
        // attempt rather than starting on the older pair. A removal's cancel
        // drops it with the account, and so does a removal begun meanwhile.
        if (refused && !m_removing.contains(uid))
            HostSignInRetirements::process().parkUnsaved(unsaved);
        schedule();
    });
    return true;
}

void SessionPool::writeParkedForQuit()
{
    // A wedged keyring answers nothing more before the process exits; the
    // pairs stay parked (lost with the process, as any unsaved rotation).
    if (m_keyring->wedged()) {
        if (!HostSignInRetirements::process().unsavedUserIds().isEmpty())
            qCWarning(lcSessionPool) << "the keyring is not answering; parked background "
                                        "sign-ins are not written at quit";
        return;
    }
    for (const QString &uid : HostSignInRetirements::process().unsavedUserIds()) {
        // That account's own write is still inside the store: its parked pair
        // stays parked rather than queue behind a write that may never return.
        if (m_keyring->writeInFlightFor(uid) && !m_writingParked.contains(uid)) {
            qCInfo(lcSessionPool) << "a parked sign-in waits: that account's keyring write "
                                     "is still in flight"
                                  << "acct=" << slugOf(uid);
            continue;
        }
        writeParked(uid);
    }
}

void SessionPool::shutdownAsync(std::function<void()> done)
{
    m_shuttingDown = true;
    m_shutdownDone = std::move(done);
    m_shutdownStartedMs = m_scheduler->now();
    m_admitTimer.stop();
    m_memoryTimer.stop();
    m_lastLiveTimer.stop();
    m_scheduler->stopProbe();
    for (const QString &uid : m_reading.keys()) {
        m_keyring->cancelFor(uid);
        abandonStart(uid, m_reading.value(uid));
    }
    // A runtime off raises nothing more either (L8).
    for (BackgroundSessionHost *host : std::as_const(m_hosts))
        host->setQuiet(true);
    for (const QString &uid : m_hosts.keys())
        retireHost(uid, true);
    m_shutdownPoll.start();
    checkShutdownDrained();
}

void SessionPool::checkShutdownDrained()
{
    if (!m_shuttingDown || !m_shutdownDone)
        return;
    // Starting hosts retire at their login result; bounded, then left.
    bool startingLeft = false;
    // Over a copy of the keys: retireHost() removes from m_hosts (review L7).
    const QStringList uids = m_hosts.keys();
    for (const QString &uid : uids) {
        BackgroundSessionHost *host = m_hosts.value(uid);
        if (!host)
            continue;
        if (host->phase() == BackgroundSessionHost::Phase::Starting)
            startingLeft = true;
        else
            retireHost(uid, true);
    }
    const bool overBudget = m_scheduler->now() - m_shutdownStartedMs >= kShutdownStartingBudgetMs;
    // A host still restoring keeps its Rust handle and its store claim: the
    // pool is not released under it. Its restore is bounded in Rust
    // (labs::BACKGROUND_RESTORE_BUDGET, 60 s, then login_failed), and it then
    // retires here. Releasing it leaked the handle and left the store claimed for
    // the rest of the process (§18 round 1, L2). Said once past the budget.
    if (startingLeft && overBudget && !m_shutdownStartingLogged) {
        m_shutdownStartingLogged = true;
        qCWarning(lcSessionPool) << "a background account is still restoring; live accounts "
                                    "stay up until it finishes, then close";
    }
    // Retirements and their handbacks done; latches need not free (a store
    // that never closes stays busy until restart).
    const bool drained = !startingLeft && m_retirement->activeJobs() == 0
        && m_keyring->pendingJobs() == 0;
    if (!drained)
        return;
    m_shutdownPoll.stop();
    std::function<void()> done = std::exchange(m_shutdownDone, {});
    qCInfo(lcSessionPool) << "live accounts pool shut down"
                          << "starting_left=" << startingLeft;
    if (done)
        done();
}

} // namespace lightning::accounts
