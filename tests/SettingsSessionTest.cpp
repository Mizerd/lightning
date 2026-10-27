#include "app/SettingsManager.h"
#include "matrix/RoomUnreadRule.h"
#include "media/MediaVisibilityStore.h"
#include "storage/AppDataPaths.h"
#include "storage/SecretStore.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QMetaMethod>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QSettings>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <utility>
#include <vector>

class FakeSecretStore final : public SecretStore
{
    Q_OBJECT

public:
    explicit FakeSecretStore(QObject *parent = nullptr) : SecretStore(parent) {}

    bool isSecure() const override { return true; }
    bool isAvailable() const override { return true; }
    QString backendName() const override { return QStringLiteral("test"); }

    bool storeSecret(const QString &userId,
                     const QString &key,
                     const QString &value) override
    {
        if (m_failStore) {
            m_error = QStringLiteral("simulated store failure");
            return false;
        }
        m_values.insert(userId + QLatin1Char('/') + key, value);
        return true;
    }

    QString readSecret(const QString &userId,
                       const QString &key) const override
    {
        return m_values.value(userId + QLatin1Char('/') + key);
    }

    bool deleteSecret(const QString &userId, const QString &key) override
    {
        m_values.remove(userId + QLatin1Char('/') + key);
        return true;
    }

    bool clearAccountSecrets(const QString &userId) override
    {
        if (m_failClear) {
            m_error = QStringLiteral("simulated clear failure");
            return false;
        }
        const QString prefix = userId + QLatin1Char('/');
        for (auto it = m_values.begin(); it != m_values.end();) {
            if (it.key().startsWith(prefix))
                it = m_values.erase(it);
            else
                ++it;
        }
        return true;
    }

    QString lastError() const override { return m_error; }
    void setFailClear(bool fail) { m_failClear = fail; }
    void setFailStore(bool fail) { m_failStore = fail; }
    bool hasSecret(const QString &userId, const QString &key) const
    {
        return m_values.contains(userId + QLatin1Char('/') + key);
    }

private:
    QHash<QString, QString> m_values;
    QString m_error;
    bool m_failClear = false;
    bool m_failStore = false;
};

class SettingsSessionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void aParticipantVolumeIsWrittenOnceWhenTheSliderIsReleased();
    void writesAreGatheredIntoOneThrottledFlush();
    void aPendingWriteReachesTheDiskOnDestruction();
    void anUnchangedValueDoesNotDirtyTheSettingsFile();
    void aWindowsDeviceIdSurvivesBeingStored();
    void clearsOnlySelectedAccount();
    void clearsMetadataWhenTokenIsAlreadyMissing();
    void normalizedIdentityClearsLegacyKey();
    void reportsSecretCleanupFailureButClearsMetadata();
    void migratesInsecureSecretsGroupIntoSecureStore();
    void keepsPlaintextWhenSecureMigrationFails();
    // The insecure-to-secure migration uses the real user id (not the folded
    // QSettings group name) and moves every secret of the account.
    void migratesEverySecretOfAnAccountWhoseIdContainsASlash();
    void aKeptPlaintextGroupNeverOverwritesADifferentSecureValue();
    void secretsWrittenHereOnlyForThisInstallsOwnSignIn();
    void leavesPlaintextWhenNoSavedAccountOwnsTheGroup();
    void insecureSecretsGroupFoldingIsStillTwoCharacters();
    // Device-global keys naming rooms and Spaces are removed with the last
    // account.
    void clearingTheLastAccountSweepsTheDeviceGlobalRoomKeys();
    void validThemeIdsRoundTripAndPersist();
    void unknownStoredThemeFallsBackToSystem();
    void composerModeIsMarkdownForAFreshProfile();
    void aStoredRichModeIsMigratedToMarkdownOnce();
    void largeAlbumArtDefaultsOnAndPersists();
    void themeChangeEmitsSignal();
    void previewDefaultsAndEncryptedOff();
    void shareQualityDefaultsPersistAndSnap();
    void callQualityDefaultsToAutoAndPersists();
    void roomActivityDefaultsEnabledAndPersists();
    void wheelSpeedDefaultsToFastPersistsAndFallsBack();
    void gifPolicyDefaultsPersistAndClamp();
    void videoPrefetchDefaultsPersistAndValidate();
    void messageLayoutAndTextScalePersistAndClamp();
    void layoutPresetIsAReversibleProfile();
    void layoutPresetIsPerAccount();
    void paneWidthsAreRememberedPerLayoutPreset();
    void accessibilityAndSpacingSettingsPersistAndFallBack();
    void interfaceZoomAndRoomFilterPersistAndClamp();
    void textRenderingRoundTripsAndUnknownValuesFallBack();
    void mainAppliesTextRenderingBeforeAnyWindow();
    void spacesRailWidthDefaultsToTheOldFixedWidthAndClamps();
    void spacesRailDepthStyleDefaultsToRegionsAndClamps();
    void appearanceIsPerAccountWithGlobalFallback();
    void oneAccountsAppearanceNeverRestylesAnother();
    void switchingAccountsReAnnouncesTheRoomListFilter();
    void uiFontPersistsPerAccountAndValidates();
    void loginHomeserverPrefillIsAccountIndependent();
    void hiddenImagesPersistPerAccountAndSurviveSigningOut();
    void pickerSizeIsWhitelistedBoundedAndForgettable();
    void freshProfileDefaultsToMatrixOrg();
    void windowGeometryRoundTripsAndRefusesAnUnrestorableSize();
    // Composer buttons the user switched off.
    void hiddenComposerButtonsDefaultToNoneAndNormalize();
    // What this device discloses while reading and typing.
    void readReceiptModeDefaultsToPublicPersistsAndClamps();
    void anOlderBuildsDoNotSendModeReadsAsPrivate();
    void theReadReceiptComboOffersTwoModes();
    void typingNotificationsDefaultOnAndPersist();
    void theEncryptedPreviewLevelDefaultsToFollowingTheGeneralOne();
    void anUnrecognisedPreviewModeErrsTowardLessDisclosure();
    void anUnknownEncryptionStateTakesTheStricterLevel();
    void strictDeviceTrustDefaultsOffAndPersists();
    // Microphone noise suppression (#20): one of four keys, never clamped.
    void noiseSuppressionDefaultsToOffPersistsAndFallsBack();
    void keepMediaOnDeviceDefaultsOnAndPersists();
    void developerToolsDefaultOffAndPersist();
    void theKeepMediaCheckboxSaysWhereEncryptedMediaIsKept();
    // Every account-scoped value is re-announced on an account switch.
    void switchingAccountsReAnnouncesEveryAccountScopedAppearanceValue();
    void everyAccountScopedGetterHasItsSignalInTheAccountSwitch();
    void theSoundSectionsScopeSentenceMatchesWhereThingsActuallyLive();
    void turningOffCloseToTrayAnnouncesTheStartInTrayItDerives();
    // Live accounts Phase 0: the per-room mode readers for any saved account
    // (roomNotificationModeFor & co) answer exactly what the active accessors
    // answer, which in turn answer what the pre-refactor code did.
    void perAccountModeReadersMatchTheActiveAccessorsAndTheOldRule();
    void perAccountModeReadersReadANonActiveAccountsOwnScope();
    void perAccountModeReadersUseTheDeviceScopeForAnEmptyOrUnknownUser();
    void anInterruptedSignOutNamesItsStoreUntilTheStoreIsGone();
    void aPendingSignOutNeverNamesAStoreItCannotProve();
    void anUnreadableSettingsFileIsReported();
    void aSettingsFileThatListsNoAccountBesideAStoreIsReported();
    // R6: a store left by a sign-in that never finished is not a lost account.
    void aStoreLeftByAnUnfinishedSignInIsNotAMissingAccount();
    void aPendingSignInEndsWithItsSignInOrItsStore();
    // R6 review M1: typed as a stand-in, recorded under the server's id.
    void aPendingSignInTypedAsAStandInEndsWhenItsStoreIsRecorded();
    // R6: what a writer killed mid-write leaves behind never reads as damage.
    void aWriterKilledMidWriteLeavesTheLastGoodFile();

private:
    // Writes the QSettings shape SettingsManager::upsertAccountRecord produces
    // without a SecretStore: the migration cases need an existing account
    // record with a token only in plaintext, which saveSession() cannot make.
    static void seedAccountRecord(const QString &userId,
                                  const QString &homeserver);

    QTemporaryDir m_configHome;
};

void SettingsSessionTest::seedAccountRecord(const QString &userId,
                                            const QString &homeserver)
{
    const QString slug = matrix::app_data::safeUserSlug(userId);
    QVERIFY2(!slug.isEmpty(), "the fixture user id has no valid slug");
    QSettings seed;
    const QString base = QStringLiteral("accounts/") + slug + QLatin1Char('/');
    seed.setValue(base + QStringLiteral("userId"), userId);
    seed.setValue(base + QStringLiteral("homeserver"), homeserver);
    seed.setValue(base + QStringLiteral("deviceId"), QStringLiteral("DEVICE"));
    seed.setValue(base + QStringLiteral("addedAt"),
                  QStringLiteral("2026-09-08T00:00:00"));
    seed.sync();
}

void SettingsSessionTest::initTestCase()
{
    QVERIFY(m_configHome.isValid());
    qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
    QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
    QCoreApplication::setApplicationName(QStringLiteral("settings-session-test"));
}

void SettingsSessionTest::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
}

namespace {
QByteArray settingsFileBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
}

// A call volume is written when its slider is RELEASED, once, and never
// mid-drag: the live effect follows every step (the changed signal fires), but
// the disk is touched on release. Decision recorded 2026-10-26 (Rokas),
// replacing "every step syncs". Asserted against the file, since reading back
// through QSettings would be answered from its cache.
void SettingsSessionTest::aParticipantVolumeIsWrittenOnceWhenTheSliderIsReleased()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    const QString me = QStringLiteral("@alice:matrix.example");
    settings.saveSession(QStringLiteral("https://matrix.example"), me,
                         QStringLiteral("ALICEDEVICE"),
                         QStringLiteral("alice-token-fixture"));
    settings.flushPendingWrites();

    // Resolve the path before the write: constructing a QSettings syncs the
    // shared per-path QConfFile, so asking afterwards would flush the very
    // write under test.
    const QString path = QSettings().fileName();
    const QString other = QStringLiteral("@bob:matrix.example");
    QSignalSpy live(&settings, &SettingsManager::callParticipantVolumeChanged);

    settings.holdWritesWhileDragging(true);
    for (int step = 40; step <= 47; ++step)
        settings.setCallParticipantVolume(other, step);
    settings.setCallShareVolume(other, 150);
    // Past the throttle's delay, still held: nothing on disk mid-drag.
    QTest::qWait(SettingsManager::kFlushDelayMs + 300);
    QCOMPARE(live.count(), 8);   // every step reached the live audio
    QCOMPARE(settings.callParticipantVolume(other), 47);
    QVERIFY2(!QString::fromUtf8(settingsFileBytes(path))
                  .contains(QStringLiteral("callVolumes")),
             "a volume reached the disk in the middle of a drag");

    settings.holdWritesWhileDragging(false);   // release
    const QString onDisk = QString::fromUtf8(settingsFileBytes(path));
    QVERIFY2(onDisk.contains(QStringLiteral("=47")),
             qPrintable(QStringLiteral("volume not on disk after release; "
                                       "file said:\n%1").arg(onDisk)));
    QVERIFY(onDisk.contains(QStringLiteral("callVolumes")));
    QVERIFY(onDisk.contains(QStringLiteral("=150")));
    QVERIFY(!settings.hasUnflushedWrites());

    // A hold nobody releases (a popup destroyed mid-drag) lets go by itself.
    settings.holdWritesWhileDragging(true);
    QVERIFY(settings.writesHeld());
}

namespace {
// Lets a posted QSettings update request reach the manager's filter, then
// writes it, so the next assertion starts from a file with nothing pending.
void settle(SettingsManager &settings)
{
    QCoreApplication::processEvents();
    settings.flushPendingWrites();
    QVERIFY(!settings.hasUnflushedWrites());
}
} // namespace

// QSettings rewrites the whole file (new file, fsync, rename, lock file) on
// the event-loop turn after ANY change. A run of changes, a slider drag for
// instance, now reaches the disk as one write within kFlushDelayMs. Old code:
// the first processEvents() below rewrote the file, so the value was on disk
// at once, and every later turn rewrote it again.
void SettingsSessionTest::writesAreGatheredIntoOneThrottledFlush()
{
    // Resolved before any unsynced write: constructing a QSettings syncs the
    // shared per-path QConfFile.
    const QString path = QSettings().fileName();
    SettingsManager settings;
    settle(settings);

    settings.setRoomListWidth(301);
    QCoreApplication::processEvents();
    QVERIFY(settings.hasUnflushedWrites());
    QCOMPARE(settings.roomListWidth(), 301); // in-process view is current
    QVERIFY2(!settingsFileBytes(path).contains("roomListWidth=301"),
             "the file was rewritten on the first event-loop turn");

    for (int width = 302; width <= 320; ++width) {
        settings.setRoomListWidth(width);
        QCoreApplication::processEvents();
    }
    QVERIFY(!settingsFileBytes(path).contains("roomListWidth=3"));

    QTRY_VERIFY_WITH_TIMEOUT(settingsFileBytes(path).contains("roomListWidth=320"),
                             SettingsManager::kFlushDelayMs * 5);
    QVERIFY(!settings.hasUnflushedWrites());
}

// Nothing is lost by the throttle when the manager goes away first (quit,
// account teardown): the destructor writes what was pending.
void SettingsSessionTest::aPendingWriteReachesTheDiskOnDestruction()
{
    const QString path = QSettings().fileName();
    {
        SettingsManager settings;
        settle(settings);
        settings.setRoomListWidth(333);
        QCoreApplication::processEvents();
        QVERIFY(settings.hasUnflushedWrites());
        QVERIFY(!settingsFileBytes(path).contains("roomListWidth=333"));
    }
    QVERIFY(settingsFileBytes(path).contains("roomListWidth=333"));
}

// Values learned again and again (a window restored to the saved geometry, a
// video's size and shape on every fetch, the same draft saved on every room
// switch) are not written again: any setValue, even of the stored value,
// rewrites the whole file. Old code: each repeat below left a write pending.
void SettingsSessionTest::anUnchangedValueDoesNotDirtyTheSettingsFile()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    settings.saveSession(QStringLiteral("https://matrix.example"),
                         QStringLiteral("@alice:matrix.example"),
                         QStringLiteral("ALICEDEVICE"),
                         QStringLiteral("alice-token-fixture"));
    settle(settings);

    const auto repeatIsClean = [&](const char *what, const auto &write) {
        write();
        settle(settings); // the first write is a real change
        write();
        QCoreApplication::processEvents();
        QVERIFY2(!settings.hasUnflushedWrites(),
                 qPrintable(QStringLiteral("repeating %1 dirtied the file")
                                .arg(QLatin1String(what))));
    };

    repeatIsClean("the window geometry",
                  [&] { settings.saveWindowGeometry(40, 30, 900, 700); });
    repeatIsClean("a video's dimensions", [&] {
        settings.setKnownVideoDimensions(QStringLiteral("$video"), 640, 360);
    });
    repeatIsClean("a media size", [&] {
        settings.setKnownMediaSizeBytes(QStringLiteral("$video"), 123456);
    });
    const QVariantMap draft{{QStringLiteral("text"), QStringLiteral("hello")}};
    repeatIsClean("a draft",
                  [&] { settings.setRoomDraft(QStringLiteral("!room:x"), draft); });

    // A real change still writes, and reads back.
    settings.saveWindowGeometry(41, 30, 900, 700);
    QCoreApplication::processEvents();
    QVERIFY(settings.hasUnflushedWrites());
    settle(settings);
    settings.setKnownVideoDimensions(QStringLiteral("$video"), 1280, 720);
    QCoreApplication::processEvents();
    QVERIFY(settings.hasUnflushedWrites());
    QCOMPARE(settings.knownVideoDimensions(QStringLiteral("$video")), QSize(1280, 720));
    settle(settings);
    const QVariantMap edited{{QStringLiteral("text"), QStringLiteral("hello!")}};
    settings.setRoomDraft(QStringLiteral("!room:x"), edited);
    QCoreApplication::processEvents();
    QVERIFY(settings.hasUnflushedWrites());
    QCOMPARE(settings.roomDraft(QStringLiteral("!room:x")), edited);
    // Another entry became the most recent, so re-touching the first one is
    // an LRU move and is written.
    settle(settings);
    settings.setKnownMediaSizeBytes(QStringLiteral("$other"), 99);
    settle(settings);
    settings.setKnownMediaSizeBytes(QStringLiteral("$video"), 123456);
    QCoreApplication::processEvents();
    QVERIFY(settings.hasUnflushedWrites());
}

// A Windows device id (a device path starting with backslashes) can be
// stored. Nothing interpolates these ids into a pipeline string on Windows,
// so refusing backslashes there only made the choice silently read back as
// "System default".
void SettingsSessionTest::aWindowsDeviceIdSurvivesBeingStored()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);

    // The real shape: a Windows symbolic link name.
    const QString windowsId = QStringLiteral(
        "\\\\?\\usb#vid_322e&pid_233a&mi_00#7&1f2e3d4c&0&0000#"
        "{65e8773d-8f56-11d0-a3b9-00a0c9223196}\\global");
    settings.setPreferredCameraId(windowsId);
    QCOMPARE(settings.preferredCameraId(), windowsId);

    settings.setPreferredMicrophoneId(windowsId);
    QCOMPARE(settings.preferredMicrophoneId(), windowsId);

    // A Linux id is unaffected.
    settings.setPreferredCameraId(QStringLiteral("/dev/video0"));
    QCOMPARE(settings.preferredCameraId(), QStringLiteral("/dev/video0"));

    // Control characters and over-long values are still refused: the value
    // lives in a hand-editable config file.
    settings.setPreferredCameraId(QStringLiteral("bad\u0007id"));
    QVERIFY(settings.preferredCameraId().isEmpty());
    settings.setPreferredCameraId(QString(257, QLatin1Char('x')));
    QVERIFY(settings.preferredCameraId().isEmpty());
}

void SettingsSessionTest::clearsOnlySelectedAccount()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    settings.saveSession(QStringLiteral("https://matrix.example"),
                         QStringLiteral("@alice:matrix.example"),
                         QStringLiteral("ALICEDEVICE"),
                         QStringLiteral("alice-token-fixture"));
    secrets.storeSecret(QStringLiteral("@bob:matrix.example"),
                        QStringLiteral("accessToken"),
                        QStringLiteral("bob-token-fixture"));

    QVERIFY(settings.clearSessionForAccount(
        QStringLiteral("@bob:matrix.example")));
    QCOMPARE(settings.userId(), QStringLiteral("@alice:matrix.example"));
    QCOMPARE(settings.deviceId(), QStringLiteral("ALICEDEVICE"));
    QVERIFY(settings.hasSession());

    QVERIFY(settings.clearSessionForAccount(
        QStringLiteral("@alice:matrix.example")));
    QVERIFY(settings.userId().isEmpty());
    QVERIFY(settings.deviceId().isEmpty());
    QVERIFY(!settings.hasSession());
}

