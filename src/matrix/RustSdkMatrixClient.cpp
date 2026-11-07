#include "matrix/RustSdkMatrixClient.h"

#include "matrix/RoomActionError.h"

#include "app/GuiStallTracer.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QMutex>
#include <QThreadPool>
#include "app/SyncLatencyTracer.h"

#include "accounts/HostRetirementPool.h"
#include "accounts/RevocationProofs.h"
#include "accounts/RustBridgeApi.h"
#include "accounts/SessionTokenCustody.h"
#include "accounts/StoreOwnershipRegistry.h"
#include "app/SettingsManager.h"
#include "app/SwitchTrace.h"
#include "auth/OAuthCallbackServer.h"
#include "crypto/BackupController.h"
#include "crypto/E2eeDiagnostics.h"
#include "crypto/QrImageProvider.h"
#include "i18n/LocalizationManager.h"
#include "matrix/EventPreview.h"
#include "matrix/LoginFailureText.h"
#include "matrix/MediaStoreKey.h"
#include "matrix/NetworkRoute.h"
#include "matrix/PushRulesSnapshot.h"
#include "matrix/RoomRecencyRule.h"
#include "matrix/RustSessionPolicy.h"
#include "matrix/RustRoomRegistry.h"
#include "matrix/RustTimelineMirror.h"
#include "matrix/RustTimelineIngest.h"
#include "matrix_rust.h"
#include "models/UserLookup.h"
#include "storage/AppDataPaths.h"
#include "storage/SecretStore.h"

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
#include <QScopeGuard>
#include <QSet>
#include <QSignalBlocker>
#include <QTimeZone>
#include <QCoreApplication>
#include <QUrl>

#include <algorithm>
#include <limits>

Q_LOGGING_CATEGORY(lcRust, "matrix.rust")
Q_LOGGING_CATEGORY(lcRustSfu, "lightning.calls.group")
Q_LOGGING_CATEGORY(lcTokenTrace, "lightning.auth.tokentrace")

namespace {

QString nonBlankOr(const QString &name, const QString &fallback)
{
    return name.trimmed().isEmpty() ? fallback : name.trimmed();
}

bool tokenTraceEnabled()
{
    static const bool enabled =
        lightning::switchtrace::enabledFromEnvValue(qgetenv("LIGHTNING_TOKEN_TRACE"));
    return enabled;
}

const char *liveWriteWord(lightning::accounts::custody::LiveWrite outcome)
{
    using lightning::accounts::custody::LiveWrite;
    switch (outcome) {
    case LiveWrite::Written: return "written";
    case LiveWrite::WriteFailed: return "write_failed";
    case LiveWrite::SkippedRotatedPast: return "skipped_rotated_past";
    case LiveWrite::SkippedNoOwner: return "skipped_no_owner";
    }
    return "unknown";
}

QString takeRustString(char *raw)
{
    if (!raw)
        return {};
    QString out = QString::fromUtf8(raw);
    mx_rust_free_cstring(raw);
    return out;
}

QString passwordLoginFailureText(const QString &reason, const QString &message)
{
    qCInfo(lcRust) << "password sign-in failed reason=" << reason;
    return matrix::login_failure::passwordText(reason, message);
}

QByteArray bodySpecJson(const QVariantMap &spec)
{
    if (spec.isEmpty())
        return {};
    return QJsonDocument(QJsonObject::fromVariantMap(spec))
        .toJson(QJsonDocument::Compact);
}

QByteArray takeRustBytes(char *raw)
{
    if (!raw)
        return {};
    QByteArray out(raw);
    mx_rust_free_cstring(raw);
    return out;
}

constexpr int kPollFailureReportEvery = 600;
constexpr int kPaginationPollMs = 8;
constexpr int kPaginationPollTicks = 250;
constexpr int kPaginationPollQuietTicks = 6;

bool rustPollFailed(const QByteArray &raw)
{
    static qint64 failuresInARow = 0;
    if (!raw.startsWith("error: ")) {
        failuresInARow = 0;
        return false;
    }
    ++failuresInARow;
    if (failuresInARow == 1) {
        qCWarning(lcRust) << "Rust SDK event poll failed:"
                          << QString::fromUtf8(raw.mid(7, 160));
    } else if (failuresInARow % kPollFailureReportEvery == 0) {
        qCInfo(lcRust) << "Rust SDK event poll still failing"
                       << "attempts=" << failuresInARow;
    }
    return true;
}

bool pathExistsOrIsLink(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

void finishInterruptedSignOuts(SettingsManager *settings)
{
    if (!settings)
        return;
    const auto &registry = lightning::accounts::StoreOwnershipRegistry::process();
    for (const auto &identity : settings->pendingSignOuts()) {
        if (registry.claimForDeletion(identity.rustStorePath)
            != lightning::accounts::StoreOwnershipRegistry::ClaimVerdict::Granted) {
            qCWarning(lcRust) << "interrupted sign-out: the store is still in use; "
                                 "finishing at the next start"
                              << "slug=" << identity.slug;
            continue;
        }
        bool matchedRecord = false;
        const bool sessionOk =
            settings->clearSessionForAccount(identity.userId, &matchedRecord);
        const auto files = matrix::app_data::removeAccountRustState(identity);
        const auto backgrounds = matrix::app_data::removePersonalBackgrounds(identity);
        const auto accountDir =
            matrix::app_data::removeAccountRootIfEmpty(identity.accountRoot);
        if (files.ok() && sessionOk)
            settings->clearPendingSignOut(identity.userId);
        else if (!sessionOk)
            qCWarning(lcRust) << "interrupted sign-out: the account's secrets "
                                 "were not removed, retrying at the next start"
                              << "slug=" << identity.slug;
        qCInfo(lcRust) << "finished an interrupted sign-out"
                       << "slug=" << identity.slug
                       << "matched_record=" << matchedRecord
                       << "session=" << (sessionOk ? "ok" : "failed")
                       << "deleted=" << files.deleted
                       << "missing=" << files.missing
                       << "failed=" << files.failed
                       << "backgrounds_deleted=" << backgrounds.deleted
                       << "account_dir="
                       << matrix::app_data::emptyDirRemovalName(accountDir);
    }
}

QDateTime timestampFromMs(qint64 ms)
{
    if (ms <= 0)
        return {};
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC);
}

void setPreviewFrom(RoomInfo &room, const TimelineEvent &event,
                    const QString &actorLabel = QString())
{
    const auto token = matrix::preview::summaryToken(event, actorLabel);
    room.setPreview(token.kind, token.arg);
}

}

RustSdkMatrixClient::RustSdkMatrixClient(SettingsManager *settings, QObject *parent)
    : MatrixClient(parent)
    , m_settings(settings)
{
    m_pollTimer.setInterval(100);
    connect(&m_pollTimer, &QTimer::timeout, this, &RustSdkMatrixClient::pollRustEvents);
    m_tokenWriteRetry.setSingleShot(true);
    connect(&m_tokenWriteRetry, &QTimer::timeout, this,
            &RustSdkMatrixClient::retryRotatedTokenWrite);
    m_highlightGapTimer.setSingleShot(true);
    connect(&m_highlightGapTimer, &QTimer::timeout, this,
            &RustSdkMatrixClient::flushHighlightGaps);
    connect(AppLocale::instance(), &AppLocale::changed, this, [this] {
        QStringList changed;
        for (auto it = m_rooms.begin(); it != m_rooms.end(); ++it) {
            if (it->retranslatePreview())
                changed.append(it.key());
        }
        for (const QString &roomId : std::as_const(changed))
            Q_EMIT roomUpdated(roomId);
    });

    qCInfo(lcRust) << "Rust SDK backend loaded:"
                   << rustBackendName()
                   << "version" << rustBackendVersion()
                   << "supports_e2ee=" << rustSupportsE2ee();
    finishInterruptedSignOuts(m_settings);
}

void RustSdkMatrixClient::beginQuitRetirement()
{
    if (m_quitRetirementBegun)
        return;
    m_quitRetirementBegun = true;
    m_quitRetirementClock.start();
    m_pollTimer.stop();
    m_retireAsSignOut = m_lifecycle.signingOut();
    m_lifecycle.invalidate();
    endOAuthAttempt();
    releaseAuthHandle();
    releaseProvidersHandle();
    releaseRevokeHandle();
    releaseWhoamiHandle();
    releaseRustHandle();
    qCInfo(lcRust) << "quit: the foreground store started closing early"
                   << "sign_out=" << m_retireAsSignOut;
}

RustSdkMatrixClient::~RustSdkMatrixClient()
{
    m_pollTimer.stop();
    if (!m_quitRetirementBegun) {
        m_retireAsSignOut = m_lifecycle.signingOut();
        m_lifecycle.invalidate();
    }
    endOAuthAttempt();
    releaseAuthHandle();
    releaseProvidersHandle();
    releaseRevokeHandle();
    releaseWhoamiHandle();
    releaseRustHandle();
    stalltrace::Scope stallScope("rust-retirement-wait");
    QElapsedTimer waited;
    waited.start();
    const int budgetMs = m_quitRetirementBegun
        ? int(std::max<qint64>(0, kStoreCloseBudgetMs - m_quitRetirementClock.elapsed()))
        : kStoreCloseBudgetMs;
    const bool retired = waitForRustRetirement(budgetMs);
    const qint64 waitedMs = waited.elapsed();
    // The event loop is over: write what the retired handles left now, or a
    // rotation made while quitting is lost with the process.
    persistRetiredSessionTokens();
    // Quit in the middle of a sign-out (Ctrl+Q, or SIGTERM through the quit
    // bridge): its store is closed now, so finish the deletion here rather than
    // leave the keys on disk until the next start.
    if (m_retireAsSignOut && retired) {
        // This runs inside ~AppController's member teardown, and its
        // connections to the settings' accountsChanged/sessionChanged would
        // fire on a half-destroyed controller. Nothing needs to hear them now.
        const QSignalBlocker quiet(m_settings);
        finishInterruptedSignOuts(m_settings);
    }
    if (!retired) {
        qCWarning(lcRust) << "a retiring Rust client did not close within the"
                          << "budget at shutdown, waited_ms=" << waitedMs;
    } else if (waitedMs * 2 >= kStoreCloseBudgetMs) {
        qCInfo(lcRust) << "rust client retirement wait took" << waitedMs
                       << "ms, over half its" << kStoreCloseBudgetMs
                       << "ms budget";
    }
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

void RustSdkMatrixClient::setStoreOwnershipRegistry(
    lightning::accounts::StoreOwnershipRegistry *r)
{
    m_storeOwnershipRegistry = r;
}

lightning::accounts::StoreOwnershipRegistry &RustSdkMatrixClient::storeOwnershipRegistry() const
{
    return m_storeOwnershipRegistry ? *m_storeOwnershipRegistry
                                    : lightning::accounts::StoreOwnershipRegistry::process();
}

void RustSdkMatrixClient::setRevocationProofs(lightning::accounts::RevocationProofs *proofs)
{
    m_revocationProofs = proofs;
}

void RustSdkMatrixClient::setLiveAccountsLabs(bool on)
{
    if (!on || m_liveAccountsLabs)
        return;
    m_liveAccountsLabs = true;
    storeOwnershipRegistry().setEnforced(true);
    qCInfo(lcRust) << "live accounts (Labs): store claims enforced, revocation proofs consulted";
}

bool RustSdkMatrixClient::labsHardProofAllowsReplace(const QString &recordUserId,
                                                     const QString &recordDeviceId) const
{
    if (!m_liveAccountsLabs || !m_revocationProofs)
        return false;
    const auto hard = m_revocationProofs->hardFor(recordUserId);
    return hard
        && matrix::rust_session::revokedDeviceMayBeReplaced(recordUserId, recordDeviceId,
                                                            recordUserId, hard->deviceId);
}

bool RustSdkMatrixClient::labsSoftProofAllowsResume(const QString &typedUserId,
                                                    const QString &typedHomeserver,
                                                    const QString &recordUserId,
                                                    const QString &recordHomeserver,
                                                    const QString &recordDeviceId) const
{
    if (!m_liveAccountsLabs || !m_revocationProofs)
        return false;
    const auto soft = m_revocationProofs->softFor(recordUserId);
    return soft
        && matrix::rust_session::softLoggedOutDeviceMayResume(
            typedUserId, typedHomeserver, recordUserId, recordHomeserver, recordDeviceId,
            recordUserId, soft->homeserver, soft->deviceId);
}

void RustSdkMatrixClient::reportStoreClaimRefused(int verdict, const QString &slug,
                                                  const char *operation)
{
    qCWarning(lcRust) << "store claim refused; nothing was opened, moved or deleted"
                      << "operation=" << operation << "slug=" << slug
                      << "verdict="
                      << lightning::accounts::StoreOwnershipRegistry::verdictName(
                             lightning::accounts::StoreOwnershipRegistry::ClaimVerdict(verdict));
}

QString RustSdkMatrixClient::storeInUseByBackgroundText()
{
    return tr("This account's data is still in use by a background session. Wait a "
              "moment and try again.");
}

bool RustSdkMatrixClient::storeHeldByBackgroundHost(const QString &storePath) const
{
    using Ownership = lightning::accounts::StoreOwnershipRegistry;
    const auto id = lightning::accounts::fileIdentityOf(storePath);
    const QString absPath = QFileInfo(storePath).absoluteFilePath();
    for (const Ownership::Record &record : storeOwnershipRegistry().records()) {
        const bool same = (id.valid && record.id.valid) ? record.id == id
                                                        : record.absPath == absPath;
        if (same && record.role != Ownership::Role::Foreground)
            return true;
    }
    return false;
}

QString RustSdkMatrixClient::storeClaimRefusalText(const QString &storePath) const
{
    if (storeHeldByBackgroundHost(storePath))
        return storeInUseByBackgroundText();
    return tr("This account's data is still in use. Wait a moment and try again.");
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
    m_callSdpStore.clear();
    clearTimelineInsertBatch();
    m_loggedIn = false;
    m_restoredOffline = false;
    m_ownIdentityKeyMismatchLogged = false;
    m_homeserver.clear();
    m_userId.clear();
    m_deviceId.clear();
    m_rooms.clear();
    m_memberListRecency.clear();
    m_roomOrder.clear();
    m_lastReceiptSent.clear();
    m_syncMode = QStringLiteral("stopped");
    m_lastSyncState.clear();
    m_timelines.clear();
    m_forwardedEditIds.clear();
    resetNotificationProducerState();
    m_pendingSends.clear();
    m_pendingProbes.clear();
    m_timelineTracker.reset();
    m_threadTracker.reset();
    m_contextTracker.reset();
    m_recoveryReopens.clear();
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
    if (!m_storePathOverride.isEmpty())
        return m_storePathOverride;

    return matrix::app_data::rustSdkStorePath(userIdForStore);
}

bool RustSdkMatrixClient::ensureRustHandleForIdentity(
    const matrix::app_data::AccountIdentity &identity)
{
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

    qCInfo(lcRust) << "Rust SDK store path resolved"
                   << "base=" << matrix::app_data::primaryRoot()
                   << "slug=" << slug
                   << "store=" << storePath
                   << "exists=" << QFileInfo::exists(storePath)
                   << "mode=" << (m_storePathOverride.isEmpty()
                                  ? QStringLiteral("persistent")
                                  : QStringLiteral("temporary"));

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

    using Ownership = lightning::accounts::StoreOwnershipRegistry;
    Ownership &ownership = storeOwnershipRegistry();
    const bool claimsEnforced = ownership.enforced();
    quint64 conflictSerial = 0;
    if (!claimsEnforced && ownership.wouldRefuse(storePath, &conflictSerial)) {
        qCInfo(lcRust) << "store ownership: would refuse (record-only)"
                       << "slug=" << slug << "conflict_serial=" << conflictSerial;
    }
    quint64 claimedSerial = 0;
    if (claimsEnforced) {
        auto verdict = ownership.claimOpen(storePath, slug, Ownership::Role::Foreground,
                                           &claimedSerial, &conflictSerial);
        if (verdict == Ownership::ClaimVerdict::RefusedRetiring) {
            bool ownForegroundHandle = false;
            for (const Ownership::Record &record : ownership.records()) {
                if (record.serial == conflictSerial)
                    ownForegroundHandle = record.role == Ownership::Role::Foreground;
            }
            if (ownForegroundHandle && waitForRustRetirement(kStoreCloseBudgetMs))
                verdict = ownership.claimOpen(storePath, slug, Ownership::Role::Foreground,
                                              &claimedSerial, &conflictSerial);
        }
        if (verdict != Ownership::ClaimVerdict::Granted) {
            reportStoreClaimRefused(int(verdict), slug, "open");
            Q_EMIT errorOccurred(storeClaimRefusalText(storePath));
            return false;
        }
    }

    const QByteArray path = QFileInfo(storePath).absoluteFilePath().toUtf8();
    m_rustHandle = mx_rust_create(path.constData());
    if (!m_rustHandle) {
        if (claimedSerial != 0)
            ownership.noteClosed(claimedSerial, true);
        Q_EMIT errorOccurred(tr("Failed to create Rust SDK backend handle."));
        return false;
    }
    if (!claimsEnforced)
        ownership.claimOpen(storePath, slug, Ownership::Role::Foreground, &claimedSerial);
    m_storeOwnershipSerial = claimedSerial;

    m_storePath = storePath;
    m_handleGeneration = m_lifecycle.beginSession();
    applyPerHandleSettings();

    if (!m_sessionFilePath.isEmpty()) {
        const QByteArray sessionPath = m_sessionFilePath.toUtf8();
        const QString result = takeRustString(mx_rust_set_session_file(m_rustHandle,
                                                                        sessionPath.constData()));
        if (!result.isEmpty()) {
            mx_rust_destroy(m_rustHandle);
            m_rustHandle = nullptr;
            m_handleGeneration = 0;
            if (m_storeOwnershipSerial != 0) {
                ownership.noteClosed(m_storeOwnershipSerial, true);
                m_storeOwnershipSerial = 0;
            }
            Q_EMIT errorOccurred(result.startsWith(QLatin1String("error: "))
                                 ? result.mid(7)
                                 : result);
            return false;
        }
    }

    applyMediaStoreKey(slug);
    if (!applyNetworkRoute(slug)) {
        mx_rust_destroy(m_rustHandle);
        m_rustHandle = nullptr;
        m_handleGeneration = 0;
        if (m_storeOwnershipSerial != 0) {
            ownership.noteClosed(m_storeOwnershipSerial, true);
            m_storeOwnershipSerial = 0;
        }
        Q_EMIT errorOccurred(tr("This account's network route could not be applied, so "
                                "Lightning will not connect it directly. Check the proxy "
                                "in Settings."));
        return false;
    }
    ensurePollTimer();
    return true;
}

void RustSdkMatrixClient::applySignInRoute(void *handle, const QString &homeserver,
                                           const QString &userId)
{
    namespace route = matrix::network_route;
    if (!handle)
        return;
    QByteArray json = route::rustJsonForSignIn(
        m_settings, m_settings ? m_settings->secretStore() : nullptr, homeserver, userId);
    const QString state = route::describeJson(json);
    const QString result = takeRustString(mx_rust_set_network_route(handle, json.constData()));
    json.fill('\0');
    json.clear();
    qCInfo(lcRust) << "sign-in network route" << "state=" << state
                   << "applied=" << result.isEmpty();
}

bool RustSdkMatrixClient::applyNetworkRoute(const QString &slug)
{
    namespace route = matrix::network_route;
    const QString owner = (m_storePathOverride.isEmpty() && m_settings)
        ? m_settings->accountOwningStoreSlug(slug)
        : QString();
    QByteArray json = route::rustJson(m_settings, m_settings ? m_settings->secretStore() : nullptr,
                                      owner);
    const QString state = route::describeJson(json);
    const QString result = takeRustString(mx_rust_set_network_route(m_rustHandle,
                                                                    json.constData()));
    json.fill('\0');
    json.clear();
    m_networkRouteActive = state != QLatin1String("none");
    qCInfo(lcRust) << "network route" << "slug=" << slug << "state=" << state
                   << "applied=" << result.isEmpty();
    return result.isEmpty();
}

void RustSdkMatrixClient::applyMediaStoreKey(const QString &slug)
{
    namespace msk = matrix::media_store_key;
    const QString owner = (m_storePathOverride.isEmpty() && m_settings)
        ? m_settings->accountOwningStoreSlug(slug)
        : QString();
    SecretStore *store = m_settings ? m_settings->secretStore() : nullptr;
    const bool storeIsSecure = store && store->isSecure();
    const bool mayCreate = m_settings
                           && msk::mayCreate(m_settings->secretBackendUnavailable(),
                                             m_settings->secretMissesAreInconclusive(),
                                             storeIsSecure);
    msk::Resolution resolution;
    if (!owner.isEmpty())
        resolution = msk::resolve(store, owner, mayCreate);
    // Media opened in encrypted rooms is cached only under a key a secure
    // keyring holds. macOS's QSettings fallback, a portable folder and the
    // fallback standing in for a missing keyring keep the key on the same
    // disk: the store still encrypts, admits none of it, and still holds what
    // the send queue keeps of sent attachments.
    const bool admitEncrypted = msk::admitsEncryptedRooms(resolution, storeIsSecure);
    m_mediaKeyOutcome = owner.isEmpty()
        ? QStringLiteral("no-saved-account")
        : (resolution.key.size() == msk::kKeyBytes ? QStringLiteral("key")
                                                   : QStringLiteral("no-key"));
    takeRustString(mx_rust_set_media_store_key(
        m_rustHandle,
        reinterpret_cast<const unsigned char *>(resolution.key.constData()),
        static_cast<unsigned long long>(resolution.key.size()),
        admitEncrypted ? 1 : 0));
    resolution.key.fill('\0');
    resolution.key.clear();
    qCInfo(lcRust) << "media store key" << "slug=" << slug
                   << "state=" << (owner.isEmpty() ? "no-saved-account"
                                                   : msk::describe(resolution))
                   << "admits_encrypted_rooms=" << admitEncrypted;
}

void RustSdkMatrixClient::refreshMediaKeepState()
{
    if (!m_rustHandle) {
        setMediaKeepState({});
        return;
    }
    const QString opened = takeRustString(mx_rust_media_store_state(m_rustHandle));
    QString state;
    if (opened == QLatin1String("encrypted_admits"))
        state = QStringLiteral("kept");
    else if (opened == QLatin1String("encrypted"))
        state = QStringLiteral("keptExceptEncrypted");
    else if (opened == QLatin1String("legacy"))
        state = QStringLiteral("keptExceptEncryptedPendingUpload");
    else if (opened == QLatin1String("memory"))
        state = m_mediaKeyOutcome == QLatin1String("no-saved-account")
            ? QStringLiteral("notKeptNewSignIn")
            : (m_mediaKeyOutcome == QLatin1String("key")
                   ? QStringLiteral("notKeptStore")
                   : QStringLiteral("notKeptKeyring"));
    setMediaKeepState(state);
}

void RustSdkMatrixClient::setMediaKeepState(const QString &state)
{
    if (state == m_mediaKeepState)
        return;
    m_mediaKeepState = state;
    Q_EMIT mediaKeepStateChanged();
}

namespace {
QThreadPool &rustRetirementPool()
{
    static QThreadPool *pool = [] {
        auto *p = new QThreadPool;
        p->setMaxThreadCount(2);
        p->setExpiryTimeout(-1);
        return p;
    }();
    return *pool;
}

namespace custody = lightning::accounts::custody;
using custody::accessTokenDigest;

class SettingsTokenStore final : public lightning::accounts::TokenStore
{
public:
    explicit SettingsTokenStore(SettingsManager *settings) : m_settings(settings) {}
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

struct RetiredSignIn {
    QString userId;
    QString deviceId;
    QByteArray expectedDigest;
    QString accessToken;
    QString refreshToken;
};
struct RetiredSignIns {
    QMutex mutex;
    QList<RetiredSignIn> waiting;
    QHash<QString, int> retiring;
};
RetiredSignIns &retiredSignIns()
{
    static RetiredSignIns *box = new RetiredSignIns;
    return *box;
}
}

void RustSdkMatrixClient::retireRustHandleAsync(void *handle,
                                                const QString &typingRoom)
{
    retireRustHandleAsync(handle, typingRoom, RetiringSignIn{}, {});
}

void RustSdkMatrixClient::retireRustHandleAsync(void *handle,
                                                const QString &typingRoom,
                                                const RetiringSignIn &signIn,
                                                QPointer<RustSdkMatrixClient> sink,
                                                quint64 storeOwnershipSerial,
                                                lightning::accounts::StoreOwnershipRegistry *storeRegistry)
{
    if (!handle)
        return;
    const bool tracked = signIn.persist && !signIn.userId.isEmpty();
    if (tracked) {
        QMutexLocker lock(&retiredSignIns().mutex);
        ++retiredSignIns().retiring[signIn.userId];
    }
    rustRetirementPool().start([handle, typingRoom, signIn, sink, tracked,
                                storeOwnershipSerial, storeRegistry] {
        QElapsedTimer teardown;
        teardown.start();
        if (!typingRoom.isEmpty()) {
            const QByteArray room = typingRoom.toUtf8();
            takeRustString(mx_rust_send_typing(handle, room.constData(), 0));
        }
        const QString shutdown = takeRustString(mx_rust_shutdown_tasks(handle));
        const qint64 shutdownMs = teardown.elapsed();
        bool handedBack = false;
        QString handedBackAccess;
        if (tracked) {
            const QString json = takeRustString(mx_rust_final_session_tokens(handle));
            const lightning::accounts::SessionTokenPair tokens =
                custody::parseFinalSessionTokens(json);
            if (custody::needsHandback(tokens.accessToken, signIn.accessDigest)) {
                QMutexLocker lock(&retiredSignIns().mutex);
                retiredSignIns().waiting.append(RetiredSignIn{
                    signIn.userId, signIn.deviceId, signIn.accessDigest,
                    tokens.accessToken, tokens.refreshToken});
                handedBack = true;
                handedBackAccess = tokens.accessToken;
            }
        }
        void *ledger = tracked ? mx_rust_token_ledger_retain(handle) : nullptr;
        teardown.restart();
        const QString released = takeRustString(mx_rust_destroy_and_report(handle));
        const qint64 destroyMs = teardown.elapsed();
        bool rotatedWhileClosing = false;
        if (ledger) {
            const QString json = takeRustString(mx_rust_token_ledger_latest(ledger));
            mx_rust_token_ledger_release(ledger);
            const lightning::accounts::SessionTokenPair latest =
                custody::parseFinalSessionTokens(json);
            const QByteArray latestDigest = custody::accessTokenDigest(latest.accessToken);
            if (!latest.accessToken.isEmpty()
                && latestDigest != custody::accessTokenDigest(handedBackAccess)
                && custody::needsHandback(latest.accessToken, signIn.accessDigest)) {
                rotatedWhileClosing = true;
                QMutexLocker lock(&retiredSignIns().mutex);
                QList<RetiredSignIn> &waiting = retiredSignIns().waiting;
                bool replaced = false;
                for (RetiredSignIn &entry : waiting) {
                    if (handedBack && entry.userId == signIn.userId
                        && entry.deviceId == signIn.deviceId
                        && entry.accessToken == handedBackAccess) {
                        entry.accessToken = latest.accessToken;
                        entry.refreshToken = latest.refreshToken;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) {
                    waiting.append(RetiredSignIn{
                        signIn.userId, signIn.deviceId,
                        handedBack ? custody::accessTokenDigest(handedBackAccess)
                                   : signIn.accessDigest,
                        latest.accessToken, latest.refreshToken});
                }
                handedBack = true;
            }
        }
        qCInfo(lcRust) << "rust client retired off the GUI thread"
                       << shutdown
                       << "shutdown_ms=" << shutdownMs
                       << "destroy_ms=" << destroyMs
                       << "teardown_total_ms=" << (shutdownMs + destroyMs)
                       << "sign_in_rotated_since_last_write=" << handedBack
                       << "rotated_while_closing=" << rotatedWhileClosing
                       << released;
        if (!released.startsWith(QLatin1String("store_closed=true")))
            qCWarning(lcRust) << "rust client retired without confirming its "
                                 "account store closed; a sign-out or reset that "
                                 "deletes the store now may not finish on Windows"
                              << released;
        if (storeRegistry && storeOwnershipSerial != 0)
            storeRegistry->noteClosed(storeOwnershipSerial,
                                      released.startsWith(QLatin1String("store_closed=true")));
        if (tracked) {
            QMutexLocker lock(&retiredSignIns().mutex);
            auto &retiring = retiredSignIns().retiring;
            if (--retiring[signIn.userId] <= 0)
                retiring.remove(signIn.userId);
        }
        if (handedBack && QCoreApplication::instance()) {
            QMetaObject::invokeMethod(QCoreApplication::instance(), [sink] {
                if (sink)
                    sink->persistRetiredSessionTokens();
            }, Qt::QueuedConnection);
        }
    });
}

void RustSdkMatrixClient::persistRetiredSessionTokens()
{
    persistRetiredSessionTokens(QString());
}

void RustSdkMatrixClient::persistRetiredSessionTokens(const QString &restoringUserId)
{
    QList<RetiredSignIn> waiting;
    {
        QMutexLocker lock(&retiredSignIns().mutex);
        waiting.swap(retiredSignIns().waiting);
    }
    SettingsTokenStore settingsStore(m_settings);
    lightning::accounts::TokenStore *store = m_settings ? &settingsStore : nullptr;
    for (const RetiredSignIn &signIn : std::as_const(waiting)) {
        const QString slug = matrix::app_data::safeUserSlug(signIn.userId);
        const bool restoring = !restoringUserId.isEmpty()
            && signIn.userId == restoringUserId;
        const bool sameDeviceRunning = store && !restoring && m_rustHandle && m_loggedIn
            && m_deviceId == signIn.deviceId
            && tokenOwnerForRunningSession() == signIn.userId;
        const custody::HandbackEntry entry{
            signIn.userId, signIn.deviceId, signIn.expectedDigest,
            lightning::accounts::SessionTokenPair{signIn.accessToken, signIn.refreshToken}};
        const custody::Handback outcome =
            custody::writeHandback(store, entry, sameDeviceRunning);
        switch (outcome) {
        case custody::Handback::SkippedNoStore:
            break;
        case custody::Handback::SkippedSameDeviceRunning:
            qCInfo(lcRust) << "a retired sign-in's last tokens were not kept: "
                              "the same device is running again"
                           << "slug=" << slug;
            break;
        case custody::Handback::SkippedRecordGoneOrOtherDevice:
            qCInfo(lcRust) << "a retired sign-in's last tokens were not kept: "
                              "its account record is gone or names another device"
                           << "slug=" << slug;
            break;
        case custody::Handback::SkippedKeyringChanged:
            qCInfo(lcRust) << "a retired sign-in's last tokens were not kept: "
                              "the keyring holds a different sign-in now"
                           << "slug=" << slug;
            break;
        case custody::Handback::Written:
        case custody::Handback::WriteFailed:
            qCInfo(lcRust) << "kept a token rotation the retired session made after "
                              "its last write"
                           << "slug=" << slug
                           << "ok=" << (outcome == custody::Handback::Written);
            break;
        }
    }
    if (m_liveAccountsLabs && !restoringUserId.isEmpty()) {
        const auto parked =
            lightning::accounts::HostSignInRetirements::process().takeUnsaved(restoringUserId);
        for (const auto &unsaved : parked) {
            const custody::HandbackEntry entry{
                unsaved.userId, unsaved.deviceId,
                custody::handbackExpectedDigest(store, unsaved.userId, unsaved.expectedDigest,
                                                unsaved.acceptedDigests),
                unsaved.tokens};
            const custody::Handback outcome = custody::writeHandback(store, entry, false);
            qCInfo(lcRust) << "wrote a background account's last sign-in before reading it"
                           << "slug=" << matrix::app_data::safeUserSlug(unsaved.userId)
                           << "ok=" << (outcome == custody::Handback::Written);
            if (outcome == custody::Handback::WriteFailed)
                lightning::accounts::HostSignInRetirements::process().parkUnsaved(unsaved);
        }
    }
}

void RustSdkMatrixClient::scheduleRotatedTokenWrite()
{
    const int shift = std::min(m_tokenWriteAttempts, 5);
    m_tokenWriteRetry.start(std::min(m_tokenWriteRetryMs << shift, 600000));
}

void RustSdkMatrixClient::retryRotatedTokenWrite()
{
    // A sign-out deletes the tokens; nothing may write them back.
    if (!m_rustHandle || !m_settings || m_lifecycle.signingOut())
        return;
    const QString owner = tokenOwnerForRunningSession();
    if (owner.isEmpty())
        return;
    const QString json = takeRustString(mx_rust_final_session_tokens(m_rustHandle));
    const lightning::accounts::SessionTokenPair tokens = custody::parseFinalSessionTokens(json);
    const QString &access = tokens.accessToken;
    if (!custody::retryWriteNeeded(access, m_persistedAccessDigest))
        return;
    ++m_tokenWriteAttempts;
    if (m_settings->updateSessionTokens(owner, access, tokens.refreshToken)) {
        m_persistedAccessDigest = accessTokenDigest(access);
        m_tokenWriteFailingFor.invalidate();
        qCInfo(lcRust) << "a rotated sign-in the keyring refused earlier is saved now"
                       << "attempts=" << m_tokenWriteAttempts
                       << "slug=" << matrix::app_data::safeUserSlug(owner);
        return;
    }
    if (m_tokenWriteFailingFor.isValid()
        && m_tokenWriteFailingFor.elapsed() >= m_tokenWriteGiveUpMs) {
        m_tokenWriteFailingFor.invalidate();
        qCWarning(lcRust) << "a rotated sign-in is still not saved after an hour "
                             "of retries; the keyring holds a used one, and the "
                             "next rotation tries again"
                          << "attempts=" << m_tokenWriteAttempts
                          << "slug=" << matrix::app_data::safeUserSlug(owner);
        return;
    }
    qCWarning(lcRust) << "a rotated sign-in is still not saved; the keyring holds "
                         "a used one until this succeeds"
                      << "attempts=" << m_tokenWriteAttempts
                      << "slug=" << matrix::app_data::safeUserSlug(owner);
    scheduleRotatedTokenWrite();
}

bool RustSdkMatrixClient::settleRetiringSignIn(const QString &userId)
{
    bool retiring = false;
    {
        QMutexLocker lock(&retiredSignIns().mutex);
        retiring = retiredSignIns().retiring.contains(userId);
    }
    if (m_liveAccountsLabs && lightning::accounts::HostSignInRetirements::process().contains(userId)) {
        qCInfo(lcRust) << "a background session of this account is still handing back its "
                          "sign-in"
                       << "slug=" << matrix::app_data::safeUserSlug(userId);
    }
    if (retiring) {
        qCInfo(lcRust) << "waiting for this account's previous session to "
                          "close before reading its sign-in"
                       << "slug=" << matrix::app_data::safeUserSlug(userId);
        if (!waitForRustRetirement(kStoreCloseBudgetMs))
            qCWarning(lcRust) << "the previous session did not close within the "
                                 "budget; its last tokens may be missed";
    }
    persistRetiredSessionTokens(userId);
    return retiring;
}

QString RustSdkMatrixClient::tokenOwnerForRunningSession() const
{
    if (!m_settings || m_userId.isEmpty() || m_deviceId.isEmpty())
        return {};
    QStringList candidates{ m_userId };
    matrix::app_data::AccountIdentity identity;
    if (matrix::app_data::resolveAccountIdentity(m_homeserver, m_userId, &identity))
        candidates << identity.userId;
    candidates << m_settings->userId();
    return custody::tokenOwnerFor(SettingsTokenStore(m_settings), candidates, m_deviceId);
}

bool RustSdkMatrixClient::waitForRustRetirement(int budgetMs)
{
    return rustRetirementPool().waitForDone(budgetMs);
}

void RustSdkMatrixClient::applyPerHandleSettings(bool adopted)
{
    if (!m_rustHandle)
        return;
    if (m_readReceiptPrivacy != 0 || adopted)
        takeRustString(mx_rust_set_receipt_privacy(m_rustHandle,
                                                   m_readReceiptPrivacy));
    if (m_threadsInLive)
        takeRustString(mx_rust_timeline_set_threads_in_live(m_rustHandle, 1));
    if (m_unthreadedReceipts)
        takeRustString(mx_rust_set_unthreaded_receipts(m_rustHandle, 1));
    if (m_indexAllHold != 0 || adopted)
        takeRustString(mx_rust_search_index_all_hold(m_rustHandle,
                                                     m_indexAllHold));
    if (m_callMediaCapable)
        takeRustString(mx_rust_calls_set_media_capable(
            m_rustHandle, static_cast<unsigned char>(1)));
}

bool RustSdkMatrixClient::adoptLiveSession(
    const lightning::accounts::LiveSessionTicket &ticket)
{
    if (!ticket.valid())
        return false;
    releaseRustHandle();
    m_freshLoginIdentity = {};
    m_freshBrowserRecord = {};
    m_loginSentBareUsername = false;
    m_restoredOffline = false;
    m_homeserver = ticket.homeserver;
    m_userId = ticket.userId;
    m_deviceId = ticket.deviceId;
    m_rustHandle = ticket.handle;
    m_storePath = ticket.storePath;
    m_storeOwnershipSerial = ticket.storeSerial;
    m_persistedAccessDigest = ticket.persistedDigest;
    // A new generation: every callback stamped by the previous foreground
    // handle is stale from here (CLAUDE.md §9).
    m_handleGeneration = m_lifecycle.beginSession();
    applyMediaStoreKey(ticket.slug);
    ensurePollTimer();

    const bool routed = m_settings && m_settings->networkRouteConfigured(ticket.userId);
    const QString refusal = routed
        ? QStringLiteral("error: routed account")
        : takeRustString(mx_rust_bg_promote(m_rustHandle));
    if (!refusal.isEmpty()) {
        qCWarning(lcRust) << "adoption refused by the handle; falling back to a"
                             " cold restore" << "acct=" << ticket.slug;
        releaseRustHandle();
        m_loggedIn = false;
        m_userId.clear();
        m_deviceId.clear();
        return false;
    }
    applyPerHandleSettings(true);
    reconcileAdoptedTokens();
    qCInfo(lcRust) << "adopted a live background session" << "acct=" << ticket.slug;
    setState(Connecting);
    return true;
}

void RustSdkMatrixClient::reconcileAdoptedTokens()
{
    const QString json = takeRustString(mx_rust_take_custody(m_rustHandle));
    const lightning::accounts::CustodySnapshot live =
        lightning::accounts::parseCustodyJson(json.toUtf8());
    if (!live.valid || !live.hasSession)
        return;
    const auto plan = lightning::accounts::custody::reconcileAdopted(
        live.accessToken, m_persistedAccessDigest, live.ledgerGeneration);
    bool saved = plan.answerSavedIfNoWrite;
    if (plan.write) {
        const QString owner = tokenOwnerForRunningSession();
        if (!owner.isEmpty() && m_settings && !m_lifecycle.signingOut()
            && m_settings->updateSessionTokens(owner, live.accessToken,
                                               live.refreshToken)) {
            m_persistedAccessDigest =
                lightning::accounts::custody::accessTokenDigest(live.accessToken);
            saved = true;
            qCInfo(lcRust) << "adoption: a rotation after the host's last write is saved"
                           << "slug=" << matrix::app_data::safeUserSlug(owner);
        } else {
            qCWarning(lcRust) << "adoption: a rotation after the host's last write is "
                                 "not saved yet; retrying";
            scheduleRotatedTokenWrite();
        }
    }
    if (plan.answerGeneration > 0)
        mx_rust_session_tokens_persisted(m_rustHandle, plan.answerGeneration,
                                         saved ? 1 : 0);
}

void RustSdkMatrixClient::releaseRustHandle()
{
    m_pollTimer.stop();
    clearTimelineInsertBatch();
    if (!m_rustHandle)
        return;

    void *retiring = m_rustHandle;
    const QString typingRoom = m_typingRoom;
    m_typingRoom.clear();
    RetiringSignIn signIn;
    signIn.userId = tokenOwnerForRunningSession();
    signIn.deviceId = m_deviceId;
    signIn.accessDigest = m_persistedAccessDigest;
    signIn.persist = !signIn.userId.isEmpty() && !m_lifecycle.signingOut()
        && !m_retireAsSignOut;
    const quint64 storeSerial = m_storeOwnershipSerial;
    lightning::accounts::StoreOwnershipRegistry *storeRegistry =
        storeSerial != 0 ? &storeOwnershipRegistry() : nullptr;
    if (storeRegistry)
        storeRegistry->noteReleasing(storeSerial);
    m_storeOwnershipSerial = 0;
    m_persistedAccessDigest.clear();
    m_tokenWriteRetry.stop();
    m_tokenWriteFailingFor.invalidate();
    m_rustHandle = nullptr;
    m_handleGeneration = 0;
    m_storePath.clear();
    m_mediaKeyOutcome.clear();
    m_networkRouteActive = false;
    setMediaKeepState({});
    m_timelineTracker.reset();
    m_threadTracker.reset();
    m_contextTracker.reset();
    m_recoveryReopens.clear();
    m_pagination.clear();

    retireRustHandleAsync(retiring, typingRoom, signIn,
                          QPointer<RustSdkMatrixClient>(this), storeSerial,
                          storeRegistry);
}

void RustSdkMatrixClient::login(const QString &homeserver,
                                const QString &user,
                                const QString &password)
{
    matrix::app_data::AccountIdentity identity;
    QString why;
    const bool resolved =
        matrix::app_data::resolveAccountIdentity(homeserver, user, &identity,
                                                 &why);
    if (!resolved || password.isEmpty()) {
        QString message;
        if (!resolved && homeserver.trimmed().isEmpty()) {
            message = tr("Enter a server, for example matrix.org.");
        } else if (!resolved && user.trimmed().isEmpty()) {
            message = tr("Enter your username.");
        } else if (!resolved
                   && why == QLatin1String("invalid homeserver or empty user")) {
            message = tr("That is not a server address. Enter one like matrix.org.");
        } else if (!resolved) {
            message = tr("That username is not a valid Matrix id. Use your "
                         "username, or the full @you:server form.");
        } else {
            message = tr("Enter your password.");
        }
        Q_EMIT loginFailed(message);
        return;
    }

    const bool bareUsername = !user.trimmed().startsWith(QLatin1Char('@'));
    const bool standInIdentity = m_settings && bareUsername
        && m_settings->canonicalUserIdForTypedIdentity(identity.userId).isEmpty();
    bool bareUsernameLocatedRecord = false;
    if (standInIdentity) {
        QList<matrix::app_data::AccountIdentity> saved;
        for (const QString &savedUserId : m_settings->savedAccountUserIds()) {
            matrix::app_data::AccountIdentity record;
            if (m_settings->resolveSavedIdentity(savedUserId, &record))
                saved.append(record);
        }
        bool ambiguous = false;
        const QString named = matrix::rust_session::savedAccountForBareUsername(
            user, identity.homeserver, saved, &ambiguous);
        if (ambiguous) {
            qCWarning(lcRust) << "login refused: several saved accounts on this "
                                 "address share the typed username";
            Q_EMIT loginFailed(matrix::rust_session::userMessage(
                matrix::rust_session::StoreBlockReason::AmbiguousStoreCandidates));
            return;
        }
        matrix::app_data::AccountIdentity record;
        if (!named.isEmpty() && m_settings->resolveSavedIdentity(named, &record)) {
            qCInfo(lcRust) << "bare username located a saved account"
                           << "typed_slug=" << identity.slug
                           << "account=" << record.slug
                           << "store_slug=" << record.effectiveStoreSlug();
            identity = record;
            bareUsernameLocatedRecord = true;
        }
    }
    const QString typedUserId = bareUsernameLocatedRecord
        ? matrix::rust_session::typedUserIdOnSavedServer(user, identity.userId)
        : identity.userId;
    if (bareUsernameLocatedRecord && typedUserId != identity.userId) {
        qCInfo(lcRust) << "bare username matched a saved account only by case;"
                          " it locates the store and never resumes its device"
                       << "account=" << identity.slug;
    }

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
            matrix::app_data::bindStoreSlug(
                &identity, m_settings->storeSlugFor(identity.userId));
        }
    }
    m_openingIdentity = identity;

    if (m_settings && m_settings->accountSlugConflicts(identity.userId)) {
        qCWarning(lcRust) << "login refused: account slug collision"
                          << "slug=" << identity.slug;
        Q_EMIT loginFailed(tr(
            "This account's local storage name collides with a different "
            "account already saved on this device. Remove that account "
            "first if you want to sign in with this one."));
        return;
    }

    if (!pathExistsOrIsLink(identity.rustStorePath) && m_settings
        && !loginOwnerUserId.isEmpty()) {
        matrix::app_data::AccountIdentity adoptTarget;
        if (m_settings->resolveSavedIdentity(loginOwnerUserId, &adoptTarget)) {
            auto adoptionRefusal = matrix::rust_session::StoreBlockReason::None;
            if (adoptDivergentStoreIfUnambiguous(&adoptTarget, &adoptionRefusal))
                matrix::app_data::bindStoreSlug(&identity,
                                                adoptTarget.effectiveStoreSlug());
            if (adoptionRefusal != matrix::rust_session::StoreBlockReason::None) {
                failWithBlockReason(adoptionRefusal, identity);
                return;
            }
        }
    }

    bool storeExists = pathExistsOrIsLink(identity.rustStorePath);
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
    matrix::app_data::AccountIdentity blockIdentity = identity;
    if (standInIdentity && !bareUsernameLocatedRecord && !storeOwner.isEmpty()
        && storeOwner != identity.userId) {
        matrix::app_data::AccountIdentity owner;
        const auto clean = [](const QString &path) {
            return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        };
        if (m_settings->resolveSavedIdentity(storeOwner, &owner)
            && clean(owner.rustStorePath) == clean(identity.rustStorePath)) {
            blockIdentity = owner;
        }
    }

    if (storeExists && !targetHasRecord) {
        // Moved aside, never deleted: the store may hold the only copy of
        // someone's room keys.
        // A handle this process retired a moment ago (a failed attempt, an
        // earlier account) may still hold the SQLite files open; on Windows
        // that alone makes the rename fail.
        if (!waitForRustRetirement(kStoreCloseBudgetMs))
            qCWarning(lcRust) << "a retiring client still held a store open "
                                 "after the close budget";
        m_storeClaimRefusalText.clear();
        const auto claim = storeOwnershipRegistry().claimForDeletion(identity.rustStorePath);
        if (claim != lightning::accounts::StoreOwnershipRegistry::ClaimVerdict::Granted) {
            reportStoreClaimRefused(int(claim), identity.effectiveStoreSlug(), "quarantine");
            m_storeClaimRefusalText = storeClaimRefusalText(identity.rustStorePath);
            setState(Error);
            Q_EMIT loginFailed(storeMoveFailureText());
            return;
        }
        const QString quarantined = matrix::app_data::quarantineRustStore(
            identity, &m_lastStoreMoveFailure);
        qCInfo(lcRust) << "quarantined unclaimed store before login"
                       << "slug=" << identity.effectiveStoreSlug()
                       << "moved=" << !quarantined.isEmpty();
        storeExists = pathExistsOrIsLink(identity.rustStorePath);
        if (storeExists) {
            setState(Error);
            Q_EMIT loginFailed(storeMoveFailureText());
            return;
        }
    }
    const QString targetSavedDeviceId = m_settings
        ? m_settings->accountRecord(recordUserId)
              .value(QStringLiteral("deviceId")).toString()
        : QString{};
    if (storeExists && targetHasSavedSession
        && (matrix::rust_session::revokedDeviceMayBeReplaced(
                recordUserId, targetSavedDeviceId, m_revokedUserId,
                m_revokedDeviceId)
            || labsHardProofAllowsReplace(recordUserId, targetSavedDeviceId))) {
        if (!moveRevokedDeviceStoreAside(identity)) {
            setState(Error);
            Q_EMIT loginFailed(storeMoveFailureText());
            return;
        }
        if (m_liveAccountsLabs && m_revocationProofs)
            m_revocationProofs->clearHard(recordUserId);
        storeExists = false;
    }
    m_freshLoginIdentity = storeExists ? matrix::app_data::AccountIdentity{}
                                       : identity;
    if (m_freshLoginIdentity.isValid() && m_settings)
        m_settings->recordPendingSignIn(m_freshLoginIdentity);
    m_freshBrowserRecord = {};
    if (matrix::rust_session::unreadableSecretBlocksLogin(
            storeExists, targetHasRecord, targetTokenReadable,
            m_settings && m_settings->secretBackendUnavailable(),
            m_settings && m_settings->secretMissesAreInconclusive())) {
        failWithBlockReason(
            matrix::rust_session::StoreBlockReason::SecretBackendUnavailable,
            blockIdentity);
        return;
    }

    matrix::app_data::AccountIdentity recordIdentity;
    const bool recordResolved = m_settings
        && m_settings->resolveSavedIdentity(recordUserId, &recordIdentity);
    const bool resumeSoftLoggedOut = storeExists && targetHasSavedSession
        && recordResolved
        && (matrix::rust_session::softLoggedOutDeviceMayResume(
                typedUserId, identity.homeserver, recordUserId,
                recordIdentity.homeserver, targetSavedDeviceId, m_softLogoutUserId,
                m_softLogoutHomeserver, m_softLogoutDeviceId)
            || labsSoftProofAllowsResume(typedUserId, identity.homeserver, recordUserId,
                                         recordIdentity.homeserver, targetSavedDeviceId));
    auto block = matrix::rust_session::passwordLoginBlockReason(
        identity, storeExists, targetHasSavedSession, targetSavedDeviceId);
    if (resumeSoftLoggedOut
        && block == matrix::rust_session::StoreBlockReason::ExistingStoreNeedsRestore) {
        block = matrix::rust_session::StoreBlockReason::None;
        qCInfo(lcRust) << "password sign-in resumes the soft-logged-out device"
                       << "slug=" << identity.slug;
    }
    bool resumeLostSignIn = false;
    if (block == matrix::rust_session::StoreBlockReason::MissingSessionMetadata
        && m_settings && storeExists && targetHasRecord && !targetTokenReadable
        && (m_settings->secretsWrittenHereForRecordedDevice(recordUserId)
            || m_settings->keyringItemsDeclinedForOtherInstall(recordUserId))) {
        resumeLostSignIn = recordResolved
            && matrix::rust_session::lostSignInDeviceMayResume(
                typedUserId, identity.homeserver, recordUserId,
                recordIdentity.homeserver, targetSavedDeviceId,
                m_settings->authTypeFor(recordUserId) != QLatin1String("password"));
        if (!resumeLostSignIn) {
            failWithBlockReason(
                matrix::rust_session::StoreBlockReason::KeyringLostSession,
                blockIdentity);
            return;
        }
        block = matrix::rust_session::StoreBlockReason::None;
        qCInfo(lcRust) << "password sign-in continues as the device whose saved "
                          "sign-in the keyring lost"
                       << "slug=" << identity.slug;
    }
    if (block == matrix::rust_session::StoreBlockReason::ExistingStoreNeedsRestore
        && targetHasSavedSession) {
        qCInfo(lcRust) << "login redirected to switch"
                       << "slug=" << identity.slug;
        Q_EMIT loginFailed(matrix::rust_session::userMessage(block));
        return;
    }
    if (block != matrix::rust_session::StoreBlockReason::None) {
        failWithBlockReason(block, blockIdentity);
        return;
    }

    if (!ensureRustHandleForIdentity(identity)) {
        m_freshLoginIdentity = {};
        m_freshBrowserRecord = {};
        setState(Error);
        Q_EMIT loginFailed(tr("Rust SDK backend could not be initialized."));
        return;
    }

    m_homeserver = identity.homeserver;
    m_userId.clear();
    m_deviceId.clear();
    m_loggedIn = false;
    m_resumeDeviceId = (resumeSoftLoggedOut || resumeLostSignIn)
        ? targetSavedDeviceId : QString();
    m_restoredOffline = false;
    m_ownIdentityKeyMismatchLogged = false;
    m_rooms.clear();
    m_memberListRecency.clear();
    m_roomOrder.clear();
    m_timelines.clear();
    m_forwardedEditIds.clear();
    resetNotificationProducerState();
    m_pendingSends.clear();
    setInitialSyncDone(false);
    Q_EMIT roomsChanged();
    setState(Connecting);

    const QByteArray hsBytes = identity.homeserver.toUtf8();
    const QByteArray userBytes =
        matrix::rust_session::passwordLoginIdentifier(user, identity).toUtf8();
    m_loginSentBareUsername = bareUsername;
    QByteArray passwordBytes = password.toUtf8();
    const QByteArray deviceBytes = m_resumeDeviceId.toUtf8();
    const QString result = takeRustString(mx_rust_login(m_rustHandle,
                                                        hsBytes.constData(),
                                                        userBytes.constData(),
                                                        passwordBytes.constData(),
                                                        deviceBytes.constData()));
    volatile char *raw = passwordBytes.data();
    for (int i = 0; i < passwordBytes.size(); ++i)
        raw[i] = 0;
    if (!result.isEmpty()) {
        m_resumeDeviceId.clear();
        setState(Error);
        Q_EMIT loginFailed(result.startsWith(QLatin1String("error: "))
                           ? result.mid(7)
                           : result);
    }
}

bool RustSdkMatrixClient::detachSession()
{
    stalltrace::Scope stallScope("account-detach");
    if (m_lifecycle.signingOut()) {
        qCWarning(lcRust) << "detach refused: sign-out still in flight";
        return false;
    }
    qCInfo(lcRust) << "detaching local session"
                   << "slug=" << matrix::app_data::safeUserSlug(m_userId);
    m_freshLoginIdentity = {};
    m_freshBrowserRecord = {};
    m_resumeDeviceId.clear();
    disarmResumedDeviceCheck();
    m_callSdpStore.clear();
    m_lifecycle.invalidate();
    releaseRustHandle();
    clearLocalState();
    Q_EMIT loggedOut();
    return true;
}

bool RustSdkMatrixClient::ensureOAuthBootstrapHandle(const QString &homeserver)
{
    releaseAuthHandle();
    m_authHandle = mx_rust_oauth_bootstrap_create();
    ++m_authHandleGeneration;
    if (!m_authHandle) {
        Q_EMIT errorOccurred(tr("Failed to create Rust SDK backend handle."));
        return false;
    }
    applySignInRoute(m_authHandle, homeserver);
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
    ++m_authHandleGeneration;
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
    if (m_oauthInFlight || m_ssoInFlight)
        return;

    const QString hs = homeserver.trimmed();
    if (hs.isEmpty()) {
        Q_EMIT authMethodsDiscovered(hs, false, false, false);
        return;
    }
    if (!ensureOAuthBootstrapHandle(hs)) {
        Q_EMIT authMethodsDiscovered(hs, false, false, false);
        return;
    }
    const QByteArray hsBytes = hs.toUtf8();
    const QString result =
        takeRustString(mx_rust_oauth_discover(m_authHandle, hsBytes.constData()));
    if (!result.isEmpty()) {
        Q_EMIT authMethodsDiscovered(hs, false, false, false);
    }
}

void RustSdkMatrixClient::beginOAuthLogin(const QString &homeserver)
{
    startOAuthAttempt(homeserver, false);
}

void RustSdkMatrixClient::beginOAuthSignUp(const QString &homeserver)
{
    startOAuthAttempt(homeserver, true);
}

void RustSdkMatrixClient::startOAuthAttempt(const QString &homeserver,
                                            bool createAccount)
{
    const QString hs = homeserver.trimmed();
    if (hs.isEmpty()) {
        Q_EMIT loginFailed(tr("A homeserver is required."));
        return;
    }
    if (m_oauthInFlight) {
        return;
    }

    if (!ensureOAuthBootstrapHandle(hs)) {
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
        mx_rust_oauth_begin(m_authHandle, hsBytes.constData(), redirectBytes.constData(),
                            createAccount ? 1 : 0));
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
    Q_EMIT loginFailed(tr("Sign-in was cancelled."));
}

void RustSdkMatrixClient::requestSsoProviders(const QString &homeserver)
{
    const QString hs = homeserver.trimmed();
    if (hs.isEmpty())
        return;
    releaseProvidersHandle();
    m_providersHandle = mx_rust_oauth_bootstrap_create();
    if (!m_providersHandle)
        return;
    applySignInRoute(m_providersHandle, hs);
    if (!m_pollTimer.isActive())
        m_pollTimer.start();
    const QByteArray hsBytes = hs.toUtf8();
    takeRustString(mx_rust_sso_providers(m_providersHandle, hsBytes.constData()));
}

void RustSdkMatrixClient::releaseProvidersHandle()
{
    if (!m_providersHandle)
        return;
    mx_rust_destroy(m_providersHandle);
    m_providersHandle = nullptr;
}

void RustSdkMatrixClient::drainProvidersEvents()
{
    if (!m_providersHandle)
        return;
    for (int i = 0; i < 16 && m_providersHandle; ++i) {
        const QByteArray raw = takeRustBytes(mx_rust_poll_event(m_providersHandle));
        if (raw.isEmpty())
            return;
        const QJsonObject event = QJsonDocument::fromJson(raw).object();
        if (event.value(QStringLiteral("type")).toString()
            != QLatin1String("sso_providers"))
            continue;
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
            entry.insert(QStringLiteral("brand"),
                         row.value(QStringLiteral("brand")).toString());
            providers.append(entry);
        }
        releaseProvidersHandle();
        Q_EMIT ssoProvidersReceived(
            event.value(QStringLiteral("homeserver")).toString(),
            event.value(QStringLiteral("sso")).toBool(), providers);
        return;
    }
}

