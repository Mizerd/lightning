// LibSecretStore's install scoping against a real Secret Service.
//
// Runs ONLY on a private session bus: set LIGHTNING_KEYRING_TEST_BUS to the
// value of DBUS_SESSION_BUS_ADDRESS to vouch that the bus is a throwaway one.
// Anywhere else every case skips, so a developer's own keyring is never
// touched. For example, with a scratch HOME and XDG_RUNTIME_DIR, run
//
//   dbus-run-session -- sh -c 'printf throwaway |
//       gnome-keyring-daemon --unlock --components=secrets --daemonize;
//       LIGHTNING_KEYRING_TEST_BUS="$DBUS_SESSION_BUS_ADDRESS"
//       LIGHTNING_KEYRING_TEST_PASSWORD=throwaway ./libsecret-scope-test'
//
// What it pins is what the fake keyring in KeyringScopeTest assumes: Secret
// Service lookups and clears match a SUBSET of attributes, so only a distinct
// schema name keeps older builds from reading, and on sign-out deleting, the
// scoped items.
//
// One case needs libsecret's FILE backend, which libsecret picks only inside
// a Flatpak or snap whose Secret portal answers: run the binary again with
// SNAP_NAME set and a Secret portal (org.freedesktop.portal.Desktop,
// interface org.freedesktop.portal.Secret, version 1, handing out a 64-byte
// secret) on the same private bus. Everywhere else that case skips, and under
// that setup every other case does.

#include "storage/LibSecretStore.h"

#include <QUuid>
#include <QtTest>

#pragma push_macro("signals")
#undef signals
#include <libsecret/secret.h>
#pragma pop_macro("signals")

