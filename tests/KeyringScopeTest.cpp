
#include "app/SettingsManager.h"
#include "storage/AppDataPaths.h"
#include "storage/SecretStore.h"
#include "storage/TwoBackendSecretStore.h"

#include <QCoreApplication>
#include <QHash>
#include <QList>
#include <QSettings>
#include <QTemporaryDir>
#include <QRegularExpression>
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
    QString backendKind() const override { return kind; }

    bool failStores = false;
    QString kind;
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

// LibSecretStore inside a Flatpak or snap: libsecret's sandbox FILE keyring,
// whose key only the Secret portal hands out, and the desktop's Secret
// Service; `kind` is the one this "process" landed on. Only the two backends
// are faked: which one each account is read and written in is the shipped
// routing (TwoBackendSecretStore). A locked FakeKeyring is a backend that
// cannot answer (a Secret Service not started yet, a locked wallet).
// No Q_OBJECT: it adds no meta-object of its own.
class SandboxKeyringStore final : public TwoBackendSecretStore
{
public:
    SandboxKeyringStore(FakeKeyring *file, FakeKeyring *service,
                        const QString &kind)
        : m_file(file), m_service(service), m_kind(kind) {}

    bool isSecure() const override { return true; }
    bool isAvailable() const override { return true; }
    QString backendName() const override { return QStringLiteral("fake sandbox keyring"); }
    bool isSharedBetweenInstalls() const override { return true; }
    QString backendKind() const override { return m_kind; }

protected:
    QString readFrom(Backend backend, const QString &userId,
                     const QString &key) const override
    {
        const FakeKeyring *keyring = keyringFor(backend);
        if (keyring->locked) {
            setError(QStringLiteral("simulated: backend cannot answer"));
            m_lastReadFailed = true;
            return {};
        }
        m_lastReadFailed = false;
        QString value;
        keyring->lookup(scopedAttrs(userId, key, installScope()), &value);
        return value;
    }
    bool storeIn(Backend backend, const QString &userId, const QString &key,
                 const QString &value) override
    {
        FakeKeyring *keyring = keyringFor(backend);
        if (keyring->locked)
            return false;
        keyring->store(scopedAttrs(userId, key, installScope()), value);
        return true;
    }
    bool clearFrom(Backend backend, const QString &userId,
                   const QString *key) override
    {
        FakeKeyring *keyring = keyringFor(backend);
        if (keyring->locked)
            return false;
        Attributes query = scopedAttrs(userId, key ? *key : QString(), installScope());
        if (!key)
            query.remove(QStringLiteral("key"));
        keyring->clear(query);
        return true;
    }

private:
    FakeKeyring *keyringFor(Backend backend) const
    {
        if (backend == Backend::Service)
            return m_service;
        return m_kind == QLatin1String("file") ? m_file : m_service;
    }

    FakeKeyring *m_file;
    FakeKeyring *m_service;
    QString m_kind;
};

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

}

class KeyringScopeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();

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

    void aCopyThatCannotBeWrittenStillReadsTheSharedItem();
    void aCopyInterruptedPartWayKeepsReadingOneWholeSession();
    void anOlderBuildSigningInAgainIsNeverHandedTheStaleToken();
    void signingOutLeavesASharedItemHoldingAnotherToken();
    void aStoreInsideOneInstallIsNotScoped();

    void aRefusedRefreshTokenWriteLeavesTheWholeOldPair();

    void aSignInRecordsTheKeyringThatHoldsItAndALaunchReadingAnotherSaysSo();

    void aSignInTheSecretServiceHoldsIsReadThereWhenALaterLaunchGetsTheFileKeyring();
    void aSignInTheFileKeyringHoldsIsUnreadableNotLostWhenThePortalDidNotAnswer();
    void aSecretServiceNotUpYetIsTransientAndTheNextReadFindsTheSignIn();
    void signingOutOfAnAccountReadPastTheFileKeyringClearsBothKeyrings();
    void aSignInSavedBeforeTheRecordIsRecordedWhereItIsFound();

