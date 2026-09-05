#include "app/SettingsManager.h"
#include "media/MediaVisibilityStore.h"
#include "storage/SecretStore.h"

#include <QFile>
#include <QHash>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

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
    void clearsOnlySelectedAccount();
    void clearsMetadataWhenTokenIsAlreadyMissing();
    void normalizedIdentityClearsLegacyKey();
    void reportsSecretCleanupFailureButClearsMetadata();
    void migratesInsecureSecretsGroupIntoSecureStore();
    void keepsPlaintextWhenSecureMigrationFails();
    // v0.5.11.
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
    void appearanceIsPerAccountWithGlobalFallback();
    void switchingAccountsReAnnouncesTheRoomListFilter();
    void uiFontPersistsPerAccountAndValidates();
    void loginHomeserverPrefillIsAccountIndependent();
    void hiddenImagesPersistPerAccountAndSurviveSigningOut();
    // v0.6.7.
    void pickerSizeIsWhitelistedBoundedAndForgettable();
    void freshProfileDefaultsToMatrixOrg();
    // 2026-08-23 tester report: window geometry was not saved at all.
    void windowGeometryRoundTripsAndRefusesAnUnrestorableSize();
    // 2026-09-03: composer buttons the user switched off.
    void hiddenComposerButtonsDefaultToNoneAndNormalize();
    // 2026-09-05: what this device discloses while merely reading and typing.
    void readReceiptModeDefaultsToPublicPersistsAndClamps();
    void typingNotificationsDefaultOnAndPersist();
    void theEncryptedPreviewLevelDefaultsToFollowingTheGeneralOne();
    void anUnknownEncryptionStateTakesTheStricterLevel();
    void strictDeviceTrustDefaultsOffAndPersists();

private:
    QTemporaryDir m_configHome;
};

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
    // Simulate a prior InsecureFallback run: a plaintext token under
    // secrets/<user>/accessToken in QSettings.
    const QString user = QStringLiteral("@alice:matrix.example");
    const QString token = QStringLiteral("plaintext-token-fixture");
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