namespace {

const SecretSchema *olderBuildSchema()
{
    static const SecretSchema schema = {
        "net.smetonis.matrixclient.Secret", SECRET_SCHEMA_NONE,
        {
            { "user_id", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { "key",     SECRET_SCHEMA_ATTRIBUTE_STRING },
            { nullptr,   SECRET_SCHEMA_ATTRIBUTE_STRING },
        },
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    };
    return &schema;
}

bool olderBuildStores(const QString &userId, const QString &key,
                      const QString &value)
{
    GError *err = nullptr;
    const gboolean ok = secret_password_store_sync(
        olderBuildSchema(), SECRET_COLLECTION_DEFAULT, "older build",
        value.toUtf8().constData(), nullptr, &err,
        "user_id", userId.toUtf8().constData(),
        "key", key.toUtf8().constData(), nullptr);
    if (err)
        g_error_free(err);
    return ok;
}

QString olderBuildLooksUp(const QString &userId, const QString &key)
{
    GError *err = nullptr;
    gchar *raw = secret_password_lookup_sync(
        olderBuildSchema(), nullptr, &err,
        "user_id", userId.toUtf8().constData(),
        "key", key.toUtf8().constData(), nullptr);
    if (err)
        g_error_free(err);
    if (!raw)
        return {};
    const QString out = QString::fromUtf8(raw);
    secret_password_free(raw);
    return out;
}

void olderBuildSignsOut(const QString &userId)
{
    GError *err = nullptr;
    secret_password_clear_sync(olderBuildSchema(), nullptr, &err,
                               "user_id", userId.toUtf8().constData(),
                               nullptr);
    if (err)
        g_error_free(err);
}

bool secretServiceStores(const QString &userId, const QString &key,
                         const QString &install, const QString &value)
{
    GHashTable *attributes =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(attributes, g_strdup("xdg:schema"),
                        g_strdup("org.lightning_matrix.Lightning.Secret"));
    g_hash_table_insert(attributes, g_strdup("user_id"),
                        g_strdup(userId.toUtf8().constData()));
    g_hash_table_insert(attributes, g_strdup("key"),
                        g_strdup(key.toUtf8().constData()));
    g_hash_table_insert(attributes, g_strdup("install"),
                        g_strdup(install.toUtf8().constData()));
    SecretValue *secret =
        secret_value_new(value.toUtf8().constData(), -1, "text/plain");
    GError *err = nullptr;
    const gboolean ok = secret_service_store_sync(
        nullptr, nullptr, attributes, SECRET_COLLECTION_DEFAULT, "earlier launch",
        secret, nullptr, &err);
    secret_value_unref(secret);
    g_hash_table_unref(attributes);
    if (err)
        g_error_free(err);
    return ok;
}

int countScoped(const QString &userId, const QString &install)
{
    GHashTable *attributes =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(attributes, g_strdup("xdg:schema"),
                        g_strdup("org.lightning_matrix.Lightning.Secret"));
    g_hash_table_insert(attributes, g_strdup("user_id"),
                        g_strdup(userId.toUtf8().constData()));
    g_hash_table_insert(attributes, g_strdup("install"),
                        g_strdup(install.toUtf8().constData()));
    GError *err = nullptr;
    GList *items = secret_service_search_sync(
        nullptr, nullptr, attributes, SECRET_SEARCH_ALL, nullptr, &err);
    g_hash_table_unref(attributes);
    if (err) {
        g_error_free(err);
        return -1;
    }
    const int count = static_cast<int>(g_list_length(items));
    g_list_free_full(items, g_object_unref);
    return count;
}

QByteArray defaultCollectionPath(GDBusConnection *bus)
{
    GVariant *reply = g_dbus_connection_call_sync(
        bus, "org.freedesktop.secrets", "/org/freedesktop/secrets",
        "org.freedesktop.Secret.Service", "ReadAlias",
        g_variant_new("(s)", "default"), G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
    if (!reply)
        return {};
    const gchar *path = nullptr;
    g_variant_get(reply, "(&o)", &path);
    const QByteArray out(path);
    g_variant_unref(reply);
    return out;
}

bool lockDefaultCollection()
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    const QByteArray path = defaultCollectionPath(bus);
    const char *paths[] = {path.constData(), nullptr};
    GVariant *reply = path.isEmpty() ? nullptr : g_dbus_connection_call_sync(
        bus, "org.freedesktop.secrets", "/org/freedesktop/secrets",
        "org.freedesktop.Secret.Service", "Lock",
        g_variant_new("(^ao)", paths), nullptr, G_DBUS_CALL_FLAGS_NONE, 5000,
        nullptr, nullptr);
    if (reply)
        g_variant_unref(reply);
    g_object_unref(bus);
    return reply != nullptr;
}

// gnome-keyring's own unlock, the one its --unlock flag uses: no prompt.
bool unlockDefaultCollection(const QByteArray &password)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    const QByteArray path = defaultCollectionPath(bus);
    GVariant *opened = g_dbus_connection_call_sync(
        bus, "org.freedesktop.secrets", "/org/freedesktop/secrets",
        "org.freedesktop.Secret.Service", "OpenSession",
        g_variant_new("(sv)", "plain", g_variant_new_string("")),
        G_VARIANT_TYPE("(vo)"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
    bool ok = false;
    if (opened && !path.isEmpty()) {
        GVariant *output = nullptr;
        const gchar *session = nullptr;
        g_variant_get(opened, "(v&o)", &output, &session);
        GVariant *secret = g_variant_new(
            "(o@ay@ays)", session,
            g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, "", 0, 1),
            g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, password.constData(),
                                      password.size(), 1),
            "text/plain");
        GVariant *reply = g_dbus_connection_call_sync(
            bus, "org.freedesktop.secrets", "/org/freedesktop/secrets",
            "org.gnome.keyring.InternalUnsupportedGuiltRiddenInterface",
            "UnlockWithMasterPassword",
            g_variant_new("(o@(oayays))", path.constData(), secret), nullptr,
            G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
        ok = reply != nullptr;
        if (reply)
            g_variant_unref(reply);
        g_variant_unref(output);
    }
    if (opened)
        g_variant_unref(opened);
    g_object_unref(bus);
    return ok;
}

}

class LibSecretScopeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void cleanup();

    void eachInstallReadsOnlyItsOwnItems();
    void clearingOneInstallLeavesTheOthers();
    void olderBuildsNeitherSeeNorClearScopedItems();
    void legacyItemsAreReadButNeverTouched();
    void anUnscopedStoreRefusesRatherThanMisses();
    void aStoreReplacesItsOwnItem();
    void aLegacyDeleteRemovesOnlyTheLegacyItem();

    void aSignInRecordedInTheFileKeyringIsUnreadableFromTheSecretService();
    void aSignInRecordedInTheSecretServiceIsReadPastTheFileKeyring();

