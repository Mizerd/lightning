// A browser sign-in whose restore fails must leave nothing that blocks the
// next one (D6, reported live 2026-09-29: "logged in, so it won't allow me to
// relog in, but I also can't log out").
//
// adoptBrowserSession() writes the account record before the restore, and
// opening the handle creates the store directory. A restore that then failed
// left both behind, and the next browser sign-in (a new device) was refused as
// "already signed in on this device" with no way out on the login screen.
//
// Drives the real Phase B with a stand-in restore (no network, no SDK
// restore), and the real login_failed handler with the payload a failed
// restore sends.

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

    // What a restore that failed in the SDK sends.
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

    // A store moved aside earlier ("Quarantine and rebuild", or login()'s own
    // quarantine of an unclaimed store): possibly the only copy of the keys.
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

    // A store with a stand-in crypto database; returns the database's path.
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

    // The same database, wherever the one quarantine of `store` put it.
    static bool keysAside(const QString &store)
    {
        const QStringList aside = quarantinesOf(store);
        return aside.size() == 1
               && QFileInfo::exists(QFileInfo(store).absolutePath() + QLatin1Char('/')
                                    + aside.first()
                                    + QStringLiteral("/matrix-sdk-crypto.sqlite3"));
    }

    // What the app sees when the server ends a running session: the restore
    // ran as `deviceId`, the SDK reported the revocation, and AppController
    // ended the session locally (a detach: nothing deleted).
    // `softLogout` undefined sends the event without the field.
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

    // The reported state: the restore fails after Phase B wrote the record
    // and the handle created the store. The next sign-in, a new device, must
    // go ahead.
    void aFailedRestoreLeavesNothingThatBlocksTheNextSignIn()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Browser sign-in exists on the Rust backend only.");
#else
        SettingsManager settings;
        // Never the real keyring.
        settings.setSecretStore(nullptr);
        RustSdkMatrixClient client(&settings);
        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        const QString alice = QStringLiteral("@alice:example.org");

        Restore restore;
        adopt(client, alice, QStringLiteral("DEVICEONE"), restore);
        QCOMPARE(restore.calls, 1);
        // The attempt did write both before restoring.
        QVERIFY(settings.hasSavedAccount(alice));
        QVERIFY(QFileInfo::exists(storeOf(alice)));

        client.handleRustEventForTest(restoreFailed());
        QCOMPARE(failed.count(), 1);
        QVERIFY2(failed.last().at(0).toString().contains(QStringLiteral("nothing was kept")),
                 qPrintable(failed.last().at(0).toString()));
        QVERIFY2(!settings.hasSavedAccount(alice), "the failed attempt's record survived");
        QVERIFY2(!QFileInfo::exists(storeOf(alice)), "the failed attempt's store survived");

        // The next browser sign-in reaches its restore instead of "already
        // signed in on this device".
        adopt(client, alice, QStringLiteral("DEVICETWO"), restore);
        QCOMPARE(restore.calls, 2);
#endif
    }

    // The same when the restore could not even start.
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

    // Only what the attempt created goes. Signing in again as the device that
    // owns an existing store and record, a failed restore leaves both: they
    // belong to a session that may still work.
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

    // The rollback takes back the store the attempt opened and nothing else:
    // a quarantined store beside it survives a failed restore, asynchronous
    // or immediate (review M1: the account-wide removal deleted it).
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

    // The same on the password path, where it has been since login() learned
    // to quarantine an unclaimed store ("moved aside, never deleted") and a
    // wrong password then deleted that quarantine seconds later. A real login
    // against a closed port on this machine: no network, a real failure.
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
        // login() moved the unclaimed store aside before trying.
        QCOMPARE(quarantinesOf(store).size(), 1);
        QVERIFY(failed.wait(60000));

        const QStringList left = quarantinesOf(store);
        QCOMPARE(left.size(), 1);
        QVERIFY2(QFileInfo::exists(QFileInfo(store).absolutePath() + QLatin1Char('/')
                                   + left.first() + QStringLiteral("/matrix-sdk-crypto.sqlite3")),
                 "the failed sign-in deleted the store it had just quarantined");