void SettingsSessionTest::clearsMetadataWhenTokenIsAlreadyMissing()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    const QString user = QStringLiteral("@alice:matrix.example");
    settings.saveSession(QStringLiteral("https://matrix.example"), user,
                         QStringLiteral("DEVICE"),
                         QStringLiteral("token-fixture"));
    QVERIFY(secrets.deleteSecret(user, QStringLiteral("accessToken")));
    QVERIFY(!settings.hasSession());

    QVERIFY(settings.clearSession());
    QVERIFY(settings.userId().isEmpty());
    QVERIFY(settings.deviceId().isEmpty());
}

void SettingsSessionTest::normalizedIdentityClearsLegacyKey()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    settings.saveSession(QStringLiteral("https://MATRIX.EXAMPLE/"),
                         QStringLiteral("@alice:Matrix.Example"),
                         QStringLiteral("DEVICE"),
                         QStringLiteral("token-fixture"));

    QVERIFY(settings.clearSessionForAccount(
        QStringLiteral("@alice:matrix.example")));
    QVERIFY(settings.userId().isEmpty());
    QVERIFY(settings.deviceId().isEmpty());
}

void SettingsSessionTest::reportsSecretCleanupFailureButClearsMetadata()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    settings.saveSession(QStringLiteral("https://matrix.example"),
                         QStringLiteral("@alice:matrix.example"),
                         QStringLiteral("DEVICE"),
                         QStringLiteral("token-fixture"));
    secrets.setFailClear(true);

    QVERIFY(!settings.clearSession());
    QVERIFY(settings.userId().isEmpty());
    QVERIFY(settings.deviceId().isEmpty());
}

void SettingsSessionTest::migratesInsecureSecretsGroupIntoSecureStore()
{
    // A plaintext token under secrets/<user>/accessToken from a previous
    // InsecureFallback run, belonging to a real saved account (the migration
    // refuses credentials it cannot attribute).
    const QString user = QStringLiteral("@alice:matrix.example");
    const QString token = QStringLiteral("plaintext-token-fixture");
    seedAccountRecord(user, QStringLiteral("https://matrix.example"));
    {
        QSettings seed;
        seed.setValue(QStringLiteral("secrets/%1/accessToken").arg(user), token);
        seed.sync();
    }

    FakeSecretStore secrets;   // isSecure() == true
    SettingsManager settings;
    settings.setSecretStore(&secrets);   // triggers migration

    // Token moved into the secure store, verifiable by identity.
    QCOMPARE(secrets.readSecret(user, QStringLiteral("accessToken")), token);
    // Plaintext removed from QSettings.
    QSettings check;
    QVERIFY(!check.contains(QStringLiteral("secrets/%1/accessToken").arg(user)));
}

void SettingsSessionTest::keepsPlaintextWhenSecureMigrationFails()
{
    const QString user = QStringLiteral("@bob:matrix.example");
    const QString token = QStringLiteral("plaintext-token-fixture-2");
    seedAccountRecord(user, QStringLiteral("https://matrix.example"));
    {
        QSettings seed;
        seed.setValue(QStringLiteral("secrets/%1/accessToken").arg(user), token);
        seed.sync();
    }

    FakeSecretStore secrets;
    secrets.setFailStore(true);   // secure write fails
    SettingsManager settings;
    settings.setSecretStore(&secrets);

    // Failure must not lose the session: plaintext stays, secure store empty.
    QVERIFY(!secrets.hasSecret(user, QStringLiteral("accessToken")));
    QSettings check;
    QCOMPARE(check.value(QStringLiteral("secrets/%1/accessToken").arg(user)).toString(),
             token);
}

// The group name is not the user id: InsecureFallbackSecretStore folds '/'
// and '\\' to '_', and Matrix localparts may contain '/' (appservice and
// bridge ids). The migration must store every secret (access token, refresh
// token, client id) under the real id, so runtime reads and
// clearAccountSecrets() find them.
void SettingsSessionTest::migratesEverySecretOfAnAccountWhoseIdContainsASlash()
{
    const QString user = QStringLiteral("@a/b:matrix.example");
    const QString group = QStringLiteral("@a_b:matrix.example");
    const QString token = QStringLiteral("slash-access-token-fixture");
    const QString refresh = QStringLiteral("slash-refresh-token-fixture");
    const QString clientId = QStringLiteral("slash-oauth-client-id-fixture");

    seedAccountRecord(user, QStringLiteral("https://matrix.example"));
    {
        QSettings seed;
        seed.setValue(QStringLiteral("secrets/%1/accessToken").arg(group),
                      token);
        seed.setValue(QStringLiteral("secrets/%1/refreshToken").arg(group),
                      refresh);
        seed.setValue(QStringLiteral("secrets/%1/oauthClientId").arg(group),
                      clientId);
        seed.sync();
    }

    FakeSecretStore secrets;   // isSecure() == true
    SettingsManager settings;
    settings.setSecretStore(&secrets);   // triggers the migration

    // All three, under the real user id.
    QCOMPARE(secrets.readSecret(user, QStringLiteral("accessToken")), token);
    QCOMPARE(secrets.readSecret(user, QStringLiteral("refreshToken")), refresh);
    QCOMPARE(secrets.readSecret(user, QStringLiteral("oauthClientId")),
             clientId);
    // Nothing under the folded id, which nothing would read or remove.
    QVERIFY2(!secrets.hasSecret(group, QStringLiteral("accessToken")),
             "the token was stored under the folded QSettings group name");

    // The runtime read finds it.
    QCOMPARE(settings.accessTokenFor(user), token);

    // All plaintext is gone.
    QSettings check;
    QVERIFY(!check.contains(
        QStringLiteral("secrets/%1/accessToken").arg(group)));
    QVERIFY(!check.contains(
        QStringLiteral("secrets/%1/refreshToken").arg(group)));
    QVERIFY(!check.contains(
        QStringLiteral("secrets/%1/oauthClientId").arg(group)));

    // Sign-out removes what the migration wrote.
    QVERIFY(settings.clearSessionForAccount(user));
    QVERIFY(!secrets.hasSecret(user, QStringLiteral("accessToken")));
    QVERIFY(!secrets.hasSecret(user, QStringLiteral("refreshToken")));
    QVERIFY(!secrets.hasSecret(user, QStringLiteral("oauthClientId")));
}

// The folding is not injective, so a group no single saved account resolves
// to cannot be attributed: leave the readable plaintext rather than moving it
// to a guess.
void SettingsSessionTest::leavesPlaintextWhenNoSavedAccountOwnsTheGroup()
{
    const QString ghost = QStringLiteral("@ghost:matrix.example");
    const QString token = QStringLiteral("orphan-token-fixture");
    {
        QSettings seed;   // deliberately NO account record
        seed.setValue(QStringLiteral("secrets/%1/accessToken").arg(ghost),
                      token);
        seed.sync();
    }

    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);

    QVERIFY2(!secrets.hasSecret(ghost, QStringLiteral("accessToken")),
             "a credential was moved to an account that is not saved here");
    QSettings check;
    QCOMPARE(check.value(QStringLiteral("secrets/%1/accessToken").arg(ghost))
                 .toString(),
             token);
}

// SettingsManager mirrors InsecureFallbackSecretStore's group-name folding
// (including that header would pull its vtable into every target). A mirror can
// drift, so this reads the real implementation and fails if it changes.
void SettingsSessionTest::insecureSecretsGroupFoldingIsStillTwoCharacters()
{
    QFile source(QStringLiteral(
        REPO_ROOT "/src/storage/InsecureFallbackSecretStore.cpp"));
    QVERIFY2(source.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(source.fileName()));
    const QString text = QString::fromUtf8(source.readAll());

    static const QRegularExpression fold(
        QStringLiteral(R"(safeUser\.replace\(QLatin1Char\('(.*?)'\))"));
    QSet<QString> substituted;
    auto it = fold.globalMatch(text);
    while (it.hasNext())
        substituted.insert(it.next().captured(1));

    QVERIFY2(!substituted.isEmpty(),
             "the folding could not be found at all — this scan is broken, "
             "not the code it guards");
    QVERIFY(substituted.contains(QStringLiteral("/")));
    QVERIFY(substituted.contains(QStringLiteral("\\\\")));
    QVERIFY2(substituted.size() == 2,
             qPrintable(QStringLiteral(
                            "InsecureFallbackSecretStore now folds %1 "
                            "characters into the group name; teach "
                            "insecureSecretsGroupName() in SettingsManager.cpp "
                            "the same set or the migration will fail to "
                            "attribute a group")
                            .arg(substituted.size())));
}

// Device-global keys that name Matrix objects (the rail arrangement and
// notifications/room-mode/<roomId>) are read fallbacks for accounts without a
// scoped value; with the last account removed they are swept.
void SettingsSessionTest::clearingTheLastAccountSweepsTheDeviceGlobalRoomKeys()
{
    const QString alice = QStringLiteral("@alice:matrix.example");
    const QString bob = QStringLiteral("@bob:matrix.example");
    const QString railKey =
        QString::fromLatin1(SettingsManager::kRailLayoutKey);
    const QString collapsedKey =
        QString::fromLatin1(SettingsManager::kChannelCollapsedKey);
    const QString roomModeKey =
        QStringLiteral("notifications/room-mode/!secret:matrix.example");
        // The on-disk key names are pinned: they already exist in users'
        // settings files.
    QCOMPARE(railKey, QStringLiteral("shell/railLayout"));
    QCOMPARE(collapsedKey, QStringLiteral("shell/channelCollapsed"));

    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    settings.saveSession(QStringLiteral("https://matrix.example"), alice,
                         QStringLiteral("ALICEDEVICE"),
                         QStringLiteral("alice-token-fixture"));
    settings.saveSession(QStringLiteral("https://matrix.example"), bob,
                         QStringLiteral("BOBDEVICE"),
                         QStringLiteral("bob-token-fixture"));
    {
        QSettings seed;   // the pre-scoping, device-global shape
        seed.setValue(railKey,
                      QStringLiteral("{\"folders\":[{\"id\":\"f1\","
                                     "\"name\":\"Work\",\"collapsed\":false,"
                                     "\"spaceIds\":[\"!space:matrix.example\"]}],"
                                     "\"order\":[\"f1\"]}"));
        seed.setValue(collapsedKey,
                      QStringLiteral("[\"!space:matrix.example\"]"));
        seed.setValue(roomModeKey, 2);
        seed.sync();
    }

    // One account left: they are still its read fallback.
    QVERIFY(settings.clearSessionForAccount(bob));
    {
        QSettings check;
        QVERIFY2(check.contains(railKey),
                 "the migration source was swept while an account still "
                 "needed it");
        QVERIFY(check.contains(collapsedKey));
        QVERIFY(check.contains(roomModeKey));
    }

    // None left: nothing to be a fallback for.
    QVERIFY(settings.clearSessionForAccount(alice));
    QVERIFY(settings.savedAccountUserIds().isEmpty());
    {
        QSettings check;
        QVERIFY2(!check.contains(railKey),
                 "Space room ids and the user's own folder names survived "
                 "the removal of every account");
        QVERIFY(!check.contains(collapsedKey));
        QVERIFY2(!check.contains(roomModeKey),
                 "a raw room id survived the removal of every account");
    }
}

void SettingsSessionTest::validThemeIdsRoundTripAndPersist()
{
    {
        SettingsManager settings;
        // Every preset id offered by the UI must round-trip.
        const SettingsManager::Theme ids[] = {
            SettingsManager::SystemTheme, SettingsManager::LightTheme,
            SettingsManager::DarkTheme,
            SettingsManager::GraphiteTheme, SettingsManager::MidnightBlueTheme,
            SettingsManager::NordTheme, SettingsManager::PurpleDuskTheme,
            SettingsManager::WarmTheme, SettingsManager::MossLightTheme,
            SettingsManager::IndigoNightTheme, SettingsManager::DeepTealTheme,
            SettingsManager::StormTheme,
        };
        for (const auto id : ids) {
            settings.setTheme(id);
            QCOMPARE(settings.theme(), id);
        }
        settings.setTheme(SettingsManager::NordTheme);
    }
    // Persistence across a fresh instance (same QSettings backing).
    SettingsManager reopened;
    QCOMPARE(reopened.theme(), SettingsManager::NordTheme);
}

void SettingsSessionTest::composerModeIsMarkdownForAFreshProfile()
{
    SettingsManager fresh;
    QCOMPARE(fresh.composerMode(), QStringLiteral("markdown"));
    // An explicit later choice of rich text sticks across a restart.
    fresh.setComposerMode(QStringLiteral("rich"));
    SettingsManager reopened;
    QCOMPARE(reopened.composerMode(), QStringLiteral("rich"));
}

void SettingsSessionTest::aStoredRichModeIsMigratedToMarkdownOnce()
{
    {
        QSettings raw;   // a profile from before the Markdown default
        raw.setValue(QStringLiteral("composer/mode"), QStringLiteral("rich"));
        raw.sync();
    }
    {
        SettingsManager migrated;
        QCOMPARE(migrated.composerMode(), QStringLiteral("markdown"));
        // The user then picks rich text on purpose.
        migrated.setComposerMode(QStringLiteral("rich"));
    }
    SettingsManager later;
    QCOMPARE(later.composerMode(), QStringLiteral("rich"));
}

void SettingsSessionTest::largeAlbumArtDefaultsOnAndPersists()
{
    {
        SettingsManager s;
        QVERIFY(s.largeAlbumArt());
        s.setLargeAlbumArt(false);
    }
    SettingsManager reopened;
    QVERIFY(!reopened.largeAlbumArt());
}

void SettingsSessionTest::unknownStoredThemeFallsBackToSystem()
{
    {
        QSettings raw;
        raw.setValue(QStringLiteral("ui/theme"), 999); // out of range
        raw.sync();
    }
    SettingsManager settings;
    QCOMPARE(settings.theme(), SettingsManager::SystemTheme);

    {
        QSettings raw;
        raw.setValue(QStringLiteral("ui/theme"), -3); // negative
        raw.sync();
    }
    SettingsManager negative;
    QCOMPARE(negative.theme(), SettingsManager::SystemTheme);
}

void SettingsSessionTest::themeChangeEmitsSignal()
{
    SettingsManager settings;
    settings.setTheme(SettingsManager::LightTheme);
    QSignalSpy spy(&settings, &SettingsManager::themeChanged);
    settings.setTheme(SettingsManager::PurpleDuskTheme);
    QCOMPARE(spy.count(), 1);
    // Setting the same value again must not re-emit.
    settings.setTheme(SettingsManager::PurpleDuskTheme);
    QCOMPARE(spy.count(), 1);
}

// The rail's depth style is clamped on read: a value from a newer build with a
// third style reads back as Regions, not as a style this build cannot draw.
void SettingsSessionTest::spacesRailDepthStyleDefaultsToRegionsAndClamps()
{
    SettingsManager settings;
    QCOMPARE(settings.spacesRailDepthStyle(),
             SettingsManager::kRailDepthRegions);

    QSettings raw;
    // A value written by a newer build.
    raw.setValue(QStringLiteral("shell/spacesRailDepthStyle"), 2);
    raw.sync();
    SettingsManager fromTheFuture;
    QCOMPARE(fromTheFuture.spacesRailDepthStyle(),
             SettingsManager::kRailDepthRegions);

    raw.setValue(QStringLiteral("shell/spacesRailDepthStyle"), -1);
    raw.sync();
    SettingsManager negative;
    QCOMPARE(negative.spacesRailDepthStyle(),
             SettingsManager::kRailDepthRegions);

    raw.remove(QStringLiteral("shell/spacesRailDepthStyle"));
    raw.sync();
    SettingsManager fresh;
    QSignalSpy spy(&fresh, &SettingsManager::spacesRailDepthStyleChanged);
    fresh.setSpacesRailDepthStyle(SettingsManager::kRailDepthClassic);
    QCOMPARE(fresh.spacesRailDepthStyle(),
             SettingsManager::kRailDepthClassic);
    QCOMPARE(spy.count(), 1);
    // An out-of-range write lands on the same fallback as a read.
    fresh.setSpacesRailDepthStyle(SettingsManager::kRailDepthClassic);
    QCOMPARE(spy.count(), 1);
    // 99 is an unknown style, so it reads as Regions (not std::clamp's 1,
    // Classic), and that is a change from Classic.
    fresh.setSpacesRailDepthStyle(99);
    QCOMPARE(fresh.spacesRailDepthStyle(),
             SettingsManager::kRailDepthRegions);
    QCOMPARE(spy.count(), 2);
}

// The rail width defaults to its former fixed 68 px, so an untouched install
// looks unchanged, and is clamped on read: the file is hand-editable and the
// width decides how far nested Spaces can be indented.
void SettingsSessionTest::spacesRailWidthDefaultsToTheOldFixedWidthAndClamps()
{
    SettingsManager settings;
    QCOMPARE(settings.spacesRailWidth(), 68);

    // Hand-edited out-of-range values, snapped on read by a fresh manager.
    QSettings raw;
    raw.setValue(QStringLiteral("shell/spacesRailWidth"), 4000);
    raw.sync();
    SettingsManager tooWide;
    QCOMPARE(tooWide.spacesRailWidth(), 260);

    raw.setValue(QStringLiteral("shell/spacesRailWidth"), 0);
    raw.sync();
    SettingsManager tooNarrow;
    QCOMPARE(tooNarrow.spacesRailWidth(), 68);

    // Round trip, from a known stored value: a change to a value already held
    // would be a no-op.
    raw.setValue(QStringLiteral("shell/spacesRailWidth"), 68);
    raw.sync();
    SettingsManager fresh;
    QSignalSpy spy(&fresh, &SettingsManager::spacesRailWidthChanged);
    fresh.setSpacesRailWidth(180);
    QCOMPARE(fresh.spacesRailWidth(), 180);
    QCOMPARE(spy.count(), 1);
    // Writing the value it already holds announces nothing.
    fresh.setSpacesRailWidth(180);
    QCOMPARE(spy.count(), 1);
    // ...nor does a value that clamps to the one already held.
    fresh.setSpacesRailWidth(300);
    QCOMPARE(fresh.spacesRailWidth(), 260);
    QCOMPARE(spy.count(), 2);
    fresh.setSpacesRailWidth(9999);
    QCOMPARE(fresh.spacesRailWidth(), 260);
    QCOMPARE(spy.count(), 2);
}

