// Lightning secure update system: the UpdateManager state machine.
//
// Covers the decision policy (newer / same / older / prerelease / tampered),
// the managed-install refusals, the argv contract handed to the updater
// helper, the privacy defaults, and the account-independence of the update
// settings. The signing key is generated at runtime; no key material and no
// network access are involved.
//
// Cross-account safety is proven twice here: the link set of this test
// binary contains no Matrix, account or SDK code at all — an UpdateManager
// has no way to reach one — and the settings assertions pin every persisted
// key under the non-account-scoped "update/" group.

#include "update/InstallType.h"
#include "update/UpdateDownloader.h"
#include "update/UpdateEndpoints.h"
#include "update/UpdateManager.h"
#include "update/UpdateManifest.h"
#include "update/UpdateTrustStore.h"

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

// A manifest publishing exactly one artifact, for a named install type.
QJsonObject manifestWithArtifact(const QString &version, const QString &installTypeId,
                                 const QString &filename)
{
    QJsonObject manifest = manifestObject(version, /*withDebArtifact=*/false);
    QJsonObject artifacts;
    artifacts.insert(installTypeId, artifactEntry(version, filename));
    manifest.insert(QStringLiteral("artifacts"), artifacts);
    return manifest;
}

// --- mirror-first download fixtures ---------------------------------------
//
// These use REAL bytes and their REAL SHA-256, because the whole point of the
// mirror path is that both sources are verified against the one hash in the
// signed manifest.

const QByteArray &payloadBytes()
{
    static const QByteArray bytes = QByteArray(4096, 'L');
    return bytes;
}

// Same LENGTH, different content: a pure integrity failure, not a size one.
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

// A manifest whose linux-deb entry describes payloadBytes() exactly, with an
// optional mirror_url. `mirror` empty = no mirror at all.
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
    QJsonObject manifest = manifestObject(version, /*withDebArtifact=*/false);
    manifest.insert(QStringLiteral("artifacts"), artifacts);
    return manifest;
}

QJsonObject mirroredManifest(const QString &version)
{
    return downloadableManifest(
        version, mirrorArtifactUrl(version, QStringLiteral("lightning_%1_amd64.deb").arg(version)));
}

// Records every address a download attempt asks for, and answers each with
// canned bytes (or a transport failure). Nothing here can weaken verification:
// the manager streams whatever this returns through the real downloader, and
// the manifest's size and SHA-256 still decide the outcome.
class ByteSourceRecorder
{
public:
    QList<QUrl> requests;
    QHash<QString, QByteArray> bodies; // url -> bytes; absent = unreachable

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

// The exact JSON shape src/updater/main.cpp writeStatus() produces.
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

} // namespace

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

    // A manager wired to the test trust store, a temp staging root and a
    // launcher that records instead of executing.
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

    // A manager with its OWN staging directory (so what a download left
    // behind can be asserted) and a recorded byte source instead of a
    // network. Verification is untouched by the seam.
    std::unique_ptr<UpdateManager> makeDownloadManager(const QString &stagingRoot,
                                                       ByteSourceRecorder *source)
    {
        auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
        manager->setStagingRootForTest(stagingRoot);
        manager->setArtifactByteSourceForTest(
            [source](const QUrl &url) { return (*source)(url); });
        return manager;
    }

    // Everything in the staging root except the single-instance lock.
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

    // The download lock is genuinely free again.
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
    void developmentInstallIsRefusedWithoutRunningAnything();
    void diagnosticOverrideDoesNotAuthoriseInstallation();
    void installLaunchesTheHelperWithAnArgumentVector();
    void installAndRestartAsksTheApplicationToQuit();
    void installIsIgnoredUnlessReady();
    void dismissingAVersionPersists();
    void automaticChecksAreRateLimited();
    void settingsAreApplicationWideNotAccountScoped();
    void requestsCarryOnlyTheLightningVersion();
    void managedHelpOffersACopyableCommand();

    // --- regressions found by review ---
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

    // --- GitHub bandwidth mirror (mirror-first, one canonical fallback) ---
    void usesTheMirrorFirstWhenTheManifestNamesOne();
    void fallsBackToTheCanonicalSourceWhenTheMirrorIsUnreachable();
    void fallsBackWhenTheMirrorServesModifiedBytes();
    void bothSourcesFailingIsOneCleanTerminalFailure();
    void anArtifactWithoutAMirrorDownloadsExactlyAsBefore();
    void aMirrorResponseIsNeverReadAsMetadata();
    void cancellingBetweenAttemptsStopsTheFallback();
    void updateSourcesNeverUseTheGitHubApi();
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
    // Every case starts from empty persistent settings.
    QSettings settings;
    settings.clear();
    settings.sync();
}

