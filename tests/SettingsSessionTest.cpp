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
    void migratesEverySecretOfAnAccountWhoseIdContainsASlash();
    void aKeptPlaintextGroupNeverOverwritesADifferentSecureValue();
    void secretsWrittenHereOnlyForThisInstallsOwnSignIn();
    void leavesPlaintextWhenNoSavedAccountOwnsTheGroup();
    void insecureSecretsGroupFoldingIsStillTwoCharacters();
    void clearingTheLastAccountSweepsTheDeviceGlobalRoomKeys();
    void validThemeIdsRoundTripAndPersist();
    void unknownStoredThemeFallsBackToSystem();
    void composerModeIsMarkdownForAFreshProfile();
    void aStoredRichModeIsMigratedToMarkdownOnce();
    void largeAlbumArtDefaultsOnAndAnOptOutPersists();
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
    void hiddenComposerButtonsDefaultToNoneAndNormalize();
    void readReceiptModeDefaultsToPublicPersistsAndClamps();
    void anOlderBuildsDoNotSendModeReadsAsPrivate();
    void theReadReceiptComboOffersTwoModes();
    void typingNotificationsDefaultOnAndPersist();
    void theEncryptedPreviewLevelDefaultsToFollowingTheGeneralOne();
    void anUnrecognisedPreviewModeErrsTowardLessDisclosure();
    void anUnknownEncryptionStateTakesTheStricterLevel();
    void strictDeviceTrustDefaultsOffAndPersists();
    void noiseSuppressionDefaultsToOffPersistsAndFallsBack();
    void keepMediaOnDeviceDefaultsOnAndPersists();
    void developerToolsDefaultOffAndPersist();
    void theKeepMediaCheckboxSaysWhereEncryptedMediaIsKept();
    void switchingAccountsReAnnouncesEveryAccountScopedAppearanceValue();
    void everyAccountScopedGetterHasItsSignalInTheAccountSwitch();
    void theSoundSectionsScopeSentenceMatchesWhereThingsActuallyLive();
    void turningOffCloseToTrayAnnouncesTheStartInTrayItDerives();
    void perAccountModeReadersMatchTheActiveAccessorsAndTheOldRule();
    void perAccountModeReadersReadANonActiveAccountsOwnScope();
    void perAccountModeReadersUseTheDeviceScopeForAnEmptyOrUnknownUser();
    void anInterruptedSignOutNamesItsStoreUntilTheStoreIsGone();
    void aPendingSignOutNeverNamesAStoreItCannotProve();
    void anUnreadableSettingsFileIsReported();
    void aSettingsFileThatListsNoAccountBesideAStoreIsReported();
    void aStoreLeftByAnUnfinishedSignInIsNotAMissingAccount();
    void aPendingSignInEndsWithItsSignInOrItsStore();
    void aPendingSignInTypedAsAStandInEndsWhenItsStoreIsRecorded();
    void aWriterKilledMidWriteLeavesTheLastGoodFile();

private:
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

    const QString path = QSettings().fileName();
    const QString other = QStringLiteral("@bob:matrix.example");
    QSignalSpy live(&settings, &SettingsManager::callParticipantVolumeChanged);

    settings.holdWritesWhileDragging(true);
    for (int step = 40; step <= 47; ++step)
        settings.setCallParticipantVolume(other, step);
    settings.setCallShareVolume(other, 150);
    QTest::qWait(SettingsManager::kFlushDelayMs + 300);
    QCOMPARE(live.count(), 8);
    QCOMPARE(settings.callParticipantVolume(other), 47);
    QVERIFY2(!QString::fromUtf8(settingsFileBytes(path))
                  .contains(QStringLiteral("callVolumes")),
             "a volume reached the disk in the middle of a drag");

    settings.holdWritesWhileDragging(false);
    const QString onDisk = QString::fromUtf8(settingsFileBytes(path));
    QVERIFY2(onDisk.contains(QStringLiteral("=47")),
             qPrintable(QStringLiteral("volume not on disk after release; "
                                       "file said:\n%1").arg(onDisk)));
    QVERIFY(onDisk.contains(QStringLiteral("callVolumes")));
    QVERIFY(onDisk.contains(QStringLiteral("=150")));
    QVERIFY(!settings.hasUnflushedWrites());

    settings.holdWritesWhileDragging(true);
    QVERIFY(settings.writesHeld());
}

namespace {
void settle(SettingsManager &settings)
{
    QCoreApplication::processEvents();
    settings.flushPendingWrites();
    QVERIFY(!settings.hasUnflushedWrites());
}
}

void SettingsSessionTest::writesAreGatheredIntoOneThrottledFlush()
{
    const QString path = QSettings().fileName();
    SettingsManager settings;
    settle(settings);

    settings.setRoomListWidth(301);
    QCoreApplication::processEvents();
    QVERIFY(settings.hasUnflushedWrites());
    QCOMPARE(settings.roomListWidth(), 301);
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
        settle(settings);
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
    settle(settings);
    settings.setKnownMediaSizeBytes(QStringLiteral("$other"), 99);
    settle(settings);
    settings.setKnownMediaSizeBytes(QStringLiteral("$video"), 123456);
    QCoreApplication::processEvents();
    QVERIFY(settings.hasUnflushedWrites());
}