void RustSdkMatrixClient::beginSsoLogin(const QString &homeserver,
                                        const QString &idpId)
{
    const QString hs = homeserver.trimmed();
    if (hs.isEmpty()) {
        Q_EMIT loginFailed(tr("A homeserver is required."));
        return;
    }
    if (m_ssoInFlight || m_oauthInFlight)
        return;

    if (!ensureOAuthBootstrapHandle(hs)) {
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

    const quint64 generation = m_authHandleGeneration;
    for (int i = 0; i < 64; ++i) {
        if (!m_authHandle || m_authHandleGeneration != generation)
            return;
        const QByteArray raw = takeRustBytes(mx_rust_poll_event(m_authHandle));
        if (raw.isEmpty())
            break;
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isObject())
            continue;
        const QJsonObject event = doc.object();
        const QString type = event.value(QStringLiteral("type")).toString();

        if (type == QLatin1String("auth_discovery")) {
            const QString homeserver = event.value(QStringLiteral("homeserver")).toString();
            const QJsonValue error = event.value(QStringLiteral("error"));
            QVariantMap details;
            details.insert(QStringLiteral("reachable"),
                           error.isNull() || error.isUndefined());
            details.insert(QStringLiteral("resolvedHomeserver"),
                           event.value(QStringLiteral("resolved_homeserver")).toString());
            details.insert(QStringLiteral("oauthCanCreate"),
                           event.value(QStringLiteral("oauth_can_create")).toBool());
            details.insert(QStringLiteral("accountManagementUrl"),
                           event.value(QStringLiteral("account_management_uri")).toString());
            if (!m_oauthInFlight && !m_ssoInFlight)
                releaseAuthHandle();
            Q_EMIT authDiscoveryDetails(homeserver, details);
            Q_EMIT authMethodsDiscovered(
                event.value(QStringLiteral("homeserver")).toString(),
                event.value(QStringLiteral("password")).toBool(),
                event.value(QStringLiteral("oauth")).toBool(),
                event.value(QStringLiteral("sso")).toBool());
            continue;
        }

        if (type == QLatin1String("oauth_url")) {
            const QString url = event.value(QStringLiteral("url")).toString();
            if (url.isEmpty())
                continue;
            const QUrl launch(url);
            if (launch.scheme() != QLatin1String("https")
                || launch.host().isEmpty()) {
                qCWarning(lcRust) << "refusing a non-https sign-in URL from "
                                     "homeserver discovery";
                Q_EMIT browserLaunchFailed();
            } else if (!lightning::urls::openExternally(launch)) {
                qCWarning(lcRust) << "browser sign-in: the browser could not be started";
                Q_EMIT browserLaunchFailed();
            } else {
                qCInfo(lcRust) << "browser sign-in: authorization page handed to the browser";
            }
            Q_EMIT oauthBrowserUrlReady(url);
            continue;
        }

        if (type == QLatin1String("oauth_ok")) {
            const QString userId = event.value(QStringLiteral("user_id")).toString();
            const QString deviceId = event.value(QStringLiteral("device_id")).toString();
            const QString clientId = event.value(QStringLiteral("client_id")).toString();
            const QString accessToken = event.value(QStringLiteral("access_token")).toString();
            const QString refreshToken = event.value(QStringLiteral("refresh_token")).toString();
            completeOAuthLogin(userId, deviceId, clientId, accessToken, refreshToken);
            continue;
        }

        if (type == QLatin1String("oauth_registration_retry")) {
            qCInfo(lcRust) << "oauth client registration retried without the "
                              "loopback port reason="
                           << event.value(QStringLiteral("reason")).toString();
            continue;
        }

        if (type == QLatin1String("oauth_failed")) {
            qCWarning(lcRust) << "browser sign-in failed flow=oauth stage="
                              << event.value(QStringLiteral("stage")).toString()
                              << "reason=" << event.value(QStringLiteral("reason")).toString();
            const QString message = event.value(QStringLiteral("message")).toString();
            endOAuthAttempt();
            releaseAuthHandle();
            Q_EMIT loginFailed(message.isEmpty()
                                   ? tr("The sign-in could not be completed.")
                                   : message);
            continue;
        }

        if (type == QLatin1String("sso_url")) {
            const QString url = event.value(QStringLiteral("url")).toString();
            if (url.isEmpty())
                continue;
            const QUrl launch(url);
            if (launch.scheme() != QLatin1String("https")
                || launch.host().isEmpty()) {
                qCWarning(lcRust) << "refusing a non-https sign-in URL from "
                                     "homeserver discovery";
                Q_EMIT browserLaunchFailed();
            } else if (!lightning::urls::openExternally(launch)) {
                qCWarning(lcRust) << "browser sign-in: the browser could not be started";
                Q_EMIT browserLaunchFailed();
            } else {
                qCInfo(lcRust) << "browser sign-in: SSO page handed to the browser";
            }
            Q_EMIT ssoBrowserUrlReady(url);
            continue;
        }

        if (type == QLatin1String("sso_ok")) {
            completeSsoLogin(
                event.value(QStringLiteral("user_id")).toString(),
                event.value(QStringLiteral("device_id")).toString(),
                event.value(QStringLiteral("access_token")).toString(),
                event.value(QStringLiteral("refresh_token")).toString());
            continue;
        }

        if (type == QLatin1String("sso_failed")) {
            qCWarning(lcRust) << "browser sign-in failed flow=sso stage="
                              << event.value(QStringLiteral("stage")).toString()
                              << "reason=" << event.value(QStringLiteral("reason")).toString();
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
    endOAuthAttempt();
    void *signInHandle = takeAuthHandle();

    bool restored = false;
    adoptBrowserSession(
        homeserver, userId, deviceId, clientId, accessToken, refreshToken,
        QStringLiteral("oauth"),
        [this, clientId, accessToken, refreshToken, &restored](
            const matrix::app_data::AccountIdentity &identity,
            const QString &deviceId) {
            const QByteArray hsBytes = identity.homeserver.toUtf8();
            const QByteArray userBytes = identity.userId.toUtf8();
            const QByteArray deviceBytes = deviceId.toUtf8();
            const QByteArray clientBytes = clientId.toUtf8();
            const QByteArray tokenBytes = accessToken.toUtf8();
            const QByteArray refreshBytes = refreshToken.toUtf8();
            const QString result = takeRustString(mx_rust_oauth_restore(
                m_rustHandle, hsBytes.constData(), userBytes.constData(),
                deviceBytes.constData(), clientBytes.constData(),
                tokenBytes.constData(), refreshBytes.constData()));
            restored = result.isEmpty();
            return result;
        });
    if (restored) {
        if (signInHandle)
            mx_rust_destroy(signInHandle);
        return;
    }
    revokeRefusedSignIn(signInHandle, homeserver, "oauth", clientId, userId,
                        deviceId, accessToken, refreshToken);
}

void RustSdkMatrixClient::completeSsoLogin(const QString &userId,
                                           const QString &deviceId,
                                           const QString &accessToken,
                                           const QString &refreshToken)
{
    const QString homeserver = m_ssoHomeserver;
    endSsoAttempt();
    void *signInHandle = takeAuthHandle();

    bool restored = false;
    adoptBrowserSession(
        homeserver, userId, deviceId, QString(), accessToken, refreshToken,
        QStringLiteral("sso"),
        [this, accessToken, refreshToken, &restored](
            const matrix::app_data::AccountIdentity &identity,
            const QString &deviceId) {
            const QByteArray hsBytes = identity.homeserver.toUtf8();
            const QByteArray userBytes = identity.userId.toUtf8();
            const QByteArray deviceBytes = deviceId.toUtf8();
            const QByteArray tokenBytes = accessToken.toUtf8();
            const QByteArray refreshBytes = refreshToken.toUtf8();
            const QString result = takeRustString(mx_rust_restore(
                m_rustHandle, hsBytes.constData(), userBytes.constData(),
                deviceBytes.constData(), tokenBytes.constData(),
                refreshBytes.constData()));
            restored = result.isEmpty();
            return result;
        });
    if (restored) {
        if (signInHandle)
            mx_rust_destroy(signInHandle);
        return;
    }
    revokeRefusedSignIn(signInHandle, homeserver, "sso", QString(), userId,
                        deviceId, accessToken, refreshToken);
}

void *RustSdkMatrixClient::takeAuthHandle()
{
    void *handle = m_authHandle;
    m_authHandle = nullptr;
    ++m_authHandleGeneration;
    return handle;
}

void RustSdkMatrixClient::revokeRefusedSignIn(void *signInHandle,
                                              const QString &homeserver,
                                              const char *flow,
                                              const QString &clientId,
                                              const QString &userId,
                                              const QString &deviceId,
                                              const QString &accessToken,
                                              const QString &refreshToken)
{
    if (!signInHandle)
        return;
    if (accessToken.isEmpty() || homeserver.trimmed().isEmpty()) {
        mx_rust_destroy(signInHandle);
        return;
    }
    releaseRevokeHandle();
    applySignInRoute(signInHandle, homeserver, userId);
    qCInfo(lcRust) << "browser sign-in not kept on this device; ending the "
                      "session the server issued for it"
                   << "flow=" << flow;
    const QByteArray hsBytes = homeserver.toUtf8();
    const QByteArray clientBytes = clientId.toUtf8();
    const QByteArray userBytes = userId.toUtf8();
    const QByteArray deviceBytes = deviceId.toUtf8();
    const QByteArray tokenBytes = accessToken.toUtf8();
    const QByteArray refreshBytes = refreshToken.toUtf8();
    const QString result = takeRustString(mx_rust_revoke_refused_sign_in(
        signInHandle, hsBytes.constData(), flow, clientBytes.constData(),
        userBytes.constData(), deviceBytes.constData(), tokenBytes.constData(),
        refreshBytes.constData()));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "the refused sign-in's session could not be ended"
                          << "category=start_failed";
        mx_rust_destroy(signInHandle);
        return;
    }
    m_revokeHandle = signInHandle;
    if (!m_pollTimer.isActive())
        m_pollTimer.start();
}

void RustSdkMatrixClient::releaseRevokeHandle()
{
    if (!m_revokeHandle)
        return;
    mx_rust_destroy(m_revokeHandle);
    m_revokeHandle = nullptr;
}

void RustSdkMatrixClient::drainRevokeEvents()
{
    for (int i = 0; i < 16 && m_revokeHandle; ++i) {
        const QByteArray raw = takeRustBytes(mx_rust_poll_event(m_revokeHandle));
        if (raw.isEmpty())
            return;
        const QJsonObject event = QJsonDocument::fromJson(raw).object();
        if (event.value(QStringLiteral("type")).toString()
            != QLatin1String("refused_sign_in_revoked"))
            continue;
        qCInfo(lcRust) << "the refused sign-in's session at the server"
                       << "category="
                       << event.value(QStringLiteral("category")).toString();
        releaseRevokeHandle();
        return;
    }
}

bool RustSdkMatrixClient::startAdoptedSessionCheck(
    const matrix::app_data::AccountIdentity &identity, const QString &userId,
    const QString &deviceId, const QString &accessToken)
{
    releaseWhoamiHandle();
    m_whoamiHandle = mx_rust_oauth_bootstrap_create();
    if (!m_whoamiHandle) {
        m_adoptedSessionsChecked.insert(userId);
        return restoreSession();
    }
    applySignInRoute(m_whoamiHandle, identity.homeserver, userId);
    m_adoptedSessionCheck = {userId, deviceId, identity.homeserver};
    qCInfo(lcRust) << "checking an adopted sign-in with the server before using it"
                   << "slug=" << identity.slug;
    setState(Connecting);
    if (!m_pollTimer.isActive())
        m_pollTimer.start();
    const QByteArray hsBytes = identity.homeserver.toUtf8();
    const QByteArray tokenBytes = accessToken.toUtf8();
    const QString result = takeRustString(
        mx_rust_whoami_probe(m_whoamiHandle, hsBytes.constData(), tokenBytes.constData()));
    if (!result.isEmpty()) {
        releaseWhoamiHandle();
        m_adoptedSessionCheck = {};
        m_adoptedSessionsChecked.insert(userId);
        return restoreSession();
    }
    return true;
}

void RustSdkMatrixClient::releaseWhoamiHandle()
{
    if (!m_whoamiHandle)
        return;
    mx_rust_destroy(m_whoamiHandle);
    m_whoamiHandle = nullptr;
}

void RustSdkMatrixClient::drainWhoamiEvents()
{
    for (int i = 0; i < 16 && m_whoamiHandle; ++i) {
        const QByteArray raw = takeRustBytes(mx_rust_poll_event(m_whoamiHandle));
        if (raw.isEmpty())
            return;
        const QJsonObject event = QJsonDocument::fromJson(raw).object();
        if (event.value(QStringLiteral("type")).toString()
            != QLatin1String("whoami_probe"))
            continue;
        releaseWhoamiHandle();
        const AdoptedSessionCheck check = std::exchange(m_adoptedSessionCheck, {});
        if (!m_settings || check.userId.isEmpty()
            || m_settings->userId() != check.userId)
            return;
        const QString result = event.value(QStringLiteral("result")).toString();
        const QString answeredDevice = event.value(QStringLiteral("device_id")).toString();
        const QString slug = matrix::app_data::safeUserSlug(check.userId);
        if (result == QLatin1String("device") && answeredDevice == check.deviceId) {
            m_settings->confirmAdoptedKeyringItems(check.userId);
            qCInfo(lcRust) << "the adopted sign-in is this device's; it is this "
                              "install's own now"
                           << "slug=" << slug;
        } else if (result == QLatin1String("device")) {
            // The token is another install's device. Using it would run this
            // store on that device, and this install's sign-out would end it.
            // Only while nothing newer has happened to this account during the
            // check (up to its 15 s bound): a session of it running or opening
            // owns it now, and a newer sign-in is no longer the adopted copy
            // (declineAdoptedKeyringItems() checks that and the device).
            const bool sessionOfIt = m_userId == check.userId
                && (m_loggedIn || m_rustHandle || m_oauthInFlight || m_ssoInFlight);
            if (sessionOfIt
                || !m_settings->declineAdoptedKeyringItems(check.userId,
                                                           check.deviceId)) {
                qCInfo(lcRust) << "the adoption check's answer came after the "
                                  "account moved on; nothing changed"
                               << "slug=" << slug;
                return;
            }
            qCWarning(lcRust) << "the adopted sign-in belongs to another install's "
                                 "device; this install's copy is removed and the "
                                 "shared item kept"
                              << "slug=" << slug;
            Q_EMIT localSessionBlocked(QStringLiteral("keyring_other_install"),
                                       check.userId, check.homeserver);
            setState(Error);
            Q_EMIT loginFailed(tr("This sign-in belongs to another copy of "
                                  "Lightning on this computer. Sign in again."));
            return;
        } else {
            m_adoptedSessionsChecked.insert(check.userId);
            qCInfo(lcRust) << "the adopted sign-in could not be checked; "
                              "restoring it unverified"
                           << "result=" << result << "slug=" << slug;
        }
        if (!restoreSession()) {
            setState(Error);
            Q_EMIT loginFailed(tr("Lightning could not open this account."));
        }
        return;
    }
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

    if (userId.isEmpty() || deviceId.isEmpty() || accessToken.isEmpty()) {
        Q_EMIT loginFailed(tr("The server finished the sign-in but did not say "
                              "which account or device it was for. Nothing was "
                              "saved on this device."));
        return;
    }

    matrix::app_data::AccountIdentity identity;
    if (!matrix::app_data::resolveAccountIdentity(homeserver, userId, &identity)) {
        Q_EMIT loginFailed(tr("The server signed you in as %1, an account name "
                              "Lightning can't store on this device. Nothing was "
                              "saved here.").arg(userId));
        return;
    }

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
    bool storeExists = QFileInfo::exists(storePath);

    if (m_settings && m_settings->accountSlugConflicts(identity.userId)) {
        qCWarning(lcRust) << "Browser sign-in refused: account slug collision"
                          << "slug=" << identity.slug;
        Q_EMIT loginFailed(tr(
            "This account's local storage name collides with a different "
            "account already saved on this device. Remove that account "
            "first if you want to sign in with this one."));
        return;
    }

    if (storeExists && !hasSavedSession && m_settings) {
        bool ambiguous = false;
        const QString variant =
            m_settings->canonicalUserIdForTypedIdentity(identity.userId, &ambiguous);
        const QString owner =
            m_settings->accountOwningStoreSlug(identity.effectiveStoreSlug());
        if (ambiguous || !variant.isEmpty() || !owner.isEmpty()) {
            qCWarning(lcRust) << "Browser sign-in refused: the store belongs to a "
                                 "saved account"
                              << "slug=" << identity.effectiveStoreSlug();
            Q_EMIT loginFailed(tr(
                "Another account saved on this device, with the same name in "
                "different upper and lower case, uses this account's storage. "
                "Remove that account first if you want to sign in with this one."));
            return;
        }
        if (!waitForRustRetirement(kStoreCloseBudgetMs))
            qCWarning(lcRust) << "a retiring client still held a store open "
                                 "after the close budget";
        m_storeClaimRefusalText.clear();
        const auto claim = storeOwnershipRegistry().claimForDeletion(identity.rustStorePath);
        if (claim != lightning::accounts::StoreOwnershipRegistry::ClaimVerdict::Granted) {
            reportStoreClaimRefused(int(claim), identity.effectiveStoreSlug(), "quarantine");
            m_storeClaimRefusalText = storeClaimRefusalText(identity.rustStorePath);
            setState(Error);
            Q_EMIT loginFailed(storeMoveFailureText());
            return;
        }
        const QString moved = matrix::app_data::quarantineRustStore(
            identity, &m_lastStoreMoveFailure);
        qCInfo(lcRust) << "quarantined unclaimed store before browser sign-in"
                       << "slug=" << identity.effectiveStoreSlug()
                       << "moved=" << !moved.isEmpty();
        storeExists = QFileInfo::exists(storePath);
        if (storeExists) {
            setState(Error);
            Q_EMIT loginFailed(storeMoveFailureText());
            return;
        }
    }

    if (storeExists && hasSavedSession
        && (matrix::rust_session::revokedDeviceMayBeReplaced(
                identity.userId, savedDeviceId, m_revokedUserId,
                m_revokedDeviceId)
            || labsHardProofAllowsReplace(identity.userId, savedDeviceId))) {
        if (!moveRevokedDeviceStoreAside(identity)) {
            setState(Error);
            Q_EMIT loginFailed(storeMoveFailureText());
            return;
        }
        storeExists = false;
    }

    // A device the authorization server just created must never be attached to
    // a store belonging to a different device.
    const auto reason = matrix::rust_session::oauthLoginBlockReason(
        identity, storeExists, hasSavedSession, savedDeviceId, deviceId);
    if (reason != matrix::rust_session::StoreBlockReason::None) {
        qCWarning(lcRust) << "Browser sign-in refused"
                          << "authType=" << authType
                          << "slug=" << identity.effectiveStoreSlug()
                          << "reason=" << matrix::rust_session::diagnosticName(reason);
        failWithBlockReason(reason, identity);
        return;
    }

    // Must be set before anything can emit login_failed: the failure handler
    // keys its store-slug rewrite and local-reset prompt on m_openingIdentity,
    // and a stale value would target a different account's store.
    m_openingIdentity = identity;

    m_freshLoginIdentity = storeExists ? matrix::app_data::AccountIdentity{}
                                       : identity;
    m_freshBrowserRecord = {};

    if (!ensureRustHandleForIdentity(identity)) {
        m_freshLoginIdentity = {};
        m_freshBrowserRecord = {};
        setState(Error);
        Q_EMIT loginFailed(tr("You signed in, but Lightning could not open this "
                              "account's storage on this device. Nothing was "
                              "saved here."));
        return;
    }

    m_homeserver = identity.homeserver;
    m_userId = identity.userId;
    m_deviceId = deviceId;
    m_loggedIn = false;
    m_restoredOffline = false;
    m_ownIdentityKeyMismatchLogged = false;
    m_rooms.clear();
    m_memberListRecency.clear();
    m_roomOrder.clear();
    m_timelines.clear();
    m_forwardedEditIds.clear();
    resetNotificationProducerState();
    m_pendingSends.clear();
    setInitialSyncDone(false);
    Q_EMIT roomsChanged();
    setState(Connecting);

    if (m_settings) {
        m_settings->saveSession(identity.homeserver, identity.userId, deviceId,
                                accessToken, refreshToken,
                                authType, clientId);
        m_persistedAccessDigest = accessTokenDigest(accessToken);
        if (!m_settings->hasSavedAccount(identity.userId)) {
            rollBackFailedAttempt(std::exchange(m_freshLoginIdentity, {}), {});
            setState(Error);
            Q_EMIT loginFailed(tr("You signed in, but Lightning could not save this "
                                  "account on this device, so it did not open it."));
            return;
        }
        if (!hasSavedSession)
            m_freshBrowserRecord = identity;
        m_settings->setSyncToken({});
        recordStoreLocation(identity);
    }

    const QString result = restore(identity, deviceId);
    if (!result.isEmpty()) {
        const auto store = matrix::app_data::AccountIdentity(
            std::exchange(m_freshLoginIdentity, {}));
        const auto record = matrix::app_data::AccountIdentity(
            std::exchange(m_freshBrowserRecord, {}));
        const bool complete = rollBackFailedAttempt(store, record);
        setState(Error);
        Q_EMIT loginFailed(browserRestoreFailureText(
            complete, result.startsWith(QLatin1String("error: ")) ? result.mid(7)
                                                                  : result));
    }
}

bool RustSdkMatrixClient::rollBackFailedAttempt(
    const matrix::app_data::AccountIdentity &store,
    const matrix::app_data::AccountIdentity &record)
{
    bool complete = true;
    if (store.isValid()) {
        releaseRustHandle();
        if (!waitForRustRetirement(kStoreCloseBudgetMs))
            qCWarning(lcRust) << "a failed attempt's store was still open after the "
                                 "close budget";
        const auto claim = storeOwnershipRegistry().claimForDeletion(store.rustStorePath);
        if (claim != lightning::accounts::StoreOwnershipRegistry::ClaimVerdict::Granted) {
            reportStoreClaimRefused(int(claim), store.effectiveStoreSlug(), "attempt_cleanup");
            complete = false;
        } else {
            const auto removed = matrix::app_data::removeAttemptRustStore(store);
            complete = removed.failed == 0;
            qCInfo(lcRust) << "cleaned fresh store after failed login"
                           << "slug=" << store.effectiveStoreSlug()
                           << "deleted=" << removed.deleted
                           << "failed=" << removed.failed;
        }
    }
    if (record.isValid()) {
        if (m_settings && m_settings->hasSavedAccount(record.userId)) {
            bool matched = false;
            if (!clearPersistedAccount(record, &matched))
                complete = false;
            qCInfo(lcRust) << "rolled back the record of a failed browser sign-in"
                           << "slug=" << record.slug << "matched=" << matched;
        }
        m_userId.clear();
        m_deviceId.clear();
        m_homeserver.clear();
    }
    return complete;
}

void RustSdkMatrixClient::armResumedDeviceCheck()
{
    m_resumedCheckUserId = m_userId;
    m_resumedCheckDeviceId = m_deviceId;
    m_resumedCheckAttempts = 0;
    scheduleResumedDeviceCheck();
}

void RustSdkMatrixClient::scheduleResumedDeviceCheck()
{
    const quint64 generation = ++m_resumedCheckGeneration;
    QTimer::singleShot(kResumedDeviceCheckDelayMs, this, [this, generation] {
        if (generation != m_resumedCheckGeneration
            || !resumedDeviceCheckIsFor(m_userId, m_deviceId) || !m_rustHandle) {
            return;
        }
        ++m_resumedCheckAttempts;
        const QString error =
            takeRustString(mx_rust_check_resumed_device_key(m_rustHandle));
        if (!error.isEmpty()) {
            if (m_resumedCheckAttempts < kResumedDeviceCheckAttempts)
                scheduleResumedDeviceCheck();
            else
                disarmResumedDeviceCheck();
        }
    });
}

void RustSdkMatrixClient::disarmResumedDeviceCheck()
{
    ++m_resumedCheckGeneration;
    m_resumedCheckUserId.clear();
    m_resumedCheckDeviceId.clear();
    m_resumedCheckAttempts = 0;
}

bool RustSdkMatrixClient::resumedDeviceCheckIsFor(const QString &userId,
                                                  const QString &deviceId) const
{
    return m_loggedIn && !m_resumedCheckDeviceId.isEmpty()
           && userId == m_resumedCheckUserId && deviceId == m_resumedCheckDeviceId
           && userId == m_userId && deviceId == m_deviceId;
}

QString RustSdkMatrixClient::storeMoveFailureText() const
{
    if (!m_storeClaimRefusalText.isEmpty())
        return m_storeClaimRefusalText;
    if (m_lastStoreMoveFailure == matrix::app_data::QuarantineFailure::InUse)
        return tr("This account's old local data could not be moved aside "
                  "because it is still in use. Close any other Lightning "
                  "window (check the system tray) or program using it, wait a "
                  "moment, and try again.");
    return tr("An unusable local store for this account could not be moved "
              "aside. Check filesystem permissions and try again.");
}

bool RustSdkMatrixClient::moveRevokedDeviceStoreAside(
    const matrix::app_data::AccountIdentity &identity)
{
    if (!m_storePath.isEmpty()
        && QFileInfo(m_storePath).absoluteFilePath()
               == QFileInfo(identity.rustStorePath).absoluteFilePath()) {
        releaseRustHandle();
    }
    if (!waitForRustRetirement(kStoreCloseBudgetMs))
        qCWarning(lcRust) << "the revoked device's store was still open after "
                             "the close budget";
    m_storeClaimRefusalText.clear();
    const auto claim = storeOwnershipRegistry().claimForDeletion(identity.rustStorePath);
    if (claim != lightning::accounts::StoreOwnershipRegistry::ClaimVerdict::Granted) {
        reportStoreClaimRefused(int(claim), identity.effectiveStoreSlug(), "move_aside");
        m_storeClaimRefusalText = storeClaimRefusalText(identity.rustStorePath);
        return false;
    }
    const QString moved = matrix::app_data::quarantineRustStore(
        identity, &m_lastStoreMoveFailure);
    qCInfo(lcRust) << "moved the revoked device's store aside; signing in as "
                      "a new device"
                   << "slug=" << identity.effectiveStoreSlug()
                   << "moved=" << !moved.isEmpty();
    if (pathExistsOrIsLink(identity.rustStorePath))
        return false;
    if (m_revocationProofs) {
        m_revocationProofs->clearHard(m_revokedUserId);
        if (identity.userId != m_revokedUserId)
            m_revocationProofs->clearHard(identity.userId);
    }
    m_revokedUserId.clear();
    m_revokedDeviceId.clear();
    return true;
}

QString RustSdkMatrixClient::browserRestoreFailureText(bool rolledBack,
                                                       const QString &detail) const
{
    if (rolledBack)
        return tr("You signed in, but Lightning could not open this account on "
                  "this device, so nothing was kept. You can try again. (%1)")
            .arg(detail);
    return tr("You signed in, but Lightning could not open this account on this "
              "device, and could not remove all of what it had started to save "
              "here. Check the log before trying again. (%1)")
        .arg(detail);
}

bool RustSdkMatrixClient::restoreSession()
{
    stalltrace::Scope stallScope("session-restore");
    lightning::switchtrace::StepScope switchTraceS7(lightning::switchtrace::Step::S7);
    m_freshLoginIdentity = {};
    m_freshBrowserRecord = {};
    if (!m_settings || !m_settings->hasSession())
        return false;

    matrix::app_data::AccountIdentity identity;
    if (!m_settings->resolveSavedIdentity(m_settings->userId(), &identity)
        && !matrix::app_data::resolveAccountIdentity(
            m_settings->homeserverUrl(), m_settings->userId(), &identity)) {
        matrix::app_data::AccountIdentity unresolved;
        unresolved.userId = m_settings->userId();
        unresolved.homeserver = m_settings->homeserverUrl();
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
    settleRetiringSignIn(userId);

    const bool oauthAccount = m_settings->isOAuthAccount(userId);
    QString accessToken;
    QString refreshToken;
    QString oauthClientId;
    const auto readSignIn = [&]() -> const char * {
        accessToken = m_settings->accessToken();
        refreshToken.clear();
        oauthClientId.clear();
        if (accessToken.isEmpty())
            return nullptr;
        refreshToken = m_settings->refreshToken();
        if (refreshToken.isEmpty() && m_settings->secretBackendUnavailable())
            return "refresh_token";
        if (oauthAccount) {
            oauthClientId = m_settings->oauthClientIdFor(userId);
            if (oauthClientId.isEmpty() && m_settings->secretBackendUnavailable())
                return "oauth_client_id";
        }
        return nullptr;
    };
    const auto refuseUnreadable = [&](const char *secret) {
        qCWarning(lcRust) << "restore stopped: the keyring did not answer for "
                             "part of this sign-in; nothing was changed"
                          << "secret=" << secret << "slug=" << identity.slug;
        const auto reason =
            matrix::rust_session::StoreBlockReason::SecretBackendUnavailable;
        Q_EMIT localSessionBlocked(matrix::rust_session::diagnosticName(reason),
                                   identity.userId, identity.homeserver);
        if (!m_loggedIn)
            setState(Error);
        Q_EMIT loginFailed(matrix::rust_session::userMessage(reason));
    };
    if (const char *unreadable = readSignIn()) {
        refuseUnreadable(unreadable);
        return false;
    }
    if (hs.isEmpty() || userId.isEmpty() || accessToken.isEmpty())
        return false;

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

    if (m_settings->keyringItemsAdoptedUnverified(userId)
        && !m_adoptedSessionsChecked.contains(userId)) {
        if (!oauthAccount)
            return startAdoptedSessionCheck(identity, userId, deviceId, accessToken);
        m_adoptedSessionsChecked.insert(userId);
        qCInfo(lcRust) << "an adopted OAuth sign-in is not checked: its access "
                          "token has usually expired, so the server's answer "
                          "would decide nothing; restoring it unverified"
                       << "slug=" << identity.slug;
    }

    if (!ensureRustHandleForIdentity(identity)) {
        setState(Error);
        Q_EMIT loginFailed(tr("Rust SDK backend could not be initialized."));
        return false;
    }
    if (settleRetiringSignIn(userId)) {
        if (const char *unreadable = readSignIn()) {
            refuseUnreadable(unreadable);
            return false;
        }
        if (accessToken.isEmpty())
            return false;
    }
    m_persistedAccessDigest = accessTokenDigest(accessToken);

    m_homeserver = hs;
    m_userId = userId;
    m_deviceId = deviceId;
    m_loggedIn = false;
    m_restoredOffline = false;
    m_ownIdentityKeyMismatchLogged = false;
    m_rooms.clear();
    m_memberListRecency.clear();
    m_roomOrder.clear();
    m_timelines.clear();
    m_forwardedEditIds.clear();
    resetNotificationProducerState();
    m_pendingSends.clear();
    setInitialSyncDone(false);
    Q_EMIT roomsChanged();
    setState(Connecting);

    const QByteArray hsBytes = hs.toUtf8();
    const QByteArray userBytes = userId.toUtf8();
    const QByteArray deviceBytes = deviceId.toUtf8();
    const QByteArray tokenBytes = accessToken.toUtf8();
    const QByteArray refreshBytes = refreshToken.toUtf8();

    QString result;
    if (oauthAccount) {
        const QByteArray clientBytes = oauthClientId.toUtf8();
        if (clientBytes.isEmpty()) {
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
    m_restoredOffline = false;
    m_ownIdentityKeyMismatchLogged = false;
    m_rooms.clear();
    m_memberListRecency.clear();
    m_roomOrder.clear();
    m_timelines.clear();
    m_forwardedEditIds.clear();
    resetNotificationProducerState();
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
    if (!identity || !m_storePathOverride.isEmpty() || !identity->isValid())
        return false;

    QStringList candidates =
        matrix::app_data::findCaseVariantStoreSlugs(*identity);
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

    if (!waitForRustRetirement(kStoreCloseBudgetMs))
        qCWarning(lcRust) << "store close did not finish within the budget;"
                          << "deleting anyway";

    const auto claim = storeOwnershipRegistry().claimForDeletion(storePath);
    if (claim != lightning::accounts::StoreOwnershipRegistry::ClaimVerdict::Granted) {
        reportStoreClaimRefused(int(claim), QFileInfo(QFileInfo(storePath).path()).fileName(),
                                "reset");
        return false;
    }
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
    matrix::app_data::AccountIdentity identity = requested;
    if (m_settings && identity.isValid()) {
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
    if (!waitForRustRetirement(kStoreCloseBudgetMs))
        qCWarning(lcRust) << "store close did not finish within the budget;"
                          << "quarantining anyway";
    const auto claim = storeOwnershipRegistry().claimForDeletion(identity.rustStorePath);
    if (claim != lightning::accounts::StoreOwnershipRegistry::ClaimVerdict::Granted) {
        reportStoreClaimRefused(int(claim), identity.slug, "local_reset");
        if (message)
            *message = storeClaimRefusalText(identity.rustStorePath);
        return false;
    }
    bool matchedRecord = false;
    const bool sessionOk = clearPersistedAccount(identity, &matchedRecord);
    const auto files = matrix::app_data::quarantineAccountRustState(identity);
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
    if (m_settings && m_signOutIdentity.isValid())
        m_settings->recordPendingSignOut(m_signOutIdentity);
    qCInfo(lcRust) << "rust sign-out started"
                   << "slug=" << m_signOutIdentity.slug
                   << "device_known=" << !m_signOutDeviceId.isEmpty();
    m_lifecycle.beginSignOut(m_handleGeneration);

    if (m_rustHandle && !m_typingRoom.isEmpty()) {
        const QByteArray room = m_typingRoom.toUtf8();
        takeRustString(mx_rust_send_typing(m_rustHandle, room.constData(), 0));
        m_typingRoom.clear();
    }

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
    m_contextTracker.reset();
    m_recoveryReopens.clear();
    m_pagination.clear();
    stopSync();

    if (m_rustHandle) {
        const bool oauthSession = m_settings
            && m_settings->isOAuthAccount(m_signOutIdentity.userId);
        if (oauthSession) {
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
    lightning::switchtrace::stepBegin(lightning::switchtrace::Step::S9);
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
    return {};
}

QUrl RustSdkMatrixClient::mediaThumbnailUrl(const QString &, int, int,
                                            bool) const
{
    return {};
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
    const QString realRoom = isThreadTimelineId(roomId)
        ? threadTimelineRoomId(roomId) : roomId;
    if (!timelineActiveFor(roomId)) {
        refuseUntilTimelineReady("redactEvent");
        Q_EMIT redactionFinished(realRoom, eventId, false,
                                 QStringLiteral("not_ready"));
        return;
    }
    const QByteArray roomBytes = realRoom.toUtf8();
    const QByteArray targetBytes = eventId.toUtf8();
    const QByteArray reasonBytes = reason.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_redact(
        m_rustHandle, roomBytes.constData(), targetBytes.constData(),
        reasonBytes.constData()));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "redaction refused before sending:" << result;
        const QString category = QStringLiteral("rejected");
        Q_EMIT errorOccurred(redactionFailureText(category));
        Q_EMIT redactionFinished(realRoom, eventId, false, category);
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

void RustSdkMatrixClient::setKeepMediaOnDevice(bool enabled)
{
    const QString result = takeRustString(
        mx_rust_set_keep_media(enabled ? 1 : 0));
    if (!result.isEmpty())
        qCWarning(lcRust) << "keep-media command rejected";
}

void RustSdkMatrixClient::setReadReceiptPrivacy(int mode)
{
    m_readReceiptPrivacy = (mode < 0 || mode > 2) ? 0 : mode;
    if (!m_rustHandle)
        return;
    const QString result = takeRustString(
        mx_rust_set_receipt_privacy(m_rustHandle, m_readReceiptPrivacy));
    if (!result.isEmpty())
        qCWarning(lcRust) << "receipt privacy command rejected";
}

void RustSdkMatrixClient::setThreadRepliesInLiveTimeline(bool inLive)
{
    m_threadsInLive = inLive;
    if (!m_rustHandle)
        return;
    const QString result = takeRustString(
        mx_rust_timeline_set_threads_in_live(m_rustHandle, inLive ? 1 : 0));
    if (!result.isEmpty())
        qCWarning(lcRust) << "thread style command rejected";
}

void RustSdkMatrixClient::setUnthreadedReadReceipts(bool on)
{
    m_unthreadedReceipts = on;
    if (!m_rustHandle)
        return;
    const QString result = takeRustString(
        mx_rust_set_unthreaded_receipts(m_rustHandle, on ? 1 : 0));
    if (!result.isEmpty())
        qCWarning(lcRust) << "receipt thread command rejected";
}

void RustSdkMatrixClient::sendReadReceipt(const QString &roomId, const QString &eventId)
{
    if (roomId.isEmpty() || eventId.isEmpty()
        || m_lastReceiptSent.value(roomId) == eventId)
        return;
    const auto reportFailed = [this, roomId, eventId] {
        QMetaObject::invokeMethod(this, [this, roomId, eventId] {
            Q_EMIT readReceiptFailed(roomId, eventId);
        }, Qt::QueuedConnection);
    };
    if (!m_rustHandle) {
        reportFailed();
        return;
    }
    m_lastReceiptSent.insert(roomId, eventId);
    const QByteArray room = roomId.toUtf8();
    const QByteArray event = eventId.toUtf8();
    const QString result = takeRustString(mx_rust_send_read_receipt(
        m_rustHandle, room.constData(), event.constData()));
    if (!result.isEmpty()) {
        m_lastReceiptSent.remove(roomId);
        qCWarning(lcRust) << "read receipt command rejected";
        reportFailed();
    }
}

void RustSdkMatrixClient::setFullyReadMarker(const QString &roomId,
                                             const QString &eventId)
{
    if (!m_rustHandle || roomId.isEmpty() || eventId.isEmpty())
        return;
    const QByteArray room = roomId.toUtf8();
    const QByteArray event = eventId.toUtf8();
    const QString result = takeRustString(mx_rust_set_fully_read_marker(
        m_rustHandle, room.constData(), event.constData()));
    if (!result.isEmpty())
        qCWarning(lcRust) << "fully-read marker command rejected";
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

void RustSdkMatrixClient::setRoomLowPriority(const QString &roomId, bool lowPriority)
{
    if (!m_rustHandle || roomId.isEmpty()) return;
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_set_room_low_priority(
        m_rustHandle, room.constData(), lowPriority ? 1 : 0));
    if (!result.isEmpty()) qCWarning(lcRust) << "low-priority command rejected";
}

void RustSdkMatrixClient::markRoomRead(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty()) return;
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_mark_room_read(
        m_rustHandle, room.constData()));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "mark-room-read command rejected";
        Q_EMIT markRoomReadFailed(roomId);
    }
}

void RustSdkMatrixClient::setRoomNotificationMode(const QString &roomId, int mode)
{
    if (!m_rustHandle || roomId.isEmpty() || mode < 0 || mode > 2) return;
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_set_room_notification_mode(
        m_rustHandle, room.constData(), mode));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "notification-mode command rejected";
        Q_EMIT roomNotificationModeWriteFailed(roomId);
    }
}

void RustSdkMatrixClient::requestThreadParticipants(const QString &roomId,
                                                    const QString &rootEventId)
{
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
    const QByteArray target = roomId.toUtf8();
    const QByteArray path = localPath.toUtf8();
    const QString result = takeRustString(mx_rust_set_room_banner(
        m_rustHandle, target.constData(), path.constData(), opId));
    if (!result.isEmpty()) {
        Q_EMIT roomBannerSet(opId, roomId, false, QString(),
                             QStringLiteral("rejected"));
    }
}

void RustSdkMatrixClient::fetchRoomBackground(const QString &roomId,
                                              quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty())
        return;
    const QByteArray target = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_fetch_room_background(m_rustHandle, target.constData(), opId));
    if (!result.isEmpty())
        qCWarning(lcRust) << "room background read rejected op=" << opId
                          << "reason=" << result.left(120);
}

