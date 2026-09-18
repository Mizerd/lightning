// v0.7: persistent multi-account registry tests. Cover account records in
// SettingsManager (accounts/<slug>/), the AccountManager registry view,
// per-account token/sync-token isolation, duplicate handling, legacy
// single-session migration, and unsafe-user-id rejection.

#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "storage/AppDataPaths.h"
#include "storage/SecretStore.h"

#include <QFile>
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

// The ACTIVE ACCOUNT AS THE FILE HOLDS IT.
//
// The PATH IS PASSED IN, and that is the whole point of this helper's shape.
// Every QSettings over one file shares a single QConfFile, and CONSTRUCTING a
// QSettings calls sync() on it — so a helper that said `QSettings().fileName()`
// here would flush the very unsynced write it was built to catch, and pass on
// the broken tree. It did, on the first version of this test. Resolve the
// path once, before the write, and then touch nothing but the bytes.
QString activeAccountOnDisk(const QString &settingsPath)
{
    QFile f(settingsPath);
    if (!f.open(QIODevice::ReadOnly))
        return QStringLiteral("<no settings file>");
    const QString text = QString::fromUtf8(f.readAll());
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        if (!line.startsWith(QLatin1String("active=")))
            continue;
        QString value = line.mid(7).trimmed();
        // QSettings' INI escape for a value that begins with '@'.
        if (value.startsWith(QLatin1String("@@")))
            value = value.mid(1);
        return value;
    }
    return QString{};
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

    // THE 2026-09-18 REPORT (an AppImage): "if i switch account, close the
    // app, open it again it opens in the wrong account (i think its the one i
    // signed into as the very first)".
    //
    // accounts/active is the ONLY record of which account the next launch
    // restores, and it was the one load-bearing key written without a flush.
    // QSettings writes lazily, so until something syncs, the FILE still names
    // the previous account — while saveSession() (a sign-in) has always
    // synced. So a SIGN-IN was durable the instant it happened and a SWITCH
    // was not, which is exactly the reported shape.
    //
    // Asserted with NO event-loop iteration in between, deliberately: an
    // iteration is what used to hide this, and the window between the switch
    // and the next one is where switchToAccount() does its cache clearing and
    // the backend's whole synchronous restore.
    void theActiveAccountIsOnDiskTheMomentItChanges()
    {
        // Resolved BEFORE anything is written unsynced: see the helper.
        const QString settingsPath = QSettings().fileName();

        FakeSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);

        // alice is signed into first, then bob — so the last SYNCED write of
        // accounts/active names bob, exactly as the report's registry would.
        saveAlice(settings);
        saveBob(settings);
        QCOMPARE(settings.activeAccountUserId(), kBobId);
        QCOMPARE(activeAccountOnDisk(settingsPath), kBobId);

        // The switch. Nothing spins the event loop before the assertion.
        settings.setActiveAccountUserId(kAliceId);
        QCOMPARE(settings.activeAccountUserId(), kAliceId);
        QCOMPARE(activeAccountOnDisk(settingsPath), kAliceId);

        // And back, so the durability is not a one-direction accident.
        settings.setActiveAccountUserId(kBobId);
        QCOMPARE(activeAccountOnDisk(settingsPath), kBobId);

        // Clearing it is the same promise: signing out of every account must
        // not leave the file naming one of them.
        settings.setActiveAccountUserId(QString{});
        QVERIFY(settings.activeAccountUserId().isEmpty());
        QVERIFY2(activeAccountOnDisk(settingsPath).isEmpty(),
                 "a cleared active account was left on disk");
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

    void collidingSlugIdentitiesCannotAliasARecord()
    {
        // The slug substitution is not injective: these two DISTINCT valid
        // identities flatten to the same "jane_doe_matrix.org" slug. The
        // second one must be refused instead of clobbering the first
        // account's record (and aliasing its on-disk SDK store).
        const QString janeA = QStringLiteral("@jane_doe:matrix.org");
        const QString janeB = QStringLiteral("@jane:doe_matrix.org");
        QCOMPARE(matrix::app_data::safeUserSlug(janeA),
                 matrix::app_data::safeUserSlug(janeB));

        FakeSecretStore secrets;
        SettingsManager settings;
        settings.setSecretStore(&secrets);
        settings.saveSession(QStringLiteral("https://matrix.org"), janeA,
                             QStringLiteral("JANEADEV"),
                             QStringLiteral("jane-a-token-fixture"));

        QVERIFY(settings.accountSlugConflicts(janeB));
        settings.saveSession(QStringLiteral("https://doe_matrix.org"), janeB,
                             QStringLiteral("JANEBDEV"),
                             QStringLiteral("jane-b-token-fixture"));

        // The first account is untouched; the collider was never saved.
        QCOMPARE(settings.savedAccountUserIds(), QStringList({janeA}));
        QVERIFY(settings.hasSavedAccount(janeA));
        QVERIFY(!settings.hasSavedAccount(janeB));
        QVERIFY(settings.accessTokenFor(janeB).isEmpty());
        QCOMPARE(settings.accountRecord(janeA).value("deviceId"),
                 QStringLiteral("JANEADEV"));
        QCOMPARE(settings.accessTokenFor(janeA),
                 QStringLiteral("jane-a-token-fixture"));
        QCOMPARE(settings.activeAccountUserId(), janeA);
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
