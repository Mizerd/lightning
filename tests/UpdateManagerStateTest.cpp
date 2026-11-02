
#include "update/InstallType.h"
#include "update/UpdateDownloader.h"
#include "update/UpdateEndpoints.h"
#include "update/UpdateManager.h"
#include "updater/UpdaterArgs.h"
#include "update/UpdateManifest.h"
#include "update/UpdateTrustStore.h"
#include "updater/ArtifactDigest.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLockFile>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest/QtTest>

#include <openssl/evp.h>

#include <memory>
#include <optional>

using lightning::update::InstallDetection;
using lightning::update::InstallType;
using lightning::update::TrustStore;
using lightning::update::UpdateManager;

namespace {

class FixedTrustStore final : public TrustStore
{
public:
    QHash<QString, QString> keys;
    QString publicKeyForId(QStringView keyId) const override
    {
        return keys.value(keyId.toString());
    }
};

class TestSigner
{
public:
    ~TestSigner()
    {
        if (m_key)
            EVP_PKEY_free(m_key);
    }

    bool generate()
    {
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        if (!ctx)
            return false;
        const bool ok = EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_keygen(ctx, &m_key) == 1;
        EVP_PKEY_CTX_free(ctx);
        if (!ok || !m_key)
            return false;
        std::size_t length = 0;
        if (EVP_PKEY_get_raw_public_key(m_key, nullptr, &length) != 1 || length != 32)
            return false;
        QByteArray raw(int(length), Qt::Uninitialized);
        if (EVP_PKEY_get_raw_public_key(m_key, reinterpret_cast<unsigned char *>(raw.data()),
                                        &length)
            != 1) {
            return false;
        }
        m_publicKeyBase64 = QString::fromLatin1(raw.toBase64());
        return true;
    }

    QByteArray sign(const QByteArray &payload) const
    {
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        if (!ctx)
            return {};
        QByteArray signature;
        std::size_t length = 0;
        if (EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, m_key) == 1
            && EVP_DigestSign(ctx, nullptr, &length,
                              reinterpret_cast<const unsigned char *>(payload.constData()),
                              std::size_t(payload.size()))
                == 1) {
            signature.resize(int(length));
            if (EVP_DigestSign(ctx, reinterpret_cast<unsigned char *>(signature.data()), &length,
                               reinterpret_cast<const unsigned char *>(payload.constData()),
                               std::size_t(payload.size()))
                != 1) {
                signature.clear();
            }
        }
        EVP_MD_CTX_free(ctx);
        return signature;
    }

    QString publicKeyBase64() const { return m_publicKeyBase64; }

private:
    EVP_PKEY *m_key = nullptr;
    QString m_publicKeyBase64;
};

const QString kHash =
    QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
const char kKeyId[] = "lightning-release-test";

QString artifactUrl(const QString &version, const QString &filename)
{
    return QStringLiteral("https://%1/api/v4/projects/6/packages/generic/lightning/%2/%3")
        .arg(lightning::update::canonicalUpdateHost(), version, filename);
}

QJsonObject manifestObject(const QString &version, bool withDebArtifact = true,
                           const QJsonObject &channels = {})
{
    QJsonObject manifest;
    manifest.insert(QStringLiteral("schema"), 1);
    manifest.insert(QStringLiteral("version"), version);
    manifest.insert(QStringLiteral("channel"), QStringLiteral("stable"));
    manifest.insert(QStringLiteral("tag"), QStringLiteral("v") + version);
    manifest.insert(QStringLiteral("expires"), QStringLiteral("2099-01-01T00:00:00Z"));
    manifest.insert(QStringLiteral("release_notes"), QStringLiteral("Notes for ") + version);
    if (withDebArtifact) {
        const QString filename = QStringLiteral("lightning_%1_amd64.deb").arg(version);
        QJsonObject artifact;
        artifact.insert(QStringLiteral("filename"), filename);
        artifact.insert(QStringLiteral("size"), 4096);
        artifact.insert(QStringLiteral("sha256"), kHash);
        artifact.insert(QStringLiteral("url"), artifactUrl(version, filename));
        QJsonObject artifacts;
        artifacts.insert(QStringLiteral("linux-deb"), artifact);
        manifest.insert(QStringLiteral("artifacts"), artifacts);
    }
    if (!channels.isEmpty())
        manifest.insert(QStringLiteral("channels"), channels);
    return manifest;
}

QJsonObject artifactEntry(const QString &version, const QString &filename)
{
    QJsonObject artifact;
    artifact.insert(QStringLiteral("filename"), filename);
    artifact.insert(QStringLiteral("size"), 4096);
    artifact.insert(QStringLiteral("sha256"), kHash);
    artifact.insert(QStringLiteral("url"), artifactUrl(version, filename));
    return artifact;
}

QJsonObject manifestWithArtifact(const QString &version, const QString &installTypeId,
                                 const QString &filename)
{
    QJsonObject manifest = manifestObject(version,false);
    QJsonObject artifacts;
    artifacts.insert(installTypeId, artifactEntry(version, filename));
    manifest.insert(QStringLiteral("artifacts"), artifacts);
    return manifest;
}

const QByteArray &payloadBytes()
{
    static const QByteArray bytes = QByteArray(4096, 'L');
    return bytes;
}

const QByteArray &tamperedBytes()
{
    static const QByteArray bytes = QByteArray(4096, 'X');
    return bytes;
}

QString payloadSha256()
{
    return QString::fromLatin1(
        QCryptographicHash::hash(payloadBytes(), QCryptographicHash::Sha256).toHex());
}

QString mirrorHost()
{
    const QStringList hosts = lightning::update::mirrorArtifactHosts();
    return hosts.isEmpty() ? QString() : hosts.first();
}

QString mirrorArtifactUrl(const QString &version, const QString &filename)
{
    return QStringLiteral("https://%1/Mizerd/lightning/releases/download/v%2/%3")
        .arg(mirrorHost(), version, filename);
}

QJsonObject downloadableManifest(const QString &version, const QString &mirror = QString())
{
    const QString filename = QStringLiteral("lightning_%1_amd64.deb").arg(version);
    QJsonObject artifact;
    artifact.insert(QStringLiteral("filename"), filename);
    artifact.insert(QStringLiteral("size"), qint64(payloadBytes().size()));
    artifact.insert(QStringLiteral("sha256"), payloadSha256());
    artifact.insert(QStringLiteral("url"), artifactUrl(version, filename));
    if (!mirror.isEmpty())
        artifact.insert(QStringLiteral("mirror_url"), mirror);

    QJsonObject artifacts;
    artifacts.insert(QStringLiteral("linux-deb"), artifact);
    QJsonObject manifest = manifestObject(version,false);
    manifest.insert(QStringLiteral("artifacts"), artifacts);
    return manifest;
}

QJsonObject mirroredManifest(const QString &version)
{
    return downloadableManifest(
        version, mirrorArtifactUrl(version, QStringLiteral("lightning_%1_amd64.deb").arg(version)));
}

class ByteSourceRecorder
{
public:
    QList<QUrl> requests;
    QHash<QString, QByteArray> bodies;

    void serve(const QString &url, const QByteArray &bytes) { bodies.insert(url, bytes); }

    std::optional<QByteArray> operator()(const QUrl &url)
    {
        requests.append(url);
        const auto it = bodies.constFind(url.toString());
        if (it == bodies.constEnd())
            return std::nullopt;
        return *it;
    }

    QStringList requestedHosts() const
    {
        QStringList hosts;
        for (const QUrl &url : requests)
            hosts.append(url.host());
        return hosts;
    }
};

bool writeHelperStatus(const QString &stagingRoot, bool ok, const QString &mode,
                       const QString &error)
{
    QJsonObject object;
    object.insert(QStringLiteral("ok"), ok);
    object.insert(QStringLiteral("mode"), mode);
    object.insert(QStringLiteral("error"), error);
    object.insert(QStringLiteral("timestamp"),
                  QStringLiteral("2026-08-16T12:00:00Z"));
    QFile file(QDir(stagingRoot).absoluteFilePath(QStringLiteral("update-status.json")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    file.close();
    return true;
}

InstallDetection detectionFor(InstallType type, bool diagnosticOverride = false,
                              bool developmentCheckAllowed = false)
{
    InstallDetection detection;
    detection.type = type;
    detection.diagnosticOverride = diagnosticOverride;
    detection.automaticInstallAllowed =
        lightning::update::canInstallAutomatically(type) && !diagnosticOverride;
    detection.developmentCheckAllowed = developmentCheckAllowed;
    return detection;
}

}

class UpdateManagerStateTest : public QObject
{
    Q_OBJECT

private:
    TestSigner m_signer;
    FixedTrustStore m_trust;
    QTemporaryDir m_staging;
    QString m_helperPath;
    QString m_artifactPath;

    QByteArray sign(const QByteArray &payload) const
    {
        QJsonObject envelope;
        envelope.insert(QStringLiteral("alg"), QStringLiteral("ed25519"));
        envelope.insert(QStringLiteral("key_id"), QString::fromLatin1(kKeyId));
        envelope.insert(QStringLiteral("sig"),
                        QString::fromLatin1(m_signer.sign(payload).toBase64()));
        return QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    }

    std::unique_ptr<UpdateManager> makeManager(InstallType type,
                                               const QString &installedVersion = QStringLiteral(
                                                   "0.7.0"),
                                               bool diagnosticOverride = false,
                                               bool developmentCheckAllowed = false)
    {
        auto manager = std::make_unique<UpdateManager>();
        manager->setTrustStoreForTest(&m_trust);
        manager->setInstallDetectionForTest(
            detectionFor(type, diagnosticOverride, developmentCheckAllowed));
        manager->setCurrentVersionForTest(installedVersion);
        manager->setStagingRootForTest(m_staging.path());
        manager->setHelperPathForTest(m_helperPath);
        manager->setNetworkDisabledForTest(true);
        return manager;
    }

    void ingest(UpdateManager *manager, const QJsonObject &manifest)
    {
        const QByteArray bytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
        manager->ingestCheckDocuments(bytes, sign(bytes));
    }

    std::unique_ptr<UpdateManager> makeDownloadManager(const QString &stagingRoot,
                                                       ByteSourceRecorder *source)
    {
        auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
        manager->setStagingRootForTest(stagingRoot);
        manager->setArtifactByteSourceForTest(
            [source](const QUrl &url) { return (*source)(url); });
        return manager;
    }

    static QStringList stagedFiles(const QString &root)
    {
        QStringList names;
        const QFileInfoList entries =
            QDir(root).entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
        for (const QFileInfo &entry : entries) {
            if (entry.fileName() == QLatin1String("update.lock"))
                continue;
            names.append(entry.fileName());
        }
        names.sort();
        return names;
    }

    static bool updateLockIsFree(const QString &root)
    {
        QLockFile probe(QDir(root).absoluteFilePath(QStringLiteral("update.lock")));
        probe.setStaleLockTime(60 * 60 * 1000);
        if (!probe.tryLock(0))
            return false;
        probe.unlock();
        return true;
    }

private slots:
    void theStagedHelperLandsOutsideTheInstallationWithItsLibraries();
    void everyInstallTypeBuildsArgumentsTheHelperAccepts();
    void windowsInstallersHandTheHelperTheirInstallScope();
    void declinedElevationIsExplainedNotCalledARefusal();
    void aRunningLightningIsExplainedNotCalledARefusal();
    void initTestCase();
    void init();

    void startsIdleWithAutomaticChecksOff();
    void offersANewerRelease();
    void reportsUpToDateForTheSameVersion();
    void refusesToDowngrade();
    void ignoresAPrereleaseOnStable();
    void failsOnATamperedManifest();
    void failsOnAnUnknownKey();
    void failsOnAnUnparseableLocalVersion();
    void reportsAReleaseWithoutADownloadForThisInstallType();
    void developmentBuildsDoNotCheckWithoutOptIn();
    void flatpakNeedsItsChannelToSayAvailable();
    void flatpakInstallIsRefusedWithoutRunningAnything();
    void aRepositoryRpmIsUpdatedByDnfNeverFromTheDownload();
    void developmentInstallIsRefusedWithoutRunningAnything();
    void diagnosticOverrideDoesNotAuthoriseInstallation();
    void installLaunchesTheHelperWithAnArgumentVector();
    void installAndRestartAsksTheApplicationToQuit();
    void installIsIgnoredUnlessReady();
    void dismissingAVersionPersists();
    void updateAvailableWarningTracksDismissalPerVersion();
    void automaticChecksAreRateLimited();
    void settingsAreApplicationWideNotAccountScoped();
    void requestsCarryOnlyTheLightningVersion();
    void managedHelpOffersACopyableCommand();

    void stagedArtifactTakesTheManifestFilename_data();
    void stagedArtifactTakesTheManifestFilename();
    void portableInstallTargetsTheApplicationDirectory();
    void onlyInstallAndRestartAsksTheHelperToRelaunch();
    void installWithoutRestartDefersTheHandoffUntilQuit();
    void handoffNeverClaimsTheUpdateWasInstalled();
    void helperSuccessStatusIsSurfacedOnceThenConsumed();
    void helperFailureStatusIsSurfacedWithItsError();
    void unusableStatusFileIsSimplyNoResult_data();
    void unusableStatusFileIsSimplyNoResult();
    void staleStagedArtifactsAreSweptAtStartup();
    void aRedirectThatCannotTruncateReportsAFailure();

    void usesTheMirrorFirstWhenTheManifestNamesOne();
    void fallsBackToTheCanonicalSourceWhenTheMirrorIsUnreachable();
    void fallsBackWhenTheMirrorServesModifiedBytes();
    void bothSourcesFailingIsOneCleanTerminalFailure();
    void anArtifactWithoutAMirrorDownloadsExactlyAsBefore();
    void aMirrorResponseIsNeverReadAsMetadata();
    void cancellingBetweenAttemptsStopsTheFallback();
    void updateSourcesNeverUseTheGitHubApi();

    void anExpiredManifestStillOffersTheUpdateAndSaysSo();
    void theFallbackFetcherHopsOnlyWithinTheMirror();
    void aCanonicalAnswerThatDoesNotVerifyIsAHardStop();

    void theManifestPairIsReadFromGitHubFirstAndGitLabSecond();
    void aPrimaryTransportFailureFallsBackToGitLabThenGitlabComOnce();
    void aPrimaryAnswerThatDoesNotVerifyIsAHardStop();
    void onlyAnUnavailableSourceMovesTheCheckToTheNextSource();
    void aBadSignatureFromGitlabComIsRefused();
    void aValidManifestFromGitlabComIsOffered();
    void gitlabComAddressesAreCompileTimeConstants();
    void theGitlabComFetcherAndDownloaderStayOnGitlabCom();
    void anArtifactFallsBackToGitlabComAfterTheOtherSources();
    void bytesFromGitlabComAreHashCheckedLikeAnyOther();
    void aHashMismatchFromTheCanonicalSourceDoesNotTryGitlabCom();
    void aMirrorSignedManifestNamingAForeignHostIsStillRefused();
    void aGitLabOutageCostsNothingWhenTheMirrorServesTheArtifact();
    void theReleasePageLinkIsTheGitHubTagForTheOfferedVersion();
    void aPrereleaseIsRefusedWhateverChannelTheManifestClaims();
    void installHandsTheHelperTheVerifiedDigest();
    void installRefusesAnArtifactModifiedAfterVerification();
    void aDeferredInstallReHashesAtQuitAndRecordsTheRefusal();
    void aNewDownloadDisarmsAPendingDeferredInstall();
};

void UpdateManagerStateTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("LightningTest"));
    QCoreApplication::setApplicationName(QStringLiteral("UpdateManagerStateTest"));
    QVERIFY2(m_signer.generate(), "OpenSSL could not generate an Ed25519 keypair");
    m_trust.keys.insert(QString::fromLatin1(kKeyId), m_signer.publicKeyBase64());
    QVERIFY(m_staging.isValid());

    m_helperPath = m_staging.path() + QStringLiteral("/lightning-updater");
    QFile helper(m_helperPath);
    QVERIFY(helper.open(QIODevice::WriteOnly));
    helper.write("#!/bin/false\n");
    helper.close();

    m_artifactPath = m_staging.path() + QStringLiteral("/verified-artifact.deb");
    QFile artifact(m_artifactPath);
    QVERIFY(artifact.open(QIODevice::WriteOnly));
    artifact.write("payload");
    artifact.close();
}

