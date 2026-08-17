#include "update/UpdateManager.h"

#include "update/UpdateDownloader.h"
#include "update/UpdateEndpoints.h"
#include "update/SignatureVerifier.h"

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

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

namespace lightning::update {
namespace {

constexpr char kSettingAutomaticChecks[] = "update/automaticChecks";
constexpr char kSettingLastCheck[] = "update/lastCheckTime";
constexpr char kSettingDismissedVersion[] = "update/dismissedVersion";

// The stable release channel. Prereleases are never offered on it.
constexpr char kStableChannel[] = "stable";

// The temp name a download streams into. The verified bytes are renamed to
// the manifest's own filename before the helper ever sees them.
constexpr char kPartialArtifactTemplate[] = "lightning-update-XXXXXX.part";

constexpr char kStatusFileName[] = "update-status.json";
constexpr char kLockFileName[] = "update.lock";

// Diagnostic roles for artifactSource. Deliberately not a host or a URL.
constexpr char kSourceMirror[] = "mirror";
constexpr char kSourceCanonical[] = "canonical";

// The helper writes only enum-derived tokens here, but this string is shown
// to a user, so it is bounded and stripped of anything that could rewrite a
// line of UI. No path, token or command can be in it in the first place.
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
        // v0.7.3: ON by default (maintainer decision). An installation that
        // never learns a newer version exists is the worse outcome, and the
        // check itself remains anonymous — see the copy in
        // UpdatesSettingsSection.qml, README and docs/updates.md, all of
        // which state this default and must be changed together with it.
        m_settings.value(QLatin1String(kSettingAutomaticChecks), true).toBool();
    m_lastCheckTime = m_settings.value(QLatin1String(kSettingLastCheck)).toDateTime();
    m_dismissedVersion = m_settings.value(QLatin1String(kSettingDismissedVersion)).toString();

    m_launcher = [](const QString &program, const QStringList &args) {
        // Argument VECTOR only. No shell, no command string, no
        // interpolation of anything that came off the network.
        return QProcess::startDetached(program, args);
    };

    // updateAvailableWarning is derived from three separate pieces of state;
    // deriving the NOTIFY from their own signals means no future assignment
    // can forget to raise it.
    connect(this, &UpdateManager::stateChanged,
            this, &UpdateManager::updateAvailableWarningChanged);
    connect(this, &UpdateManager::dismissedVersionChanged,
            this, &UpdateManager::updateAvailableWarningChanged);
    connect(this, &UpdateManager::updateInfoChanged,
            this, &UpdateManager::updateAvailableWarningChanged);