    void aLockedKeyringIsAnUnansweredReadNotAMiss();

private:
    QString m_user;
};

void LibSecretScopeTest::init()
{
    const QByteArray vouched = qgetenv("LIGHTNING_KEYRING_TEST_BUS");
    if (vouched.isEmpty() || vouched != qgetenv("DBUS_SESSION_BUS_ADDRESS"))
        QSKIP("needs a private Secret Service; see the file header");
    LibSecretStore probe;
    if (!probe.isAvailable())
        QSKIP("no usable Secret Service on the private bus");
    if (probe.backendKind() == QLatin1String("file")
        && qstrcmp(QTest::currentTestFunction(),
                   "aSignInRecordedInTheSecretServiceIsReadPastTheFileKeyring") != 0)
        QSKIP("libsecret's file backend; this case needs the Secret Service");
    m_user = QStringLiteral("@scope-%1:keyring.test")
                 .arg(QUuid::createUuid().toString(QUuid::Id128));
}

void LibSecretScopeTest::cleanup()
{
    if (m_user.isEmpty())
        return;
    olderBuildSignsOut(m_user);
    for (const QString &install : {QStringLiteral("install-a"),
                                   QStringLiteral("install-b")}) {
        LibSecretStore store;
        store.setInstallScope(install);
        store.clearAccountSecrets(m_user);
    }
    m_user.clear();
}

void LibSecretScopeTest::eachInstallReadsOnlyItsOwnItems()
{
    LibSecretStore flatpak;
    flatpak.setInstallScope(QStringLiteral("install-a"));
    LibSecretStore deb;
    deb.setInstallScope(QStringLiteral("install-b"));

    QVERIFY(flatpak.storeSecret(m_user, QStringLiteral("accessToken"),
                                QStringLiteral("flatpak-access")));
    QVERIFY(deb.storeSecret(m_user, QStringLiteral("accessToken"),
                            QStringLiteral("deb-access")));

    QCOMPARE(flatpak.readSecret(m_user, QStringLiteral("accessToken")),
             QStringLiteral("flatpak-access"));
    QVERIFY(!flatpak.lastReadFailed());
    QCOMPARE(deb.readSecret(m_user, QStringLiteral("accessToken")),
             QStringLiteral("deb-access"));
}

void LibSecretScopeTest::clearingOneInstallLeavesTheOthers()
{
    LibSecretStore flatpak;
    flatpak.setInstallScope(QStringLiteral("install-a"));
    LibSecretStore deb;
    deb.setInstallScope(QStringLiteral("install-b"));
    QVERIFY(flatpak.storeSecret(m_user, QStringLiteral("accessToken"),
                                QStringLiteral("flatpak-access")));
    QVERIFY(flatpak.storeSecret(m_user, QStringLiteral("refreshToken"),
                                QStringLiteral("flatpak-refresh")));
    QVERIFY(deb.storeSecret(m_user, QStringLiteral("accessToken"),
                            QStringLiteral("deb-access")));

    QVERIFY(flatpak.clearAccountSecrets(m_user));

    QVERIFY(flatpak.readSecret(m_user, QStringLiteral("accessToken")).isEmpty());
    QVERIFY(!flatpak.lastReadFailed());
    QCOMPARE(countScoped(m_user, QStringLiteral("install-a")), 0);
    QCOMPARE(deb.readSecret(m_user, QStringLiteral("accessToken")),
             QStringLiteral("deb-access"));
}

void LibSecretScopeTest::olderBuildsNeitherSeeNorClearScopedItems()
{
    LibSecretStore flatpak;
    flatpak.setInstallScope(QStringLiteral("install-a"));
    QVERIFY(flatpak.storeSecret(m_user, QStringLiteral("accessToken"),
                                QStringLiteral("flatpak-access")));

    QCOMPARE(olderBuildLooksUp(m_user, QStringLiteral("accessToken")), QString());

    olderBuildSignsOut(m_user);
    QCOMPARE(flatpak.readSecret(m_user, QStringLiteral("accessToken")),
             QStringLiteral("flatpak-access"));
}