void UpdateManagerStateTest::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
}

void UpdateManagerStateTest::startsIdleWithAutomaticChecksOff()
{
    const auto manager = makeManager(InstallType::LinuxDeb);
    QCOMPARE(manager->state(), UpdateManager::Idle);
    QVERIFY(manager->automaticChecksEnabled());
    manager->setAutomaticChecksEnabled(false);
    QVERIFY(!manager->automaticChecksEnabled());
    manager->setAutomaticChecksEnabled(true);
    QVERIFY(!manager->updateAvailable());
    QVERIFY(manager->latestVersion().isEmpty());
    QCOMPARE(manager->installTypeString(), QStringLiteral("linux-deb"));
    QVERIFY(manager->canInstallAutomatically());
    QVERIFY(!manager->packageManaged());
    QCOMPARE(manager->downloadProgress(), 0.0);
}

void UpdateManagerStateTest::offersANewerRelease()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    QSignalSpy stateSpy(manager.get(), &UpdateManager::stateChanged);
    ingest(manager.get(), manifestObject(QStringLiteral("0.8.0")));

    QCOMPARE(manager->state(), UpdateManager::UpdateAvailable);
    QVERIFY(manager->updateAvailable());
    QCOMPARE(manager->latestVersion(), QStringLiteral("0.8.0"));
    QCOMPARE(manager->releaseNotes(), QStringLiteral("Notes for 0.8.0"));
    QCOMPARE(manager->totalBytes(), qint64(4096));
    QVERIFY(manager->errorMessage().isEmpty());
    QVERIFY(manager->lastCheckTime().isValid());
    QVERIFY(stateSpy.count() >= 1);
}

void UpdateManagerStateTest::reportsUpToDateForTheSameVersion()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.8.0"));
    ingest(manager.get(), manifestObject(QStringLiteral("0.8.0")));
    QCOMPARE(manager->state(), UpdateManager::UpToDate);
    QVERIFY(!manager->updateAvailable());
    QVERIFY(manager->latestVersion().isEmpty());
}

void UpdateManagerStateTest::refusesToDowngrade()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.9.0"));
    ingest(manager.get(), manifestObject(QStringLiteral("0.8.0")));
    QCOMPARE(manager->state(), UpdateManager::UpToDate);
    QVERIFY(!manager->updateAvailable());
    QVERIFY(manager->statusDetail().contains(QStringLiteral("0.8.0")));
    QVERIFY(manager->statusDetail().contains(QStringLiteral("downgrade")));
}

void UpdateManagerStateTest::ignoresAPrereleaseOnStable()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    ingest(manager.get(), manifestObject(QStringLiteral("0.8.0-rc1")));
    QCOMPARE(manager->state(), UpdateManager::UpToDate);
    QVERIFY(!manager->updateAvailable());
    QVERIFY(manager->statusDetail().contains(QStringLiteral("pre-release")));
}

void UpdateManagerStateTest::failsOnATamperedManifest()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    QByteArray bytes =
        QJsonDocument(manifestObject(QStringLiteral("0.8.0"))).toJson(QJsonDocument::Compact);
    const QByteArray envelope = sign(bytes);
    bytes.replace("0.8.0", "0.9.0");
    manager->checkForUpdates();
    QCOMPARE(manager->state(), UpdateManager::Checking);
    manager->ingestCheckDocuments(bytes, envelope);
    QCOMPARE(manager->state(), UpdateManager::Failed);
    QVERIFY(!manager->updateAvailable());
    QVERIFY(manager->latestVersion().isEmpty());
    QVERIFY(!manager->errorMessage().isEmpty());
}

void UpdateManagerStateTest::failsOnAnUnknownKey()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    const QByteArray bytes =
        QJsonDocument(manifestObject(QStringLiteral("0.8.0"))).toJson(QJsonDocument::Compact);
    QJsonObject envelope;
    envelope.insert(QStringLiteral("alg"), QStringLiteral("ed25519"));
    envelope.insert(QStringLiteral("key_id"), QStringLiteral("rogue-key"));
    envelope.insert(QStringLiteral("sig"),
                    QString::fromLatin1(m_signer.sign(bytes).toBase64()));
    const QByteArray rogue = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    manager->checkForUpdates();
    manager->ingestCheckDocuments(bytes, rogue);
    QCOMPARE(manager->state(), UpdateManager::Failed);
    QVERIFY(!manager->updateAvailable());
}

void UpdateManagerStateTest::failsOnAnUnparseableLocalVersion()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("nightly"));
    ingest(manager.get(), manifestObject(QStringLiteral("0.8.0")));
    QCOMPARE(manager->state(), UpdateManager::Failed);
    QVERIFY(!manager->updateAvailable());
}

void UpdateManagerStateTest::reportsAReleaseWithoutADownloadForThisInstallType()
{
    const auto manager = makeManager(InstallType::LinuxRpm, QStringLiteral("0.7.0"));
    ingest(manager.get(), manifestObject(QStringLiteral("0.8.0")));
    QCOMPARE(manager->state(), UpdateManager::UpdateAvailable);
    QVERIFY(manager->updateAvailable());
    QVERIFY(manager->statusDetail().contains(QStringLiteral("manually")));

    manager->downloadUpdate();
    QCOMPARE(manager->state(), UpdateManager::Failed);
}

void UpdateManagerStateTest::developmentBuildsDoNotCheckWithoutOptIn()
{
    const auto manager = makeManager(InstallType::Development, QStringLiteral("0.7.0"));
    manager->checkForUpdates();
    QCOMPARE(manager->state(), UpdateManager::Failed);
    QVERIFY(manager->errorMessage().contains(QStringLiteral("development")));

    const auto optedIn = makeManager(InstallType::Development, QStringLiteral("0.7.0"),
false,
true);
    optedIn->checkForUpdates();
    QCOMPARE(optedIn->state(), UpdateManager::Checking);
    ingest(optedIn.get(), manifestObject(QStringLiteral("0.8.0")));
    QCOMPARE(optedIn->state(), UpdateManager::UpdateAvailable);
    QVERIFY(!optedIn->canInstallAutomatically());
}

void UpdateManagerStateTest::flatpakNeedsItsChannelToSayAvailable()
{
    QJsonObject unavailable;
    unavailable.insert(QStringLiteral("linux-flatpak"),
                       QJsonObject{ { QStringLiteral("available"), false },
                                    { QStringLiteral("version"), QJsonValue(QJsonValue::Null) },
                                    { QStringLiteral("note"),
                                      QStringLiteral("pending Flathub publication") } });

    const auto pending = makeManager(InstallType::LinuxFlatpak, QStringLiteral("0.7.0"));
    ingest(pending.get(),
           manifestObject(QStringLiteral("0.8.0"),false, unavailable));
    QCOMPARE(pending->state(), UpdateManager::UpToDate);
    QVERIFY(!pending->updateAvailable());
    QCOMPARE(pending->statusDetail(), QStringLiteral("pending Flathub publication"));

    QJsonObject available;
    available.insert(QStringLiteral("linux-flatpak"),
                     QJsonObject{ { QStringLiteral("available"), true },
                                  { QStringLiteral("version"), QStringLiteral("0.8.0") } });
    const auto published = makeManager(InstallType::LinuxFlatpak, QStringLiteral("0.7.0"));
    ingest(published.get(),
           manifestObject(QStringLiteral("0.8.0"),false, available));
    QCOMPARE(published->state(), UpdateManager::UpdateAvailable);
    QVERIFY(published->updateAvailable());
    QVERIFY(published->packageManaged());
    QVERIFY(!published->canInstallAutomatically());
}

void UpdateManagerStateTest::flatpakInstallIsRefusedWithoutRunningAnything()
{
    const auto manager = makeManager(InstallType::LinuxFlatpak, QStringLiteral("0.7.0"));
    bool launched = false;
    manager->setProcessLauncherForTest([&launched](const QString &, const QStringList &) {
        launched = true;
        return true;
    });
    manager->setStagedArtifactForTest(m_artifactPath);
    QSignalSpy refusedSpy(manager.get(), &UpdateManager::installRefused);

    manager->installUpdate();
    QCOMPARE(refusedSpy.count(), 1);
    QVERIFY(!launched);
    QVERIFY(refusedSpy.at(0).at(0).toString().contains(QStringLiteral("managed")));
    QCOMPARE(manager->state(), UpdateManager::ReadyToInstall);
}

