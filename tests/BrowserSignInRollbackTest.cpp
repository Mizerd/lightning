
#include "app/SettingsManager.h"
#include "storage/AppDataPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#ifdef ENABLE_RUST_SDK_BACKEND
#include "auth/AuthManager.h"
#include "matrix/RustSdkMatrixClient.h"
#include "matrix/RustSessionPolicy.h"
#include "storage/InMemorySecretStore.h"
#endif

class BrowserSignInRollbackTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_home;

#ifdef ENABLE_RUST_SDK_BACKEND
    static constexpr const char *kServer = "https://example.org";

    // A keyring that is locked, or that refuses an item even when open.
    class RefusingSecretStore : public InMemorySecretStore
    {
    public:
        bool locked = false;
        bool refusing = false;
        bool storeSecret(const QString &u, const QString &k, const QString &v) override
        {
            return !locked && !refusing && InMemorySecretStore::storeSecret(u, k, v);
        }
        bool unlockedForWriting() override { return !locked; }
    };

    struct Restore {
        int calls = 0;
        QString result;
        std::function<QString(const matrix::app_data::AccountIdentity &, const QString &)> fn()
        {
            return [this](const matrix::app_data::AccountIdentity &, const QString &) {
                ++calls;
                return result;
            };
        }
    };

    static void adopt(RustSdkMatrixClient &client, const QString &userId,
                      const QString &deviceId, Restore &restore)
    {
        client.adoptBrowserSession(QString::fromLatin1(kServer), userId, deviceId,
                                   QStringLiteral("client-id"),
                                   QStringLiteral("not-a-real-token"), QString(),
                                   QStringLiteral("oauth"), restore.fn());
    }

    static QJsonObject restoreFailed()
    {
        QJsonObject event;
        event.insert(QStringLiteral("type"), QStringLiteral("login_failed"));
        event.insert(QStringLiteral("message"),
                     QStringLiteral("Matrix OAuth session restore failed"));
        return event;
    }

    static QString storeOf(const QString &userId,
                           const QString &server = QString::fromLatin1(kServer))
    {
        matrix::app_data::AccountIdentity identity;
        if (!matrix::app_data::resolveAccountIdentity(server, userId, &identity))
            return {};
        return identity.rustStorePath;
    }

    static QString seedQuarantine(const QString &store)
    {
        const QString dir = store + QStringLiteral(".orphaned-20260101-000000000");
        if (!QDir().mkpath(dir))
            return {};
        QFile marker(dir + QStringLiteral("/marker"));
        if (!marker.open(QIODevice::WriteOnly))
            return {};
        marker.write("keys");
        return marker.fileName();
    }

    static QStringList quarantinesOf(const QString &store)
    {
        const QFileInfo info(store);
        return QDir(info.absolutePath())
            .entryList({ info.fileName() + QStringLiteral(".orphaned-*") },
                       QDir::Dirs | QDir::NoDotAndDotDot);
    }

    static QString seedStore(const QString &store)
    {
        if (!QDir().mkpath(store))
            return {};
        QFile keys(store + QStringLiteral("/matrix-sdk-crypto.sqlite3"));
        if (!keys.open(QIODevice::WriteOnly))
            return {};
        keys.write("keys");
        return keys.fileName();
    }

    static bool keysAside(const QString &store)
    {
        const QStringList aside = quarantinesOf(store);
        return aside.size() == 1
               && QFileInfo::exists(QFileInfo(store).absolutePath() + QLatin1Char('/')
                                    + aside.first()
                                    + QStringLiteral("/matrix-sdk-crypto.sqlite3"));
    }

    static void serverEnds(RustSdkMatrixClient &client, const QString &server,
                           const QString &userId, const QString &deviceId,
                           const QJsonValue &softLogout)
    {
        QJsonObject restored;
        restored.insert(QStringLiteral("type"), QStringLiteral("login_ok"));
        restored.insert(QStringLiteral("homeserver"), server);
        restored.insert(QStringLiteral("user_id"), userId);
        restored.insert(QStringLiteral("device_id"), deviceId);
        client.handleRustEventForTest(restored);
        QJsonObject revoked;
        revoked.insert(QStringLiteral("type"), QStringLiteral("session_token_revoked"));
        if (!softLogout.isUndefined())
            revoked.insert(QStringLiteral("soft_logout"), softLogout);
        client.handleRustEventForTest(revoked);
        client.detachSession();
    }
#endif

private slots:
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        qputenv("XDG_DATA_HOME", m_home.path().toUtf8() + "/data");
        qputenv("XDG_CONFIG_HOME", m_home.path().toUtf8() + "/config");
        qputenv("HOME", m_home.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("browser-sign-in-rollback-test"));
    }

    void init()
    {
        QSettings().clear();
        QDir(m_home.path() + QStringLiteral("/data")).removeRecursively();
    }

    void aFailedRestoreLeavesNothingThatBlocksTheNextSignIn()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Browser sign-in exists on the Rust backend only.");
#else
        SettingsManager settings;
        settings.setSecretStore(nullptr);
        RustSdkMatrixClient client(&settings);
        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        const QString alice = QStringLiteral("@alice:example.org");

        Restore restore;
        adopt(client, alice, QStringLiteral("DEVICEONE"), restore);
        QCOMPARE(restore.calls, 1);
        QVERIFY(settings.hasSavedAccount(alice));
        QVERIFY(QFileInfo::exists(storeOf(alice)));

        client.handleRustEventForTest(restoreFailed());
        QCOMPARE(failed.count(), 1);
        QVERIFY2(failed.last().at(0).toString().contains(QStringLiteral("nothing was kept")),
                 qPrintable(failed.last().at(0).toString()));
        QVERIFY2(!settings.hasSavedAccount(alice), "the failed attempt's record survived");
        QVERIFY2(!QFileInfo::exists(storeOf(alice)), "the failed attempt's store survived");

        adopt(client, alice, QStringLiteral("DEVICETWO"), restore);
        QCOMPARE(restore.calls, 2);
#endif
    }

    void aRestoreThatFailsAtOnceLeavesNothingEither()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Browser sign-in exists on the Rust backend only.");
#else
        SettingsManager settings;
        settings.setSecretStore(nullptr);
        RustSdkMatrixClient client(&settings);
        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        const QString bob = QStringLiteral("@bob:example.org");

        Restore restore;
        restore.result = QStringLiteral("error: could not start the restore");
        adopt(client, bob, QStringLiteral("DEVICEONE"), restore);
        QCOMPARE(failed.count(), 1);
        QVERIFY(!settings.hasSavedAccount(bob));
        QVERIFY(!QFileInfo::exists(storeOf(bob)));

        restore.result.clear();
        adopt(client, bob, QStringLiteral("DEVICETWO"), restore);
        QCOMPARE(restore.calls, 2);