#endif
    }

    // Slug flattening is not injective: "@a:b_example.org" and
    // "@a_b:example.org" share a storage name. A browser sign-in as the one
    // must not proceed onto the other's record and store (review S2).
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
        // No card: nothing destructive is offered against the other account.
        QCOMPARE(blocked.count(), 0);
        QCOMPARE(reset.count(), 0);
        QVERIFY(settings.hasSavedAccount(saved));
        QVERIFY(QFileInfo::exists(storeOf(saved)));
#endif
    }

    // A store with no saved account beside it (a rollback or removal that
    // could not finish, a crash, an older build) is moved aside as login()
    // does, never deleted, and the sign-in goes on. On a server with its own
    // sign-in page there is no password form, so refusing it was a dead end
    // (review S6).
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

    // Unless a saved account differs from this one only by case: on a
    // case-insensitive file system that store may be theirs. Never moved,
    // no destructive card, and the refusal says what to do.
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

    // A saved record with no device: the refusal carries the account, so the
    // login screen offers its rebuild card instead of a bare "sign in again".
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

    // A session that really is still there blocks a sign-in as a new device,
    // as before, but now with the account named so the login screen can offer
    // the way out (open it, or remove it and sign in again).
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
        // Nothing was touched by the refusal.
        QVERIFY(settings.hasSavedAccount(dave));
        QVERIFY(QFileInfo::exists(storeOf(dave)));
#endif
    }

    // The loop reported 2026-09-30: once the server has ended this device
    // (deleted, or "sign out other devices"), its store can never be restored,
    // and a password sign-in was sent to "Open it", which restored it and met
    // the revocation again. The only exit was "Remove this account", which
    // deletes the keys. Now the store is moved aside and the sign-in goes on.
    // A real login against a closed port on this machine: no network.
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
        // Not redirected (that answer is synchronous), and the old store is
        // aside before anything was tried.
        QCOMPARE(failed.count(), 0);
        QVERIFY2(keysAside(store), "the revoked device's store was not moved aside");
        QVERIFY(!QFileInfo::exists(keys));

        // The attempt fails here (closed port) and takes back only its own
        // fresh store; the old one stays aside.
        QVERIFY(failed.wait(60000));
        QVERIFY2(failed.last().at(0).toString()
                     != matrix::rust_session::userMessage(
                         matrix::rust_session::StoreBlockReason::ExistingStoreNeedsRestore),
                 "sent to \"Open it\" after the store was moved");
        QVERIFY2(keysAside(store), "a failed attempt removed the store it moved aside");

        // When the server accepts the next attempt, the record follows the
        // new device; the old store is still kept.
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

    // Short of that proof, a store with a saved session is never moved: it
    // may still restore, so the sign-in is sent to "Open it" as before.
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
        // A soft logout is a proof of its own, for resuming the device; its
        // cases are in aPasswordSignInAfterASoftLogoutResumesTheSameDevice
        // and aSoftLogoutResumesOnlyThatAccountAndDevice.
        // The record names a device other than the one that was ended.
        QTest::newRow("another device") << true << false << hana
                                        << QStringLiteral("OTHERDEVICE") << false;
        QTest::newRow("another account") << true << false
                                         << QStringLiteral("@ivan:localhost:1")
                                         << QStringLiteral("OLDDEVICE") << false;
        // Review L1: a session ran as that account again since, so the store
        // is healthy and the proof is spent.
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

    // The browser path, after the same proof: the old store goes aside and
    // the new device gets a fresh one, even when the server names the same
    // device id again (that device is gone; its store's keys are not its).
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
        // A fresh store for the new session, and not the old one.
        QVERIFY(QFileInfo::exists(storeOf(jo)));
        QVERIFY(!QFileInfo::exists(keys));
        QCOMPARE(settings.accountRecord(jo).value(QStringLiteral("deviceId")).toString(),
                 newDevice);