void UpdateManagerStateTest::aRepositoryRpmIsUpdatedByDnfNeverFromTheDownload()
{
    QJsonObject manifest = manifestWithArtifact(QStringLiteral("0.8.0"),
                                                QStringLiteral("linux-rpm"),
                                                QStringLiteral("lightning-0.8.0-1.x86_64.rpm"));
    manifest.insert(QStringLiteral("channels"),
                    QJsonObject{ { QStringLiteral("linux-rpm-repo"),
                                   QJsonObject{ { QStringLiteral("available"), false },
                                                { QStringLiteral("version"),
                                                  QJsonValue(QJsonValue::Null) },
                                                { QStringLiteral("note"),
                                                  QStringLiteral("not in the COPR yet") } } } });
    const auto waiting = makeManager(InstallType::LinuxRpmRepo, QStringLiteral("0.7.0"));
    ingest(waiting.get(), manifest);
    QCOMPARE(waiting->state(), UpdateManager::UpToDate);
    QVERIFY(!waiting->updateAvailable());
    QCOMPARE(waiting->statusDetail(), QStringLiteral("not in the COPR yet"));

    manifest.insert(QStringLiteral("channels"),
                    QJsonObject{ { QStringLiteral("linux-rpm-repo"),
                                   QJsonObject{ { QStringLiteral("available"), true },
                                                { QStringLiteral("version"),
                                                  QStringLiteral("0.8.0") } } } });
    const auto manager = makeManager(InstallType::LinuxRpmRepo, QStringLiteral("0.7.0"));
    ingest(manager.get(), manifest);
    QCOMPARE(manager->state(), UpdateManager::UpdateAvailable);
    QVERIFY(manager->packageManaged());
    QVERIFY(!manager->canInstallAutomatically());
    QCOMPARE(manager->statusDetail(),
             QStringLiteral("Updates for this installation are managed by dnf."));

    QSignalSpy refusedSpy(manager.get(), &UpdateManager::installRefused);
    manager->downloadUpdate();
    QCOMPARE(refusedSpy.count(), 1);
    QCOMPARE(manager->state(), UpdateManager::UpdateAvailable);
    bool launched = false;
    manager->setProcessLauncherForTest([&launched](const QString &, const QStringList &) {
        launched = true;
        return true;
    });
    manager->setStagedArtifactForTest(m_artifactPath);
    manager->installUpdate();
    QCOMPARE(refusedSpy.count(), 2);
    QVERIFY(!launched);
    QCOMPARE(refusedSpy.at(1).at(0).toString(),
             QStringLiteral("Updates for this installation are managed by dnf."));
    QCOMPARE(manager->state(), UpdateManager::ReadyToInstall);

    QSignalSpy helpSpy(manager.get(), &UpdateManager::managedUpdateHelpRequested);
    manager->openManagedUpdateHelp();
    QCOMPARE(helpSpy.count(), 1);
    QCOMPARE(helpSpy.at(0).at(0).toString(),
             QStringLiteral("sudo dnf upgrade --refresh lightning-matrix"));
    QVERIFY(helpSpy.at(0).at(1).toString().contains(QStringLiteral("managed by dnf")));

    const auto downloaded = makeManager(InstallType::LinuxRpm, QStringLiteral("0.7.0"));
    ingest(downloaded.get(), manifest);
    QCOMPARE(downloaded->state(), UpdateManager::UpdateAvailable);
    QVERIFY(!downloaded->packageManaged());
    QVERIFY(downloaded->canInstallAutomatically());
    QVERIFY(downloaded->managedUpdateCommand().isEmpty());
}

void UpdateManagerStateTest::developmentInstallIsRefusedWithoutRunningAnything()
{
    const auto manager = makeManager(InstallType::Development, QStringLiteral("0.7.0"));
    bool launched = false;
    manager->setProcessLauncherForTest([&launched](const QString &, const QStringList &) {
        launched = true;
        return true;
    });
    manager->setStagedArtifactForTest(m_artifactPath);
    QSignalSpy refusedSpy(manager.get(), &UpdateManager::installRefused);

    manager->installUpdate();
    manager->installAndRestart();
    QCOMPARE(refusedSpy.count(), 2);
    QVERIFY(!launched);
}

void UpdateManagerStateTest::diagnosticOverrideDoesNotAuthoriseInstallation()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"),
true);
    bool launched = false;
    manager->setProcessLauncherForTest([&launched](const QString &, const QStringList &) {
        launched = true;
        return true;
    });
    manager->setStagedArtifactForTest(m_artifactPath);
    QSignalSpy refusedSpy(manager.get(), &UpdateManager::installRefused);

    manager->installUpdate();
    QCOMPARE(refusedSpy.count(), 1);
    QVERIFY(!launched);
}

void UpdateManagerStateTest::installLaunchesTheHelperWithAnArgumentVector()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    QString program;
    QStringList arguments;
    manager->setProcessLauncherForTest([&](const QString &p, const QStringList &a) {
        program = p;
        arguments = a;
        return true;
    });
    manager->setStagedArtifactForTest(m_artifactPath);

    manager->installUpdate();
    QMetaObject::invokeMethod(QCoreApplication::instance(), "aboutToQuit");
    QCOMPARE(manager->state(), UpdateManager::RestartRequired);
    QCOMPARE(program, m_helperPath);

    const int modeIndex = arguments.indexOf(QStringLiteral("--mode"));
    QVERIFY(modeIndex >= 0);
    QCOMPARE(arguments.at(modeIndex + 1), QStringLiteral("linux-deb"));
    const int artifactIndex = arguments.indexOf(QStringLiteral("--artifact"));
    QVERIFY(artifactIndex >= 0);
    QCOMPARE(arguments.at(artifactIndex + 1), QFileInfo(m_artifactPath).absoluteFilePath());
    QVERIFY(arguments.contains(QStringLiteral("--pid")));
    QVERIFY(arguments.contains(QStringLiteral("--target")));
    QVERIFY(arguments.contains(QStringLiteral("--status")));
    QVERIFY(!arguments.contains(QStringLiteral("--relaunch")));

    QCOMPARE(QFileInfo(program).fileName(), QStringLiteral("lightning-updater"));
    for (const QString &argument : arguments) {
        QVERIFY2(!argument.contains(QLatin1Char(';')), qPrintable(argument));
        QVERIFY2(!argument.contains(QStringLiteral("&&")), qPrintable(argument));
        QVERIFY2(!argument.contains(QLatin1Char('|')), qPrintable(argument));
        QVERIFY2(!argument.contains(QLatin1Char('`')), qPrintable(argument));
    }
    QCOMPARE(manager->lastLaunchProgramForTest(), m_helperPath);
    QCOMPARE(manager->lastLaunchArgumentsForTest(), arguments);
}

void UpdateManagerStateTest::installAndRestartAsksTheApplicationToQuit()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    manager->setProcessLauncherForTest([](const QString &, const QStringList &) { return true; });
    manager->setStagedArtifactForTest(m_artifactPath);
    QSignalSpy quitSpy(manager.get(), &UpdateManager::quitRequested);

    manager->installAndRestart();
    QCOMPARE(manager->state(), UpdateManager::RestartRequired);
    QCOMPARE(quitSpy.count(), 1);
}

void UpdateManagerStateTest::installIsIgnoredUnlessReady()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    bool launched = false;
    manager->setProcessLauncherForTest([&launched](const QString &, const QStringList &) {
        launched = true;
        return true;
    });

    manager->installUpdate();
    ingest(manager.get(), manifestObject(QStringLiteral("0.8.0")));
    QCOMPARE(manager->state(), UpdateManager::UpdateAvailable);
    manager->installUpdate();
    QVERIFY(!launched);
    QCOMPARE(manager->state(), UpdateManager::UpdateAvailable);

    manager->setStagedArtifactForTest(m_staging.path() + QStringLiteral("/missing.deb"));
    manager->installUpdate();
    QVERIFY(!launched);
    QCOMPARE(manager->state(), UpdateManager::Failed);
}

void UpdateManagerStateTest::dismissingAVersionPersists()
{
    {
        const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
        ingest(manager.get(), manifestObject(QStringLiteral("0.8.0")));
        QSignalSpy dismissedSpy(manager.get(), &UpdateManager::dismissedVersionChanged);
        manager->dismissVersion();
        QCOMPARE(manager->dismissedVersion(), QStringLiteral("0.8.0"));
        QCOMPARE(dismissedSpy.count(), 1);
    }
    const auto reopened = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    QCOMPARE(reopened->dismissedVersion(), QStringLiteral("0.8.0"));
}

void UpdateManagerStateTest::updateAvailableWarningTracksDismissalPerVersion()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    QVERIFY(!manager->updateAvailableWarning());

    QSignalSpy warningSpy(manager.get(),
                          &UpdateManager::updateAvailableWarningChanged);
    ingest(manager.get(), manifestObject(QStringLiteral("0.8.0")));
    QCOMPARE(manager->state(), UpdateManager::UpdateAvailable);
    QVERIFY(manager->updateAvailableWarning());
    QVERIFY(warningSpy.count() > 0);

    manager->dismissVersion();
    QVERIFY(!manager->updateAvailableWarning());

    ingest(manager.get(), manifestObject(QStringLiteral("0.9.0")));
    QCOMPARE(manager->state(), UpdateManager::UpdateAvailable);
    QVERIFY(manager->updateAvailableWarning());
}

void UpdateManagerStateTest::automaticChecksAreRateLimited()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    manager->setNowForTest(now);

    manager->setAutomaticChecksEnabled(false);
    manager->setProcessStartForTest(now.addSecs(-3600));
    QVERIFY(!manager->maybeCheckAutomatically());
    QCOMPARE(manager->state(), UpdateManager::Idle);

    manager->setAutomaticChecksEnabled(true);
    manager->setProcessStartForTest(now.addSecs(-5));
    QVERIFY(!manager->maybeCheckAutomatically());
    QCOMPARE(manager->state(), UpdateManager::Idle);

    manager->setProcessStartForTest(now.addSecs(-3600));
    QVERIFY(manager->maybeCheckAutomatically());
    QCOMPARE(manager->state(), UpdateManager::Checking);
    ingest(manager.get(), manifestObject(QStringLiteral("0.8.0")));
    QVERIFY(manager->lastCheckTime().isValid());

    manager->setNowForTest(now.addSecs(23 * 3600));
    QVERIFY(!manager->maybeCheckAutomatically());

    manager->setNowForTest(now.addSecs(25 * 3600));
    QVERIFY(manager->maybeCheckAutomatically());
    QCOMPARE(manager->state(), UpdateManager::Checking);
}

void UpdateManagerStateTest::settingsAreApplicationWideNotAccountScoped()
{
    {
        const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
        manager->setAutomaticChecksEnabled(false);
        ingest(manager.get(), manifestObject(QStringLiteral("0.8.0")));
        manager->dismissVersion();
    }

    QSettings settings;
    settings.sync();
    const QStringList keys = settings.allKeys();
    QVERIFY(!keys.isEmpty());
    for (const QString &key : keys) {
        QVERIFY2(key.startsWith(QStringLiteral("update/")), qPrintable(key));
        QVERIFY2(!key.contains(QStringLiteral("accounts")), qPrintable(key));
        QVERIFY2(!key.contains(QLatin1Char('@')), qPrintable(key));
    }
    QVERIFY(keys.contains(QStringLiteral("update/automaticChecks")));
    QVERIFY(keys.contains(QStringLiteral("update/dismissedVersion")));
    QVERIFY(keys.contains(QStringLiteral("update/lastCheckTime")));

    QCOMPARE(settings.value(QStringLiteral("update/dismissedVersion")).toString(),
             QStringLiteral("0.8.0"));
}

void UpdateManagerStateTest::requestsCarryOnlyTheLightningVersion()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    const QString agent = manager->userAgentString();
    QCOMPARE(agent, QStringLiteral("Lightning/0.7.0"));
    QVERIFY(!agent.contains(QLatin1Char('@')));
    QVERIFY(!agent.contains(QLatin1Char('(')));
    QVERIFY(!agent.contains(QStringLiteral("matrix"), Qt::CaseInsensitive));
    QVERIFY(!agent.contains(QStringLiteral("Linux")));
    QVERIFY(!agent.contains(QStringLiteral("Windows")));
}

void UpdateManagerStateTest::managedHelpOffersACopyableCommand()
{
    const auto manager = makeManager(InstallType::LinuxSnap, QStringLiteral("0.7.0"));
    QSignalSpy helpSpy(manager.get(), &UpdateManager::managedUpdateHelpRequested);
    manager->openManagedUpdateHelp();
    QCOMPARE(helpSpy.count(), 1);
    QVERIFY(helpSpy.at(0).at(0).toString().startsWith(QStringLiteral("snap refresh")));
    QVERIFY(helpSpy.at(0).at(1).toString().contains(QStringLiteral("Snap")));

    const auto deb = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    QVERIFY(deb->managedUpdateCommand().isEmpty());
}

void UpdateManagerStateTest::stagedArtifactTakesTheManifestFilename_data()
{
    QTest::addColumn<int>("installType");
    QTest::addColumn<QString>("typeId");
    QTest::addColumn<QString>("filename");

    QTest::newRow("deb") << int(InstallType::LinuxDeb) << "linux-deb"
                         << "lightning_0.8.0_amd64.deb";
    QTest::newRow("rpm") << int(InstallType::LinuxRpm) << "linux-rpm"
                         << "lightning-0.8.0-1.x86_64.rpm";
}