private:
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

    flatpak.reset();
    SharedKeyringStore restartedStore(&keyring);
    auto restarted =
        openInstall(QStringLiteral("keyring-flatpak"), &restartedStore);
    QCOMPARE(restartedStore.installScope(), id);

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
    deb->saveSession(kHs, kUser, QStringLiteral("DEBDEV"),
                     QStringLiteral("deb-access"),
                     QStringLiteral("deb-refresh"), QStringLiteral("oauth"),
                     QStringLiteral("deb-client"));

    QCOMPARE(flatpak->accessTokenFor(kUser), QStringLiteral("flatpak-access"));
    QCOMPARE(flatpak->refreshTokenFor(kUser), QStringLiteral("flatpak-refresh"));
    QCOMPARE(flatpak->oauthClientIdFor(kUser), QStringLiteral("flatpak-client"));
    QCOMPARE(deb->accessTokenFor(kUser), QStringLiteral("deb-access"));

    QVERIFY(deb->updateSessionTokens(kUser, QStringLiteral("deb-access-2"),
                                     QStringLiteral("deb-refresh-2")));
    QCOMPARE(flatpak->accessTokenFor(kUser), QStringLiteral("flatpak-access"));
    QCOMPARE(flatpak->refreshTokenFor(kUser), QStringLiteral("flatpak-refresh"));
    QCOMPARE(deb->accessTokenFor(kUser), QStringLiteral("deb-access-2"));
    QCOMPARE(deb->refreshTokenFor(kUser), QStringLiteral("deb-refresh-2"));

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
    QCOMPARE(keyring.items.size(), 3);
}

void KeyringScopeTest::anOlderBuildsSignOutCannotReachTheseItems()
{
    FakeKeyring keyring;
    SharedKeyringStore flatpakStore(&keyring);
    auto flatpak = openInstall(QStringLiteral("keyring-flatpak"), &flatpakStore);
    flatpak->saveSession(kHs, kUser, QStringLiteral("FLATPAKDEV"),
                         QStringLiteral("flatpak-access"));

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

    QVERIFY(flatpak->hasSession());
    QCOMPARE(flatpak->accessToken(), QStringLiteral("legacy-access"));
    QCOMPARE(flatpak->refreshTokenFor(kUser), QStringLiteral("legacy-refresh"));
    QCOMPARE(flatpak->oauthClientIdFor(kUser), QStringLiteral("legacy-client"));
    QCOMPARE(flatpak->deviceId(), QStringLiteral("FLATPAKDEV"));

    const QString id = store.installScope();
    QCOMPARE(keyring.value(scopedAttrs(kUser, QStringLiteral("accessToken"), id)),
             QStringLiteral("legacy-access"));
    QCOMPARE(keyring.value(scopedAttrs(kUser, QStringLiteral("refreshToken"), id)),
             QStringLiteral("legacy-refresh"));
    QCOMPARE(keyring.value(scopedAttrs(kUser, QStringLiteral("oauthClientId"), id)),
             QStringLiteral("legacy-client"));
    const QList<Item> legacyAfter = keyring.withSchema(kLegacySchema);
    QCOMPARE(legacyAfter.size(), legacyBefore.size());
    for (const Item &item : legacyBefore)
        QCOMPARE(keyring.value(item.attributes), item.secret);

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
    SharedKeyringStore debStore(&keyring);
    auto deb = openInstall(QStringLiteral("keyring-deb"), &debStore);
    deb->saveSession(kHs, kUser, QStringLiteral("DEBDEV"),
                     QStringLiteral("deb-access"));
    seedOlderRecord(QStringLiteral("keyring-flatpak"), kUser,
                    QStringLiteral("FLATPAKDEV"), QStringLiteral("password"));
    SharedKeyringStore flatpakStore(&keyring);
    auto flatpak = openInstall(QStringLiteral("keyring-flatpak"), &flatpakStore);

    QVERIFY(flatpak->accessTokenFor(kUser).isEmpty());
    QVERIFY(!flatpak->hasSession());
    QVERIFY(!flatpak->secretBackendUnavailable());

    QVERIFY(flatpak->clearSessionForAccount(kUser));
    QCOMPARE(deb->accessTokenFor(kUser), QStringLiteral("deb-access"));

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
    QVERIFY(flatpak->accessTokenFor(kUser).isEmpty());
    QVERIFY(flatpak->secretBackendUnavailable());

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
    store.failStores = true;
    auto flatpak = openInstall(QStringLiteral("keyring-flatpak"), &store);
    store.failStores = false;

    QVERIFY(flatpak->updateSessionTokens(kUser, QStringLiteral("rotated-access"),
                                         QStringLiteral("rotated-refresh")));

    QCOMPARE(flatpak->accessTokenFor(kUser), QStringLiteral("rotated-access"));
    QCOMPARE(flatpak->refreshTokenFor(kUser), QStringLiteral("rotated-refresh"));
    QCOMPARE(keyring.value(scopedAttrs(kUser, QStringLiteral("oauthClientId"),
                                       store.installScope())),
             QStringLiteral("legacy-client"));
    QCOMPARE(flatpak->oauthClientIdFor(kUser), QStringLiteral("legacy-client"));
    QCOMPARE(keyring.value(legacyAttrs(kUser, QStringLiteral("accessToken"))),
             QStringLiteral("legacy-access"));
}

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

    QVERIFY(keyring.value(scopedAttrs(kUser, QStringLiteral("accessToken"),
                                      store.installScope()))
                .isEmpty());
    QCOMPARE(flatpak->accessTokenFor(kUser), QStringLiteral("legacy-access"));
    QCOMPARE(flatpak->refreshTokenFor(kUser), QStringLiteral("legacy-refresh"));
    QCOMPARE(flatpak->oauthClientIdFor(kUser), QStringLiteral("legacy-client"));
}

