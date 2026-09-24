#pragma once

#include "update/InstallType.h"
#include "update/UpdateDownloader.h" // TransferError, used by the download handler
#include "update/UpdateManifest.h"
#include "update/UpdateTrustStore.h"
#include "update/Version.h"

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QtQmlIntegration/qqmlintegration.h>

#include <functional>
#include <memory>

class QFile;
class QLockFile;
class QNetworkAccessManager;

namespace lightning::update {

class UpdateDocumentFetcher;
class UpdateDownloader;

// Application-level state machine for secure updates.
//
// No Matrix dependencies: settings live in the non-account-scoped "update/"
// group, so sign-in, sign-out and account switches cannot affect it.
//
// Trust chain: compiled-in public key -> signed manifest -> SHA-256 of the
// artifact -> verified bytes -> compiled-in platform strategy. Every step is
// terminal on failure, there is no "install anyway" entry point, and the
// manifest never supplies a command: the helper gets a fixed program and an
// argument vector, never a shell.
class UpdateManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("UpdateManager is exposed by the application")
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString currentVersion READ currentVersion NOTIFY currentVersionChanged)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY updateInfoChanged)
    // Persistent: an update was found. Dismissing the corner card silences only
    // updateAvailableWarning; the rail badge stays.
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY updateInfoChanged)
    Q_PROPERTY(qreal downloadProgress READ downloadProgress NOTIFY downloadProgressChanged)
    Q_PROPERTY(qint64 downloadedBytes READ downloadedBytes NOTIFY downloadProgressChanged)
    Q_PROPERTY(qint64 totalBytes READ totalBytes NOTIFY downloadProgressChanged)
    Q_PROPERTY(QString releaseNotes READ releaseNotes NOTIFY updateInfoChanged)
    Q_PROPERTY(QUrl releaseNotesUrl READ releaseNotesUrl NOTIFY updateInfoChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    // Non-error diagnostic: "not downgrading", "prerelease ignored",
    // "your package manager has not published this version yet".
    Q_PROPERTY(QString statusDetail READ statusDetail NOTIFY statusDetailChanged)
    // Where the staged bytes came from: "mirror", "canonical", or empty. A
    // role, never a host or URL.
    Q_PROPERTY(QString artifactSource READ artifactSource NOTIFY artifactSourceChanged)
    Q_PROPERTY(QString installType READ installTypeString NOTIFY installTypeChanged)
    Q_PROPERTY(QString installTypeLabel READ installTypeLabel NOTIFY installTypeChanged)
    Q_PROPERTY(bool canInstallAutomatically READ canInstallAutomatically NOTIFY installTypeChanged)
    Q_PROPERTY(bool packageManaged READ packageManaged NOTIFY installTypeChanged)
    // A Windows MSI/setup installation "for all users": installing needs
    // administrator approval, so the UI can say so beforehand.
    Q_PROPERTY(bool installNeedsAdministrator READ installNeedsAdministrator
                   NOTIFY installTypeChanged)
    Q_PROPERTY(bool automaticChecksEnabled READ automaticChecksEnabled WRITE
                   setAutomaticChecksEnabled NOTIFY automaticChecksEnabledChanged)
    Q_PROPERTY(QDateTime lastCheckTime READ lastCheckTime NOTIFY lastCheckTimeChanged)
    Q_PROPERTY(QString dismissedVersion READ dismissedVersion NOTIFY dismissedVersionChanged)
    // An update is waiting and this version was not dismissed. Drives the rail
    // badge and corner prompt; dismissal is per version.
    Q_PROPERTY(bool updateAvailableWarning READ updateAvailableWarning
                   NOTIFY updateAvailableWarningChanged)

    // Shown once the helper is launched; never claims success.
    Q_PROPERTY(QString handoffSummary READ handoffSummary NOTIFY stateChanged)
    // The previous run's install outcome from the helper's status file,
    // consumed once.
    Q_PROPERTY(LastResult lastUpdateResult READ lastUpdateResult NOTIFY lastUpdateResultChanged)
    Q_PROPERTY(QString lastUpdateError READ lastUpdateError NOTIFY lastUpdateResultChanged)
    Q_PROPERTY(QString lastUpdateMode READ lastUpdateMode NOTIFY lastUpdateResultChanged)

public:
    enum State {
        Idle,
        Checking,
        UpToDate,
        UpdateAvailable,
        Downloading,
        Verifying,
        ReadyToInstall,
        Installing,
        // The verified artifact was handed to the helper, which installs only
        // after this process exits. Nothing is applied yet; the outcome is
        // known on the next launch via lastUpdateResult. Show handoffSummary,
        // never "installed".
        RestartRequired,
        Failed,
    };
    Q_ENUM(State)

    // Outcome of the previous run's handoff. NoResult means no status file or
    // an unreadable one, never a silent success.
    enum LastResult {
        NoResult,
        InstallSucceeded,
        InstallFailed,
    };
    Q_ENUM(LastResult)

    // Program plus argument vector; never a command string or a shell.
    using ProcessLauncher = std::function<bool(const QString &program, const QStringList &args)>;

    explicit UpdateManager(QObject *parent = nullptr);
    ~UpdateManager() override;

    State state() const { return m_state; }
    QString currentVersion() const { return m_currentVersion; }
    QString latestVersion() const { return m_latestVersion; }
    bool updateAvailable() const { return m_updateAvailable; }
    qreal downloadProgress() const;
    qint64 downloadedBytes() const { return m_downloadedBytes; }
    qint64 totalBytes() const { return m_totalBytes; }
    QString releaseNotes() const { return m_releaseNotes; }
    QUrl releaseNotesUrl() const { return m_releaseNotesUrl; }
    QString errorMessage() const { return m_errorMessage; }
    QString statusDetail() const { return m_statusDetail; }
    QString artifactSource() const { return m_artifactSource; }
    QString installTypeString() const { return installTypeId(m_detection.type); }
    QString installTypeLabel() const;
    bool canInstallAutomatically() const { return m_detection.automaticInstallAllowed; }
    bool packageManaged() const { return isPackageManaged(m_detection.type); }
    bool installNeedsAdministrator() const;
    bool automaticChecksEnabled() const { return m_automaticChecksEnabled; }
    void setAutomaticChecksEnabled(bool enabled);
    QDateTime lastCheckTime() const { return m_lastCheckTime; }
    QString dismissedVersion() const { return m_dismissedVersion; }
    bool updateAvailableWarning() const;
    QString handoffSummary() const { return m_handoffSummary; }
    LastResult lastUpdateResult() const { return m_lastUpdateResult; }
    QString lastUpdateError() const { return m_lastUpdateError; }
    QString lastUpdateMode() const { return m_lastUpdateMode; }

    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void downloadUpdate();
    Q_INVOKABLE void cancelDownload();
    Q_INVOKABLE void installUpdate();
    Q_INVOKABLE void installAndRestart();
    Q_INVOKABLE void dismissVersion();
    // Acknowledges the previous outcome. The status file was deleted when read;
    // this clears the in-memory copy.
    Q_INVOKABLE void clearLastUpdateResult();
    // Emits managedUpdateHelpRequested; opening anything is the UI's job.
    Q_INVOKABLE void openManagedUpdateHelp();

    // The command a Flatpak/Snap user runs themselves. Empty otherwise.
    Q_INVOKABLE QString managedUpdateCommand() const;

    // The only identifying string an update request carries: exactly
    // "Lightning/<version>", matching the Matrix clients. Exposed for tests.
    Q_INVOKABLE QString userAgentString() const;

    // At most once per 24 h and never in the first 30 s. Returns true when a
    // check started.
    Q_INVOKABLE bool maybeCheckAutomatically();

    static constexpr qint64 kAutomaticCheckIntervalMs = qint64(24) * 60 * 60 * 1000;
    static constexpr qint64 kStartupQuietPeriodMs = 30 * 1000;
    // Anything larger is not the four-field file the helper writes.
    static constexpr qint64 kMaxStatusBytes = 8 * 1024;
    // A staged artifact this old was verified but never installed.
    static constexpr qint64 kStaleArtifactAgeMs = qint64(6) * 60 * 60 * 1000;
    // Bounded sweep, inside the staging root only.
    static constexpr int kMaxSweepEntries = 256;

    // --- test seams -----------------------------------------------------
    // Seams are additive and never relax verification: signature and hash
    // checks always run.
    void setTrustStoreForTest(const TrustStore *trust);
    void setInstallDetectionForTest(const InstallDetection &detection);
    void setCurrentVersionForTest(const QString &version);
    // Also re-runs the constructor's status-file read and stale-artifact sweep
    // against the new root.
    void setStagingRootForTest(const QString &path);
    void setProcessLauncherForTest(ProcessLauncher launcher);
    void setHelperPathForTest(const QString &path);
    void setNowForTest(const QDateTime &now);
    void setProcessStartForTest(const QDateTime &started);
    // Stops a check before any network access; cannot make an unverified update
    // succeed.
    void setNetworkDisabledForTest(bool disabled);
    // The two documents a check would fetch; tests and the real path share it.
    void ingestCheckDocuments(const QByteArray &manifestBytes, const QByteArray &sigBytes);
    // Artifact bytes per URL, so mirror-first ordering and the canonical
    // fallback are testable offline; std::nullopt means unreachable. Relaxes
    // nothing: the bytes go through the real downloader, host policy, size and
    // SHA-256 checks.
    using ArtifactByteSource = std::function<std::optional<QByteArray>(const QUrl &)>;
    void setArtifactByteSourceForTest(ArtifactByteSource source);
    // Pretends a verified artifact is staged at `path` (ReadyToInstall), using
    // the same promotion to the manifest's filename as a real download.
    void setStagedArtifactForTest(const QString &path);
    QStringList lastLaunchArgumentsForTest() const { return m_lastLaunchArguments; }
    QString lastLaunchProgramForTest() const { return m_lastLaunchProgram; }
    QString stagedArtifactPathForTest() const { return m_stagedPath; }
    // The digest the staged bytes were verified against: the manifest's value,
    // or without a manifest the file's own digest at staging time.
    QString stagedArtifactSha256ForTest() const { return m_stagedSha256; }
    // Whether this check fell back to the mirror's metadata pair.
    bool metadataFromMirrorForTest() const { return m_metadataFromMirror; }
    QString stagingRootForTest() const { return stagingRoot(); }

Q_SIGNALS:
    void stateChanged();
    void currentVersionChanged();
    void updateInfoChanged();
    void downloadProgressChanged();
    void errorMessageChanged();
    void statusDetailChanged();
    void artifactSourceChanged();
    void installTypeChanged();
    void automaticChecksEnabledChanged();
    void lastCheckTimeChanged();
    void dismissedVersionChanged();
    void updateAvailableWarningChanged();
    void lastUpdateResultChanged();
    // Refused by policy, with a user-facing reason. No override exists.
    void installRefused(const QString &reason);
    void managedUpdateHelpRequested(const QString &command, const QString &explanation);
    // Asks the application to quit so the helper (waiting on this PID)
    // proceeds.
    void quitRequested();

private:
    // Which address of the one artifact is fetched; both verify against the
    // same sha256.
    enum class ArtifactSource {
        Canonical,
        Mirror,
    };

    void setState(State state);
    void setErrorMessage(const QString &message);
    void setStatusDetail(const QString &detail);
    void setArtifactSource(const QString &source);
    void failWith(const QString &message);
    bool isBusy() const;

    // One attempt: fresh staging file, progress reset, request to that source.
    void beginDownloadAttempt(ArtifactSource source);
    void handleDownloadFinished(bool ok, TransferError error, const QString &message);

    void startCheck(bool automatic);
    void fetchSignature();
    void fetchManifest();
    void applyCheckDocuments(const QByteArray &manifestBytes, const QByteArray &sigBytes);
    // Decides UpToDate, UpdateAvailable or Failed for the verified manifest.
    void decideFromManifest(const Version &installed);

    void startInstall(bool restartAfterwards);
    // Starts the helper from aboutToQuit ("apply when I quit").
    void launchDeferredInstall();
    QString helperProgramPath() const;
    // Windows MSI/setup only: copies the helper outside the installation the
    // installer is about to rewrite. Empty and `error` set on failure. Public
    // for tests.
public:
    static QString stageHelperOutsideInstallation(const QString &helperPath,
                                                  const QString &installDir,
                                                  QString *error);
    // Non-system libraries the helper loads. The Windows artifact validation
    // asserts the shipped binary imports nothing else.
    static QStringList helperRuntimeLibraries();
    // Versioned stems: every matching DLL beside the helper is copied too.
    static QStringList helperRuntimeLibraryStems();

    // An actionable sentence for a helper failure token ("refused-unsafe-path",
    // "installer-exit-1603", ...), or empty so the caller keeps its generic
    // text.
    Q_INVOKABLE static QString explainInstallError(const QString &token);

private:
    QString installTargetPath() const;
    // What to start after installing; for an AppImage, not the running binary.
    QString relaunchProgramPath() const;
    // One mirror fallback for the manifest pair. True when a retry started.
    bool retryMetadataFromMirror();
    QString stagingRoot() const;
    QString statusFilePath() const;
    // Reads and deletes the helper's status file, then sweeps stale artifacts.
    void initializeStagingState();
    void consumeUpdateStatusFile();
    void sweepStaleStagedArtifacts();
    // Renames the verified temp file to the manifest's filename. Package
    // managers key on the extension (apt-get needs '/' and ".deb", dnf ".rpm",
    // msiexec ".msi"); a "*.part" name fails after the PolicyKit prompt was
    // answered.
    bool promoteStagedArtifact(QString *error);
    // Re-hashes the staged file: proves the path still holds the verified bytes
    // at hand-over.
    bool stagedArtifactStillVerifies() const;
    // Writes the status file for the one failure the helper cannot report: the
    // install-on-quit re-hash failing before it starts.
    void writeLocalStatusFailure(const QString &error);
    bool acquireLock();
    void releaseLock();
    void discardStagedArtifact();
    QNetworkAccessManager *network();
    QByteArray userAgent() const;
    QDateTime now() const;

    State m_state = Idle;
    QString m_currentVersion;
    QString m_latestVersion;
    bool m_updateAvailable = false;
    qint64 m_downloadedBytes = 0;
    qint64 m_totalBytes = 0;
    QString m_releaseNotes;
    QUrl m_releaseNotesUrl;
    QString m_errorMessage;
    QString m_statusDetail;
    QString m_artifactSource;
    QString m_handoffSummary;
    bool m_automaticChecksEnabled = false;
    QDateTime m_lastCheckTime;
    QString m_dismissedVersion;
    LastResult m_lastUpdateResult = NoResult;
    QString m_lastUpdateError;
    QString m_lastUpdateMode;

    InstallDetection m_detection;
    CompiledTrustStore m_compiledTrust;
    const TrustStore *m_trust = nullptr;
    UpdateManifest m_manifest;
    std::optional<ManifestArtifact> m_artifact;

    QSettings m_settings;
    QPointer<QNetworkAccessManager> m_network;
    UpdateDocumentFetcher *m_fetcher = nullptr;
    UpdateDownloader *m_downloader = nullptr;
    QByteArray m_signatureDocument;
    // Reading the mirrored manifest pair; reset per check.
    bool m_metadataFromMirror = false;
    std::unique_ptr<QFile> m_stagedFile;
    QString m_stagedPath;
    // Lowercase hex SHA-256 the staged file must match; set and cleared with
    // the path.
    QString m_stagedSha256;
    std::unique_ptr<QLockFile> m_lock;

    // The mirror and the canonical address are each tried at most once.
    ArtifactSource m_attemptSource = ArtifactSource::Canonical;
    bool m_mirrorFallbackUsed = false;
    // A queued fallback that has not started; cancel must still work here.
    bool m_fallbackPending = false;
    ArtifactByteSource m_artifactByteSource;

    QString m_stagingRootOverride;
    QString m_helperPathOverride;
    ProcessLauncher m_launcher;
    QStringList m_lastLaunchArguments;
    // The helper's wait is bounded, so the no-restart path launches it only
    // when the application is quitting.
    bool m_deferredInstallPending = false;
    bool m_deferredInstallConnected = false;
    QString m_lastLaunchProgram;
    QDateTime m_nowOverride;
    QDateTime m_processStart;
    bool m_networkDisabledForTest = false;
};

} // namespace lightning::update