namespace {
QString backgroundRefusalCategory(const QString &result)
{
    QString reason = result;
    if (reason.startsWith(QLatin1String("error: ")))
        reason = reason.mid(7);
    static const QStringList tokens = {
        QStringLiteral("read_failed"), QStringLiteral("not_a_file"),
        QStringLiteral("file_too_large"), QStringLiteral("invalid_content"),
    };
    if (tokens.contains(reason))
        return reason;
    if (reason.contains(QLatin1String("not-joined")))
        return QStringLiteral("not_joined");
    if (reason.contains(QLatin1String("no active Matrix session")))
        return QStringLiteral("signed_out");
    return QStringLiteral("rejected");
}
}

void RustSdkMatrixClient::setRoomBackground(const QString &roomId,
                                            const QString &localPath,
                                            const QString &contentJson,
                                            quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty())
        return;
    const QByteArray target = roomId.toUtf8();
    const QByteArray path = localPath.toUtf8();
    const QByteArray content = contentJson.toUtf8();
    const QString result = takeRustString(mx_rust_set_room_background(
        m_rustHandle, target.constData(), path.constData(),
        content.constData(), opId));
    if (!result.isEmpty()) {
        const QString category = backgroundRefusalCategory(result);
        qCWarning(lcRust) << "room background write rejected op=" << opId
                          << "stage=ffi category=" << category
                          << "reason=" << result.left(120);
        Q_EMIT roomBackgroundSet(opId, roomId, false, QVariantMap(), category);
    }
}

void RustSdkMatrixClient::readSyncedSettings(quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT syncedSettingsRead(opId, false, QStringLiteral("signed_out"),
                                  QStringLiteral("unreadable"), QString(), -1);
        return;
    }
    const QString result =
        takeRustString(mx_rust_settings_sync_read(m_rustHandle, opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "settings sync read rejected op=" << opId
                          << "reason=" << result.left(120);
        Q_EMIT syncedSettingsRead(opId, false, result.left(40),
                                  QStringLiteral("unreadable"), QString(), -1);
    }
}

