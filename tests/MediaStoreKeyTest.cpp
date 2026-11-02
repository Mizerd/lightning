
#include "app/SettingsManager.h"
#include "matrix/MediaStoreKey.h"
#include "storage/AppDataPaths.h"
#include "storage/InsecureFallbackSecretStore.h"
#include "storage/SecretStore.h"

#include <QCoreApplication>
#include <QHash>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

namespace msk = matrix::media_store_key;

namespace {

const QString kUser = QStringLiteral("@rokas:media.example");
const QString kOther = QStringLiteral("@other:media.example");
const QString kHs = QStringLiteral("https://media.example");

class FakeStore final : public SecretStore
{
public:
    using SecretStore::SecretStore;

    bool isSecure() const override { return secure; }
    bool isAvailable() const override { return available; }
    QString backendName() const override { return QStringLiteral("fake"); }
    bool storeSecret(const QString &userId, const QString &key,
                     const QString &value) override
    {
        ++writes;
        if (locked || failWrites)
            return false;
        m_values.insert(mapKey(userId, key), value);
        return true;
    }
    QString readSecret(const QString &userId, const QString &key) const override
    {
        if (silentWhileLocked && locked) {
            m_lastReadFailed = false;
            return {};
        }
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
    bool missesAreInconclusive() const override { return inconclusive; }

    void put(const QString &userId, const QString &key, const QString &value)
    {
        m_values.insert(mapKey(userId, key), value);
    }
    QString get(const QString &userId, const QString &key) const
    {
        return m_values.value(mapKey(userId, key));
    }

    bool secure = true;
    bool available = true;
    bool locked = false;
    bool silentWhileLocked = false;
    bool failWrites = false;
    bool inconclusive = false;
    int writes = 0;

private:
    static QString mapKey(const QString &userId, const QString &key)
    {
        return userId + QLatin1Char('\x1f') + key;
    }

    QHash<QString, QString> m_values;
    mutable bool m_lastReadFailed = false;
};

const QString kName = QLatin1String(msk::kSecretName);

void seedToken(FakeStore &store, const QString &userId)
{
    store.put(userId, QLatin1String(msk::kAccessTokenName),
              QStringLiteral("syt_token_") + userId);
}

}

class MediaStoreKeyTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();

    void anAbsentKeyIsCreatedOnceAndThenReused();
    void aLockedKeyringNeverGetsANewKey();
    void aKeyringThatAnswersNothingWhileLockedGetsNoKeyEither();
    void aSecureStoreWhoseMissesAreInconclusiveGetsNoKey();
    void anUnavailableBackendGetsNoKey();
    void whileTheBackendCannotTellAnExistingKeyIsUsedButNoneIsMade();
    void aFallbackGroupHoldingAMediaKeyStillMigratesWhole();
    void aDamagedRecordIsNeverOverwritten();
    void aKeyThatWasNotStoredIsNotUsed();
    void noAccountMeansNoKeyAndNoWrite();
    void theKeyNeverReachesALogLine();
    void signingOutDeletesTheMediaKeyWithTheTokens();
    void removingOneAccountLeavesTheOthersKey();
    void theCreationPolicyNeverOverridesABackendThatCannotAnswer();
    void aDesktopWithNoKeyringKeepsAKeyForUnencryptedRoomsOnly();
    void aFallbackHoldingNothingForTheAccountMakesNoKey();

private:
    QTemporaryDir m_configHome;
};

void MediaStoreKeyTest::initTestCase()
{
    QVERIFY(m_configHome.isValid());
    qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
    QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
    QCoreApplication::setApplicationName(QStringLiteral("media-store-key-test"));
}

void MediaStoreKeyTest::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
}

void MediaStoreKeyTest::anAbsentKeyIsCreatedOnceAndThenReused()
{
    FakeStore store;
    seedToken(store, kUser);
    const msk::Resolution first = msk::resolve(&store, kUser);
    QVERIFY(first.created);
    QCOMPARE(first.key.size(), msk::kKeyBytes);
    QCOMPARE(store.get(kUser, kName), QString::fromLatin1(first.key.toHex()));

    const int writesBefore = store.writes;
    const msk::Resolution second = msk::resolve(&store, kUser);
    QVERIFY(!second.created);
    QCOMPARE(second.found, msk::Found::Present);
    QCOMPARE(second.key, first.key);
    QCOMPARE(store.writes, writesBefore);

    FakeStore otherStore;
    seedToken(otherStore, kUser);
    QVERIFY(msk::resolve(&otherStore, kUser).key != first.key);
}