#endif
    }

    void aFailedReauthorisationKeepsTheExistingSession()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Browser sign-in exists on the Rust backend only.");
#else
        SettingsManager settings;
        settings.setSecretStore(nullptr);
        const QString carol = QStringLiteral("@carol:example.org");
        settings.saveSession(QString::fromLatin1(kServer), carol, QStringLiteral("DEVICEONE"),
                             QStringLiteral("old-token"), QString(), QStringLiteral("oauth"),
                             QStringLiteral("client-id"));
        QVERIFY(QDir().mkpath(storeOf(carol)));
        QFile keys(storeOf(carol) + QStringLiteral("/matrix-sdk-crypto.sqlite3"));
        QVERIFY(keys.open(QIODevice::WriteOnly));
        keys.write("keys");
        keys.close();

        RustSdkMatrixClient client(&settings);
        Restore restore;
        adopt(client, carol, QStringLiteral("DEVICEONE"), restore);
        QCOMPARE(restore.calls, 1);
        client.handleRustEventForTest(restoreFailed());

        QVERIFY(settings.hasSavedAccount(carol));
        QVERIFY(QFileInfo::exists(keys.fileName()));
#endif
    }

    void aFailedAttemptNeverTouchesAQuarantinedStore()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Browser sign-in exists on the Rust backend only.");
#else
        SettingsManager settings;
        settings.setSecretStore(nullptr);
        RustSdkMatrixClient client(&settings);
        const QString erin = QStringLiteral("@erin:example.org");
        const QString marker = seedQuarantine(storeOf(erin));
        QVERIFY(!marker.isEmpty());

        Restore restore;
        adopt(client, erin, QStringLiteral("DEVICEONE"), restore);
        QCOMPARE(restore.calls, 1);
        client.handleRustEventForTest(restoreFailed());
        QVERIFY2(QFileInfo::exists(marker), "an async rollback deleted the quarantine");
        QVERIFY(!QFileInfo::exists(storeOf(erin)));
        QVERIFY(!settings.hasSavedAccount(erin));

        restore.result = QStringLiteral("error: could not start the restore");
        adopt(client, erin, QStringLiteral("DEVICETWO"), restore);
        QCOMPARE(restore.calls, 2);
        QVERIFY2(QFileInfo::exists(marker), "an immediate rollback deleted the quarantine");
        QVERIFY(!QFileInfo::exists(storeOf(erin)));
#endif
    }

    void aFailedPasswordSignInKeepsTheStoreItQuarantined()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The Rust backend quarantines stores; the others have none.");
#else
        SettingsManager settings;
        settings.setSecretStore(nullptr);
        RustSdkMatrixClient client(&settings);
        const QString server = QStringLiteral("https://localhost:1");
        const QString frank = QStringLiteral("@frank:localhost:1");
        const QString store = storeOf(frank, server);
        QVERIFY(QDir().mkpath(store));
        QFile keys(store + QStringLiteral("/matrix-sdk-crypto.sqlite3"));
        QVERIFY(keys.open(QIODevice::WriteOnly));
        keys.write("keys");
        keys.close();

        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        client.login(server, QStringLiteral("frank"), QStringLiteral("not-a-password"));
        QCOMPARE(quarantinesOf(store).size(), 1);
        QVERIFY(failed.wait(60000));

        const QStringList left = quarantinesOf(store);
        QCOMPARE(left.size(), 1);
        QVERIFY2(QFileInfo::exists(QFileInfo(store).absolutePath() + QLatin1Char('/')
                                   + left.first() + QStringLiteral("/matrix-sdk-crypto.sqlite3")),
                 "the failed sign-in deleted the store it had just quarantined");
#endif
    }

    void aSlugCollisionIsRefusedBeforeAnythingIsWritten()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Browser sign-in exists on the Rust backend only.");
#else
        SettingsManager settings;
        settings.setSecretStore(nullptr);
        const QString saved = QStringLiteral("@a_b:example.org");
        settings.saveSession(QString::fromLatin1(kServer), saved, QStringLiteral("DEVICEONE"),
                             QStringLiteral("old-token"), QString(), QStringLiteral("oauth"),
                             QStringLiteral("client-id"));
        QVERIFY(settings.hasSavedAccount(saved));
        QVERIFY(QDir().mkpath(storeOf(saved)));

        RustSdkMatrixClient client(&settings);
        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        QSignalSpy blocked(&client, &RustSdkMatrixClient::localSessionBlocked);
        QSignalSpy reset(&client, &RustSdkMatrixClient::localSessionResetRequired);
        Restore restore;
        adopt(client, QStringLiteral("@a:b_example.org"), QStringLiteral("DEVICETWO"), restore);

        QCOMPARE(restore.calls, 0);
        QCOMPARE(failed.count(), 1);
        QVERIFY2(failed.at(0).at(0).toString().contains(QStringLiteral("collides")),
                 qPrintable(failed.at(0).at(0).toString()));
        QCOMPARE(blocked.count(), 0);
        QCOMPARE(reset.count(), 0);
        QVERIFY(settings.hasSavedAccount(saved));
        QVERIFY(QFileInfo::exists(storeOf(saved)));
#endif
    }

    void aStoreWithNoSavedAccountIsMovedAsideAndTheSignInGoesOn()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Browser sign-in exists on the Rust backend only.");
#else
        SettingsManager settings;
        settings.setSecretStore(nullptr);
        const QString gina = QStringLiteral("@gina:example.org");
        QVERIFY(QDir().mkpath(storeOf(gina)));
        QFile keys(storeOf(gina) + QStringLiteral("/matrix-sdk-crypto.sqlite3"));
        QVERIFY(keys.open(QIODevice::WriteOnly));
        keys.write("keys");
        keys.close();

        RustSdkMatrixClient client(&settings);
        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        QSignalSpy reset(&client, &RustSdkMatrixClient::localSessionResetRequired);
        Restore restore;
        adopt(client, gina, QStringLiteral("DEVICEONE"), restore);

        QCOMPARE(failed.count(), 0);
        QCOMPARE(restore.calls, 1);
        QCOMPARE(reset.count(), 0);
        const QStringList moved = quarantinesOf(storeOf(gina));
        QCOMPARE(moved.size(), 1);
        QVERIFY(QFileInfo::exists(QFileInfo(storeOf(gina)).absolutePath() + QLatin1Char('/')
                                  + moved.first()
                                  + QStringLiteral("/matrix-sdk-crypto.sqlite3")));