void SettingsSessionTest::shareQualityDefaultsPersistAndSnap()
{
    SettingsManager settings;

    // The defaults are what the share has always sent, so an existing user
    // sees no change until they choose something.
    QCOMPARE(settings.shareMaxHeight(), 1080);
    QCOMPARE(settings.shareFps(), 30);

    // SNAPPED ON READ as well as on write. The store is a file a user can
    // edit, and an out-of-range value would reach a GStreamer caps string --
    // 4320 would ask the encoder for 8K on every frame, 0 would negotiate
    // nothing at all.
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

    // Round trip, and the signal fires once per real change.
    //
    // A FRESH manager on a KNOWN value: the raw writes above went into the
    // same store this test's first `settings` reads, so it is already
    // holding 720 by now and asserting a change to 720 would be asserting a
    // correct no-op. Getting that wrong is how a test comes to blame the
    // code for its own setup.
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
    // ...and an out-of-set value that SNAPS to the value already held is
    // also a no-op, which is the case a naive guard misses.
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
    // BOTH preview defaults are OFF. The fetch is client-side — not through
    // the homeserver's proxy — so an automatic preview hands the reader's IP
    // and read timing to a host the SENDER chose, with no action by the
    // reader. In an encrypted room it additionally leaks that a link was
    // followed at all, which the room was otherwise keeping.
    //
    // THIS TEST FAILED TO DO ITS JOB ONCE and the failure mode is worth
    // naming: the default was flipped to ON and these assertions were edited
    // to match, leaving the case still called ...EncryptedOff with a comment
    // saying previews stay off. A test edited to agree with a change defends
    // nothing. Hence the docs coupling below — a source file and a published
    // promise now have to be changed together.
    //
    // GIF animation of already-received media is a different question (no
    // third party is contacted) and stays ON.
    QCOMPARE(settings.loadPreviewsInEncryptedRooms(), false);
    QCOMPARE(settings.autoLoadLinkPreviews(), false);
    QCOMPARE(settings.animateGifPreviews(), true);

    // docs/privacy.md documents this default in its own heading and in a
    // defaults table, and the code-signing argument in that file rests on
    // it. If the default ever changes deliberately, that document changes in
    // the same commit -- and this assertion is what forces the pairing.
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

    // Driven AWAY from the default and back: writing the value it already
    // holds is a correct no-op that emits nothing.
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
        // Defaults: autoplay Always (0, follows animateGifPreviews=true),
        // safe-search PG-13 (2), recents on, provider giphy.
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

// 2026-08-14: interface zoom (global, startup-applied via QT_SCALE_FACTOR)
// and the room-list filter chips (per-account appearance state).
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
    // Bob has no explicit choice yet: he inherits the global fallback
    // (the most recent selection), not a stale per-account value.
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

// The room-list filter is account-scoped appearance state like the theme, and
// it was the ONE such value missing from the switch's re-announcement. That is
// not cosmetic: the chips write this setting and the model follows it through a
// binding, so without the notify the switched-to account's list keeps
// filtering by the previous account's choice while the chips report it as
// current — and clicking the chip whose stored value already matches is then a
// silent no-op, because the setter returns early on an unchanged value. Which
// is exactly "sometimes you can't click All, sometimes the filter shows
// nothing, especially if the account is switched".
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

    // Back to Alice, whose stored answer differs. The signal is what makes
    // the model re-read; without it the model keeps Bob's filter.
    QSignalSpy filterSpy(&settings, &SettingsManager::roomFilterModeChanged);
    settings.setActiveAccountUserId(alice);
    QCOMPARE(settings.roomFilterMode(), 1);
    QVERIFY2(filterSpy.count() >= 1,
             "an account switch did not re-announce roomFilterMode, so the "
             "room list keeps the previous account's filter");

    // And the other direction, which is the case that made a chip click a
    // no-op: Bob's stored value is 0, so a click on All can only work if the
    // switch already told the model to go back to 0.
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

    // A family this build does not bundle DOES persist now: fonts became
    // user-selectable from everything the host has installed, and this class
    // (Qt6::Core only, by ~20 test targets) cannot ask whether a font exists.
    // FontManager resolves the name and falls back without rewriting it.
    settings.setUiFont(QStringLiteral("Comic Sans MS"));
    QCOMPARE(settings.uiFont(), QStringLiteral("Comic Sans MS"));
    // What still never persists is a name that is not a name.
    settings.setUiFont(QStringLiteral("evil\"; color:red }"));
    QCOMPARE(settings.uiFont(), QStringLiteral("Manrope"));

    settings.setUiFont(QStringLiteral("Plus Jakarta Sans"));

    // Per-account: Bob keeps his own selection; switching re-announces so
    // the UI re-reads, and Alice's choice survives the round trip.
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

// The login-screen homeserver field must be freely editable during the
// add-account flow (which keeps the current account active). homeserverUrl()
// follows the ACTIVE account, so binding the field to it reverted every
// keystroke back to "your own" server. loginHomeserverPrefill reads/writes
// the account-independent global key, so the typed value sticks and the
// active account's stored server is left untouched.
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

    // ...but the login field can be pointed at a different homeserver and it
    // STICKS (getter/setter share the global key) rather than reverting.
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

// A brand-new profile (fresh QSettings; init() clears it before each test)
// must present matrix.org as the homeserver — never a developer/personal
// server — for BOTH the account-independent login prefill and the effective
// homeserverUrl(). This is independent of environment variables and any local
// key file: SettingsManager seeds the neutral default in its constructor.
void SettingsSessionTest::freshProfileDefaultsToMatrixOrg()
{
    SettingsManager settings;
    QCOMPARE(settings.homeserverUrl(), QStringLiteral("https://matrix.org"));
    QCOMPARE(settings.loginHomeserverPrefill(),
             QStringLiteral("https://matrix.org"));
    // No personal/developer server ever leaks in as a first-run default.
    QVERIFY(!settings.homeserverUrl().contains(QStringLiteral("smetonis")));
    QVERIFY(!settings.loginHomeserverPrefill()
                 .contains(QStringLiteral("smetonis")));
}

// v0.6.7: the remembered size of a user-resizable overlay picker, stored as a
// SHARE of the space available to it rather than as pixels — which is what
// makes the picker track the window, keeps a size sensible across displays,
// and lets both pickers remember ONE value under the shared "picker" id.
// This is the whole QML-reachable surface of that feature.
void SettingsSessionTest::pickerSizeIsWhitelistedBoundedAndForgettable()
{
    SettingsManager s;

    // Never resized: 0 means "use the component's default share".
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 0);
    QCOMPARE(s.pickerHeightShare(QStringLiteral("picker")), 0);

    // A normal round trip. Both pickers pass the same id, so this IS the
    // sync between them — there is no second value to keep in step.
    s.setPickerShare(QStringLiteral("picker"), 420, 640);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 420);
    QCOMPARE(s.pickerHeightShare(QStringLiteral("picker")), 640);

    // Per-picker ids stay accepted and independent, so a future surface can
    // opt out of the shared value.
    s.setPickerShare(QStringLiteral("gif"), 300, 500);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("gif")), 300);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 420);

    // The id is a WHITELIST, not a sanitizer: an unknown id reads 0 and
    // writes nothing at all, so a QML caller cannot compose a settings key
    // out of text it controls.
    s.setPickerShare(QStringLiteral("../../secret"), 500, 500);
    s.setPickerShare(QStringLiteral("picker/../gif"), 500, 500);
    s.setPickerShare(QStringLiteral("PICKER"), 500, 500);
    s.setPickerShare(QString(), 500, 500);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("../../secret")), 0);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("PICKER")), 0);
    QCOMPARE(s.pickerWidthShare(QString()), 0);
    // ...and none of those disturbed a real entry.
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 420);

    // Out of range is FORGOTTEN, never stored: a share below the floor would
    // be unusable and one above 1000 would exceed the room the picker has.
    // "Forget" restores the component default rather than clamping to
    // something the user never chose.
    s.setPickerShare(QStringLiteral("picker"), 49, 640);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 0);
    QCOMPARE(s.pickerHeightShare(QStringLiteral("picker")), 0);
    s.setPickerShare(QStringLiteral("picker"), 420, 1001);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 0);
    // The other id is untouched by its neighbour being forgotten.
    QCOMPARE(s.pickerWidthShare(QStringLiteral("gif")), 300);

    // Exact bounds are accepted.
    s.setPickerShare(QStringLiteral("picker"), 50, 1000);
    QCOMPARE(s.pickerWidthShare(QStringLiteral("picker")), 50);
    QCOMPARE(s.pickerHeightShare(QStringLiteral("picker")), 1000);

    // And it survives a restart — the "saved for later sessions" guarantee.
    {
        SettingsManager reloaded;
        QCOMPARE(reloaded.pickerWidthShare(QStringLiteral("picker")), 50);
        QCOMPARE(reloaded.pickerHeightShare(QStringLiteral("picker")), 1000);
    }
}