void SettingsSessionTest::shareQualityDefaultsPersistAndSnap()
{
    SettingsManager settings;

    // The defaults are what the share always sent.
    QCOMPARE(settings.shareMaxHeight(), 0); // Auto
    QCOMPARE(settings.effectiveShareMaxHeight(), 1080);
    QCOMPARE(settings.shareFps(), 0); // Auto
    QCOMPARE(settings.effectiveShareFps(), 30);

    // Snapped on read as well as write: an out-of-range value would reach a
    // GStreamer caps string.
    QSettings raw;
    raw.setValue(QStringLiteral("calls/shareMaxHeight"), 4320);
    raw.setValue(QStringLiteral("calls/shareFps"), 240);
    raw.sync();
    SettingsManager reread;
    QCOMPARE(reread.shareMaxHeight(), 2160);
    QCOMPARE(reread.shareFps(), 60);

    raw.setValue(QStringLiteral("calls/shareMaxHeight"), 1);
    raw.setValue(QStringLiteral("calls/shareFps"), -5);
    raw.sync();
    SettingsManager rereadLow;
    QCOMPARE(rereadLow.shareMaxHeight(), 720);
    QCOMPARE(rereadLow.shareFps(), 15);

    // Round trip on a fresh manager from a known value: the raw writes above
    // already left 720 in the store.
    raw.setValue(QStringLiteral("calls/shareMaxHeight"), 1080);
    raw.setValue(QStringLiteral("calls/shareFps"), 30);
    raw.sync();
    SettingsManager fresh;
    QSignalSpy spy(&fresh, &SettingsManager::shareQualityChanged);
    SettingsManager &settingsRt = fresh;
    settingsRt.setShareMaxHeight(720);
    QCOMPARE(settingsRt.shareMaxHeight(), 720);
    QCOMPARE(spy.count(), 1);
    // Writing the value it already holds announces nothing.
    settingsRt.setShareMaxHeight(720);
    QCOMPARE(spy.count(), 1);
    // ...nor does a value that snaps to the one already held.
    settingsRt.setShareMaxHeight(800);
    QCOMPARE(settingsRt.shareMaxHeight(), 720);
    QCOMPARE(spy.count(), 1);

    settingsRt.setShareFps(60);
    QCOMPARE(settingsRt.shareFps(), 60);
    QCOMPARE(spy.count(), 2);
}

void SettingsSessionTest::callQualityDefaultsToAutoAndPersists()
{
    // A clean store: the neighbouring cases leave quality values behind.
    QSettings raw;
    for (const char *key :
         { "calls/shareMaxHeight", "calls/shareFps",
           "calls/shareBitrateCapKbps", "calls/cameraResolution",
           "calls/echoCancellation", "calls/autoGainControl" })
        raw.remove(QString::fromLatin1(key));
    raw.sync();

    SettingsManager settings;
    // Every default is what the build did before these were settings.
    QCOMPARE(settings.shareMaxHeight(), 0);
    QCOMPARE(settings.effectiveShareMaxHeight(), 1080);
    QCOMPARE(settings.shareFps(), 0);
    QCOMPARE(settings.effectiveShareFps(), 30);
    QCOMPARE(settings.shareBitrateCapKbps(), 0);
    QCOMPARE(settings.cameraResolution(), 0);
    QCOMPARE(settings.effectiveCameraResolution(), 720);
    QVERIFY(!settings.echoCancellation());
    QVERIFY(settings.autoGainControl());
    // Auto is never "demanding".
    QVERIFY(!settings.shareQualityDemanding());

    QSignalSpy share(&settings, &SettingsManager::shareQualityChanged);
    QSignalSpy camera(&settings, &SettingsManager::cameraQualityChanged);
    QSignalSpy voice(&settings, &SettingsManager::voiceProcessingChanged);

    // Bitrate cap: snapped to the offered ceilings, 0 stays Auto.
    settings.setShareBitrateCapKbps(1800);
    QCOMPARE(settings.shareBitrateCapKbps(), 2000);
    settings.setShareBitrateCapKbps(99999);
    QCOMPARE(settings.shareBitrateCapKbps(), 4500);
    QCOMPARE(share.count(), 2);
    settings.setShareBitrateCapKbps(4400); // snaps to what it already holds
    QCOMPARE(share.count(), 2);
    settings.setShareBitrateCapKbps(0);
    QCOMPARE(settings.shareBitrateCapKbps(), 0);

    // Camera: an unoffered value is Auto, never "the nearest" size.
    settings.setCameraResolution(480);
    QCOMPARE(settings.cameraResolution(), 480);
    QCOMPARE(camera.count(), 1);
    settings.setCameraResolution(481);
    QCOMPARE(settings.cameraResolution(), 0);
    QCOMPARE(settings.effectiveCameraResolution(), 720);

    // Share Auto round-trips and an explicit value leaves Auto.
    settings.setShareMaxHeight(1440);
    QCOMPARE(settings.effectiveShareMaxHeight(), 1440);
    settings.setShareMaxHeight(0);
    QCOMPARE(settings.shareMaxHeight(), 0);
    settings.setShareFps(0);
    QCOMPARE(settings.effectiveShareFps(), 30);

    // The webrtcdsp switches persist and announce once.
    settings.setEchoCancellation(true);
    settings.setEchoCancellation(true);
    settings.setAutoGainControl(false);
    QCOMPARE(voice.count(), 2);
    settings.setCameraResolution(1080);
    settings.setShareBitrateCapKbps(1000);
    SettingsManager reread;
    QVERIFY(reread.echoCancellation());
    QVERIFY(!reread.autoGainControl());
    QCOMPARE(reread.cameraResolution(), 1080);
    QCOMPARE(reread.shareBitrateCapKbps(), 1000);

    // Garbage in the store (a newer build, a hand edit) falls back to Auto
    // rather than reaching a caps string.
    raw.setValue(QStringLiteral("calls/cameraResolution"), 99999);
    raw.setValue(QStringLiteral("calls/shareBitrateCapKbps"), -3);
    raw.sync();
    SettingsManager garbage;
    QCOMPARE(garbage.cameraResolution(), 0);
    QCOMPARE(garbage.shareBitrateCapKbps(), 0);

    for (const char *key :
         { "calls/shareMaxHeight", "calls/shareFps",
           "calls/shareBitrateCapKbps", "calls/cameraResolution",
           "calls/echoCancellation", "calls/autoGainControl" })
        raw.remove(QString::fromLatin1(key));
    raw.sync();
}

void SettingsSessionTest::previewDefaultsAndEncryptedOff()
{
    SettingsManager settings;
    // Both preview defaults are off: previews are fetched client-side, handing
    // the reader's IP and timing to a host the sender chose (and, in encrypted
    // rooms, revealing that a link was followed). GIF animation of received
    // media contacts no third party and stays on.
    QCOMPARE(settings.loadPreviewsInEncryptedRooms(), false);
    QCOMPARE(settings.autoLoadLinkPreviews(), false);
    QCOMPARE(settings.animateGifPreviews(), true);

    // docs/privacy.md documents this default; if it changes deliberately, the
    // document must change in the same commit.
    QFile privacyDoc(QStringLiteral(REPO_ROOT "/docs/privacy.md"));
    QVERIFY2(privacyDoc.open(QIODevice::ReadOnly | QIODevice::Text),
             "docs/privacy.md is unreadable");
    const QString privacy = QString::fromUtf8(privacyDoc.readAll());
    QVERIFY2(privacy.contains(QStringLiteral("Link previews")),
             "the privacy document no longer has a link-preview section, so "
             "this coupling is checking nothing");
    QVERIFY2(privacy.contains(QStringLiteral("off by default")),
             "docs/privacy.md no longer says link previews are off by "
             "default, but the code still defaults them off -- one of the "
             "two moved without the other");

    // Driven away from the default and back, since writing the held value
    // emits nothing.
    QSignalSpy spy(&settings,
                   &SettingsManager::loadPreviewsInEncryptedRoomsChanged);
    settings.setLoadPreviewsInEncryptedRooms(true);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(settings.loadPreviewsInEncryptedRooms(), true);
    settings.setLoadPreviewsInEncryptedRooms(false);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(settings.loadPreviewsInEncryptedRooms(), false);
}

void SettingsSessionTest::roomActivityDefaultsEnabledAndPersists()
{
    SettingsManager settings;
    QCOMPARE(settings.showRoomActivity(), true);
    QSignalSpy spy(&settings, &SettingsManager::showRoomActivityChanged);
    settings.setShowRoomActivity(false);
    QCOMPARE(spy.count(), 1);
    settings.setShowRoomActivity(false);
    QCOMPARE(spy.count(), 1);

    SettingsManager reopened;
    QCOMPARE(reopened.showRoomActivity(), false);
    reopened.setShowRoomActivity(true);
}

void SettingsSessionTest::wheelSpeedDefaultsToFastPersistsAndFallsBack()
{
    {
        SettingsManager settings;
        // Default is Fast (1).
        QCOMPARE(settings.timelineWheelSpeed(), 1);

        QSignalSpy spy(&settings, &SettingsManager::timelineWheelSpeedChanged);
        settings.setTimelineWheelSpeed(2);          // Very fast
        QCOMPARE(spy.count(), 1);
        QCOMPARE(settings.timelineWheelSpeed(), 2);
        settings.setTimelineWheelSpeed(2);          // no-op, no extra signal
        QCOMPARE(spy.count(), 1);
    }
    {
        // Persists across a fresh SettingsManager (restart).
        SettingsManager reopened;
        QCOMPARE(reopened.timelineWheelSpeed(), 2);
    }
    {
        // An out-of-range write is coerced to Fast rather than stored raw.
        SettingsManager settings;
        settings.setTimelineWheelSpeed(99);
        QCOMPARE(settings.timelineWheelSpeed(), 1);
        settings.setTimelineWheelSpeed(-5);
        QCOMPARE(settings.timelineWheelSpeed(), 1);
    }
    {
        // A corrupt persisted value reads back as Fast.
        QSettings raw;
        raw.setValue(QStringLiteral("timeline/wheelSpeed"), 7);
        raw.sync();
        SettingsManager settings;
        QCOMPARE(settings.timelineWheelSpeed(), 1);
    }
}

void SettingsSessionTest::videoPrefetchDefaultsPersistAndValidate()
{
    {
        SettingsManager s;
        // Defaults: on, short clips only (8 MB, decided 2026-10-18; it was
        // 32 MB, the threshold the app first shipped with).
        QCOMPARE(s.videoPrefetchEnabled(), true);
        QCOMPARE(s.videoPrefetchMaxMb(), 8);
        QCOMPARE(s.videoPrefetchMinMb(), 1);
        QCOMPARE(s.videoPrefetchLimitMb(), 100);

        QSignalSpy en(&s, &SettingsManager::videoPrefetchEnabledChanged);
        QSignalSpy mb(&s, &SettingsManager::videoPrefetchMaxMbChanged);
        s.setVideoPrefetchEnabled(false);
        s.setVideoPrefetchEnabled(false); // no change, no signal
        QCOMPARE(en.count(), 1);
        QCOMPARE(s.videoPrefetchEnabled(), false);
        s.setVideoPrefetchMaxMb(50);
        s.setVideoPrefetchMaxMb(50);
        QCOMPARE(mb.count(), 1);
        QCOMPARE(s.videoPrefetchMaxMb(), 50);
        // A quantity clamps.
        s.setVideoPrefetchMaxMb(4000);
        QCOMPARE(s.videoPrefetchMaxMb(), 100);
        s.setVideoPrefetchMaxMb(-5);
        QCOMPARE(s.videoPrefetchMaxMb(), 1);
        s.setVideoPrefetchMaxMb(20);
    }
    {
        SettingsManager reopened; // persists across restart
        QCOMPARE(reopened.videoPrefetchEnabled(), false);
        QCOMPARE(reopened.videoPrefetchMaxMb(), 20);
    }
    {
        // A stored value that is nonsense reads as the default, not as an
        // end of the range.
        QSettings raw;
        raw.setValue(QStringLiteral("media/videoPrefetchMaxMb"), 5000);
        raw.sync();
        SettingsManager s;
        QCOMPARE(s.videoPrefetchMaxMb(), 8);
        raw.setValue(QStringLiteral("media/videoPrefetchMaxMb"),
                     QStringLiteral("lots"));
        raw.sync();
        QCOMPARE(s.videoPrefetchMaxMb(), 8);
        raw.setValue(QStringLiteral("media/videoPrefetchMaxMb"), 0);
        raw.sync();
        QCOMPARE(s.videoPrefetchMaxMb(), 8);
    }
}

void SettingsSessionTest::gifPolicyDefaultsPersistAndClamp()
{
    {
        SettingsManager s;
        // Defaults: autoplay Always (0), safe-search PG-13 (2), recents on,
        // provider giphy.
        QCOMPARE(s.gifAutoplay(), 0);
        QCOMPARE(s.gifSafeSearch(), 2);
        QCOMPARE(s.storeRecentGifs(), true);
        QCOMPARE(s.gifPreferredProvider(), QStringLiteral("giphy"));

        QSignalSpy ap(&s, &SettingsManager::gifAutoplayChanged);
        s.setGifAutoplay(1);
        QCOMPARE(ap.count(), 1);
        QCOMPARE(s.gifAutoplay(), 1);
        s.setGifSafeSearch(0);
        QCOMPARE(s.gifSafeSearch(), 0);
        s.setStoreRecentGifs(false);
        QCOMPARE(s.storeRecentGifs(), false);
        s.setGifPreferredProvider(QStringLiteral("klipy"));
        QCOMPARE(s.gifPreferredProvider(), QStringLiteral("klipy"));
        // An unknown provider is ignored.
        s.setGifPreferredProvider(QStringLiteral("bogus"));
        QCOMPARE(s.gifPreferredProvider(), QStringLiteral("klipy"));
    }
    {
        SettingsManager reopened; // persists across restart
        QCOMPARE(reopened.gifAutoplay(), 1);
        QCOMPARE(reopened.gifSafeSearch(), 0);
        QCOMPARE(reopened.storeRecentGifs(), false);
        QCOMPARE(reopened.gifPreferredProvider(), QStringLiteral("klipy"));
    }
    {
        // Out-of-range writes clamp to safe values.
        SettingsManager s;
        s.setGifAutoplay(99);
        QCOMPARE(s.gifAutoplay(), 0);
        s.setGifSafeSearch(-3);
        QCOMPARE(s.gifSafeSearch(), 2);
    }
    {
        // Corrupt persisted values read back as safe defaults.
        QSettings raw;
        raw.setValue(QStringLiteral("gif/autoplay"), 42);
        raw.setValue(QStringLiteral("gif/safeSearch"), 9);
        raw.setValue(QStringLiteral("gif/provider"), QStringLiteral("evil"));
        raw.sync();
        SettingsManager s;
        QCOMPARE(s.gifAutoplay(), 0);
        QCOMPARE(s.gifSafeSearch(), 2);
        QCOMPARE(s.gifPreferredProvider(), QStringLiteral("giphy"));
    }
}

void SettingsSessionTest::messageLayoutAndTextScalePersistAndClamp()
{
    {
        SettingsManager settings;
        QCOMPARE(settings.messageLayout(), 0);
        QCOMPARE(settings.textScale(), 100);
        QSignalSpy layoutSpy(&settings, &SettingsManager::messageLayoutChanged);
        QSignalSpy scaleSpy(&settings, &SettingsManager::textScaleChanged);
        settings.setMessageLayout(2);
        QCOMPARE(settings.messageLayout(), 2);
        QCOMPARE(layoutSpy.count(), 1);
        // Out-of-range writes clamp to Modern instead of persisting junk.
        settings.setMessageLayout(99);
        QCOMPARE(settings.messageLayout(), 0);
        settings.setMessageLayout(1);
        settings.setTextScale(120);
        QCOMPARE(settings.textScale(), 120);
        QCOMPARE(scaleSpy.count(), 1);
        settings.setTextScale(400);
        QCOMPARE(settings.textScale(), SettingsManager::kMaxTextScale);
        settings.setTextScale(10);
        QCOMPARE(settings.textScale(), SettingsManager::kMinTextScale);
        settings.setTextScale(130);
    }
    SettingsManager reopened;
    QCOMPARE(reopened.messageLayout(), 1);
    QCOMPARE(reopened.textScale(), 130);
}

// Accessibility (high contrast, underline links, name colour readability) and
// message spacing: defaults leave the shipped look unchanged, values persist,
// and an enum that is out of range or written by a newer build falls back to
// its DEFAULT rather than clamping to an end.
void SettingsSessionTest::accessibilityAndSpacingSettingsPersistAndFallBack()
{
    // The text size range widened to 75-150.
    QCOMPARE(SettingsManager::kMinTextScale, 75);
    QCOMPARE(SettingsManager::kMaxTextScale, 150);
    {
        SettingsManager settings;
        settings.setTextScale(80);
        QCOMPARE(settings.textScale(), 80);   // was clamped to 90
        settings.setTextScale(100);

        QCOMPARE(settings.highContrast(), false);
        QCOMPARE(settings.underlineLinks(), false);
        QCOMPARE(settings.nameColorCorrection(), SettingsManager::kNameColorStrong);
        QCOMPARE(settings.messageSpacing(), SettingsManager::kSpacingCozy);

        QSignalSpy contrastSpy(&settings, &SettingsManager::highContrastChanged);
        QSignalSpy underlineSpy(&settings, &SettingsManager::underlineLinksChanged);
        QSignalSpy nameSpy(&settings, &SettingsManager::nameColorCorrectionChanged);
        QSignalSpy spacingSpy(&settings, &SettingsManager::messageSpacingChanged);
        settings.setHighContrast(true);
        settings.setHighContrast(true);   // no change, no signal
        settings.setUnderlineLinks(true);
        settings.setNameColorCorrection(SettingsManager::kNameColorOff);
        settings.setMessageSpacing(SettingsManager::kSpacingRoomy);
        QCOMPARE(contrastSpy.count(), 1);
        QCOMPARE(underlineSpy.count(), 1);
        QCOMPARE(nameSpy.count(), 1);
        QCOMPARE(spacingSpy.count(), 1);

        // Out of range writes go to the default, not the nearest end.
        settings.setNameColorCorrection(9);
        QCOMPARE(settings.nameColorCorrection(), SettingsManager::kNameColorStrong);
        settings.setMessageSpacing(-3);
        QCOMPARE(settings.messageSpacing(), SettingsManager::kSpacingCozy);
        settings.setNameColorCorrection(SettingsManager::kNameColorWeak);
        settings.setMessageSpacing(SettingsManager::kSpacingCompact);
    }
    {
        SettingsManager reopened;
        QCOMPARE(reopened.highContrast(), true);
        QCOMPARE(reopened.underlineLinks(), true);
        QCOMPARE(reopened.nameColorCorrection(), SettingsManager::kNameColorWeak);
        QCOMPARE(reopened.messageSpacing(), SettingsManager::kSpacingCompact);
    }
    // A value a newer build wrote (a fourth mode) reads back as the default.
    {
        QSettings raw;
        raw.setValue(QStringLiteral("ui/nameColorCorrection"), 3);
        raw.setValue(QStringLiteral("ui/messageSpacing"), 12);
        raw.sync();
        SettingsManager reopened;
        QCOMPARE(reopened.nameColorCorrection(), SettingsManager::kNameColorStrong);
        QCOMPARE(reopened.messageSpacing(), SettingsManager::kSpacingCozy);
    }
}