void UpdateManagerStateTest::stagedArtifactTakesTheManifestFilename()
{
    QFETCH(int, installType);
    QFETCH(QString, typeId);
    QFETCH(QString, filename);

    const auto manager =
        makeManager(static_cast<InstallType>(installType), QStringLiteral("0.7.0"));
    ingest(manager.get(), manifestWithArtifact(QStringLiteral("0.8.0"), typeId, filename));
    QCOMPARE(manager->state(), UpdateManager::UpdateAvailable);

    const QString partPath =
        QDir(m_staging.path()).absoluteFilePath(QStringLiteral("lightning-update-a7Xk21.part"));
    {
        QFile part(partPath);
        QVERIFY(part.open(QIODevice::WriteOnly));
        part.write("payload");
        part.close();
    }

    QStringList arguments;
    manager->setProcessLauncherForTest([&](const QString &, const QStringList &a) {
        arguments = a;
        return true;
    });
    manager->setStagedArtifactForTest(partPath);
    QCOMPARE(manager->state(), UpdateManager::ReadyToInstall);

    manager->installUpdate();
    QMetaObject::invokeMethod(QCoreApplication::instance(), "aboutToQuit");
    const int artifactIndex = arguments.indexOf(QStringLiteral("--artifact"));
    QVERIFY(artifactIndex >= 0);
    const QString staged = arguments.at(artifactIndex + 1);

    QCOMPARE(QFileInfo(staged).fileName(), filename);
    QVERIFY(!staged.endsWith(QStringLiteral(".part")));
    QVERIFY(QFileInfo(staged).isAbsolute());
    QVERIFY(staged.contains(QLatin1Char('/')));
    QCOMPARE(QDir::cleanPath(QFileInfo(staged).absolutePath()),
             QDir::cleanPath(m_staging.path()));
    QVERIFY(QFileInfo::exists(staged));
    QVERIFY(!QFileInfo::exists(partPath));
    QCOMPARE(manager->stagedArtifactPathForTest(), staged);

    QFile::remove(staged);
}

void UpdateManagerStateTest::portableInstallTargetsTheApplicationDirectory()
{
    const auto manager =
        makeManager(InstallType::WindowsPortable, QStringLiteral("0.7.0"));
    QStringList arguments;
    manager->setProcessLauncherForTest([&](const QString &, const QStringList &a) {
        arguments = a;
        return true;
    });
    manager->setStagedArtifactForTest(m_artifactPath);
    manager->installUpdate();
    QMetaObject::invokeMethod(QCoreApplication::instance(), "aboutToQuit");

    const int targetIndex = arguments.indexOf(QStringLiteral("--target"));
    QVERIFY(targetIndex >= 0);
    QCOMPARE(arguments.at(targetIndex + 1), QCoreApplication::applicationDirPath());
    QVERIFY(QFileInfo(arguments.at(targetIndex + 1)).isDir());

    const auto deb = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    QStringList debArguments;
    deb->setProcessLauncherForTest([&](const QString &, const QStringList &a) {
        debArguments = a;
        return true;
    });
    deb->setStagedArtifactForTest(m_artifactPath);
    deb->installUpdate();
    QMetaObject::invokeMethod(QCoreApplication::instance(), "aboutToQuit");
    const int debTargetIndex = debArguments.indexOf(QStringLiteral("--target"));
    QVERIFY(debTargetIndex >= 0);
    QCOMPARE(debArguments.at(debTargetIndex + 1),
             QCoreApplication::applicationFilePath());
}

void UpdateManagerStateTest::onlyInstallAndRestartAsksTheHelperToRelaunch()
{
    const auto quiet = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    QStringList quietArguments;
    quiet->setProcessLauncherForTest([&](const QString &, const QStringList &a) {
        quietArguments = a;
        return true;
    });
    quiet->setStagedArtifactForTest(m_artifactPath);
    quiet->installUpdate();
    QMetaObject::invokeMethod(QCoreApplication::instance(), "aboutToQuit");
    QVERIFY(!quietArguments.isEmpty());
    QVERIFY(!quietArguments.contains(QStringLiteral("--relaunch")));

    const auto restarting = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    QStringList restartArguments;
    restarting->setProcessLauncherForTest([&](const QString &, const QStringList &a) {
        restartArguments = a;
        return true;
    });
    restarting->setStagedArtifactForTest(m_artifactPath);
    restarting->installAndRestart();
    const int index = restartArguments.indexOf(QStringLiteral("--relaunch"));
    QVERIFY(index >= 0);
    QCOMPARE(restartArguments.at(index + 1), QCoreApplication::applicationFilePath());
}

void UpdateManagerStateTest::installWithoutRestartDefersTheHandoffUntilQuit()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    int launches = 0;
    manager->setProcessLauncherForTest([&](const QString &, const QStringList &) {
        ++launches;
        return true;
    });
    manager->setStagedArtifactForTest(m_artifactPath);

    manager->installUpdate();
    QCOMPARE(launches, 0);
    QCOMPARE(manager->state(), UpdateManager::RestartRequired);
    QVERIFY(!manager->handoffSummary().contains(QStringLiteral("minute")));

    QMetaObject::invokeMethod(QCoreApplication::instance(), "aboutToQuit");
    QCOMPARE(launches, 1);

    QMetaObject::invokeMethod(QCoreApplication::instance(), "aboutToQuit");
    QCOMPARE(launches, 1);
}

void UpdateManagerStateTest::handoffNeverClaimsTheUpdateWasInstalled()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    manager->setProcessLauncherForTest([](const QString &, const QStringList &) { return true; });
    manager->setStagedArtifactForTest(m_artifactPath);
    manager->installUpdate();

    QCOMPARE(manager->state(), UpdateManager::RestartRequired);
    const QString summary = manager->handoffSummary();
    QVERIFY(!summary.isEmpty());
    QCOMPARE(manager->statusDetail(), summary);
    QVERIFY(summary.contains(QStringLiteral("Nothing has been installed yet")));
    QVERIFY(!summary.contains(QStringLiteral("installed successfully")));
    QVERIFY(!summary.contains(QStringLiteral("updated")));
    QCOMPARE(manager->lastUpdateResult(), UpdateManager::NoResult);

    const auto restarting = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    restarting->setProcessLauncherForTest(
        [](const QString &, const QStringList &) { return true; });
    restarting->setStagedArtifactForTest(m_artifactPath);
    restarting->installAndRestart();
    QVERIFY(restarting->handoffSummary().contains(QStringLiteral("Nothing has been "
                                                                "installed yet")));
}

void UpdateManagerStateTest::helperSuccessStatusIsSurfacedOnceThenConsumed()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(writeHelperStatus(root.path(), true, QStringLiteral("linux-deb"), QString()));
    const QString statusPath =
        QDir(root.path()).absoluteFilePath(QStringLiteral("update-status.json"));

    {
        UpdateManager manager;
        QSignalSpy resultSpy(&manager, &UpdateManager::lastUpdateResultChanged);
        manager.setStagingRootForTest(root.path());
        QCOMPARE(manager.lastUpdateResult(), UpdateManager::InstallSucceeded);
        QCOMPARE(manager.lastUpdateMode(), QStringLiteral("linux-deb"));
        QVERIFY(manager.lastUpdateError().isEmpty());
        QCOMPARE(resultSpy.count(), 1);
        QVERIFY(!QFileInfo::exists(statusPath));

        manager.clearLastUpdateResult();
        QCOMPARE(manager.lastUpdateResult(), UpdateManager::NoResult);
        QVERIFY(manager.lastUpdateMode().isEmpty());
    }

    UpdateManager next;
    next.setStagingRootForTest(root.path());
    QCOMPARE(next.lastUpdateResult(), UpdateManager::NoResult);
}

void UpdateManagerStateTest::helperFailureStatusIsSurfacedWithItsError()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(writeHelperStatus(root.path(), false, QStringLiteral("linux-deb"),
                              QStringLiteral("installer-exit-100")));

    UpdateManager manager;
    manager.setStagingRootForTest(root.path());
    QCOMPARE(manager.lastUpdateResult(), UpdateManager::InstallFailed);
    QCOMPARE(manager.lastUpdateError(), QStringLiteral("installer-exit-100"));
    QCOMPARE(manager.lastUpdateMode(), QStringLiteral("linux-deb"));
    QVERIFY(!QFileInfo::exists(
        QDir(root.path()).absoluteFilePath(QStringLiteral("update-status.json"))));

    QVERIFY(!manager.lastUpdateError().contains(QLatin1Char('/')));
    QVERIFY(!manager.lastUpdateError().contains(QLatin1Char('@')));
}

void UpdateManagerStateTest::unusableStatusFileIsSimplyNoResult_data()
{
    QTest::addColumn<QByteArray>("contents");
    QTest::addColumn<bool>("present");

    QTest::newRow("absent") << QByteArray() << false;
    QTest::newRow("empty") << QByteArray() << true;
    QTest::newRow("not-json") << QByteArray("this is not json at all") << true;
    QTest::newRow("truncated") << QByteArray("{\"ok\":tr") << true;
    QTest::newRow("json-array") << QByteArray("[1,2,3]") << true;
    QTest::newRow("no-ok-field") << QByteArray("{\"mode\":\"linux-deb\"}") << true;
    QTest::newRow("ok-not-a-bool") << QByteArray("{\"ok\":\"yes\"}") << true;
    QTest::newRow("ok-is-a-number") << QByteArray("{\"ok\":1}") << true;
}

void UpdateManagerStateTest::unusableStatusFileIsSimplyNoResult()
{
    QFETCH(QByteArray, contents);
    QFETCH(bool, present);

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString statusPath =
        QDir(root.path()).absoluteFilePath(QStringLiteral("update-status.json"));
    if (present) {
        QFile file(statusPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
        file.close();
    }

    UpdateManager manager;
    manager.setStagingRootForTest(root.path());
    QCOMPARE(manager.lastUpdateResult(), UpdateManager::NoResult);
    QVERIFY(manager.lastUpdateError().isEmpty());
    QVERIFY(manager.lastUpdateMode().isEmpty());
    QCOMPARE(manager.state(), UpdateManager::Idle);
    QVERIFY(!QFileInfo::exists(statusPath));
}

void UpdateManagerStateTest::staleStagedArtifactsAreSweptAtStartup()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QDir dir(root.path());
    const QString stale = dir.absoluteFilePath(QStringLiteral("lightning_0.7.9_amd64.deb"));
    const QString stalePart =
        dir.absoluteFilePath(QStringLiteral("lightning-update-abcdef.part"));
    const QString fresh = dir.absoluteFilePath(QStringLiteral("lightning_0.8.0_amd64.deb"));
    const QString lock = dir.absoluteFilePath(QStringLiteral("update.lock"));

    for (const QString &path : { stale, stalePart, fresh, lock }) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("bytes");
        file.close();
    }

    const QDateTime old = QDateTime::currentDateTimeUtc().addDays(-1);
    bool aged = true;
    for (const QString &path : { stale, stalePart }) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        aged = file.setFileTime(old, QFileDevice::FileModificationTime) && aged;
        file.close();
    }
    if (!aged)
        QSKIP("this filesystem does not support setting a modification time");

    UpdateManager manager;
    manager.setStagingRootForTest(root.path());

    QVERIFY(!QFileInfo::exists(stale));
    QVERIFY(!QFileInfo::exists(stalePart));
    QVERIFY(QFileInfo::exists(fresh));
    QVERIFY(QFileInfo::exists(lock));

    QTemporaryDir elsewhere;
    QVERIFY(elsewhere.isValid());
    const QString outsider = QDir(elsewhere.path()).absoluteFilePath(QStringLiteral("keep.deb"));
    {
        QFile file(outsider);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("bytes");
        file.close();
        QFile aging(outsider);
        QVERIFY(aging.open(QIODevice::ReadWrite));
        aging.setFileTime(old, QFileDevice::FileModificationTime);
        aging.close();
    }
#ifndef Q_OS_WIN
    QVERIFY(QFile::link(outsider, dir.absoluteFilePath(QStringLiteral("linked.deb"))));
#endif
    UpdateManager again;
    again.setStagingRootForTest(root.path());
    QVERIFY(QFileInfo::exists(outsider));
}

void UpdateManagerStateTest::aRedirectThatCannotTruncateReportsAFailure()
{
    using lightning::update::TransferError;

    const QString path =
        QDir(m_staging.path()).absoluteFilePath(QStringLiteral("redirect-target.bin"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("partial bytes from before the redirect");
        file.close();
    }

    QFile readOnly(path);
    QVERIFY(readOnly.open(QIODevice::ReadOnly));
    QVERIFY(!readOnly.resize(0));

    int calls = 0;
    bool ok = true;
    TransferError seen = TransferError::None;
    lightning::update::UpdateDownloader downloader(nullptr);
    QObject::connect(&downloader, &lightning::update::UpdateTransferBase::finished,
                     [&](bool success, TransferError error, const QString &) {
                         ++calls;
                         ok = success;
                         seen = error;
                     });
    downloader.prepareForTest(&readOnly, 4096, kHash);
    downloader.restartForTest();

    QCOMPARE(calls, 1);
    QVERIFY(!ok);
    QCOMPARE(seen, TransferError::FileError);
    readOnly.close();

    QFile writable(path);
    QVERIFY(writable.open(QIODevice::WriteOnly));
    writable.write("partial");
    QVERIFY(writable.flush());

    int quietCalls = 0;
    lightning::update::UpdateDownloader healthy(nullptr);
    QObject::connect(&healthy, &lightning::update::UpdateTransferBase::finished,
                     [&](bool, TransferError, const QString &) { ++quietCalls; });
    healthy.prepareForTest(&writable, 4096, kHash);
    healthy.restartForTest();
    QCOMPARE(quietCalls, 0);
    QCOMPARE(writable.size(), qint64(0));
    writable.close();
    QFile::remove(path);
}

void UpdateManagerStateTest::usesTheMirrorFirstWhenTheManifestNamesOne()
{
    if (mirrorHost().isEmpty())
        QSKIP("this build trusts no artifact mirror");

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString version = QStringLiteral("0.8.0");
    const QString filename = QStringLiteral("lightning_%1_amd64.deb").arg(version);

    ByteSourceRecorder source;
    source.serve(mirrorArtifactUrl(version, filename), payloadBytes());
    source.serve(artifactUrl(version, filename), payloadBytes());

    const auto manager = makeDownloadManager(root.path(), &source);
    ingest(manager.get(), mirroredManifest(version));
    QCOMPARE(manager->state(), UpdateManager::UpdateAvailable);

    manager->downloadUpdate();
    QTRY_COMPARE(manager->state(), UpdateManager::ReadyToInstall);

    QCOMPARE(source.requests.size(), 1);
    QCOMPARE(source.requests.first().host(), mirrorHost());
    QCOMPARE(manager->artifactSource(), QStringLiteral("mirror"));
    QVERIFY(manager->errorMessage().isEmpty());
    QVERIFY(manager->statusDetail().isEmpty());

    const QString staged = manager->stagedArtifactPathForTest();
    QCOMPARE(QFileInfo(staged).fileName(), filename);
    QFile file(staged);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), payloadBytes());
}