void SettingsSessionTest::aWindowsDeviceIdSurvivesBeingStored()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);

    const QString windowsId = QStringLiteral(
        "\\\\?\\usb#vid_322e&pid_233a&mi_00#7&1f2e3d4c&0&0000#"
        "{65e8773d-8f56-11d0-a3b9-00a0c9223196}\\global");
    settings.setPreferredCameraId(windowsId);
    QCOMPARE(settings.preferredCameraId(), windowsId);

    settings.setPreferredMicrophoneId(windowsId);
    QCOMPARE(settings.preferredMicrophoneId(), windowsId);

    settings.setPreferredCameraId(QStringLiteral("/dev/video0"));
    QCOMPARE(settings.preferredCameraId(), QStringLiteral("/dev/video0"));

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
    const QString user = QStringLiteral("@alice:matrix.example");
    const QString token = QStringLiteral("plaintext-token-fixture");
    seedAccountRecord(user, QStringLiteral("https://matrix.example"));
    {
        QSettings seed;
        seed.setValue(QStringLiteral("secrets/%1/accessToken").arg(user), token);
        seed.sync();
    }

    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);

    QCOMPARE(secrets.readSecret(user, QStringLiteral("accessToken")), token);
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
    secrets.setFailStore(true);
    SettingsManager settings;
    settings.setSecretStore(&secrets);

    QVERIFY(!secrets.hasSecret(user, QStringLiteral("accessToken")));
    QSettings check;
    QCOMPARE(check.value(QStringLiteral("secrets/%1/accessToken").arg(user)).toString(),
             token);
}

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

    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);

    QCOMPARE(secrets.readSecret(user, QStringLiteral("accessToken")), token);
    QCOMPARE(secrets.readSecret(user, QStringLiteral("refreshToken")), refresh);
    QCOMPARE(secrets.readSecret(user, QStringLiteral("oauthClientId")),
             clientId);
    QVERIFY2(!secrets.hasSecret(group, QStringLiteral("accessToken")),
             "the token was stored under the folded QSettings group name");

    QCOMPARE(settings.accessTokenFor(user), token);

    QSettings check;
    QVERIFY(!check.contains(
        QStringLiteral("secrets/%1/accessToken").arg(group)));
    QVERIFY(!check.contains(
        QStringLiteral("secrets/%1/refreshToken").arg(group)));
    QVERIFY(!check.contains(
        QStringLiteral("secrets/%1/oauthClientId").arg(group)));

    QVERIFY(settings.clearSessionForAccount(user));
    QVERIFY(!secrets.hasSecret(user, QStringLiteral("accessToken")));
    QVERIFY(!secrets.hasSecret(user, QStringLiteral("refreshToken")));
    QVERIFY(!secrets.hasSecret(user, QStringLiteral("oauthClientId")));
}

void SettingsSessionTest::leavesPlaintextWhenNoSavedAccountOwnsTheGroup()
{
    const QString ghost = QStringLiteral("@ghost:matrix.example");
    const QString token = QStringLiteral("orphan-token-fixture");
    {
        QSettings seed;
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
        QSettings seed;
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

    QVERIFY(settings.clearSessionForAccount(bob));
    {
        QSettings check;
        QVERIFY2(check.contains(railKey),
                 "the migration source was swept while an account still "
                 "needed it");
        QVERIFY(check.contains(collapsedKey));
        QVERIFY(check.contains(roomModeKey));
    }

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
    SettingsManager reopened;
    QCOMPARE(reopened.theme(), SettingsManager::NordTheme);
}

void SettingsSessionTest::composerModeIsMarkdownForAFreshProfile()
{
    SettingsManager fresh;
    QCOMPARE(fresh.composerMode(), QStringLiteral("markdown"));
    fresh.setComposerMode(QStringLiteral("rich"));
    SettingsManager reopened;
    QCOMPARE(reopened.composerMode(), QStringLiteral("rich"));
}

void SettingsSessionTest::aStoredRichModeIsMigratedToMarkdownOnce()
{
    {
        QSettings raw;
        raw.setValue(QStringLiteral("composer/mode"), QStringLiteral("rich"));
        raw.sync();
    }
    {
        SettingsManager migrated;
        QCOMPARE(migrated.composerMode(), QStringLiteral("markdown"));
        migrated.setComposerMode(QStringLiteral("rich"));
    }
    SettingsManager later;
    QCOMPARE(later.composerMode(), QStringLiteral("rich"));
}

void SettingsSessionTest::largeAlbumArtDefaultsOnAndAnOptOutPersists()
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
        raw.setValue(QStringLiteral("ui/theme"), 999);
        raw.sync();
    }
    SettingsManager settings;
    QCOMPARE(settings.theme(), SettingsManager::SystemTheme);

    {
        QSettings raw;
        raw.setValue(QStringLiteral("ui/theme"), -3);
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
    settings.setTheme(SettingsManager::PurpleDuskTheme);
    QCOMPARE(spy.count(), 1);
}

void SettingsSessionTest::spacesRailDepthStyleDefaultsToRegionsAndClamps()
{
    SettingsManager settings;
    QCOMPARE(settings.spacesRailDepthStyle(),
             SettingsManager::kRailDepthRegions);

    QSettings raw;
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
    fresh.setSpacesRailDepthStyle(SettingsManager::kRailDepthClassic);
    QCOMPARE(spy.count(), 1);
    fresh.setSpacesRailDepthStyle(99);
    QCOMPARE(fresh.spacesRailDepthStyle(),
             SettingsManager::kRailDepthRegions);
    QCOMPARE(spy.count(), 2);
}

