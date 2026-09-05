#include "matrix/RustSdkMatrixClient.h"

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
#include "matrix/RustTimelineIngest.h"
#include "matrix_rust.h"
#include "models/UserLookup.h"
#include "storage/AppDataPaths.h"

#include <QDateTime>
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

// v0.9 formatted sends: serialize the body spec for the FFI. Empty map =
// empty bytes = the historical markdown path (callers pass nullptr then).
QByteArray bodySpecJson(const QVariantMap &spec)
{
    if (spec.isEmpty())
        return {};
    return QJsonDocument(QJsonObject::fromVariantMap(spec))
        .toJson(QJsonDocument::Compact);
}

// JSON poll path: the Rust string is already UTF-8, and QJsonDocument
// parses UTF-8 bytes — the previous QString round trip transcoded every
// polled event UTF-8 -> UTF-16 -> UTF-8 on the GUI thread (up to 512
// events per 100 ms tick).
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
    if (msgtype == QLatin1String("notice"))
        return TimelineEvent::Notice;
    if (msgtype == QLatin1String("emote"))
        return TimelineEvent::Emote;
    return TimelineEvent::TextMessage;
}

QString previewFor(const TimelineEvent &event)
{
    // One normalizing choke point for every side-surface summary: a poll's
    // multi-line MSC3381 fallback, a mention's markdown permalink, or any
    // multi-line body must never reach the room list verbatim.
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
    // Closing during discovery or a browser sign-in must not leak the
    // bootstrap handle — it owns a tokio runtime, an in-memory crypto store,
    // and this attempt's session tokens.
    endOAuthAttempt();
    releaseAuthHandle();
    releaseRustHandle();
    // Process exit is the one place the wait is right: the retirement runs on
    // a pool thread, and letting the process tear down around a half-closed
    // SQLite store is how a store gets left mid-write. It is bounded, and by
    // this point there is no UI left to keep responsive.
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
    // SDP store: remote session descriptions must never outlive their
    // session on ANY teardown path (review 2026-08-18 round 2 M1).
    m_callSdpStore.clear();
    clearTimelineInsertBatch();
    m_loggedIn = false;
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
    // Testing hook wins: the smoke harness passes an absolute
    // QTemporaryDir path so every run starts from a clean crypto store.
    if (!m_storePathOverride.isEmpty())
        return m_storePathOverride;

    return matrix::app_data::rustSdkStorePath(userIdForStore);
}

