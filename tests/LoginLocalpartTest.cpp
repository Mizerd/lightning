// A bare username signs in when the server address is not the server name
// (break-it rerun 2026-10-16: server http://127.0.0.1:18008 whose server name
// is localhs.test; "bk010" got "There is no account with that username" while
// "@bk010:localhs.test" and an API login with user=bk010 both worked).
//
// The client completed the username with the ADDRESS's host and sent
// "@bk010:127.0.0.1:18008", an account that does not exist. The spec's way,
// and every mainstream client's, is to send the typed username as the
// m.id.user identifier and let the server name the account; the record, and
// the store it records, then follow the server's answer (CLAUDE.md §6).
//
// Real RustSdkMatrixClient, SettingsManager and Rust login, against a loopback
// homeserver that knows exactly one account, @bk010:localhs.test, and answers
// like Continuwuity: 404 M_NOT_FOUND for any identifier that is not it. A
// fixture keyring, never the host's.

#include "app/SettingsManager.h"
#include "matrix/RustSdkMatrixClient.h"
#include "matrix/RustSessionPolicy.h"
#include "storage/AppDataPaths.h"
#include "storage/SecretStore.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

constexpr auto kCanonical = "@bk010:localhs.test";
constexpr auto kPassword = "synthetic-fixture-only";

// A keyring in a hash. No Q_OBJECT: it adds nothing to SecretStore's
// meta-object.
class FixtureSecrets final : public SecretStore
{
public:
    using SecretStore::SecretStore;
    bool isSecure() const override { return true; }
    bool isAvailable() const override { return true; }
    QString backendName() const override { return QStringLiteral("fixture"); }
    bool storeSecret(const QString &u, const QString &k, const QString &v) override
    {
        m_values.insert(u + QLatin1Char('|') + k, v);
        return true;
    }
    QString readSecret(const QString &u, const QString &k) const override
    {
        return m_values.value(u + QLatin1Char('|') + k);
    }
    bool deleteSecret(const QString &u, const QString &k) override
    {
        m_values.remove(u + QLatin1Char('|') + k);
        return true;
    }
    bool clearAccountSecrets(const QString &u) override
    {
        const QString prefix = u + QLatin1Char('|');
        for (auto it = m_values.begin(); it != m_values.end();)
            it = it.key().startsWith(prefix) ? m_values.erase(it) : std::next(it);
        return true;
    }
    QString lastError() const override { return {}; }

private:
    QHash<QString, QString> m_values;
};

// One account, @bk010:localhs.test, served from an address whose host is
// 127.0.0.1. A password login names it by its localpart or its full id;
// anything else is "This account does not exist." Each login asked is
// recorded by the identifier and the device id it carried (empty when it
// asked for a new device); a device id asked for is the one granted.
// Everything else is 404.
class LocalpartHomeserver : public QObject
{
public:
    QStringList loginIdentifiers;
    QStringList loginDeviceIds;
    int loginsAnswered = 0;

    quint16 listen()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this,
                        [this, socket] { serve(socket); });
                connect(socket, &QTcpSocket::disconnected, socket,
                        &QObject::deleteLater);
            }
        });
        return m_server.listen(QHostAddress::LocalHost, 0) ? m_server.serverPort() : 0;
    }