void SettingsSessionTest::spacesRailWidthDefaultsToTheOldFixedWidthAndClamps()
{
    SettingsManager settings;
    QCOMPARE(settings.spacesRailWidth(), 68);

    QSettings raw;
    raw.setValue(QStringLiteral("shell/spacesRailWidth"), 4000);
    raw.sync();
    SettingsManager tooWide;
    QCOMPARE(tooWide.spacesRailWidth(), 260);

    raw.setValue(QStringLiteral("shell/spacesRailWidth"), 0);
    raw.sync();
    SettingsManager tooNarrow;
    QCOMPARE(tooNarrow.spacesRailWidth(), 68);

    raw.setValue(QStringLiteral("shell/spacesRailWidth"), 68);
    raw.sync();
    SettingsManager fresh;
    QSignalSpy spy(&fresh, &SettingsManager::spacesRailWidthChanged);
    fresh.setSpacesRailWidth(180);
    QCOMPARE(fresh.spacesRailWidth(), 180);
    QCOMPARE(spy.count(), 1);
    fresh.setSpacesRailWidth(180);
    QCOMPARE(spy.count(), 1);
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

    QCOMPARE(settings.shareMaxHeight(), 0);
    QCOMPARE(settings.effectiveShareMaxHeight(), 1080);
    QCOMPARE(settings.shareFps(), 0);
    QCOMPARE(settings.effectiveShareFps(), 30);

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

    raw.setValue(QStringLiteral("calls/shareMaxHeight"), 1080);
    raw.setValue(QStringLiteral("calls/shareFps"), 30);
    raw.sync();
    SettingsManager fresh;
    QSignalSpy spy(&fresh, &SettingsManager::shareQualityChanged);
    SettingsManager &settingsRt = fresh;
    settingsRt.setShareMaxHeight(720);
    QCOMPARE(settingsRt.shareMaxHeight(), 720);
    QCOMPARE(spy.count(), 1);
    settingsRt.setShareMaxHeight(720);
    QCOMPARE(spy.count(), 1);
    settingsRt.setShareMaxHeight(800);
    QCOMPARE(settingsRt.shareMaxHeight(), 720);
    QCOMPARE(spy.count(), 1);

    settingsRt.setShareFps(60);
    QCOMPARE(settingsRt.shareFps(), 60);
    QCOMPARE(spy.count(), 2);
}