void LibSecretScopeTest::legacyItemsAreReadButNeverTouched()
{
    QVERIFY(olderBuildStores(m_user, QStringLiteral("accessToken"),
                             QStringLiteral("legacy-access")));
    LibSecretStore flatpak;
    flatpak.setInstallScope(QStringLiteral("install-a"));

    QCOMPARE(flatpak.readLegacySecret(m_user, QStringLiteral("accessToken")),
             QStringLiteral("legacy-access"));
    QVERIFY(!flatpak.lastReadFailed());
    QVERIFY(flatpak.readSecret(m_user, QStringLiteral("accessToken")).isEmpty());

    QVERIFY(flatpak.storeSecret(m_user, QStringLiteral("accessToken"),
                                QStringLiteral("flatpak-access")));
    QVERIFY(flatpak.deleteSecret(m_user, QStringLiteral("accessToken")));
    QVERIFY(flatpak.storeSecret(m_user, QStringLiteral("accessToken"),
                                QStringLiteral("flatpak-access")));
    QVERIFY(flatpak.clearAccountSecrets(m_user));

    QCOMPARE(olderBuildLooksUp(m_user, QStringLiteral("accessToken")),
             QStringLiteral("legacy-access"));
}

void LibSecretScopeTest::anUnscopedStoreRefusesRatherThanMisses()
{
    LibSecretStore flatpak;
    flatpak.setInstallScope(QStringLiteral("install-a"));
    QVERIFY(flatpak.storeSecret(m_user, QStringLiteral("accessToken"),
                                QStringLiteral("flatpak-access")));

    LibSecretStore unscoped;
    QVERIFY(unscoped.readSecret(m_user, QStringLiteral("accessToken")).isEmpty());
    QVERIFY2(unscoped.lastReadFailed(),
             "an unscoped read reported a miss, which reads as signed out");
    QVERIFY(!unscoped.storeSecret(m_user, QStringLiteral("accessToken"),
                                  QStringLiteral("stray")));
    QVERIFY(!unscoped.deleteSecret(m_user, QStringLiteral("accessToken")));
    QVERIFY(!unscoped.clearAccountSecrets(m_user));

    QCOMPARE(flatpak.readSecret(m_user, QStringLiteral("accessToken")),
             QStringLiteral("flatpak-access"));
}

void LibSecretScopeTest::aStoreReplacesItsOwnItem()
{
    LibSecretStore flatpak;
    flatpak.setInstallScope(QStringLiteral("install-a"));
    QVERIFY(flatpak.storeSecret(m_user, QStringLiteral("accessToken"),
                                QStringLiteral("first")));
    QVERIFY(flatpak.storeSecret(m_user, QStringLiteral("accessToken"),
                                QStringLiteral("second")));
    QCOMPARE(countScoped(m_user, QStringLiteral("install-a")), 1);
    QCOMPARE(flatpak.readSecret(m_user, QStringLiteral("accessToken")),
             QStringLiteral("second"));
}

void LibSecretScopeTest::aLegacyDeleteRemovesOnlyTheLegacyItem()
{
    LibSecretStore flatpak;
    flatpak.setInstallScope(QStringLiteral("install-a"));
    QVERIFY(flatpak.storeSecret(m_user, QStringLiteral("accessToken"),
                                QStringLiteral("flatpak-access")));
    QVERIFY(olderBuildStores(m_user, QStringLiteral("accessToken"),
                             QStringLiteral("legacy-access")));
    QVERIFY(olderBuildStores(m_user, QStringLiteral("refreshToken"),
                             QStringLiteral("legacy-refresh")));

    QVERIFY(flatpak.deleteLegacySecret(m_user, QStringLiteral("accessToken")));

    QCOMPARE(olderBuildLooksUp(m_user, QStringLiteral("accessToken")), QString());
    QCOMPARE(olderBuildLooksUp(m_user, QStringLiteral("refreshToken")),
             QStringLiteral("legacy-refresh"));
    QCOMPARE(flatpak.readSecret(m_user, QStringLiteral("accessToken")),
             QStringLiteral("flatpak-access"));
}

