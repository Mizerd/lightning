// Two installs of Lightning on one computer (the Flatpak and a .deb, say) share
// one Secret Service but keep separate account records and SDK stores. Each
// must keep its own tokens: one install's sign-in must not overwrite the
// other's, and one install's sign-out must not delete the other's. Sessions
// saved before install scoping are copied, never taken from someone else or
// deleted.
//
// The fake keyring below behaves as gnome-keyring was measured to behave
// (2026-09-29, gnome-keyring 50.0 / libsecret 0.21.7): lookups and clears match
// a SUBSET of attributes, a store replaces an item with the exact same
// attributes. The fake store maps an unset install scope to the pre-scoping
// layout, which is what LibSecretStore did before, so this suite run against
// the unscoped SettingsManager shows the collision it guards.

#include "app/SettingsManager.h"
#include "storage/AppDataPaths.h"
#include "storage/SecretStore.h"

#include <QCoreApplication>
#include <QHash>
#include <QList>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace {

const QString kScopedSchema = QStringLiteral("org.lightning_matrix.Lightning.Secret");
const QString kLegacySchema = QStringLiteral("net.smetonis.matrixclient.Secret");
const QString kSchemaAttr = QStringLiteral("xdg:schema");

const QString kUser = QStringLiteral("@rokas:keyring.example");
const QString kHs = QStringLiteral("https://keyring.example");

using Attributes = QHash<QString, QString>;

struct Item {
    Attributes attributes;
    QString secret;
};

// One desktop session's Secret Service.
class FakeKeyring
{
public:
    QList<Item> items;
    bool locked = false;

    static bool matches(const Item &item, const Attributes &query)
    {
        for (auto it = query.cbegin(); it != query.cend(); ++it) {
            if (item.attributes.value(it.key()) != it.value()
                || !item.attributes.contains(it.key())) {
                return false;
            }
        }
        return true;
    }
    bool lookup(const Attributes &query, QString *out) const
    {
        for (const Item &item : items) {
            if (matches(item, query)) {
                *out = item.secret;
                return true;
            }
        }
        return false;
    }
    void store(const Attributes &attributes, const QString &secret)
    {
        for (Item &item : items) {
            if (item.attributes == attributes) {
                item.secret = secret;
                return;
            }
        }
        items.append({attributes, secret});
    }
    int clear(const Attributes &query)
    {
        int removed = 0;
        for (qsizetype i = items.size() - 1; i >= 0; --i) {
            if (matches(items.at(i), query)) {
                items.removeAt(i);
                ++removed;
            }
        }
        return removed;
    }
    QList<Item> withSchema(const QString &schema) const
    {
        QList<Item> out;
        for (const Item &item : items) {
            if (item.attributes.value(kSchemaAttr) == schema)
                out.append(item);
        }
        return out;
    }
    QString value(const Attributes &exact) const
    {
        for (const Item &item : items) {
            if (item.attributes == exact)
                return item.secret;
        }
        return {};
    }
    bool has(const Attributes &exact) const
    {
        for (const Item &item : items) {
            if (item.attributes == exact)
                return true;
        }
        return false;
    }
};

Attributes legacyAttrs(const QString &userId, const QString &key)
{
    return {{kSchemaAttr, kLegacySchema},
            {QStringLiteral("user_id"), userId},
            {QStringLiteral("key"), key}};
}

Attributes scopedAttrs(const QString &userId, const QString &key,
                       const QString &install)
{
    return {{kSchemaAttr, kScopedSchema},
            {QStringLiteral("user_id"), userId},
            {QStringLiteral("key"), key},
            {QStringLiteral("install"), install}};
}

// What a build from before install scoping does to the keyring.
void olderBuildSignsIn(FakeKeyring &keyring, const QString &userId,
                       const QString &access, const QString &refresh,
                       const QString &clientId)
{
    keyring.store(legacyAttrs(userId, QStringLiteral("accessToken")), access);
    keyring.store(legacyAttrs(userId, QStringLiteral("refreshToken")), refresh);
    keyring.store(legacyAttrs(userId, QStringLiteral("oauthClientId")), clientId);
}

void olderBuildSignsOut(FakeKeyring &keyring, const QString &userId)
{
    keyring.clear({{kSchemaAttr, kLegacySchema},
                   {QStringLiteral("user_id"), userId}});
}