void UpdateManagerStateTest::fallsBackToTheCanonicalSourceWhenTheMirrorIsUnreachable()
{
    if (mirrorHost().isEmpty())
        QSKIP("this build trusts no artifact mirror");

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString version = QStringLiteral("0.8.0");
    const QString filename = QStringLiteral("lightning_%1_amd64.deb").arg(version);

    ByteSourceRecorder source;
    source.serve(artifactUrl(version, filename), payloadBytes());

    const auto manager = makeDownloadManager(root.path(), &source);
    ingest(manager.get(), mirroredManifest(version));
    manager->downloadUpdate();
    QTRY_COMPARE(manager->state(), UpdateManager::ReadyToInstall);

    QCOMPARE(source.requestedHosts(),
             (QStringList{ mirrorHost(), lightning::update::canonicalUpdateHost() }));
    QCOMPARE(manager->artifactSource(), QStringLiteral("canonical"));
    QVERIFY(manager->errorMessage().isEmpty());
    QVERIFY(manager->statusDetail().contains(QStringLiteral("mirror")));

    QCOMPARE(manager->totalBytes(), qint64(payloadBytes().size()));
    QCOMPARE(manager->downloadedBytes(), qint64(payloadBytes().size()));
    QCOMPARE(manager->downloadProgress(), 1.0);

    QCOMPARE(stagedFiles(root.path()), QStringList{ filename });
    QFile file(manager->stagedArtifactPathForTest());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), payloadBytes());
}

void UpdateManagerStateTest::fallsBackWhenTheMirrorServesModifiedBytes()
{
    if (mirrorHost().isEmpty())
        QSKIP("this build trusts no artifact mirror");

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString version = QStringLiteral("0.8.0");
    const QString filename = QStringLiteral("lightning_%1_amd64.deb").arg(version);

    ByteSourceRecorder source;
    source.serve(mirrorArtifactUrl(version, filename), tamperedBytes());
    source.serve(artifactUrl(version, filename), payloadBytes());

    const auto manager = makeDownloadManager(root.path(), &source);
    QList<qint64> observed;
    QObject::connect(manager.get(), &UpdateManager::downloadProgressChanged, manager.get(),
                     [&]() { observed.append(manager->downloadedBytes()); });

    ingest(manager.get(), mirroredManifest(version));
    manager->downloadUpdate();
    QTRY_COMPARE(manager->state(), UpdateManager::ReadyToInstall);

    QCOMPARE(source.requestedHosts(),
             (QStringList{ mirrorHost(), lightning::update::canonicalUpdateHost() }));
    QCOMPARE(manager->artifactSource(), QStringLiteral("canonical"));
    QVERIFY(manager->statusDetail().contains(QStringLiteral("mirror")));

    const qint64 size = payloadBytes().size();
    QString trace;
    for (const qint64 value : observed)
        trace += QString::number(value) + QLatin1Char(' ');
    QVERIFY2(observed.contains(size), qPrintable(trace));
    QVERIFY2(observed.lastIndexOf(qint64(0)) > observed.indexOf(size), qPrintable(trace));
    QCOMPARE(manager->downloadedBytes(), size);
    QCOMPARE(manager->totalBytes(), size);

    QCOMPARE(stagedFiles(root.path()), QStringList{ filename });
    QFile file(manager->stagedArtifactPathForTest());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray installed = file.readAll();
    QCOMPARE(installed, payloadBytes());
    QVERIFY(installed != tamperedBytes());
    QCOMPARE(QString::fromLatin1(
                 QCryptographicHash::hash(installed, QCryptographicHash::Sha256).toHex()),
             payloadSha256());
}

void UpdateManagerStateTest::bothSourcesFailingIsOneCleanTerminalFailure()
{
    if (mirrorHost().isEmpty())
        QSKIP("this build trusts no artifact mirror");

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString version = QStringLiteral("0.8.0");

    ByteSourceRecorder source;

    const auto manager = makeDownloadManager(root.path(), &source);
    QSignalSpy stateSpy(manager.get(), &UpdateManager::stateChanged);
    ingest(manager.get(), mirroredManifest(version));
    manager->downloadUpdate();
    QTRY_COMPARE(manager->state(), UpdateManager::Failed);

    QCOMPARE(source.requestedHosts(),
             (QStringList{ mirrorHost(), lightning::update::canonicalUpdateHost(),
                           QStringLiteral("gitlab.com") }));
    QCoreApplication::processEvents();
    QCOMPARE(source.requests.size(), 3);
    QCOMPARE(manager->state(), UpdateManager::Failed);

    QCOMPARE(stagedFiles(root.path()), QStringList{});
    QVERIFY(updateLockIsFree(root.path()));
    QVERIFY(!manager->errorMessage().isEmpty());
    QVERIFY(!manager->errorMessage().contains(mirrorHost()));
    QVERIFY(!manager->errorMessage().contains(lightning::update::canonicalUpdateHost()));
    QVERIFY(!manager->errorMessage().contains(QStringLiteral("http")));
    QVERIFY(manager->artifactSource().isEmpty());
    QVERIFY(stateSpy.count() >= 1);
}

void UpdateManagerStateTest::anArtifactWithoutAMirrorDownloadsExactlyAsBefore()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString version = QStringLiteral("0.8.0");
    const QString filename = QStringLiteral("lightning_%1_amd64.deb").arg(version);

    ByteSourceRecorder source;
    source.serve(artifactUrl(version, filename), payloadBytes());

    const auto manager = makeDownloadManager(root.path(), &source);
    ingest(manager.get(), downloadableManifest(version));
    QCOMPARE(manager->state(), UpdateManager::UpdateAvailable);

    manager->downloadUpdate();
    QTRY_COMPARE(manager->state(), UpdateManager::ReadyToInstall);

    QCOMPARE(source.requestedHosts(),
             QStringList{ lightning::update::canonicalUpdateHost() });
    QCOMPARE(manager->artifactSource(), QStringLiteral("canonical"));
    QVERIFY(manager->statusDetail().isEmpty());
    QCOMPARE(stagedFiles(root.path()), QStringList{ filename });
}

void UpdateManagerStateTest::aMirrorResponseIsNeverReadAsMetadata()
{
    if (mirrorHost().isEmpty())
        QSKIP("this build trusts no artifact mirror");

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString version = QStringLiteral("0.8.0");
    const QString filename = QStringLiteral("lightning_%1_amd64.deb").arg(version);

    // Not a raw string literal: moc's preprocessor ends the macro argument at
    // the first ')' inside R"(...)".
    QByteArray githubish = QByteArrayLiteral(
        "{\"tag_name\":\"v99.0.0\",\"name\":\"Lightning 99.0.0\",\"draft\":false,"
        "\"assets\":[{\"browser_download_url\":\"https://evil.example/payload.deb\"}]}");
    githubish.append(QByteArray(payloadBytes().size() - githubish.size(), ' '));
    QCOMPARE(githubish.size(), payloadBytes().size());

    ByteSourceRecorder source;
    source.serve(mirrorArtifactUrl(version, filename), githubish);
    source.serve(artifactUrl(version, filename), payloadBytes());

    const auto manager = makeDownloadManager(root.path(), &source);
    ingest(manager.get(), mirroredManifest(version));
    manager->downloadUpdate();
    QTRY_COMPARE(manager->state(), UpdateManager::ReadyToInstall);

    QCOMPARE(manager->latestVersion(), version);
    QCOMPARE(manager->releaseNotes(), QStringLiteral("Notes for ") + version);
    QVERIFY(!manager->releaseNotes().contains(QStringLiteral("99.0.0")));
    QVERIFY(!manager->statusDetail().contains(QStringLiteral("99.0.0")));
    QVERIFY(!manager->errorMessage().contains(QStringLiteral("99.0.0")));
    QCOMPARE(manager->artifactSource(), QStringLiteral("canonical"));

    QFile file(manager->stagedArtifactPathForTest());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray installed = file.readAll();
    QCOMPARE(installed, payloadBytes());
    QVERIFY(!installed.contains("browser_download_url"));
    QVERIFY(!installed.contains("evil.example"));
}

void UpdateManagerStateTest::cancellingBetweenAttemptsStopsTheFallback()
{
    if (mirrorHost().isEmpty())
        QSKIP("this build trusts no artifact mirror");

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString version = QStringLiteral("0.8.0");
    const QString filename = QStringLiteral("lightning_%1_amd64.deb").arg(version);

    ByteSourceRecorder source;
    source.serve(artifactUrl(version, filename), payloadBytes());

    const auto manager = makeDownloadManager(root.path(), &source);
    ingest(manager.get(), mirroredManifest(version));
    manager->downloadUpdate();

    QCOMPARE(source.requests.size(), 1);
    QCOMPARE(manager->state(), UpdateManager::Downloading);

    manager->cancelDownload();
    QCOMPARE(manager->state(), UpdateManager::UpdateAvailable);
    QVERIFY(manager->statusDetail().contains(QStringLiteral("cancelled")));

    QCoreApplication::processEvents();
    QCOMPARE(source.requests.size(), 1);
    QCOMPARE(manager->state(), UpdateManager::UpdateAvailable);
    QVERIFY(manager->artifactSource().isEmpty());
    QCOMPARE(stagedFiles(root.path()), QStringList{});
    QVERIFY(updateLockIsFree(root.path()));
}

void UpdateManagerStateTest::updateSourcesNeverUseTheGitHubApi()
{
    QString sourceDir;
#ifdef LIGHTNING_UPDATE_SOURCE_DIR
    if (QFileInfo::exists(QString::fromLatin1(LIGHTNING_UPDATE_SOURCE_DIR)
                          + QStringLiteral("/UpdateEndpoints.cpp"))) {
        sourceDir = QString::fromLatin1(LIGHTNING_UPDATE_SOURCE_DIR);
    }
#endif
    for (const QString &start :
         { QCoreApplication::applicationDirPath(), QDir::currentPath() }) {
        if (!sourceDir.isEmpty())
            break;
        QDir dir(start);
        for (int depth = 0; depth < 8; ++depth) {
            const QString candidate = dir.absoluteFilePath(QStringLiteral("src/update"));
            if (QFileInfo::exists(candidate + QStringLiteral("/UpdateEndpoints.cpp"))) {
                sourceDir = candidate;
                break;
            }
            if (!dir.cdUp())
                break;
        }
    }
    QVERIFY2(!sourceDir.isEmpty(), "could not locate src/update to scan");

    const QFileInfoList files =
        QDir(sourceDir).entryInfoList(QStringList{ QStringLiteral("*.h"), QStringLiteral("*.cpp") },
                                      QDir::Files);
    QVERIFY(files.size() >= 8);

    const QStringList forbidden{
        QStringLiteral("api.github.com"), QStringLiteral("uploads.github.com"),
        QStringLiteral("releases/latest"), QStringLiteral("vnd.github"),
        QStringLiteral("/repos/"),
    };
    for (const QFileInfo &info : files) {
        QFile file(info.absoluteFilePath());
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 qPrintable(info.absoluteFilePath()));
        const QString text = QString::fromUtf8(file.readAll());
        for (const QString &token : forbidden) {
            QVERIFY2(!text.contains(token, Qt::CaseInsensitive),
                     qPrintable(QStringLiteral("%1 contains %2")
                                    .arg(info.fileName(), token)));
        }
    }
}