#endif
    }

    void aStoreACaseVariantMayOwnIsNeverMoved()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Browser sign-in exists on the Rust backend only.");
#else
        SettingsManager settings;
        settings.setSecretStore(nullptr);
        settings.saveSession(QString::fromLatin1(kServer), QStringLiteral("@Hank:example.org"),
                             QStringLiteral("DEVICEONE"), QStringLiteral("old-token"),
                             QString(), QStringLiteral("oauth"), QStringLiteral("client-id"));
        const QString hank = QStringLiteral("@hank:example.org");
        QVERIFY(QDir().mkpath(storeOf(hank)));

        RustSdkMatrixClient client(&settings);
        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        QSignalSpy reset(&client, &RustSdkMatrixClient::localSessionResetRequired);
        Restore restore;
        adopt(client, hank, QStringLiteral("DEVICETWO"), restore);

        QCOMPARE(restore.calls, 0);
        QCOMPARE(failed.count(), 1);
        QVERIFY2(failed.at(0).at(0).toString().contains(QStringLiteral("upper and lower case")),
                 qPrintable(failed.at(0).at(0).toString()));
        QCOMPARE(reset.count(), 0);
        QVERIFY(quarantinesOf(storeOf(hank)).isEmpty());
        QVERIFY(QFileInfo::exists(storeOf(hank)));
#endif
    }

    void aRecordWithoutADeviceOffersTheRebuildCard()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Browser sign-in exists on the Rust backend only.");
#else
        SettingsManager settings;
        settings.setSecretStore(nullptr);
        const QString ivy = QStringLiteral("@ivy:example.org");
        settings.saveSession(QString::fromLatin1(kServer), ivy, QString(),
                             QStringLiteral("old-token"), QString(), QStringLiteral("oauth"),
                             QStringLiteral("client-id"));
        QVERIFY(settings.hasSavedAccount(ivy));
        QVERIFY(QDir().mkpath(storeOf(ivy)));

        RustSdkMatrixClient client(&settings);
        QSignalSpy reset(&client, &RustSdkMatrixClient::localSessionResetRequired);
        Restore restore;
        adopt(client, ivy, QStringLiteral("DEVICETWO"), restore);

        QCOMPARE(restore.calls, 0);
        QCOMPARE(reset.count(), 1);
        QCOMPARE(reset.at(0).at(0).toString(), QStringLiteral("session_without_device_id"));
        QCOMPARE(reset.at(0).at(1).toString(), ivy);
        QVERIFY(QFileInfo::exists(storeOf(ivy)));
#endif
    }

    void aBlockingSessionIsReportedWithTheWayOut()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Browser sign-in exists on the Rust backend only.");
#else
        SettingsManager settings;
        settings.setSecretStore(nullptr);
        const QString dave = QStringLiteral("@dave:example.org");
        settings.saveSession(QString::fromLatin1(kServer), dave, QStringLiteral("DEVICEONE"),
                             QStringLiteral("old-token"), QString(), QStringLiteral("oauth"),
                             QStringLiteral("client-id"));
        QVERIFY(QDir().mkpath(storeOf(dave)));

        RustSdkMatrixClient client(&settings);
        QSignalSpy blocked(&client, &RustSdkMatrixClient::localSessionBlocked);
        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        Restore restore;
        adopt(client, dave, QStringLiteral("DEVICETWO"), restore);

        QCOMPARE(restore.calls, 0);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(blocked.count(), 1);
        QCOMPARE(blocked.at(0).at(0).toString(),
                 QStringLiteral("existing_store_requires_restore"));
        QCOMPARE(blocked.at(0).at(1).toString(), dave);
        QVERIFY(settings.hasSavedAccount(dave));
        QVERIFY(QFileInfo::exists(storeOf(dave)));
#endif
    }

    void aPasswordSignInAfterTheServerEndedTheDeviceMovesItsStoreAside()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The Rust backend owns the SDK store.");
#else
        InMemorySecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        const QString server = QStringLiteral("https://localhost:1");
        const QString gina = QStringLiteral("@gina:localhost:1");
        settings.saveSession(server, gina, QStringLiteral("OLDDEVICE"),
                             QStringLiteral("not-a-real-token"));
        const QString store = storeOf(gina, server);
        const QString keys = seedStore(store);
        QVERIFY(!keys.isEmpty());

        RustSdkMatrixClient client(&settings);
        serverEnds(client, server, gina, QStringLiteral("OLDDEVICE"), false);

        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        client.login(server, QStringLiteral("gina"), QStringLiteral("not-a-password"));
        QCOMPARE(failed.count(), 0);
        QVERIFY2(keysAside(store), "the revoked device's store was not moved aside");
        QVERIFY(!QFileInfo::exists(keys));

        QVERIFY(failed.wait(60000));
        QVERIFY2(failed.last().at(0).toString()
                     != matrix::rust_session::userMessage(
                         matrix::rust_session::StoreBlockReason::ExistingStoreNeedsRestore),
                 "sent to \"Open it\" after the store was moved");
        QVERIFY2(keysAside(store), "a failed attempt removed the store it moved aside");

        QJsonObject accepted;
        accepted.insert(QStringLiteral("type"), QStringLiteral("login_ok"));
        accepted.insert(QStringLiteral("homeserver"), server);
        accepted.insert(QStringLiteral("user_id"), gina);
        accepted.insert(QStringLiteral("device_id"), QStringLiteral("NEWDEVICE"));
        accepted.insert(QStringLiteral("access_token"), QStringLiteral("not-a-real-token"));
        client.handleRustEventForTest(accepted);
        QCOMPARE(settings.accountRecord(gina).value(QStringLiteral("deviceId")).toString(),
                 QStringLiteral("NEWDEVICE"));
        QVERIFY(keysAside(store));
#endif
    }

    void aPasswordSignInWithoutThatProofIsStillSentToOpenIt_data()
    {
        QTest::addColumn<bool>("revoke");
        QTest::addColumn<bool>("softLogout");
        QTest::addColumn<QString>("revokedUser");
        QTest::addColumn<QString>("revokedDevice");
        QTest::addColumn<bool>("signedInSince");
        const QString hana = QStringLiteral("@hana:localhost:1");
        QTest::newRow("nothing revoked") << false << false << hana
                                         << QStringLiteral("OLDDEVICE") << false;
        QTest::newRow("another device") << true << false << hana
                                        << QStringLiteral("OTHERDEVICE") << false;
        QTest::newRow("another account") << true << false
                                         << QStringLiteral("@ivan:localhost:1")
                                         << QStringLiteral("OLDDEVICE") << false;
        QTest::newRow("signed in again since") << true << false << hana
                                               << QStringLiteral("OLDDEVICE") << true;
    }

    void aPasswordSignInWithoutThatProofIsStillSentToOpenIt()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The Rust backend owns the SDK store.");