void UpdateManagerStateTest::startsIdleWithAutomaticChecksOff()
{
    const auto manager = makeManager(InstallType::LinuxDeb);
    QCOMPARE(manager->state(), UpdateManager::Idle);
    // Privacy: automatic checking is opt-in.
    QVERIFY(!manager->automaticChecksEnabled());
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
    // The situation is disclosed rather than silently ignored.
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
    manager->ingestCheckDocuments(bytes,
                                  QJsonDocument(envelope).toJson(QJsonDocument::Compact));

    QCOMPARE(manager->state(), UpdateManager::Failed);
    QVERIFY(!manager->updateAvailable());
}

void UpdateManagerStateTest::failsOnAnUnparseableLocalVersion()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("nightly"));
    ingest(manager.get(), manifestObject(QStringLiteral("0.8.0")));
    // Never "assume newer": an incomparable local version is a failure.
    QCOMPARE(manager->state(), UpdateManager::Failed);
    QVERIFY(!manager->updateAvailable());
}

void UpdateManagerStateTest::reportsAReleaseWithoutADownloadForThisInstallType()
{
    const auto manager = makeManager(InstallType::LinuxRpm, QStringLiteral("0.7.0"));
    ingest(manager.get(), manifestObject(QStringLiteral("0.8.0"))); // only a .deb artifact
    QCOMPARE(manager->state(), UpdateManager::UpdateAvailable);
    QVERIFY(manager->updateAvailable());
    QVERIFY(manager->statusDetail().contains(QStringLiteral("manually")));

    // Downloading is refused: there is nothing verified to download.
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
                                     /*diagnosticOverride=*/false,
                                     /*developmentCheckAllowed=*/true);
    optedIn->checkForUpdates();
    // The check is allowed to start (the network is stubbed out here).
    QCOMPARE(optedIn->state(), UpdateManager::Checking);
    ingest(optedIn.get(), manifestObject(QStringLiteral("0.8.0")));
    QCOMPARE(optedIn->state(), UpdateManager::UpdateAvailable);
    // ...but installing stays refused.
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
           manifestObject(QStringLiteral("0.8.0"), /*withDebArtifact=*/false, unavailable));
    // A newer release exists, but nothing is actionable for this user.
    QCOMPARE(pending->state(), UpdateManager::UpToDate);
    QVERIFY(!pending->updateAvailable());
    QCOMPARE(pending->statusDetail(), QStringLiteral("pending Flathub publication"));

    QJsonObject available;
    available.insert(QStringLiteral("linux-flatpak"),
                     QJsonObject{ { QStringLiteral("available"), true },
                                  { QStringLiteral("version"), QStringLiteral("0.8.0") } });
    const auto published = makeManager(InstallType::LinuxFlatpak, QStringLiteral("0.7.0"));
    ingest(published.get(),
           manifestObject(QStringLiteral("0.8.0"), /*withDebArtifact=*/false, available));
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
    // No hidden state change, and certainly no "install anyway" path.
    QCOMPARE(manager->state(), UpdateManager::ReadyToInstall);
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
                                     /*diagnosticOverride=*/true);
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
    // installUpdate() hands over at QUIT, not at the click (the helper's wait
    // for our PID is bounded). Drive that here so the argument vector exists.
    QMetaObject::invokeMethod(QCoreApplication::instance(), "aboutToQuit");
    QCOMPARE(manager->state(), UpdateManager::RestartRequired);
    QCOMPARE(program, m_helperPath);

    // The fixed argv contract, one option per pair.
    const int modeIndex = arguments.indexOf(QStringLiteral("--mode"));
    QVERIFY(modeIndex >= 0);
    QCOMPARE(arguments.at(modeIndex + 1), QStringLiteral("linux-deb"));
    const int artifactIndex = arguments.indexOf(QStringLiteral("--artifact"));
    QVERIFY(artifactIndex >= 0);
    QCOMPARE(arguments.at(artifactIndex + 1), QFileInfo(m_artifactPath).absoluteFilePath());
    QVERIFY(arguments.contains(QStringLiteral("--pid")));
    QVERIFY(arguments.contains(QStringLiteral("--target")));
    QVERIFY(arguments.contains(QStringLiteral("--status")));
    // installUpdate() is "apply this when I quit". It must NOT ask the helper
    // to start Lightning again afterwards.
    QVERIFY(!arguments.contains(QStringLiteral("--relaunch")));

    // The program is the compiled-in helper, not an interpreter, and no
    // argument carries shell metacharacters.
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

    manager->installUpdate(); // Idle
    ingest(manager.get(), manifestObject(QStringLiteral("0.8.0")));
    QCOMPARE(manager->state(), UpdateManager::UpdateAvailable);
    manager->installUpdate(); // UpdateAvailable, nothing verified yet
    QVERIFY(!launched);
    QCOMPARE(manager->state(), UpdateManager::UpdateAvailable);

    // A staged path that no longer exists cannot be installed either.
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
    // A new manager (a later launch) reads the same non-account-scoped key.
    const auto reopened = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    QCOMPARE(reopened->dismissedVersion(), QStringLiteral("0.8.0"));
}

