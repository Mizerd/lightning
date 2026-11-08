#pragma once

#include "update/InstallType.h"
#include "update/UpdateDownloader.h"
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

class UpdateManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("UpdateManager is exposed by the application")
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString currentVersion READ currentVersion NOTIFY currentVersionChanged)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY updateInfoChanged)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY updateInfoChanged)
    Q_PROPERTY(qreal downloadProgress READ downloadProgress NOTIFY downloadProgressChanged)
    Q_PROPERTY(qint64 downloadedBytes READ downloadedBytes NOTIFY downloadProgressChanged)
    Q_PROPERTY(qint64 totalBytes READ totalBytes NOTIFY downloadProgressChanged)
    Q_PROPERTY(QString releaseNotes READ releaseNotes NOTIFY updateInfoChanged)
    Q_PROPERTY(QUrl releaseNotesUrl READ releaseNotesUrl NOTIFY updateInfoChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString statusDetail READ statusDetail NOTIFY statusDetailChanged)
    Q_PROPERTY(QString artifactSource READ artifactSource NOTIFY artifactSourceChanged)
    Q_PROPERTY(QString installType READ installTypeString NOTIFY installTypeChanged)
    Q_PROPERTY(QString installTypeLabel READ installTypeLabel NOTIFY installTypeChanged)
    Q_PROPERTY(bool canInstallAutomatically READ canInstallAutomatically NOTIFY installTypeChanged)
    Q_PROPERTY(bool packageManaged READ packageManaged NOTIFY installTypeChanged)
    Q_PROPERTY(QString packageManagerName READ packageManagerName NOTIFY installTypeChanged)
    Q_PROPERTY(bool installNeedsAdministrator READ installNeedsAdministrator
                   NOTIFY installTypeChanged)
    Q_PROPERTY(bool automaticChecksEnabled READ automaticChecksEnabled WRITE
                   setAutomaticChecksEnabled NOTIFY automaticChecksEnabledChanged)
    Q_PROPERTY(QDateTime lastCheckTime READ lastCheckTime NOTIFY lastCheckTimeChanged)
    Q_PROPERTY(QString dismissedVersion READ dismissedVersion NOTIFY dismissedVersionChanged)
    Q_PROPERTY(bool updateAvailableWarning READ updateAvailableWarning
                   NOTIFY updateAvailableWarningChanged)

    Q_PROPERTY(QString handoffSummary READ handoffSummary NOTIFY stateChanged)
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
        RestartRequired,
        Failed,
    };
    Q_ENUM(State)

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
    QString packageManagerName() const
    {
        return lightning::update::packageManagerName(m_detection.type);
    }
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
    Q_INVOKABLE void clearLastUpdateResult();
    Q_INVOKABLE void openManagedUpdateHelp();

    Q_INVOKABLE QString managedUpdateCommand() const;

    Q_INVOKABLE QString userAgentString() const;

    Q_INVOKABLE bool maybeCheckAutomatically();

    static constexpr qint64 kAutomaticCheckIntervalMs = qint64(24) * 60 * 60 * 1000;
    static constexpr qint64 kStartupQuietPeriodMs = 30 * 1000;
    static constexpr qint64 kMaxStatusBytes = 8 * 1024;
    static constexpr qint64 kStaleArtifactAgeMs = qint64(6) * 60 * 60 * 1000;
    static constexpr int kMaxSweepEntries = 256;

    void setTrustStoreForTest(const TrustStore *trust);
    void setInstallDetectionForTest(const InstallDetection &detection);
    void setCurrentVersionForTest(const QString &version);
    void setStagingRootForTest(const QString &path);
    void setProcessLauncherForTest(ProcessLauncher launcher);
    void setHelperPathForTest(const QString &path);
    void setNowForTest(const QDateTime &now);
    void setProcessStartForTest(const QDateTime &started);
    void setNetworkDisabledForTest(bool disabled);
    void ingestCheckDocuments(const QByteArray &manifestBytes, const QByteArray &sigBytes);
    using ArtifactByteSource = std::function<std::optional<QByteArray>(const QUrl &)>;
    void setArtifactByteSourceForTest(ArtifactByteSource source);
    void setStagedArtifactForTest(const QString &path);
    QStringList lastLaunchArgumentsForTest() const { return m_lastLaunchArguments; }
    QString lastLaunchProgramForTest() const { return m_lastLaunchProgram; }
    QString stagedArtifactPathForTest() const { return m_stagedPath; }
    QString stagedArtifactSha256ForTest() const { return m_stagedSha256; }
    bool metadataInFallbackStageForTest() const { return m_metadataStage > 0; }
    int metadataStageForTest() const { return m_metadataStage; }
    QUrl lastSignatureUrlForTest() const { return m_lastSignatureUrl; }
    void simulateMetadataFetchFailureForTest(const QString &message,
                                             TransferError error = TransferError::Network)
    {
        onMetadataFetchFailed(error, message);
    }
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
    void installRefused(const QString &reason);
    void managedUpdateHelpRequested(const QString &command, const QString &explanation);
    void quitRequested();

