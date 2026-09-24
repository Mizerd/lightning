#include "update/UpdateManager.h"

#include "storage/AppDataPaths.h"
#include "update/UpdateDownloader.h"
#include "update/UpdateEndpoints.h"
#include "update/SignatureVerifier.h"
#include "updater/ArtifactDigest.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

namespace lightning::update {
namespace {

constexpr char kSettingAutomaticChecks[] = "update/automaticChecks";
constexpr char kSettingLastCheck[] = "update/lastCheckTime";
constexpr char kSettingDismissedVersion[] = "update/dismissedVersion";

// This build never installs pre-releases. The decision is not left to the
// manifest's self-declared `channel`, which the guarded document could change.
constexpr bool kOffersPrereleases = false;

// Status written by both the helper and the application when the staged file
// no longer matches the signed digest; one spelling for both.
constexpr char kDigestMismatchStatus[] = "artifact-digest-mismatch";

// Downloads stream here; verified bytes are renamed to the manifest's filename
// before the helper sees them.
constexpr char kPartialArtifactTemplate[] = "lightning-update-XXXXXX.part";

constexpr char kStatusFileName[] = "update-status.json";
constexpr char kLockFileName[] = "update.lock";

// Diagnostic roles for artifactSource. Deliberately not a host or a URL.
constexpr char kSourceMirror[] = "mirror";
constexpr char kSourceCanonical[] = "canonical";

// Shown to the user, so bounded and stripped of control characters even
// though the helper writes only enum-derived tokens.
QString sanitizedStatusToken(const QJsonValue &value)
{
    if (!value.isString())
        return {};
    const QString text = value.toString().left(200);
    QString clean;
    clean.reserve(text.size());
    for (const QChar c : text) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7F)
            continue;
        clean.append(c);
    }
    return clean.trimmed();
}

} // namespace

UpdateManager::UpdateManager(QObject *parent)
    : QObject(parent)
    , m_trust(&m_compiledTrust)
{
    m_currentVersion = QCoreApplication::applicationVersion();
    if (m_currentVersion.isEmpty())
        m_currentVersion = QString::fromLatin1(APP_VERSION);

    m_detection = detectInstall();
    m_processStart = QDateTime::currentDateTimeUtc();

    m_automaticChecksEnabled =
        // On by default; the check is anonymous. UpdatesSettingsSection.qml,
        // README and docs/updates.md state this default and must change with
        // it.
        m_settings.value(QLatin1String(kSettingAutomaticChecks), true).toBool();
    m_lastCheckTime = m_settings.value(QLatin1String(kSettingLastCheck)).toDateTime();
    m_dismissedVersion = m_settings.value(QLatin1String(kSettingDismissedVersion)).toString();

    m_launcher = [](const QString &program, const QStringList &args) {
        // Argument vector only, no shell. The working directory is the helper's
        // own: on Windows the loader searches it, and inheriting the
        // installation directory would load libraries from the tree being
        // replaced.
        return QProcess::startDetached(program, args,
                                       QFileInfo(program).absolutePath());
    };

    // Derived from three pieces of state; raise the NOTIFY from their signals.
    connect(this, &UpdateManager::stateChanged,
            this, &UpdateManager::updateAvailableWarningChanged);
    connect(this, &UpdateManager::dismissedVersionChanged,
            this, &UpdateManager::updateAvailableWarningChanged);
    connect(this, &UpdateManager::updateInfoChanged,
            this, &UpdateManager::updateAvailableWarningChanged);

    // A previous run may have handed off to the helper; read its outcome.
    initializeStagingState();
}

UpdateManager::~UpdateManager()
{
    releaseLock();
}

bool UpdateManager::updateAvailableWarning() const
{
    // updateAvailable minus versions the user dismissed. ReadyToInstall counts.
    if (m_state != UpdateAvailable && m_state != ReadyToInstall)
        return false;
    if (m_latestVersion.isEmpty())
        return false;
    return m_latestVersion != m_dismissedVersion;
}

QDateTime UpdateManager::now() const
{
    return m_nowOverride.isValid() ? m_nowOverride : QDateTime::currentDateTimeUtc();
}

QByteArray UpdateManager::userAgent() const
{
    // Version-only, identical to the Matrix clients' user agent: an update
    // check must not be more identifying than ordinary traffic.
    return QByteArrayLiteral("Lightning/") + m_currentVersion.toLatin1();
}

QString UpdateManager::userAgentString() const
{
    return QString::fromLatin1(userAgent());
}

qreal UpdateManager::downloadProgress() const
{
    if (m_totalBytes <= 0)
        return 0.0;
    const qreal ratio = qreal(m_downloadedBytes) / qreal(m_totalBytes);
    return qBound(qreal(0.0), ratio, qreal(1.0));
}

QString UpdateManager::installTypeLabel() const
{
    return lightning::update::installTypeLabel(m_detection.type);
}

void UpdateManager::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    Q_EMIT stateChanged();
}

void UpdateManager::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message)
        return;
    m_errorMessage = message;
    Q_EMIT errorMessageChanged();
}

void UpdateManager::setStatusDetail(const QString &detail)
{
    if (m_statusDetail == detail)
        return;
    m_statusDetail = detail;
    Q_EMIT statusDetailChanged();
}

void UpdateManager::setArtifactSource(const QString &source)
{
    if (m_artifactSource == source)
        return;
    m_artifactSource = source;
    Q_EMIT artifactSourceChanged();
}

void UpdateManager::failWith(const QString &message)
{
    setErrorMessage(message);
    setState(Failed);
}

bool UpdateManager::isBusy() const
{
    return m_state == Checking || m_state == Downloading || m_state == Verifying
        || m_state == Installing;
}

void UpdateManager::setAutomaticChecksEnabled(bool enabled)
{
    if (m_automaticChecksEnabled == enabled)
        return;
    m_automaticChecksEnabled = enabled;
    m_settings.setValue(QLatin1String(kSettingAutomaticChecks), enabled);
    Q_EMIT automaticChecksEnabledChanged();
}

void UpdateManager::setTrustStoreForTest(const TrustStore *trust)
{
    m_trust = trust ? trust : static_cast<const TrustStore *>(&m_compiledTrust);
}

void UpdateManager::setInstallDetectionForTest(const InstallDetection &detection)
{
    m_detection = detection;
    Q_EMIT installTypeChanged();
}

void UpdateManager::setCurrentVersionForTest(const QString &version)
{
    if (m_currentVersion == version)
        return;
    m_currentVersion = version;
    Q_EMIT currentVersionChanged();
}

void UpdateManager::setStagingRootForTest(const QString &path)
{
    m_stagingRootOverride = path;
    // Re-run startup work so the override is what gets read and swept.
    initializeStagingState();
}