void UpdateManagerStateTest::automaticChecksAreRateLimited()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    manager->setNowForTest(now);

    // Off by default: nothing happens.
    manager->setProcessStartForTest(now.addSecs(-3600));
    QVERIFY(!manager->maybeCheckAutomatically());
    QCOMPARE(manager->state(), UpdateManager::Idle);

    manager->setAutomaticChecksEnabled(true);
    // Startup quiet period: no check in the first 30 seconds.
    manager->setProcessStartForTest(now.addSecs(-5));
    QVERIFY(!manager->maybeCheckAutomatically());
    QCOMPARE(manager->state(), UpdateManager::Idle);

    // Past the quiet period and never checked: allowed.
    manager->setProcessStartForTest(now.addSecs(-3600));
    QVERIFY(manager->maybeCheckAutomatically());
    QCOMPARE(manager->state(), UpdateManager::Checking);
    ingest(manager.get(), manifestObject(QStringLiteral("0.8.0")));
    QVERIFY(manager->lastCheckTime().isValid());

    // Within 24 h of the last check: refused.
    manager->setNowForTest(now.addSecs(23 * 3600));
    QVERIFY(!manager->maybeCheckAutomatically());

    // After 24 h: allowed again.
    manager->setNowForTest(now.addSecs(25 * 3600));
    QVERIFY(manager->maybeCheckAutomatically());
    QCOMPARE(manager->state(), UpdateManager::Checking);
}

