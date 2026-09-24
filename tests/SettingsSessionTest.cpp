#include "app/SettingsManager.h"
#include "media/MediaVisibilityStore.h"
#include "storage/AppDataPaths.h"
#include "storage/SecretStore.h"

#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include <utility>

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
    void aParticipantVolumeReachesTheDiskImmediately();
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
    void leavesPlaintextWhenNoSavedAccountOwnsTheGroup();
    void insecureSecretsGroupFoldingIsStillTwoCharacters();
    // Device-global keys naming rooms and Spaces are removed with the last
    // account.
    void clearingTheLastAccountSweepsTheDeviceGlobalRoomKeys();
    void validThemeIdsRoundTripAndPersist();
    void unknownStoredThemeFallsBackToSystem();
    void themeChangeEmitsSignal();
    void previewDefaultsAndEncryptedOff();
    void shareQualityDefaultsPersistAndSnap();
    void roomActivityDefaultsEnabledAndPersists();
    void wheelSpeedDefaultsToFastPersistsAndFallsBack();
    void gifPolicyDefaultsPersistAndClamp();
    void messageLayoutAndTextScalePersistAndClamp();
    void interfaceZoomAndRoomFilterPersistAndClamp();
    void spacesRailWidthDefaultsToTheOldFixedWidthAndClamps();
    void spacesRailDepthStyleDefaultsToRegionsAndClamps();
    void appearanceIsPerAccountWithGlobalFallback();
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
    void typingNotificationsDefaultOnAndPersist();
    void theEncryptedPreviewLevelDefaultsToFollowingTheGeneralOne();
    void anUnknownEncryptionStateTakesTheStricterLevel();
    void strictDeviceTrustDefaultsOffAndPersists();
    // Every account-scoped value is re-announced on an account switch.
    void switchingAccountsReAnnouncesEveryAccountScopedAppearanceValue();
    void everyAccountScopedGetterHasItsSignalInTheAccountSwitch();
    void theSoundSectionsScopeSentenceMatchesWhereThingsActuallyLive();
    void turningOffCloseToTrayAnnouncesTheStartInTrayItDerives();

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

// A participant volume reaches the settings file immediately (explicit
// sync()): QSettings writes lazily, and calls are when the client is least
// likely to exit cleanly. Asserted against the file, since reading back
// through QSettings would be answered from its cache.
void SettingsSessionTest::aParticipantVolumeReachesTheDiskImmediately()
{
    FakeSecretStore secrets;
    SettingsManager settings;
    settings.setSecretStore(&secrets);
    const QString me = QStringLiteral("@alice:matrix.example");
    settings.saveSession(QStringLiteral("https://matrix.example"), me,
                         QStringLiteral("ALICEDEVICE"),
                         QStringLiteral("alice-token-fixture"));

    QFile file(QSettings().fileName());   // path resolved BEFORE the write

    const QString other = QStringLiteral("@bob:matrix.example");
    settings.setCallParticipantVolume(other, 47);
    QCOMPARE(settings.callParticipantVolume(other), 47);

    // Resolve the path before the write: constructing a QSettings syncs the
    // shared per-path QConfFile, so asking afterwards would flush the very
    // write under test.
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
    const QString onDisk = QString::fromUtf8(file.readAll());
    // The value, under the right account, not merely the group name.
    QVERIFY2(onDisk.contains(QStringLiteral("=47")),
             qPrintable(QStringLiteral("volume not on disk; file said:\n%1")
                            .arg(onDisk)));
    QVERIFY(onDisk.contains(QStringLiteral("callVolumes")));
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
    QCOMPARE(settings.shareMaxHeight(), 1080);
    QCOMPARE(settings.shareFps(), 30);

    // Snapped on read as well as write: an out-of-range value would reach a
    // GStreamer caps string.
    QSettings raw;
    raw.setValue(QStringLiteral("calls/shareMaxHeight"), 4320);
    raw.setValue(QStringLiteral("calls/shareFps"), 240);
    raw.sync();
    SettingsManager reread;
    QCOMPARE(reread.shareMaxHeight(), 2160);
    QCOMPARE(reread.shareFps(), 60);

    raw.setValue(QStringLiteral("calls/shareMaxHeight"), 0);
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
        settings.setRoomFilterMode(3);
    }
    SettingsManager reopened;
    QCOMPARE(reopened.interfaceZoom(), 110);
    QCOMPARE(reopened.roomFilterMode(), 3);
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
    // Bob has no explicit choice yet: he inherits the global fallback (the
    // most recent selection), not a stale per-account value.
    QCOMPARE(settings.theme(), SettingsManager::MossLightTheme);

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

        settings.setReadReceiptMode(2);
        QCOMPARE(settings.readReceiptMode(), 2);
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

QTEST_MAIN(SettingsSessionTest)
#include "SettingsSessionTest.moc"