void MediaStoreKeyTest::aLockedKeyringNeverGetsANewKey()
{
    FakeStore store;
    seedToken(store, kUser);
    const QString existing(64, QLatin1Char('a'));
    store.put(kUser, kName, existing);
    store.locked = true;

    const msk::Resolution r = msk::resolve(&store, kUser);
    QCOMPARE(r.found, msk::Found::Unreadable);
    QVERIFY(r.key.isEmpty());
    QVERIFY(!r.created);
    QCOMPARE(store.writes, 0);
    store.locked = false;
    QCOMPARE(store.get(kUser, kName), existing);
}

void MediaStoreKeyTest::aKeyringThatAnswersNothingWhileLockedGetsNoKeyEither()
{
    FakeStore store;
    seedToken(store, kUser);
    const QString existing(64, QLatin1Char('b'));
    store.put(kUser, kName, existing);
    store.locked = true;
    store.silentWhileLocked = true;

    const msk::Resolution r = msk::resolve(&store, kUser);
    QVERIFY(r.key.isEmpty());
    QVERIFY(!r.created);
    QCOMPARE(store.writes, 0);
    store.locked = false;
    QCOMPARE(store.get(kUser, kName), existing);
}

void MediaStoreKeyTest::aSecureStoreWhoseMissesAreInconclusiveGetsNoKey()
{
    FakeStore store;
    seedToken(store, kUser);
    store.inconclusive = true;
    const msk::Resolution r = msk::resolve(&store, kUser);
    QCOMPARE(r.found, msk::Found::Unreadable);
    QVERIFY(r.key.isEmpty());
    QCOMPARE(store.writes, 0);
}

void MediaStoreKeyTest::anUnavailableBackendGetsNoKey()
{
    FakeStore store;
    seedToken(store, kUser);
    store.available = false;
    const msk::Resolution r = msk::resolve(&store, kUser);
    QVERIFY(r.key.isEmpty());
    QCOMPARE(r.found, msk::Found::Unreadable);
    QCOMPARE(store.writes, 0);
}

void MediaStoreKeyTest::whileTheBackendCannotTellAnExistingKeyIsUsedButNoneIsMade()
{
    FakeStore store;
    seedToken(store, kUser);
    const msk::Resolution absent = msk::resolve(&store, kUser, false);
    QVERIFY(absent.key.isEmpty());
    QVERIFY(!absent.created);
    QCOMPARE(store.writes, 0);

    const msk::Resolution made = msk::resolve(&store, kUser);
    QVERIFY(made.created);
    const msk::Resolution reused = msk::resolve(&store, kUser, false);
    QCOMPARE(reused.found, msk::Found::Present);
    QCOMPARE(reused.key, made.key);
}

void MediaStoreKeyTest::aFallbackGroupHoldingAMediaKeyStillMigratesWhole()
{
    const QString slug = matrix::app_data::safeUserSlug(kUser);
    const QString mediaKey(64, QLatin1Char('c'));
    const QString group = QStringLiteral("secrets/") + kUser + QLatin1Char('/');
    {
        QSettings seed;
        const QString base = QStringLiteral("accounts/") + slug + QLatin1Char('/');
        seed.setValue(base + QStringLiteral("userId"), kUser);
        seed.setValue(base + QStringLiteral("homeserver"), kHs);
        seed.setValue(base + QStringLiteral("deviceId"), QStringLiteral("DEVICE"));
        seed.setValue(base + QStringLiteral("authType"), QStringLiteral("password"));
        seed.setValue(base + QStringLiteral("addedAt"),
                      QStringLiteral("2026-09-01T00:00:00"));
        seed.setValue(QStringLiteral("accounts/active"), kUser);
        seed.setValue(group + QLatin1String(msk::kAccessTokenName),
                      QStringLiteral("syt_plaintext"));
        seed.setValue(group + kName, mediaKey);
        seed.sync();
    }

    FakeStore secure;
    SettingsManager settings;
    settings.setSecretStore(&secure);

    QCOMPARE(secure.get(kUser, QLatin1String(msk::kAccessTokenName)),
             QStringLiteral("syt_plaintext"));
    QCOMPARE(secure.get(kUser, kName), mediaKey);
    QSettings after;
    QVERIFY2(!after.contains(group + QLatin1String(msk::kAccessTokenName)),
             "the access token was left in plaintext");
    QVERIFY2(!after.contains(group + kName), "the media key was left in plaintext");
    const msk::Resolution r = msk::read(&secure, kUser);
    QCOMPARE(r.found, msk::Found::Present);
    QCOMPARE(QString::fromLatin1(r.key.toHex()), mediaKey);
}