// LibSecretStore over the fake keyring. With no install scope it behaves as
// the pre-scoping backend did.
// No Q_OBJECT: it adds no meta-object of its own (as InMemorySecretStore).
class SharedKeyringStore final : public SecretStore
{
public:
    explicit SharedKeyringStore(FakeKeyring *keyring, QObject *parent = nullptr)
        : SecretStore(parent), m_keyring(keyring) {}

    bool isSecure() const override { return true; }
    bool isAvailable() const override { return true; }
    QString backendName() const override { return QStringLiteral("fake keyring"); }
    bool isSharedBetweenInstalls() const override { return true; }

    bool storeSecret(const QString &userId, const QString &key,
                     const QString &value) override
    {
        if (m_keyring->locked || failStores || key == failKey) {
            m_error = QStringLiteral("simulated store failure");
            return false;
        }
        m_keyring->store(attrs(userId, key), value);
        return true;
    }
    QString readSecret(const QString &userId, const QString &key) const override
    {
        return read(attrs(userId, key));
    }
    QString readLegacySecret(const QString &userId,
                             const QString &key) const override
    {
        return read(legacyAttrs(userId, key));
    }
    bool deleteSecret(const QString &userId, const QString &key) override
    {
        if (m_keyring->locked)
            return false;
        m_keyring->clear(attrs(userId, key));
        return true;
    }
    bool deleteLegacySecret(const QString &userId, const QString &key) override
    {
        if (m_keyring->locked)
            return false;
        m_keyring->clear(legacyAttrs(userId, key));
        return true;
    }
    bool clearAccountSecrets(const QString &userId) override
    {
        if (m_keyring->locked)
            return false;
        Attributes query = attrs(userId, QString());
        query.remove(QStringLiteral("key"));
        m_keyring->clear(query);
        return true;
    }
    QString lastError() const override { return m_error; }
    bool lastReadFailed() const override { return m_lastReadFailed; }

    bool failStores = false;
    // Refuse writes of this one key only.
    QString failKey;

private:
    Attributes attrs(const QString &userId, const QString &key) const
    {
        if (installScope().isEmpty())
            return legacyAttrs(userId, key);
        return scopedAttrs(userId, key, installScope());
    }
    QString read(const Attributes &query) const
    {
        if (m_keyring->locked) {
            m_lastReadFailed = true;
            return {};
        }
        m_lastReadFailed = false;
        QString value;
        m_keyring->lookup(query, &value);
        return value;
    }

    FakeKeyring *m_keyring;
    QString m_error;
    mutable bool m_lastReadFailed = false;
};

// A store that lives inside one install (Credential Manager, portable file).
class PerInstallStore final : public SecretStore
{
public:
    using SecretStore::SecretStore;
    bool isSecure() const override { return true; }
    bool isAvailable() const override { return true; }
    QString backendName() const override { return QStringLiteral("per-install"); }
    bool storeSecret(const QString &userId, const QString &key,
                     const QString &value) override
    {
        m_values.insert(userId + QLatin1Char('\x1f') + key, value);
        return true;
    }
    QString readSecret(const QString &userId, const QString &key) const override
    {
        return m_values.value(userId + QLatin1Char('\x1f') + key);
    }
    bool deleteSecret(const QString &userId, const QString &key) override
    {
        m_values.remove(userId + QLatin1Char('\x1f') + key);
        return true;
    }
    bool clearAccountSecrets(const QString &) override
    {
        m_values.clear();
        return true;
    }
    QString lastError() const override { return {}; }

private:
    QHash<QString, QString> m_values;
};

} // namespace

class KeyringScopeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();

    // Regression cases: each fails on the unscoped SettingsManager.
    void theInstallIdIsCreatedOnceAndKeptAcrossRestarts();
    void twoInstallsSignedInToOneAccountKeepTheirOwnTokens();
    void signingOutOfOneInstallLeavesTheOtherSignedIn();
    void anOlderBuildsSignOutCannotReachTheseItems();
    void anUpgradeKeepsTheSessionWithoutASignIn();
    void anAdoptedSessionNoLongerFollowsTheSharedItem();
    void aNonOwnerNeverTakesOrDeletesAnotherInstallsItems();
    void aSignInByThisBuildNeverFallsBackToTheSharedItem();
    void aLockedKeyringDuringTheUpgradeIsTransient();
    void aTokenRefreshBeforeTheCopyKeepsTheWholeSession();
    void signingOutOfAnAdoptedSessionRemovesTheSharedCopyOfItsToken();

    // Guards for the new code's own failure paths.
    void aCopyThatCannotBeWrittenStillReadsTheSharedItem();
    void aCopyInterruptedPartWayKeepsReadingOneWholeSession();
    void anOlderBuildSigningInAgainIsNeverHandedTheStaleToken();
    void signingOutLeavesASharedItemHoldingAnotherToken();
    void aStoreInsideOneInstallIsNotScoped();

