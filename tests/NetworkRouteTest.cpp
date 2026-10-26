// The per-account network route (src/matrix/NetworkRoute.h,
// docs/network-profiles.md): where each half is kept, that the password never
// comes back out except to Rust, that it goes with the account, and that every
// state that cannot be used reaches Rust as "unavailable", never as "none".
// "none" would build the client direct, which is exactly what a user who set
// a proxy asked not to happen (fail closed).
//
// Against a fake SecretStore with the real contract: a miss with
// lastReadFailed() false is an answer, true is "could not ask".

#include "app/NetworkRouteController.h"
#include "app/SettingsManager.h"
#include "matrix/NetworkRoute.h"
#include "storage/SecretStore.h"

#include <QCoreApplication>
#include <QFile>
#include <QRegularExpression>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaProperty>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace route = matrix::network_route;

namespace {

const QString kUser = QStringLiteral("@rokas:route.example");
const QString kOther = QStringLiteral("@other:route.example");
const QString kHs = QStringLiteral("https://route.example");
const QString kPassword = QStringLiteral("pr0xy-s3cret");

class FakeStore final : public SecretStore
{
public:
    using SecretStore::SecretStore;

    bool isSecure() const override { return true; }
    bool isAvailable() const override { return available; }
    QString backendName() const override { return QStringLiteral("fake"); }
    bool storeSecret(const QString &userId, const QString &key,
                     const QString &value) override
    {
        ++writes;
        if (locked)
            return false;
        m_values.insert(mapKey(userId, key), value);
        return true;
    }
    QString readSecret(const QString &userId, const QString &key) const override
    {
        m_lastReadFailed = locked;
        return locked ? QString() : m_values.value(mapKey(userId, key));
    }
    bool deleteSecret(const QString &userId, const QString &key) override
    {
        if (locked)
            return false;
        m_values.remove(mapKey(userId, key));
        return true;
    }
    bool clearAccountSecrets(const QString &userId) override
    {
        if (locked)
            return false;
        const QString prefix = userId + QLatin1Char('\x1f');
        for (auto it = m_values.begin(); it != m_values.end();) {
            if (it.key().startsWith(prefix))
                it = m_values.erase(it);
            else
                ++it;
        }
        return true;
    }
    QString lastError() const override { return {}; }
    bool lastReadFailed() const override { return m_lastReadFailed; }

    QString get(const QString &userId, const QString &key) const
    {
        return m_values.value(mapKey(userId, key));
    }

    bool available = true;
    bool locked = false;
    int writes = 0;

private:
    static QString mapKey(const QString &userId, const QString &key)
    {
        return userId + QLatin1Char('\x1f') + key;
    }

    QHash<QString, QString> m_values;
    mutable bool m_lastReadFailed = false;
};

route::Route socks(const QString &username = {})
{
    route::Route r;
    r.kind = route::Kind::Socks5;
    r.host = QStringLiteral("127.0.0.1");
    r.port = 1080;
    r.username = username;
    return r;
}

QJsonObject parsed(const QByteArray &json)
{
    return QJsonDocument::fromJson(json).object();
}

QString state(const QByteArray &json)
{
    return parsed(json).value(QStringLiteral("state")).toString();
}

} // namespace

class NetworkRouteTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("network-route-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void validationRefusesWhatCouldNotBeAProxyAddress()
    {
        QVERIFY(route::validate(route::Route{}).isEmpty());
        QVERIFY(route::validate(socks()).isEmpty());
        route::Route r = socks();
        for (const QString &host : {QString(), QStringLiteral("user@host"),
                                    QStringLiteral("host/path"), QStringLiteral("host:80"),
                                    QStringLiteral("-host"), QStringLiteral("[zz::1]"),
                                    QStringLiteral("hést")}) {
            r.host = host;
            QCOMPARE(route::validate(r), QStringLiteral("invalid_host"));
        }
        r = socks();
        for (const int port : {0, -1, 65536}) {
            r.port = port;
            QCOMPARE(route::validate(r), QStringLiteral("invalid_port"));
        }
        r = socks(QStringLiteral("name\nwith newline"));
        QCOMPARE(route::validate(r), QStringLiteral("invalid_username"));
    }

    // A bare IPv6 literal is bracketed on save, as a proxy URL needs it.
    void aBareIpv6HostIsBracketed()
    {
        FakeStore store;
        SettingsManager settings;
        settings.setSecretStore(&store);
        settings.saveSession(kHs, kUser, QStringLiteral("DEVICE"), QStringLiteral("syt"));
        route::Route r = socks();
        r.host = QStringLiteral("::1");
        QCOMPARE(route::save(&settings, &store, kUser, r, route::PasswordChange::Keep, {}),
                 QString());
        QCOMPARE(route::load(&settings, kUser).host, QStringLiteral("[::1]"));
    }

    void thePasswordIsKeptInTheKeyringAndNeverInSettings()
    {
        FakeStore store;
        SettingsManager settings;
        settings.setSecretStore(&store);
        settings.saveSession(kHs, kUser, QStringLiteral("DEVICE"), QStringLiteral("syt"));
        QCOMPARE(route::save(&settings, &store, kUser, socks(QStringLiteral("alice")),
                             route::PasswordChange::Replace, kPassword),
                 QString());
        QCOMPARE(store.get(kUser, QLatin1String(route::kSecretName)), kPassword);

        // Nothing in the settings file carries it.
        QSettings raw;
        bool sawRecord = false;
        for (const QString &key : raw.allKeys()) {
            sawRecord = sawRecord || key.endsWith(QLatin1String("networkRoute/kind"));
            QVERIFY2(!raw.value(key).toString().contains(kPassword), qPrintable(key));
        }
        // The scan read the file the record is in, or it proved nothing.
        QVERIFY(sawRecord);
        // The record holds the rest.
        const QVariantMap record = settings.networkRouteRecord(kUser);
        QCOMPARE(record.value(QStringLiteral("kind")).toString(), QStringLiteral("socks5"));
        QCOMPARE(record.value(QStringLiteral("username")).toString(), QStringLiteral("alice"));
        QVERIFY(!record.contains(QStringLiteral("password")));
    }

    // QML is told whether one is stored, never what it is.
    void theControllerNeverHandsThePasswordToQml()
    {
        FakeStore store;
        SettingsManager settings;
        settings.setSecretStore(&store);
        settings.saveSession(kHs, kUser, QStringLiteral("DEVICE"), QStringLiteral("syt"));
        NetworkRouteController controller(&settings);
        QVERIFY(controller.available());
        QCOMPARE(controller.route().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("none"));

        QSignalSpy changed(&controller, &NetworkRouteController::changed);
        QCOMPARE(controller.save(QStringLiteral("http"), QStringLiteral("proxy.example"), 3128,
                                 QStringLiteral("alice"), kPassword, true),
                 QString());
        QVERIFY(changed.count() >= 1);
        const QVariantMap shown = controller.route();
        QCOMPARE(shown.value(QStringLiteral("kind")).toString(), QStringLiteral("http"));
        QCOMPARE(shown.value(QStringLiteral("host")).toString(), QStringLiteral("proxy.example"));
        QCOMPARE(shown.value(QStringLiteral("port")).toInt(), 3128);
        QVERIFY(shown.value(QStringLiteral("hasPassword")).toBool());
        for (auto it = shown.cbegin(); it != shown.cend(); ++it)
            QVERIFY2(!it.value().toString().contains(kPassword), qPrintable(it.key()));
        // And no property of the object carries it either.
        const QMetaObject *meta = controller.metaObject();
        for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
            const QVariant v = meta->property(i).read(&controller);
            QVERIFY(!v.toString().contains(kPassword));
            QVERIFY(!QJsonDocument::fromVariant(v).toJson().contains(kPassword.toUtf8()));
        }

        // Keeping the password while changing the host does not ask again.
        QCOMPARE(controller.save(QStringLiteral("http"), QStringLiteral("other.example"), 3128,
                                 QStringLiteral("alice"), QString(), false),
                 QString());
        QCOMPARE(store.get(kUser, QLatin1String(route::kSecretName)), kPassword);
        // A different username cannot keep the old one's password.
        QCOMPARE(controller.save(QStringLiteral("http"), QStringLiteral("other.example"), 3128,
                                 QStringLiteral("bob"), QString(), false),
                 QStringLiteral("password_required"));
        // "none" removes both halves.
        QCOMPARE(controller.clear(), QString());
        QVERIFY(settings.networkRouteRecord(kUser).isEmpty());
        QVERIFY(store.get(kUser, QLatin1String(route::kSecretName)).isEmpty());
    }

    // A keyring that cannot take the password stores nothing at all: no
    // record that names a password that is not there.
    void aKeyringThatRefusesTheWriteStoresNoRoute()
    {
        FakeStore store;
        SettingsManager settings;
        settings.setSecretStore(&store);
        settings.saveSession(kHs, kUser, QStringLiteral("DEVICE"), QStringLiteral("syt"));
        store.locked = true;
        QCOMPARE(route::save(&settings, &store, kUser, socks(QStringLiteral("alice")),
                             route::PasswordChange::Replace, kPassword),
                 QStringLiteral("keyring_unavailable"));
        QVERIFY(settings.networkRouteRecord(kUser).isEmpty());
        store.locked = false;
        store.available = false;
        QCOMPARE(route::save(&settings, &store, kUser, socks(QStringLiteral("alice")),
                             route::PasswordChange::Replace, kPassword),
                 QStringLiteral("keyring_unavailable"));
        QVERIFY(settings.networkRouteRecord(kUser).isEmpty());
    }

    void noSavedAccountHasNoRoute()
    {
        FakeStore store;
        SettingsManager settings;
        settings.setSecretStore(&store);
        QCOMPARE(route::save(&settings, &store, kUser, socks(), route::PasswordChange::Keep, {}),
                 QStringLiteral("no_account"));
        QCOMPARE(state(route::rustJson(&settings, &store, kUser)), QStringLiteral("none"));
        // A first sign-in has no owner yet: "none" too (documented).
        QCOMPARE(state(route::rustJson(&settings, &store, QString())), QStringLiteral("none"));
    }

    void aUsableRouteReachesRustWithItsCredentials()
    {
        FakeStore store;
        SettingsManager settings;
        settings.setSecretStore(&store);
        settings.saveSession(kHs, kUser, QStringLiteral("DEVICE"), QStringLiteral("syt"));
        QCOMPARE(route::save(&settings, &store, kUser, socks(), route::PasswordChange::Keep, {}),
                 QString());
        QJsonObject json = parsed(route::rustJson(&settings, &store, kUser));
        QCOMPARE(json.value(QStringLiteral("state")).toString(), QStringLiteral("configured"));
        QCOMPARE(json.value(QStringLiteral("kind")).toString(), QStringLiteral("socks5"));
        QCOMPARE(json.value(QStringLiteral("port")).toInt(), 1080);
        QVERIFY(!json.contains(QStringLiteral("password")));

        QCOMPARE(route::save(&settings, &store, kUser, socks(QStringLiteral("alice")),
                             route::PasswordChange::Replace, kPassword),
                 QString());
        const QByteArray bytes = route::rustJson(&settings, &store, kUser);
        json = parsed(bytes);
        QCOMPARE(json.value(QStringLiteral("username")).toString(), QStringLiteral("alice"));
        QCOMPARE(json.value(QStringLiteral("password")).toString(), kPassword);
        // What the client logs about it: the kind, nothing else.
        QCOMPARE(route::describeJson(bytes), QStringLiteral("socks5"));
    }

    // The fail-closed half: a route that exists but cannot be used is
    // "unavailable", which refuses the client in Rust. Each of these old
    // behaviours would have been "none" (direct) or a configured route with
    // no credentials (rejected by the proxy, but only after leaking that the
    // account tried).
    void everyUnusableRouteIsUnavailableNeverNone()
    {
        FakeStore store;
        SettingsManager settings;
        settings.setSecretStore(&store);
        settings.saveSession(kHs, kUser, QStringLiteral("DEVICE"), QStringLiteral("syt"));
        QCOMPARE(route::save(&settings, &store, kUser, socks(QStringLiteral("alice")),
                             route::PasswordChange::Replace, kPassword),
                 QString());

        // A locked keyring.
        store.locked = true;
        QCOMPARE(state(route::rustJson(&settings, &store, kUser)), QStringLiteral("unavailable"));
        store.locked = false;
        // An unavailable backend.
        store.available = false;
        QCOMPARE(state(route::rustJson(&settings, &store, kUser)), QStringLiteral("unavailable"));
        QCOMPARE(state(route::rustJson(&settings, nullptr, kUser)), QStringLiteral("unavailable"));
        store.available = true;
        // The password gone (another tool cleared the keyring).
        QVERIFY(store.deleteSecret(kUser, QLatin1String(route::kSecretName)));
        QCOMPARE(state(route::rustJson(&settings, &store, kUser)), QStringLiteral("unavailable"));
        // A record a newer build wrote with a kind this one does not know,
        // or one edited by hand into something invalid.
        settings.setNetworkRouteRecord(kUser, {{QStringLiteral("kind"), QStringLiteral("wireguard")},
                                               {QStringLiteral("host"), QStringLiteral("h")},
                                               {QStringLiteral("port"), 1}});
        QCOMPARE(state(route::rustJson(&settings, &store, kUser)), QStringLiteral("unavailable"));
        settings.setNetworkRouteRecord(kUser, {{QStringLiteral("kind"), QStringLiteral("http")},
                                               {QStringLiteral("host"), QStringLiteral("a b")},
                                               {QStringLiteral("port"), 1}});
        QCOMPARE(state(route::rustJson(&settings, &store, kUser)), QStringLiteral("unavailable"));
        QVERIFY(settings.networkRouteConfigured(kUser));
    }

    // The route is the account's: sign-out takes the password with the
    // tokens (clearAccountSecrets) and the record with the account's record,
    // and another account's route is untouched.
    void signingOutTakesTheRouteAndLeavesTheOtherAccounts()
    {
        FakeStore store;
        SettingsManager settings;
        settings.setSecretStore(&store);
        settings.saveSession(kHs, kUser, QStringLiteral("DEVICE"), QStringLiteral("syt_one"));
        settings.saveSession(kHs, kOther, QStringLiteral("DEVICE"), QStringLiteral("syt_two"));
        QCOMPARE(route::save(&settings, &store, kUser, socks(QStringLiteral("alice")),
                             route::PasswordChange::Replace, kPassword),
                 QString());
        QCOMPARE(route::save(&settings, &store, kOther, socks(QStringLiteral("bob")),
                             route::PasswordChange::Replace, QStringLiteral("other-pw")),
                 QString());

        QVERIFY(settings.clearSessionForAccount(kUser));
        QVERIFY(store.get(kUser, QLatin1String(route::kSecretName)).isEmpty());
        QVERIFY(!settings.networkRouteConfigured(kUser));
        QCOMPARE(store.get(kOther, QLatin1String(route::kSecretName)), QStringLiteral("other-pw"));
        QVERIFY(settings.networkRouteConfigured(kOther));
    }

    // The Settings contract, read from source: the password field is never
    // filled from the controller, nothing reads a password out of the route,
    // the copy states the fail-closed rule and its consequences, and the card
    // is on the Account page with an outline entry and a search entry.
    void theSettingsCardKeepsThePasswordWriteOnly()
    {
        QFile card(QStringLiteral(NETWORK_ROUTE_QML_PATH));
        QVERIFY(card.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString qml = QString::fromUtf8(card.readAll());
        const qsizetype field = qml.indexOf(QLatin1String("id: passwordField"));
        QVERIFY(field > 0);
        const qsizetype fieldEnd = qml.indexOf(QLatin1String("\n        }"), field);
        QVERIFY(fieldEnd > field);
        const QString block = qml.mid(field, fieldEnd - field);
        QVERIFY2(!QRegularExpression(QStringLiteral("\\btext\\s*:")).match(block).hasMatch(),
                 "the password field binds its text to something");
        QVERIFY(block.contains(QLatin1String("TextInput.Password")));
        QVERIFY(!qml.contains(QLatin1String("route.password")));
        // The only writes to the field clear it.
        const QRegularExpression write(
            QStringLiteral("passwordField\\.text\\s*=(?!=)\\s*(\\S+)"));
        auto it = write.globalMatch(qml);
        int writes = 0;
        while (it.hasNext()) {
            const auto m = it.next();
            QVERIFY2(m.captured(1).startsWith(QLatin1String("\"\"")),
                     qPrintable(m.captured(0)));
            ++writes;
        }
        QCOMPARE(writes, 2);   // load() and save() clear it
        // The fail-closed rule is said where the user sets the proxy.
        QVERIFY(qml.contains(QLatin1String("stays offline instead of")));
        QVERIFY(qml.contains(QLatin1String("connecting directly")));

        QFile screen(QStringLiteral(SETTINGS_SCREEN_QML_PATH));
        QVERIFY(screen.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString settings = QString::fromUtf8(screen.readAll());
        QVERIFY(settings.contains(QLatin1String("objectName: \"networkRouteCard\"")));
        QVERIFY(settings.contains(QLatin1String("anchors: [\"networkRouteCard\"]")));
        QVERIFY(settings.contains(QLatin1String("anchor: \"networkRouteCard\"")));
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_GUILESS_MAIN(NetworkRouteTest)
#include "NetworkRouteTest.moc"