#else
        QFETCH(bool, revoke);
        QFETCH(bool, softLogout);
        QFETCH(QString, revokedUser);
        QFETCH(QString, revokedDevice);
        QFETCH(bool, signedInSince);
        InMemorySecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        const QString server = QStringLiteral("https://localhost:1");
        const QString hana = QStringLiteral("@hana:localhost:1");
        settings.saveSession(server, hana, QStringLiteral("OLDDEVICE"),
                             QStringLiteral("not-a-real-token"));
        const QString store = storeOf(hana, server);
        const QString keys = seedStore(store);
        QVERIFY(!keys.isEmpty());

        RustSdkMatrixClient client(&settings);
        if (revoke)
            serverEnds(client, server, revokedUser, revokedDevice, softLogout);
        if (signedInSince) {
            QJsonObject again;
            again.insert(QStringLiteral("type"), QStringLiteral("login_ok"));
            again.insert(QStringLiteral("homeserver"), server);
            again.insert(QStringLiteral("user_id"), hana);
            again.insert(QStringLiteral("device_id"), QStringLiteral("OLDDEVICE"));
            client.handleRustEventForTest(again);
        }

        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        client.login(server, QStringLiteral("hana"), QStringLiteral("not-a-password"));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.last().at(0).toString(),
                 matrix::rust_session::userMessage(
                     matrix::rust_session::StoreBlockReason::ExistingStoreNeedsRestore));
        QVERIFY(quarantinesOf(store).isEmpty());
        QVERIFY(QFileInfo::exists(keys));
        QCOMPARE(settings.accountRecord(hana).value(QStringLiteral("deviceId")).toString(),
                 QStringLiteral("OLDDEVICE"));
#endif
    }

    void aBrowserSignInAfterTheServerEndedTheDeviceMovesItsStoreAside_data()
    {
        QTest::addColumn<QString>("newDevice");
        QTest::newRow("a new device") << QStringLiteral("NEWDEVICE");
        QTest::newRow("the same device id") << QStringLiteral("OLDDEVICE");
    }

    void aBrowserSignInAfterTheServerEndedTheDeviceMovesItsStoreAside()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Browser sign-in exists on the Rust backend only.");
#else
        QFETCH(QString, newDevice);
        InMemorySecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        const QString jo = QStringLiteral("@jo:example.org");
        settings.saveSession(QString::fromLatin1(kServer), jo, QStringLiteral("OLDDEVICE"),
                             QStringLiteral("not-a-real-token"), QString(),
                             QStringLiteral("oauth"), QStringLiteral("client-id"));
        const QString keys = seedStore(storeOf(jo));
        QVERIFY(!keys.isEmpty());

        RustSdkMatrixClient client(&settings);
        serverEnds(client, QString::fromLatin1(kServer), jo, QStringLiteral("OLDDEVICE"),
                   false);
        Restore restore;
        adopt(client, jo, newDevice, restore);

        QCOMPARE(restore.calls, 1);
        QVERIFY2(keysAside(storeOf(jo)), "the revoked device's store was not moved aside");
        QVERIFY(QFileInfo::exists(storeOf(jo)));
        QVERIFY(!QFileInfo::exists(keys));
        QCOMPARE(settings.accountRecord(jo).value(QStringLiteral("deviceId")).toString(),
                 newDevice);
#endif
    }

    void aBrowserSignInAfterASoftLogoutIsStillRefused()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Browser sign-in exists on the Rust backend only.");
#else
        InMemorySecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        const QString kim = QStringLiteral("@kim:example.org");
        settings.saveSession(QString::fromLatin1(kServer), kim, QStringLiteral("OLDDEVICE"),
                             QStringLiteral("not-a-real-token"));
        const QString keys = seedStore(storeOf(kim));
        QVERIFY(!keys.isEmpty());

        RustSdkMatrixClient client(&settings);
        serverEnds(client, QString::fromLatin1(kServer), kim, QStringLiteral("OLDDEVICE"),
                   true);
        Restore restore;
        adopt(client, kim, QStringLiteral("NEWDEVICE"), restore);

        QCOMPARE(restore.calls, 0);
        QVERIFY(quarantinesOf(storeOf(kim)).isEmpty());
        QVERIFY(QFileInfo::exists(keys));
        QCOMPARE(settings.accountRecord(kim).value(QStringLiteral("deviceId")).toString(),
                 QStringLiteral("OLDDEVICE"));
#endif
    }

    void aSoftLogoutOfAnOAuthAccountEndsItsDevice()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Browser sign-in exists on the Rust backend only.");
#else
        InMemorySecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        const QString uma = QStringLiteral("@uma:example.org");
        settings.saveSession(QString::fromLatin1(kServer), uma, QStringLiteral("OLDDEVICE"),
                             QStringLiteral("not-a-real-token"), QString(),
                             QStringLiteral("oauth"), QStringLiteral("client-id"));
        const QString keys = seedStore(storeOf(uma));
        QVERIFY(!keys.isEmpty());

        RustSdkMatrixClient client(&settings);
        QSignalSpy blocked(&client, &RustSdkMatrixClient::localSessionBlocked);
        serverEnds(client, QString::fromLatin1(kServer), uma, QStringLiteral("OLDDEVICE"),
                   true);
        QCOMPARE(blocked.count(), 1);
        QCOMPARE(blocked.last().at(0).toString(), QStringLiteral("access_token_revoked"));
        QVERIFY(client.m_softLogoutDeviceId.isEmpty());

        Restore restore;
        adopt(client, uma, QStringLiteral("NEWDEVICE"), restore);
        QCOMPARE(restore.calls, 1);
        QVERIFY2(keysAside(storeOf(uma)), "the ended OAuth device's store was not moved aside");
        QVERIFY(!QFileInfo::exists(keys));
        QCOMPARE(settings.accountRecord(uma).value(QStringLiteral("deviceId")).toString(),
                 QStringLiteral("NEWDEVICE"));
#endif
    }

    void aResumedDeviceTheServerLostIsEndedForANewSignIn_data()
    {
        QTest::addColumn<QString>("state");
        QTest::addColumn<bool>("ended");
        QTest::newRow("missing") << QStringLiteral("missing") << true;
        QTest::newRow("different") << QStringLiteral("different") << true;
        QTest::newRow("matches") << QStringLiteral("matches") << false;
        QTest::newRow("unknown") << QStringLiteral("unknown") << false;
    }

    void aResumedDeviceTheServerLostIsEndedForANewSignIn()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The Rust backend owns the SDK store.");