void UpdateManagerStateTest::anExpiredManifestStillOffersTheUpdateAndSaysSo()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    manager->setNowForTest(QDateTime::fromString(QStringLiteral("2026-09-02T12:00:00Z"),
                                                 Qt::ISODate));
    QJsonObject manifest = manifestObject(QStringLiteral("0.8.0"));
    manifest.insert(QStringLiteral("expires"), QStringLiteral("2026-09-01T00:00:00Z"));
    ingest(manager.get(), manifest);
    QCOMPARE(manager->state(), UpdateManager::UpdateAvailable);
    QVERIFY(manager->updateAvailable());
    QVERIFY(manager->errorMessage().isEmpty());
    QVERIFY2(manager->statusDetail().contains(QStringLiteral("expected to be refreshed")),
             qPrintable(manager->statusDetail()));

    const auto current = makeManager(InstallType::LinuxDeb, QStringLiteral("0.8.0"));
    current->setNowForTest(QDateTime::fromString(QStringLiteral("2026-09-02T12:00:00Z"),
                                                 Qt::ISODate));
    ingest(current.get(), manifest);
    QCOMPARE(current->state(), UpdateManager::UpToDate);
    QVERIFY(current->statusDetail().contains(QStringLiteral("expected to be refreshed")));

    const auto fresh = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    fresh->setNowForTest(QDateTime::fromString(QStringLiteral("2026-08-31T12:00:00Z"),
                                               Qt::ISODate));
    ingest(fresh.get(), manifest);
    QCOMPARE(fresh->state(), UpdateManager::UpdateAvailable);
    QVERIFY(!fresh->statusDetail().contains(QStringLiteral("expected to be refreshed")));
    const auto undated = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    manifest.remove(QStringLiteral("expires"));
    ingest(undated.get(), manifest);
    QCOMPARE(undated->state(), UpdateManager::UpdateAvailable);
    QVERIFY(!undated->statusDetail().contains(QStringLiteral("expected to be refreshed")));

    const auto garbled = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    manifest.insert(QStringLiteral("expires"), QStringLiteral("soon"));
    ingest(garbled.get(), manifest);
    QCOMPARE(garbled->state(), UpdateManager::UpdateAvailable);
    QVERIFY2(garbled->statusDetail().contains(QStringLiteral("unreadable refresh date")),
             qPrintable(garbled->statusDetail()));
}

void UpdateManagerStateTest::aCanonicalAnswerThatDoesNotVerifyIsAHardStop()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    manager->checkForUpdates();
    QCOMPARE(manager->state(), UpdateManager::Checking);
    QVERIFY(!manager->metadataInFallbackStageForTest());
    const QByteArray bytes =
        QJsonDocument(manifestObject(QStringLiteral("0.8.0"))).toJson(QJsonDocument::Compact);
    QByteArray tampered = bytes;
    tampered.replace("0.8.0", "0.8.1");
    manager->ingestCheckDocuments(tampered, sign(bytes));
    QCOMPARE(manager->state(), UpdateManager::Failed);
    QVERIFY(!manager->updateAvailable());
}

void UpdateManagerStateTest::theManifestPairIsReadFromGitHubFirstAndGitLabSecond()
{
    namespace u = lightning::update;
    if (u::mirrorLatestManifestUrl().isEmpty())
        QSKIP("this build compiles in no mirror manifest slot");

    QCOMPARE(u::primaryManifestUrl(), u::mirrorLatestManifestUrl());
    QCOMPARE(u::primaryManifestSignatureUrl(), u::mirrorLatestManifestSignatureUrl());
    QCOMPARE(u::fallbackManifestUrl(), u::latestManifestUrl());
    QCOMPARE(u::fallbackManifestSignatureUrl(), u::latestManifestSignatureUrl());
    QVERIFY(u::mirrorArtifactHosts().contains(u::primaryManifestUrl().host()));
    QCOMPARE(u::fallbackManifestUrl().host(), u::canonicalUpdateHost());

    QVERIFY(!u::primaryManifestUrl().path().contains(QStringLiteral("/latest/")));
    QVERIFY(u::primaryManifestUrl().path().contains(QStringLiteral("/download/")));

    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    manager->checkForUpdates();
    QCOMPARE(manager->state(), UpdateManager::Checking);
    QVERIFY(!manager->metadataInFallbackStageForTest());
    QCOMPARE(manager->lastSignatureUrlForTest(), u::primaryManifestSignatureUrl());
    QVERIFY(manager->lastSignatureUrlForTest().host() != u::canonicalUpdateHost());
}

void UpdateManagerStateTest::aPrimaryTransportFailureFallsBackToGitLabThenGitlabComOnce()
{
    namespace u = lightning::update;
    if (u::mirrorLatestManifestUrl().isEmpty())
        QSKIP("this build compiles in no mirror manifest slot");

    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    manager->checkForUpdates();
    QCOMPARE(manager->lastSignatureUrlForTest(), u::primaryManifestSignatureUrl());

    manager->simulateMetadataFetchFailureForTest(QStringLiteral("primary unreachable"));
    QCOMPARE(manager->state(), UpdateManager::Checking);
    QVERIFY(manager->metadataInFallbackStageForTest());
    QCOMPARE(manager->lastSignatureUrlForTest(), u::latestManifestSignatureUrl());
    QCOMPARE(manager->lastSignatureUrlForTest().host(), u::canonicalUpdateHost());

    manager->simulateMetadataFetchFailureForTest(QStringLiteral("fallback unreachable"));
    QCOMPARE(manager->state(), UpdateManager::Checking);
    QCOMPARE(manager->metadataStageForTest(), 2);
    QCOMPARE(manager->lastSignatureUrlForTest(), u::gitlabComManifestSignatureUrl());
    QCOMPARE(manager->lastSignatureUrlForTest().host(), QStringLiteral("gitlab.com"));

    manager->simulateMetadataFetchFailureForTest(QStringLiteral("gitlab.com unreachable"));
    QCOMPARE(manager->state(), UpdateManager::Failed);
    QCOMPARE(manager->metadataStageForTest(), 2);
    QCOMPARE(manager->lastSignatureUrlForTest(), u::gitlabComManifestSignatureUrl());

    manager->checkForUpdates();
    QVERIFY(!manager->metadataInFallbackStageForTest());
    QCOMPARE(manager->lastSignatureUrlForTest(), u::primaryManifestSignatureUrl());
}

void UpdateManagerStateTest::aPrimaryAnswerThatDoesNotVerifyIsAHardStop()
{
    if (lightning::update::mirrorLatestManifestUrl().isEmpty())
        QSKIP("this build compiles in no mirror manifest slot");

    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    manager->checkForUpdates();
    const QByteArray bytes =
        QJsonDocument(manifestObject(QStringLiteral("0.8.0"))).toJson(QJsonDocument::Compact);
    QByteArray tampered = bytes;
    tampered.replace("0.8.0", "0.9.9");
    manager->ingestCheckDocuments(tampered, sign(bytes));
    QCOMPARE(manager->state(), UpdateManager::Failed);
    QCOMPARE(manager->metadataStageForTest(), 0);
    QVERIFY(!manager->updateAvailable());
    QVERIFY(manager->latestVersion().isEmpty());
}

void UpdateManagerStateTest::onlyAnUnavailableSourceMovesTheCheckToTheNextSource()
{
    if (lightning::update::mirrorLatestManifestUrl().isEmpty())
        QSKIP("this build compiles in no mirror manifest slot");

    using lightning::update::TransferError;
    for (const TransferError error :
         { TransferError::HttpRejected, TransferError::SizeExceeded, TransferError::ForeignHost,
           TransferError::TooManyRedirects, TransferError::InsecureUrl }) {
        const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
        manager->checkForUpdates();
        manager->simulateMetadataFetchFailureForTest(QStringLiteral("refused"), error);
        QCOMPARE(manager->state(), UpdateManager::Failed);
        QCOMPARE(manager->metadataStageForTest(), 0);
    }

    for (const TransferError error : { TransferError::Network, TransferError::Stalled }) {
        const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
        manager->checkForUpdates();
        manager->simulateMetadataFetchFailureForTest(QStringLiteral("down"), error);
        QCOMPARE(manager->state(), UpdateManager::Checking);
        QCOMPARE(manager->metadataStageForTest(), 1);
    }
}

void UpdateManagerStateTest::aBadSignatureFromGitlabComIsRefused()
{
    if (lightning::update::mirrorLatestManifestUrl().isEmpty())
        QSKIP("this build compiles in no mirror manifest slot");

    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    manager->checkForUpdates();
    manager->simulateMetadataFetchFailureForTest(QStringLiteral("github down"));
    manager->simulateMetadataFetchFailureForTest(QStringLiteral("gitlab down"));
    QCOMPARE(manager->metadataStageForTest(), 2);

    const QByteArray bytes =
        QJsonDocument(manifestObject(QStringLiteral("0.8.0"))).toJson(QJsonDocument::Compact);
    QByteArray tampered = bytes;
    tampered.replace("0.8.0", "0.9.9");
    manager->ingestCheckDocuments(tampered, sign(bytes));
    QCOMPARE(manager->state(), UpdateManager::Failed);
    QVERIFY(!manager->updateAvailable());
    QVERIFY(manager->latestVersion().isEmpty());
    QVERIFY(!manager->errorMessage().isEmpty());
}

void UpdateManagerStateTest::aValidManifestFromGitlabComIsOffered()
{
    if (lightning::update::mirrorLatestManifestUrl().isEmpty())
        QSKIP("this build compiles in no mirror manifest slot");

    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    manager->checkForUpdates();
    manager->simulateMetadataFetchFailureForTest(QStringLiteral("github 503"));
    manager->simulateMetadataFetchFailureForTest(QStringLiteral("gitlab 503"));
    QCOMPARE(manager->metadataStageForTest(), 2);

    const QByteArray bytes =
        QJsonDocument(manifestObject(QStringLiteral("0.8.0"))).toJson(QJsonDocument::Compact);
    manager->ingestCheckDocuments(bytes, sign(bytes));
    QVERIFY(manager->updateAvailable());
    QCOMPARE(manager->latestVersion(), QStringLiteral("0.8.0"));

    const auto older = makeManager(InstallType::LinuxDeb, QStringLiteral("0.9.0"));
    older->checkForUpdates();
    older->simulateMetadataFetchFailureForTest(QStringLiteral("github 503"));
    older->simulateMetadataFetchFailureForTest(QStringLiteral("gitlab 503"));
    older->ingestCheckDocuments(bytes, sign(bytes));
    QVERIFY(!older->updateAvailable());
    QCOMPARE(older->state(), UpdateManager::UpToDate);
}

void UpdateManagerStateTest::gitlabComAddressesAreCompileTimeConstants()
{
    namespace u = lightning::update;
    const QString base = QStringLiteral(
        "https://gitlab.com/api/v4/projects/88402223/packages/generic/lightning");
    QCOMPARE(u::gitlabComManifestUrl().toString(),
             base + QStringLiteral("/update-latest/update-manifest-v1.json"));
    QCOMPARE(u::gitlabComManifestSignatureUrl().toString(),
             base + QStringLiteral("/update-latest/update-manifest-v1.json.sig"));
    QCOMPARE(u::gitlabComArtifactUrl(QStringLiteral("0.8.0"), QStringLiteral("a_b-1.deb"))
                 .toString(),
             base + QStringLiteral("/0.8.0/a_b-1.deb"));

    for (const QString &bad : { QString(), QStringLiteral("."), QStringLiteral("../x"),
                                QStringLiteral("a/b"), QStringLiteral("a?b"),
                                QStringLiteral("a#b"), QStringLiteral("a%2e"),
                                QStringLiteral(".hidden"), QStringLiteral("a b") }) {
        QVERIFY2(u::gitlabComArtifactUrl(bad, QStringLiteral("x.deb")).isEmpty(), qPrintable(bad));
        QVERIFY2(u::gitlabComArtifactUrl(QStringLiteral("0.8.0"), bad).isEmpty(), qPrintable(bad));
    }

    QVERIFY(u::isAllowedGitlabComUrl(u::gitlabComManifestUrl()));
    QVERIFY(!u::isAllowedGitlabComUrl(QUrl(QStringLiteral("http://gitlab.com/x"))));
    QVERIFY(!u::isAllowedGitlabComUrl(QUrl(QStringLiteral("https://u@gitlab.com/x"))));
    QVERIFY(!u::isAllowedGitlabComUrl(QUrl(QStringLiteral("https://gitlab.com:8443/x"))));
    QVERIFY(!u::isAllowedGitlabComUrl(QUrl(QStringLiteral("https://gitlab.com.evil.example/x"))));
    QVERIFY(!u::isAllowedGitlabComUrl(u::latestManifestUrl()));

    QVERIFY(!u::isAllowedArtifactUrl(u::gitlabComManifestUrl()));
    QVERIFY(!u::isAllowedManifestUrl(u::gitlabComManifestUrl()));
    QVERIFY(!u::isAllowedFallbackManifestUrl(u::gitlabComManifestUrl()));
    QVERIFY(!u::allowedUpdateHosts().contains(QStringLiteral("gitlab.com")));
}

