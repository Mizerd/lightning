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

// Lightning secure update system — the application-level state machine.
//
// Zero Matrix dependencies, by construction: this class knows nothing about
// accounts, homeservers, tokens, rooms or the SDK, and its settings live in
// the NON-account-scoped QSettings group "update/". Signing in, signing out
// and switching accounts cannot alter update state, because there is no
// path from any of them to this object.
//
// The trust chain is: compiled-in public key -> signed manifest -> SHA-256
// of the exact artifact -> verified bytes -> a compiled-in platform
// strategy. Every step is terminal on failure; there is no "install anyway"
// entry point anywhere in this API, and the manifest can never supply a
// command — the helper is launched with a fixed program plus an argument
// VECTOR, never a command string and never a shell.
class UpdateManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("UpdateManager is exposed by the application")
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString currentVersion READ currentVersion NOTIFY currentVersionChanged)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY updateInfoChanged)
    // The quiet, PERSISTENT fact: an update was found. Dismissing the corner
    // card silences the CARD (updateAvailableWarning), never this — the rail
    // badge is what is left saying an update is still waiting.
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
    // Where the bytes now on disk came from: "mirror", "canonical", or empty
    // when nothing is staged. A ROLE, never a host or a URL — it is a
    // diagnostic, not an address book, and a persistently useless mirror is
    // meant to be visible here rather than silent.
    Q_PROPERTY(QString artifactSource READ artifactSource NOTIFY artifactSourceChanged)
    Q_PROPERTY(QString installType READ installTypeString NOTIFY installTypeChanged)
    Q_PROPERTY(QString installTypeLabel READ installTypeLabel NOTIFY installTypeChanged)
    Q_PROPERTY(bool canInstallAutomatically READ canInstallAutomatically NOTIFY installTypeChanged)
    Q_PROPERTY(bool packageManaged READ packageManaged NOTIFY installTypeChanged)
    Q_PROPERTY(bool automaticChecksEnabled READ automaticChecksEnabled WRITE
                   setAutomaticChecksEnabled NOTIFY automaticChecksEnabledChanged)
    Q_PROPERTY(QDateTime lastCheckTime READ lastCheckTime NOTIFY lastCheckTimeChanged)
    Q_PROPERTY(QString dismissedVersion READ dismissedVersion NOTIFY dismissedVersionChanged)
    // v0.7.3: an update is waiting for the user AND they have not dismissed
    // THIS version. Drives the rail badge and the corner prompt, exactly as
    // sessionVerificationWarning does for verification — dismissal is per
    // version, so a later release asks again while the dismissed one stays
    // quiet.
    Q_PROPERTY(bool updateAvailableWarning READ updateAvailableWarning
                   NOTIFY updateAvailableWarningChanged)

    // What the user must be told once the helper has been launched. See the
    // RestartRequired note on the State enum: this NEVER claims success.
    Q_PROPERTY(QString handoffSummary READ handoffSummary NOTIFY stateChanged)
    // The previous run's install outcome, recovered from the helper's status
    // file at construction and consumed exactly once.
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
        // The verified artifact has been HANDED OFF to the updater helper,
        // which waits for this process to exit and only then installs
        // anything. It does NOT mean the update was applied: at this point
        // the helper has not run the package manager, extracted an archive,
        // or replaced a file. The outcome is only known on the NEXT launch,
        // through lastUpdateResult. Render handoffSummary here, never a
        // past-tense "installed".
        RestartRequired,
        Failed,
    };
    Q_ENUM(State)

    // Outcome of the PREVIOUS run's handoff, read back from the helper's
    // status file. NoResult means there was no status file, or it was
    // unreadable/garbage — never a silent success.
    enum LastResult {
        NoResult,
        InstallSucceeded,
        InstallFailed,
    };
    Q_ENUM(LastResult)

    // program + argument vector. Never a command string, never a shell.
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
    // Acknowledge the previous run's outcome so it is not shown again. The
    // status FILE is already gone by this point — it is deleted the moment it
    // is read — so this only clears the in-memory copy.
    Q_INVOKABLE void clearLastUpdateResult();
    // Emits managedUpdateHelpRequested with a copyable command and an
    // explanation. It deliberately does NOT open anything itself: opening a
    // URL is the UI layer's job, which keeps this class free of QtGui.
    Q_INVOKABLE void openManagedUpdateHelp();

    // The command a Flatpak/Snap user runs themselves. Empty otherwise.
    Q_INVOKABLE QString managedUpdateCommand() const;

    // The only identifying string any update request carries, and it is
    // exactly "Lightning/<version>" — the same value the Rust SDK and the
    // C++ HTTP client already send, with NO platform, architecture, build or
    // locale token. Exposed so a test can assert that no Matrix data, and
    // nothing else identifying, is attached.
    Q_INVOKABLE QString userAgentString() const;

    // Automatic checking: opt-in, at most once per 24 h, and never in the
    // first 30 s of the process's life. Returns true when a check started.
    Q_INVOKABLE bool maybeCheckAutomatically();

    static constexpr qint64 kAutomaticCheckIntervalMs = qint64(24) * 60 * 60 * 1000;
    static constexpr qint64 kStartupQuietPeriodMs = 30 * 1000;
    // The helper's status file carries four short fields. Anything larger is
    // not the file we wrote and is discarded rather than parsed.
    static constexpr qint64 kMaxStatusBytes = 8 * 1024;
    // A staged artifact this old was verified but never installed; the run
    // that downloaded it is long gone. Six hours is comfortably longer than
    // any plausible download-then-quit gap.
    static constexpr qint64 kStaleArtifactAgeMs = qint64(6) * 60 * 60 * 1000;
    // The sweep is bounded: it inspects at most this many entries and only
    // ever inside the staging root.
    static constexpr int kMaxSweepEntries = 256;

    // --- test seams -----------------------------------------------------
    // Every seam is additive and cannot relax a verification rule: a test
    // may supply its own trust store (with a runtime-generated key), its
    // own install detection, its own clock and its own process launcher,
    // but signature and hash checking still run unmodified.
    void setTrustStoreForTest(const TrustStore *trust);
    void setInstallDetectionForTest(const InstallDetection &detection);
    void setCurrentVersionForTest(const QString &version);
    // Redirects the staging root AND re-runs the two things the constructor
    // does against it: consuming the helper's status file and sweeping stale
    // staged artifacts. Without the re-run a test could only ever exercise
    // the real cache directory.
    void setStagingRootForTest(const QString &path);
    void setProcessLauncherForTest(ProcessLauncher launcher);
    void setHelperPathForTest(const QString &path);
    void setNowForTest(const QDateTime &now);
    void setProcessStartForTest(const QDateTime &started);
    // Stops a check before it touches the network, so state-machine tests
    // never issue a request. It cannot make an unverified update succeed.
    void setNetworkDisabledForTest(bool disabled);
    // Feed the two documents a check would have fetched. Used by tests and
    // by the real fetch path alike, so both take the identical code path.
    void ingestCheckDocuments(const QByteArray &manifestBytes, const QByteArray &sigBytes);
    // Supplies the artifact BYTES a download would have fetched, per URL, so
    // the mirror-first order and its single canonical fallback are testable
    // with no network. std::nullopt means that source was unreachable.
    //
    // It relaxes NOTHING: the bytes are streamed through the real
    // UpdateDownloader into the real staging file, the URL is still checked
    // against the artifact host policy, and the manifest's size and SHA-256
    // are still what decide whether anything reaches ReadyToInstall. A source
    // returning wrong bytes fails here exactly as it would in production.
    using ArtifactByteSource = std::function<std::optional<QByteArray>(const QUrl &)>;
    void setArtifactByteSourceForTest(ArtifactByteSource source);
    // Pretend a verified artifact is staged at `path` (state ReadyToInstall)
    // so install-refusal and argv construction are testable without a
    // network. It takes the SAME promotion step the verified download path
    // takes — the artifact is renamed to the manifest's validated filename —
    // so a test observes the real staged name. It does NOT bypass
    // verification for a real download.
    void setStagedArtifactForTest(const QString &path);
    QStringList lastLaunchArgumentsForTest() const { return m_lastLaunchArguments; }
    QString lastLaunchProgramForTest() const { return m_lastLaunchProgram; }
    QString stagedArtifactPathForTest() const { return m_stagedPath; }
    // The digest the staged bytes were verified against. In production it is
    // the signed manifest's value; setStagedArtifactForTest() without a
    // manifest records the file's own digest at staging time, which is the
    // same promise ("these bytes were checked") for the purposes of the
    // re-check that every launch performs.
    QString stagedArtifactSha256ForTest() const { return m_stagedSha256; }
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
    // The install was refused by policy (managed install, development build,
    // diagnostic override). Carries a user-facing reason; no override exists.
    void installRefused(const QString &reason);
    void managedUpdateHelpRequested(const QString &command, const QString &explanation);
    // installAndRestart() asks the application to quit so the helper — which
    // waits for this PID — can proceed.
    void quitRequested();