// 2026-08-23 tester report: "Window geometry and position is not saved."
//
// Two invariants beyond the round trip, both of which cost a user their window
// if they slip. An UNSET geometry must read back INVALID, not (0,0,0,0)
// treated as a real position — that is how the window knows to use its own
// default. And a size below the window's own minimum must be REFUSED on
// write, because Qt reports transient 0x0 and 1x1 geometry while a window is
// being shown, hidden into the tray or restored from minimized: storing one of
// those would overwrite the last good value with one that can never be
// restored, and the close-to-tray path fires at exactly that moment.
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

    // A negative position is legitimate — a monitor left of the primary one —
    // and must survive. Whether it is still reachable is judged later, against
    // the live display layout (AppController::restorableWindowGeometry).
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

// Hidden images survive a restart, are scoped to the ACCOUNT, and — the part
// that is easy to get catastrophically wrong — are NOT erased by signing out.
//
// clear() is the user's "Show all hidden images" and it persists an empty
// list. The sign-out path called it back when this state was session-only;
// leaving that call in place after adding persistence would have wiped the
// account's hidden images on its own sign-out. resetForSession() exists as a
// separate verb for exactly that reason.
void SettingsSessionTest::hiddenImagesPersistPerAccountAndSurviveSigningOut()
{
    const QString key = QStringLiteral("$evt-persist:example.org");

    // No account resolves yet, so the storage is inert BY DESIGN — asserted
    // rather than assumed, because a fixture that silently persists nothing
    // would make everything below pass for the wrong reason.
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

        // Sign-out drops it from THIS session and keeps it on disk.
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

        // "Show all hidden images" is a real reset that leaves no row behind.
        store.clear();
        QVERIFY(!store.isHidden(key));
        QVERIFY2(settings.hiddenMediaKeys().isEmpty(),
                 "Show all hidden images left the stored list behind");
    }
}