void UpdateManagerStateTest::settingsAreApplicationWideNotAccountScoped()
{
    {
        const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
        manager->setAutomaticChecksEnabled(true);
        ingest(manager.get(), manifestObject(QStringLiteral("0.8.0")));
        manager->dismissVersion();
    }

    QSettings settings;
    settings.sync();
    const QStringList keys = settings.allKeys();
    QVERIFY(!keys.isEmpty());
    for (const QString &key : keys) {
        // Everything the update system persists lives under "update/", and
        // nothing is scoped to an account, user id, or homeserver.
        QVERIFY2(key.startsWith(QStringLiteral("update/")), qPrintable(key));
        QVERIFY2(!key.contains(QStringLiteral("accounts")), qPrintable(key));
        QVERIFY2(!key.contains(QLatin1Char('@')), qPrintable(key));
    }
    QVERIFY(keys.contains(QStringLiteral("update/automaticChecks")));
    QVERIFY(keys.contains(QStringLiteral("update/dismissedVersion")));
    QVERIFY(keys.contains(QStringLiteral("update/lastCheckTime")));

    // The stored values carry no account information either.
    QCOMPARE(settings.value(QStringLiteral("update/dismissedVersion")).toString(),
             QStringLiteral("0.8.0"));
}

void UpdateManagerStateTest::requestsCarryOnlyTheLightningVersion()
{
    const auto manager = makeManager(InstallType::LinuxDeb, QStringLiteral("0.7.0"));
    const QString agent = manager->userAgentString();
    // Version ONLY — byte for byte the string rust/src/lib.rs USER_AGENT and
    // CppHttpMatrixClient already send. No platform, architecture, build id
    // or locale token: an update check must not be more identifying than
    // ordinary Lightning traffic.
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

// ---------------------------------------------------------------------------
// Regressions found by the independent review of the update system.
// ---------------------------------------------------------------------------

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

    // A download streams into an unpredictable "*.part" name, exactly as the
    // real path does.
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
    // installUpdate() hands over at QUIT, not at the click (the helper's wait
    // for our PID is bounded). Drive that here so the argument vector exists.
    QMetaObject::invokeMethod(QCoreApplication::instance(), "aboutToQuit");
    const int artifactIndex = arguments.indexOf(QStringLiteral("--artifact"));
    QVERIFY(artifactIndex >= 0);
    const QString staged = arguments.at(artifactIndex + 1);

    // This is the whole defect: `apt-get install <path>` only treats an
    // argument as a LOCAL package when it contains '/' AND ends in ".deb";
    // dnf/dnf5 key on ".rpm" and `msiexec /i` on ".msi". A "*.part" name
    // reaches the package manager as an unknown package NAME, so the install
    // fails after the user has already answered a PolicyKit prompt.
    QCOMPARE(QFileInfo(staged).fileName(), filename);
    QVERIFY(!staged.endsWith(QStringLiteral(".part")));
    QVERIFY(QFileInfo(staged).isAbsolute());
    QVERIFY(staged.contains(QLatin1Char('/')));
    // Renamed inside the private staging directory; nothing left the root.
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
    // installUpdate() hands over at QUIT, not at the click (the helper's wait
    // for our PID is bounded). Drive that here so the argument vector exists.
    QMetaObject::invokeMethod(QCoreApplication::instance(), "aboutToQuit");

    const int targetIndex = arguments.indexOf(QStringLiteral("--target"));
    QVERIFY(targetIndex >= 0);
    // UpdaterArgs refuses a non-directory --target for windows-portable
    // (swapDirectory replaces the whole install directory), so passing the
    // executable path killed every portable update at argument parsing.
    QCOMPARE(arguments.at(targetIndex + 1), QCoreApplication::applicationDirPath());
    QVERIFY(QFileInfo(arguments.at(targetIndex + 1)).isDir());

    // Every other install type still targets the executable.
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
    // installUpdate() defers the launch to quit, so drive that here —
    // otherwise quietArguments stays empty and "does not contain --relaunch"
    // would be trivially true no matter what the code did.
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

// The helper waits for our PID for a BOUNDED time (two minutes, see
// src/updater/ProcessWaiter.h). Launching it at the click for the
// "apply when I quit" path promised something it could not keep: a user who
// kept working for twenty minutes would find it had long since timed out,
// while the UI still said the update was queued. The launch therefore happens
// at aboutToQuit, which is the only moment the wait can succeed.
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
    // And it must not promise a deadline it does not enforce.
    QVERIFY(!manager->handoffSummary().contains(QStringLiteral("minute")));

    QMetaObject::invokeMethod(QCoreApplication::instance(), "aboutToQuit");
    QCOMPARE(launches, 1);

    // Quitting again must not run the helper a second time.
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
    // RestartRequired means "handed off"; the helper is still waiting for
    // this PID and has installed nothing.
    const QString summary = manager->handoffSummary();
    QVERIFY(!summary.isEmpty());
    QCOMPARE(manager->statusDetail(), summary);
    QVERIFY(summary.contains(QStringLiteral("Nothing has been installed yet")));
    QVERIFY(!summary.contains(QStringLiteral("installed successfully")));
    QVERIFY(!summary.contains(QStringLiteral("updated")));
    // And the outcome is genuinely not known yet.
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
        // Read once and deleted: a result is never shown twice.
        QVERIFY(!QFileInfo::exists(statusPath));

        manager.clearLastUpdateResult();
        QCOMPARE(manager.lastUpdateResult(), UpdateManager::NoResult);
        QVERIFY(manager.lastUpdateMode().isEmpty());
    }

    // A later launch finds nothing to report.
    UpdateManager next;
    next.setStagingRootForTest(root.path());
    QCOMPARE(next.lastUpdateResult(), UpdateManager::NoResult);
}