#else
        QFETCH(QString, state);
        QFETCH(bool, ended);
        InMemorySecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        const QString server = QStringLiteral("https://localhost:1");
        const QString vic = QStringLiteral("@vic:localhost:1");
        settings.saveSession(server, vic, QStringLiteral("OLDDEVICE"),
                             QStringLiteral("not-a-real-token"));
        const QString store = storeOf(vic, server);
        const QString keys = seedStore(store);
        QVERIFY(!keys.isEmpty());

        RustSdkMatrixClient client(&settings);
        serverEnds(client, server, vic, QStringLiteral("OLDDEVICE"), true);
        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        client.login(server, QStringLiteral("vic"), QStringLiteral("not-a-password"));
        QCOMPARE(failed.count(), 0);
        QCOMPARE(client.m_resumeDeviceId, QStringLiteral("OLDDEVICE"));

        QJsonObject accepted;
        accepted.insert(QStringLiteral("type"), QStringLiteral("login_ok"));
        accepted.insert(QStringLiteral("homeserver"), server);
        accepted.insert(QStringLiteral("user_id"), vic);
        accepted.insert(QStringLiteral("device_id"), QStringLiteral("OLDDEVICE"));
        accepted.insert(QStringLiteral("access_token"), QStringLiteral("not-a-real-token"));
        client.handleRustEventForTest(accepted);
        QCOMPARE(client.m_resumedCheckDeviceId, QStringLiteral("OLDDEVICE"));

        QSignalSpy blocked(&client, &RustSdkMatrixClient::localSessionBlocked);
        QJsonObject other;
        other.insert(QStringLiteral("type"), QStringLiteral("resumed_device_key"));
        other.insert(QStringLiteral("user_id"), vic);
        other.insert(QStringLiteral("device_id"), QStringLiteral("OTHERDEVICE"));
        other.insert(QStringLiteral("state"), QStringLiteral("missing"));
        client.handleRustEventForTest(other);
        QCOMPARE(blocked.count(), 0);

        QJsonObject answer = other;
        answer.insert(QStringLiteral("device_id"), QStringLiteral("OLDDEVICE"));
        answer.insert(QStringLiteral("state"), state);
        client.handleRustEventForTest(answer);
        if (!ended) {
            QCOMPARE(blocked.count(), 0);
            QVERIFY(client.m_revokedDeviceId.isEmpty());
            return;
        }
        QCOMPARE(blocked.count(), 1);
        QCOMPARE(blocked.last().at(0).toString(), QStringLiteral("access_token_revoked"));
        QCOMPARE(client.m_revokedDeviceId, QStringLiteral("OLDDEVICE"));
        QVERIFY(client.m_resumedCheckDeviceId.isEmpty());

        client.detachSession();
        const int before = failed.count();
        client.login(server, QStringLiteral("vic"), QStringLiteral("not-a-password"));
        QCOMPARE(failed.count(), before);
        QVERIFY(client.m_resumeDeviceId.isEmpty());
        QVERIFY2(keysAside(store), "the lost device's store was resumed again, not moved aside");
        QVERIFY(failed.wait(60000));
#endif
    }

    void aRefusedResumeOffersNoReset_data()
    {
        QTest::addColumn<bool>("resume");
        QTest::newRow("a resume") << true;
        QTest::newRow("an ordinary sign-in") << false;
    }

    void aRefusedResumeOffersNoReset()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The Rust backend owns the SDK store.");
#else
        QFETCH(bool, resume);
        InMemorySecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        const QString server = QStringLiteral("https://localhost:1");
        const QString wes = QStringLiteral("@wes:localhost:1");
        settings.saveSession(server, wes, QStringLiteral("OLDDEVICE"),
                             QStringLiteral("not-a-real-token"));
        QVERIFY(!seedStore(storeOf(wes, server)).isEmpty());

        RustSdkMatrixClient client(&settings);
        if (resume) {
            serverEnds(client, server, wes, QStringLiteral("OLDDEVICE"), true);
            client.login(server, QStringLiteral("wes"), QStringLiteral("not-a-password"));
            QCOMPARE(client.m_resumeDeviceId, QStringLiteral("OLDDEVICE"));
        }
        QSignalSpy reset(&client, &RustSdkMatrixClient::localSessionResetRequired);
        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        QJsonObject refused;
        refused.insert(QStringLiteral("type"), QStringLiteral("login_failed"));
        refused.insert(QStringLiteral("stage"), QStringLiteral("password"));
        refused.insert(QStringLiteral("reason"), QStringLiteral("crypto_store"));
        refused.insert(QStringLiteral("message"),
                       QStringLiteral("Matrix Rust SDK login failed: the account in "
                                      "the store doesn't match the account in the "
                                      "constructor"));
        client.handleRustEventForTest(refused);
        QCOMPARE(reset.count(), resume ? 0 : 1);
        QVERIFY(failed.count() >= 1);
        if (resume) {
            QVERIFY2(failed.at(0).at(0).toString().contains(QStringLiteral("Nothing on this "
                                                                           "device was changed")),
                     qPrintable(failed.at(0).at(0).toString()));
            QVERIFY(client.m_resumeDeviceId.isEmpty());
        }
#endif
    }

    void aPasswordSignInAfterASoftLogoutResumesTheSameDevice()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The Rust backend owns the SDK store.");
