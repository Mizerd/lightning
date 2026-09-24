#include "matrix/RustSdkMatrixClient.h"

#include "matrix/RoomActionError.h"

#include "app/GuiStallTracer.h"

#include <QElapsedTimer>
#include <QThreadPool>
#include "app/SyncLatencyTracer.h"

#include "app/SettingsManager.h"
#include "auth/OAuthCallbackServer.h"
#include "crypto/E2eeDiagnostics.h"
#include "crypto/QrImageProvider.h"
#include "matrix/EventPreview.h"
#include "matrix/RustSessionPolicy.h"
#include "matrix/RustRoomRegistry.h"
#include "matrix/RustTimelineMirror.h"
#include "matrix/RustTimelineIngest.h"
#include "matrix_rust.h"
#include "models/UserLookup.h"
#include "storage/AppDataPaths.h"

#include <QDateTime>
#include <QHash>
#include "app/UrlLauncher.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantList>
#include <QVariantMap>
#include <QLoggingCategory>
#include <QSet>
#include <QTimeZone>
#include <QUrl>

#include <algorithm>

Q_LOGGING_CATEGORY(lcRust, "matrix.rust")

namespace {

QString takeRustString(char *raw)
{
    if (!raw)
        return {};
    QString out = QString::fromUtf8(raw);
    mx_rust_free_cstring(raw);
    return out;
}

// Serializes a formatted-body spec for the FFI. An empty map yields empty
// bytes, which selects the plain markdown path.
QByteArray bodySpecJson(const QVariantMap &spec)
{
    if (spec.isEmpty())
        return {};
    return QJsonDocument(QJsonObject::fromVariantMap(spec))
        .toJson(QJsonDocument::Compact);
}

// The Rust string is already UTF-8 and QJsonDocument parses UTF-8, so skip
// the QString round trip on this hot poll path.
QByteArray takeRustBytes(char *raw)
{
    if (!raw)
        return {};
    QByteArray out(raw);
    mx_rust_free_cstring(raw);
    return out;
}

bool pathExistsOrIsLink(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

QDateTime timestampFromMs(qint64 ms)
{
    if (ms <= 0)
        return {};
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC);
}

TimelineEvent::Type typeFromString(const QString &msgtype)
{
    // Shares the bridge-wide mapping in rowTypeForMsgtype so sync-delivered
    // media rows are typed like live-timeline ones.
    const TimelineEvent::Type type =
        matrix::rust_timeline::rowTypeForMsgtype(msgtype);
    // Unlike the live-timeline ingest, a side-surface summary falls back to a
    // plain message rather than an Unknown row.
    return type == TimelineEvent::Unknown ? TimelineEvent::TextMessage : type;
}

QString previewFor(const TimelineEvent &event)
{
    // Single normalizing choke point: multi-line bodies (poll fallbacks,
    // mention permalinks) must never reach the room list verbatim.
    return matrix::preview::oneLineSummary(event);
}

} // namespace

RustSdkMatrixClient::RustSdkMatrixClient(SettingsManager *settings, QObject *parent)
    : MatrixClient(parent)
    , m_settings(settings)
{
    m_pollTimer.setInterval(100);
    connect(&m_pollTimer, &QTimer::timeout, this, &RustSdkMatrixClient::pollRustEvents);

    qCInfo(lcRust) << "Rust SDK backend loaded:"
                   << rustBackendName()
                   << "version" << rustBackendVersion()
                   << "supports_e2ee=" << rustSupportsE2ee();
}

RustSdkMatrixClient::~RustSdkMatrixClient()
{
    m_pollTimer.stop();
    m_lifecycle.invalidate();
    // Closing mid-discovery or mid-sign-in must not leak the bootstrap handle,
    // which owns a tokio runtime, a crypto store and this attempt's tokens.
    endOAuthAttempt();
    releaseAuthHandle();
    releaseRustHandle();
    // Process exit is the one place waiting is right: tearing down around a
    // half-closed SQLite store can leave it mid-write.
    if (!waitForRustRetirement(kStoreCloseBudgetMs))
        qCWarning(lcRust) << "a retiring Rust client did not close within the"
                          << "budget at shutdown";
}

QString RustSdkMatrixClient::rustBackendName() const
{
    return takeRustString(mx_rust_backend_name());
}

QString RustSdkMatrixClient::rustBackendStatus() const
{
    return takeRustString(mx_rust_status_string());
}

QString RustSdkMatrixClient::rustBackendVersion() const
{
    return takeRustString(mx_rust_version());
}

bool RustSdkMatrixClient::rustSupportsE2ee() const
{
    return mx_rust_supports_e2ee(m_rustHandle) != 0;
}

void RustSdkMatrixClient::setStorePathOverride(const QString &absolutePath)
{
    m_storePathOverride = absolutePath;
}

void RustSdkMatrixClient::setPersistentSessionFile(const QString &absolutePath)
{
    m_sessionFilePath = absolutePath;
    if (!m_rustHandle)
        return;
    const QByteArray path = m_sessionFilePath.toUtf8();
    const QString result = takeRustString(mx_rust_set_session_file(m_rustHandle,
                                                                    path.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                             ? result.mid(7)
                             : result);
    }
}

QString RustSdkMatrixClient::rustStorePath() const
{
    return m_storePath;
}

bool RustSdkMatrixClient::rustStorePathIsOverride() const
{
    return !m_storePathOverride.isEmpty();
}

QString RustSdkMatrixClient::currentDeviceId() const
{
    return m_deviceId;
}

void RustSdkMatrixClient::setState(ConnectionState state)
{
    // A restored session that has never reached its homeserver is Offline, not
    // Syncing. loginSucceeded synchronously ends in startSync() -> Syncing, so
    // an Offline set at login_ok would be overwritten before it is ever
    // rendered. Cleared by the first `room_list_sync_state: running`, the only
    // proof the server was reached.
    if (m_restoredOffline && state == Syncing)
        state = Offline;
    if (m_state == state)
        return;
    m_state = state;
    Q_EMIT connectionStateChanged(m_state);
}

void RustSdkMatrixClient::setInitialSyncDone(bool done)
{
    if (m_initialSyncDone == done)
        return;
    m_initialSyncDone = done;
    Q_EMIT initialSyncDoneChanged();
}

void RustSdkMatrixClient::clearLocalState()
{
    // Remote session descriptions must never outlive their session on any
    // teardown path.
    m_callSdpStore.clear();
    clearTimelineInsertBatch();
    m_loggedIn = false;
    m_restoredOffline = false;
    // Log dedupe is per session, not per process, so a second broken account in
    // the same run still logs. Reset wherever a session ends.
    m_ownIdentityKeyMismatchLogged = false;
    m_homeserver.clear();
    m_userId.clear();
    m_deviceId.clear();
    m_rooms.clear();
    m_roomOrder.clear();
    m_lastReceiptSent.clear();
    m_syncMode = QStringLiteral("stopped");
    m_lastSyncState.clear();
    m_timelines.clear();
    m_pendingSends.clear();
    m_pendingProbes.clear();
    m_timelineTracker.reset();
    m_threadTracker.reset();
    m_pagination.clear();
    m_maxUploadSize = 0;
    m_uploadLimitRequested = false;
    setInitialSyncDone(false);
    Q_EMIT roomsChanged();
    setState(Disconnected);
}

void RustSdkMatrixClient::ensurePollTimer()
{
    if (!m_pollTimer.isActive())
        m_pollTimer.start();
}

QString RustSdkMatrixClient::rustStorePathForUser(const QString &userIdForStore) const
{
    // Test hook: the smoke harness passes a temporary directory so every run
    // starts from a clean crypto store.
    if (!m_storePathOverride.isEmpty())
        return m_storePathOverride;

    return matrix::app_data::rustSdkStorePath(userIdForStore);
}

bool RustSdkMatrixClient::ensureRustHandleForIdentity(
    const matrix::app_data::AccountIdentity &identity)
{
    // Open the recorded location, not one re-derived from the user id; they
    // differ when the typed casing or delegated server name differs from the
    // homeserver's canonical answer.
    if (!m_storePathOverride.isEmpty())
        return ensureRustHandleForStorePath(m_storePathOverride,
                                            QStringLiteral("(override)"));
    if (!identity.isValid())
        return false;
    return ensureRustHandleForStorePath(identity.rustStorePath,
                                        identity.effectiveStoreSlug());
}

bool RustSdkMatrixClient::ensureRustHandleForUser(const QString &userIdForStore)
{
    return ensureRustHandleForStorePath(
        rustStorePathForUser(userIdForStore),
        m_storePathOverride.isEmpty()
            ? matrix::app_data::safeUserSlug(userIdForStore)
            : QStringLiteral("(override)"));
}

bool RustSdkMatrixClient::ensureRustHandleForStorePath(const QString &storePath,
                                                       const QString &slug)
{
    if (storePath.isEmpty())
        return false;
    if (m_storePathOverride.isEmpty() && QFileInfo(storePath).isSymLink()) {
        qCWarning(lcRust) << "refusing symlinked Rust SDK store";
        return false;
    }

    // A handle/event queue is never reused across login generations, which is
    // what makes stale async callbacks unobservable.
    releaseRustHandle();

    // Paths only, never tokens/keys/bodies. Logged before mkpath because the
    // failure message below points at the log instead of naming the path.
    qCInfo(lcRust) << "Rust SDK store path resolved"
                   << "base=" << matrix::app_data::primaryRoot()
                   << "slug=" << slug
                   << "store=" << storePath
                   << "exists=" << QFileInfo::exists(storePath)
                   << "mode=" << (m_storePathOverride.isEmpty()
                                  ? QStringLiteral("persistent")
                                  : QStringLiteral("temporary"));

    // Rust only tightens the leaf directory; mkpath creates parents with the
    // umask's mode, and the account directory name is the Matrix localpart.
    // Tighten every level created here.
    const auto restrictNewParents = [&storePath] {
        QString walked;
        const QStringList parts = storePath.split(QLatin1Char('/'));
        for (const QString &part : parts) {
            if (part.isEmpty()) {
                walked += QLatin1Char('/');
                continue;
            }
            if (!walked.isEmpty() && !walked.endsWith(QLatin1Char('/')))
                walked += QLatin1Char('/');
            walked += part;
            // Only inside our own data root: never touch $HOME or above.
            if (!walked.startsWith(matrix::app_data::primaryRoot()))
                continue;
            QFile::setPermissions(walked,
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                      | QFileDevice::ExeOwner);
        }
    };
    if (!QDir().mkpath(storePath)) {
        // The path stays out of this user-visible message: it contains the
        // localpart and home directory. It is in the log line above.
        Q_EMIT errorOccurred(tr("Lightning could not create its local storage "
                                "directory for this account. Check filesystem "
                                "permissions and free space."));
        return false;
    }
    restrictNewParents();

    const QByteArray path = QFileInfo(storePath).absoluteFilePath().toUtf8();
    m_rustHandle = mx_rust_create(path.constData());
    if (!m_rustHandle) {
        Q_EMIT errorOccurred(tr("Failed to create Rust SDK backend handle."));
        return false;
    }

    m_storePath = storePath;
    m_handleGeneration = m_lifecycle.beginSession();
    // Receipt privacy defaults to public on a fresh bridge; re-apply the user's
    // choice on every account switch.
    if (m_readReceiptPrivacy != 0)
        takeRustString(mx_rust_set_receipt_privacy(m_rustHandle,
                                                   m_readReceiptPrivacy));
    // The Rust-side flag defaults off on a fresh handle; a registered media
    // backend must survive account switches.
    if (m_callMediaCapable)
        // The FFI returns an owned char*; discarding it would leak.
        takeRustString(mx_rust_calls_set_media_capable(
            m_rustHandle, static_cast<unsigned char>(1)));

    if (!m_sessionFilePath.isEmpty()) {
        const QByteArray sessionPath = m_sessionFilePath.toUtf8();
        const QString result = takeRustString(mx_rust_set_session_file(m_rustHandle,
                                                                        sessionPath.constData()));
        if (!result.isEmpty()) {
            mx_rust_destroy(m_rustHandle);
            m_rustHandle = nullptr;
            m_handleGeneration = 0;
            Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
            return false;
        }
    }

    ensurePollTimer();
    return true;
}

// Deliberately does not clear m_freshLoginIdentity: login() arms it and then
// reaches here. The marker is compared against the attempt's account in the
// login_failed handler and cleared in detachSession().
namespace {
// Threads that close retired Rust clients. Retirement is I/O-bound; two
// threads suffice and rapid switches queue rather than spawning a thread each.
QThreadPool &rustRetirementPool()
{
    static QThreadPool *pool = [] {
        auto *p = new QThreadPool;
        p->setMaxThreadCount(2);
        // Never let an idle worker be reaped mid-teardown.
        p->setExpiryTimeout(-1);
        return p;
    }();
    return *pool;
}
} // namespace

void RustSdkMatrixClient::retireRustHandleAsync(void *handle,
                                                const QString &typingRoom)
{
    if (!handle)
        return;
    rustRetirementPool().start([handle, typingRoom] {
        QElapsedTimer teardown;
        teardown.start();
        // The courtesy "stopped typing" is a network send, so it belongs here
        // and off the caller's critical path.
        if (!typingRoom.isEmpty()) {
            const QByteArray room = typingRoom.toUtf8();
            takeRustString(mx_rust_send_typing(handle, room.constData(), 0));
        }
        const QString shutdown = takeRustString(mx_rust_shutdown_tasks(handle));
        const qint64 shutdownMs = teardown.elapsed();
        teardown.restart();
        // Drops the tokio runtime, blocking until every in-flight
        // spawn_blocking (including SQLite closes) finishes.
        mx_rust_destroy(handle);
        const qint64 destroyMs = teardown.elapsed();
        qCInfo(lcRust) << "rust client retired off the GUI thread"
                       << shutdown
                       << "shutdown_ms=" << shutdownMs
                       << "destroy_ms=" << destroyMs
                       << "teardown_total_ms=" << (shutdownMs + destroyMs);
    });
}

bool RustSdkMatrixClient::waitForRustRetirement(int budgetMs)
{
    return rustRetirementPool().waitForDone(budgetMs);
}

// The GUI thread does not wait for retirement. Task shutdown and runtime
// drop block until SQLite closes, so they run on a worker that owns the handle
// from here on; this thread only stops polling, drops trackers and detaches
// the pointer.
//
// This is safe only because Rust never calls back into C++: events are pulled
// by pollRustEvents() on a timer, so a retiring client has no route to any
// QObject. Callers about to delete the store must call
// waitForRustRetirement() first.
void RustSdkMatrixClient::releaseRustHandle()
{
    m_pollTimer.stop();
    clearTimelineInsertBatch();
    if (!m_rustHandle)
        return;

    void *retiring = m_rustHandle;
    const QString typingRoom = m_typingRoom;
    m_typingRoom.clear();
    // Detached before the hand-off so nothing on this thread can reach the
    // client the worker now owns.
    m_rustHandle = nullptr;
    m_handleGeneration = 0;
    m_storePath.clear();
    m_timelineTracker.reset();
    m_threadTracker.reset();
    m_pagination.clear();

    retireRustHandleAsync(retiring, typingRoom);
}

void RustSdkMatrixClient::login(const QString &homeserver,
                                const QString &user,
                                const QString &password)
{
    // Report the specific reason resolveAccountIdentity refused rather than one
    // generic "fields are required". The missing-scheme case gets an example,
    // since "invalid URL" does not tell anyone to add https://.
    matrix::app_data::AccountIdentity identity;
    QString why;
    const bool resolved =
        matrix::app_data::resolveAccountIdentity(homeserver, user, &identity,
                                                 &why);
    if (!resolved || password.isEmpty()) {
        // Split the two empty-input cases the resolver folds together.
        QString message;
        if (!resolved && homeserver.trimmed().isEmpty()) {
            message = tr("Enter your homeserver, for example "
                         "https://matrix.org");
        } else if (!resolved && user.trimmed().isEmpty()) {
            message = tr("Enter your username.");
        } else if (!resolved
                   && why == QLatin1String("invalid homeserver or empty user")) {
            // Both fields are set and the resolver still refused: in practice a
            // missing scheme.
            message = tr("That homeserver address is not a full URL. Include "
                         "https://, for example https://matrix.org");
        } else if (!resolved) {
            message = tr("That username is not a valid Matrix id. Use your "
                         "username, or the full @you:server form.");
        } else {
            message = tr("Enter your password.");
        }
        Q_EMIT loginFailed(message);
        return;
    }

    // Localparts are case-sensitive, so the typed casing is kept, but the saved
    // record uses the server's canonical id. Find the account whose store this
    // login should open: the canonical id when the typed casing maps onto one,
    // else the typed id if it is itself saved.
    QString loginOwnerUserId;
    if (m_settings) {
        bool ambiguous = false;
        const QString canonical =
            m_settings->canonicalUserIdForTypedIdentity(identity.userId,
                                                        &ambiguous);
        if (ambiguous) {
            qCWarning(lcRust) << "login refused: several saved accounts differ "
                                 "only by localpart case";
            Q_EMIT loginFailed(matrix::rust_session::userMessage(
                matrix::rust_session::StoreBlockReason::AmbiguousStoreCandidates));
            return;
        }
        // The match locates the store only; it must not rewrite the identity
        // sent to the homeserver, since @alice and @Alice can be different
        // people. The server's answer settles the mapping in login_ok ->
        // recordStoreLocation.
        matrix::app_data::AccountIdentity savedIdentity;
        if (!canonical.isEmpty()
            && m_settings->resolveSavedIdentity(canonical, &savedIdentity)) {
            if (canonical != identity.userId) {
                qCInfo(lcRust) << "login store located from saved account"
                               << "typed_slug=" << identity.slug
                               << "store_slug=" << savedIdentity.effectiveStoreSlug();
            }
            loginOwnerUserId = canonical;
            matrix::app_data::bindStoreSlug(&identity,
                                            savedIdentity.effectiveStoreSlug());
        } else {
            if (m_settings->hasSavedAccount(identity.userId))
                loginOwnerUserId = identity.userId;
            // Accounts from older builds may keep their store under a divergent
            // directory; open the recorded one.
            matrix::app_data::bindStoreSlug(
                &identity, m_settings->storeSlugFor(identity.userId));
        }
    }
    m_openingIdentity = identity;

    // Slug flattening is not injective: refuse an identity that collides with a
    // different saved account before contacting the server.
    if (m_settings && m_settings->accountSlugConflicts(identity.userId)) {
        qCWarning(lcRust) << "login refused: account slug collision"
                          << "slug=" << identity.slug;
        Q_EMIT loginFailed(tr(
            "This account's local storage name collides with a different "
            "account already saved on this device. Remove that account "
            "first if you want to sign in with this one."));
        return;
    }

    // Adoption must also run on the login path. Otherwise an account whose
    // store sits under a divergent slug (typed casing, .well-known delegation)
    // and has no readable session reaches here, finds nothing at the canonical
    // path, and creates a new empty store, abandoning its Megolm keys.
    if (!pathExistsOrIsLink(identity.rustStorePath) && m_settings
        && !loginOwnerUserId.isEmpty()) {
        // Adopt against the saved identity, not the typed one:
        //  * hasSavedAccount() is an exact match, and a record saved as
        //    "@mizerd:…" is not found by the typed "@Mizerd:…".
        //  * The candidate scan excludes the identity's own slug, which for the
        //    typed identity is exactly the divergent directory holding the
        //    store.
        // The resulting store slug is bound onto the typed identity, which
        // still goes to the server unchanged.
        matrix::app_data::AccountIdentity adoptTarget;
        if (m_settings->resolveSavedIdentity(loginOwnerUserId, &adoptTarget)) {
            auto adoptionRefusal = matrix::rust_session::StoreBlockReason::None;
            if (adoptDivergentStoreIfUnambiguous(&adoptTarget, &adoptionRefusal))
                matrix::app_data::bindStoreSlug(&identity,
                                                adoptTarget.effectiveStoreSlug());
            if (adoptionRefusal != matrix::rust_session::StoreBlockReason::None) {
                // Contested ownership: refuse rather than guess. Nothing was
                // moved or deleted.
                failWithBlockReason(adoptionRefusal, identity);
                return;
            }
        }
    }

    bool storeExists = pathExistsOrIsLink(identity.rustStorePath);
    // Only the target account's own record is consulted; other signed-in
    // accounts never block a login.
    //
    // Record existence and token readability are separate: an unreadable secret
    // store (locked keyring, no session bus) must never look like "no account"
    // and send a real store to orphan cleanup. The owner is resolved from the
    // store directory, which also covers recorded and delegated slugs.
    const QString storeOwner = m_settings
        ? m_settings->accountOwningStoreSlug(identity.effectiveStoreSlug())
        : QString{};
    const QString recordUserId =
        storeOwner.isEmpty() ? identity.userId : storeOwner;
    const bool targetHasRecord =
        m_settings && m_settings->hasSavedAccount(recordUserId);
    const bool targetTokenReadable = targetHasRecord
        && !m_settings->accessTokenFor(recordUserId).isEmpty();
    const bool targetHasSavedSession = targetHasRecord && targetTokenReadable;

    // An orphaned store (exists, but no record could ever restore it) usually
    // comes from an earlier failed login, since the directory is created before
    // the server accepts the password. Clean it up rather than dead-ending the
    // user on the reset prompt.
    if (storeExists && !targetHasRecord) {
        // Moved aside, never deleted: the store may hold the only copy of
        // someone's room keys.
        const QString quarantined =
            matrix::app_data::quarantineRustStore(identity);
        qCInfo(lcRust) << "quarantined unclaimed store before login"
                       << "slug=" << identity.effectiveStoreSlug()
                       << "moved=" << !quarantined.isEmpty();
        storeExists = pathExistsOrIsLink(identity.rustStorePath);
        if (storeExists) {
            setState(Error);
            Q_EMIT loginFailed(tr(
                "An unusable local store for this account could not be moved "
                "aside. Check filesystem permissions and try again."));
            return;
        }
    }
    // Remember fresh-store attempts so a failure can clean up after itself.
    m_freshLoginIdentity = storeExists ? matrix::app_data::AccountIdentity{}
                                       : identity;
    // A record and store exist but the secret backend cannot be read. The
    // session may be intact, so this must not be classified as "store with no
    // session metadata" and routed to a destructive repair. The condition lives
    // in RustSessionPolicy, where it is pure and tested.
    if (matrix::rust_session::unreadableSecretBlocksLogin(
            storeExists, targetHasRecord, targetTokenReadable,
            m_settings->secretBackendUnavailable(),
            m_settings->secretMissesAreInconclusive())) {
        failWithBlockReason(
            matrix::rust_session::StoreBlockReason::SecretBackendUnavailable,
            identity);
        return;
    }

    const QString targetSavedDeviceId = m_settings
        ? m_settings->accountRecord(recordUserId)
              .value(QStringLiteral("deviceId")).toString()
        : QString{};
    const auto block = matrix::rust_session::passwordLoginBlockReason(
        identity, storeExists, targetHasSavedSession, targetSavedDeviceId);
    if (block == matrix::rust_session::StoreBlockReason::ExistingStoreNeedsRestore
        && targetHasSavedSession) {
        // Not an error state: the account is already usable on this device.
        qCInfo(lcRust) << "login redirected to switch"
                       << "slug=" << identity.slug;
        Q_EMIT loginFailed(matrix::rust_session::userMessage(block));
        return;
    }
    if (block != matrix::rust_session::StoreBlockReason::None) {
        failWithBlockReason(block, identity);
        return;
    }

    if (!ensureRustHandleForIdentity(identity)) {
        // This attempt never started, so nothing may remain marked as its fresh
        // store; the marker is armed only while an attempt is in flight.
        m_freshLoginIdentity = {};
        setState(Error);
        Q_EMIT loginFailed(tr("Rust SDK backend could not be initialized."));
        return;
    }

    m_homeserver = identity.homeserver;
    m_userId.clear();
    m_deviceId.clear();
    m_loggedIn = false;
    // A new attempt: reset what the previous one needed to open its store.
    m_restoredOffline = false;
    // Log dedupe is per session, not per process, so a second broken account in
    // the same run still logs. Reset wherever a session ends.
    m_ownIdentityKeyMismatchLogged = false;
    m_rooms.clear();
    m_roomOrder.clear();
    m_timelines.clear();
    m_pendingSends.clear();
    setInitialSyncDone(false);
    Q_EMIT roomsChanged();
    setState(Connecting);

    const QByteArray hsBytes = identity.homeserver.toUtf8();
    const QByteArray userBytes = identity.userId.toUtf8();
    // Convert once, then scrub the transit buffer, as the recovery-key and
    // passphrase paths do. The QString belongs to the caller, which clears it.
    QByteArray passwordBytes = password.toUtf8();
    const QString result = takeRustString(mx_rust_login(m_rustHandle,
                                                        hsBytes.constData(),
                                                        userBytes.constData(),
                                                        passwordBytes.constData()));
    // volatile so the dead-store optimizer cannot drop the zeroing.
    volatile char *raw = passwordBytes.data();
    for (int i = 0; i < passwordBytes.size(); ++i)
        raw[i] = 0;
    if (!result.isEmpty()) {
        setState(Error);
        Q_EMIT loginFailed(result.startsWith(QLatin1String("error: "))
                           ? result.mid(7)
                           : result);
    }
}

bool RustSdkMatrixClient::detachSession()
{
    // Named scope so the stall tracer attributes the account switch; everything
    // reached here, including the Rust teardown, is synchronous on the GUI
    // thread.
    stalltrace::Scope stallScope("account-detach");
    // A sign-out is in flight and its completion is the only path that deletes
    // this account's token, record and store. Invalidating now would drop that
    // completion and silently downgrade it to a local detach, so refuse.
    if (m_lifecycle.signingOut()) {
        qCWarning(lcRust) << "detach refused: sign-out still in flight";
        return false;
    }
    // Account switch: end the local session without a server logout. The store,
    // token and account record stay; restoreSession() reactivates them later.
    qCInfo(lcRust) << "detaching local session"
                   << "slug=" << matrix::app_data::safeUserSlug(m_userId);
    // A detach abandons any running login attempt and drops its callbacks as
    // stale, so its fresh-store marker must go with it.
    m_freshLoginIdentity = {};
    m_callSdpStore.clear();
    // Stale callbacks from this session become unobservable immediately;
    // releaseRustHandle() then retires the handle.
    m_lifecycle.invalidate();
    releaseRustHandle();
    clearLocalState();
    Q_EMIT loggedOut();
    return true;
}

// --- OAuth 2.0 / OIDC ------------------------------------------------------
//
// Phase A runs on m_authHandle, which has no store (see rust/src/oauth.rs).
// Phase B, completeOAuthLogin(), is the only place that opens a store, after
// the homeserver has named the account and device.

bool RustSdkMatrixClient::ensureOAuthBootstrapHandle()
{
    releaseAuthHandle();
    m_authHandle = mx_rust_oauth_bootstrap_create();
    if (!m_authHandle) {
        Q_EMIT errorOccurred(tr("Failed to create Rust SDK backend handle."));
        return false;
    }
    // Signing in from the login screen: there is no session handle, so the poll
    // timer may be stopped.
    if (!m_pollTimer.isActive())
        m_pollTimer.start();
    return true;
}

void RustSdkMatrixClient::releaseAuthHandle()
{
    if (!m_authHandle)
        return;
    mx_rust_destroy(m_authHandle);
    m_authHandle = nullptr;
}

void RustSdkMatrixClient::endOAuthAttempt()
{
    m_oauthInFlight = false;
    m_oauthHomeserver.clear();
    if (m_oauthCallback) {
        m_oauthCallback->stop();
        m_oauthCallback->deleteLater();
        m_oauthCallback = nullptr;
    }
}

void RustSdkMatrixClient::discoverAuthMethods(const QString &homeserver)
{
    // Discovery and sign-in share the bootstrap handle, and
    // ensureOAuthBootstrapHandle() destroys whatever is there. Re-probing
    // mid-sign-in (e.g. editing the homeserver field) would discard the client
    // holding this attempt's PKCE verifier/CSRF state or single-use SSO token
    // context. The running attempt wins; the user can cancel it.
    if (m_oauthInFlight || m_ssoInFlight)
        return;

    const QString hs = homeserver.trimmed();
    if (hs.isEmpty()) {
        Q_EMIT authMethodsDiscovered(hs, false, false, false);
        return;
    }
    if (!ensureOAuthBootstrapHandle()) {
        Q_EMIT authMethodsDiscovered(hs, false, false, false);
        return;
    }
    const QByteArray hsBytes = hs.toUtf8();
    const QString result =
        takeRustString(mx_rust_oauth_discover(m_authHandle, hsBytes.constData()));
    if (!result.isEmpty()) {
        // Discovery could not even start: report "nothing known" rather than
        // guessing that password login works.
        Q_EMIT authMethodsDiscovered(hs, false, false, false);
    }
}

void RustSdkMatrixClient::beginOAuthLogin(const QString &homeserver)
{
    const QString hs = homeserver.trimmed();
    if (hs.isEmpty()) {
        Q_EMIT loginFailed(tr("A homeserver is required."));
        return;
    }
    if (m_oauthInFlight) {
        // A second attempt must not race the first; the user can cancel.
        return;
    }

    if (!ensureOAuthBootstrapHandle()) {
        Q_EMIT loginFailed(tr("Rust SDK backend could not be initialized."));
        return;
    }

    m_oauthCallback = new OAuthCallbackServer(this);
    if (!m_oauthCallback->listen()) {
        endOAuthAttempt();
        releaseAuthHandle();
        Q_EMIT loginFailed(tr("Lightning could not open a local port to receive "
                              "the sign-in response. Check whether a firewall is "
                              "blocking loopback connections."));
        return;
    }

    connect(m_oauthCallback, &OAuthCallbackServer::callbackReceived,
            this, [this](const QString &redirectUrl) {
        // SENSITIVE: redirectUrl carries the authorization code. It goes
        // straight to the SDK and is never logged or shown.
        if (!m_oauthInFlight || !m_authHandle)
            return;
        const QByteArray cb = redirectUrl.toUtf8();
        const QString result =
            takeRustString(mx_rust_oauth_finish(m_authHandle, cb.constData()));
        if (!result.isEmpty()) {
            endOAuthAttempt();
            releaseAuthHandle();
            Q_EMIT loginFailed(tr("The sign-in could not be completed."));
        }
    });

    connect(m_oauthCallback, &OAuthCallbackServer::callbackFailed,
            this, [this](const QString &error) {
        if (!m_oauthInFlight)
            return;
        if (m_authHandle)
            takeRustString(mx_rust_oauth_abort(m_authHandle));
        endOAuthAttempt();
        releaseAuthHandle();
        // access_denied is the ordinary "user said no", not a fault.
        Q_EMIT loginFailed(error == QLatin1String("access_denied")
                               ? tr("Sign-in was cancelled.")
                               : tr("The server refused the sign-in request."));
    });

    connect(m_oauthCallback, &OAuthCallbackServer::timedOut, this, [this] {
        if (!m_oauthInFlight)
            return;
        if (m_authHandle)
            takeRustString(mx_rust_oauth_abort(m_authHandle));
        endOAuthAttempt();
        releaseAuthHandle();
        Q_EMIT loginFailed(tr("The sign-in timed out. Please try again."));
    });

    m_oauthInFlight = true;
    m_oauthHomeserver = hs;

    const QByteArray hsBytes = hs.toUtf8();
    const QByteArray redirectBytes = m_oauthCallback->redirectUri().toUtf8();
    const QString result = takeRustString(
        mx_rust_oauth_begin(m_authHandle, hsBytes.constData(), redirectBytes.constData()));
    if (!result.isEmpty()) {
        endOAuthAttempt();
        releaseAuthHandle();
        Q_EMIT loginFailed(result.startsWith(QLatin1String("error: ")) ? result.mid(7)
                                                                      : result);
    }
}

void RustSdkMatrixClient::cancelOAuthLogin()
{
    if (!m_oauthInFlight)
        return;
    if (m_authHandle)
        takeRustString(mx_rust_oauth_abort(m_authHandle));
    endOAuthAttempt();
    releaseAuthHandle();
    // A resolved state, not silence: the UI must leave "Signing in".
    Q_EMIT loginFailed(tr("Sign-in was cancelled."));
}

void RustSdkMatrixClient::requestSsoProviders(const QString &homeserver)
{
    const QString hs = homeserver.trimmed();
    if (hs.isEmpty() || !ensureOAuthBootstrapHandle())
        return;
    const QByteArray hsBytes = hs.toUtf8();
    takeRustString(mx_rust_sso_providers(m_authHandle, hsBytes.constData()));
}

void RustSdkMatrixClient::beginSsoLogin(const QString &homeserver,
                                        const QString &idpId)
{
    const QString hs = homeserver.trimmed();
    if (hs.isEmpty()) {
        Q_EMIT loginFailed(tr("A homeserver is required."));
        return;
    }
    // One browser sign-in at a time: SSO and OAuth would fight over the same
    // bootstrap handle and client slot.
    if (m_ssoInFlight || m_oauthInFlight)
        return;

    if (!ensureOAuthBootstrapHandle()) {
        Q_EMIT loginFailed(tr("Rust SDK backend could not be initialized."));
        return;
    }

    m_ssoCallback = new OAuthCallbackServer(this);
    m_ssoCallback->setFlow(OAuthCallbackServer::Flow::Sso);
    if (!m_ssoCallback->listen()) {
        endSsoAttempt();
        releaseAuthHandle();
        Q_EMIT loginFailed(tr("Lightning could not open a local port to receive "
                              "the sign-in response. Check whether a firewall is "
                              "blocking loopback connections."));
        return;
    }

    connect(m_ssoCallback, &OAuthCallbackServer::callbackReceived,
            this, [this](const QString &loginToken) {
        // SENSITIVE: single-use login token. Never logged, shown or given to
        // QML. The in-flight guard makes a stale callback from a cancelled or
        // timed-out attempt inert.
        if (!m_ssoInFlight || !m_authHandle)
            return;
        const QByteArray tokenBytes = loginToken.toUtf8();
        const QString result =
            takeRustString(mx_rust_sso_finish(m_authHandle, tokenBytes.constData()));
        if (!result.isEmpty()) {
            endSsoAttempt();
            releaseAuthHandle();
            Q_EMIT loginFailed(tr("The sign-in could not be completed."));
        }
    });

    connect(m_ssoCallback, &OAuthCallbackServer::callbackFailed,
            this, [this](const QString &error) {
        if (!m_ssoInFlight)
            return;
        if (m_authHandle)
            takeRustString(mx_rust_sso_abort(m_authHandle));
        endSsoAttempt();
        releaseAuthHandle();
        Q_EMIT loginFailed(error == QLatin1String("access_denied")
                               ? tr("Sign-in was cancelled.")
                               : tr("The sign-in response was incomplete. "
                                    "Please try again."));
    });

    connect(m_ssoCallback, &OAuthCallbackServer::timedOut, this, [this] {
        if (!m_ssoInFlight)
            return;
        if (m_authHandle)
            takeRustString(mx_rust_sso_abort(m_authHandle));
        endSsoAttempt();
        releaseAuthHandle();
        Q_EMIT loginFailed(tr("The sign-in timed out. Please try again."));
    });

    m_ssoInFlight = true;
    m_ssoHomeserver = hs;

    const QByteArray hsBytes = hs.toUtf8();
    const QByteArray redirectBytes = m_ssoCallback->redirectUri().toUtf8();
    const QByteArray idpBytes = idpId.trimmed().toUtf8();
    const QString result = takeRustString(mx_rust_sso_begin(m_authHandle,
                                                            hsBytes.constData(),
                                                            redirectBytes.constData(),
                                                            idpBytes.constData()));
    if (!result.isEmpty()) {
        endSsoAttempt();
        releaseAuthHandle();
        Q_EMIT loginFailed(result.startsWith(QLatin1String("error: ")) ? result.mid(7)
                                                                       : result);
    }
}

void RustSdkMatrixClient::cancelSsoLogin()
{
    if (!m_ssoInFlight)
        return;
    if (m_authHandle)
        takeRustString(mx_rust_sso_abort(m_authHandle));
    endSsoAttempt();
    releaseAuthHandle();
    // A resolved state, never silence: the UI must leave "Signing in".
    Q_EMIT loginFailed(tr("Sign-in was cancelled."));
}

void RustSdkMatrixClient::endSsoAttempt()
{
    m_ssoInFlight = false;
    m_ssoHomeserver.clear();
    if (m_ssoCallback) {
        m_ssoCallback->stop();
        m_ssoCallback->deleteLater();
        m_ssoCallback = nullptr;
    }
}

void RustSdkMatrixClient::drainAuthEvents()
{
    if (!m_authHandle)
        return;

    for (int i = 0; i < 64; ++i) {
        const QByteArray raw = takeRustBytes(mx_rust_poll_event(m_authHandle));
        if (raw.isEmpty())
            break;
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isObject())
            continue;
        const QJsonObject event = doc.object();
        const QString type = event.value(QStringLiteral("type")).toString();

        if (type == QLatin1String("auth_discovery")) {
            Q_EMIT authMethodsDiscovered(
                event.value(QStringLiteral("homeserver")).toString(),
                event.value(QStringLiteral("password")).toBool(),
                event.value(QStringLiteral("oauth")).toBool(),
                event.value(QStringLiteral("sso")).toBool());
            // Discovery is one-shot; drop the handle unless a sign-in is using
            // it.
            if (!m_oauthInFlight)
                releaseAuthHandle();
            continue;
        }

        if (type == QLatin1String("oauth_url")) {
            const QString url = event.value(QStringLiteral("url")).toString();
            if (url.isEmpty())
                continue;
            // Open the browser here so the flow works even if the UI ignores
            // the signal. A failed launch is reported, not fatal: timeout and
            // Cancel stay available. HTTPS only: the URL comes from
            // server-controlled discovery and openExternally would hand any
            // scheme to a registered application.
            const QUrl launch(url);
            if (launch.scheme() != QLatin1String("https")
                || launch.host().isEmpty()) {
                qCWarning(lcRust) << "refusing a non-https sign-in URL from "
                                     "homeserver discovery";
                Q_EMIT browserLaunchFailed();
            } else if (!lightning::urls::openExternally(launch)) {
                Q_EMIT browserLaunchFailed();
            }
            Q_EMIT oauthBrowserUrlReady(url);
            continue;
        }

        if (type == QLatin1String("oauth_ok")) {
            // SENSITIVE: carries access and refresh tokens. Never log `event`.
            const QString userId = event.value(QStringLiteral("user_id")).toString();
            const QString deviceId = event.value(QStringLiteral("device_id")).toString();
            const QString clientId = event.value(QStringLiteral("client_id")).toString();
            const QString accessToken = event.value(QStringLiteral("access_token")).toString();
            const QString refreshToken = event.value(QStringLiteral("refresh_token")).toString();
            completeOAuthLogin(userId, deviceId, clientId, accessToken, refreshToken);
            continue;
        }

        if (type == QLatin1String("oauth_registration_retry")) {
            // The server rejected our client metadata and we re-register
            // without the loopback port (RFC 8252 §7.3). Fixed reason token
            // only; no URI, port, nonce or token. Distinguishes never retried /
            // retried OK / refused again.
            qCInfo(lcRust) << "oauth client registration retried without the "
                              "loopback port reason="
                           << event.value(QStringLiteral("reason")).toString();
            continue;
        }

        if (type == QLatin1String("oauth_failed")) {
            const QString message = event.value(QStringLiteral("message")).toString();
            endOAuthAttempt();
            releaseAuthHandle();
            Q_EMIT loginFailed(message.isEmpty()
                                   ? tr("The sign-in could not be completed.")
                                   : message);
            continue;
        }

        if (type == QLatin1String("sso_providers")) {
            const QJsonArray rows = event.value(QStringLiteral("providers")).toArray();
            QVariantList providers;
            providers.reserve(rows.size());
            for (const QJsonValue &value : rows) {
                const QJsonObject row = value.toObject();
                QVariantMap entry;
                entry.insert(QStringLiteral("id"),
                             row.value(QStringLiteral("id")).toString());
                entry.insert(QStringLiteral("name"),
                             row.value(QStringLiteral("name")).toString());
                entry.insert(QStringLiteral("icon"),
                             row.value(QStringLiteral("icon")).toString());
                providers.append(entry);
            }
            Q_EMIT ssoProvidersReceived(
                event.value(QStringLiteral("homeserver")).toString(),
                event.value(QStringLiteral("sso")).toBool(), providers);
            // The bootstrap handle was taken only for this question; release it
            // if no sign-in is running.
            if (!m_ssoInFlight && !m_oauthInFlight)
                releaseAuthHandle();
            continue;
        }

        if (type == QLatin1String("sso_url")) {
            const QString url = event.value(QStringLiteral("url")).toString();
            if (url.isEmpty())
                continue;
            // HTTPS only: the URL comes from server-controlled discovery and
            // openExternally would hand any scheme to a registered application.
            const QUrl launch(url);
            if (launch.scheme() != QLatin1String("https")
                || launch.host().isEmpty()) {
                qCWarning(lcRust) << "refusing a non-https sign-in URL from "
                                     "homeserver discovery";
                Q_EMIT browserLaunchFailed();
            } else if (!lightning::urls::openExternally(launch)) {
                Q_EMIT browserLaunchFailed();
            }
            Q_EMIT ssoBrowserUrlReady(url);
            continue;
        }

        if (type == QLatin1String("sso_ok")) {
            // SENSITIVE: carries access and refresh tokens. Never log `event`.
            completeSsoLogin(
                event.value(QStringLiteral("user_id")).toString(),
                event.value(QStringLiteral("device_id")).toString(),
                event.value(QStringLiteral("access_token")).toString(),
                event.value(QStringLiteral("refresh_token")).toString());
            continue;
        }

        if (type == QLatin1String("sso_failed")) {
            const QString message = event.value(QStringLiteral("message")).toString();
            endSsoAttempt();
            releaseAuthHandle();
            Q_EMIT loginFailed(message.isEmpty()
                                   ? tr("The sign-in could not be completed.")
                                   : message);
            continue;
        }
    }
}

void RustSdkMatrixClient::completeOAuthLogin(const QString &userId,
                                             const QString &deviceId,
                                             const QString &clientId,
                                             const QString &accessToken,
                                             const QString &refreshToken)
{
    const QString homeserver = m_oauthHomeserver;
    // Phase A is over either way: release the store-less handle (and the tokens
    // it holds) before the checks below can refuse.
    endOAuthAttempt();
    releaseAuthHandle();

    adoptBrowserSession(
        homeserver, userId, deviceId, clientId, accessToken, refreshToken,
        QStringLiteral("oauth"),
        [this, clientId, accessToken, refreshToken](
            const matrix::app_data::AccountIdentity &identity,
            const QString &deviceId) {
            const QByteArray hsBytes = identity.homeserver.toUtf8();
            const QByteArray userBytes = identity.userId.toUtf8();
            const QByteArray deviceBytes = deviceId.toUtf8();
            const QByteArray clientBytes = clientId.toUtf8();
            const QByteArray tokenBytes = accessToken.toUtf8();
            const QByteArray refreshBytes = refreshToken.toUtf8();
            return takeRustString(mx_rust_oauth_restore(m_rustHandle,
                                                        hsBytes.constData(),
                                                        userBytes.constData(),
                                                        deviceBytes.constData(),
                                                        clientBytes.constData(),
                                                        tokenBytes.constData(),
                                                        refreshBytes.constData()));
        });
}

// Legacy SSO Phase B. Same account/store handling as adoptBrowserSession;
// only the persisted auth type and restore call differ, since an SSO session
// restores through matrix_auth().
void RustSdkMatrixClient::completeSsoLogin(const QString &userId,
                                           const QString &deviceId,
                                           const QString &accessToken,
                                           const QString &refreshToken)
{
    const QString homeserver = m_ssoHomeserver;
    endSsoAttempt();
    releaseAuthHandle();

    adoptBrowserSession(
        homeserver, userId, deviceId, QString(), accessToken, refreshToken,
        QStringLiteral("sso"),
        [this, accessToken, refreshToken](
            const matrix::app_data::AccountIdentity &identity,
            const QString &deviceId) {
            const QByteArray hsBytes = identity.homeserver.toUtf8();
            const QByteArray userBytes = identity.userId.toUtf8();
            const QByteArray deviceBytes = deviceId.toUtf8();
            const QByteArray tokenBytes = accessToken.toUtf8();
            const QByteArray refreshBytes = refreshToken.toUtf8();
            return takeRustString(mx_rust_restore(m_rustHandle,
                                                         hsBytes.constData(),
                                                         userBytes.constData(),
                                                         deviceBytes.constData(),
                                                         tokenBytes.constData(),
                                                         refreshBytes.constData()));
        });
}

void RustSdkMatrixClient::adoptBrowserSession(
    const QString &homeserver,
    const QString &userId,
    const QString &deviceId,
    const QString &clientId,
    const QString &accessToken,
    const QString &refreshToken,
    const QString &authType,
    const std::function<QString(const matrix::app_data::AccountIdentity &,
                                const QString &)> &restore)
{
    // Phase B: the homeserver has named the account, so a store can be chosen.

    if (userId.isEmpty() || deviceId.isEmpty() || accessToken.isEmpty()) {
        Q_EMIT loginFailed(tr("The server completed sign-in without returning a "
                              "usable session."));
        return;
    }

    matrix::app_data::AccountIdentity identity;
    if (!matrix::app_data::resolveAccountIdentity(homeserver, userId, &identity)) {
        Q_EMIT loginFailed(matrix::rust_session::userMessage(
            matrix::rust_session::StoreBlockReason::InvalidSavedIdentity));
        return;
    }

    // Use the store this account is recorded to use, as the password path does.
    QString savedDeviceId;
    bool hasSavedSession = false;
    if (m_settings) {
        matrix::app_data::AccountIdentity savedIdentity;
        if (m_settings->resolveSavedIdentity(identity.userId, &savedIdentity)) {
            matrix::app_data::bindStoreSlug(&identity,
                                            savedIdentity.effectiveStoreSlug());
        }
        hasSavedSession = m_settings->hasSavedAccount(identity.userId);
        savedDeviceId = m_settings->accountRecord(identity.userId)
                            .value(QStringLiteral("deviceId"))
                            .toString();
    }

    const QString storePath = identity.rustStorePath;
    const bool storeExists = QFileInfo::exists(storePath);

    // A device the authorization server just created must never be attached to
    // a store belonging to a different device.
    const auto reason = matrix::rust_session::oauthLoginBlockReason(
        identity, storeExists, hasSavedSession, savedDeviceId, deviceId);
    if (reason != matrix::rust_session::StoreBlockReason::None) {
        qCWarning(lcRust) << "Browser sign-in refused"
                          << "authType=" << authType
                          << "slug=" << identity.effectiveStoreSlug()
                          << "reason=" << matrix::rust_session::diagnosticName(reason);
        Q_EMIT loginFailed(matrix::rust_session::userMessage(reason));
        return;
    }

    // Must be set before anything can emit login_failed: the failure handler
    // keys its store-slug rewrite and local-reset prompt on m_openingIdentity,
    // and a stale value would target a different account's store.
    m_openingIdentity = identity;

    if (!ensureRustHandleForIdentity(identity)) {
        setState(Error);
        Q_EMIT loginFailed(tr("Rust SDK backend could not be initialized."));
        return;
    }

    m_homeserver = identity.homeserver;
    m_userId = identity.userId;
    m_deviceId = deviceId;
    m_loggedIn = false;
    // A new attempt: reset what the previous one needed to open its store.
    m_restoredOffline = false;
    // Log dedupe is per session, not per process, so a second broken account in
    // the same run still logs. Reset wherever a session ends.
    m_ownIdentityKeyMismatchLogged = false;
    m_rooms.clear();
    m_roomOrder.clear();
    m_timelines.clear();
    m_pendingSends.clear();
    setInitialSyncDone(false);
    Q_EMIT roomsChanged();
    setState(Connecting);

    // Record the session before restoring, so a crash mid-restore leaves a
    // store with a matching record rather than an apparent orphan.
    if (m_settings) {
        // authType routes restore: "oauth" goes to oauth().restore_session(),
        // anything else (including "sso") to matrix_auth(). SSO is stored under
        // its own name so the record stays truthful about the account's origin.
        m_settings->saveSession(identity.homeserver, identity.userId, deviceId,
                                accessToken, refreshToken,
                                authType, clientId);
        m_settings->setSyncToken({});
        // Record the store location (the store an account uses is recorded,
        // never re-derived). The password path records it from login_ok, which
        // OAuth does not reach.
        recordStoreLocation(identity);
    }

    const QString result = restore(identity, deviceId);
    if (!result.isEmpty()) {
        setState(Error);
        Q_EMIT loginFailed(result.startsWith(QLatin1String("error: ")) ? result.mid(7)
                                                                       : result);
    }
}

bool RustSdkMatrixClient::restoreSession()
{
    // Named scope for the stall tracer, as in detachSession().
    stalltrace::Scope stallScope("session-restore");
    if (!m_settings || !m_settings->hasSession())
        return false;

    matrix::app_data::AccountIdentity identity;
    // resolveSavedIdentity binds the recorded store location; the plain
    // resolver is the fallback for sessions that predate recording.
    if (!m_settings->resolveSavedIdentity(m_settings->userId(), &identity)
        && !matrix::app_data::resolveAccountIdentity(
            m_settings->homeserverUrl(), m_settings->userId(), &identity)) {
        // No safe identity could be derived, so there is no account to name.
        matrix::app_data::AccountIdentity unresolved;
        unresolved.userId = m_settings->userId();
        unresolved.homeserver = m_settings->homeserverUrl();
        // An unparsable saved record is not "this store belongs to someone
        // else"; report the specific reason.
        const auto reason =
            matrix::rust_session::StoreBlockReason::InvalidSavedIdentity;
        requireLocalReset(matrix::rust_session::diagnosticName(reason),
                          unresolved);
        setState(Error);
        Q_EMIT loginFailed(matrix::rust_session::userMessage(reason));
        return false;
    }
    m_openingIdentity = identity;

    const QString hs = identity.homeserver;
    const QString userId = m_settings->userId();
    const QString deviceId = m_settings->deviceId();
    const QString accessToken = m_settings->accessToken();
    if (hs.isEmpty() || userId.isEmpty() || accessToken.isEmpty())
        return false;

    // Repair installs where an older build left the store under the typed
    // localpart casing, before deciding the store is missing.
    auto refusal = matrix::rust_session::StoreBlockReason::None;
    if (!pathExistsOrIsLink(identity.rustStorePath)) {
        adoptDivergentStoreIfUnambiguous(&identity, &refusal);
        m_openingIdentity = identity;
    }
    if (refusal != matrix::rust_session::StoreBlockReason::None) {
        failWithBlockReason(refusal, identity);
        return false;
    }

    const auto block = matrix::rust_session::restoreBlockReason(
        identity, pathExistsOrIsLink(identity.rustStorePath), deviceId);
    if (block != matrix::rust_session::StoreBlockReason::None) {
        failWithBlockReason(block, identity);
        return false;
    }

    if (!ensureRustHandleForIdentity(identity)) {
        setState(Error);
        Q_EMIT loginFailed(tr("Rust SDK backend could not be initialized."));
        return false;
    }

    m_homeserver = hs;
    m_userId = userId;
    m_deviceId = deviceId;
    m_loggedIn = false;
    // A new attempt: reset what the previous one needed to open its store.
    m_restoredOffline = false;
    // Log dedupe is per session, not per process, so a second broken account in
    // the same run still logs. Reset wherever a session ends.
    m_ownIdentityKeyMismatchLogged = false;
    m_rooms.clear();
    m_roomOrder.clear();
    m_timelines.clear();
    m_pendingSends.clear();
    setInitialSyncDone(false);
    Q_EMIT roomsChanged();
    setState(Connecting);

    const QByteArray hsBytes = hs.toUtf8();
    const QByteArray userBytes = userId.toUtf8();
    const QByteArray deviceBytes = deviceId.toUtf8();
    const QByteArray tokenBytes = accessToken.toUtf8();
    // Carry the refresh token when there is one, so an expired access token can
    // be renewed. Empty is normal for servers that issue none.
    const QByteArray refreshBytes = m_settings->refreshToken().toUtf8();

    // A restart without logout must restore the existing device through the SDK
    // API that owns this session type; routing OAuth through matrix_auth()
    // would fail and lose refresh handling. The discriminator lives in
    // QSettings so it stays readable when the keyring is not.
    QString result;
    if (m_settings->isOAuthAccount(userId)) {
        const QByteArray clientBytes = m_settings->oauthClientIdFor(userId).toUtf8();
        if (clientBytes.isEmpty()) {
            // An OAuth account without a registration id cannot be restored;
            // say so instead of falling back to the password path.
            setState(Error);
            Q_EMIT loginFailed(matrix::rust_session::userMessage(
                matrix::rust_session::StoreBlockReason::MissingSessionMetadata));
            return false;
        }
        result = takeRustString(mx_rust_oauth_restore(m_rustHandle,
                                                      hsBytes.constData(),
                                                      userBytes.constData(),
                                                      deviceBytes.constData(),
                                                      clientBytes.constData(),
                                                      tokenBytes.constData(),
                                                      refreshBytes.constData()));
    } else {
        result = takeRustString(mx_rust_restore(m_rustHandle,
                                                hsBytes.constData(),
                                                userBytes.constData(),
                                                deviceBytes.constData(),
                                                tokenBytes.constData(),
                                                refreshBytes.constData()));
    }
    if (!result.isEmpty()) {
        setState(Error);
        Q_EMIT loginFailed(result.startsWith(QLatin1String("error: "))
                           ? result.mid(7)
                           : result);
        return false;
    }
    return true;
}

bool RustSdkMatrixClient::restoreSessionFromFile(const QString &homeserver,
                                                 const QString &userIdForStore)
{
    matrix::app_data::AccountIdentity identity;
    if (!matrix::app_data::resolveAccountIdentity(
            homeserver, userIdForStore, &identity)
        || m_sessionFilePath.isEmpty())
        return false;
    const QString hs = identity.homeserver;
    const QString expectedUser = identity.userId;

    if (!ensureRustHandleForUser(expectedUser)) {
        setState(Error);
        Q_EMIT loginFailed(tr("Rust SDK backend could not be initialized."));
        return false;
    }

    m_homeserver = hs;
    m_userId = expectedUser;
    m_deviceId.clear();
    m_loggedIn = false;
    // A new attempt: reset what the previous one needed to open its store.
    m_restoredOffline = false;
    // Log dedupe is per session, not per process, so a second broken account in
    // the same run still logs. Reset wherever a session ends.
    m_ownIdentityKeyMismatchLogged = false;
    m_rooms.clear();
    m_roomOrder.clear();
    m_timelines.clear();
    m_pendingSends.clear();
    setInitialSyncDone(false);
    Q_EMIT roomsChanged();
    setState(Connecting);

    const QByteArray hsBytes = hs.toUtf8();
    const QByteArray userBytes = expectedUser.toUtf8();
    const QString result = takeRustString(mx_rust_restore_from_file(m_rustHandle,
                                                                     hsBytes.constData(),
                                                                     userBytes.constData()));
    if (!result.isEmpty()) {
        setState(Error);
        Q_EMIT loginFailed(result.startsWith(QLatin1String("error: "))
                           ? result.mid(7)
                           : result);
        return false;
    }
    return true;
}

bool RustSdkMatrixClient::adoptDivergentStoreIfUnambiguous(
    matrix::app_data::AccountIdentity *identity,
    matrix::rust_session::StoreBlockReason *refusal)
{
    if (refusal)
        *refusal = matrix::rust_session::StoreBlockReason::None;
    // The test harness pins an absolute store path; there is no per-account
    // layout to adopt within.
    if (!identity || !m_storePathOverride.isEmpty() || !identity->isValid())
        return false;

    QStringList candidates =
        matrix::app_data::findCaseVariantStoreSlugs(*identity);
    // Besides casing, older builds could split store and record under
    // .well-known delegation (localpart paired with the homeserver URL's host).
    // Reconstruct that slug and take it only if a store is really there.
    const QString delegated =
        matrix::app_data::delegatedHomeserverStoreSlug(*identity);
    if (!delegated.isEmpty() && !candidates.contains(delegated)) {
        matrix::app_data::AccountIdentity probe = *identity;
        if (matrix::app_data::bindStoreSlug(&probe, delegated)
            && QFileInfo(probe.rustStorePath).isDir()
            && !QFileInfo(probe.rustStorePath).isSymLink()) {
            candidates.append(delegated);
        }
    }
    // A directory another saved account owns (by canonical slug or recorded
    // location) is never ours. Uppercase localparts are valid identities.
    if (m_settings) {
        const QStringList saved = m_settings->savedAccountUserIds();
        candidates.removeIf([&](const QString &slug) {
            for (const QString &other : saved) {
                if (other == identity->userId)
                    continue;
                if (matrix::app_data::safeUserSlug(other) == slug
                    || m_settings->storeSlugFor(other) == slug) {
                    return true;
                }
            }
            return false;
        });
    }
    if (candidates.isEmpty())
        return false;
    if (candidates.size() > 1) {
        qCWarning(lcRust) << "store adoption refused: ambiguous ownership"
                          << "slug=" << identity->slug
                          << "candidates=" << candidates.size();
        if (refusal)
            *refusal = matrix::rust_session::StoreBlockReason::AmbiguousStoreCandidates;
        return false;
    }

    const QString source = candidates.first();
    matrix::app_data::AccountIdentity adopted = *identity;
    if (!matrix::app_data::bindStoreSlug(&adopted, source)) {
        qCWarning(lcRust) << "store adoption refused: unsafe candidate path"
                          << "slug=" << identity->slug;
        return false;
    }

    // Recording, not relocating: the store holds the only copy of this
    // account's Megolm keys, and pointing at it is reversible where moving is
    // not. An SDK ownership rejection clears the record again (see
    // login_failed).
    qCInfo(lcRust) << "adopting store recorded under a divergent slug"
                   << "from=" << source << "for=" << identity->slug;
    if (m_settings)
        m_settings->setStoreSlugFor(identity->userId, source);
    *identity = adopted;
    return true;
}

void RustSdkMatrixClient::recordStoreLocation(
    const matrix::app_data::AccountIdentity &identity)
{
    if (!m_settings || !m_storePathOverride.isEmpty() || m_storePath.isEmpty()
        || !identity.isValid()) {
        return;
    }
    // Taken from the directory actually opened, never re-derived.
    const QString opened = QFileInfo(m_storePath).absoluteFilePath();
    const QString slug = QFileInfo(QFileInfo(opened).path()).fileName();
    if (slug.isEmpty())
        return;
    if (slug != identity.slug) {
        qCInfo(lcRust) << "recording divergent store location"
                       << "account=" << identity.slug << "store=" << slug;
    }
    m_settings->setStoreSlugFor(identity.userId, slug);
}

bool RustSdkMatrixClient::resetRustStore()
{
    const QString storePath = m_storePath;
    m_lifecycle.invalidate();
    releaseRustHandle();
    m_storePath.clear();
    clearLocalState();

    if (storePath.isEmpty() || !QFileInfo::exists(storePath))
        return true;

    // Wait for the retiring client's SQLite close before deleting; unlinking
    // under an open connection leaves a half-deleted store.
    if (!waitForRustRetirement(kStoreCloseBudgetMs))
        qCWarning(lcRust) << "store close did not finish within the budget;"
                          << "deleting anyway";

    QDir storeDir(storePath);
    return storeDir.removeRecursively();
}

bool RustSdkMatrixClient::clearPersistedAccount(
    const matrix::app_data::AccountIdentity &identity,
    bool *matchedRecord)
{
    if (matchedRecord)
        *matchedRecord = false;
    if (!m_settings)
        return true;
    return m_settings->clearSessionForAccount(identity.userId, matchedRecord);
}

bool RustSdkMatrixClient::resetLocalSession(
    const matrix::app_data::AccountIdentity &requested,
    QString *message)
{
    // Reset the store this account really uses; resolving from the user id
    // alone would delete the canonical slug and leave the divergent one.
    matrix::app_data::AccountIdentity identity = requested;
    if (m_settings && identity.isValid()) {
        // clearSessionForAccount() matches case variants against the saved
        // record, so the store lookup must use the same rule or the record is
        // cleared while the store is left behind.
        const QString canonical =
            m_settings->canonicalUserIdForTypedIdentity(identity.userId);
        matrix::app_data::AccountIdentity saved;
        if (!canonical.isEmpty()
            && m_settings->resolveSavedIdentity(canonical, &saved)) {
            matrix::app_data::bindStoreSlug(&identity,
                                            saved.effectiveStoreSlug());
        } else {
            matrix::app_data::bindStoreSlug(
                &identity, m_settings->storeSlugFor(identity.userId));
        }
    }
    if (message)
        message->clear();
    if (!identity.isValid()) {
        if (message) {
            *message = tr("Enter a valid homeserver and Matrix user ID before "
                          "resetting the local Lightning session.");
        }
        return false;
    }
    if (m_loggedIn || m_lifecycle.signingOut()) {
        if (message) {
            *message = tr("Lightning could not completely reset the local "
                          "session for this account. Check the application "
                          "logs and filesystem permissions, then try again.");
        }
        return false;
    }
    if (m_rustHandle && !m_storePath.isEmpty()
        && QFileInfo(m_storePath).absoluteFilePath()
            != QFileInfo(identity.rustStorePath).absoluteFilePath()) {
        if (message) {
            *message = tr("Lightning could not completely reset the local "
                          "session for this account. Check the application "
                          "logs and filesystem permissions, then try again.");
        }
        return false;
    }

    m_lifecycle.invalidate();
    releaseRustHandle();
    m_storePath.clear();
    clearLocalState();
    // As in resetRustStore(): renaming the store under an open SQLite
    // connection is the same race as deleting it.
    if (!waitForRustRetirement(kStoreCloseBudgetMs))
        qCWarning(lcRust) << "store close did not finish within the budget;"
                          << "quarantining anyway";
    bool matchedRecord = false;
    const bool sessionOk = clearPersistedAccount(identity, &matchedRecord);
    // Moved aside, not deleted: this repair acts on a belief that the store is
    // unusable or foreign, which can be wrong. Explicit sign-out and account
    // removal still delete (finishSignOut), since leaving key material there
    // would be a data-at-rest defect.
    const auto files = matrix::app_data::quarantineAccountRustState(identity);
    // Matching no record and deleting no store is a no-op and must not report
    // success (a no-op secret clear and an absent store both look successful).
    const bool didSomething = matchedRecord || files.removedAnything();
    const bool ok = sessionOk && files.ok() && didSomething;
    qCInfo(lcRust) << "local Rust reset"
                   << "slug=" << identity.slug
                   << "record=" << (matchedRecord ? "cleared" : "not_found")
                   << "deleted=" << files.deleted
                   << "missing=" << files.missing
                   << "failed=" << files.failed
                   << "session=" << (sessionOk ? "ok" : "failed");

    if (ok) {
        if (message) {
            *message = tr("Local Lightning session rebuilt. The previous "
                          "encryption store was moved aside, not deleted, and "
                          "is still in this account's data directory. You can "
                          "sign in again.");
        }
    } else if (sessionOk && files.ok() && !didSomething) {
        // Nothing was wrong with the filesystem; this account is simply
        // unknown. Never arm the reset UI for a no-op.
        if (message) {
            *message = tr("Lightning has no saved session or local data for "
                          "that account, so there was nothing to reset. Check "
                          "the Matrix user ID and try signing in.");
        }
    } else {
        requireLocalReset(QStringLiteral("cleanup_incomplete"), identity);
        if (message) {
            *message = tr("Lightning could not completely reset the local "
                          "session for this account. Check the application "
                          "logs and filesystem permissions, then try again.");
        }
    }
    return ok;
}

void RustSdkMatrixClient::logout()
{
    if (m_lifecycle.signingOut())
        return;

    matrix::app_data::resolveAccountIdentity(
        m_homeserver, m_userId, &m_signOutIdentity);
    // Delete the store this session actually opened, not one re-derived from
    // the user id, or sign-out can leave Megolm and device keys on disk.
    // m_storePath is authoritative; the recorded slug is the fallback once the
    // handle is gone.
    if (m_storePathOverride.isEmpty() && !m_storePath.isEmpty()) {
        const QString openedSlug = QFileInfo(
            QFileInfo(m_storePath).absoluteFilePath()).dir().dirName();
        matrix::app_data::bindStoreSlug(&m_signOutIdentity, openedSlug);
    } else if (m_settings) {
        matrix::app_data::bindStoreSlug(
            &m_signOutIdentity, m_settings->storeSlugFor(m_signOutIdentity.userId));
    }
    m_signOutDeviceId = m_deviceId;
    qCInfo(lcRust) << "rust sign-out started"
                   << "slug=" << m_signOutIdentity.slug
                   << "device_known=" << !m_signOutDeviceId.isEmpty();
    m_lifecycle.beginSignOut(m_handleGeneration);

    // Queue typing=false before the join, while the client and sync transport
    // still belong to this lifecycle.
    if (m_rustHandle && !m_typingRoom.isEmpty()) {
        const QByteArray room = m_typingRoom.toUtf8();
        takeRustString(mx_rust_send_typing(m_rustHandle, room.constData(), 0));
        m_typingRoom.clear();
    }

    // Deterministic managed-task shutdown: Rust cancels and joins the timeline
    // subscription, joins any in-flight room-key import (never delete the
    // crypto store under a live write), and stops sync. The internal timeout is
    // a last-resort boundary only.
    if (m_rustHandle) {
        const QString shutdown =
            takeRustString(mx_rust_shutdown_tasks(m_rustHandle));
        qCInfo(lcRust) << "logout: managed-task shutdown" << shutdown;
        if (mx_rust_room_key_import_active(m_rustHandle)) {
            qCWarning(lcRust)
                << "logout: import did not finish in time; proceeding anyway";
        }
    }
    m_timelineTracker.reset();
    m_threadTracker.reset();
    m_pagination.clear();
    stopSync();

    if (m_rustHandle) {
        // The session type decides the sign-out, as it decides the restore.
        // mx_rust_logout is POST /logout via matrix_auth(); an OAuth/OIDC
        // server (MAS) revokes tokens through its RFC 7009 endpoint instead,
        // which mx_rust_oauth_logout calls. Using the wrong one leaves the
        // tokens live on the server. The discriminator lives in QSettings so it
        // stays readable when the keyring is not.
        const bool oauthSession = m_settings
            && m_settings->isOAuthAccount(m_signOutIdentity.userId);
        if (oauthSession) {
            // This is the dispatch result only; the revocation outcome arrives
            // in the logged_out event's "result" field, as on the password
            // lane.
            takeRustString(mx_rust_oauth_logout(m_rustHandle));
            qCInfo(lcRust) << "logout: oauth revocation dispatched";
        } else {
            mx_rust_logout(m_rustHandle);
        }
        ensurePollTimer();
    } else {
        finishSignOut(QStringLiteral("no_active_session"), QString{});
    }
}

void RustSdkMatrixClient::startSync()
{
    if (!m_loggedIn || !m_rustHandle)
        return;
    setState(Syncing);
    mx_rust_start_sync(m_rustHandle);
    ensurePollTimer();
}

void RustSdkMatrixClient::stopSync()
{
    if (m_rustHandle) {
        const bool stopped = mx_rust_stop_sync(m_rustHandle) != 0;
        qCInfo(lcRust) << "rust sync stop result="
                       << (stopped ? "ok" : "already_stopped");
    }
    if (m_state == Syncing || m_state == Offline)
        setState(Disconnected);
}

QList<RoomInfo> RustSdkMatrixClient::rooms() const
{
    QList<RoomInfo> list;
    list.reserve(m_rooms.size());
    QSet<QString> seen;
    for (const auto &roomId : m_roomOrder) {
        const auto it = m_rooms.constFind(roomId);
        if (it != m_rooms.constEnd() && !seen.contains(roomId)) {
            list.append(*it);
            seen.insert(roomId);
        }
    }
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it) {
        if (!seen.contains(it.key())) list.append(*it);
    }
    return list;
}

QList<TimelineEvent> RustSdkMatrixClient::timeline(const QString &roomId) const
{
    return m_timelines.value(roomId);
}

QString RustSdkMatrixClient::displayNameFor(const QString &roomId, const QString &userId) const
{
    const auto it = m_rooms.constFind(roomId);
    if (it == m_rooms.constEnd())
        return userId;
    const auto memberIt = it->members.constFind(userId);
    if (memberIt == it->members.constEnd() || memberIt->displayName.isEmpty())
        return userId;
    return memberIt->displayName;
}

QString RustSdkMatrixClient::avatarMxcFor(const QString &roomId, const QString &userId) const
{
    const auto it = m_rooms.constFind(roomId);
    if (it == m_rooms.constEnd())
        return {};
    const auto memberIt = it->members.constFind(userId);
    return memberIt == it->members.constEnd() ? QString() : memberIt->avatarMxcUrl;
}

QStringList RustSdkMatrixClient::typingUsersFor(const QString &roomId) const
{
    const auto it = m_rooms.constFind(roomId);
    return it == m_rooms.constEnd() ? QStringList() : it->typingUserIds;
}

QUrl RustSdkMatrixClient::mediaDownloadUrl(const QString &) const
{
    // Deliberately empty. Rust-backend media goes through the SDK's Media API
    // (authenticated /_matrix/client/v1/media); the legacy builders produce
    // unauthenticated /_matrix/media/v3 links that modern servers refuse and
    // that would only end up handed to a browser.
    return {};
}

QUrl RustSdkMatrixClient::mediaThumbnailUrl(const QString &, int, int,
                                            bool) const
{
    return {}; // see mediaDownloadUrl — same audit decision
}

QString RustSdkMatrixClient::nextTxnId()
{
    return QStringLiteral("r%1.%2")
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(++m_txnCounter);
}

bool RustSdkMatrixClient::isRoomEncrypted(const QString &roomId) const
{
    const auto it = m_rooms.constFind(roomId);
    return it != m_rooms.constEnd() && it->encrypted;
}

TimelineEvent RustSdkMatrixClient::buildOwnEcho(const QString &roomId,
                                                const QString &body,
                                                TimelineEvent::Type type) const
{
    TimelineEvent event;
    event.roomId = roomId;
    event.sender = m_userId;
    event.senderDisplayName = QStringLiteral("You");
    event.body = body;
    event.timestamp = QDateTime::currentDateTimeUtc();
    event.type = type;
    event.status = TimelineEvent::Sending;
    return event;
}

void RustSdkMatrixClient::sendTextMessage(const QString &roomId, const QString &body)
{
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT errorOccurred(tr("Not signed in."));
        return;
    }
    if (!m_rooms.contains(roomId)) {
        Q_EMIT errorOccurred(tr("Unknown room: %1").arg(roomId));
        return;
    }
    if (isRoomEncrypted(roomId) && !rustSupportsE2ee()) {
        Q_EMIT errorOccurred(tr(
            "Cannot send to encrypted rooms yet: Rust SDK encrypted send is not verified."));
        return;
    }

    // Rooms with a live SDK timeline send through Timeline::send: the SDK owns
    // the local echo, send-state transitions and remote-echo reconciliation.
    if (timelineActiveFor(roomId)) {
        const QByteArray roomBytes = roomId.toUtf8();
        const QByteArray bodyBytes = body.toUtf8();
        const QString result = takeRustString(mx_rust_timeline_send_text(
            m_rustHandle, roomBytes.constData(), bodyBytes.constData(),
            nullptr, nullptr));
        if (!result.isEmpty()) {
            Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                     ? result.mid(7)
                                     : result);
        }
        return;
    }

    const QString txnId = nextTxnId();
    TimelineEvent echo = buildOwnEcho(roomId, body, TimelineEvent::TextMessage);
    echo.eventId = QLatin1String("local:") + txnId;
    m_timelines[roomId].append(echo);
    m_pendingSends.insert(txnId, PendingSend{roomId, echo.eventId});
    Q_EMIT eventAppended(roomId, echo);

    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray bodyBytes = body.toUtf8();
    const QByteArray txnBytes = txnId.toUtf8();
    const QString result = takeRustString(mx_rust_send_text(m_rustHandle,
                                                            roomBytes.constData(),
                                                            bodyBytes.constData(),
                                                            txnBytes.constData()));
    if (!result.isEmpty()) {
        failPendingSend(txnId, result.startsWith(QLatin1String("error: "))
                                   ? result.mid(7)
                                   : result);
    }
}

// Outgoing @-mentions: the body carries matrix.to links and the id list lets
// the SDK write m.mentions. Empty ids or no live timeline use the plain send.
void RustSdkMatrixClient::sendTextMessage(const QString &roomId,
                                          const QString &body,
                                          const QStringList &mentionUserIds)
{
    if (mentionUserIds.isEmpty()) {
        sendTextMessage(roomId, body);
        return;
    }
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT errorOccurred(tr("Not signed in."));
        return;
    }
    if (!m_rooms.contains(roomId)) {
        Q_EMIT errorOccurred(tr("Unknown room: %1").arg(roomId));
        return;
    }
    if (isRoomEncrypted(roomId) && !rustSupportsE2ee()) {
        Q_EMIT errorOccurred(tr(
            "Cannot send to encrypted rooms yet: Rust SDK encrypted send is not verified."));
        return;
    }
    if (!timelineActiveFor(roomId)) {
        sendTextMessage(roomId, body);
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray bodyBytes = body.toUtf8();
    const QByteArray mentionBytes =
        mentionUserIds.join(QLatin1Char('\n')).toUtf8();
    const QString result = takeRustString(mx_rust_timeline_send_text(
        m_rustHandle, roomBytes.constData(), bodyBytes.constData(),
        mentionBytes.constData(), nullptr));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

// Replies, edits, reactions and redactions go through matrix-sdk-ui timeline
// actions when the room's live timeline is open; relation JSON is never
// hand-built in C++.
void RustSdkMatrixClient::sendReply(const QString &roomId,
                                    const QString &replyToEventId,
                                    const QString &body)
{
    if (!timelineActiveFor(roomId)) {
        refuseUntilTimelineReady("sendReply");
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray targetBytes = replyToEventId.toUtf8();
    const QByteArray bodyBytes = body.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_send_reply(
        m_rustHandle, roomBytes.constData(), targetBytes.constData(),
        bodyBytes.constData(), nullptr, nullptr));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::sendReply(const QString &roomId,
                                    const QString &replyToEventId,
                                    const QString &body,
                                    const QStringList &mentionUserIds)
{
    if (mentionUserIds.isEmpty() || !timelineActiveFor(roomId)) {
        sendReply(roomId, replyToEventId, body);
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray targetBytes = replyToEventId.toUtf8();
    const QByteArray bodyBytes = body.toUtf8();
    const QByteArray mentionBytes =
        mentionUserIds.join(QLatin1Char('\n')).toUtf8();
    const QString result = takeRustString(mx_rust_timeline_send_reply(
        m_rustHandle, roomBytes.constData(), targetBytes.constData(),
        bodyBytes.constData(), mentionBytes.constData(), nullptr));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::editMessage(const QString &roomId,
                                      const QString &targetEventId,
                                      const QString &newBody)
{
    if (!timelineActiveFor(roomId)) {
        refuseUntilTimelineReady("editMessage");
        return;
    }
    // Decompose the composite id here; it never crosses the FFI. The thread
    // root selects the timeline holding the event, since the live room timeline
    // hides threaded events.
    const bool inThread = isThreadTimelineId(roomId);
    const QString realRoom = inThread ? threadTimelineRoomId(roomId) : roomId;
    const QString threadRoot = inThread ? threadTimelineRootId(roomId) : QString();
    const QByteArray rootBytes = threadRoot.toUtf8();
    const QByteArray roomBytes = realRoom.toUtf8();
    const QByteArray targetBytes = targetEventId.toUtf8();
    const QByteArray bodyBytes = newBody.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_edit(
        m_rustHandle, roomBytes.constData(), rootBytes.constData(),
        targetBytes.constData(), bodyBytes.constData(), nullptr, nullptr));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::editMessage(const QString &roomId,
                                      const QString &targetEventId,
                                      const QString &newBody,
                                      const QStringList &mentionUserIds)
{
    if (mentionUserIds.isEmpty() || !timelineActiveFor(roomId)) {
        editMessage(roomId, targetEventId, newBody);
        return;
    }
    // Decompose the composite id here; it never crosses the FFI. The thread
    // root selects the timeline holding the event, since the live room timeline
    // hides threaded events.
    const bool inThread = isThreadTimelineId(roomId);
    const QString realRoom = inThread ? threadTimelineRoomId(roomId) : roomId;
    const QString threadRoot = inThread ? threadTimelineRootId(roomId) : QString();
    const QByteArray rootBytes = threadRoot.toUtf8();
    const QByteArray roomBytes = realRoom.toUtf8();
    const QByteArray targetBytes = targetEventId.toUtf8();
    const QByteArray bodyBytes = newBody.toUtf8();
    const QByteArray mentionBytes =
        mentionUserIds.join(QLatin1Char('\n')).toUtf8();
    const QString result = takeRustString(mx_rust_timeline_edit(
        m_rustHandle, roomBytes.constData(), rootBytes.constData(),
        targetBytes.constData(), bodyBytes.constData(),
        mentionBytes.constData(), nullptr));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

// ---- Formatted sends. The spec overloads carry the full guard set; empty
// specs fall back to the plain overloads. A non-empty spec is never silently
// degraded: an honest refusal beats losing formatting behind the user's back.
void RustSdkMatrixClient::sendTextMessage(const QString &roomId,
                                          const QString &body,
                                          const QStringList &mentionUserIds,
                                          const QVariantMap &bodySpec)
{
    if (bodySpec.isEmpty()) {
        sendTextMessage(roomId, body, mentionUserIds);
        return;
    }
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT errorOccurred(tr("Not signed in."));
        return;
    }
    if (!m_rooms.contains(roomId)) {
        Q_EMIT errorOccurred(tr("Unknown room: %1").arg(roomId));
        return;
    }
    if (isRoomEncrypted(roomId) && !rustSupportsE2ee()) {
        Q_EMIT errorOccurred(tr(
            "Cannot send to encrypted rooms yet: Rust SDK encrypted send is not verified."));
        return;
    }
    if (!timelineActiveFor(roomId)) {
        // Send it anyway, plain. This is an ordinary message with no target
        // event, so the room-level send can carry it. The composer attaches a
        // spec to most messages, so refusing here would make a room without a
        // live timeline impossible to type in; losing formatting on one message
        // is the lesser harm.
        qCWarning(lcRust) << "no live timeline for" << roomId
                          << "— sending as plain text without the body spec";
        sendTextMessage(roomId, body);
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray bodyBytes = body.toUtf8();
    const QByteArray mentionBytes =
        mentionUserIds.join(QLatin1Char('\n')).toUtf8();
    const QByteArray specBytes = bodySpecJson(bodySpec);
    const QString result = takeRustString(mx_rust_timeline_send_text(
        m_rustHandle, roomBytes.constData(), bodyBytes.constData(),
        mentionUserIds.isEmpty() ? nullptr : mentionBytes.constData(),
        specBytes.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::sendReply(const QString &roomId,
                                    const QString &replyToEventId,
                                    const QString &body,
                                    const QStringList &mentionUserIds,
                                    const QVariantMap &bodySpec)
{
    if (bodySpec.isEmpty()) {
        sendReply(roomId, replyToEventId, body, mentionUserIds);
        return;
    }
    if (!timelineActiveFor(roomId)) {
        refuseUntilTimelineReady("sendReply(formatted)");
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray targetBytes = replyToEventId.toUtf8();
    const QByteArray bodyBytes = body.toUtf8();
    const QByteArray mentionBytes =
        mentionUserIds.join(QLatin1Char('\n')).toUtf8();
    const QByteArray specBytes = bodySpecJson(bodySpec);
    const QString result = takeRustString(mx_rust_timeline_send_reply(
        m_rustHandle, roomBytes.constData(), targetBytes.constData(),
        bodyBytes.constData(),
        mentionUserIds.isEmpty() ? nullptr : mentionBytes.constData(),
        specBytes.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::editMessage(const QString &roomId,
                                      const QString &targetEventId,
                                      const QString &newBody,
                                      const QStringList &mentionUserIds,
                                      const QVariantMap &bodySpec)
{
    if (bodySpec.isEmpty()) {
        editMessage(roomId, targetEventId, newBody, mentionUserIds);
        return;
    }
    if (!timelineActiveFor(roomId)) {
        refuseUntilTimelineReady("editMessage(formatted)");
        return;
    }
    // Decompose the composite id here; it never crosses the FFI.
    const bool inThread = isThreadTimelineId(roomId);
    const QString realRoom = inThread ? threadTimelineRoomId(roomId) : roomId;
    const QString threadRoot = inThread ? threadTimelineRootId(roomId) : QString();
    const QByteArray rootBytes = threadRoot.toUtf8();
    const QByteArray roomBytes = realRoom.toUtf8();
    const QByteArray targetBytes = targetEventId.toUtf8();
    const QByteArray bodyBytes = newBody.toUtf8();
    const QByteArray mentionBytes =
        mentionUserIds.join(QLatin1Char('\n')).toUtf8();
    const QByteArray specBytes = bodySpecJson(bodySpec);
    const QString result = takeRustString(mx_rust_timeline_edit(
        m_rustHandle, roomBytes.constData(), rootBytes.constData(),
        targetBytes.constData(), bodyBytes.constData(),
        mentionUserIds.isEmpty() ? nullptr : mentionBytes.constData(),
        specBytes.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::redactEvent(const QString &roomId,
                                      const QString &eventId,
                                      const QString &reason)
{
    if (!timelineActiveFor(roomId)) {
        refuseUntilTimelineReady("redactEvent");
        return;
    }
    // The composite id never crosses the FFI. Redaction is addressed by event
    // id through the room, so the real room is all Rust needs; the live room
    // timeline would not find a thread reply.
    const QString realRoom = isThreadTimelineId(roomId)
        ? threadTimelineRoomId(roomId) : roomId;
    const QByteArray roomBytes = realRoom.toUtf8();
    const QByteArray targetBytes = eventId.toUtf8();
    const QByteArray reasonBytes = reason.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_redact(
        m_rustHandle, roomBytes.constData(), targetBytes.constData(),
        reasonBytes.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::toggleReaction(const QString &roomId,
                                         const QString &targetEventId,
                                         const QString &key)
{
    if (!timelineActiveFor(roomId)) {
        refuseUntilTimelineReady("toggleReaction");
        return;
    }
    // Decompose the composite id here; it never crosses the FFI. The thread
    // root selects the timeline holding the event, since the live room timeline
    // hides threaded events.
    const bool inThread = isThreadTimelineId(roomId);
    const QString realRoom = inThread ? threadTimelineRoomId(roomId) : roomId;
    const QString threadRoot = inThread ? threadTimelineRootId(roomId) : QString();
    const QByteArray roomBytes = realRoom.toUtf8();
    const QByteArray rootBytes = threadRoot.toUtf8();
    const QByteArray targetBytes = targetEventId.toUtf8();
    const QByteArray keyBytes = key.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_toggle_reaction(
        m_rustHandle, roomBytes.constData(), rootBytes.constData(),
        targetBytes.constData(), keyBytes.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::removeMessageEdits(const QString &roomId,
                                             const QString &eventId)
{
    // Not gated on timelineActiveFor(): this reads relations through the room,
    // and the menu may be open while the timeline is rebuilt. Rust re-validates
    // the room and event id.
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty() || eventId.isEmpty())
        return;
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray eventBytes = eventId.toUtf8();
    const QString result = takeRustString(mx_rust_remove_message_edits(
        m_rustHandle, roomBytes.constData(), eventBytes.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

// Polls. Votes and ends act on a poll in the current room or one of its
// threads, so the live-timeline guard applies; Rust resolves the thread
// target (open panel timeline, else a transient thread-focused one).
void RustSdkMatrixClient::sendPollResponse(const QString &roomId,
                                           const QString &threadRootId,
                                           const QString &pollStartEventId,
                                           const QStringList &answerIds)
{
    if (!timelineActiveFor(roomId)) {
        refuseUntilTimelineReady("sendPollResponse");
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray threadBytes = threadRootId.toUtf8();
    const QByteArray pollBytes = pollStartEventId.toUtf8();
    // The FFI list is newline-joined; answer ids embedding newlines would split
    // into non-matching ids, so they are dropped.
    QStringList safeIds;
    for (const QString &id : answerIds) {
        if (!id.contains(QLatin1Char('\n')))
            safeIds.append(id);
    }
    const QByteArray answerBytes = safeIds.join(QLatin1Char('\n')).toUtf8();
    const QString result = takeRustString(mx_rust_timeline_poll_response(
        m_rustHandle, roomBytes.constData(), threadBytes.constData(),
        pollBytes.constData(), answerBytes.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::endPoll(const QString &roomId,
                                  const QString &threadRootId,
                                  const QString &pollStartEventId)
{
    if (!timelineActiveFor(roomId)) {
        refuseUntilTimelineReady("endPoll");
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray threadBytes = threadRootId.toUtf8();
    const QByteArray pollBytes = pollStartEventId.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_poll_end(
        m_rustHandle, roomBytes.constData(), threadBytes.constData(),
        pollBytes.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::createPoll(const QString &roomId,
                                     const QString &threadRootId,
                                     const QString &question,
                                     const QStringList &answers,
                                     bool undisclosed,
                                     int maxSelections)
{
    if (!timelineActiveFor(roomId)) {
        refuseUntilTimelineReady("createPoll");
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray threadBytes = threadRootId.toUtf8();
    const QByteArray questionBytes = question.toUtf8();
    const QByteArray answerBytes = answers.join(QLatin1Char('\n')).toUtf8();
    const QString result = takeRustString(mx_rust_timeline_poll_create(
        m_rustHandle, roomBytes.constData(), threadBytes.constData(),
        questionBytes.constData(), answerBytes.constData(),
        undisclosed ? 1 : 0,
        static_cast<unsigned int>(qMax(1, maxSelections))));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::sendTyping(const QString &roomId, bool typing, int)
{
    if (!m_rustHandle || roomId.isEmpty()) return;
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_send_typing(
        m_rustHandle, room.constData(), typing ? 1 : 0));
    if (result.isEmpty()) {
        if (typing) m_typingRoom = roomId;
        else if (m_typingRoom == roomId) m_typingRoom.clear();
    } else {
        qCWarning(lcRust) << "typing command rejected";
    }
}

void RustSdkMatrixClient::setStrictDeviceTrust(bool enabled)
{
    const QString result = takeRustString(
        mx_rust_set_strict_device_trust(enabled ? 1 : 0));
    if (!result.isEmpty())
        qCWarning(lcRust) << "strict device trust command rejected";
}

void RustSdkMatrixClient::setReadReceiptPrivacy(int mode)
{
    // Remembered without a handle too: the setting is read at startup, before
    // the bridge exists.
    m_readReceiptPrivacy = (mode < 0 || mode > 2) ? 0 : mode;
    if (!m_rustHandle)
        return;
    const QString result = takeRustString(
        mx_rust_set_receipt_privacy(m_rustHandle, m_readReceiptPrivacy));
    if (!result.isEmpty())
        qCWarning(lcRust) << "receipt privacy command rejected";
}

void RustSdkMatrixClient::sendReadReceipt(const QString &roomId, const QString &eventId)
{
    if (!m_rustHandle || roomId.isEmpty() || eventId.isEmpty()
        || m_lastReceiptSent.value(roomId) == eventId)
        return;
    m_lastReceiptSent.insert(roomId, eventId);
    const QByteArray room = roomId.toUtf8();
    const QByteArray event = eventId.toUtf8();
    const QString result = takeRustString(mx_rust_send_read_receipt(
        m_rustHandle, room.constData(), event.constData()));
    if (!result.isEmpty()) {
        m_lastReceiptSent.remove(roomId);
        qCWarning(lcRust) << "read receipt command rejected";
    }
}

void RustSdkMatrixClient::setRoomMarkedUnread(const QString &roomId, bool unread)
{
    if (!m_rustHandle || roomId.isEmpty()) return;
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_set_marked_unread(
        m_rustHandle, room.constData(), unread ? 1 : 0));
    if (!result.isEmpty()) qCWarning(lcRust) << "marked-unread command rejected";
}

void RustSdkMatrixClient::setRoomFavourite(const QString &roomId, bool favourite)
{
    if (!m_rustHandle || roomId.isEmpty()) return;
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_set_room_favourite(
        m_rustHandle, room.constData(), favourite ? 1 : 0));
    if (!result.isEmpty()) qCWarning(lcRust) << "favourite command rejected";
}

void RustSdkMatrixClient::markRoomRead(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty()) return;
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_mark_room_read(
        m_rustHandle, room.constData()));
    if (!result.isEmpty()) qCWarning(lcRust) << "mark-room-read command rejected";
}

void RustSdkMatrixClient::setRoomNotificationMode(const QString &roomId, int mode)
{
    // Mode 3 (account default) is a rule removal routed through
    // clearRoomNotificationMode; it must not cross the FFI as a mode.
    if (!m_rustHandle || roomId.isEmpty() || mode < 0 || mode > 2) return;
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_set_room_notification_mode(
        m_rustHandle, room.constData(), mode));
    if (!result.isEmpty())
        qCWarning(lcRust) << "notification-mode command rejected";
}

void RustSdkMatrixClient::requestThreadParticipants(const QString &roomId,
                                                    const QString &rootEventId)
{
    // The composite thread-timeline id must never reach a protocol call.
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty()
        || rootEventId.isEmpty() || isThreadTimelineId(roomId))
        return;
    const QByteArray room = roomId.toUtf8();
    const QByteArray root = rootEventId.toUtf8();
    const QString result = takeRustString(mx_rust_thread_participants(
        m_rustHandle, room.constData(), root.constData()));
    if (!result.isEmpty())
        qCWarning(lcRust) << "thread participants request rejected";
}

void RustSdkMatrixClient::requestPresence(const QStringList &userIds,
                                          quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || userIds.isEmpty())
        return;
    QJsonArray ids;
    for (const QString &userId : userIds)
        ids.append(userId);
    const QByteArray payload =
        QJsonDocument(ids).toJson(QJsonDocument::Compact);
    const QString result = takeRustString(
        mx_rust_get_presence(m_rustHandle, payload.constData(), opId));
    if (!result.isEmpty())
        qCWarning(lcRust) << "presence request rejected";
}

void RustSdkMatrixClient::fetchProfileBanner(const QString &userId,
                                             quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || userId.isEmpty())
        return;
    const QByteArray target = userId.toUtf8();
    const QString result = takeRustString(
        mx_rust_fetch_profile_banner(m_rustHandle, target.constData(), opId));
    if (!result.isEmpty())
        qCWarning(lcRust) << "profile banner request rejected";
}

void RustSdkMatrixClient::fetchNameColor(const QString &userId, quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || userId.isEmpty())
        return;
    const QByteArray target = userId.toUtf8();
    const QString result = takeRustString(
        mx_rust_fetch_name_color(m_rustHandle, target.constData(), opId));
    if (!result.isEmpty())
        qCWarning(lcRust) << "name colour request rejected";
}

void RustSdkMatrixClient::setNameColor(const QString &value, quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle)
        return;
    const QByteArray colour = value.toUtf8();
    const QString result = takeRustString(
        mx_rust_set_name_color(m_rustHandle, colour.constData(), opId));
    if (!result.isEmpty())
        Q_EMIT nameColorSet(opId, false, QString(), QStringLiteral("rejected"));
}

void RustSdkMatrixClient::setProfileBanner(const QString &localPath,
                                           quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle)
        return;
    // The path is never logged: it is a filesystem path the user picked.
    const QByteArray path = localPath.toUtf8();
    const QString result = takeRustString(
        mx_rust_set_profile_banner(m_rustHandle, path.constData(), opId));
    if (!result.isEmpty()) {
        Q_EMIT profileBannerSet(opId, false, QString(),
                                QStringLiteral("rejected"));
    }
}

void RustSdkMatrixClient::fetchProfileBio(const QString &userId, quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || userId.isEmpty())
        return;
    const QByteArray target = userId.toUtf8();
    const QString result = takeRustString(
        mx_rust_fetch_profile_bio(m_rustHandle, target.constData(), opId));
    if (!result.isEmpty())
        qCWarning(lcRust) << "profile bio request rejected";
}

void RustSdkMatrixClient::setProfileBio(const QString &text, quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle)
        return;
    // The text is never logged: it is prose the user wrote about themselves.
    const QByteArray payload = text.toUtf8();
    const QString result = takeRustString(
        mx_rust_set_profile_bio(m_rustHandle, payload.constData(), opId));
    if (!result.isEmpty()) {
        Q_EMIT profileBioSet(opId, false, QString(),
                             QStringLiteral("rejected"));
    }
}

void RustSdkMatrixClient::fetchRoomBanner(const QString &roomId, quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty())
        return;
    const QByteArray target = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_fetch_room_banner(m_rustHandle, target.constData(), opId));
    if (!result.isEmpty())
        qCWarning(lcRust) << "room banner request rejected";
}

void RustSdkMatrixClient::setRoomBanner(const QString &roomId,
                                        const QString &localPath, quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty())
        return;
    // The path is never logged: it is a filesystem path the user picked.
    const QByteArray target = roomId.toUtf8();
    const QByteArray path = localPath.toUtf8();
    const QString result = takeRustString(mx_rust_set_room_banner(
        m_rustHandle, target.constData(), path.constData(), opId));
    if (!result.isEmpty()) {
        Q_EMIT roomBannerSet(opId, roomId, false, QString(),
                             QStringLiteral("rejected"));
    }
}

// ---------------------------------------------------------------------------
// Stickers and custom emoji (MSC2545 image packs)
// ---------------------------------------------------------------------------

void RustSdkMatrixClient::fetchStickerPacks(const QString &roomId, quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle)
        return;
    // An empty room id asks for the global packs only.
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_stickers_fetch_packs(m_rustHandle, room.constData(), opId));
    if (!result.isEmpty()) {
        // A rejection must still answer, or the picker waits forever. An empty
        // list is honest here: with the handle checked above, Rust can only
        // refuse when there is no session, and then there are no packs. (A read
        // that was accepted and then failed answers through the poll event
        // instead.)
        qCWarning(lcRust) << "sticker pack request rejected";
        Q_EMIT stickerPacksReceived(opId, roomId, false, QVariantList());
    }
}

void RustSdkMatrixClient::sendSticker(const QString &roomId,
                                      const QString &rootId,
                                      const QString &url, const QString &body,
                                      const QString &mimetype, quint64 width,
                                      quint64 height, quint64 size)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty() || url.isEmpty())
        return;
    // Neither mxc nor body is logged: the body is pack-author alt text and an
    // mxc identifies media.
    const QByteArray room = roomId.toUtf8();
    const QByteArray root = rootId.toUtf8();
    const QByteArray mxc = url.toUtf8();
    const QByteArray alt = body.toUtf8();
    const QByteArray mime = mimetype.toUtf8();
    const QString result = takeRustString(mx_rust_stickers_send(
        m_rustHandle, room.constData(), root.constData(), mxc.constData(),
        alt.constData(), mime.constData(), width, height, size));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "sticker send rejected";
        Q_EMIT errorOccurred(tr("The sticker could not be sent."));
    }
}

void RustSdkMatrixClient::addStickerToRoomPack(
    const QString &roomId, const QString &stateKey, const QString &shortcode,
    const QString &url, const QString &body, const QString &mimetype,
    quint64 width, quint64 height, quint64 size, quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty() || url.isEmpty())
        return;
    const QByteArray room = roomId.toUtf8();
    const QByteArray key = stateKey.toUtf8();
    const QByteArray code = shortcode.toUtf8();
    const QByteArray mxc = url.toUtf8();
    const QByteArray alt = body.toUtf8();
    const QByteArray mime = mimetype.toUtf8();
    const QString result = takeRustString(mx_rust_stickers_add_to_room_pack(
        m_rustHandle, room.constData(), key.constData(), code.constData(),
        mxc.constData(), alt.constData(), mime.constData(), width, height,
        size, opId));
    if (!result.isEmpty()) {
        Q_EMIT stickerPackAddFinished(opId, false, QStringLiteral("rejected"),
                                      QString());
    }
}

void RustSdkMatrixClient::setStickerRoomPackEnabled(const QString &roomId,
                                                    const QString &stateKey,
                                                    bool enabled, quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty())
        return;
    const QByteArray room = roomId.toUtf8();
    const QByteArray key = stateKey.toUtf8();
    const QString result =
        takeRustString(mx_rust_stickers_set_room_pack_enabled(
            m_rustHandle, room.constData(), key.constData(), enabled, opId));
    if (!result.isEmpty()) {
        Q_EMIT stickerPackRoomsSet(opId, false, QStringLiteral("rejected"),
                                   roomId, stateKey, enabled);
    }
}

void RustSdkMatrixClient::uploadStickerToUserPack(
    const QString &shortcode, const QString &body, const QString &localPath,
    quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || localPath.isEmpty()) {
        Q_EMIT stickerPackAddFinished(opId, false, QStringLiteral("rejected"),
                                      QString());
        return;
    }
    const QByteArray code = shortcode.toUtf8();
    const QByteArray alt = body.toUtf8();
    const QByteArray path = localPath.toUtf8();
    const QString result = takeRustString(mx_rust_stickers_upload_to_user_pack(
        m_rustHandle, code.constData(), alt.constData(), path.constData(),
        opId));
    if (!result.isEmpty()) {
        // A literal tag only: the rejection can echo the path, which contains
        // the user's name.
        qCWarning(lcRust) << "sticker upload rejected";
        Q_EMIT stickerPackAddFinished(opId, false, QStringLiteral("rejected"),
                                      QString());
    }
}

// ── Policy lists ───────────────────────────────────────────────────────

void RustSdkMatrixClient::fetchPolicyRules(const QString &roomId, quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty())
        return;
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_policy_fetch_rules(m_rustHandle, room.constData(), opId));
    if (!result.isEmpty()) {
        Q_EMIT policyRulesReceived(opId, false, roomId, false, false, {});
    }
}

void RustSdkMatrixClient::writePolicyRule(const QString &roomId,
                                          const QString &kind,
                                          const QString &entity,
                                          const QString &stateKey,
                                          const QString &recommendation,
                                          const QString &reason, quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty())
        return;
    const QByteArray room = roomId.toUtf8();
    const QByteArray k = kind.toUtf8();
    const QByteArray e = entity.toUtf8();
    const QByteArray key = stateKey.toUtf8();
    const QByteArray rec = recommendation.toUtf8();
    const QByteArray why = reason.toUtf8();
    const QString result = takeRustString(mx_rust_policy_write_rule(
        m_rustHandle, room.constData(), k.constData(), e.constData(),
        key.constData(), rec.constData(), why.constData(), opId));
    // A synchronous refusal still reports; the caller holds an op slot.
    if (!result.isEmpty())
        Q_EMIT policyRuleWritten(opId, false, QStringLiteral("rejected"));
}

void RustSdkMatrixClient::setPolicySubscribed(const QString &roomId,
                                              bool subscribed, quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty())
        return;
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_policy_subscribe(
        m_rustHandle, room.constData(), subscribed ? 1 : 0, opId));
    if (!result.isEmpty()) {
        Q_EMIT policySubscriptionsReceived(opId, false,
                                           QStringLiteral("rejected"), {});
    }
}

void RustSdkMatrixClient::fetchPolicySubscriptions(quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle)
        return;
    const QString result =
        takeRustString(mx_rust_policy_subscriptions(m_rustHandle, opId));
    if (!result.isEmpty()) {
        Q_EMIT policySubscriptionsReceived(opId, false,
                                           QStringLiteral("rejected"), {});
    }
}

void RustSdkMatrixClient::checkPolicyEntity(const QString &kind,
                                            const QString &entity,
                                            quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || entity.isEmpty())
        return;
    const QByteArray k = kind.toUtf8();
    const QByteArray e = entity.toUtf8();
    takeRustString(mx_rust_policy_check(m_rustHandle, k.constData(),
                                        e.constData(), opId));
}

// ── MSC4108 sign-in-another-device ─────────────────────────────────────
//
// The starters answer with the flow's generation as a decimal string;
// anything else is an error message.

quint64 RustSdkMatrixClient::qrLoginGenerate()
{
    if (!m_loggedIn || !m_rustHandle)
        return 0;
    const QString result =
        takeRustString(mx_rust_qr_login_generate(m_rustHandle));
    bool ok = false;
    const quint64 generation = result.toULongLong(&ok);
    if (!ok || generation == 0) {
        qCWarning(lcRust) << "qr login could not start";
        return 0;
    }
    return generation;
}

quint64 RustSdkMatrixClient::qrLoginScan(const QString &payload)
{
    if (!m_loggedIn || !m_rustHandle || payload.trimmed().isEmpty())
        return 0;
    // Never logged: the payload carries the ephemeral public key and rendezvous
    // URL of a channel about to receive this account's cross-signing secrets.
    const QByteArray data = payload.toUtf8();
    const QString result = takeRustString(
        mx_rust_qr_login_scan(m_rustHandle, data.constData()));
    bool ok = false;
    const quint64 generation = result.toULongLong(&ok);
    if (!ok || generation == 0) {
        qCWarning(lcRust) << "qr login could not start from a scanned code";
        return 0;
    }
    return generation;
}

void RustSdkMatrixClient::qrLoginSubmitCheckCode(quint64 generation, int code)
{
    if (!m_loggedIn || !m_rustHandle || generation == 0)
        return;
    const QString result = takeRustString(
        mx_rust_qr_login_check_code(m_rustHandle, generation, code));
    if (!result.isEmpty())
        qCWarning(lcRust) << "qr login check code rejected";
}

void RustSdkMatrixClient::qrLoginCancel()
{
    if (!m_rustHandle)
        return;
    takeRustString(mx_rust_qr_login_cancel(m_rustHandle));
}

void RustSdkMatrixClient::editStickerPack(
    const QString &roomId, const QString &stateKey, const QString &action,
    const QString &argA, const QString &argB, quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || action.isEmpty())
        return;
    const QByteArray room = roomId.toUtf8();
    const QByteArray key = stateKey.toUtf8();
    const QByteArray verb = action.toUtf8();
    const QByteArray a = argA.toUtf8();
    const QByteArray b = argB.toUtf8();
    const QString result = takeRustString(mx_rust_stickers_edit_pack(
        m_rustHandle, room.constData(), key.constData(), verb.constData(),
        a.constData(), b.constData(), opId));
    // A synchronous refusal still reports; the caller is waiting on the op id.
    if (!result.isEmpty()) {
        Q_EMIT stickerPackEditFinished(opId, false, QStringLiteral("rejected"),
                                       QString());
    }
}

void RustSdkMatrixClient::addStickerToUserPack(
    const QString &shortcode, const QString &url, const QString &body,
    const QString &mimetype, quint64 width, quint64 height, quint64 size,
    quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || url.isEmpty())
        return;
    const QByteArray code = shortcode.toUtf8();
    const QByteArray mxc = url.toUtf8();
    const QByteArray alt = body.toUtf8();
    const QByteArray mime = mimetype.toUtf8();
    const QString result = takeRustString(mx_rust_stickers_add_to_user_pack(
        m_rustHandle, code.constData(), mxc.constData(), alt.constData(),
        mime.constData(), width, height, size, opId));
    if (!result.isEmpty()) {
        Q_EMIT stickerPackAddFinished(opId, false, QStringLiteral("rejected"),
                                      QString());
    }
}

void RustSdkMatrixClient::publishPresence(int state)
{
    publishPresence(state, QString());
}

void RustSdkMatrixClient::publishPresence(int state, const QString &statusMsg)
{
    if (!m_loggedIn || !m_rustHandle || state < 0 || state > 2)
        return;
    const QByteArray status = statusMsg.toUtf8();
    const QString result = takeRustString(mx_rust_set_presence(
        m_rustHandle, static_cast<unsigned int>(state),
        statusMsg.isEmpty() ? nullptr : status.constData()));
    if (!result.isEmpty())
        qCWarning(lcRust) << "presence publish rejected";
}

void RustSdkMatrixClient::clearRoomNotificationMode(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty()) return;
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_clear_room_notification_mode(m_rustHandle, room.constData()));
    if (!result.isEmpty())
        qCWarning(lcRust) << "notification-mode clear rejected";
}

void RustSdkMatrixClient::requestRoomNotificationMode(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty()) return;
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_get_room_notification_mode(
        m_rustHandle, room.constData()));
    if (!result.isEmpty())
        qCWarning(lcRust) << "notification-mode query rejected";
}

void RustSdkMatrixClient::acceptInvite(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty()) return;
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_accept_invite(m_rustHandle, room.constData()));
    if (!result.isEmpty()) qCWarning(lcRust) << "invite accept command rejected";
}

void RustSdkMatrixClient::rejectInvite(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty()) return;
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_reject_invite(m_rustHandle, room.constData()));
    if (!result.isEmpty()) qCWarning(lcRust) << "invite reject command rejected";
}

void RustSdkMatrixClient::sendImage(const QString &, const QString &)
{
    refuseSend("sendImage");
}

void RustSdkMatrixClient::sendFile(const QString &, const QString &)
{
    refuseSend("sendFile");
}

namespace {
// Matches timeline::PAGINATION_BATCH on the Rust side.
constexpr unsigned short kPaginationBatch = 20;
// Ceiling for a fully filtered run; not a general page-size increase.
// Unconditional large pages were measured to hurt rooms whose pages add rows
// (overshoot and per-row ingest cost). This escalates only after a page whose
// events were all filtered out (e.g. long MatrixRTC membership runs), where
// there is nothing to overshoot, and drops back to the default as soon as a
// page yields a row. See docs/timeline-scrolling.md.
constexpr unsigned short kPaginationMaxBatch = 180;
} // namespace

void RustSdkMatrixClient::loadOlderMessages(const QString &roomId)
{
    if (!timelineActiveFor(roomId))
        return;
    auto &state = m_pagination[roomId];
    if (state.loading || state.reachedStart)
        return;
    QString result;
    if (isThreadTimelineId(roomId)) {
        const QByteArray room = threadTimelineRoomId(roomId).toUtf8();
        const QByteArray root = threadTimelineRootId(roomId).toUtf8();
        result = takeRustString(mx_rust_thread_paginate_back(
            m_rustHandle, room.constData(), root.constData(),
            state.batchSize > 0 ? state.batchSize : kPaginationBatch));
    } else {
        const QByteArray roomBytes = roomId.toUtf8();
        result = takeRustString(mx_rust_timeline_paginate_back(
            m_rustHandle, roomBytes.constData(),
            state.batchSize > 0 ? state.batchSize : kPaginationBatch));
    }
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "timeline pagination dispatch failed";
        state.failed = true;
        state.failureTransient = true;
        Q_EMIT paginationStateChanged(roomId);
    } else if (state.failed) {
        // An accepted retry has left the terminal state; enter loading now
        // rather than flashing the old error until Rust's loading event
        // arrives.
        state.failed = false;
        state.failureTransient = false;
        Q_EMIT paginationStateChanged(roomId);
    }
}

bool RustSdkMatrixClient::canPaginate(const QString &roomId) const
{
    if (!timelineActiveFor(roomId))
        return false;
    const auto it = m_pagination.constFind(roomId);
    if (it == m_pagination.constEnd())
        return true;
    return !it->loading && !it->reachedStart;
}

bool RustSdkMatrixClient::paginating(const QString &roomId) const
{
    const auto it = m_pagination.constFind(roomId);
    return it != m_pagination.constEnd() && it->loading;
}

bool RustSdkMatrixClient::paginationFailed(const QString &roomId) const
{
    const auto it = m_pagination.constFind(roomId);
    return it != m_pagination.constEnd() && it->failed;
}

bool RustSdkMatrixClient::paginationFailureTransient(const QString &roomId) const
{
    const auto it = m_pagination.constFind(roomId);
    return it != m_pagination.constEnd() && it->failed
        && it->failureTransient;
}

bool RustSdkMatrixClient::lastPaginationFullyFiltered(const QString &roomId) const
{
    const auto it = m_pagination.constFind(roomId);
    return it != m_pagination.constEnd() && it->lastFullyFiltered;
}

void RustSdkMatrixClient::retryFailedSend(const QString &roomId,
                                          const QString &transactionId)
{
    if (isThreadTimelineId(roomId)) {
        // A thread echo is a room send-queue entry; retry it through the room
        // timeline, which outlives the thread panel.
        retryFailedSend(threadTimelineRoomId(roomId), transactionId);
        return;
    }
    if (!timelineActiveFor(roomId) || transactionId.isEmpty())
        return;
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray txnBytes = transactionId.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_retry_send(
        m_rustHandle, roomBytes.constData(), txnBytes.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::cancelSend(const QString &roomId,
                                    const QString &transactionId)
{
    if (isThreadTimelineId(roomId)) {
        // As in retryFailedSend: a thread echo is a room send-queue entry.
        cancelSend(threadTimelineRoomId(roomId), transactionId);
        return;
    }
    if (!timelineActiveFor(roomId) || transactionId.isEmpty())
        return;
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray txnBytes = transactionId.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_cancel_send(
        m_rustHandle, roomBytes.constData(), txnBytes.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

bool RustSdkMatrixClient::timelineActiveFor(const QString &roomId) const
{
    if (isThreadTimelineId(roomId))
        return threadTimelineActiveFor(roomId);
    return m_rustHandle && !roomId.isEmpty()
        && (m_timelineTracker.activeRoom() == roomId
            || m_timelineTracker.requestedRoom() == roomId);
}

bool RustSdkMatrixClient::threadTimelineActiveFor(const QString &timelineId) const
{
    return m_rustHandle && !timelineId.isEmpty()
        && (m_threadTracker.activeRoom() == timelineId
            || m_threadTracker.requestedRoom() == timelineId);
}

bool RustSdkMatrixClient::timelineReadyForPagination(const QString &roomId) const
{
    // Not yet pagination-ready: Rust's timeline_for() accepts requests only
    // after the initial timeline_reset has adopted a live room generation.
    if (isThreadTimelineId(roomId))
        return m_rustHandle && m_threadTracker.readyForPagination(roomId);
    return m_rustHandle && !roomId.isEmpty()
        && m_timelineTracker.readyForPagination(roomId);
}

void RustSdkMatrixClient::openRoomTimeline(const QString &roomId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty())
        return;
    clearThreadTimelineState();
    // request() forgets the previous room without touching anything keyed by
    // it, so retire the room being left here. Rust's timeline_closed is only
    // the backstop; a rejected generation or released handle never delivers it.
    const QString leavingRequested = m_timelineTracker.requestedRoom();
    const QString leavingActive = m_timelineTracker.activeRoom();
    m_timelineTracker.request(roomId);
    retireRoomTimelineMirror(leavingRequested);
    retireRoomTimelineMirror(leavingActive);
    m_pagination.insert(roomId, PaginationState{});
    // redactId() distinguishes rooms; a suffix of the id only shows the server.
    qCInfo(lcRust) << "timeline open room="
                   << matrix::e2ee::redactId(roomId);
    const QByteArray roomBytes = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_timeline_open(m_rustHandle, roomBytes.constData()));
    if (!result.isEmpty()) {
        m_timelineTracker.reset();
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
        return;
    }
    Q_EMIT paginationStateChanged(roomId);
}

bool RustSdkMatrixClient::reloadRoomTimelineAtLive(const QString &roomId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty())
        return false;
    // The reload produces a timeline_reset under a new generation, so reset
    // pagination state or a stale "reached start" suppresses backfill.
    clearThreadTimelineState();
    // Same retirement as an open. A reload of the current room is refused by
    // retireRoomTimelineMirror(), so rows stay until the reset lands.
    const QString leavingRequested = m_timelineTracker.requestedRoom();
    const QString leavingActive = m_timelineTracker.activeRoom();
    m_timelineTracker.request(roomId);
    retireRoomTimelineMirror(leavingRequested);
    retireRoomTimelineMirror(leavingActive);
    m_pagination.insert(roomId, PaginationState{});
    qCInfo(lcRust) << "timeline reload at live room="
                   << matrix::e2ee::redactId(roomId);
    const QByteArray roomBytes = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_timeline_reload_at_live(m_rustHandle, roomBytes.constData()));
    if (!result.isEmpty()) {
        m_timelineTracker.reset();
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
        return false;
    }
    Q_EMIT paginationStateChanged(roomId);
    return true;
}

// ── SDK-backed thread timelines ─────────────────────────────────────────

void RustSdkMatrixClient::openThread(const QString &roomId,
                                     const QString &rootEventId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty()
        || rootEventId.isEmpty()) {
        Q_EMIT threadTimelineFailed(roomId, rootEventId,
                                    QStringLiteral("not_ready"));
        return;
    }
    clearThreadTimelineState();
    const QString timelineId = threadTimelineId(roomId, rootEventId);
    m_threadTracker.request(timelineId);
    m_pagination.insert(timelineId, PaginationState{});
    qCInfo(lcRust) << "thread open root=" << rootEventId.right(12);
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray rootBytes = rootEventId.toUtf8();
    const QString result = takeRustString(mx_rust_thread_open(
        m_rustHandle, roomBytes.constData(), rootBytes.constData()));
    if (!result.isEmpty()) {
        m_threadTracker.reset();
        m_pagination.remove(timelineId);
        Q_EMIT threadTimelineFailed(roomId, rootEventId,
                                    QStringLiteral("dispatch_failed"));
        return;
    }
    Q_EMIT paginationStateChanged(timelineId);
}

void RustSdkMatrixClient::closeThread()
{
    if (m_rustHandle && m_threadTracker.hasActiveTimeline())
        qCInfo(lcRust) << "thread close";
    if (m_rustHandle)
        takeRustString(mx_rust_thread_close(m_rustHandle));
    clearThreadTimelineState();
}

void RustSdkMatrixClient::sendThreadReply(const QString &roomId,
                                          const QString &threadRootEventId,
                                          const QString &body)
{
    sendThreadReplyTo(roomId, threadRootEventId, QString{}, body);
}

void RustSdkMatrixClient::sendThreadReplyTo(const QString &roomId,
                                            const QString &threadRootEventId,
                                            const QString &inReplyToEventId,
                                            const QString &body)
{
    sendThreadReplyTo(roomId, threadRootEventId, inReplyToEventId, body,
                      QStringList());
}

void RustSdkMatrixClient::sendThreadReplyTo(const QString &roomId,
                                            const QString &threadRootEventId,
                                            const QString &inReplyToEventId,
                                            const QString &body,
                                            const QStringList &mentionUserIds)
{
    sendThreadReplyTo(roomId, threadRootEventId, inReplyToEventId, body,
                      mentionUserIds, QVariantMap());
}

void RustSdkMatrixClient::sendThreadReplyTo(const QString &roomId,
                                            const QString &threadRootEventId,
                                            const QString &inReplyToEventId,
                                            const QString &body,
                                            const QStringList &mentionUserIds,
                                            const QVariantMap &bodySpec)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty()
        || threadRootEventId.isEmpty() || body.trimmed().isEmpty()) {
        refuseUntilTimelineReady("sendThreadReply");
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray rootBytes = threadRootEventId.toUtf8();
    const QByteArray bodyBytes = body.toUtf8();
    const QByteArray replyBytes = inReplyToEventId.toUtf8();
    const QByteArray mentionBytes =
        mentionUserIds.join(QLatin1Char('\n')).toUtf8();
    const QByteArray specBytes = bodySpecJson(bodySpec);
    const QString result = takeRustString(mx_rust_thread_send_text(
        m_rustHandle, roomBytes.constData(), rootBytes.constData(),
        bodyBytes.constData(),
        inReplyToEventId.isEmpty() ? nullptr : replyBytes.constData(),
        mentionUserIds.isEmpty() ? nullptr : mentionBytes.constData(),
        specBytes.isEmpty() ? nullptr : specBytes.constData()));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

void RustSdkMatrixClient::queryCryptoHealth()
{
    if (!m_loggedIn || !m_rustHandle)
        return;
    takeRustString(mx_rust_query_crypto_health(m_rustHandle));
}

void RustSdkMatrixClient::requestDeviceList()
{
    if (!m_loggedIn || !m_rustHandle)
        return;
    takeRustString(mx_rust_list_devices(m_rustHandle));
}

void RustSdkMatrixClient::retryDecryption(const QString &roomId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty())
        return;
    // A thread retry targets its parent room; Rust retries both timelines.
    const QString targetRoom = isThreadTimelineId(roomId)
        ? threadTimelineRoomId(roomId)
        : roomId;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 last = m_lastDecryptionRetryMs.value(targetRoom, 0);
    if (now - last < 2000) {
        qCDebug(lcE2ee) << "retry coalesced" << "room="
                        << matrix::e2ee::redactId(targetRoom);
        return;   // bounded: coalesce rapid repeat requests
    }
    const QByteArray roomBytes = targetRoom.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_retry_decryption(
        m_rustHandle, roomBytes.constData()));
    // Stamp the coalescing window only after a successful dispatch. Rust
    // refuses while a room open is in flight, and a failed attempt must not
    // also suppress the user's own Retry, which shares this map.
    if (!result.isEmpty()) {
        qCInfo(lcRust) << "decryption retry NOT dispatched";
        qCDebug(lcE2ee) << "retry refused" << "room="
                        << matrix::e2ee::redactId(targetRoom)
                        << "reason=" << result;
        return;
    }
    m_lastDecryptionRetryMs.insert(targetRoom, now);
    // Provenance: this serves both the Retry button and automatic triggers.
    qCInfo(lcRust) << "decryption retry dispatched";
    qCDebug(lcE2ee) << "retry dispatched" << "room="
                    << matrix::e2ee::redactId(targetRoom);
}

void RustSdkMatrixClient::openThreadList(const QString &roomId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty())
        return;
    m_threadListRoom = roomId;
    m_threadListGeneration = 0;
    const QByteArray roomBytes = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_thread_list_open(m_rustHandle, roomBytes.constData()));
    if (!result.isEmpty()) {
        m_threadListRoom.clear();
        Q_EMIT threadListUpdated(roomId, {}, true, true);
    }
}

void RustSdkMatrixClient::closeThreadList()
{
    if (m_rustHandle && !m_threadListRoom.isEmpty())
        takeRustString(mx_rust_thread_list_close(m_rustHandle));
    m_threadListRoom.clear();
    m_threadListGeneration = 0;
}

void RustSdkMatrixClient::paginateThreadList(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty() || roomId != m_threadListRoom)
        return;
    const QByteArray roomBytes = roomId.toUtf8();
    takeRustString(
        mx_rust_thread_list_paginate(m_rustHandle, roomBytes.constData()));
}

void RustSdkMatrixClient::markThreadRead(const QString &roomId,
                                         const QString &rootEventId)
{
    if (!m_rustHandle || roomId.isEmpty() || rootEventId.isEmpty())
        return;
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray rootBytes = rootEventId.toUtf8();
    takeRustString(mx_rust_thread_mark_read(
        m_rustHandle, roomBytes.constData(), rootBytes.constData()));
}

void RustSdkMatrixClient::queryThreadSubscription(const QString &roomId,
                                                  const QString &rootEventId)
{
    if (!m_rustHandle || roomId.isEmpty() || rootEventId.isEmpty())
        return;
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray rootBytes = rootEventId.toUtf8();
    takeRustString(mx_rust_thread_subscription_query(
        m_rustHandle, roomBytes.constData(), rootBytes.constData()));
}

void RustSdkMatrixClient::setThreadSubscribed(const QString &roomId,
                                              const QString &rootEventId,
                                              bool subscribed)
{
    if (!m_rustHandle || roomId.isEmpty() || rootEventId.isEmpty())
        return;
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray rootBytes = rootEventId.toUtf8();
    takeRustString(mx_rust_thread_set_subscribed(
        m_rustHandle, roomBytes.constData(), rootBytes.constData(),
        subscribed ? 1 : 0));
}

void RustSdkMatrixClient::handleThreadListReset(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    if (roomId != m_threadListRoom)
        return;   // stale: list view moved to another room (or closed)
    const auto generation = static_cast<quint64>(
        event.value(QStringLiteral("thread_list_generation")).toDouble(0));
    if (generation < m_threadListGeneration)
        return;
    m_threadListGeneration = generation;

    if (event.value(QStringLiteral("type")).toString()
        == QLatin1String("thread_list_error")) {
        Q_EMIT threadListUpdated(roomId, {}, true, true);
        return;
    }

    QVariantList threads;
    const QJsonArray items = event.value(QStringLiteral("items")).toArray();
    for (const auto &value : items) {
        const QJsonObject obj = value.toObject();
        QVariantMap entry;
        entry.insert(QStringLiteral("rootEventId"),
                     obj.value(QStringLiteral("root_event_id")).toString());
        entry.insert(QStringLiteral("rootSender"),
                     obj.value(QStringLiteral("root_sender")).toString());
        entry.insert(QStringLiteral("rootSenderName"),
                     obj.value(QStringLiteral("root_sender_name")).toString(
                         matrix::user_lookup::localpartOrUserId(
                             obj.value(QStringLiteral("root_sender"))
                                 .toString())));
        entry.insert(QStringLiteral("rootPreview"),
                     obj.value(QStringLiteral("root_preview")).toString());
        entry.insert(QStringLiteral("rootTimestamp"),
                     timestampFromMs(static_cast<qint64>(
                         obj.value(QStringLiteral("root_timestamp_ms"))
                             .toDouble(0))));
        entry.insert(QStringLiteral("replyCount"),
                     obj.value(QStringLiteral("reply_count")).toInt(0));
        entry.insert(QStringLiteral("latestSender"),
                     obj.value(QStringLiteral("latest_sender")).toString());
        entry.insert(QStringLiteral("latestSenderName"),
                     obj.value(QStringLiteral("latest_sender_name")).toString(
                         matrix::user_lookup::localpartOrUserId(
                             obj.value(QStringLiteral("latest_sender"))
                                 .toString())));
        entry.insert(QStringLiteral("latestPreview"),
                     obj.value(QStringLiteral("latest_preview")).toString());
        entry.insert(QStringLiteral("latestTimestamp"),
                     timestampFromMs(static_cast<qint64>(
                         obj.value(QStringLiteral("latest_timestamp_ms"))
                             .toDouble(0))));
        threads.append(entry);
    }
    Q_EMIT threadListUpdated(
        roomId, threads,
        event.value(QStringLiteral("end_reached")).toBool(false),
        event.value(QStringLiteral("failed")).toBool(false));
}

void RustSdkMatrixClient::handleThreadSubscriptionEvent(const QString &type,
                                                        const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QString rootId =
        event.value(QStringLiteral("thread_root_id")).toString();
    if (type == QLatin1String("thread_subscription_state")) {
        Q_EMIT threadSubscriptionState(
            roomId, rootId,
            event.value(QStringLiteral("supported")).toBool(false),
            event.value(QStringLiteral("subscribed")).toBool(false),
            event.value(QStringLiteral("automatic")).toBool(false));
    } else {
        Q_EMIT threadSubscriptionResult(
            roomId, rootId, event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("subscribed")).toBool(false));
    }
}

void RustSdkMatrixClient::clearThreadTimelineState()
{
    const QString requested = m_threadTracker.requestedRoom();
    const QString active = m_threadTracker.activeRoom();
    for (const QString &timelineId : { requested, active }) {
        if (timelineId.isEmpty())
            continue;
        m_timelines.remove(timelineId);
        m_pagination.remove(timelineId);
    }
    m_threadTracker.reset();
}

// Retire the room mirror as thread mirrors are retired: trimmed back to the
// background bound, not dropped (see trimToBackgroundBound in
// RustTimelineMirror.h). A room-to-room switch never reaches
// closeRoomTimeline(), so every transition point calls this: both C++ ones,
// the close, and Rust's timeline_closed. It is idempotent.
void RustSdkMatrixClient::retireRoomTimelineMirror(const QString &roomId)
{
    if (roomId.isEmpty())
        return;
    // Never the room being read or opened: a re-open of the same room (the
    // history-trim reload, Rust's close-then-open) keeps its rows until the new
    // timeline_reset replaces them.
    if (roomId == m_timelineTracker.requestedRoom()
        || roomId == m_timelineTracker.activeRoom())
        return;
    m_pagination.remove(roomId);
    const auto it = m_timelines.find(roomId);
    if (it == m_timelines.end())
        return;
    const qsizetype before = it->size();
    if (matrix::rust_timeline::trimToBackgroundBound(*it) > 0) {
        qCDebug(lcRust) << "timeline mirror retired room="
                        << matrix::e2ee::redactId(roomId)
                        << "rows=" << before << "->" << it->size();
    }
}

void RustSdkMatrixClient::handleThreadReset(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QString rootId =
        event.value(QStringLiteral("thread_root_id")).toString();
    const QString timelineId = threadTimelineId(roomId, rootId);
    const auto generation = static_cast<quint64>(
        event.value(QStringLiteral("thread_generation")).toDouble(0));
    if (!m_threadTracker.adoptReset(timelineId, generation)) {
        qCInfo(lcRust) << "thread stale reset ignored generation="
                       << generation;
        return;
    }
    const QJsonArray items = event.value(QStringLiteral("items")).toArray();
    m_timelines[timelineId] =
        matrix::rust_timeline::eventsFromItemArray(items, timelineId);
    qCInfo(lcRust) << "thread subscription started"
                   << "thread_generation=" << generation
                   << "items=" << m_timelines[timelineId].size();
    Q_EMIT timelineReset(timelineId);
    Q_EMIT paginationStateChanged(timelineId);
}

void RustSdkMatrixClient::handleThreadDiff(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QString rootId =
        event.value(QStringLiteral("thread_root_id")).toString();
    const QString timelineId = threadTimelineId(roomId, rootId);
    const auto generation = static_cast<quint64>(
        event.value(QStringLiteral("thread_generation")).toDouble(0));
    if (!m_threadTracker.accepts(timelineId, generation)) {
        qCInfo(lcRust) << "thread stale diff ignored generation="
                       << generation;
        return;
    }

    using matrix::rust_timeline::DiffOutcome;
    auto &mirror = m_timelines[timelineId];
    const DiffOutcome outcome =
        matrix::rust_timeline::applyTimelineDiff(mirror, event, timelineId);

    switch (outcome.kind) {
    case DiffOutcome::Appended:
        for (const auto &item : outcome.items)
            Q_EMIT eventAppended(timelineId, item);
        break;
    case DiffOutcome::Prepended:
        Q_EMIT eventsPrepended(timelineId, outcome.items);
        break;
    case DiffOutcome::Inserted:
        Q_EMIT eventInsertedAt(timelineId, outcome.index, outcome.items.first());
        break;
    case DiffOutcome::Changed:
        Q_EMIT eventChangedAt(timelineId, outcome.index, outcome.items.first());
        break;
    case DiffOutcome::Removed:
        Q_EMIT eventRemovedAt(timelineId, outcome.index);
        break;
    case DiffOutcome::Cleared:
    case DiffOutcome::Reset:
        Q_EMIT timelineReset(timelineId);
        break;
    case DiffOutcome::Truncated:
        Q_EMIT eventsTruncatedTo(timelineId, outcome.length);
        break;
    case DiffOutcome::Invalid:
        // Never apply a malformed/stale thread diff; recover with a fresh
        // snapshot. No message bodies in this log line.
        qCWarning(lcRust) << "thread invalid diff rejected"
                          << "op=" << event.value(QStringLiteral("op")).toString()
                          << "mirror_size=" << mirror.size();
        openThread(roomId, rootId);
        break;
    }
}

void RustSdkMatrixClient::handleThreadPagination(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QString rootId =
        event.value(QStringLiteral("thread_root_id")).toString();
    const QString timelineId = threadTimelineId(roomId, rootId);
    const auto generation = static_cast<quint64>(
        event.value(QStringLiteral("thread_generation")).toDouble(0));
    if (!m_threadTracker.accepts(timelineId, generation)) {
        qCInfo(lcRust) << "thread stale pagination ignored generation="
                       << generation;
        return;
    }
    auto &state = m_pagination[timelineId];
    const QString paginationState =
        event.value(QStringLiteral("state")).toString();
    if (paginationState == QLatin1String("loading")) {
        state.loading = true;
        state.failed = false;
        state.failureTransient = false;
    } else if (paginationState == QLatin1String("idle")) {
        state.loading = false;
        state.failed = false;
        state.failureTransient = false;
        state.reachedStart =
            event.value(QStringLiteral("reached_start")).toBool(false);
    } else if (paginationState == QLatin1String("failed")) {
        state.loading = false;
        state.failed = true;
        const QString category =
            event.value(QStringLiteral("category")).toString();
        state.failureTransient = category == QLatin1String("network")
            || category == QLatin1String("not_ready");
        qCWarning(lcRust) << "thread pagination failed category=" << category;
    }
    Q_EMIT paginationStateChanged(timelineId);
}

void RustSdkMatrixClient::handleThreadError(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QString rootId =
        event.value(QStringLiteral("thread_root_id")).toString();
    const QString timelineId = threadTimelineId(roomId, rootId);
    const QString category = event.value(QStringLiteral("category"))
                                 .toString(QStringLiteral("unknown"));
    qCWarning(lcRust) << "thread error category=" << category;
    // Only the currently requested thread may surface the failure.
    if (m_threadTracker.requestedRoom() == timelineId
        || m_threadTracker.activeRoom() == timelineId) {
        clearThreadTimelineState();
        Q_EMIT threadTimelineFailed(roomId, rootId, category);
    }
}

void RustSdkMatrixClient::handleThreadClosed(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QString rootId =
        event.value(QStringLiteral("thread_root_id")).toString();
    const QString timelineId = threadTimelineId(roomId, rootId);
    // Drop only the closed thread's mirror; a newer thread may already be
    // requested under a different id.
    m_timelines.remove(timelineId);
    m_pagination.remove(timelineId);
    if (m_threadTracker.activeRoom() == timelineId
        || m_threadTracker.requestedRoom() == timelineId)
        m_threadTracker.reset();
    qCInfo(lcRust) << "thread subscription stopped";
}

void RustSdkMatrixClient::closeRoomTimeline()
{
    // Thread timelines never outlive their room; Rust closes them on timeline
    // close/open and the C++ mirrors drop now.
    clearThreadTimelineState();
    m_threadListRoom.clear();
    m_threadListGeneration = 0;
    const QString closingRequested = m_timelineTracker.requestedRoom();
    const QString closingActive = m_timelineTracker.activeRoom();
    if (m_rustHandle && m_timelineTracker.hasActiveTimeline()) {
        takeRustString(mx_rust_timeline_close(m_rustHandle));
        qCInfo(lcRust) << "timeline close room="
                       << matrix::e2ee::redactId(closingActive);
    }
    // After reset(): retireRoomTimelineMirror() refuses the room the tracker
    // still names, which is the room being closed.
    m_timelineTracker.reset();
    retireRoomTimelineMirror(closingRequested);
    retireRoomTimelineMirror(closingActive);
}

void RustSdkMatrixClient::refuseSend(const char *op)
{
    // Genuinely unimplemented; only sendImage and sendFile reach this.
    qCWarning(lcRust) << "send refused: not implemented" << op;
    Q_EMIT errorOccurred(
        tr("Lightning cannot send that yet."));
}

void RustSdkMatrixClient::refuseUntilTimelineReady(const char *op)
{
    // These operations need a live SDK timeline for the room, which is
    // transient while a room opens; they are implemented. Say so and suggest
    // retrying.
    qCWarning(lcRust) << "send refused: no live timeline for the room" << op
                      << "active=" << m_timelineTracker.activeRoom()
                      << "requested=" << m_timelineTracker.requestedRoom();
    Q_EMIT errorOccurred(
        tr("This room is still loading. Try that again in a moment."));
}

void RustSdkMatrixClient::pollRustEvents()
{
    // Stall attribution (no-op unless LIGHTNING_GUI_STALL_TRACE is set). The
    // drain applies every queued diff on the GUI thread.
    stalltrace::Scope stallScope("rust-poll-drain");

    // Phase A events first, unconditionally: a browser sign-in runs from the
    // login screen, where there is no session handle.
    drainAuthEvents();

    if (!m_rustHandle)
        return;

    const quint64 eventGeneration = m_handleGeneration;

    // Drain the terminal command lane completely before the bounded bulk batch
    // so media/GIF results are never starved behind a diff flood. The lane is
    // bounded by the C++ in-flight discipline; 256 is a defensive cap.
    for (int i = 0; i < 256; ++i) {
        const QByteArray raw =
            takeRustBytes(mx_rust_poll_command_event(m_rustHandle));
        if (raw.isEmpty())
            break;
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isObject()) {
            qCWarning(lcRust) << "discarding malformed Rust SDK command event";
            continue;
        }
        const QJsonObject event = doc.object();
        if (m_lifecycle.acceptsActive(eventGeneration)) {
            handleRustEvent(event, eventGeneration);
        } else {
            qCInfo(lcRust) << "ignored stale command callback"
                           << "type="
                           << event.value(QStringLiteral("type")).toString();
        }
    }

    // Read once per tick; the drain runs up to 256 times.
    const bool traceSync = synctrace::enabled();

    m_coalesceTimelineInserts = true;
    // Soft fairness cap per tick with a bounded extension for timeline diffs:
    // one SDK transaction arrives as adjacent diffs (a receipt move is two
    // Sets), and splitting them across ticks paints a visible flicker. Past the
    // soft cap the drain continues only while timeline diffs keep coming. A
    // pair can still straddle the hard cap, which only happens in
    // hydration-scale bursts.
    constexpr int kSoftDrainCap = 64;
    constexpr int kHardDrainCap = 256;
    for (int i = 0; i < kHardDrainCap; ++i) {
        const QByteArray raw = takeRustBytes(mx_rust_poll_event(m_rustHandle));
        if (raw.isEmpty())
            break;

        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isObject()) {
            qCWarning(lcRust) << "discarding malformed Rust SDK event";
            continue;
        }
        const QJsonObject event = doc.object();
        const QString type = event.value(QStringLiteral("type")).toString();
        // Only consecutive room-timeline diffs can share a transaction;
        // preserve ordering across every other callback.
        if (type != QLatin1String("timeline_diff"))
            flushTimelineInsertBatch();
        if (m_lifecycle.acceptsActive(eventGeneration)) {
            handleRustEvent(event, eventGeneration);
        } else if (type == QLatin1String("logged_out")
                   && m_lifecycle.acceptsShutdownCompletion(eventGeneration)) {
            handleRustEvent(event, eventGeneration);
            m_coalesceTimelineInserts = false;
            return; // finishSignOut releases the handle being polled.
        } else {
            qCInfo(lcRust) << "ignored stale callback"
                           << "type=" << type
                           << "generation=" << eventGeneration
                           << "active_generation="
                           << m_lifecycle.activeGeneration();
        }
        // Sync liveness is recorded on drained events, never on the timer tick,
        // which would report a healthy 100 ms gap through a total outage. The
        // gap between real events shows whether a dead connection went
        // unnoticed.
        if (traceSync)
            synctrace::noteSyncResponse();
        if (i >= kSoftDrainCap - 1 && type != QLatin1String("timeline_diff"))
            break;
    }
    flushTimelineInsertBatch();
    m_coalesceTimelineInserts = false;
}

void RustSdkMatrixClient::requireLocalReset(
    const QString &reasonCode,
    const matrix::app_data::AccountIdentity &identity)
{
    // Slug only; the user id travels in the signal but never reaches the log.
    qCWarning(lcRust) << "local session reset required reason=" << reasonCode
                      << "slug=" << matrix::app_data::safeUserSlug(identity.userId);
    Q_EMIT localSessionResetRequired(reasonCode, identity.userId,
                                     identity.homeserver);
}

void RustSdkMatrixClient::failWithBlockReason(
    matrix::rust_session::StoreBlockReason reason,
    const matrix::app_data::AccountIdentity &identity)
{
    qCWarning(lcRust) << "local session open blocked"
                      << "detail=" << matrix::rust_session::diagnosticName(reason)
                      << "slug=" << identity.slug;
    // Only conditions a local reset can repair arm the destructive recovery UI.
    // A missing store has nothing to delete; a revoked token needs a new
    // sign-in.
    if (matrix::rust_session::suggestsLocalReset(reason)) {
        requireLocalReset(matrix::rust_session::diagnosticName(reason), identity);
    } else {
        Q_EMIT localSessionBlocked(
            matrix::rust_session::diagnosticName(reason),
            identity.userId, identity.homeserver);
    }
    setState(Error);
    Q_EMIT loginFailed(matrix::rust_session::userMessage(reason));
}

void RustSdkMatrixClient::finishSignOut(const QString &serverResult,
                                        const QString &serverMessage)
{
    const auto identity = m_signOutIdentity;
    if (serverResult == QLatin1String("already_invalid")) {
        qCInfo(lcRust) << "rust server logout result=already_logged_out";
    } else if (serverResult == QLatin1String("failed")) {
        // Diagnostic only: local cleanup is authoritative and the user is
        // signing out anyway.
        qCWarning(lcRust) << "rust server logout result=failed"
                          << "message=" << serverMessage;
    } else {
        qCInfo(lcRust) << "rust server logout result=" << serverResult;
    }

    releaseRustHandle();
    clearLocalState();
    // Use matchedRecord rather than the discarding overload: "target absent"
    // and "reset completed" are different outcomes and must not both report
    // success.
    bool matchedRecord = false;
    const bool sessionOk =
        identity.isValid() && clearPersistedAccount(identity, &matchedRecord);
    const auto files = identity.isValid()
        ? matrix::app_data::removeAccountRustState(identity)
        : matrix::app_data::RemovalSummary{0, 0, 1};
    const bool didSomething = matchedRecord || files.removedAnything();
    const bool ok = sessionOk && files.ok() && didSomething;

    qCInfo(lcRust) << "rust local sign-out cleanup"
                   << "slug=" << identity.slug
                   << "matched_record=" << matchedRecord
                   << "did_something=" << didSomething
                   << "session=" << (sessionOk ? "ok" : "failed")
                   << "store_and_sidecars=" << (files.ok() ? "ok" : "failed")
                   << "deleted=" << files.deleted
                   << "missing=" << files.missing
                   << "failed=" << files.failed;

    m_storePath.clear();
    m_signOutIdentity = {};
    m_signOutDeviceId.clear();
    m_lifecycle.finishSignOut();

    Q_EMIT loggedOut();
    if (ok) {
        Q_EMIT localSessionCleanupFinished(
            true, tr("Local Lightning session reset. You can sign in again."));
    } else {
        requireLocalReset(QStringLiteral("cleanup_incomplete"), identity);
        const QString failure = tr(
            "Lightning could not completely reset the local session for this "
            "account. Check the application logs and filesystem permissions, "
            "then try again.");
        Q_EMIT localSessionCleanupFinished(false, failure);
        Q_EMIT errorOccurred(failure);
    }
}

void RustSdkMatrixClient::handleRustEvent(const QJsonObject &event,
                                          quint64 eventGeneration)
{
    if (!m_lifecycle.acceptsActive(eventGeneration)
        && !m_lifecycle.acceptsShutdownCompletion(eventGeneration)) {
        qCInfo(lcRust) << "ignored stale callback"
                       << "generation=" << eventGeneration
                       << "active_generation=" << m_lifecycle.activeGeneration();
        return;
    }

    const QString type = event.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("status")) {
        const QString state = event.value(QStringLiteral("state")).toString();
        if (state == QLatin1String("connecting"))
            setState(Connecting);
        else if (state == QLatin1String("syncing"))
            setState(Syncing);
        else if (state == QLatin1String("disconnected"))
            setState(Disconnected);
        else if (state == QLatin1String("error"))
            setState(Error);
        return;
    }

    if (type == QLatin1String("session_tokens_refreshed")) {
        // SENSITIVE: carries the rotated access and refresh tokens; never log
        // `event`. If not written back, the store keeps the consumed refresh
        // token, and presenting it again can get the whole session revoked.
        // Keyed on the canonical active-account id: saveSession() writes
        // secrets under it, while m_userId may be the server's raw answer.
        // Writing under the raw id would leave the consumed token where it is
        // read.
        const QString tokenOwner = m_settings ? m_settings->userId() : QString{};
        if (m_settings && !tokenOwner.isEmpty()) {
            m_settings->updateSessionTokens(
                tokenOwner,
                event.value(QStringLiteral("access_token")).toString(),
                event.value(QStringLiteral("refresh_token")).toString());
        }
        return;
    }

    if (type == QLatin1String("session_token_revoked")) {
        // The credential died and the SDK could not renew it. Surface the
        // revoked-credential state instead of looping sync failures. The local
        // store is fine, so this must not arm a reset (as with M_UNKNOWN_TOKEN
        // below).
        qCInfo(lcRust) << "session credential rejected and could not be renewed"
                       << "slug=" << m_openingIdentity.slug;
        Q_EMIT localSessionBlocked(
            matrix::rust_session::diagnosticName(
                matrix::rust_session::StoreBlockReason::AccessTokenRevoked),
            m_openingIdentity.userId, m_openingIdentity.homeserver);
        Q_EMIT loginFailed(matrix::rust_session::userMessage(
            matrix::rust_session::StoreBlockReason::AccessTokenRevoked));
        return;
    }

    if (type == QLatin1String("session_restored_offline")) {
        // The session is real but the server did not answer. Enqueued before
        // login_ok, so the connection state starts honest. Nothing account-
        // identifying is logged.
        qCInfo(lcRust) << "session restored from the local store — the "
                          "homeserver could not be reached; rooms and "
                          "messages are the cached copy and sync will retry";
        m_restoredOffline = true;
        return;
    }

    if (type == QLatin1String("login_ok")) {
        m_freshLoginIdentity = {};
        // SENSITIVE: this event carries `access_token`. Never log `event` or
        // `accessToken`; the token goes only to SettingsManager::saveSession.
        matrix::app_data::AccountIdentity identity;
        if (matrix::app_data::resolveAccountIdentity(
                event.value(QStringLiteral("homeserver")).toString(m_homeserver),
                event.value(QStringLiteral("user_id")).toString(m_userId),
                &identity)) {
            m_homeserver = identity.homeserver;
        }
        m_userId = event.value(QStringLiteral("user_id")).toString(m_userId);
        m_deviceId = event.value(QStringLiteral("device_id")).toString(m_deviceId);
        m_loggedIn = !m_userId.isEmpty();
        const QString accessToken = event.value(QStringLiteral("access_token")).toString();
        // SENSITIVE: a refresh token mints access tokens; it goes straight to
        // the SecretStore and is never logged. Absent for non-refreshable
        // sessions and on restore.
        const QString refreshToken = event.value(QStringLiteral("refresh_token")).toString();
        if (m_loggedIn && m_settings && !accessToken.isEmpty()) {
            // Password login and restore only; OAuth sessions are saved by
            // Phase B with the "oauth" auth type and client id.
            m_settings->saveSession(m_homeserver, m_userId, m_deviceId, accessToken,
                                    refreshToken);
            m_settings->setSyncToken({});
        }
        // The homeserver decides the canonical user id, so a first login may
        // have created the store under the typed casing (or the delegated URL
        // host) while the record uses the server's answer. Record where the
        // store really is before anything derives a path from the record.
        // Nothing is moved.
        if (m_loggedIn && !accessToken.isEmpty() && identity.isValid()
            && identity.userId == m_userId) {
            recordStoreLocation(identity);
        }
        // Offline unless the restore reached the server; otherwise the footer
        // shows "Loading rooms…" over a complete list that will never load.
        setState(m_restoredOffline ? Offline : Disconnected);
        if (m_loggedIn)
            Q_EMIT loginSucceeded(m_userId);
        else
            Q_EMIT loginFailed(tr("Rust SDK login response did not include a user id."));
        return;
    }

    if (type == QLatin1String("login_failed")) {
        m_loggedIn = false;
    // Log dedupe is per session, not per process, so a second broken account in
    // the same run still logs. Reset wherever a session ends.
    m_ownIdentityKeyMismatchLogged = false;
        setState(Error);
        // A failed fresh-store login must not leave a half-initialised store
        // that poisons later attempts. Release the handle first so no SDK task
        // owns it. The marker can outlive its attempt (detachSession drops that
        // attempt's terminal event as stale), so only act if it names the store
        // the live handle actually opened; otherwise a later failure could
        // delete another account's store.
        const bool freshMatchesThisAttempt =
            m_freshLoginIdentity.isValid() && !m_storePath.isEmpty()
            && QFileInfo(m_freshLoginIdentity.rustStorePath).absoluteFilePath()
                   == QFileInfo(m_storePath).absoluteFilePath();
        if (m_freshLoginIdentity.isValid() && !freshMatchesThisAttempt) {
            qCWarning(lcRust)
                << "discarding a fresh-store marker that names a different "
                   "account than this attempt; no store was removed";
            m_freshLoginIdentity = {};
        }
        if (m_freshLoginIdentity.isValid()) {
            const auto identity = m_freshLoginIdentity;
            m_freshLoginIdentity = {};
            releaseRustHandle();
            const auto removed =
                matrix::app_data::removeAccountRustState(identity);
            qCInfo(lcRust) << "cleaned fresh store after failed login"
                           << "slug=" << identity.slug
                           << "deleted=" << removed.deleted
                           << "failed=" << removed.failed;
        }
        const QString message = event.value(QStringLiteral("message")).toString(
            tr("Rust SDK login failed."));
        if (matrix::rust_session::isStoreOwnershipMismatch(message)) {
            // The SDK is the authority on store ownership: drop a
            // divergent-directory recording it just rejected so the next start
            // re-evaluates. Only the mapping is cleared; no store is touched.
            if (m_settings && !m_openingIdentity.userId.isEmpty()
                && m_openingIdentity.storeSlug != m_openingIdentity.slug) {
                qCWarning(lcRust) << "clearing rejected store recording"
                                  << "account=" << m_openingIdentity.slug;
                m_settings->setStoreSlugFor(m_openingIdentity.userId, QString{});
            }
            requireLocalReset(QStringLiteral("sdk_store_ownership_mismatch"),
                              m_openingIdentity);
            Q_EMIT loginFailed(matrix::rust_session::userMessage(
                matrix::rust_session::StoreBlockReason::DifferentAccount));
        } else if (matrix::rust_session::isUnknownToken(message)) {
            // The homeserver revoked this session. The local store is fine, and
            // offering to delete it would destroy key material the user still
            // needs.
            qCInfo(lcRust) << "saved session rejected by the homeserver"
                           << "slug=" << m_openingIdentity.slug;
            Q_EMIT localSessionBlocked(
                matrix::rust_session::diagnosticName(
                    matrix::rust_session::StoreBlockReason::AccessTokenRevoked),
                m_openingIdentity.userId, m_openingIdentity.homeserver);
            Q_EMIT loginFailed(matrix::rust_session::userMessage(
                matrix::rust_session::StoreBlockReason::AccessTokenRevoked));
        } else {
            Q_EMIT loginFailed(message);
        }
        return;
    }

    if (type == QLatin1String("logged_out")) {
        m_callSdpStore.clear();
        finishSignOut(event.value(QStringLiteral("result")).toString(
                          QStringLiteral("ok")),
                      event.value(QStringLiteral("message")).toString());
        return;
    }

    if (type == QLatin1String("rooms") || type == QLatin1String("room_list_reset")) {
        handleRoomsEvent(event.value(QStringLiteral("rooms")).toArray());
        return;
    }

    if (type == QLatin1String("room_snapshot")) {
        handleRoomSnapshotEvent(event.value(QStringLiteral("rooms")).toArray());
        return;
    }

    // Response-harvested recency (harvest_room_activity in rust/src/lib.rs).
    // Timestamps only; it may raise a room's sort key and nothing else.
    // roomUpdated, not roomsChanged: the registry order is untouched, and a
    // structural refresh would re-resolve every avatar on each sync response.
    if (type == QLatin1String("room_activity")) {
        const QStringList moved = matrix::rust_rooms::applyRoomActivity(
            {m_rooms, m_roomOrder},
            event.value(QStringLiteral("rooms")).toArray());
        for (const QString &roomId : moved)
            Q_EMIT roomUpdated(roomId);
        return;
    }

    if (type.startsWith(QLatin1String("room_list_"))
        && type != QLatin1String("room_list_mode")
        && type != QLatin1String("room_list_sync_state")
        && type != QLatin1String("room_list_error")) {
        handleRoomListDiff(event);
        return;
    }

    if (type == QLatin1String("latest_event_watch_report")) {
        // Counts and timing only; evidence for tuning LATEST_EVENT_WATCH_CAP.
        qCDebug(lcRust) << "latest-event watch reconcile:"
                        << "elapsed_ms=" << event.value(QStringLiteral("elapsed_ms")).toInt()
                        << "watched=" << event.value(QStringLiteral("watched")).toInt()
                        << "of rooms=" << event.value(QStringLiteral("rooms")).toInt()
                        << "buckets=" << event.value(QStringLiteral("buckets")).toInt()
                        << "added=" << event.value(QStringLiteral("added")).toInt()
                        << "forgot=" << event.value(QStringLiteral("forgot")).toInt();
        return;
    }

    if (type == QLatin1String("room_list_mode")) {
        const QString mode = event.value(QStringLiteral("mode")).toString();
        if (!mode.isEmpty() && mode != m_syncMode) {
            m_syncMode = mode;
            qCInfo(lcRust) << "room_list mode=" << mode;
            Q_EMIT syncModeChanged();
        }
        return;
    }

    if (type == QLatin1String("room_list_sync_state")) {
        const QString state = event.value(QStringLiteral("state")).toString();
        // The classic path re-announces "running" on every /sync callback.
        // Collapse repeats; distinct transitions are never coalesced, so
        // reconnects still show.
        if (state == m_lastSyncState)
            return;
        m_lastSyncState = state;
        qCInfo(lcRust) << "sync state=" << state;
        // Literal state names only — never server text.
        if (state == QLatin1String("running"))
            synctrace::noteSyncState("running");
        else if (state == QLatin1String("offline"))
            synctrace::noteSyncState("offline");
        else if (state == QLatin1String("retrying"))
            synctrace::noteSyncState("retrying");
        else if (state == QLatin1String("starting"))
            synctrace::noteSyncState("starting");
        // A response arrived: the server is reachable, so release the Offline
        // override.
        if (state == QLatin1String("running"))
            m_restoredOffline = false;
        if (state == QLatin1String("offline")) setState(Offline);
        else if (state == QLatin1String("starting") || state == QLatin1String("retrying"))
            setState(Syncing);
        else if (state == QLatin1String("running")) setState(Syncing);
        return;
    }

    if (type == QLatin1String("room_list_error")) {
        const QString category = event.value(QStringLiteral("category")).toString();
        if (category == QLatin1String("authentication")) {
            setState(Error);
            Q_EMIT errorOccurred(tr("Matrix session is no longer authorized."));
            return;
        }
        // Every other category is logged too, so a sync that could not start is
        // distinguishable from a slow one. The category is a Rust literal,
        // never server text.
        qCWarning(lcRust) << "room_list error category=" << category
                          << "— the sync supervisor is retrying";
        return;
    }

    if (type == QLatin1String("space_list_reset")) {
        handleSpacesEvent(event.value(QStringLiteral("spaces")).toArray());
        return;
    }

    if (type == QLatin1String("typing_update")) {
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        auto room = m_rooms.find(roomId);
        if (room == m_rooms.end()) return;
        QStringList users;
        for (const auto &entry : event.value(QStringLiteral("users")).toArray()) {
            const auto object = entry.toObject();
            const QString userId = object.value(QStringLiteral("user_id")).toString();
            if (userId.isEmpty() || userId == m_userId) continue;
            users.append(userId);
            const QString displayName = object.value(QStringLiteral("display_name")).toString();
            if (!displayName.isEmpty()) {
                auto member = room->members.value(userId);
                member.userId = userId;
                member.displayName = displayName;
                room->members.insert(userId, member);
            }
        }
        room->typingUserIds = users;
        Q_EMIT typingChanged(roomId);
        return;
    }

    if (type == QLatin1String("room_members_changed")) {
        // Membership poke: reaches only the roster-refetch consumers via
        // roomMemberEventSeen, never membersChanged (which repaints every
        // loaded row). The m.room.member handler in Rust is limited to one poke
        // per room per second. m.room.power_levels reuses this event
        // unthrottled; it is human-paced and RoomInfoController single-flights
        // its refetch.
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        if (m_rooms.contains(roomId))
            Q_EMIT roomMemberEventSeen(roomId);
        return;
    }

    if (type == QLatin1String("room_pinned_changed")) {
        // m.room.pinned_events changed remotely. No payload: the consumer
        // re-reads the authoritative list, so remote and local pins share one
        // path.
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        if (m_rooms.contains(roomId))
            Q_EMIT pinnedEventsChanged(roomId);
        return;
    }

    if (type == QLatin1String("room_tombstone_changed")) {
        // This room was replaced. Unlike the pinned poke this carries the
        // successor, which Rust took from the SDK's typed accessor. Nothing
        // follows the upgrade here; joining happens only when the user
        // activates the banner.
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        auto room = m_rooms.find(roomId);
        if (room == m_rooms.end()) return;
        const QString successor =
            event.value(QStringLiteral("successor_room_id")).toString();
        if (room->successorRoomId == successor) return;
        room->successorRoomId = successor;
        Q_EMIT roomsChanged();
        return;
    }

    if (type == QLatin1String("invite_state_update")) {
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        auto room = m_rooms.find(roomId);
        if (room == m_rooms.end()) return;
        const QString state = event.value(QStringLiteral("state")).toString();
        room->invitePending = state == QLatin1String("pending");
        room->inviteError = state == QLatin1String("failed")
            ? tr("Invite action failed. Try again.") : QString{};
        Q_EMIT roomUpdated(roomId);
        return;
    }

    if (type == QLatin1String("room_action_error")) {
        const QString action = event.value(QStringLiteral("action")).toString();
        if (action == QLatin1String("read_receipt"))
            m_lastReceiptSent.remove(event.value(QStringLiteral("room_id")).toString());
        qCWarning(lcRust) << "room action failed category=" << action;
        // A write the user asked for must not fail silently: an unchanged list
        // is indistinguishable from a slow sync. read_receipt stays silent on
        // purpose; see matrix/RoomActionError.h.
        const QString message = matrix::room_action::userFacingError(action);
        if (!message.isEmpty())
            Q_EMIT errorOccurred(message);
        return;
    }

    // Server push-rule state for one room: an explicit user-defined rule or the
    // resolved account default. Mode integers only, no rule JSON.
    if (type == QLatin1String("room_notification_mode")) {
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        const int mode = event.value(QStringLiteral("mode")).toInt(-1);
        if (roomId.isEmpty()) return;
        // "Follow account default" succeeded: the room's rules were removed.
        // That is a distinct outcome from a rule write, so it has its own
        // signal; routing it through roomNotificationModeChanged would drop it
        // or reconcile a rule with value 3, and a once-failed clear could never
        // be acknowledged.
        if (event.value(QStringLiteral("followed_default")).toBool()) {
            Q_EMIT roomNotificationModeCleared(roomId);
            return;
        }
        if (mode < 0 || mode > 2) return;
        Q_EMIT roomNotificationModeChanged(
            roomId, mode,
            event.value(QStringLiteral("user_defined")).toBool());
        return;
    }

    // A push-rule write failed. The device-local mode is kept; the signal lets
    // the pickers say it is saved on this device only.
    if (type == QLatin1String("notification_mode_error")) {
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        if (roomId.isEmpty()) return;
        qCWarning(lcRust) << "room action failed category= notification_mode";
        Q_EMIT roomNotificationModeWriteFailed(roomId);
        return;
    }

    if (type == QLatin1String("initial_sync_done")) {
        setInitialSyncDone(true);
        // Fetch the server upload limit once per session so the composer can
        // enforce m.upload.size before dispatching.
        if (!m_uploadLimitRequested && m_rustHandle) {
            m_uploadLimitRequested = true;
            takeRustString(mx_rust_fetch_upload_limit(m_rustHandle));
        }
        return;
    }

    if (type == QLatin1String("timeline_event")) {
        handleTimelineEvent(event);
        return;
    }

    // Live SDK timeline events.
    if (type == QLatin1String("timeline_reset")) {
        handleTimelineReset(event);
        return;
    }
    if (type == QLatin1String("timeline_diff")) {
        // Sync-latency tracing at the sdk->bridge boundary, stamped on the Rust
        // side. No-op unless LIGHTNING_SYNC_TRACE is set.
        if (synctrace::enabled()) {
            const quint64 traceId = synctrace::beginEvent(
                event.value(QStringLiteral("room_id")).toString(),
                static_cast<qint64>(
                    event.value(QStringLiteral("trace_sdk_ms")).toDouble()));
            synctrace::noteBridge(traceId);
            handleTimelineDiff(event);
            // The model has the row now.
            synctrace::noteModel(traceId);
            // The UI stage is "the GUI thread finished this event-loop
            // iteration", via a queued call. A proxy for presentation, not a
            // frame callback; per-frame timing belongs to QSG_RENDER_TIMING.
            QMetaObject::invokeMethod(this, [traceId] {
                synctrace::noteUi(traceId);
            }, Qt::QueuedConnection);
            return;
        }
        handleTimelineDiff(event);
        return;
    }
    if (type == QLatin1String("timeline_pagination")) {
        handleTimelinePagination(event);
        return;
    }
    // SDK-backed thread timeline events.
    if (type == QLatin1String("thread_reset")) {
        handleThreadReset(event);
        return;
    }
    if (type == QLatin1String("thread_diff")) {
        handleThreadDiff(event);
        return;
    }
    if (type == QLatin1String("thread_pagination")) {
        handleThreadPagination(event);
        return;
    }
    if (type == QLatin1String("thread_error")) {
        handleThreadError(event);
        return;
    }
    if (type == QLatin1String("thread_closed")) {
        handleThreadClosed(event);
        return;
    }
    if (type == QLatin1String("device_list")) {
        QVariantList devices;
        const QJsonArray items = event.value(QStringLiteral("devices")).toArray();
        for (const auto &value : items) {
            const QJsonObject obj = value.toObject();
            QVariantMap entry;
            entry.insert(QStringLiteral("deviceId"),
                         obj.value(QStringLiteral("device_id")).toString());
            entry.insert(QStringLiteral("displayName"),
                         obj.value(QStringLiteral("display_name")).toString());
            const auto ts = static_cast<qint64>(
                obj.value(QStringLiteral("last_seen_ts")).toDouble(0));
            entry.insert(QStringLiteral("lastSeen"),
                         ts > 0 ? QDateTime::fromMSecsSinceEpoch(ts, Qt::UTC)
                                : QDateTime{});
            entry.insert(QStringLiteral("lastSeenIp"),
                         obj.value(QStringLiteral("last_seen_ip")).toString());
            entry.insert(QStringLiteral("isCurrent"),
                         obj.value(QStringLiteral("is_current")).toBool(false));
            entry.insert(QStringLiteral("hasCryptoIdentity"),
                         obj.value(QStringLiteral("has_crypto_identity"))
                             .toBool(false));
            entry.insert(QStringLiteral("verified"),
                         obj.value(QStringLiteral("verified")).toBool(false));
            entry.insert(QStringLiteral("crossSigned"),
                         obj.value(QStringLiteral("cross_signed")).toBool(false));
            devices.append(entry);
        }
        Q_EMIT deviceListUpdated(
            event.value(QStringLiteral("ok")).toBool(false), devices);
        return;
    }
    if (type == QLatin1String("to_device_undecryptable")) {
        // Separates "never arrived" from "arrived but this device cannot open
        // it". matrix-sdk passes an undecryptable to-device event through as
        // the raw `m.room.encrypted` envelope, which no handler listens for.
        // Sanitized: a public Matrix id and a count only. The Rust handler
        // cannot say which SDK cause it was.
        qCWarning(lcRust)
            << "a to-device message could not be decrypted sender="
            << event.value(QStringLiteral("sender")).toString()
            << "count="
            << static_cast<qint64>(
                   event.value(QStringLiteral("count")).toDouble(0))
            << "(if these are call media keys the call is silent one way "
               "while messages keep working)";
        return;
    }
    if (type == QLatin1String("own_identity_key")) {
        // A device whose published key is not its own cannot decrypt anything:
        // peers encrypt to the server's copy, so room keys and call media keys
        // addressed to us are unreadable while sending still works. Absent
        // means "could not be established" and is not a fault; only an explicit
        // false is.
        const QJsonValue matches =
            event.value(QStringLiteral("matches_server"));
        const bool established = matches.isBool();
        const bool agrees = established && matches.toBool();
        if (established && !agrees) {
            // Once per transition, not per check, so the periodic backstop does
            // not flood the log.
            if (!m_ownIdentityKeyMismatchLogged) {
                m_ownIdentityKeyMismatchLogged = true;
                qCCritical(lcRust)
                    << "this device's published identity key does not match "
                       "its local account. Nothing encrypted to this device "
                       "can be decrypted, so encrypted messages will not open "
                       "and encrypted calls will be silent in one direction. "
                       "Signing out and signing in again is the repair.";
            }
        } else if (agrees) {
            m_ownIdentityKeyMismatchLogged = false;
        }
        Q_EMIT ownDeviceIdentityKeyChecked(established, agrees);
        return;
    }
    if (type == QLatin1String("crypto_health")) {
        // Forward verbatim (sanitized in Rust); AppController stamps the
        // generation.
        QVariantMap snapshot = event.toVariantMap();
        snapshot.remove(QStringLiteral("type"));
        Q_EMIT cryptoHealthUpdated(snapshot);
        return;
    }
    if (type == QLatin1String("crypto_bootstrap")) {
        // Sanitized observer state; the poll layer already rejected stale
        // handles.
        Q_EMIT cryptoBootstrapEvent(
            event.value(QStringLiteral("kind")).toString(),
            event.value(QStringLiteral("state")).toString(),
            static_cast<quint64>(
                event.value(QStringLiteral("count")).toDouble(0)),
            static_cast<quint64>(
                event.value(QStringLiteral("inconclusive")).toDouble(0)));
        return;
    }
    if (type == QLatin1String("thread_list_reset")
        || type == QLatin1String("thread_list_error")) {
        handleThreadListReset(event);
        return;
    }
    if (type == QLatin1String("thread_subscription_state")
        || type == QLatin1String("thread_subscription_result")) {
        handleThreadSubscriptionEvent(type, event);
        return;
    }
    if (type == QLatin1String("thread_send_failed")) {
        const QString category =
            event.value(QStringLiteral("category")).toString();
        qCWarning(lcRust) << "thread send state=failed category=" << category;
        // The category decides the wording, so a reaction, sticker or poll vote
        // in a thread is not reported as a failed thread reply. edit_rejected
        // is deliberately not handled here: thread edits emit
        // timeline_send_failed, whose branch carries "The edit could not be
        // applied."
        if (category == QLatin1String("reaction_rejected")) {
            Q_EMIT errorOccurred(tr("The reaction could not be applied."));
            return;
        }
        if (category == QLatin1String("sticker_send_failed")) {
            Q_EMIT errorOccurred(tr("The sticker could not be sent."));
            return;
        }
        if (category.startsWith(QLatin1String("poll_"))) {
            Q_EMIT errorOccurred(tr("The poll action could not be completed."));
            return;
        }
        Q_EMIT errorOccurred(tr("The thread reply could not be sent."));
        return;
    }
    if (type == QLatin1String("timeline_retry_decryption")) {
        handleTimelineRetryDecryption(event);
        return;
    }
    if (type == QLatin1String("timeline_send_failed")) {
        const QString category =
            event.value(QStringLiteral("category")).toString(
                QStringLiteral("rejected"));
        qCWarning(lcRust) << "timeline send state=failed category=" << category;
        if (category == QLatin1String("reaction_rejected")) {
            // There is no Retry for a reaction, so the message must not point
            // at one.
            Q_EMIT errorOccurred(tr("The reaction could not be applied."));
            return;
        }
        if (category == QLatin1String("redact_rejected")) {
            Q_EMIT errorOccurred(tr("The message could not be deleted."));
            return;
        }
        // Cancel categories are not send failures. `cancel_too_late` means the
        // message did reach the server, so the fallback's "could not be sent,
        // retry" would be wrong twice.
        if (category == QLatin1String("cancel_too_late")) {
            Q_EMIT errorOccurred(tr("That message had already been sent, so it "
                                    "could not be cancelled."));
            return;
        }
        if (category == QLatin1String("cancel_failed")
            || category == QLatin1String("cancel_target_missing")) {
            Q_EMIT errorOccurred(tr("That message could not be cancelled."));
            return;
        }
        // The user's Retry just failed, so don't tell them to use Retry.
        if (category == QLatin1String("retry_target_missing")) {
            Q_EMIT errorOccurred(tr("That message is no longer available to "
                                    "retry."));
            return;
        }
        // Neither has a Retry affordance, so the fallback's advice cannot help.
        if (category == QLatin1String("edit_rejected")) {
            Q_EMIT errorOccurred(tr("The edit could not be applied."));
            return;
        }
        if (category == QLatin1String("sticker_send_failed")) {
            Q_EMIT errorOccurred(tr("The sticker could not be sent."));
            return;
        }
        if (category.startsWith(QLatin1String("poll_"))) {
            Q_EMIT errorOccurred(tr("The poll action could not be completed."));
            return;
        }
        Q_EMIT errorOccurred(tr("Message could not be sent. You can retry "
                                "from the message's Retry action."));
        return;
    }
    if (type == QLatin1String("timeline_error")) {
        const QString category =
            event.value(QStringLiteral("category")).toString(
                QStringLiteral("unknown"));
        qCWarning(lcRust) << "timeline error category=" << category;
        if (category != QLatin1String("unknown_room")) {
            Q_EMIT errorOccurred(tr("The room timeline could not be opened."));
        }
        return;
    }
    if (type == QLatin1String("timeline_closed")
        || type == QLatin1String("timeline_shutdown")) {
        qCInfo(lcRust) << "timeline subscription stopped"
                       << "kind=" << type;
        // Rust closes the previous room's timeline on every open and on
        // explicit close, naming it in `room_id` (timeline_shutdown carries
        // none). This is the authoritative retirement point for the C++ mirror;
        // it refuses the room the tracker wants, so a close-then-open of the
        // same room keeps its rows.
        retireRoomTimelineMirror(
            event.value(QStringLiteral("room_id")).toString());
        return;
    }

    if (type == QLatin1String("send_ok")) {
        handleSendOk(event);
        return;
    }

    if (type == QLatin1String("send_failed")) {
        handleSendFailed(event);
        return;
    }

    if (type == QLatin1String("encrypted_send_ok")) {
        handleEncryptedSendOk(event);
        return;
    }

    if (type == QLatin1String("encrypted_send_failed")) {
        handleEncryptedSendFailed(event);
        return;
    }

    if (type == QLatin1String("key_backup_status")) {
        const QString state = event.value(QStringLiteral("state")).toString();
        const QString message = event.value(QStringLiteral("message")).toString();
        Q_EMIT keyBackupResult(state, message);
        return;
    }

    if (type == QLatin1String("reload_timeline_done")) {
        Q_EMIT roomTimelineReloaded(
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("events")).toInt(0),
            event.value(QStringLiteral("decrypted")).toInt(0),
            event.value(QStringLiteral("undecryptable")).toInt(0));
        return;
    }

    if (type == QLatin1String("reload_timeline_failed")) {
        Q_EMIT errorOccurred(tr("Reload timeline failed: %1").arg(
            event.value(QStringLiteral("message")).toString(
                tr("Matrix Rust SDK error."))));
        return;
    }

    if (type == QLatin1String("verification_request_received")) {
        Q_EMIT verificationRequestReceived(
            event.value(QStringLiteral("flow_id")).toString(),
            event.value(QStringLiteral("other_user_id")).toString(),
            event.value(QStringLiteral("other_device_id")).toString(),
            event.value(QStringLiteral("is_self_verification")).toBool(false));
        return;
    }
    if (type == QLatin1String("verification_request_started")) {
        Q_EMIT verificationRequestStarted(
            event.value(QStringLiteral("flow_id")).toString(),
            event.value(QStringLiteral("other_user_id")).toString(),
            event.value(QStringLiteral("is_self_verification")).toBool(true));
        return;
    }
    if (type == QLatin1String("room_key_import_started")) {
        Q_EMIT roomKeyImportStarted();
        return;
    }
    if (type == QLatin1String("room_key_import_progress")) {
        Q_EMIT roomKeyImportProgress(
            event.value(QStringLiteral("imported")).toInt(0),
            event.value(QStringLiteral("total")).toInt(0));
        return;
    }
    if (type == QLatin1String("room_key_import_done")) {
        QStringList roomIds;
        for (const auto &v : event.value(QStringLiteral("room_ids")).toArray()) {
            const QString id = v.toString();
            if (!id.isEmpty()) roomIds.append(id);
        }
        const int imported = event.value(QStringLiteral("imported")).toInt(0);
        const int total = event.value(QStringLiteral("total")).toInt(0);
        const int affected = event.value(QStringLiteral("affected_rooms"))
                                 .toInt(roomIds.size());
        qCInfo(lcRust) << "room key import completed"
                       << "imported=" << imported
                       << "total=" << total
                       << "affected_rooms=" << affected;
        Q_EMIT roomKeyImportDone(imported, total, affected, roomIds);
        return;
    }
    if (type == QLatin1String("room_key_import_failed")) {
        const QString category =
            event.value(QStringLiteral("category")).toString(
                QStringLiteral("import_failed"));
        // Never log the raw message; category only.
        qCWarning(lcRust) << "room key import failed category=" << category;
        Q_EMIT roomKeyImportFailed(
            category,
            event.value(QStringLiteral("message")).toString(
                tr("Room-key import failed.")));
        return;
    }
    if (type == QLatin1String("verification_sas_ready")) {
        QVariantList emojis;
        for (const auto &v : event.value(QStringLiteral("emojis")).toArray()) {
            QVariantMap m;
            m.insert(QStringLiteral("symbol"),
                     v.toObject().value(QStringLiteral("symbol")).toString());
            m.insert(QStringLiteral("description"),
                     v.toObject().value(QStringLiteral("description")).toString());
            emojis.append(m);
        }
        QVariantList decimals;
        for (const auto &v : event.value(QStringLiteral("decimals")).toArray())
            decimals.append(v.toInt());
        Q_EMIT verificationSasReady(
            event.value(QStringLiteral("flow_id")).toString(),
            emojis, decimals);
        return;
    }
    if (type == QLatin1String("verification_sas_confirmed")) {
        // Our confirmation registered; waiting for the peer's.
        Q_EMIT verificationSasConfirmed(
            event.value(QStringLiteral("flow_id")).toString());
        return;
    }
    if (type == QLatin1String("verification_qr_ready")) {
        // Geometry only: `bits_b64` is the QR module grid, never the payload,
        // which stays inside Rust. Nothing is logged, since the grid
        // reconstructs it.
        const QString flowId = event.value(QStringLiteral("flow_id")).toString();
        const int modules = event.value(QStringLiteral("size")).toInt(0);
        // Bound the geometry before decoding so an absurd size cannot drive an
        // oversized base64 decode.
        if (modules <= 0 || modules > QrCodeStore::kMaxModules) {
            qCWarning(lcRust) << "verification QR grid rejected: bad geometry";
            return;
        }
        const QByteArray bits = QByteArray::fromBase64(
            event.value(QStringLiteral("bits_b64")).toString().toLatin1(),
            QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
        // Reject a malformed grid: an unscannable code presented as working is
        // worse than no QR offer.
        const int stride = (modules + 7) / 8;
        if (bits.size() != stride * modules) {
            qCWarning(lcRust) << "verification QR grid rejected: bad geometry";
            return;
        }
        Q_EMIT verificationQrReady(flowId, modules, bits);
        return;
    }
    if (type == QLatin1String("verification_qr_scanned")) {
        Q_EMIT verificationQrScanned(
            event.value(QStringLiteral("flow_id")).toString());
        return;
    }
    if (type == QLatin1String("verification_qr_confirmed")) {
        Q_EMIT verificationQrConfirmed(
            event.value(QStringLiteral("flow_id")).toString());
        return;
    }
    if (type == QLatin1String("verification_qr_dismissed")) {
        Q_EMIT verificationQrDismissed(
            event.value(QStringLiteral("flow_id")).toString(),
            event.value(QStringLiteral("reason")).toString());
        return;
    }
    if (type == QLatin1String("verification_done")) {
        Q_EMIT verificationDone(
            event.value(QStringLiteral("flow_id")).toString());
        return;
    }
    if (type == QLatin1String("verification_cancelled")) {
        Q_EMIT verificationCancelled(
            event.value(QStringLiteral("flow_id")).toString(),
            event.value(QStringLiteral("message")).toString());
        return;
    }
    if (type == QLatin1String("verification_failed")) {
        Q_EMIT verificationFailed(
            event.value(QStringLiteral("flow_id")).toString(),
            event.value(QStringLiteral("message")).toString());
        return;
    }
    if (type == QLatin1String("verification_ready")) {
        // Lets the UI tell "peer has not answered" from "handshake running".
        Q_EMIT verificationReady(
            event.value(QStringLiteral("flow_id")).toString());
        return;
    }
    // Informational; sas_ready / done / cancelled carry every state the UI acts
    // on.
    if (type == QLatin1String("verification_sas_started"))
        return;

    if (type == QLatin1String("sync_stalled")) {
        // Deliberately not an error state: a first full-state request on a
        // large account can be slow. This leaves a log line for a wedged sync
        // instead of an unexplained spinner.
        qCWarning(lcRust) << "sync has not received a first response"
                          << "phase="
                          << event.value(QStringLiteral("phase")).toString()
                          << "waited_secs="
                          << event.value(QStringLiteral("waited_secs")).toInt();
        return;
    }

    if (type == QLatin1String("sync_error")) {
        // Only the active generation reaches here (shutdown callbacks were
        // rejected in pollRustEvents), so M_UNKNOWN_TOKEN keeps its real error
        // meaning.
        setState(Error);
        Q_EMIT errorOccurred(event.value(QStringLiteral("message")).toString(
            tr("Rust SDK sync failed.")));
        return;
    }

    // Room-management / user-search / media command results.
    if (handleRoomCommandEvent(type, event))
        return;

    if (type == QLatin1String("error")) {
        Q_EMIT errorOccurred(event.value(QStringLiteral("message")).toString(
            tr("Rust SDK backend error.")));
        return;
    }

    if (type == QLatin1String("queue_overflow")) {
        // Rust dropped events because the poll timer stalled. The queue is
        // positional (timeline and room-list index diffs), so after dropping
        // the oldest entries later ops address the wrong base, and dropped Sets
        // or in-range inserts pass every bounds check. Treat the stream as
        // untrusted and re-snapshot with the primitives used for detected
        // damage: resync the room list and reload the open timeline. Both are
        // idempotent, and the producer injects at most one marker per overflow
        // episode.
        qCWarning(lcRust) << event.value(QStringLiteral("message")).toString()
                          << "— resyncing rooms and reloading the open room";
        Q_EMIT errorOccurred(event.value(QStringLiteral("message")).toString(
            tr("Rust SDK event queue overflowed.")));
        if (m_rustHandle)
            takeRustString(mx_rust_resync_rooms(m_rustHandle));
        const QString openRoom = m_timelineTracker.activeRoom();
        if (!openRoom.isEmpty())
            openRoomTimeline(openRoom);
    }
}

void RustSdkMatrixClient::handleRoomsEvent(const QJsonArray &rooms)
{
    matrix::rust_rooms::applyIndexReset({m_rooms, m_roomOrder}, rooms);
    Q_EMIT roomsChanged();
}

void RustSdkMatrixClient::handleRoomSnapshotEvent(const QJsonArray &rooms)
{
    // A snapshot is not an index base: it walks the whole state store in store
    // order, while m_roomOrder follows the sliding-sync adapter's filtered,
    // sorted vector. Rebuilding m_roomOrder from it misaddresses the next
    // Set{index}. On classic sync there are no diffs and m_roomOrder is empty,
    // so applySnapshot() defines the room set. See matrix::rust_rooms.
    matrix::rust_rooms::applySnapshot({m_rooms, m_roomOrder}, rooms);
    Q_EMIT roomsChanged();
}

RoomInfo RustSdkMatrixClient::roomInfoFromJson(const QJsonObject &obj) const
{
    // Merging rules live in matrix::rust_rooms so they can be tested without a
    // handle; this wrapper merges over what this class already knows.
    const QString id = obj.value(QStringLiteral("id")).toString();
    return matrix::rust_rooms::roomInfoFromJson(obj, m_rooms.value(id));
}

void RustSdkMatrixClient::handleRoomListDiff(const QJsonObject &event)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    if (matrix::rust_rooms::applyRoomListDiff({m_rooms, m_roomOrder}, event)) {
        Q_EMIT roomsChanged();
        return;
    }

    // Never apply a mismatched diff; it corrupts the ordered registry. Ask the
    // producer that owns the index space to re-emit its base:
    // mx_rust_resync_rooms re-sets the dynamic adapter's filter, which yields a
    // real Reset (a client.rooms() snapshot would be a different vector and
    // keep the drift going). Room ids are public identifiers; `expected` is
    // Rust's id at the index, `holding` ours.
    const QJsonObject roomObject = event.value(QStringLiteral("room")).toObject();
    const QString roomId = roomObject.value(QStringLiteral("id")).toString();
    const int index = event.value(QStringLiteral("index")).toInt(-1);
    const QString holding = index >= 0 && index < m_roomOrder.size()
        ? m_roomOrder.at(index) : QString();
    qCWarning(lcRust) << "room_list malformed diff rejected op=" << type
                      << "index=" << index
                      << "length=" << event.value(QStringLiteral("length")).toInt(-1)
                      << "room=" << roomId
                      << "expected=" << event.value(QStringLiteral("expected_id")).toString()
                      << "holding=" << holding
                      << "known=" << (!roomId.isEmpty() && m_rooms.contains(roomId))
                      << "indexed=" << (!roomId.isEmpty() && m_roomOrder.contains(roomId))
                      << "registry=" << m_roomOrder.size()
                      << "— asking the room list to re-emit its index base";
    if (m_rustHandle)
        takeRustString(mx_rust_resync_rooms(m_rustHandle));
}

void RustSdkMatrixClient::handleSpacesEvent(const QJsonArray &spaces)
{
    QSet<QString> present;
    for (const auto &value : spaces) {
        const auto object = value.toObject();
        const QString id = object.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) continue;
        present.insert(id);
        RoomInfo room = m_rooms.value(id);
        room.id = id; room.isSpace = true; room.membership = RoomInfo::Joined;
        room.name = object.value(QStringLiteral("name")).toString(room.name);
        room.avatarUrl = object.value(QStringLiteral("avatar_url")).toString(room.avatarUrl);
        // Direct children in the Space's m.space.child order, never the
        // transitive `descendants`, matching the mock and HTTP backends;
        // SpaceManager::rebuild derives transitive membership itself.
        // `descendants` is the fallback only when `children` is absent: an
        // empty `children` means the last child was removed.
        room.childRoomIds.clear();
        const QJsonArray childSource =
            object.contains(QStringLiteral("children"))
                ? object.value(QStringLiteral("children")).toArray()
                : object.value(QStringLiteral("descendants")).toArray();
        for (const auto &child : childSource) {
            const QString childId = child.toString();
            if (!childId.isEmpty() && childId != id && !room.childRoomIds.contains(childId))
                room.childRoomIds.append(childId);
        }
        room.parentSpaceIds.clear();
        for (const auto &parent : object.value(QStringLiteral("parents")).toArray()) {
            const QString parentId = parent.toString();
            if (!parentId.isEmpty()) room.parentSpaceIds.append(parentId);
        }
        m_rooms.insert(id, room);
        // Deliberately not appended to m_roomOrder, which mirrors the SDK's
        // room list one-for-one because every diff addresses it by index. Extra
        // entries shift the indices and trigger a reject/resnapshot loop.
        // rooms() still returns unordered m_rooms entries after the ordered
        // ones.
    }
    // A left Space is erased, not blanked: `present` is the complete
    // joined-Space set. The rule and its index-space guard live with the
    // function in RustRoomRegistry.cpp.
    const int retiredSpaces =
        matrix::rust_rooms::retireAbsentSpaces({m_rooms, m_roomOrder}, present);
    if (retiredSpaces > 0) {
        qCInfo(lcRust) << "spaces retired count=" << retiredSpaces
                       << "joined=" << present.size();
    }
    Q_EMIT roomsChanged();
}

void RustSdkMatrixClient::handleTimelineEvent(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QJsonObject obj = event.value(QStringLiteral("event")).toObject();
    const QString eventId = obj.value(QStringLiteral("event_id")).toString();
    if (roomId.isEmpty() || eventId.isEmpty())
        return;

    // Rooms with a live SDK timeline are fed only through timeline_reset /
    // timeline_diff; appending here would duplicate rows. Keep only the
    // room-list preview update.
    if (m_timelineTracker.activeRoom() == roomId
        || m_timelineTracker.requestedRoom() == roomId) {
        auto roomIt = m_rooms.find(roomId);
        if (roomIt != m_rooms.end()) {
            // Raw sync bodies can be multi-line; this writer must be one-line
            // too.
            const QString body = matrix::preview::normalizePreviewText(
                obj.value(QStringLiteral("body")).toString());
            if (!body.isEmpty())
                roomIt->lastMessagePreview = body;
            roomIt->raiseActivity(timestampFromMs(static_cast<qint64>(
                obj.value(QStringLiteral("timestamp_ms")).toDouble(0))));
            Q_EMIT roomUpdated(roomId);
        }
        return;
    }

    // Mirror for an unopened room, bounded by kBackgroundMirrorCap, which
    // bounds this de-dup scan too. The scan stays here so a duplicate does not
    // re-emit eventAppended or raise the room's activity.
    auto &timeline = m_timelines[roomId];
    for (const auto &existing : timeline) {
        if (existing.eventId == eventId)
            return;
    }

    TimelineEvent timelineEvent;
    timelineEvent.eventId = eventId;
    timelineEvent.roomId = roomId;
    timelineEvent.sender = obj.value(QStringLiteral("sender")).toString();
    timelineEvent.senderDisplayName = displayNameFor(roomId, timelineEvent.sender);
    timelineEvent.body = obj.value(QStringLiteral("body")).toString();
    // Untrusted sender HTML, passed through as-is; TimelineModel sanitizes it
    // before QML sees it.
    timelineEvent.formattedBody =
        obj.value(QStringLiteral("formatted_body")).toString();
    timelineEvent.timestamp = timestampFromMs(static_cast<qint64>(
        obj.value(QStringLiteral("timestamp_ms")).toDouble(0)));
    if (!timelineEvent.timestamp.isValid())
        timelineEvent.timestamp = QDateTime::currentDateTimeUtc();
    timelineEvent.type = typeFromString(obj.value(QStringLiteral("msgtype")).toString());
    // Media rows carry both body and filename, as the live payload does:
    // oneLineSummary reads mediaFilename for files and falls back to body for a
    // location. An older bridge leaves it empty and the preview degrades to the
    // media kind.
    timelineEvent.mediaFilename =
        obj.value(QStringLiteral("media_filename")).toString();
    timelineEvent.status = TimelineEvent::Sent;

    // Encryption metadata from the bridge, falling back to the older
    // `decrypted` flag. Metadata only; never derive plaintext from these
    // fields.
    const bool undecryptable =
        obj.value(QStringLiteral("undecryptable")).toBool(false);
    const bool isDecrypted =
        obj.value(QStringLiteral("is_decrypted"))
           .toBool(obj.value(QStringLiteral("decrypted")).toBool(false));
    const bool isEncrypted =
        obj.value(QStringLiteral("is_encrypted"))
           .toBool(undecryptable || isDecrypted);
    timelineEvent.isEncrypted   = isEncrypted;
    timelineEvent.isDecrypted   = isDecrypted;
    timelineEvent.undecryptable = undecryptable;
    timelineEvent.errorKind     =
        obj.value(QStringLiteral("error_kind")).toString();
    // Mention/thread metadata for notification policy in rooms without a live
    // timeline.
    timelineEvent.mentionsMe =
        obj.value(QStringLiteral("mentions_me")).toBool(false);
    timelineEvent.mentionsRoom =
        obj.value(QStringLiteral("mentions_room")).toBool(false);
    timelineEvent.threadRootId =
        obj.value(QStringLiteral("thread_root_id")).toString();

    // Undecryptable events arrive with an empty body; show an honest
    // placeholder. The SDK replaces the event (`event_replaced`) if keys
    // arrive.
    if (undecryptable && timelineEvent.body.isEmpty()) {
        timelineEvent.body = tr("[unable to decrypt yet]");
        timelineEvent.type = TimelineEvent::Notice;
    }

    // Recovery-lifecycle diagnostics for encrypted events only: redacted ids
    // and error category, never bodies or ciphertext.
    if (isEncrypted) {
        qCDebug(lcE2ee) << "encrypted-event"
                        << "room=" << matrix::e2ee::redactId(roomId)
                        << "event=" << matrix::e2ee::redactId(timelineEvent.eventId)
                        << (undecryptable ? "state=utd" : "state=decryptable")
                        << "error=" << (timelineEvent.errorKind.isEmpty()
                                            ? QStringLiteral("none")
                                            : timelineEvent.errorKind);
    }

    // A true thread reply never enters the main-timeline mirror. The mirror
    // fills while a room is closed and TimelineModel::reload() reads it before
    // the SDK timeline opens, so replies would flash as standalone rows (and
    // stay, if the open failed). Filtered here rather than in the model, which
    // derives thread roots and reply counts by counting replies in its own
    // list. The signal is still emitted: it feeds notifications and the
    // Activity Center, and a thread mention must still reach the user.
    const bool threadedReply = !timelineEvent.threadRootId.isEmpty()
        && timelineEvent.threadRootId != timelineEvent.eventId;
    if (!threadedReply)
        matrix::rust_timeline::appendBounded(timeline, timelineEvent);
    Q_EMIT eventAppended(roomId, timelineEvent);

    auto roomIt = m_rooms.find(roomId);
    if (roomIt != m_rooms.end()) {
        // What may move a room up the list is deliberately narrow. Opening a
        // room delivers its backlog here as live appends, which must not
        // reorder the list:
        //   * virtual rows (dividers, read marker, timeline start) are not
        //     activity;
        //   * state changes are not activity (joins/leaves must not raise a
        //     room);
        //   * activity never moves backwards, so replayed history is harmless.
        const bool countsAsActivity = !timelineEvent.isVirtual()
            && timelineEvent.type != TimelineEvent::StateChange
            // Call rows carry no body (TimelineModel builds the sentence), so
            // they must not replace the room's preview with an empty string.
            && timelineEvent.type != TimelineEvent::CallEvent;
        if (countsAsActivity) {
            roomIt->lastMessagePreview = previewFor(timelineEvent);
            roomIt->raiseActivity(timelineEvent.timestamp);
            Q_EMIT roomUpdated(roomId);
        }
    }
}

void RustSdkMatrixClient::updateRoomPreviewFrom(
    const QString &roomId, const QList<TimelineEvent> &newestFirstCandidates)
{
    auto roomIt = m_rooms.find(roomId);
    if (roomIt == m_rooms.end())
        return;
    for (const auto &event : newestFirstCandidates) {
        if (event.isVirtual())
            continue;
        roomIt->lastMessagePreview = previewFor(event);
        roomIt->raiseActivity(event.timestamp);
        Q_EMIT roomUpdated(roomId);
        return;
    }
}

void RustSdkMatrixClient::handleTimelineReset(const QJsonObject &event)
{
    // Stall attribution: ingesting a batch rebuilds model rows. No-op unless
    // LIGHTNING_GUI_STALL_TRACE is set.
    stalltrace::Scope stallScope("timeline-reset");
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const auto generation = static_cast<quint64>(
        event.value(QStringLiteral("room_generation")).toDouble(0));
    if (!m_timelineTracker.adoptReset(roomId, generation)) {
        qCInfo(lcRust) << "timeline stale reset ignored"
                       << "generation=" << generation
                       << "adopted=" << m_timelineTracker.generation();
        return;
    }

    const QJsonArray items = event.value(QStringLiteral("items")).toArray();
    // A jump-to-live history trim reports its outcome here (counts only).
    // Logged so "nothing was released" is visible in a capture.
    if (event.contains(QStringLiteral("trimmed_from"))
        && !event.value(QStringLiteral("trimmed_from")).isNull()) {
        const int before =
            event.value(QStringLiteral("trimmed_from")).toInt(0);
        const bool shrunk =
            event.value(QStringLiteral("trim_shrunk")).toBool(false);
        qCInfo(lcRust) << "timeline live-trim room="
                       << matrix::e2ee::redactId(roomId)
                       << "cachedBefore=" << before
                       << "released=" << shrunk
                       << "reloadedItems=" << items.size();
    }
    m_timelines[roomId] =
        matrix::rust_timeline::eventsFromItemArray(items, roomId);
    qCInfo(lcRust) << "timeline subscription started"
                   << "room_generation=" << generation
                   << "items=" << m_timelines[roomId].size();
    Q_EMIT timelineReset(roomId);
    Q_EMIT paginationStateChanged(roomId);

    QList<TimelineEvent> newestFirst = m_timelines[roomId];
    std::reverse(newestFirst.begin(), newestFirst.end());
    updateRoomPreviewFrom(roomId, newestFirst);
}

void RustSdkMatrixClient::clearTimelineInsertBatch()
{
    m_timelineInsertBatchRoom.clear();
    m_timelineInsertBatchGeneration = 0;
    m_timelineInsertBatchFirst = -1;
    m_timelineInsertBatchCount = 0;
    m_timelineInsertBatchChangedIds.clear();
}

void RustSdkMatrixClient::flushTimelineInsertBatch()
{
    if (m_timelineInsertBatchCount <= 0)
        return;

    const QString roomId = m_timelineInsertBatchRoom;
    const int first = m_timelineInsertBatchFirst;
    const int count = m_timelineInsertBatchCount;
    const auto timelineIt = m_timelines.constFind(roomId);
    QList<TimelineEvent> items;
    QList<QPair<int, TimelineEvent>> changedItems;
    if (timelineIt != m_timelines.cend() && first >= 0
        && first + count <= timelineIt->size()) {
        items = timelineIt->mid(first, count);
        changedItems.reserve(m_timelineInsertBatchChangedIds.size());
        if (!m_timelineInsertBatchChangedIds.isEmpty()) {
            // One pass builds the stable-id index, avoiding a per-id rescan on
            // every pagination flush.
            QHash<QString, int> rowByStableId;
            rowByStableId.reserve(timelineIt->size());
            for (int row = 0; row < timelineIt->size(); ++row) {
                const TimelineEvent &candidate = timelineIt->at(row);
                const QString &candidateId = !candidate.itemId.isEmpty()
                    ? candidate.itemId : candidate.eventId;
                if (!candidateId.isEmpty() && !rowByStableId.contains(candidateId))
                    rowByStableId.insert(candidateId, row);
            }
            for (const QString &stableId : std::as_const(
                     m_timelineInsertBatchChangedIds)) {
                const int row = rowByStableId.value(stableId, -1);
                if (row < 0)
                    continue;
                // A later insertion may have brought this item into the new
                // range, whose payload already includes the update.
                if (row < first || row >= first + count)
                    changedItems.append({row, timelineIt->at(row)});
            }
        }
    }

    // Clear before emitting: a synchronous model observer may switch rooms or
    // otherwise re-enter the client, and must never see this batch as active.
    clearTimelineInsertBatch();
    if (items.size() != count) {
        qCWarning(lcRust) << "timeline insert batch lost mirror range"
                          << "first=" << first << "count=" << count;
        Q_EMIT timelineReset(roomId);
        return;
    }

    if (first == 0)
        Q_EMIT eventsPrepended(roomId, items);
    else
        Q_EMIT eventsInsertedAt(roomId, first, items);
    for (const auto &changed : std::as_const(changedItems))
        Q_EMIT eventChangedAt(roomId, changed.first, changed.second);
}

void RustSdkMatrixClient::reportStaleTimelineDiffs()
{
    if (m_staleDiffCount <= 0)
        return;
    qCInfo(lcRust) << "timeline stale diffs ignored"
                   << "count=" << m_staleDiffCount
                   << "generation=" << m_staleDiffGeneration
                   << "adopted=" << m_timelineTracker.generation();
    m_staleDiffCount = 0;
    m_staleDiffGeneration = 0;
}

void RustSdkMatrixClient::handleTimelineDiff(const QJsonObject &event)
{
    // Stall attribution, split by diff op: an in-place Set (reaction, edit,
    // receipt, late decryption) and an insert (new message, pagination) have
    // very different costs. Keyed on the op alone, which this function knows
    // for certain. No-op unless LIGHTNING_GUI_STALL_TRACE is set.
    const QString diffOp = event.value(QStringLiteral("op")).toString();
    stalltrace::Scope stallScope(diffOp == QLatin1String("set")
                                     ? "timeline-diff-set"
                                     : "timeline-diff");
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const auto generation = static_cast<quint64>(
        event.value(QStringLiteral("room_generation")).toDouble(0));
    if (!m_timelineTracker.accepts(roomId, generation)) {
        flushTimelineInsertBatch();
        // Counted rather than logged per diff: a superseded generation keeps
        // delivering until its subscription stops. Reported once, by the first
        // diff the new generation accepts.
        if (m_staleDiffGeneration != generation) {
            reportStaleTimelineDiffs();
            m_staleDiffGeneration = generation;
        }
        ++m_staleDiffCount;
        return;
    }
    reportStaleTimelineDiffs();

    const QString &op = diffOp;
    const int insertionIndex = op == QLatin1String("push_front")
        ? 0 : (op == QLatin1String("insert")
                   ? event.value(QStringLiteral("index")).toInt(-1) : -1);
    const bool sameBatch = m_timelineInsertBatchCount > 0
        && m_timelineInsertBatchRoom == roomId
        && m_timelineInsertBatchGeneration == generation;
    // A page starts at index 0 (push_front) or 1 (after the SDK's TimelineStart
    // sentinel). Later inserts may land inside or right after the new range
    // (e.g. a date divider); the net mutation is still one contiguous range.
    const bool startsPaginationRange = m_timelineInsertBatchCount == 0
        && (insertionIndex == 0 || insertionIndex == 1);
    const bool extendsPaginationRange = sameBatch
        && insertionIndex >= m_timelineInsertBatchFirst
        && insertionIndex <= m_timelineInsertBatchFirst
                             + m_timelineInsertBatchCount;
    const bool canBatchInsertion = m_coalesceTimelineInserts
        && insertionIndex >= 0
        && (startsPaginationRange || extendsPaginationRange);
    // matrix-sdk-ui interleaves `set` diffs while building a page (dividers,
    // receipts, profile/decryption refreshes). Flushing on each would split one
    // page into many insert transactions, so defer valid sets: inside the range
    // they fold into the payload, outside it they replay by stable id
    // afterwards.
    const bool canDeferSet = m_coalesceTimelineInserts && sameBatch
        && op == QLatin1String("set");
    if (m_timelineInsertBatchCount > 0
        && !canBatchInsertion && !canDeferSet)
        flushTimelineInsertBatch();

    using matrix::rust_timeline::DiffOutcome;
    auto &mirror = m_timelines[roomId];
    const DiffOutcome outcome =
        matrix::rust_timeline::applyTimelineDiff(mirror, event, roomId);

    if (canBatchInsertion
        && (outcome.kind == DiffOutcome::Prepended
            || outcome.kind == DiffOutcome::Inserted)) {
        if (m_timelineInsertBatchCount == 0) {
            m_timelineInsertBatchRoom = roomId;
            m_timelineInsertBatchGeneration = generation;
            m_timelineInsertBatchFirst = insertionIndex;
        }
        ++m_timelineInsertBatchCount;
        return;
    }

    if (canDeferSet && outcome.kind == DiffOutcome::Changed) {
        if (outcome.index >= m_timelineInsertBatchFirst
            && outcome.index < m_timelineInsertBatchFirst
                               + m_timelineInsertBatchCount) {
            return;
        }
        const TimelineEvent &changed = outcome.items.first();
        const QString stableId = !changed.itemId.isEmpty()
            ? changed.itemId : changed.eventId;
        if (!stableId.isEmpty()) {
            if (!m_timelineInsertBatchChangedIds.contains(stableId))
                m_timelineInsertBatchChangedIds.append(stableId);
            return;
        }
        // An identity-less virtual item cannot be found again after rows shift.
        // Publish the assembled page first, then let the Changed path update
        // it.
    }

    // An insert-like event can still fail validation; flush any valid batch
    // before the recovery reset below.
    if (m_timelineInsertBatchCount > 0)
        flushTimelineInsertBatch();

    switch (outcome.kind) {
    case DiffOutcome::Appended:
        for (const auto &item : outcome.items)
            Q_EMIT eventAppended(roomId, item);
        {
            QList<TimelineEvent> newestFirst = outcome.items;
            std::reverse(newestFirst.begin(), newestFirst.end());
            updateRoomPreviewFrom(roomId, newestFirst);
        }
        break;
    case DiffOutcome::Prepended:
        Q_EMIT eventsPrepended(roomId, outcome.items);
        break;
    case DiffOutcome::Inserted:
        Q_EMIT eventInsertedAt(roomId, outcome.index, outcome.items.first());
        break;
    case DiffOutcome::Changed:
        Q_EMIT eventChangedAt(roomId, outcome.index, outcome.items.first());
        break;
    case DiffOutcome::Removed:
        Q_EMIT eventRemovedAt(roomId, outcome.index);
        break;
    case DiffOutcome::Cleared:
    case DiffOutcome::Reset:
        Q_EMIT timelineReset(roomId);
        break;
    case DiffOutcome::Truncated:
        Q_EMIT eventsTruncatedTo(roomId, outcome.length);
        break;
    case DiffOutcome::Invalid:
        // Never apply a malformed/stale diff; recover with a fresh snapshot. No
        // message bodies in this log line.
        qCWarning(lcRust) << "timeline invalid diff rejected"
                          << "op=" << event.value(QStringLiteral("op")).toString()
                          << "index=" << event.value(QStringLiteral("index")).toInt(-1)
                          << "mirror_size=" << mirror.size();
        openRoomTimeline(roomId);
        break;
    }
}

void RustSdkMatrixClient::handleTimelinePagination(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const auto generation = static_cast<quint64>(
        event.value(QStringLiteral("room_generation")).toDouble(0));
    if (!m_timelineTracker.accepts(roomId, generation)) {
        qCInfo(lcRust) << "timeline stale pagination ignored"
                       << "generation=" << generation;
        return;
    }

    auto &state = m_pagination[roomId];
    const QString paginationState =
        event.value(QStringLiteral("state")).toString();
    if (paginationState == QLatin1String("loading")) {
        state.loading = true;
        state.failed = false;
        state.failureTransient = false;
        qCInfo(lcRust) << "timeline pagination started";
    } else if (paginationState == QLatin1String("idle")) {
        state.loading = false;
        state.failed = false;
        state.failureTransient = false;
        state.reachedStart =
            event.value(QStringLiteral("reached_start")).toBool(false);
        // Cumulative process-wide totals from the Rust timeline filter (deltas
        // would race the async ingest). Across pages: `offered` and
        // `droppedRtc` rising together is MatrixRTC churn; `offered` rising
        // alone is hidden threads or aggregation; `offered` flat means the
        // pages really were empty. Escalate while the filter eats whole pages.
        // A concurrent thread-panel pagination can inflate the deltas, costing
        // at most one larger page.
        {
            const quint64 offered = static_cast<quint64>(
                event.value(QStringLiteral("filter_offered")).toDouble(0));
            const quint64 dropped = static_cast<quint64>(
                event.value(QStringLiteral("filter_dropped_sdk")).toDouble(0)
                + event.value(QStringLiteral("filter_dropped_rtc"))
                      .toDouble(0));
            const quint64 offeredDelta =
                offered > state.lastFilterOffered
                    ? offered - state.lastFilterOffered : 0;
            const quint64 droppedDelta =
                dropped > state.lastFilterDropped
                    ? dropped - state.lastFilterDropped : 0;
            state.lastFilterOffered = offered;
            state.lastFilterDropped = dropped;
            const unsigned short current =
                state.batchSize > 0 ? state.batchSize : kPaginationBatch;
            const bool fullyFiltered = offeredDelta > 0
                && droppedDelta >= offeredDelta;
            // Lets the controller skip waiting for rows that cannot arrive.
            state.lastFullyFiltered = fullyFiltered;
            if (!state.reachedStart && fullyFiltered) {
                // Only matters when the SDK reaches a network gap; a page
                // served from a stored chunk ignores the batch size.
                state.batchSize = static_cast<unsigned short>(
                    qMin<quint64>(current * 3u, kPaginationMaxBatch));
            } else {
                // Rows came through: back to the ordinary page size.
                state.batchSize = 0;
            }
        }
        qCInfo(lcRust) << "timeline pagination complete reached_start="
                       << state.reachedStart
                       << "nextBatch="
                       << (state.batchSize > 0 ? state.batchSize
                                               : kPaginationBatch)
                       << "filterOffered="
                       << event.value(QStringLiteral("filter_offered"))
                              .toDouble(0)
                       << "droppedSdk="
                       << event.value(QStringLiteral("filter_dropped_sdk"))
                              .toDouble(0)
                       << "droppedRtc="
                       << event.value(QStringLiteral("filter_dropped_rtc"))
                              .toDouble(0);
    } else if (paginationState == QLatin1String("failed")) {
        state.loading = false;
        state.failed = true;
        const QString category =
            event.value(QStringLiteral("category")).toString();
        state.failureTransient = category == QLatin1String("network")
            || category == QLatin1String("not_ready");
        qCWarning(lcRust) << "timeline pagination failed category="
                          << category;
    }
    Q_EMIT paginationStateChanged(roomId);
}

void RustSdkMatrixClient::handleTimelineRetryDecryption(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QString state = event.value(QStringLiteral("state")).toString();
    const int sessions = event.value(QStringLiteral("sessions")).toInt(0);
    qCInfo(lcRust) << "timeline retry decryption" << state
                   << "sessions=" << sessions;
    // Redacted room id, state and a session count only; never session ids, keys
    // or bodies.
    qCDebug(lcE2ee) << "retry-decryption" << "room=" << matrix::e2ee::redactId(roomId)
                    << "state=" << state << "sessions=" << sessions;
    if (state == QLatin1String("done"))
        Q_EMIT roomKeysApplied(roomId, sessions);
}

void RustSdkMatrixClient::handleSendOk(const QJsonObject &event)
{
    const QString txnId = event.value(QStringLiteral("transaction_id")).toString();
    const QString realEventId = event.value(QStringLiteral("event_id")).toString();
    const auto pendingIt = m_pendingSends.find(txnId);
    if (pendingIt == m_pendingSends.end())
        return;

    const PendingSend pending = pendingIt.value();
    m_pendingSends.erase(pendingIt);

    auto &timeline = m_timelines[pending.roomId];
    for (auto &timelineEvent : timeline) {
        if (timelineEvent.eventId != pending.localEventId)
            continue;

        if (!realEventId.isEmpty()) {
            const QString oldId = timelineEvent.eventId;
            timelineEvent.eventId = realEventId;
            timelineEvent.status = TimelineEvent::Sent;
            Q_EMIT eventReplaced(pending.roomId, oldId, timelineEvent);
        } else {
            timelineEvent.status = TimelineEvent::Sent;
            Q_EMIT eventStatusChanged(pending.roomId,
                                      pending.localEventId,
                                      TimelineEvent::Sent);
        }
        return;
    }
}

void RustSdkMatrixClient::handleSendFailed(const QJsonObject &event)
{
    failPendingSend(event.value(QStringLiteral("transaction_id")).toString(),
                    event.value(QStringLiteral("message")).toString(
                        tr("Rust SDK send failed.")));
}

void RustSdkMatrixClient::recoverFromBackup(const QString &recoveryKey)
{
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT keyBackupResult(QStringLiteral("failed"),
                               tr("Not signed in."));
        return;
    }
    if (recoveryKey.isEmpty()) {
        Q_EMIT keyBackupResult(QStringLiteral("failed"),
                               tr("Recovery key is empty."));
        return;
    }
    QByteArray keyBytes = recoveryKey.toUtf8();
    const QString result = takeRustString(mx_rust_recover_from_backup(
        m_rustHandle, keyBytes.constData()));
    // Best-effort scrub of the recovery secret's transit buffer, as in
    // importRoomKeys.
    keyBytes.fill('\0');
    if (!result.isEmpty()) {
        Q_EMIT keyBackupResult(QStringLiteral("failed"),
            result.startsWith(QLatin1String("error: "))
                ? result.mid(7) : result);
    }
}

void RustSdkMatrixClient::reloadRoomTimeline(const QString &roomId, int limit)
{
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT errorOccurred(tr("Reload timeline: not signed in."));
        return;
    }
    if (roomId.isEmpty()) return;
    const QByteArray idBytes = roomId.toUtf8();
    const unsigned int clamped =
        limit <= 0 ? 30u
                   : static_cast<unsigned int>(std::min(limit, 200));
    qCInfo(lcRust) << "reload_timeline start room="
                   << matrix::e2ee::redactId(roomId)
                   << "limit=" << clamped;
    const QString result = takeRustString(mx_rust_reload_room_timeline(
        m_rustHandle, idBytes.constData(), clamped));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(
            result.startsWith(QLatin1String("error: "))
                ? result.mid(7) : result);
    }
}

void RustSdkMatrixClient::acceptVerification(const QString &flowId)
{
    if (!m_rustHandle || flowId.isEmpty()) return;
    const QByteArray b = flowId.toUtf8();
    const QString r = takeRustString(mx_rust_accept_verification(m_rustHandle, b.constData()));
    if (!r.isEmpty()) Q_EMIT verificationFailed(flowId,
        r.startsWith(QLatin1String("error: ")) ? r.mid(7) : r);
}

void RustSdkMatrixClient::confirmVerification(const QString &flowId)
{
    if (!m_rustHandle || flowId.isEmpty()) return;
    const QByteArray b = flowId.toUtf8();
    const QString r = takeRustString(mx_rust_confirm_verification(m_rustHandle, b.constData()));
    if (!r.isEmpty()) Q_EMIT verificationFailed(flowId,
        r.startsWith(QLatin1String("error: ")) ? r.mid(7) : r);
}

void RustSdkMatrixClient::confirmQrVerification(const QString &flowId)
{
    if (!m_rustHandle || flowId.isEmpty()) return;
    const QByteArray b = flowId.toUtf8();
    const QString r =
        takeRustString(mx_rust_confirm_qr_verification(m_rustHandle, b.constData()));
    if (!r.isEmpty()) Q_EMIT verificationFailed(flowId,
        r.startsWith(QLatin1String("error: ")) ? r.mid(7) : r);
}

void RustSdkMatrixClient::mismatchVerification(const QString &flowId)
{
    if (!m_rustHandle || flowId.isEmpty()) return;
    const QByteArray b = flowId.toUtf8();
    const QString r = takeRustString(mx_rust_mismatch_verification(m_rustHandle, b.constData()));
    if (!r.isEmpty()) Q_EMIT verificationFailed(flowId,
        r.startsWith(QLatin1String("error: ")) ? r.mid(7) : r);
}

void RustSdkMatrixClient::cancelVerification(const QString &flowId)
{
    if (!m_rustHandle || flowId.isEmpty()) return;
    const QByteArray b = flowId.toUtf8();
    const QString r = takeRustString(mx_rust_cancel_verification(m_rustHandle, b.constData()));
    if (!r.isEmpty()) Q_EMIT verificationFailed(flowId,
        r.startsWith(QLatin1String("error: ")) ? r.mid(7) : r);
}

void RustSdkMatrixClient::startOwnVerification()
{
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT verificationFailed(QString{}, tr("Not signed in."));
        return;
    }
    const QString r = takeRustString(mx_rust_start_own_verification(m_rustHandle));
    if (!r.isEmpty()) {
        Q_EMIT verificationFailed(QString{},
            r.startsWith(QLatin1String("error: ")) ? r.mid(7) : r);
    }
}

void RustSdkMatrixClient::requestMissingSecrets()
{
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT errorOccurred(tr("Not signed in."));
        return;
    }
    const QString r =
        takeRustString(mx_rust_request_missing_secrets(m_rustHandle));
    if (!r.isEmpty()) {
        qCWarning(lcRust) << "request_missing_secrets dispatch failed";
        Q_EMIT errorOccurred(
            r.startsWith(QLatin1String("error: ")) ? r.mid(7) : r);
    }
}

void RustSdkMatrixClient::refreshOwnDeviceStatus()
{
    if (!m_loggedIn || !m_rustHandle)
        return;
    const QString raw = takeRustString(mx_rust_query_own_device_status(m_rustHandle));
    if (raw.isEmpty() || raw.startsWith(QLatin1String("error: ")))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
    if (!doc.isObject()) return;
    const QJsonObject obj = doc.object();
    Q_EMIT ownDeviceStatusUpdated(
        obj.value(QStringLiteral("device_id")).toString(),
        obj.value(QStringLiteral("own_identity_available")).toBool(false),
        obj.value(QStringLiteral("own_identity_verified")).toBool(false),
        obj.value(QStringLiteral("device_cross_signed")).toBool(false),
        obj.value(QStringLiteral("has_master")).toBool(false),
        obj.value(QStringLiteral("has_self_signing")).toBool(false),
        obj.value(QStringLiteral("has_user_signing")).toBool(false));
}

// See the header and OwnDeviceKeyWatch for the policy.
void RustSdkMatrixClient::checkOwnIdentityKey()
{
    if (!m_loggedIn || !m_rustHandle)
        return;
    // The answer arrives as an `own_identity_key` event. A dispatch error is
    // not an answer; the tri-state stays unknown.
    const QString r =
        takeRustString(mx_rust_check_own_identity_key(m_rustHandle));
    if (!r.isEmpty())
        qCDebug(lcRust) << "own identity key check not dispatched";
}

void RustSdkMatrixClient::importRoomKeys(const QString &filePath,
                                         const QString &passphrase)
{
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT roomKeyImportFailed(QStringLiteral("not_signed_in"),
                                   tr("Not signed in."));
        return;
    }
    if (filePath.isEmpty()) {
        Q_EMIT roomKeyImportFailed(QStringLiteral("invalid_file"),
                                   tr("No file selected."));
        return;
    }
    // Convert once; keep no QString copy of the passphrase in C++.
    QByteArray pathBytes = filePath.toUtf8();
    QByteArray passphraseBytes = passphrase.toUtf8();
    const QString r = takeRustString(mx_rust_import_room_keys(
        m_rustHandle, pathBytes.constData(), passphraseBytes.constData()));
    // Best-effort scrub; the passphrase is not kept anywhere in C++ after this.
    for (int i = 0; i < passphraseBytes.size(); ++i)
        passphraseBytes[i] = 0;
    if (!r.isEmpty()) {
        Q_EMIT roomKeyImportFailed(QStringLiteral("import_failed"),
            r.startsWith(QLatin1String("error: ")) ? r.mid(7) : r);
    }
}

bool RustSdkMatrixClient::roomKeyImportActive() const
{
    if (!m_rustHandle) return false;
    return mx_rust_room_key_import_active(m_rustHandle) != 0;
}

void RustSdkMatrixClient::probeEncryptedSend(const QString &roomId,
                                             const QString &body,
                                             const QString &marker)
{
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT encryptedSendProbeResult(roomId, marker, false,
                                        QString(), tr("Not signed in."));
        return;
    }
    if (!m_rooms.contains(roomId)) {
        Q_EMIT encryptedSendProbeResult(roomId, marker, false,
                                        QString(),
                                        tr("Unknown room: %1").arg(roomId));
        return;
    }
    if (!isRoomEncrypted(roomId)) {
        Q_EMIT encryptedSendProbeResult(roomId, marker, false,
                                        QString(),
                                        tr("Probe refused: target room is not encrypted."));
        return;
    }

    const QString txnId = nextTxnId();
    m_pendingProbes.insert(txnId, PendingProbe{ roomId, marker });

    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray bodyBytes = body.toUtf8();
    const QByteArray txnBytes  = txnId.toUtf8();
    const QString result = takeRustString(mx_rust_probe_encrypted_send(
        m_rustHandle, roomBytes.constData(),
        bodyBytes.constData(), txnBytes.constData()));
    if (!result.isEmpty()) {
        m_pendingProbes.remove(txnId);
        Q_EMIT encryptedSendProbeResult(roomId, marker, false, QString(),
            result.startsWith(QLatin1String("error: "))
                ? result.mid(7) : result);
    }
}

void RustSdkMatrixClient::handleEncryptedSendOk(const QJsonObject &event)
{
    const QString txnId = event.value(QStringLiteral("transaction_id")).toString();
    const QString serverEventId = event.value(QStringLiteral("event_id")).toString();
    const auto it = m_pendingProbes.find(txnId);
    if (it == m_pendingProbes.end())
        return;
    const PendingProbe probe = it.value();
    m_pendingProbes.erase(it);
    Q_EMIT encryptedSendProbeResult(probe.roomId, probe.marker, true,
                                    serverEventId, QString());
}

void RustSdkMatrixClient::handleEncryptedSendFailed(const QJsonObject &event)
{
    const QString txnId = event.value(QStringLiteral("transaction_id")).toString();
    const QString message = event.value(QStringLiteral("message")).toString(
        tr("Rust SDK encrypted send probe failed."));
    const auto it = m_pendingProbes.find(txnId);
    if (it == m_pendingProbes.end()) {
        Q_EMIT errorOccurred(tr("Encrypted send probe failed: %1").arg(message));
        return;
    }
    const PendingProbe probe = it.value();
    m_pendingProbes.erase(it);
    Q_EMIT encryptedSendProbeResult(probe.roomId, probe.marker, false,
                                    QString(), message);
}

void RustSdkMatrixClient::failPendingSend(const QString &transactionId, const QString &message)
{
    const auto pendingIt = m_pendingSends.find(transactionId);
    if (pendingIt == m_pendingSends.end()) {
        if (!message.isEmpty())
            Q_EMIT errorOccurred(tr("Send failed: %1").arg(message));
        return;
    }

    const PendingSend pending = pendingIt.value();
    m_pendingSends.erase(pendingIt);

    auto &timeline = m_timelines[pending.roomId];
    for (auto &timelineEvent : timeline) {
        if (timelineEvent.eventId == pending.localEventId) {
            timelineEvent.status = TimelineEvent::Failed;
            Q_EMIT eventStatusChanged(pending.roomId,
                                      pending.localEventId,
                                      TimelineEvent::Failed);
            break;
        }
    }

    if (!message.isEmpty())
        Q_EMIT errorOccurred(tr("Send failed: %1").arg(message));
}

// ---------------------------------------------------------------------------
// Conversation creation, membership, room editing, media bridge.
//
// Each command generates an op id, dispatches to Rust and returns the id (0
// on synchronous rejection). Results arrive on the poll queue; stale handle
// generations are rejected there and Rust stamps its lifecycle.
// ---------------------------------------------------------------------------

quint64 RustSdkMatrixClient::searchUsers(const QString &query, int limit)
{
    if (!m_rustHandle || query.trimmed().isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray q = query.toUtf8();
    const QString result = takeRustString(mx_rust_search_users(
        m_rustHandle, q.constData(),
        static_cast<unsigned long long>(qBound(1, limit, 50)), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "user search rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::fetchUserProfile(const QString &userId)
{
    if (!m_rustHandle || userId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray user = userId.toUtf8();
    const QString result = takeRustString(mx_rust_get_user_profile(
        m_rustHandle, user.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "profile lookup rejected";
        return 0;
    }
    return opId;
}

void RustSdkMatrixClient::setOwnDisplayName(const QString &name, quint64 opId)
{
    // Reported, never dropped, or the Settings editor spins forever. Posted
    // rather than emitted inline: the caller records the op id after this
    // returns.
    const auto refuse = [this, opId] {
        QMetaObject::invokeMethod(this, [this, opId] {
            Q_EMIT ownDisplayNameChanged(opId, false, QString());
        }, Qt::QueuedConnection);
    };
    if (!m_rustHandle || !m_loggedIn) {
        refuse();
        return;
    }
    // An empty payload means clear (Rust maps it to None). Never trimmed or
    // filtered here; Rust bounds it by characters.
    const QByteArray payload = name.toUtf8();
    const QString result = takeRustString(mx_rust_set_display_name(
        m_rustHandle, payload.constData(), opId));
    if (!result.isEmpty()) {
        // A literal tag only: the rejection can echo the submitted name.
        qCWarning(lcRust) << "display-name write rejected";
        refuse();
    }
}

void RustSdkMatrixClient::setOwnAvatar(const QString &localPath, quint64 opId)
{
    // Posted, not emitted inline: the caller records the op id after this
    // returns.
    const auto refuse = [this, opId] {
        QMetaObject::invokeMethod(this, [this, opId] {
            Q_EMIT ownAvatarChanged(opId, false, QString());
        }, Qt::QueuedConnection);
    };
    if (!m_rustHandle || !m_loggedIn || localPath.isEmpty()) {
        refuse();
        return;
    }
    const QByteArray payload = localPath.toUtf8();
    const QString result = takeRustString(mx_rust_set_own_avatar(
        m_rustHandle, payload.constData(), opId));
    if (!result.isEmpty()) {
        // A literal tag only: the rejection can echo the path, which contains
        // the user's name.
        qCWarning(lcRust) << "own-avatar write rejected";
        refuse();
    }
}

void RustSdkMatrixClient::clearOwnAvatar(quint64 opId)
{
    const auto refuse = [this, opId] {
        QMetaObject::invokeMethod(this, [this, opId] {
            Q_EMIT ownAvatarChanged(opId, false, QString());
        }, Qt::QueuedConnection);
    };
    if (!m_rustHandle || !m_loggedIn) {
        refuse();
        return;
    }
    const QString result =
        takeRustString(mx_rust_clear_own_avatar(m_rustHandle, opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "own-avatar clear rejected";
        refuse();
    }
}

quint64 RustSdkMatrixClient::fetchMutualRooms(const QString &userId)
{
    if (!m_rustHandle || !m_loggedIn || userId.isEmpty())
        return 0;
    const quint64 op = nextOpId();
    const QByteArray payload = userId.toUtf8();
    const QString result = takeRustString(
        mx_rust_mutual_rooms(m_rustHandle, payload.constData(), op));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "mutual-rooms request rejected";
        return 0;
    }
    return op;
}

quint64 RustSdkMatrixClient::fetchUrlPreview(const QString &url)
{
    // Rust enforces the scheme allow-list too; this keeps unsafe schemes from
    // crossing the FFI at all.
    const QString lowered = url.trimmed().toLower();
    if (!m_rustHandle
        || !lowered.startsWith(QLatin1String("https://")))
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray target = url.toUtf8();
    const QString result = takeRustString(mx_rust_get_url_preview(
        m_rustHandle, target.constData(), opId));
    if (!result.isEmpty()) {
        // No URL in the log.
        qCWarning(lcRust) << "url preview rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::gifGet(const QString &url)
{
    // https-only guard before the FFI. The URL carries the provider key, so it
    // is never logged here or in Rust.
    if (!m_rustHandle || !url.trimmed().toLower().startsWith(QLatin1String("https://")))
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray target = url.toUtf8();
    const QString result =
        takeRustString(mx_rust_gif_get(m_rustHandle, target.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "gif request rejected"; // no URL
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::gifDownload(const QString &url)
{
    if (!m_rustHandle || !url.trimmed().toLower().startsWith(QLatin1String("https://")))
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray target = url.toUtf8();
    const QString result = takeRustString(
        mx_rust_gif_download(m_rustHandle, target.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "gif download rejected"; // no URL
        return 0;
    }
    return opId;
}

QVariantList RustSdkMatrixClient::existingDirectRooms(const QString &userId) const
{
    if (!m_rustHandle || userId.isEmpty())
        return {};
    const QByteArray user = userId.toUtf8();
    const QString payload =
        takeRustString(mx_rust_get_dm_rooms(m_rustHandle, user.constData()));
    if (payload.isEmpty() || payload.startsWith(QLatin1String("error:")))
        return {};
    const QJsonObject obj = QJsonDocument::fromJson(payload.toUtf8()).object();
    QVariantList out;
    const QJsonArray rooms = obj.value(QStringLiteral("rooms")).toArray();
    for (const QJsonValue &value : rooms) {
        const QJsonObject room = value.toObject();
        QVariantMap entry;
        entry.insert(QStringLiteral("roomId"),
                     room.value(QStringLiteral("room_id")).toString());
        entry.insert(QStringLiteral("name"),
                     room.value(QStringLiteral("name")).toString());
        out.append(entry);
    }
    return out;
}

quint64 RustSdkMatrixClient::createDirectChat(const QString &userId)
{
    if (!m_rustHandle || userId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray user = userId.toUtf8();
    const QString result =
        takeRustString(mx_rust_create_dm(m_rustHandle, user.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "create DM rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::createRoom(const QVariantMap &options)
{
    if (!m_rustHandle)
        return 0;
    QJsonObject payload;
    payload.insert(QStringLiteral("name"),
                   options.value(QStringLiteral("name")).toString());
    payload.insert(QStringLiteral("topic"),
                   options.value(QStringLiteral("topic")).toString());
    payload.insert(QStringLiteral("public"),
                   options.value(QStringLiteral("public")).toBool());
    payload.insert(QStringLiteral("encrypted"),
                   options.value(QStringLiteral("encrypted")).toBool());
    payload.insert(QStringLiteral("alias"),
                   options.value(QStringLiteral("alias")).toString());
    payload.insert(QStringLiteral("space_id"),
                   options.value(QStringLiteral("spaceId")).toString());
    payload.insert(QStringLiteral("is_space"),
                   options.value(QStringLiteral("isSpace")).toBool());
    payload.insert(QStringLiteral("invites"),
                   QJsonArray::fromStringList(
                       options.value(QStringLiteral("invites")).toStringList()));
    const quint64 opId = nextOpId();
    const QByteArray json =
        QJsonDocument(payload).toJson(QJsonDocument::Compact);
    const QString result = takeRustString(
        mx_rust_create_room(m_rustHandle, json.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "create room rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::inviteUsers(const QString &roomId,
                                         const QStringList &userIds)
{
    if (!m_rustHandle || roomId.isEmpty() || userIds.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray users =
        QJsonDocument(QJsonArray::fromStringList(userIds))
            .toJson(QJsonDocument::Compact);
    const QString result = takeRustString(mx_rust_invite_users(
        m_rustHandle, room.constData(), users.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "invite command rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::requestRoomMembers(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_room_members(m_rustHandle, room.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "member snapshot rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::setRoomName(const QString &roomId, const QString &name)
{
    if (!m_rustHandle || roomId.isEmpty() || name.trimmed().isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray value = name.toUtf8();
    const QString result = takeRustString(mx_rust_set_room_name(
        m_rustHandle, room.constData(), value.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::setRoomTopic(const QString &roomId, const QString &topic)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray value = topic.toUtf8();
    const QString result = takeRustString(mx_rust_set_room_topic(
        m_rustHandle, room.constData(), value.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::setRoomAvatar(const QString &roomId,
                                           const QString &localPath)
{
    if (!m_rustHandle || roomId.isEmpty() || localPath.isEmpty())
        return 0;
    const QFileInfo info(localPath);
    if (!info.isFile() || !info.isReadable() || info.size() <= 0)
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray path = localPath.toUtf8();
    const QString result = takeRustString(mx_rust_set_room_avatar(
        m_rustHandle, room.constData(), path.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::removeRoomAvatar(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_remove_room_avatar(m_rustHandle, room.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::leaveRoom(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_leave_room(m_rustHandle, room.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::kickUser(const QString &roomId,
                                      const QString &userId,
                                      const QString &reason)
{
    return moderateUser(roomId, userId, reason, 0);
}

quint64 RustSdkMatrixClient::banUser(const QString &roomId,
                                     const QString &userId,
                                     const QString &reason)
{
    return moderateUser(roomId, userId, reason, 1);
}

quint64 RustSdkMatrixClient::unbanUser(const QString &roomId,
                                       const QString &userId,
                                       const QString &reason)
{
    return moderateUser(roomId, userId, reason, 2);
}

quint64 RustSdkMatrixClient::moderateUser(const QString &roomId,
                                          const QString &userId,
                                          const QString &reason, int op)
{
    if (!m_rustHandle || roomId.isEmpty() || userId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray user = userId.toUtf8();
    const QByteArray why = reason.toUtf8();
    const QString result = takeRustString(mx_rust_moderate_user(
        m_rustHandle, room.constData(), user.constData(), why.constData(),
        static_cast<unsigned char>(op), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::setMemberPowerLevel(const QString &roomId,
                                                 const QString &userId,
                                                 qlonglong level)
{
    if (!m_rustHandle || roomId.isEmpty() || userId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray user = userId.toUtf8();
    const QString result = takeRustString(mx_rust_set_member_power_level(
        m_rustHandle, room.constData(), user.constData(),
        static_cast<long long>(level), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::setRoomPowerLevelKey(const QString &roomId,
                                                  const QString &key,
                                                  qlonglong level)
{
    if (!m_rustHandle || roomId.isEmpty() || key.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray levelKey = key.toUtf8();
    const QString result = takeRustString(mx_rust_set_room_power_level_key(
        m_rustHandle, room.constData(), levelKey.constData(),
        static_cast<long long>(level), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::setRoomJoinRule(const QString &roomId,
                                             const QString &rule)
{
    return setRoomJoinRule(roomId, rule, QStringList());
}

quint64 RustSdkMatrixClient::setRoomJoinRule(const QString &roomId,
                                             const QString &rule,
                                             const QStringList &allowedRoomIds)
{
    if (!m_rustHandle || roomId.isEmpty() || rule.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray value = rule.toUtf8();
    const QByteArray allowed = allowedRoomIds.join(QLatin1Char('\n')).toUtf8();
    const QString result = takeRustString(mx_rust_set_room_join_rule(
        m_rustHandle, room.constData(), value.constData(),
        allowedRoomIds.isEmpty() ? nullptr : allowed.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::setRoomHistoryVisibility(const QString &roomId,
                                                      const QString &visibility)
{
    if (!m_rustHandle || roomId.isEmpty() || visibility.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray value = visibility.toUtf8();
    const QString result = takeRustString(mx_rust_set_room_history_visibility(
        m_rustHandle, room.constData(), value.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::setRoomGuestAccess(const QString &roomId,
                                                const QString &access)
{
    if (!m_rustHandle || roomId.isEmpty() || access.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray value = access.toUtf8();
    const QString result = takeRustString(mx_rust_set_room_guest_access(
        m_rustHandle, room.constData(), value.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

void RustSdkMatrixClient::requestRoomDirectoryVisibility(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty())
        return;
    const QByteArray room = roomId.toUtf8();
    takeRustString(mx_rust_request_room_directory_visibility(
        m_rustHandle, room.constData()));
}

quint64 RustSdkMatrixClient::setRoomDirectoryVisibility(const QString &roomId,
                                                        bool published)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_set_room_directory_visibility(
        m_rustHandle, room.constData(), published, opId));
    return result.isEmpty() ? opId : 0;
}

void RustSdkMatrixClient::probeDelayedEvents()
{
    if (!m_rustHandle)
        return;
    takeRustString(mx_rust_probe_delayed_events(m_rustHandle));
}

quint64 RustSdkMatrixClient::scheduleMessage(const QString &roomId,
                                             const QString &body,
                                             const QVariantMap &bodySpec,
                                             const QStringList &mentionUserIds,
                                             qint64 delayMs)
{
    if (!m_rustHandle || roomId.isEmpty() || body.isEmpty() || delayMs <= 0)
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray bodyBytes = body.toUtf8();
    const QByteArray specBytes = bodySpecJson(bodySpec);
    const QByteArray mentionBytes = mentionUserIds.join(QLatin1Char('\n')).toUtf8();
    const QString result = takeRustString(mx_rust_schedule_message(
        m_rustHandle, room.constData(), bodyBytes.constData(),
        specBytes.isEmpty() ? nullptr : specBytes.constData(),
        mentionUserIds.isEmpty() ? nullptr : mentionBytes.constData(),
        static_cast<unsigned long long>(delayMs), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::updateScheduledMessage(const QString &delayId,
                                                    const QString &action)
{
    if (!m_rustHandle || delayId.isEmpty() || action.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray id = delayId.toUtf8();
    const QByteArray act = action.toUtf8();
    const QString result = takeRustString(mx_rust_update_scheduled_message(
        m_rustHandle, id.constData(), act.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::sendRoomMessage(const QString &roomId,
                                             const QString &body,
                                             const QVariantMap &bodySpec,
                                             const QStringList &mentionUserIds,
                                             const QString &replyToEventId,
                                             const QString &threadRootEventId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty() || body.isEmpty())
        return 0;
    const quint64 op = nextOpId();
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray bodyBytes = body.toUtf8();
    const QByteArray mentionBytes = mentionUserIds.join(QLatin1Char('\n')).toUtf8();
    const QByteArray specBytes = bodySpecJson(bodySpec);
    const QByteArray replyBytes = replyToEventId.toUtf8();
    const QByteArray rootBytes = threadRootEventId.toUtf8();
    const QString result = takeRustString(mx_rust_send_room_message(
        m_rustHandle, roomBytes.constData(), bodyBytes.constData(),
        mentionUserIds.isEmpty() ? nullptr : mentionBytes.constData(),
        specBytes.constData(),
        replyToEventId.isEmpty() ? nullptr : replyBytes.constData(),
        threadRootEventId.isEmpty() ? nullptr : rootBytes.constData(), op));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7) : result);
        return 0;
    }
    return op;
}

void RustSdkMatrixClient::requestActivitySeed(int limit)
{
    if (!m_loggedIn || !m_rustHandle)
        return;
    const QString result = takeRustString(mx_rust_request_activity_seed(
        m_rustHandle, static_cast<unsigned int>(qBound(1, limit, 100))));
    if (!result.isEmpty())
        qCDebug(lcRust) << "activity seed request refused";
}

void RustSdkMatrixClient::requestEditHistory(const QString &roomId,
                                             const QString &eventId)
{
    if (!m_rustHandle || roomId.isEmpty() || eventId.isEmpty())
        return;
    const QByteArray room = roomId.toUtf8();
    const QByteArray ev = eventId.toUtf8();
    takeRustString(mx_rust_request_edit_history(m_rustHandle, room.constData(),
                                                ev.constData()));
}

void RustSdkMatrixClient::requestEventSource(const QString &roomId,
                                             const QString &eventId)
{
    if (!m_rustHandle || roomId.isEmpty() || eventId.isEmpty())
        return;
    const QByteArray room = roomId.toUtf8();
    const QByteArray ev = eventId.toUtf8();
    takeRustString(mx_rust_request_event_source(m_rustHandle, room.constData(),
                                                ev.constData()));
}

quint64 RustSdkMatrixClient::eventAtTimestamp(const QString &roomId,
                                             qint64 timestampMs)
{
    if (!m_rustHandle || roomId.isEmpty() || timestampMs <= 0)
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    takeRustString(mx_rust_event_at_timestamp(
        m_rustHandle, room.constData(),
        static_cast<long long>(timestampMs),
        static_cast<unsigned long long>(opId)));
    return opId;
}

// ── Local message search ────────────────────────────────────────────────
//
// These answer through the poll loop, except forget/clear: synchronous SQLite
// deletes, so a redacted message is never briefly still findable.

quint64 RustSdkMatrixClient::localSearch(const QString &query,
                                         const QString &roomId,
                                         int limit, int offset)
{
    if (!m_rustHandle || query.trimmed().isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray q = query.toUtf8();
    const QByteArray room = roomId.toUtf8();
    takeRustString(mx_rust_local_search(
        m_rustHandle, q.constData(), room.constData(), limit, offset,
        static_cast<unsigned long long>(opId)));
    return opId;
}

quint64 RustSdkMatrixClient::setRoomDisplayName(const QString &roomId,
                                                const QString &name)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray value = name.toUtf8();
    takeRustString(mx_rust_set_room_member_display_name(
        m_rustHandle, room.constData(), value.constData(),
        static_cast<unsigned long long>(opId)));
    return opId;
}

quint64 RustSdkMatrixClient::setRoomMemberAvatar(const QString &roomId,
                                                 const QString &mxc)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray value = mxc.toUtf8();
    takeRustString(mx_rust_set_room_member_avatar(
        m_rustHandle, room.constData(), value.constData(),
        static_cast<unsigned long long>(opId)));
    return opId;
}

quint64 RustSdkMatrixClient::requestMediaHistoryPage(const QString &roomId,
                                                    int limit, bool restart)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    takeRustString(mx_rust_media_history_page(
        m_rustHandle, room.constData(),
        static_cast<unsigned int>(limit < 0 ? 0 : limit),
        restart ? 1 : 0,
        static_cast<unsigned long long>(opId)));
    return opId;
}

quint64 RustSdkMatrixClient::writeRoomWidget(const QString &roomId,
                                             const QString &widgetId,
                                             const QString &contentJson)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty() || widgetId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray id = widgetId.toUtf8();
    const QByteArray content = contentJson.toUtf8();
    const QString result = takeRustString(mx_rust_room_widget_write(
        m_rustHandle, room.constData(), id.constData(), content.constData(),
        opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "widget write rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::roomWidgets(const QString &roomId,
                                        const QString &theme,
                                        const QString &language)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray th = theme.toUtf8();
    const QByteArray lang = language.toUtf8();
    takeRustString(mx_rust_room_widgets(
        m_rustHandle, room.constData(), th.constData(), lang.constData(),
        static_cast<unsigned long long>(opId)));
    return opId;
}

quint64 RustSdkMatrixClient::roomBridges(const QString &roomId,
                                        bool allowNetwork)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    takeRustString(mx_rust_room_bridges(
        m_rustHandle, room.constData(),
        static_cast<unsigned char>(allowNetwork ? 1 : 0),
        static_cast<unsigned long long>(opId)));
    return opId;
}

quint64 RustSdkMatrixClient::searchIndexStats()
{
    if (!m_rustHandle)
        return 0;
    const quint64 opId = nextOpId();
    takeRustString(mx_rust_search_index_stats(
        m_rustHandle, static_cast<unsigned long long>(opId)));
    return opId;
}

quint64 RustSdkMatrixClient::sweepSearchIndex()
{
    if (!m_rustHandle)
        return 0;
    const quint64 opId = nextOpId();
    takeRustString(mx_rust_search_index_sweep(
        m_rustHandle, static_cast<unsigned long long>(opId)));
    return opId;
}

quint64 RustSdkMatrixClient::deepenSearchIndex(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    takeRustString(mx_rust_search_index_deep(
        m_rustHandle, room.constData(),
        static_cast<unsigned long long>(opId)));
    return opId;
}

void RustSdkMatrixClient::forgetIndexedEvent(const QString &eventId)
{
    if (!m_rustHandle || eventId.isEmpty())
        return;
    const QByteArray id = eventId.toUtf8();
    takeRustString(mx_rust_search_index_forget_event(m_rustHandle,
                                                     id.constData()));
}

void RustSdkMatrixClient::forgetIndexedRoom(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty())
        return;
    const QByteArray id = roomId.toUtf8();
    takeRustString(mx_rust_search_index_forget_room(m_rustHandle,
                                                    id.constData()));
}

void RustSdkMatrixClient::clearSearchIndex()
{
    if (!m_rustHandle)
        return;
    takeRustString(mx_rust_search_index_clear(m_rustHandle));
}

quint64 RustSdkMatrixClient::renameDevice(const QString &deviceId,
                                          const QString &name)
{
    if (!m_rustHandle || deviceId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray id = deviceId.toUtf8();
    const QByteArray value = name.toUtf8();
    const QString result = takeRustString(mx_rust_rename_device(
        m_rustHandle, id.constData(), value.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::backupAction(const QString &action)
{
    if (!m_rustHandle || action.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray value = action.toUtf8();
    const QString result = takeRustString(
        mx_rust_backup_action(m_rustHandle, value.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

void RustSdkMatrixClient::requestBackupProgress()
{
    if (!m_rustHandle)
        return;
    takeRustString(mx_rust_request_backup_progress(m_rustHandle));
}

void RustSdkMatrixClient::requestRoomVersions()
{
    if (!m_rustHandle)
        return;
    takeRustString(mx_rust_request_room_versions(m_rustHandle));
}

quint64 RustSdkMatrixClient::upgradeRoom(const QString &roomId,
                                         const QString &newVersion)
{
    if (!m_rustHandle || roomId.isEmpty() || newVersion.trimmed().isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray version = newVersion.trimmed().toUtf8();
    const QString result = takeRustString(mx_rust_upgrade_room(
        m_rustHandle, room.constData(), version.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::setRoomAltAliases(const QString &roomId,
                                               const QStringList &aliases)
{
    // An empty list is meaningful: it clears every alternative alias.
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray value = aliases.join(QLatin1Char('\n')).toUtf8();
    const QString result = takeRustString(mx_rust_set_room_alt_aliases(
        m_rustHandle, room.constData(), value.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::setRoomCanonicalAlias(const QString &roomId,
                                                   const QString &alias)
{
    // An empty alias is meaningful (it clears the canonical alias), so the
    // empty string is accepted here.
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray value = alias.toUtf8();
    const QString result = takeRustString(mx_rust_set_room_canonical_alias(
        m_rustHandle, room.constData(), value.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::requestPinnedMessages(const QString &roomId,
                                                   bool allowRemote)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_room_pinned(
        m_rustHandle, room.constData(),
        allowRemote ? static_cast<unsigned char>(1)
                    : static_cast<unsigned char>(0),
        opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::setEventPinned(const QString &roomId,
                                            const QString &eventId, bool pin)
{
    if (!m_rustHandle || roomId.isEmpty() || eventId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray event = eventId.toUtf8();
    const QString result = takeRustString(mx_rust_set_room_pinned(
        m_rustHandle, room.constData(), event.constData(),
        pin ? static_cast<unsigned char>(1) : static_cast<unsigned char>(0),
        opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::resolveRoomTarget(const QString &input)
{
    if (!m_rustHandle || input.trimmed().isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray value = input.toUtf8();
    const QString result = takeRustString(mx_rust_resolve_room_target(
        m_rustHandle, value.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::searchPublicRooms(const QString &query,
                                               const QString &server,
                                               const QString &since, int limit)
{
    if (!m_rustHandle)
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray queryBytes = query.toUtf8();
    const QByteArray serverBytes = server.toUtf8();
    const QByteArray sinceBytes = since.toUtf8();
    const QString result = takeRustString(mx_rust_search_public_rooms(
        m_rustHandle, queryBytes.constData(), serverBytes.constData(),
        sinceBytes.constData(),
        static_cast<unsigned long long>(qMax(1, limit)), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::joinRoomByIdOrAlias(const QString &target,
                                                 const QStringList &via)
{
    if (!m_rustHandle || target.trimmed().isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray targetBytes = target.toUtf8();
    const QByteArray viaBytes = via.join(QLatin1Char('\n')).toUtf8();
    const QString result = takeRustString(mx_rust_join_room(
        m_rustHandle, targetBytes.constData(), viaBytes.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::knockRoom(const QString &target,
                                        const QStringList &via,
                                        const QString &reason)
{
    if (!m_rustHandle || target.trimmed().isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray targetBytes = target.toUtf8();
    const QByteArray viaBytes = via.join(QLatin1Char('\n')).toUtf8();
    const QByteArray reasonBytes = reason.toUtf8();
    const QString result = takeRustString(mx_rust_knock_room(
        m_rustHandle, targetBytes.constData(), viaBytes.constData(),
        reasonBytes.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::cancelKnock(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_cancel_knock(m_rustHandle, room.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

// ── Voice-call signaling sends ─────────────────────────────────────────
// SDP crosses once, into the FFI call, and is never logged, stored or
// echoed. Results arrive as call_send_result; inbound observations as call_*
// events (see CallSignal.h).

quint64 RustSdkMatrixClient::callInvite(const QString &roomId,
                                        const QString &callId,
                                        const QString &partyId,
                                        const QString &offerType,
                                        const QString &offerSdp,
                                        quint64 lifetimeMs,
                                        const QString &invitee)
{
    if (!m_rustHandle || roomId.isEmpty() || callId.isEmpty()
        || partyId.isEmpty() || offerSdp.trimmed().isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray call = callId.toUtf8();
    const QByteArray party = partyId.toUtf8();
    const QByteArray type = offerType.toUtf8();
    const QByteArray sdp = offerSdp.toUtf8();
    const QByteArray target = invitee.toUtf8();
    const QString result = takeRustString(mx_rust_calls_invite(
        m_rustHandle, room.constData(), call.constData(), party.constData(),
        type.constData(), sdp.constData(), lifetimeMs, target.constData(),
        opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::callAnswer(const QString &roomId,
                                        const QString &callId,
                                        const QString &partyId,
                                        const QString &answerType,
                                        const QString &answerSdp)
{
    if (!m_rustHandle || roomId.isEmpty() || callId.isEmpty()
        || partyId.isEmpty() || answerSdp.trimmed().isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray call = callId.toUtf8();
    const QByteArray party = partyId.toUtf8();
    const QByteArray type = answerType.toUtf8();
    const QByteArray sdp = answerSdp.toUtf8();
    const QString result = takeRustString(mx_rust_calls_answer(
        m_rustHandle, room.constData(), call.constData(), party.constData(),
        type.constData(), sdp.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::callReject(const QString &roomId,
                                        const QString &callId,
                                        const QString &partyId)
{
    if (!m_rustHandle || roomId.isEmpty() || callId.isEmpty()
        || partyId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray call = callId.toUtf8();
    const QByteArray party = partyId.toUtf8();
    const QString result = takeRustString(mx_rust_calls_reject(
        m_rustHandle, room.constData(), call.constData(), party.constData(),
        opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::callHangup(const QString &roomId,
                                        const QString &callId,
                                        const QString &partyId,
                                        const QString &reason)
{
    if (!m_rustHandle || roomId.isEmpty() || callId.isEmpty()
        || partyId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray call = callId.toUtf8();
    const QByteArray party = partyId.toUtf8();
    const QByteArray reasonBytes = reason.toUtf8();
    const QString result = takeRustString(mx_rust_calls_hangup(
        m_rustHandle, room.constData(), call.constData(), party.constData(),
        reasonBytes.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::callSelectAnswer(const QString &roomId,
                                              const QString &callId,
                                              const QString &partyId,
                                              const QString &selectedPartyId)
{
    if (!m_rustHandle || roomId.isEmpty() || callId.isEmpty()
        || partyId.isEmpty() || selectedPartyId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray call = callId.toUtf8();
    const QByteArray party = partyId.toUtf8();
    const QByteArray selected = selectedPartyId.toUtf8();
    const QString result = takeRustString(mx_rust_calls_select_answer(
        m_rustHandle, room.constData(), call.constData(), party.constData(),
        selected.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::callRtcDecline(const QString &roomId,
                                            const QString &notificationEventId)
{
    if (!m_rustHandle || roomId.isEmpty() || notificationEventId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray event = notificationEventId.toUtf8();
    const QString result = takeRustString(mx_rust_calls_rtc_decline(
        m_rustHandle, room.constData(), event.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::rtcSession(const QString &roomId,
                                       bool preferServer)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_rtc_session(
        m_rustHandle, room.constData(), preferServer ? 1 : 0, opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::rtcTransports(const QString &roomId)
{
    if (!m_rustHandle)
        return 0;
    // An empty room id is legal: discovery is account-scoped and the room only
    // adds the participant-advertised fallback focus.
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_rtc_transports(m_rustHandle, room.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::rtcPublishMembership(const QString &roomId,
                                                 const QString &focusUrl,
                                                 const QString &intent)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray focus = focusUrl.toUtf8();
    const QByteArray callIntent = intent.toUtf8();
    const QString result = takeRustString(mx_rust_rtc_publish_membership(
        m_rustHandle, room.constData(), focus.constData(),
        callIntent.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::rtcRestartDelayedLeave(const QString &delayId)
{
    if (!m_rustHandle || delayId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray id = delayId.toUtf8();
    const QString result = takeRustString(mx_rust_rtc_restart_delayed_leave(
        m_rustHandle, id.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::rtcRetractMembership(const QString &roomId,
                                                  const QString &delayId)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray id = delayId.toUtf8();
    const QString result = takeRustString(mx_rust_rtc_retract_membership(
        m_rustHandle, room.constData(), id.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::rtcSendMediaKey(const QString &roomId,
                                             const QString &keyBase64,
                                             int keyIndex,
                                             const QString &targetsJson)
{
    if (!m_rustHandle || roomId.isEmpty() || keyBase64.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    // SENSITIVE: keyBase64 is raw media key material. It goes straight into the
    // FFI and is never logged, stored or echoed.
    const QByteArray room = roomId.toUtf8();
    const QByteArray key = keyBase64.toUtf8();
    const QByteArray targets = targetsJson.toUtf8();
    const QString result = takeRustString(mx_rust_rtc_send_media_key(
        m_rustHandle, room.constData(), key.constData(),
        static_cast<unsigned char>(qBound(0, keyIndex, 15)),
        targets.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::sfuConnect(const QString &serviceUrl,
                                        const QString &roomId)
{
    if (!m_rustHandle || serviceUrl.isEmpty() || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray url = serviceUrl.toUtf8();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_sfu_connect(
        m_rustHandle, url.constData(), room.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

void RustSdkMatrixClient::sfuLocalDescription(const QString &kind,
                                              const QString &target,
                                              const QString &sdp)
{
    // SDP carries host IPs: opaque here, never logged.
    if (!m_rustHandle || sdp.isEmpty())
        return;
    const QByteArray k = kind.toUtf8();
    const QByteArray t = target.toUtf8();
    const QByteArray s = sdp.toUtf8();
    takeRustString(mx_rust_sfu_local_description(
        m_rustHandle, k.constData(), t.constData(), s.constData()));
}

void RustSdkMatrixClient::sfuLocalCandidate(const QString &target,
                                            const QString &candidateInit)
{
    if (!m_rustHandle || candidateInit.isEmpty())
        return;
    const QByteArray t = target.toUtf8();
    const QByteArray c = candidateInit.toUtf8();
    takeRustString(mx_rust_sfu_local_candidate(m_rustHandle, t.constData(),
                                               c.constData()));
}

void RustSdkMatrixClient::sfuAddTrack(const QString &cid, const QString &name,
                                      int kind, int width, int height,
                                      bool screenShare, bool encrypted)
{
    if (!m_rustHandle || cid.isEmpty())
        return;
    const QByteArray c = cid.toUtf8();
    const QByteArray n = name.toUtf8();
    takeRustString(mx_rust_sfu_add_track(
        m_rustHandle, c.constData(), n.constData(), kind,
        static_cast<unsigned int>(qMax(0, width)),
        static_cast<unsigned int>(qMax(0, height)),
        screenShare ? 1 : 0, encrypted ? 1 : 0));
}

void RustSdkMatrixClient::sfuMuteTrack(const QString &sid, bool muted)
{
    if (!m_rustHandle || sid.isEmpty())
        return;
    const QByteArray s = sid.toUtf8();
    takeRustString(
        mx_rust_sfu_mute_track(m_rustHandle, s.constData(), muted ? 1 : 0));
}

void RustSdkMatrixClient::sfuDisconnect()
{
    if (!m_rustHandle)
        return;
    takeRustString(mx_rust_sfu_disconnect(m_rustHandle));
}

quint64 RustSdkMatrixClient::rtcSendCallReaction(
    const QString &roomId, const QString &membershipEventId,
    const QString &emoji, const QString &name)
{
    if (!m_rustHandle || roomId.isEmpty() || membershipEventId.isEmpty()
        || emoji.isEmpty() || name.isEmpty()) {
        return 0;
    }
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray membership = membershipEventId.toUtf8();
    const QByteArray emojiBytes = emoji.toUtf8();
    const QByteArray nameBytes = name.toUtf8();
    // Rust validates the (emoji, name) pair against element-call's table:
    // Element looks up a reaction's sound by name.
    const QString result = takeRustString(mx_rust_rtc_send_call_reaction(
        m_rustHandle, room.constData(), membership.constData(),
        emojiBytes.constData(), nameBytes.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::rtcSetHandRaised(
    const QString &roomId, const QString &membershipEventId,
    const QString &reactionEventId, bool raised)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    // A raise annotates the membership and a lower redacts the raise's
    // reaction; each needs its own id.
    if (raised ? membershipEventId.isEmpty() : reactionEventId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray membership = membershipEventId.toUtf8();
    const QByteArray reaction = reactionEventId.toUtf8();
    const QString result = takeRustString(mx_rust_rtc_set_hand(
        m_rustHandle, room.constData(), membership.constData(),
        reaction.constData(), raised, opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::rtcReadRaisedHands(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_rtc_read_hands(m_rustHandle, room.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::rtcNotify(const QString &roomId,
                                       const QString &notificationType,
                                       const QString &intent,
                                       quint64 lifetimeMs,
                                       const QString &membershipEventId)
{
    if (!m_rustHandle || roomId.isEmpty() || notificationType.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray type = notificationType.toUtf8();
    const QByteArray callIntent = intent.toUtf8();
    const QByteArray membership = membershipEventId.toUtf8();
    const QString result = takeRustString(mx_rust_rtc_notify(
        m_rustHandle, room.constData(), type.constData(),
        callIntent.constData(), lifetimeMs, membership.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::callCandidates(const QString &roomId,
                                            const QString &callId,
                                            const QString &partyId,
                                            const QVariantList &candidates)
{
    if (!m_rustHandle || roomId.isEmpty() || callId.isEmpty()
        || partyId.isEmpty() || candidates.isEmpty())
        return 0;
    QJsonArray entries;
    for (const QVariant &value : candidates) {
        const QVariantMap map = value.toMap();
        QJsonObject entry;
        entry.insert(QStringLiteral("candidate"),
                     map.value(QStringLiteral("candidate")).toString());
        if (map.contains(QStringLiteral("sdpMid")))
            entry.insert(QStringLiteral("sdp_mid"),
                         map.value(QStringLiteral("sdpMid")).toString());
        if (map.contains(QStringLiteral("sdpMLineIndex")))
            entry.insert(QStringLiteral("sdp_m_line_index"),
                         map.value(QStringLiteral("sdpMLineIndex")).toInt());
        entries.append(entry);
    }
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray call = callId.toUtf8();
    const QByteArray party = partyId.toUtf8();
    const QByteArray json =
        QJsonDocument(entries).toJson(QJsonDocument::Compact);
    const QString result = takeRustString(mx_rust_calls_candidates(
        m_rustHandle, room.constData(), call.constData(), party.constData(),
        json.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::requestCallTurnServers()
{
    if (!m_rustHandle)
        return 0;
    const quint64 opId = nextOpId();
    const QString result =
        takeRustString(mx_rust_calls_turn_servers(m_rustHandle, opId));
    return result.isEmpty() ? opId : 0;
}

void RustSdkMatrixClient::setCallMediaCapable(bool capable)
{
    // Cached so a recreated handle re-learns the mode instead of reverting to
    // off while a media backend is registered.
    m_callMediaCapable = capable;
    if (m_rustHandle)
        takeRustString(mx_rust_calls_set_media_capable(
            m_rustHandle, capable ? static_cast<unsigned char>(1)
                                  : static_cast<unsigned char>(0)));
    if (!capable)
        m_callSdpStore.clear();
}

QString RustSdkMatrixClient::takeCallSessionDescription(const QString &eventId)
{
    return m_callSdpStore.take(eventId);
}

quint64 RustSdkMatrixClient::setUserIgnored(const QString &userId,
                                            bool ignored)
{
    if (!m_rustHandle || userId.trimmed().isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray user = userId.toUtf8();
    const QString result = takeRustString(mx_rust_set_user_ignored(
        m_rustHandle, user.constData(),
        ignored ? static_cast<unsigned char>(1)
                : static_cast<unsigned char>(0),
        opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::requestIgnoredUsers()
{
    if (!m_rustHandle)
        return 0;
    const quint64 opId = nextOpId();
    const QString result =
        takeRustString(mx_rust_list_ignored_users(m_rustHandle, opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::reportMessage(const QString &roomId,
                                           const QString &eventId,
                                           const QString &reason)
{
    if (!m_rustHandle || roomId.isEmpty() || eventId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray event = eventId.toUtf8();
    const QByteArray reasonBytes = reason.toUtf8();
    const QString result = takeRustString(mx_rust_report_message(
        m_rustHandle, room.constData(), event.constData(),
        reasonBytes.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::deleteDevices(const QStringList &deviceIds)
{
    if (!m_rustHandle || deviceIds.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray ids = deviceIds.join(QLatin1Char('\n')).toUtf8();
    const QString result = takeRustString(
        mx_rust_delete_devices(m_rustHandle, ids.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

bool RustSdkMatrixClient::uiaSubmitPassword(quint64 uiaId,
                                            const QString &password)
{
    if (!m_rustHandle || uiaId == 0 || password.isEmpty())
        return false;
    // Convert once; keep no QString copy of the password in C++. Rust scrubs
    // its own transit buffer.
    QByteArray passwordBytes = password.toUtf8();
    const QString result = takeRustString(mx_rust_uia_submit_password(
        m_rustHandle, uiaId, passwordBytes.constData()));
    // volatile so the dead-store optimizer cannot drop the zeroing.
    volatile char *raw = passwordBytes.data();
    for (int i = 0; i < passwordBytes.size(); ++i)
        raw[i] = 0;
    return result.isEmpty();
}

void RustSdkMatrixClient::uiaCancel(quint64 uiaId)
{
    if (!m_rustHandle || uiaId == 0)
        return;
    takeRustString(mx_rust_uia_cancel(m_rustHandle, uiaId));
}

quint64 RustSdkMatrixClient::requestOAuthManagementUrl(const QString &deviceId)
{
    if (!m_rustHandle)
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray device = deviceId.toUtf8();
    const QString result = takeRustString(mx_rust_oauth_management_url(
        m_rustHandle, device.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::searchMessages(const QString &term,
                                             const QString &roomId,
                                             const QString &nextBatch,
                                             int limit,
                                             const QVariantMap &filters)
{
    if (!m_rustHandle || term.trimmed().isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray termBytes = term.toUtf8();
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray batchBytes = nextBatch.toUtf8();
    const QByteArray filterBytes = QJsonDocument::fromVariant(filters)
                                       .toJson(QJsonDocument::Compact);
    const QString result = takeRustString(mx_rust_search_messages(
        m_rustHandle, termBytes.constData(), roomBytes.constData(),
        batchBytes.constData(), filterBytes.constData(),
        static_cast<unsigned long long>(qMax(1, limit)), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::requestSpaceChildren(const QString &spaceId)
{
    if (!m_rustHandle || spaceId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray space = spaceId.toUtf8();
    const QString result = takeRustString(
        mx_rust_space_children(m_rustHandle, space.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::addRoomToSpace(const QString &spaceId,
                                            const QString &roomId)
{
    if (!m_rustHandle || spaceId.isEmpty() || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray space = spaceId.toUtf8();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_add_room_to_space(
        m_rustHandle, space.constData(), room.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::setSpaceChildSuggested(const QString &spaceId,
                                                    const QString &roomId,
                                                    bool suggested)
{
    if (!m_rustHandle || spaceId.isEmpty() || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray space = spaceId.toUtf8();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_set_space_child_suggested(
        m_rustHandle, space.constData(), room.constData(),
        suggested ? 1 : 0, opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::removeRoomFromSpace(const QString &spaceId,
                                                 const QString &roomId)
{
    if (!m_rustHandle || spaceId.isEmpty() || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray space = spaceId.toUtf8();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_remove_room_from_space(
        m_rustHandle, space.constData(), room.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::sendAttachment(const QString &roomId,
                                            const QString &localPath,
                                            const QString &mime,
                                            const QString &caption,
                                            int width, int height,
                                            bool animated, qint64 durationMs)
{
    if (!m_rustHandle || roomId.isEmpty() || localPath.isEmpty() || mime.isEmpty())
        return 0;
    if (!timelineActiveFor(roomId)) {
        qCWarning(lcRust) << "attachment send requires the open room timeline";
        return 0;
    }
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray path = localPath.toUtf8();
    const QByteArray mimeBytes = mime.toUtf8();
    const QByteArray captionBytes = caption.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_send_attachment(
        m_rustHandle, room.constData(), path.constData(), mimeBytes.constData(),
        captionBytes.constData(),
        static_cast<unsigned long long>(qMax(0, width)),
        static_cast<unsigned long long>(qMax(0, height)),
        animated ? 1 : 0,
        // Clamped rather than refused: a bad clock reading must not stop the
        // send.
        static_cast<unsigned long long>(qMax<qint64>(0, durationMs)), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "attachment send rejected";
        return 0;
    }
    return opId;
}

// The poster crosses the FFI as raw bytes; Rust copies it before returning
// and re-validates it by magic sniffing. A missing or rejected poster never
// fails the video send.
quint64 RustSdkMatrixClient::sendVideo(const QString &roomId,
                                       const QString &localPath,
                                       const QString &mime,
                                       const QString &caption,
                                       int width, int height,
                                       qint64 durationMs,
                                       const QByteArray &thumbnail,
                                       int thumbnailWidth,
                                       int thumbnailHeight)
{
    if (!m_rustHandle || roomId.isEmpty() || localPath.isEmpty() || mime.isEmpty())
        return 0;
    if (!timelineActiveFor(roomId)) {
        qCWarning(lcRust) << "video send requires the open room timeline";
        return 0;
    }
    const bool hasPoster = !thumbnail.isEmpty() && thumbnailWidth > 0
        && thumbnailHeight > 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray path = localPath.toUtf8();
    const QByteArray mimeBytes = mime.toUtf8();
    const QByteArray captionBytes = caption.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_send_video(
        m_rustHandle, room.constData(), path.constData(),
        mimeBytes.constData(), captionBytes.constData(),
        static_cast<unsigned long long>(qMax(0, width)),
        static_cast<unsigned long long>(qMax(0, height)),
        static_cast<unsigned long long>(qMax<qint64>(0, durationMs)),
        hasPoster ? reinterpret_cast<const unsigned char *>(thumbnail.constData())
                  : nullptr,
        hasPoster ? static_cast<size_t>(thumbnail.size()) : 0,
        static_cast<unsigned long long>(hasPoster ? thumbnailWidth : 0),
        static_cast<unsigned long long>(hasPoster ? thumbnailHeight : 0),
        opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "video send rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::sendThreadVideo(const QString &roomId,
                                             const QString &rootEventId,
                                             const QString &localPath,
                                             const QString &mime,
                                             const QString &caption,
                                             int width, int height,
                                             qint64 durationMs,
                                             const QByteArray &thumbnail,
                                             int thumbnailWidth,
                                             int thumbnailHeight)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty()
        || rootEventId.isEmpty() || localPath.isEmpty() || mime.isEmpty())
        return 0;
    const bool hasPoster = !thumbnail.isEmpty() && thumbnailWidth > 0
        && thumbnailHeight > 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray root = rootEventId.toUtf8();
    const QByteArray path = localPath.toUtf8();
    const QByteArray mimeBytes = mime.toUtf8();
    const QByteArray captionBytes = caption.toUtf8();
    const QString result = takeRustString(mx_rust_thread_send_video(
        m_rustHandle, room.constData(), root.constData(), path.constData(),
        mimeBytes.constData(), captionBytes.constData(),
        static_cast<unsigned long long>(qMax(0, width)),
        static_cast<unsigned long long>(qMax(0, height)),
        static_cast<unsigned long long>(qMax<qint64>(0, durationMs)),
        hasPoster ? reinterpret_cast<const unsigned char *>(thumbnail.constData())
                  : nullptr,
        hasPoster ? static_cast<size_t>(thumbnail.size()) : 0,
        static_cast<unsigned long long>(hasPoster ? thumbnailWidth : 0),
        static_cast<unsigned long long>(hasPoster ? thumbnailHeight : 0),
        opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "thread video send rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::sendVoiceMessage(const QString &roomId,
                                              const QString &localPath,
                                              const QString &mime,
                                              qint64 durationMs,
                                              const QList<int> &waveform)
{
    if (!m_rustHandle || roomId.isEmpty() || localPath.isEmpty()
        || mime.isEmpty() || durationMs <= 0)
        return 0;
    if (!timelineActiveFor(roomId)) {
        qCWarning(lcRust) << "voice send requires the open room timeline";
        return 0;
    }
    // Clamp to the bridge scale; the FFI bounds the length.
    QByteArray amplitudes;
    amplitudes.reserve(waveform.size());
    for (int value : waveform)
        amplitudes.append(static_cast<char>(qBound(0, value, 100)));
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray path = localPath.toUtf8();
    const QByteArray mimeBytes = mime.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_send_voice(
        m_rustHandle, room.constData(), path.constData(),
        mimeBytes.constData(), static_cast<unsigned long long>(durationMs),
        reinterpret_cast<const unsigned char *>(amplitudes.constData()),
        static_cast<size_t>(amplitudes.size()), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "voice send rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::sendThreadVoiceMessage(
    const QString &roomId, const QString &rootEventId,
    const QString &localPath, const QString &mime, qint64 durationMs,
    const QList<int> &waveform)
{
    // A thread attachment with voice metadata, plus the MSC3245 duration. Not
    // gated on timelineActiveFor(): it sends through the thread-focused
    // timeline the panel already holds open.
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty()
        || rootEventId.isEmpty() || localPath.isEmpty() || mime.isEmpty()
        || durationMs <= 0)
        return 0;
    // Clamp to the bridge scale; the FFI bounds the length.
    QByteArray amplitudes;
    amplitudes.reserve(waveform.size());
    for (int value : waveform)
        amplitudes.append(static_cast<char>(qBound(0, value, 100)));
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray root = rootEventId.toUtf8();
    const QByteArray path = localPath.toUtf8();
    const QByteArray mimeBytes = mime.toUtf8();
    const QString result = takeRustString(mx_rust_thread_send_voice(
        m_rustHandle, room.constData(), root.constData(), path.constData(),
        mimeBytes.constData(), static_cast<unsigned long long>(durationMs),
        reinterpret_cast<const unsigned char *>(amplitudes.constData()),
        static_cast<size_t>(amplitudes.size()), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "thread voice send rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::sendAttachmentBytes(const QString &roomId,
                                                 const QByteArray &bytes,
                                                 const QString &filename,
                                                 const QString &mime,
                                                 int width, int height)
{
    if (!m_rustHandle || roomId.isEmpty() || bytes.isEmpty() || mime.isEmpty())
        return 0;
    if (!timelineActiveFor(roomId)) {
        qCWarning(lcRust) << "attachment send requires the open room timeline";
        return 0;
    }
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray name = filename.toUtf8();
    const QByteArray mimeBytes = mime.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_send_attachment_bytes(
        m_rustHandle, room.constData(),
        reinterpret_cast<const unsigned char *>(bytes.constData()),
        static_cast<size_t>(bytes.size()), name.constData(),
        mimeBytes.constData(),
        static_cast<unsigned long long>(qMax(0, width)),
        static_cast<unsigned long long>(qMax(0, height)), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "clipboard attachment send rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::sendAttachmentBytesToRoom(const QString &roomId,
                                                      const QByteArray &bytes,
                                                      const QString &filename,
                                                      const QString &mime,
                                                      int width, int height)
{
    if (!m_rustHandle || roomId.isEmpty() || bytes.isEmpty() || mime.isEmpty())
        return 0;
    // No timelineActiveFor() gate: this variant exists for rooms whose timeline
    // is not open. Room::send_attachment still encrypts for encrypted rooms.
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray name = filename.toUtf8();
    const QByteArray mimeBytes = mime.toUtf8();
    const QString result = takeRustString(mx_rust_room_send_attachment_bytes(
        m_rustHandle, room.constData(),
        reinterpret_cast<const quint8 *>(bytes.constData()),
        static_cast<size_t>(bytes.size()),
        name.constData(), mimeBytes.constData(),
        width > 0 ? static_cast<quint64>(width) : 0,
        height > 0 ? static_cast<quint64>(height) : 0,
        opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "room attachment send rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::sendThreadAttachment(const QString &roomId,
                                                  const QString &rootEventId,
                                                  const QString &localPath,
                                                  const QString &mime,
                                                  const QString &caption,
                                                  int width, int height,
                                                  bool animated,
                                                  qint64 durationMs)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty()
        || rootEventId.isEmpty() || localPath.isEmpty() || mime.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray root = rootEventId.toUtf8();
    const QByteArray path = localPath.toUtf8();
    const QByteArray mimeBytes = mime.toUtf8();
    const QByteArray captionBytes = caption.toUtf8();
    const QString result = takeRustString(mx_rust_thread_send_attachment(
        m_rustHandle, room.constData(), root.constData(), path.constData(),
        mimeBytes.constData(), captionBytes.constData(),
        static_cast<unsigned long long>(qMax(0, width)),
        static_cast<unsigned long long>(qMax(0, height)),
        animated ? 1 : 0,
        static_cast<unsigned long long>(qMax<qint64>(0, durationMs)), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "thread attachment send rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::sendThreadAttachmentBytes(const QString &roomId,
                                                       const QString &rootEventId,
                                                       const QByteArray &bytes,
                                                       const QString &filename,
                                                       const QString &mime,
                                                       int width, int height)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty()
        || rootEventId.isEmpty() || bytes.isEmpty() || mime.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray root = rootEventId.toUtf8();
    const QByteArray name = filename.toUtf8();
    const QByteArray mimeBytes = mime.toUtf8();
    const QString result = takeRustString(mx_rust_thread_send_attachment_bytes(
        m_rustHandle, room.constData(), root.constData(),
        reinterpret_cast<const unsigned char *>(bytes.constData()),
        static_cast<size_t>(bytes.size()), name.constData(),
        mimeBytes.constData(),
        static_cast<unsigned long long>(qMax(0, width)),
        static_cast<unsigned long long>(qMax(0, height)), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "thread clipboard attachment send rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::fetchMedia(const QString &mediaKey, int kind,
                                        int timeoutClass)
{
    if (!m_rustHandle || mediaKey.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray key = mediaKey.toUtf8();
    // Kinds: 0 full, 1 thumb, 2 list thumb. List thumbnails must not escalate
    // to full thumbnails, which for an encrypted file without one means
    // downloading the whole file.
    const QString result = takeRustString(mx_rust_media_fetch(
        m_rustHandle, key.constData(),
        static_cast<unsigned int>(qBound(0, kind, 2)), opId,
        static_cast<unsigned int>(qBound(0, timeoutClass, 2))));
    if (!result.isEmpty()) {
        // Log synchronous refusals so a stuck placeholder is explainable. The
        // reason is a constant bridge string; no key, path or body.
        qCWarning(lcRust) << "media fetch refused kind=" << kind
                                << "reason=" << result;
        return 0;
    }
    return opId;
}

void RustSdkMatrixClient::cancelMediaFetch(quint64 opId)
{
    if (!m_rustHandle || opId == 0)
        return;
    mx_rust_media_cancel(m_rustHandle, opId);
}

quint64 RustSdkMatrixClient::fetchMxcThumbnail(const QString &mxc,
                                               int width, int height)
{
    if (!m_rustHandle || !mxc.startsWith(QLatin1String("mxc://")))
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray uri = mxc.toUtf8();
    const QString result = takeRustString(mx_rust_media_fetch_mxc(
        m_rustHandle, uri.constData(),
        static_cast<unsigned long long>(qMax(0, width)),
        static_cast<unsigned long long>(qMax(0, height)), opId));
    return result.isEmpty() ? opId : 0;
}

void RustSdkMatrixClient::handleMediaReady(const QJsonObject &event)
{
    const quint64 opId =
        static_cast<quint64>(event.value(QStringLiteral("op_id")).toDouble());
    size_t len = 0;
    unsigned char *raw = mx_rust_media_take(m_rustHandle, opId, &len);
    if (!raw || len == 0) {
        // Stale or already-taken payload; treat as a failed fetch.
        Q_EMIT mediaFailed(opId, event.value(QStringLiteral("key")).toString(),
                           event.value(QStringLiteral("kind")).toInt(),
                           QStringLiteral("gone"));
        return;
    }
    // One bounded copy into Qt-owned memory, then release the Rust buffer.
    QByteArray bytes(reinterpret_cast<const char *>(raw),
                     static_cast<qsizetype>(len));
    mx_rust_media_free(raw, len);
    Q_EMIT mediaReady(opId, event.value(QStringLiteral("key")).toString(),
                      event.value(QStringLiteral("kind")).toInt(), bytes,
                      event.value(QStringLiteral("mimetype")).toString(),
                      event.value(QStringLiteral("filename")).toString());
}

namespace {
// Shared camelCase reshape for one discovery row (directory entry or Space
// child).
QVariantMap discoveryRoomRow(const QJsonObject &row)
{
    QVariantMap out;
    out.insert(QStringLiteral("roomId"),
               row.value(QStringLiteral("room_id")).toString());
    out.insert(QStringLiteral("name"),
               row.value(QStringLiteral("name")).toString());
    out.insert(QStringLiteral("alias"),
               row.value(QStringLiteral("alias")).toString());
    out.insert(QStringLiteral("topic"),
               row.value(QStringLiteral("topic")).toString());
    out.insert(QStringLiteral("avatarUrl"),
               row.value(QStringLiteral("avatar_url")).toString());
    out.insert(QStringLiteral("members"),
               static_cast<qlonglong>(
                   row.value(QStringLiteral("members")).toDouble()));
    out.insert(QStringLiteral("joinRule"),
               row.value(QStringLiteral("join_rule")).toString());
    out.insert(QStringLiteral("membership"),
               row.value(QStringLiteral("membership")).toString());
    out.insert(QStringLiteral("isSpace"),
               row.value(QStringLiteral("is_space")).toBool());
    return out;
}
} // namespace

bool RustSdkMatrixClient::handleRoomCommandEvent(const QString &type,
                                                 const QJsonObject &event)
{
    const auto opId = [&event]() {
        return static_cast<quint64>(
            event.value(QStringLiteral("op_id")).toDouble());
    };

    if (type == QLatin1String("call_candidates")) {
        // Media-capable mode only (gated in Rust and here): ICE for the engine.
        // Never logged or rendered.
        if (!m_callMediaCapable)
            return true;
        QVariantList candidates;
        const QJsonArray rows =
            event.value(QStringLiteral("candidates")).toArray();
        for (const QJsonValue &value : rows) {
            const QJsonObject row = value.toObject();
            QVariantMap entry;
            entry.insert(QStringLiteral("candidate"),
                         row.value(QStringLiteral("candidate")).toString());
            if (row.contains(QStringLiteral("sdp_mid")))
                entry.insert(QStringLiteral("sdpMid"),
                             row.value(QStringLiteral("sdp_mid")).toString());
            if (row.contains(QStringLiteral("sdp_m_line_index")))
                entry.insert(QStringLiteral("sdpMLineIndex"),
                             row.value(QStringLiteral("sdp_m_line_index"))
                                 .toInt());
            candidates.append(entry);
        }
        if (candidates.isEmpty())
            return true;
        Q_EMIT callCandidatesReceived(
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("call_id")).toString(),
            event.value(QStringLiteral("party_id")).toString(),
            event.value(QStringLiteral("own")).toBool(), candidates);
        return true;
    }
    // MatrixRTC (MSC4143) observation. Strings were bounded and sanitized in
    // rust/src/rtc.rs; ids and the transport URL are opaque (compared, never
    // logged or rendered raw).
    if (type == QLatin1String("rtc_session_changed")) {
        // Payload-free poke; the owner re-reads, so remote and local changes
        // share one parse path.
        Q_EMIT rtcSessionChanged(
            event.value(QStringLiteral("room_id")).toString());
        return true;
    }
    if (type == QLatin1String("rtc_session")) {
        RtcSessionData session;
        session.roomId = event.value(QStringLiteral("room_id")).toString();
        session.slotPresent =
            event.value(QStringLiteral("slot_present")).toBool();
        session.slotClosed =
            event.value(QStringLiteral("slot_closed")).toBool();
        session.source = event.value(QStringLiteral("source")).toString();
        session.rawMembershipEvents =
            event.value(QStringLiteral("raw_count")).toInt();
        const QJsonObject focus =
            event.value(QStringLiteral("focus")).toObject();
        session.focusServiceUrl =
            focus.value(QStringLiteral("livekit_service_url")).toString();
        const QString ownUser = currentUserId();
        const QString ownDevice = currentDeviceId();
        const QJsonArray members =
            event.value(QStringLiteral("members")).toArray();
        session.participants.reserve(members.size());
        for (const QJsonValue &value : members) {
            const QJsonObject row = value.toObject();
            RtcParticipant participant;
            participant.userId =
                row.value(QStringLiteral("user_id")).toString();
            participant.deviceId =
                row.value(QStringLiteral("device_id")).toString();
            participant.rtcIdentity =
                row.value(QStringLiteral("rtc_identity")).toString();
            participant.intent = row.value(QStringLiteral("intent")).toString();
            participant.displayName =
                row.value(QStringLiteral("display_name")).toString();
            participant.avatarMxc =
                row.value(QStringLiteral("avatar_mxc")).toString();
            participant.joinedAtMs = static_cast<qint64>(
                row.value(QStringLiteral("created_ts")).toDouble());
            participant.expiresAtMs = static_cast<qint64>(
                row.value(QStringLiteral("expires_at_ms")).toDouble());
            participant.wireFormat = row.value(QStringLiteral("kind")).toString();
            participant.membershipEventId =
                row.value(QStringLiteral("event_id")).toString();
            participant.ownUser =
                !ownUser.isEmpty() && participant.userId == ownUser;
            // Own device, not own user: the same account on another device is a
            // genuine second participant.
            participant.ownDevice = participant.ownUser
                && !ownDevice.isEmpty() && participant.deviceId == ownDevice;
            session.participants.append(participant);
        }
        session.observedAtMs = QDateTime::currentMSecsSinceEpoch();
        Q_EMIT rtcSessionReceived(opId(), session);
        return true;
    }
    if (type == QLatin1String("rtc_transports")) {
        QStringList urls;
        const QJsonArray rows =
            event.value(QStringLiteral("server_transports")).toArray();
        for (const QJsonValue &value : rows) {
            const QString url = value.toObject()
                                    .value(QStringLiteral("livekit_service_url"))
                                    .toString();
            if (!url.isEmpty())
                urls.append(url);
        }
        Q_EMIT rtcTransportsReceived(
            opId(), event.value(QStringLiteral("server_answered")).toBool(),
            event.value(QStringLiteral("category")).toString(), urls,
            event.value(QStringLiteral("participant_focus"))
                .toObject()
                .value(QStringLiteral("livekit_service_url"))
                .toString());
        return true;
    }
    if (type == QLatin1String("rtc_membership_published")) {
        Q_EMIT rtcMembershipPublished(
            opId(), event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("category")).toString(),
            event.value(QStringLiteral("event_id")).toString(),
            event.value(QStringLiteral("delay_id")).toString(),
            event.value(QStringLiteral("delayed_category")).toString());
        return true;
    }
    if (type == QLatin1String("rtc_membership_retracted")
        || type == QLatin1String("rtc_delayed_updated")) {
        Q_EMIT rtcMembershipRetracted(
            opId(), event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }
    if (type == QLatin1String("rtc_key_sent")) {
        Q_EMIT rtcMediaKeySent(
            opId(), event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("category")).toString(),
            event.value(QStringLiteral("delivered")).toInt(),
            event.value(QStringLiteral("key_index")).toInt());
        return true;
    }
    if (type == QLatin1String("rtc_key_received")) {
        // SENSITIVE: raw media key material. Goes to the frame cryptor only;
        // never logged, never QML.
        Q_EMIT rtcMediaKeyReceived(
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("sender")).toString(),
            event.value(QStringLiteral("claimed_device_id")).toString(),
            event.value(QStringLiteral("key_index")).toInt(),
            event.value(QStringLiteral("key")).toString());
        return true;
    }
    if (type == QLatin1String("rtc_key_discarded")) {
        // Why a media key was discarded; otherwise the receiver hears nothing
        // and the sender sees a successful send. A cooldown rather than
        // once-per-session, so a second call's fault is still reported without
        // spamming on a wedged Olm session.
        static QHash<QString, qint64> lastSaid;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        constexpr qint64 kCooldownMs = 60000;
        const QString sender =
            event.value(QStringLiteral("sender")).toString();
        const QString reason =
            event.value(QStringLiteral("reason")).toString();
        const QString subject = sender + QChar(0x1f) + reason;
        const auto said = lastSaid.constFind(subject);
        const bool due = said == lastSaid.cend()
            || now - *said >= kCooldownMs;
        if (due && (lastSaid.size() < 256 || said != lastSaid.cend())) {
            lastSaid.insert(subject, now);
            qCWarning(lcRust)
                << "call diagnosis: a media key from" << sender
                << "was DISCARDED before it could be installed —" << reason
                << ". Their audio and video will be dropped for want of a "
                   "key until this is resolved.";
        }
        return true;
    }
    if (type == QLatin1String("sfu_state")) {
        // Log LiveKit's DisconnectReason for a server-initiated leave.
        if (event.contains(QStringLiteral("reason"))) {
            qCInfo(lcRust) << "sfu leave reason="
                           << event.value(QStringLiteral("reason")).toInt()
                           << "action="
                           << event.value(QStringLiteral("action")).toInt();
        }
        Q_EMIT sfuStateChanged(
            event.value(QStringLiteral("state")).toString(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }
    if (type == QLatin1String("sfu_joined")) {
        // Server-injected-frame trailer, already capped in the bridge. Decoded
        // strictly and re-bounded: a malformed value must disarm (empty), never
        // arm a truncated trailer. Not secret, but not logged either.
        QByteArray sifTrailer;
        const QString sifB64 = event.value(QStringLiteral("sif_trailer")).toString();
        if (!sifB64.isEmpty() && sifB64.size() <= 128) {
            const auto decoded = QByteArray::fromBase64Encoding(
                sifB64.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
            if (decoded && decoded.decoded.size() <= 64)
                sifTrailer = decoded.decoded;
        }
        Q_EMIT sfuJoined(
            event.value(QStringLiteral("identity")).toString(),
            event.value(QStringLiteral("participants")).toArray().toVariantList(),
            event.value(QStringLiteral("ice_servers")).toArray().toVariantList(),
            sifTrailer);
        return true;
    }
    if (type == QLatin1String("sfu_participants")) {
        Q_EMIT sfuParticipantsChanged(
            event.value(QStringLiteral("participants")).toArray().toVariantList());
        return true;
    }
    if (type == QLatin1String("sfu_track_published")) {
        Q_EMIT sfuTrackPublished(
            event.value(QStringLiteral("cid")).toString(),
            event.value(QStringLiteral("sid")).toString());
        return true;
    }
    if (type == QLatin1String("sfu_speakers")) {
        Q_EMIT sfuSpeakersChanged(
            event.value(QStringLiteral("speakers")).toArray().toVariantList());
        return true;
    }
    if (type == QLatin1String("sfu_quality")) {
        Q_EMIT sfuConnectionQuality(
            event.value(QStringLiteral("updates")).toArray().toVariantList());
        return true;
    }
    if (type == QLatin1String("sfu_remote_description")) {
        // Media transport only; gated in Rust and here so no SDP crosses
        // without an engine.
        if (!m_callMediaCapable)
            return true;
        Q_EMIT sfuRemoteDescription(
            event.value(QStringLiteral("kind")).toString(),
            event.value(QStringLiteral("target")).toString(),
            event.value(QStringLiteral("sdp")).toString());
        return true;
    }
    if (type == QLatin1String("sfu_remote_candidate")) {
        if (!m_callMediaCapable)
            return true;
        Q_EMIT sfuRemoteCandidate(
            event.value(QStringLiteral("target")).toString(),
            event.value(QStringLiteral("candidate_init")).toString());
        return true;
    }
    if (type == QLatin1String("sfu_server_mute")) {
        return true; // observed; the engine's own valve is authoritative
    }
    if (type == QLatin1String("rtc_send_result")) {
        Q_EMIT rtcSendFinished(
            opId(), event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("category")).toString(),
            event.value(QStringLiteral("event_id")).toString());
        return true;
    }
    if (type == QLatin1String("rtc_hand_result")) {
        Q_EMIT rtcHandResult(
            opId(), event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("raised")).toBool(),
            event.value(QStringLiteral("category")).toString(),
            event.value(QStringLiteral("event_id")).toString());
        return true;
    }
    if (type == QLatin1String("rtc_hand_changed")) {
        // Bounded in rust/src/rtc.rs. Ids are opaque: compared, never rendered
        // or logged.
        Q_EMIT rtcHandChanged(
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("sender")).toString(),
            event.value(QStringLiteral("membership_event_id")).toString(),
            event.value(QStringLiteral("reaction_event_id")).toString(),
            event.value(QStringLiteral("raised")).toBool());
        return true;
    }
    if (type == QLatin1String("rtc_call_reaction")) {
        // element-call's transient reaction. Bounded in Rust and reduced to a
        // single cluster; rendered as plain text and attributed through the
        // referenced membership, never through the sender alone.
        Q_EMIT rtcCallReactionReceived(
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("sender")).toString(),
            event.value(QStringLiteral("membership_event_id")).toString(),
            event.value(QStringLiteral("emoji")).toString());
        return true;
    }
    if (type == QLatin1String("rtc_hands")) {
        QVariantList hands;
        const QJsonArray rows = event.value(QStringLiteral("hands")).toArray();
        for (const QJsonValue &value : rows) {
            const QJsonObject hand = value.toObject();
            hands.append(QVariantMap{
                { QStringLiteral("userId"),
                  hand.value(QStringLiteral("user_id")).toString() },
                { QStringLiteral("deviceId"),
                  hand.value(QStringLiteral("device_id")).toString() },
                { QStringLiteral("rtcIdentity"),
                  hand.value(QStringLiteral("rtc_identity")).toString() },
                { QStringLiteral("membershipEventId"),
                  hand.value(QStringLiteral("membership_event_id")).toString() },
                { QStringLiteral("reactionEventId"),
                  hand.value(QStringLiteral("reaction_event_id")).toString() },
            });
        }
        Q_EMIT rtcHandsReceived(
            opId(), event.value(QStringLiteral("room_id")).toString(), hands);
        return true;
    }
    if (type == QLatin1String("call_turn_servers")) {
        // SENSITIVE: username/password are live TURN credentials. Never log
        // `event` or these fields.
        QStringList uris;
        const QJsonArray rows = event.value(QStringLiteral("uris")).toArray();
        for (const QJsonValue &value : rows)
            uris.append(value.toString());
        Q_EMIT callTurnServersReceived(
            opId(), event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("username")).toString(),
            event.value(QStringLiteral("password")).toString(), uris,
            // Clamp before narrowing: an out-of-range double->int64 cast is UB.
            // Rust bounds it too.
            static_cast<qint64>(qBound(
                0.0, event.value(QStringLiteral("ttl_seconds")).toDouble(),
                86400.0)),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    // Voice-call signaling. Fields are public Matrix ids, closed-set strings
    // sanitized in Rust, booleans, or sender-chosen opaque call/party ids
    // (bounded, never logged or rendered). Never an SDP (see CallSignal.h).
    if (type == QLatin1String("call_send_result")) {
        Q_EMIT callSendFinished(
            opId(), event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("category")).toString(),
            event.value(QStringLiteral("call_id")).toString(),
            event.value(QStringLiteral("event_id")).toString());
        return true;
    }
    if (type == QLatin1String("call_invite")
        || type == QLatin1String("call_answer")
        || type == QLatin1String("call_hangup")
        || type == QLatin1String("call_reject")
        || type == QLatin1String("call_select_answer")
        || type == QLatin1String("call_rtc_notification")
        || type == QLatin1String("call_rtc_decline")) {
        CallSignal signal;
        if (type == QLatin1String("call_invite"))
            signal.kind = CallSignal::Kind::Invite;
        else if (type == QLatin1String("call_answer"))
            signal.kind = CallSignal::Kind::Answer;
        else if (type == QLatin1String("call_hangup"))
            signal.kind = CallSignal::Kind::Hangup;
        else if (type == QLatin1String("call_reject"))
            signal.kind = CallSignal::Kind::Reject;
        else if (type == QLatin1String("call_select_answer"))
            signal.kind = CallSignal::Kind::SelectAnswer;
        else if (type == QLatin1String("call_rtc_notification"))
            signal.kind = CallSignal::Kind::RtcNotification;
        else
            signal.kind = CallSignal::Kind::RtcDecline;
        signal.roomId = event.value(QStringLiteral("room_id")).toString();
        signal.eventId = event.value(QStringLiteral("event_id")).toString();
        signal.sender = event.value(QStringLiteral("sender")).toString();
        signal.own = event.value(QStringLiteral("own")).toBool();
        signal.callId = event.value(QStringLiteral("call_id")).toString();
        signal.partyId = event.value(QStringLiteral("party_id")).toString();
        signal.invitee = event.value(QStringLiteral("invitee")).toString();
        signal.lifetimeMs = static_cast<qint64>(
            event.value(QStringLiteral("lifetime_ms")).toDouble());
        signal.originServerTs = static_cast<qint64>(
            event.value(QStringLiteral("origin_server_ts")).toDouble());
        signal.senderTs = static_cast<qint64>(
            event.value(QStringLiteral("sender_ts")).toDouble());
        signal.version = event.value(QStringLiteral("version")).toString();
        if (signal.kind == CallSignal::Kind::Invite) {
            signal.sessionType =
                event.value(QStringLiteral("offer_type")).toString();
            signal.hasDescription =
                event.value(QStringLiteral("has_offer")).toBool();
        } else if (signal.kind == CallSignal::Kind::Answer) {
            signal.sessionType =
                event.value(QStringLiteral("answer_type")).toString();
            signal.hasDescription =
                event.value(QStringLiteral("has_answer")).toBool();
        }
        signal.reason = event.value(QStringLiteral("reason")).toString();
        signal.selectedPartyId =
            event.value(QStringLiteral("selected_party_id")).toString();
        signal.callIntent =
            event.value(QStringLiteral("call_intent")).toString();
        signal.targetEventId =
            event.value(QStringLiteral("target_event_id")).toString();
        if (signal.roomId.isEmpty() || signal.eventId.isEmpty())
            return true; // malformed: drop, never dispatch a partial signal
        // Media-capable mode only: the remote SDP goes into the bounded
        // single-shot store, never onto the signal (CallSignal is SDP-free) and
        // never into a log.
        if (m_callMediaCapable) {
            if (signal.kind == CallSignal::Kind::Invite) {
                m_callSdpStore.insert(
                    signal.eventId,
                    event.value(QStringLiteral("offer_sdp")).toString());
            } else if (signal.kind == CallSignal::Kind::Answer) {
                m_callSdpStore.insert(
                    signal.eventId,
                    event.value(QStringLiteral("answer_sdp")).toString());
            }
        }
        Q_EMIT callSignalReceived(signal);
        return true;
    }

    if (type == QLatin1String("user_search_result")) {
        QVariantList results;
        const QJsonArray rows = event.value(QStringLiteral("results")).toArray();
        for (const QJsonValue &value : rows) {
            const QJsonObject row = value.toObject();
            QVariantMap entry;
            entry.insert(QStringLiteral("userId"),
                         row.value(QStringLiteral("user_id")).toString());
            entry.insert(QStringLiteral("displayName"),
                         row.value(QStringLiteral("display_name")).toString());
            entry.insert(QStringLiteral("avatarUrl"),
                         row.value(QStringLiteral("avatar_url")).toString());
            results.append(entry);
        }
        Q_EMIT userSearchFinished(opId(),
                                  event.value(QStringLiteral("ok")).toBool(),
                                  results,
                                  event.value(QStringLiteral("limited")).toBool(),
                                  event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("user_profile_result")) {
        Q_EMIT userProfileFinished(
            opId(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("user_id")).toString(),
            event.value(QStringLiteral("display_name")).toString(),
            event.value(QStringLiteral("avatar_url")).toString(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("url_preview_result")) {
        QVariantMap fields;
        const QJsonObject raw = event.value(QStringLiteral("fields")).toObject();
        fields.insert(QStringLiteral("title"),
                      raw.value(QStringLiteral("title")).toString());
        fields.insert(QStringLiteral("description"),
                      raw.value(QStringLiteral("description")).toString());
        fields.insert(QStringLiteral("siteName"),
                      raw.value(QStringLiteral("site_name")).toString());
        fields.insert(QStringLiteral("previewKind"),
                      raw.value(QStringLiteral("preview_kind")).toString());
        // Which route produced this card: "server" or "client". Fields are
        // mapped explicitly, so omitting this one would leave the UI unable to
        // tell whether the member's IP reached the linked site.
        fields.insert(QStringLiteral("previewRoute"),
                      raw.value(QStringLiteral("preview_route")).toString());
        fields.insert(QStringLiteral("imageMxc"),
                      raw.value(QStringLiteral("image_mxc")).toString());
        fields.insert(QStringLiteral("imageSource"),
                      raw.value(QStringLiteral("image_source")).toString());
        fields.insert(QStringLiteral("imageMime"),
                      raw.value(QStringLiteral("image_mime")).toString());
        fields.insert(QStringLiteral("imageWidth"),
                      raw.value(QStringLiteral("image_width")).toInt());
        fields.insert(QStringLiteral("imageHeight"),
                      raw.value(QStringLiteral("image_height")).toInt());
        fields.insert(QStringLiteral("imageSize"),
                      static_cast<qint64>(
                          raw.value(QStringLiteral("image_size")).toDouble()));
        Q_EMIT urlPreviewFinished(
            opId(), event.value(QStringLiteral("ok")).toBool(), fields,
            event.value(QStringLiteral("category")).toString(),
            event.value(QStringLiteral("status")).toInt(),
            event.value(QStringLiteral("redirects")).toInt());
        return true;
    }

    if (type == QLatin1String("gif_response")) {
        // Provider data (no key, no Matrix ids); the GIF controller parses it
        // into safe structs. Never logged.
        Q_EMIT gifResponse(
            opId(), event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("status")).toInt(),
            event.value(QStringLiteral("body")).toString().toUtf8(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("gif_download_result")) {
        const quint64 op = opId();
        const bool ok = event.value(QStringLiteral("ok")).toBool(false);
        if (!ok) {
            Q_EMIT gifDownloadFinished(
                op, false, QByteArray(), QString(), 0, 0, 0,
                event.value(QStringLiteral("category")).toString());
            return true;
        }
        // One bounded copy into Qt-owned memory, then release the Rust buffer,
        // as in media_ready.
        size_t len = 0;
        unsigned char *raw = mx_rust_media_take(m_rustHandle, op, &len);
        if (!raw || len == 0) {
            Q_EMIT gifDownloadFinished(op, false, QByteArray(), QString(), 0, 0,
                                       0, QStringLiteral("gone"));
            return true;
        }
        QByteArray bytes(reinterpret_cast<const char *>(raw),
                         static_cast<qsizetype>(len));
        mx_rust_media_free(raw, len);
        Q_EMIT gifDownloadFinished(
            op, true, bytes, event.value(QStringLiteral("mime")).toString(),
            event.value(QStringLiteral("width")).toInt(),
            event.value(QStringLiteral("height")).toInt(),
            static_cast<qint64>(event.value(QStringLiteral("size")).toDouble()),
            QString());
        return true;
    }

    if (type == QLatin1String("dm_create_result")) {
        Q_EMIT dmCreateFinished(opId(),
                                event.value(QStringLiteral("ok")).toBool(),
                                event.value(QStringLiteral("room_id")).toString(),
                                event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("room_create_result")) {
        Q_EMIT roomCreateFinished(opId(),
                                  event.value(QStringLiteral("ok")).toBool(),
                                  event.value(QStringLiteral("room_id")).toString(),
                                  event.value(QStringLiteral("category")).toString(),
                                  event.value(QStringLiteral("warning")).toString());
        return true;
    }

    if (type == QLatin1String("room_invite_result")) {
        Q_EMIT inviteUserFinished(opId(),
                                  event.value(QStringLiteral("room_id")).toString(),
                                  event.value(QStringLiteral("user_id")).toString(),
                                  event.value(QStringLiteral("ok")).toBool(),
                                  event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("room_invite_done")) {
        Q_EMIT inviteBatchFinished(opId(),
                                   event.value(QStringLiteral("room_id")).toString(),
                                   event.value(QStringLiteral("ok_count")).toInt(),
                                   event.value(QStringLiteral("fail_count")).toInt());
        return true;
    }

    if (type == QLatin1String("message_edits_removed")) {
        Q_EMIT messageEditsRemoved(
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("event_id")).toString(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("removed")).toInt(),
            event.value(QStringLiteral("failed")).toInt(),
            event.value(QStringLiteral("truncated")).toBool());
        return true;
    }

    if (type == QLatin1String("thread_participants")) {
        // A failed lookup is forwarded as ok=false so the card keeps what it
        // had rather than showing "nobody is in this thread".
        if (!event.value(QStringLiteral("ok")).toBool()) {
            Q_EMIT threadParticipantsReceived(
                event.value(QStringLiteral("room_id")).toString(),
                event.value(QStringLiteral("root_event_id")).toString(),
                {}, 0, false);
            return true;
        }
        QVariantList participants;
        const QJsonArray rows =
            event.value(QStringLiteral("participants")).toArray();
        for (const QJsonValue &row : rows) {
            const QJsonObject obj = row.toObject();
            participants.append(QVariantMap{
                { QStringLiteral("userId"),
                  obj.value(QStringLiteral("user_id")).toString() },
                { QStringLiteral("displayName"),
                  obj.value(QStringLiteral("display_name")).toString() },
                { QStringLiteral("avatarUrl"),
                  obj.value(QStringLiteral("avatar_url")).toString() },
            });
        }
        Q_EMIT threadParticipantsReceived(
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("root_event_id")).toString(),
            participants,
            event.value(QStringLiteral("distinct")).toInt(),
            event.value(QStringLiteral("truncated")).toBool());
        return true;
    }
    if (type == QLatin1String("name_color")) {
        Q_EMIT nameColorReceived(
            static_cast<quint64>(
                event.value(QStringLiteral("op_id")).toDouble(0)),
            event.value(QStringLiteral("user_id")).toString(),
            event.value(QStringLiteral("color")).toString(),
            event.value(QStringLiteral("supported")).toBool(false));
        return true;
    }
    if (type == QLatin1String("name_color_set")) {
        Q_EMIT nameColorSet(
            static_cast<quint64>(
                event.value(QStringLiteral("op_id")).toDouble(0)),
            event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("color")).toString(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }
    if (type == QLatin1String("profile_banner")) {
        Q_EMIT profileBannerReceived(
            static_cast<quint64>(
                event.value(QStringLiteral("op_id")).toDouble(0)),
            event.value(QStringLiteral("user_id")).toString(),
            event.value(QStringLiteral("mxc")).toString(),
            event.value(QStringLiteral("supported")).toBool(false));
        return true;
    }
    if (type == QLatin1String("profile_banner_set")) {
        Q_EMIT profileBannerSet(
            static_cast<quint64>(
                event.value(QStringLiteral("op_id")).toDouble(0)),
            event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("mxc")).toString(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }
    if (type == QLatin1String("profile_bio")) {
        Q_EMIT profileBioReceived(
            static_cast<quint64>(
                event.value(QStringLiteral("op_id")).toDouble(0)),
            event.value(QStringLiteral("user_id")).toString(),
            event.value(QStringLiteral("bio")).toString(),
            event.value(QStringLiteral("supported")).toBool(false));
        return true;
    }
    if (type == QLatin1String("profile_bio_set")) {
        Q_EMIT profileBioSet(
            static_cast<quint64>(
                event.value(QStringLiteral("op_id")).toDouble(0)),
            event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("bio")).toString(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }
    if (type == QLatin1String("room_banner")) {
        Q_EMIT roomBannerReceived(
            static_cast<quint64>(
                event.value(QStringLiteral("op_id")).toDouble(0)),
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("mxc")).toString(),
            event.value(QStringLiteral("can_set")).toBool(false));
        return true;
    }
    if (type == QLatin1String("room_banner_set")) {
        Q_EMIT roomBannerSet(
            static_cast<quint64>(
                event.value(QStringLiteral("op_id")).toDouble(0)),
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("mxc")).toString(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }
    if (type == QLatin1String("sticker_packs")) {
        // Validated and bounded in rust/src/stickers.rs (mxc-only urls,
        // allowlisted mimetype, control characters stripped, caps). A plain
        // transcription, so one place decides what a pack may contain.
        QVariantList packs;
        const QJsonArray rawPacks =
            event.value(QStringLiteral("packs")).toArray();
        packs.reserve(rawPacks.size());
        for (const QJsonValue &packValue : rawPacks) {
            const QJsonObject pack = packValue.toObject();
            QVariantMap entry;
            entry.insert(QStringLiteral("id"),
                         pack.value(QStringLiteral("id")).toString());
            entry.insert(QStringLiteral("displayName"),
                         pack.value(QStringLiteral("display_name")).toString());
            entry.insert(QStringLiteral("avatarUrl"),
                         pack.value(QStringLiteral("avatar_url")).toString());
            entry.insert(QStringLiteral("attribution"),
                         pack.value(QStringLiteral("attribution")).toString());
            entry.insert(QStringLiteral("source"),
                         pack.value(QStringLiteral("source")).toString());
            entry.insert(QStringLiteral("roomId"),
                         pack.value(QStringLiteral("room_id")).toString());
            entry.insert(QStringLiteral("stateKey"),
                         pack.value(QStringLiteral("state_key")).toString());
            entry.insert(
                QStringLiteral("enabledGlobally"),
                pack.value(QStringLiteral("enabled_globally")).toBool());
            entry.insert(QStringLiteral("canManage"),
                         pack.value(QStringLiteral("can_manage")).toBool());
            QVariantList images;
            const QJsonArray rawImages =
                pack.value(QStringLiteral("images")).toArray();
            images.reserve(rawImages.size());
            for (const QJsonValue &imageValue : rawImages) {
                const QJsonObject image = imageValue.toObject();
                QVariantMap row;
                row.insert(QStringLiteral("shortcode"),
                           image.value(QStringLiteral("shortcode")).toString());
                row.insert(QStringLiteral("url"),
                           image.value(QStringLiteral("url")).toString());
                row.insert(QStringLiteral("body"),
                           image.value(QStringLiteral("body")).toString());
                row.insert(QStringLiteral("mimetype"),
                           image.value(QStringLiteral("mimetype")).toString());
                row.insert(QStringLiteral("width"),
                           static_cast<int>(
                               image.value(QStringLiteral("width")).toDouble(0)));
                row.insert(QStringLiteral("height"),
                           static_cast<int>(
                               image.value(QStringLiteral("height")).toDouble(0)));
                row.insert(QStringLiteral("size"),
                           static_cast<qlonglong>(
                               image.value(QStringLiteral("size")).toDouble(0)));
                row.insert(QStringLiteral("isEmoticon"),
                           image.value(QStringLiteral("is_emoticon")).toBool());
                row.insert(QStringLiteral("isSticker"),
                           image.value(QStringLiteral("is_sticker")).toBool());
                images.append(row);
            }
            entry.insert(QStringLiteral("images"), images);
            packs.append(entry);
        }
        Q_EMIT stickerPacksReceived(
            static_cast<quint64>(
                event.value(QStringLiteral("op_id")).toDouble(0)),
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("room_can_manage")).toBool(false),
            packs);
        return true;
    }
    if (type == QLatin1String("sticker_pack_rooms_set")) {
        Q_EMIT stickerPackRoomsSet(
            static_cast<quint64>(
                event.value(QStringLiteral("op_id")).toDouble(0)),
            event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("category")).toString(),
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("state_key")).toString(),
            event.value(QStringLiteral("enabled")).toBool(false));
        return true;
    }
    if (type == QLatin1String("policy_rules")) {
        QVariantList rules;
        const QJsonArray rows = event.value(QStringLiteral("rules")).toArray();
        for (const QJsonValue &row : rows)
            rules.append(row.toObject().toVariantMap());
        Q_EMIT policyRulesReceived(
            static_cast<quint64>(
                event.value(QStringLiteral("op_id")).toDouble(0)),
            event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("can_write")).toBool(false),
            event.value(QStringLiteral("truncated")).toBool(false), rules);
        return true;
    }
    if (type == QLatin1String("policy_rule_written")) {
        Q_EMIT policyRuleWritten(
            static_cast<quint64>(
                event.value(QStringLiteral("op_id")).toDouble(0)),
            event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("category")).toString());
        return true;
    }
    if (type == QLatin1String("policy_subscriptions")) {
        QStringList rooms;
        const QJsonArray raw = event.value(QStringLiteral("rooms")).toArray();
        for (const QJsonValue &room : raw)
            rooms.append(room.toString());
        Q_EMIT policySubscriptionsReceived(
            static_cast<quint64>(
                event.value(QStringLiteral("op_id")).toDouble(0)),
            event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("category")).toString(), rooms);
        return true;
    }
    if (type == QLatin1String("policy_check")) {
        QVariantMap detail;
        // Only the keys a match carries, so no stale reason is readable.
        if (event.value(QStringLiteral("matched")).toBool(false)) {
            detail.insert(QStringLiteral("roomId"),
                          event.value(QStringLiteral("room_id")).toString());
            detail.insert(
                QStringLiteral("ruleEntity"),
                event.value(QStringLiteral("rule_entity")).toString());
            detail.insert(QStringLiteral("ruleKind"),
                          event.value(QStringLiteral("rule_kind")).toString());
            detail.insert(QStringLiteral("reason"),
                          event.value(QStringLiteral("reason")).toString());
        }
        Q_EMIT policyCheckFinished(
            static_cast<quint64>(
                event.value(QStringLiteral("op_id")).toDouble(0)),
            event.value(QStringLiteral("entity")).toString(),
            event.value(QStringLiteral("matched")).toBool(false), detail);
        return true;
    }
    if (type == QLatin1String("qr_login_progress")) {
        const QString step = event.value(QStringLiteral("step")).toString();
        QVariantMap detail;
        // Only the keys this step carries, so no stale value from a previous
        // step is readable.
        if (event.contains(QStringLiteral("qr_size"))) {
            detail.insert(QStringLiteral("qrSize"),
                          event.value(QStringLiteral("qr_size")).toInt());
            detail.insert(QStringLiteral("qrBits"),
                          event.value(QStringLiteral("qr_bits")).toString());
            detail.insert(QStringLiteral("qrText"),
                          event.value(QStringLiteral("qr_text")).toString());
        }
        if (event.contains(QStringLiteral("check_code"))) {
            detail.insert(QStringLiteral("checkCode"),
                          event.value(QStringLiteral("check_code")).toInt());
        }
        if (event.contains(QStringLiteral("verification_uri"))) {
            detail.insert(
                QStringLiteral("verificationUri"),
                event.value(QStringLiteral("verification_uri")).toString());
        }
        if (event.contains(QStringLiteral("category"))) {
            detail.insert(QStringLiteral("category"),
                          event.value(QStringLiteral("category")).toString());
        }
        Q_EMIT qrLoginProgress(
            static_cast<quint64>(
                event.value(QStringLiteral("generation")).toDouble(0)),
            step, detail);
        return true;
    }
    if (type == QLatin1String("sticker_pack_edit_result")) {
        Q_EMIT stickerPackEditFinished(
            static_cast<quint64>(
                event.value(QStringLiteral("op_id")).toDouble(0)),
            event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("category")).toString(),
            event.value(QStringLiteral("shortcode")).toString());
        return true;
    }
    if (type == QLatin1String("sticker_pack_add_result")) {
        Q_EMIT stickerPackAddFinished(
            static_cast<quint64>(
                event.value(QStringLiteral("op_id")).toDouble(0)),
            event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("category")).toString(),
            event.value(QStringLiteral("shortcode")).toString());
        return true;
    }
    if (type == QLatin1String("presence_batch")) {
        // Presentation-safe rows. ok=false means unknown for that user;
        // PresenceManager decides, and nothing here fabricates "offline".
        QVariantList entries;
        const QJsonArray rows = event.value(QStringLiteral("entries")).toArray();
        for (const QJsonValue &row : rows) {
            const QJsonObject obj = row.toObject();
            QVariantMap entry{
                { QStringLiteral("userId"),
                  obj.value(QStringLiteral("user_id")).toString() },
                { QStringLiteral("ok"),
                  obj.value(QStringLiteral("ok")).toBool(false) },
            };
            if (entry.value(QStringLiteral("ok")).toBool()) {
                entry.insert(QStringLiteral("state"),
                             obj.value(QStringLiteral("state")).toString());
                entry.insert(QStringLiteral("currentlyActive"),
                             obj.value(QStringLiteral("currently_active"))
                                 .toBool(false));
                // Absent or null last_active_ago_ms is -1 ("server sent none").
                // toDouble() turns null into 0, which would render as "active
                // just now".
                entry.insert(
                    QStringLiteral("lastActiveAgoMs"),
                    static_cast<qlonglong>(
                        obj.value(QStringLiteral("last_active_ago_ms"))
                            .toDouble(-1.0)));
                // The peer's status text, bounded and control-stripped in Rust.
                entry.insert(QStringLiteral("statusMsg"),
                             obj.value(QStringLiteral("status_msg")).toString());
            } else {
                entry.insert(QStringLiteral("category"),
                             obj.value(QStringLiteral("category")).toString());
            }
            entries.append(entry);
        }
        Q_EMIT presenceReceived(opId(), entries);
        return true;
    }

    if (type == QLatin1String("presence_publish_failed")) {
        Q_EMIT presencePublishFailed(
            event.value(QStringLiteral("category")).toString(),
            // Present only for rate_limited, and only when the server said; a
            // null value reads as 0, which the receiver treats as "nothing
            // said".
            event.value(QStringLiteral("retry_after_ms")).toVariant()
                .toLongLong());
        return true;
    }

    if (type == QLatin1String("room_members")) {
        QVariantMap snapshot;
        snapshot.insert(QStringLiteral("ok"),
                        event.value(QStringLiteral("ok")).toBool());
        snapshot.insert(QStringLiteral("truncated"),
                        event.value(QStringLiteral("truncated")).toBool());
        snapshot.insert(QStringLiteral("joinedCount"),
                        event.value(QStringLiteral("joined_count")).toInt());
        snapshot.insert(QStringLiteral("invitedCount"),
                        event.value(QStringLiteral("invited_count")).toInt());
        snapshot.insert(QStringLiteral("canInvite"),
                        event.value(QStringLiteral("own_can_invite")).toBool());
        snapshot.insert(QStringLiteral("canEditName"),
                        event.value(QStringLiteral("own_can_edit_name")).toBool());
        snapshot.insert(QStringLiteral("canEditTopic"),
                        event.value(QStringLiteral("own_can_edit_topic")).toBool());
        snapshot.insert(QStringLiteral("canEditAvatar"),
                        event.value(QStringLiteral("own_can_edit_avatar")).toBool());
        snapshot.insert(QStringLiteral("canKick"),
                        event.value(QStringLiteral("own_can_kick")).toBool());
        snapshot.insert(QStringLiteral("canBan"),
                        event.value(QStringLiteral("own_can_ban")).toBool());
        snapshot.insert(
            QStringLiteral("canNotifyRoom"),
            event.value(QStringLiteral("own_can_notify_room")).toBool());
        snapshot.insert(QStringLiteral("canUnban"),
                        event.value(QStringLiteral("own_can_unban")).toBool());
        snapshot.insert(
            QStringLiteral("ownPowerLevel"),
            static_cast<qlonglong>(
                event.value(QStringLiteral("own_power_level")).toDouble()));
        // Room administration: remaining SDK-derived permissions plus the room
        // state the admin surface renders.
        snapshot.insert(
            QStringLiteral("canChangePowerLevels"),
            event.value(QStringLiteral("own_can_change_power_levels")).toBool());
        snapshot.insert(QStringLiteral("canPinMessages"),
                        event.value(QStringLiteral("own_can_pin")).toBool());
        snapshot.insert(
            QStringLiteral("canChangeJoinRule"),
            event.value(QStringLiteral("own_can_change_join_rule")).toBool());
        snapshot.insert(
            QStringLiteral("canChangeAlias"),
            event.value(QStringLiteral("own_can_change_alias")).toBool());
        snapshot.insert(
            QStringLiteral("canManageSpaceChildren"),
            event.value(QStringLiteral("own_can_manage_space_children"))
                .toBool());
        snapshot.insert(
            QStringLiteral("usersDefaultPowerLevel"),
            static_cast<qlonglong>(
                event.value(QStringLiteral("users_default_power_level"))
                    .toDouble()));
        // The room's real m.room.power_levels thresholds, copied from a fixed
        // key set chosen in Rust; the `events` map's own keys are sender-chosen
        // and never cross. An absent key means unknown, never 0 (a real, common
        // threshold); RoomInfoController::powerLevelKnown() asks.
        {
            static const char *const kPowerKeys[] = {
                "ban", "invite", "kick", "redact",
                "events_default", "state_default", "users_default",
                "m.space.child", "m.room.name", "m.room.avatar",
                "m.room.topic", "m.room.join_rules",
                "m.room.canonical_alias", "m.room.power_levels",
                "m.room.tombstone",
            };
            const QJsonObject levels =
                event.value(QStringLiteral("power_levels")).toObject();
            QVariantMap powerLevels;
            for (const char *const name : kPowerKeys) {
                const QString key = QString::fromLatin1(name);
                const QJsonValue value = levels.value(key);
                if (value.isUndefined() || value.isNull())
                    continue;
                powerLevels.insert(
                    key, static_cast<qlonglong>(value.toDouble()));
            }
            snapshot.insert(QStringLiteral("powerLevels"), powerLevels);
        }
        snapshot.insert(QStringLiteral("roomVersion"),
                        event.value(QStringLiteral("room_version")).toString());
        snapshot.insert(
            QStringLiteral("canUpgradeRoom"),
            event.value(QStringLiteral("own_can_upgrade")).toBool());
        // Whether this account may write the call membership, so Join is not
        // offered to a user who cannot.
        snapshot.insert(
            QStringLiteral("canPublishCallMembership"),
            event.value(QStringLiteral("own_can_publish_rtc_membership"))
                .toBool());
        snapshot.insert(QStringLiteral("joinRule"),
                        event.value(QStringLiteral("join_rule")).toString());
        snapshot.insert(
            QStringLiteral("canonicalAlias"),
            event.value(QStringLiteral("canonical_alias")).toString());
        // Room access: history visibility, guest access, alt aliases, the
        // restricted allow list, and two power gates not among the own_can_*
        // flags.
        {
            const QJsonObject access =
                event.value(QStringLiteral("access")).toObject();
            snapshot.insert(QStringLiteral("historyVisibility"),
                            access.value(QStringLiteral("history_visibility"))
                                .toString());
            snapshot.insert(QStringLiteral("guestAccess"),
                            access.value(QStringLiteral("guest_access"))
                                .toString());
            QStringList altAliases;
            for (const QJsonValue &v :
                 access.value(QStringLiteral("alt_aliases")).toArray())
                altAliases.append(v.toString());
            snapshot.insert(QStringLiteral("altAliases"), altAliases);
            QStringList allowed;
            for (const QJsonValue &v :
                 access.value(QStringLiteral("restricted_allow")).toArray())
                allowed.append(v.toString());
            snapshot.insert(QStringLiteral("restrictedAllowedRooms"), allowed);
            snapshot.insert(
                QStringLiteral("restrictedHasUnknownRules"),
                access.value(QStringLiteral("restricted_has_unknown")).toBool());
            snapshot.insert(
                QStringLiteral("canChangeHistoryVisibility"),
                access.value(QStringLiteral("own_can_change_history_visibility"))
                    .toBool());
            snapshot.insert(
                QStringLiteral("canChangeGuestAccess"),
                access.value(QStringLiteral("own_can_change_guest_access"))
                    .toBool());
        }
        snapshot.insert(QStringLiteral("category"),
                        event.value(QStringLiteral("category")).toString());
        // A cache-only snapshot ahead of the synced roster under the same op;
        // the controller renders it but keeps the op pending.
        snapshot.insert(QStringLiteral("partial"),
                        event.value(QStringLiteral("partial")).toBool());
        QVariantList members;
        const QJsonArray rows = event.value(QStringLiteral("members")).toArray();
        for (const QJsonValue &value : rows) {
            const QJsonObject row = value.toObject();
            QVariantMap entry;
            entry.insert(QStringLiteral("userId"),
                         row.value(QStringLiteral("user_id")).toString());
            entry.insert(QStringLiteral("displayName"),
                         row.value(QStringLiteral("display_name")).toString());
            entry.insert(QStringLiteral("avatarUrl"),
                         row.value(QStringLiteral("avatar_url")).toString());
            entry.insert(QStringLiteral("membership"),
                         row.value(QStringLiteral("membership")).toString());
            entry.insert(QStringLiteral("role"),
                         row.value(QStringLiteral("role")).toString());
            entry.insert(
                QStringLiteral("powerLevel"),
                static_cast<qlonglong>(
                    row.value(QStringLiteral("power_level")).toDouble()));
            entry.insert(QStringLiteral("ambiguous"),
                         row.value(QStringLiteral("ambiguous")).toBool());
            entry.insert(QStringLiteral("isOwn"),
                         row.value(QStringLiteral("is_own")).toBool());
            members.append(entry);
        }
        snapshot.insert(QStringLiteral("members"), members);
        // The roster also feeds the member cache behind displayNameFor() /
        // avatarMxcFor() (mention chips, reply headers, thread summaries).
        const QString membersRoomId =
            event.value(QStringLiteral("room_id")).toString();
        if (event.value(QStringLiteral("ok")).toBool()) {
            const auto roomIt = m_rooms.find(membersRoomId);
            if (roomIt != m_rooms.end()) {
                const auto fetched = matrix::rust_timeline::membersFromPayload(rows);
                for (auto it = fetched.constBegin(); it != fetched.constEnd();
                     ++it) {
                    // Merge without clobbering known data with empty fields.
                    MemberInfo &slot = roomIt->members[it.key()];
                    slot.userId = it.key();
                    if (!it->displayName.isEmpty())
                        slot.displayName = it->displayName;
                    if (!it->avatarMxcUrl.isEmpty())
                        slot.avatarMxcUrl = it->avatarMxcUrl;
                }
                // One refresh per fetch: the partial snapshot primes the cache,
                // but only the full roster emits membersChanged, which dirties
                // every loaded row.
                if (!event.value(QStringLiteral("partial")).toBool())
                    Q_EMIT membersChanged(membersRoomId);
            }
        }
        Q_EMIT roomMembersReceived(opId(), membersRoomId, snapshot);
        return true;
    }

    if (type == QLatin1String("room_edit_result")) {
        Q_EMIT roomEditFinished(opId(),
                                event.value(QStringLiteral("room_id")).toString(),
                                event.value(QStringLiteral("field")).toString(),
                                event.value(QStringLiteral("ok")).toBool(),
                                event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("delayed_events_support")) {
        Q_EMIT delayedEventsSupportReceived(
            event.value(QStringLiteral("supported")).toBool(),
            event.value(QStringLiteral("advertised")).toBool());
        return true;
    }

    if (type == QLatin1String("scheduled_send_result")) {
        Q_EMIT scheduledSendFinished(
            opId(), event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("delay_id")).toString(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("reaction_event")) {
        Q_EMIT reactionEventReceived(
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("event_id")).toString(),
            event.value(QStringLiteral("target_event_id")).toString(),
            event.value(QStringLiteral("sender")).toString(),
            event.value(QStringLiteral("key")).toString(),
            static_cast<qint64>(event.value(QStringLiteral("timestamp_ms")).toDouble()));
        return true;
    }
    if (type == QLatin1String("room_profile_result")) {
        Q_EMIT roomProfileResult(
            opId(), event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("field")).toString(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("error")).toString());
        return true;
    }
    if (type == QLatin1String("media_history_page")) {
        QVariantList entries;
        for (const QJsonValue &v :
             event.value(QStringLiteral("entries")).toArray()) {
            const QJsonObject row = v.toObject();
            entries.append(QVariantMap{
                { QStringLiteral("eventId"),
                  row.value(QStringLiteral("event_id")).toString() },
                { QStringLiteral("sender"),
                  row.value(QStringLiteral("sender")).toString() },
                { QStringLiteral("timestampMs"),
                  static_cast<qint64>(
                      row.value(QStringLiteral("ts_ms")).toDouble()) },
                { QStringLiteral("kind"),
                  row.value(QStringLiteral("kind")).toString() },
                { QStringLiteral("body"),
                  row.value(QStringLiteral("body")).toString() },
                { QStringLiteral("filename"),
                  row.value(QStringLiteral("filename")).toString() },
                { QStringLiteral("mimetype"),
                  row.value(QStringLiteral("mimetype")).toString() },
                { QStringLiteral("size"),
                  static_cast<qint64>(
                      row.value(QStringLiteral("size")).toDouble()) },
                { QStringLiteral("width"),
                  static_cast<int>(row.value(QStringLiteral("width")).toInt()) },
                { QStringLiteral("height"),
                  static_cast<int>(row.value(QStringLiteral("height")).toInt()) },
                { QStringLiteral("durationMs"),
                  static_cast<qint64>(
                      row.value(QStringLiteral("duration_ms")).toDouble()) },
                { QStringLiteral("mxc"),
                  row.value(QStringLiteral("mxc")).toString() },
                { QStringLiteral("thumbnailMxc"),
                  row.value(QStringLiteral("thumbnail_mxc")).toString() },
                { QStringLiteral("mediaKey"),
                  row.value(QStringLiteral("media_key")).toString() },
                { QStringLiteral("encrypted"),
                  row.value(QStringLiteral("encrypted")).toBool() },
                { QStringLiteral("url"),
                  row.value(QStringLiteral("url")).toString() },
                { QStringLiteral("host"),
                  row.value(QStringLiteral("host")).toString() },
            });
        }
        Q_EMIT mediaHistoryPage(
            opId(), event.value(QStringLiteral("room_id")).toString(), entries,
            static_cast<qint64>(event.value(QStringLiteral("scanned")).toDouble()),
            static_cast<qint64>(
                event.value(QStringLiteral("scanned_total")).toDouble()),
            static_cast<qint64>(
                event.value(QStringLiteral("undecryptable_total")).toDouble()),
            event.value(QStringLiteral("complete")).toBool(),
            event.value(QStringLiteral("encrypted_room")).toBool());
        return true;
    }
    if (type == QLatin1String("media_history_failed")) {
        Q_EMIT mediaHistoryFailed(
            opId(), event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("message")).toString());
        return true;
    }
    if (type == QLatin1String("room_widget_written")) {
        Q_EMIT roomWidgetWritten(
            static_cast<quint64>(
                event.value(QStringLiteral("op_id")).toDouble(0)),
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("category")).toString());
        return true;
    }
    if (type == QLatin1String("room_widgets")) {
        QVariantList widgets;
        for (const QJsonValue &v :
             event.value(QStringLiteral("widgets")).toArray()) {
            const QJsonObject row = v.toObject();
            QStringList discloses;
            for (const QJsonValue &d :
                 row.value(QStringLiteral("discloses")).toArray()) {
                discloses.append(d.toString());
            }
            widgets.append(QVariantMap{
                { QStringLiteral("id"), row.value(QStringLiteral("id")).toString() },
                { QStringLiteral("creator"),
                  row.value(QStringLiteral("creator")).toString() },
                { QStringLiteral("kind"), row.value(QStringLiteral("kind")).toString() },
                { QStringLiteral("name"), row.value(QStringLiteral("name")).toString() },
                { QStringLiteral("url"), row.value(QStringLiteral("url")).toString() },
                { QStringLiteral("refusal"),
                  row.value(QStringLiteral("refusal")).toString() },
                { QStringLiteral("discloses"), discloses },
            });
        }
        Q_EMIT roomWidgetsReceived(opId(), event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("can_manage")).toBool(false), widgets);
        return true;
    }
    if (type == QLatin1String("room_bridges")) {
        // MSC2346 bridge info is room state any sufficiently powered member can
        // write; every string was sanitized in rust/src/bridges.rs. No user id
        // is carried.
        QVariantList bridges;
        for (const QJsonValue &v :
             event.value(QStringLiteral("bridges")).toArray()) {
            const QJsonObject row = v.toObject();
            bridges.append(QVariantMap{
                { QStringLiteral("protocol"),
                  row.value(QStringLiteral("protocol")).toString() },
                { QStringLiteral("protocolName"),
                  row.value(QStringLiteral("protocolName")).toString() },
                { QStringLiteral("network"),
                  row.value(QStringLiteral("network")).toString() },
            });
        }
        Q_EMIT roomBridgesReceived(
            opId(), event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("ok")).toBool(false), bridges);
        return true;
    }
    if (type == QLatin1String("local_search_result")) {
        QVariantList results;
        for (const QJsonValue &v :
             event.value(QStringLiteral("results")).toArray()) {
            const QJsonObject row = v.toObject();
            results.append(QVariantMap{
                { QStringLiteral("eventId"),
                  row.value(QStringLiteral("event_id")).toString() },
                { QStringLiteral("roomId"),
                  row.value(QStringLiteral("room_id")).toString() },
                { QStringLiteral("sender"),
                  row.value(QStringLiteral("sender")).toString() },
                // senderDisplayName, the key MessageSearchController reads for
                // server search too.
                { QStringLiteral("senderDisplayName"),
                  row.value(QStringLiteral("sender_name")).toString() },
                { QStringLiteral("body"),
                  row.value(QStringLiteral("body")).toString() },
                { QStringLiteral("msgtype"),
                  row.value(QStringLiteral("msgtype")).toString() },
                { QStringLiteral("timestampMs"),
                  static_cast<qint64>(
                      row.value(QStringLiteral("timestamp_ms")).toDouble()) },
            });
        }
        Q_EMIT localSearchFinished(
            opId(), event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("category")).toString(),
            event.value(QStringLiteral("min_chars")).toInt(),
            results);
        return true;
    }
    if (type == QLatin1String("search_index_stats")) {
        Q_EMIT searchIndexStatsReceived(
            opId(),
            static_cast<qint64>(
                event.value(QStringLiteral("messages")).toDouble()),
            static_cast<qint64>(
                event.value(QStringLiteral("rooms")).toDouble()));
        return true;
    }
    if (type == QLatin1String("search_index_swept")) {
        Q_EMIT searchIndexSwept(
            opId(), event.value(QStringLiteral("rooms")).toInt(),
            event.value(QStringLiteral("written")).toInt(),
            static_cast<qint64>(
                event.value(QStringLiteral("messages")).toDouble()),
            static_cast<qint64>(
                event.value(QStringLiteral("indexed_rooms")).toDouble()));
        return true;
    }
    if (type == QLatin1String("search_index_deepened")) {
        Q_EMIT searchIndexDeepened(
            opId(), event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("pages")).toInt(),
            event.value(QStringLiteral("reached_start")).toBool(),
            event.value(QStringLiteral("written")).toInt(),
            static_cast<qint64>(
                event.value(QStringLiteral("messages")).toDouble()),
            event.value(QStringLiteral("category")).toString());
        return true;
    }
    if (type == QLatin1String("timestamp_event")) {
        Q_EMIT eventAtTimestampReceived(
            opId(), event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("event_id")).toString(),
            static_cast<qint64>(
                event.value(QStringLiteral("timestamp_ms")).toDouble()),
            event.value(QStringLiteral("category")).toString());
        return true;
    }
    if (type == QLatin1String("activity_seed")) {
        QVariantList entries;
        const QJsonArray rows = event.value(QStringLiteral("entries")).toArray();
        for (const QJsonValue &v : rows) {
            const QJsonObject o = v.toObject();
            entries.append(QVariantMap{
                { QStringLiteral("eventId"), o.value(QStringLiteral("event_id")).toString() },
                { QStringLiteral("roomId"), o.value(QStringLiteral("room_id")).toString() },
                { QStringLiteral("senderId"), o.value(QStringLiteral("sender")).toString() },
                { QStringLiteral("timestampMs"),
                  static_cast<qint64>(o.value(QStringLiteral("timestamp_ms")).toDouble()) },
                { QStringLiteral("read"), o.value(QStringLiteral("read")).toBool() },
                { QStringLiteral("encrypted"), o.value(QStringLiteral("encrypted")).toBool() },
                { QStringLiteral("preview"), o.value(QStringLiteral("body")).toString() },
                { QStringLiteral("threadRootId"),
                  o.value(QStringLiteral("thread_root_id")).toString() },
                // Not "mention": the seed is GET /notifications?only=highlight,
                // which covers @room, keywords and server rules as well as
                // personal mentions.
                { QStringLiteral("kind"), QStringLiteral("highlight") },
            });
        }
        Q_EMIT activitySeedReceived(entries);
        return true;
    }
    if (type == QLatin1String("room_send_result")) {
        Q_EMIT roomSendFinished(
            opId(), event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }
    if (type == QLatin1String("scheduled_update_result")) {
        Q_EMIT scheduledUpdateFinished(
            opId(), event.value(QStringLiteral("delay_id")).toString(),
            event.value(QStringLiteral("action")).toString(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("message_edit_history")) {
        // SENSITIVE in encrypted rooms: revision bodies are plaintext.
        // Forwarded to the open dialog only; never logged or cached.
        QVariantList revisions;
        for (const QJsonValue &v :
             event.value(QStringLiteral("revisions")).toArray()) {
            const QJsonObject row = v.toObject();
            revisions.append(QVariantMap{
                { QStringLiteral("eventId"),
                  row.value(QStringLiteral("event_id")).toString() },
                { QStringLiteral("sender"),
                  row.value(QStringLiteral("sender")).toString() },
                { QStringLiteral("timestamp"),
                  QDateTime::fromMSecsSinceEpoch(
                      row.value(QStringLiteral("timestamp_ms"))
                          .toVariant().toLongLong()) },
                { QStringLiteral("body"),
                  row.value(QStringLiteral("body")).toString() },
                { QStringLiteral("formattedBody"),
                  row.value(QStringLiteral("formatted_body")).toString() },
                { QStringLiteral("redacted"),
                  row.value(QStringLiteral("redacted")).toBool() },
                { QStringLiteral("undecryptable"),
                  row.value(QStringLiteral("undecryptable")).toBool() },
                { QStringLiteral("isOriginal"),
                  row.value(QStringLiteral("is_original")).toBool() },
                { QStringLiteral("isLatest"),
                  row.value(QStringLiteral("is_latest")).toBool() },
            });
        }
        Q_EMIT editHistoryReceived(
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("event_id")).toString(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("partial")).toBool(), revisions);
        return true;
    }

    if (type == QLatin1String("event_source")) {
        // SENSITIVE: `json` is the decrypted event. Never log it.
        const QJsonObject enc = event.value(QStringLiteral("encryption")).toObject();
        Q_EMIT eventSourceReceived(
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("event_id")).toString(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("json")).toString(),
            QVariantMap{
                { QStringLiteral("encrypted"),
                  enc.value(QStringLiteral("encrypted")).toBool() },
                { QStringLiteral("sender"),
                  enc.value(QStringLiteral("sender")).toString() },
                { QStringLiteral("senderDevice"),
                  enc.value(QStringLiteral("sender_device")).toString() },
                { QStringLiteral("algorithm"),
                  enc.value(QStringLiteral("algorithm")).toString() },
                { QStringLiteral("verification"),
                  enc.value(QStringLiteral("verification")).toString() },
            });
        return true;
    }

    if (type == QLatin1String("device_renamed")) {
        Q_EMIT deviceRenamed(opId(), event.value(QStringLiteral("ok")).toBool(),
                             event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("backup_action_result")) {
        // SENSITIVE: `event` may carry a recovery key. Never log it.
        Q_EMIT backupActionFinished(
            opId(), event.value(QStringLiteral("action")).toString(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("recovery_key")).toString(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("backup_progress")) {
        Q_EMIT backupProgress(
            event.value(QStringLiteral("backup_state")).toString(),
            event.value(QStringLiteral("upload_state")).toString(),
            event.value(QStringLiteral("backed_up")).toVariant().toLongLong(),
            event.value(QStringLiteral("total")).toVariant().toLongLong());
        return true;
    }

    if (type == QLatin1String("room_versions")) {
        QVariantList available;
        for (const QJsonValue &v :
             event.value(QStringLiteral("available")).toArray()) {
            const QJsonObject row = v.toObject();
            available.append(QVariantMap{
                { QStringLiteral("version"),
                  row.value(QStringLiteral("version")).toString() },
                { QStringLiteral("stable"),
                  row.value(QStringLiteral("stable")).toBool() },
            });
        }
        Q_EMIT roomVersionsReceived(
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("default")).toString(), available);
        return true;
    }

    if (type == QLatin1String("room_upgrade_result")) {
        Q_EMIT roomUpgradeFinished(
            opId(), event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("replacement_room_id")).toString(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("room_directory_visibility")) {
        Q_EMIT roomDirectoryVisibilityReceived(
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("published")).toBool());
        return true;
    }

    if (type == QLatin1String("room_leave_result")) {
        Q_EMIT roomLeaveFinished(opId(),
                                 event.value(QStringLiteral("room_id")).toString(),
                                 event.value(QStringLiteral("ok")).toBool(),
                                 event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("room_moderation_result")) {
        Q_EMIT moderationFinished(
            opId(),
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("user_id")).toString(),
            event.value(QStringLiteral("op")).toString(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("room_power_level_result")) {
        Q_EMIT powerLevelChangeFinished(
            opId(),
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("user_id")).toString(),
            // QJsonValue has no toLongLong(); real power levels are exactly
            // representable as doubles.
            static_cast<qlonglong>(
                event.value(QStringLiteral("level")).toDouble()),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("room_power_matrix_result")) {
        Q_EMIT roomPowerMatrixFinished(
            opId(),
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("key")).toString(),
            // QJsonValue has no toLongLong(); real power levels are exactly
            // representable as doubles.
            static_cast<qlonglong>(
                event.value(QStringLiteral("level")).toDouble()),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("room_pinned")) {
        // Re-shaped into camelCase like the member snapshot.
        QVariantList entries;
        const QJsonArray raw =
            event.value(QStringLiteral("entries")).toArray();
        entries.reserve(raw.size());
        for (const QJsonValue &value : raw) {
            const QJsonObject row = value.toObject();
            QVariantMap entry;
            entry.insert(QStringLiteral("eventId"),
                         row.value(QStringLiteral("event_id")).toString());
            const bool available =
                row.value(QStringLiteral("available")).toBool();
            entry.insert(QStringLiteral("available"), available);
            if (available) {
                entry.insert(QStringLiteral("sender"),
                             row.value(QStringLiteral("sender")).toString());
                entry.insert(
                    QStringLiteral("senderDisplayName"),
                    row.value(QStringLiteral("sender_display_name")).toString());
                entry.insert(
                    QStringLiteral("senderAvatarUrl"),
                    row.value(QStringLiteral("sender_avatar_url")).toString());
                entry.insert(
                    QStringLiteral("timestampMs"),
                    static_cast<qlonglong>(
                        row.value(QStringLiteral("timestamp_ms")).toDouble()));
                entry.insert(QStringLiteral("kind"),
                             row.value(QStringLiteral("kind")).toString());
                entry.insert(QStringLiteral("preview"),
                             row.value(QStringLiteral("preview")).toString());
            }
            entries.append(entry);
        }
        QVariantMap snapshot;
        snapshot.insert(QStringLiteral("ok"),
                        event.value(QStringLiteral("ok")).toBool());
        snapshot.insert(QStringLiteral("canPin"),
                        event.value(QStringLiteral("can_pin")).toBool());
        snapshot.insert(QStringLiteral("total"),
                        event.value(QStringLiteral("total")).toInt());
        snapshot.insert(QStringLiteral("truncated"),
                        event.value(QStringLiteral("truncated")).toBool());
        snapshot.insert(QStringLiteral("category"),
                        event.value(QStringLiteral("category")).toString());
        // The complete, uncapped id list; it answers "is this pinned?".
        QStringList ids;
        const QJsonArray rawIds = event.value(QStringLiteral("ids")).toArray();
        ids.reserve(rawIds.size());
        for (const QJsonValue &value : rawIds) {
            const QString id = value.toString();
            if (!id.isEmpty())
                ids.append(id);
        }
        snapshot.insert(QStringLiteral("ids"), ids);
        snapshot.insert(QStringLiteral("entries"), entries);
        Q_EMIT pinnedReceived(opId(),
                              event.value(QStringLiteral("room_id")).toString(),
                              snapshot);
        return true;
    }

    if (type == QLatin1String("room_pin_result")) {
        Q_EMIT pinChangeFinished(
            opId(),
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("event_id")).toString(),
            event.value(QStringLiteral("pin")).toBool(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("changed")).toBool(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("space_child_result")) {
        Q_EMIT spaceChildFinished(opId(),
                                  event.value(QStringLiteral("space_id")).toString(),
                                  event.value(QStringLiteral("room_id")).toString(),
                                  event.value(QStringLiteral("ok")).toBool());
        return true;
    }

    if (type == QLatin1String("space_child_suggested_result")) {
        Q_EMIT spaceChildSuggestedFinished(
            opId(),
            event.value(QStringLiteral("space_id")).toString(),
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("suggested")).toBool(),
            event.value(QStringLiteral("ok")).toBool());
        return true;
    }

    if (type == QLatin1String("own_display_name_result")) {
        // `error` is the server's sentence, collapsed and bounded in Rust;
        // empty means nothing usable. The name is deliberately not in the
        // payload.
        Q_EMIT ownDisplayNameChanged(
            opId(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("error")).toString());
        return true;
    }

    if (type == QLatin1String("mutual_rooms_result")) {
        QVariantList rooms;
        const QJsonArray arr =
            event.value(QStringLiteral("rooms")).toArray();
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            QVariantMap row;
            row.insert(QStringLiteral("roomId"),
                       o.value(QStringLiteral("room_id")).toString());
            row.insert(QStringLiteral("name"),
                       o.value(QStringLiteral("name")).toString());
            row.insert(QStringLiteral("avatarUrl"),
                       o.value(QStringLiteral("avatar_url")).toString());
            row.insert(QStringLiteral("isDirect"),
                       o.value(QStringLiteral("is_direct")).toBool(false));
            rooms.append(row);
        }
        Q_EMIT mutualRoomsReceived(
            opId(), event.value(QStringLiteral("user_id")).toString(), rooms);
        return true;
    }

    if (type == QLatin1String("own_avatar_result")) {
        // As own_display_name_result; the path is deliberately not in the
        // payload.
        Q_EMIT ownAvatarChanged(
            opId(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("error")).toString());
        return true;
    }

    if (type == QLatin1String("space_child_removed_result")) {
        Q_EMIT spaceChildRemoveFinished(
            opId(),
            event.value(QStringLiteral("space_id")).toString(),
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("ok")).toBool());
        return true;
    }

    if (type == QLatin1String("room_target_resolved")) {
        QVariantMap result;
        const bool ok = event.value(QStringLiteral("ok")).toBool();
        result.insert(QStringLiteral("ok"), ok);
        result.insert(QStringLiteral("category"),
                      event.value(QStringLiteral("category")).toString());
        if (ok) {
            result.insert(QStringLiteral("target"),
                          event.value(QStringLiteral("target")).toString());
            QStringList via;
            const QJsonArray rawVia =
                event.value(QStringLiteral("via")).toArray();
            for (const QJsonValue &value : rawVia) {
                const QString server = value.toString();
                if (!server.isEmpty())
                    via.append(server);
            }
            result.insert(QStringLiteral("via"), via);
            result.insert(QStringLiteral("eventId"),
                          event.value(QStringLiteral("event_id")).toString());
            const bool previewOk =
                event.value(QStringLiteral("preview_ok")).toBool();
            result.insert(QStringLiteral("previewOk"), previewOk);
            if (previewOk) {
                result.insert(QStringLiteral("roomId"),
                              event.value(QStringLiteral("room_id")).toString());
                result.insert(QStringLiteral("alias"),
                              event.value(QStringLiteral("alias")).toString());
                result.insert(QStringLiteral("name"),
                              event.value(QStringLiteral("name")).toString());
                result.insert(QStringLiteral("topic"),
                              event.value(QStringLiteral("topic")).toString());
                result.insert(
                    QStringLiteral("avatarUrl"),
                    event.value(QStringLiteral("avatar_url")).toString());
                result.insert(
                    QStringLiteral("members"),
                    static_cast<qlonglong>(
                        event.value(QStringLiteral("members")).toDouble()));
                result.insert(
                    QStringLiteral("joinRule"),
                    event.value(QStringLiteral("join_rule")).toString());
                result.insert(
                    QStringLiteral("membership"),
                    event.value(QStringLiteral("membership")).toString());
                result.insert(QStringLiteral("isSpace"),
                              event.value(QStringLiteral("is_space")).toBool());
            } else {
                result.insert(
                    QStringLiteral("previewCategory"),
                    event.value(QStringLiteral("preview_category")).toString());
            }
        }
        Q_EMIT roomTargetResolved(opId(), result);
        return true;
    }

    if (type == QLatin1String("public_rooms_result")) {
        QVariantList rooms;
        const QJsonArray raw =
            event.value(QStringLiteral("results")).toArray();
        rooms.reserve(raw.size());
        for (const QJsonValue &value : raw) {
            const QJsonObject row = value.toObject();
            QVariantMap entry = discoveryRoomRow(row);
            entry.insert(QStringLiteral("worldReadable"),
                         row.value(QStringLiteral("world_readable")).toBool());
            entry.insert(QStringLiteral("guestCanJoin"),
                         row.value(QStringLiteral("guest_can_join")).toBool());
            rooms.append(entry);
        }
        Q_EMIT publicRoomsReceived(
            opId(), event.value(QStringLiteral("ok")).toBool(), rooms,
            event.value(QStringLiteral("next_batch")).toString(),
            static_cast<quint64>(
                event.value(QStringLiteral("total_estimate")).toDouble()),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("room_join_result")) {
        Q_EMIT roomJoinFinished(
            opId(), event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("room_knock_result")) {
        Q_EMIT roomKnockFinished(
            opId(), event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("knock_cancel_result")) {
        Q_EMIT knockCancelFinished(
            opId(), event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("ignore_user_result")) {
        Q_EMIT ignoreUserFinished(
            opId(), event.value(QStringLiteral("user_id")).toString(),
            event.value(QStringLiteral("ignored")).toBool(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("ignored_users_list")) {
        QStringList users;
        const QJsonArray raw = event.value(QStringLiteral("users")).toArray();
        users.reserve(raw.size());
        for (const QJsonValue &value : raw) {
            const QString id = value.toString();
            if (!id.isEmpty())
                users.append(id);
        }
        Q_EMIT ignoredUsersReceived(
            opId(), event.value(QStringLiteral("ok")).toBool(), users);
        return true;
    }

    if (type == QLatin1String("ignored_users_changed")) {
        // Sync push (no op id): local and remote changes share one update path.
        QStringList users;
        const QJsonArray raw = event.value(QStringLiteral("users")).toArray();
        users.reserve(raw.size());
        for (const QJsonValue &value : raw) {
            const QString id = value.toString();
            if (!id.isEmpty())
                users.append(id);
        }
        Q_EMIT ignoredUsersChanged(users);
        return true;
    }

    if (type == QLatin1String("report_message_result")) {
        Q_EMIT reportMessageFinished(
            opId(), event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("event_id")).toString(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("uia_required")) {
        // Sanitized challenge: stage names only, flattened for the "unsupported
        // stage" display.
        QStringList stages;
        const QJsonArray flows = event.value(QStringLiteral("flows")).toArray();
        for (const QJsonValue &flow : flows) {
            const QJsonArray flowStages = flow.toArray();
            for (const QJsonValue &stage : flowStages) {
                const QString name = stage.toString();
                if (!name.isEmpty() && !stages.contains(name))
                    stages.append(name);
            }
        }
        Q_EMIT uiaRequired(
            opId(),
            event.value(QStringLiteral("has_password_stage")).toBool(),
            event.value(QStringLiteral("wrong_password")).toBool(), stages);
        return true;
    }

    if (type == QLatin1String("device_delete_result")) {
        Q_EMIT deviceDeleteFinished(
            opId(), event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("oauth_management_url")) {
        Q_EMIT oauthManagementUrlReceived(
            opId(), event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("url")).toString());
        return true;
    }

    if (type == QLatin1String("message_search_result")) {
        QVariantList results;
        const QJsonArray raw =
            event.value(QStringLiteral("results")).toArray();
        results.reserve(raw.size());
        for (const QJsonValue &value : raw) {
            const QJsonObject row = value.toObject();
            QVariantMap entry;
            entry.insert(QStringLiteral("roomId"),
                         row.value(QStringLiteral("room_id")).toString());
            entry.insert(QStringLiteral("eventId"),
                         row.value(QStringLiteral("event_id")).toString());
            entry.insert(QStringLiteral("sender"),
                         row.value(QStringLiteral("sender")).toString());
            entry.insert(
                QStringLiteral("senderDisplayName"),
                row.value(QStringLiteral("sender_display_name")).toString());
            entry.insert(
                QStringLiteral("senderAvatarUrl"),
                row.value(QStringLiteral("sender_avatar_url")).toString());
            entry.insert(
                QStringLiteral("timestampMs"),
                static_cast<qlonglong>(
                    row.value(QStringLiteral("timestamp_ms")).toDouble()));
            entry.insert(QStringLiteral("msgtype"),
                         row.value(QStringLiteral("msgtype")).toString());
            entry.insert(QStringLiteral("isSticker"),
                         row.value(QStringLiteral("is_sticker")).toBool());
            entry.insert(
                QStringLiteral("mentionUserIds"),
                row.value(QStringLiteral("mention_user_ids")).toArray()
                    .toVariantList());
            entry.insert(QStringLiteral("hasLink"),
                         row.value(QStringLiteral("has_link")).toBool());
            entry.insert(QStringLiteral("body"),
                         row.value(QStringLiteral("body")).toString());
            results.append(entry);
        }
        Q_EMIT messageSearchFinished(
            opId(), event.value(QStringLiteral("ok")).toBool(), results,
            event.value(QStringLiteral("next_batch")).toString(),
            static_cast<quint64>(
                event.value(QStringLiteral("count")).toDouble()),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("space_children_result")) {
        QVariantList rooms;
        const QJsonArray raw =
            event.value(QStringLiteral("results")).toArray();
        rooms.reserve(raw.size());
        for (const QJsonValue &value : raw) {
            const QJsonObject row = value.toObject();
            QVariantMap entry = discoveryRoomRow(row);
            entry.insert(
                QStringLiteral("childrenCount"),
                static_cast<qlonglong>(
                    row.value(QStringLiteral("children_count")).toDouble()));
            entry.insert(QStringLiteral("suggested"),
                         row.value(QStringLiteral("suggested")).toBool());
            QStringList via;
            const QJsonArray rawVia =
                row.value(QStringLiteral("via")).toArray();
            for (const QJsonValue &server : rawVia) {
                const QString name = server.toString();
                if (!name.isEmpty())
                    via.append(name);
            }
            entry.insert(QStringLiteral("via"), via);
            rooms.append(entry);
        }
        Q_EMIT spaceChildrenReceived(
            opId(), event.value(QStringLiteral("space_id")).toString(),
            event.value(QStringLiteral("ok")).toBool(), rooms,
            event.value(QStringLiteral("truncated")).toBool(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("attachment_send_result")) {
        Q_EMIT attachmentQueueFinished(
            opId(), event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("media_ready")) {
        handleMediaReady(event);
        return true;
    }

    if (type == QLatin1String("media_failed")) {
        Q_EMIT mediaFailed(opId(),
                           event.value(QStringLiteral("key")).toString(),
                           event.value(QStringLiteral("kind")).toInt(),
                           event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("upload_limit")) {
        const qint64 bytes =
            static_cast<qint64>(event.value(QStringLiteral("bytes")).toDouble());
        if (bytes > 0 && bytes != m_maxUploadSize) {
            m_maxUploadSize = bytes;
            Q_EMIT maxUploadSizeChanged();
        }
        return true;
    }

    return false;
}