void SettingsSessionTest::callQualityDefaultsToAutoAndPersists()
{
    QSettings raw;
    for (const char *key :
         { "calls/shareMaxHeight", "calls/shareFps",
           "calls/shareBitrateCapKbps", "calls/cameraResolution",
           "calls/echoCancellation", "calls/autoGainControl" })
        raw.remove(QString::fromLatin1(key));
    raw.sync();

    SettingsManager settings;
    QCOMPARE(settings.shareMaxHeight(), 0);
    QCOMPARE(settings.effectiveShareMaxHeight(), 1080);
    QCOMPARE(settings.shareFps(), 0);
    QCOMPARE(settings.effectiveShareFps(), 30);
    QCOMPARE(settings.shareBitrateCapKbps(), 0);
    QCOMPARE(settings.cameraResolution(), 0);
    QCOMPARE(settings.effectiveCameraResolution(), 720);
    QVERIFY(!settings.echoCancellation());
    QVERIFY(settings.autoGainControl());
    QVERIFY(!settings.shareQualityDemanding());

    QSignalSpy share(&settings, &SettingsManager::shareQualityChanged);
    QSignalSpy camera(&settings, &SettingsManager::cameraQualityChanged);
    QSignalSpy voice(&settings, &SettingsManager::voiceProcessingChanged);

    settings.setShareBitrateCapKbps(1800);
    QCOMPARE(settings.shareBitrateCapKbps(), 2000);
    settings.setShareBitrateCapKbps(99999);
    QCOMPARE(settings.shareBitrateCapKbps(), 4500);
    QCOMPARE(share.count(), 2);
    settings.setShareBitrateCapKbps(4400);
    QCOMPARE(share.count(), 2);
    settings.setShareBitrateCapKbps(0);
    QCOMPARE(settings.shareBitrateCapKbps(), 0);

    settings.setCameraResolution(480);
    QCOMPARE(settings.cameraResolution(), 480);
    QCOMPARE(camera.count(), 1);
    settings.setCameraResolution(481);
    QCOMPARE(settings.cameraResolution(), 0);
    QCOMPARE(settings.effectiveCameraResolution(), 720);

    settings.setShareMaxHeight(1440);
    QCOMPARE(settings.effectiveShareMaxHeight(), 1440);
    settings.setShareMaxHeight(0);
    QCOMPARE(settings.shareMaxHeight(), 0);
    settings.setShareFps(0);
    QCOMPARE(settings.effectiveShareFps(), 30);

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
    QCOMPARE(settings.loadPreviewsInEncryptedRooms(), false);
    QCOMPARE(settings.autoLoadLinkPreviews(), false);
    QCOMPARE(settings.animateGifPreviews(), true);

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
        QCOMPARE(settings.timelineWheelSpeed(), 1);

        QSignalSpy spy(&settings, &SettingsManager::timelineWheelSpeedChanged);
        settings.setTimelineWheelSpeed(2);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(settings.timelineWheelSpeed(), 2);
        settings.setTimelineWheelSpeed(2);
        QCOMPARE(spy.count(), 1);
    }
    {
        SettingsManager reopened;
        QCOMPARE(reopened.timelineWheelSpeed(), 2);
    }
    {
        SettingsManager settings;
        settings.setTimelineWheelSpeed(99);
        QCOMPARE(settings.timelineWheelSpeed(), 1);
        settings.setTimelineWheelSpeed(-5);
        QCOMPARE(settings.timelineWheelSpeed(), 1);
    }
    {
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
        QCOMPARE(s.videoPrefetchEnabled(), true);
        QCOMPARE(s.videoPrefetchMaxMb(), 8);
        QCOMPARE(s.videoPrefetchMinMb(), 1);
        QCOMPARE(s.videoPrefetchLimitMb(), 100);

        QSignalSpy en(&s, &SettingsManager::videoPrefetchEnabledChanged);
        QSignalSpy mb(&s, &SettingsManager::videoPrefetchMaxMbChanged);
        s.setVideoPrefetchEnabled(false);
        s.setVideoPrefetchEnabled(false);
        QCOMPARE(en.count(), 1);
        QCOMPARE(s.videoPrefetchEnabled(), false);
        s.setVideoPrefetchMaxMb(50);
        s.setVideoPrefetchMaxMb(50);
        QCOMPARE(mb.count(), 1);
        QCOMPARE(s.videoPrefetchMaxMb(), 50);
        s.setVideoPrefetchMaxMb(4000);
        QCOMPARE(s.videoPrefetchMaxMb(), 100);
        s.setVideoPrefetchMaxMb(-5);
        QCOMPARE(s.videoPrefetchMaxMb(), 1);
        s.setVideoPrefetchMaxMb(20);
    }
    {
        SettingsManager reopened;
        QCOMPARE(reopened.videoPrefetchEnabled(), false);
        QCOMPARE(reopened.videoPrefetchMaxMb(), 20);
    }
    {
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
        s.setGifPreferredProvider(QStringLiteral("bogus"));
        QCOMPARE(s.gifPreferredProvider(), QStringLiteral("klipy"));
    }
    {
        SettingsManager reopened;
        QCOMPARE(reopened.gifAutoplay(), 1);
        QCOMPARE(reopened.gifSafeSearch(), 0);
        QCOMPARE(reopened.storeRecentGifs(), false);
        QCOMPARE(reopened.gifPreferredProvider(), QStringLiteral("klipy"));
    }
    {
        SettingsManager s;
        s.setGifAutoplay(99);
        QCOMPARE(s.gifAutoplay(), 0);
        s.setGifSafeSearch(-3);
        QCOMPARE(s.gifSafeSearch(), 2);
    }
    {
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

void SettingsSessionTest::accessibilityAndSpacingSettingsPersistAndFallBack()
{
    QCOMPARE(SettingsManager::kMinTextScale, 75);
    QCOMPARE(SettingsManager::kMaxTextScale, 150);
    {
        SettingsManager settings;
        settings.setTextScale(80);
        QCOMPARE(settings.textScale(), 80);
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
        settings.setHighContrast(true);
        settings.setUnderlineLinks(true);
        settings.setNameColorCorrection(SettingsManager::kNameColorOff);
        settings.setMessageSpacing(SettingsManager::kSpacingRoomy);
        QCOMPARE(contrastSpy.count(), 1);
        QCOMPARE(underlineSpy.count(), 1);
        QCOMPARE(nameSpy.count(), 1);
        QCOMPARE(spacingSpy.count(), 1);

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
        settings.setRoomFilterMode(9);
        QCOMPARE(settings.roomFilterMode(), 0);
        settings.setRoomFilterMode(4);
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
        QCOMPARE(layoutSpy.count(), 1);
        QCOMPARE(spacingSpy.count(), 1);
        QCOMPARE(placementSpy.count(), 1);
        QCOMPARE(settings.textScale(), 120);
        QCOMPARE(scaleSpy.count(), 0);
        QCOMPARE(settings.theme(), SettingsManager::MossLightTheme);
        QVERIFY(!settings.layoutPresetCustomized());
        QVERIFY(settings.presetSuppliesValue(QStringLiteral("messageSpacing")));

        settings.setMessageSpacing(SettingsManager::kSpacingCompact);
        QCOMPARE(settings.messageSpacing(), SettingsManager::kSpacingCompact);
        QVERIFY(settings.layoutPresetCustomized());
        QVERIFY(!settings.presetSuppliesValue(QStringLiteral("messageSpacing")));
        QVERIFY(settings.presetSuppliesValue(QStringLiteral("messageLayout")));

        settings.setLayoutPreset(0);
        QCOMPARE(settings.messageLayout(), 1);
        QCOMPARE(settings.messageSpacing(), SettingsManager::kSpacingRoomy);
        QCOMPARE(settings.threadPlacement(), 0);
        QVERIFY(!settings.layoutPresetCustomized());
        QVERIFY(!settings.presetSuppliesValue(QStringLiteral("messageLayout")));

        settings.setLayoutPreset(1);
        QCOMPARE(settings.messageSpacing(), SettingsManager::kSpacingCompact);
        settings.resetLayoutPresetOverrides();
        QCOMPARE(settings.messageSpacing(), SettingsManager::kSpacingCozy);
        QVERIFY(!settings.layoutPresetCustomized());
        QCOMPARE(settings.textScale(), 120);
        settings.setThreadPlacement(0);
    }
    {
        SettingsManager settings;
        QCOMPARE(settings.layoutPreset(), 1);
        QCOMPARE(settings.lightningPresentationValue(
                     QStringLiteral("messageLayout")).toInt(), 1);
        QSignalSpy layoutSpy(&settings, &SettingsManager::messageLayoutChanged);
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

void SettingsSessionTest::paneWidthsAreRememberedPerLayoutPreset()
{
    SettingsManager settings;
    QCOMPARE(settings.spacesRailWidth(), 68);
    settings.setRoomListWidth(360);
    settings.setRoomListExpandedWidth(360);
    settings.setSidePanelWidth(400);
    settings.setLayoutPreset(SettingsManager::kLayoutPresetNhekoCompact);
    QCOMPARE(settings.spacesRailWidth(), 66);
    settings.setSpacesRailWidth(62);
    QCOMPARE(settings.spacesRailWidth(), 62);
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
    QCOMPARE(settings.spacesRailWidth(), 68);
    QCOMPARE(settings.roomListWidth(), 360);
    QCOMPARE(settings.roomListExpandedWidth(), 360);
    QCOMPARE(settings.sidePanelWidth(), 400);
    QCOMPARE(listSpy.count(), 3);
    settings.setLayoutPreset(SettingsManager::kLayoutPresetNhekoCompact);
    QCOMPARE(settings.spacesRailWidth(), 62);
    QCOMPARE(settings.roomListWidth(), 64);
    QCOMPARE(settings.roomListExpandedWidth(), 240);
    QCOMPARE(settings.sidePanelWidth(), 400);
    settings.setLayoutPreset(SettingsManager::kLayoutPresetLightning);
    settle(settings);
    SettingsManager reopened;
    QCOMPARE(reopened.roomListWidth(), 360);
    QCOMPARE(reopened.roomListExpandedWidth(), 360);
    reopened.setLayoutPreset(SettingsManager::kLayoutPresetNhekoCompact);
    QCOMPARE(reopened.spacesRailWidth(), 62);
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

    QSignalSpy themeSpy(&settings, &SettingsManager::themeChanged);
    settings.setActiveAccountUserId(bob);
    QVERIFY(themeSpy.count() >= 1);
    QCOMPARE(settings.theme(), SettingsManager::SystemTheme);
    QCOMPARE(settings.messageLayout(), 0);
    QCOMPARE(settings.textScale(), 100);

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

void SettingsSessionTest::oneAccountsAppearanceNeverRestylesAnother()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    settings.setTheme(SettingsManager::NordTheme);

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
    settings.setRoomFilterMode(1);
    settings.setActiveAccountUserId(bob);
    settings.setRoomFilterMode(0);
    QCOMPARE(settings.roomFilterMode(), 0);

    QSignalSpy filterSpy(&settings, &SettingsManager::roomFilterModeChanged);
    settings.setActiveAccountUserId(alice);
    QCOMPARE(settings.roomFilterMode(), 1);
    QVERIFY2(filterSpy.count() >= 1,
             "an account switch did not re-announce roomFilterMode, so the "
             "room list keeps the previous account's filter");

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

    settings.setUiFont(QStringLiteral("Comic Sans MS"));
    QCOMPARE(settings.uiFont(), QStringLiteral("Comic Sans MS"));
    settings.setUiFont(QStringLiteral("evil\"; color:red }"));
    QCOMPARE(settings.uiFont(), QStringLiteral("Manrope"));

    settings.setUiFont(QStringLiteral("Plus Jakarta Sans"));

    settings.setActiveAccountUserId(bob);
    settings.setUiFont(QStringLiteral("IBM Plex Sans"));
    QCOMPARE(settings.uiFont(), QStringLiteral("IBM Plex Sans"));
    fontSpy.clear();
    settings.setActiveAccountUserId(alice);
    QVERIFY(fontSpy.count() >= 1);
    QCOMPARE(settings.uiFont(), QStringLiteral("Plus Jakarta Sans"));
    settings.setActiveAccountUserId(bob);
    QCOMPARE(settings.uiFont(), QStringLiteral("IBM Plex Sans"));

    SettingsManager reopened;
    reopened.setSecretStore(&secrets);
    QCOMPARE(reopened.uiFont(), QStringLiteral("IBM Plex Sans"));
}

void SettingsSessionTest::loginHomeserverPrefillIsAccountIndependent()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);

    QCOMPARE(settings.loginHomeserverPrefill(),
             QStringLiteral("https://matrix.org"));

    const QString alice = QStringLiteral("@alice:matrix.example");
    settings.saveSession(QStringLiteral("https://matrix.example"), alice,
                         QStringLiteral("ALICEDEVICE"),
                         QStringLiteral("alice-token-fixture"));
    settings.setActiveAccountUserId(alice);
    QCOMPARE(settings.homeserverUrl(),
             QStringLiteral("https://matrix.example"));

    QSignalSpy spy(&settings,
                   &SettingsManager::loginHomeserverPrefillChanged);
    settings.setLoginHomeserverPrefill(
        QStringLiteral("https://other.example"));
    QCOMPARE(settings.loginHomeserverPrefill(),
             QStringLiteral("https://other.example"));
    QVERIFY(spy.count() >= 1);
    QCOMPARE(settings.homeserverUrl(),
             QStringLiteral("https://matrix.example"));
}

void SettingsSessionTest::freshProfileDefaultsToMatrixOrg()
{
    SettingsManager settings;
    QCOMPARE(settings.homeserverUrl(), QStringLiteral("https://matrix.org"));
    QCOMPARE(settings.loginHomeserverPrefill(),
             QStringLiteral("https://matrix.org"));
    QVERIFY(!settings.homeserverUrl().contains(QStringLiteral("smetonis")));
    QVERIFY(!settings.loginHomeserverPrefill()
                 .contains(QStringLiteral("smetonis")));
}

void SettingsSessionTest::pickerSizeIsWhitelistedBoundedAndForgettable()
{
    SettingsManager s;

    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 0);
    QCOMPARE(s.pickerHeightShare(QStringLiteral("picker")), 0);

    s.setPickerShare(QStringLiteral("picker"), 420, 640);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 420);
    QCOMPARE(s.pickerHeightShare(QStringLiteral("picker")), 640);

    s.setPickerShare(QStringLiteral("gif"), 300, 500);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("gif")), 300);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 420);
    s.setPickerShare(QStringLiteral("reaction"), 190, 400);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("reaction")), 190);
    QCOMPARE(s.pickerHeightShare(QStringLiteral("reaction")), 400);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 420);

    s.setPickerShare(QStringLiteral("../../secret"), 500, 500);
    s.setPickerShare(QStringLiteral("picker/../gif"), 500, 500);
    s.setPickerShare(QStringLiteral("PICKER"), 500, 500);
    s.setPickerShare(QString(), 500, 500);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("../../secret")), 0);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("PICKER")), 0);
    QCOMPARE(s.pickerWidthShare(QString()), 0);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 420);

    s.setPickerShare(QStringLiteral("picker"), 49, 640);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 0);
    QCOMPARE(s.pickerHeightShare(QStringLiteral("picker")), 0);
    s.setPickerShare(QStringLiteral("picker"), 420, 1001);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 0);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("gif")), 300);

    s.setPickerShare(QStringLiteral("picker"), 50, 1000);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 50);
    QCOMPARE(s.pickerHeightShare(QStringLiteral("picker")), 1000);

    {
        SettingsManager reloaded;
        QCOMPARE(reloaded.pickerWidthShare(QStringLiteral("picker")), 50);
        QCOMPARE(reloaded.pickerHeightShare(QStringLiteral("picker")), 1000);
    }
}

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
        SettingsManager reopened;
        QCOMPARE(reopened.initialWindowGeometry(), QRect(140, 90, 1280, 800));
        QVERIFY(reopened.initialWindowMaximized());
    }

    {
        SettingsManager settings;
        settings.saveWindowGeometry(0, 0, 0, 0);
        settings.saveWindowGeometry(7, 7, 320, 240);
    }
    {
        SettingsManager reopened;
        QCOMPARE(reopened.initialWindowGeometry(), QRect(140, 90, 1280, 800));
    }

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