private:
    // Each install has its own settings file, as the Flatpak and a native
    // build do. QSettings binds its file when constructed.
    static void useInstall(const QString &name)
    {
        QCoreApplication::setApplicationName(name);
    }
    static std::unique_ptr<SettingsManager> openInstall(const QString &name,
                                                        SecretStore *store)
    {
        useInstall(name);
        auto settings = std::make_unique<SettingsManager>();
        settings->setSecretStore(store);
        return settings;
    }
    // An account record written by a build from before install scoping,
    // without any keyring write.
    static void seedOlderRecord(const QString &install, const QString &userId,
                                const QString &deviceId,
                                const QString &authType)
    {
        useInstall(install);
        const QString slug = matrix::app_data::safeUserSlug(userId);
        QSettings seed;
        const QString base = QStringLiteral("accounts/") + slug + QLatin1Char('/');
        seed.setValue(base + QStringLiteral("userId"), userId);
        seed.setValue(base + QStringLiteral("homeserver"), kHs);
        seed.setValue(base + QStringLiteral("deviceId"), deviceId);
        seed.setValue(base + QStringLiteral("authType"), authType);
        seed.setValue(base + QStringLiteral("addedAt"),
                      QStringLiteral("2026-09-01T00:00:00"));
        seed.setValue(QStringLiteral("accounts/active"), userId);
        seed.sync();
    }
    static QString recordValue(const QString &install, const QString &userId,
                               const QString &key)
    {
        useInstall(install);
        QSettings raw;
        return raw.value(QStringLiteral("accounts/")
                         + matrix::app_data::safeUserSlug(userId)
                         + QLatin1Char('/') + key)
            .toString();
    }
    static QString installIdOf(const QString &install)
    {
        useInstall(install);
        QSettings raw;
        return raw.value(QStringLiteral("keyring/installId")).toString();
    }

    QTemporaryDir m_configHome;
};

void KeyringScopeTest::initTestCase()
{
    QVERIFY(m_configHome.isValid());
    qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
    QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
}

void KeyringScopeTest::init()
{
    for (const QString &name : {QStringLiteral("keyring-flatpak"),
                                QStringLiteral("keyring-deb")}) {
        useInstall(name);
        QSettings settings;
        settings.clear();
        settings.sync();
    }
}

void KeyringScopeTest::theInstallIdIsCreatedOnceAndKeptAcrossRestarts()
{
    FakeKeyring keyring;
    SharedKeyringStore flatpakStore(&keyring);
    auto flatpak = openInstall(QStringLiteral("keyring-flatpak"), &flatpakStore);
    const QString id = flatpakStore.installScope();
    QVERIFY2(!id.isEmpty(), "a shared store was left unscoped");
    QCOMPARE(installIdOf(QStringLiteral("keyring-flatpak")), id);

    // A restart of the same install reads the same id.
    flatpak.reset();
    SharedKeyringStore restartedStore(&keyring);
    auto restarted =
        openInstall(QStringLiteral("keyring-flatpak"), &restartedStore);
    QCOMPARE(restartedStore.installScope(), id);

    // Another install on the same computer gets its own.
    SharedKeyringStore debStore(&keyring);
    auto deb = openInstall(QStringLiteral("keyring-deb"), &debStore);
    QVERIFY(!debStore.installScope().isEmpty());
    QVERIFY(debStore.installScope() != id);
}