// Interface zoom (global, applied at startup via QT_SCALE_FACTOR) and the
// room-list filter chips (per-account appearance state).
void SettingsSessionTest::interfaceZoomAndRoomFilterPersistAndClamp()
{
    {
        SettingsManager settings;
        QCOMPARE(settings.interfaceZoom(), 100);
        QCOMPARE(settings.roomFilterMode(), 0);
        QSignalSpy zoomSpy(&settings, &SettingsManager::interfaceZoomChanged);
        QSignalSpy filterSpy(&settings,
                             &SettingsManager::roomFilterModeChanged);
        settings.setInterfaceZoom(125);
        QCOMPARE(settings.interfaceZoom(), 125);
        QCOMPARE(zoomSpy.count(), 1);
        settings.setInterfaceZoom(400);
        QCOMPARE(settings.interfaceZoom(),
                 SettingsManager::kMaxInterfaceZoom);
        settings.setInterfaceZoom(10);
        QCOMPARE(settings.interfaceZoom(),
                 SettingsManager::kMinInterfaceZoom);
        settings.setInterfaceZoom(110);

        settings.setRoomFilterMode(2);
        QCOMPARE(settings.roomFilterMode(), 2);
        QCOMPARE(filterSpy.count(), 1);
        // Out-of-range falls back to All instead of persisting junk.
        settings.setRoomFilterMode(9);
        QCOMPARE(settings.roomFilterMode(), 0);
        settings.setRoomFilterMode(4);   // Favourites
        QCOMPARE(settings.roomFilterMode(), 4);
        settings.setRoomFilterMode(3);
        QVERIFY(settings.showBridgeBadges());
        QVERIFY(!settings.compactRoomList());
        settings.setShowBridgeBadges(false);
        settings.setCompactRoomList(true);
    }
    SettingsManager reopened;
    QVERIFY(!reopened.showBridgeBadges());
    QVERIFY(reopened.compactRoomList());
    QCOMPARE(reopened.interfaceZoom(), 110);
    QCOMPARE(reopened.roomFilterMode(), 3);
}

void SettingsSessionTest::textRenderingRoundTripsAndUnknownValuesFallBack()
{
    QSettings().remove(QLatin1String(SettingsManager::kTextRenderingKey));
    {
        SettingsManager settings;
        QCOMPARE(settings.textRendering(), SettingsManager::kTextRenderingSmooth);
        QSignalSpy spy(&settings, &SettingsManager::textRenderingChanged);
        settings.setTextRendering(SettingsManager::kTextRenderingNative);
        QCOMPARE(settings.textRendering(), SettingsManager::kTextRenderingNative);
        QCOMPARE(spy.count(), 1);
        settings.setTextRendering(SettingsManager::kTextRenderingNative);
        QCOMPARE(spy.count(), 1);
    }
    {
        SettingsManager reopened;
        QCOMPARE(reopened.textRendering(), SettingsManager::kTextRenderingNative);
    }
    // An enum, not a quantity: a value from a newer build, or junk, is the
    // default and NOT the nearest id (std::clamp(2, 0, 1) would be Native).
    for (const QVariant &bad : { QVariant(2), QVariant(-1), QVariant(99),
                                 QVariant(QStringLiteral("native")),
                                 QVariant() }) {
        QCOMPARE(SettingsManager::textRenderingFromValue(bad),
                 SettingsManager::kTextRenderingSmooth);
        QSettings().setValue(QLatin1String(SettingsManager::kTextRenderingKey), bad);
        SettingsManager s;
        QCOMPARE(s.textRendering(), SettingsManager::kTextRenderingSmooth);
    }
    QCOMPARE(SettingsManager::textRenderingFromValue(1),
             SettingsManager::kTextRenderingNative);
    QSettings().remove(QLatin1String(SettingsManager::kTextRenderingKey));
}

// main() cannot be called from a test, so assert its source: the stored key
// must be read and handed to QQuickWindow::setTextRenderType before the
// QApplication (and therefore any window) is constructed.
void SettingsSessionTest::mainAppliesTextRenderingBeforeAnyWindow()
{
    QFile f(QStringLiteral(REPO_ROOT "/src/main.cpp"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString src = QString::fromUtf8(f.readAll());
    QVERIFY(src.contains(QStringLiteral("QQuickWindow::setTextRenderType")));
    QVERIFY(src.contains(QStringLiteral("QQuickWindow::NativeTextRendering")));
    const qsizetype call = src.indexOf(
        QStringLiteral("applyTextRenderingPreference(\n        QSettings()"));
    const qsizetype app = src.indexOf(QStringLiteral("QApplication app(argc, argv);"));
    QVERIFY2(call > 0, "main() never applies the stored preference");
    QVERIFY2(app > call, "text rendering must be applied before QApplication");

    // Findable in settings search, and the tray options too (including the
    // macOS wording).
    QFile q(QStringLiteral(REPO_ROOT "/qml/SettingsScreen.qml"));
    QVERIFY(q.open(QIODevice::ReadOnly));
    const QString qml = QString::fromUtf8(q.readAll());
    for (const char *anchor : { "textRenderingCombo", "closeToTrayCheck",
                                "startInTrayCheck" }) {
        QVERIFY2(qml.contains(QStringLiteral("anchor: \"%1\"").arg(QLatin1String(anchor))),
                 anchor);
        QVERIFY2(qml.contains(QStringLiteral("objectName: \"%1\"").arg(QLatin1String(anchor))),
                 anchor);
    }
}

// Settings > Appearance > Layout preset. Nheko compact is a profile for the
// presentation keys: its own values (Small avatars, Compact spacing, threads in
// the main view) and its own overrides, while the Lightning values are never
// written -- switching back restores them exactly. Typography and colours are
// not presentation keys.
void SettingsSessionTest::layoutPresetIsAReversibleProfile()
{
    {
        SettingsManager settings;
        QCOMPARE(settings.layoutPreset(), 0);
        settings.setMessageLayout(1);
        settings.setMessageSpacing(SettingsManager::kSpacingRoomy);
        settings.setTextScale(120);
        settings.setTheme(SettingsManager::MossLightTheme);
        QCOMPARE(settings.threadPlacement(), 0);

        QSignalSpy layoutSpy(&settings, &SettingsManager::messageLayoutChanged);
        QSignalSpy spacingSpy(&settings, &SettingsManager::messageSpacingChanged);
        QSignalSpy placementSpy(&settings,
                                &SettingsManager::threadPlacementChanged);
        QSignalSpy scaleSpy(&settings, &SettingsManager::textScaleChanged);
        settings.setLayoutPreset(1);
        QCOMPARE(settings.layoutPreset(), 1);
        QCOMPARE(settings.messageLayout(),
                 SettingsManager::kMessageLayoutSmallAvatars);
        QCOMPARE(settings.messageSpacing(), SettingsManager::kSpacingCozy);
        QCOMPARE(settings.threadPlacement(),
                 SettingsManager::kThreadPlacementMain);
        // One transaction: each moved key announced once.
        QCOMPARE(layoutSpy.count(), 1);
        QCOMPARE(spacingSpy.count(), 1);
        QCOMPARE(placementSpy.count(), 1);
        // Typography and palette untouched.
        QCOMPARE(settings.textScale(), 120);
        QCOMPARE(scaleSpy.count(), 0);
        QCOMPARE(settings.theme(), SettingsManager::MossLightTheme);
        QVERIFY(!settings.layoutPresetCustomized());
        QVERIFY(settings.presetSuppliesValue(QStringLiteral("messageSpacing")));

        // A change while the preset is selected customizes the preset only.
        settings.setMessageSpacing(SettingsManager::kSpacingCompact);
        QCOMPARE(settings.messageSpacing(), SettingsManager::kSpacingCompact);
        QVERIFY(settings.layoutPresetCustomized());
        QVERIFY(!settings.presetSuppliesValue(QStringLiteral("messageSpacing")));
        QVERIFY(settings.presetSuppliesValue(QStringLiteral("messageLayout")));

        // Back to Lightning: exactly what was there.
        settings.setLayoutPreset(0);
        QCOMPARE(settings.messageLayout(), 1);
        QCOMPARE(settings.messageSpacing(), SettingsManager::kSpacingRoomy);
        QCOMPARE(settings.threadPlacement(), 0);
        QVERIFY(!settings.layoutPresetCustomized());
        QVERIFY(!settings.presetSuppliesValue(QStringLiteral("messageLayout")));

        // The preset remembers its override, and Reset drops only those.
        settings.setLayoutPreset(1);
        QCOMPARE(settings.messageSpacing(), SettingsManager::kSpacingCompact);
        settings.resetLayoutPresetOverrides();
        QCOMPARE(settings.messageSpacing(), SettingsManager::kSpacingCozy);
        QVERIFY(!settings.layoutPresetCustomized());
        QCOMPARE(settings.textScale(), 120);
        settings.setThreadPlacement(0);
    }
    {
        // Settings sync sees and writes only the Lightning configuration:
        // the preset's Small avatars never reaches another device, and a
        // synced value lands in the Lightning layout without restyling the
        // preset that is on screen.
        SettingsManager settings;
        QCOMPARE(settings.layoutPreset(), 1);
        QCOMPARE(settings.lightningPresentationValue(
                     QStringLiteral("messageLayout")).toInt(), 1);
        QSignalSpy layoutSpy(&settings, &SettingsManager::messageLayoutChanged);
        // The block above left a threadPlacement override, so the preset is
        // customized already; the synced write must not add a messageLayout
        // override of its own.
        QVERIFY(settings.presetSuppliesValue(QStringLiteral("messageLayout")));
        QVERIFY(settings.setLightningPresentationValue(
            QStringLiteral("messageLayout"), 2));
        QCOMPARE(settings.messageLayout(),
                 SettingsManager::kMessageLayoutSmallAvatars);
        QCOMPARE(layoutSpy.count(), 0);
        QVERIFY(settings.presetSuppliesValue(QStringLiteral("messageLayout")));
        QVERIFY(!settings.setLightningPresentationValue(
            QStringLiteral("theme"), 1));
        settings.setLayoutPreset(0);
        QCOMPARE(settings.messageLayout(), 2);
        settings.setMessageLayout(1);
        settings.setLayoutPreset(1);
    }
    SettingsManager reopened;
    QCOMPARE(reopened.layoutPreset(), 1);
    QCOMPARE(reopened.threadPlacement(), 0);
    QCOMPARE(reopened.messageLayout(),
             SettingsManager::kMessageLayoutSmallAvatars);
    reopened.setLayoutPreset(0);
    QCOMPARE(reopened.messageLayout(), 1);
    QCOMPARE(reopened.messageSpacing(), SettingsManager::kSpacingRoomy);
}

// A pane width dragged under Nheko compact belongs to that preset: the
// Lightning widths come back with Lightning, a preset with no width of its own
// starts from the Lightning one, and export carries the Lightning widths.
void SettingsSessionTest::paneWidthsAreRememberedPerLayoutPreset()
{
    SettingsManager settings;
    settings.setRoomListWidth(360);
    settings.setRoomListExpandedWidth(360);
    settings.setSidePanelWidth(400);
    settings.setLayoutPreset(SettingsManager::kLayoutPresetNhekoCompact);
    QCOMPARE(settings.roomListWidth(), 360);
    QSignalSpy listSpy(&settings, &SettingsManager::roomListWidthChanged);
    settings.setRoomListWidth(240);
    QCOMPARE(settings.roomListWidth(), 240);
    settings.setRoomListExpandedWidth(240);
    settings.setRoomListWidth(64);
    QCOMPARE(settings.roomListWidth(), 64);
    QCOMPARE(settings.roomListExpandedWidth(), 240);
    QCOMPARE(listSpy.count(), 2);
    QCOMPARE(settings.lightningPresentationValue(
                 QStringLiteral("roomListWidth")).toInt(), 360);
    settings.setLayoutPreset(SettingsManager::kLayoutPresetLightning);
    QCOMPARE(settings.roomListWidth(), 360);
    QCOMPARE(settings.roomListExpandedWidth(), 360);
    QCOMPARE(settings.sidePanelWidth(), 400);
    QCOMPARE(listSpy.count(), 3);
    settings.setLayoutPreset(SettingsManager::kLayoutPresetNhekoCompact);
    QCOMPARE(settings.roomListWidth(), 64);
    QCOMPARE(settings.roomListExpandedWidth(), 240);
    QCOMPARE(settings.sidePanelWidth(), 400);
    settings.setLayoutPreset(SettingsManager::kLayoutPresetLightning);
    settle(settings);
    SettingsManager reopened;
    QCOMPARE(reopened.roomListWidth(), 360);
    QCOMPARE(reopened.roomListExpandedWidth(), 360);
    reopened.setLayoutPreset(SettingsManager::kLayoutPresetNhekoCompact);
    QCOMPARE(reopened.roomListWidth(), 64);
    QCOMPARE(reopened.roomListExpandedWidth(), 240);
}

void SettingsSessionTest::layoutPresetIsPerAccount()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    const QString alice = QStringLiteral("@alice:matrix.example");
    const QString bob = QStringLiteral("@bob:matrix.example");
    settings.saveSession(QStringLiteral("https://matrix.example"), alice,
                         QStringLiteral("ALICEDEVICE"),
                         QStringLiteral("alice-token-fixture"));
    settings.saveSession(QStringLiteral("https://matrix.example"), bob,
                         QStringLiteral("BOBDEVICE"),
                         QStringLiteral("bob-token-fixture"));
    settings.setActiveAccountUserId(alice);
    settings.setLayoutPreset(1);
    settings.setMessageSpacing(SettingsManager::kSpacingRoomy);
    QSignalSpy presetSpy(&settings, &SettingsManager::layoutPresetChanged);
    settings.setActiveAccountUserId(bob);
    QVERIFY(presetSpy.count() >= 1);
    QCOMPARE(settings.layoutPreset(), 0);
    QCOMPARE(settings.messageLayout(), 0);
    QCOMPARE(settings.messageSpacing(), SettingsManager::kSpacingCozy);
    settings.setLayoutPreset(1);
    // Alice's override is hers.
    QCOMPARE(settings.messageSpacing(), SettingsManager::kSpacingCozy);
    settings.setActiveAccountUserId(alice);
    QCOMPARE(settings.layoutPreset(), 1);
    QCOMPARE(settings.messageSpacing(), SettingsManager::kSpacingRoomy);
}

void SettingsSessionTest::appearanceIsPerAccountWithGlobalFallback()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    const QString alice = QStringLiteral("@alice:matrix.example");
    const QString bob = QStringLiteral("@bob:matrix.example");
    settings.saveSession(QStringLiteral("https://matrix.example"), alice,
                         QStringLiteral("ALICEDEVICE"),
                         QStringLiteral("alice-token-fixture"));
    settings.saveSession(QStringLiteral("https://matrix.example"), bob,
                         QStringLiteral("BOBDEVICE"),
                         QStringLiteral("bob-token-fixture"));

    settings.setActiveAccountUserId(alice);
    settings.setTheme(SettingsManager::MossLightTheme);
    settings.setMessageLayout(1);
    settings.setTextScale(110);

    // Switching accounts re-announces appearance so the UI re-reads it.
    QSignalSpy themeSpy(&settings, &SettingsManager::themeChanged);
    settings.setActiveAccountUserId(bob);
    QVERIFY(themeSpy.count() >= 1);
    // Bob has no choice of his own and keeps what he showed before Alice
    // chose (the default here): one account's choice never restyles another
    // (B004).
    QCOMPARE(settings.theme(), SettingsManager::SystemTheme);
    QCOMPARE(settings.messageLayout(), 0);
    QCOMPARE(settings.textScale(), 100);

    // Bob's own choices must not leak back into Alice's account.
    settings.setTheme(SettingsManager::DeepTealTheme);
    settings.setMessageLayout(2);
    settings.setTextScale(140);
    settings.setActiveAccountUserId(alice);
    QCOMPARE(settings.theme(), SettingsManager::MossLightTheme);
    QCOMPARE(settings.messageLayout(), 1);
    QCOMPARE(settings.textScale(), 110);
    settings.setActiveAccountUserId(bob);
    QCOMPARE(settings.theme(), SettingsManager::DeepTealTheme);
    QCOMPARE(settings.messageLayout(), 2);
    QCOMPARE(settings.textScale(), 140);
}

// B004: three accounts, none with a theme of its own, all showing the one the
// signed-out shell chose. A theme picked in the middle one restyled the other
// two, because the write also moved the global fallback they read. They keep
// what they showed; the global still follows the latest choice, so an account
// added afterwards starts from it.
void SettingsSessionTest::oneAccountsAppearanceNeverRestylesAnother()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    settings.setTheme(SettingsManager::NordTheme);   // signed out: the global

    const QString hs = QStringLiteral("https://matrix.example");
    const QString a = QStringLiteral("@bk004:matrix.example");
    const QString b = QStringLiteral("@bk005:matrix.example");
    const QString c = QStringLiteral("@bk006:matrix.example");
    settings.saveSession(hs, a, QStringLiteral("A"), QStringLiteral("a-token-fixture"));
    settings.saveSession(hs, b, QStringLiteral("B"), QStringLiteral("b-token-fixture"));
    settings.saveSession(hs, c, QStringLiteral("C"), QStringLiteral("c-token-fixture"));

    settings.setActiveAccountUserId(b);
    QCOMPARE(settings.theme(), SettingsManager::NordTheme);
    settings.setTheme(SettingsManager::MossLightTheme);
    settings.setTextScale(120);
    QCOMPARE(settings.theme(), SettingsManager::MossLightTheme);

    for (const QString &other : { a, c }) {
        settings.setActiveAccountUserId(other);
        QVERIFY2(settings.theme() == SettingsManager::NordTheme,
                 qPrintable(other + QStringLiteral(" was restyled by another account's theme")));
        QCOMPARE(settings.textScale(), 100);
    }
    settings.setActiveAccountUserId(b);
    QCOMPARE(settings.theme(), SettingsManager::MossLightTheme);
    QCOMPARE(settings.textScale(), 120);

    // A later account starts from the latest choice, and then is its own.
    const QString d = QStringLiteral("@bk007:matrix.example");
    settings.saveSession(hs, d, QStringLiteral("D"), QStringLiteral("d-token-fixture"));
    settings.setActiveAccountUserId(d);
    QCOMPARE(settings.theme(), SettingsManager::MossLightTheme);
    settings.setTheme(SettingsManager::DeepTealTheme);
    for (const QString &other : { a, c })  {
        settings.setActiveAccountUserId(other);
        QCOMPARE(settings.theme(), SettingsManager::NordTheme);
    }
    settings.setActiveAccountUserId(b);
    QCOMPARE(settings.theme(), SettingsManager::MossLightTheme);
}

// The room-list filter is re-announced on an account switch: the model
// follows it through a binding, and without the notify it keeps the previous
// account's filter while the chips show the new one (and a click on the
// matching chip is a no-op).
void SettingsSessionTest::switchingAccountsReAnnouncesTheRoomListFilter()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    const QString alice = QStringLiteral("@alice:matrix.example");
    const QString bob = QStringLiteral("@bob:matrix.example");
    settings.saveSession(QStringLiteral("https://matrix.example"), alice,
                         QStringLiteral("ALICEDEVICE"),
                         QStringLiteral("alice-token-fixture"));
    settings.saveSession(QStringLiteral("https://matrix.example"), bob,
                         QStringLiteral("BOBDEVICE"),
                         QStringLiteral("bob-token-fixture"));

    settings.setActiveAccountUserId(alice);
    settings.setRoomFilterMode(1);   // People
    settings.setActiveAccountUserId(bob);
    settings.setRoomFilterMode(0);   // All
    QCOMPARE(settings.roomFilterMode(), 0);

    // Back to Alice, whose stored value differs: the signal makes the model
    // re-read.
    QSignalSpy filterSpy(&settings, &SettingsManager::roomFilterModeChanged);
    settings.setActiveAccountUserId(alice);
    QCOMPARE(settings.roomFilterMode(), 1);
    QVERIFY2(filterSpy.count() >= 1,
             "an account switch did not re-announce roomFilterMode, so the "
             "room list keeps the previous account's filter");

    // And the other direction: Bob's stored value is 0, so a click on All only
    // works if the switch already announced it.
    filterSpy.clear();
    settings.setActiveAccountUserId(bob);
    QCOMPARE(settings.roomFilterMode(), 0);
    QVERIFY(filterSpy.count() >= 1);
}