void UpdateManagerStateTest::helperFailureStatusIsSurfacedWithItsError()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    // Exactly what src/updater/main.cpp writes when the package manager
    // returns non-zero. Without this being read, every post-handoff failure
    // was invisible to the user.
    QVERIFY(writeHelperStatus(root.path(), false, QStringLiteral("linux-deb"),
                              QStringLiteral("installer-exit-100")));

    UpdateManager manager;
    manager.setStagingRootForTest(root.path());
    QCOMPARE(manager.lastUpdateResult(), UpdateManager::InstallFailed);
    QCOMPARE(manager.lastUpdateError(), QStringLiteral("installer-exit-100"));
    QCOMPARE(manager.lastUpdateMode(), QStringLiteral("linux-deb"));
    QVERIFY(!QFileInfo::exists(
        QDir(root.path()).absoluteFilePath(QStringLiteral("update-status.json"))));

    // The status file carries nothing sensitive, and nothing is invented.
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
    // Never a crash, never a fabricated success, never a fabricated failure.
    QCOMPARE(manager.lastUpdateResult(), UpdateManager::NoResult);
    QVERIFY(manager.lastUpdateError().isEmpty());
    QVERIFY(manager.lastUpdateMode().isEmpty());
    QCOMPARE(manager.state(), UpdateManager::Idle);
    // Unreadable rubbish is consumed too, so it cannot be re-examined forever.
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

    // A verified-but-never-installed package is potentially a hundred
    // megabytes; it must not live in the cache forever.
    QVERIFY(!QFileInfo::exists(stale));
    QVERIFY(!QFileInfo::exists(stalePart));
    // Recent work and the single-instance lock are untouched.
    QVERIFY(QFileInfo::exists(fresh));
    QVERIFY(QFileInfo::exists(lock));

    // And nothing outside the staging root is reachable: the sweep lists one
    // directory, non-recursively, excluding symlinks.
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

    // Read-only handle: truncating it genuinely fails. Asserted, so the case
    // cannot silently stop testing anything.
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

    // Before the fix this set an internal flag and returned quietly:
    // onCompleted() then returned false without ever calling fail(), so
    // finished() was never emitted, the stall timer was already stopped, and
    // UpdateManager sat in Downloading forever holding the lock.
    QCOMPARE(calls, 1);
    QVERIFY(!ok);
    QCOMPARE(seen, TransferError::FileError);
    readOnly.close();

    // A restart that CAN truncate resets the file and reports nothing.
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

