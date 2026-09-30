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
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix/RustSdkMatrixClient.h"
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
};

QTEST_MAIN(BrowserSignInRollbackTest)
#include "BrowserSignInRollbackTest.moc"