void SettingsSessionTest::hiddenImagesPersistPerAccountAndSurviveSigningOut()
{
    const QString key = QStringLiteral("$evt-persist:example.org");

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

        store.resetForSession();
        QVERIFY(!store.isHidden(key));
        QVERIFY2(settings.hiddenMediaKeys().contains(key),
                 "signing out ERASED the account's hidden images");
    }

    {
        SettingsManager settings;
        MediaVisibilityStore store;
        store.setSettings(&settings);
        QVERIFY2(store.isHidden(key),
                 "the image did not stay hidden across a restart");

        store.clear();
        QVERIFY(!store.isHidden(key));
        QVERIFY2(settings.hiddenMediaKeys().isEmpty(),
                 "Show all hidden images left the stored list behind");
    }
}

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

    settings.setComposerButtonShown(QStringLiteral("emoji"), false);
    QCOMPARE(spy.count(), 1);

    settings.setHiddenComposerButtons({ QStringLiteral("emoji"),
                                        QStringLiteral("emoji"),
                                        QString() });
    QCOMPARE(spy.count(), 1);
    settings.setHiddenComposerButtons({ QStringLiteral("voice"),
                                        QStringLiteral("emoji") });
    QCOMPARE(spy.count(), 2);
    QCOMPARE(settings.hiddenComposerButtons(),
             (QStringList{ QStringLiteral("emoji"), QStringLiteral("voice") }));

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