void RustSdkMatrixClient::writeSyncedSettings(int kind,
                                              const QString &contentJson,
                                              quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT syncedSettingsWritten(opId, kind, false,
                                     QStringLiteral("signed_out"));
        return;
    }
    const QByteArray content = contentJson.toUtf8();
    const QString result = takeRustString(mx_rust_settings_sync_write(
        m_rustHandle, static_cast<unsigned int>(qBound(0, kind, 2)),
        content.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "settings sync write rejected op=" << opId
                          << "kind=" << kind << "reason=" << result.left(120);
        Q_EMIT syncedSettingsWritten(opId, kind, false, result.left(40));
    }
}

void RustSdkMatrixClient::readPersonalBackgrounds(const QString &scope,
                                                  quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT personalBackgroundsRead(opId, scope, QVariantList(),
                                       QStringList{ scope }, -1);
        return;
    }
    const QByteArray target = scope.toUtf8();
    const QString result = takeRustString(mx_rust_personal_backgrounds_read(
        m_rustHandle, target.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "personal background read rejected op=" << opId
                          << "reason=" << result.left(120);
        Q_EMIT personalBackgroundsRead(opId, scope, QVariantList(),
                                       QStringList{ scope }, -1);
    }
}

void RustSdkMatrixClient::downloadPersonalBackground(const QString &scope,
                                                     const QString &expectedId,
                                                     quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT personalBackgroundDownloaded(opId, scope, false, QVariantMap(),
                                            QByteArray(),
                                            QStringLiteral("signed_out"));
        return;
    }
    const QByteArray target = scope.toUtf8();
    const QByteArray expected = expectedId.toUtf8();
    const QString result = takeRustString(mx_rust_personal_background_download(
        m_rustHandle, target.constData(), expected.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "personal background download rejected op=" << opId
                          << "reason=" << result.left(120);
        Q_EMIT personalBackgroundDownloaded(opId, scope, false, QVariantMap(),
                                            QByteArray(),
                                            backgroundRefusalCategory(result));
    }
}

void RustSdkMatrixClient::writePersonalBackground(const QString &scope,
                                                  int mode,
                                                  const QString &localPath,
                                                  const QString &requestedJson,
                                                  quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT personalBackgroundWritten(opId, scope, false, QVariantMap(),
                                         QStringLiteral("signed_out"));
        return;
    }
    const QByteArray target = scope.toUtf8();
    const QByteArray path = localPath.toUtf8();
    const QByteArray requested = requestedJson.toUtf8();
    const QString result = takeRustString(mx_rust_personal_background_write(
        m_rustHandle, target.constData(),
        static_cast<unsigned int>(qBound(0, mode, 3)), path.constData(),
        requested.constData(), opId));
    if (!result.isEmpty()) {
        const QString category = backgroundRefusalCategory(result);
        qCWarning(lcRust) << "personal background write rejected op=" << opId
                          << "stage=ffi category=" << category
                          << "reason=" << result.left(120);
        Q_EMIT personalBackgroundWritten(opId, scope, false, QVariantMap(),
                                         category);
    }
}

void RustSdkMatrixClient::clearAllPersonalBackgrounds(
    const QStringList &knownRooms, quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT personalBackgroundsCleared(opId, false, 0, 0, 0, false);
        return;
    }
    const QByteArray known =
        QJsonDocument(QJsonArray::fromStringList(knownRooms))
            .toJson(QJsonDocument::Compact);
    const QString result = takeRustString(mx_rust_personal_backgrounds_clear_all(
        m_rustHandle, known.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "personal background removal rejected op=" << opId
                          << "reason=" << result.left(120);
        Q_EMIT personalBackgroundsCleared(opId, false, 0, 0, 0, false);
    }
}

void RustSdkMatrixClient::fetchStickerPacks(const QString &roomId, quint64 opId,
                                            bool deep)
{
    if (!m_loggedIn || !m_rustHandle)
        return;
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_stickers_fetch_packs(m_rustHandle, room.constData(), deep, opId));
    if (!result.isEmpty()) {
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

void RustSdkMatrixClient::uploadStickerToRoomPack(
    const QString &roomId, const QString &stateKey, const QString &ns,
    const QString &shortcode, const QString &body, const QString &localPath,
    const QString &reuseUrl, quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty()) {
        Q_EMIT stickerPackAddFinished(opId, false, QStringLiteral("rejected"), QString());
        return;
    }
    const QByteArray room = roomId.toUtf8(), key = stateKey.toUtf8(),
                     dialect = ns.toUtf8(), code = shortcode.toUtf8(),
                     alt = body.toUtf8(), path = localPath.toUtf8(),
                     reuse = reuseUrl.toUtf8();
    const QString result = takeRustString(mx_rust_stickers_upload_to_room_pack(
        m_rustHandle, room.constData(), key.constData(), dialect.constData(),
        code.constData(), alt.constData(), path.constData(), reuse.constData(),
        opId));
    if (!result.isEmpty())
        Q_EMIT stickerPackAddFinished(opId, false, QStringLiteral("rejected"), QString());
}

void RustSdkMatrixClient::migrateUserStickerPack(quint64 opId, bool startNew)
{
    if (!m_loggedIn || !m_rustHandle) {
        Q_EMIT stickerPackMigrated(opId, false, QStringLiteral("rejected"), {});
        return;
    }
    const QString result =
        takeRustString(mx_rust_stickers_migrate_user_pack(m_rustHandle, startNew, opId));
    if (!result.isEmpty())
        Q_EMIT stickerPackMigrated(opId, false, QStringLiteral("rejected"), {});
}

void RustSdkMatrixClient::createStickerRoomPack(const QString &roomId,
                                                const QString &stateKey,
                                                const QString &name,
                                                const QString &usage,
                                                quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty()) {
        Q_EMIT stickerPackEditFinished(opId, false, QStringLiteral("rejected"),
                                       QString());
        return;
    }
    const QByteArray room = roomId.toUtf8();
    const QByteArray key = stateKey.toUtf8();
    const QByteArray label = name.toUtf8();
    const QByteArray kind = usage.toUtf8();
    const QString result = takeRustString(mx_rust_stickers_create_room_pack(
        m_rustHandle, room.constData(), key.constData(), label.constData(),
        kind.constData(), opId));
    if (!result.isEmpty())
        Q_EMIT stickerPackEditFinished(opId, false, QStringLiteral("rejected"),
                                       QString());
}

void RustSdkMatrixClient::addStickerToRoomPack(
    const QString &roomId, const QString &stateKey, const QString &ns,
    const QString &shortcode,
    const QString &url, const QString &body, const QString &mimetype,
    quint64 width, quint64 height, quint64 size, quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty() || url.isEmpty())
        return;
    const QByteArray room = roomId.toUtf8();
    const QByteArray key = stateKey.toUtf8();
    const QByteArray dialect = ns.toUtf8();
    const QByteArray code = shortcode.toUtf8();
    const QByteArray mxc = url.toUtf8();
    const QByteArray alt = body.toUtf8();
    const QByteArray mime = mimetype.toUtf8();
    const QString result = takeRustString(mx_rust_stickers_add_to_room_pack(
        m_rustHandle, room.constData(), key.constData(), dialect.constData(),
        code.constData(), mxc.constData(), alt.constData(), mime.constData(),
        width, height, size, opId));
    if (!result.isEmpty()) {
        Q_EMIT stickerPackAddFinished(opId, false, QStringLiteral("rejected"),
                                      QString());
    }
}

void RustSdkMatrixClient::setStickerRoomPackEnabled(const QString &roomId,
                                                    const QString &stateKey,
                                                    const QString &ns,
                                                    bool enabled, quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty())
        return;
    const QByteArray room = roomId.toUtf8();
    const QByteArray key = stateKey.toUtf8();
    const QByteArray dialect = ns.toUtf8();
    const QString result =
        takeRustString(mx_rust_stickers_set_room_pack_enabled(
            m_rustHandle, room.constData(), key.constData(),
            dialect.constData(), enabled, opId));
    if (!result.isEmpty()) {
        Q_EMIT stickerPackRoomsSet(opId, false, QStringLiteral("rejected"),
                                   roomId, stateKey, enabled,
                                   QStringLiteral("none"));
    }
}

void RustSdkMatrixClient::uploadStickerToUserPack(
    const QString &shortcode, const QString &body, const QString &localPath,
    const QString &usage, quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || localPath.isEmpty()) {
        Q_EMIT stickerPackAddFinished(opId, false, QStringLiteral("rejected"),
                                      QString());
        return;
    }
    const QByteArray code = shortcode.toUtf8();
    const QByteArray alt = body.toUtf8();
    const QByteArray path = localPath.toUtf8();
    const QByteArray use = usage.toUtf8();
    const QString result = takeRustString(mx_rust_stickers_upload_to_user_pack(
        m_rustHandle, code.constData(), alt.constData(), path.constData(),
        use.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "sticker upload rejected";
        Q_EMIT stickerPackAddFinished(opId, false, QStringLiteral("rejected"),
                                      QString());
    }
}

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
    const QString &roomId, const QString &stateKey, const QString &ns,
    const QString &action,
    const QString &argA, const QString &argB, quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || action.isEmpty())
        return;
    const QByteArray room = roomId.toUtf8();
    const QByteArray key = stateKey.toUtf8();
    const QByteArray dialect = ns.toUtf8();
    const QByteArray verb = action.toUtf8();
    const QByteArray a = argA.toUtf8();
    const QByteArray b = argB.toUtf8();
    const QString result = takeRustString(mx_rust_stickers_edit_pack(
        m_rustHandle, room.constData(), key.constData(), dialect.constData(),
        verb.constData(), a.constData(), b.constData(), opId));
    if (!result.isEmpty()) {
        Q_EMIT stickerPackEditFinished(opId, false, QStringLiteral("rejected"),
                                       QString());
    }
}

void RustSdkMatrixClient::addStickerToUserPack(
    const QString &shortcode, const QString &url, const QString &body,
    const QString &mimetype, quint64 width, quint64 height, quint64 size,
    const QString &usage, quint64 opId)
{
    if (!m_loggedIn || !m_rustHandle || url.isEmpty()) {
        Q_EMIT stickerPackAddFinished(opId, false, QStringLiteral("rejected"),
                                      QString());
        return;
    }
    const QByteArray code = shortcode.toUtf8();
    const QByteArray mxc = url.toUtf8();
    const QByteArray alt = body.toUtf8();
    const QByteArray mime = mimetype.toUtf8();
    const QByteArray use = usage.toUtf8();
    const QString result = takeRustString(mx_rust_stickers_add_to_user_pack(
        m_rustHandle, code.constData(), mxc.constData(), alt.constData(),
        mime.constData(), width, height, size, use.constData(), opId));
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
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "notification-mode clear rejected";
        Q_EMIT roomNotificationModeWriteFailed(roomId);
    }
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

void RustSdkMatrixClient::requestAllRoomNotificationModes()
{
    if (!m_rustHandle) return;
    const QString result = takeRustString(
        mx_rust_refresh_room_notification_modes(m_rustHandle));
    if (!result.isEmpty())
        qCWarning(lcRust) << "notification-mode bulk query rejected";
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
constexpr unsigned short kPaginationBatch = 20;
constexpr unsigned short kPaginationMaxBatch = 180;
}

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
        state.failed = false;
        state.failureTransient = false;
        Q_EMIT paginationStateChanged(roomId);
    }
    if (result.isEmpty())
        armPaginationPoll(++m_paginationPollChain, kPaginationPollTicks, 0);
}