void LibSecretScopeTest::aSignInRecordedInTheFileKeyringIsUnreadableFromTheSecretService()
{
    LibSecretStore flatpak;
    if (flatpak.backendKind() != QLatin1String("service"))
        QSKIP("this process's libsecret backend is not the Secret Service");
    flatpak.setInstallScope(QStringLiteral("install-a"));
    QVERIFY(flatpak.storeSecret(m_user, QStringLiteral("accessToken"),
                                QStringLiteral("older-access")));

    flatpak.setRecordedBackendLookup(
        [](const QString &) { return QStringLiteral("file"); });
    QVERIFY(flatpak.readSecret(m_user, QStringLiteral("accessToken")).isEmpty());
    QVERIFY2(flatpak.lastReadFailed(),
             "a sign-in in a keyring this launch cannot open read as a miss");
    QVERIFY(!flatpak.clearAccountSecrets(m_user));
    QCOMPARE(countScoped(m_user, QStringLiteral("install-a")), 0);
}

void LibSecretScopeTest::aSignInRecordedInTheSecretServiceIsReadPastTheFileKeyring()
{
    LibSecretStore flatpak;
    if (flatpak.backendKind() != QLatin1String("file"))
        QSKIP("needs libsecret's file backend; see the file header");
    flatpak.setInstallScope(QStringLiteral("install-a"));
    QVERIFY(secretServiceStores(m_user, QStringLiteral("accessToken"),
                                QStringLiteral("install-a"),
                                QStringLiteral("service-access")));

    QVERIFY(flatpak.readSecret(m_user, QStringLiteral("accessToken")).isEmpty());
    QVERIFY(!flatpak.lastReadFailed());

    flatpak.setRecordedBackendLookup(
        [](const QString &) { return QStringLiteral("service"); });
    QCOMPARE(flatpak.backendKindFor(m_user), QStringLiteral("service"));
    QCOMPARE(flatpak.readSecret(m_user, QStringLiteral("accessToken")),
             QStringLiteral("service-access"));
    QVERIFY(!flatpak.lastReadFailed());
    QVERIFY(flatpak.storeSecret(m_user, QStringLiteral("refreshToken"),
                                QStringLiteral("service-refresh")));
    QCOMPARE(countScoped(m_user, QStringLiteral("install-a")), 2);

    flatpak.setRecordedBackendLookup({});
    QVERIFY(flatpak.storeSecret(m_user, QStringLiteral("mediaStoreKey"),
                                QStringLiteral("file-item")));
    QCOMPARE(countScoped(m_user, QStringLiteral("install-a")), 2);

    flatpak.setRecordedBackendLookup(
        [](const QString &) { return QStringLiteral("service"); });
    QVERIFY(flatpak.clearAccountSecrets(m_user));
    QCOMPARE(countScoped(m_user, QStringLiteral("install-a")), 0);
    flatpak.setRecordedBackendLookup({});
    QVERIFY(flatpak.readSecret(m_user, QStringLiteral("mediaStoreKey")).isEmpty());
    QVERIFY(!flatpak.lastReadFailed());
}

void LibSecretScopeTest::aLockedKeyringIsAnUnansweredReadNotAMiss()
{
    const QByteArray password = qgetenv("LIGHTNING_KEYRING_TEST_PASSWORD");
    if (password.isEmpty())
        QSKIP("needs LIGHTNING_KEYRING_TEST_PASSWORD, the private keyring's "
              "password, to unlock it again afterwards");
    LibSecretStore store;
    store.setInstallScope(QStringLiteral("install-a"));
    QVERIFY(store.storeSecret(m_user, QStringLiteral("accessToken"),
                              QStringLiteral("saved-access")));
    QVERIFY(lockDefaultCollection());

    // No prompter on the private bus: the one unlock prompt is dismissed.
    const QString locked = store.readSecret(m_user, QStringLiteral("accessToken"));
    const bool failed = store.lastReadFailed();
    const QString again = store.readSecret(m_user, QStringLiteral("accessToken"));
    const bool failedAgain = store.lastReadFailed();
    QVERIFY(unlockDefaultCollection(password));

    QVERIFY(locked.isEmpty());
    QVERIFY2(failed, "a locked keyring read as a missing sign-in");
    QVERIFY(again.isEmpty());
    QVERIFY(failedAgain);
    QCOMPARE(store.readSecret(m_user, QStringLiteral("accessToken")),
             QStringLiteral("saved-access"));
    QVERIFY(!store.lastReadFailed());
}

QTEST_GUILESS_MAIN(LibSecretScopeTest)
#include "LibSecretScopeTest.moc"