void UpdateManager::setProcessLauncherForTest(ProcessLauncher launcher)
{
    if (launcher)
        m_launcher = std::move(launcher);
}

void UpdateManager::setHelperPathForTest(const QString &path)
{
    m_helperPathOverride = path;
}

void UpdateManager::setNowForTest(const QDateTime &now)
{
    m_nowOverride = now;
}

void UpdateManager::setProcessStartForTest(const QDateTime &started)
{
    m_processStart = started;
}

void UpdateManager::setNetworkDisabledForTest(bool disabled)
{
    m_networkDisabledForTest = disabled;
}

void UpdateManager::setArtifactByteSourceForTest(ArtifactByteSource source)
{
    m_artifactByteSource = std::move(source);
}

void UpdateManager::setStagedArtifactForTest(const QString &path)
{
    m_stagedFile.reset();
    m_stagedPath = path;
    // Mirror the real path: promote verified bytes to the manifest's filename
    // when there is a manifest artifact and a file.
    if (m_artifact && QFileInfo::exists(path)) {
        QString error;
        if (!promoteStagedArtifact(&error)) {
            failWith(error);
            return;
        }
    }
    // The seam declares the bytes verified, so record their current digest; the
    // pre-launch re-check then guards the seam like a real download.
    m_stagedSha256 = updater::sha256HexOfFile(m_stagedPath);
    setState(ReadyToInstall);
}

// --- checking ------------------------------------------------------------

void UpdateManager::checkForUpdates()
{
    startCheck(/*automatic=*/false);
}

bool UpdateManager::maybeCheckAutomatically()
{
    if (!m_automaticChecksEnabled)
        return false;
    if (m_processStart.isValid() && m_processStart.msecsTo(now()) < kStartupQuietPeriodMs)
        return false;
    if (m_lastCheckTime.isValid() && m_lastCheckTime.msecsTo(now()) < kAutomaticCheckIntervalMs)
        return false;
    startCheck(/*automatic=*/true);
    return m_state == Checking;
}

void UpdateManager::startCheck(bool automatic)
{
    Q_UNUSED(automatic)
    if (isBusy())
        return;

    // A new check invalidates the previous download's provenance.
    setArtifactSource({});
    setErrorMessage({});
    setStatusDetail({});
    m_handoffSummary.clear();

    if (m_detection.type == InstallType::Development && !m_detection.developmentCheckAllowed) {
        failWith(QStringLiteral(
            "This is a development build; set LIGHTNING_ALLOW_DEV_UPDATE_CHECK=1 to check "
            "for updates. Installation stays disabled."));
        return;
    }
    if (m_trust == &m_compiledTrust && !hasUsableTrustedKey()) {
        failWith(QStringLiteral(
            "This build has no update signing key compiled in, so an update could not be "
            "verified. Please update manually."));
        return;
    }

    setState(Checking);
    m_signatureDocument.clear();
    m_metadataFromMirror = false;
    fetchSignature();
}

void UpdateManager::fetchSignature()
{
    if (m_networkDisabledForTest)
        return; // stays in Checking; the test supplies the documents itself
    if (!m_fetcher) {
        m_fetcher = new UpdateDocumentFetcher(network(), this);
    }
    disconnect(m_fetcher, nullptr, this, nullptr);
    connect(m_fetcher, &UpdateDocumentFetcher::finished, this,
            [this](bool ok, TransferError error, const QString &message) {
                Q_UNUSED(error)
                if (m_state != Checking)
                    return;
                if (!ok) {
                    if (retryMetadataFromMirror())
                        return;
                    failWith(message);
                    return;
                }
                m_signatureDocument = m_fetcher->document();
                fetchManifest();
            });
    m_fetcher->start(m_metadataFromMirror ? mirrorLatestManifestSignatureUrl()
                                          : latestManifestSignatureUrl(),
                     kMaxSignatureEnvelopeBytes, userAgent());
}

bool UpdateManager::retryMetadataFromMirror()
{
    // One fallback attempt away from the canonical host. The signature is still
    // verified against the compiled-in key and only strictly newer versions are
    // accepted, so this can only restore availability (see UpdateEndpoints.h).
    if (m_metadataFromMirror)
        return false;
    if (mirrorLatestManifestUrl().isEmpty()
        || mirrorLatestManifestSignatureUrl().isEmpty()) {
        return false;
    }
    m_metadataFromMirror = true;
    m_signatureDocument.clear();
    fetchSignature();
    return true;
}

void UpdateManager::fetchManifest()
{
    disconnect(m_fetcher, nullptr, this, nullptr);
    connect(m_fetcher, &UpdateDocumentFetcher::finished, this,
            [this](bool ok, TransferError error, const QString &message) {
                Q_UNUSED(error)
                if (m_state != Checking)
                    return;
                if (!ok) {
                    // Restart the pair from the mirror: a canonical signature
                    // over a mirrored manifest would fail as tampering rather
                    // than read as an outage.
                    if (retryMetadataFromMirror())
                        return;
                    failWith(message);
                    return;
                }
                const QByteArray manifestBytes = m_fetcher->document();
                const QByteArray signatureBytes = m_signatureDocument;
                m_signatureDocument.clear();
                applyCheckDocuments(manifestBytes, signatureBytes);
            });
    m_fetcher->start(m_metadataFromMirror ? mirrorLatestManifestUrl()
                                          : latestManifestUrl(),
                     UpdateManifest::kMaxManifestBytes, userAgent());
}

void UpdateManager::ingestCheckDocuments(const QByteArray &manifestBytes,
                                         const QByteArray &sigBytes)
{
    applyCheckDocuments(manifestBytes, sigBytes);
}