#endif
    }

    // And without the proof the browser path refuses as before
    // (aBlockingSessionIsReportedWithTheWayOut covers nothing revoked). A
    // password account's soft logout is a proof for resuming, never for
    // moving its store.
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

    // Review MINOR 4: an OAuth session that the SDK calls soft-logged-out
    // cannot be continued from here (the refresh was refused, and the browser
    // sign-in never asks for this device again), so it counts as ended: the
    // card is the "signed out remotely" one, and the browser sign-in moves
    // the store aside for a new device instead of refusing for ever.
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

    // Review MAJOR 2: the device was removed on the server after the soft
    // logout, so the resumed sign-in re-created it under the same id with no
    // keys, and this store would never upload them again. Once the server has
    // ANSWERED that it publishes no key (or another one) for the device, the
    // session ends with the hard proof and the next sign-in moves the store
    // aside for a new device. "matches" and "no answer" end nothing.
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

        // The server accepts the resume: the check is armed for exactly that
        // account and device.
        QJsonObject accepted;
        accepted.insert(QStringLiteral("type"), QStringLiteral("login_ok"));
        accepted.insert(QStringLiteral("homeserver"), server);
        accepted.insert(QStringLiteral("user_id"), vic);
        accepted.insert(QStringLiteral("device_id"), QStringLiteral("OLDDEVICE"));
        accepted.insert(QStringLiteral("access_token"), QStringLiteral("not-a-real-token"));
        client.handleRustEventForTest(accepted);
        QCOMPARE(client.m_resumedCheckDeviceId, QStringLiteral("OLDDEVICE"));

        QSignalSpy blocked(&client, &RustSdkMatrixClient::localSessionBlocked);
        // An answer about another device is not this check's.
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

        // AppController ends it locally; the next sign-in is a new device.
        client.detachSession();
        const int before = failed.count();
        client.login(server, QStringLiteral("vic"), QStringLiteral("not-a-password"));
        QCOMPARE(failed.count(), before);
        QVERIFY(client.m_resumeDeviceId.isEmpty());
        QVERIFY2(keysAside(store), "the lost device's store was resumed again, not moved aside");
        QVERIFY(failed.wait(60000));
