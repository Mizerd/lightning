// v0.7: persistent multi-account registry tests. Cover account records in
// SettingsManager (accounts/<slug>/), the AccountManager registry view,
// per-account token/sync-token isolation, duplicate handling, legacy
// single-session migration, and unsafe-user-id rejection.

#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "storage/AppDataPaths.h"
#include "storage/SecretStore.h"

#include <QHash>
#include <QSettings>
#include <QSignalSpy>
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
        const QString prefix = userId + QLatin1Char('/');
        for (auto it = m_values.begin(); it != m_values.end();) {
            if (it.key().startsWith(prefix))
                it = m_values.erase(it);
            else
                ++it;
        }
        return true;
    }

    QString lastError() const override { return {}; }

private:
    QHash<QString, QString> m_values;
};

namespace {

const QString kAliceId = QStringLiteral("@alice:one.example");
const QString kBobId = QStringLiteral("@bob:one.example");
const QString kCarolId = QStringLiteral("@carol:two.example");
const QString kHsOne = QStringLiteral("https://one.example");
const QString kHsTwo = QStringLiteral("https://two.example");

void saveAlice(SettingsManager &s)
{
    s.saveSession(kHsOne, kAliceId, QStringLiteral("ALICEDEV"),
                  QStringLiteral("alice-token-fixture"));
}

void saveBob(SettingsManager &s)
{
    s.saveSession(kHsOne, kBobId, QStringLiteral("BOBDEV"),
                  QStringLiteral("bob-token-fixture"));
}

void saveCarol(SettingsManager &s)
{
    s.saveSession(kHsTwo, kCarolId, QStringLiteral("CAROLDEV"),
                  QStringLiteral("carol-token-fixture"));
}

} // namespace

class AccountRegistryTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("account-registry-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void savesMultipleAccountsDistinctly()
    {
        FakeSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);

        saveAlice(settings);
        saveBob(settings);   // same homeserver as alice
        saveCarol(settings); // different homeserver

        QCOMPARE(settings.savedAccountUserIds().size(), 3);
        QVERIFY(settings.hasSavedAccount(kAliceId));
        QVERIFY(settings.hasSavedAccount(kBobId));
        QVERIFY(settings.hasSavedAccount(kCarolId));

        QCOMPARE(settings.accountRecord(kAliceId).value("homeserver"), kHsOne);
        QCOMPARE(settings.accountRecord(kBobId).value("homeserver"), kHsOne);
        QCOMPARE(settings.accountRecord(kCarolId).value("homeserver"), kHsTwo);
        QCOMPARE(settings.accountRecord(kAliceId).value("deviceId"),
                 QStringLiteral("ALICEDEV"));
        QCOMPARE(settings.accountRecord(kBobId).value("deviceId"),
                 QStringLiteral("BOBDEV"));

        // Every account keeps its own token.
        QCOMPARE(settings.accessTokenFor(kAliceId),
                 QStringLiteral("alice-token-fixture"));
        QCOMPARE(settings.accessTokenFor(kBobId),
                 QStringLiteral("bob-token-fixture"));
        QCOMPARE(settings.accessTokenFor(kCarolId),
                 QStringLiteral("carol-token-fixture"));
    }

    void saveSessionPreservesOtherAccounts()
    {
        // Pre-0.7, logging in a second account destroyed the first one's
        // secrets and sync position. That must never happen again.
        FakeSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);

        saveAlice(settings);
        settings.setSyncToken(QStringLiteral("alice-sync-pos"));
        saveBob(settings);

        QCOMPARE(settings.accessTokenFor(kAliceId),
                 QStringLiteral("alice-token-fixture"));
        QCOMPARE(settings.activeAccountUserId(), kBobId);

        settings.setActiveAccountUserId(kAliceId);
        QCOMPARE(settings.syncToken(), QStringLiteral("alice-sync-pos"));
    }

    void duplicateSaveUpdatesInsteadOfDuplicating()
    {
        FakeSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);

        saveAlice(settings);
        const QString firstAdded =
            settings.accountRecord(kAliceId).value("addedAt").toString();
        QVERIFY(!firstAdded.isEmpty());

        settings.saveSession(kHsOne, kAliceId, QStringLiteral("NEWDEV"),
                             QStringLiteral("alice-token-2"));

        QCOMPARE(settings.savedAccountUserIds().size(), 1);
        QCOMPARE(settings.accountRecord(kAliceId).value("deviceId"),
                 QStringLiteral("NEWDEV"));
        QCOMPARE(settings.accessTokenFor(kAliceId),
                 QStringLiteral("alice-token-2"));
        QCOMPARE(settings.accountRecord(kAliceId).value("addedAt").toString(),
                 firstAdded);
    }

    void activeAccountAndSessionViewsFollowSelection()
    {
        FakeSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);

        saveAlice(settings);
        saveBob(settings);
        QCOMPARE(settings.activeAccountUserId(), kBobId);
        QCOMPARE(settings.userId(), kBobId);
        QCOMPARE(settings.deviceId(), QStringLiteral("BOBDEV"));
        QCOMPARE(settings.accessToken(), QStringLiteral("bob-token-fixture"));

        settings.setActiveAccountUserId(kAliceId);
        QCOMPARE(settings.userId(), kAliceId);
        QCOMPARE(settings.deviceId(), QStringLiteral("ALICEDEV"));
        QCOMPARE(settings.accessToken(), QStringLiteral("alice-token-fixture"));
        QCOMPARE(settings.homeserverUrl(), kHsOne);
        QVERIFY(settings.hasSession());

        // Selecting an unknown account clears the selection.
        settings.setActiveAccountUserId(QStringLiteral("@nobody:one.example"));
        QVERIFY(settings.userId().isEmpty());
        QVERIFY(!settings.hasSession());
    }

    void activeAccountPersistsAcrossReopen()
    {
        FakeSecretStore secrets;
        {
            SettingsManager settings;
            settings.setSecretStore(&secrets);
            saveAlice(settings);
            saveCarol(settings);
            settings.setActiveAccountUserId(kAliceId);
        }
        SettingsManager reopened;
        reopened.setSecretStore(&secrets);
        QCOMPARE(reopened.savedAccountUserIds().size(), 2);
        QCOMPARE(reopened.activeAccountUserId(), kAliceId);
        QCOMPARE(reopened.deviceId(), QStringLiteral("ALICEDEV"));
        QCOMPARE(reopened.homeserverUrl(), kHsOne);
    }

    void perAccountSyncTokens()
    {
        FakeSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);

        saveAlice(settings);
        settings.setSyncToken(QStringLiteral("alice-pos"));
        saveBob(settings);
        QVERIFY(settings.syncToken().isEmpty());
        settings.setSyncToken(QStringLiteral("bob-pos"));

        settings.setActiveAccountUserId(kAliceId);
        QCOMPARE(settings.syncToken(), QStringLiteral("alice-pos"));
        settings.setActiveAccountUserId(kBobId);
        QCOMPARE(settings.syncToken(), QStringLiteral("bob-pos"));
    }

    void removingOneAccountLeavesOthersIntact()
    {
        FakeSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);

        saveAlice(settings);
        saveBob(settings);
        saveCarol(settings);

        QVERIFY(settings.clearSessionForAccount(kAliceId));
        QVERIFY(!settings.hasSavedAccount(kAliceId));
        QVERIFY(settings.accessTokenFor(kAliceId).isEmpty());
        // Others untouched.
        QVERIFY(settings.hasSavedAccount(kBobId));
        QVERIFY(settings.hasSavedAccount(kCarolId));
        QCOMPARE(settings.accessTokenFor(kBobId),
                 QStringLiteral("bob-token-fixture"));
        // Carol was active and stays active.
        QCOMPARE(settings.activeAccountUserId(), kCarolId);

        // Removing the active account clears the session.
        QVERIFY(settings.clearSessionForAccount(kCarolId));
        QVERIFY(settings.activeAccountUserId().isEmpty());
        QVERIFY(!settings.hasSession());
        QVERIFY(settings.hasSavedAccount(kBobId));
    }

    void legacySingleSessionMigratesToAccountRecord()
    {
        {
            QSettings raw;
            raw.setValue(QStringLiteral("homeserver/url"), kHsOne);
            raw.setValue(QStringLiteral("session/userId"), kAliceId);
            raw.setValue(QStringLiteral("session/deviceId"),
                         QStringLiteral("LEGACYDEV"));
            raw.setValue(QStringLiteral("session/syncToken"),
                         QStringLiteral("legacy-pos"));
            raw.sync();
        }
        SettingsManager settings;
        QVERIFY(settings.hasSavedAccount(kAliceId));
        QCOMPARE(settings.activeAccountUserId(), kAliceId);
        QCOMPARE(settings.deviceId(), QStringLiteral("LEGACYDEV"));
        QCOMPARE(settings.syncToken(), QStringLiteral("legacy-pos"));
        {
            QSettings raw;
            QVERIFY(!raw.contains(QStringLiteral("session/userId")));
            QVERIFY(!raw.contains(QStringLiteral("session/deviceId")));
            QVERIFY(!raw.contains(QStringLiteral("session/syncToken")));
        }
    }

    void unsafeUserIdsAreRejected()
    {
        FakeSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);

        // Structurally invalid ids never become account records.
        const QStringList rejected = {
            QString{},
            QStringLiteral(".."),
            QStringLiteral("no-at-sign:server"),
            QStringLiteral("@nocolon"),
        };
        for (const QString &uid : rejected) {
            settings.saveSession(kHsOne, uid, QStringLiteral("DEV"),
                                 QStringLiteral("token-fixture"));
        }
        QVERIFY2(settings.savedAccountUserIds().isEmpty(),
                 qPrintable(settings.savedAccountUserIds().join(
                     QLatin1Char(','))));
        QSettings raw;
        QVERIFY(!raw.contains(QStringLiteral("accounts/active")));

        // Hostile-but-parseable ids are flattened: every path separator is
        // replaced before the slug is used, so the derived account storage
        // stays a single path component and cannot traverse.
        const QStringList hostile = {
            QStringLiteral("@..:evil"),
            QStringLiteral("@a/../b:server"),
            QStringLiteral("@a\\..\\b:server"),
        };
        for (const QString &uid : hostile) {
            const QString slug = matrix::app_data::safeUserSlug(uid);
            if (slug.isEmpty())
                continue; // outright rejected — also fine
            QVERIFY(!slug.contains(QLatin1Char('/')));
            QVERIFY(!slug.contains(QLatin1Char('\\')));
            QVERIFY(slug != QLatin1String(".")
                    && slug != QLatin1String(".."));
        }
    }

    void accountManagerReflectsRegistry()
    {
        FakeSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        AccountManager manager(&settings);
        QSignalSpy accountsSpy(&manager, &AccountManager::accountsChanged);

        saveAlice(settings);
        saveCarol(settings);
        QVERIFY(accountsSpy.count() >= 2);

        QCOMPARE(manager.knownUserIds(),
                 QStringList({kAliceId, kCarolId}));
        QCOMPARE(manager.activeUserId(), kCarolId);

        settings.updateAccountProfile(kAliceId, QStringLiteral("Alice"),
                                      QStringLiteral("mxc://one.example/ava"));
        const QVariantList accounts = manager.accounts();
        QCOMPARE(accounts.size(), 2);
        const QVariantMap alice = accounts.at(0).toMap();
        QCOMPARE(alice.value("userId"), kAliceId);
        QCOMPARE(alice.value("displayName"), QStringLiteral("Alice"));
        QCOMPARE(alice.value("avatarUrl"),
                 QStringLiteral("mxc://one.example/ava"));
        QCOMPARE(alice.value("isActive"), false);
        QVERIFY(!alice.contains(QStringLiteral("deviceId")));
        const QVariantMap carol = accounts.at(1).toMap();
        QCOMPARE(carol.value("isActive"), true);

        manager.setActiveUser(kAliceId);
        QCOMPARE(settings.activeAccountUserId(), kAliceId);

        QVERIFY(manager.removeAccount(kCarolId));
        QCOMPARE(manager.knownUserIds(), QStringList({kAliceId}));
        QCOMPARE(manager.activeUserId(), kAliceId);
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_GUILESS_MAIN(AccountRegistryTest)
#include "AccountRegistryTest.moc"