void SettingsSessionTest::uiFontPersistsPerAccountAndValidates()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    const QString alice = QStringLiteral("@alice:matrix.example");
    const QString bob = QStringLiteral("@bob:matrix.example");
    settings.saveSession(QStringLiteral("https://matrix.example"), alice,
                         QStringLiteral("ALICEDEVICE"),
                         QStringLiteral("alice-token-fixture"));
    settings.saveSession(QStringLiteral("https://matrix.example"), bob,
                         QStringLiteral("BOBDEVICE"),
                         QStringLiteral("bob-token-fixture"));

    // Manrope is the default; the curated list carries the bundled set.
    QCOMPARE(settings.uiFont(), QStringLiteral("Manrope"));
    const QStringList choices = SettingsManager::uiFontChoices();
    QVERIFY(choices.contains(QStringLiteral("Inter")));
    QVERIFY(choices.contains(QStringLiteral("IBM Plex Sans")));
    QVERIFY(choices.contains(QStringLiteral("Source Sans 3")));
    QVERIFY(choices.contains(QStringLiteral("Plus Jakarta Sans")));

    settings.setActiveAccountUserId(alice);
    QSignalSpy fontSpy(&settings, &SettingsManager::uiFontChanged);
    settings.setUiFont(QStringLiteral("Inter"));
    QCOMPARE(settings.uiFont(), QStringLiteral("Inter"));
    QCOMPARE(fontSpy.count(), 1);

    // A family this build does not bundle persists: fonts come from the host,
    // and this class (Qt6::Core only) cannot ask whether one exists.
    // FontManager resolves and falls back without rewriting it.
    settings.setUiFont(QStringLiteral("Comic Sans MS"));
    QCOMPARE(settings.uiFont(), QStringLiteral("Comic Sans MS"));
    // A value that is not a font name never persists.
    settings.setUiFont(QStringLiteral("evil\"; color:red }"));
    QCOMPARE(settings.uiFont(), QStringLiteral("Manrope"));

    settings.setUiFont(QStringLiteral("Plus Jakarta Sans"));

    // Per-account: switching re-announces, and each account's choice
    // survives the round trip.
    settings.setActiveAccountUserId(bob);
    settings.setUiFont(QStringLiteral("IBM Plex Sans"));
    QCOMPARE(settings.uiFont(), QStringLiteral("IBM Plex Sans"));
    fontSpy.clear();
    settings.setActiveAccountUserId(alice);
    QVERIFY(fontSpy.count() >= 1);
    QCOMPARE(settings.uiFont(), QStringLiteral("Plus Jakarta Sans"));
    settings.setActiveAccountUserId(bob);
    QCOMPARE(settings.uiFont(), QStringLiteral("IBM Plex Sans"));

    // Restart: the persisted per-account value restores.
    SettingsManager reopened;
    reopened.setSecretStore(&secrets);
    QCOMPARE(reopened.uiFont(), QStringLiteral("IBM Plex Sans"));
}

// The login-screen homeserver field is account-independent: it reads and
// writes the global key, so typing in it during add-account does not revert
// to (or overwrite) the active account's server.
void SettingsSessionTest::loginHomeserverPrefillIsAccountIndependent()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);

    // No accounts yet: the prefill is the neutral default.
    QCOMPARE(settings.loginHomeserverPrefill(),
             QStringLiteral("https://matrix.org"));

    const QString alice = QStringLiteral("@alice:matrix.example");
    settings.saveSession(QStringLiteral("https://matrix.example"), alice,
                         QStringLiteral("ALICEDEVICE"),
                         QStringLiteral("alice-token-fixture"));
    settings.setActiveAccountUserId(alice);
    // The active account's own server drives homeserverUrl()...
    QCOMPARE(settings.homeserverUrl(),
             QStringLiteral("https://matrix.example"));

    // ...but the login field can point elsewhere and the value sticks.
    QSignalSpy spy(&settings,
                   &SettingsManager::loginHomeserverPrefillChanged);
    settings.setLoginHomeserverPrefill(
        QStringLiteral("https://other.example"));
    QCOMPARE(settings.loginHomeserverPrefill(),
             QStringLiteral("https://other.example"));
    QVERIFY(spy.count() >= 1);
    // The active account's stored server is untouched by the login prefill.
    QCOMPARE(settings.homeserverUrl(),
             QStringLiteral("https://matrix.example"));
}

// A fresh profile shows matrix.org for both the login prefill and
// homeserverUrl(), independent of environment variables or local key files.
void SettingsSessionTest::freshProfileDefaultsToMatrixOrg()
{
    SettingsManager settings;
    QCOMPARE(settings.homeserverUrl(), QStringLiteral("https://matrix.org"));
    QCOMPARE(settings.loginHomeserverPrefill(),
             QStringLiteral("https://matrix.org"));
    // No personal or developer server leaks in as a default.
    QVERIFY(!settings.homeserverUrl().contains(QStringLiteral("smetonis")));
    QVERIFY(!settings.loginHomeserverPrefill()
                 .contains(QStringLiteral("smetonis")));
}

// The remembered size of a resizable overlay picker, stored as a share of the
// available space (so it tracks the window) under the shared "picker" id.
void SettingsSessionTest::pickerSizeIsWhitelistedBoundedAndForgettable()
{
    SettingsManager s;

    // Never resized: 0 means "use the component's default share".
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 0);
    QCOMPARE(s.pickerHeightShare(QStringLiteral("picker")), 0);

    // A round trip. Both pickers use the same id, so this is their sync.
    s.setPickerShare(QStringLiteral("picker"), 420, 640);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 420);
    QCOMPARE(s.pickerHeightShare(QStringLiteral("picker")), 640);

    // Per-picker ids stay accepted and independent.
    s.setPickerShare(QStringLiteral("gif"), 300, 500);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("gif")), 300);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 420);
    // The reaction picker keeps its own size: a share of the window, never
    // the composer card's.
    s.setPickerShare(QStringLiteral("reaction"), 190, 400);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("reaction")), 190);
    QCOMPARE(s.pickerHeightShare(QStringLiteral("reaction")), 400);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 420);

    // The id is a whitelist, not a sanitizer: an unknown id reads 0 and writes
    // nothing, so QML cannot compose a settings key from text it controls.
    s.setPickerShare(QStringLiteral("../../secret"), 500, 500);
    s.setPickerShare(QStringLiteral("picker/../gif"), 500, 500);
    s.setPickerShare(QStringLiteral("PICKER"), 500, 500);
    s.setPickerShare(QString(), 500, 500);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("../../secret")), 0);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("PICKER")), 0);
    QCOMPARE(s.pickerWidthShare(QString()), 0);
    // ...and none of those disturbed a real entry.
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 420);

    // Out of range is forgotten (restoring the component default), never
    // clamped or stored.
    s.setPickerShare(QStringLiteral("picker"), 49, 640);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 0);
    QCOMPARE(s.pickerHeightShare(QStringLiteral("picker")), 0);
    s.setPickerShare(QStringLiteral("picker"), 420, 1001);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 0);
    // The other id is untouched.
    QCOMPARE(s.pickerWidthShare(QStringLiteral("gif")), 300);

    // Exact bounds are accepted.
    s.setPickerShare(QStringLiteral("picker"), 50, 1000);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 50);
    QCOMPARE(s.pickerHeightShare(QStringLiteral("picker")), 1000);

    // It survives a restart.
    {
        SettingsManager reloaded;
        QCOMPARE(reloaded.pickerWidthShare(QStringLiteral("picker")), 50);
        QCOMPARE(reloaded.pickerHeightShare(QStringLiteral("picker")), 1000);
    }
}

// Window geometry round-trips. An unset geometry reads back invalid, not
// (0,0,0,0), and a size below the window minimum is refused on write: Qt
// reports transient 0x0/1x1 geometry while showing, hiding to tray or
// restoring, which would overwrite the last good value.
void SettingsSessionTest::windowGeometryRoundTripsAndRefusesAnUnrestorableSize()
{
    {
        SettingsManager fresh;
        QVERIFY2(!fresh.initialWindowGeometry().isValid(),
                 "an unsaved geometry must not read back as a real rect");
        QVERIFY(!fresh.initialWindowMaximized());
    }

    {
        SettingsManager settings;
        settings.saveWindowGeometry(140, 90, 1280, 800);
        settings.saveWindowMaximized(true);
    }
    {
        // A fresh manager, because the value is captured at construction.
        SettingsManager reopened;
        QCOMPARE(reopened.initialWindowGeometry(), QRect(140, 90, 1280, 800));
        QVERIFY(reopened.initialWindowMaximized());
    }

    // Transient sizes are refused and leave the good value in place.
    {
        SettingsManager settings;
        settings.saveWindowGeometry(0, 0, 0, 0);
        settings.saveWindowGeometry(7, 7, 320, 240);   // below the minimum
    }
    {
        SettingsManager reopened;
        QCOMPARE(reopened.initialWindowGeometry(), QRect(140, 90, 1280, 800));
    }

    // A negative position (a monitor left of the primary) survives; whether it
    // is reachable is judged later against the live display layout.
    {
        SettingsManager settings;
        settings.saveWindowGeometry(-1920, -120, 900, 700);
        settings.saveWindowMaximized(false);
    }
    {
        SettingsManager reopened;
        QCOMPARE(reopened.initialWindowGeometry(), QRect(-1920, -120, 900, 700));
        QVERIFY(!reopened.initialWindowMaximized());
    }
}

// Hidden images persist per account and are not erased by signing out:
// sign-out calls resetForSession(), while clear() is the user's "Show all
// hidden images" and persists an empty list.
void SettingsSessionTest::hiddenImagesPersistPerAccountAndSurviveSigningOut()
{
    const QString key = QStringLiteral("$evt-persist:example.org");

    // With no account resolved, storage is inert by design; asserted so the
    // cases below cannot pass by persisting nothing.
    {
        SettingsManager settings;
        QVERIFY(settings.activeAccountUserId().isEmpty());
        MediaVisibilityStore store;
        store.setSettings(&settings);
        store.hide(key);
        QVERIFY(store.isHidden(key));
        QVERIFY2(settings.hiddenMediaKeys().isEmpty(),
                 "hidden images were stored with no account to scope them to");
    }

    // Give it an account the way the login flow would.
    const QString userId = QStringLiteral("@alice:example.org");
    {
        QSettings seed;
        seed.setValue(QStringLiteral("accounts/alice_example.org/userId"),
                      userId);
        seed.setValue(QStringLiteral("accounts/active"), userId);
        seed.sync();
        QCOMPARE(seed.status(), QSettings::NoError);
    }

    {
        SettingsManager settings;
        QCOMPARE(settings.activeAccountUserId(), userId);
        MediaVisibilityStore store;
        store.setSettings(&settings);
        store.hide(key);
        QVERIFY2(settings.hiddenMediaKeys().contains(key),
                 "hiding did not persist");

        // Sign-out drops it from this session but keeps it on disk.
        store.resetForSession();
        QVERIFY(!store.isHidden(key));
        QVERIFY2(settings.hiddenMediaKeys().contains(key),
                 "signing out ERASED the account's hidden images");
    }

    // The restart.
    {
        SettingsManager settings;
        MediaVisibilityStore store;
        store.setSettings(&settings);
        QVERIFY2(store.isHidden(key),
                 "the image did not stay hidden across a restart");

        // "Show all hidden images" is a real reset.
        store.clear();
        QVERIFY(!store.isHidden(key));
        QVERIFY2(settings.hiddenMediaKeys().isEmpty(),
                 "Show all hidden images left the stored list behind");
    }
}

// The stored value is the set of hidden composer buttons, so a button added
// later is shown to everyone. Pins the default and the normalisation.
void SettingsSessionTest::hiddenComposerButtonsDefaultToNoneAndNormalize()
{
    SettingsManager settings;
    QVERIFY2(settings.hiddenComposerButtons().isEmpty(),
             "a fresh profile must show every composer button");

    QSignalSpy spy(&settings, &SettingsManager::hiddenComposerButtonsChanged);
    settings.setComposerButtonShown(QStringLiteral("emoji"), false);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(settings.hiddenComposerButtons(),
             QStringList{ QStringLiteral("emoji") });

    // Idempotent: hiding what is already hidden announces nothing.
    settings.setComposerButtonShown(QStringLiteral("emoji"), false);
    QCOMPARE(spy.count(), 1);

    // Order and duplicates are normalised, so a no-op write does not fire the
    // notify every composer binding listens to.
    settings.setHiddenComposerButtons({ QStringLiteral("emoji"),
                                        QStringLiteral("emoji"),
                                        QString() });
    QCOMPARE(spy.count(), 1);
    settings.setHiddenComposerButtons({ QStringLiteral("voice"),
                                        QStringLiteral("emoji") });
    QCOMPARE(spy.count(), 2);
    QCOMPARE(settings.hiddenComposerButtons(),
             (QStringList{ QStringLiteral("emoji"), QStringLiteral("voice") }));

    // An empty key is not a button and must not enter the list.
    settings.setComposerButtonShown(QString(), false);
    QCOMPARE(spy.count(), 2);

    {
        SettingsManager reopened;
        QCOMPARE(reopened.hiddenComposerButtons(),
                 (QStringList{ QStringLiteral("emoji"),
                               QStringLiteral("voice") }));
        reopened.setComposerButtonShown(QStringLiteral("emoji"), true);
        QCOMPARE(reopened.hiddenComposerButtons(),
                 QStringList{ QStringLiteral("voice") });
    }
}


// Reading and typing privacy: settings deciding what leaves this device while
// the user is only looking at a room. Their defaults must not change what an
// existing install discloses.

void SettingsSessionTest::strictDeviceTrustDefaultsOffAndPersists()
{
    {
        SettingsManager settings;
        // Off by default: MSC4153 makes users who have not cross-signed their
        // devices unreadable.
        QVERIFY(!settings.strictDeviceTrust());

        QSignalSpy spy(&settings, &SettingsManager::strictDeviceTrustChanged);
        settings.setStrictDeviceTrust(true);
        QVERIFY(settings.strictDeviceTrust());
        QCOMPARE(spy.count(), 1);
        settings.setStrictDeviceTrust(true);
        QCOMPARE(spy.count(), 1);
    }
    // It survives a restart, which is when it applies: the SDK reads it at
    // client build and has no runtime setter.
    SettingsManager reopened;
    QVERIFY(reopened.strictDeviceTrust());
}

void SettingsSessionTest::noiseSuppressionDefaultsToOffPersistsAndFallsBack()
{
    {
        SettingsManager settings;
        // Off since 2026-10-07 (Rokas): suppression is a Labs choice, with
        // RNNoise marked as recommended in the UI.
        QCOMPARE(settings.noiseSuppressionMode(), QStringLiteral("off"));

        QSignalSpy spy(&settings, &SettingsManager::noiseSuppressionModeChanged);
        settings.setNoiseSuppressionMode(QStringLiteral("rnnoise"));
        QCOMPARE(settings.noiseSuppressionMode(), QStringLiteral("rnnoise"));
        QCOMPARE(spy.count(), 1);
        settings.setNoiseSuppressionMode(QStringLiteral("rnnoise"));
        QCOMPARE(spy.count(), 1);
        // Anything but the four keys is refused and changes nothing.
        for (const char *bad : {"RNNoise", "2", "3", "loud", "", "deepfilternet "}) {
            settings.setNoiseSuppressionMode(QString::fromLatin1(bad));
            QCOMPARE(settings.noiseSuppressionMode(), QStringLiteral("rnnoise"));
        }
        QCOMPARE(spy.count(), 1);
        settings.setNoiseSuppressionMode(QStringLiteral("off"));
        QCOMPARE(spy.count(), 2);
    }
    {
        SettingsManager reopened;
        QCOMPARE(reopened.noiseSuppressionMode(), QStringLiteral("off"));
    }
    // A stored value this build cannot read (a newer build's mode, an enum
    // index, a hand edit) reads as the default, never as the "nearest" mode:
    // 3 is DeepFilterNet's index, and a clamp would pick the heaviest engine.
    for (const QVariant &stored :
         {QVariant(QStringLiteral("krisp")), QVariant(3), QVariant(99),
          QVariant(QStringLiteral("DeepFilterNet"))}) {
        {
            QSettings raw;
            raw.setValue(QStringLiteral("calls/noiseSuppression"), stored);
            raw.sync();
        }
        SettingsManager settings;
        QCOMPARE(settings.noiseSuppressionMode(), QStringLiteral("off"));
    }
}

// The Developer tools switch reveals read-only panels; off until chosen, and
// kept across a restart so a developer is not asked again each launch.
void SettingsSessionTest::developerToolsDefaultOffAndPersist()
{
    {
        SettingsManager settings;
        QVERIFY(!settings.developerTools());
        QSignalSpy spy(&settings, &SettingsManager::developerToolsChanged);
        settings.setDeveloperTools(true);
        QVERIFY(settings.developerTools());
        QCOMPARE(spy.count(), 1);
        settings.setDeveloperTools(true);
        QCOMPARE(spy.count(), 1);
    }
    SettingsManager reopened;
    QVERIFY(reopened.developerTools());
}

void SettingsSessionTest::keepMediaOnDeviceDefaultsOnAndPersists()
{
    {
        SettingsManager settings;
        // On by default: the SDK media store has always kept unencrypted media,
        // so an existing install must not start downloading everything again.
        QVERIFY(settings.keepMediaOnDevice());

        QSignalSpy spy(&settings, &SettingsManager::keepMediaOnDeviceChanged);
        settings.setKeepMediaOnDevice(false);
        QVERIFY(!settings.keepMediaOnDevice());
        QCOMPARE(spy.count(), 1);
        settings.setKeepMediaOnDevice(false);
        QCOMPARE(spy.count(), 1);
    }
    // Rust reads the flag on every fetch, but it is only pushed from here, so
    // a choice that did not survive a restart would silently turn back on.
    SettingsManager reopened;
    QVERIFY(!reopened.keepMediaOnDevice());
}