void MediaStoreKeyTest::aDamagedRecordIsNeverOverwritten()
{
    for (const QString &damaged : { QString(63, QLatin1Char('a')),
                                    QString(64, QLatin1Char('A')),
                                    QString(64, QLatin1Char('g')),
                                    QStringLiteral("not a key") }) {
        FakeStore store;
        seedToken(store, kUser);
        store.put(kUser, kName, damaged);
        const msk::Resolution r = msk::resolve(&store, kUser);
        QVERIFY2(r.key.isEmpty(), qPrintable(damaged));
        QCOMPARE(r.found, msk::Found::Unreadable);
        QCOMPARE(store.writes, 0);
        QCOMPARE(store.get(kUser, kName), damaged);
    }
}

void MediaStoreKeyTest::aKeyThatWasNotStoredIsNotUsed()
{
    FakeStore store;
    seedToken(store, kUser);
    store.failWrites = true;
    const msk::Resolution r = msk::resolve(&store, kUser);
    QVERIFY(r.key.isEmpty());
    QVERIFY(!r.created);
    QCOMPARE(store.writes, 1);
}

void MediaStoreKeyTest::noAccountMeansNoKeyAndNoWrite()
{
    FakeStore store;
    QVERIFY(msk::resolve(&store, QString()).key.isEmpty());
    QVERIFY(msk::resolve(nullptr, kUser).key.isEmpty());
    QVERIFY(msk::resolve(&store, kUser).key.isEmpty());
    QCOMPARE(store.writes, 0);
}

void MediaStoreKeyTest::theKeyNeverReachesALogLine()
{
    static QStringList captured;
    captured.clear();
    const QtMessageHandler previous = qInstallMessageHandler(
        [](QtMsgType, const QMessageLogContext &, const QString &message) {
            captured << message;
        });
    FakeStore store;
    seedToken(store, kUser);
    const msk::Resolution created = msk::resolve(&store, kUser);
    const msk::Resolution again = msk::resolve(&store, kUser);
    qInstallMessageHandler(previous);
    QCOMPARE(created.key.size(), msk::kKeyBytes);
    const QString hex = QString::fromLatin1(created.key.toHex());
    for (const QString &line : std::as_const(captured))
        QVERIFY2(!line.contains(hex.left(16)), "the key was logged");
    QCOMPARE(QString::fromLatin1(msk::describe(created)), QStringLiteral("created"));
    QCOMPARE(QString::fromLatin1(msk::describe(again)), QStringLiteral("present"));
}

void MediaStoreKeyTest::signingOutDeletesTheMediaKeyWithTheTokens()
{
    FakeStore store;
    SettingsManager settings;
    settings.setSecretStore(&store);
    settings.saveSession(kHs, kUser, QStringLiteral("DEVICE"),
                         QStringLiteral("syt_access"));
    const msk::Resolution r = msk::resolve(&store, kUser);
    QVERIFY(r.created);
    QVERIFY(!store.get(kUser, kName).isEmpty());

    QVERIFY(settings.clearSessionForAccount(kUser));
    QVERIFY2(store.get(kUser, kName).isEmpty(),
             "the media key outlived the account's sign-out");
    settings.saveSession(kHs, kUser, QStringLiteral("DEVICE2"),
                         QStringLiteral("syt_access2"));
    const msk::Resolution next = msk::resolve(&store, kUser);
    QVERIFY(next.created);
    QVERIFY(next.key != r.key);
}