void UpdateManager::applyCheckDocuments(const QByteArray &manifestBytes,
                                        const QByteArray &sigBytes)
{
    m_lastCheckTime = now();
    m_settings.setValue(QLatin1String(kSettingLastCheck), m_lastCheckTime);
    Q_EMIT lastCheckTimeChanged();

    m_updateAvailable = false;
    m_latestVersion.clear();
    m_releaseNotes.clear();
    m_releaseNotesUrl.clear();
    m_artifact.reset();
    m_manifest = UpdateManifest();

    // parseVerified checks the signature over the raw bytes before any field is
    // trusted.
    const UpdateManifest::Result result =
        UpdateManifest::parseVerified(manifestBytes, sigBytes, *m_trust);
    if (!result.ok) {
        // A canonical answer that does not verify counts as no answer (a
        // hijacked domain or proxy returns 200 with wrong bytes). Retry the
        // mirror once; it is held to the same verification.
        if (retryMetadataFromMirror())
            return;
        Q_EMIT updateInfoChanged();
        failWith(result.message);
        return;
    }

    const std::optional<Version> installed = Version::parse(m_currentVersion);
    if (!installed) {
        Q_EMIT updateInfoChanged();
        failWith(QStringLiteral("This build reports a version that cannot be compared (%1).")
                     .arg(m_currentVersion));
        return;
    }

    m_manifest = result.manifest;

    // Freshness is informational, never a lock: installations must keep
    // updating from the mirror even if the project's servers disappear. Past
    // `expires` the decision is unchanged and a status line notes the
    // staleness. A replay can never install unsigned bytes or downgrade.
    decideFromManifest(*installed);
    const bool expired = m_manifest.expires().isValid() && m_manifest.expires() <= now();
    if ((expired || m_manifest.expiresMalformed()) && m_state != Failed) {
        const QString stale = expired
            ? QStringLiteral("Update information was expected to be refreshed by %1; the "
                             "project may be offline. Downloads still verify against the "
                             "signed release.")
                  .arg(m_manifest.expires().toString(Qt::ISODate))
            : QStringLiteral("Update information carries an unreadable refresh date and is "
                             "treated as stale. Downloads still verify against the signed "
                             "release.");
        setStatusDetail(m_statusDetail.isEmpty() ? stale
                                                 : m_statusDetail + QLatin1Char(' ') + stale);
    }
}

void UpdateManager::decideFromManifest(const Version &installed)
{
    const Version &remote = m_manifest.version();

    if (remote.isPrerelease() && !kOffersPrereleases) {
        setStatusDetail(QStringLiteral("A pre-release (%1) is published; this installation "
                                       "only installs releases.")
                            .arg(m_manifest.versionString()));
        Q_EMIT updateInfoChanged();
        setState(UpToDate);
        return;
    }

    if (remote <= installed) {
        if (remote < installed) {
            setStatusDetail(QStringLiteral("The published version (%1) is older than this "
                                           "installation (%2); no downgrade is offered.")
                                .arg(m_manifest.versionString(), m_currentVersion));
        }
        Q_EMIT updateInfoChanged();
        setState(UpToDate);
        return;
    }

    m_latestVersion = m_manifest.versionString();
    m_releaseNotes = m_manifest.releaseNotes();
    m_releaseNotesUrl = m_manifest.releaseNotesUrl();

    // Package-managed installs: only the channel block may declare an update,
    // and only when it is genuinely newer.
    if (isPackageManaged(m_detection.type)) {
        const QString channelId = UpdateManifest::channelIdForInstallType(m_detection.type);
        if (!m_manifest.channelOffersUpdate(channelId, installed)) {
            const std::optional<ManifestChannel> channel = m_manifest.channelFor(channelId);
            QString note = channel ? channel->note : QString();
            if (note.isEmpty()) {
                note = QStringLiteral("Version %1 has not been published for %2 yet.")
                           .arg(m_manifest.versionString(),
                                lightning::update::installTypeLabel(m_detection.type));
            }
            setStatusDetail(note);
            m_latestVersion.clear();
            Q_EMIT updateInfoChanged();
            setState(UpToDate);
            return;
        }
        m_updateAvailable = true;
        m_totalBytes = 0;
        m_downloadedBytes = 0;
        setStatusDetail(QStringLiteral("Updates for this installation are managed by %1.")
                            .arg(lightning::update::installTypeLabel(m_detection.type)));
        Q_EMIT downloadProgressChanged();
        Q_EMIT updateInfoChanged();
        setState(UpdateAvailable);
        return;
    }

    m_artifact = m_manifest.artifactFor(m_detection.type);
    if (!m_artifact) {
        setStatusDetail(QStringLiteral("Version %1 is available, but this release publishes no "
                                       "download for %2. Please update manually.")
                            .arg(m_manifest.versionString(),
                                 lightning::update::installTypeLabel(m_detection.type)));
    }

    m_updateAvailable = true;
    m_totalBytes = m_artifact ? m_artifact->size : 0;
    m_downloadedBytes = 0;
    Q_EMIT downloadProgressChanged();
    Q_EMIT updateInfoChanged();
    setState(UpdateAvailable);
}

// --- downloading ---------------------------------------------------------

QNetworkAccessManager *UpdateManager::network()
{
    if (!m_network) {
        auto *manager = new QNetworkAccessManager(this);
        // No automatic redirects; each hop is validated by the transfer.
        manager->setAutoDeleteReplies(false);
        manager->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
        m_network = manager;
    }
    return m_network.data();
}

QString UpdateManager::stagingRoot() const
{
    if (!m_stagingRootOverride.isEmpty())
        return m_stagingRootOverride;
    // Lightning-owned staging area. A portable copy stages inside its own
    // folder.
    const QString base = matrix::app_data::cacheRoot();
    return base + QStringLiteral("/updates");
}

QString UpdateManager::statusFilePath() const
{
    // Not account-scoped and free of sensitive data: the helper writes four
    // enum-derived fields.
    return QDir(stagingRoot()).absoluteFilePath(QLatin1String(kStatusFileName));
}

void UpdateManager::initializeStagingState()
{
    consumeUpdateStatusFile();
    sweepStaleStagedArtifacts();
}

void UpdateManager::consumeUpdateStatusFile()
{
    const QString path = statusFilePath();
    if (!QFileInfo::exists(path))
        return; // no handoff happened, or its result was already shown

    LastResult result = NoResult;
    QString mode;
    QString error;

    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray bytes = file.read(kMaxStatusBytes + 1);
        file.close();
        if (bytes.size() <= kMaxStatusBytes) {
            QJsonParseError parseError{};
            const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
            if (parseError.error == QJsonParseError::NoError && document.isObject()) {
                const QJsonObject object = document.object();
                const QJsonValue okValue = object.value(QLatin1String("ok"));
                // "ok" must be a real boolean; anything else is no result.
                if (okValue.isBool()) {
                    result = okValue.toBool() ? InstallSucceeded : InstallFailed;
                    mode = sanitizedStatusToken(object.value(QLatin1String("mode")));
                    error = sanitizedStatusToken(object.value(QLatin1String("error")));
                }
            }
        }
    }

    // Read once, then removed, even when unparseable.
    QFile::remove(path);

    if (result == NoResult)
        return;

    m_lastUpdateResult = result;
    m_lastUpdateMode = mode;
    m_lastUpdateError = error;
    Q_EMIT lastUpdateResultChanged();
}

void UpdateManager::clearLastUpdateResult()
{
    if (m_lastUpdateResult == NoResult && m_lastUpdateError.isEmpty()
        && m_lastUpdateMode.isEmpty()) {
        return;
    }
    m_lastUpdateResult = NoResult;
    m_lastUpdateError.clear();
    m_lastUpdateMode.clear();
    Q_EMIT lastUpdateResultChanged();
}