bool RustSdkMatrixClient::ensureRustHandleForIdentity(
    const matrix::app_data::AccountIdentity &identity)
{
    // Open the RECORDED location, not one re-derived from the user id. These
    // two disagreed for any account whose typed localpart casing (or whose
    // delegated server name) differed from the homeserver's canonical answer,
    // and that disagreement is the whole defect.
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

    // A handle/event queue is never reused across login generations. This is
    // the ownership boundary that makes stale async callbacks unobservable.
    releaseRustHandle();

    // Safe path diagnostic — paths only, never tokens/keys/bodies. Logged at
    // INFO so users can grep matrix.rust: from the terminal when the SDK
    // complains about crypto-store mismatches. Emitted BEFORE mkpath: the
    // failure below deliberately keeps the path out of its user-visible
    // message and points at the log instead, so the log line has to exist by
    // then.
    qCInfo(lcRust) << "Rust SDK store path resolved"
                   << "base=" << matrix::app_data::primaryRoot()
                   << "slug=" << slug
                   << "store=" << storePath
                   << "exists=" << QFileInfo::exists(storePath)
                   << "mode=" << (m_storePathOverride.isEmpty()
                                  ? QStringLiteral("persistent")
                                  : QStringLiteral("temporary"));

    // The Rust side tightens the store directory itself, but only the LEAF:
    // mkpath creates every missing parent with the umask's mode, so an
    // account directory created here stays 0755 and lists the slug — which is
    // the Matrix localpart. Tighten each level this call actually creates.
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
        // The path is deliberately NOT in the message: it contains the
        // account slug (the Matrix localpart) and the home directory, and
        // this string is user-visible and copy-pasteable. The full path is
        // already in the log line above for local debugging.
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
    // Receipt privacy defaults to PUBLIC on a fresh bridge, so a user who
    // chose private or off would silently start disclosing again after every
    // account switch. Re-applied here for the same reason media-capable is.
    if (m_readReceiptPrivacy != 0)
        takeRustString(mx_rust_set_receipt_privacy(m_rustHandle,
                                                   m_readReceiptPrivacy));
    // Re-apply media-capable mode: the Rust-side flag defaults OFF on a
    // fresh handle, and a registered media backend must survive account
    // switches (review 2026-08-18 round 2 L4).
    if (m_callMediaCapable)
        // takeRustString: this FFI returns an OWNED char*, and discarding it
        // leaks the allocation. These were the only two unwrapped
        // char*-returning call sites in the tree.
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

// NOTE: this deliberately does NOT clear m_freshLoginIdentity. login() arms
// the marker and then reaches here through ensureRustHandleForIdentity, so
// clearing it would disarm the fresh-store cleanup it exists for. The marker
// is made safe by being COMPARED against the account the attempt is for (see
// the login_failed handler) and by being cleared in detachSession().
namespace {
// The threads that close retired Rust clients.
//
// TWO, not more: retirement is I/O-bound (joining tasks, then SQLite closes),
// and the only way to have several at once is to switch accounts faster than
// they close — which must not spawn a thread per switch. A third switch
// queues behind the first two, which costs nothing the user can see, because
// nothing is waiting on this pool.
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
        // The courtesy "I stopped typing" goes here rather than on the GUI
        // thread: it is a NETWORK send, and the whole point of this function
        // is that no network call is on the caller's critical path.
        if (!typingRoom.isEmpty()) {
            const QByteArray room = typingRoom.toUtf8();
            takeRustString(mx_rust_send_typing(handle, room.constData(), 0));
        }
        const QString shutdown = takeRustString(mx_rust_shutdown_tasks(handle));
        const qint64 shutdownMs = teardown.elapsed();
        teardown.restart();
        // Drops the tokio runtime, so it blocks until every in-flight
        // spawn_blocking finishes, including the SQLite closes. That is the
        // wait this whole change exists to move off the GUI thread.
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

// THE GUI THREAD DOES NOT WAIT FOR ANY OF THIS.
//
// It used to. `mx_rust_shutdown_tasks` joins managed tasks under budgets and
// `mx_rust_destroy` drops the tokio runtime — which blocks until every
// in-flight spawn_blocking finishes, SQLite closes included — and both ran
// here, on the thread that draws the window, reached through
// switchToAccount -> detachSession. That is the reported multi-second freeze
// on an account switch, and the Rust side's own comment called it "the
// largest uninstrumented GUI-thread section in the application".
//
// Cutting the budgets would not have fixed it: a shorter block is still a
// block, and the requirement is that the user can move and use the window
// while the old account closes.
//
// What runs here now is only what is instant and what must be ordered: stop
// polling, drop the C++-side trackers, and DETACH the pointer. Everything
// that can wait is handed to a worker thread with the handle, which owns it
// from that moment.
//
// This is safe for one specific reason, and it is worth stating because it is
// the property that would break the change if it ever stopped being true:
// **the C++ side never receives a callback from Rust.** Events are pulled by
// `pollRustEvents()` on a 100ms timer. Stop the timer, null the pointer, and
// a retiring client has no route back into any QObject — so it cannot reach
// one that has since been destroyed, no matter how long it takes to close.
//
// Callers that are about to DELETE the store must still call
// waitForRustRetirement() first: unlinking a directory out from under an open
// SQLite connection is the one thing this must not race.
void RustSdkMatrixClient::releaseRustHandle()
{
    m_pollTimer.stop();
    clearTimelineInsertBatch();
    if (!m_rustHandle)
        return;

    void *retiring = m_rustHandle;
    const QString typingRoom = m_typingRoom;
    m_typingRoom.clear();
    // Detached BEFORE the hand-off, so nothing on this thread can reach the
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
    matrix::app_data::AccountIdentity identity;
    if (!matrix::app_data::resolveAccountIdentity(homeserver, user, &identity)
        || password.isEmpty()) {
        Q_EMIT loginFailed(tr("Homeserver, user, and password are required."));
        return;
    }

    // Matrix localparts are case-sensitive, so resolveAccountIdentity() keeps
    // the typed casing — but the homeserver answers a login with ITS canonical
    // user id, and that is what gets saved. Typing "Mizerd" for the account
    // saved as "@mizerd:…" therefore used to open a brand-new store under a
    // second slug, leaving the saved session pointing at a store that never
    // existed. Adopt the saved canonical id before any path is derived.
    // The account whose store this login should open, if any: the saved
    // canonical id when the typed casing maps onto one, else the typed id
    // when it is itself saved. Used below to adopt a divergent store.
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
        // The match locates the STORE. It must NOT rewrite the identity sent
        // to the homeserver: localparts are case-sensitive, so @alice and
        // @Alice can be different people, and substituting one for the other
        // means the user cannot sign into their own account and gets an
        // unexplained auth failure. The server's own answer settles the
        // mapping later, in login_ok -> recordStoreLocation.
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
            // An account signed in by an older build may hold its store under
            // a divergent directory. Open the one that is recorded, not the
            // one the user id happens to derive to.
            matrix::app_data::bindStoreSlug(
                &identity, m_settings->storeSlugFor(identity.userId));
        }
    }
    m_openingIdentity = identity;

    // The slug flattening is not injective: refuse a login whose identity
    // collides with a DIFFERENT saved account before contacting the server,
    // since both would alias one settings record and one SDK store.
    if (m_settings && m_settings->accountSlugConflicts(identity.userId)) {
        qCWarning(lcRust) << "login refused: account slug collision"
                          << "slug=" << identity.slug;
        Q_EMIT loginFailed(tr(
            "This account's local storage name collides with a different "
            "account already saved on this device. Remove that account "
            "first if you want to sign in with this one."));
        return;
    }

    // Adoption must run on the LOGIN path too, not only on restore. With
    // nothing recorded yet, an account whose real store sits under a
    // divergent slug (typed localpart casing, or .well-known delegation)
    // reaches login — a locked keyring makes hasSession() false, and
    // "Add account" goes straight here — finds no store at the canonical
    // path, is not blocked because a record exists, and then creates a
    // BRAND-NEW EMPTY store while silently abandoning the only local copy of
    // that account's Megolm keys. No deletion, but the same practical loss of
    // encrypted history as the bug this whole change exists to fix, reached
    // one route over. Adopting here also produces the correct downstream
    // verdict, because the store then genuinely exists.
    if (!pathExistsOrIsLink(identity.rustStorePath) && m_settings
        && !loginOwnerUserId.isEmpty()) {
        // Adopt against the SAVED identity, not the typed one. Two reasons,
        // both of which made an earlier version of this block a no-op in
        // exactly the case it exists for:
        //
        //  * `hasSavedAccount()` is an exact match, and at this point
        //    `identity.userId` is still what the user TYPED (deliberately —
        //    it must not be rewritten before it reaches the homeserver). A
        //    record saved as "@mizerd:…" is not found by "@Mizerd:…", so the
        //    gate never opened for the canonical case-variant scenario.
        //  * The candidate scan excludes the identity's own slug. For the
        //    typed identity that slug IS the divergent directory holding the
        //    real store, so the one candidate that matters was excluded.
        //
        // The saved identity's slug is the canonical one, so the scan can see
        // the typed-casing directory; the resulting store slug is then bound
        // onto the typed identity, which keeps going to the server unchanged.
        matrix::app_data::AccountIdentity adoptTarget;
        if (m_settings->resolveSavedIdentity(loginOwnerUserId, &adoptTarget)) {
            auto adoptionRefusal = matrix::rust_session::StoreBlockReason::None;
            if (adoptDivergentStoreIfUnambiguous(&adoptTarget, &adoptionRefusal))
                matrix::app_data::bindStoreSlug(&identity,
                                                adoptTarget.effectiveStoreSlug());
            if (adoptionRefusal != matrix::rust_session::StoreBlockReason::None) {
                // Contestable ownership: refuse rather than guess, exactly as
                // the restore path does. Nothing was moved or deleted.
                failWithBlockReason(adoptionRefusal, identity);
                return;
            }
        }
    }

    bool storeExists = pathExistsOrIsLink(identity.rustStorePath);
    // v0.7 multi-account: only the TARGET account's own saved record is
    // consulted — other signed-in accounts never block a new login.
    //
    // Record existence and token readability are deliberately SEPARATE. They
    // used to be one flag, which meant a SecretStore that could not be read —
    // a locked keyring, an unreachable session bus, any libsecret error —
    // looked exactly like "no account here" and sent a real user's crypto
    // store into the orphan deletion below. A store is only ever deleted when
    // no account record claims it.
    //
    // "Owner" is resolved from the STORE DIRECTORY, not from the typed user
    // id. A record can be bound to a directory by its canonical slug, by a
    // recorded storeSlug, or by the delegated reconstruction, and asking only
    // "is there a record under the slug I derived?" answers none of those.
    // Under .well-known delegation it answered "no" for a directory holding a
    // real account's crypto store, and the branch below deleted it.
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

    // An orphaned store — the directory exists but no account record could
    // ever restore it — is unusable by definition. The classic source is an
    // earlier failed or cancelled login attempt (the store directory is
    // created before the server accepts the password). Clean it up instead of
    // dead-ending the user on the reset prompt.
    if (storeExists && !targetHasRecord) {
        // Moved aside, never deleted. This verdict has been wrong before, and
        // the store may hold the only copy of someone's room keys, so it has
        // to stay recoverable.
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
    // Remember fresh-store attempts so a failure can clean up after
    // itself instead of poisoning the next attempt.
    m_freshLoginIdentity = storeExists ? matrix::app_data::AccountIdentity{}
                                       : identity;
    // A record exists and its store is here, but the secret backend cannot be
    // read — a locked keyring, an unavailable session bus. The sign-in may be
    // perfectly intact; we simply cannot ask. Classifying that as "store with
    // no session metadata" routes the user to a destructive repair for a
    // problem deletion cannot fix, and costs them their room keys. This is
    // the same "unreadable token is not a missing account" rule the orphan
    // branch above follows, applied to the classification.
    if (storeExists && targetHasRecord && !targetTokenReadable
        && m_settings->secretBackendUnavailable()) {
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
        // This attempt never started, so nothing may still be marked as its
        // fresh store. Without this the marker outlives the only return path
        // that emits neither login_ok nor login_failed. The consumer's
        // store-path comparison already makes such a marker inert, so this is
        // the invariant made literal rather than a second guard: it is armed
        // only while an attempt is actually in flight.
        m_freshLoginIdentity = {};
        setState(Error);
        Q_EMIT loginFailed(tr("Rust SDK backend could not be initialized."));
        return;
    }

    m_homeserver = identity.homeserver;
    m_userId.clear();
    m_deviceId.clear();
    m_loggedIn = false;
    m_rooms.clear();
    m_roomOrder.clear();
    m_timelines.clear();
    m_pendingSends.clear();
    setInitialSyncDone(false);
    Q_EMIT roomsChanged();
    setState(Connecting);

    const QByteArray hsBytes = identity.homeserver.toUtf8();
    const QByteArray userBytes = identity.userId.toUtf8();
    // Convert once and pass through, then scrub the transit buffer — the
    // same rule the recovery-key and import-passphrase paths follow. (The
    // QString original is the caller's; the login form clears its field
    // right after submitting.)
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
    // NAMED, because it used to report as "unattributed". The switch freeze
    // was captured as `GUI stall 45618 ms category= unattributed`, and an
    // unattributed stall tells you only that the thread was blocked — the
    // whole point of the tracer is to say WHERE. Everything this function
    // reaches, including the Rust teardown's now-budgeted waits, is
    // synchronous on the GUI thread, so it belongs in one scope.
    stalltrace::Scope stallScope("account-detach");
    // A real sign-out is in flight: its completion event is the ONLY path
    // that deletes this account's persisted token, record, and store.
    // Invalidating the lifecycle now would discard that completion and
    // silently downgrade the sign-out to a local detach — refuse instead;
    // the caller reports "try again in a moment".
    if (m_lifecycle.signingOut()) {
        qCWarning(lcRust) << "detach refused: sign-out still in flight";
        return false;
    }
    // v0.7 account switch: end the local session without a server logout.
    // The account's SDK store, SecretStore token, and account record are
    // deliberately untouched — restoreSession() reactivates it later.
    qCInfo(lcRust) << "detaching local session"
                   << "slug=" << matrix::app_data::safeUserSlug(m_userId);
    // A detach ABANDONS whatever login attempt was running, and it
    // invalidates the lifecycle so that attempt's login_ok/login_failed is
    // dropped as stale — which is exactly how a fresh-store marker used to
    // survive into the NEXT account. Drop it with the session it belonged to.
    m_freshLoginIdentity = {};
    m_callSdpStore.clear();
    // Stale callbacks from this session become unobservable immediately;
    // releaseRustHandle() then cancels/joins every managed task before the
    // handle is destroyed.
    m_lifecycle.invalidate();
    releaseRustHandle();
    clearLocalState();
    Q_EMIT loggedOut();
    return true;
}

// --- OAuth 2.0 / OIDC ------------------------------------------------------
//
// Phase A runs entirely on m_authHandle, which has no store (see
// rust/src/oauth.rs). Nothing here opens, creates or deletes an account store.
// Phase B — completeOAuthLogin() — is the only place that does, and it runs
// only after the homeserver has named the account and the device.

bool RustSdkMatrixClient::ensureOAuthBootstrapHandle()
{
    releaseAuthHandle();
    m_authHandle = mx_rust_oauth_bootstrap_create();
    if (!m_authHandle) {
        Q_EMIT errorOccurred(tr("Failed to create Rust SDK backend handle."));
        return false;
    }
    // The sign-in may be happening from the login screen, where there is no
    // session handle and the poll timer would otherwise be stopped.
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
    // Discovery and an in-flight sign-in share the bootstrap handle, and
    // ensureOAuthBootstrapHandle() destroys whatever is there. Re-probing
    // mid-sign-in would therefore throw away the Client holding THIS
    // attempt's PKCE verifier and CSRF state, and the callback would arrive
    // to an empty slot. The running attempt wins; the user can cancel it.
    //
    // THE SSO FLOW NEEDS THE SAME GUARD and did not have it. mx_rust_sso_begin
    // parks its bootstrap Client in the same slot, so editing the homeserver
    // field while the browser tab was open — the field has no guard of its
    // own, and typing restarts a debounced re-probe — destroyed the handle
    // the callback needed. The user completed a real sign-in in the browser
    // and got "The sign-in could not be completed", with the login token
    // already spent, because it is single-use.
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
        // A local failure to even start discovery. Report "nothing known"
        // rather than guessing that password works.
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
    // Deliberately a resolved state, not silence: the UI must leave
    // "Signing in".
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
    // One browser sign-in at a time, and never two flows at once: an SSO
    // attempt racing an OAuth attempt would have them fighting over the same
    // bootstrap handle and the same client slot in the bridge.
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
        // SENSITIVE: a single-use login token. It goes straight to the SDK and
        // is never logged, never shown, and never given to QML.
        //
        // The in-flight guard is what makes a STALE callback inert: an earlier
        // attempt that was cancelled or timed out has already cleared it, so a
        // token arriving late cannot complete the newer sign-in.
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
            // Discovery is a one-shot question; drop the handle unless a
            // sign-in is using it.
            if (!m_oauthInFlight)
                releaseAuthHandle();
            continue;
        }

        if (type == QLatin1String("oauth_url")) {
            const QString url = event.value(QStringLiteral("url")).toString();
            if (url.isEmpty())
                continue;
            // Open the system browser here so the flow works even if the UI
            // ignores the signal; the signal drives the waiting/cancel state.
            // A failed launch is REPORTED, not fatal: the flow (timeout,
            // Cancel) stays alive, the UI just gets to say why nothing
            // appeared.
            // HTTPS ONLY. This URL comes from homeserver DISCOVERY, so it
            // is chosen by whatever server the user typed the name of, and
            // openExternally hands it to the desktop's URL handler — any
            // scheme, any registered application. A discovery response
            // naming a non-https scheme is not a sign-in flow.
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
            // SENSITIVE: this event carries access and refresh tokens. Never
            // log `event`.
            const QString userId = event.value(QStringLiteral("user_id")).toString();
            const QString deviceId = event.value(QStringLiteral("device_id")).toString();
            const QString clientId = event.value(QStringLiteral("client_id")).toString();
            const QString accessToken = event.value(QStringLiteral("access_token")).toString();
            const QString refreshToken = event.value(QStringLiteral("refresh_token")).toString();
            completeOAuthLogin(userId, deviceId, clientId, accessToken, refreshToken);
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
            // The bootstrap handle was taken purely to ask this question; if
            // no sign-in is actually running, give it back.
            if (!m_ssoInFlight && !m_oauthInFlight)
                releaseAuthHandle();
            continue;
        }

        if (type == QLatin1String("sso_url")) {
            const QString url = event.value(QStringLiteral("url")).toString();
            if (url.isEmpty())
                continue;
            // HTTPS ONLY. This URL comes from homeserver DISCOVERY, so it
            // is chosen by whatever server the user typed the name of, and
            // openExternally hands it to the desktop's URL handler — any
            // scheme, any registered application. A discovery response
            // naming a non-https scheme is not a sign-in flow.
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
    // Phase A is finished either way: release the store-less handle before
    // anything else, so the bootstrap client (and the tokens it holds in
    // memory) go away even if the checks below refuse.
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

// Legacy SSO's Phase B. Identical account/store handling — see
// adoptBrowserSession — differing only in the persisted auth type and in the
// restore call, because an SSO session IS an ordinary Matrix session and
// restores through matrix_auth(), not through oauth().
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
    // PHASE B. The homeserver has answered, so the account is finally known
    // and a store can be chosen. Everything before this point ran without one.

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

    // Point at the store this account is RECORDED to use, never a freshly
    // derived one — the same rule the password path follows.
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

    // THE gate. A device the authorization server just created must never be
    // attached to a store that belongs to a different device.
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

    // The account this attempt is opening. MUST be set before anything can
    // emit login_failed: the shared failure handler keys its store-slug
    // rewrite and its destructive local-reset prompt on m_openingIdentity, so
    // leaving the previously-opened account here would point both at the
    // WRONG account — a failed browser sign-in for account B offering to
    // delete account A's crypto store.
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
    m_rooms.clear();
    m_roomOrder.clear();
    m_timelines.clear();
    m_pendingSends.clear();
    setInitialSyncDone(false);
    Q_EMIT roomsChanged();
    setState(Connecting);

    // Record the session BEFORE restoring, so a crash mid-restore leaves a
    // store with a matching record rather than an apparent orphan.
    if (m_settings) {
        // authType is the RESTORE ROUTING discriminator: "oauth" goes to
        // oauth().restore_session(), and anything else — including "sso" —
        // takes the ordinary matrix_auth() path, which is correct because an
        // SSO session is an ordinary Matrix session. It is stored under its
        // own name rather than as "password" so the account's origin stays
        // truthful in the record.
        m_settings->saveSession(identity.homeserver, identity.userId, deviceId,
                                accessToken, refreshToken,
                                authType, clientId);
        m_settings->setSyncToken({});
        // RECORD the store location. CLAUDE.md section 6: the store an account
        // uses is recorded, never re-derived twice. The password path does
        // this from the login_ok handler, which is gated on an access_token
        // that the OAuth restore event deliberately does not carry — so
        // without this call OAuth would be the one account type whose slug is
        // recomputed on every restore, logout, reset and orphan check.
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
    // The other half of a switch, and the half the 45-second capture ended
    // in. Named for the same reason as detachSession().
    stalltrace::Scope stallScope("session-restore");
    if (!m_settings || !m_settings->hasSession())
        return false;

    matrix::app_data::AccountIdentity identity;
    // resolveSavedIdentity binds the RECORDED store location; the plain
    // resolver is only the fallback for a session that predates recording.
    if (!m_settings->resolveSavedIdentity(m_settings->userId(), &identity)
        && !matrix::app_data::resolveAccountIdentity(
            m_settings->homeserverUrl(), m_settings->userId(), &identity)) {
        // No safe identity could be derived, so there is no account to name.
        matrix::app_data::AccountIdentity unresolved;
        unresolved.userId = m_settings->userId();
        unresolved.homeserver = m_settings->homeserverUrl();
        // "The saved account details cannot be parsed" is not "this store
        // belongs to someone else". Emitting the latter sentence here is how
        // one generic message came to cover six unrelated causes.
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

    // Repair installs broken by the old typed-slug store path before deciding
    // the store is missing: an older build may have left this account's real
    // store one directory over, under the localpart casing the user typed.
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
    // Carry the refresh token when the account has one. Before 0.6.7 this was
    // dropped on every restore (the Rust side hardcoded None), so a session
    // whose access token expired could not be renewed and surfaced as
    // M_UNKNOWN_TOKEN instead. Empty is normal for password sessions on
    // servers that issue no refresh token.
    const QByteArray refreshBytes = m_settings->refreshToken().toUtf8();

    // Restart WITHOUT logout must restore the existing device and session, not
    // create a new one — and it must do so through the SDK API that owns this
    // session type. Routing an OAuth session through matrix_auth() would fail
    // confusingly and give up its refresh handling. The discriminator is read
    // from QSettings, so it stays readable even when the keyring is not.
    QString result;
    if (m_settings->isOAuthAccount(userId)) {
        const QByteArray clientBytes = m_settings->oauthClientIdFor(userId).toUtf8();
        if (clientBytes.isEmpty()) {
            // An OAuth account with no registration id cannot be restored;
            // say so instead of silently taking the password path.
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
    // The smoke/test harness pins an absolute store path; there is no
    // per-account layout to adopt within.
    if (!identity || !m_storePathOverride.isEmpty() || !identity->isValid())
        return false;

    QStringList candidates =
        matrix::app_data::findCaseVariantStoreSlugs(*identity);
    // Case divergence is only one of the two ways the old code split a store
    // from its record. The other is .well-known delegation, where a bare
    // localpart was paired with the homeserver URL's host instead of the real
    // server name — no casing involved, so the scan above cannot see it.
    // Reconstruct that slug exactly and take it only if a store is really
    // there.
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
    // A directory another saved account owns — by its canonical slug or by
    // its own recorded store location — is that account's store, never ours.
    // Uppercase localparts are valid Matrix identities.
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

    // Recording, not relocating. The store holds the only copy of this
    // account's Megolm keys; pointing at it is reversible, moving it is not.
    // mx_rust_restore renders the verdict — an SDK ownership rejection clears
    // the recording again (see the login_failed handler).
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
    // Taken from the directory that was actually opened, never re-derived:
    // deriving it a second time is what produced two locations for one
    // account in the first place.
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

    // Retirement is asynchronous now, and this is one of the two places that
    // must not benefit from it: unlinking the directory out from under an
    // open SQLite connection is exactly the race that leaves a half-deleted
    // store behind and reports success. Wait for the close, then delete.
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
    // Reset the store this account really uses. A caller that resolved the
    // identity from a user id alone would otherwise delete the canonical slug
    // and leave the divergent one — the exact failure that made "Local
    // Lightning session reset" a lie.
    matrix::app_data::AccountIdentity identity = requested;
    if (m_settings && identity.isValid()) {
        // clearSessionForAccount() matches a case variant against the saved
        // record, so the store lookup has to use the same rule. Looking up
        // storeSlugFor() with a non-exact id returns nothing, and the reset
        // would then clear the record while leaving the store behind.
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
    // Same reason as resetRustStore(): quarantineAccountRustState() RENAMES
    // the store directory, and renaming one out from under an open SQLite
    // connection is the same race as deleting it.
    if (!waitForRustRetirement(kStoreCloseBudgetMs))
        qCWarning(lcRust) << "store close did not finish within the budget;"
                          << "quarantining anyway";
    bool matchedRecord = false;
    const bool sessionOk = clearPersistedAccount(identity, &matchedRecord);
    // Moved aside, not deleted. This is a REPAIR: it acts on the app's belief
    // that the store is unusable or foreign, and for
    // session_account_mismatch / sdk_store_ownership_mismatch that belief is
    // precisely "this store belongs to someone else" — a verdict that has
    // been wrong. The card offers "Quarantine and rebuild" and this is what
    // makes that label true. Explicit sign-out and account removal still
    // delete (finishSignOut), because there the user has stated the account
    // should be gone and leaving key material would be a data-at-rest defect.
    const auto files = matrix::app_data::quarantineAccountRustState(identity);
    // A reset that matched no saved record AND deleted no store did nothing
    // at all. It used to report success anyway — SecretStore backends treat a
    // no-op clear as success and a store that was never there counts as
    // `missing`, not `failed` — which is how a reset aimed at the wrong
    // localpart casing could claim "you can sign in again" while the real
    // record, token and store all survived untouched.
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
        // Nothing was wrong with the filesystem — we simply do not know this
        // account. Never arm the reset UI again for a no-op.
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
    // the user id. Those disagreed for any account whose store slug diverged,
    // and sign-out then reported success while leaving the real crypto store
    // — Megolm and device keys — on disk. m_storePath is the authority; the
    // recorded slug is the fallback when the handle is already gone.
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

    // Queue typing=false before the deterministic join so the old room is
    // cleared while the client and sync transport still belong to this
    // lifecycle.
    if (m_rustHandle && !m_typingRoom.isEmpty()) {
        const QByteArray room = m_typingRoom.toUtf8();
        takeRustString(mx_rust_send_typing(m_rustHandle, room.constData(), 0));
        m_typingRoom.clear();
    }

    // v0.5.7: deterministic managed-task shutdown replaces the 0.5.6
    // import_active poll loop. Rust cancels and *joins* the timeline
    // subscription, joins an in-flight room-key import (the crypto store
    // must never be deleted under a live write), and stops the sync loop.
    // The bounded timeout inside is a last-resort error boundary only.
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
        // THE SESSION TYPE DECIDES THE SIGN-OUT, exactly as it already
        // decides the RESTORE a few hundred lines above — and until now only
        // restore branched. `mx_rust_logout` is `matrix_auth().logout()`,
        // POST /_matrix/client/v3/logout. Against an OAuth/OIDC homeserver
        // (MAS) that is not the revocation endpoint: the tokens are revoked
        // through OAuth's own RFC 7009 endpoint, which is what
        // `mx_rust_oauth_logout` calls. So signing out of an OAuth account
        // tore down everything locally and left the access and refresh tokens
        // LIVE on the server — and the failure was invisible, because the
        // error is mapped to a result category and sign-out proceeds anyway.
        //
        // `mx_rust_oauth_logout` had no caller anywhere in the tree.
        //
        // The discriminator is read from QSettings, so it is still readable
        // when the keyring is not — the same reason the restore path uses it.
        const bool oauthSession = m_settings
            && m_settings->isOAuthAccount(m_signOutIdentity.userId);
        if (oauthSession) {
            // The return value is the DISPATCH result, not the revocation's:
            // the revocation is asynchronous and reports through the
            // logged_out event's "result" field, exactly as the password
            // lane does. Logging this as though it were the outcome said
            // "ok" on every path, including a failed revocation.
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
    // v0.7.x authenticated-media audit: deliberately EMPTY. Every media
    // byte on the Rust backend flows through the SDK's Media API, which
    // negotiates the authenticated /_matrix/client/v1/media endpoints
    // itself. The legacy MediaHelpers URL builders produce UNAUTHENTICATED
    // /_matrix/media/v3 links that modern servers refuse — the only thing
    // a non-empty answer here could do is hand such a dead link to the
    // browser (MediaManager::openExternal). Returning empty makes the
    // legacy branch structurally unreachable instead of dead-by-invariant.
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

    // v0.5.7: rooms with a live SDK timeline send through Timeline::send —
    // the SDK creates the local echo, drives sending → sent/failed
    // transitions, and reconciles the remote echo in place. No C++-side
    // echo, no duplicate.
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

// v0.7: outgoing @-mentions. The body already carries matrix.to markdown
// links; the id list is forwarded to the SDK so it writes m.mentions. Empty
// ids or a room without a live timeline fall back to the plain send path.
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

// v0.5.7: replies, edits, reactions, and redactions route through the
// official matrix-sdk-ui timeline actions when the room's live timeline is
// open (relation JSON is never hand-built in C++). Rooms without a live
// timeline keep the previous refusal.
void RustSdkMatrixClient::sendReply(const QString &roomId,
                                    const QString &replyToEventId,
                                    const QString &body)
{
    if (!timelineActiveFor(roomId)) {
        refuseSend("sendReply");
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
        refuseSend("editMessage");
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray targetBytes = targetEventId.toUtf8();
    const QByteArray bodyBytes = newBody.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_edit(
        m_rustHandle, roomBytes.constData(), targetBytes.constData(),
        bodyBytes.constData(), nullptr, nullptr));
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
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray targetBytes = targetEventId.toUtf8();
    const QByteArray bodyBytes = newBody.toUtf8();
    const QByteArray mentionBytes =
        mentionUserIds.join(QLatin1Char('\n')).toUtf8();
    const QString result = takeRustString(mx_rust_timeline_edit(
        m_rustHandle, roomBytes.constData(), targetBytes.constData(),
        bodyBytes.constData(), mentionBytes.constData(), nullptr));
    if (!result.isEmpty()) {
        Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
    }
}

// ---- v0.9 formatted sends. One real path per lane: the spec overloads
// carry the full guard set; empty specs fall back to the historical
// overloads (which keep their no-timeline fallbacks). A NON-empty spec is
// never silently degraded — losing a formatted body behind the user's back
// is worse than an honest refusal, and the composer only sends into the
// open room, whose timeline is live by construction.
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
        refuseSend("sendTextMessage(formatted)");
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
        refuseSend("sendReply(formatted)");
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
        refuseSend("editMessage(formatted)");
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray targetBytes = targetEventId.toUtf8();
    const QByteArray bodyBytes = newBody.toUtf8();
    const QByteArray mentionBytes =
        mentionUserIds.join(QLatin1Char('\n')).toUtf8();
    const QByteArray specBytes = bodySpecJson(bodySpec);
    const QString result = takeRustString(mx_rust_timeline_edit(
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

void RustSdkMatrixClient::redactEvent(const QString &roomId,
                                      const QString &eventId,
                                      const QString &reason)
{
    if (!timelineActiveFor(roomId)) {
        refuseSend("redactEvent");
        return;
    }
    // A composite thread-timeline id never crosses the FFI (§8). The
    // redaction is addressed by event id through the ROOM, so the real room
    // is all Rust needs — and that is also why deleting a thread reply works
    // now: it used to go through the live room timeline, which hides threaded
    // events, so the SDK could not find the item and nothing was sent.
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
        refuseSend("toggleReaction");
        return;
    }
    // The composite is decomposed HERE and never crosses the FFI (§8). The
    // thread root selects the timeline that actually holds the event: the
    // live room timeline hides threaded events, so a reaction on a thread
    // reply was looked up in a list it is not in and silently did nothing.
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
    // Deliberately NOT gated on timelineActiveFor(): this reads the event's
    // relations through the room, not through the open timeline, and the
    // menu that offers it can be open over a room whose timeline is being
    // rebuilt. The Rust side re-validates the room and the event id.
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

// v0.7 polls. Votes and ends act on a poll visible in the CURRENT room (or
// one of its threads), so the room-timeline-active guard applies to all
// three actions; the thread target is resolved Rust-side (open panel
// timeline, else a transient thread-focused timeline).
void RustSdkMatrixClient::sendPollResponse(const QString &roomId,
                                           const QString &threadRootId,
                                           const QString &pollStartEventId,
                                           const QStringList &answerIds)
{
    if (!timelineActiveFor(roomId)) {
        refuseSend("sendPollResponse");
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray threadBytes = threadRootId.toUtf8();
    const QByteArray pollBytes = pollStartEventId.toUtf8();
    // The FFI list is newline-joined; a hostile poll whose answer ids embed
    // newlines would otherwise submit split, non-matching ids (a spoiled
    // vote). Such ids are dropped rather than mangled.
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
        refuseSend("endPoll");
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
        refuseSend("createPoll");
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
    // Remembered even with no handle, so the value survives a login: the
    // setting is read at startup and the bridge is created afterwards.
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
    // Mode 3 (follow account default) deliberately does NOT reach this
    // range: it is a rule REMOVAL, routed through
    // clearRoomNotificationMode. Accepting it here would send an invalid
    // RoomNotificationMode across the FFI.
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
    // Defence-in-depth: the composite thread-timeline id must never reach a
    // protocol call (see isThreadTimelineId's other guards).
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
    // An EMPTY room id is legitimate: it asks for the globally available
    // packs only. Rust skips the active-room step in that case.
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_stickers_fetch_packs(m_rustHandle, room.constData(), opId));
    if (!result.isEmpty()) {
        // A rejection at the edge still has to ANSWER, or a picker that
        // opened on it waits forever for a snapshot that will never arrive.
        //
        // An empty list is the HONEST shape here rather than a lost read:
        // the Rust entry point can only refuse for a null handle, a
        // non-UTF-8 argument, or no logged-in session, and the first and
        // third are already excluded by the guard above. So reaching this
        // line means there is no session, and an account with no session
        // genuinely has no packs. It is not the "a failed read must keep the
        // last known list" case — that one is a request that was ACCEPTED
        // and then failed, which answers through the poll event and never
        // through here.
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
    // Neither the mxc nor the body is logged: the body is the sticker's own
    // alt text, which a pack author chose, and an mxc identifies media.
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
        // A literal tag only: the rejection can carry the PATH back, and a
        // home directory contains the user's name.
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
    // A synchronous refusal still reports: the caller holds an op slot and
    // would otherwise sit disabled forever.
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
// The two starters answer with the flow's generation as a decimal string;
// anything else is an error message and means the flow did not start.

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
    // The payload is a QR code's own base64 text. It is NOT logged, here or
    // anywhere: it carries the ephemeral public key and the rendezvous URL
    // for a channel that is about to receive this account's cross-signing
    // secrets.
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
    // A synchronous refusal still has to REPORT: the caller is waiting on the
    // op id and would otherwise sit disabled forever.
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
// One pagination batch. Matches timeline::PAGINATION_BATCH on the Rust
// side; large enough to fill a screen, small enough to stay responsive.
constexpr unsigned short kPaginationBatch = 20;
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
            kPaginationBatch));
    } else {
        const QByteArray roomBytes = roomId.toUtf8();
        result = takeRustString(mx_rust_timeline_paginate_back(
            m_rustHandle, roomBytes.constData(), kPaginationBatch));
    }
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "timeline pagination dispatch failed";
        state.failed = true;
        state.failureTransient = true;
        Q_EMIT paginationStateChanged(roomId);
    } else if (state.failed) {
        // An accepted explicit retry has left the previous terminal state.
        // The Rust loading event follows asynchronously, but presentation
        // must enter loading immediately instead of flashing the old error.
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

void RustSdkMatrixClient::retryFailedSend(const QString &roomId,
                                          const QString &transactionId)
{
    if (isThreadTimelineId(roomId)) {
        // A thread echo is a room send-queue entry; retry it through the
        // room timeline, which always outlives its thread panel.
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
        // Same reasoning as retryFailedSend: a thread echo is a ROOM
        // send-queue entry, and the room timeline outlives its thread panel.
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
    // A requested room is not yet pagination-ready: the Rust registry's
    // timeline_for() accepts requests only after the initial timeline_reset
    // snapshot has supplied and adopted a live room generation.
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
    m_timelineTracker.request(roomId);
    m_pagination.insert(roomId, PaginationState{});
    qCInfo(lcRust) << "timeline open room=" << roomId.right(12);
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
    // Same local bookkeeping an open does — the reload produces a genuine
    // timeline_reset under a NEW room generation, so pagination state must
    // start clean or a stale "reached start" would suppress the backfill the
    // reader gets when they scroll up again.
    clearThreadTimelineState();
    m_timelineTracker.request(roomId);
    m_pagination.insert(roomId, PaginationState{});
    qCInfo(lcRust) << "timeline reload at live room=" << roomId.right(12);
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

// ── v0.6.0: SDK-backed thread timelines ─────────────────────────────────

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
        refuseSend("sendThreadReply");
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
    // A thread panel retry targets its parent room (both timelines are
    // retried in one Rust pass).
    const QString targetRoom = isThreadTimelineId(roomId)
        ? threadTimelineRoomId(roomId)
        : roomId;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 last = m_lastDecryptionRetryMs.value(targetRoom, 0);
    if (now - last < 2000) {
        qCDebug(lcE2ee) << "manual-retry coalesced" << "room="
                        << matrix::e2ee::redactId(targetRoom);
        return;   // bounded: coalesce rapid repeat requests
    }
    m_lastDecryptionRetryMs.insert(targetRoom, now);
    const QByteArray roomBytes = targetRoom.toUtf8();
    takeRustString(mx_rust_timeline_retry_decryption(
        m_rustHandle, roomBytes.constData()));
    qCInfo(lcRust) << "manual decryption retry dispatched";
    qCDebug(lcE2ee) << "manual-retry dispatched" << "room="
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
        // Never apply a malformed/stale thread diff; recover with one fresh
        // snapshot of the same thread. No message bodies in this log line.
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
    // Only the currently requested/active thread may surface the failure.
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
    // Drop mirror state for the closed thread only; a newer thread may
    // already have been requested (its id differs, so it is untouched).
    m_timelines.remove(timelineId);
    m_pagination.remove(timelineId);
    if (m_threadTracker.activeRoom() == timelineId
        || m_threadTracker.requestedRoom() == timelineId)
        m_threadTracker.reset();
    qCInfo(lcRust) << "thread subscription stopped";
}

void RustSdkMatrixClient::closeRoomTimeline()
{
    // A thread panel / Threads view never survives its room: Rust closes
    // them as part of timeline close/open; the C++ mirrors drop immediately.
    clearThreadTimelineState();
    m_threadListRoom.clear();
    m_threadListGeneration = 0;
    if (m_rustHandle && m_timelineTracker.hasActiveTimeline()) {
        const QString room = m_timelineTracker.activeRoom();
        takeRustString(mx_rust_timeline_close(m_rustHandle));
        qCInfo(lcRust) << "timeline close room=" << room.right(12);
    }
    m_timelineTracker.reset();
}

void RustSdkMatrixClient::refuseSend(const char *op)
{
    Q_EMIT errorOccurred(tr("Rust SDK backend does not implement %1 yet.").arg(QLatin1String(op)));
}

void RustSdkMatrixClient::pollRustEvents()
{
    // Stall attribution only — a no-op unless LIGHTNING_GUI_STALL_TRACE is
    // on. The poll drain applies every queued backend diff on the GUI
    // thread, so it is the prime suspect for any unexplained freeze.
    stalltrace::Scope stallScope("rust-poll-drain");

    // Phase A events first, and unconditionally: a browser sign-in normally
    // runs from the login screen, where there is no session handle at all, so
    // this must not sit behind the m_rustHandle guard below.
    drainAuthEvents();

    if (!m_rustHandle)
        return;

    const quint64 eventGeneration = m_handleGeneration;

    // v0.7 defense-in-depth: drain the TERMINAL command lane completely
    // before the bounded bulk batch, so media/GIF results can never be
    // starved (or dropped) behind a timeline-diff flood. The lane's
    // population is bounded by the C++ in-flight discipline, so "fully"
    // is a handful of events; 256 is a defensive iteration cap only.
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

    // Read once per tick rather than per event: enabled() is an atomic load,
    // and the drain runs up to 256 times.
    const bool traceSync = synctrace::enabled();

    m_coalesceTimelineInserts = true;
    // Soft fairness cap per 100 ms tick, with a bounded extension for
    // timeline diffs: one structural SDK transaction arrives as ADJACENT
    // diffs (a receipt move is Set(old row) then Set(new row)), and cutting
    // the drain between them paints the removal a full tick before the
    // addition — a visible receipt flicker indistinguishable in a live
    // capture from a real loss. Past the soft cap the drain continues only
    // while timeline diffs keep coming; the first other event ends the tick.
    // This is a bounded MITIGATION, not an elimination: a pair straddling
    // the hard cap is still split (a >256-event tick means a hydration-
    // scale burst, where a one-tick receipt flicker is invisible anyway),
    // and the worst-case synchronous per-tick work is 4x the old cap —
    // accepted, since diff application is mirror-index bookkeeping and the
    // heavy model updates were already batched by the insert coalescing.
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
        // Only consecutive room-timeline diffs can share one structural
        // transaction. Preserve signal ordering across every other callback.
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
        // Sync-loop liveness. Recorded on a DRAINED event, never on the timer
        // tick: pollRustEvents() runs every 100 ms whether or not the backend
        // produced anything, so stamping it there would measure the timer and
        // report a healthy 100 ms gap through a total outage. The GAP between
        // real events is the measurement that confirms or refutes the leading
        // hypothesis for the minute-long lag — sliding sync's 60 s request
        // timeout (30 s poll + 30 s network) failing to notice a silently
        // dead connection.
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
    // Slug only — the user id itself travels in the signal for the UI to
    // target the repair with, but it never reaches the log.
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
    // Only conditions a local reset can actually repair arm the destructive
    // recovery UI. A missing store has nothing to delete; a revoked token
    // needs a new sign-in, not deletion.
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
        // Safe diagnostic only. Local cleanup remains authoritative and the
        // user is intentionally signing out, so this is not a fatal UI error.
        qCWarning(lcRust) << "rust server logout result=failed"
                          << "message=" << serverMessage;
    } else {
        qCInfo(lcRust) << "rust server logout result=" << serverResult;
    }

    releaseRustHandle();
    clearLocalState();
    // matchedRecord, NOT the discarding overload. resetLocalSession has
    // required `matchedRecord || removedAnything()` since the wrong-casing
    // incident; sign-out used the overload that throws the answer away, so a
    // cleanup that matched no record and deleted no store still reported
    // "Local Lightning session reset. You can sign in again." §6: "target
    // absent" and "reset completed" are different outcomes, and conflating
    // them hides a no-op behind a success message.
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
        // SENSITIVE: carries the rotated access and refresh tokens. Never log
        // `event`. The SDK renewed them in memory; if they are not written
        // back, the store keeps the CONSUMED refresh token and presenting it
        // again can make the server revoke the whole session.
        // Keyed on the CANONICAL active-account id, not raw m_userId.
        // saveSession() writes secrets under the canonicalized id (server name
        // lowercased), and on the password path m_userId is the server's raw
        // answer, which :2798 already acknowledges may differ. Writing under
        // the raw id would put the rotated pair where nothing reads it and
        // leave the CONSUMED refresh token under the canonical key — silently
        // reintroducing the exact bug this event exists to prevent.
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
        // existing revoked-credential state instead of letting sync fail in a
        // loop. The local store is fine — this must NOT invite a reset.
        // Same shape as the M_UNKNOWN_TOKEN path below: the local store is
        // fine, only the credential died, so this reports and must NOT arm a
        // destructive reset (suggestsLocalReset(AccessTokenRevoked) is false).
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

    if (type == QLatin1String("login_ok")) {
        m_freshLoginIdentity = {};
        // SENSITIVE: this event object carries `access_token`. Never pass
        // `event` or the extracted `accessToken` to a log stream. The token
        // must flow only into SecretStore-backed SettingsManager::saveSession
        // and then be forgotten locally. No qCDebug / qCInfo of `event` here.
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
        // SENSITIVE, same rule as the access token: a refresh token mints new
        // access tokens, so it goes straight to the SecretStore and is never
        // logged. Absent for servers that issue non-refreshable sessions, and
        // absent on the restore path (which already has one saved).
        const QString refreshToken = event.value(QStringLiteral("refresh_token")).toString();
        if (m_loggedIn && m_settings && !accessToken.isEmpty()) {
            // This handler serves password login and password restore. OAuth
            // sessions are saved by the OAuth phase-B path, which supplies the
            // "oauth" auth type and the registration client id.
            m_settings->saveSession(m_homeserver, m_userId, m_deviceId, accessToken,
                                    refreshToken);
            m_settings->setSyncToken({});
        }
        // The homeserver, not the login form, decides the canonical user id,
        // and a first-ever login has no saved record to canonicalize against.
        // So the store may have just been created under the typed localpart
        // casing (or, under .well-known delegation, under the URL host)
        // while the record above went in under the server's answer. Record
        // where the store REALLY is, now, before anything else derives a path
        // from the record. Nothing is moved: the mapping is the fix.
        if (m_loggedIn && !accessToken.isEmpty() && identity.isValid()
            && identity.userId == m_userId) {
            recordStoreLocation(identity);
        }
        setState(Disconnected);
        if (m_loggedIn)
            Q_EMIT loginSucceeded(m_userId);
        else
            Q_EMIT loginFailed(tr("Rust SDK login response did not include a user id."));
        return;
    }

    if (type == QLatin1String("login_failed")) {
        m_loggedIn = false;
        setState(Error);
        // A failed fresh-store login must not leave a half-initialised
        // store directory behind — that is exactly what used to poison
        // every later attempt for this account. Release the handle first
        // so no SDK task still owns the store files.
        // IT MUST NAME THE ACCOUNT THIS ATTEMPT ACTUALLY OPENED. The flag is
        // armed in login() and cleared only by login_ok / login_failed — not
        // by login()'s own early returns, nor by detachSession(), logout() or
        // restoreSession(). So: arm it for a fresh-store login of B, switch
        // accounts before B's terminal event drains (detachSession
        // invalidates the generation, so login_ok/login_failed is dropped as
        // stale and the flag survives), then let a later login_failed for A
        // arrive — and this block deletes B's store directory. Comparing
        // against the identity the CURRENT attempt is for makes a stale flag
        // inert instead of destructive.
        // The authority is the store the LIVE HANDLE actually opened, the
        // same rule sign-out uses to decide which store to delete. A marker
        // naming any other store belongs to an attempt that is over.
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
            // The SDK is the authority on store ownership. If we had adopted
            // a divergent directory for this account, that recording is now
            // demonstrably wrong — drop it so the next start re-evaluates
            // instead of pointing at the same wrong store forever. Only the
            // mapping is cleared; no store is touched.
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
            // The homeserver revoked this session. The local store is fine —
            // offering to delete it would destroy the very key material the
            // user still needs. Say what happened and let them sign in again.
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

    if (type.startsWith(QLatin1String("room_list_"))
        && type != QLatin1String("room_list_mode")
        && type != QLatin1String("room_list_sync_state")
        && type != QLatin1String("room_list_error")) {
        handleRoomListDiff(event);
        return;
    }

    if (type == QLatin1String("latest_event_watch_report")) {
        // Counts and timing only (see run_classic_sync): the evidence for
        // tuning LATEST_EVENT_WATCH_CAP against a real account.
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
        // v0.5.8: the classic path re-announces "running" on every /sync
        // callback. Collapse consecutive identical states so the log and
        // downstream handling see each transition once. setState() is
        // already idempotent; distinct transitions (running → offline →
        // retrying → running) are never coalesced, so reconnect is intact.
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
        }
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
        // Sync membership poke: reaches ONLY the roster-refetch consumers
        // via roomMemberEventSeen — never membersChanged, whose timeline
        // consumer repaints every loaded row (review H1). Without this poke
        // the People panel only refreshed when it was reopened (live report
        // 2026-08-14).
        //
        // Rate limiting: the m.room.member handler in Rust limits itself to
        // one poke per room per second. The v0.7.x m.room.power_levels
        // handler reuses this SAME event type and is NOT rate-limited —
        // power-level changes are human-paced, and the consumer is
        // single-flighted anyway (RoomInfoController refetches only when no
        // members op is pending), so the cost of a spamming room is
        // serialized refetches rather than a flood.
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        if (m_rooms.contains(roomId))
            Q_EMIT roomMemberEventSeen(roomId);
        return;
    }

    if (type == QLatin1String("room_pinned_changed")) {
        // v0.7.x: m.room.pinned_events changed remotely. No payload — the
        // consumer re-reads the authoritative list, so a remote pin and a
        // local one converge on one code path.
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        if (m_rooms.contains(roomId))
            Q_EMIT pinnedEventsChanged(roomId);
        return;
    }

    if (type == QLatin1String("room_tombstone_changed")) {
        // v0.7.x room upgrades: this room was replaced. Unlike the pinned
        // poke this one CARRIES the successor, because the successor id is
        // the entire fact and Rust took it from the SDK's own typed
        // accessor — re-reading would only add a round trip to reach the
        // same value through the same parse.
        //
        // Nothing here follows the upgrade. It updates one field and lets
        // the room list and the banner observe it; joining or navigating
        // happens only when the user activates the banner.
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
        return;
    }

    // Server push-rule state for one room: an explicit user-defined rule, or
    // the resolved account default. AppController reconciles the device-local
    // cache from user-defined reports. Mode integers only — no rule JSON.
    if (type == QLatin1String("room_notification_mode")) {
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        const int mode = event.value(QStringLiteral("mode")).toInt(-1);
        if (roomId.isEmpty()) return;
        // A successful "follow account default" reports mode 3 with
        // followed_default — the room's user-defined rules were REMOVED.
        // It is a distinct outcome from a rule write, so it travels on its
        // own signal: routing it through roomNotificationModeChanged would
        // either be dropped as a non-user-defined report or, worse, be
        // reconciled as though the server held a rule whose value is 3.
        // Without this the clear could never be acknowledged, and a room
        // whose clear failed once would claim "couldn't save" forever even
        // after a retry succeeded.
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

    // A push-rule write failed. The device-local mode is deliberately kept
    // (notification policy already reflects the user's choice); the signal
    // lets the pickers replace their "saved to your account" wording with
    // an honest kept-on-this-device state for the room.
    if (type == QLatin1String("notification_mode_error")) {
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        if (roomId.isEmpty()) return;
        qCWarning(lcRust) << "room action failed category= notification_mode";
        Q_EMIT roomNotificationModeWriteFailed(roomId);
        return;
    }

    if (type == QLatin1String("initial_sync_done")) {
        setInitialSyncDone(true);
        // v0.5.9: fetch the server upload limit once per session so the
        // composer can enforce the real m.upload.size before dispatching.
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

    // v0.5.7 live SDK timeline events.
    if (type == QLatin1String("timeline_reset")) {
        handleTimelineReset(event);
        return;
    }
    if (type == QLatin1String("timeline_diff")) {
        // Sync-latency tracing: this is the sdk->bridge boundary. The stamp
        // comes from the Rust side (crate::sync_trace_stamp_ms), so the leg is
        // measured rather than assumed. No-op unless LIGHTNING_SYNC_TRACE is
        // set; the id is threaded to the model stage through the diff.
        if (synctrace::enabled()) {
            const quint64 traceId = synctrace::beginEvent(
                event.value(QStringLiteral("room_id")).toString(),
                static_cast<qint64>(
                    event.value(QStringLiteral("trace_sdk_ms")).toDouble()));
            synctrace::noteBridge(traceId);
            handleTimelineDiff(event);
            // The model has the row now.
            synctrace::noteModel(traceId);
            // ...and the UI stage is measured as "the GUI thread finished this
            // event-loop iteration and came back", via a queued call. Stated
            // plainly because it matters: this is a PROXY for presentation,
            // not a frame-presented callback. It captures the delay between a
            // model change and the GUI thread being free again — which is the
            // quantity that makes a message feel late — and it needs no QML
            // plumbing. Per-frame timing belongs to QSG_RENDER_TIMING.
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
    // v0.6.0: SDK-backed thread timeline events.
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
    if (type == QLatin1String("crypto_health")) {
        // Forward verbatim (already sanitized in Rust); AppController stamps
        // the generation before the model adopts it.
        QVariantMap snapshot = event.toVariantMap();
        snapshot.remove(QStringLiteral("type"));
        Q_EMIT cryptoHealthUpdated(snapshot);
        return;
    }
    if (type == QLatin1String("crypto_bootstrap")) {
        // Sanitized observer state (the poll layer already rejected stale
        // session handles; AppController resets the model per session).
        Q_EMIT cryptoBootstrapEvent(
            event.value(QStringLiteral("kind")).toString(),
            event.value(QStringLiteral("state")).toString(),
            static_cast<quint64>(
                event.value(QStringLiteral("count")).toDouble(0)));
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
        // THE CATEGORY DECIDES THE WORDS. Telling someone a REACTION "could
        // not be sent as a thread reply" is the same wrong-text defect the
        // redaction path had, and it arrives on a surface with no retry.
        Q_EMIT errorOccurred(
            category == QLatin1String("reaction_rejected")
                ? tr("The reaction could not be applied.")
                : tr("The thread reply could not be sent."));
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
            // No Retry affordance exists for a reaction, so the message must
            // not point at one.
            Q_EMIT errorOccurred(tr("The reaction could not be applied."));
            return;
        }
        if (category == QLatin1String("redact_rejected")) {
            Q_EMIT errorOccurred(tr("The message could not be deleted."));
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
        // Never log the raw message — categorized only.
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
        // v0.7.1: our confirmation registered; waiting for the peer's.
        Q_EMIT verificationSasConfirmed(
            event.value(QStringLiteral("flow_id")).toString());
        return;
    }
    if (type == QLatin1String("verification_qr_ready")) {
        // Geometry only. `bits_b64` is the QR MODULE GRID, never the
        // payload the code encodes — that stays inside the Rust bridge.
        // Nothing here is logged: even the grid reconstructs the payload.
        const QString flowId = event.value(QStringLiteral("flow_id")).toString();
        const int modules = event.value(QStringLiteral("size")).toInt(0);
        // Bound the geometry BEFORE decoding, so an absurd size can never
        // drive the base64 decode of an oversized payload.
        if (modules <= 0 || modules > QrCodeStore::kMaxModules) {
            qCWarning(lcRust) << "verification QR grid rejected: bad geometry";
            return;
        }
        const QByteArray bits = QByteArray::fromBase64(
            event.value(QStringLiteral("bits_b64")).toString().toLatin1(),
            QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
        // Reject a malformed grid rather than rendering a sheared or
        // truncated code: an unscannable picture presented as a working one
        // is worse than no QR offer at all.
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
        // Surfacing this is what lets the UI distinguish "the peer has not
        // answered yet" from "the handshake is running". Dropping it meant
        // an accepted request looked identical to an unanswered one until
        // the emoji arrived — or, on a stall, forever.
        Q_EMIT verificationReady(
            event.value(QStringLiteral("flow_id")).toString());
        return;
    }
    // verification_sas_started is informational — the sas_ready / done /
    // cancelled path carries every state the UI acts on. Ignore.
    if (type == QLatin1String("verification_sas_started"))
        return;

    if (type == QLatin1String("sync_stalled")) {
        // NOT an error state, deliberately. The sync may still be working —
        // a first full-state request on a large account is heavy — and
        // declaring failure over a slow one would be a worse defect than the
        // silence this reports. It exists so a wedge (issue #2: "starting"
        // for 13 minutes, no socket, no I/O, no sync_error) leaves a line
        // behind instead of an unexplained spinner.
        qCWarning(lcRust) << "sync has not received a first response"
                          << "phase="
                          << event.value(QStringLiteral("phase")).toString()
                          << "waited_secs="
                          << event.value(QStringLiteral("waited_secs")).toInt();
        return;
    }

    if (type == QLatin1String("sync_error")) {
        // This branch is reachable only for the active generation. Shutdown
        // callbacks were rejected in pollRustEvents, so M_UNKNOWN_TOKEN keeps
        // its real error semantics for a live signed-in session.
        setState(Error);
        Q_EMIT errorOccurred(event.value(QStringLiteral("message")).toString(
            tr("Rust SDK sync failed.")));
        return;
    }

    // v0.5.9 room-management / user-search / media command results.
    if (handleRoomCommandEvent(type, event))
        return;

    if (type == QLatin1String("error")) {
        Q_EMIT errorOccurred(event.value(QStringLiteral("message")).toString(
            tr("Rust SDK backend error.")));
        return;
    }

    if (type == QLatin1String("queue_overflow")) {
        // Rust dropped events because the poll timer stalled. Surface once
        // as an error banner so users know some events were lost; do not
        // treat as fatal.
        qCWarning(lcRust) << event.value(QStringLiteral("message")).toString();
        Q_EMIT errorOccurred(event.value(QStringLiteral("message")).toString(
            tr("Rust SDK event queue overflowed.")));
    }
}

void RustSdkMatrixClient::handleRoomsEvent(const QJsonArray &rooms)
{
    QHash<QString, RoomInfo> nextRooms;
    nextRooms.reserve(rooms.size());
    QStringList nextOrder;
    QSet<QString> seen;

    for (const auto &value : rooms) {
        const QJsonObject obj = value.toObject();
        RoomInfo room = roomInfoFromJson(obj);
        if (room.id.isEmpty() || seen.contains(room.id)) continue;
        seen.insert(room.id);
        nextRooms.insert(room.id, room);
        nextOrder.append(room.id);
    }
    // Spaces are NOT in the SDK's room list, so a snapshot of that list does
    // not mention them — and replacing the whole map wholesale therefore
    // dropped the entire Space hierarchy until the next spaces event
    // happened to arrive. Carried over instead: they are keyed separately in
    // m_rooms and rooms() returns unordered entries after the ordered ones.
    for (auto it = m_rooms.cbegin(); it != m_rooms.cend(); ++it) {
        if (it->isSpace && !seen.contains(it.key()))
            nextRooms.insert(it.key(), *it);
    }
    m_rooms = nextRooms;
    m_roomOrder = nextOrder;
    Q_EMIT roomsChanged();
}

RoomInfo RustSdkMatrixClient::roomInfoFromJson(const QJsonObject &obj) const
{
    const QString id = obj.value(QStringLiteral("id")).toString();
    RoomInfo room = m_rooms.value(id);
    room.id = id;
    room.name = obj.value(QStringLiteral("name")).toString(room.name);
    if (room.name.isEmpty()) room.name = room.id;
    room.topic = obj.value(QStringLiteral("topic")).toString(room.topic);
    room.canonicalAlias = obj.value(QStringLiteral("canonical_alias")).toString(room.canonicalAlias);
    room.avatarUrl = obj.value(QStringLiteral("avatar_url")).toString(room.avatarUrl);
    // A present-but-empty preview must not clobber one we already learned
    // from the open timeline or a live event: Rust legitimately sends ""
    // whenever the SDK has no latest event for the room yet, and room-list
    // set/insert diffs arrive on every unread/order change — pre-0.7 this
    // raced previews back to empty until the room was reopened.
    {
        // The Rust latest-event path sends plain text (typed summaries are
        // built Rust-side); normalization still guards legacy multi-line
        // bodies and mention markdown.
        const QString incomingPreview = matrix::preview::normalizePreviewText(
            obj.value(QStringLiteral("last_message_preview")).toString());
        if (!incomingPreview.isEmpty())
            room.lastMessagePreview = incomingPreview;
    }
    // RoomInfo::raiseActivity is monotonic; see its comment. Every writer of
    // the room list's sort key goes through it.
    room.raiseActivity(timestampFromMs(static_cast<qint64>(
        obj.value(QStringLiteral("last_activity_ms")).toDouble(0))));
    room.unreadCount = obj.value(QStringLiteral("unread_count")).toInt(room.unreadCount);
    room.highlightCount = obj.value(QStringLiteral("highlight_count")).toInt(room.highlightCount);
    room.markedUnread = obj.value(QStringLiteral("marked_unread")).toBool(room.markedUnread);
    room.hasUnreadMessages = obj.value(QStringLiteral("has_unread_messages"))
                                 .toBool(room.hasUnreadMessages || room.unreadCount > 0);
    room.encrypted = obj.value(QStringLiteral("encrypted")).toBool(room.encrypted);
    // Review H1: EncryptionState::Unknown must never read as "not
    // encrypted" — absent field defaults to NOT known (fail closed).
    room.encryptionKnown =
        obj.value(QStringLiteral("encryption_known")).toBool(false);
    room.isSpace = obj.value(QStringLiteral("is_space")).toBool(room.isSpace);
    // Defaults to FALSE, not to the previous value, exactly like is_direct
    // below: un-favouriting a room must actually clear the flag. Defaulting
    // to the old value would latch a favourite on for the rest of the
    // session the moment one payload arrived without the field.
    room.isFavourite = obj.value(QStringLiteral("is_favourite")).toBool(false);
    room.isDirect = obj.value(QStringLiteral("is_direct")).toBool(false);
    room.directUserId = obj.value(QStringLiteral("direct_user_id")).toString();
    room.directUserIds.clear();
    for (const auto &value : obj.value(QStringLiteral("direct_user_ids")).toArray())
        room.directUserIds.append(value.toString());
    room.roomType = obj.value(QStringLiteral("room_type")).toString();
    room.prevBatchToken = obj.value(QStringLiteral("prev_batch")).toString(room.prevBatchToken);
    room.inviterUserId = obj.value(QStringLiteral("inviter_user_id")).toString();
    room.inviterDisplayName = obj.value(QStringLiteral("inviter_display_name")).toString();
    // v0.7.x room upgrades. The defaulting form is deliberate and does the
    // right thing in both directions: an ABSENT field (a payload built by
    // an older path, or a backend with no tombstone support) keeps what we
    // already knew, while a PRESENT-but-empty one clears it, because Rust
    // computes these from SDK state on every emission and empty there means
    // the room genuinely has no successor. Both are room ids parsed by
    // ruma; neither is ever free text.
    room.successorRoomId =
        obj.value(QStringLiteral("successor_room_id")).toString(room.successorRoomId);
    room.predecessorRoomId =
        obj.value(QStringLiteral("predecessor_room_id")).toString(room.predecessorRoomId);
    const QString membership = obj.value(QStringLiteral("membership")).toString(
        QStringLiteral("joined"));
    room.membership = membership == QLatin1String("invited") ? RoomInfo::Invited
        : membership == QLatin1String("knocked") ? RoomInfo::Knocked
        : membership == QLatin1String("left") ? RoomInfo::Left : RoomInfo::Joined;
    return room;
}

void RustSdkMatrixClient::handleRoomListDiff(const QJsonObject &event)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    auto reject = [this, &type, &event] {
        // Never apply a malformed/out-of-range diff — that is what would
        // corrupt the ordered registry. Instead request a controlled fresh
        // snapshot from Rust so the model recovers to a complete, correct
        // room set rather than staying stale. (Well-formed dynamic-adapter
        // diffs should never reach here.)
        //
        // NAMED, since 2026-09-05: the storm came back on one account
        // (twelve rejections a minute, every one a snapshot refetch) and the
        // open item asks for the rejected diff itself before any fix — the
        // op alone could not say whether the index was past the registry or
        // the id collided with a row already held. Room ids are stable
        // public identifiers; nothing else of the room is logged.
        const QJsonObject roomObject = event.value(QStringLiteral("room")).toObject();
        const QString roomId = roomObject.value(QStringLiteral("id")).toString();
        qCWarning(lcRust) << "room_list malformed diff rejected op=" << type
                          << "index=" << event.value(QStringLiteral("index")).toInt(-1)
                          << "length=" << event.value(QStringLiteral("length")).toInt(-1)
                          << "room=" << roomId
                          << "known=" << (!roomId.isEmpty() && m_rooms.contains(roomId))
                          << "registry=" << m_roomOrder.size()
                          << "— requesting fresh room-list snapshot";
        if (m_rustHandle)
            takeRustString(mx_rust_resync_rooms(m_rustHandle));
    };
    auto addRoom = [this](int index, const QJsonObject &object) {
        RoomInfo room = roomInfoFromJson(object);
        if (room.id.isEmpty() || m_rooms.contains(room.id)
            || index < 0 || index > m_roomOrder.size()) return false;
        m_rooms.insert(room.id, room);
        m_roomOrder.insert(index, room.id);
        return true;
    };

    bool ok = true;
    if (type == QLatin1String("room_list_append")) {
        for (const auto &value : event.value(QStringLiteral("rooms")).toArray())
            ok = addRoom(m_roomOrder.size(), value.toObject()) && ok;
    } else if (type == QLatin1String("room_list_push_front")) {
        ok = addRoom(0, event.value(QStringLiteral("room")).toObject());
    } else if (type == QLatin1String("room_list_push_back")) {
        ok = addRoom(m_roomOrder.size(), event.value(QStringLiteral("room")).toObject());
    } else if (type == QLatin1String("room_list_insert")) {
        ok = addRoom(event.value(QStringLiteral("index")).toInt(-1),
                     event.value(QStringLiteral("room")).toObject());
    } else if (type == QLatin1String("room_list_set")) {
        const int index = event.value(QStringLiteral("index")).toInt(-1);
        const RoomInfo room = roomInfoFromJson(event.value(QStringLiteral("room")).toObject());
        if (index < 0 || index >= m_roomOrder.size() || room.id.isEmpty()) ok = false;
        else {
            const QString oldId = m_roomOrder.at(index);
            if (room.id != oldId && m_rooms.contains(room.id)) ok = false;
            else {
                m_rooms.remove(oldId); m_rooms.insert(room.id, room); m_roomOrder[index] = room.id;
            }
        }
    } else if (type == QLatin1String("room_list_remove")) {
        const int index = event.value(QStringLiteral("index")).toInt(-1);
        if (index < 0 || index >= m_roomOrder.size()) ok = false;
        else m_rooms.remove(m_roomOrder.takeAt(index));
    } else if (type == QLatin1String("room_list_pop_front")) {
        if (m_roomOrder.isEmpty()) ok = false; else m_rooms.remove(m_roomOrder.takeFirst());
    } else if (type == QLatin1String("room_list_pop_back")) {
        if (m_roomOrder.isEmpty()) ok = false; else m_rooms.remove(m_roomOrder.takeLast());
    } else if (type == QLatin1String("room_list_clear")) {
        m_rooms.clear(); m_roomOrder.clear();
    } else if (type == QLatin1String("room_list_truncate")) {
        const int length = event.value(QStringLiteral("length")).toInt(-1);
        if (length < 0 || length > m_roomOrder.size()) ok = false;
        else while (m_roomOrder.size() > length) m_rooms.remove(m_roomOrder.takeLast());
    } else {
        ok = false;
    }
    if (!ok) { reject(); return; }
    Q_EMIT roomsChanged();
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
        // DIRECT children, in the Space's own m.space.child order — never
        // `descendants`, which is the TRANSITIVE closure. Reading the
        // transitive list here made RoomInfo::childRoomIds mean a different
        // thing on this backend than on the mock and HTTP ones (where it is
        // and always was the direct list), so every consumer that needs the
        // structure the Space's admin built — the rail's subspace nesting and
        // the Channels layout's per-Space rooms — saw one flat run of the
        // whole tree and listed subspace rooms twice. SpaceManager::rebuild
        // walks these to derive the transitive membership it needs.
        //
        // `descendants` is still the fallback: an older/other producer that
        // does not send `children` keeps working, degraded rather than empty.
        room.childRoomIds.clear();
        const QJsonArray directChildren =
            object.value(QStringLiteral("children")).toArray();
        const QJsonArray childSource =
            directChildren.isEmpty()
                ? object.value(QStringLiteral("descendants")).toArray()
                : directChildren;
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
        // DELIBERATELY not appended to m_roomOrder. That list mirrors the
        // SDK's own room list ONE FOR ONE, because every Set/Remove/Truncate
        // diff addresses it BY INDEX. Appending spaces made our list longer
        // than the SDK's, so as soon as the room list grew past the point the
        // spaces were appended at, every index referred to a different room
        // here than there: `Set` then landed on the wrong entry, saw an id
        // that already existed elsewhere, and was rejected as malformed —
        // which requested a fresh snapshot, which re-appended the spaces, and
        // round again. That loop is the "room_list malformed diff rejected"
        // storm in the logs, and it re-emitted the whole room list (with its
        // avatar fetches) many times a minute.
        //
        // Nothing is lost by leaving them out: rooms() returns every m_rooms
        // entry that is not in the order list, after the ordered ones.
    }
    for (auto it = m_rooms.begin(); it != m_rooms.end(); ++it) {
        if (it->isSpace && !present.contains(it.key())) {
            it->childRoomIds.clear(); it->parentSpaceIds.clear();
        }
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

    // v0.5.7: rooms with a live SDK timeline are fed exclusively through
    // timeline_reset / timeline_diff — appending the raw sync event here
    // would duplicate rows. Keep only the room-list preview update.
    if (m_timelineTracker.activeRoom() == roomId
        || m_timelineTracker.requestedRoom() == roomId) {
        auto roomIt = m_rooms.find(roomId);
        if (roomIt != m_rooms.end()) {
            // Raw sync bodies are free-form (poll fallbacks, mention
            // markdown, newlines); the live-timeline diff path follows up
            // with the typed summary, but this writer must be one-line too.
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
    // Untrusted sender HTML — carried through as-is; TimelineModel sanitizes
    // it before QML ever sees it.
    timelineEvent.formattedBody =
        obj.value(QStringLiteral("formatted_body")).toString();
    timelineEvent.timestamp = timestampFromMs(static_cast<qint64>(
        obj.value(QStringLiteral("timestamp_ms")).toDouble(0)));
    if (!timelineEvent.timestamp.isValid())
        timelineEvent.timestamp = QDateTime::currentDateTimeUtc();
    timelineEvent.type = typeFromString(obj.value(QStringLiteral("msgtype")).toString());
    timelineEvent.status = TimelineEvent::Sent;

    // v0.5.0-prep+6: propagate the encryption metadata the Rust bridge
    // emits (is_encrypted / is_decrypted / undecryptable / error_kind).
    // Fall back to the prep+5 `decrypted` boolean for backward
    // compatibility if the FFI is ever downgraded. Never derive
    // plaintext from these fields — they are metadata only.
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
    // v0.6.0 checkpoint 12: mention/thread metadata for notification policy
    // in rooms without a live timeline.
    timelineEvent.mentionsMe =
        obj.value(QStringLiteral("mentions_me")).toBool(false);
    timelineEvent.mentionsRoom =
        obj.value(QStringLiteral("mentions_room")).toBool(false);
    timelineEvent.threadRootId =
        obj.value(QStringLiteral("thread_root_id")).toString();

    // v0.5-prep+3: Rust bridges undecryptable encrypted events with
    // `undecryptable = true` and an empty body. Render an honest
    // placeholder here instead of an empty bubble. The SDK will
    // upgrade the event later (via `event_replaced`) if / when keys
    // arrive; until then the user sees WHY the timeline is silent.
    if (undecryptable && timelineEvent.body.isEmpty()) {
        timelineEvent.body = tr("[unable to decrypt yet]");
        timelineEvent.type = TimelineEvent::Notice;
    }

    // Safe recovery-lifecycle diagnostics for encrypted events (redacted ids,
    // semantic error category — never bodies or ciphertext). Only encrypted
    // events are traced, so this stays quiet in unencrypted rooms.
    if (isEncrypted) {
        qCDebug(lcE2ee) << "encrypted-event"
                        << "room=" << matrix::e2ee::redactId(roomId)
                        << "event=" << matrix::e2ee::redactId(timelineEvent.eventId)
                        << (undecryptable ? "state=utd" : "state=decryptable")
                        << "error=" << (timelineEvent.errorKind.isEmpty()
                                            ? QStringLiteral("none")
                                            : timelineEvent.errorKind);
    }

    // A TRUE THREAD REPLY NEVER ENTERS THE ROOM'S MAIN-TIMELINE MIRROR (§8).
    //
    // This mirror is what TimelineModel::reload() reads, and it keeps filling
    // while a room is CLOSED. openRoom then sets the room id — which reloads
    // from here — BEFORE it opens the SDK timeline, so every room open
    // replayed whatever thread replies had arrived in the background as
    // standalone main-timeline rows, until the SDK snapshot replaced them.
    // If the open failed, no snapshot ever came.
    //
    // Filtered HERE rather than in the model, and that distinction was
    // learned the hard way: TimelineModel derives its thread-root flags and
    // reply counts BY COUNTING the replies in its own event list, so removing
    // them from that list erases the roots and their counts along with them.
    // The mirror is the main timeline's content; keeping it correct at the
    // source leaves every derived index intact.
    //
    // The SIGNAL is still emitted for a threaded event, because it is the
    // notification and Activity Center feed, not the timeline: a mention in a
    // thread of a background room must still reach the user.
    const bool threadedReply = !timelineEvent.threadRootId.isEmpty()
        && timelineEvent.threadRootId != timelineEvent.eventId;
    if (!threadedReply)
        timeline.append(timelineEvent);
    Q_EMIT eventAppended(roomId, timelineEvent);

    auto roomIt = m_rooms.find(roomId);
    if (roomIt != m_rooms.end()) {
        // What may move a room up the list is deliberately NARROW.
        //
        // Reported as "clicking an older room moves it upwards, then it drops
        // back down". Opening a room subscribes it in sliding sync and its
        // BACKLOG then arrives here as ordinary live appends (the same
        // mechanism behind the 0.7.3 self-notification fix), so every one of
        // those appends was writing lastActivity — reordering the list from
        // history — until the next authoritative room-list reconcile put it
        // back. The user saw a room jump and fall for no reason they caused.
        //
        // Three rules, and each excludes a real case seen in that report:
        //   * a VIRTUAL row (date divider, read marker, timeline start) is
        //     not activity at all;
        //   * a StateChange is not activity either — a member joining or
        //     leaving must not raise a silent room above one that is being
        //     talked in, which is the "hidden room updates" the tester
        //     suspected;
        //   * activity NEVER moves backwards. That is what makes replayed
        //     history harmless: an older event cannot lower a room, and a
        //     re-delivered one cannot reorder anything.
        // updateRoomPreviewFrom() already skipped virtual rows; this path did
        // not, and it is the one every live event takes.
        const bool countsAsActivity = !timelineEvent.isVirtual()
            && timelineEvent.type != TimelineEvent::StateChange
            // A call row carries NO body (the sentence is built in
            // TimelineModel), so letting it raise activity would replace the
            // room's last-message preview with an empty string. It used to be
            // a StateChange and was excluded by the clause above, until calls
            // got their own row kind.
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
    // Attributed for stall tracing (2026-08-19): ingesting a batch
    // rebuilds model rows and emits the signals that drive delegate
    // work. No-op unless LIGHTNING_GUI_STALL_TRACE is set.
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
    // A jump-to-live history trim reports its outcome here (counts only, no
    // content). Logged rather than merely emitted: a field nothing consumes
    // cannot verify anything, and "we waited and nothing was released" must
    // be legible in a capture — not inferred (review finding, 2026-08-19).
    if (event.contains(QStringLiteral("trimmed_from"))
        && !event.value(QStringLiteral("trimmed_from")).isNull()) {
        const int before =
            event.value(QStringLiteral("trimmed_from")).toInt(0);
        const bool shrunk =
            event.value(QStringLiteral("trim_shrunk")).toBool(false);
        qCInfo(lcRust) << "timeline live-trim room=" << roomId.right(12)
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
            // One pass over the mirror builds the stable-id index; the
            // previous per-id rescan was O(changed x mirror) QString
            // compares on every pagination flush.
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
                // A later insertion may have brought this updated item into
                // the new range. Its final range payload already contains
                // the update, so a second dataChanged would be redundant.
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
    // Attributed for stall tracing (2026-08-19): ingesting a batch
    // rebuilds model rows and emits the signals that drive delegate
    // work. No-op unless LIGHTNING_GUI_STALL_TRACE is set.
    //
    // Split by OPERATION since 0.7.6+, because "timeline-diff" could not
    // answer the question the unreproduced reaction freeze actually poses. A
    // reaction, an edit, a receipt move and a late decryption all arrive as
    // `Set` on an existing row; a new message or a pagination page arrives as
    // an insert. Those are different costs — an in-place update re-runs one
    // delegate's bindings, an insert restructures the view — and one shared
    // label made a burst of the first indistinguishable from a burst of the
    // second in a capture.
    //
    // Deliberately keyed on the op alone, NOT on what changed inside the item:
    // the tracer records a single global category and CLAUDE.md's rule for it
    // is that a confidently wrong category is worse than a coarse one. "A Set
    // was being applied" is something this function knows for certain.
    const QString diffOp = event.value(QStringLiteral("op")).toString();
    stalltrace::Scope stallScope(diffOp == QLatin1String("set")
                                     ? "timeline-diff-set"
                                     : "timeline-diff");
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const auto generation = static_cast<quint64>(
        event.value(QStringLiteral("room_generation")).toDouble(0));
    if (!m_timelineTracker.accepts(roomId, generation)) {
        flushTimelineInsertBatch();
        // COUNTED, not one line per diff. A superseded generation keeps
        // delivering until its subscription actually stops, and that is a
        // whole timeline's worth of diffs — 97 identical lines in a row in a
        // real session log, for a guard that is WORKING. The count is
        // reported once, by the first diff the new generation accepts.
        //
        // Same rule as the media-burst and pagination summaries: a line that
        // fires per CALLER does not belong in a default-on category; only
        // state transitions do.
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
    // A page starts at index 0 (plain push_front) or 1 (the SDK keeps a
    // TimelineStart sentinel at index 0). Later inserts may land anywhere
    // inside or immediately after the newly inserted range, for example when
    // matrix-sdk-ui adds a date divider. In every such case the net mutation
    // is still one contiguous range and can be published atomically.
    const bool startsPaginationRange = m_timelineInsertBatchCount == 0
        && (insertionIndex == 0 || insertionIndex == 1);
    const bool extendsPaginationRange = sameBatch
        && insertionIndex >= m_timelineInsertBatchFirst
        && insertionIndex <= m_timelineInsertBatchFirst
                             + m_timelineInsertBatchCount;
    const bool canBatchInsertion = m_coalesceTimelineInserts
        && insertionIndex >= 0
        && (startsPaginationRange || extendsPaginationRange);
    // matrix-sdk-ui interleaves `set` diffs while constructing a page (date
    // separators, receipts, profile/decryption refreshes). Flushing on every
    // such update turned one 20-row page into as many as eleven independent Qt
    // insertion transactions. Keep valid sets inside the assembly window: a
    // set inside the inserted range is folded into the final range payload;
    // one outside it is replayed by stable id after the insertion signal.
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
        // An identity-less virtual item cannot safely be found again after
        // later insertions shift its row. Publish the assembled page first,
        // then let the ordinary Changed path below update its current index.
    }

    // A syntactically insert-like event can still fail validation. Any older
    // valid batch must reach observers before the recovery reset below.
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
        // Never apply a malformed/stale diff. Recover with one fresh
        // snapshot instead of corrupting model state. No message bodies
        // in this log line.
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
        qCInfo(lcRust) << "timeline pagination complete reached_start="
                       << state.reachedStart;
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
    // Safe recovery-lifecycle diagnostics: redacted room id, semantic state,
    // and a session COUNT only — never session ids, keys, or bodies.
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
    // Best-effort scrub of the recovery secret's transit buffer, mirroring
    // importRoomKeys (the QString original is owned by the caller, which
    // clears its field immediately after submitting).
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
    qCInfo(lcRust) << "reload_timeline start room=" << roomId.right(12)
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
    // Convert once and pass through — do NOT keep a QString copy of the
    // passphrase alive in the C++ layer beyond this call.
    QByteArray pathBytes = filePath.toUtf8();
    QByteArray passphraseBytes = passphrase.toUtf8();
    const QString r = takeRustString(mx_rust_import_room_keys(
        m_rustHandle, pathBytes.constData(), passphraseBytes.constData()));
    // Best-effort scrub. QByteArray is not zeroizing but the buffers go
    // out of scope on return and the passphrase is not kept anywhere in
    // C++ after this line.
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
// v0.5.9 — conversation creation, membership, room editing, media bridge.
//
// Pattern shared by all commands: generate an op id, dispatch to Rust, and
// return the id on acceptance (0 on synchronous rejection). Results arrive
// on the poll queue; handleRustEvent has already rejected stale handle
// generations, and Rust stamps its lifecycle so a signed-out session can
// never complete into a new one.
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
    // Reported, never dropped. This command returns void (the caller owns
    // the op id), so a silent refusal would leave the Settings editor
    // spinning with nothing left to answer it. The failure is posted
    // rather than emitted inline: the caller records the op id AFTER this
    // call returns, and a synchronous emit would arrive before it exists.
    const auto refuse = [this, opId] {
        QMetaObject::invokeMethod(this, [this, opId] {
            Q_EMIT ownDisplayNameChanged(opId, false, QString());
        }, Qt::QueuedConnection);
    };
    if (!m_rustHandle || !m_loggedIn) {
        refuse();
        return;
    }
    // An empty payload is the CLEAR request; Rust maps it to None, which
    // is a different request from storing an empty name. Never trimmed or
    // filtered here — the name is the user's text, emoji, combining marks
    // and non-Latin scripts included, and it is bounded (by characters,
    // not bytes) on the Rust side.
    const QByteArray payload = name.toUtf8();
    const QString result = takeRustString(mx_rust_set_display_name(
        m_rustHandle, payload.constData(), opId));
    if (!result.isEmpty()) {
        // Counts and a literal tag only — the rejection message can carry
        // the submitted name back in some FFI error paths.
        qCWarning(lcRust) << "display-name write rejected";
        refuse();
    }
}

void RustSdkMatrixClient::setOwnAvatar(const QString &localPath, quint64 opId)
{
    // Posted, never emitted inline — identical reasoning to the display-name
    // path above: the caller records the op id AFTER this returns, so a
    // synchronous emit would arrive before there is anything to match it.
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
        // A literal tag only. The rejection can carry the PATH back, and a
        // home directory contains the user's name.
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
    // Scheme allow-list is enforced again in Rust; this early check keeps
    // obviously unsafe schemes from ever crossing the FFI.
    const QString lowered = url.trimmed().toLower();
    if (!m_rustHandle
        || !lowered.startsWith(QLatin1String("https://")))
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray target = url.toUtf8();
    const QString result = takeRustString(mx_rust_get_url_preview(
        m_rustHandle, target.constData(), opId));
    if (!result.isEmpty()) {
        // No URL in the log — operation state only.
        qCWarning(lcRust) << "url preview rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::gifGet(const QString &url)
{
    // https-only guard before the FFI; the URL carries the provider key so it
    // is never logged, here or in Rust.
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
// Every one of these takes an op id and answers through the poll loop, like
// the rest of this bridge. The forget/clear calls are the exception: they are
// synchronous SQLite deletes with nothing to report, and making them
// asynchronous would leave a window in which a redacted message is still
// findable.

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
    // An EMPTY list is meaningful (it clears every alternative alias).
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
    // An EMPTY alias is meaningful here (it clears the canonical alias), so
    // unlike every other setter this one must not reject the empty string.
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

// ── 2026-08-18 voice-call signaling sends ─────────────────────────────
// SDP parameters cross exactly once, into the FFI call, and are never
// logged, stored, or echoed. Results arrive as call_send_result on the
// poll lane; inbound observations as call_* events (see CallSignal.h).

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

quint64 RustSdkMatrixClient::rtcSession(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_rtc_session(m_rustHandle, room.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::rtcTransports(const QString &roomId)
{
    if (!m_rustHandle)
        return 0;
    // An empty room id is legal: discovery is account-scoped and the room
    // only contributes the participant-advertised fallback focus.
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
    // SENSITIVE: keyBase64 is raw media key material. It goes straight into
    // the FFI call and is never logged, never stored, never echoed back.
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

quint64 RustSdkMatrixClient::rtcSetHandRaised(
    const QString &roomId, const QString &membershipEventId,
    const QString &reactionEventId, bool raised)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    // Each direction needs its own id and neither substitutes for the other:
    // a raise annotates the membership, a lower redacts the reaction the
    // raise produced. Refusing here keeps the FFI edge from having to invent
    // a meaning for a missing one.
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
    // Cached so a recreated Rust handle (sign-out → sign-in, account
    // switch) re-learns the mode instead of silently reverting to OFF
    // while a media backend is still registered (review L4).
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
    // Convert once and pass through — do NOT keep a QString copy of the
    // password alive in the C++ layer beyond this call; the Rust side
    // scrubs its own transit buffer the same way (import-passphrase
    // precedent).
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
                                            int width, int height, bool animated)
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
        animated ? 1 : 0, opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "attachment send rejected";
        return 0;
    }
    return opId;
}

// v0.7 video round: the poster crosses the FFI as raw bytes; Rust copies it
// into its own memory before this returns and re-validates it by magic
// sniffing. An absent or rejected poster never fails the video send.
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
    // Same preconditions as sendThreadAttachment (which is what this is —
    // a thread attachment carrying voice metadata), plus the duration the
    // MSC3245 block requires. Deliberately NOT gated on
    // timelineActiveFor(roomId): the room path needs the open room timeline
    // because it sends through it, while the thread path sends through the
    // thread-focused timeline the panel already holds open.
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
    // Deliberately NO timelineActiveFor() gate: this variant exists exactly
    // for rooms whose timeline is not open. The Rust side goes straight to
    // Room::send_attachment, which the SDK still encrypts for the target
    // room when that room is encrypted.
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
                                                  bool animated)
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
        animated ? 1 : 0, opId));
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
    // Kinds 0 (full), 1 (thumb) and 2 (list thumb) all exist on the Rust
    // side; the old clamp to 0..1 turned every list-thumbnail request into a
    // full-thumbnail one, which for an encrypted attachment with no embedded
    // thumbnail meant downloading the whole file to fill a 42 px tile.
    const QString result = takeRustString(mx_rust_media_fetch(
        m_rustHandle, key.constData(),
        static_cast<unsigned int>(qBound(0, kind, 2)), opId,
        static_cast<unsigned int>(qBound(0, timeoutClass, 2))));
    if (!result.isEmpty()) {
        // A synchronous refusal ("unknown media item", …) used to vanish
        // into a return of 0 — a tile that stays a placeholder with nothing
        // in the log. The reason is a constant string from the bridge; no
        // key, path or body is in it.
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
// Shared camelCase reshape for one discovery row (a directory page entry or
// a Space child) so nothing downstream knows the bridge's snake_case names.
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
        // Media-capable mode only (gated in Rust AND here): pure ICE for
        // the engine. Never logged, never rendered.
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
    // MatrixRTC (MSC4143) observation. Every string here was bounded and
    // sanitized in rust/src/rtc.rs; ids and the transport URL are opaque
    // (compared, never logged, never rendered raw).
    if (type == QLatin1String("rtc_session_changed")) {
        // Payload-free poke: a membership in that room changed. The owner
        // answers by re-reading, so a remote change and our own follow the
        // same parse path.
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
            // Own DEVICE, not just own user: the same account on another
            // device is a genuine second participant, and conflating them
            // would make "am I in this call?" answer yes from the wrong
            // device.
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
            event.value(QStringLiteral("delay_id")).toString());
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
        // SENSITIVE: `key` is raw media key material (base64). It goes to
        // the frame cryptor and nowhere else — never logged, never QML.
        Q_EMIT rtcMediaKeyReceived(
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("sender")).toString(),
            event.value(QStringLiteral("claimed_device_id")).toString(),
            event.value(QStringLiteral("key_index")).toInt(),
            event.value(QStringLiteral("key")).toString());
        return true;
    }
    if (type == QLatin1String("sfu_state")) {
        // A server-initiated leave carries LiveKit's DisconnectReason as a
        // closed enum. Logged rather than dropped: "the server told us to
        // leave" with no reason cost a whole debugging round.
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
        Q_EMIT sfuJoined(
            event.value(QStringLiteral("identity")).toString(),
            event.value(QStringLiteral("participants")).toArray().toVariantList(),
            event.value(QStringLiteral("ice_servers")).toArray().toVariantList());
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
        // Media transport only. Gated in Rust on media-capable mode and
        // gated again here, so an SDP cannot cross without an engine.
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
        // Every field was bounded in rust/src/rtc.rs. Ids are opaque here:
        // compared against what we already hold, never rendered, never logged.
        Q_EMIT rtcHandChanged(
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("sender")).toString(),
            event.value(QStringLiteral("membership_event_id")).toString(),
            event.value(QStringLiteral("reaction_event_id")).toString(),
            event.value(QStringLiteral("raised")).toBool());
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
        // SENSITIVE: username/password are live TURN credentials. Never
        // pass `event` or these fields to a log stream (login_ok rule).
        QStringList uris;
        const QJsonArray rows = event.value(QStringLiteral("uris")).toArray();
        for (const QJsonValue &value : rows)
            uris.append(value.toString());
        Q_EMIT callTurnServersReceived(
            opId(), event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("username")).toString(),
            event.value(QStringLiteral("password")).toString(), uris,
            // Clamp BEFORE narrowing: an out-of-range double→int64 cast is
            // UB; Rust already bounds this at the source, this is belt.
            static_cast<qint64>(qBound(
                0.0, event.value(QStringLiteral("ttl_seconds")).toDouble(),
                86400.0)),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    // 2026-08-18 voice-call signaling. One decoder per inbound kind; a
    // field is a stable public Matrix identifier, a closed-set string
    // sanitized in Rust, a boolean — or a SENDER-CHOSEN opaque id
    // (call/party ids: bounded in Rust, never logged or rendered). Never
    // an SDP (see CallSignal.h).
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
            return true; // malformed — drop, never dispatch a partial signal
        // Media-capable mode only: the Rust side includes the remote SDP
        // for invites/answers. It goes into the bounded single-shot store,
        // NEVER onto the signal (CallSignal is structurally SDP-free) and
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
        // WHICH ROUTE PRODUCED THIS CARD: "server" or "client".
        //
        // The mapping here is field-by-field and explicit, so a field the
        // Rust side emits and this list omits is dropped SILENTLY — which
        // for this one would mean the UI could never tell whether the
        // member's IP reached the linked site, and would have to keep
        // warning about an exposure that did not happen.
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
        // The bounded JSON body is provider data (no key, no Matrix ids); the
        // GIF controller parses it into safe structs. Never logged.
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
        // Take the parked GIF bytes into Qt-owned memory (one bounded copy),
        // then release the Rust buffer — mirrors media_ready.
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
        // Presentation-safe rows only; the Rust side already refused to
        // send anything else. A failed lookup is forwarded as ok=false so
        // the card keeps what it had rather than being handed an empty set
        // that would read as "nobody is in this thread".
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
        // Every field below was validated and bounded in rust/src/stickers.rs
        // (mxc-only urls, an allowlisted declared mimetype, control
        // characters stripped, lengths and counts capped). Nothing is
        // re-derived here; this is a transcription, deliberately, so there is
        // exactly ONE place that decides what a pack may contain.
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
        // Only the keys a MATCH carries, so a caller cannot read a stale
        // reason off a check that found nothing.
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
        // Only the keys this step actually carries, so a consumer cannot
        // read a stale value from a previous step's shape.
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
        // Presentation-safe rows only (state string, activity flag, coarse
        // last-active age). ok=false entries carry a category and mean
        // UNKNOWN for that user — PresenceManager decides what to do with
        // them; nothing here fabricates an offline.
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
                // Type-checked default (the .toInt(-1) idiom below): an
                // absent OR null last_active_ago_ms is -1 = "server sent
                // none". contains() is true for an explicit JSON null and
                // no-argument toDouble() turns null into 0, which the
                // popover would render as "active just now" — a fabricated
                // activity claim (review H1).
                entry.insert(
                    QStringLiteral("lastActiveAgoMs"),
                    static_cast<qlonglong>(
                        obj.value(QStringLiteral("last_active_ago_ms"))
                            .toDouble(-1.0)));
                // v0.9 (phase 10): the peer's status text (already bounded
                // and control-stripped at the Rust boundary).
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
            event.value(QStringLiteral("category")).toString());
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
        // v0.7.x room administration: the remaining SDK-derived permissions
        // plus the room state the admin surface renders.
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
        // 2026-08-26 Space settings: the room's REAL m.room.power_levels
        // thresholds. Copied key-for-key from a FIXED set the Rust side
        // chose — the `events` map's own keys are event types written by
        // whoever last sent the state event, i.e. unbounded sender-chosen
        // strings, and none of them crosses.
        //
        // AN ABSENT KEY IS UNKNOWN, NEVER 0. A threshold of 0 is a real and
        // common configuration, so a defaulted insert would claim the room
        // requires nothing; the map is left without the key instead and
        // RoomInfoController::powerLevelKnown() is what asks.
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
        snapshot.insert(QStringLiteral("joinRule"),
                        event.value(QStringLiteral("join_rule")).toString());
        snapshot.insert(
            QStringLiteral("canonicalAlias"),
            event.value(QStringLiteral("canonical_alias")).toString());
        // v0.9 room access (phase 4): the room's history visibility, guest
        // access, alternative aliases and restricted allow list, plus the
        // two power gates that are not among the older own_can_* flags.
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
        // A cache-only snapshot that precedes the synced roster under the
        // same op; the controller renders it but keeps the op pending.
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
        // v0.6.5: the fetched roster is ALSO the member cache behind
        // displayNameFor()/avatarMxcFor() — mention chips, reply headers
        // and thread summaries resolve through it, and TimelineModel /
        // RoomListModel refresh on membersChanged. Before this write the
        // cache only ever held currently-typing users, so on the Rust
        // backend those surfaces fell back to bare localparts forever
        // (review: both model-layer fixes were inert without this).
        const QString membersRoomId =
            event.value(QStringLiteral("room_id")).toString();
        if (event.value(QStringLiteral("ok")).toBool()) {
            const auto roomIt = m_rooms.find(membersRoomId);
            if (roomIt != m_rooms.end()) {
                const auto fetched = matrix::rust_timeline::membersFromPayload(rows);
                for (auto it = fetched.constBegin(); it != fetched.constEnd();
                     ++it) {
                    // Merge, never clobber known data with empty fields: a
                    // typing-sourced name survives a rosterless avatar row
                    // and vice versa.
                    MemberInfo &slot = roomIt->members[it.key()];
                    slot.userId = it.key();
                    if (!it->displayName.isEmpty())
                        slot.displayName = it->displayName;
                    if (!it->avatarMxcUrl.isEmpty())
                        slot.avatarMxcUrl = it->avatarMxcUrl;
                }
                // One presentation refresh per FETCH, not per snapshot:
                // the partial merge still primes the member cache, but
                // only the full roster fires membersChanged — its
                // timeline consumer dirties every loaded row (review H1).
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
                // senderDisplayName, NOT senderName. MessageSearchController
                // is shared with the SERVER search path, which has always
                // emitted senderDisplayName, and the controller reads only
                // that. A second spelling here meant every local-search row
                // reached the find bar with no sender at all.
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
                // NOT "mention". The seed is GET /notifications with
                // only=highlight, and a highlight is whatever the account's
                // push rules highlight: an @room announcement, a keyword hit
                // or a server-side rule, as well as a personal mention.
                // Claiming "Mentioned you" for all of them made the first
                // screenful of every fresh session assert something the
                // server never said.
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
        // SENSITIVE in an encrypted room: revision bodies are plaintext.
        // Forwarded to the open dialog only; never logged, never cached.
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
            // This payload is a QJsonObject, so value() yields QJsonValue —
            // no toLongLong(). Via toDouble(), matching own_power_level
            // above; real Matrix power levels are far inside the exactly
            // representable range.
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
            // QJsonValue has no toLongLong(); via toDouble() like every
            // other power level on this bridge. Real Matrix levels sit far
            // inside the exactly representable range.
            static_cast<qlonglong>(
                event.value(QStringLiteral("level")).toDouble()),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("room_pinned")) {
        // Re-shaped into camelCase here, exactly like the member snapshot,
        // so nothing downstream has to know the bridge's JSON naming.
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
        // The complete, uncapped id list — what answers "is this pinned?".
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
        // `error` is the server's own sentence, already collapsed and
        // bounded in Rust; empty means it said nothing usable. The name is
        // deliberately absent from the payload.
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
        // Same convention as own_display_name_result: `error` is the
        // server's own sanitized sentence, empty when it said nothing
        // usable. The path is deliberately absent from the payload.
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
        // Sync push (no op id): local and remote list changes both arrive
        // here, so every consumer converges on one update path.
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
        // Sanitized challenge: stage NAMES only. Flows flatten into one
        // list for the honest "unsupported stage" display.
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