#else
        InMemorySecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        const QString server = QStringLiteral("https://localhost:1");
        const QString nia = QStringLiteral("@nia:localhost:1");
        settings.saveSession(server, nia, QStringLiteral("OLDDEVICE"),
                             QStringLiteral("not-a-real-token"));
        const QString store = storeOf(nia, server);
        const QString keys = seedStore(store);
        QVERIFY(!keys.isEmpty());

        RustSdkMatrixClient client(&settings);
        serverEnds(client, server, nia, QStringLiteral("OLDDEVICE"), true);

        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        client.login(server, QStringLiteral("nia"), QStringLiteral("not-a-password"));
        QCOMPARE(failed.count(), 0);
        QCOMPARE(client.m_resumeDeviceId, QStringLiteral("OLDDEVICE"));
        QVERIFY(quarantinesOf(store).isEmpty());
        QVERIFY(QFileInfo::exists(keys));

        QVERIFY(failed.wait(60000));
        QVERIFY(failed.last().at(0).toString()
                != matrix::rust_session::userMessage(
                    matrix::rust_session::StoreBlockReason::ExistingStoreNeedsRestore));
        QVERIFY(client.m_resumeDeviceId.isEmpty());
        QVERIFY(QFileInfo::exists(keys));
        client.login(server, QStringLiteral("nia"), QStringLiteral("not-a-password"));
        QCOMPARE(client.m_resumeDeviceId, QStringLiteral("OLDDEVICE"));
        QVERIFY(failed.wait(60000));

        QJsonObject accepted;
        accepted.insert(QStringLiteral("type"), QStringLiteral("login_ok"));
        accepted.insert(QStringLiteral("homeserver"), server);
        accepted.insert(QStringLiteral("user_id"), nia);
        accepted.insert(QStringLiteral("device_id"), QStringLiteral("OLDDEVICE"));
        accepted.insert(QStringLiteral("access_token"), QStringLiteral("not-a-real-token"));
        client.handleRustEventForTest(accepted);
        QCOMPARE(settings.accountRecord(nia).value(QStringLiteral("deviceId")).toString(),
                 QStringLiteral("OLDDEVICE"));
        QVERIFY(quarantinesOf(store).isEmpty());
        QVERIFY(QFileInfo::exists(keys));
        client.detachSession();
        const int before = failed.count();
        client.login(server, QStringLiteral("nia"), QStringLiteral("not-a-password"));
        QCOMPARE(failed.count(), before + 1);
        QCOMPARE(failed.last().at(0).toString(),
                 matrix::rust_session::userMessage(
                     matrix::rust_session::StoreBlockReason::ExistingStoreNeedsRestore));
#endif
    }

    void aSoftLogoutResumesOnlyThatAccountAndDevice_data()
    {
        QTest::addColumn<QString>("typedServer");
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("endedUser");
        QTest::addColumn<QString>("endedDevice");
        QTest::addColumn<QJsonValue>("softLogout");
        const QString server = QStringLiteral("https://localhost:1");
        const QString oto = QStringLiteral("@oto:localhost:1");
        QTest::newRow("another device") << server << QStringLiteral("oto") << oto
                                        << QStringLiteral("OTHERDEVICE") << QJsonValue(true);
        QTest::newRow("another account") << server << QStringLiteral("oto")
                                         << QStringLiteral("@pia:localhost:1")
                                         << QStringLiteral("OLDDEVICE") << QJsonValue(true);
        QTest::newRow("another casing typed") << server << QStringLiteral("Oto") << oto
                                              << QStringLiteral("OLDDEVICE")
                                              << QJsonValue(true);
        QTest::newRow("another homeserver typed") << QStringLiteral("https://localhost:2")
                                                  << oto << oto
                                                  << QStringLiteral("OLDDEVICE")
                                                  << QJsonValue(true);
        QTest::newRow("no soft_logout field") << server << QStringLiteral("oto") << oto
                                              << QStringLiteral("OLDDEVICE")
                                              << QJsonValue(QJsonValue::Undefined);
    }

    void aSoftLogoutResumesOnlyThatAccountAndDevice()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The Rust backend owns the SDK store.");
#else
        QFETCH(QString, typedServer);
        QFETCH(QString, typed);
        QFETCH(QString, endedUser);
        QFETCH(QString, endedDevice);
        QFETCH(QJsonValue, softLogout);
        InMemorySecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        const QString server = QStringLiteral("https://localhost:1");
        const QString oto = QStringLiteral("@oto:localhost:1");
        settings.saveSession(server, oto, QStringLiteral("OLDDEVICE"),
                             QStringLiteral("not-a-real-token"));
        const QString store = storeOf(oto, server);
        const QString keys = seedStore(store);
        QVERIFY(!keys.isEmpty());

        RustSdkMatrixClient client(&settings);
        serverEnds(client, server, endedUser, endedDevice, softLogout);

        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        client.login(typedServer, typed, QStringLiteral("not-a-password"));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.last().at(0).toString(),
                 matrix::rust_session::userMessage(
                     matrix::rust_session::StoreBlockReason::ExistingStoreNeedsRestore));
        QVERIFY(client.m_resumeDeviceId.isEmpty());
        QVERIFY(quarantinesOf(store).isEmpty());
        QVERIFY(QFileInfo::exists(keys));
        QCOMPARE(settings.accountRecord(oto).value(QStringLiteral("homeserver")).toString(),
                 server);
#endif
    }

    void aDeviceEndedAfterASoftLogoutIsNotResumed()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The Rust backend owns the SDK store.");
#else
        InMemorySecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        const QString server = QStringLiteral("https://localhost:1");
        const QString rue = QStringLiteral("@rue:localhost:1");
        settings.saveSession(server, rue, QStringLiteral("OLDDEVICE"),
                             QStringLiteral("not-a-real-token"));
        const QString store = storeOf(rue, server);
        QVERIFY(!seedStore(store).isEmpty());

        RustSdkMatrixClient client(&settings);
        serverEnds(client, server, rue, QStringLiteral("OLDDEVICE"), true);
        serverEnds(client, server, rue, QStringLiteral("OLDDEVICE"), false);

        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        client.login(server, QStringLiteral("rue"), QStringLiteral("not-a-password"));
        QCOMPARE(failed.count(), 0);
        QVERIFY(client.m_resumeDeviceId.isEmpty());
        QVERIFY2(keysAside(store), "an ended device's store was resumed, not moved aside");
        QVERIFY(failed.wait(60000));
#endif
    }

    void aSoftLogoutIsReportedAsExpiredAndAHardOneAsRevoked()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The Rust backend owns the SDK store.");
#else
        InMemorySecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        const QString server = QStringLiteral("https://localhost:1");
        const QString sol = QStringLiteral("@sol:localhost:1");
        RustSdkMatrixClient client(&settings);
        QSignalSpy blocked(&client, &RustSdkMatrixClient::localSessionBlocked);
        for (const QJsonValue &soft :
             { QJsonValue(true), QJsonValue(false), QJsonValue(QJsonValue::Undefined) })
            serverEnds(client, server, sol, QStringLiteral("DEVICE"), soft);
        QCOMPARE(blocked.count(), 3);
        QCOMPARE(blocked.at(0).at(0).toString(), QStringLiteral("access_token_expired"));
        QCOMPARE(blocked.at(1).at(0).toString(), QStringLiteral("access_token_revoked"));
        QCOMPARE(blocked.at(2).at(0).toString(), QStringLiteral("access_token_revoked"));
#endif
    }

    void theProofIsUsedOnce()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Browser sign-in exists on the Rust backend only.");