void UpdateManager::sweepStaleStagedArtifacts()
{
    const QString root = stagingRoot();
    QDir dir(root);
    if (!dir.exists())
        return;

    // Skip the sweep while another instance holds the staging lock: it may own
    // an older verified artifact or an in-flight download.
    {
        QLockFile probe(QDir(root).absoluteFilePath(QLatin1String(kLockFileName)));
        probe.setStaleLockTime(60 * 60 * 1000);
        if (probe.tryLock(0)) {
            probe.unlock();
        } else {
            // Defer only to a lock held by a different live process; a leftover
            // lock must not disable cleanup forever.
            qint64 pid = 0;
            QString host;
            QString application;
            if (probe.getLockInfo(&pid, &host, &application)
                && pid != QCoreApplication::applicationPid()) {
                return;
            }
        }
    }

    // The staging root belongs to this class; anything but the lock, the status
    // file, the in-flight ".part" and the promoted package is a leftover.
    // Bounded and non-recursive: one directory, no symlinks or subdirectories,
    // at most kMaxSweepEntries entries.
    const QFileInfoList entries =
        dir.entryInfoList(QDir::Files | QDir::NoSymLinks | QDir::NoDotAndDotDot,
                          QDir::Time);
    const QDateTime cutoff = now().addMSecs(-kStaleArtifactAgeMs);
    int inspected = 0;
    for (const QFileInfo &entry : entries) {
        if (++inspected > kMaxSweepEntries)
            break;
        if (entry.isSymLink() || entry.isDir())
            continue;
        const QString name = entry.fileName();
        if (name == QLatin1String(kLockFileName) || name.endsWith(QLatin1String(".lock")))
            continue;
        if (name == QLatin1String(kStatusFileName))
            continue;
        // Never delete the artifact this process is using.
        if (!m_stagedPath.isEmpty() && entry.absoluteFilePath() == m_stagedPath)
            continue;
        const QDateTime modified = entry.lastModified().toUTC();
        if (modified.isValid() && modified > cutoff)
            continue;
        QFile::remove(entry.absoluteFilePath());
    }
}

bool UpdateManager::promoteStagedArtifact(QString *error)
{
    Q_ASSERT(error);
    if (!m_artifact)
        return true; // nothing declares a name; leave the file where it is

    // The digest comes from the signed manifest, never from the file.
    m_stagedSha256 = m_artifact->sha256;

    const QString filename = m_artifact->filename;
    // Re-validate: this name becomes a path and a package-manager argument.
    if (!isSafeArtifactFilename(filename)) {
        *error = QStringLiteral("The update information named a file that cannot be used.");
        return false;
    }

    const QDir root(stagingRoot());
    const QString target = root.absoluteFilePath(filename);
    // Redundant with isSafeArtifactFilename, but a failure here would write
    // outside the staging root.
    if (QDir::cleanPath(QFileInfo(target).absolutePath())
        != QDir::cleanPath(root.absolutePath())) {
        *error = QStringLiteral("The update information named a file that cannot be used.");
        return false;
    }
    if (target == m_stagedPath)
        return true;

    // Release the QTemporaryFile before renaming (autoRemove is off).
    if (m_stagedFile) {
        m_stagedFile->close();
        m_stagedFile.reset();
    }

    const QFileInfo existing(target);
    if (existing.exists() || existing.isSymLink()) {
        // A leftover from an earlier run.
        if (!QFile::remove(target)) {
            *error = QStringLiteral("The verified update file could not be prepared for "
                                    "installation.");
            return false;
        }
    }
    if (!QFile::rename(m_stagedPath, target)) {
        *error = QStringLiteral("The verified update file could not be prepared for "
                                "installation.");
        return false;
    }

    m_stagedPath = target;
    // The temp file was 0600; keep it that way under its new name.
    QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner);
    return true;
}