private:
    // Which address of the ONE artifact an attempt is fetching. Both are
    // verified against the same manifest sha256; the source only decides
    // where the bytes are asked for.
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

    // One download attempt: a fresh staging file, progress reset to zero, and
    // the request issued at the chosen source's address.
    void beginDownloadAttempt(ArtifactSource source);
    void handleDownloadFinished(bool ok, TransferError error, const QString &message);

    void startCheck(bool automatic);
    void fetchSignature();
    void fetchManifest();
    void applyCheckDocuments(const QByteArray &manifestBytes, const QByteArray &sigBytes);

    void startInstall(bool restartAfterwards);
    // Starts the helper for the "apply when I quit" path, from aboutToQuit.
    void launchDeferredInstall();
    QString helperProgramPath() const;
    QString installTargetPath() const;
    // What to start after a successful install; differs from the running
    // executable for an AppImage. See the definition.
    QString relaunchProgramPath() const;
    // One availability fallback for the manifest pair when the canonical
    // host does not answer. Returns true when a retry was started.
    bool retryMetadataFromMirror();
    QString stagingRoot() const;
    QString statusFilePath() const;
    // Reads and DELETES the helper's status file, then removes stale staged
    // artifacts. Run from the constructor and from setStagingRootForTest().
    void initializeStagingState();
    void consumeUpdateStatusFile();
    void sweepStaleStagedArtifacts();
    // Renames the verified temp file to the manifest's validated filename.
    // The package managers key on the EXTENSION (apt-get only treats an
    // argument as a local package when it contains '/' and ends in ".deb";
    // dnf wants ".rpm"; msiexec /i wants ".msi"), so a "*.part" name reaches
    // them as an unknown package NAME and the install fails after the user
    // has already answered a PolicyKit prompt.
    bool promoteStagedArtifact(QString *error);
    // Re-hashes the staged file and compares it with m_stagedSha256. The
    // download verified the bytes as they streamed in; this proves the file
    // at the path is STILL those bytes at the moment they are handed over.
    bool stagedArtifactStillVerifies() const;
    // Writes the helper's status document ourselves, for the one failure the
    // helper cannot report because it was never started: the pre-launch
    // re-hash failing on the install-on-quit path, where the UI is gone.
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
    // True while this check is reading the MIRRORED manifest pair because
    // the canonical host failed. Reset at the start of every check.
    bool m_metadataFromMirror = false;
    std::unique_ptr<QFile> m_stagedFile;
    QString m_stagedPath;
    // Lowercase hex SHA-256 the staged file must still hash to. Set with the
    // path, cleared with it.
    QString m_stagedSha256;
    std::unique_ptr<QLockFile> m_lock;

    // Mirror-first download state. The mirror is attempted at most once and
    // the canonical address at most once; there is no third attempt and no
    // second mirror try after the canonical address fails.
    ArtifactSource m_attemptSource = ArtifactSource::Canonical;
    bool m_mirrorFallbackUsed = false;
    // A fallback is queued but has not started yet. Cancelling in that window
    // must still cancel, so it is a state of its own rather than a gap.
    bool m_fallbackPending = false;
    ArtifactByteSource m_artifactByteSource;

    QString m_stagingRootOverride;
    QString m_helperPathOverride;
    ProcessLauncher m_launcher;
    QStringList m_lastLaunchArguments;
    // The helper's wait for our PID is bounded, so for the no-restart path we
    // hold the launch until the application is actually quitting.
    bool m_deferredInstallPending = false;
    bool m_deferredInstallConnected = false;
    QString m_lastLaunchProgram;
    QDateTime m_nowOverride;
    QDateTime m_processStart;
    bool m_networkDisabledForTest = false;
};

} // namespace lightning::update