#else
        InMemorySecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        const QString lou = QStringLiteral("@lou:example.org");
        settings.saveSession(QString::fromLatin1(kServer), lou, QStringLiteral("OLDDEVICE"),
                             QStringLiteral("not-a-real-token"), QString(),
                             QStringLiteral("oauth"), QStringLiteral("client-id"));
        QVERIFY(!seedStore(storeOf(lou)).isEmpty());

        RustSdkMatrixClient client(&settings);
        serverEnds(client, QString::fromLatin1(kServer), lou, QStringLiteral("OLDDEVICE"),
                   false);
        Restore restore;
        adopt(client, lou, QStringLiteral("OLDDEVICE"), restore);
        QCOMPARE(restore.calls, 1);
        QCOMPARE(quarantinesOf(storeOf(lou)).size(), 1);

        adopt(client, lou, QStringLiteral("OLDDEVICE"), restore);
        QCOMPARE(restore.calls, 2);
        QVERIFY2(quarantinesOf(storeOf(lou)).size() == 1,
                 "a healthy store was moved aside on a spent proof");
#endif
    }

    void aFailedMoveLeavesAnotherRunningSessionIntact()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The Rust backend owns the SDK store.");
#else
        InMemorySecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        const QString server = QStringLiteral("https://localhost:1");
        const QString xena = QStringLiteral("@xena:localhost:1");
        const QString walt = QStringLiteral("@walt:localhost:1");
        settings.saveSession(server, xena, QStringLiteral("OLDDEVICE"),
                             QStringLiteral("not-a-real-token"));
        settings.saveSession(server, walt, QStringLiteral("WALTDEVICE"),
                             QStringLiteral("not-a-real-token"));
        const QString xenaStore = storeOf(xena, server);
        const QString keys = seedStore(xenaStore);
        QVERIFY(!keys.isEmpty());

        RustSdkMatrixClient client(&settings);
        serverEnds(client, server, xena, QStringLiteral("OLDDEVICE"), false);

        matrix::app_data::AccountIdentity waltIdentity;
        QVERIFY(matrix::app_data::resolveAccountIdentity(server, walt, &waltIdentity));
        QVERIFY(client.ensureRustHandleForIdentity(waltIdentity));
        QJsonObject waltOk;
        waltOk.insert(QStringLiteral("type"), QStringLiteral("login_ok"));
        waltOk.insert(QStringLiteral("homeserver"), server);
        waltOk.insert(QStringLiteral("user_id"), walt);
        waltOk.insert(QStringLiteral("device_id"), QStringLiteral("WALTDEVICE"));
        client.handleRustEventForTest(waltOk);
        QVERIFY(client.isLoggedIn());

        const QString accountDir = QFileInfo(xenaStore).absolutePath();
        QVERIFY(QFile::setPermissions(accountDir, QFileDevice::ReadOwner
                                                      | QFileDevice::ExeOwner));
        const auto unlock = qScopeGuard([&accountDir] {
            QFile::setPermissions(accountDir, QFileDevice::ReadOwner
                                                  | QFileDevice::WriteOwner
                                                  | QFileDevice::ExeOwner);
        });
        {
            QFile probe(accountDir + QStringLiteral("/probe"));
            if (probe.open(QIODevice::WriteOnly)) {
                probe.close();
                probe.remove();
                QSKIP("Permissions do not stop a rename here (running as root?).");
            }
        }

        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        client.login(server, QStringLiteral("xena"), QStringLiteral("not-a-password"));
        QCOMPARE(failed.count(), 1);
        QVERIFY2(failed.last().at(0).toString().contains(QStringLiteral("could not be moved")),
                 qPrintable(failed.last().at(0).toString()));
        QVERIFY(QFileInfo::exists(keys));
        QVERIFY2(client.m_rustHandle != nullptr, "walt's running session was released");
        QCOMPARE(QFileInfo(client.m_storePath).absoluteFilePath(),
                 QFileInfo(waltIdentity.rustStorePath).absoluteFilePath());
        QVERIFY(client.isLoggedIn());
        QCOMPARE(client.currentUserId(), walt);
#endif
    }

    void aSignInTheKeyringLostContinuesAsTheSameDevice()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The Rust backend owns the SDK store.");
#else
        InMemorySecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        const QString server = QStringLiteral("https://localhost:1");
        const QString kai = QStringLiteral("@kai:localhost:1");
        settings.saveSession(server, kai, QStringLiteral("OLDDEVICE"),
                             QStringLiteral("not-a-real-token"));
        const QString store = storeOf(kai, server);
        const QString keys = seedStore(store);
        QVERIFY(!keys.isEmpty());
        QVERIFY(secrets.clearAccountSecrets(kai));
        QVERIFY(settings.accessTokenFor(kai).isEmpty());
        QVERIFY(!settings.secretBackendUnavailable());
        QVERIFY(settings.secretsWrittenHereForRecordedDevice(kai));

        RustSdkMatrixClient client(&settings);
        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        QSignalSpy reset(&client, &RustSdkMatrixClient::localSessionResetRequired);
        client.login(server, QStringLiteral("kai"), QStringLiteral("not-a-password"));
        QCOMPARE(failed.count(), 0);
        QCOMPARE(reset.count(), 0);
        QCOMPARE(client.m_resumeDeviceId, QStringLiteral("OLDDEVICE"));
        QVERIFY(quarantinesOf(store).isEmpty());
        QVERIFY(QFileInfo::exists(keys));
        QVERIFY(failed.wait(60000));
        QCOMPARE(reset.count(), 0);
        QVERIFY(quarantinesOf(store).isEmpty());
        QVERIFY(QFileInfo::exists(keys));
#endif
    }

    void aLostSignInThatCannotContinueIsExplainedNotRebuilt_data()
    {
        QTest::addColumn<QString>("typedServer");
        QTest::addColumn<QString>("typedUser");
        QTest::addColumn<QString>("authType");
        QTest::addColumn<bool>("adopted");
        QTest::addColumn<QString>("code");
        const QString card = QStringLiteral("keyring_lost_session");
        QTest::newRow("another homeserver typed")
            << QStringLiteral("https://localhost:2") << QStringLiteral("@lia:localhost:1")
            << QStringLiteral("password") << false << card;
        QTest::newRow("oauth session") << QStringLiteral("https://localhost:1")
                                       << QStringLiteral("lia") << QStringLiteral("oauth")
                                       << false << card;
        QTest::newRow("sso session") << QStringLiteral("https://localhost:1")
                                     << QStringLiteral("lia") << QStringLiteral("sso")
                                     << false << card;
        QTest::newRow("adopted, not written here")
            << QStringLiteral("https://localhost:1") << QStringLiteral("lia")
            << QStringLiteral("password") << true
            << QStringLiteral("store_without_session_metadata");
    }

    void aLostSignInThatCannotContinueIsExplainedNotRebuilt()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The Rust backend owns the SDK store.");