// ---------------------------------------------------------------------------
// GitHub bandwidth mirror.
//
// GitLab decides WHAT may be installed; GitHub is only a faster place to get
// the bytes that decision already named, sized and hashed. Every case below
// exists to keep that asymmetry true.
// ---------------------------------------------------------------------------

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

    // The mirror was asked FIRST, and once it answered correctly the
    // canonical address was never contacted at all.
    QCOMPARE(source.requests.size(), 1);
    QCOMPARE(source.requests.first().host(), mirrorHost());
    QCOMPARE(manager->artifactSource(), QStringLiteral("mirror"));
    QVERIFY(manager->errorMessage().isEmpty());
    QVERIFY(manager->statusDetail().isEmpty());

    // What landed is the verified payload, under the manifest's own name.
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

    // The mirror is simply not there (DNS failure / 404 / 500 — all the same
    // from here). Only the canonical address answers.
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
    // A persistently useless mirror must be visible, not silent.
    QVERIFY(manager->statusDetail().contains(QStringLiteral("mirror")));

    QCOMPARE(manager->totalBytes(), qint64(payloadBytes().size()));
    QCOMPARE(manager->downloadedBytes(), qint64(payloadBytes().size()));
    QCOMPARE(manager->downloadProgress(), 1.0);

    // Exactly one file is left: the verified artifact. The mirror attempt's
    // partial file was removed before the fallback wrote a single byte.
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

    // A compromised or corrupt mirror serving the RIGHT length and the WRONG
    // content: a pure integrity failure. Falling back preserves availability,
    // and the installed bytes are still the ones the signed manifest hashed.
    ByteSourceRecorder source;
    source.serve(mirrorArtifactUrl(version, filename), tamperedBytes());
    source.serve(artifactUrl(version, filename), payloadBytes());

    const auto manager = makeDownloadManager(root.path(), &source);
    // Every value downloadProgressChanged reported, in order.
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

    // The mirror streamed a full 4096 bytes before failing its hash, so this
    // is where double-counting would show: progress must have gone back to
    // ZERO for the fallback rather than continuing from the failed attempt.
    const qint64 size = payloadBytes().size();
    QString trace;
    for (const qint64 value : observed)
        trace += QString::number(value) + QLatin1Char(' ');
    QVERIFY2(observed.contains(size), qPrintable(trace));
    QVERIFY2(observed.lastIndexOf(qint64(0)) > observed.indexOf(size), qPrintable(trace));
    QCOMPARE(manager->downloadedBytes(), size);
    QCOMPARE(manager->totalBytes(), size);

    // The tampered bytes reached nothing: they were discarded, and the file
    // that exists is byte-for-byte the manifest's payload.
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

    ByteSourceRecorder source; // nothing is served: both addresses fail

    const auto manager = makeDownloadManager(root.path(), &source);
    QSignalSpy stateSpy(manager.get(), &UpdateManager::stateChanged);
    ingest(manager.get(), mirroredManifest(version));
    manager->downloadUpdate();
    QTRY_COMPARE(manager->state(), UpdateManager::Failed);

    // Two attempts, in that order, and NO third — the mirror is never
    // retried after the canonical address fails.
    QCOMPARE(source.requestedHosts(),
             (QStringList{ mirrorHost(), lightning::update::canonicalUpdateHost() }));
    QCoreApplication::processEvents();
    QCOMPARE(source.requests.size(), 2);
    QCOMPARE(manager->state(), UpdateManager::Failed);

    // One clean terminal failure: nothing partial on disk, the
    // single-instance lock released, and no address in the message.
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
    ingest(manager.get(), downloadableManifest(version)); // no mirror_url
    QCOMPARE(manager->state(), UpdateManager::UpdateAvailable);

    manager->downloadUpdate();
    QTRY_COMPARE(manager->state(), UpdateManager::ReadyToInstall);

    // One request, to the canonical address, and no mirror talk anywhere.
    QCOMPARE(source.requestedHosts(),
             QStringList{ lightning::update::canonicalUpdateHost() });
    QCOMPARE(manager->artifactSource(), QStringLiteral("canonical"));
    QVERIFY(manager->statusDetail().isEmpty());
    QCOMPARE(stagedFiles(root.path()), QStringList{ filename });
}