bool UpdateManager::acquireLock()
{
    if (m_lock)
        return true;
    QDir dir;
    if (!dir.mkpath(stagingRoot()))
        return false;
    // Owner-only: the promoted name discloses the staged version.
    QFile::setPermissions(stagingRoot(),
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    auto lock = std::make_unique<QLockFile>(
        QDir(stagingRoot()).absoluteFilePath(QLatin1String(kLockFileName)));
    lock->setStaleLockTime(60 * 60 * 1000);
    if (!lock->tryLock(0))
        return false;
    m_lock = std::move(lock);
    return true;
}

void UpdateManager::releaseLock()
{
    if (!m_lock)
        return;
    m_lock->unlock();
    m_lock.reset();
}

void UpdateManager::discardStagedArtifact()
{
    if (m_stagedFile) {
        m_stagedFile->close();
        m_stagedFile->remove();
        m_stagedFile.reset();
    } else if (!m_stagedPath.isEmpty()) {
        QFile::remove(m_stagedPath);
    }
    m_stagedPath.clear();
    m_stagedSha256.clear();
}

bool UpdateManager::stagedArtifactStillVerifies() const
{
    if (m_stagedPath.isEmpty() || m_stagedSha256.isEmpty())
        return false;
    const QString digest = updater::sha256HexOfFile(m_stagedPath);
    return !digest.isEmpty() && digest == m_stagedSha256;
}

void UpdateManager::writeLocalStatusFailure(const QString &error)
{
    // Same shape as src/updater/main.cpp writeStatus(), so the next launch
    // reads it the same way.
    QJsonObject object;
    object.insert(QStringLiteral("ok"), false);
    object.insert(QStringLiteral("mode"), installTypeString());
    object.insert(QStringLiteral("error"), error);
    object.insert(QStringLiteral("timestamp"), now().toUTC().toString(Qt::ISODate));
    QFile file(statusFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    file.close();
}

void UpdateManager::downloadUpdate()
{
    if (m_state != UpdateAvailable)
        return;
    if (!m_detection.automaticInstallAllowed) {
        Q_EMIT installRefused(QStringLiteral(
            "Lightning does not install updates for this installation type (%1).")
                                  .arg(installTypeLabel()));
        return;
    }
    if (!m_artifact) {
        failWith(QStringLiteral("This release publishes no download for %1.")
                     .arg(installTypeLabel()));
        return;
    }
    if (!acquireLock()) {
        failWith(QStringLiteral("Another Lightning instance is updating."));
        return;
    }

    m_handoffSummary.clear();
    m_mirrorFallbackUsed = false;
    m_fallbackPending = false;
    setArtifactSource({});

    // Connected once per download: the canonical fallback restarts this same
    // downloader, and reconnecting from inside finished() is best avoided.
    if (!m_downloader)
        m_downloader = new UpdateDownloader(network(), this);
    disconnect(m_downloader, nullptr, this, nullptr);
    connect(m_downloader, &UpdateDownloader::progress, this,
            [this](qint64 received, qint64 total) {
                m_downloadedBytes = received;
                if (total > 0)
                    m_totalBytes = total;
                Q_EMIT downloadProgressChanged();
            });
    connect(m_downloader, &UpdateDownloader::finished, this,
            &UpdateManager::handleDownloadFinished);

    // Mirror first when the signed manifest names one. It only supplies bytes
    // the manifest already named, sized and hashed, verified against the same
    // sha256.
    beginDownloadAttempt(m_artifact->mirrorUrl.isEmpty() ? ArtifactSource::Canonical
                                                         : ArtifactSource::Mirror);
}

void UpdateManager::beginDownloadAttempt(ArtifactSource source)
{
    if (!m_artifact || !m_downloader)
        return;

    m_attemptSource = source;
    m_fallbackPending = false;
    // A deferred install names the file this attempt is about to delete.
    m_deferredInstallPending = false;

    // Remove the previous partial file before writing, so a failed mirror
    // attempt can never contribute bytes to what gets installed.
    discardStagedArtifact();

    auto file = std::make_unique<QTemporaryFile>(
        QDir(stagingRoot()).absoluteFilePath(QLatin1String(kPartialArtifactTemplate)));
    // 0600 with an unpredictable name. Auto-removal is off because the file
    // must outlive this scope; failure paths remove it by hand.
    file->setAutoRemove(false);
    if (!file->open()) {
        releaseLock();
        failWith(QStringLiteral("A temporary file for the download could not be created."));
        return;
    }
    file->setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    m_stagedPath = file->fileName();
    m_stagedFile = std::move(file);

    // Restart progress so a fallback does not double-count.
    m_downloadedBytes = 0;
    m_totalBytes = m_artifact->size;
    Q_EMIT downloadProgressChanged();

    const QUrl url =
        source == ArtifactSource::Mirror ? m_artifact->mirrorUrl : m_artifact->url;

    setState(Downloading);
    if (m_artifactByteSource) {
        // Test seam: same downloader, file, size and SHA-256 checks; no
        // transport.
        m_downloader->deliverForTest(url, m_artifact->size, m_artifact->sha256,
                                     m_stagedFile.get(), m_artifactByteSource(url));
        return;
    }
    m_downloader->start(url, m_artifact->size, m_artifact->sha256, m_stagedFile.get(),
                        userAgent());
}

void UpdateManager::handleDownloadFinished(bool ok, TransferError error, const QString &message)
{
    if (m_state != Downloading)
        return;

    if (!ok) {
        // Every failure, including a hash mismatch, deletes the bytes. There is
        // no "install anyway" path.
        discardStagedArtifact();

        if (error == TransferError::Cancelled) {
            m_fallbackPending = false;
            releaseLock();
            setArtifactSource({});
            setStatusDetail(QStringLiteral("The download was cancelled."));
            setState(UpdateAvailable);
            return;
        }

        if (m_attemptSource == ArtifactSource::Mirror) {
            // One retry at the canonical address with the same sha256; never a
            // third attempt. Queued so the downloader is not restarted from
            // inside its own finished() emission. The lock is kept across the
            // gap.
            m_mirrorFallbackUsed = true;
            m_fallbackPending = true;
            setStatusDetail(QStringLiteral(
                "The mirror copy could not be used; retrying from the canonical source."));
            QMetaObject::invokeMethod(
                this,
                [this]() {
                    if (!m_fallbackPending || m_state != Downloading)
                        return; // cancelled, or superseded, in the meantime
                    beginDownloadAttempt(ArtifactSource::Canonical);
                },
                Qt::QueuedConnection);
            return;
        }

        releaseLock();
        setArtifactSource({});
        failWith(message);
        return;
    }

    setState(Verifying);
    // The downloader verified while streaming; re-check what landed on disk.
    if (m_stagedFile)
        m_stagedFile->close();
    const QFileInfo info(m_stagedPath);
    if (!info.exists() || info.size() != (m_artifact ? m_artifact->size : -1)) {
        discardStagedArtifact();
        releaseLock();
        setArtifactSource({});
        failWith(QStringLiteral("The downloaded file did not survive verification."));
        return;
    }
    // Only now, verified, does the file take the manifest's name (renamed
    // within the staging directory). The extension is what makes
    // apt-get/dnf/msiexec treat the argument as a local package.
    QString promoteError;
    if (!promoteStagedArtifact(&promoteError)) {
        discardStagedArtifact();
        releaseLock();
        setArtifactSource({});
        failWith(promoteError);
        return;
    }
    setArtifactSource(m_attemptSource == ArtifactSource::Mirror
                          ? QString::fromLatin1(kSourceMirror)
                          : QString::fromLatin1(kSourceCanonical));
    // Disclose a mirror fallback even when the update succeeded.
    setStatusDetail(m_mirrorFallbackUsed
                        ? QStringLiteral("The mirror copy could not be used, so this update "
                                         "was downloaded from the canonical source.")
                        : QString());
    setState(ReadyToInstall);
}

void UpdateManager::cancelDownload()
{
    if (m_state != Downloading)
        return;
    if (m_fallbackPending) {
        // Cancelled between a failed mirror attempt and the queued canonical
        // retry; no live transfer exists, so stop the fallback here.
        m_fallbackPending = false;
        discardStagedArtifact();
        releaseLock();
        setArtifactSource({});
        setStatusDetail(QStringLiteral("The download was cancelled."));
        setState(UpdateAvailable);
        return;
    }
    if (!m_downloader)
        return;
    m_downloader->cancel();
}

// --- installing ----------------------------------------------------------

QString UpdateManager::helperProgramPath() const
{
    if (!m_helperPathOverride.isEmpty())
        return m_helperPathOverride;
    QString name = QStringLiteral("lightning-updater");
#ifdef Q_OS_WIN
    name += QStringLiteral(".exe");
#endif
    return QCoreApplication::applicationDirPath() + QLatin1Char('/') + name;
}

QString UpdateManager::explainInstallError(const QString &token)
{
    // The helper reports failures as enum tokens so no path, command line or
    // process output reaches the UI. This maps each token to a sentence,
    // grouped by what the user should do. Unknown tokens return empty and the
    // caller keeps its generic wording.
    const QString key = token.trimmed().toLower();
    if (key.isEmpty())
        return QString();

    // Per-machine install and the UAC prompt was declined (by the helper, or by
    // the setup EXE: exit 1223, ERROR_CANCELLED). Nothing changed; must precede
    // the generic installer-exit wording.
    if (key == QLatin1String("elevation-declined")
        || key == QLatin1String("installer-exit-1223")) {
        return tr("Lightning is installed for all users of this computer, so "
                  "installing an update needs administrator approval, and "
                  "Windows did not get it. Nothing was changed. Approve the "
                  "prompt next time, or ask an administrator to install the "
                  "update.");
    }

    // The installer ran and refused; the exit code is the detail to report.
    if (key.startsWith(QLatin1String("installer-exit-"))) {
        const QString code = key.mid(QStringLiteral("installer-exit-").size());
        return tr("The Windows installer refused the update (code %1). "
                  "Installing the new version over the old one by hand "
                  "usually works, and the code is worth reporting.")
            .arg(code);
    }

    // Integrity: never soften these.
    if (key == QLatin1String("artifact-digest-mismatch")
        || key == QLatin1String("checksum-mismatch")
        || key == QLatin1String("invalid-digest")) {
        return tr("The downloaded update did not match the signed release, so "
                  "it was discarded and nothing was changed. This is usually a "
                  "corrupted download. Checking again is safe.");
    }

    // The archive itself could not be trusted or read.
    if (key == QLatin1String("not-a-zip-archive")
        || key == QLatin1String("layout-invalid")
        || key == QLatin1String("inflate-failed")
        || key == QLatin1String("inflate-unavailable")
        || key.startsWith(QLatin1String("unsupported-"))
        || key == QLatin1String("archive-too-large")
        || key == QLatin1String("too-many-entries")
        || key == QLatin1String("entry-size-exceeded")
        || key == QLatin1String("total-size-exceeded")
        || key == QLatin1String("compression-ratio-exceeded")
        || key == QLatin1String("symlink-entry-rejected")
        || key == QLatin1String("unsafe-entry-path")) {
        return tr("The downloaded update could not be read and nothing was "
                  "changed. Checking for the update again is safe.");
    }

    // Somewhere to write, or permission to write there.
    if (key == QLatin1String("target-not-writable")
        || key == QLatin1String("write-failed")
        || key == QLatin1String("copy-failed")
        || key == QLatin1String("open-failed")
        || key == QLatin1String("destination-unusable")
        || key == QLatin1String("backup-failed")
        || key == QLatin1String("backup-path-unusable")
        || key == QLatin1String("destination-not-empty")) {
        return tr("Lightning could not write to its own installation folder, "
                  "so nothing was changed. This usually means another program "
                  "is holding files open, or the folder needs administrator "
                  "rights. Closing Lightning fully and installing the new "
                  "version by hand will work.");
    }

    // The application never got out of the way.
    if (key == QLatin1String("timed-out")) {
        return tr("Lightning did not finish closing, so the update was "
                  "cancelled and nothing was changed. If Lightning is set to "
                  "keep running in the tray, quit it from the tray first, then "
                  "install the update.");
    }

    // The old build is back and intact.
    if (key == QLatin1String("promote-failed")) {
        return tr("The new version could not be put in place, so the previous "
                  "one was restored. Nothing was lost.");
    }

    // The one genuinely dangerous outcome: the rollback ALSO failed.
    if (key == QLatin1String("rollback-failed")) {
        return tr("The update failed and the previous version could not be "
                  "fully restored. Please reinstall Lightning from the "
                  "downloads page. Your messages and account are on the "
                  "server and are not affected.");
    }

    if (key == QLatin1String("installer-did-not-run")) {
        return tr("The Windows installer could not be started, so nothing was "
                  "changed. Installing the new version by hand will work.");
    }

    if (key == QLatin1String("elevation-helper-missing")
        || key == QLatin1String("no-package-manager-found")
        || key == QLatin1String("not-self-installable")
        || key == QLatin1String("mode-not-self-installable")) {
        return tr("This installation cannot update itself, so nothing was "
                  "changed. Install the new version the same way this copy "
                  "was installed.");
    }

    // Safety refusals and argument faults: stopped before anything moved, and
    // they indicate a bug in Lightning.
    if (key == QLatin1String("refused-unsafe-path")
        || key == QLatin1String("target-missing")
        || key == QLatin1String("source-missing")
        || key == QLatin1String("unhandled-mode")
        || key == QLatin1String("unknown-mode")
        || key.startsWith(QLatin1String("path-"))
        || key.startsWith(QLatin1String("target-"))
        || key.startsWith(QLatin1String("source-"))
        || key.startsWith(QLatin1String("invalid-"))
        || key.startsWith(QLatin1String("missing-"))
        || key.startsWith(QLatin1String("unknown-"))
        || key.startsWith(QLatin1String("unsafe-"))
        || key.startsWith(QLatin1String("empty-"))
        || key.startsWith(QLatin1String("duplicate-"))) {
        return tr("The update was stopped by one of Lightning's own safety "
                  "checks and nothing was changed. This is a fault in "
                  "Lightning rather than anything you did. Please report it "
                  "with the code below.");
    }
    return QString();
}

QStringList UpdateManager::helperRuntimeLibraries()
{
    // The full runtime closure, not just the helper's direct imports: the
    // Windows loader resolves the whole graph before main(). A missing DLL
    // either kills the staged copy silently, or is found in the installation
    // directory and mapped there, blocking the installer again.
    //
    // From the payload's runtime-dependencies.json:
    //   lightning-updater.exe -> Qt6Core, libgcc_s_seh-1, libstdc++-6, zlib1
    //   Qt6Core.dll           -> icui18n77, icuuc77, libpcre2-16-0,
    //                            libwinpthread-1, libgcc_s_seh-1,
    //                            libstdc++-6, zlib1
    //   icuuc77.dll           -> icudata77
    //   icui18n77.dll         -> icuuc77
    //
    // The ICU version will move, so this list is a floor: stems below are also
    // matched, and the Windows artifact validation fails the build if the real
    // import graph is not covered.
    return {
        QStringLiteral("Qt6Core.dll"),
        QStringLiteral("libgcc_s_seh-1.dll"),
        QStringLiteral("libstdc++-6.dll"),
        QStringLiteral("libwinpthread-1.dll"),
        QStringLiteral("zlib1.dll"),
        QStringLiteral("libpcre2-16-0.dll"),
        // Versioned; also matched by stem.
        QStringLiteral("icuuc77.dll"),
        QStringLiteral("icui18n77.dll"),
        QStringLiteral("icudata77.dll"),
    };
}

QStringList UpdateManager::helperRuntimeLibraryStems()
{
    // Every DLL beside the helper matching one of these stems is copied too, so
    // an ICU version bump cannot break updates.
    return {
        QStringLiteral("icuuc"),
        QStringLiteral("icui18n"),
        QStringLiteral("icudata"),
        QStringLiteral("libpcre2-16"),
    };
}

QString UpdateManager::stageHelperOutsideInstallation(const QString &helperPath,
                                                      const QString &installDir,
                                                      QString *error)
{
    // The MSI and NSIS setup rewrite the installation directory, where the
    // running helper and its Qt/mingw DLLs are mapped; Windows refuses to
    // overwrite a mapped image, so the install would fail on the helper's own
    // files. Copy the helper and its libraries outside the installation and run
    // that copy. (The portable path renames entries, which Windows allows, and
    // does not need this.) The next run's fresh directory replaces the copy.
    if (error)
        error->clear();
    const QFileInfo helperInfo(helperPath);
    if (!helperInfo.exists() || !helperInfo.isFile()) {
        if (error)
            *error = QStringLiteral("the updater helper is missing");
        return QString();
    }

    const QString base = QDir(QDir::tempPath())
                             .absoluteFilePath(QStringLiteral("lightning-updater-staged"));
    QDir staged(base);
    if (staged.exists())
        staged.removeRecursively();
    if (!QDir().mkpath(base)) {
        if (error)
            *error = QStringLiteral("could not create a staging directory for the updater");
        return QString();
    }

    // Must not land inside the installation.
    const QString cleanInstall =
        QDir::cleanPath(QDir(installDir).absolutePath());
    const QString cleanStaged = QDir::cleanPath(QDir(base).absolutePath());
    if (!cleanInstall.isEmpty()
        && (cleanStaged == cleanInstall
            || cleanStaged.startsWith(cleanInstall + QLatin1Char('/')))) {
        if (error)
            *error = QStringLiteral("the updater staging directory is inside the installation");
        return QString();
    }

    const QString stagedHelper =
        QDir(base).absoluteFilePath(helperInfo.fileName());
    if (!QFile::copy(helperPath, stagedHelper)) {
        if (error)
            *error = QStringLiteral("could not copy the updater helper out of the installation");
        return QString();
    }
    QFile::setPermissions(stagedHelper,
                          QFile::permissions(stagedHelper) | QFile::ExeOwner
                              | QFile::ReadOwner | QFile::WriteOwner);

    // A failed library copy is fatal: the staged helper would die in the loader
    // or map the installation's copy, and both look like success from here. A
    // missing source is tolerated because Linux trees have none; the Windows
    // artifact validation proves the payload carries them.
    const QDir source(helperInfo.absolutePath());
    QStringList wanted = helperRuntimeLibraries();
    // Plus everything matching a versioned stem.
    const QStringList stems = helperRuntimeLibraryStems();
    const QStringList beside =
        source.entryList(QStringList{ QStringLiteral("*.dll") }, QDir::Files);
    for (const QString &candidate : beside) {
        for (const QString &stem : stems) {
            if (candidate.startsWith(stem, Qt::CaseInsensitive)
                && !wanted.contains(candidate, Qt::CaseInsensitive)) {
                wanted << candidate;
                break;
            }
        }
    }
    for (const QString &library : wanted) {
        const QString from = source.absoluteFilePath(library);
        if (!QFile::exists(from))
            continue;
        const QString to = QDir(base).absoluteFilePath(library);
        if (!QFile::copy(from, to)) {
            if (error)
                *error = QStringLiteral("could not stage the updater's libraries");
            return QString();
        }
    }
    return stagedHelper;
}

namespace {

// A per-machine upgrade raises UAC from the windowless helper. Windows only
// grants the foreground to a process the foreground one allowed, so without
// this the prompt may only flash in the taskbar.
void allowTheHelperToTakeTheForeground()
{
#ifdef Q_OS_WIN
    AllowSetForegroundWindow(ASFW_ANY);
#endif
}

} // namespace

bool UpdateManager::installNeedsAdministrator() const
{
    return m_detection.scope == InstallScope::Machine
        && (m_detection.type == InstallType::WindowsMsi
            || m_detection.type == InstallType::WindowsSetup);
}

QString UpdateManager::installTargetPath() const
{
    switch (m_detection.type) {
    case InstallType::LinuxAppImage: {
        // Replaces the running .AppImage file.
        const QString appImage = qEnvironmentVariable("APPIMAGE");
        if (!appImage.isEmpty())
            return appImage;
        break;
    }
    case InstallType::WindowsPortable:
        // The portable strategy swaps the whole install directory; UpdaterArgs
        // refuses a non-directory --target for this mode.
        return QCoreApplication::applicationDirPath();
    default:
        break;
    }
    return QCoreApplication::applicationFilePath();
}

QString UpdateManager::relaunchProgramPath() const
{
    // Inside an AppImage, applicationFilePath() is in the mounted squashfs;
    // relaunching it would start the old version. Run the replaced .AppImage.
    if (m_detection.type == InstallType::LinuxAppImage) {
        const QString appImage = qEnvironmentVariable("APPIMAGE");
        if (!appImage.isEmpty())
            return appImage;
    }
    return QCoreApplication::applicationFilePath();
}

void UpdateManager::installUpdate()
{
    startInstall(/*restartAfterwards=*/false);
}

void UpdateManager::installAndRestart()
{
    startInstall(/*restartAfterwards=*/true);
}

void UpdateManager::startInstall(bool restartAfterwards)
{
    if (m_state != ReadyToInstall)
        return;

    // Policy refusal is terminal; there is no override.
    if (!m_detection.automaticInstallAllowed) {
        const QString reason = isPackageManaged(m_detection.type)
            ? QStringLiteral("Updates for this installation are managed by %1.")
                  .arg(installTypeLabel())
            : QStringLiteral("Lightning does not install updates for %1 installations.")
                  .arg(installTypeLabel());
        Q_EMIT installRefused(reason);
        return;
    }
    if (m_stagedPath.isEmpty() || !QFileInfo::exists(m_stagedPath)) {
        failWith(QStringLiteral("The verified update file is no longer available."));
        return;
    }
    // Prove the file is still the verified bytes before handing its path to
    // something running as root. See ArtifactDigest.h.
    if (!stagedArtifactStillVerifies()) {
        discardStagedArtifact();
        releaseLock();
        setArtifactSource({});
        failWith(QStringLiteral("The downloaded update no longer matches the signed release "
                                "and has been discarded. Please download it again."));
        return;
    }

    QString program = helperProgramPath();
    const QFileInfo helper(program);
    if (!helper.exists() || !helper.isFile()) {
        failWith(QStringLiteral("The updater helper is missing from this installation."));
        return;
    }

    // The MSI and setup EXE rewrite the installation directory, where the
    // helper is running (see stageHelperOutsideInstallation). Portable and
    // AppImage do their own file work and run in place.
    if (m_detection.type == InstallType::WindowsMsi
        || m_detection.type == InstallType::WindowsSetup) {
        QString stageError;
        const QString staged = stageHelperOutsideInstallation(
            program, QCoreApplication::applicationDirPath(), &stageError);
        if (staged.isEmpty()) {
            failWith(QStringLiteral("The update could not be prepared (%1).")
                         .arg(stageError.isEmpty()
                                  ? QStringLiteral("the updater helper could not be staged")
                                  : stageError));
            return;
        }
        program = staged;
    }

    // Resolve symlinks first: the helper refuses a symlink on every path
    // option.
    const auto resolved = [](const QString &path) {
        const QString canonical = QFileInfo(path).canonicalFilePath();
        return canonical.isEmpty() ? path : canonical;
    };
    QStringList arguments{
        QStringLiteral("--mode"),     installTypeString(),
        QStringLiteral("--artifact"), QFileInfo(m_stagedPath).absoluteFilePath(),
        QStringLiteral("--pid"),      QString::number(QCoreApplication::applicationPid()),
        QStringLiteral("--target"),   resolved(installTargetPath()),
        QStringLiteral("--status"),   statusFilePath(),
        QStringLiteral("--sha256"),   m_stagedSha256,
    };
    // --relaunch only for "install and restart"; installUpdate() means "apply
    // when I quit".
    if (restartAfterwards) {
        arguments << QStringLiteral("--relaunch") << resolved(relaunchProgramPath());
    }
    // Upgrade in the installation's own scope: a per-machine MSI without
    // ALLUSERS=1 installs a second copy, and an unelevated setup cannot write
    // Program Files. The helper refuses this option for other modes.
    if (m_detection.type == InstallType::WindowsMsi
        || m_detection.type == InstallType::WindowsSetup) {
        arguments << QStringLiteral("--install-scope") << installScopeId(m_detection.scope);
    }

    setState(Installing);
    m_lastLaunchProgram = program;
    m_lastLaunchArguments = arguments;

    // The helper waits for this process to exit for at most
    // kDefaultWaitTimeoutMs (src/updater/ProcessWaiter.h) and then gives up.
    // "Install and restart" launches now because the quit follows immediately;
    // "install without restarting" defers the launch to aboutToQuit.
    if (restartAfterwards) {
        if (installNeedsAdministrator())
            allowTheHelperToTakeTheForeground();
        if (!m_launcher || !m_launcher(program, arguments)) {
            failWith(QStringLiteral("The updater helper could not be started."));
            return;
        }
    } else {
        if (!m_launcher) {
            failWith(QStringLiteral("The updater helper could not be started."));
            return;
        }
        m_deferredInstallPending = true;
        if (!m_deferredInstallConnected) {
            m_deferredInstallConnected = true;
            if (QCoreApplication *app = QCoreApplication::instance()) {
                connect(app, &QCoreApplication::aboutToQuit, this,
                        &UpdateManager::launchDeferredInstall);
            }
        }
    }

    // The helper is launched (or armed), not finished. The real outcome arrives
    // on the next launch via lastUpdateResult.
    m_handoffSummary = restartAfterwards
        ? QStringLiteral("Lightning will close now so the update can be applied, then "
                         "start again. Nothing has been installed yet.")
        : QStringLiteral("The update will be applied when you quit Lightning, however "
                         "long that is. Nothing has been installed yet.");
    if (installNeedsAdministrator()) {
        m_handoffSummary += QStringLiteral(
            " Lightning is installed for all users, so Windows will ask for "
            "administrator approval before the update is installed.");
    }
    setStatusDetail(m_handoffSummary);
    setState(RestartRequired);
    if (restartAfterwards)
        Q_EMIT quitRequested();
}

// aboutToQuit handler for "apply when I quit": the helper's bounded wait for
// this PID starts as the process is actually exiting.
void UpdateManager::launchDeferredInstall()
{
    if (!m_deferredInstallPending)
        return;
    m_deferredInstallPending = false;
    if (m_lastLaunchProgram.isEmpty() || !m_launcher)
        return;
    // The file has sat at a predictable path for the whole session; re-hash it.
    // On mismatch the helper never starts, the file is removed, and the refusal
    // goes to the status file (the UI is gone).
    if (!stagedArtifactStillVerifies()) {
        // "Gone" and "changed" need different next steps.
        writeLocalStatusFailure(QFileInfo::exists(m_stagedPath)
                                    ? QString::fromLatin1(kDigestMismatchStatus)
                                    : QStringLiteral("artifact-missing"));
        discardStagedArtifact();
        return;
    }
    // If the launch fails, no status file is written, so the next launch shows
    // no result rather than a false success.
    if (installNeedsAdministrator())
        allowTheHelperToTakeTheForeground();
    m_launcher(m_lastLaunchProgram, m_lastLaunchArguments);
}

// --- user actions --------------------------------------------------------

void UpdateManager::dismissVersion()
{
    if (m_latestVersion.isEmpty())
        return;
    if (m_dismissedVersion == m_latestVersion)
        return;
    m_dismissedVersion = m_latestVersion;
    m_settings.setValue(QLatin1String(kSettingDismissedVersion), m_dismissedVersion);
    Q_EMIT dismissedVersionChanged();
}

QString UpdateManager::managedUpdateCommand() const
{
    switch (m_detection.type) {
    case InstallType::LinuxFlatpak: {
        const QString appId = qEnvironmentVariable("FLATPAK_ID");
        return appId.isEmpty() ? QStringLiteral("flatpak update")
                               : QStringLiteral("flatpak update %1").arg(appId);
    }
    case InstallType::LinuxSnap: {
        const QString name = qEnvironmentVariable("SNAP_NAME");
        return name.isEmpty() ? QStringLiteral("snap refresh")
                              : QStringLiteral("snap refresh %1").arg(name);
    }
    default:
        break;
    }
    return {};
}

void UpdateManager::openManagedUpdateHelp()
{
    const QString command = managedUpdateCommand();
    QString explanation;
    switch (m_detection.type) {
    case InstallType::LinuxFlatpak:
        explanation = QStringLiteral(
            "Updates for this installation are managed by Flatpak. Use your software centre, "
            "or run the command below.");
        break;
    case InstallType::LinuxSnap:
        explanation = QStringLiteral(
            "Updates for this installation are managed by Snap. Use your software centre, or "
            "run the command below.");
        break;
    default:
        explanation = QStringLiteral(
            "Updates for this installation are managed outside Lightning. Please update it the "
            "same way you installed it.");
        break;
    }
    Q_EMIT managedUpdateHelpRequested(command, explanation);
}

} // namespace lightning::update