// A cached page is ready in milliseconds; at the 100 ms poll a multi-page walk
// (saved-position return, viewport fill) would wait one poll per page.
void RustSdkMatrixClient::armPaginationPoll(quint64 chain, int ticksLeft,
                                            int quietTicks)
{
    QTimer::singleShot(kPaginationPollMs, this, [this, chain, ticksLeft, quietTicks] {
        if (chain != m_paginationPollChain || !m_rustHandle)
            return;
        pollRustEvents();
        if (chain != m_paginationPollChain || ticksLeft <= 1)
            return;
        bool loading = false;
        for (const PaginationState &state : std::as_const(m_pagination))
            loading = loading || state.loading;
        const int quiet = loading ? 0 : quietTicks + 1;
        if (quiet < kPaginationPollQuietTicks)
            armPaginationPoll(chain, ticksLeft - 1, quiet);
    });
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
    clearContextTimelineState();
    const QString leavingRequested = m_timelineTracker.requestedRoom();
    const QString leavingActive = m_timelineTracker.activeRoom();
    m_timelineTracker.request(roomId);
    retireRoomTimelineMirror(leavingRequested);
    retireRoomTimelineMirror(leavingActive);
    m_pagination.insert(roomId, PaginationState{});
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

namespace {
qint64 monotonicMs()
{
    static QElapsedTimer clock = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return clock.elapsed();
}
}

bool RustSdkMatrixClient::roomTimelineStillOpen(const QString &roomId) const
{
    return m_timelineTracker.activeRoom() == roomId
        || m_timelineTracker.requestedRoom() == roomId;
}

bool RustSdkMatrixClient::threadTimelineStillOpen(const QString &timelineId) const
{
    return m_threadTracker.activeRoom() == timelineId
        || m_threadTracker.requestedRoom() == timelineId;
}

bool RustSdkMatrixClient::contextTimelineStillOpen(const QString &timelineId) const
{
    return m_contextTracker.activeRoom() == timelineId
        || m_contextTracker.requestedRoom() == timelineId;
}

bool RustSdkMatrixClient::admitRecoveryReopen(const QString &timelineId,
                                              const char *reason,
                                              std::function<bool()> stillOpen,
                                              std::function<void()> reopen)
{
    using Budget = matrix::RecoveryReopenBudget;
    qint64 retryInMs = 0;
    switch (m_recoveryReopens.request(timelineId, monotonicMs(), &retryInMs)) {
    case Budget::Verdict::Reopen:
        return true;
    case Budget::Verdict::AlreadyDeferred:
        return false;
    case Budget::Verdict::Defer:
        break;
    }
    qCWarning(lcRust) << "timeline recovery reopen capped reason=" << reason
                      << "timeline=" << matrix::e2ee::redactId(timelineId)
                      << "reopens=" << Budget::kMaxReopens
                      << "window_ms=" << m_recoveryReopens.windowMs()
                      << "deferred_ms=" << retryInMs;
    QTimer::singleShot(retryInMs, this,
                       [this, timelineId, reason, stillOpen = std::move(stillOpen),
                        reopen = std::move(reopen)] {
        if (!m_recoveryReopens.takeDeferred(timelineId, monotonicMs()))
            return;
        if (!stillOpen()) {
            qCInfo(lcRust) << "timeline recovery reopen deferred skipped: "
                              "no longer open reason=" << reason << "timeline="
                           << matrix::e2ee::redactId(timelineId);
            return;
        }
        qCInfo(lcRust) << "timeline recovery reopen deferred ran reason="
                       << reason << "timeline="
                       << matrix::e2ee::redactId(timelineId);
        reopen();
    });
    return false;
}

bool RustSdkMatrixClient::reloadRoomTimelineAtLive(const QString &roomId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty())
        return false;
    clearThreadTimelineState();
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
    const QString targetRoom = isThreadTimelineId(roomId)
        ? threadTimelineRoomId(roomId)
        : roomId;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 last = m_lastDecryptionRetryMs.value(targetRoom, 0);
    if (now - last < 2000) {
        qCDebug(lcE2ee) << "retry coalesced" << "room="
                        << matrix::e2ee::redactId(targetRoom);
        return;
    }
    const QByteArray roomBytes = targetRoom.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_retry_decryption(
        m_rustHandle, roomBytes.constData()));
    if (!result.isEmpty()) {
        qCInfo(lcRust) << "decryption retry NOT dispatched";
        qCDebug(lcE2ee) << "retry refused" << "room="
                        << matrix::e2ee::redactId(targetRoom)
                        << "reason=" << result;
        return;
    }
    m_lastDecryptionRetryMs.insert(targetRoom, now);
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
                                         const QString &rootEventId,
                                         quint64 opId)
{
    if (!m_rustHandle || roomId.isEmpty() || rootEventId.isEmpty()) {
        Q_EMIT threadMarkReadFailed(roomId, rootEventId, opId);
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray rootBytes = rootEventId.toUtf8();
    const QString error = takeRustString(mx_rust_thread_mark_read(
        m_rustHandle, roomBytes.constData(), rootBytes.constData(), opId));
    if (!error.isEmpty())
        Q_EMIT threadMarkReadFailed(roomId, rootEventId, opId);
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
        return;
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
                     nonBlankOr(obj.value(QStringLiteral("root_sender_name")).toString(),
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
                     nonBlankOr(obj.value(QStringLiteral("latest_sender_name")).toString(),
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

void RustSdkMatrixClient::retireRoomTimelineMirror(const QString &roomId)
{
    if (roomId.isEmpty())
        return;
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
    const auto notificationFacts = qScopeGuard([this, roomId, rootId, outcome] {
        emitDiffNotificationFacts(roomId, rootId, outcome);
    });

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
        qCWarning(lcRust) << "thread invalid diff rejected"
                          << "op=" << event.value(QStringLiteral("op")).toString()
                          << "mirror_size=" << mirror.size();
        if (admitRecoveryReopen(
                timelineId, "invalid_diff",
                [this, timelineId] { return threadTimelineStillOpen(timelineId); },
                [this, roomId, rootId] { openThread(roomId, rootId); }))
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
    m_timelines.remove(timelineId);
    m_pagination.remove(timelineId);
    if (m_threadTracker.activeRoom() == timelineId
        || m_threadTracker.requestedRoom() == timelineId)
        m_threadTracker.reset();
    qCInfo(lcRust) << "thread subscription stopped";
}

void RustSdkMatrixClient::openEventContext(const QString &roomId,
                                           const QString &eventId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty() || eventId.isEmpty()) {
        Q_EMIT eventContextFailed(roomId, eventId, QStringLiteral("not_ready"));
        return;
    }
    clearContextTimelineState();
    m_contextTracker.request(contextTimelineId(roomId, eventId));
    qCInfo(lcRust) << "event context open target="
                   << matrix::e2ee::redactId(eventId);
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray eventBytes = eventId.toUtf8();
    const QString result = takeRustString(mx_rust_context_open(
        m_rustHandle, roomBytes.constData(), eventBytes.constData()));
    if (!result.isEmpty()) {
        m_contextTracker.reset();
        Q_EMIT eventContextFailed(roomId, eventId,
                                  QStringLiteral("dispatch_failed"));
    }
}

void RustSdkMatrixClient::closeEventContext()
{
    if (m_rustHandle && m_contextTracker.hasActiveTimeline())
        qCInfo(lcRust) << "event context close";
    if (m_rustHandle)
        takeRustString(mx_rust_context_close(m_rustHandle));
    clearContextTimelineState();
}

void RustSdkMatrixClient::paginateEventContext(const QString &roomId,
                                               const QString &eventId,
                                               bool forward)
{
    const QString timelineId = contextTimelineId(roomId, eventId);
    if (!m_rustHandle || !m_contextTracker.readyForPagination(timelineId)) {
        Q_EMIT eventContextPagination(roomId, eventId, forward,
                                      QStringLiteral("failed"), false);
        return;
    }
    const QByteArray roomBytes = roomId.toUtf8();
    const QByteArray eventBytes = eventId.toUtf8();
    qCInfo(lcRust) << "event context paginate forward=" << forward;
    const QString result = takeRustString(mx_rust_context_paginate(
        m_rustHandle, roomBytes.constData(), eventBytes.constData(), forward));
    if (!result.isEmpty())
        Q_EMIT eventContextPagination(roomId, eventId, forward,
                                      QStringLiteral("failed"), false);
}

void RustSdkMatrixClient::clearContextTimelineState()
{
    for (const QString &timelineId : { m_contextTracker.requestedRoom(),
                                       m_contextTracker.activeRoom() }) {
        if (!timelineId.isEmpty())
            m_timelines.remove(timelineId);
    }
    m_contextTracker.reset();
}

void RustSdkMatrixClient::handleContextReset(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QString eventId = event.value(QStringLiteral("event_id")).toString();
    const QString timelineId = contextTimelineId(roomId, eventId);
    const auto generation = static_cast<quint64>(
        event.value(QStringLiteral("context_generation")).toDouble(0));
    if (!m_contextTracker.adoptReset(timelineId, generation)) {
        qCInfo(lcRust) << "context stale reset ignored generation="
                       << generation;
        return;
    }
    const QJsonArray items = event.value(QStringLiteral("items")).toArray();
    m_timelines[timelineId] =
        matrix::rust_timeline::eventsFromItemArray(items, timelineId);
    qCInfo(lcRust) << "event context started generation=" << generation
                   << "items=" << m_timelines[timelineId].size();
    Q_EMIT timelineReset(timelineId);
}

void RustSdkMatrixClient::handleContextDiff(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QString eventId = event.value(QStringLiteral("event_id")).toString();
    const QString timelineId = contextTimelineId(roomId, eventId);
    const auto generation = static_cast<quint64>(
        event.value(QStringLiteral("context_generation")).toDouble(0));
    if (!m_contextTracker.accepts(timelineId, generation)) {
        qCInfo(lcRust) << "context stale diff ignored generation="
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
        qCWarning(lcRust) << "context invalid diff rejected mirror_size="
                          << mirror.size();
        if (admitRecoveryReopen(
                timelineId, "invalid_diff",
                [this, timelineId] { return contextTimelineStillOpen(timelineId); },
                [this, roomId, eventId] { openEventContext(roomId, eventId); }))
            openEventContext(roomId, eventId);
        break;
    }
}

void RustSdkMatrixClient::handleContextPagination(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QString eventId = event.value(QStringLiteral("event_id")).toString();
    const QString timelineId = contextTimelineId(roomId, eventId);
    const auto generation = static_cast<quint64>(
        event.value(QStringLiteral("context_generation")).toDouble(0));
    if (!m_contextTracker.accepts(timelineId, generation)) {
        qCInfo(lcRust) << "context stale pagination ignored generation="
                       << generation;
        return;
    }
    const bool forward = event.value(QStringLiteral("direction")).toString()
        == QLatin1String("forward");
    Q_EMIT eventContextPagination(
        roomId, eventId, forward,
        event.value(QStringLiteral("state")).toString(),
        event.value(QStringLiteral("reached_edge")).toBool(false));
}

void RustSdkMatrixClient::handleContextError(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QString eventId = event.value(QStringLiteral("event_id")).toString();
    const QString timelineId = contextTimelineId(roomId, eventId);
    const QString category = event.value(QStringLiteral("category"))
                                 .toString(QStringLiteral("unknown"));
    qCWarning(lcRust) << "event context error category=" << category;
    if (m_contextTracker.requestedRoom() == timelineId
        || m_contextTracker.activeRoom() == timelineId) {
        clearContextTimelineState();
        Q_EMIT eventContextFailed(roomId, eventId, category);
    }
}

void RustSdkMatrixClient::handleContextClosed(const QJsonObject &event)
{
    const QString timelineId = contextTimelineId(
        event.value(QStringLiteral("room_id")).toString(),
        event.value(QStringLiteral("event_id")).toString());
    if (m_contextTracker.activeRoom() != timelineId
        && m_contextTracker.requestedRoom() != timelineId)
        m_timelines.remove(timelineId);
    qCInfo(lcRust) << "event context stopped";
}

void RustSdkMatrixClient::closeRoomTimeline()
{
    clearThreadTimelineState();
    clearContextTimelineState();
    m_threadListRoom.clear();
    m_threadListGeneration = 0;
    const QString closingRequested = m_timelineTracker.requestedRoom();
    const QString closingActive = m_timelineTracker.activeRoom();
    if (m_rustHandle && m_timelineTracker.hasActiveTimeline()) {
        takeRustString(mx_rust_timeline_close(m_rustHandle));
        qCInfo(lcRust) << "timeline close room="
                       << matrix::e2ee::redactId(closingActive);
    }
    m_timelineTracker.reset();
    retireRoomTimelineMirror(closingRequested);
    retireRoomTimelineMirror(closingActive);
}

void RustSdkMatrixClient::refuseSend(const char *op)
{
    qCWarning(lcRust) << "send refused: not implemented" << op;
    Q_EMIT errorOccurred(
        tr("Lightning cannot send that yet."));
}

void RustSdkMatrixClient::refuseUntilTimelineReady(const char *op)
{
    qCWarning(lcRust) << "send refused: no live timeline for the room" << op
                      << "active=" << m_timelineTracker.activeRoom()
                      << "requested=" << m_timelineTracker.requestedRoom();
    Q_EMIT errorOccurred(
        tr("This room is still loading. Try that again in a moment."));
}

void RustSdkMatrixClient::pollRustEvents()
{
    stalltrace::Scope stallScope("rust-poll-drain");

    drainAuthEvents();
    drainProvidersEvents();
    drainRevokeEvents();
    drainWhoamiEvents();

    if (!m_rustHandle)
        return;

    const quint64 eventGeneration = m_handleGeneration;

    for (int i = 0; i < 256; ++i) {
        const QByteArray raw =
            takeRustBytes(mx_rust_poll_command_event(m_rustHandle));
        if (raw.isEmpty())
            break;
        if (rustPollFailed(raw))
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
        if (!m_rustHandle || m_handleGeneration != eventGeneration)
            return;
    }

    const bool traceSync = synctrace::enabled();

    m_coalesceTimelineInserts = true;
    constexpr int kSoftDrainCap = 64;
    constexpr int kHardDrainCap = 256;
    for (int i = 0; i < kHardDrainCap; ++i) {
        const QByteArray raw = takeRustBytes(mx_rust_poll_event(m_rustHandle));
        if (raw.isEmpty())
            break;
        if (rustPollFailed(raw))
            break;

        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isObject()) {
            qCWarning(lcRust) << "discarding malformed Rust SDK event";
            continue;
        }
        const QJsonObject event = doc.object();
        const QString type = event.value(QStringLiteral("type")).toString();
        if (type != QLatin1String("timeline_diff"))
            flushTimelineInsertBatch();
        if (m_lifecycle.acceptsActive(eventGeneration)) {
            handleRustEvent(event, eventGeneration);
        } else if (type == QLatin1String("logged_out")
                   && m_lifecycle.acceptsShutdownCompletion(eventGeneration)) {
            handleRustEvent(event, eventGeneration);
            m_coalesceTimelineInserts = false;
            return;
        } else {
            qCInfo(lcRust) << "ignored stale callback"
                           << "type=" << type
                           << "generation=" << eventGeneration
                           << "active_generation="
                           << m_lifecycle.activeGeneration();
        }
        if (traceSync)
            synctrace::noteSyncResponse();
        if (!m_rustHandle || m_handleGeneration != eventGeneration)
            break;
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
        qCWarning(lcRust) << "rust server logout result=failed"
                          << "message=" << serverMessage;
    } else {
        qCInfo(lcRust) << "rust server logout result=" << serverResult;
    }

    releaseRustHandle();
    clearLocalState();
    // releaseRustHandle() only hands the handle to a retirement worker; its
    // SQLite stores close when that worker's mx_rust_destroy() has run. On
    // Windows an open SQLite file cannot be deleted, so deleting now made every
    // sign-out race that close and report "cleanup_incomplete". Every other
    // deleter waits here too (resetLocalSession, rollBackFailedAttempt,
    // AppController::removeAccountLocalState, the sign-in quarantine).
    {
        QElapsedTimer closeWait;
        closeWait.start();
        const bool retired = waitForRustRetirement(kStoreCloseBudgetMs);
        qCInfo(lcRust) << "rust sign-out store close"
                       << "retired=" << retired
                       << "waited_ms=" << closeWait.elapsed();
        if (!retired)
            qCWarning(lcRust) << "sign-out: the store was still open after the "
                                 "close budget; deleting anyway";
    }
    bool claimGranted = true;
    if (identity.isValid()) {
        const auto claim = storeOwnershipRegistry().claimForDeletion(identity.rustStorePath);
        claimGranted = claim == lightning::accounts::StoreOwnershipRegistry::ClaimVerdict::Granted;
        if (!claimGranted)
            reportStoreClaimRefused(int(claim), identity.slug, "sign_out");
    }
    bool matchedRecord = false;
    const bool sessionOk = claimGranted && identity.isValid()
        && clearPersistedAccount(identity, &matchedRecord);
    const auto files = (identity.isValid() && claimGranted)
        ? matrix::app_data::removeAccountRustState(identity)
        : matrix::app_data::RemovalSummary{0, 0, 1};
    if (m_settings && identity.isValid() && files.ok() && sessionOk)
        m_settings->clearPendingSignOut(identity.userId);
    const bool didSomething = matchedRecord || files.removedAnything();
    const bool ok = sessionOk && files.ok() && didSomething;
    const auto backgrounds = (identity.isValid() && claimGranted)
        ? matrix::app_data::removePersonalBackgrounds(identity)
        : matrix::app_data::RemovalSummary{0, 0, 0};
    if (!backgrounds.ok()) {
        qCWarning(lcRust) << "personal backgrounds sign-out cleanup FAILED slug="
                          << identity.slug << "failed=" << backgrounds.failed;
    }
    const auto accountDir = (identity.isValid() && claimGranted)
        ? matrix::app_data::removeAccountRootIfEmpty(identity.accountRoot)
        : matrix::app_data::EmptyDirRemoval::Refused;

    qCInfo(lcRust) << "rust local sign-out cleanup"
                   << "slug=" << identity.slug
                   << "matched_record=" << matchedRecord
                   << "did_something=" << didSomething
                   << "session=" << (sessionOk ? "ok" : "failed")
                   << "store_and_sidecars=" << (files.ok() ? "ok" : "failed")
                   << "deleted=" << files.deleted
                   << "missing=" << files.missing
                   << "failed=" << files.failed
                   << "backgrounds_deleted=" << backgrounds.deleted
                   << "backgrounds_failed=" << backgrounds.failed
                   << "account_dir="
                   << matrix::app_data::emptyDirRemovalName(accountDir);

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
        // Keyed on the record that names THIS handle's device, in its
        // canonical id (saveSession() writes secrets under it, while m_userId
        // may be the server's raw answer), never on whichever account is
        // active: those can differ, and the tokens would land on another
        // account.
        const QString tokenOwner = tokenOwnerForRunningSession();
        const lightning::accounts::SessionTokenPair rotated{
            event.value(QStringLiteral("access_token")).toString(),
            event.value(QStringLiteral("refresh_token")).toString()};
        // Only the pair the session holds NOW is written: two rotations can
        // wait in one drain, and the older one must never land over the newer
        // (its refresh token is already used up). An event older than the
        // session's tokens is skipped; the newer one follows it.
        QString currentAccess;
        if (m_rustHandle) {
            const QString current = takeRustString(mx_rust_final_session_tokens(m_rustHandle));
            currentAccess = custody::parseFinalSessionTokens(current).accessToken;
        }
        SettingsTokenStore settingsStore(m_settings);
        QElapsedTimer writeTimer;
        writeTimer.start();
        const custody::LiveWrite outcome = custody::writeLive(
            m_settings ? &settingsStore : nullptr, tokenOwner, currentAccess, rotated);
        const qint64 writeMs = writeTimer.elapsed();
        const qint64 generation = event.value(QStringLiteral("generation")).toInteger(0);
        const custody::PersistAnswer answer = custody::persistAnswerFor(outcome);
        if (generation > 0 && m_rustHandle && answer != custody::PersistAnswer::None) {
            mx_rust_session_tokens_persisted(
                m_rustHandle, static_cast<quint64>(generation),
                answer == custody::PersistAnswer::Saved ? 1 : 0);
        }
        if (tokenTraceEnabled()) {
            qCInfo(lcTokenTrace) << "rotation"
                                 << "generation=" << generation
                                 << "write=" << liveWriteWord(outcome)
                                 << "write_ms=" << writeMs
                                 << "store=" << (m_settings ? m_settings->secretBackendName()
                                                            : QString())
                                 << "recorded_backend="
                                 << (m_settings && !tokenOwner.isEmpty()
                                         ? m_settings->keyringBackendFor(tokenOwner)
                                         : QString())
                                 << "has_refresh_token=" << !rotated.refreshToken.isEmpty()
                                 << "slug=" << matrix::app_data::safeUserSlug(m_userId);
        }
        switch (outcome) {
        case custody::LiveWrite::SkippedRotatedPast:
            qCInfo(lcRust) << "skipped a rotation the session has already "
                              "rotated past"
                           << "slug=" << matrix::app_data::safeUserSlug(m_userId);
            return;
        case custody::LiveWrite::Written:
            m_persistedAccessDigest = accessTokenDigest(rotated.accessToken);
            m_tokenWriteRetry.stop();
            m_tokenWriteFailingFor.invalidate();
            break;
        case custody::LiveWrite::WriteFailed:
            m_tokenWriteAttempts = 0;
            if (!m_tokenWriteFailingFor.isValid())
                m_tokenWriteFailingFor.start();
            scheduleRotatedTokenWrite();
            break;
        case custody::LiveWrite::SkippedNoOwner:
            qCWarning(lcRust) << "a rotated sign-in matches no saved account "
                                 "for this device; not stored"
                              << "slug=" << matrix::app_data::safeUserSlug(m_userId);
            break;
        }
        return;
    }

    if (type == QLatin1String("session_token_revoked")) {
        const QJsonValue softValue = event.value(QStringLiteral("soft_logout"));
        const QJsonValue hadRefresh = event.value(QStringLiteral("had_refresh_token"));
        lightning::accounts::RevocationEventFacts facts;
        if (softValue.isBool())
            facts.softLogout = softValue.toBool();
        if (hadRefresh.isBool())
            facts.hadRefreshToken = hadRefresh.toBool();
        facts.oauthSession = m_settings && !m_userId.isEmpty()
            && m_settings->isOAuthAccount(m_userId);
        const bool oauthSession = facts.oauthSession;
        bool keyringHoldsOrCannotTell = false;
        if (lightning::accounts::revocationNeedsKeyringProbe(facts)) {
            QString owner = tokenOwnerForRunningSession();
            if (owner.isEmpty())
                owner = m_openingIdentity.userId;
            if (owner.isEmpty()) {
                keyringHoldsOrCannotTell = true;
            } else {
                const QString stored = m_settings->refreshTokenFor(owner);
                keyringHoldsOrCannotTell =
                    !stored.isEmpty() || m_settings->secretBackendUnavailable();
            }
        }
        const lightning::accounts::RevocationClassification classified =
            lightning::accounts::classifyRevocation(facts, keyringHoldsOrCannotTell);
        lightning::accounts::SignOutFacts causeFacts;
        causeFacts.hadRefreshToken = facts.hadRefreshToken;
        if (event.value(QStringLiteral("rotations")).isDouble())
            causeFacts.rotations = event.value(QStringLiteral("rotations")).toInteger();
        if (event.value(QStringLiteral("unsaved_rotation")).isBool())
            causeFacts.unsavedRotation = event.value(QStringLiteral("unsaved_rotation")).toBool();
        const char *cause = lightning::accounts::signOutCause(causeFacts);
        const qint64 sinceRotationMs =
            event.value(QStringLiteral("since_rotation_ms")).toInteger(-1);
        const bool softLogoutReported = classified.softLogoutReported;
        if (classified.outcome
            == lightning::accounts::RevocationOutcome::KeyringHoldsOrCannotTell) {
            const auto reason =
                matrix::rust_session::StoreBlockReason::SecretBackendUnavailable;
            const QString user = m_loggedIn ? m_userId : m_openingIdentity.userId;
            const QString server =
                m_loggedIn ? m_homeserver : m_openingIdentity.homeserver;
            qCInfo(lcRust) << "session credential rejected and could not be renewed"
                           << "slug=" << matrix::app_data::safeUserSlug(user)
                           << "oauth=" << true << "had_refresh_token=false"
                           << "keyring_holds_one_or_cannot_tell=" << true
                           << "cause=" << cause;
            Q_EMIT localSessionBlocked(matrix::rust_session::diagnosticName(reason),
                                       user, server);
            Q_EMIT loginFailed(matrix::rust_session::userMessage(reason));
            return;
        }
        const bool hardLogout =
            classified.outcome == lightning::accounts::RevocationOutcome::Hard;
        const bool softLogout =
            classified.outcome == lightning::accounts::RevocationOutcome::Soft;
        if (hardLogout && !m_userId.isEmpty() && !m_deviceId.isEmpty()) {
            m_revokedUserId = m_userId;
            m_revokedDeviceId = m_deviceId;
            if (m_softLogoutUserId == m_userId
                && m_softLogoutDeviceId == m_deviceId) {
                m_softLogoutUserId.clear();
                m_softLogoutHomeserver.clear();
                m_softLogoutDeviceId.clear();
            }
            if (m_revocationProofs)
                m_revocationProofs->recordHard(m_userId, m_deviceId);
        } else if (softLogout && !m_userId.isEmpty() && !m_deviceId.isEmpty()
                   && !(m_revokedUserId == m_userId
                        && m_revokedDeviceId == m_deviceId)) {
            matrix::app_data::AccountIdentity running;
            if (matrix::app_data::resolveAccountIdentity(m_homeserver, m_userId,
                                                         &running)) {
                m_softLogoutUserId = m_userId;
                m_softLogoutHomeserver = running.homeserver;
                m_softLogoutDeviceId = m_deviceId;
                if (m_revocationProofs)
                    m_revocationProofs->recordSoft(m_userId, running.homeserver,
                                                   m_deviceId);
            }
        }
        const auto reason = softLogout
            ? matrix::rust_session::StoreBlockReason::AccessTokenExpired
            : matrix::rust_session::StoreBlockReason::AccessTokenRevoked;
        const QString revokedUser = m_loggedIn ? m_userId : m_openingIdentity.userId;
        const QString revokedServer =
            m_loggedIn ? m_homeserver : m_openingIdentity.homeserver;
        qCInfo(lcRust) << "session credential rejected and could not be renewed"
                       << "slug=" << matrix::app_data::safeUserSlug(revokedUser)
                       << "soft_logout=" << softLogoutReported
                       << "oauth=" << oauthSession
                       << "had_refresh_token="
                       << (hadRefresh.isBool()
                               ? (hadRefresh.toBool() ? "true" : "false")
                               : "unknown")
                       << "cause=" << cause
                       << "rotations=" << causeFacts.rotations.value_or(-1)
                       << "since_rotation_ms=" << sinceRotationMs;
        if (tokenTraceEnabled()) {
            const QString owner = tokenOwnerForRunningSession();
            qCInfo(lcTokenTrace) << "declared signed out"
                                 << "cause=" << cause
                                 << "outcome=" << (hardLogout ? "hard" : softLogout ? "soft"
                                                                                    : "unproven")
                                 << "unsaved_rotation="
                                 << causeFacts.unsavedRotation.value_or(false)
                                 << "persisted_digest_known=" << !m_persistedAccessDigest.isEmpty()
                                 << "write_retry_pending=" << m_tokenWriteRetry.isActive()
                                 << "store=" << (m_settings ? m_settings->secretBackendName()
                                                            : QString())
                                 << "recorded_backend="
                                 << (m_settings && !owner.isEmpty()
                                         ? m_settings->keyringBackendFor(owner) : QString())
                                 << "slug=" << matrix::app_data::safeUserSlug(revokedUser);
        }
        Q_EMIT localSessionBlocked(matrix::rust_session::diagnosticName(reason),
                                   revokedUser, revokedServer);
        Q_EMIT loginFailed(matrix::rust_session::userMessage(reason));
        return;
    }

    if (type == QLatin1String("token_trace")) {
        if (tokenTraceEnabled())
            qCInfo(lcTokenTrace).noquote()
                << "rust" << QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact));
        return;
    }

    if (type == QLatin1String("session_restored_offline")) {
        qCInfo(lcRust) << "session restored from the local store — the "
                          "homeserver could not be reached; rooms and "
                          "messages are the cached copy and sync will retry";
        m_restoredOffline = true;
        return;
    }

    if (type == QLatin1String("login_ok")) {
        if (lightning::switchtrace::active()) {
            const qint64 receivedWallMs = QDateTime::currentMSecsSinceEpoch();
            const QJsonValue trace = event.value(QStringLiteral("trace"));
            if (trace.isObject())
                lightning::switchtrace::noteRustRestore(trace.toObject(), receivedWallMs);
        }
        const QString answerUserId =
            event.value(QStringLiteral("user_id")).toString().trimmed();
        const bool sentBareUsername = std::exchange(m_loginSentBareUsername, false);
        if (sentBareUsername && m_settings && m_storePathOverride.isEmpty()
            && !m_storePath.isEmpty() && !answerUserId.isEmpty()
            && !event.value(QStringLiteral("access_token")).toString().isEmpty()) {
            const QString openedSlug = QFileInfo(
                QFileInfo(m_storePath).absoluteFilePath()).dir().dirName();
            const QString openedOwner =
                m_settings->accountOwningStoreSlug(openedSlug);
            matrix::app_data::AccountIdentity answerRecord;
            const bool answerHasRecord =
                m_settings->resolveSavedIdentity(answerUserId, &answerRecord);
            if (matrix::rust_session::signInAnswerCollidesWithSavedStore(
                    answerUserId, m_storePath, openedOwner, answerHasRecord,
                    answerRecord.rustStorePath)) {
                const bool answerIsSaved = answerHasRecord
                    && (openedOwner.isEmpty() || openedOwner == answerUserId);
                qCWarning(lcRust)
                    << "sign-in not kept: the server's answer and the store this "
                       "attempt opened belong to different saved accounts"
                    << "account=" << matrix::app_data::safeUserSlug(answerUserId)
                    << "store=" << openedSlug
                    << "answer_saved=" << answerHasRecord
                    << "store_owned=" << !openedOwner.isEmpty();
                const bool fresh = m_freshLoginIdentity.isValid()
                    && QFileInfo(m_freshLoginIdentity.rustStorePath).absoluteFilePath()
                           == QFileInfo(m_storePath).absoluteFilePath();
                const auto freshStore = fresh ? m_freshLoginIdentity
                                              : matrix::app_data::AccountIdentity{};
                m_freshLoginIdentity = {};
                m_freshBrowserRecord = {};
                m_resumeDeviceId.clear();
                m_loggedIn = false;
                m_userId.clear();
                m_deviceId.clear();
                setState(Error);
                if (freshStore.isValid())
                    rollBackFailedAttempt(freshStore, {});
                else
                    releaseRustHandle();
                Q_EMIT loginFailed(answerIsSaved
                    ? tr("%1 is already on this device, so this sign-in was not "
                         "kept and the saved account was not changed. Switch to "
                         "it from the account menu, or sign in with the full "
                         "Matrix ID %1. The server now lists an unused session "
                         "from this attempt; you can sign it out under "
                         "Sessions.").arg(answerUserId)
                    : tr("The server signed you in as %1, but this device keeps "
                         "another account's data where this sign-in was going, "
                         "so it was not kept and no saved account was changed. "
                         "Sign in with the full Matrix ID %1. The server now "
                         "lists an unused session from this attempt; you can "
                         "sign it out under Sessions after signing in with "
                         "%1.").arg(answerUserId));
                return;
            }
        }
        const matrix::app_data::AccountIdentity attempt =
            std::exchange(m_freshLoginIdentity, {});
        m_freshBrowserRecord = {};
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
        if (m_loggedIn && m_userId == m_revokedUserId) {
            m_revokedUserId.clear();
            m_revokedDeviceId.clear();
        }
        if (m_loggedIn && m_userId == m_softLogoutUserId) {
            m_softLogoutUserId.clear();
            m_softLogoutHomeserver.clear();
            m_softLogoutDeviceId.clear();
        }
        if (m_loggedIn && m_revocationProofs)
            m_revocationProofs->clear(m_userId);
        if (!m_resumeDeviceId.isEmpty() && m_deviceId != m_resumeDeviceId) {
            qCWarning(lcRust) << "a resumed sign-in came back as another device"
                              << "slug=" << matrix::app_data::safeUserSlug(m_userId);
        }
        disarmResumedDeviceCheck();
        if (m_loggedIn && !m_resumeDeviceId.isEmpty()
            && m_deviceId == m_resumeDeviceId) {
            armResumedDeviceCheck();
        }
        m_resumeDeviceId.clear();
        const QString accessToken = event.value(QStringLiteral("access_token")).toString();
        const QString refreshToken = event.value(QStringLiteral("refresh_token")).toString();
        if (m_loggedIn && m_settings && !accessToken.isEmpty()) {
            m_settings->saveSession(m_homeserver, m_userId, m_deviceId, accessToken,
                                    refreshToken);
            m_persistedAccessDigest = accessTokenDigest(accessToken);
            m_settings->setSyncToken({});
        }
        if (m_loggedIn && !accessToken.isEmpty() && identity.isValid()
            && identity.userId == m_userId) {
            recordStoreLocation(identity);
        }
        if (m_loggedIn && m_settings && !accessToken.isEmpty())
            m_settings->clearPendingSignIn(attempt);
        refreshMediaKeepState();
        setState(m_restoredOffline ? Offline : Disconnected);
        if (m_loggedIn)
            Q_EMIT loginSucceeded(m_userId);
        else
            Q_EMIT loginFailed(tr("Rust SDK login response did not include a user id."));
        return;
    }

    if (type == QLatin1String("login_failed")) {
        if (lightning::switchtrace::active()) {
            const qint64 receivedWallMs = QDateTime::currentMSecsSinceEpoch();
            const QJsonValue trace = event.value(QStringLiteral("trace"));
            if (trace.isObject())
                lightning::switchtrace::noteRustRestore(trace.toObject(), receivedWallMs);
        }
        m_loginSentBareUsername = false;
        m_loggedIn = false;
        const bool resumeAttempt = !m_resumeDeviceId.isEmpty();
        m_resumeDeviceId.clear();
    m_ownIdentityKeyMismatchLogged = false;
        setState(Error);
        const bool freshMatchesThisAttempt =
            m_freshLoginIdentity.isValid() && !m_storePath.isEmpty()
            && QFileInfo(m_freshLoginIdentity.rustStorePath).absoluteFilePath()
                   == QFileInfo(m_storePath).absoluteFilePath();
        const bool browserRecordMatchesThisAttempt =
            m_freshBrowserRecord.isValid() && !m_storePath.isEmpty()
            && QFileInfo(m_freshBrowserRecord.rustStorePath).absoluteFilePath()
                   == QFileInfo(m_storePath).absoluteFilePath();
        const matrix::app_data::AccountIdentity browserRecord =
            browserRecordMatchesThisAttempt ? m_freshBrowserRecord
                                            : matrix::app_data::AccountIdentity{};
        m_freshBrowserRecord = {};
        if (m_freshLoginIdentity.isValid() && !freshMatchesThisAttempt) {
            qCWarning(lcRust)
                << "discarding a fresh-store marker that names a different "
                   "account than this attempt; no store was removed";
            m_freshLoginIdentity = {};
        }
        const auto freshStore = matrix::app_data::AccountIdentity(
            std::exchange(m_freshLoginIdentity, {}));
        const bool rolledBack = (freshStore.isValid() || browserRecord.isValid())
            ? rollBackFailedAttempt(freshStore, browserRecord)
            : true;
        const QString message = event.value(QStringLiteral("message")).toString(
            tr("Rust SDK login failed."));
        qCWarning(lcRust) << "sign-in or restore failed"
                          << "stage=" << event.value(QStringLiteral("stage")).toString()
                          << "reason=" << event.value(QStringLiteral("reason")).toString()
                          << "message=" << message.left(400);
        if (browserRecord.isValid()) {
            Q_EMIT loginFailed(browserRestoreFailureText(rolledBack, message));
        } else if (resumeAttempt
                   && matrix::rust_session::isStoreOwnershipMismatch(message)) {
            qCWarning(lcRust) << "resumed sign-in not accepted by the store; "
                                 "nothing changed"
                              << "slug=" << m_openingIdentity.slug;
            Q_EMIT loginFailed(tr(
                "Signing in again as this device did not work. Nothing on this "
                "device was changed. Try again, or remove this account below."));
        } else if (matrix::rust_session::isStoreOwnershipMismatch(message)) {
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
            qCInfo(lcRust) << "saved session rejected by the homeserver"
                           << "slug=" << m_openingIdentity.slug;
            Q_EMIT localSessionBlocked(
                matrix::rust_session::diagnosticName(
                    matrix::rust_session::StoreBlockReason::AccessTokenRevoked),
                m_openingIdentity.userId, m_openingIdentity.homeserver);
            Q_EMIT loginFailed(matrix::rust_session::userMessage(
                matrix::rust_session::StoreBlockReason::AccessTokenRevoked));
        } else if (event.value(QStringLiteral("stage")).toString()
                   == QLatin1String("password")) {
            Q_EMIT loginFailed(passwordLoginFailureText(
                event.value(QStringLiteral("reason")).toString(), message));
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
        const QJsonArray rooms = event.value(QStringLiteral("rooms")).toArray();
        if (!lightning::switchtrace::active()) {
            handleRoomsEvent(rooms);
            return;
        }
        const qint64 receivedWallMs = QDateTime::currentMSecsSinceEpoch();
        qint64 parseMs = -1;
        qint64 modelMs = -1;
        handleRoomsEvent(rooms, &parseMs, &modelMs);
        lightning::switchtrace::noteFirstReset(
            int(rooms.size()), parseMs, modelMs,
            event.value(QStringLiteral("trace")).toObject(), receivedWallMs);
        return;
    }

    if (type == QLatin1String("room_snapshot")) {
        handleRoomSnapshotEvent(event.value(QStringLiteral("rooms")).toArray());
        return;
    }

    if (type == QLatin1String("room_tag_confirmed")) {
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        auto room = m_rooms.find(roomId);
        if (room == m_rooms.end() || !event.value(QStringLiteral("low_priority")).isBool())
            return;
        const bool lowPriority = event.value(QStringLiteral("low_priority")).toBool();
        room->lowPriority = lowPriority;
        if (lowPriority)
            room->isFavourite = false;
        Q_EMIT roomsChanged();
        return;
    }

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
        if (state == m_lastSyncState)
            return;
        m_lastSyncState = state;
        qCInfo(lcRust) << "sync state=" << state;
        if (state == QLatin1String("running"))
            synctrace::noteSyncState("running");
        else if (state == QLatin1String("offline"))
            synctrace::noteSyncState("offline");
        else if (state == QLatin1String("retrying"))
            synctrace::noteSyncState("retrying");
        else if (state == QLatin1String("starting"))
            synctrace::noteSyncState("starting");
        if (state == QLatin1String("running"))
            m_restoredOffline = false;
        if (state == QLatin1String("offline")) setState(Offline);
        else if (state == QLatin1String("starting") || state == QLatin1String("retrying"))
            setState(Syncing);
        else if (state == QLatin1String("running")) setState(Syncing);
        if (state == QLatin1String("running"))
            Q_EMIT homeserverReachable();
        return;
    }

    if (type == QLatin1String("room_list_error")) {
        const QString category = event.value(QStringLiteral("category")).toString();
        if (category == QLatin1String("authentication")) {
            setState(Error);
            Q_EMIT errorOccurred(tr("Matrix session is no longer authorized."));
            return;
        }
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
            const QString displayName = object.value(QStringLiteral("display_name")).toString().trimmed();
            if (!displayName.isEmpty()) {
                auto member = room->members.value(userId);
                member.userId = userId;
                member.displayName = displayName;
                room->members.insert(userId, member);
            }
        }
        room->typingUserIds = users;
        const quint64 serial = ++m_typingSerial[roomId];
        if (!users.isEmpty()) {
            QTimer::singleShot(m_typingExpiryMs, this,
                               [this, roomId, serial, gen = m_lifecycle.activeGeneration()] {
                if (!m_lifecycle.acceptsActive(gen)
                    || m_typingSerial.value(roomId) != serial)
                    return;
                auto expired = m_rooms.find(roomId);
                if (expired == m_rooms.end() || expired->typingUserIds.isEmpty())
                    return;
                expired->typingUserIds.clear();
                Q_EMIT typingChanged(roomId);
            });
        }
        Q_EMIT typingChanged(roomId);
        return;
    }

    if (type == QLatin1String("room_members_changed")) {
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        if (m_rooms.contains(roomId))
            Q_EMIT roomMemberEventSeen(roomId);
        return;
    }

    if (type == QLatin1String("resumed_device_key")) {
        const QString userId = event.value(QStringLiteral("user_id")).toString();
        const QString deviceId = event.value(QStringLiteral("device_id")).toString();
        if (!resumedDeviceCheckIsFor(userId, deviceId))
            return;
        const QString state = event.value(QStringLiteral("state")).toString();
        if (state == QLatin1String("matches")) {
            qCInfo(lcRust) << "resumed device checked: the server publishes "
                              "this store's key"
                           << "slug=" << matrix::app_data::safeUserSlug(userId);
            disarmResumedDeviceCheck();
            return;
        }
        if (state != QLatin1String("missing") && state != QLatin1String("different")) {
            if (m_resumedCheckAttempts < kResumedDeviceCheckAttempts) {
                scheduleResumedDeviceCheck();
            } else {
                qCInfo(lcRust) << "resumed device check gave up without an answer"
                               << "slug=" << matrix::app_data::safeUserSlug(userId);
                disarmResumedDeviceCheck();
            }
            return;
        }
        qCWarning(lcRust) << "the resumed device has no keys of this store on "
                             "the server; ending it for a new sign-in"
                          << "state=" << state
                          << "ended_on_server="
                          << event.value(QStringLiteral("ended_on_server")).toBool()
                          << "slug=" << matrix::app_data::safeUserSlug(userId);
        disarmResumedDeviceCheck();
        m_revokedUserId = userId;
        m_revokedDeviceId = deviceId;
        m_softLogoutUserId.clear();
        m_softLogoutHomeserver.clear();
        m_softLogoutDeviceId.clear();
        if (m_revocationProofs) {
            m_revocationProofs->recordHard(userId, deviceId);
            m_revocationProofs->clearSoft(userId);
        }
        Q_EMIT localSessionBlocked(
            matrix::rust_session::diagnosticName(
                matrix::rust_session::StoreBlockReason::AccessTokenRevoked),
            userId, m_homeserver);
        Q_EMIT loginFailed(matrix::rust_session::userMessage(
            matrix::rust_session::StoreBlockReason::AccessTokenRevoked));
        return;
    }

    if (type == QLatin1String("direct_peer_avatar")) {
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        const QString userId = event.value(QStringLiteral("user_id")).toString();
        if (m_rooms.contains(roomId) && !userId.isEmpty() && userId != m_userId) {
            Q_EMIT directPeerAvatarChanged(
                roomId, userId,
                event.value(QStringLiteral("avatar_url")).toString());
        }
        return;
    }

    if (type == QLatin1String("room_preview_backfill")) {
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        const QString preview = matrix::preview::normalizePreviewText(
            event.value(QStringLiteral("preview")).toString());
        matrix::preview::Kind kind = matrix::preview::kindFromWire(
            event.value(QStringLiteral("preview_kind")).toString());
        QString arg = kind == matrix::preview::Kind::Text
            ? preview
            : matrix::preview::normalizePreviewText(
                  event.value(QStringLiteral("preview_arg")).toString());
        if (preview.isEmpty()) {
            kind = event.value(QStringLiteral("undecryptable")).toBool(false)
                ? matrix::preview::Kind::Undecryptable
                : matrix::preview::Kind::Text;
            arg.clear();
        }
        auto it = m_rooms.find(roomId);
        if (it != m_rooms.end()
            && (it->lastMessagePreview.isEmpty()
                || it->previewKind == matrix::preview::Kind::Undecryptable)
            && (kind != matrix::preview::Kind::Text || !arg.isEmpty())) {
            it->setPreview(kind, arg);
            it->raiseActivity(timestampFromMs(static_cast<qint64>(
                event.value(QStringLiteral("last_activity_ms")).toDouble(0))));
            Q_EMIT roomUpdated(roomId);
        }
        return;
    }

    if (type == QLatin1String("room_background_changed")) {
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        if (m_rooms.contains(roomId))
            Q_EMIT roomBackgroundChanged(roomId);
        return;
    }

    if (type == QLatin1String("room_pinned_changed")) {
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        if (m_rooms.contains(roomId))
            Q_EMIT pinnedEventsChanged(roomId);
        return;
    }

    if (type == QLatin1String("room_tombstone_changed")) {
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
        const QString state = event.value(QStringLiteral("state")).toString();
        if (state == QLatin1String("done") && !roomId.isEmpty()) {
            Q_EMIT inviteAnswered(
                roomId, event.value(QStringLiteral("action")).toString()
                            == QLatin1String("accept"));
        }
        auto room = m_rooms.find(roomId);
        if (room == m_rooms.end()) return;
        room->invitePending = state == QLatin1String("pending");
        room->inviteError = state == QLatin1String("failed")
            ? tr("Invite action failed. Try again.") : QString{};
        Q_EMIT roomUpdated(roomId);
        return;
    }

    if (type == QLatin1String("stale_invites_retired")) {
        qCInfo(lcRust) << "stale invitations retired count="
                       << event.value(QStringLiteral("count")).toInt();
        return;
    }

    if (type == QLatin1String("room_membership_lost")) {
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        if (roomId.isEmpty())
            return;
        const QString loss = event.value(QStringLiteral("loss")).toString();
        QString name = event.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) {
            const auto listed = m_rooms.constFind(roomId);
            if (listed != m_rooms.cend())
                name = listed->name;
        }
        qCInfo(lcRust) << "room membership lost room="
                       << matrix::e2ee::redactId(roomId) << "loss=" << loss;
        Q_EMIT roomMembershipLost(roomId, loss, name);
        return;
    }

    if (type == QLatin1String("read_marker_advanced")) {
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        const QString accepted =
            event.value(QStringLiteral("event_id")).toString(
                m_lastReceiptSent.value(roomId));
        Q_EMIT readMarkerAdvanced(roomId);
        if (!roomId.isEmpty() && !accepted.isEmpty())
            Q_EMIT readReceiptAccepted(roomId, accepted);
        return;
    }

    if (type == QLatin1String("push_rules_changed")) {
        Q_EMIT pushRulesChanged();
        return;
    }
    if (type == QLatin1String("thread_read_marker_advanced")) {
        Q_EMIT threadReadMarkerAdvanced(
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("thread_root_id")).toString(),
            static_cast<quint64>(event.value(QStringLiteral("op_id")).toDouble()));
        return;
    }
    if (type == QLatin1String("thread_mark_read_failed")) {
        Q_EMIT threadMarkReadFailed(
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("thread_root_id")).toString(),
            static_cast<quint64>(event.value(QStringLiteral("op_id")).toDouble()));
        return;
    }

    if (type == QLatin1String("room_action_error")) {
        const QString action = event.value(QStringLiteral("action")).toString();
        if (action == QLatin1String("favourite")
            || action == QLatin1String("low_priority"))
            Q_EMIT roomActionFailed(action,
                event.value(QStringLiteral("room_id")).toString());
        if (action == QLatin1String("mark_read"))
            Q_EMIT markRoomReadFailed(event.value(QStringLiteral("room_id")).toString());
        if (action == QLatin1String("read_receipt")) {
            const QString roomId = event.value(QStringLiteral("room_id")).toString();
            const QString outstanding = m_lastReceiptSent.value(roomId);
            const QString failed =
                event.value(QStringLiteral("event_id")).toString(outstanding);
            if (!outstanding.isEmpty() && outstanding == failed)
                m_lastReceiptSent.remove(roomId);
            if (!roomId.isEmpty() && !failed.isEmpty())
                Q_EMIT readReceiptFailed(roomId, failed);
        }
        const QString reason = event.value(QStringLiteral("reason")).toString();
        qCWarning(lcRust) << "room action failed category=" << action
                          << "reason=" << reason << "attempts="
                          << event.value(QStringLiteral("attempts")).toInt() << "room="
                          << matrix::e2ee::redactId(event.value(QStringLiteral("room_id")).toString());
        const QString message = matrix::room_action::userFacingError(action, reason);
        if (!message.isEmpty())
            Q_EMIT errorOccurred(message);
        return;
    }

    if (type == QLatin1String("room_notification_mode")) {
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        const int mode = event.value(QStringLiteral("mode")).toInt(-1);
        if (roomId.isEmpty()) return;
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

    if (type == QLatin1String("notification_mode_error")) {
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        if (roomId.isEmpty()) return;
        qCWarning(lcRust) << "room action failed category= notification_mode";
        Q_EMIT roomNotificationModeWriteFailed(roomId);
        return;
    }

    if (type == QLatin1String("initial_sync_done")) {
        setInitialSyncDone(true);
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
    if (type == QLatin1String("event_redacted")) {
        handleEventRedacted(event);
        return;
    }
    if (type == QLatin1String("event_decrypted")) {
        handleEventDecrypted(event);
        return;
    }

    if (type == QLatin1String("timeline_reset")) {
        handleTimelineReset(event);
        return;
    }
    if (type == QLatin1String("timeline_diff")) {
        if (synctrace::enabled()) {
            const quint64 traceId = synctrace::beginEvent(
                event.value(QStringLiteral("room_id")).toString(),
                static_cast<qint64>(
                    event.value(QStringLiteral("trace_sdk_ms")).toDouble()));
            synctrace::noteBridge(traceId);
            handleTimelineDiff(event);
            synctrace::noteModel(traceId);
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
    if (type == QLatin1String("context_reset")) {
        handleContextReset(event);
        return;
    }
    if (type == QLatin1String("context_diff")) {
        handleContextDiff(event);
        return;
    }
    if (type == QLatin1String("context_pagination")) {
        handleContextPagination(event);
        return;
    }
    if (type == QLatin1String("context_error")) {
        handleContextError(event);
        return;
    }
    if (type == QLatin1String("context_closed")) {
        handleContextClosed(event);
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
            entry.insert(QStringLiteral("trusted"),
                         obj.value(QStringLiteral("trusted")).toBool(false));
            devices.append(entry);
        }
        Q_EMIT deviceListUpdated(
            event.value(QStringLiteral("ok")).toBool(false), devices);
        return;
    }
    if (type == QLatin1String("to_device_undecryptable")) {
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
        const QJsonValue matches =
            event.value(QStringLiteral("matches_server"));
        const bool established = matches.isBool();
        const bool agrees = established && matches.toBool();
        if (established && !agrees) {
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
        QVariantMap snapshot = event.toVariantMap();
        snapshot.remove(QStringLiteral("type"));
        Q_EMIT cryptoHealthUpdated(snapshot);
        return;
    }
    if (type == QLatin1String("crypto_bootstrap")) {
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
    if (type == QLatin1String("thread_subscription_result")
        && !event.value(QStringLiteral("ok")).toBool()) {
        qCWarning(lcRust) << "thread subscription change refused reason="
                          << event.value(QStringLiteral("reason")).toString();
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
    if (type == QLatin1String("timeline_redaction_finished")) {
        const QString roomId = event.value(QStringLiteral("room_id")).toString();
        const QString eventId = event.value(QStringLiteral("event_id")).toString();
        if (roomId.isEmpty() || eventId.isEmpty())
            return;
        const bool ok = event.value(QStringLiteral("ok")).toBool();
        const QString category =
            ok ? QString()
               : event.value(QStringLiteral("category")).toString(
                     QStringLiteral("rejected"));
        if (!ok) {
            qCWarning(lcRust) << "redaction failed category=" << category;
            Q_EMIT errorOccurred(redactionFailureText(category));
        }
        Q_EMIT redactionFinished(roomId, eventId, ok, category);
        return;
    }
    if (type == QLatin1String("timeline_send_failed")) {
        const QString category =
            event.value(QStringLiteral("category")).toString(
                QStringLiteral("rejected"));
        qCWarning(lcRust) << "timeline send state=failed category=" << category;
        if (category == QLatin1String("reaction_rejected")) {
            Q_EMIT errorOccurred(tr("The reaction could not be applied."));
            return;
        }
        if (category == QLatin1String("redact_rejected")) {
            Q_EMIT errorOccurred(tr("The message could not be deleted."));
            return;
        }
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
        if (category == QLatin1String("retry_target_missing")) {
            Q_EMIT errorOccurred(tr("That message is no longer available to "
                                    "retry."));
            return;
        }
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
        QString message;
        if (state == QLatin1String("failed")) {
            const QString category =
                event.value(QStringLiteral("category")).toString();
            const bool sessionVerified =
                event.value(QStringLiteral("session_verified")).toBool(false);
            const bool becameVerified =
                event.value(QStringLiteral("became_verified")).toBool(false);
            qCWarning(lcRust) << "key backup recover failed category="
                              << category << "session_verified=" << sessionVerified
                              << "became_verified=" << becameVerified;
            message = BackupController::recoveryFailureText(
                category, sessionVerified, becameVerified);
        }
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
        Q_EMIT verificationSasConfirmed(
            event.value(QStringLiteral("flow_id")).toString());
        return;
    }
    if (type == QLatin1String("verification_qr_ready")) {
        const QString flowId = event.value(QStringLiteral("flow_id")).toString();
        const int modules = event.value(QStringLiteral("size")).toInt(0);
        if (modules <= 0 || modules > QrCodeStore::kMaxModules) {
            qCWarning(lcRust) << "verification QR grid rejected: bad geometry";
            return;
        }
        const QByteArray bits = QByteArray::fromBase64(
            event.value(QStringLiteral("bits_b64")).toString().toLatin1(),
            QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
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
        Q_EMIT verificationReady(
            event.value(QStringLiteral("flow_id")).toString());
        return;
    }
    if (type == QLatin1String("verification_sas_started"))
        return;

    if (type == QLatin1String("sync_stalled")) {
        const QString phase = event.value(QStringLiteral("phase")).toString();
        if (phase == QLatin1String("no_progress")) {
            qCWarning(lcRust) << "sync loop has delivered nothing, not even an error"
                              << "phase=" << phase
                              << "waited_secs="
                              << event.value(QStringLiteral("waited_secs")).toInt();
        } else {
            qCWarning(lcRust) << "sync has not received a first response"
                              << "phase=" << phase
                              << "waited_secs="
                              << event.value(QStringLiteral("waited_secs")).toInt();
        }
        return;
    }

    if (type == QLatin1String("sync_error")) {
        setState(Error);
        Q_EMIT errorOccurred(event.value(QStringLiteral("message")).toString(
            tr("Rust SDK sync failed.")));
        return;
    }

    if (type == QLatin1String("event_cache_repaired")) {
        if (!m_lifecycle.acceptsActive(eventGeneration)) {
            qCInfo(lcRust) << "event cache repair reload ignored: stale session"
                           << "generation=" << eventGeneration
                           << "active_generation=" << m_lifecycle.activeGeneration();
            return;
        }
        qCInfo(lcRust) << "event cache repaired under an open view; reopening it";
        Q_EMIT eventCacheRepaired();
        return;
    }

    if (handleRoomCommandEvent(type, event))
        return;

    if (type == QLatin1String("error")) {
        Q_EMIT errorOccurred(event.value(QStringLiteral("message")).toString(
            tr("Rust SDK backend error.")));
        return;
    }

    if (type == QLatin1String("queue_overflow")) {
        qCWarning(lcRust) << event.value(QStringLiteral("message")).toString()
                          << "— resyncing rooms and reloading the open room";
        Q_EMIT errorOccurred(event.value(QStringLiteral("message")).toString(
            tr("Rust SDK event queue overflowed.")));
        if (m_rustHandle)
            takeRustString(mx_rust_resync_rooms(m_rustHandle));
        const QString openRoom = m_timelineTracker.activeRoom();
        if (!openRoom.isEmpty()
            && admitRecoveryReopen(
                   openRoom, "queue_overflow",
                   [this, openRoom] { return roomTimelineStillOpen(openRoom); },
                   [this, openRoom] { openRoomTimeline(openRoom); }))
            openRoomTimeline(openRoom);
    }
}

void RustSdkMatrixClient::handleRoomsEvent(const QJsonArray &rooms, qint64 *parseMs,
                                           qint64 *modelMs)
{
    QElapsedTimer timed;
    if (parseMs || modelMs)
        timed.start();
    matrix::rust_rooms::applyIndexReset({m_rooms, m_roomOrder}, rooms);
    m_highlightGaps.forgetRoomsNotIn(
        QSet<QString>(m_roomOrder.cbegin(), m_roomOrder.cend()));
    observeHighlightCounts(rooms);
    if (parseMs)
        *parseMs = timed.elapsed();
    if (modelMs)
        timed.restart();
    Q_EMIT roomsChanged();
    if (modelMs)
        *modelMs = timed.elapsed();
}

void RustSdkMatrixClient::handleRoomSnapshotEvent(const QJsonArray &rooms)
{
    matrix::rust_rooms::applySnapshot({m_rooms, m_roomOrder}, rooms);
    observeHighlightCounts(rooms);
    Q_EMIT roomsChanged();
}

void RustSdkMatrixClient::observeHighlightCounts(const QJsonArray &rooms)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool live = m_initialSyncDone;
    for (const QJsonValue &value : rooms) {
        const QJsonObject obj = value.toObject();
        const QString id = obj.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || !obj.contains(QStringLiteral("highlight_count")))
            continue;
        const QString membership = obj.value(QStringLiteral("membership"))
                                       .toString(QStringLiteral("joined"));
        if (membership != QLatin1String("joined")
            || obj.value(QStringLiteral("is_space")).toBool(false)) {
            m_highlightGaps.forgetRoom(id);
            continue;
        }
        m_highlightGaps.observeCount(
            id, obj.value(QStringLiteral("highlight_count")).toInt(0), now, live);
    }
    const qint64 due = m_highlightGaps.nextDueMs();
    if (due > 0 && !m_highlightGapTimer.isActive())
        m_highlightGapTimer.start(static_cast<int>(std::clamp<qint64>(
            due - now, 0, matrix::notification_producer::HighlightGapTracker::kWindowMs)));
}

void RustSdkMatrixClient::flushHighlightGaps()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto gaps = m_highlightGaps.takeDue(now);
    for (const auto &gap : gaps) {
        qCInfo(lcRust) << "highlights not delivered as events room="
                       << matrix::e2ee::redactId(gap.first)
                       << "count=" << gap.second;
        Q_EMIT roomHighlightsUnnotified(gap.first, gap.second);
    }
    const qint64 due = m_highlightGaps.nextDueMs();
    if (due > 0)
        m_highlightGapTimer.start(static_cast<int>(std::clamp<qint64>(
            due - now, 0, matrix::notification_producer::HighlightGapTracker::kWindowMs)));
}

void RustSdkMatrixClient::resetNotificationProducerState()
{
    m_undecryptableSeen.clear();
    m_highlightGaps.reset();
    m_highlightGapTimer.stop();
}

RoomInfo RustSdkMatrixClient::roomInfoFromJson(const QJsonObject &obj) const
{
    const QString id = obj.value(QStringLiteral("id")).toString();
    return matrix::rust_rooms::roomInfoFromJson(obj, m_rooms.value(id));
}

void RustSdkMatrixClient::handleRoomListDiff(const QJsonObject &event)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    const bool removes = type == QLatin1String("room_list_remove")
        || type == QLatin1String("room_list_pop_front")
        || type == QLatin1String("room_list_pop_back")
        || type == QLatin1String("room_list_truncate")
        || type == QLatin1String("room_list_clear");
    const QStringList orderBefore = removes ? m_roomOrder : QStringList();
    const QString slotBefore = type == QLatin1String("room_list_set")
        ? m_roomOrder.value(event.value(QStringLiteral("index")).toInt(-1))
        : QString();
    if (matrix::rust_rooms::applyRoomListDiff({m_rooms, m_roomOrder}, event)) {
        if (removes) {
            const QSet<QString> after(m_roomOrder.cbegin(), m_roomOrder.cend());
            for (const QString &id : orderBefore) {
                if (!after.contains(id))
                    m_highlightGaps.forgetRoom(id);
            }
        }
        if (!slotBefore.isEmpty()
            && slotBefore != event.value(QStringLiteral("room")).toObject()
                                 .value(QStringLiteral("id")).toString())
            m_highlightGaps.forgetRoom(slotBefore);
        QJsonArray payloads = event.value(QStringLiteral("rooms")).toArray();
        if (event.value(QStringLiteral("room")).isObject())
            payloads.append(event.value(QStringLiteral("room")));
        observeHighlightCounts(payloads);
        Q_EMIT roomsChanged();
        return;
    }

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
        room.name = matrix::presentable_name::withoutDirectionalControls(
            object.value(QStringLiteral("name")).toString(room.name));
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
        room.orderedChildIds.clear();
        room.childOrderKnown = object.contains(QStringLiteral("ordered_children"));
        for (const auto &child : object.value(QStringLiteral("ordered_children")).toArray()) {
            const QString childId = child.toString();
            if (!childId.isEmpty() && childId != id && !room.orderedChildIds.contains(childId))
                room.orderedChildIds.append(childId);
        }
        room.parentSpaceIds.clear();
        for (const auto &parent : object.value(QStringLiteral("parents")).toArray()) {
            const QString parentId = parent.toString();
            if (!parentId.isEmpty()) room.parentSpaceIds.append(parentId);
        }
        m_rooms.insert(id, room);
    }
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
    TimelineEvent timelineEvent =
        matrix::rust_timeline::syncEventFromPayload(obj, roomId);
    const QString eventId = timelineEvent.eventId;
    if (roomId.isEmpty() || eventId.isEmpty())
        return;
    const bool isEdit = timelineEvent.isEdit;

    using matrix::rust_timeline::SyncRoute;
    const bool liveTimeline = m_timelineTracker.activeRoom() == roomId
        || m_timelineTracker.requestedRoom() == roomId;
    const SyncRoute route =
        matrix::rust_timeline::syncRouteFor(liveTimeline, timelineEvent);

    if (route == SyncRoute::PreviewOnly || route == SyncRoute::ObserveOnly) {
        if (timelineEvent.undecryptable)
            m_undecryptableSeen.insert(eventId, timelineEvent.backlog);
        auto roomIt = m_rooms.find(roomId);
        if (roomIt != m_rooms.end() && !isEdit) {
            const QString body =
                matrix::preview::normalizePreviewText(timelineEvent.body);
            if (!body.isEmpty())
                roomIt->setPreviewText(body);
            roomIt->raiseActivity(timestampFromMs(static_cast<qint64>(
                obj.value(QStringLiteral("timestamp_ms")).toDouble(0))));
            Q_EMIT roomUpdated(roomId);
        }
        if (route == SyncRoute::ObserveOnly) {
            timelineEvent.senderDisplayName =
                displayNameFor(roomId, timelineEvent.sender);
            if (timelineEvent.undecryptable && timelineEvent.body.isEmpty()) {
                timelineEvent.body = tr("[unable to decrypt yet]");
                timelineEvent.type = TimelineEvent::Notice;
            }
            noteNotifiedEvent(roomId, timelineEvent);
            Q_EMIT liveEventObserved(roomId, timelineEvent);
        }
        return;
    }

    if (isEdit)
        m_timelines.remove(roomId);

    auto &timeline = m_timelines[roomId];
    for (const auto &existing : timeline) {
        if (existing.eventId == eventId)
            return;
    }
    const bool threadedReply = !timelineEvent.threadRootId.isEmpty()
        && timelineEvent.threadRootId != eventId;
    if (!threadedReply) {
        if (m_forwardedEditIds.contains(eventId))
            return;
        m_forwardedEditIds.insert(eventId, false);
    }

    if (!timelineEvent.replyToEventId.isEmpty()) {
        for (const auto &existing : std::as_const(timeline)) {
            if (existing.eventId == timelineEvent.replyToEventId) {
                timelineEvent.replyToSenderId = existing.sender;
                break;
            }
        }
    }
    timelineEvent.senderDisplayName = displayNameFor(roomId, timelineEvent.sender);

    if (timelineEvent.undecryptable && timelineEvent.body.isEmpty()) {
        timelineEvent.body = tr("[unable to decrypt yet]");
        timelineEvent.type = TimelineEvent::Notice;
    }

    if (timelineEvent.isEncrypted) {
        qCDebug(lcE2ee) << "encrypted-event"
                        << "room=" << matrix::e2ee::redactId(roomId)
                        << "event=" << matrix::e2ee::redactId(timelineEvent.eventId)
                        << (timelineEvent.undecryptable ? "state=utd" : "state=decryptable")
                        << "error=" << (timelineEvent.errorKind.isEmpty()
                                            ? QStringLiteral("none")
                                            : timelineEvent.errorKind);
    }

    if (route == SyncRoute::MirrorAndAppend)
        matrix::rust_timeline::appendBounded(timeline, timelineEvent);
    noteNotifiedEvent(roomId, timelineEvent);
    Q_EMIT eventAppended(roomId, timelineEvent);

    auto roomIt = m_rooms.find(roomId);
    if (roomIt != m_rooms.end()) {
        const bool countsAsActivity = roomrecency::raisesRecency(timelineEvent)
            && !isEdit
            && timelineEvent.type != TimelineEvent::StateChange;
        if (countsAsActivity) {
            if (roomrecency::replacesPreview(timelineEvent))
                setPreviewFrom(*roomIt, timelineEvent);
            roomIt->raiseActivity(timelineEvent.timestamp);
            Q_EMIT roomUpdated(roomId);
        }
    }
}

void RustSdkMatrixClient::noteNotifiedEvent(const QString &roomId,
                                            const TimelineEvent &event)
{
    if (roomId.isEmpty() || event.eventId.isEmpty())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (event.undecryptable)
        m_undecryptableSeen.insert(event.eventId, event.backlog);
    if (event.backlog) {
        m_highlightGaps.noteBacklog(roomId, now);
        return;
    }
    const bool highlighted = event.pushHighlight
        || (event.pushVerdict == TimelineEvent::PushVerdict::Unknown
            && (event.mentionsMe || event.mentionsRoom));
    if (highlighted)
        m_highlightGaps.creditHighlight(roomId, now);
}

void RustSdkMatrixClient::emitDecryptedOnce(const QString &roomId,
                                            TimelineEvent event)
{
    bool wasBacklog = false;
    if (roomId.isEmpty() || !m_undecryptableSeen.take(event.eventId, &wasBacklog))
        return;
    event.roomId = roomId;
    event.backlog = event.backlog || wasBacklog;
    noteNotifiedEvent(roomId, event);
    qCDebug(lcE2ee) << "late-decryption announced"
                    << "room=" << matrix::e2ee::redactId(roomId)
                    << "event=" << matrix::e2ee::redactId(event.eventId)
                    << "backlog=" << event.backlog;
    Q_EMIT eventDecrypted(roomId, event);
}

void RustSdkMatrixClient::handleEventDecrypted(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    TimelineEvent decrypted = matrix::rust_timeline::syncEventFromPayload(
        event.value(QStringLiteral("event")).toObject(), roomId);
    if (roomId.isEmpty() || decrypted.eventId.isEmpty() || decrypted.undecryptable)
        return;
    decrypted.senderDisplayName = displayNameFor(roomId, decrypted.sender);

    const bool liveTimeline = m_timelineTracker.activeRoom() == roomId
        || m_timelineTracker.requestedRoom() == roomId;
    if (!liveTimeline && decrypted.isEdit)
        m_timelines.remove(roomId);
    if (!liveTimeline) {
        auto mirrorIt = m_timelines.find(roomId);
        if (mirrorIt != m_timelines.end()) {
            for (auto &row : *mirrorIt) {
                if (row.eventId == decrypted.eventId) {
                    if (row.undecryptable && !row.redacted)
                        row = decrypted;
                    break;
                }
            }
        }
    }
    emitDecryptedOnce(roomId, decrypted);
}

void RustSdkMatrixClient::handleEventRedacted(const QJsonObject &event)
{
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const QString redacts = event.value(QStringLiteral("redacts")).toString();
    if (roomId.isEmpty() || redacts.isEmpty())
        return;
    m_undecryptableSeen.take(redacts);

    const bool liveTimeline = m_timelineTracker.activeRoom() == roomId
        || m_timelineTracker.requestedRoom() == roomId;
    if (liveTimeline) {
        const auto mirrorIt = m_timelines.constFind(roomId);
        if (mirrorIt != m_timelines.cend()) {
            for (const auto &row : *mirrorIt) {
                if (row.eventId == redacts)
                    return;
            }
        }
    } else {
        bool found = false;
        auto mirrorIt = m_timelines.find(roomId);
        if (mirrorIt != m_timelines.end()) {
            for (auto &row : *mirrorIt) {
                if (row.eventId == redacts) {
                    row.redacted = true;
                    row.body.clear();
                    row.formattedBody.clear();
                    found = true;
                    break;
                }
            }
        }
        if (!found)
            m_timelines.remove(roomId);
    }
    Q_EMIT eventRedacted(roomId, redacts);
}

void RustSdkMatrixClient::emitDiffNotificationFacts(
    const QString &roomId, const QString &threadRootId,
    const matrix::rust_timeline::DiffOutcome &outcome)
{
    using matrix::rust_timeline::DiffOutcome;
    if (roomId.isEmpty())
        return;
    const auto forConsumers = [&](TimelineEvent item) {
        item.roomId = roomId;
        if (!threadRootId.isEmpty() && item.threadRootId.isEmpty()
            && item.eventId != threadRootId)
            item.threadRootId = threadRootId;
        return item;
    };
    switch (outcome.kind) {
    case DiffOutcome::Appended:
        for (const auto &item : outcome.items) {
            if (!item.isLocalEcho && !item.isVirtual())
                noteNotifiedEvent(roomId, item);
        }
        break;
    case DiffOutcome::Inserted:
        if (outcome.liveRemoteInsert && !outcome.items.isEmpty()) {
            const TimelineEvent item = forConsumers(outcome.items.first());
            noteNotifiedEvent(roomId, item);
            Q_EMIT liveEventObserved(roomId, item);
        }
        break;
    case DiffOutcome::Changed:
        if (outcome.items.isEmpty())
            break;
        if (outcome.items.first().edited && qEnvironmentVariableIsSet("LIGHTNING_EDIT_TRACE")) {
            qCInfo(lcRust) << "edit-trace stage=bridge-set"
                          << "room=" << matrix::e2ee::redactId(roomId)
                          << "event=" << matrix::e2ee::redactId(outcome.items.first().eventId)
                          << "thread=" << !threadRootId.isEmpty()
                          << "index=" << outcome.index;
        }
        if (outcome.becameRedacted) {
            m_undecryptableSeen.take(outcome.items.first().eventId);
            Q_EMIT eventRedacted(roomId, outcome.items.first().eventId);
        } else if (outcome.becameDecrypted) {
            emitDecryptedOnce(roomId, forConsumers(outcome.items.first()));
        }
        break;
    default:
        break;
    }
}

void RustSdkMatrixClient::updateRoomPreviewFrom(
    const QString &roomId, const QList<TimelineEvent> &newestFirstCandidates)
{
    auto roomIt = m_rooms.find(roomId);
    if (roomIt == m_rooms.end())
        return;
    for (const auto &event : newestFirstCandidates) {
        if (!roomrecency::raisesRecency(event))
            continue;
        if (!roomrecency::replacesPreview(event)) {
            roomIt->raiseActivity(event.timestamp);
            Q_EMIT roomUpdated(roomId);
            return;
        }
        QString actor;
        if (event.type == TimelineEvent::StateChange
            && event.senderDisplayName.isEmpty()) {
            const QString known = displayNameFor(roomId, event.sender);
            if (known != event.sender)
                actor = known;
        }
        setPreviewFrom(*roomIt, event, actor);
        roomIt->raiseActivity(event.timestamp);
        Q_EMIT roomUpdated(roomId);
        return;
    }
}

void RustSdkMatrixClient::handleTimelineReset(const QJsonObject &event)
{
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
                if (row < first || row >= first + count)
                    changedItems.append({row, timelineIt->at(row)});
            }
        }
    }

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
    const QString diffOp = event.value(QStringLiteral("op")).toString();
    stalltrace::Scope stallScope(diffOp == QLatin1String("set")
                                     ? "timeline-diff-set"
                                     : "timeline-diff");
    const QString roomId = event.value(QStringLiteral("room_id")).toString();
    const auto generation = static_cast<quint64>(
        event.value(QStringLiteral("room_generation")).toDouble(0));
    if (!m_timelineTracker.accepts(roomId, generation)) {
        flushTimelineInsertBatch();
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
    const bool startsPaginationRange = m_timelineInsertBatchCount == 0
        && (insertionIndex == 0 || insertionIndex == 1);
    const bool extendsPaginationRange = sameBatch
        && insertionIndex >= m_timelineInsertBatchFirst
        && insertionIndex <= m_timelineInsertBatchFirst
                             + m_timelineInsertBatchCount;
    const bool canBatchInsertion = m_coalesceTimelineInserts
        && insertionIndex >= 0
        && (startsPaginationRange || extendsPaginationRange);
    const bool canDeferSet = m_coalesceTimelineInserts && sameBatch
        && op == QLatin1String("set");
    if (m_timelineInsertBatchCount > 0
        && !canBatchInsertion && !canDeferSet)
        flushTimelineInsertBatch();

    using matrix::rust_timeline::DiffOutcome;
    auto &mirror = m_timelines[roomId];
    const DiffOutcome outcome =
        matrix::rust_timeline::applyTimelineDiff(mirror, event, roomId);
    const auto notificationFacts = qScopeGuard([this, roomId, outcome] {
        emitDiffNotificationFacts(roomId, QString(), outcome);
    });

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
    }

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
        qCWarning(lcRust) << "timeline invalid diff rejected"
                          << "op=" << event.value(QStringLiteral("op")).toString()
                          << "index=" << event.value(QStringLiteral("index")).toInt(-1)
                          << "mirror_size=" << mirror.size();
        if (admitRecoveryReopen(
                roomId, "invalid_diff",
                [this, roomId] { return roomTimelineStillOpen(roomId); },
                [this, roomId] { openRoomTimeline(roomId); }))
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
            state.lastFullyFiltered = fullyFiltered;
            if (!state.reachedStart && fullyFiltered) {
                state.batchSize = static_cast<unsigned short>(
                    qMin<quint64>(current * 3u, kPaginationMaxBatch));
            } else {
                state.batchSize = 0;
            }
        }
        qCInfo(lcRust) << "timeline pagination complete reached_start="
                       << state.reachedStart
                       << "walkedPages="
                       << event.value(QStringLiteral("walked_pages")).toInt(1)
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
                              .toDouble(0)
                       << "startCheck="
                       << event.value(QStringLiteral("start_check"))
                              .toString();
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