#else
        QFETCH(QString, typedServer);
        QFETCH(QString, typedUser);
        QFETCH(QString, authType);
        QFETCH(bool, adopted);
        QFETCH(QString, code);
        InMemorySecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        const QString server = QStringLiteral("https://localhost:1");
        const QString lia = QStringLiteral("@lia:localhost:1");
        settings.saveSession(server, lia, QStringLiteral("OLDDEVICE"),
                             QStringLiteral("not-a-real-token"), QString(), authType,
                             authType == QLatin1String("oauth")
                                 ? QStringLiteral("client-id") : QString());
        if (adopted) {
            QSettings raw;
            raw.setValue(QStringLiteral("accounts/%1/keyringItems")
                             .arg(matrix::app_data::safeUserSlug(lia)),
                         QStringLiteral("adopted-unverified"));
            raw.sync();
        }
        const QString store = storeOf(lia, server);
        const QString keys = seedStore(store);
        QVERIFY(!keys.isEmpty());
        QVERIFY(secrets.clearAccountSecrets(lia));

        RustSdkMatrixClient client(&settings);
        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        QSignalSpy blocked(&client, &RustSdkMatrixClient::localSessionBlocked);
        QSignalSpy reset(&client, &RustSdkMatrixClient::localSessionResetRequired);
        client.login(typedServer, typedUser, QStringLiteral("not-a-password"));
        QCOMPARE(failed.count(), 1);
        QVERIFY(client.m_resumeDeviceId.isEmpty());
        QVERIFY(quarantinesOf(store).isEmpty());
        QVERIFY(QFileInfo::exists(keys));
        if (code == QLatin1String("keyring_lost_session")) {
            QCOMPARE(reset.count(), 0);
            QCOMPARE(blocked.count(), 1);
            QCOMPARE(blocked.last().at(0).toString(), code);
            QCOMPARE(blocked.last().at(1).toString(), lia);
            QVERIFY(!matrix::rust_session::suggestsLocalResetForCode(code));
        } else {
            QCOMPARE(reset.count(), 1);
            QCOMPARE(reset.last().at(0).toString(), code);
        }
#endif
    }

    void aSignInTheKeyringDoesNotTakeIsNotKept_data()
    {
        QTest::addColumn<bool>("locked");
        QTest::addColumn<bool>("browser");
        QTest::addColumn<bool>("known");
        for (const bool known : {false, true}) {
            const char *who = known ? "saved account" : "new account";
            QTest::addRow("password, locked, %s", who) << true << false << known;
            QTest::addRow("password, refused, %s", who) << false << false << known;
            QTest::addRow("browser, locked, %s", who) << true << true << known;
            QTest::addRow("browser, refused, %s", who) << false << true << known;
        }
    }

    void aSignInTheKeyringDoesNotTakeIsNotKept()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The Rust backend owns the SDK store.");
#else
        QFETCH(bool, locked);
        QFETCH(bool, browser);
        QFETCH(bool, known);
        RefusingSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        const QString kay = QStringLiteral("@kay:example.org");
        const QString oldDevice = QStringLiteral("OLDDEVICE");
        const QString oldToken = QStringLiteral("old-not-a-real-token");
        QString keys;
        if (known) {
            QVERIFY(settings.saveSession(QString::fromLatin1(kServer), kay, oldDevice,
                                         oldToken));
            keys = seedStore(storeOf(kay));
            QVERIFY(!keys.isEmpty());
        }
        secrets.locked = locked;
        secrets.refusing = !locked;
        RustSdkMatrixClient client(&settings);
        QSignalSpy succeeded(&client, &MatrixClient::loginSucceeded);
        QSignalSpy failed(&client, &MatrixClient::loginFailed);

        // A browser sign-in may only continue the device its store belongs to.
        const QString device = known && browser ? oldDevice : QStringLiteral("NEWDEVICE");
        Restore restore;
        if (browser) {
            adopt(client, kay, device, restore);
        } else {
            QJsonObject accepted;
            accepted.insert(QStringLiteral("type"), QStringLiteral("login_ok"));
            accepted.insert(QStringLiteral("homeserver"), QString::fromLatin1(kServer));
            accepted.insert(QStringLiteral("user_id"), kay);
            accepted.insert(QStringLiteral("device_id"), device);
            accepted.insert(QStringLiteral("access_token"), QStringLiteral("not-a-real-token"));
            client.handleRustEventForTest(accepted);
        }

        QCOMPARE(succeeded.count(), 0);
        QCOMPARE(failed.count(), 1);
        QVERIFY2(failed.last().at(0).toString().contains(QStringLiteral("keyring")),
                 qPrintable(failed.last().at(0).toString()));
        QCOMPARE(restore.calls, 0);
        QVERIFY(!client.isLoggedIn());
        QVERIFY(client.currentUserId().isEmpty());
        if (!known) {
            QVERIFY(!settings.hasSavedAccount(kay));
            QVERIFY(settings.activeAccountUserId().isEmpty());
            return;
        }
        QVERIFY(settings.hasSavedAccount(kay));
        QCOMPARE(settings.accountRecord(kay).value(QStringLiteral("deviceId")).toString(),
                 oldDevice);
        QCOMPARE(settings.activeAccountUserId(), kay);
        QVERIFY(QFileInfo::exists(keys));
        QCOMPARE(settings.accessTokenFor(kay), oldToken);
#endif
    }

    void aSignInIsRefusedBeforeTheServerWhileTheKeyringIsLocked()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Browser sign-in exists on the Rust backend only.");
#else
        InMemorySecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        RustSdkMatrixClient client(&settings);
        AuthManager auth(&client);
        int asked = 0;
        auth.setKeyringReady([&asked] {
            ++asked;
            return false;
        });
        QSignalSpy failed(&auth, &AuthManager::loginFailed);
        const QString server = QStringLiteral("https://localhost:1");

        auth.login(server, QStringLiteral("kay"), QStringLiteral("not-a-password"));
        QCOMPARE(asked, 1);
        QCOMPARE(failed.count(), 1);
        QVERIFY(!auth.isLoggingIn());
        QVERIFY2(auth.lastError().contains(QStringLiteral("keyring")),
                 qPrintable(auth.lastError()));

        auth.beginBrowserLogin(server);
        auth.beginSsoLogin(server, QString());
        QCOMPARE(asked, 3);
        QCOMPARE(failed.count(), 3);
        QVERIFY(!auth.browserLoginInProgress());
        QVERIFY(!auth.isLoggingIn());
#endif
    }
};

QTEST_MAIN(BrowserSignInRollbackTest)
#include "BrowserSignInRollbackTest.moc"