#endif
    }

    // Review MINOR 3: a resume that the store refuses (the server did not hand
    // the same device back) must not offer to reset a healthy store. The same
    // message on an ordinary sign-in still arms the reset, as before.
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

    // Reported 2026-09-30: after a SOFT logout (the server keeps the device;
    // Synapse says so when a token expires) the password sign-in was sent to
    // "Open it", which restored the dead token and met the same answer, for
    // ever. Now it signs in again as that device, into the same store: no
    // redirect, nothing moved aside, and the record keeps the device.
    // A real login against a closed port on this machine: no network.
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
        // Not redirected (that answer is synchronous), asking for the saved
        // device, and the store left exactly where it is.
        QCOMPARE(failed.count(), 0);
        QCOMPARE(client.m_resumeDeviceId, QStringLiteral("OLDDEVICE"));
        QVERIFY(quarantinesOf(store).isEmpty());
        QVERIFY(QFileInfo::exists(keys));

        // A failed attempt (closed port) keeps the store and the proof: the
        // next try resumes again.
        QVERIFY(failed.wait(60000));
        QVERIFY(failed.last().at(0).toString()
                != matrix::rust_session::userMessage(
                    matrix::rust_session::StoreBlockReason::ExistingStoreNeedsRestore));
        QVERIFY(client.m_resumeDeviceId.isEmpty());
        QVERIFY(QFileInfo::exists(keys));
        client.login(server, QStringLiteral("nia"), QStringLiteral("not-a-password"));
        QCOMPARE(client.m_resumeDeviceId, QStringLiteral("OLDDEVICE"));
        QVERIFY(failed.wait(60000));

        // The server accepts: the same device, the same store, the record
        // unchanged, and the proof spent.
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

    // The proof covers exactly the account and device the SDK named, and a
    // sign-in typed as exactly that account. Anything else is sent to "Open
    // it" as before, with nothing moved.
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
        // Localparts are case-sensitive: "Oto" may be somebody else, and the
        // device id would then be asked for on their account.
        QTest::newRow("another casing typed") << server << QStringLiteral("Oto") << oto
                                              << QStringLiteral("OLDDEVICE")
                                              << QJsonValue(true);
        // Review MAJOR 1: the full id typed with another server. The password
        // and this device id must never go there, and the record must never
        // be rewritten to it.
        QTest::newRow("another homeserver typed") << QStringLiteral("https://localhost:2")
                                                  << oto << oto
                                                  << QStringLiteral("OLDDEVICE")
                                                  << QJsonValue(true);
        // The SDK event without the field proves nothing.
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

    // Soft first, then the device ended for good: the hard proof wins, so the
    // store goes aside for a new device rather than being resumed.
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

    // The card needs to know which of the two it is.
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

    // Review L1: the proof is used once. The server re-issued the same device
    // id, so the record names X/D again; a later sign-in in this process meets
    // the new session's healthy store and must not move it too.
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

    // Review L2: moving a revoked store aside releases only a handle on THAT
    // store. During add-account another account's session runs; when the move
    // fails, that session is left running on its own store.
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

        // walt runs, on its own store.
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

        // xena's store cannot be moved: its account directory is read-only.
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

    // Reported 2026-09-30 on a Flatpak: the keyring came back without the
    // sign-in this install had saved (a portal secret that changed, or
    // libsecret switching between its file and Secret Service backends). The
    // store answered "no such item", so the password sign-in was classified
    // store_without_session_metadata and offered "Quarantine and rebuild": a
    // new device, the old one's store moved aside. The account had not signed
    // out. Now it continues as the recorded device, into the same store.
    // A real login against a closed port on this machine: no network.
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
        // The keyring answers, and no longer has it.
        QVERIFY(secrets.clearAccountSecrets(kai));
        QVERIFY(settings.accessTokenFor(kai).isEmpty());
        QVERIFY(!settings.secretBackendUnavailable());
        QVERIFY(settings.secretsWrittenHereForRecordedDevice(kai));

        RustSdkMatrixClient client(&settings);
        QSignalSpy failed(&client, &MatrixClient::loginFailed);
        QSignalSpy reset(&client, &RustSdkMatrixClient::localSessionResetRequired);
        client.login(server, QStringLiteral("kai"), QStringLiteral("not-a-password"));
        // Not refused (that answer is synchronous), asking for the recorded
        // device, and nothing moved or offered for reset.
        QCOMPARE(failed.count(), 0);
        QCOMPARE(reset.count(), 0);
        QCOMPARE(client.m_resumeDeviceId, QStringLiteral("OLDDEVICE"));
        QVERIFY(quarantinesOf(store).isEmpty());
        QVERIFY(QFileInfo::exists(keys));
        // The closed port fails the attempt; the store stays, nothing is armed.
        QVERIFY(failed.wait(60000));
        QCOMPARE(reset.count(), 0);
        QVERIFY(quarantinesOf(store).isEmpty());
        QVERIFY(QFileInfo::exists(keys));
#endif
    }

    // The same lost sign-in, but a sign-in that must not continue as that
    // device: the card that says what happened, never the rebuild. A session
    // adopted from before install scoping may be another install's device, so
    // it keeps the old verdict.
    void aLostSignInThatCannotContinueIsExplainedNotRebuilt_data()
    {
        QTest::addColumn<QString>("typedServer");
        QTest::addColumn<QString>("typedUser");
        QTest::addColumn<QString>("authType");
        QTest::addColumn<bool>("adopted");
        QTest::addColumn<QString>("code");
        const QString card = QStringLiteral("keyring_lost_session");
        // The password and the device id go to no other server.
        QTest::newRow("another homeserver typed")
            << QStringLiteral("https://localhost:2") << QStringLiteral("@lia:localhost:1")
            << QStringLiteral("password") << false << card;
        // A browser sign-in here cannot ask for a device.
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
};

QTEST_MAIN(BrowserSignInRollbackTest)
#include "BrowserSignInRollbackTest.moc"