void SettingsSessionTest::strictDeviceTrustDefaultsOffAndPersists()
{
    {
        SettingsManager settings;
        QVERIFY(!settings.strictDeviceTrust());

        QSignalSpy spy(&settings, &SettingsManager::strictDeviceTrustChanged);
        settings.setStrictDeviceTrust(true);
        QVERIFY(settings.strictDeviceTrust());
        QCOMPARE(spy.count(), 1);
        settings.setStrictDeviceTrust(true);
        QCOMPARE(spy.count(), 1);
    }
    SettingsManager reopened;
    QVERIFY(reopened.strictDeviceTrust());
}

void SettingsSessionTest::noiseSuppressionDefaultsToOffPersistsAndFallsBack()
{
    {
        SettingsManager settings;
        QCOMPARE(settings.noiseSuppressionMode(), QStringLiteral("off"));

        QSignalSpy spy(&settings, &SettingsManager::noiseSuppressionModeChanged);
        settings.setNoiseSuppressionMode(QStringLiteral("rnnoise"));
        QCOMPARE(settings.noiseSuppressionMode(), QStringLiteral("rnnoise"));
        QCOMPARE(spy.count(), 1);
        settings.setNoiseSuppressionMode(QStringLiteral("rnnoise"));
        QCOMPARE(spy.count(), 1);
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
        QVERIFY(settings.keepMediaOnDevice());

        QSignalSpy spy(&settings, &SettingsManager::keepMediaOnDeviceChanged);
        settings.setKeepMediaOnDevice(false);
        QVERIFY(!settings.keepMediaOnDevice());
        QCOMPARE(spy.count(), 1);
        settings.setKeepMediaOnDevice(false);
        QCOMPARE(spy.count(), 1);
    }
    SettingsManager reopened;
    QVERIFY(!reopened.keepMediaOnDevice());
}

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
    QVERIFY2(text.contains(QLatin1String("desktop with no system keyring")),
             qPrintable(text));
    QVERIFY2(!text.contains(QLatin1String("locked or missing, nothing is kept")),
             qPrintable(text));
    QVERIFY2(text.contains(QLatin1String("encrypted rooms are downloaded again each session")),
             qPrintable(text));
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
        QCOMPARE(settings.readReceiptMode(), 0);

        QSignalSpy spy(&settings, &SettingsManager::readReceiptModeChanged);
        settings.setReadReceiptMode(1);
        QCOMPARE(settings.readReceiptMode(), 1);
        QCOMPARE(spy.count(), 1);
        settings.setReadReceiptMode(1);
        QCOMPARE(spy.count(), 1);

        settings.setReadReceiptMode(2);
        QCOMPARE(settings.readReceiptMode(), 1);
        QCOMPARE(spy.count(), 1);
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

void SettingsSessionTest::anOlderBuildsDoNotSendModeReadsAsPrivate()
{
    {
        QSettings raw;
        raw.setValue(QStringLiteral("privacy/readReceiptMode"), 2);
        raw.sync();
    }
    SettingsManager settings;
    QCOMPARE(settings.readReceiptMode(), 1);
    QSignalSpy spy(&settings, &SettingsManager::readReceiptModeChanged);
    settings.setReadReceiptMode(1);
    QCOMPARE(spy.count(), 0);
    settings.setReadReceiptMode(0);
    QCOMPARE(settings.readReceiptMode(), 0);
    QCOMPARE(spy.count(), 1);
}

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
        QCOMPARE(settings.notificationPreview(), 0);
    }
    {
        QSettings seed;
        seed.setValue(QStringLiteral("notifications/preview"), 7);
        seed.sync();
    }
    SettingsManager settings;
    QCOMPARE(settings.notificationPreview(), 1);
    settings.setNotificationPreview(7);
    QCOMPARE(settings.notificationPreview(), 1);
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
    QCOMPARE(settings.notificationPreviewEncrypted(), 3);
    settings.setNotificationPreview(0);

    QCOMPARE(settings.effectiveNotificationPreview(true, true), 0);
    QCOMPARE(settings.effectiveNotificationPreview(false, true), 0);

    settings.setNotificationPreviewEncrypted(2);
    QCOMPARE(settings.effectiveNotificationPreview(true, true), 2);
    QCOMPARE(settings.effectiveNotificationPreview(false, true), 0);
}