void RustSdkMatrixClient::acknowledgeInterruptedSetup()
{
    if (m_rustHandle)
        mx_rust_acknowledge_interrupted_setup(m_rustHandle);
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
    keyBytes.fill('\0');
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "key backup recover not started:" << result;
        Q_EMIT keyBackupResult(QStringLiteral("failed"),
                               tr("Recovery could not start: this session is "
                                  "not signed in."));
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

void RustSdkMatrixClient::checkOwnIdentityKey()
{
    if (!m_loggedIn || !m_rustHandle)
        return;
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
    QByteArray pathBytes = filePath.toUtf8();
    QByteArray passphraseBytes = passphrase.toUtf8();
    const QString r = takeRustString(mx_rust_import_room_keys(
        m_rustHandle, pathBytes.constData(), passphraseBytes.constData()));
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
    const auto refuse = [this, opId] {
        QMetaObject::invokeMethod(this, [this, opId] {
            Q_EMIT ownDisplayNameChanged(opId, false, QString());
        }, Qt::QueuedConnection);
    };
    if (!m_rustHandle || !m_loggedIn) {
        refuse();
        return;
    }
    const QByteArray payload = name.toUtf8();
    const QString result = takeRustString(mx_rust_set_display_name(
        m_rustHandle, payload.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "display-name write rejected";
        refuse();
    }
}

void RustSdkMatrixClient::setOwnAvatar(const QString &localPath, quint64 opId)
{
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
    const QString lowered = url.trimmed().toLower();
    if (!m_rustHandle
        || !lowered.startsWith(QLatin1String("https://")))
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray target = url.toUtf8();
    const QString result = takeRustString(mx_rust_get_url_preview(
        m_rustHandle, target.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "url preview rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::fetchLinkMedia(const QString &url,
                                            const QString &linkKey,
                                            int expect, int timeoutClass)
{
    if (!m_rustHandle || !linkKey.startsWith(QLatin1String("link:"))
        || !url.trimmed().toLower().startsWith(QLatin1String("https://")))
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray target = url.toUtf8();
    const QByteArray key = linkKey.toUtf8();
    const QString result = takeRustString(mx_rust_link_media_fetch(
        m_rustHandle, target.constData(), key.constData(),
        static_cast<unsigned int>(qBound(0, expect, 1)),
        static_cast<unsigned int>(qBound(0, timeoutClass, 2)), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "link media fetch refused reason=" << result;
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::gifGet(const QString &url)
{
    if (!m_rustHandle || !url.trimmed().toLower().startsWith(QLatin1String("https://")))
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray target = url.toUtf8();
    const QString result =
        takeRustString(mx_rust_gif_get(m_rustHandle, target.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "gif request rejected";
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
        qCWarning(lcRust) << "gif download rejected";
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

quint64 RustSdkMatrixClient::requestRoomOwnPermissions(const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_room_own_permissions(m_rustHandle, room.constData(), opId));
    return result.isEmpty() ? opId : 0;
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

quint64 RustSdkMatrixClient::requestModerationPlan(const QStringList &roomIds,
                                                   const QString &userId,
                                                   const QString &op)
{
    if (!m_rustHandle || roomIds.isEmpty() || userId.isEmpty())
        return 0;
    int code = -1;
    if (op == QLatin1String("kick"))
        code = 0;
    else if (op == QLatin1String("ban"))
        code = 1;
    else if (op == QLatin1String("unban"))
        code = 2;
    if (code < 0)
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray rooms = QJsonDocument(QJsonArray::fromStringList(roomIds))
                                 .toJson(QJsonDocument::Compact);
    const QByteArray user = userId.toUtf8();
    const QString result = takeRustString(mx_rust_moderation_plan(
        m_rustHandle, rooms.constData(), user.constData(),
        static_cast<unsigned char>(code), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::requestRoomClosurePlan(const QStringList &roomIds,
                                                    const QStringList &parentIds)
{
    if (!m_rustHandle || roomIds.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray rooms = QJsonDocument(QJsonArray::fromStringList(roomIds))
                                 .toJson(QJsonDocument::Compact);
    const QByteArray parents =
        QJsonDocument(QJsonArray::fromStringList(parentIds))
            .toJson(QJsonDocument::Compact);
    const QString result = takeRustString(mx_rust_room_closure_plan(
        m_rustHandle, rooms.constData(), parents.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::closeRoom(const QString &roomId,
                                       const QString &reason, bool leave,
                                       const QStringList &unlistFromSpaceIds)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray why = reason.toUtf8();
    const QByteArray spaces =
        QJsonDocument(QJsonArray::fromStringList(unlistFromSpaceIds))
            .toJson(QJsonDocument::Compact);
    const QString result = takeRustString(mx_rust_close_room(
        m_rustHandle, room.constData(), why.constData(), leave ? 1 : 0,
        spaces.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::clearEventCache()
{
    if (!m_rustHandle)
        return 0;
    const quint64 opId = nextOpId();
    const QString result = takeRustString(mx_rust_event_cache_clear(
        m_rustHandle, static_cast<unsigned long long>(opId)));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::requestHomeserverInfo()
{
    if (!m_rustHandle)
        return 0;
    const quint64 opId = nextOpId();
    const QString result = takeRustString(mx_rust_homeserver_info(
        m_rustHandle, static_cast<unsigned long long>(opId)));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::requestAccountDataListing(const QString &roomId)
{
    if (!m_rustHandle)
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_account_data_listing(
        m_rustHandle, room.constData(), static_cast<unsigned long long>(opId)));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::requestUserClients(const QString &userId)
{
    if (!m_rustHandle || userId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray user = userId.toUtf8();
    const QString result = takeRustString(mx_rust_user_clients(
        m_rustHandle, user.constData(), static_cast<unsigned long long>(opId)));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::requestServerAdminStatus()
{
    if (!m_rustHandle)
        return 0;
    const quint64 opId = nextOpId();
    const QString result =
        takeRustString(mx_rust_server_admin_status(m_rustHandle, opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::adminDeleteRoom(const QString &roomId, bool block)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_admin_delete_room(
        m_rustHandle, room.constData(), block ? 1 : 0, opId));
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

void RustSdkMatrixClient::requestActivitySeedPage(const QString &from, int limit)
{
    if (!m_loggedIn || !m_rustHandle || from.isEmpty())
        return;
    const QByteArray token = from.toUtf8();
    const QString result = takeRustString(mx_rust_request_activity_seed_page(
        m_rustHandle, static_cast<unsigned int>(qBound(1, limit, 100)),
        token.constData()));
    if (!result.isEmpty())
        Q_EMIT activitySeedPageFailed(result.left(48));
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

quint64 RustSdkMatrixClient::startRoomExport(const QString &roomId, int mode,
                                             qint64 limit, qint64 fromMs,
                                             qint64 toMs)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QString error = takeRustString(mx_rust_room_export_start(
        m_rustHandle, room.constData(), mode,
        static_cast<unsigned long long>(qMax<qint64>(0, limit)),
        static_cast<unsigned long long>(qMax<qint64>(0, fromMs)),
        static_cast<unsigned long long>(qMax<qint64>(0, toMs)),
        static_cast<unsigned long long>(opId)));
    if (!error.isEmpty())
        return 0;
    return opId;
}

void RustSdkMatrixClient::cancelRoomExport(quint64 opId)
{
    if (!m_rustHandle || opId == 0)
        return;
    mx_rust_room_export_cancel(m_rustHandle,
                               static_cast<unsigned long long>(opId));
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

quint64 RustSdkMatrixClient::clearStoredMedia()
{
    if (!m_rustHandle)
        return 0;
    const quint64 opId = nextOpId();
    const QString result = takeRustString(mx_rust_media_cache_clear(
        m_rustHandle, static_cast<unsigned long long>(opId)));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "clear stored media rejected";
        return 0;
    }
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

bool RustSdkMatrixClient::clearSearchIndex()
{
    if (!m_rustHandle)
        return false;
    return takeRustString(mx_rust_search_index_clear(m_rustHandle)).isEmpty();
}

quint64 RustSdkMatrixClient::indexAllRooms(bool resumeOnly)
{
    if (!m_rustHandle)
        return 0;
    const quint64 opId = nextOpId();
    const QString error = takeRustString(mx_rust_search_index_all_start(
        m_rustHandle, resumeOnly ? 1u : 0u,
        static_cast<unsigned long long>(opId)));
    return error.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::pauseIndexAll()
{
    if (!m_rustHandle)
        return 0;
    const quint64 opId = nextOpId();
    takeRustString(mx_rust_search_index_all_pause(
        m_rustHandle, static_cast<unsigned long long>(opId)));
    return opId;
}

quint64 RustSdkMatrixClient::cancelIndexAll()
{
    if (!m_rustHandle)
        return 0;
    const quint64 opId = nextOpId();
    takeRustString(mx_rust_search_index_all_cancel(
        m_rustHandle, static_cast<unsigned long long>(opId)));
    return opId;
}

quint64 RustSdkMatrixClient::requestIndexAllStatus()
{
    if (!m_rustHandle)
        return 0;
    const quint64 opId = nextOpId();
    takeRustString(mx_rust_search_index_all_status(
        m_rustHandle, static_cast<unsigned long long>(opId)));
    return opId;
}

void RustSdkMatrixClient::setIndexAllHold(unsigned bits)
{
    m_indexAllHold = bits;
    if (!m_rustHandle)
        return;
    takeRustString(mx_rust_search_index_all_hold(m_rustHandle, bits));
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

quint64 RustSdkMatrixClient::crossSigningAction(const QString &action,
                                                const QString &recoveryKey,
                                                bool replaceRecoveryKeyConfirmed)
{
    if (!m_rustHandle || action.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray value = action.toUtf8();
    // SENSITIVE: the current recovery key. Converted once, zeroed after the
    // call; Rust scrubs its own copy once the secret store is opened.
    QByteArray keyBytes = recoveryKey.toUtf8();
    const QString result = takeRustString(mx_rust_cross_signing_action(
        m_rustHandle, value.constData(), keyBytes.constData(),
        replaceRecoveryKeyConfirmed ? 1 : 0, opId));
    volatile char *raw = keyBytes.data();
    for (int i = 0; i < keyBytes.size(); ++i)
        raw[i] = 0;
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::requestBackupProgress()
{
    if (!m_rustHandle)
        return 0;
    static quint64 sequence = 0;
    const quint64 id = ++sequence;
    takeRustString(mx_rust_request_backup_progress(m_rustHandle, id));
    return id;
}

void RustSdkMatrixClient::readAccountPref(const QString &prefType)
{
    if (!m_rustHandle) {
        Q_EMIT accountPrefReadFailed(prefType, QStringLiteral("signed_out"));
        return;
    }
    const QByteArray type = prefType.toUtf8();
    const QString refused = takeRustString(
        mx_rust_account_pref_read(m_rustHandle, type.constData()));
    if (!refused.isEmpty()) {
        qCWarning(lcRust) << "account preference read refused:"
                          << refused.left(80);
        Q_EMIT accountPrefReadFailed(prefType, QStringLiteral("read_refused"));
    }
}

void RustSdkMatrixClient::writeAccountPref(const QString &prefType,
                                           const QVariantMap &content)
{
    if (!m_rustHandle) {
        Q_EMIT accountPrefWritten(prefType, false, QStringLiteral("signed_out"));
        return;
    }
    const QByteArray type = prefType.toUtf8();
    const QByteArray json = QJsonDocument(QJsonObject::fromVariantMap(content))
                                .toJson(QJsonDocument::Compact);
    const QString refused = takeRustString(mx_rust_account_pref_write(
        m_rustHandle, type.constData(), json.constData()));
    if (!refused.isEmpty()) {
        qCWarning(lcRust) << "account preference write refused:"
                          << refused.left(80);
        Q_EMIT accountPrefWritten(prefType, false, refused.left(40));
    }
}

void RustSdkMatrixClient::requestInvitePolicySupport()
{
    if (!m_rustHandle)
        return;
    const QString refused =
        takeRustString(mx_rust_invite_policy_support(m_rustHandle));
    if (!refused.isEmpty()) {
        qCWarning(lcRust) << "invite policy support probe refused:"
                          << refused.left(80);
        Q_EMIT invitePolicySupportReceived(false, false, false);
    }
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

quint64 RustSdkMatrixClient::callInvite(const QString &roomId,
                                        const QString &callId,
                                        const QString &partyId,
                                        const QString &offerType,
                                        const QString &offerSdp,
                                        quint64 lifetimeMs,
                                        const QString &invitee,
                                        const QString &streamMetadataJson)
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
    const QByteArray metadata = streamMetadataJson.toUtf8();
    const QString result = takeRustString(mx_rust_calls_invite(
        m_rustHandle, room.constData(), call.constData(), party.constData(),
        type.constData(), sdp.constData(), lifetimeMs, target.constData(),
        metadata.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::callAnswer(const QString &roomId,
                                        const QString &callId,
                                        const QString &partyId,
                                        const QString &answerType,
                                        const QString &answerSdp,
                                        const QString &streamMetadataJson)
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
    const QByteArray metadata = streamMetadataJson.toUtf8();
    const QString result = takeRustString(mx_rust_calls_answer(
        m_rustHandle, room.constData(), call.constData(), party.constData(),
        type.constData(), sdp.constData(), metadata.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::callNegotiate(const QString &roomId,
                                           const QString &callId,
                                           const QString &partyId,
                                           const QString &type,
                                           const QString &sdp,
                                           quint64 lifetimeMs,
                                           const QString &streamMetadataJson)
{
    if (!m_rustHandle || roomId.isEmpty() || callId.isEmpty()
        || partyId.isEmpty() || sdp.trimmed().isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray call = callId.toUtf8();
    const QByteArray party = partyId.toUtf8();
    const QByteArray typeBytes = type.toUtf8();
    const QByteArray sdpBytes = sdp.toUtf8();
    const QByteArray metadata = streamMetadataJson.toUtf8();
    const QString result = takeRustString(mx_rust_calls_negotiate(
        m_rustHandle, room.constData(), call.constData(), party.constData(),
        typeBytes.constData(), sdpBytes.constData(), lifetimeMs,
        metadata.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::callStreamMetadata(const QString &roomId,
                                                const QString &callId,
                                                const QString &partyId,
                                                const QString &streamMetadataJson)
{
    if (!m_rustHandle || roomId.isEmpty() || callId.isEmpty()
        || partyId.isEmpty() || streamMetadataJson.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray call = callId.toUtf8();
    const QByteArray party = partyId.toUtf8();
    const QByteArray metadata = streamMetadataJson.toUtf8();
    const QString result = takeRustString(mx_rust_calls_stream_metadata(
        m_rustHandle, room.constData(), call.constData(), party.constData(),
        metadata.constData(), opId));
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
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(
        mx_rust_rtc_transports(m_rustHandle, room.constData(), opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::rtcPublishMembership(const QString &roomId,
                                                 const QString &focusUrl,
                                                 const QString &intent,
                                                 bool multiSfu)
{
    if (!m_rustHandle || roomId.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray focus = focusUrl.toUtf8();
    const QByteArray callIntent = intent.toUtf8();
    const QString result = takeRustString(mx_rust_rtc_publish_membership(
        m_rustHandle, room.constData(), focus.constData(),
        callIntent.constData(), multiSfu, opId));
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
    m_sfuConnectOp = result.isEmpty() ? opId : 0;
    m_sfuSessionGeneration = 0;
    return m_sfuConnectOp;
}

quint64 RustSdkMatrixClient::sfuConnectMany(const QStringList &focusUrls,
                                            const QString &roomId)
{
    if (!m_rustHandle || roomId.isEmpty() || focusUrls.isEmpty()
        || focusUrls.size() > 8)
        return 0;
    QJsonArray rows;
    for (int i = 0; i < focusUrls.size(); ++i) {
        if (focusUrls.at(i).isEmpty())
            return 0;
        rows.append(QJsonObject{{QStringLiteral("id"), i},
                                {QStringLiteral("url"), focusUrls.at(i)}});
    }
    const quint64 opId = nextOpId();
    const QByteArray plan = QJsonDocument(rows).toJson(QJsonDocument::Compact);
    const QByteArray room = roomId.toUtf8();
    const QString result = takeRustString(mx_rust_sfu_connect_many(
        m_rustHandle, plan.constData(), room.constData(), opId));
    m_sfuConnectOp = result.isEmpty() ? opId : 0;
    m_sfuSessionGeneration = 0;
    return m_sfuConnectOp;
}

void RustSdkMatrixClient::sfuLocalDescriptionFor(
    quint64 generation, int connectionId, const QString &kind,
    const QString &target, const QString &sdp)
{
    if (!m_rustHandle || generation == 0 || connectionId < 0
        || connectionId >= 8 || sdp.isEmpty())
        return;
    const QByteArray k = kind.toUtf8();
    const QByteArray t = target.toUtf8();
    const QByteArray s = sdp.toUtf8();
    takeRustString(mx_rust_sfu_local_description_for(
        m_rustHandle, generation, static_cast<unsigned int>(connectionId),
        k.constData(), t.constData(), s.constData()));
}

void RustSdkMatrixClient::sfuLocalCandidateFor(
    quint64 generation, int connectionId, const QString &target,
    const QString &candidateInit)
{
    if (!m_rustHandle || generation == 0 || connectionId < 0
        || connectionId >= 8 || candidateInit.isEmpty())
        return;
    const QByteArray t = target.toUtf8();
    const QByteArray c = candidateInit.toUtf8();
    takeRustString(mx_rust_sfu_local_candidate_for(
        m_rustHandle, generation, static_cast<unsigned int>(connectionId),
        t.constData(), c.constData()));
}

void RustSdkMatrixClient::sfuLocalDescription(const QString &kind,
                                              const QString &target,
                                              const QString &sdp)
{
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

void RustSdkMatrixClient::sfuAddTrackFor(
    quint64 generation, int connectionId, const QString &cid,
    const QString &name, int kind, int width, int height,
    bool screenShare, bool encrypted)
{
    if (!m_rustHandle || generation == 0 || connectionId < 0
        || connectionId >= 8 || cid.isEmpty())
        return;
    const QByteArray c = cid.toUtf8();
    const QByteArray n = name.toUtf8();
    takeRustString(mx_rust_sfu_add_track_for(
        m_rustHandle, generation, static_cast<unsigned int>(connectionId),
        c.constData(), n.constData(), kind,
        static_cast<unsigned int>(qMax(0, width)),
        static_cast<unsigned int>(qMax(0, height)),
        screenShare ? 1 : 0, encrypted ? 1 : 0));
}

void RustSdkMatrixClient::sfuAddShareWithBackupFor(
    quint64 generation, const QString &cid, const QString &backupCid,
    int width, int height, bool encrypted)
{
    if (!m_rustHandle || !generation || cid.isEmpty() || backupCid.isEmpty())
        return;
    const auto c = cid.toUtf8();
    const auto b = backupCid.toUtf8();
    takeRustString(mx_rust_sfu_add_share_backup_for(
        m_rustHandle, generation, c.constData(), b.constData(),
        static_cast<unsigned int>(qMax(0, width)),
        static_cast<unsigned int>(qMax(0, height)), encrypted ? 1 : 0));
}

void RustSdkMatrixClient::sfuMuteTrack(const QString &sid, bool muted)
{
    if (!m_rustHandle || sid.isEmpty())
        return;
    const QByteArray s = sid.toUtf8();
    takeRustString(
        mx_rust_sfu_mute_track(m_rustHandle, s.constData(), muted ? 1 : 0));
}

void RustSdkMatrixClient::sfuMuteTrackFor(
    quint64 generation, int connectionId, const QString &sid, bool muted)
{
    if (!m_rustHandle || generation == 0 || connectionId < 0
        || connectionId >= 8 || sid.isEmpty())
        return;
    const QByteArray s = sid.toUtf8();
    takeRustString(mx_rust_sfu_mute_track_for(
        m_rustHandle, generation, static_cast<unsigned int>(connectionId),
        s.constData(), muted ? 1 : 0));
}

void RustSdkMatrixClient::sfuDisconnect()
{
    m_sfuConnectOp = 0;
    m_sfuSessionGeneration = 0;
    if (!m_rustHandle)
        return;
    takeRustString(mx_rust_sfu_disconnect(m_rustHandle));
}

quint64 RustSdkMatrixClient::rtcSendCallReaction(
    const QString &roomId, const QString &membershipEventId,
    const QString &emoji, const QString &name)
{
    if (!m_rustHandle || roomId.isEmpty() || membershipEventId.isEmpty()
        || emoji.isEmpty()) {
        return 0;
    }
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray membership = membershipEventId.toUtf8();
    const QByteArray emojiBytes = emoji.toUtf8();
    const QByteArray nameBytes = name.toUtf8();
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

quint64 RustSdkMatrixClient::requestPushRules()
{
    if (!m_rustHandle)
        return 0;
    const quint64 opId = nextOpId();
    const QString result =
        takeRustString(mx_rust_push_rules_snapshot(m_rustHandle, opId));
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::writePushRule(const QString &kind,
                                           const QString &key,
                                           const QString &value)
{
    if (!m_rustHandle || kind.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray k = kind.toUtf8();
    const QByteArray ky = key.toUtf8();
    const QByteArray v = value.toUtf8();
    const QString result = takeRustString(mx_rust_push_rules_write(
        m_rustHandle, k.constData(), ky.constData(), v.constData(), opId));
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
    QByteArray passwordBytes = password.toUtf8();
    const QString result = takeRustString(mx_rust_uia_submit_password(
        m_rustHandle, uiaId, passwordBytes.constData()));
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

quint64 RustSdkMatrixClient::changePassword(const QString &currentPassword,
                                            const QString &newPassword,
                                            bool logoutDevices)
{
    if (!m_rustHandle || currentPassword.isEmpty() || newPassword.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    QByteArray currentBytes = currentPassword.toUtf8();
    QByteArray newBytes = newPassword.toUtf8();
    const QString result = takeRustString(mx_rust_change_password(
        m_rustHandle, currentBytes.constData(), newBytes.constData(),
        logoutDevices, opId));
    for (QByteArray *bytes : { &currentBytes, &newBytes }) {
        volatile char *raw = bytes->data();
        for (qsizetype i = 0; i < bytes->size(); ++i)
            raw[i] = 0;
    }
    return result.isEmpty() ? opId : 0;
}

quint64 RustSdkMatrixClient::exportRoomKeys(const QString &filePath,
                                            const QString &passphrase)
{
    if (!m_loggedIn || !m_rustHandle || filePath.isEmpty()
        || passphrase.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray pathBytes = filePath.toUtf8();
    QByteArray passphraseBytes = passphrase.toUtf8();
    const QString result = takeRustString(mx_rust_export_room_keys(
        m_rustHandle, pathBytes.constData(), passphraseBytes.constData(),
        opId));
    volatile char *raw = passphraseBytes.data();
    for (qsizetype i = 0; i < passphraseBytes.size(); ++i)
        raw[i] = 0;
    if (!result.isEmpty()) {
        const QString category = result.startsWith(QLatin1String("error: "))
            ? result.mid(7) : QStringLiteral("failed");
        qCWarning(lcRust) << "room key export refused category=" << category;
        QMetaObject::invokeMethod(this, [this, opId, category] {
            Q_EMIT roomKeyExportFinished(opId, false, 0, category);
        }, Qt::QueuedConnection);
        return opId;
    }
    qCInfo(lcRust) << "room key export started";
    return opId;
}

quint64 RustSdkMatrixClient::probePasswordChange()
{
    if (!m_rustHandle)
        return 0;
    const quint64 opId = nextOpId();
    const QString result = takeRustString(
        mx_rust_password_change_probe(m_rustHandle, opId));
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

quint64 RustSdkMatrixClient::setSpaceChildOrder(const QString &spaceId,
                                               const QStringList &roomIds)
{
    if (!m_rustHandle || spaceId.isEmpty() || roomIds.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray space = spaceId.toUtf8();
    const QByteArray ids = QJsonDocument(QJsonArray::fromStringList(roomIds)).toJson(QJsonDocument::Compact);
    const QString result = takeRustString(mx_rust_set_space_child_order(
        m_rustHandle, space.constData(), ids.constData(), opId));
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
                                            bool animated, qint64 durationMs,
                                            const QString &replyToEventId)
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
    const QByteArray replyBytes = replyToEventId.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_send_attachment(
        m_rustHandle, room.constData(), path.constData(), mimeBytes.constData(),
        captionBytes.constData(),
        static_cast<unsigned long long>(qMax(0, width)),
        static_cast<unsigned long long>(qMax(0, height)),
        animated ? 1 : 0,
        static_cast<unsigned long long>(qMax<qint64>(0, durationMs)), replyBytes.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "attachment send rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::sendImageWithThumbnail(const QString &roomId,
                                                    const QString &localPath,
                                                    const QString &mime,
                                                    const QString &caption,
                                                    int width, int height,
                                                    const QByteArray &thumbnail,
                                                    int thumbnailWidth,
                                                    int thumbnailHeight,
                                                    const QString &replyToEventId)
{
    if (!m_rustHandle || roomId.isEmpty() || localPath.isEmpty() || mime.isEmpty())
        return 0;
    if (!timelineActiveFor(roomId)) {
        qCWarning(lcRust) << "image send requires the open room timeline";
        return 0;
    }
    const bool hasThumb = !thumbnail.isEmpty() && thumbnailWidth > 0
        && thumbnailHeight > 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray path = localPath.toUtf8();
    const QByteArray mimeBytes = mime.toUtf8();
    const QByteArray captionBytes = caption.toUtf8();
    const QByteArray replyBytes = replyToEventId.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_send_image(
        m_rustHandle, room.constData(), path.constData(),
        mimeBytes.constData(), captionBytes.constData(),
        static_cast<unsigned long long>(qMax(0, width)),
        static_cast<unsigned long long>(qMax(0, height)),
        hasThumb ? reinterpret_cast<const unsigned char *>(thumbnail.constData())
                 : nullptr,
        hasThumb ? static_cast<size_t>(thumbnail.size()) : 0,
        static_cast<unsigned long long>(hasThumb ? thumbnailWidth : 0),
        static_cast<unsigned long long>(hasThumb ? thumbnailHeight : 0),
        replyBytes.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "image send rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::sendThreadImageWithThumbnail(
    const QString &roomId, const QString &rootEventId,
    const QString &localPath, const QString &mime, const QString &caption,
    int width, int height, const QByteArray &thumbnail, int thumbnailWidth,
    int thumbnailHeight,
    const QString &replyToEventId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty()
        || rootEventId.isEmpty() || localPath.isEmpty() || mime.isEmpty())
        return 0;
    const bool hasThumb = !thumbnail.isEmpty() && thumbnailWidth > 0
        && thumbnailHeight > 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray root = rootEventId.toUtf8();
    const QByteArray path = localPath.toUtf8();
    const QByteArray mimeBytes = mime.toUtf8();
    const QByteArray captionBytes = caption.toUtf8();
    const QByteArray replyBytes = replyToEventId.toUtf8();
    const QString result = takeRustString(mx_rust_thread_send_image(
        m_rustHandle, room.constData(), root.constData(), path.constData(),
        mimeBytes.constData(), captionBytes.constData(),
        static_cast<unsigned long long>(qMax(0, width)),
        static_cast<unsigned long long>(qMax(0, height)),
        hasThumb ? reinterpret_cast<const unsigned char *>(thumbnail.constData())
                 : nullptr,
        hasThumb ? static_cast<size_t>(thumbnail.size()) : 0,
        static_cast<unsigned long long>(hasThumb ? thumbnailWidth : 0),
        static_cast<unsigned long long>(hasThumb ? thumbnailHeight : 0),
        replyBytes.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "thread image send rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::sendVideo(const QString &roomId,
                                       const QString &localPath,
                                       const QString &mime,
                                       const QString &caption,
                                       int width, int height,
                                       qint64 durationMs,
                                       const QByteArray &thumbnail,
                                       int thumbnailWidth,
                                       int thumbnailHeight,
                                       const QString &replyToEventId)
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
    const QByteArray replyBytes = replyToEventId.toUtf8();
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
        replyBytes.constData(), opId));
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
                                             int thumbnailHeight,
                                             const QString &replyToEventId)
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
    const QByteArray replyBytes = replyToEventId.toUtf8();
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
        replyBytes.constData(), opId));
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
                                              const QList<int> &waveform,
                                              const QString &replyToEventId)
{
    if (!m_rustHandle || roomId.isEmpty() || localPath.isEmpty()
        || mime.isEmpty() || durationMs <= 0)
        return 0;
    if (!timelineActiveFor(roomId)) {
        qCWarning(lcRust) << "voice send requires the open room timeline";
        return 0;
    }
    QByteArray amplitudes;
    amplitudes.reserve(waveform.size());
    for (int value : waveform)
        amplitudes.append(static_cast<char>(qBound(0, value, 100)));
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray path = localPath.toUtf8();
    const QByteArray mimeBytes = mime.toUtf8();
    const QByteArray replyBytes = replyToEventId.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_send_voice(
        m_rustHandle, room.constData(), path.constData(),
        mimeBytes.constData(), static_cast<unsigned long long>(durationMs),
        reinterpret_cast<const unsigned char *>(amplitudes.constData()),
        static_cast<size_t>(amplitudes.size()), replyBytes.constData(), opId));
    if (!result.isEmpty()) {
        qCWarning(lcRust) << "voice send rejected";
        return 0;
    }
    return opId;
}

quint64 RustSdkMatrixClient::sendThreadVoiceMessage(
    const QString &roomId, const QString &rootEventId,
    const QString &localPath, const QString &mime, qint64 durationMs,
    const QList<int> &waveform,
    const QString &replyToEventId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty()
        || rootEventId.isEmpty() || localPath.isEmpty() || mime.isEmpty()
        || durationMs <= 0)
        return 0;
    QByteArray amplitudes;
    amplitudes.reserve(waveform.size());
    for (int value : waveform)
        amplitudes.append(static_cast<char>(qBound(0, value, 100)));
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray root = rootEventId.toUtf8();
    const QByteArray path = localPath.toUtf8();
    const QByteArray mimeBytes = mime.toUtf8();
    const QByteArray replyBytes = replyToEventId.toUtf8();
    const QString result = takeRustString(mx_rust_thread_send_voice(
        m_rustHandle, room.constData(), root.constData(), path.constData(),
        mimeBytes.constData(), static_cast<unsigned long long>(durationMs),
        reinterpret_cast<const unsigned char *>(amplitudes.constData()),
        static_cast<size_t>(amplitudes.size()), replyBytes.constData(), opId));
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
                                                 int width, int height,
                                                 const QString &replyToEventId)
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
    const QByteArray replyBytes = replyToEventId.toUtf8();
    const QString result = takeRustString(mx_rust_timeline_send_attachment_bytes(
        m_rustHandle, room.constData(),
        reinterpret_cast<const unsigned char *>(bytes.constData()),
        static_cast<size_t>(bytes.size()), name.constData(),
        mimeBytes.constData(),
        static_cast<unsigned long long>(qMax(0, width)),
        static_cast<unsigned long long>(qMax(0, height)), replyBytes.constData(), opId));
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
                                                  qint64 durationMs,
                                                  const QString &replyToEventId)
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
    const QByteArray replyBytes = replyToEventId.toUtf8();
    const QString result = takeRustString(mx_rust_thread_send_attachment(
        m_rustHandle, room.constData(), root.constData(), path.constData(),
        mimeBytes.constData(), captionBytes.constData(),
        static_cast<unsigned long long>(qMax(0, width)),
        static_cast<unsigned long long>(qMax(0, height)),
        animated ? 1 : 0,
        static_cast<unsigned long long>(qMax<qint64>(0, durationMs)), replyBytes.constData(), opId));
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
                                                       int width, int height,
                                                       const QString &replyToEventId)
{
    if (!m_loggedIn || !m_rustHandle || roomId.isEmpty()
        || rootEventId.isEmpty() || bytes.isEmpty() || mime.isEmpty())
        return 0;
    const quint64 opId = nextOpId();
    const QByteArray room = roomId.toUtf8();
    const QByteArray root = rootEventId.toUtf8();
    const QByteArray name = filename.toUtf8();
    const QByteArray mimeBytes = mime.toUtf8();
    const QByteArray replyBytes = replyToEventId.toUtf8();
    const QString result = takeRustString(mx_rust_thread_send_attachment_bytes(
        m_rustHandle, room.constData(), root.constData(),
        reinterpret_cast<const unsigned char *>(bytes.constData()),
        static_cast<size_t>(bytes.size()), name.constData(),
        mimeBytes.constData(),
        static_cast<unsigned long long>(qMax(0, width)),
        static_cast<unsigned long long>(qMax(0, height)), replyBytes.constData(), opId));
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
    const QString result = takeRustString(mx_rust_media_fetch(
        m_rustHandle, key.constData(),
        static_cast<unsigned int>(qBound(0, kind, 2)), opId,
        static_cast<unsigned int>(qBound(0, timeoutClass, 2))));
    if (!result.isEmpty()) {
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
        Q_EMIT mediaFailed(opId, event.value(QStringLiteral("key")).toString(),
                           event.value(QStringLiteral("kind")).toInt(),
                           QStringLiteral("gone"));
        return;
    }
    QByteArray bytes(reinterpret_cast<const char *>(raw),
                     static_cast<qsizetype>(len));
    mx_rust_media_free(raw, len);
    Q_EMIT mediaReady(opId, event.value(QStringLiteral("key")).toString(),
                      event.value(QStringLiteral("kind")).toInt(), bytes,
                      event.value(QStringLiteral("mimetype")).toString(),
                      event.value(QStringLiteral("filename")).toString());
}

namespace {
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
}

bool RustSdkMatrixClient::handleRoomCommandEvent(const QString &type,
                                                 const QJsonObject &event)
{
    const auto opId = [&event]() {
        return static_cast<quint64>(
            event.value(QStringLiteral("op_id")).toDouble());
    };

    if (type == QLatin1String("call_candidates")) {
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
            event.value(QStringLiteral("sender")).toString(),
            event.value(QStringLiteral("own")).toBool(), candidates);
        return true;
    }
    if (type == QLatin1String("rtc_session_changed")) {
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
            participant.hashedRtcIdentity =
                row.value(QStringLiteral("hashed_rtc_identity")).toString();
            const QJsonArray foci = row.value(QStringLiteral("foci")).toArray();
            if (!foci.isEmpty()) {
                participant.focusServiceUrl = foci.first().toObject()
                    .value(QStringLiteral("livekit_service_url")).toString();
            }
            participant.focusSelection =
                row.value(QStringLiteral("focus_selection")).toString();
            participant.intent = row.value(QStringLiteral("intent")).toString();
            participant.displayName =
                row.value(QStringLiteral("display_name")).toString().trimmed();
            participant.avatarMxc =
                row.value(QStringLiteral("avatar_mxc")).toString();
            participant.joinedAtMs = static_cast<qint64>(
                row.value(QStringLiteral("created_ts")).toDouble());
            const double expiresAt =
                row.value(QStringLiteral("expires_at_ms")).toDouble();
            participant.expiresAtMs = !(expiresAt < 9.2e18)
                ? std::numeric_limits<qint64>::max()
                : static_cast<qint64>(qMax(0.0, expiresAt));
            participant.wireFormat = row.value(QStringLiteral("kind")).toString();
            participant.membershipEventId =
                row.value(QStringLiteral("event_id")).toString();
            participant.ownUser =
                !ownUser.isEmpty() && participant.userId == ownUser;
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
            event.value(QStringLiteral("key_index")).toInt(),
            event.value(QStringLiteral("undelivered")).toArray()
                .toVariantList());
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
    if (type.startsWith(QLatin1String("sfu_"))) {
        const quint64 generation = static_cast<quint64>(
            event.value(QStringLiteral("generation")).toDouble());
        if ((type == QLatin1String("sfu_state")
             || type == QLatin1String("sfu_permission"))
            && event.contains(QStringLiteral("op_id"))) {
            if (m_sfuConnectOp == 0 || opId() != m_sfuConnectOp) {
                qCInfo(lcRust) << "sfu report from an abandoned connect"
                                  " ignored state="
                               << event.value(QStringLiteral("state"))
                                      .toString();
                return true;
            }
            m_sfuSessionGeneration = generation;
        } else if (m_sfuSessionGeneration == 0
                   || generation != m_sfuSessionGeneration) {
            if (type == QLatin1String("sfu_state")) {
                qCInfo(lcRust) << "sfu report from an earlier session ignored"
                                  " state="
                               << event.value(QStringLiteral("state"))
                                      .toString();
            }
            return true;
        }
        if (m_callMediaCapable)
            Q_EMIT sfuConnectionEvent(event.toVariantMap());
        if (type == QLatin1String("sfu_command_dropped")) {
            qCWarning(lcRustSfu) << "sfu command queue warning connection="
                              << event.value(QStringLiteral("connection_id")).toInt()
                              << "dropped="
                              << event.value(QStringLiteral("dropped")).toVariant()
                              << "category="
                              << event.value(QStringLiteral("category")).toString();
            return true;
        }
        if (type == QLatin1String("sfu_state") && event.contains(QStringLiteral("reason"))) {
            qCInfo(lcRust) << "sfu connection leave connection="
                           << event.value(QStringLiteral("connection_id")).toInt()
                           << "reason=" << event.value(QStringLiteral("reason")).toInt()
                           << "action=" << event.value(QStringLiteral("action")).toInt();
        }
        if (type == QLatin1String("sfu_state")
            && event.contains(QStringLiteral("address_class"))) {
            qCWarning(lcRust) << "sfu focus address refused connection="
                              << event.value(QStringLiteral("connection_id")).toInt()
                              << "stage=" << event.value(QStringLiteral("stage")).toString()
                              << "category=" << event.value(QStringLiteral("category")).toString()
                              << "addressClass="
                              << event.value(QStringLiteral("address_class")).toString()
                              << "advertisedByOwnHomeserver="
                              << event.value(QStringLiteral("own_focus")).toBool();
        }
        if (event.value(QStringLiteral("connection_id")).toInt() != 0)
            return true;
    }
    if (type == QLatin1String("sfu_permission")) {
        qCInfo(lcRust) << "sfu publish permission"
                       << "connection="
                       << event.value(QStringLiteral("connection_id")).toInt()
                       << "serviceHostHash="
                       << event.value(QStringLiteral("service_host_hash")).toString()
                       << "canPublish="
                       << event.value(QStringLiteral("can_publish")).toBool()
                       << "source="
                       << event.value(QStringLiteral("source")).toString();
        return true;
    }
    if (type == QLatin1String("sfu_state")) {
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
        const QString service = event.value(QStringLiteral("service_host_hash")).toString();
        qCInfo(lcRust) << "sfu join serviceHostHash=" << service
                       << "canPublish=" << event.value(QStringLiteral("can_publish")).toVariant()
                       << "canSubscribe=" << event.value(QStringLiteral("can_subscribe")).toVariant();
        const auto rows = event.value(QStringLiteral("participants")).toArray();
        for (const auto &row : rows) {
            const auto participant = row.toObject();
            if (participant.value(QStringLiteral("identity"))
                    == event.value(QStringLiteral("identity")))
                continue;
            qCInfo(lcRust) << "sfu remote tracks announced serviceHostHash=" << service
                           << "count=" << participant.value(QStringLiteral("tracks")).toArray().size();
        }
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
        qCInfo(lcRust) << "sfu TrackPublished serviceHostHash="
                       << event.value(QStringLiteral("service_host_hash")).toString();
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
        return true;
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
        Q_EMIT rtcHandChanged(
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("sender")).toString(),
            event.value(QStringLiteral("membership_event_id")).toString(),
            event.value(QStringLiteral("reaction_event_id")).toString(),
            event.value(QStringLiteral("raised")).toBool());
        return true;
    }
    if (type == QLatin1String("rtc_call_reaction")) {
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
            static_cast<qint64>(qBound(
                0.0, event.value(QStringLiteral("ttl_seconds")).toDouble(),
                86400.0)),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

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
        || type == QLatin1String("call_rtc_decline")
        || type == QLatin1String("call_negotiate")
        || type == QLatin1String("call_stream_metadata")) {
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
        else if (type == QLatin1String("call_negotiate"))
            signal.kind = CallSignal::Kind::Negotiate;
        else if (type == QLatin1String("call_stream_metadata"))
            signal.kind = CallSignal::Kind::StreamMetadata;
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
        } else if (signal.kind == CallSignal::Kind::Negotiate) {
            signal.sessionType =
                event.value(QStringLiteral("description_type")).toString();
            signal.hasDescription =
                event.value(QStringLiteral("has_description")).toBool();
        }
        signal.hasVideo = event.value(QStringLiteral("has_video")).toBool();
        signal.hasStreamMetadata =
            event.value(QStringLiteral("has_stream_metadata")).toBool();
        signal.remoteAudioMuted =
            event.value(QStringLiteral("remote_audio_muted")).toBool();
        signal.remoteVideoMuted =
            event.value(QStringLiteral("remote_video_muted")).toBool();
        signal.reason = event.value(QStringLiteral("reason")).toString();
        signal.selectedPartyId =
            event.value(QStringLiteral("selected_party_id")).toString();
        signal.callIntent =
            event.value(QStringLiteral("call_intent")).toString();
        signal.targetEventId =
            event.value(QStringLiteral("target_event_id")).toString();
        if (signal.kind == CallSignal::Kind::RtcNotification) {
            const QString notificationType =
                event.value(QStringLiteral("notification_type")).toString();
            if (notificationType == QLatin1String("ring")
                || notificationType == QLatin1String("notification"))
                signal.notificationType = notificationType;
            constexpr int kMaxMentionIds = 256;
            const QJsonArray mentioned =
                event.value(QStringLiteral("mentions_user_ids")).toArray();
            for (const QJsonValue &value : mentioned) {
                if (signal.mentionUserIds.size() >= kMaxMentionIds)
                    break;
                const QString userId = value.toString();
                if (userId.startsWith(QLatin1Char('@')))
                    signal.mentionUserIds.append(userId);
            }
            signal.mentionsRoom =
                event.value(QStringLiteral("mentions_room")).toBool(false);
            signal.mentionsMe =
                event.value(QStringLiteral("mentions_me")).toBool(false);
        }
        if (signal.roomId.isEmpty() || signal.eventId.isEmpty())
            return true;
        if (m_callMediaCapable) {
            if (signal.kind == CallSignal::Kind::Invite) {
                m_callSdpStore.insert(
                    signal.eventId,
                    event.value(QStringLiteral("offer_sdp")).toString());
            } else if (signal.kind == CallSignal::Kind::Answer) {
                m_callSdpStore.insert(
                    signal.eventId,
                    event.value(QStringLiteral("answer_sdp")).toString());
            } else if (signal.kind == CallSignal::Kind::Negotiate) {
                m_callSdpStore.insert(
                    signal.eventId,
                    event.value(QStringLiteral("description_sdp")).toString());
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
                         row.value(QStringLiteral("display_name"))
                             .toString().trimmed());
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
        fields.insert(QStringLiteral("videoMime"),
                      raw.value(QStringLiteral("video_mime")).toString());
        fields.insert(QStringLiteral("videoSize"),
                      static_cast<qint64>(
                          raw.value(QStringLiteral("video_size")).toDouble()));
        fields.insert(QStringLiteral("mediaTooLarge"),
                      raw.value(QStringLiteral("media_too_large")).toBool());
        Q_EMIT urlPreviewFinished(
            opId(), event.value(QStringLiteral("ok")).toBool(), fields,
            event.value(QStringLiteral("category")).toString(),
            event.value(QStringLiteral("status")).toInt(),
            event.value(QStringLiteral("redirects")).toInt());
        return true;
    }

    if (type == QLatin1String("gif_response")) {
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
    if (type == QLatin1String("member_profile_changed")) {
        Q_EMIT userProfileChanged(
            event.value(QStringLiteral("user_id")).toString());
        return true;
    }
    if (type == QLatin1String("name_color")) {
        const quint64 colorOp = static_cast<quint64>(
            event.value(QStringLiteral("op_id")).toDouble(0));
        if (event.value(QStringLiteral("failed")).toBool(false)) {
            Q_EMIT nameColorFailed(
                colorOp, event.value(QStringLiteral("user_id")).toString());
            return true;
        }
        Q_EMIT nameColorReceived(
            colorOp,
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
    if (type == QLatin1String("room_background")) {
        Q_EMIT roomBackgroundReceived(
            static_cast<quint64>(
                event.value(QStringLiteral("op_id")).toDouble(0)),
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("content")).toObject().toVariantMap(),
            event.value(QStringLiteral("can_set")).toBool(false),
            event.value(QStringLiteral("unsupported_version")).toBool(false));
        return true;
    }
    if (type == QLatin1String("room_background_set")) {
        if (!event.value(QStringLiteral("ok")).toBool(false)) {
            qCWarning(lcRust)
                << "room background write failed op="
                << static_cast<quint64>(event.value(QStringLiteral("op_id")).toDouble(0))
                << "stage=" << event.value(QStringLiteral("stage")).toString()
                << "category=" << event.value(QStringLiteral("category")).toString()
                << "detail=" << event.value(QStringLiteral("detail")).toString().left(80);
        }
        Q_EMIT roomBackgroundSet(
            static_cast<quint64>(
                event.value(QStringLiteral("op_id")).toDouble(0)),
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("content")).toObject().toVariantMap(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }
    if (type == QLatin1String("settings_sync_changed")) {
        Q_EMIT syncedSettingsChanged();
        return true;
    }
    if (type == QLatin1String("settings_sync_read")) {
        const QJsonValue sw = event.value(QStringLiteral("switch"));
        const QJsonValue blob = event.value(QStringLiteral("blob"));
        Q_EMIT syncedSettingsRead(
            opId(), event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("category")).toString(),
            event.value(QStringLiteral("blob_state")).toString(),
            blob.isObject()
                ? QString::fromUtf8(QJsonDocument(blob.toObject())
                                        .toJson(QJsonDocument::Compact))
                : QString(),
            sw.isBool() ? (sw.toBool() ? 1 : 0) : -1);
        return true;
    }
    if (type == QLatin1String("settings_sync_written")) {
        Q_EMIT syncedSettingsWritten(
            opId(), event.value(QStringLiteral("kind")).toInt(),
            event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("category")).toString());
        return true;
    }
    if (type == QLatin1String("personal_background_changed")) {
        Q_EMIT personalBackgroundChanged(
            event.value(QStringLiteral("scope")).toString());
        return true;
    }
    if (type == QLatin1String("personal_backgrounds")) {
        qCDebug(lcRust) << "personal background read op=" << opId()
                        << "gets=" << event.value(QStringLiteral("gets")).toInt();
        QStringList failed;
        for (const QJsonValue &value :
             event.value(QStringLiteral("failed")).toArray())
            failed.append(value.toString());
        Q_EMIT personalBackgroundsRead(
            opId(), event.value(QStringLiteral("scope")).toString(),
            event.value(QStringLiteral("entries")).toArray().toVariantList(),
            failed,
            event.value(QStringLiteral("enabled")).isBool()
                ? (event.value(QStringLiteral("enabled")).toBool() ? 1 : 0)
                : -1);
        return true;
    }
    if (type == QLatin1String("personal_background_bytes")) {
        const QString scope = event.value(QStringLiteral("scope")).toString();
        const bool ok = event.value(QStringLiteral("ok")).toBool(false);
        QByteArray bytes;
        if (ok) {
            size_t len = 0;
            unsigned char *raw = mx_rust_personal_background_take(m_rustHandle, opId(), &len);
            if (raw && len > 0)
                bytes = QByteArray(reinterpret_cast<const char *>(raw),
                                   static_cast<qsizetype>(len));
            if (raw)
                mx_rust_media_free(raw, len);
        }
        const bool delivered = ok && !bytes.isEmpty();
        Q_EMIT personalBackgroundDownloaded(
            opId(), scope, delivered,
            event.value(QStringLiteral("entry")).toObject().toVariantMap(),
            bytes,
            delivered ? QString()
                      : (ok ? QStringLiteral("gone")
                            : event.value(QStringLiteral("category")).toString()));
        return true;
    }
    if (type == QLatin1String("personal_background_written")) {
        if (!event.value(QStringLiteral("ok")).toBool(false)) {
            qCWarning(lcRust)
                << "personal background write failed op=" << opId()
                << "mode=" << event.value(QStringLiteral("mode")).toString()
                << "stage=" << event.value(QStringLiteral("stage")).toString()
                << "category=" << event.value(QStringLiteral("category")).toString();
        }
        Q_EMIT personalBackgroundWritten(
            opId(), event.value(QStringLiteral("scope")).toString(),
            event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("entry")).toObject().toVariantMap(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }
    if (type == QLatin1String("personal_backgrounds_cleared")) {
        Q_EMIT personalBackgroundsCleared(
            opId(), event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("cleared")).toInt(),
            event.value(QStringLiteral("failed")).toInt(),
            event.value(QStringLiteral("skipped")).toInt(),
            event.value(QStringLiteral("switched_off")).toBool(false));
        return true;
    }
    if (type == QLatin1String("sticker_packs")) {
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
            entry.insert(QStringLiteral("namespace"),
                         pack.value(QStringLiteral("namespace")).toString());
            entry.insert(QStringLiteral("legacyTwin"),
                         pack.value(QStringLiteral("legacy_twin")).toBool());
            entry.insert(QStringLiteral("legacySubscribed"),
                         pack.value(QStringLiteral("legacy_subscribed")).toBool());
            entry.insert(QStringLiteral("usageUnsupported"),
                         pack.value(QStringLiteral("usage_unsupported")).toBool());
            entry.insert(QStringLiteral("available"),
                         pack.value(QStringLiteral("available")).toBool(true));
            entry.insert(QStringLiteral("unavailable"),
                         pack.value(QStringLiteral("unavailable")).toString());
            entry.insert(QStringLiteral("fromSpace"),
                         pack.value(QStringLiteral("from_space")).toBool());
            entry.insert(QStringLiteral("packEmoticon"),
                         pack.value(QStringLiteral("pack_emoticon")).toBool(true));
            entry.insert(QStringLiteral("packSticker"),
                         pack.value(QStringLiteral("pack_sticker")).toBool(true));
            QVariantList images;
            const QJsonArray rawImages =
                pack.value(QStringLiteral("images")).toArray();
            images.reserve(rawImages.size());
            for (const QJsonValue &imageValue : rawImages) {
                const QJsonObject image = imageValue.toObject();
                QVariantMap row;
                row.insert(QStringLiteral("key"),
                           image.value(QStringLiteral("key")).toString());
                row.insert(QStringLiteral("keyValid"),
                           image.value(QStringLiteral("key_valid")).toBool(true));
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
        Q_EMIT stickerPackSnapshotMeta(
            static_cast<quint64>(
                event.value(QStringLiteral("op_id")).toDouble(0)),
            QVariantMap{
                { QStringLiteral("roomCanManageLegacy"),
                  event.value(QStringLiteral("room_can_manage_legacy")).toBool() },
                { QStringLiteral("roomScanFailed"),
                  event.value(QStringLiteral("room_scan_failed")).toBool() },
                { QStringLiteral("subscriptionsSource"),
                  event.value(QStringLiteral("subscriptions_source")).toString() },
                { QStringLiteral("stableSubscriptionsUnreadable"),
                  event.value(QStringLiteral("stable_subscriptions_unreadable"))
                      .toBool() },
                { QStringLiteral("legacyOnlySubscriptions"),
                  event.value(QStringLiteral("legacy_only_subscriptions")).toInt() },
                { QStringLiteral("canonicalAncestorIds"),
                  event.value(QStringLiteral("canonical_ancestor_ids")).toVariant()
                      .toStringList() },
            });
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
            event.value(QStringLiteral("enabled")).toBool(false),
            event.value(QStringLiteral("mirror")).toString());
        return true;
    }
    if (type == QLatin1String("sticker_pack_migration")) {
        Q_EMIT stickerPackMigrated(
            static_cast<quint64>(event.value(QStringLiteral("op_id")).toDouble(0)),
            event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("category")).toString(),
            QVariantMap{
                { QStringLiteral("roomId"), event.value(QStringLiteral("room_id")).toString() },
                { QStringLiteral("created"), event.value(QStringLiteral("created")).toBool() },
                { QStringLiteral("duplicates"), event.value(QStringLiteral("duplicates")).toInt() },
                { QStringLiteral("packs"), event.value(QStringLiteral("packs")).toInt() },
                { QStringLiteral("invalid"), event.value(QStringLiteral("invalid")).toInt() },
                { QStringLiteral("subscribed"), event.value(QStringLiteral("subscribed")).toBool() },
            });
        return true;
    }
    if (type == QLatin1String("sticker_packs_changed")) {
        Q_EMIT stickerPacksChanged(
            event.value(QStringLiteral("room_id")).toString());
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
    if (type == QLatin1String("sticker_pack_add_result")
        && !event.value(QStringLiteral("uploaded_url")).toString().isEmpty()) {
        Q_EMIT stickerPackUploaded(
            static_cast<quint64>(event.value(QStringLiteral("op_id")).toDouble(0)),
            event.value(QStringLiteral("uploaded_url")).toString());
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
                entry.insert(
                    QStringLiteral("lastActiveAgoMs"),
                    static_cast<qlonglong>(
                        obj.value(QStringLiteral("last_active_ago_ms"))
                            .toDouble(-1.0)));
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

    if (type == QLatin1String("rust_trace")) {
        qInfo("%s", qPrintable(event.value(QStringLiteral("line")).toString()));
        return true;
    }

    if (type == QLatin1String("presence_published")) {
        Q_EMIT presencePublished();
        return true;
    }

    if (type == QLatin1String("presence_publish_failed")) {
        Q_EMIT presencePublishFailed(
            event.value(QStringLiteral("category")).toString(),
            event.value(QStringLiteral("retry_after_ms")).toVariant()
                .toLongLong());
        return true;
    }

    if (type == QLatin1String("room_own_permissions")) {
        QVariantMap facts;
        facts.insert(QStringLiteral("ownMembership"),
                     event.value(QStringLiteral("own_membership")).toString());
        if (const QJsonValue v = event.value(QStringLiteral("own_can_send_message"));
            v.isBool()) {
            facts.insert(QStringLiteral("canSendMessages"), v.toBool());
        }
        if (const QJsonValue v = event.value(QStringLiteral("own_can_redact_other"));
            v.isBool()) {
            facts.insert(QStringLiteral("canRedactOthers"), v.toBool());
        }
        static const struct { const char *wire; const char *key; } kModeration[] = {
            {"own_can_kick", "canKick"},
            {"own_can_ban", "canBan"},
            {"own_can_unban", "canUnban"},
            {"own_can_invite", "canInvite"},
            {"own_can_edit_topic", "canEditTopic"},
            {"own_can_edit_name", "canEditName"},
        };
        for (const auto &m : kModeration) {
            if (const QJsonValue v = event.value(QLatin1String(m.wire));
                v.isBool()) {
                facts.insert(QLatin1String(m.key), v.toBool());
            }
        }
        Q_EMIT roomOwnPermissionsReceived(
            opId(), event.value(QStringLiteral("room_id")).toString(), facts);
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
        if (const QJsonValue canPublish =
                event.value(QStringLiteral("own_can_publish_rtc_membership"));
            canPublish.isBool()) {
            snapshot.insert(QStringLiteral("canPublishCallMembership"),
                            canPublish.toBool());
        }
        if (const QJsonValue v = event.value(QStringLiteral("own_can_send_message"));
            v.isBool()) {
            snapshot.insert(QStringLiteral("canSendMessages"), v.toBool());
        }
        if (const QJsonValue v = event.value(QStringLiteral("own_can_redact_other"));
            v.isBool()) {
            snapshot.insert(QStringLiteral("canRedactOthers"), v.toBool());
        }
        snapshot.insert(QStringLiteral("ownMembership"),
                        event.value(QStringLiteral("own_membership")).toString());
        snapshot.insert(QStringLiteral("joinRule"),
                        event.value(QStringLiteral("join_rule")).toString());
        snapshot.insert(
            QStringLiteral("canonicalAlias"),
            event.value(QStringLiteral("canonical_alias")).toString());
        snapshot.insert(
            QStringLiteral("canonicalSpaceParent"),
            event.value(QStringLiteral("canonical_space_parent"))
                .toObject().toVariantMap());
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
                         row.value(QStringLiteral("display_name"))
                             .toString().trimmed());
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
        const QString membersRoomId =
            event.value(QStringLiteral("room_id")).toString();
        if (event.value(QStringLiteral("ok")).toBool()) {
            const auto roomIt = m_rooms.find(membersRoomId);
            if (roomIt != m_rooms.end()) {
                const auto fetched = matrix::rust_timeline::membersFromPayload(rows);
                for (auto it = fetched.constBegin(); it != fetched.constEnd();
                     ++it) {
                    MemberInfo &slot = roomIt->members[it.key()];
                    slot.userId = it.key();
                    if (!it->displayName.isEmpty())
                        slot.displayName = it->displayName;
                    if (!it->avatarMxcUrl.isEmpty())
                        slot.avatarMxcUrl = it->avatarMxcUrl;
                }
                QHash<QString, qsizetype> counts;
                for (auto r = m_rooms.constBegin(); r != m_rooms.constEnd(); ++r) {
                    if (r->members.size() >= matrix::rust_timeline::kLargeMemberList)
                        counts.insert(r.key(), r->members.size());
                }
                const QSet<QString> open{ m_timelineTracker.requestedRoom(),
                                          m_timelineTracker.activeRoom(),
                                          m_threadTracker.requestedRoom(),
                                          m_threadTracker.activeRoom() };
                for (const QString &gone : matrix::rust_timeline::memberListVictims(
                         m_memberListRecency, membersRoomId, counts, open)) {
                    const auto goneIt = m_rooms.find(gone);
                    if (goneIt == m_rooms.end())
                        continue;
                    QHash<QString, MemberInfo> names;
                    names.reserve(goneIt->members.size());
                    for (auto m = goneIt->members.constBegin();
                         m != goneIt->members.constEnd(); ++m) {
                        if (m->displayName.isEmpty())
                            continue;
                        MemberInfo kept;
                        kept.userId = m.key();
                        kept.displayName = m->displayName;
                        names.insert(m.key(), kept);
                    }
                    goneIt->members = names;
                    Q_EMIT roomMembersEvicted(gone);
                }
                if (!event.value(QStringLiteral("partial")).toBool()
                    && !event.value(QStringLiteral("truncated")).toBool()
                    && !roomIt->typingUserIds.isEmpty()) {
                    QSet<QString> joined;
                    for (const QJsonValue &v : rows) {
                        const QJsonObject r = v.toObject();
                        if (r.value(QStringLiteral("membership")).toString()
                            == QLatin1String("joined"))
                            joined.insert(r.value(QStringLiteral("user_id")).toString());
                    }
                    QStringList kept;
                    for (const QString &u : std::as_const(roomIt->typingUserIds)) {
                        if (joined.contains(u))
                            kept.append(u);
                    }
                    if (kept != roomIt->typingUserIds) {
                        roomIt->typingUserIds = kept;
                        Q_EMIT typingChanged(membersRoomId);
                    }
                }
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
            static_cast<qint64>(event.value(QStringLiteral("timestamp_ms")).toDouble()),
            event.value(QStringLiteral("target_is_own")).toBool(false),
            event.value(QStringLiteral("backlog")).toBool(false));
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
                { QStringLiteral("gif"),
                  row.value(QStringLiteral("gif")).toBool() },
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
    if (type == QLatin1String("room_export_page")) {
        QVariantList rows;
        for (const QJsonValue &v :
             event.value(QStringLiteral("rows")).toArray()) {
            const QJsonObject row = v.toObject();
            rows.append(QVariantMap{
                { QStringLiteral("eventId"),
                  row.value(QStringLiteral("event_id")).toString() },
                { QStringLiteral("sender"),
                  row.value(QStringLiteral("sender")).toString() },
                { QStringLiteral("senderName"),
                  row.value(QStringLiteral("sender_name")).toString() },
                { QStringLiteral("timestampMs"),
                  static_cast<qint64>(
                      row.value(QStringLiteral("ts_ms")).toDouble()) },
                { QStringLiteral("kind"),
                  row.value(QStringLiteral("kind")).toString() },
                { QStringLiteral("body"),
                  row.value(QStringLiteral("body")).toString() },
                { QStringLiteral("formattedBody"),
                  row.value(QStringLiteral("formatted_body")).toString() },
                { QStringLiteral("threadRoot"),
                  row.value(QStringLiteral("thread_root")).toString() },
                { QStringLiteral("replyTo"),
                  row.value(QStringLiteral("reply_to")).toString() },
                { QStringLiteral("edited"),
                  row.value(QStringLiteral("edited")).toBool() },
                { QStringLiteral("filename"),
                  row.value(QStringLiteral("filename")).toString() },
                { QStringLiteral("mimetype"),
                  row.value(QStringLiteral("mimetype")).toString() },
                { QStringLiteral("size"),
                  static_cast<qint64>(
                      row.value(QStringLiteral("size")).toDouble()) },
                { QStringLiteral("mediaKey"),
                  row.value(QStringLiteral("media_key")).toString() },
            });
        }
        Q_EMIT roomExportPage(
            opId(), event.value(QStringLiteral("room_id")).toString(), rows,
            static_cast<qint64>(
                event.value(QStringLiteral("rows_total")).toDouble()),
            static_cast<qint64>(
                event.value(QStringLiteral("scanned_total")).toDouble()),
            static_cast<qint64>(
                event.value(QStringLiteral("undecryptable_total")).toDouble()),
            event.value(QStringLiteral("done")).toBool(),
            event.value(QStringLiteral("stop_reason")).toString(),
            event.value(QStringLiteral("encrypted_room")).toBool());
        return true;
    }
    if (type == QLatin1String("room_export_failed")) {
        Q_EMIT roomExportFailed(
            opId(), event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("message")).toString());
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
                { QStringLiteral("senderDisplayName"),
                  row.value(QStringLiteral("sender_name")).toString() },
                { QStringLiteral("body"),
                  row.value(QStringLiteral("body")).toString() },
                { QStringLiteral("msgtype"),
                  row.value(QStringLiteral("msgtype")).toString() },
                { QStringLiteral("filename"),
                  row.value(QStringLiteral("filename")).toString() },
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
    if (type == QLatin1String("media_cache_cleared")) {
        Q_EMIT storedMediaCleared(
            opId(), event.value(QStringLiteral("ok")).toBool(),
            static_cast<qint64>(
                event.value(QStringLiteral("files")).toDouble()),
            static_cast<qint64>(
                event.value(QStringLiteral("bytes")).toDouble()));
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
    if (type == QLatin1String("search_index_all")) {
        const auto count = [&event](const char *key) {
            return static_cast<qint64>(
                event.value(QLatin1String(key)).toDouble());
        };
        QVariantMap status;
        status.insert(QStringLiteral("state"),
                      event.value(QStringLiteral("state")).toString());
        status.insert(QStringLiteral("total"), count("total"));
        status.insert(QStringLiteral("position"), count("position"));
        status.insert(QStringLiteral("currentRoomId"),
                      event.value(QStringLiteral("current_room")).toString());
        status.insert(QStringLiteral("written"), count("written"));
        status.insert(QStringLiteral("skipped"), count("skipped"));
        status.insert(QStringLiteral("undecryptable"), count("undecryptable"));
        status.insert(QStringLiteral("undecryptableRooms"),
                      count("undecryptable_rooms"));
        status.insert(QStringLiteral("failedRooms"), count("failed_rooms"));
        status.insert(QStringLiteral("completeRooms"), count("complete_rooms"));
        status.insert(QStringLiteral("retryInMs"), count("retry_in_ms"));
        status.insert(QStringLiteral("elapsedMs"), count("elapsed_ms"));
        if (event.contains(QStringLiteral("messages"))) {
            status.insert(QStringLiteral("messages"), count("messages"));
            status.insert(QStringLiteral("indexedRooms"),
                          count("indexed_rooms"));
        }
        Q_EMIT searchIndexAllProgress(opId(), status);
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
    if (type == QLatin1String("activity_seed")
        || type == QLatin1String("activity_seed_page")) {
        const bool page = type == QLatin1String("activity_seed_page");
        static int seedFailuresInARow = 0;
        if (!event.value(QStringLiteral("ok")).toBool(true)) {
            if (page) {
                Q_EMIT activitySeedPageFailed(
                    event.value(QStringLiteral("category")).toString().left(48));
                return true;
            }
            ++seedFailuresInARow;
            const QString category = event.value(QStringLiteral("category")).toString();
            if (seedFailuresInARow == 1) {
                qCWarning(lcRust) << "activity seed failed"
                                  << "category=" << (category.isEmpty()
                                                         ? QStringLiteral("unknown")
                                                         : category.left(48));
            } else {
                qCInfo(lcRust) << "activity seed failed again"
                               << "attempts=" << seedFailuresInARow;
            }
            Q_EMIT activitySeedFailed(category.left(48));
            return true;
        }
        seedFailuresInARow = 0;
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
                { QStringLiteral("msgtype"),
                  o.value(QStringLiteral("msgtype")).toString() },
                { QStringLiteral("kind"), QStringLiteral("highlight") },
            });
        }
        const QString next = event.value(QStringLiteral("next_token")).toString();
        if (page)
            Q_EMIT activitySeedPageReceived(entries, next);
        else {
            Q_EMIT activitySeedCursorReceived(next);
            Q_EMIT activitySeedReceived(entries);
        }
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
        Q_EMIT backupActionFinished(
            opId(), event.value(QStringLiteral("action")).toString(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("recovery_key")).toString(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("cross_signing_approval")) {
        Q_EMIT crossSigningApprovalRequired(
            opId(), event.value(QStringLiteral("url")).toString());
        return true;
    }

    if (type == QLatin1String("backup_progress")) {
        Q_EMIT backupProgress(
            event.value(QStringLiteral("backup_state")).toString(),
            event.value(QStringLiteral("upload_state")).toString(),
            event.value(QStringLiteral("backed_up")).toVariant().toLongLong(),
            event.value(QStringLiteral("total")).toVariant().toLongLong(),
            static_cast<quint64>(
                event.value(QStringLiteral("request_id")).toDouble(0)));
        return true;
    }

    if (type == QLatin1String("account_pref")) {
        Q_EMIT accountPrefRead(
            event.value(QStringLiteral("pref_type")).toString(),
            event.value(QStringLiteral("present")).toBool(false),
            event.value(QStringLiteral("content")).toObject().toVariantMap());
        return true;
    }
    if (type == QLatin1String("account_pref_failed")) {
        Q_EMIT accountPrefReadFailed(
            event.value(QStringLiteral("pref_type")).toString(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }
    if (type == QLatin1String("account_pref_changed")) {
        Q_EMIT accountPrefChanged(
            event.value(QStringLiteral("pref_type")).toString());
        return true;
    }
    if (type == QLatin1String("invite_policy_support")) {
        Q_EMIT invitePolicySupportReceived(
            event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("stable")).toBool(false),
            event.value(QStringLiteral("unstable")).toBool(false));
        return true;
    }
    if (type == QLatin1String("account_pref_written")) {
        Q_EMIT accountPrefWritten(
            event.value(QStringLiteral("pref_type")).toString(),
            event.value(QStringLiteral("ok")).toBool(false),
            event.value(QStringLiteral("category")).toString());
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

    if (type == QLatin1String("moderation_plan")) {
        QVariantList rooms;
        const QJsonArray arr = event.value(QStringLiteral("rooms")).toArray();
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            QVariantMap row;
            row.insert(QStringLiteral("roomId"),
                       o.value(QStringLiteral("room_id")).toString());
            row.insert(QStringLiteral("name"),
                       o.value(QStringLiteral("name")).toString());
            row.insert(QStringLiteral("isSpace"),
                       o.value(QStringLiteral("is_space")).toBool(false));
            row.insert(QStringLiteral("membership"),
                       o.value(QStringLiteral("membership")).toString());
            row.insert(QStringLiteral("ownLevel"),
                       static_cast<qlonglong>(
                           o.value(QStringLiteral("own_level")).toDouble()));
            row.insert(QStringLiteral("targetLevel"),
                       static_cast<qlonglong>(
                           o.value(QStringLiteral("target_level")).toDouble()));
            row.insert(QStringLiteral("reason"),
                       o.value(QStringLiteral("reason")).toString());
            rooms.append(row);
        }
        Q_EMIT moderationPlanReceived(
            opId(), event.value(QStringLiteral("user_id")).toString(),
            event.value(QStringLiteral("op")).toString(),
            event.value(QStringLiteral("truncated")).toBool(), rooms);
        return true;
    }

    if (type == QLatin1String("room_closure_plan")) {
        QVariantList rooms;
        const QJsonArray arr = event.value(QStringLiteral("rooms")).toArray();
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            QVariantMap row;
            row.insert(QStringLiteral("roomId"),
                       o.value(QStringLiteral("room_id")).toString());
            row.insert(QStringLiteral("name"),
                       o.value(QStringLiteral("name")).toString());
            row.insert(QStringLiteral("isSpace"),
                       o.value(QStringLiteral("is_space")).toBool(false));
            if (o.contains(QStringLiteral("reason")))
                row.insert(QStringLiteral("reason"),
                           o.value(QStringLiteral("reason")).toString());
            row.insert(QStringLiteral("joinRule"),
                       o.value(QStringLiteral("join_rule")).toString());
            row.insert(QStringLiteral("canKick"),
                       o.value(QStringLiteral("can_kick")).toBool(false));
            row.insert(QStringLiteral("canEditChildren"),
                       o.value(QStringLiteral("can_edit_children")).toBool(false));
            row.insert(QStringLiteral("worldReadable"),
                       o.value(QStringLiteral("world_readable")).toBool(false));
            row.insert(QStringLiteral("removable"),
                       o.value(QStringLiteral("removable")).toInt());
            row.insert(QStringLiteral("staying"),
                       o.value(QStringLiteral("staying")).toInt());
            QStringList names;
            for (const QJsonValue &n :
                 o.value(QStringLiteral("staying_names")).toArray())
                names.append(n.toString());
            row.insert(QStringLiteral("stayingNames"), names);
            rooms.append(row);
        }
        Q_EMIT roomClosurePlanReceived(
            opId(), event.value(QStringLiteral("truncated")).toBool(), rooms);
        return true;
    }

    if (type == QLatin1String("room_closure_progress")) {
        Q_EMIT roomClosureProgress(
            opId(), event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("done")).toInt(),
            event.value(QStringLiteral("total")).toInt());
        return true;
    }

    if (type == QLatin1String("room_closure_result")) {
        QVariantMap result;
        result.insert(QStringLiteral("outcome"),
                      event.value(QStringLiteral("outcome")).toString());
        result.insert(QStringLiteral("joinRule"),
                      event.value(QStringLiteral("join_rule")).toString());
        result.insert(QStringLiteral("directory"),
                      event.value(QStringLiteral("directory")).toString());
        const QList<QPair<QString, QString>> counts{
            { QStringLiteral("unlisted"), QStringLiteral("unlisted") },
            { QStringLiteral("unlist_failed"), QStringLiteral("unlistFailed") },
            { QStringLiteral("removed"), QStringLiteral("removed") },
            { QStringLiteral("remove_failed"), QStringLiteral("removeFailed") },
            { QStringLiteral("not_attempted"), QStringLiteral("notAttempted") },
            { QStringLiteral("staying"), QStringLiteral("staying") },
        };
        for (const auto &[key, name] : counts)
            result.insert(name, event.value(key).toInt());
        result.insert(QStringLiteral("membersRead"),
                      event.value(QStringLiteral("members_read")).toBool());
        if (event.contains(QStringLiteral("can_kick")))
            result.insert(QStringLiteral("canKick"),
                          event.value(QStringLiteral("can_kick")).toBool());
        result.insert(QStringLiteral("left"),
                      event.value(QStringLiteral("left")).toBool());
        result.insert(QStringLiteral("category"),
                      event.value(QStringLiteral("category")).toString());
        Q_EMIT roomClosureFinished(
            opId(), event.value(QStringLiteral("room_id")).toString(), result);
        return true;
    }

    if (type == QLatin1String("event_cache_cleared")) {
        Q_EMIT eventCacheCleared(opId(),
                                 event.value(QStringLiteral("ok")).toBool());
        return true;
    }
    if (type == QLatin1String("cache_compacted")) {
        Q_EMIT cacheCompacted(
            opId(), event.value(QStringLiteral("store")).toString(),
            event.value(QStringLiteral("outcome")).toString(),
            static_cast<qint64>(
                event.value(QStringLiteral("bytes_before")).toDouble()),
            static_cast<qint64>(
                event.value(QStringLiteral("bytes_after")).toDouble()));
        return true;
    }
    if (type == QLatin1String("homeserver_info")) {
        QVariantMap info;
        info.insert(QStringLiteral("ok"), event.value(QStringLiteral("ok")).toBool());
        info.insert(QStringLiteral("address"),
                    event.value(QStringLiteral("address")).toString());
        info.insert(QStringLiteral("category"),
                    event.value(QStringLiteral("category")).toString());
        info.insert(QStringLiteral("softwareName"),
                    event.value(QStringLiteral("software_name")).toString());
        info.insert(QStringLiteral("softwareVersion"),
                    event.value(QStringLiteral("software_version")).toString());
        QStringList versions;
        for (const QJsonValue &v : event.value(QStringLiteral("versions")).toArray())
            versions.append(v.toString());
        info.insert(QStringLiteral("versions"), versions);
        QVariantList features;
        for (const QJsonValue &v : event.value(QStringLiteral("features")).toArray()) {
            const QJsonObject f = v.toObject();
            features.append(QVariantMap{
                {QStringLiteral("name"), f.value(QStringLiteral("name")).toString()},
                {QStringLiteral("enabled"), f.value(QStringLiteral("enabled")).toBool()}});
        }
        info.insert(QStringLiteral("features"), features);
        Q_EMIT homeserverInfoReceived(opId(), info);
        return true;
    }
    if (type == QLatin1String("account_data_listing")) {
        QVariantMap listing;
        listing.insert(QStringLiteral("ok"), event.value(QStringLiteral("ok")).toBool());
        listing.insert(QStringLiteral("roomId"),
                       event.value(QStringLiteral("room_id")).toString());
        listing.insert(QStringLiteral("truncated"),
                       event.value(QStringLiteral("truncated")).toBool());
        listing.insert(QStringLiteral("category"),
                       event.value(QStringLiteral("category")).toString());
        QVariantList entries;
        for (const QJsonValue &v : event.value(QStringLiteral("entries")).toArray()) {
            const QJsonObject e = v.toObject();
            entries.append(QVariantMap{
                {QStringLiteral("type"), e.value(QStringLiteral("type")).toString()},
                {QStringLiteral("redacted"), e.value(QStringLiteral("redacted")).toBool()},
                {QStringLiteral("json"), e.value(QStringLiteral("json")).toString()},
                {QStringLiteral("cut"), e.value(QStringLiteral("cut")).toBool()}});
        }
        listing.insert(QStringLiteral("entries"), entries);
        Q_EMIT accountDataListed(opId(), listing);
        return true;
    }

    if (type == QLatin1String("user_clients")) {
        QVariantList devices;
        for (const QJsonValue &v : event.value(QStringLiteral("devices")).toArray()) {
            const QJsonObject d = v.toObject();
            devices.append(QVariantMap{
                {QStringLiteral("deviceId"), d.value(QStringLiteral("device_id")).toString()},
                {QStringLiteral("name"), d.value(QStringLiteral("name")).toString()}});
        }
        Q_EMIT userClientsListed(
            opId(), QVariantMap{
                {QStringLiteral("ok"), event.value(QStringLiteral("ok")).toBool()},
                {QStringLiteral("userId"), event.value(QStringLiteral("user_id")).toString()},
                {QStringLiteral("devices"), devices}});
        return true;
    }

    if (type == QLatin1String("server_admin_status")) {
        Q_EMIT serverAdminStatusReceived(
            opId(), event.value(QStringLiteral("admin")).toBool(false),
            event.value(QStringLiteral("detail")).toString());
        return true;
    }

    if (type == QLatin1String("admin_room_delete_progress")) {
        Q_EMIT adminRoomDeleteProgress(
            opId(), event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("status")).toString());
        return true;
    }

    if (type == QLatin1String("admin_room_delete_result")) {
        QVariantMap result;
        result.insert(QStringLiteral("status"),
                      event.value(QStringLiteral("status")).toString());
        result.insert(QStringLiteral("ok"),
                      event.value(QStringLiteral("ok")).toBool(false));
        result.insert(QStringLiteral("removed"),
                      event.value(QStringLiteral("removed")).toInt());
        result.insert(QStringLiteral("failedToRemove"),
                      event.value(QStringLiteral("failed_to_remove")).toInt());
        result.insert(QStringLiteral("category"),
                      event.value(QStringLiteral("category")).toString());
        Q_EMIT adminRoomDeleteFinished(
            opId(), event.value(QStringLiteral("room_id")).toString(), result);
        return true;
    }

    if (type == QLatin1String("room_power_level_result")) {
        Q_EMIT powerLevelChangeFinished(
            opId(),
            event.value(QStringLiteral("room_id")).toString(),
            event.value(QStringLiteral("user_id")).toString(),
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
            static_cast<qlonglong>(
                event.value(QStringLiteral("level")).toDouble()),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("room_pinned")) {
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
                    row.value(QStringLiteral("sender_display_name")).toString().trimmed());
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

    if (type == QLatin1String("space_child_order_result")) {
        Q_EMIT spaceChildOrderFinished(opId(),
            event.value(QStringLiteral("space_id")).toString(),
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

    if (type == QLatin1String("push_rules_snapshot")) {
        Q_EMIT pushRulesReceived(
            opId(), event.value(QStringLiteral("ok")).toBool(),
            PushRulesSnapshot::fromRustEvent(event),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("push_rules_write")) {
        Q_EMIT pushRuleWriteFinished(
            opId(), event.value(QStringLiteral("kind")).toString(),
            event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("category")).toString());
        return true;
    }

    if (type == QLatin1String("ignored_users_changed")) {
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

    if (type == QLatin1String("password_change_probe")) {
        Q_EMIT passwordChangeProbed(
            opId(), event.value(QStringLiteral("known")).toBool(),
            event.value(QStringLiteral("can_change")).toBool(true),
            event.value(QStringLiteral("management_url")).toString());
        return true;
    }

    if (type == QLatin1String("room_key_export_progress")) {
        Q_EMIT roomKeyExportProgress(
            opId(), event.value(QStringLiteral("collected")).toInt());
        return true;
    }
    if (type == QLatin1String("room_key_export_result")) {
        const bool ok = event.value(QStringLiteral("ok")).toBool();
        const int exported = event.value(QStringLiteral("exported")).toInt();
        const QString category =
            event.value(QStringLiteral("category")).toString();
        qCInfo(lcRust) << "room key export finished ok=" << ok
                       << "exported=" << exported << "category=" << category;
        Q_EMIT roomKeyExportFinished(opId(), ok, exported, category);
        return true;
    }

    if (type == QLatin1String("password_change_result")) {
        Q_EMIT passwordChangeFinished(
            opId(), event.value(QStringLiteral("ok")).toBool(),
            event.value(QStringLiteral("category")).toString());
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
                row.value(QStringLiteral("sender_display_name")).toString().trimmed());
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