// The checkbox's description is the only disclosure of what is kept: that
// media is kept encrypted under a key in the keyring, that encrypted-room
// media is kept only then, what is left unencrypted, and that sent
// attachments are kept whatever the setting says (review of store B: the
// copy used to promise that turning it off stopped keeping new media). All
// of it must be in the copy, next to the control it describes.
void SettingsSessionTest::theKeepMediaCheckboxSaysWhereEncryptedMediaIsKept()
{
    QFile file(QStringLiteral(REPO_ROOT "/qml/SettingsScreen.qml"));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(file.errorString()));
    const QString qml = QString::fromUtf8(file.readAll());
    const qsizetype check = qml.indexOf(
        QLatin1String("objectName: \"keepMediaOnDeviceCheck\""));
    QVERIFY2(check >= 0, "the keep-media checkbox is missing");
    QVERIFY(qml.contains(QLatin1String("app.settings.keepMediaOnDevice = checked")));
    const qsizetype note = qml.indexOf(
        QLatin1String("objectName: \"keepMediaOnDeviceNote\""), check);
    QVERIFY2(note > check, "the checkbox has no description after it");
    // The description's text, joined the way qsTr() concatenation reads.
    const qsizetype end = qml.indexOf(QLatin1String(")\n"), note);
    QVERIFY(end > note);
    QString text = qml.mid(note, end - note);
    static const QRegularExpression joins(QStringLiteral("\"\\s*\\+\\s*\""));
    text.replace(joins, QString());
    QVERIFY2(text.contains(QLatin1String("the key is kept in your system keyring")),
             qPrintable(text));
    QVERIFY2(text.contains(QLatin1String("you open in encrypted rooms is not kept")),
             qPrintable(text));
    QVERIFY2(text.contains(QLatin1String("unencrypted rooms are kept unencrypted")),
             qPrintable(text));
    QVERIFY2(text.contains(QLatin1String("Attachments you send, from any room, are kept")),
             qPrintable(text));
    QVERIFY2(text.contains(QLatin1String("whatever this setting says")),
             qPrintable(text));
    QVERIFY2(!text.contains(QLatin1String("stops keeping new media")),
             "the copy promises the setting covers sent attachments");
    QVERIFY2(text.contains(QLatin1String("removed when you sign out")),
             qPrintable(text));
    // 2026-10-01: a desktop with no keyring keeps unencrypted-room media (the
    // key on this disk), and large encrypted-room files are fetched again.
    // The copy used to say a missing keyring kept nothing.
    QVERIFY2(text.contains(QLatin1String("desktop with no system keyring")),
             qPrintable(text));
    QVERIFY2(!text.contains(QLatin1String("locked or missing, nothing is kept")),
             qPrintable(text));
    QVERIFY2(text.contains(QLatin1String("encrypted rooms are downloaded again each session")),
             qPrintable(text));
    // And what THIS session keeps sits between the box and the paragraph.
    const qsizetype state = qml.indexOf(
        QLatin1String("objectName: \"keepMediaOnDeviceState\""), check);
    QVERIFY2(state > check && state < note, "no state line beside the checkbox");
    const qsizetype bound = qml.indexOf(QLatin1String("app.mediaKeepState"), state);
    QVERIFY2(bound > state && bound < note, "the state line is not bound to the client");
    QVERIFY(qml.contains(QLatin1String("objectName: \"clearStoredMediaButton\"")));
    QVERIFY(qml.contains(QLatin1String("app.clearStoredMedia()")));
}

void SettingsSessionTest::readReceiptModeDefaultsToPublicPersistsAndClamps()
{
    {
        SettingsManager settings;
        // Public, as every previous version sent.
        QCOMPARE(settings.readReceiptMode(), 0);

        QSignalSpy spy(&settings, &SettingsManager::readReceiptModeChanged);
        settings.setReadReceiptMode(1);
        QCOMPARE(settings.readReceiptMode(), 1);
        QCOMPARE(spy.count(), 1);
        // Idempotent: setting the same value again is not a change.
        settings.setReadReceiptMode(1);
        QCOMPARE(spy.count(), 1);

        // The former "Do not send" mode is Private (decided 2026-10-18).
        settings.setReadReceiptMode(2);
        QCOMPARE(settings.readReceiptMode(), 1);
        QCOMPARE(spy.count(), 1);
        // Out of range falls back to the previous behaviour (public), so a
        // corrupt store does not silently stop receipts.
        settings.setReadReceiptMode(9);
        QCOMPARE(settings.readReceiptMode(), 0);
    }
    {
        SettingsManager settings;
        settings.setReadReceiptMode(1);
    }
    SettingsManager reopened;
    QCOMPARE(reopened.readReceiptMode(), 1);
}

// Decided 2026-10-18 (C1): "Do not send read receipts" sent m.read.private,
// identical on the wire to Private, so the two are one mode, as in Element,
// Element X and Signal. A 2 an older build stored reads as Private, never as
// Public (which would start disclosing reads the user had hidden) and never as
// a third combo entry that no longer exists. Old code returned 2.
void SettingsSessionTest::anOlderBuildsDoNotSendModeReadsAsPrivate()
{
    {
        QSettings raw;
        raw.setValue(QStringLiteral("privacy/readReceiptMode"), 2);
        raw.sync();
    }
    SettingsManager settings;
    QCOMPARE(settings.readReceiptMode(), 1);
    // Choosing Private over it is not a change anyone sees.
    QSignalSpy spy(&settings, &SettingsManager::readReceiptModeChanged);
    settings.setReadReceiptMode(1);
    QCOMPARE(spy.count(), 0);
    settings.setReadReceiptMode(0);
    QCOMPARE(settings.readReceiptMode(), 0);
    QCOMPARE(spy.count(), 1);
}

// The settings page offers exactly the two modes.
void SettingsSessionTest::theReadReceiptComboOffersTwoModes()
{
    QFile file(QStringLiteral(REPO_ROOT "/qml/SettingsScreen.qml"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString qml = QString::fromUtf8(file.readAll());
    QVERIFY(qml.contains(QStringLiteral("qsTr(\"Private — only my own devices\")")));
    QVERIFY(!qml.contains(QLatin1String("Do not send read receipts")));
}

void SettingsSessionTest::typingNotificationsDefaultOnAndPersist()
{
    {
        SettingsManager settings;
        QVERIFY(settings.sendTypingNotifications());
        QSignalSpy spy(&settings,
                       &SettingsManager::sendTypingNotificationsChanged);
        settings.setSendTypingNotifications(false);
        QVERIFY(!settings.sendTypingNotifications());
        QCOMPARE(spy.count(), 1);
        settings.setSendTypingNotifications(false);
        QCOMPARE(spy.count(), 1);
    }
    SettingsManager reopened;
    QVERIFY(!reopened.sendTypingNotifications());
}

void SettingsSessionTest::anUnrecognisedPreviewModeErrsTowardLessDisclosure()
{
    {
        SettingsManager settings;
        QCOMPARE(settings.notificationPreview(), 0);   // absent key: default
    }
    {
        // A value a newer build wrote (a fourth mode) or hand editing.
        QSettings seed;
        seed.setValue(QStringLiteral("notifications/preview"), 7);
        seed.sync();
    }
    SettingsManager settings;
    QCOMPARE(settings.notificationPreview(), 1);       // sender only, not 0
    settings.setNotificationPreview(7);
    QCOMPARE(settings.notificationPreview(), 1);       // setter agrees
    {
        QSettings seed;
        seed.setValue(QStringLiteral("notifications/preview"),
                      QStringLiteral("not-a-number"));
        seed.sync();
    }
    QCOMPARE(SettingsManager().notificationPreview(), 1);
}

void SettingsSessionTest::theEncryptedPreviewLevelDefaultsToFollowingTheGeneralOne()
{
    SettingsManager settings;
    QCOMPARE(settings.notificationPreviewEncrypted(), 3);   // follow
    settings.setNotificationPreview(0);                     // sender+message

    // Following: an encrypted room is treated like any other (the behaviour
    // before the split existed).
    QCOMPARE(settings.effectiveNotificationPreview(true, true), 0);
    QCOMPARE(settings.effectiveNotificationPreview(false, true), 0);

    // Split: encrypted rooms withhold the body, everything else does not.
    settings.setNotificationPreviewEncrypted(2);            // private
    QCOMPARE(settings.effectiveNotificationPreview(true, true), 2);
    QCOMPARE(settings.effectiveNotificationPreview(false, true), 0);
}

void SettingsSessionTest::anUnknownEncryptionStateTakesTheStricterLevel()
{
    SettingsManager settings;
    settings.setNotificationPreview(0);                     // sender+message
    settings.setNotificationPreviewEncrypted(2);            // private

    // Encryption not yet known (during hydration): the stricter of the two
    // levels wins, so a body is never shown that an encrypted room withholds.
    QCOMPARE(settings.effectiveNotificationPreview(false, false), 2);
    QCOMPARE(settings.effectiveNotificationPreview(true, false), 2);

    // When the general level is stricter, unknown does not relax to the
    // encrypted level.
    settings.setNotificationPreview(2);
    settings.setNotificationPreviewEncrypted(0);
    QCOMPARE(settings.effectiveNotificationPreview(false, false), 2);
    // Known-encrypted still means exactly what the user asked for.
    QCOMPARE(settings.effectiveNotificationPreview(true, true), 0);
}

// Every account-scoped appearance value (resolved through appearanceValue())
// is re-announced on an account switch, or consumers keep the previous
// account's value; e.g. reducedMotion feeds AppTheme through a one-way
// Binding.
void SettingsSessionTest::switchingAccountsReAnnouncesEveryAccountScopedAppearanceValue()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    const QString alice = QStringLiteral("@alice:matrix.example");
    const QString bob = QStringLiteral("@bob:matrix.example");
    settings.saveSession(QStringLiteral("https://matrix.example"), alice,
                         QStringLiteral("ALICEDEVICE"),
                         QStringLiteral("alice-token-fixture"));
    settings.saveSession(QStringLiteral("https://matrix.example"), bob,
                         QStringLiteral("BOBDEVICE"),
                         QStringLiteral("bob-token-fixture"));

    // Alice's answers, all different from the defaults Bob will resolve.
    settings.setActiveAccountUserId(alice);
    settings.setReducedMotion(true);
    settings.setSmoothScrolling(false);
    settings.setHiddenComposerButtons(QStringList{ QStringLiteral("gif") });
    settings.setClockFormat(2);
    settings.setMicrophoneGain(140);

    // Bob's, so switching back to Alice moves every value.
    settings.setActiveAccountUserId(bob);
    settings.setReducedMotion(false);
    settings.setSmoothScrolling(true);
    settings.setHiddenComposerButtons(QStringList{});
    settings.setClockFormat(1);
    settings.setMicrophoneGain(60);

    QSignalSpy motion(&settings, &SettingsManager::reducedMotionChanged);
    QSignalSpy scrolling(&settings, &SettingsManager::smoothScrollingChanged);
    QSignalSpy composer(&settings,
                        &SettingsManager::hiddenComposerButtonsChanged);
    QSignalSpy clock(&settings, &SettingsManager::clockFormatChanged);
    QSignalSpy gain(&settings, &SettingsManager::microphoneGainChanged);

    settings.setActiveAccountUserId(alice);

    // The values really moved, or the notify assertions assert nothing.
    QCOMPARE(settings.reducedMotion(), true);
    QCOMPARE(settings.smoothScrolling(), false);
    QCOMPARE(settings.hiddenComposerButtons(),
             QStringList{ QStringLiteral("gif") });
    QCOMPARE(settings.clockFormat(), 2);
    QCOMPARE(settings.microphoneGain(), 140);

    QVERIFY2(motion.count() >= 1,
             "an account switch did not re-announce reducedMotion, so "
             "AppTheme keeps the previous account's accessibility choice");
    QVERIFY2(scrolling.count() >= 1,
             "an account switch did not re-announce smoothScrolling, so the "
             "wheel handler keeps the previous account's choice");
    QVERIFY2(composer.count() >= 1,
             "an account switch did not re-announce hiddenComposerButtons, so "
             "the composer keeps the previous account's button set");
    QVERIFY2(clock.count() >= 1,
             "an account switch did not re-announce clockFormat, so every "
             "timestamp keeps the previous account's clock");
    QVERIFY2(gain.count() >= 1,
             "an account switch did not re-announce microphoneGain, so a live "
             "call keeps the previous account's gain");
}

// Derived rather than hand-listed: every getter that calls appearanceValue()
// resolves per account, and setActiveAccountUserId() must emit its
// `<getter>Changed`.
void SettingsSessionTest::everyAccountScopedGetterHasItsSignalInTheAccountSwitch()
{
    QFile file(QStringLiteral(REPO_ROOT "/src/app/SettingsManager.cpp"));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(file.errorString()));
    const QStringList lines =
        QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));

    // The body of setActiveAccountUserId, to scope the search.
    QString switchBody;
    bool inSwitch = false;
    int depth = 0;
    QStringList scopedGetters;
    QString currentGetter;
    // `QVariant SettingsManager::appearanceValue(` must not count itself.
    static const QRegularExpression definition(
        QStringLiteral("^[A-Za-z_].*\\bSettingsManager::([A-Za-z_][A-Za-z0-9_]*)\\("));
    for (const QString &line : lines) {
        const auto match = definition.match(line);
        if (match.hasMatch())
            currentGetter = match.captured(1);
        if (line.contains(QLatin1String("SettingsManager::setActiveAccountUserId(")))
            inSwitch = true;
        if (inSwitch) {
            switchBody += line + QLatin1Char('\n');
            depth += line.count(QLatin1Char('{')) - line.count(QLatin1Char('}'));
            if (depth == 0 && switchBody.contains(QLatin1Char('}')))
                inSwitch = false;
        }
        // A comment mentioning appearanceValue() is not a call site (the
        // switch's own comment names it).
        const QString code = line.trimmed();
        if (code.startsWith(QLatin1String("//")))
            continue;
        if (code.contains(QLatin1String("appearanceValue("))
            && !currentGetter.startsWith(QLatin1String("appearanceValue"))
            && !currentGetter.startsWith(QLatin1String("setAppearanceValue"))
            && !scopedGetters.contains(currentGetter)) {
            scopedGetters.append(currentGetter);
        }
    }

    // The scan must have found something.
    QVERIFY2(scopedGetters.size() >= 10,
             qPrintable(QStringLiteral("the appearanceValue() scan found only "
                                       "%1 account-scoped getters — the scan "
                                       "itself is broken")
                            .arg(scopedGetters.size())));
    QVERIFY(!switchBody.isEmpty());

    QStringList missing;
    for (const QString &getter : std::as_const(scopedGetters)) {
        const QString emitLine =
            QStringLiteral("Q_EMIT %1Changed();").arg(getter);
        if (!switchBody.contains(emitLine))
            missing.append(getter);
    }
    QVERIFY2(missing.isEmpty(),
             qPrintable(QStringLiteral(
                            "these getters resolve per account through "
                            "appearanceValue() but setActiveAccountUserId() "
                            "never announces them, so a switch changes their "
                            "value and tells nobody: %1")
                            .arg(missing.join(QStringLiteral(", ")))));
}

// startInTray() is `closeToTray() && stored`, so setCloseToTray must also
// announce startInTrayChanged.
void SettingsSessionTest::turningOffCloseToTrayAnnouncesTheStartInTrayItDerives()
{
    SettingsManager settings;
    settings.setCloseToTray(true);
    settings.setStartInTray(true);
    QCOMPARE(settings.startInTray(), true);

    QSignalSpy startSpy(&settings, &SettingsManager::startInTrayChanged);
    settings.setCloseToTray(false);
    QCOMPARE(settings.startInTray(), false);
    QVERIFY2(startSpy.count() >= 1,
             "turning close-to-tray off silently changed startInTray, so its "
             "checkbox still draws ticked for a setting that reads false");

    // Back on: the stored preference returns and is announced.
    startSpy.clear();
    settings.setCloseToTray(true);
    QCOMPARE(settings.startInTray(), true);
    QVERIFY(startSpy.count() >= 1);
}

// The sound card's explainer about where settings live matches storage: the
// microphone level is account-scoped and the devices belong to this
// computer. Checked from both ends and against the text.
void SettingsSessionTest::theSoundSectionsScopeSentenceMatchesWhereThingsActuallyLive()
{
    QFile cpp(QStringLiteral(REPO_ROOT "/src/app/SettingsManager.cpp"));
    QVERIFY2(cpp.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(cpp.errorString()));
    const QString src = QString::fromUtf8(cpp.readAll());

    auto body = [&src](const QString &signature) {
        const int at = src.indexOf(signature);
        if (at < 0)
            return QString();
        const int end = src.indexOf(QStringLiteral("\n}\n"), at);
        return end > at ? src.mid(at, end - at) : QString();
    };

    // 1. The level is account-scoped (`setAppearanceValue`: account key plus
    //    global fallback), not a bare `m_store->setValue`.
    const QString gain =
        body(QStringLiteral("void SettingsManager::setMicrophoneGain("));
    QVERIFY2(!gain.isEmpty(), "setMicrophoneGain is gone or was renamed");
    QVERIFY2(gain.contains(QStringLiteral("setAppearanceValue")),
             "the microphone level is no longer account-scoped, so the Sound "
             "card's sentence now says the wrong thing about it");

    // 2. The device is not account-scoped.
    const QString device =
        body(QStringLiteral("void SettingsManager::setPreferredMicrophoneId("));
    QVERIFY2(!device.isEmpty(),
             "setPreferredMicrophoneId is gone or was renamed");
    QVERIFY2(!device.contains(QStringLiteral("setAppearanceValue")),
             "the microphone DEVICE became account-scoped, so the Sound "
             "card's sentence now says the wrong thing about it");

    // 3. The text still says both.
    QFile qml(QStringLiteral(REPO_ROOT "/qml/CallDeviceSettings.qml"));
    QVERIFY2(qml.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(qml.errorString()));
    const QString ui = QString::fromUtf8(qml.readAll());
    QVERIFY2(ui.contains(QStringLiteral("belong to this computer")),
             "the Sound card no longer tells the user the devices are "
             "per-computer");
    QVERIFY2(ui.contains(QStringLiteral("level belongs to your account")),
             "the Sound card no longer tells the user the microphone level "
             "is per-account, which is the half it used to get wrong");
}

// A plaintext group with a key this build does not know is kept, and offered
// to the migration again at every start (and at every keyring retry). Its
// refresh token is the one copied at the first start; the secure store's has
// been rotated since. Copying the stale one back makes the next refresh
// present a used token, and the server ends the session. A group that goes
// after the pass was written after anything the secure store holds, and
// still wins.
void SettingsSessionTest::aKeptPlaintextGroupNeverOverwritesADifferentSecureValue()
{
    const QString kept = QStringLiteral("@kate:matrix.example");
    const QString moved = QStringLiteral("@mo:matrix.example");
    seedAccountRecord(kept, QStringLiteral("https://matrix.example"));
    seedAccountRecord(moved, QStringLiteral("https://matrix.example"));
    {
        QSettings seed;
        seed.setValue(QStringLiteral("secrets/%1/accessToken").arg(kept),
                      QStringLiteral("kept-access-fixture"));
        seed.setValue(QStringLiteral("secrets/%1/refreshToken").arg(kept),
                      QStringLiteral("stale-refresh-fixture"));
        seed.setValue(QStringLiteral("secrets/%1/aKeyFromANewerBuild").arg(kept),
                      QStringLiteral("unknown-fixture"));
        seed.setValue(QStringLiteral("secrets/%1/accessToken").arg(moved),
                      QStringLiteral("newer-access-fixture"));
        seed.sync();
    }

    FakeSecretStore secrets;
    QVERIFY(secrets.storeSecret(kept, QStringLiteral("accessToken"),
                                QStringLiteral("kept-access-fixture")));
    QVERIFY(secrets.storeSecret(kept, QStringLiteral("refreshToken"),
                                QStringLiteral("rotated-refresh-fixture")));
    QVERIFY(secrets.storeSecret(moved, QStringLiteral("accessToken"),
                                QStringLiteral("older-access-fixture")));
    SettingsManager settings;
    settings.setSecretStore(&secrets);   // the migration

    QCOMPARE(secrets.readSecret(kept, QStringLiteral("refreshToken")),
             QStringLiteral("rotated-refresh-fixture"));
    QCOMPARE(secrets.readSecret(kept, QStringLiteral("accessToken")),
             QStringLiteral("kept-access-fixture"));
    QSettings check;
    QVERIFY(check.contains(
        QStringLiteral("secrets/%1/aKeyFromANewerBuild").arg(kept)));
    // The group that goes: its value wins, and the plaintext is gone.
    QCOMPARE(secrets.readSecret(moved, QStringLiteral("accessToken")),
             QStringLiteral("newer-access-fixture"));
    QVERIFY(!check.contains(QStringLiteral("secrets/%1/accessToken").arg(moved)));
}