    // The previous run may have handed an artifact to the helper and quit.
    // Its outcome is a file on disk and nothing else reads it.
    initializeStagingState();
}

UpdateManager::~UpdateManager()
{
    releaseLock();
}

bool UpdateManager::updateAvailableWarning() const
{
    // The existing updateAvailable fact, minus the versions the user has
    // already answered for. ReadyToInstall still counts — verified bytes
    // waiting to be installed are still something being asked of them.
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
    // Deliberately version-only: "Lightning/<version>", byte for byte the
    // same string rust/src/lib.rs USER_AGENT and CppHttpMatrixClient already
    // send. No platform, architecture, build id or locale token is added —
    // an update check must not be more identifying than ordinary traffic.
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
    // The constructor already ran against the real cache location; re-run the
    // startup work so the override is what actually gets read and swept.
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
    // Mirror the real verified-download path exactly: once the bytes are
    // trusted they are promoted to the manifest's filename. With no manifest
    // artifact, or no file on disk, there is nothing to promote and the path
    // is used as given (the install path then fails on its own existence
    // check, which is the behaviour under test elsewhere).
    if (m_artifact && QFileInfo::exists(path)) {
        QString error;
        if (!promoteStagedArtifact(&error)) {
            failWith(error);
            return;
        }
    }
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

    // A new check invalidates the previous download's provenance; leaving it
    // set would attribute the old bytes to whatever this check finds.
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
    // One fallback attempt, and only away from the canonical host. The
    // signature is still verified against the compiled-in key, and the
    // installed-version comparison still refuses anything not strictly
    // newer, so this can only ever restore availability — see
    // UpdateEndpoints.h for why that is the whole of the exposure.
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
                    // Restart the PAIR from the mirror rather than mixing a
                    // canonical signature with a mirrored manifest: the two
                    // must describe the same release, and a signature that
                    // does not match its manifest is a hard failure that
                    // would read as tampering rather than an outage.
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

    // Signature over the raw bytes happens inside parseVerified, before any
    // field of the document is trusted. There is no other way in.
    const UpdateManifest::Result result =
        UpdateManifest::parseVerified(manifestBytes, sigBytes, *m_trust);
    if (!result.ok) {
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
    const Version &remote = m_manifest.version();

    // The stable channel never offers a prerelease.
    if (remote.isPrerelease() && m_manifest.channel() == QLatin1String(kStableChannel)) {
        setStatusDetail(QStringLiteral("A pre-release (%1) is published; stable installations "
                                       "do not offer pre-releases.")
                            .arg(m_manifest.versionString()));
        Q_EMIT updateInfoChanged();
        setState(UpToDate);
        return;
    }

    if (remote <= *installed) {
        if (remote < *installed) {
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

    // Ecosystem-managed installs: only the channel block may say an update
    // is actionable, and only when it is genuinely newer.
    if (isPackageManaged(m_detection.type)) {
        const QString channelId = UpdateManifest::channelIdForInstallType(m_detection.type);
        if (!m_manifest.channelOffersUpdate(channelId, *installed)) {
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
        // No cookie jar contents survive, no disk cache, no redirects
        // followed automatically (each hop is validated by the transfer).
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
    const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return base + QStringLiteral("/updates");
}

QString UpdateManager::statusFilePath() const
{
    // Non-account-scoped and free of anything sensitive by construction: the
    // helper writes four fields, all derived from its own enums.
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
                // "ok" must be a real boolean. A missing or oddly typed field
                // is not read as success, and is not read as failure either —
                // it is simply no result.
                if (okValue.isBool()) {
                    result = okValue.toBool() ? InstallSucceeded : InstallFailed;
                    mode = sanitizedStatusToken(object.value(QLatin1String("mode")));
                    error = sanitizedStatusToken(object.value(QLatin1String("error")));
                }
            }
        }
    }

    // Read once, then gone — including when it was garbage. A file that
    // cannot be parsed must not be re-examined on every subsequent launch,
    // and a real outcome must never be reported twice.
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

    // Only sweep when no other Lightning is using this directory. Another
    // instance may be holding a verified artifact, or part-way through a
    // download, that is older than the staleness cutoff; deleting it would
    // make its install fail. Probe the same lock the download path takes, and
    // simply skip the sweep if it is held — a deferred clean-up costs nothing.
    {
        QLockFile probe(QDir(root).absoluteFilePath(QLatin1String(kLockFileName)));
        probe.setStaleLockTime(60 * 60 * 1000);
        if (probe.tryLock(0)) {
            probe.unlock();
        } else {
            // Defer ONLY to a lock we can attribute to a different live
            // process. An unreadable or leftover lock file must not disable
            // clean-up forever -- and the artifact this process is using is
            // excluded by name below regardless.
            qint64 pid = 0;
            QString host;
            QString application;
            if (probe.getLockInfo(&pid, &host, &application)
                && pid != QCoreApplication::applicationPid()) {
                return;
            }
        }
    }

    // The staging root belongs exclusively to this class: it holds the
    // single-instance lock, the helper's status file, the in-flight ".part"
    // download and the promoted package. Anything else in it is a leftover
    // from a run that verified an artifact and then never installed it —
    // potentially a hundred megabytes sitting in the cache forever.
    //
    // Bounded and non-recursive by construction: entryInfoList lists this ONE
    // directory, directories and symlinks are excluded, the lock and status
    // files are excluded by name, at most kMaxSweepEntries entries are
    // considered, and nothing outside the root is reachable from here.
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

    const QString filename = m_artifact->filename;
    // Re-validate rather than trust the parse: this name is about to become a
    // filesystem path and an argv element handed to a package manager.
    if (!isSafeArtifactFilename(filename)) {
        *error = QStringLiteral("The update information named a file that cannot be used.");
        return false;
    }

    const QDir root(stagingRoot());
    const QString target = root.absoluteFilePath(filename);
    // isSafeArtifactFilename already refuses separators and "..", so this can
    // only fail if something upstream changed; check anyway, because the
    // consequence would be a write outside the staging root.
    if (QDir::cleanPath(QFileInfo(target).absolutePath())
        != QDir::cleanPath(root.absolutePath())) {
        *error = QStringLiteral("The update information named a file that cannot be used.");
        return false;
    }
    if (target == m_stagedPath)
        return true;

    // The QTemporaryFile object must let go before the rename; it was created
    // with setAutoRemove(false), so releasing it deletes nothing.
    if (m_stagedFile) {
        m_stagedFile->close();
        m_stagedFile.reset();
    }

    const QFileInfo existing(target);
    if (existing.exists() || existing.isSymLink()) {
        // A leftover from an earlier run inside our own staging directory.
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

    // Wired ONCE per download rather than once per attempt: the canonical
    // fallback restarts this same downloader, and re-connecting it from
    // inside its own finished() emission is a subtlety worth not having.
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

    // MIRROR FIRST when the signed manifest names one. The mirror is only a
    // faster place to get bytes the manifest has already named, sized and
    // hashed: it decides nothing, and it is verified against exactly the same
    // sha256 as the canonical address.
    beginDownloadAttempt(m_artifact->mirrorUrl.isEmpty() ? ArtifactSource::Canonical
                                                         : ArtifactSource::Mirror);
}

void UpdateManager::beginDownloadAttempt(ArtifactSource source)
{
    if (!m_artifact || !m_downloader)
        return;

    m_attemptSource = source;
    m_fallbackPending = false;

    // Nothing from a previous attempt survives into this one. The partial
    // file is removed BEFORE a byte of the next source is written, so a
    // failed mirror attempt leaves nothing behind and certainly cannot
    // contribute bytes to the file that eventually gets installed.
    discardStagedArtifact();

    auto file = std::make_unique<QTemporaryFile>(
        QDir(stagingRoot()).absoluteFilePath(QLatin1String(kPartialArtifactTemplate)));
    // QTemporaryFile creates with 0600 and an unpredictable name; the file
    // must survive this scope, so auto-removal is disabled explicitly and
    // every failure path removes it by hand.
    file->setAutoRemove(false);
    if (!file->open()) {
        releaseLock();
        failWith(QStringLiteral("A temporary file for the download could not be created."));
        return;
    }
    file->setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    m_stagedPath = file->fileName();
    m_stagedFile = std::move(file);

    // Progress restarts from zero. A fallback must not double-count what the
    // failed attempt already reported.
    m_downloadedBytes = 0;
    m_totalBytes = m_artifact->size;
    Q_EMIT downloadProgressChanged();

    const QUrl url =
        source == ArtifactSource::Mirror ? m_artifact->mirrorUrl : m_artifact->url;

    setState(Downloading);
    if (m_artifactByteSource) {
        // Test seam. Same downloader, same file, same size and SHA-256
        // enforcement — only the transport is absent.
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
        // Every failure — including a hash mismatch — deletes the bytes.
        // There is no "install anyway" path, and there is no path that
        // installs bytes one source produced and another verified.
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
            // ONE retry, at the canonical address, verified against the same
            // sha256 — a mirror failure never relaxes anything. There is no
            // third attempt and the mirror is never retried afterwards.
            //
            // Queued, not immediate: restarting the downloader from inside
            // its own finished() emission would re-enter a frame that is
            // still unwinding. The lock is deliberately KEPT across the gap.
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
    // The downloader verified size and SHA-256 while streaming; this
    // re-checks what actually landed on disk.
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
    // Only NOW, with the bytes verified, does the file take the manifest's
    // own name. It is downloaded under an unpredictable "*.part" name, and it
    // is renamed inside the same staging directory, so nothing is exposed by
    // the predictable name that was not already there — but the extension is
    // what makes apt-get/dnf/msiexec treat the argument as a local package at
    // all.
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
    // A mirror that keeps failing should be visible rather than silent, so
    // the fallback is disclosed even though the update itself succeeded.
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
        // Cancelled in the window between a failed mirror attempt and the
        // queued canonical retry. There is no live transfer to cancel here,
        // so this path has to do it, or the fallback would start anyway.
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

QString UpdateManager::installTargetPath() const
{
    switch (m_detection.type) {
    case InstallType::LinuxAppImage: {
        // The AppImage strategy replaces the running .AppImage FILE.
        const QString appImage = qEnvironmentVariable("APPIMAGE");
        if (!appImage.isEmpty())
            return appImage;
        break;
    }
    case InstallType::WindowsPortable:
        // The portable strategy swaps the whole install DIRECTORY
        // (swapDirectory needs the directory, and UpdaterArgs refuses a
        // non-directory --target for this mode). Handing it the executable
        // path killed every portable update at argument parsing.
        return QCoreApplication::applicationDirPath();
    default:
        break;
    }
    return QCoreApplication::applicationFilePath();
}

QString UpdateManager::relaunchProgramPath() const
{
    // Inside an AppImage, applicationFilePath() is the executable in the
    // MOUNTED squashfs (/tmp/.mount_XXXX/usr/bin/...), not the .AppImage
    // file. Relaunching that starts the image that is being replaced: the
    // maintainer's first AppImage upgrade came back up reporting the OLD
    // version, and only showed the new one after being closed and started
    // again by hand. The file we just replaced is the one to run.
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

    // Policy refusal is terminal and has no override anywhere in this API.
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

    const QString program = helperProgramPath();
    const QFileInfo helper(program);
    if (!helper.exists() || !helper.isFile()) {
        failWith(QStringLiteral("The updater helper is missing from this installation."));
        return;
    }

    QStringList arguments{
        QStringLiteral("--mode"),     installTypeString(),
        QStringLiteral("--artifact"), QFileInfo(m_stagedPath).absoluteFilePath(),
        QStringLiteral("--pid"),      QString::number(QCoreApplication::applicationPid()),
        QStringLiteral("--target"),   installTargetPath(),
        QStringLiteral("--status"),   statusFilePath(),
    };
    // --relaunch is the helper's entire relaunch policy and it is passed ONLY
    // here. installUpdate() means "apply this when I quit"; starting the
    // application back up afterwards would be the opposite of what the user
    // asked for.
    if (restartAfterwards) {
        arguments << QStringLiteral("--relaunch") << relaunchProgramPath();
    }

    setState(Installing);
    m_lastLaunchProgram = program;
    m_lastLaunchArguments = arguments;

    // WHEN the helper is started is not a detail: once started it waits for
    // this process to exit for at most kDefaultWaitTimeoutMs (two minutes,
    // src/updater/ProcessWaiter.h) and then gives up. Starting it now for the
    // "apply when I quit" path would promise something it cannot keep -- a
    // user who keeps chatting for twenty minutes would find the helper had
    // timed out long ago, with the UI still saying the update was queued.
    //
    // So: "Install and restart" launches now, because the quit follows
    // immediately. "Install without restarting" defers the launch to
    // aboutToQuit, which is exactly when the wait can succeed.
    if (restartAfterwards) {
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

    // The helper has been LAUNCHED (or armed for quit), not completed. It has
    // installed nothing yet. Saying otherwise here is what M1 was: the state
    // claimed an installation that had not happened, and no code read the
    // helper's status file, so every post-handoff failure was invisible. The
    // real outcome arrives on the next launch, via lastUpdateResult.
    m_handoffSummary = restartAfterwards
        ? QStringLiteral("Lightning will close now so the update can be applied, then "
                         "start again. Nothing has been installed yet.")
        : QStringLiteral("The update will be applied when you quit Lightning, however "
                         "long that is. Nothing has been installed yet.");
    setStatusDetail(m_handoffSummary);
    setState(RestartRequired);
    if (restartAfterwards)
        Q_EMIT quitRequested();
}

// Runs from QCoreApplication::aboutToQuit for the "apply when I quit" path.
// Starting the helper here, rather than at the click, is what makes the
// promise in m_handoffSummary true: its bounded wait for this PID begins as
// the process is actually going away.
void UpdateManager::launchDeferredInstall()
{
    if (!m_deferredInstallPending)
        return;
    m_deferredInstallPending = false;
    if (m_lastLaunchProgram.isEmpty() || !m_launcher)
        return;
    // Nothing useful can be reported from here -- the UI is gone and the
    // event loop is ending. A failure to start leaves the verified artifact
    // in place and the status file absent, so the next launch simply shows no
    // result rather than a false success.
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