void UpdateManagerStateTest::theGitlabComFetcherAndDownloaderStayOnGitlabCom()
{
    namespace u = lightning::update;
    using lightning::update::TransferError;
    u::UpdateDocumentFetcher fetcher(nullptr);
    const QUrl start = u::gitlabComManifestUrl();
    const QUrl other(QStringLiteral("https://evil.example/update-manifest-v1.json"));

    QVERIFY(fetcher.permitsForTest(start, start));
    QVERIFY(!fetcher.permitsForTest(start, other));
    QVERIFY(!fetcher.permitsForTest(start, u::latestManifestUrl()));
    QVERIFY(!fetcher.permitsForTest(start, QUrl(QStringLiteral("https://objects.githubusercontent.com/x"))));
    QVERIFY(!fetcher.permitsForTest(u::latestManifestUrl(), start));
    if (!u::mirrorLatestManifestUrl().isEmpty())
        QVERIFY(!fetcher.permitsForTest(u::mirrorLatestManifestUrl(), start));

    QTemporaryDir root;
    QVERIFY(root.isValid());
    QFile target(root.filePath(QStringLiteral("t.part")));
    QVERIFY(target.open(QIODevice::WriteOnly));
    u::UpdateDownloader downloader(nullptr);
    TransferError seen = TransferError::None;
    QObject::connect(&downloader, &u::UpdateTransferBase::finished,
                     [&](bool, TransferError error, const QString &) { seen = error; });
    downloader.deliverForTest(QUrl(QStringLiteral("https://gitlab.com.evil.example/x.deb")),
                              payloadBytes().size(), payloadSha256(), &target, payloadBytes());
    QCOMPARE(seen, TransferError::ForeignHost);
    downloader.deliverForTest(u::gitlabComArtifactUrl(QStringLiteral("0.8.0"),
                                                      QStringLiteral("x.deb")),
                              payloadBytes().size(), payloadSha256(), &target, payloadBytes());
    QCOMPARE(seen, TransferError::None);
}

void UpdateManagerStateTest::anArtifactFallsBackToGitlabComAfterTheOtherSources()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString version = QStringLiteral("0.8.0");
    const QString filename = QStringLiteral("lightning_%1_amd64.deb").arg(version);
    const QUrl gitlabCom = lightning::update::gitlabComArtifactUrl(version, filename);

    ByteSourceRecorder source;
    source.serve(gitlabCom.toString(), payloadBytes());

    const auto manager = makeDownloadManager(root.path(), &source);
    ingest(manager.get(), mirrorHost().isEmpty() ? downloadableManifest(version)
                                                 : mirroredManifest(version));
    manager->downloadUpdate();
    QTRY_COMPARE(manager->state(), UpdateManager::ReadyToInstall);

    QStringList expected;
    if (!mirrorHost().isEmpty())
        expected.append(mirrorHost());
    expected.append(lightning::update::canonicalUpdateHost());
    expected.append(QStringLiteral("gitlab.com"));
    QCOMPARE(source.requestedHosts(), expected);
    QCOMPARE(source.requests.last(), gitlabCom);
    QCOMPARE(manager->artifactSource(), QStringLiteral("gitlab-com"));
    QVERIFY(manager->statusDetail().contains(QStringLiteral("gitlab.com")));
    QCOMPARE(stagedFiles(root.path()), QStringList{ filename });
}

void UpdateManagerStateTest::bytesFromGitlabComAreHashCheckedLikeAnyOther()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString version = QStringLiteral("0.8.0");
    const QString filename = QStringLiteral("lightning_%1_amd64.deb").arg(version);

    ByteSourceRecorder source;
    source.serve(lightning::update::gitlabComArtifactUrl(version, filename).toString(),
                 tamperedBytes());

    const auto manager = makeDownloadManager(root.path(), &source);
    ingest(manager.get(), downloadableManifest(version));
    manager->downloadUpdate();
    QTRY_COMPARE(manager->state(), UpdateManager::Failed);
    QCoreApplication::processEvents();

    QCOMPARE(source.requestedHosts().last(), QStringLiteral("gitlab.com"));
    QCOMPARE(source.requests.size(), 2);
    QCOMPARE(stagedFiles(root.path()), QStringList{});
    QVERIFY(updateLockIsFree(root.path()));
    QVERIFY(manager->artifactSource().isEmpty());
}

void UpdateManagerStateTest::aHashMismatchFromTheCanonicalSourceDoesNotTryGitlabCom()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString version = QStringLiteral("0.8.0");
    const QString filename = QStringLiteral("lightning_%1_amd64.deb").arg(version);

    ByteSourceRecorder source;
    source.serve(artifactUrl(version, filename), tamperedBytes());
    source.serve(lightning::update::gitlabComArtifactUrl(version, filename).toString(),
                 payloadBytes());

    const auto manager = makeDownloadManager(root.path(), &source);
    ingest(manager.get(), downloadableManifest(version));
    manager->downloadUpdate();
    QTRY_COMPARE(manager->state(), UpdateManager::Failed);
    QCoreApplication::processEvents();

    QCOMPARE(source.requestedHosts(), QStringList{ lightning::update::canonicalUpdateHost() });
    QCOMPARE(stagedFiles(root.path()), QStringList{});
    QVERIFY(updateLockIsFree(root.path()));
}

static void refusedOnBothSources(UpdateManager *m, const QByteArray &bytes,
                                 const QByteArray &sig)
{
    m->checkForUpdates();
    QCOMPARE(m->state(), UpdateManager::Checking);
    m->ingestCheckDocuments(bytes, sig);
    if (m->state() == UpdateManager::Checking) {
        QVERIFY(m->metadataInFallbackStageForTest());
        m->ingestCheckDocuments(bytes, sig);
    }
    QCOMPARE(m->state(), UpdateManager::Failed);
    QVERIFY(!m->updateAvailable());
}

void UpdateManagerStateTest::aMirrorSignedManifestNamingAForeignHostIsStillRefused()
{
    if (mirrorHost().isEmpty())
        QSKIP("this build trusts no artifact mirror");

    const QString version = QStringLiteral("0.8.0");
    const QString filename = QStringLiteral("lightning_%1_amd64.deb").arg(version);

    {
        const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
        QJsonObject manifest = downloadableManifest(version);
        QJsonObject artifacts = manifest.value(QStringLiteral("artifacts")).toObject();
        QJsonObject deb = artifacts.value(QStringLiteral("linux-deb")).toObject();
        deb.insert(QStringLiteral("url"), mirrorArtifactUrl(version, filename));
        artifacts.insert(QStringLiteral("linux-deb"), deb);
        manifest.insert(QStringLiteral("artifacts"), artifacts);
        const QByteArray bytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
        refusedOnBothSources(manager.get(), bytes, sign(bytes));
        if (QTest::currentTestFailed())
            return;
    }

    {
        const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
        QJsonObject manifest = downloadableManifest(version);
        QJsonObject artifacts = manifest.value(QStringLiteral("artifacts")).toObject();
        QJsonObject deb = artifacts.value(QStringLiteral("linux-deb")).toObject();
        deb.insert(QStringLiteral("url"),
                   QStringLiteral("https://evil.example/%1").arg(filename));
        artifacts.insert(QStringLiteral("linux-deb"), deb);
        manifest.insert(QStringLiteral("artifacts"), artifacts);
        const QByteArray bytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
        refusedOnBothSources(manager.get(), bytes, sign(bytes));
        if (QTest::currentTestFailed())
            return;
    }

    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        ByteSourceRecorder source;
        source.serve(artifactUrl(version, filename), payloadBytes());
        const auto manager = makeDownloadManager(root.path(), &source);
        ingest(manager.get(),
               downloadableManifest(
                   version, QStringLiteral("https://evil.example/%1").arg(filename)));
        manager->downloadUpdate();
        QTRY_COMPARE(manager->state(), UpdateManager::ReadyToInstall);
        QCOMPARE(source.requestedHosts(),
                 (QStringList{ lightning::update::canonicalUpdateHost() }));
    }

    for (const QString &bad : { QStringLiteral("http://%1/x/%2").arg(mirrorHost(), filename),
                                QStringLiteral("https://user@%1/x/%2").arg(mirrorHost(), filename),
                                QStringLiteral("https://%1:8443/x/%2").arg(mirrorHost(), filename) }) {
        QVERIFY2(!lightning::update::isAllowedArtifactUrl(QUrl(bad)), qPrintable(bad));
    }
    QVERIFY(!lightning::update::isAllowedManifestUrl(
        lightning::update::mirrorLatestManifestUrl()));
    QVERIFY(lightning::update::isAllowedFallbackManifestUrl(
        lightning::update::mirrorLatestManifestUrl())
            || lightning::update::mirrorLatestManifestUrl().isEmpty());
}

void UpdateManagerStateTest::aGitLabOutageCostsNothingWhenTheMirrorServesTheArtifact()
{
    if (mirrorHost().isEmpty())
        QSKIP("this build trusts no artifact mirror");

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString version = QStringLiteral("0.8.0");
    const QString filename = QStringLiteral("lightning_%1_amd64.deb").arg(version);

    ByteSourceRecorder source;
    source.serve(mirrorArtifactUrl(version, filename), payloadBytes());
    const auto manager = makeDownloadManager(root.path(), &source);
    ingest(manager.get(), mirroredManifest(version));
    manager->downloadUpdate();
    QTRY_COMPARE(manager->state(), UpdateManager::ReadyToInstall);
    QCOMPARE(source.requestedHosts(), (QStringList{ mirrorHost() }));
    QCOMPARE(manager->artifactSource(), QStringLiteral("mirror"));
    QVERIFY(manager->statusDetail().isEmpty());
}

void UpdateManagerStateTest::theReleasePageLinkIsTheGitHubTagForTheOfferedVersion()
{
    if (lightning::update::releasePageUrlForVersion(QStringLiteral("0.8.0")).isEmpty())
        QSKIP("this build trusts no mirror host, so the link falls back to the manifest's");
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    QJsonObject manifest = manifestObject(QStringLiteral("0.8.0"));
    manifest.insert(QStringLiteral("release_notes_url"),
                    QStringLiteral("https://%1/Mizerd/lightning/-/releases/v0.8.0")
                        .arg(lightning::update::canonicalUpdateHost()));
    ingest(manager.get(), manifest);
    QVERIFY(manager->updateAvailable());
    QCOMPARE(manager->releaseNotesUrl(),
             QUrl(QStringLiteral("https://github.com/Mizerd/lightning/releases/tag/v0.8.0")));

    const auto bare = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    ingest(bare.get(), manifestObject(QStringLiteral("0.8.0")));
    QCOMPARE(bare->releaseNotesUrl(), manager->releaseNotesUrl());

    const auto foreign = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    QJsonObject bad = manifestObject(QStringLiteral("0.8.0"));
    bad.insert(QStringLiteral("release_notes_url"),
               QStringLiteral("https://github.com/Mizerd/lightning/releases/tag/v0.8.0"));
    ingest(foreign.get(), bad);
    QVERIFY(!foreign->updateAvailable());
    QVERIFY(foreign->releaseNotesUrl().isEmpty());

    QVERIFY(lightning::update::releasePageUrlForVersion(QStringLiteral("0.8.0/../x")).isEmpty());
    QVERIFY(lightning::update::releasePageUrlForVersion(QString()).isEmpty());
}

void UpdateManagerStateTest::aPrereleaseIsRefusedWhateverChannelTheManifestClaims()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    QJsonObject manifest = manifestObject(QStringLiteral("0.8.0-rc1"));
    manifest.insert(QStringLiteral("channel"), QStringLiteral("beta"));
    ingest(manager.get(), manifest);
    QCOMPARE(manager->state(), UpdateManager::UpToDate);
    QVERIFY(!manager->updateAvailable());
    QVERIFY(manager->statusDetail().contains(QStringLiteral("pre-release")));
}

void UpdateManagerStateTest::installHandsTheHelperTheVerifiedDigest()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    QStringList arguments;
    manager->setProcessLauncherForTest([&](const QString &, const QStringList &a) {
        arguments = a;
        return true;
    });
    manager->setStagedArtifactForTest(m_artifactPath);
    manager->installAndRestart();
    const int index = arguments.indexOf(QStringLiteral("--sha256"));
    QVERIFY(index >= 0);
    QCOMPARE(arguments.at(index + 1), updater::sha256HexOfFile(m_artifactPath));
    QCOMPARE(arguments.at(index + 1), manager->stagedArtifactSha256ForTest());
}

void UpdateManagerStateTest::installRefusesAnArtifactModifiedAfterVerification()
{
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    const QString artifact = QDir(staging.path()).absoluteFilePath(
        QStringLiteral("lightning_0.8.0_amd64.deb"));
    {
        QFile file(artifact);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(payloadBytes());
    }
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    manager->setStagingRootForTest(staging.path());
    int launches = 0;
    manager->setProcessLauncherForTest([&](const QString &, const QStringList &) {
        ++launches;
        return true;
    });
    manager->setStagedArtifactForTest(artifact);
    QCOMPARE(manager->state(), UpdateManager::ReadyToInstall);

    {
        QFile file(artifact);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(tamperedBytes());
    }
    manager->installAndRestart();
    QCOMPARE(launches, 0);
    QCOMPARE(manager->state(), UpdateManager::Failed);
    QVERIFY2(manager->errorMessage().contains(QStringLiteral("no longer matches")),
             qPrintable(manager->errorMessage()));
    QVERIFY(!QFileInfo::exists(artifact));
}