// The evidence that a secret store which answers "no such item" LOST a sign-in
// rather than that the account signed out: this install saved it itself, for
// the device its record names now. Settings only; never a secret read.
void SettingsSessionTest::secretsWrittenHereOnlyForThisInstallsOwnSignIn()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    const QString own = QStringLiteral("@own:matrix.example");
    settings.saveSession(QStringLiteral("https://matrix.example"), own,
                         QStringLiteral("OWNDEVICE"), QStringLiteral("token-fixture"));
    QVERIFY(settings.secretsWrittenHereForRecordedDevice(own));
    // Removing the secrets leaves the evidence: that is the point.
    QVERIFY(secrets.clearAccountSecrets(own));
    QVERIFY(settings.secretsWrittenHereForRecordedDevice(own));

    const QString slug = matrix::app_data::safeUserSlug(own);
    QSettings raw;
    // An older build signed in again: a new device, the marks still name the
    // old one.
    raw.setValue(QStringLiteral("accounts/%1/deviceId").arg(slug),
                 QStringLiteral("NEWERDEVICE"));
    raw.sync();
    QVERIFY(!settings.secretsWrittenHereForRecordedDevice(own));
    raw.setValue(QStringLiteral("accounts/%1/deviceId").arg(slug),
                 QStringLiteral("OWNDEVICE"));
    // Copied from before install scoping: may be another install's device.
    raw.setValue(QStringLiteral("accounts/%1/keyringItems").arg(slug),
                 QStringLiteral("adopted-unverified"));
    raw.sync();
    QVERIFY(!settings.secretsWrittenHereForRecordedDevice(own));

    // A record from before the marks, and no record at all.
    const QString older = QStringLiteral("@older:matrix.example");
    seedAccountRecord(older, QStringLiteral("https://matrix.example"));
    QVERIFY(!settings.secretsWrittenHereForRecordedDevice(older));
    QVERIFY(!settings.secretsWrittenHereForRecordedDevice(
        QStringLiteral("@nobody:matrix.example")));
}

namespace {

// Everything the per-room mode readers say about one room in one scope.
struct ModeAnswers {
    int mode = 0;
    bool unsynced = false;
    int base = -1;
    bool deviceOnly = false;
    bool stored = false;
    int unread = 0;
    bool decidesLocally = false;
    bool operator==(const ModeAnswers &) const = default;
    QString toString() const
    {
        return QStringLiteral("mode=%1 unsynced=%2 base=%3 deviceOnly=%4 "
                              "stored=%5 unread=%6 decidesLocally=%7")
            .arg(mode)
            .arg(unsynced)
            .arg(base)
            .arg(deviceOnly)
            .arg(stored)
            .arg(unread)
            .arg(decidesLocally);
    }
};

// A VERBATIM copy of the readers as they were before the Phase 0 refactor
// (SettingsManager.cpp at 9177fc58), over a raw QSettings and an explicit
// account slug (empty = the signed-out device scope). It is the golden half
// of the parity tests: the refactor moved these bodies behind a slug
// parameter, so a parity test comparing only the new active accessor with
// the new *For twin would pass on a broken shared core. This copy does not
// share it. decidesLocally is AppController::roomNotificationModeDecidesLocally.
ModeAnswers oldRuleAnswers(QSettings &s, const QString &slug,
                           const QString &roomId)
{
    const QString prefix = slug.isEmpty()
        ? QString()
        : QStringLiteral("accounts/") + slug + QLatin1Char('/');
    const QString globalKey = QStringLiteral("notifications/room-mode/") + roomId;
    const QString scopedKey = slug.isEmpty() ? QString() : prefix + globalKey;
    const QString unsyncedKey =
        prefix + QStringLiteral("notifications/room-mode-unsynced/") + roomId;

    ModeAnswers a;
    // roomNotificationModeUnsynced
    a.unsynced = roomId.isEmpty() ? false : s.contains(unsyncedKey);
    // roomNotificationModeUnsyncedBase
    if (roomId.isEmpty()) {
        a.base = -1;
    } else {
        bool ok = false;
        const int base = s.value(unsyncedKey).toInt(&ok);
        a.base = (ok && base >= 0 && base <= 3) ? base : -1;
    }
    // roomNotificationModeDeviceOnly
    if (roomId.isEmpty()) {
        a.deviceOnly = false;
    } else if (!scopedKey.isEmpty() && s.contains(scopedKey)) {
        a.deviceOnly = false;
    } else {
        const int legacy = s.value(globalKey, 0).toInt();
        a.deviceOnly = legacy == 1 || legacy == 2;
    }
    // hasStoredRoomNotificationMode
    a.stored = roomId.isEmpty()
        ? false
        : ((!scopedKey.isEmpty() && s.contains(scopedKey))
           || s.contains(globalKey));
    // roomNotificationMode
    if (roomId.isEmpty()) {
        a.mode = 0;
    } else {
        const QString readKey = (!scopedKey.isEmpty() && s.contains(scopedKey))
            ? scopedKey
            : globalKey;
        const int mode = s.value(readKey, 0).toInt();
        a.mode = (mode < 0 || mode > 3) ? 0 : mode;
    }
    // roomUnreadMode
    a.unread = (a.mode == roomunread::kModeMentions
                && (a.deviceOnly || a.unsynced))
        ? roomunread::kModeMentionsUnverified
        : a.mode;
    a.decidesLocally = a.unsynced || a.deviceOnly;
    return a;
}

// The active accessors; decidesLocally is AppController's rule over them.
ModeAnswers activeAnswers(const SettingsManager &s, const QString &roomId)
{
    ModeAnswers a;
    a.mode = s.roomNotificationMode(roomId);
    a.unsynced = s.roomNotificationModeUnsynced(roomId);
    a.base = s.roomNotificationModeUnsyncedBase(roomId);
    a.deviceOnly = s.roomNotificationModeDeviceOnly(roomId);
    a.stored = s.hasStoredRoomNotificationMode(roomId);
    a.unread = s.roomUnreadMode(roomId);
    a.decidesLocally = s.roomNotificationModeUnsynced(roomId)
        || s.roomNotificationModeDeviceOnly(roomId);
    return a;
}

ModeAnswers forAnswers(const SettingsManager &s, const QString &userId,
                       const QString &roomId)
{
    ModeAnswers a;
    a.mode = s.roomNotificationModeFor(userId, roomId);
    a.unsynced = s.roomNotificationModeUnsyncedFor(userId, roomId);
    a.base = s.roomNotificationModeUnsyncedBaseFor(userId, roomId);
    a.deviceOnly = s.roomNotificationModeDeviceOnlyFor(userId, roomId);
    a.stored = s.hasStoredRoomNotificationModeFor(userId, roomId);
    a.unread = s.roomUnreadModeFor(userId, roomId);
    a.decidesLocally = s.roomNotificationModeDecidesLocallyFor(userId, roomId);
    return a;
}

// Every signal SettingsManager (and QObject) declares, so "never emits" is
// asserted for all of them rather than for the few a reader might think of.
class EverySignalSpy
{
public:
    explicit EverySignalSpy(QObject *object)
    {
        const QMetaObject *meta = object->metaObject();
        for (int i = 0; i < meta->methodCount(); ++i) {
            const QMetaMethod method = meta->method(i);
            if (method.methodType() != QMetaMethod::Signal)
                continue;
            m_names.append(QString::fromLatin1(method.methodSignature()));
            m_spies.push_back(std::make_unique<QSignalSpy>(object, method));
        }
    }
    int watched() const { return int(m_spies.size()); }
    bool allValid() const
    {
        for (const auto &spy : m_spies)
            if (!spy->isValid())
                return false;
        return true;
    }
    QStringList fired() const
    {
        QStringList out;
        for (size_t i = 0; i < m_spies.size(); ++i)
            if (m_spies[i]->count() > 0)
                out.append(m_names.at(int(i)));
        return out;
    }

private:
    std::vector<std::unique_ptr<QSignalSpy>> m_spies;
    QStringList m_names;
};

// The stored shapes a room's mode can be in. Index 0 of each list is "no key".
const QList<QVariant> kModeValues = {QVariant(), 0, 1, 2, 3, -1, 7};
const QList<QVariant> kUnsyncedValues = {QVariant(), -1, 0, 1, 2, 3, 9,
                                         QStringLiteral("junk")};
const int kGridRooms =
    int(kModeValues.size() * kModeValues.size() * kUnsyncedValues.size());

QString gridRoom(int i)
{
    return QStringLiteral("!grid%1:matrix.example").arg(i);
}

void putOrSkip(QSettings &s, const QString &key, const QVariant &value)
{
    if (value.isValid())
        s.setValue(key, value);
}

// One room per combination of: the account's own mode key (absent, 0..3,
// out of range), the legacy device-global key (same), and the unsynced flag
// (absent, bases -1 and 0..3, out of range, unparsable). `alice` gets the
// combination itself; `bob` and the device scope get rotated copies, so that
// for most rooms the three scopes answer differently and a reader looking in
// the wrong scope cannot agree by accident.
void seedModeGrid(const QString &aliceSlug, const QString &bobSlug)
{
    QSettings seed;
    const int nm = int(kModeValues.size());
    const int nu = int(kUnsyncedValues.size());
    for (int i = 0; i < kGridRooms; ++i) {
        const int accountIdx = i % nm;
        const int legacyIdx = (i / nm) % nm;
        const int unsyncedIdx = i / (nm * nm);
        const QString room = gridRoom(i);
        const QString modeTail = QStringLiteral("/notifications/room-mode/") + room;
        const QString unsyncedTail =
            QStringLiteral("/notifications/room-mode-unsynced/") + room;
        putOrSkip(seed, QStringLiteral("notifications/room-mode/") + room,
                  kModeValues.at(legacyIdx));
        putOrSkip(seed, QStringLiteral("accounts/") + aliceSlug + modeTail,
                  kModeValues.at(accountIdx));
        putOrSkip(seed, QStringLiteral("accounts/") + aliceSlug + unsyncedTail,
                  kUnsyncedValues.at(unsyncedIdx));
        putOrSkip(seed, QStringLiteral("accounts/") + bobSlug + modeTail,
                  kModeValues.at((accountIdx + 3) % nm));
        putOrSkip(seed, QStringLiteral("accounts/") + bobSlug + unsyncedTail,
                  kUnsyncedValues.at((unsyncedIdx + 5) % nu));
        putOrSkip(seed,
                  QStringLiteral("notifications/room-mode-unsynced/") + room,
                  kUnsyncedValues.at((unsyncedIdx + 2) % nu));
    }
    seed.sync();
}

// The rooms checked: the whole grid plus the empty room id, which every
// reader short-circuits.
QStringList modeGridRooms()
{
    QStringList rooms;
    for (int i = 0; i < kGridRooms; ++i)
        rooms.append(gridRoom(i));
    rooms.append(QString());
    return rooms;
}

} // namespace

// For the active account, every *For answer equals the active accessor's, and
// both equal the pre-refactor rule (the verbatim copy above), over every
// stored shape. Then the same with no active account (device scope, uid "").
// The coverage counts at the end assert that the grid really produced every
// answer the readers can give, so a fixture that stopped seeding would fail
// rather than compare a few identical defaults.
void SettingsSessionTest::perAccountModeReadersMatchTheActiveAccessorsAndTheOldRule()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    const QString alice = QStringLiteral("@alice:matrix.example");
    const QString bob = QStringLiteral("@bob:matrix.example");
    settings.saveSession(QStringLiteral("https://matrix.example"), alice,
                         QStringLiteral("ALICEDEVICE"),
                         QStringLiteral("alice-token-fixture"));
    settings.saveSession(QStringLiteral("https://matrix.example"), bob,
                         QStringLiteral("BOBDEVICE"),
                         QStringLiteral("bob-token-fixture"));
    const QString aliceSlug = matrix::app_data::safeUserSlug(alice);
    const QString bobSlug = matrix::app_data::safeUserSlug(bob);
    seedModeGrid(aliceSlug, bobSlug);
    settings.setActiveAccountUserId(alice);
    QCOMPARE(settings.activeAccountUserId(), alice);

    QSettings raw;
    const QStringList rooms = modeGridRooms();
    QCOMPARE(rooms.size(), kGridRooms + 1);

    QSet<int> modesSeen, basesSeen, unreadSeen;
    QSet<bool> unsyncedSeen, deviceOnlySeen, storedSeen, decidesSeen;
    int checked = 0;
    for (const QString &room : rooms) {
        const ModeAnswers old = oldRuleAnswers(raw, aliceSlug, room);
        const ModeAnswers active = activeAnswers(settings, room);
        const ModeAnswers twin = forAnswers(settings, alice, room);
        QVERIFY2(active == old,
                 qPrintable(QStringLiteral("room %1: the active accessors "
                                           "changed behaviour: %2, the old "
                                           "rule says %3")
                                .arg(room, active.toString(), old.toString())));
        QVERIFY2(twin == active,
                 qPrintable(QStringLiteral("room %1: the *For twin disagrees "
                                           "with the active accessor for the "
                                           "active account: %2 vs %3")
                                .arg(room, twin.toString(), active.toString())));
        modesSeen.insert(twin.mode);
        basesSeen.insert(twin.base);
        unreadSeen.insert(twin.unread);
        unsyncedSeen.insert(twin.unsynced);
        deviceOnlySeen.insert(twin.deviceOnly);
        storedSeen.insert(twin.stored);
        decidesSeen.insert(twin.decidesLocally);
        ++checked;
    }
    QCOMPARE(checked, kGridRooms + 1);
    QCOMPARE(modesSeen, (QSet<int>{0, 1, 2, 3}));
    QCOMPARE(basesSeen, (QSet<int>{-1, 0, 1, 2, 3}));
    QCOMPARE(unreadSeen,
             (QSet<int>{0, 1, 2, 3, roomunread::kModeMentionsUnverified}));
    QCOMPARE(unsyncedSeen.size(), 2);
    QCOMPARE(deviceOnlySeen.size(), 2);
    QCOMPARE(storedSeen.size(), 2);
    QCOMPARE(decidesSeen.size(), 2);

    // No active account: the active accessors read the device scope, and so
    // does the twin for an empty user id.
    settings.setActiveAccountUserId(QString());
    QVERIFY(settings.activeAccountUserId().isEmpty());
    QSet<int> deviceUnreadSeen;
    QSet<bool> deviceUnsyncedSeen;
    for (const QString &room : rooms) {
        const ModeAnswers old = oldRuleAnswers(raw, QString(), room);
        const ModeAnswers active = activeAnswers(settings, room);
        const ModeAnswers twin = forAnswers(settings, QString(), room);
        QVERIFY2(active == old,
                 qPrintable(QStringLiteral("room %1, device scope: %2, the old "
                                           "rule says %3")
                                .arg(room, active.toString(), old.toString())));
        QVERIFY2(twin == active,
                 qPrintable(QStringLiteral("room %1, device scope: twin %2 vs "
                                           "active %3")
                                .arg(room, twin.toString(), active.toString())));
        deviceUnreadSeen.insert(twin.unread);
        deviceUnsyncedSeen.insert(twin.unsynced);
    }
    QVERIFY2(deviceUnreadSeen.contains(roomunread::kModeMentionsUnverified),
             "the device-scope grid never produced an unverified mentions "
             "mode, so it did not exercise the unsynced/device-only branch");
    QCOMPARE(deviceUnsyncedSeen.size(), 2);
}

// A non-active saved account is read from ITS keys, without touching the
// active account and without a single signal. The expected answers are taken
// two independent ways: the old rule over Bob's slug, and the active
// accessors once Bob really is the active account.
void SettingsSessionTest::perAccountModeReadersReadANonActiveAccountsOwnScope()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    const QString alice = QStringLiteral("@alice:matrix.example");
    const QString bob = QStringLiteral("@bob:matrix.example");
    settings.saveSession(QStringLiteral("https://matrix.example"), alice,
                         QStringLiteral("ALICEDEVICE"),
                         QStringLiteral("alice-token-fixture"));
    settings.saveSession(QStringLiteral("https://matrix.example"), bob,
                         QStringLiteral("BOBDEVICE"),
                         QStringLiteral("bob-token-fixture"));
    const QString aliceSlug = matrix::app_data::safeUserSlug(alice);
    const QString bobSlug = matrix::app_data::safeUserSlug(bob);
    seedModeGrid(aliceSlug, bobSlug);
    settings.setActiveAccountUserId(alice);
    QCOMPARE(settings.activeAccountUserId(), alice);

    QSettings raw;
    const QStringList rooms = modeGridRooms();
    QHash<QString, ModeAnswers> bobWhileAliceActive;
    int differFromAlice = 0;
    {
        EverySignalSpy spy(&settings);
        QVERIFY2(spy.watched() > 20,
                 "the signal spy found almost no signals; it is not watching "
                 "SettingsManager");
        QVERIFY(spy.allValid());
        for (const QString &room : rooms) {
            const ModeAnswers forBob = forAnswers(settings, bob, room);
            const ModeAnswers old = oldRuleAnswers(raw, bobSlug, room);
            QVERIFY2(forBob == old,
                     qPrintable(QStringLiteral("room %1: Bob's answers while "
                                               "Alice is active are %2, Bob's "
                                               "own keys say %3")
                                    .arg(room, forBob.toString(),
                                         old.toString())));
            if (forBob != activeAnswers(settings, room))
                ++differFromAlice;
            bobWhileAliceActive.insert(room, forBob);
        }
        QVERIFY2(spy.fired().isEmpty(),
                 qPrintable(QStringLiteral("reading another account's modes "
                                           "emitted: %1")
                                .arg(spy.fired().join(QStringLiteral(", ")))));
    }
    QCOMPARE(settings.activeAccountUserId(), alice);
    // The fixture gives Bob rotated keys; if almost every room agreed with
    // Alice the case could not tell the two scopes apart.
    QVERIFY2(differFromAlice > kGridRooms / 2,
             qPrintable(QStringLiteral("only %1 of %2 rooms differ between "
                                       "Alice and Bob; the fixture no longer "
                                       "separates the scopes")
                            .arg(differFromAlice)
                            .arg(kGridRooms)));

    // The second oracle: make Bob active and ask the active accessors.
    settings.setActiveAccountUserId(bob);
    QCOMPARE(settings.activeAccountUserId(), bob);
    for (const QString &room : rooms) {
        const ModeAnswers active = activeAnswers(settings, room);
        QVERIFY2(bobWhileAliceActive.value(room) == active,
                 qPrintable(QStringLiteral("room %1: Bob read while Alice was "
                                           "active (%2) is not what Bob reads "
                                           "as the active account (%3)")
                                .arg(room,
                                     bobWhileAliceActive.value(room).toString(),
                                     active.toString())));
        // And the twin for Alice, now inactive, still reads Alice's keys.
        QVERIFY(forAnswers(settings, alice, room)
                == oldRuleAnswers(raw, aliceSlug, room));
    }
}