private:
    static void reply(QTcpSocket *socket, int code, const QByteArray &body)
    {
        socket->write("HTTP/1.1 " + QByteArray::number(code) + " X\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: " + QByteArray::number(body.size())
                      + "\r\n\r\n" + body);
    }

    void serve(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        for (;;) {
            const qsizetype end = buffer.indexOf("\r\n\r\n");
            if (end < 0)
                return;
            qsizetype length = 0;
            const QList<QByteArray> lines = buffer.left(end).split('\n');
            for (const QByteArray &raw : lines) {
                const QByteArray line = raw.trimmed();
                if (line.toLower().startsWith("content-length:"))
                    length = line.mid(15).trimmed().toLongLong();
            }
            if (buffer.size() < end + 4 + length)
                return;
            const QList<QByteArray> request = lines.value(0).trimmed().split(' ');
            const QByteArray method = request.value(0);
            const QByteArray path = request.value(1).split('?').value(0);
            const QByteArray body = buffer.mid(end + 4, length);
            buffer.remove(0, end + 4 + length);
            if (path == "/_matrix/client/versions") {
                reply(socket, 200,
                      R"({"versions":["v1.1","v1.11"],"unstable_features":{}})");
            } else if (path == "/_matrix/client/v3/login" && method == "GET") {
                reply(socket, 200, R"({"flows":[{"type":"m.login.password"}]})");
            } else if (path == "/_matrix/client/v3/login" && method == "POST") {
                const QJsonObject request = QJsonDocument::fromJson(body).object();
                const QJsonObject identifier =
                    request.value(QStringLiteral("identifier")).toObject();
                const QString user = identifier.value(QStringLiteral("user")).toString();
                const QString askedDevice =
                    request.value(QStringLiteral("device_id")).toString();
                loginIdentifiers.append(user);
                loginDeviceIds.append(askedDevice);
                ++loginsAnswered;
                if (identifier.value(QStringLiteral("type")).toString()
                        == QLatin1String("m.id.user")
                    && (user == QLatin1String("bk010")
                        || user == QLatin1String(kCanonical))) {
                    const QJsonObject answer{
                        {QStringLiteral("user_id"), QString::fromLatin1(kCanonical)},
                        {QStringLiteral("access_token"), QStringLiteral("tok-new")},
                        {QStringLiteral("device_id"),
                         askedDevice.isEmpty() ? QStringLiteral("NEWDEVICE") : askedDevice},
                        {QStringLiteral("home_server"), QStringLiteral("localhs.test")}};
                    reply(socket, 200, QJsonDocument(answer).toJson(QJsonDocument::Compact));
                } else {
                    reply(socket, 404,
                          R"({"errcode":"M_NOT_FOUND","error":"This account does not exist."})");
                }
            } else {
                reply(socket, 404, R"({"errcode":"M_UNRECOGNIZED","error":"not here"})");
            }
        }
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

} // namespace

class LoginLocalpartTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_home;

    // Every SDK store directory under the data root, absolute and cleaned.
    static QStringList storesOnDisk()
    {
        QStringList out;
        const QString root = matrix::app_data::primaryRoot();
        QDirIterator it(root, QDir::Dirs | QDir::NoDotAndDotDot);
        while (it.hasNext()) {
            const QString store = it.next() + QStringLiteral("/matrix-rust-sdk-store");
            if (QFileInfo(store).isDir())
                out.append(QDir::cleanPath(QFileInfo(store).absoluteFilePath()));
        }
        out.sort();
        return out;
    }

    static QString clean(const QString &path)
    {
        return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    }

    // A saved, signed-in account on `server` whose store holds a stand-in
    // key database; returns that database's path.
    static QString saveSignedIn(SettingsManager &settings, const QString &server)
    {
        settings.saveSession(server, QString::fromLatin1(kCanonical),
                             QStringLiteral("OLDDEVICE"), QStringLiteral("tok-old"));
        matrix::app_data::AccountIdentity record;
        if (!settings.resolveSavedIdentity(QString::fromLatin1(kCanonical), &record)
            || !QDir().mkpath(record.rustStorePath)) {
            return {};
        }
        QFile keys(record.rustStorePath + QStringLiteral("/matrix-sdk-crypto.sqlite3"));
        if (!keys.open(QIODevice::WriteOnly))
            return {};
        keys.write("keys");
        return keys.fileName();
    }

    // The keyring lost the sign-in of a saved account this install signed in
    // itself: a record, its store directory (empty, so a client that did
    // continue as the device could open it and send the login), and no
    // token. The state a lost-sign-in resume answers.
    static bool saveWithLostSignIn(SettingsManager &settings, FixtureSecrets &secrets,
                                   const QString &server)
    {
        const QString canonical = QString::fromLatin1(kCanonical);
        settings.saveSession(server, canonical, QStringLiteral("OLDDEVICE"),
                             QStringLiteral("tok-old"));
        matrix::app_data::AccountIdentity record;
        return settings.resolveSavedIdentity(canonical, &record)
            && QDir().mkpath(record.rustStorePath)
            && secrets.clearAccountSecrets(canonical)
            && settings.accessTokenFor(canonical).isEmpty()
            && !settings.secretBackendUnavailable()
            && settings.secretsWrittenHereForRecordedDevice(canonical);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        qputenv("XDG_CONFIG_HOME", (m_home.path() + QStringLiteral("/config")).toUtf8());
        qputenv("XDG_DATA_HOME", (m_home.path() + QStringLiteral("/data")).toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("login-localpart-test"));
    }

    void init()
    {
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));
        QSettings raw;
        raw.clear();
        raw.sync();
        QDir(m_home.path() + QStringLiteral("/data")).removeRecursively();
    }

    // The reported case. The server receives the username as typed, names the
    // account, and the record and its store follow that answer.
    void aBareUsernameIsSentAsTypedAndTheServerNamesTheAccount()
    {
        LocalpartHomeserver homeserver;
        const quint16 port = homeserver.listen();
        QVERIFY(port);
        const QString server = QStringLiteral("http://127.0.0.1:%1").arg(port);
        const QString canonical = QString::fromLatin1(kCanonical);
        FixtureSecrets secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        {
            RustSdkMatrixClient client(&settings);
            QSignalSpy succeeded(&client, &MatrixClient::loginSucceeded);
            QSignalSpy failed(&client, &MatrixClient::loginFailed);
            client.login(server, QStringLiteral("bk010"), QString::fromLatin1(kPassword));
            QTRY_VERIFY_WITH_TIMEOUT(!succeeded.isEmpty() || !failed.isEmpty(), 30000);
            QVERIFY2(failed.isEmpty(),
                     qPrintable(failed.value(0).value(0).toString()));
            // Old code: "@bk010:127.0.0.1:<port>", completed with the address.
            QCOMPARE(homeserver.loginIdentifiers, QStringList{QStringLiteral("bk010")});
            QCOMPARE(succeeded.first().at(0).toString(), canonical);
            QCOMPARE(client.currentUserId(), canonical);

            // Saved under the server's answer, never under the stand-in.
            QVERIFY(settings.hasSavedAccount(canonical));
            QVERIFY(!settings.hasSavedAccount(
                QStringLiteral("@bk010:127.0.0.1:%1").arg(port)));
            QCOMPARE(settings.savedAccountUserIds(), QStringList{canonical});
            QCOMPARE(settings.accessTokenFor(canonical), QStringLiteral("tok-new"));
            QCOMPARE(settings.accountRecord(canonical).value(QStringLiteral("deviceId"))
                         .toString(),
                     QStringLiteral("NEWDEVICE"));

            // The record names the one store that exists, which every reader
            // (restore, sign-out, removal) resolves through it.
            matrix::app_data::AccountIdentity record;
            QVERIFY(settings.resolveSavedIdentity(canonical, &record));
            QCOMPARE(storesOnDisk(), QStringList{clean(record.rustStorePath)});
            QCOMPARE(settings.accountOwningStoreSlug(record.effectiveStoreSlug()),
                     canonical);
            QVERIFY(client.detachSession());
        }
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));
        // Detaching keeps the account and its store.
        matrix::app_data::AccountIdentity record;
        QVERIFY(settings.resolveSavedIdentity(canonical, &record));
        QVERIFY(QFileInfo::exists(record.rustStorePath));
    }

    // A full Matrix ID still goes as typed.
    void aFullMatrixIdIsSentAsTyped()
    {
        LocalpartHomeserver homeserver;
        const quint16 port = homeserver.listen();
        QVERIFY(port);
        const QString server = QStringLiteral("http://127.0.0.1:%1").arg(port);
        const QString canonical = QString::fromLatin1(kCanonical);
        FixtureSecrets secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        {
            RustSdkMatrixClient client(&settings);
            QSignalSpy succeeded(&client, &MatrixClient::loginSucceeded);
            QSignalSpy failed(&client, &MatrixClient::loginFailed);
            client.login(server, canonical, QString::fromLatin1(kPassword));
            QTRY_VERIFY_WITH_TIMEOUT(!succeeded.isEmpty() || !failed.isEmpty(), 30000);
            QVERIFY2(failed.isEmpty(),
                     qPrintable(failed.value(0).value(0).toString()));
            QCOMPARE(homeserver.loginIdentifiers, QStringList{canonical});
            matrix::app_data::AccountIdentity record;
            QVERIFY(settings.resolveSavedIdentity(canonical, &record));
            QCOMPARE(storesOnDisk(), QStringList{clean(record.rustStorePath)});
            QVERIFY(client.detachSession());
        }
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));
    }

    // The account is saved for this address already. The username finds it
    // from its record, so the sign-in is the "already signed in here" redirect
    // it is for the full id: no password goes to the server, no new device,
    // and no store is built from the address beside the real one.
    void aBareUsernameFindsTheAccountSavedForThisAddress()
    {
        LocalpartHomeserver homeserver;
        const quint16 port = homeserver.listen();
        QVERIFY(port);
        const QString server = QStringLiteral("http://127.0.0.1:%1").arg(port);
        const QString canonical = QString::fromLatin1(kCanonical);
        FixtureSecrets secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        const QString keys = saveSignedIn(settings, server);
        QVERIFY(!keys.isEmpty());
        const QStringList before = storesOnDisk();
        {
            RustSdkMatrixClient client(&settings);
            QSignalSpy succeeded(&client, &MatrixClient::loginSucceeded);
            QSignalSpy failed(&client, &MatrixClient::loginFailed);
            client.login(server, QStringLiteral("bk010"), QString::fromLatin1(kPassword));
            QTRY_VERIFY_WITH_TIMEOUT(!succeeded.isEmpty() || !failed.isEmpty(), 30000);
            QVERIFY(succeeded.isEmpty());
            QCOMPARE(failed.first().at(0).toString(),
                     matrix::rust_session::userMessage(
                         matrix::rust_session::StoreBlockReason::ExistingStoreNeedsRestore));
        }
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));
        // Old code: one login with "@bk010:127.0.0.1:<port>", into a store
        // built from the address.
        QCOMPARE(homeserver.loginsAnswered, 0);
        QCOMPARE(settings.accessTokenFor(canonical), QStringLiteral("tok-old"));
        QCOMPARE(settings.accountRecord(canonical).value(QStringLiteral("deviceId"))
                     .toString(),
                 QStringLiteral("OLDDEVICE"));
        QVERIFY(QFileInfo::exists(keys));
        QCOMPARE(storesOnDisk(), before);
    }

    // The account is saved, but under ANOTHER address for the same server, so
    // no record names it before the server answers. The answer must not take
    // the record over: that would point the account at this attempt's new
    // store and orphan the one holding its keys. Nothing is saved, the
    // attempt's own store goes, and the saved account is exactly as it was.
    void anAnswerNamingAnAccountSavedElsewhereKeepsItsStore()
    {
        LocalpartHomeserver homeserver;
        const quint16 port = homeserver.listen();
        QVERIFY(port);
        const QString server = QStringLiteral("http://127.0.0.1:%1").arg(port);
        const QString otherAddress = QStringLiteral("http://localhost:%1").arg(port);
        const QString canonical = QString::fromLatin1(kCanonical);
        FixtureSecrets secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        const QString keys = saveSignedIn(settings, otherAddress);
        QVERIFY(!keys.isEmpty());
        matrix::app_data::AccountIdentity before;
        QVERIFY(settings.resolveSavedIdentity(canonical, &before));
        const QString storeSlugBefore = settings.storeSlugFor(canonical);
        const QStringList storesBefore = storesOnDisk();
        {
            RustSdkMatrixClient client(&settings);
            QSignalSpy succeeded(&client, &MatrixClient::loginSucceeded);
            QSignalSpy failed(&client, &MatrixClient::loginFailed);
            client.login(server, QStringLiteral("bk010"), QString::fromLatin1(kPassword));
            QTRY_VERIFY_WITH_TIMEOUT(!succeeded.isEmpty() || !failed.isEmpty(), 30000);
            QVERIFY(succeeded.isEmpty());
            QCOMPARE(homeserver.loginIdentifiers, QStringList{QStringLiteral("bk010")});
            const QString message = failed.first().at(0).toString();
            // Old code: "There is no account with that username …".
            QVERIFY2(message.contains(canonical)
                         && message.contains(QStringLiteral("already on this device")),
                     qPrintable(message));
            QVERIFY(!client.isLoggedIn());
            QVERIFY(client.currentUserId().isEmpty());
        }
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));
        // The saved account, untouched: its token, device, recorded store and
        // keys; and nothing new on disk.
        QCOMPARE(settings.savedAccountUserIds(), QStringList{canonical});
        QCOMPARE(settings.accessTokenFor(canonical), QStringLiteral("tok-old"));
        QCOMPARE(settings.accountRecord(canonical).value(QStringLiteral("deviceId"))
                     .toString(),
                 QStringLiteral("OLDDEVICE"));
        QCOMPARE(settings.storeSlugFor(canonical), storeSlugBefore);
        matrix::app_data::AccountIdentity after;
        QVERIFY(settings.resolveSavedIdentity(canonical, &after));
        QCOMPARE(clean(after.rustStorePath), clean(before.rustStorePath));
        QVERIFY(QFileInfo::exists(keys));
        QCOMPARE(storesOnDisk(), storesBefore);
    }

    // The control for the next case: typed exactly as the saved account, a
    // bare username whose sign-in the keyring lost continues as the
    // account's own device, as the full id does. Proves the fixture reaches
    // the resume, so the next case's "no resume" is about the casing.
    void anExactBareUsernameContinuesAsTheDeviceTheKeyringLost()
    {
        LocalpartHomeserver homeserver;
        const quint16 port = homeserver.listen();
        QVERIFY(port);
        const QString server = QStringLiteral("http://127.0.0.1:%1").arg(port);
        FixtureSecrets secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        QVERIFY(saveWithLostSignIn(settings, secrets, server));
        {
            RustSdkMatrixClient client(&settings);
            QSignalSpy succeeded(&client, &MatrixClient::loginSucceeded);
            QSignalSpy failed(&client, &MatrixClient::loginFailed);
            client.login(server, QStringLiteral("bk010"), QString::fromLatin1(kPassword));
            QTRY_VERIFY_WITH_TIMEOUT(!succeeded.isEmpty() || !failed.isEmpty(), 30000);
            QCOMPARE(homeserver.loginIdentifiers, QStringList{QStringLiteral("bk010")});
            QCOMPARE(homeserver.loginDeviceIds, QStringList{QStringLiteral("OLDDEVICE")});
            if (client.isLoggedIn())
                QVERIFY(client.detachSession());
        }
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));
    }

    // §18 review of this fix: "BK010" found @bk010:localhs.test by a
    // case-insensitive match and the identity BECAME the record, so the
    // exact-id resume checks compared the record with itself and the sign-in
    // carried @bk010's device id to the server for a name that can be another
    // account. A folded match locates the store and never resumes: the same
    // answer as typing @BK010:localhs.test, the lost-sign-in card, and no
    // password and no device id sent anywhere.
    void aCaseFoldedBareUsernameNeverContinuesAsTheSavedDevice()
    {
        LocalpartHomeserver homeserver;
        const quint16 port = homeserver.listen();
        QVERIFY(port);
        const QString server = QStringLiteral("http://127.0.0.1:%1").arg(port);
        const QString canonical = QString::fromLatin1(kCanonical);
        FixtureSecrets secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        QVERIFY(saveWithLostSignIn(settings, secrets, server));
        const QStringList storesBefore = storesOnDisk();
        {
            RustSdkMatrixClient client(&settings);
            QSignalSpy succeeded(&client, &MatrixClient::loginSucceeded);
            QSignalSpy failed(&client, &MatrixClient::loginFailed);
            client.login(server, QStringLiteral("BK010"), QString::fromLatin1(kPassword));
            QTRY_VERIFY_WITH_TIMEOUT(!succeeded.isEmpty() || !failed.isEmpty(), 30000);
            QVERIFY(succeeded.isEmpty());
            // Before the fix: one login, "BK010" with device_id OLDDEVICE.
            QCOMPARE(homeserver.loginsAnswered, 0);
            QVERIFY(homeserver.loginDeviceIds.isEmpty());
            QCOMPARE(failed.first().at(0).toString(),
                     matrix::rust_session::userMessage(
                         matrix::rust_session::StoreBlockReason::KeyringLostSession));
        }
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));
        QCOMPARE(settings.savedAccountUserIds(), QStringList{canonical});
        QCOMPARE(settings.accountRecord(canonical).value(QStringLiteral("deviceId"))
                     .toString(),
                 QStringLiteral("OLDDEVICE"));
        QCOMPARE(storesOnDisk(), storesBefore);
    }

    // A bare username the records do not locate (the address is spelled with
    // another scheme) whose stand-in store directory is the one recorded for
    // a saved account. The block names that account, so its reset card acts
    // on the record and the store together; for the stand-in id it matched
    // no record and would have moved the account's store aside and kept the
    // record pointing at nothing.
    void aBlockOnAStandInStoreNamesTheAccountItIsRecordedFor()
    {
        const quint16 port = 18008;
        const QString server = QStringLiteral("http://127.0.0.1:%1").arg(port);
        const QString canonical = QString::fromLatin1(kCanonical);
        const QString standInSlug = matrix::app_data::safeUserSlug(
            QStringLiteral("@bk010:127.0.0.1:%1").arg(port));
        FixtureSecrets secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        settings.saveSession(server, canonical, QStringLiteral("OLDDEVICE"),
                             QStringLiteral("tok-old"));
        // As a first bare-username sign-in records it.
        settings.setStoreSlugFor(canonical, standInSlug);
        QCOMPARE(settings.storeSlugFor(canonical), standInSlug);
        matrix::app_data::AccountIdentity record;
        QVERIFY(settings.resolveSavedIdentity(canonical, &record));
        QVERIFY(QDir().mkpath(record.rustStorePath));
        QCOMPARE(settings.accountOwningStoreSlug(standInSlug), canonical);
        // No sign-in this install can prove it wrote: the store-without-a-
        // sign-in block, which arms the reset card.
        QVERIFY(secrets.clearAccountSecrets(canonical));
        QVERIFY(settings.accessTokenFor(canonical).isEmpty());
        {
            QSettings raw;
            raw.setValue(QStringLiteral("accounts/%1/keyringItems")
                             .arg(matrix::app_data::safeUserSlug(canonical)),
                         QStringLiteral("adopted-unverified"));
            raw.sync();
        }
        QVERIFY(!settings.secretsWrittenHereForRecordedDevice(canonical));
        QVERIFY(!settings.keyringItemsDeclinedForOtherInstall(canonical));
        {
            RustSdkMatrixClient client(&settings);
            QSignalSpy failed(&client, &MatrixClient::loginFailed);
            QSignalSpy reset(&client, &RustSdkMatrixClient::localSessionResetRequired);
            // Nothing listens here: the block is decided before any request.
            client.login(QStringLiteral("https://127.0.0.1:%1").arg(port),
                         QStringLiteral("bk010"), QString::fromLatin1(kPassword));
            QTRY_VERIFY_WITH_TIMEOUT(!failed.isEmpty(), 30000);
            QCOMPARE(failed.first().at(0).toString(),
                     matrix::rust_session::userMessage(
                         matrix::rust_session::StoreBlockReason::MissingSessionMetadata));
            QCOMPARE(reset.size(), 1);
            // Before: "@bk010:127.0.0.1:18008", which names no record.
            QCOMPARE(reset.first().at(1).toString(), canonical);
            QCOMPARE(reset.first().at(2).toString(), record.homeserver);
        }
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));
        // Reported, never acted on: the record and its store are as they were.
        QCOMPARE(settings.storeSlugFor(canonical), standInSlug);
        QVERIFY(QFileInfo(record.rustStorePath).isDir());
    }

    // R6: login() records the sign-in as unfinished before the SDK creates
    // its store, so a store left by a sign-in that never got an answer (a
    // cancel, a quit, a kill) is not counted at the next start as an account
    // the settings file lost. A failed attempt rolls its store back and the
    // next start drops the marker; a finished one leaves none, although the
    // marker is the typed stand-in's and the record the server's.
    void aSignInIsUnfinishedUntilItsAnswerIsRecorded()
    {
        LocalpartHomeserver homeserver;
        const quint16 port = homeserver.listen();
        QVERIFY(port);
        const QString server = QStringLiteral("http://127.0.0.1:%1").arg(port);
        const auto markers = [] {
            QSettings raw;
            raw.beginGroup(QStringLiteral("pendingSignIn"));
            return raw.childGroups();
        };
        FixtureSecrets secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        {
            RustSdkMatrixClient client(&settings);
            QSignalSpy succeeded(&client, &MatrixClient::loginSucceeded);
            QSignalSpy failed(&client, &MatrixClient::loginFailed);
            // An account the server does not have: refused after the store
            // was created.
            client.login(server, QStringLiteral("nobody"), QString::fromLatin1(kPassword));
            QTRY_VERIFY_WITH_TIMEOUT(!succeeded.isEmpty() || !failed.isEmpty(), 30000);
            QVERIFY(succeeded.isEmpty());
        }
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));
        // Old code: none, so a store left by a kill here was a "lost" account.
        QCOMPARE(markers().size(), 1);
        {
            SettingsManager next;
            QCOMPARE(next.settingsReadProblem(), int(SettingsManager::NoReadProblem));
        }
        QVERIFY2(markers().isEmpty(), "a rolled-back attempt kept its marker");

        {
            RustSdkMatrixClient client(&settings);
            QSignalSpy succeeded(&client, &MatrixClient::loginSucceeded);
            QSignalSpy failed(&client, &MatrixClient::loginFailed);
            client.login(server, QStringLiteral("bk010"), QString::fromLatin1(kPassword));
            QTRY_VERIFY_WITH_TIMEOUT(!succeeded.isEmpty() || !failed.isEmpty(), 30000);
            QVERIFY2(failed.isEmpty(), qPrintable(failed.value(0).value(0).toString()));
            QCOMPARE(succeeded.first().at(0).toString(), QString::fromLatin1(kCanonical));
            QVERIFY2(markers().isEmpty(), "a finished stand-in sign-in kept its marker");
            QVERIFY(client.detachSession());
        }
        QVERIFY(RustSdkMatrixClient::waitForRustRetirement(30000));
    }
};

QTEST_MAIN(LoginLocalpartTest)
#include "LoginLocalpartTest.moc"