void KeyringScopeTest::twoInstallsSignedInToOneAccountKeepTheirOwnTokens()
{
    FakeKeyring keyring;
    SharedKeyringStore flatpakStore(&keyring);
    SharedKeyringStore debStore(&keyring);
    auto flatpak = openInstall(QStringLiteral("keyring-flatpak"), &flatpakStore);
    auto deb = openInstall(QStringLiteral("keyring-deb"), &debStore);

    flatpak->saveSession(kHs, kUser, QStringLiteral("FLATPAKDEV"),
                         QStringLiteral("flatpak-access"),
                         QStringLiteral("flatpak-refresh"),
                         QStringLiteral("oauth"),
                         QStringLiteral("flatpak-client"));
    // The .deb signs in second: before scoping this overwrote the Flatpak's.
    deb->saveSession(kHs, kUser, QStringLiteral("DEBDEV"),
                     QStringLiteral("deb-access"),
                     QStringLiteral("deb-refresh"), QStringLiteral("oauth"),
                     QStringLiteral("deb-client"));

    QCOMPARE(flatpak->accessTokenFor(kUser), QStringLiteral("flatpak-access"));
    QCOMPARE(flatpak->refreshTokenFor(kUser), QStringLiteral("flatpak-refresh"));
    QCOMPARE(flatpak->oauthClientIdFor(kUser), QStringLiteral("flatpak-client"));
    QCOMPARE(deb->accessTokenFor(kUser), QStringLiteral("deb-access"));

    // A token refresh in one install stays there.
    QVERIFY(deb->updateSessionTokens(kUser, QStringLiteral("deb-access-2"),
                                     QStringLiteral("deb-refresh-2")));
    QCOMPARE(flatpak->accessTokenFor(kUser), QStringLiteral("flatpak-access"));
    QCOMPARE(flatpak->refreshTokenFor(kUser), QStringLiteral("flatpak-refresh"));
    QCOMPARE(deb->accessTokenFor(kUser), QStringLiteral("deb-access-2"));
    QCOMPARE(deb->refreshTokenFor(kUser), QStringLiteral("deb-refresh-2"));

    // And the restarts read the same.
    flatpak.reset();
    SharedKeyringStore flatpakAgain(&keyring);
    auto restarted = openInstall(QStringLiteral("keyring-flatpak"), &flatpakAgain);
    QVERIFY(restarted->hasSession());
    QCOMPARE(restarted->accessToken(), QStringLiteral("flatpak-access"));
    QCOMPARE(restarted->deviceId(), QStringLiteral("FLATPAKDEV"));
}

void KeyringScopeTest::signingOutOfOneInstallLeavesTheOtherSignedIn()
{
    FakeKeyring keyring;
    SharedKeyringStore flatpakStore(&keyring);
    SharedKeyringStore debStore(&keyring);
    auto flatpak = openInstall(QStringLiteral("keyring-flatpak"), &flatpakStore);
    auto deb = openInstall(QStringLiteral("keyring-deb"), &debStore);
    flatpak->saveSession(kHs, kUser, QStringLiteral("FLATPAKDEV"),
                         QStringLiteral("flatpak-access"));
    deb->saveSession(kHs, kUser, QStringLiteral("DEBDEV"),
                     QStringLiteral("deb-access"),
                     QStringLiteral("deb-refresh"), QStringLiteral("oauth"),
                     QStringLiteral("deb-client"));

    QVERIFY(flatpak->clearSessionForAccount(kUser));

    QVERIFY(!flatpak->hasSession());
    QVERIFY(flatpak->accessTokenFor(kUser).isEmpty());
    QVERIFY(!flatpak->secretBackendUnavailable());
    QVERIFY(deb->hasSession());
    QCOMPARE(deb->accessTokenFor(kUser), QStringLiteral("deb-access"));
    QCOMPARE(deb->refreshTokenFor(kUser), QStringLiteral("deb-refresh"));
    QCOMPARE(deb->oauthClientIdFor(kUser), QStringLiteral("deb-client"));
    // Nothing of the Flatpak's is left behind.
    QCOMPARE(keyring.items.size(), 3);
}

void KeyringScopeTest::anOlderBuildsSignOutCannotReachTheseItems()
{
    FakeKeyring keyring;
    SharedKeyringStore flatpakStore(&keyring);
    auto flatpak = openInstall(QStringLiteral("keyring-flatpak"), &flatpakStore);
    flatpak->saveSession(kHs, kUser, QStringLiteral("FLATPAKDEV"),
                         QStringLiteral("flatpak-access"));

    // A .deb not yet updated signs in and out of the same account.
    olderBuildSignsIn(keyring, kUser, QStringLiteral("old-access"),
                      QString(), QString());
    olderBuildSignsOut(keyring, kUser);

    QCOMPARE(flatpak->accessTokenFor(kUser), QStringLiteral("flatpak-access"));
}