void MediaStoreKeyTest::removingOneAccountLeavesTheOthersKey()
{
    FakeStore store;
    SettingsManager settings;
    settings.setSecretStore(&store);
    settings.saveSession(kHs, kUser, QStringLiteral("DEVICE"),
                         QStringLiteral("syt_one"));
    settings.saveSession(kHs, kOther, QStringLiteral("DEVICE"),
                         QStringLiteral("syt_two"));
    const QByteArray mine = msk::resolve(&store, kUser).key;
    const QByteArray theirs = msk::resolve(&store, kOther).key;
    QCOMPARE(mine.size(), msk::kKeyBytes);
    QCOMPARE(theirs.size(), msk::kKeyBytes);

    QVERIFY(settings.clearSessionForAccount(kUser));
    QVERIFY(store.get(kUser, kName).isEmpty());
    const msk::Resolution kept = msk::resolve(&store, kOther);
    QCOMPARE(kept.found, msk::Found::Present);
    QCOMPARE(kept.key, theirs);
}

void MediaStoreKeyTest::theCreationPolicyNeverOverridesABackendThatCannotAnswer()
{
    for (const bool inconclusive : { false, true }) {
        for (const bool secure : { false, true })
            QVERIFY(!msk::mayCreate(true, inconclusive, secure));
    }
    QVERIFY(msk::mayCreate(false, false, true));
    QVERIFY(msk::mayCreate(false, false, false));
    QVERIFY(msk::mayCreate(false, true, false));
    QVERIFY(!msk::mayCreate(false, true, true));

    msk::Resolution withKey;
    withKey.key = QByteArray(msk::kKeyBytes, '\x01');
    QVERIFY(msk::admitsEncryptedRooms(withKey, true));
    QVERIFY(!msk::admitsEncryptedRooms(withKey, false));
    QVERIFY(!msk::admitsEncryptedRooms(msk::Resolution{}, true));
}

void MediaStoreKeyTest::aDesktopWithNoKeyringKeepsAKeyForUnencryptedRoomsOnly()
{
    InsecureFallbackSecretStore store(nullptr,true);
    QVERIFY(store.missesAreInconclusive());
    QVERIFY(!store.isSecure());
    QVERIFY(store.storeSecret(kUser, QLatin1String(msk::kAccessTokenName),
                              QStringLiteral("syt_no_keyring")));
    QCOMPARE(store.readSecret(kUser, QLatin1String(msk::kAccessTokenName)),
             QStringLiteral("syt_no_keyring"));
    const bool unavailable = !store.isAvailable() || store.lastReadFailed();
    const bool mayCreate =
        msk::mayCreate(unavailable, store.missesAreInconclusive(), store.isSecure());
    QVERIFY(mayCreate);

    const msk::Resolution first = msk::resolve(&store, kUser, mayCreate);
    QVERIFY2(first.created, msk::describe(first));
    QCOMPARE(first.key.size(), msk::kKeyBytes);
    QVERIFY(!msk::admitsEncryptedRooms(first, store.isSecure()));

    InsecureFallbackSecretStore again(nullptr, true);
    const msk::Resolution second = msk::resolve(&again, kUser, mayCreate);
    QCOMPARE(second.found, msk::Found::Present);
    QCOMPARE(second.key, first.key);
    QVERIFY(!msk::admitsEncryptedRooms(second, again.isSecure()));
}

void MediaStoreKeyTest::aFallbackHoldingNothingForTheAccountMakesNoKey()
{
    InsecureFallbackSecretStore store(nullptr,true);
    QVERIFY(store.storeSecret(kOther, QLatin1String(msk::kAccessTokenName),
                              QStringLiteral("syt_other")));
    const msk::Resolution r = msk::resolve(
        &store, kUser, msk::mayCreate(false, store.missesAreInconclusive(), store.isSecure()));
    QVERIFY(r.key.isEmpty());
    QVERIFY(!r.created);
    QCOMPARE(r.found, msk::Found::Unreadable);
    QSettings settings;
    QVERIFY2(!settings.contains(QStringLiteral("secrets/") + kUser + QLatin1Char('/') + kName),
             "a key was written for an account the fallback does not hold");
}

QTEST_GUILESS_MAIN(MediaStoreKeyTest)
#include "MediaStoreKeyTest.moc"