void SettingsSessionTest::anUnknownEncryptionStateTakesTheStricterLevel()
{
    SettingsManager settings;
    settings.setNotificationPreview(0);
    settings.setNotificationPreviewEncrypted(2);

    QCOMPARE(settings.effectiveNotificationPreview(false, false), 2);
    QCOMPARE(settings.effectiveNotificationPreview(true, false), 2);

    settings.setNotificationPreview(2);
    settings.setNotificationPreviewEncrypted(0);
    QCOMPARE(settings.effectiveNotificationPreview(false, false), 2);
    QCOMPARE(settings.effectiveNotificationPreview(true, true), 0);
}

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

    settings.setActiveAccountUserId(alice);
    settings.setReducedMotion(true);
    settings.setSmoothScrolling(false);
    settings.setHiddenComposerButtons(QStringList{ QStringLiteral("gif") });
    settings.setClockFormat(2);
    settings.setMicrophoneGain(140);

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

void SettingsSessionTest::everyAccountScopedGetterHasItsSignalInTheAccountSwitch()
{
    QFile file(QStringLiteral(REPO_ROOT "/src/app/SettingsManager.cpp"));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(file.errorString()));
    const QStringList lines =
        QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));

    QString switchBody;
    bool inSwitch = false;
    int depth = 0;
    QStringList scopedGetters;
    QString currentGetter;
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

    startSpy.clear();
    settings.setCloseToTray(true);
    QCOMPARE(settings.startInTray(), true);
    QVERIFY(startSpy.count() >= 1);
}

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

    const QString gain =
        body(QStringLiteral("void SettingsManager::setMicrophoneGain("));
    QVERIFY2(!gain.isEmpty(), "setMicrophoneGain is gone or was renamed");
    QVERIFY2(gain.contains(QStringLiteral("setAppearanceValue")),
             "the microphone level is no longer account-scoped, so the Sound "
             "card's sentence now says the wrong thing about it");

    const QString device =
        body(QStringLiteral("void SettingsManager::setPreferredMicrophoneId("));
    QVERIFY2(!device.isEmpty(),
             "setPreferredMicrophoneId is gone or was renamed");
    QVERIFY2(!device.contains(QStringLiteral("setAppearanceValue")),
             "the microphone DEVICE became account-scoped, so the Sound "
             "card's sentence now says the wrong thing about it");

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
    settings.setSecretStore(&secrets);

    QCOMPARE(secrets.readSecret(kept, QStringLiteral("refreshToken")),
             QStringLiteral("rotated-refresh-fixture"));
    QCOMPARE(secrets.readSecret(kept, QStringLiteral("accessToken")),
             QStringLiteral("kept-access-fixture"));
    QSettings check;
    QVERIFY(check.contains(
        QStringLiteral("secrets/%1/aKeyFromANewerBuild").arg(kept)));
    QCOMPARE(secrets.readSecret(moved, QStringLiteral("accessToken")),
             QStringLiteral("newer-access-fixture"));
    QVERIFY(!check.contains(QStringLiteral("secrets/%1/accessToken").arg(moved)));
}

void SettingsSessionTest::secretsWrittenHereOnlyForThisInstallsOwnSignIn()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    const QString own = QStringLiteral("@own:matrix.example");
    settings.saveSession(QStringLiteral("https://matrix.example"), own,
                         QStringLiteral("OWNDEVICE"), QStringLiteral("token-fixture"));
    QVERIFY(settings.secretsWrittenHereForRecordedDevice(own));
    QVERIFY(secrets.clearAccountSecrets(own));
    QVERIFY(settings.secretsWrittenHereForRecordedDevice(own));

    const QString slug = matrix::app_data::safeUserSlug(own);
    QSettings raw;
    raw.setValue(QStringLiteral("accounts/%1/deviceId").arg(slug),
                 QStringLiteral("NEWERDEVICE"));
    raw.sync();
    QVERIFY(!settings.secretsWrittenHereForRecordedDevice(own));
    raw.setValue(QStringLiteral("accounts/%1/deviceId").arg(slug),
                 QStringLiteral("OWNDEVICE"));
    raw.setValue(QStringLiteral("accounts/%1/keyringItems").arg(slug),
                 QStringLiteral("adopted-unverified"));
    raw.sync();
    QVERIFY(!settings.secretsWrittenHereForRecordedDevice(own));

    const QString older = QStringLiteral("@older:matrix.example");
    seedAccountRecord(older, QStringLiteral("https://matrix.example"));
    QVERIFY(!settings.secretsWrittenHereForRecordedDevice(older));
    QVERIFY(!settings.secretsWrittenHereForRecordedDevice(
        QStringLiteral("@nobody:matrix.example")));
}

namespace {

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
    a.unsynced = roomId.isEmpty() ? false : s.contains(unsyncedKey);
    if (roomId.isEmpty()) {
        a.base = -1;
    } else {
        bool ok = false;
        const int base = s.value(unsyncedKey).toInt(&ok);
        a.base = (ok && base >= 0 && base <= 3) ? base : -1;
    }
    if (roomId.isEmpty()) {
        a.deviceOnly = false;
    } else if (!scopedKey.isEmpty() && s.contains(scopedKey)) {
        a.deviceOnly = false;
    } else {
        const int legacy = s.value(globalKey, 0).toInt();
        a.deviceOnly = legacy == 1 || legacy == 2;
    }
    a.stored = roomId.isEmpty()
        ? false
        : ((!scopedKey.isEmpty() && s.contains(scopedKey))
           || s.contains(globalKey));
    if (roomId.isEmpty()) {
        a.mode = 0;
    } else {
        const QString readKey = (!scopedKey.isEmpty() && s.contains(scopedKey))
            ? scopedKey
            : globalKey;
        const int mode = s.value(readKey, 0).toInt();
        a.mode = (mode < 0 || mode > 3) ? 0 : mode;
    }
    a.unread = (a.mode == roomunread::kModeMentions
                && (a.deviceOnly || a.unsynced))
        ? roomunread::kModeMentionsUnverified
        : a.mode;
    a.decidesLocally = a.unsynced || a.deviceOnly;
    return a;
}

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