// An empty user id, one with no saved record, one that is not a Matrix id,
// and one whose slug collides with a saved account's (the slug mapping is not
// injective) all read the device scope: exactly what the active accessors
// answer with no active account. Never the active account's scope.
void SettingsSessionTest::perAccountModeReadersUseTheDeviceScopeForAnEmptyOrUnknownUser()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    const QString alice = QStringLiteral("@alice:matrix.example");
    const QString bob = QStringLiteral("@bob:matrix.example");
    // "@eve:a_b.example" and "@eve_a:b.example" share the slug
    // "eve_a_b.example"; only the first is saved.
    const QString eve = QStringLiteral("@eve:a_b.example");
    const QString eveCollision = QStringLiteral("@eve_a:b.example");
    settings.saveSession(QStringLiteral("https://matrix.example"), alice,
                         QStringLiteral("ALICEDEVICE"),
                         QStringLiteral("alice-token-fixture"));
    settings.saveSession(QStringLiteral("https://matrix.example"), bob,
                         QStringLiteral("BOBDEVICE"),
                         QStringLiteral("bob-token-fixture"));
    seedAccountRecord(eve, QStringLiteral("https://a_b.example"));
    const QString aliceSlug = matrix::app_data::safeUserSlug(alice);
    const QString eveSlug = matrix::app_data::safeUserSlug(eve);
    QCOMPARE(matrix::app_data::safeUserSlug(eveCollision), eveSlug);
    QVERIFY(settings.hasSavedAccount(eve));
    QVERIFY(!settings.hasSavedAccount(eveCollision));
    // Eve's scope gets Alice's grid, so the colliding id would read
    // account-scoped answers if the lookup matched on the slug alone.
    seedModeGrid(aliceSlug, eveSlug);
    settings.setActiveAccountUserId(alice);
    QCOMPARE(settings.activeAccountUserId(), alice);

    QSettings raw;
    const QStringList rooms = modeGridRooms();
    const QStringList strangers = {
        QString(),
        QStringLiteral("@nobody:matrix.example"),
        QStringLiteral("not-a-matrix-id"),
        eveCollision,
    };
    int differFromActive = 0;
    {
        EverySignalSpy spy(&settings);
        QVERIFY(spy.allValid());
        for (const QString &stranger : strangers) {
            for (const QString &room : rooms) {
                const ModeAnswers got = forAnswers(settings, stranger, room);
                const ModeAnswers device = oldRuleAnswers(raw, QString(), room);
                QVERIFY2(got == device,
                         qPrintable(QStringLiteral("user '%1' room %2: %3, the "
                                                   "device scope says %4")
                                        .arg(stranger, room, got.toString(),
                                             device.toString())));
                if (got != activeAnswers(settings, room))
                    ++differFromActive;
            }
        }
        QVERIFY2(spy.fired().isEmpty(),
                 qPrintable(QStringLiteral("reading an unknown account's "
                                           "modes emitted: %1")
                                .arg(spy.fired().join(QStringLiteral(", ")))));
    }
    QCOMPARE(settings.activeAccountUserId(), alice);
    // Without this, a twin that fell back to the ACTIVE account's scope would
    // pass wherever the active and device scopes happen to agree.
    QVERIFY2(differFromActive > int(strangers.size()) * kGridRooms / 2,
             qPrintable(QStringLiteral("only %1 answers differ between the "
                                       "device scope and the active account")
                            .arg(differFromActive)));

    // The same device-scope answers the active accessors give once no
    // account is active.
    settings.setActiveAccountUserId(QString());
    for (const QString &room : rooms)
        QVERIFY(forAnswers(settings, QStringLiteral("@nobody:matrix.example"),
                           room)
                == activeAnswers(settings, room));
}

// B044: sign-out deleted the store only after the server logout and the store
// close, so a quit, crash or kill in between left the keys and search index on
// disk with no record pointing at them. The marker is written before the
// server logout, survives a restart, names the recorded store exactly, and
// goes when the store does or when the account signs in again.
void SettingsSessionTest::anInterruptedSignOutNamesItsStoreUntilTheStoreIsGone()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    const QByteArray oldData = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    const auto restore = qScopeGuard([&] { qputenv("XDG_DATA_HOME", oldData); });

    FakeSecretStore secrets;
    const QString hs = QStringLiteral("https://localhs.test");
    const QString me = QStringLiteral("@bk001:localhs.test");
    matrix::app_data::AccountIdentity recorded;
    {
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        settings.saveSession(hs, me, QStringLiteral("DEV"), QStringLiteral("t-fixture"));
        QVERIFY(settings.resolveSavedIdentity(me, &recorded));
        QVERIFY(settings.pendingSignOuts().isEmpty());
        settings.recordPendingSignOut(recorded);
    }
    // The next start: a new manager reads it back from the file.
    SettingsManager reopened;
    reopened.setSecretStore(&secrets);
    QList<matrix::app_data::AccountIdentity> pending = reopened.pendingSignOuts();
    QCOMPARE(pending.size(), 1);
    QCOMPARE(pending.first().userId, me);
    QCOMPARE(QDir::cleanPath(pending.first().rustStorePath),
             QDir::cleanPath(recorded.rustStorePath));
    QCOMPARE(pending.first().effectiveStoreSlug(), recorded.effectiveStoreSlug());

    // Cleared once the store is gone.
    reopened.clearPendingSignOut(me);
    QVERIFY(reopened.pendingSignOuts().isEmpty());

    // A new sign-in of the account supersedes an unfinished sign-out: its
    // store must not be deleted at the next start.
    reopened.recordPendingSignOut(recorded);
    QCOMPARE(reopened.pendingSignOuts().size(), 1);
    reopened.saveSession(hs, me, QStringLiteral("DEV2"), QStringLiteral("t2-fixture"));
    QVERIFY(reopened.pendingSignOuts().isEmpty());
    // Another account's id never clears it.
    reopened.recordPendingSignOut(recorded);
    reopened.clearPendingSignOut(QStringLiteral("@other:localhs.test"));
    QCOMPARE(reopened.pendingSignOuts().size(), 1);
}

// A marker that no longer resolves to the very store it named (another data
// root, an edited file) or whose store another saved account owns is dropped
// with nothing to delete: a missing proof is never permission (CLAUDE.md §6).
void SettingsSessionTest::aPendingSignOutNeverNamesAStoreItCannotProve()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    const QByteArray oldData = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    const auto restore = qScopeGuard([&] { qputenv("XDG_DATA_HOME", oldData); });

    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    const QString hs = QStringLiteral("https://localhs.test");
    const QString alice = QStringLiteral("@alice:localhs.test");
    const QString bob = QStringLiteral("@bob:localhs.test");
    settings.saveSession(hs, alice, QStringLiteral("A"), QStringLiteral("a-fixture"));
    settings.saveSession(hs, bob, QStringLiteral("B"), QStringLiteral("b-fixture"));
    matrix::app_data::AccountIdentity aliceIdentity;
    QVERIFY(settings.resolveSavedIdentity(alice, &aliceIdentity));

    // The recorded path no longer matches what the record resolves to.
    settings.recordPendingSignOut(aliceIdentity);
    {
        QSettings raw;
        raw.setValue(QStringLiteral("pendingSignOut/") + aliceIdentity.slug
                         + QStringLiteral("/storePath"),
                     QStringLiteral("/elsewhere/matrix-rust-sdk-store"));
        raw.sync();
    }
    SettingsManager moved;
    QVERIFY(moved.pendingSignOuts().isEmpty());
    // Dropped, not kept to be refused again forever.
    QVERIFY(!QSettings().contains(QStringLiteral("pendingSignOut/") + aliceIdentity.slug
                                  + QStringLiteral("/userId")));

    // Alice's record is gone (her sign-out got that far) and Bob's record now
    // names her directory as his store: it is his, and it stays.
    settings.recordPendingSignOut(aliceIdentity);
    QVERIFY(settings.clearSessionForAccount(alice));
    settings.setStoreSlugFor(bob, aliceIdentity.effectiveStoreSlug());
    QCOMPARE(settings.accountOwningStoreSlug(aliceIdentity.effectiveStoreSlug()), bob);
    QVERIFY2(settings.pendingSignOuts().isEmpty(),
             "a pending sign-out named a store another account owns");
}

// B118: a settings file QSettings cannot parse used to look exactly like a
// fresh install: the login page, the default server, no hint.
void SettingsSessionTest::anUnreadableSettingsFileIsReported()
{
    const QString path = QSettings().fileName();
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("garbage garbage garbage\n");
    }
    SettingsManager settings;
    QCOMPARE(settings.settingsReadProblem(), int(SettingsManager::SettingsUnreadable));
}

// Most damage parses without an error and reads as "no accounts". The account
// stores on disk are then the witness; a store a sign-out is still finishing
// is not, and a file that lists an account is fine.
void SettingsSessionTest::aSettingsFileThatListsNoAccountBesideAStoreIsReported()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    const QByteArray oldData = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    const auto restore = qScopeGuard([&] { qputenv("XDG_DATA_HOME", oldData); });

    {
        SettingsManager fresh;
        QCOMPARE(fresh.settingsReadProblem(), int(SettingsManager::NoReadProblem));
    }
    matrix::app_data::AccountIdentity lost;
    QVERIFY(matrix::app_data::resolveAccountIdentity(
        QStringLiteral("https://localhs.test"), QStringLiteral("bk059"), &lost));
    QVERIFY(QDir().mkpath(lost.rustStorePath));
    {
        SettingsManager damaged;
        QCOMPARE(damaged.settingsReadProblem(), int(SettingsManager::AccountsMissing));
    }
    // The store of a sign-out still being finished is not evidence.
    {
        QSettings raw;
        const QString base = QStringLiteral("pendingSignOut/") + lost.slug + QLatin1Char('/');
        raw.setValue(base + QStringLiteral("userId"), lost.userId);
        raw.setValue(base + QStringLiteral("storePath"),
                     QDir::cleanPath(QFileInfo(lost.rustStorePath).absoluteFilePath()));
        raw.sync();
    }
    {
        SettingsManager finishing;
        QCOMPARE(finishing.settingsReadProblem(), int(SettingsManager::NoReadProblem));
    }
    QSettings().remove(QStringLiteral("pendingSignOut"));
    // A file that lists an account says nothing about other stores.
    FakeSecretStore secrets;
    {
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        settings.saveSession(QStringLiteral("https://localhs.test"),
                             QStringLiteral("@someone:localhs.test"),
                             QStringLiteral("D"), QStringLiteral("d-fixture"));
    }
    SettingsManager listed;
    QCOMPARE(listed.settingsReadProblem(), int(SettingsManager::NoReadProblem));
}

// R6. Reported as "Lightning couldn't read its saved settings" after a
// SIGTERM: the file was intact (the log said AccountsMissing), and the only
// store on disk was the one a cancelled first sign-in had created before the
// server answered. A store whose sign-in is recorded as unfinished is not
// evidence that the file lost an account; any other stray store still is.
void SettingsSessionTest::aStoreLeftByAnUnfinishedSignInIsNotAMissingAccount()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    const QByteArray oldData = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    const auto restore = qScopeGuard([&] { qputenv("XDG_DATA_HOME", oldData); });

    matrix::app_data::AccountIdentity attempt;
    QVERIFY(matrix::app_data::resolveAccountIdentity(
        QStringLiteral("https://localhs.test"), QStringLiteral("r6signin"), &attempt));
    // The marker in the shape recordPendingSignIn() writes, written raw so the
    // case states the on-disk contract, then the store the SDK created.
    {
        QSettings raw;
        const QString base = QStringLiteral("pendingSignIn/") + attempt.slug + QLatin1Char('/');
        raw.setValue(base + QStringLiteral("userId"), attempt.userId);
        raw.setValue(base + QStringLiteral("storePath"),
                     QDir::cleanPath(QFileInfo(attempt.rustStorePath).absoluteFilePath()));
        raw.sync();
    }
    QVERIFY(QDir().mkpath(attempt.rustStorePath));
    {
        SettingsManager afterTheKill;
        QCOMPARE(afterTheKill.settingsReadProblem(), int(SettingsManager::NoReadProblem));
    }

    // A second, unexplained store beside it is still reported.
    matrix::app_data::AccountIdentity lost;
    QVERIFY(matrix::app_data::resolveAccountIdentity(
        QStringLiteral("https://localhs.test"), QStringLiteral("r6lost"), &lost));
    QVERIFY(QDir().mkpath(lost.rustStorePath));
    {
        SettingsManager damaged;
        QCOMPARE(damaged.settingsReadProblem(), int(SettingsManager::AccountsMissing));
    }
    QVERIFY(QDir(QFileInfo(lost.rustStorePath).absolutePath()).removeRecursively());

    // A marker naming a different store explains nothing about this one.
    {
        QSettings raw;
        raw.setValue(QStringLiteral("pendingSignIn/") + attempt.slug
                         + QStringLiteral("/storePath"),
                     QDir::cleanPath(data.path() + QStringLiteral("/elsewhere")));
        raw.sync();
    }
    QVERIFY(QDir().mkpath(data.path() + QStringLiteral("/elsewhere")));
    {
        SettingsManager misnamed;
        QCOMPARE(misnamed.settingsReadProblem(), int(SettingsManager::AccountsMissing));
    }
}

// The marker lives exactly as long as the window it covers: recorded before
// the store exists, gone once saveSession() records the account, and gone at a
// start that finds its store removed (a failed attempt rolls its store back).
void SettingsSessionTest::aPendingSignInEndsWithItsSignInOrItsStore()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    const QByteArray oldData = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    const auto restore = qScopeGuard([&] { qputenv("XDG_DATA_HOME", oldData); });

    matrix::app_data::AccountIdentity attempt;
    QVERIFY(matrix::app_data::resolveAccountIdentity(
        QStringLiteral("https://localhs.test"), QStringLiteral("r6finish"), &attempt));
    const QString key = QStringLiteral("pendingSignIn/") + attempt.slug
        + QStringLiteral("/storePath");

    FakeSecretStore secrets;
    {
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        settings.recordPendingSignIn(attempt);
        // Durable at once: the next thing that happens may be the kill.
        QVERIFY(QSettings().contains(key));
        QVERIFY(QDir().mkpath(attempt.rustStorePath));
        // Another user id on the same slug never clears it.
        settings.clearPendingSignIn(attempt.userId + QStringLiteral("x"));
        QVERIFY(QSettings().contains(key));
        settings.saveSession(attempt.homeserver, attempt.userId, QStringLiteral("D"),
                             QStringLiteral("d-fixture"));
        QVERIFY2(!QSettings().contains(key), "a finished sign-in kept its marker");
    }

    // Abandoned, then its store rolled back: the next start drops the marker.
    QVERIFY(QDir(QFileInfo(attempt.rustStorePath).absolutePath()).removeRecursively());
    QVERIFY(QDir().mkpath(QFileInfo(attempt.rustStorePath).absolutePath()));
    {
        QSettings().clear();
        SettingsManager settings;
        settings.recordPendingSignIn(attempt);
    }
    QVERIFY(QSettings().contains(key));
    {
        SettingsManager next;
        QCOMPARE(next.settingsReadProblem(), int(SettingsManager::NoReadProblem));
    }
    QVERIFY2(!QSettings().contains(key), "a marker outlived the store it named");
}

// The marker is written under the identity as TYPED (a bare username's
// stand-in, other casing) and the record under the server's canonical id, so
// saveSession() alone never finds it. Recording the store the sign-in opened
// ends it, and so does the attempt clearing its own marker.
void SettingsSessionTest::aPendingSignInTypedAsAStandInEndsWhenItsStoreIsRecorded()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    const QByteArray oldData = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    const auto restore = qScopeGuard([&] { qputenv("XDG_DATA_HOME", oldData); });

    const QString canonical = QStringLiteral("@r6m1:localhs.test");
    matrix::app_data::AccountIdentity typed;
    QVERIFY(matrix::app_data::resolveAccountIdentity(
        QStringLiteral("http://127.0.0.1:18008"), QStringLiteral("R6m1"), &typed));
    QVERIFY(typed.userId != canonical);
    const QString key = QStringLiteral("pendingSignIn/") + typed.slug
        + QStringLiteral("/storePath");

    FakeSecretStore secrets;
    {
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        settings.recordPendingSignIn(typed);
        QVERIFY(QDir().mkpath(typed.rustStorePath));
        settings.saveSession(QStringLiteral("https://localhs.test"), canonical,
                             QStringLiteral("D"), QStringLiteral("d-fixture"));
        // The record is the server's; the marker is the stand-in's.
        QVERIFY2(QSettings().contains(key), "the fixture is not a stand-in case");
        // What login_ok records next: the store the sign-in opened.
        settings.setStoreSlugFor(canonical, typed.effectiveStoreSlug());
        QVERIFY2(!QSettings().contains(key),
                 "a finished stand-in sign-in kept its marker");

        // The attempt's own clear, by identity, finds it as well.
        settings.recordPendingSignIn(typed);
        QVERIFY(QSettings().contains(key));
        settings.clearPendingSignIn(canonical);
        QVERIFY(QSettings().contains(key));
        settings.clearPendingSignIn(typed);
        QVERIFY(!QSettings().contains(key));
    }
}

// R6, the hypothesis that was refuted. QSettings writes through QSaveFile (a
// temporary sibling renamed over the file) under a QLockFile, so a writer
// killed mid-write leaves at most a partial temporary and a lock naming a dead
// process. Neither may read as damage, and the next write must not wedge.
// Passes on the old code too: a guard on the Qt behaviour the fix relies on,
// not a regression test (the kill measurement is in the R6 notes).
void SettingsSessionTest::aWriterKilledMidWriteLeavesTheLastGoodFile()
{
    const QString account = QStringLiteral("@r6kill:localhs.test");
    FakeSecretStore secrets;
    {
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        settings.saveSession(QStringLiteral("https://localhs.test"), account,
                             QStringLiteral("D"), QStringLiteral("d-fixture"));
    }
    const QString path = QSettings().fileName();
    QByteArray good;
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        good = file.readAll();
    }
    QVERIFY(good.contains("r6kill"));
    {
        // QSaveFile's temporary: the name QTemporaryFile derives, half written.
        QFile partial(path + QStringLiteral(".kIlLd1"));
        QVERIFY(partial.open(QIODevice::WriteOnly | QIODevice::Truncate));
        partial.write(good.left(good.size() / 2));
        // QLockFile's contents, for a process that no longer exists.
        QFile lock(path + QStringLiteral(".lock"));
        QVERIFY(lock.open(QIODevice::WriteOnly | QIODevice::Truncate));
        lock.write("2147483646\nlightning-matrix\n" + QSysInfo::machineHostName().toUtf8()
                   + "\n\n\n");
    }
    const auto cleanup = qScopeGuard([&] {
        QFile::remove(path + QStringLiteral(".kIlLd1"));
        QFile::remove(path + QStringLiteral(".lock"));
    });

    SettingsManager next;
    QCOMPARE(next.settingsReadProblem(), int(SettingsManager::NoReadProblem));
    QVERIFY(next.savedAccountUserIds().contains(account));
    // The stale lock is taken over, not waited on for ever.
    QSettings writer;
    writer.setValue(QStringLiteral("r6/afterTheKill"), true);
    writer.sync();
    QCOMPARE(writer.status(), QSettings::NoError);
    QVERIFY(QSettings().value(QStringLiteral("r6/afterTheKill")).toBool());
}

QTEST_MAIN(SettingsSessionTest)
#include "SettingsSessionTest.moc"