// A tester asked for a plainer send bar. The stored value is what is HIDDEN,
// never what is shown, so a button added in a later release appears for
// everyone instead of being hidden from every existing user — this pins the
// default and the normalization the notify depends on.
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

    // Order and duplicates are normalized, so a write that changes nothing
    // cannot fire the notify every binding in both composers listens to.
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


// ── Reading and typing privacy ──────────────────────────────────────────
//
// Three settings that decide what leaves this device while the user is only
// looking at a room. Their DEFAULTS are the load-bearing part: an upgrade
// must not change what an existing install discloses, in either direction.

void SettingsSessionTest::strictDeviceTrustDefaultsOffAndPersists()
{
    {
        SettingsManager settings;
        // OFF, and the default is the load-bearing part. Enabling MSC4153
        // makes anyone who has not cross-signed their own devices
        // unreadable — doing that to an existing install without being asked
        // would read as the client breaking, not as a privacy improvement.
        QVERIFY(!settings.strictDeviceTrust());

        QSignalSpy spy(&settings, &SettingsManager::strictDeviceTrustChanged);
        settings.setStrictDeviceTrust(true);
        QVERIFY(settings.strictDeviceTrust());
        QCOMPARE(spy.count(), 1);
        settings.setStrictDeviceTrust(true);
        QCOMPARE(spy.count(), 1);
    }
    // It has to SURVIVE a restart, because a restart is when it applies:
    // the SDK reads it while building a client and offers no runtime setter.
    // A setting that reset on restart would therefore never take effect at
    // all, and the UI's "takes effect next start" would be a lie.
    SettingsManager reopened;
    QVERIFY(reopened.strictDeviceTrust());
}

void SettingsSessionTest::readReceiptModeDefaultsToPublicPersistsAndClamps()
{
    {
        SettingsManager settings;
        // Public, which is what every previous version sent. Making this
        // default to private would be a behaviour change disguised as a
        // privacy improvement.
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
        // Out of range falls back to the safe, previous-behaviour value
        // rather than to the strictest one — a corrupt store must not
        // silently stop a user's receipts.
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

    // Following: an encrypted room is treated exactly like any other, which
    // is the pre-0.9.0 behaviour and must survive the upgrade untouched.
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

    // Encryption not yet known — during hydration, this is a real state.
    // Guessing "unencrypted" would put a body on the desktop that the user
    // asked an encrypted room to withhold, and nobody would ever learn it
    // had happened. The stricter of the two wins.
    QCOMPARE(settings.effectiveNotificationPreview(false, false), 2);
    QCOMPARE(settings.effectiveNotificationPreview(true, false), 2);

    // And the reverse arrangement is respected too: when the general level
    // is the stricter one, unknown must not RELAX to the encrypted level.
    settings.setNotificationPreview(2);
    settings.setNotificationPreviewEncrypted(0);
    QCOMPARE(settings.effectiveNotificationPreview(false, false), 2);
    // Known-encrypted still means exactly what the user asked for.
    QCOMPARE(settings.effectiveNotificationPreview(true, true), 0);
}

QTEST_MAIN(SettingsSessionTest)
#include "SettingsSessionTest.moc"