// GitHub can advertise whatever it likes: nothing it returns is read as
// metadata. The mirror body here is a plausible GitHub release-API document
// naming a much newer version; it is treated as bytes, hashed, rejected, and
// the version decision stays exactly where the signed manifest put it.
void UpdateManagerStateTest::aMirrorResponseIsNeverReadAsMetadata()
{
    if (mirrorHost().isEmpty())
        QSKIP("this build trusts no artifact mirror");

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString version = QStringLiteral("0.8.0");
    const QString filename = QStringLiteral("lightning_%1_amd64.deb").arg(version);

    // Deliberately NOT a raw string literal: moc's preprocessor ends the
    // macro argument at the first ')' inside R"(...)".
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

    // The version, the notes and the staged bytes all still come from the
    // signed manifest alone.
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

// The fallback is queued rather than run inside the failed attempt's own
// signal, so there is a window between the two attempts. Cancelling in it
// must still cancel — otherwise the canonical download would start after the
// user said stop.
void UpdateManagerStateTest::cancellingBetweenAttemptsStopsTheFallback()
{
    if (mirrorHost().isEmpty())
        QSKIP("this build trusts no artifact mirror");

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString version = QStringLiteral("0.8.0");
    const QString filename = QStringLiteral("lightning_%1_amd64.deb").arg(version);

    ByteSourceRecorder source;
    source.serve(artifactUrl(version, filename), payloadBytes()); // would succeed

    const auto manager = makeDownloadManager(root.path(), &source);
    ingest(manager.get(), mirroredManifest(version));
    manager->downloadUpdate();

    // The mirror has failed and the fallback is queued but has not run.
    QCOMPARE(source.requests.size(), 1);
    QCOMPARE(manager->state(), UpdateManager::Downloading);

    manager->cancelDownload();
    QCOMPARE(manager->state(), UpdateManager::UpdateAvailable);
    QVERIFY(manager->statusDetail().contains(QStringLiteral("cancelled")));

    QCoreApplication::processEvents();
    // The queued fallback did not fire, nothing was staged, and the lock is
    // free again.
    QCOMPARE(source.requests.size(), 1);
    QCOMPARE(manager->state(), UpdateManager::UpdateAvailable);
    QVERIFY(manager->artifactSource().isEmpty());
    QCOMPARE(stagedFiles(root.path()), QStringList{});
    QVERIFY(updateLockIsFree(root.path()));
}

// The absolute rule of the mirror design, asserted against the sources
// themselves: Lightning never talks to a GitHub API, never reads
// /releases/latest, and never derives a download address from a GitHub
// response. A mirror response can only ever be artifact bytes that are then
// hashed.
void UpdateManagerStateTest::updateSourcesNeverUseTheGitHubApi()
{
    QString sourceDir;
#ifdef LIGHTNING_UPDATE_SOURCE_DIR
    if (QFileInfo::exists(QString::fromLatin1(LIGHTNING_UPDATE_SOURCE_DIR)
                          + QStringLiteral("/UpdateEndpoints.cpp"))) {
        sourceDir = QString::fromLatin1(LIGHTNING_UPDATE_SOURCE_DIR);
    }
#endif
    // Otherwise walk up from the test binary (and the working directory):
    // both build trees live inside the source tree.
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

    // Concrete GitHub-API tokens. A comment may say the words "GitHub API";
    // what must not exist is a way to CALL one.
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

QTEST_GUILESS_MAIN(UpdateManagerStateTest)
#include "UpdateManagerStateTest.moc"