QStringList modeGridRooms()
{
    QStringList rooms;
    for (int i = 0; i < kGridRooms; ++i)
        rooms.append(gridRoom(i));
    rooms.append(QString());
    return rooms;
}

}

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
    QVERIFY2(differFromAlice > kGridRooms / 2,
             qPrintable(QStringLiteral("only %1 of %2 rooms differ between "
                                       "Alice and Bob; the fixture no longer "
                                       "separates the scopes")
                            .arg(differFromAlice)
                            .arg(kGridRooms)));

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
        QVERIFY(forAnswers(settings, alice, room)
                == oldRuleAnswers(raw, aliceSlug, room));
    }
}

void SettingsSessionTest::perAccountModeReadersUseTheDeviceScopeForAnEmptyOrUnknownUser()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    const QString alice = QStringLiteral("@alice:matrix.example");
    const QString bob = QStringLiteral("@bob:matrix.example");
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
    QVERIFY2(differFromActive > int(strangers.size()) * kGridRooms / 2,
             qPrintable(QStringLiteral("only %1 answers differ between the "
                                       "device scope and the active account")
                            .arg(differFromActive)));

    settings.setActiveAccountUserId(QString());
    for (const QString &room : rooms)
        QVERIFY(forAnswers(settings, QStringLiteral("@nobody:matrix.example"),
                           room)
                == activeAnswers(settings, room));
}

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
    SettingsManager reopened;
    reopened.setSecretStore(&secrets);
    QList<matrix::app_data::AccountIdentity> pending = reopened.pendingSignOuts();
    QCOMPARE(pending.size(), 1);
    QCOMPARE(pending.first().userId, me);
    QCOMPARE(QDir::cleanPath(pending.first().rustStorePath),
             QDir::cleanPath(recorded.rustStorePath));
    QCOMPARE(pending.first().effectiveStoreSlug(), recorded.effectiveStoreSlug());

    reopened.clearPendingSignOut(me);
    QVERIFY(reopened.pendingSignOuts().isEmpty());

    reopened.recordPendingSignOut(recorded);
    QCOMPARE(reopened.pendingSignOuts().size(), 1);
    reopened.saveSession(hs, me, QStringLiteral("DEV2"), QStringLiteral("t2-fixture"));
    QVERIFY(reopened.pendingSignOuts().isEmpty());
    reopened.recordPendingSignOut(recorded);
    reopened.clearPendingSignOut(QStringLiteral("@other:localhs.test"));
    QCOMPARE(reopened.pendingSignOuts().size(), 1);
}

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
    QVERIFY(!QSettings().contains(QStringLiteral("pendingSignOut/") + aliceIdentity.slug
                                  + QStringLiteral("/userId")));

    settings.recordPendingSignOut(aliceIdentity);
    QVERIFY(settings.clearSessionForAccount(alice));
    settings.setStoreSlugFor(bob, aliceIdentity.effectiveStoreSlug());
    QCOMPARE(settings.accountOwningStoreSlug(aliceIdentity.effectiveStoreSlug()), bob);
    QVERIFY2(settings.pendingSignOuts().isEmpty(),
             "a pending sign-out named a store another account owns");
}

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

    matrix::app_data::AccountIdentity lost;
    QVERIFY(matrix::app_data::resolveAccountIdentity(
        QStringLiteral("https://localhs.test"), QStringLiteral("r6lost"), &lost));
    QVERIFY(QDir().mkpath(lost.rustStorePath));
    {
        SettingsManager damaged;
        QCOMPARE(damaged.settingsReadProblem(), int(SettingsManager::AccountsMissing));
    }
    QVERIFY(QDir(QFileInfo(lost.rustStorePath).absolutePath()).removeRecursively());

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
        QVERIFY(QSettings().contains(key));
        QVERIFY(QDir().mkpath(attempt.rustStorePath));
        settings.clearPendingSignIn(attempt.userId + QStringLiteral("x"));
        QVERIFY(QSettings().contains(key));
        settings.saveSession(attempt.homeserver, attempt.userId, QStringLiteral("D"),
                             QStringLiteral("d-fixture"));
        QVERIFY2(!QSettings().contains(key), "a finished sign-in kept its marker");
    }

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
        QVERIFY2(QSettings().contains(key), "the fixture is not a stand-in case");
        settings.setStoreSlugFor(canonical, typed.effectiveStoreSlug());
        QVERIFY2(!QSettings().contains(key),
                 "a finished stand-in sign-in kept its marker");

        settings.recordPendingSignIn(typed);
        QVERIFY(QSettings().contains(key));
        settings.clearPendingSignIn(canonical);
        QVERIFY(QSettings().contains(key));
        settings.clearPendingSignIn(typed);
        QVERIFY(!QSettings().contains(key));
    }
}

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
        QFile partial(path + QStringLiteral(".kIlLd1"));
        QVERIFY(partial.open(QIODevice::WriteOnly | QIODevice::Truncate));
        partial.write(good.left(good.size() / 2));
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
    QSettings writer;
    writer.setValue(QStringLiteral("r6/afterTheKill"), true);
    writer.sync();
    QCOMPARE(writer.status(), QSettings::NoError);
    QVERIFY(QSettings().value(QStringLiteral("r6/afterTheKill")).toBool());
}

QTEST_MAIN(SettingsSessionTest)
#include "SettingsSessionTest.moc"