void KeyringScopeTest::anUpgradeKeepsTheSessionWithoutASignIn()
{
    FakeKeyring keyring;
    seedOlderRecord(QStringLiteral("keyring-flatpak"), kUser,
                    QStringLiteral("FLATPAKDEV"), QStringLiteral("oauth"));
    olderBuildSignsIn(keyring, kUser, QStringLiteral("legacy-access"),
                      QStringLiteral("legacy-refresh"),
                      QStringLiteral("legacy-client"));
    const QList<Item> legacyBefore = keyring.withSchema(kLegacySchema);
    QCOMPARE(legacyBefore.size(), 3);

    SharedKeyringStore store(&keyring);
    auto flatpak = openInstall(QStringLiteral("keyring-flatpak"), &store);

    // Signed in exactly as before the update.
    QVERIFY(flatpak->hasSession());
    QCOMPARE(flatpak->accessToken(), QStringLiteral("legacy-access"));
    QCOMPARE(flatpak->refreshTokenFor(kUser), QStringLiteral("legacy-refresh"));
    QCOMPARE(flatpak->oauthClientIdFor(kUser), QStringLiteral("legacy-client"));
    QCOMPARE(flatpak->deviceId(), QStringLiteral("FLATPAKDEV"));

    // Copied at startup into this install's items...
    const QString id = store.installScope();
    QCOMPARE(keyring.value(scopedAttrs(kUser, QStringLiteral("accessToken"), id)),
             QStringLiteral("legacy-access"));
    QCOMPARE(keyring.value(scopedAttrs(kUser, QStringLiteral("refreshToken"), id)),
             QStringLiteral("legacy-refresh"));
    QCOMPARE(keyring.value(scopedAttrs(kUser, QStringLiteral("oauthClientId"), id)),
             QStringLiteral("legacy-client"));
    // ...and the shared items are exactly as they were.
    const QList<Item> legacyAfter = keyring.withSchema(kLegacySchema);
    QCOMPARE(legacyAfter.size(), legacyBefore.size());
    for (const Item &item : legacyBefore)
        QCOMPARE(keyring.value(item.attributes), item.secret);

    // Recorded as a copy whose owner is unproven, for the recorded device.
    QCOMPARE(recordValue(QStringLiteral("keyring-flatpak"), kUser,
                         QStringLiteral("keyringItems")),
             QStringLiteral("adopted-unverified"));
    QCOMPARE(recordValue(QStringLiteral("keyring-flatpak"), kUser,
                         QStringLiteral("keyringDevice")),
             QStringLiteral("FLATPAKDEV"));
}

void KeyringScopeTest::anAdoptedSessionNoLongerFollowsTheSharedItem()
{
    FakeKeyring keyring;
    seedOlderRecord(QStringLiteral("keyring-flatpak"), kUser,
                    QStringLiteral("FLATPAKDEV"), QStringLiteral("password"));
    olderBuildSignsIn(keyring, kUser, QStringLiteral("legacy-access"),
                      QString(), QString());
    SharedKeyringStore store(&keyring);
    auto flatpak = openInstall(QStringLiteral("keyring-flatpak"), &store);

    // A .deb not yet updated signs in afterwards and overwrites the shared item.
    olderBuildSignsIn(keyring, kUser, QStringLiteral("deb-access"), QString(),
                      QString());

    QCOMPARE(flatpak->accessTokenFor(kUser), QStringLiteral("legacy-access"));
    flatpak.reset();
    SharedKeyringStore again(&keyring);
    auto restarted = openInstall(QStringLiteral("keyring-flatpak"), &again);
    QCOMPARE(restarted->accessToken(), QStringLiteral("legacy-access"));
}