private:
    enum class ArtifactSource {
        Canonical,
        Mirror,
        GitlabCom,
    };

    void setState(State state);
    void setErrorMessage(const QString &message);
    void setStatusDetail(const QString &detail);
    void setArtifactSource(const QString &source);
    void failWith(const QString &message);
    bool isBusy() const;

    bool hasGitlabComArtifact() const;
    void beginDownloadAttempt(ArtifactSource source);
    void handleDownloadFinished(bool ok, TransferError error, const QString &message);

    void startCheck(bool automatic);
    void fetchSignature();
    void fetchManifest();
    void applyCheckDocuments(const QByteArray &manifestBytes, const QByteArray &sigBytes);
    void decideFromManifest(const Version &installed);

    void startInstall(bool restartAfterwards);
    void launchDeferredInstall();
    QString helperProgramPath() const;
public:
    static QString stageHelperOutsideInstallation(const QString &helperPath,
                                                  const QString &installDir,
                                                  QString *error);
    // Non-system libraries the helper loads. The Windows artifact validation
    // asserts the shipped binary imports nothing else.
    static QStringList helperRuntimeLibraries();
    static QStringList helperRuntimeLibraryStems();

    Q_INVOKABLE static QString explainInstallError(const QString &token);

private:
    QString installTargetPath() const;
    QString relaunchProgramPath() const;
    bool retryMetadataFromFallback();
    void onMetadataFetchFailed(TransferError error, const QString &message);
    QString stagingRoot() const;
    QString statusFilePath() const;
    void initializeStagingState();
    void consumeUpdateStatusFile();
    void sweepStaleStagedArtifacts();
    // Renames the verified temp file to the manifest's filename. Package
    // managers key on the extension (apt-get needs '/' and ".deb", dnf ".rpm",
    // msiexec ".msi"); a "*.part" name fails after the PolicyKit prompt was
    // answered.
    bool promoteStagedArtifact(QString *error);
    bool stagedArtifactStillVerifies() const;
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
    int m_metadataStage = 0;
    QUrl m_lastSignatureUrl;
    std::unique_ptr<QFile> m_stagedFile;
    QString m_stagedPath;
    QString m_stagedSha256;
    std::unique_ptr<QLockFile> m_lock;

    ArtifactSource m_attemptSource = ArtifactSource::Canonical;
    bool m_githubFailed = false;
    bool m_gitlabComFailed = false;
    bool m_fallbackPending = false;
    ArtifactByteSource m_artifactByteSource;

    QString m_stagingRootOverride;
    QString m_helperPathOverride;
    ProcessLauncher m_launcher;
    QStringList m_lastLaunchArguments;
    bool m_deferredInstallPending = false;
    bool m_deferredInstallConnected = false;
    QString m_lastLaunchProgram;
    QDateTime m_nowOverride;
    QDateTime m_processStart;
    bool m_networkDisabledForTest = false;
};

}