void UpdateManagerStateTest::aDeferredInstallReHashesAtQuitAndRecordsTheRefusal()
{
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    const QString artifact = QDir(staging.path()).absoluteFilePath(
        QStringLiteral("lightning_0.8.0_amd64.deb"));
    {
        QFile file(artifact);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(payloadBytes());
    }
    int launches = 0;
    {
        const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
        manager->setStagingRootForTest(staging.path());
        manager->setProcessLauncherForTest([&](const QString &, const QStringList &) {
            ++launches;
            return true;
        });
        manager->setStagedArtifactForTest(artifact);
        manager->installUpdate();
        QCOMPARE(manager->state(), UpdateManager::RestartRequired);

        QFile file(artifact);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(tamperedBytes());
        file.close();

        QMetaObject::invokeMethod(QCoreApplication::instance(), "aboutToQuit");
        QCOMPARE(launches, 0);
        QVERIFY(!QFileInfo::exists(artifact));
    }
    const auto next = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    next->setStagingRootForTest(staging.path());
    QCOMPARE(next->lastUpdateResult(), UpdateManager::InstallFailed);
    QCOMPARE(next->lastUpdateError(), QStringLiteral("artifact-digest-mismatch"));
    QCOMPARE(next->lastUpdateMode(), QStringLiteral("linux-deb"));
}

void UpdateManagerStateTest::aNewDownloadDisarmsAPendingDeferredInstall()
{
    QTemporaryDir staging;
    QVERIFY(staging.isValid());
    ByteSourceRecorder source;
    const auto manager = makeDownloadManager(staging.path(), &source);
    int launches = 0;
    manager->setProcessLauncherForTest([&](const QString &, const QStringList &) {
        ++launches;
        return true;
    });
    const QString version = QStringLiteral("0.8.0");
    const QString filename = QStringLiteral("lightning_%1_amd64.deb").arg(version);
    source.serve(artifactUrl(version, filename), payloadBytes());
    ingest(manager.get(), downloadableManifest(version));
    manager->downloadUpdate();
    QTRY_COMPARE(manager->state(), UpdateManager::ReadyToInstall);
    manager->installUpdate();
    QCOMPARE(manager->state(), UpdateManager::RestartRequired);

    ingest(manager.get(), downloadableManifest(version));
    manager->downloadUpdate();
    QTRY_COMPARE(manager->state(), UpdateManager::ReadyToInstall);
    QMetaObject::invokeMethod(QCoreApplication::instance(), "aboutToQuit");
    QCOMPARE(launches, 0);
    QVERIFY(!QFileInfo::exists(
        QDir(staging.path()).absoluteFilePath(QStringLiteral("update-status.json"))));
}

void UpdateManagerStateTest::theFallbackFetcherHopsOnlyWithinTheMirror()
{
    lightning::update::UpdateDocumentFetcher fetcher(nullptr);
    const QUrl canonical = lightning::update::latestManifestUrl();
    const QUrl fallback = lightning::update::mirrorLatestManifestUrl();
    const QUrl objectHost(QStringLiteral(
        "https://objects.githubusercontent.com/github-production-release-asset/x/y"));
    const QUrl third(QStringLiteral("https://evil.example/update-manifest-v1.json"));

    if (!fallback.isEmpty()) {
        QVERIFY(fetcher.permitsForTest(fallback, fallback));
        QVERIFY(fetcher.permitsForTest(fallback, objectHost));
        QVERIFY(!fetcher.permitsForTest(fallback, third));
        QVERIFY(!fetcher.permitsForTest(fallback, canonical));
        QVERIFY(!fetcher.permitsForTest(canonical, fallback));
    }
    QVERIFY(fetcher.permitsForTest(canonical, canonical));
    QVERIFY(!fetcher.permitsForTest(canonical, objectHost));
    QVERIFY(!fetcher.permitsForTest(canonical, third));
    QVERIFY(!fetcher.permitsForTest(third, third));
}
void UpdateManagerStateTest::everyInstallTypeBuildsArgumentsTheHelperAccepts()
{
    struct Case { InstallType type; const char *name; };
    const QVector<Case> everyType{
        { InstallType::WindowsMsi, "windows-msi" },
        { InstallType::WindowsSetup, "windows-setup" },
        { InstallType::WindowsPortable, "windows-portable" },
        { InstallType::LinuxAppImage, "linux-appimage" },
        { InstallType::LinuxDeb, "linux-deb" },
        { InstallType::LinuxRpm, "linux-rpm" },
        { InstallType::LinuxFlatpak, "linux-flatpak" },
        { InstallType::LinuxSnap, "linux-snap" },
        { InstallType::LinuxRpmRepo, "linux-rpm-repo" },
        { InstallType::MacosDmg, "macos-dmg" },
        { InstallType::Development, "development" },
        { InstallType::Unknown, "unknown" },
    };
    QVector<Case> cases;
    for (const Case &c : everyType) {
        QCOMPARE(lightning::update::installTypeId(c.type),
                 QString::fromLatin1(c.name));
        if (lightning::update::canInstallAutomatically(c.type))
            cases.append(c);
    }
    QVERIFY2(cases.size() >= 6,
             "fewer install types claim automatic installation than this "
             "product ships; either a type lost the claim or the enum list "
             "above went stale");
    for (const Case &c : cases)
        QVERIFY(c.type != InstallType::MacosDmg);

    for (const Case &c : cases) {
        const auto manager = makeManager(c.type, QStringLiteral("0.7.0"));
        manager->setProcessLauncherForTest(
            [](const QString &, const QStringList &) { return true; });
        manager->setStagedArtifactForTest(m_artifactPath);
        manager->installAndRestart();

        const QStringList arguments = manager->lastLaunchArgumentsForTest();
        QVERIFY2(!arguments.isEmpty(),
                 qPrintable(QStringLiteral("%1 launched nothing").arg(c.name)));

        const updater::ArgsParseResult parsed =
            updater::parseUpdaterArgs(arguments);
        QVERIFY2(parsed.ok(),
                 qPrintable(QStringLiteral("%1: the helper REFUSED the "
                                           "arguments the application built: "
                                           "%2 (%3)")
                                .arg(QLatin1String(c.name),
                                     updater::parseErrorName(parsed.error),
                                     parsed.message)));
        QCOMPARE(updater::modeToString(parsed.args.mode),
                 QString::fromLatin1(c.name));
        QCOMPARE(parsed.args.targetPath,
                 QFileInfo(parsed.args.targetPath).canonicalFilePath());
        QVERIFY(!parsed.args.artifactPath.isEmpty());
        QVERIFY(parsed.args.pid > 1);
    }
}

void UpdateManagerStateTest::windowsInstallersHandTheHelperTheirInstallScope()
{
    using lightning::update::InstallScope;
    for (const InstallType type : {InstallType::WindowsMsi, InstallType::WindowsSetup}) {
        for (const InstallScope scope : {InstallScope::User, InstallScope::Machine}) {
            const bool machine = scope == InstallScope::Machine;
            const auto manager = makeManager(type, QStringLiteral("0.7.0"));
            InstallDetection detection = detectionFor(type);
            detection.scope = scope;
            manager->setInstallDetectionForTest(detection);
            QCOMPARE(manager->installNeedsAdministrator(), machine);

            manager->setProcessLauncherForTest(
                [](const QString &, const QStringList &) { return true; });
            manager->setStagedArtifactForTest(m_artifactPath);
            manager->installAndRestart();

            const QStringList arguments = manager->lastLaunchArgumentsForTest();
            const int at = arguments.indexOf(QStringLiteral("--install-scope"));
            QVERIFY2(at >= 0 && at + 1 < arguments.size(),
                     "a Windows installer upgrade must always state its scope");
            QCOMPARE(arguments.at(at + 1),
                     machine ? QStringLiteral("machine") : QStringLiteral("user"));

            const updater::ArgsParseResult parsed = updater::parseUpdaterArgs(arguments);
            QVERIFY2(parsed.ok(), qPrintable(updater::parseErrorName(parsed.error)));
            QCOMPARE(parsed.args.installScope,
                     machine ? updater::InstallScope::Machine : updater::InstallScope::User);

            QCOMPARE(manager->handoffSummary().contains(QStringLiteral("administrator")),
                     machine);
        }
    }

    for (const InstallType type : {InstallType::WindowsPortable, InstallType::LinuxAppImage,
                                   InstallType::LinuxDeb, InstallType::LinuxRpm}) {
        const auto manager = makeManager(type, QStringLiteral("0.7.0"));
        InstallDetection detection = detectionFor(type);
        detection.scope = InstallScope::Machine;
        manager->setInstallDetectionForTest(detection);
        QVERIFY(!manager->installNeedsAdministrator());
        manager->setProcessLauncherForTest(
            [](const QString &, const QStringList &) { return true; });
        manager->setStagedArtifactForTest(m_artifactPath);
        manager->installAndRestart();
        const QStringList arguments = manager->lastLaunchArgumentsForTest();
        QVERIFY(!arguments.isEmpty());
        QVERIFY(!arguments.contains(QStringLiteral("--install-scope")));
        QVERIFY(updater::parseUpdaterArgs(arguments).ok());
    }
}

void UpdateManagerStateTest::aRunningLightningIsExplainedNotCalledARefusal()
{
    const QString running = UpdateManager::explainInstallError(QStringLiteral("installer-exit-32"));
    const QString refused = UpdateManager::explainInstallError(QStringLiteral("installer-exit-1603"));
    QVERIFY(running.contains(QStringLiteral("still running")));
    QVERIFY(running.contains(QStringLiteral("before changing anything")));
    QVERIFY(!running.contains(QStringLiteral("32")));
    QVERIFY(running != refused);
}

void UpdateManagerStateTest::declinedElevationIsExplainedNotCalledARefusal()
{
    const QString declined = UpdateManager::explainInstallError(QStringLiteral("elevation-declined"));
    const QString cancelled = UpdateManager::explainInstallError(QStringLiteral("installer-exit-1223"));
    const QString refused = UpdateManager::explainInstallError(QStringLiteral("installer-exit-1603"));
    QVERIFY(!declined.isEmpty());
    QCOMPARE(cancelled, declined);
    QVERIFY(declined.contains(QStringLiteral("administrator")));
    QVERIFY(declined != refused);
    QVERIFY(refused.contains(QStringLiteral("1603")));
}

void UpdateManagerStateTest::theStagedHelperLandsOutsideTheInstallationWithItsLibraries()
{
    QTemporaryDir installRoot;
    QVERIFY(installRoot.isValid());
    const QString installDir = installRoot.path();

    const QString helper =
        QDir(installDir).absoluteFilePath(QStringLiteral("lightning-updater.exe"));
    QFile exe(helper);
    QVERIFY(exe.open(QIODevice::WriteOnly));
    exe.write(QByteArray("helper"));
    exe.close();
    const QStringList libraries = UpdateManager::helperRuntimeLibraries();
    QVERIFY2(!libraries.isEmpty(), "the helper's library list must not be empty");
    for (const QString &library : libraries) {
        QFile lib(QDir(installDir).absoluteFilePath(library));
        QVERIFY(lib.open(QIODevice::WriteOnly));
        lib.write(library.toUtf8());
        lib.close();
    }

    QString error;
    const QString staged = UpdateManager::stageHelperOutsideInstallation(
        helper, installDir, &error);
    QVERIFY2(!staged.isEmpty(), qPrintable(error));
    QVERIFY(error.isEmpty());

    const QString cleanInstall = QDir::cleanPath(QDir(installDir).absolutePath());
    const QString cleanStaged = QDir::cleanPath(QFileInfo(staged).absolutePath());
    QVERIFY2(cleanStaged != cleanInstall
                 && !cleanStaged.startsWith(cleanInstall + QLatin1Char('/')),
             qPrintable(QStringLiteral("staged inside the installation: %1").arg(staged)));

    QVERIFY(QFileInfo::exists(staged));
    QFile copied(staged);
    QVERIFY(copied.open(QIODevice::ReadOnly));
    QCOMPARE(copied.readAll(), QByteArray("helper"));
    copied.close();
    for (const QString &library : libraries) {
        QVERIFY2(QFileInfo::exists(QDir(cleanStaged).absoluteFilePath(library)),
                 qPrintable(QStringLiteral("library not staged: %1").arg(library)));
    }

    const QString again = UpdateManager::stageHelperOutsideInstallation(
        helper, installDir, &error);
    QVERIFY2(!again.isEmpty(), qPrintable(error));

    const QString absent =
        QDir(installDir).absoluteFilePath(QStringLiteral("not-here.exe"));
    QVERIFY(UpdateManager::stageHelperOutsideInstallation(absent, installDir, &error)
                .isEmpty());
    QVERIFY(!error.isEmpty());
}

QTEST_GUILESS_MAIN(UpdateManagerStateTest)
#include "UpdateManagerStateTest.moc"