void KeyringScopeTest::aNonOwnerNeverTakesOrDeletesAnotherInstallsItems()
{
    FakeKeyring keyring;
    // The .deb signed in with this build.
    SharedKeyringStore debStore(&keyring);
    auto deb = openInstall(QStringLiteral("keyring-deb"), &debStore);
    deb->saveSession(kHs, kUser, QStringLiteral("DEBDEV"),
                     QStringLiteral("deb-access"));
    // The Flatpak remembers the account but has no token of its own.
    seedOlderRecord(QStringLiteral("keyring-flatpak"), kUser,
                    QStringLiteral("FLATPAKDEV"), QStringLiteral("password"));
    SharedKeyringStore flatpakStore(&keyring);
    auto flatpak = openInstall(QStringLiteral("keyring-flatpak"), &flatpakStore);

    QVERIFY(flatpak->accessTokenFor(kUser).isEmpty());
    QVERIFY(!flatpak->hasSession());
    // A conclusive miss: the keyring answered.
    QVERIFY(!flatpak->secretBackendUnavailable());

    QVERIFY(flatpak->clearSessionForAccount(kUser));
    QCOMPARE(deb->accessTokenFor(kUser), QStringLiteral("deb-access"));

    // An install with no record for an account never copies its shared item,
    // and its sign-outs never delete it.
    const QString other = QStringLiteral("@someone:keyring.example");
    olderBuildSignsIn(keyring, other, QStringLiteral("other-access"),
                      QString(), QString());
    flatpak->saveSession(kHs, kUser, QStringLiteral("FLATPAKDEV2"),
                         QStringLiteral("flatpak-access"));
    QVERIFY(flatpak->accessTokenFor(other).isEmpty());
    QVERIFY(flatpak->clearSessionForAccount(other));
    QVERIFY(flatpak->clearSessionForAccount(kUser));
    QCOMPARE(keyring.value(legacyAttrs(other, QStringLiteral("accessToken"))),
             QStringLiteral("other-access"));
    for (const Item &item : keyring.items) {
        QVERIFY2(item.attributes.value(QStringLiteral("install"))
                     != flatpakStore.installScope(),
                 "the Flatpak left an item of its own behind");
    }
    QCOMPARE(deb->accessTokenFor(kUser), QStringLiteral("deb-access"));
}

void KeyringScopeTest::aSignInByThisBuildNeverFallsBackToTheSharedItem()
{
    FakeKeyring keyring;
    SharedKeyringStore store(&keyring);
    auto flatpak = openInstall(QStringLiteral("keyring-flatpak"), &store);
    flatpak->saveSession(kHs, kUser, QStringLiteral("FLATPAKDEV"),
                         QStringLiteral("flatpak-access"));
    // The item is lost (removed by hand, say) while an older build's item for
    // the same account sits in the keyring.
    QVERIFY(store.deleteSecret(kUser, QStringLiteral("accessToken")));
    olderBuildSignsIn(keyring, kUser, QStringLiteral("deb-access"), QString(),
                      QString());

    QVERIFY2(flatpak->accessTokenFor(kUser).isEmpty(),
             "a session this build signed in read another install's token");
    QVERIFY(!flatpak->secretBackendUnavailable());
}

void KeyringScopeTest::aLockedKeyringDuringTheUpgradeIsTransient()
{
    FakeKeyring keyring;
    seedOlderRecord(QStringLiteral("keyring-flatpak"), kUser,
                    QStringLiteral("FLATPAKDEV"), QStringLiteral("password"));
    olderBuildSignsIn(keyring, kUser, QStringLiteral("legacy-access"),
                      QString(), QString());
    keyring.locked = true;

    SharedKeyringStore store(&keyring);
    auto flatpak = openInstall(QStringLiteral("keyring-flatpak"), &store);
    QCOMPARE(keyring.items.size(), 3);
    // Cannot tell, which is not "signed out" (CLAUDE.md §6).
    QVERIFY(flatpak->accessTokenFor(kUser).isEmpty());
    QVERIFY(flatpak->secretBackendUnavailable());

    // Unlocked later in the same run: the first read adopts.
    keyring.locked = false;
    QCOMPARE(flatpak->accessTokenFor(kUser), QStringLiteral("legacy-access"));
    QVERIFY(!flatpak->secretBackendUnavailable());
    QCOMPARE(keyring.value(scopedAttrs(kUser, QStringLiteral("accessToken"),
                                       store.installScope())),
             QStringLiteral("legacy-access"));
    QCOMPARE(keyring.value(legacyAttrs(kUser, QStringLiteral("accessToken"))),
             QStringLiteral("legacy-access"));
}

void KeyringScopeTest::aTokenRefreshBeforeTheCopyKeepsTheWholeSession()
{
    FakeKeyring keyring;
    seedOlderRecord(QStringLiteral("keyring-flatpak"), kUser,
                    QStringLiteral("FLATPAKDEV"), QStringLiteral("oauth"));
    olderBuildSignsIn(keyring, kUser, QStringLiteral("legacy-access"),
                      QStringLiteral("legacy-refresh"),
                      QStringLiteral("legacy-client"));
    SharedKeyringStore store(&keyring);
    // The keyring refuses writes at startup, so nothing is copied yet.
    store.failStores = true;
    auto flatpak = openInstall(QStringLiteral("keyring-flatpak"), &store);
    store.failStores = false;

    // The SDK rotates the tokens before anything else reads them.
    QVERIFY(flatpak->updateSessionTokens(kUser, QStringLiteral("rotated-access"),
                                         QStringLiteral("rotated-refresh")));

    QCOMPARE(flatpak->accessTokenFor(kUser), QStringLiteral("rotated-access"));
    QCOMPARE(flatpak->refreshTokenFor(kUser), QStringLiteral("rotated-refresh"));
    // The registration the rotated tokens belong to came along.
    QCOMPARE(keyring.value(scopedAttrs(kUser, QStringLiteral("oauthClientId"),
                                       store.installScope())),
             QStringLiteral("legacy-client"));
    QCOMPARE(flatpak->oauthClientIdFor(kUser), QStringLiteral("legacy-client"));
    // The shared item keeps what it had.
    QCOMPARE(keyring.value(legacyAttrs(kUser, QStringLiteral("accessToken"))),
             QStringLiteral("legacy-access"));
}