void KeyringScopeTest::anOlderBuildSigningInAgainIsNeverHandedTheStaleToken()
{
    FakeKeyring keyring;
    SharedKeyringStore store(&keyring);
    auto deb = openInstall(QStringLiteral("keyring-deb"), &store);
    deb->saveSession(kHs, kUser, QStringLiteral("NEWBUILDDEV"),
                     QStringLiteral("new-build-access"));
    deb.reset();

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

void KeyringScopeTest::aRefusedRefreshTokenWriteLeavesTheWholeOldPair()
{
    FakeKeyring keyring;
    SharedKeyringStore store(&keyring);
    auto install = openInstall(QStringLiteral("keyring-flatpak"), &store);
    install->saveSession(kHs, kUser, QStringLiteral("FLATPAKDEV"),
                         QStringLiteral("access-0"), QStringLiteral("refresh-0"),
                         QStringLiteral("oauth"), QStringLiteral("client"));

    store.failKey = QStringLiteral("refreshToken");
    QVERIFY(!install->updateSessionTokens(kUser, QStringLiteral("access-1"),
                                          QStringLiteral("refresh-1")));
    QCOMPARE(install->accessTokenFor(kUser), QStringLiteral("access-0"));
    QCOMPARE(install->refreshTokenFor(kUser), QStringLiteral("refresh-0"));

    store.failKey.clear();
    QVERIFY(install->updateSessionTokens(kUser, QStringLiteral("access-1"),
                                         QStringLiteral("refresh-1")));
    QCOMPARE(install->accessTokenFor(kUser), QStringLiteral("access-1"));
    QCOMPARE(install->refreshTokenFor(kUser), QStringLiteral("refresh-1"));
}

void KeyringScopeTest::aSignInRecordsTheKeyringThatHoldsItAndALaunchReadingAnotherSaysSo()
{
    FakeKeyring keyring;
    SharedKeyringStore store(&keyring);
    store.kind = QStringLiteral("file");
    auto flatpak = openInstall(QStringLiteral("keyring-flatpak"), &store);
    flatpak->saveSession(kHs, kUser, QStringLiteral("FLATPAKDEV"),
                         QStringLiteral("flatpak-access"));
    QCOMPARE(flatpak->keyringBackendFor(kUser), QStringLiteral("file"));
    QVERIFY(!flatpak->keyringBackendChanged(kUser));

    store.kind = QStringLiteral("service");
    QVERIFY(flatpak->keyringBackendChanged(kUser));
    QTest::ignoreMessage(
        QtWarningMsg,
        QRegularExpression(QStringLiteral(
            "sign-in was last saved in another keyring.*saved_in= \"?file\"? "
            "reading= \"?service\"?")));
    flatpak->accessTokenFor(kUser);
    flatpak->accessTokenFor(kUser);

    QVERIFY(flatpak->updateSessionTokens(kUser, QStringLiteral("rotated"), QString()));
    QCOMPARE(flatpak->keyringBackendFor(kUser), QStringLiteral("service"));
    QVERIFY(!flatpak->keyringBackendChanged(kUser));

    PerInstallStore single;
    auto deb = openInstall(QStringLiteral("keyring-deb"), &single);
    deb->saveSession(kHs, kUser, QStringLiteral("DEBDEV"), QStringLiteral("deb-access"));
    QVERIFY(deb->keyringBackendFor(kUser).isEmpty());
}

namespace {
const QString kFlatpak = QStringLiteral("keyring-flatpak");

QString recordedBackendOf(const QString &userId)
{
    QSettings raw;
    return raw.value(QStringLiteral("accounts/")
                     + matrix::app_data::safeUserSlug(userId)
                     + QStringLiteral("/keyringBackend"))
        .toString();
}

int scopedItemsFor(const FakeKeyring &keyring, const QString &userId)
{
    int count = 0;
    for (const Item &item : keyring.withSchema(kScopedSchema)) {
        if (item.attributes.value(QStringLiteral("user_id")) == userId)
            ++count;
    }
    return count;
}
}

void KeyringScopeTest::aSignInTheSecretServiceHoldsIsReadThereWhenALaterLaunchGetsTheFileKeyring()
{
    FakeKeyring file;
    FakeKeyring service;
    {
        SandboxKeyringStore store(&file, &service, QStringLiteral("service"));
        auto launch = openInstall(kFlatpak, &store);
        launch->saveSession(kHs, kUser, QStringLiteral("FLATPAKDEV"),
                            QStringLiteral("access-0"), QStringLiteral("refresh-0"),
                            QStringLiteral("oauth"), QStringLiteral("client"));
        QCOMPARE(launch->keyringBackendFor(kUser), QStringLiteral("service"));
    }
    QCOMPARE(scopedItemsFor(file, kUser), 0);
    QVERIFY(scopedItemsFor(service, kUser) > 0);

    {
        SandboxKeyringStore store(&file, &service, QStringLiteral("file"));
        auto launch = openInstall(kFlatpak, &store);
        QTest::ignoreMessage(
            QtWarningMsg,
            QRegularExpression(QStringLiteral(
                "saved_in= \"?service\"? reading= \"?file\"? .*reading it where "
                "it was saved")));
        QCOMPARE(launch->accessTokenFor(kUser), QStringLiteral("access-0"));
        QVERIFY(!launch->secretBackendUnavailable());
        QCOMPARE(launch->refreshTokenFor(kUser), QStringLiteral("refresh-0"));
        QCOMPARE(launch->oauthClientIdFor(kUser), QStringLiteral("client"));

        QVERIFY(launch->updateSessionTokens(kUser, QStringLiteral("access-1"),
                                            QStringLiteral("refresh-1")));
        QCOMPARE(launch->keyringBackendFor(kUser), QStringLiteral("service"));
        QCOMPARE(scopedItemsFor(file, kUser), 0);
    }

    {
        SandboxKeyringStore store(&file, &service, QStringLiteral("service"));
        auto launch = openInstall(kFlatpak, &store);
        QCOMPARE(launch->accessTokenFor(kUser), QStringLiteral("access-1"));
        QCOMPARE(launch->refreshTokenFor(kUser), QStringLiteral("refresh-1"));
    }
}

void KeyringScopeTest::aSignInTheFileKeyringHoldsIsUnreadableNotLostWhenThePortalDidNotAnswer()
{
    FakeKeyring file;
    FakeKeyring service;
    {
        SandboxKeyringStore store(&file, &service, QStringLiteral("file"));
        auto launch = openInstall(kFlatpak, &store);
        launch->saveSession(kHs, kUser, QStringLiteral("FLATPAKDEV"),
                            QStringLiteral("file-access"));
        QCOMPARE(launch->keyringBackendFor(kUser), QStringLiteral("file"));
    }
    const int fileItems = scopedItemsFor(file, kUser);
    QVERIFY(fileItems > 0);

    {
        SandboxKeyringStore store(&file, &service, QStringLiteral("service"));
        auto launch = openInstall(kFlatpak, &store);
        QTest::ignoreMessage(
            QtWarningMsg,
            QRegularExpression(QStringLiteral(
                "saved_in= \"?file\"? reading= \"?service\"? .*cannot be read in "
                "this launch")));
        QVERIFY(launch->accessTokenFor(kUser).isEmpty());
        QVERIFY2(launch->secretBackendUnavailable(),
                 "a sign-in this launch cannot reach read as gone");
        QVERIFY(launch->secretsWrittenHereForRecordedDevice(kUser));
        QCOMPARE(launch->keyringBackendFor(kUser), QStringLiteral("file"));
        QVERIFY(!store.clearAccountSecrets(kUser));
        QVERIFY(!store.lastError().isEmpty());
    }
    QCOMPARE(scopedItemsFor(file, kUser), fileItems);

    {
        SandboxKeyringStore store(&file, &service, QStringLiteral("file"));
        auto launch = openInstall(kFlatpak, &store);
        QCOMPARE(launch->accessTokenFor(kUser), QStringLiteral("file-access"));
        QVERIFY(!launch->secretBackendUnavailable());
    }
}

void KeyringScopeTest::aSecretServiceNotUpYetIsTransientAndTheNextReadFindsTheSignIn()
{
    FakeKeyring file;
    FakeKeyring service;
    {
        SandboxKeyringStore store(&file, &service, QStringLiteral("service"));
        auto launch = openInstall(kFlatpak, &store);
        launch->saveSession(kHs, kUser, QStringLiteral("FLATPAKDEV"),
                            QStringLiteral("service-access"));
    }
    SandboxKeyringStore store(&file, &service, QStringLiteral("file"));
    service.locked = true;
    auto launch = openInstall(kFlatpak, &store);
    QTest::ignoreMessage(QtWarningMsg,
                         QRegularExpression(QStringLiteral("saved_in= \"?service\"?")));
    QVERIFY(launch->accessTokenFor(kUser).isEmpty());
    QVERIFY(launch->secretBackendUnavailable());

    service.locked = false;
    QCOMPARE(launch->accessTokenFor(kUser), QStringLiteral("service-access"));
    QVERIFY(!launch->secretBackendUnavailable());
}

void KeyringScopeTest::signingOutOfAnAccountReadPastTheFileKeyringClearsBothKeyrings()
{
    FakeKeyring file;
    FakeKeyring service;
    {
        SandboxKeyringStore store(&file, &service, QStringLiteral("service"));
        auto launch = openInstall(kFlatpak, &store);
        launch->saveSession(kHs, kUser, QStringLiteral("FLATPAKDEV"),
                            QStringLiteral("service-access"));
    }
    SandboxKeyringStore store(&file, &service, QStringLiteral("file"));
    auto launch = openInstall(kFlatpak, &store);
    file.store(scopedAttrs(kUser, QStringLiteral("mediaStoreKey"), store.installScope()),
               QStringLiteral("stray"));

    QVERIFY(launch->clearSessionForAccount(kUser));
    QCOMPARE(scopedItemsFor(service, kUser), 0);
    QCOMPARE(scopedItemsFor(file, kUser), 0);
}

void KeyringScopeTest::aSignInSavedBeforeTheRecordIsRecordedWhereItIsFound()
{
    FakeKeyring file;
    FakeKeyring service;
    {
        SandboxKeyringStore store(&file, &service, QStringLiteral("service"));
        auto launch = openInstall(kFlatpak, &store);
        launch->saveSession(kHs, kUser, QStringLiteral("FLATPAKDEV"),
                            QStringLiteral("service-access"));
    }
    {
        useInstall(kFlatpak);
        QSettings raw;
        raw.remove(QStringLiteral("accounts/")
                   + matrix::app_data::safeUserSlug(kUser)
                   + QStringLiteral("/keyringBackend"));
        raw.sync();
        QVERIFY(recordedBackendOf(kUser).isEmpty());
    }
    {
        SandboxKeyringStore store(&file, &service, QStringLiteral("service"));
        auto launch = openInstall(kFlatpak, &store);
        QCOMPARE(launch->accessTokenFor(kUser), QStringLiteral("service-access"));
        QCOMPARE(launch->keyringBackendFor(kUser), QStringLiteral("service"));
    }
    {
        SandboxKeyringStore store(&file, &service, QStringLiteral("file"));
        auto launch = openInstall(kFlatpak, &store);
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression(QStringLiteral("saved_in= \"?service\"?")));
        QCOMPARE(launch->accessTokenFor(kUser), QStringLiteral("service-access"));
    }
}

QTEST_GUILESS_MAIN(KeyringScopeTest)
#include "KeyringScopeTest.moc"