// Before scoping, sign-out deleted the shared item. An adopted session must
// still take the copy of its own token with it: the server logout can fail and
// a local reset never logs out, so leaving it would keep a live token at rest.
void KeyringScopeTest::signingOutOfAnAdoptedSessionRemovesTheSharedCopyOfItsToken()
{
    FakeKeyring keyring;
    seedOlderRecord(QStringLiteral("keyring-flatpak"), kUser,
                    QStringLiteral("FLATPAKDEV"), QStringLiteral("oauth"));
    olderBuildSignsIn(keyring, kUser, QStringLiteral("legacy-access"),
                      QStringLiteral("legacy-refresh"),
                      QStringLiteral("legacy-client"));
    SharedKeyringStore store(&keyring);
    auto flatpak = openInstall(QStringLiteral("keyring-flatpak"), &store);
    QCOMPARE(flatpak->accessToken(), QStringLiteral("legacy-access"));

    QVERIFY(flatpak->clearSessionForAccount(kUser));

    QCOMPARE(keyring.withSchema(kLegacySchema).size(), 0);
    QCOMPARE(keyring.withSchema(kScopedSchema).size(), 0);
}

void KeyringScopeTest::aCopyThatCannotBeWrittenStillReadsTheSharedItem()
{
    FakeKeyring keyring;
    seedOlderRecord(QStringLiteral("keyring-flatpak"), kUser,
                    QStringLiteral("FLATPAKDEV"), QStringLiteral("oauth"));
    olderBuildSignsIn(keyring, kUser, QStringLiteral("legacy-access"),
                      QStringLiteral("legacy-refresh"),
                      QStringLiteral("legacy-client"));
    SharedKeyringStore store(&keyring);
    store.failStores = true;
    auto flatpak = openInstall(QStringLiteral("keyring-flatpak"), &store);

    // Still signed in, as before the update: never reported as gone.
    QVERIFY(flatpak->hasSession());
    QCOMPARE(flatpak->accessToken(), QStringLiteral("legacy-access"));
    QCOMPARE(flatpak->refreshTokenFor(kUser), QStringLiteral("legacy-refresh"));
    QCOMPARE(flatpak->oauthClientIdFor(kUser), QStringLiteral("legacy-client"));
    QCOMPARE(keyring.withSchema(kScopedSchema).size(), 0);
}

void KeyringScopeTest::aCopyInterruptedPartWayKeepsReadingOneWholeSession()
{
    FakeKeyring keyring;
    seedOlderRecord(QStringLiteral("keyring-flatpak"), kUser,
                    QStringLiteral("FLATPAKDEV"), QStringLiteral("oauth"));
    olderBuildSignsIn(keyring, kUser, QStringLiteral("legacy-access"),
                      QStringLiteral("legacy-refresh"),
                      QStringLiteral("legacy-client"));
    SharedKeyringStore store(&keyring);
    store.failKey = QStringLiteral("refreshToken");
    auto flatpak = openInstall(QStringLiteral("keyring-flatpak"), &store);

    // The access token is copied last, so a copy that stopped part way leaves
    // none, and every key still comes from the one session that is whole.
    QVERIFY(keyring.value(scopedAttrs(kUser, QStringLiteral("accessToken"),
                                      store.installScope()))
                .isEmpty());
    QCOMPARE(flatpak->accessTokenFor(kUser), QStringLiteral("legacy-access"));
    QCOMPARE(flatpak->refreshTokenFor(kUser), QStringLiteral("legacy-refresh"));
    QCOMPARE(flatpak->oauthClientIdFor(kUser), QStringLiteral("legacy-client"));
}

// A release AppImage and a development .deb share one data root, so they are
// one install. When an older build signs in again it changes the record's
// device and writes only the shared item: the items this build left for the
// previous device must not be read for the new one.
void KeyringScopeTest::anOlderBuildSigningInAgainIsNeverHandedTheStaleToken()
{
    FakeKeyring keyring;
    SharedKeyringStore store(&keyring);
    auto deb = openInstall(QStringLiteral("keyring-deb"), &store);
    deb->saveSession(kHs, kUser, QStringLiteral("NEWBUILDDEV"),
                     QStringLiteral("new-build-access"));
    deb.reset();

    // The older build signs in again on the same record.
    useInstall(QStringLiteral("keyring-deb"));
    {
        QSettings raw;
        raw.setValue(QStringLiteral("accounts/")
                         + matrix::app_data::safeUserSlug(kUser)
                         + QStringLiteral("/deviceId"),
                     QStringLiteral("OLDBUILDDEV"));
        raw.sync();
    }
    olderBuildSignsIn(keyring, kUser, QStringLiteral("old-build-access"),
                      QString(), QString());

    SharedKeyringStore again(&keyring);
    auto restarted = openInstall(QStringLiteral("keyring-deb"), &again);
    QCOMPARE(restarted->deviceId(), QStringLiteral("OLDBUILDDEV"));
    QCOMPARE(restarted->accessToken(), QStringLiteral("old-build-access"));
    QCOMPARE(recordValue(QStringLiteral("keyring-deb"), kUser,
                         QStringLiteral("keyringDevice")),
             QStringLiteral("OLDBUILDDEV"));

    // And with no shared item to take, the stale token is still not handed out.
    restarted.reset();
    olderBuildSignsOut(keyring, kUser);
    useInstall(QStringLiteral("keyring-deb"));
    {
        QSettings raw;
        raw.setValue(QStringLiteral("accounts/")
                         + matrix::app_data::safeUserSlug(kUser)
                         + QStringLiteral("/deviceId"),
                     QStringLiteral("THIRDDEV"));
        raw.sync();
    }
    SharedKeyringStore third(&keyring);
    auto once = openInstall(QStringLiteral("keyring-deb"), &third);
    QVERIFY(once->accessToken().isEmpty());
    QVERIFY(!once->secretBackendUnavailable());
}

void KeyringScopeTest::signingOutLeavesASharedItemHoldingAnotherToken()
{
    FakeKeyring keyring;
    seedOlderRecord(QStringLiteral("keyring-flatpak"), kUser,
                    QStringLiteral("FLATPAKDEV"), QStringLiteral("oauth"));
    olderBuildSignsIn(keyring, kUser, QStringLiteral("legacy-access"),
                      QStringLiteral("legacy-refresh"),
                      QStringLiteral("legacy-client"));
    SharedKeyringStore store(&keyring);
    auto flatpak = openInstall(QStringLiteral("keyring-flatpak"), &store);
    // A .deb not yet updated signs in afterwards: the shared item is its now.
    olderBuildSignsIn(keyring, kUser, QStringLiteral("deb-access"),
                      QStringLiteral("deb-refresh"), QStringLiteral("deb-client"));

    QVERIFY(flatpak->clearSessionForAccount(kUser));

    QCOMPARE(keyring.value(legacyAttrs(kUser, QStringLiteral("accessToken"))),
             QStringLiteral("deb-access"));
    QCOMPARE(keyring.value(legacyAttrs(kUser, QStringLiteral("refreshToken"))),
             QStringLiteral("deb-refresh"));
    QCOMPARE(keyring.value(legacyAttrs(kUser, QStringLiteral("oauthClientId"))),
             QStringLiteral("deb-client"));
    QCOMPARE(keyring.withSchema(kScopedSchema).size(), 0);
}

void KeyringScopeTest::aStoreInsideOneInstallIsNotScoped()
{
    PerInstallStore store;
    auto install = openInstall(QStringLiteral("keyring-deb"), &store);
    install->saveSession(kHs, kUser, QStringLiteral("DEBDEV"),
                         QStringLiteral("deb-access"));
    QCOMPARE(install->accessTokenFor(kUser), QStringLiteral("deb-access"));
    QVERIFY(store.installScope().isEmpty());
    QVERIFY(installIdOf(QStringLiteral("keyring-deb")).isEmpty());
}

QTEST_GUILESS_MAIN(KeyringScopeTest)
#include "KeyringScopeTest.moc"
