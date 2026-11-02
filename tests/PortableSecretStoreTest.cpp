#include "storage/InsecureFallbackSecretStore.h"
#include "storage/PortableSecretStore.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

class PortableSecretStoreTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void freshDirectoryIsUsableAndReportsNothingStored();
    void itsNameIsWhatSettingsWordsPortableModeOn();
    void roundTripsIncludingUnicodeAndLargeValues();
    void survivesRestartOverTheSameDirectory();
    void relocatesToADifferentAbsoluteRoot();
    void writesNoAbsolutePathAndNoPlaintext();
    void separateRootsCannotSeeEachOther();
    void aForeignDocumentFailsInsteadOfReadingEmpty();
    void truncatedDocumentFailsAndIsLeftUntouched();
    void zeroLengthDocumentFailsAndIsLeftUntouched();
    void zeroLengthKeyFileIsNeverMintedOver();
    void missingKeyFileFailsAndLeavesTheDocument();
    void flippedCiphertextByteFailsAuthentication();
    void deletionActuallyDeletes();
    void clearAccountSecretsScopesToOneAccount();
    void everyWriteUsesAFreshNonce();
    void ownerOnlyPermissionsWhereTheFilesystemSupportsThem();
    void neverClaimsToBeSecure();
    void aSubstitutedFallbackVouchesForAReadThatFoundSomething();
    void aSubstitutedFallbackStillRefusesToVouchForAMiss();
    void anUnsubstitutedFallbackTreatsAMissAsAFact();
    void aSubstitutedFallbackTrustsAMissForAnAccountItAlreadyHolds();
    void aSubstitutedStoreNeverSoftensTheStructuralVerdict();

private:
    static QString secretsDirIn(const QTemporaryDir &root);
    static QString dataPathIn(const QString &secretsDir);
    static QString keyPathIn(const QString &secretsDir);
    static QByteArray readAll(const QString &path);
    static bool writeAll(const QString &path, const QByteArray &bytes);
    static bool copyFile(const QString &from, const QString &to);
    static void useTemporarySettings(const QTemporaryDir &root,
                                     const QString &appName);
};

void PortableSecretStoreTest::useTemporarySettings(const QTemporaryDir &root,
                                                   const QString &appName)
{
    QCoreApplication::setOrganizationName(QStringLiteral("LightningTest"));
    QCoreApplication::setApplicationName(appName);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root.path());
}

QString PortableSecretStoreTest::secretsDirIn(const QTemporaryDir &root)
{
    return QDir(root.path()).filePath(QStringLiteral("data/secrets"));
}

QString PortableSecretStoreTest::dataPathIn(const QString &secretsDir)
{
    return QDir(secretsDir).filePath(PortableSecretStore::dataFileName());
}

QString PortableSecretStoreTest::keyPathIn(const QString &secretsDir)
{
    return QDir(secretsDir).filePath(PortableSecretStore::keyFileName());
}

QByteArray PortableSecretStoreTest::readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

bool PortableSecretStoreTest::writeAll(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(bytes) == bytes.size();
}

bool PortableSecretStoreTest::copyFile(const QString &from, const QString &to)
{
    QFile::remove(to);
    return QFile::copy(from, to);
}

void PortableSecretStoreTest::freshDirectoryIsUsableAndReportsNothingStored()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString dir = secretsDirIn(root);

    PortableSecretStore store(dir);
    QVERIFY(store.isAvailable());
    QVERIFY(store.lastError().isEmpty());
    QCOMPARE(store.readSecret(QStringLiteral("@a:example.org"),
                              QStringLiteral("accessToken")),
             QString());
    QVERIFY(!store.lastReadFailed());
    QVERIFY(!QFile::exists(dataPathIn(dir)));
    QVERIFY(!QFile::exists(keyPathIn(dir)));
}

void PortableSecretStoreTest::itsNameIsWhatSettingsWordsPortableModeOn()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    PortableSecretStore store(secretsDirIn(root));
    QVERIFY(store.backendName().startsWith(QLatin1String("portable file")));
    QVERIFY(!store.isSecure());

    QFile qml(QStringLiteral(QML_DIR "/SettingsScreen.qml"));
    QVERIFY(qml.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(qml.readAll());
    QVERIFY2(text.contains(QLatin1String("app.settings.secretsArePortable")),
             "Settings must word the portable store apart from the plaintext fallback");
}

void PortableSecretStoreTest::roundTripsIncludingUnicodeAndLargeValues()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString dir = secretsDirIn(root);

    const QString user = QStringLiteral("@ąžuolas:例え.example.org");
    const QString key = QStringLiteral("accessToken-Ω");
    const QString value = QStringLiteral("syt_ĄČĘ-ẞ-😀-\u00a0-token");
    const QString bulk = QString(200000, QLatin1Char('x'));

    PortableSecretStore store(dir);
    QVERIFY(store.isAvailable());
    QVERIFY(store.storeSecret(user, key, value));
    QVERIFY(store.storeSecret(user, QStringLiteral("refreshToken"), bulk));
    QCOMPARE(store.readSecret(user, key), value);
    QCOMPARE(store.readSecret(user, QStringLiteral("refreshToken")), bulk);
    QVERIFY(!store.lastReadFailed());

    QVERIFY(store.storeSecret(user, QStringLiteral("empty"), QString()));
    QCOMPARE(store.readSecret(user, QStringLiteral("empty")), QString());
    QVERIFY(!store.lastReadFailed());

    const QStringList entries =
        QDir(dir).entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    QCOMPARE(entries.size(), 2);
    QVERIFY(entries.contains(PortableSecretStore::dataFileName()));
    QVERIFY(entries.contains(PortableSecretStore::keyFileName()));
}

void PortableSecretStoreTest::survivesRestartOverTheSameDirectory()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString dir = secretsDirIn(root);
    const QString user = QStringLiteral("@alice:example.org");

    {
        PortableSecretStore first(dir);
        QVERIFY(first.storeSecret(user, QStringLiteral("accessToken"),
                                  QStringLiteral("syt_first")));
    }
    {
        PortableSecretStore second(dir);
        QVERIFY(second.isAvailable());
        QCOMPARE(second.readSecret(user, QStringLiteral("accessToken")),
                 QStringLiteral("syt_first"));
    }
}

void PortableSecretStoreTest::relocatesToADifferentAbsoluteRoot()
{
    QTemporaryDir rootA;
    QTemporaryDir rootB;
    QVERIFY(rootA.isValid());
    QVERIFY(rootB.isValid());
    const QString dirA = secretsDirIn(rootA);
    const QString dirB =
        QDir(rootB.path()).filePath(QStringLiteral("deeper/Portable Apps/Ąžuolas/data/secrets"));

    const QString user = QStringLiteral("@alice:example.org");
    const QString value = QStringLiteral("syt_relocation_proof");

    {
        PortableSecretStore origin(dirA);
        QVERIFY(origin.storeSecret(user, QStringLiteral("accessToken"), value));
        QVERIFY(origin.storeSecret(user, QStringLiteral("deviceId"),
                                   QStringLiteral("LIGHTNINGDEV")));
    }

    QVERIFY(QDir().mkpath(dirB));
    QVERIFY(copyFile(dataPathIn(dirA), dataPathIn(dirB)));
    QVERIFY(copyFile(keyPathIn(dirA), keyPathIn(dirB)));
    QVERIFY(QFile::remove(dataPathIn(dirA)));
    QVERIFY(QFile::remove(keyPathIn(dirA)));

    PortableSecretStore moved(dirB);
    QVERIFY2(moved.isAvailable(), qPrintable(moved.lastError()));
    QCOMPARE(moved.readSecret(user, QStringLiteral("accessToken")), value);
    QCOMPARE(moved.readSecret(user, QStringLiteral("deviceId")),
             QStringLiteral("LIGHTNINGDEV"));
    QVERIFY(!moved.lastReadFailed());

    QVERIFY(moved.storeSecret(user, QStringLiteral("accessToken"),
                              QStringLiteral("syt_rotated_after_move")));
    PortableSecretStore reopened(dirB);
    QCOMPARE(reopened.readSecret(user, QStringLiteral("accessToken")),
             QStringLiteral("syt_rotated_after_move"));
}

void PortableSecretStoreTest::writesNoAbsolutePathAndNoPlaintext()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString dir = secretsDirIn(root);
    const QString user = QStringLiteral("@alice:example.org");
    const QString value = QStringLiteral("syt_a_very_distinctive_token_value");

    PortableSecretStore store(dir);
    QVERIFY(store.storeSecret(user, QStringLiteral("accessToken"), value));

    const QByteArray data = readAll(dataPathIn(dir));
    const QByteArray keyFile = readAll(keyPathIn(dir));
    QVERIFY(!data.isEmpty());
    QVERIFY(!keyFile.isEmpty());

    QVERIFY(!data.contains(value.toUtf8()));
    QVERIFY(!data.contains(user.toUtf8()));
    QVERIFY(!keyFile.contains(value.toUtf8()));
    QVERIFY(!keyFile.contains(user.toUtf8()));

    const QByteArray absolute = QDir::toNativeSeparators(dir).toUtf8();
    const QByteArray absoluteForward = dir.toUtf8();
    QVERIFY(!data.contains(absolute));
    QVERIFY(!data.contains(absoluteForward));
    QVERIFY(!keyFile.contains(absolute));
    QVERIFY(!keyFile.contains(absoluteForward));
    QVERIFY(!data.contains(root.path().toUtf8()));
    QVERIFY(!keyFile.contains(root.path().toUtf8()));
}

void PortableSecretStoreTest::separateRootsCannotSeeEachOther()
{
    QTemporaryDir rootA;
    QTemporaryDir rootB;
    QVERIFY(rootA.isValid());
    QVERIFY(rootB.isValid());
    const QString dirA = secretsDirIn(rootA);
    const QString dirB = secretsDirIn(rootB);
    const QString user = QStringLiteral("@alice:example.org");
    const QString key = QStringLiteral("accessToken");

    PortableSecretStore a(dirA);
    PortableSecretStore b(dirB);
    QVERIFY(a.storeSecret(user, key, QStringLiteral("token_A")));
    QVERIFY(b.storeSecret(user, key, QStringLiteral("token_B")));

    QCOMPARE(a.readSecret(user, key), QStringLiteral("token_A"));
    QCOMPARE(b.readSecret(user, key), QStringLiteral("token_B"));

    QVERIFY(readAll(keyPathIn(dirA)) != readAll(keyPathIn(dirB)));
}

void PortableSecretStoreTest::aForeignDocumentFailsInsteadOfReadingEmpty()
{
    QTemporaryDir rootA;
    QTemporaryDir rootB;
    QVERIFY(rootA.isValid());
    QVERIFY(rootB.isValid());
    const QString dirA = secretsDirIn(rootA);
    const QString dirB = secretsDirIn(rootB);
    const QString user = QStringLiteral("@alice:example.org");

    {
        PortableSecretStore a(dirA);
        QVERIFY(a.storeSecret(user, QStringLiteral("accessToken"),
                              QStringLiteral("token_A")));
        PortableSecretStore b(dirB);
        QVERIFY(b.storeSecret(user, QStringLiteral("accessToken"),
                              QStringLiteral("token_B")));
    }

    QVERIFY(copyFile(dataPathIn(dirA), dataPathIn(dirB)));
    const QByteArray before = readAll(dataPathIn(dirB));

    PortableSecretStore mismatched(dirB);
    QVERIFY(!mismatched.isAvailable());
    QVERIFY(!mismatched.lastError().isEmpty());
    QCOMPARE(mismatched.readSecret(user, QStringLiteral("accessToken")), QString());
    QVERIFY(mismatched.lastReadFailed());

    QVERIFY(!mismatched.storeSecret(user, QStringLiteral("accessToken"),
                                    QStringLiteral("overwrite")));
    QVERIFY(!mismatched.deleteSecret(user, QStringLiteral("accessToken")));
    QVERIFY(!mismatched.clearAccountSecrets(user));
    QCOMPARE(readAll(dataPathIn(dirB)), before);
}

void PortableSecretStoreTest::truncatedDocumentFailsAndIsLeftUntouched()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString dir = secretsDirIn(root);
    const QString user = QStringLiteral("@alice:example.org");

    {
        PortableSecretStore store(dir);
        QVERIFY(store.storeSecret(user, QStringLiteral("accessToken"),
                                  QStringLiteral("syt_will_be_truncated")));
    }

    const QByteArray full = readAll(dataPathIn(dir));
    QVERIFY(full.size() > 8);
    QVERIFY(writeAll(dataPathIn(dir), full.left(full.size() / 2)));
    const QByteArray truncated = readAll(dataPathIn(dir));

    PortableSecretStore damaged(dir);
    QVERIFY(!damaged.isAvailable());
    QVERIFY(!damaged.lastError().isEmpty());
    QCOMPARE(damaged.readSecret(user, QStringLiteral("accessToken")), QString());
    QVERIFY(damaged.lastReadFailed());
    QVERIFY(!damaged.storeSecret(user, QStringLiteral("accessToken"),
                                 QStringLiteral("nope")));
    QCOMPARE(readAll(dataPathIn(dir)), truncated);
}

void PortableSecretStoreTest::zeroLengthDocumentFailsAndIsLeftUntouched()
{
    // A zero-byte secrets.dat is a truncation, not "nothing stored yet";
    // treating it as absent would let the next sign-in seal over it.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString dir = secretsDirIn(root);
    const QString user = QStringLiteral("@alice:example.org");

    {
        PortableSecretStore store(dir);
        QVERIFY(store.storeSecret(user, QStringLiteral("accessToken"),
                                  QStringLiteral("syt_zeroed")));
    }
    QVERIFY(writeAll(dataPathIn(dir), QByteArray{}));

    PortableSecretStore damaged(dir);
    QVERIFY(!damaged.isAvailable());
    QVERIFY(!damaged.lastError().isEmpty());
    QVERIFY(damaged.readSecret(user, QStringLiteral("accessToken")).isEmpty());
    QVERIFY(damaged.lastReadFailed());
    QVERIFY(!damaged.storeSecret(user, QStringLiteral("accessToken"),
                                 QStringLiteral("nope")));
    QCOMPARE(QFileInfo(dataPathIn(dir)).size(), qint64(0));
}

void PortableSecretStoreTest::zeroLengthKeyFileIsNeverMintedOver()
{
    // Minting a fresh key over a truncated secrets.key would orphan the sealed
    // document. The store must refuse and preserve both files so restoring
    // the key from a backup still works.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString dir = secretsDirIn(root);
    const QString user = QStringLiteral("@alice:example.org");
    const QString value = QStringLiteral("syt_recoverable");

    {
        PortableSecretStore store(dir);
        QVERIFY(store.storeSecret(user, QStringLiteral("accessToken"), value));
    }

    const QByteArray keyBackup = readAll(keyPathIn(dir));
    const QByteArray dataBefore = readAll(dataPathIn(dir));
    QVERIFY(!keyBackup.isEmpty());
    QVERIFY(writeAll(keyPathIn(dir), QByteArray{}));

    {
        PortableSecretStore damaged(dir);
        QVERIFY(!damaged.isAvailable());
        QVERIFY(!damaged.lastError().isEmpty());
        QVERIFY(damaged.readSecret(user, QStringLiteral("accessToken")).isEmpty());
        QVERIFY(damaged.lastReadFailed());
        QVERIFY(!damaged.storeSecret(user, QStringLiteral("accessToken"),
                                     QStringLiteral("nope")));
        QCOMPARE(QFileInfo(keyPathIn(dir)).size(), qint64(0));
        QCOMPARE(readAll(dataPathIn(dir)), dataBefore);
    }

    QVERIFY(writeAll(keyPathIn(dir), keyBackup));
    PortableSecretStore recovered(dir);
    QVERIFY2(recovered.isAvailable(), qPrintable(recovered.lastError()));
    QCOMPARE(recovered.readSecret(user, QStringLiteral("accessToken")), value);
}

void PortableSecretStoreTest::missingKeyFileFailsAndLeavesTheDocument()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString dir = secretsDirIn(root);
    const QString user = QStringLiteral("@alice:example.org");

    {
        PortableSecretStore store(dir);
        QVERIFY(store.storeSecret(user, QStringLiteral("accessToken"),
                                  QStringLiteral("syt_orphaned")));
    }
    const QByteArray dataBefore = readAll(dataPathIn(dir));
    QVERIFY(QFile::remove(keyPathIn(dir)));

    PortableSecretStore orphaned(dir);
    QVERIFY(!orphaned.isAvailable());
    QVERIFY(orphaned.lastError().contains(PortableSecretStore::keyFileName()));
    QVERIFY(orphaned.readSecret(user, QStringLiteral("accessToken")).isEmpty());
    QVERIFY(orphaned.lastReadFailed());
    QVERIFY(!orphaned.storeSecret(user, QStringLiteral("accessToken"),
                                  QStringLiteral("nope")));
    QVERIFY(!QFile::exists(keyPathIn(dir)));
    QCOMPARE(readAll(dataPathIn(dir)), dataBefore);
}

void PortableSecretStoreTest::flippedCiphertextByteFailsAuthentication()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString dir = secretsDirIn(root);
    const QString user = QStringLiteral("@alice:example.org");

    {
        PortableSecretStore store(dir);
        QVERIFY(store.storeSecret(user, QStringLiteral("accessToken"),
                                  QStringLiteral("syt_authenticated")));
    }

    QJsonDocument envelope = QJsonDocument::fromJson(readAll(dataPathIn(dir)));
    QVERIFY(envelope.isObject());
    QJsonObject object = envelope.object();
    QByteArray ciphertext = QByteArray::fromBase64(
        object.value(QStringLiteral("ct")).toString().toLatin1());
    QVERIFY(ciphertext.size() > 4);
    ciphertext[ciphertext.size() / 2] =
        static_cast<char>(ciphertext.at(ciphertext.size() / 2) ^ 0x01);
    object.insert(QStringLiteral("ct"), QString::fromLatin1(ciphertext.toBase64()));
    QVERIFY(writeAll(dataPathIn(dir),
                     QJsonDocument(object).toJson(QJsonDocument::Compact)));

    PortableSecretStore tampered(dir);
    QVERIFY(!tampered.isAvailable());
    QVERIFY(!tampered.lastError().isEmpty());
    QVERIFY(tampered.readSecret(user, QStringLiteral("accessToken")).isEmpty());
    QVERIFY(tampered.lastReadFailed());
}

void PortableSecretStoreTest::deletionActuallyDeletes()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString dir = secretsDirIn(root);
    const QString user = QStringLiteral("@alice:example.org");

    PortableSecretStore store(dir);
    QVERIFY(store.storeSecret(user, QStringLiteral("accessToken"),
                              QStringLiteral("syt_gone_soon")));
    QVERIFY(store.storeSecret(user, QStringLiteral("deviceId"),
                              QStringLiteral("KEEPME")));
    QVERIFY(store.deleteSecret(user, QStringLiteral("accessToken")));
    QVERIFY(store.readSecret(user, QStringLiteral("accessToken")).isEmpty());
    QVERIFY(!store.lastReadFailed());

    QVERIFY(!store.deleteSecret(user, QStringLiteral("accessToken")));

    PortableSecretStore reopened(dir);
    QVERIFY(reopened.isAvailable());
    QVERIFY(reopened.readSecret(user, QStringLiteral("accessToken")).isEmpty());
    QCOMPARE(reopened.readSecret(user, QStringLiteral("deviceId")),
             QStringLiteral("KEEPME"));
    QVERIFY(!readAll(dataPathIn(dir)).contains(QByteArray("syt_gone_soon")));
}

void PortableSecretStoreTest::clearAccountSecretsScopesToOneAccount()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString dir = secretsDirIn(root);
    const QString alice = QStringLiteral("@alice:example.org");
    const QString bob = QStringLiteral("@bob:example.org");

    PortableSecretStore store(dir);
    QVERIFY(store.storeSecret(alice, QStringLiteral("accessToken"),
                              QStringLiteral("token_alice")));
    QVERIFY(store.storeSecret(alice, QStringLiteral("refreshToken"),
                              QStringLiteral("refresh_alice")));
    QVERIFY(store.storeSecret(bob, QStringLiteral("accessToken"),
                              QStringLiteral("token_bob")));

    QVERIFY(store.clearAccountSecrets(alice));
    QVERIFY(store.readSecret(alice, QStringLiteral("accessToken")).isEmpty());
    QVERIFY(store.readSecret(alice, QStringLiteral("refreshToken")).isEmpty());
    QCOMPARE(store.readSecret(bob, QStringLiteral("accessToken")),
             QStringLiteral("token_bob"));

    PortableSecretStore reopened(dir);
    QVERIFY(reopened.readSecret(alice, QStringLiteral("accessToken")).isEmpty());
    QCOMPARE(reopened.readSecret(bob, QStringLiteral("accessToken")),
             QStringLiteral("token_bob"));
    QVERIFY(!readAll(dataPathIn(dir)).contains(QByteArray("token_alice")));
}

void PortableSecretStoreTest::everyWriteUsesAFreshNonce()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString dir = secretsDirIn(root);
    const QString user = QStringLiteral("@alice:example.org");
    const QString value = QStringLiteral("syt_identical_every_time");

    PortableSecretStore store(dir);
    QSet<QString> nonces;
    QSet<QByteArray> ciphertexts;
    for (int i = 0; i < 8; ++i) {
        QVERIFY(store.storeSecret(user, QStringLiteral("accessToken"),
                                  i % 2 == 0 ? value : QStringLiteral("other")));
        const QJsonObject object =
            QJsonDocument::fromJson(readAll(dataPathIn(dir))).object();
        nonces.insert(object.value(QStringLiteral("nonce")).toString());
        ciphertexts.insert(object.value(QStringLiteral("ct")).toString().toLatin1());
    }
    QCOMPARE(nonces.size(), 8);
    QCOMPARE(ciphertexts.size(), 8);
}

void PortableSecretStoreTest::ownerOnlyPermissionsWhereTheFilesystemSupportsThem()
{
#ifdef Q_OS_UNIX
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString dir = secretsDirIn(root);

    PortableSecretStore store(dir);
    QVERIFY(store.storeSecret(QStringLiteral("@alice:example.org"),
                              QStringLiteral("accessToken"),
                              QStringLiteral("syt_private")));

    const auto ownerOnly = [](const QString &path) {
        const QFileDevice::Permissions permissions = QFile::permissions(path);
        return !permissions.testFlag(QFileDevice::ReadGroup)
            && !permissions.testFlag(QFileDevice::WriteGroup)
            && !permissions.testFlag(QFileDevice::ReadOther)
            && !permissions.testFlag(QFileDevice::WriteOther)
            && permissions.testFlag(QFileDevice::ReadOwner);
    };
    QVERIFY(ownerOnly(dataPathIn(dir)));
    QVERIFY(ownerOnly(keyPathIn(dir)));
    QVERIFY(ownerOnly(dir));
#else
    QSKIP("owner-only permissions are not enforceable on this platform");
#endif
}

void PortableSecretStoreTest::neverClaimsToBeSecure()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    PortableSecretStore store(secretsDirIn(root));

    QVERIFY(!store.isSecure());
    const QString name = store.backendName();
    QVERIFY(!name.isEmpty());
    QVERIFY(name.contains(QStringLiteral("folder")));
}

void PortableSecretStoreTest::aSubstitutedFallbackVouchesForAReadThatFoundSomething()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    useTemporarySettings(root, QStringLiteral("fallback-vouches"));
    InsecureFallbackSecretStore store(nullptr,true);
    QVERIFY(store.isAvailable());
    QVERIFY(!store.isSecure());

    QVERIFY(store.storeSecret(QStringLiteral("@a:example.org"),
                              QStringLiteral("access_token"),
                              QStringLiteral("syt_real_token")));
    QCOMPARE(store.readSecret(QStringLiteral("@a:example.org"),
                              QStringLiteral("access_token")),
             QStringLiteral("syt_real_token"));
    QVERIFY2(!store.lastReadFailed(),
             "a substituted store refused to vouch for a secret it had just "
             "returned; substitution makes a MISS ambiguous, never a hit");
}

void PortableSecretStoreTest::aSubstitutedFallbackStillRefusesToVouchForAMiss()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    useTemporarySettings(root, QStringLiteral("fallback-miss"));
    InsecureFallbackSecretStore store(nullptr,true);

    QVERIFY(store.lastReadFailed());
    QVERIFY(store.readSecret(QStringLiteral("@gone:example.org"),
                             QStringLiteral("access_token")).isEmpty());
    QVERIFY2(store.lastReadFailed(),
             "a substituted store claimed a miss was authoritative, which is "
             "the conflation that once armed a destructive reset");
}

void PortableSecretStoreTest::anUnsubstitutedFallbackTreatsAMissAsAFact()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    useTemporarySettings(root, QStringLiteral("fallback-plain"));
    InsecureFallbackSecretStore store(nullptr);

    QVERIFY(store.readSecret(QStringLiteral("@gone:example.org"),
                             QStringLiteral("access_token")).isEmpty());
    QVERIFY(!store.lastReadFailed());
    QVERIFY(store.storeSecret(QStringLiteral("@a:example.org"),
                              QStringLiteral("access_token"),
                              QStringLiteral("syt_real_token")));
    QCOMPARE(store.readSecret(QStringLiteral("@a:example.org"),
                              QStringLiteral("access_token")),
             QStringLiteral("syt_real_token"));
    QVERIFY(!store.lastReadFailed());
}

void PortableSecretStoreTest::aSubstitutedFallbackTrustsAMissForAnAccountItAlreadyHolds()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    useTemporarySettings(root, QStringLiteral("fallback-known-account"));
    InsecureFallbackSecretStore store(nullptr,true);

    const QString known = QStringLiteral("@known:example.org");
    QVERIFY(store.storeSecret(known, QStringLiteral("device_id"),
                              QStringLiteral("ABCDEF")));
    QVERIFY(store.readSecret(known, QStringLiteral("access_token")).isEmpty());
    QVERIFY2(!store.lastReadFailed(),
             "a miss was called unknowable for an account this store already "
             "holds secrets for, so an expired sign-in could never be reported");

    QVERIFY(store.readSecret(QStringLiteral("@stranger:example.org"),
                             QStringLiteral("access_token")).isEmpty());
    QVERIFY2(store.lastReadFailed(),
             "a miss for an account this store has never held anything for was "
             "treated as authoritative; the native store may hold it");

    QVERIFY(store.clearAccountSecrets(known));
    QVERIFY(store.readSecret(known, QStringLiteral("access_token")).isEmpty());
    QVERIFY(store.lastReadFailed());
}

void PortableSecretStoreTest::aSubstitutedStoreNeverSoftensTheStructuralVerdict()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    useTemporarySettings(root, QStringLiteral("fallback-structural"));

    InsecureFallbackSecretStore substituted(nullptr,
true);
    QVERIFY(substituted.missesAreInconclusive());

    const QString user = QStringLiteral("@known:example.org");
    QVERIFY(substituted.storeSecret(user, QStringLiteral("access_token"),
                                    QStringLiteral("syt_real")));
    QCOMPARE(substituted.readSecret(user, QStringLiteral("access_token")),
             QStringLiteral("syt_real"));
    QVERIFY(!substituted.lastReadFailed());
    QVERIFY2(substituted.missesAreInconclusive(),
             "a successful read softened the STRUCTURAL verdict, which is what "
             "a destructive repair keys on");

    QVERIFY(substituted.readSecret(user, QStringLiteral("refresh_token")).isEmpty());
    QVERIFY(!substituted.lastReadFailed());
    QVERIFY(substituted.missesAreInconclusive());

    InsecureFallbackSecretStore own(nullptr);
    QVERIFY(!own.missesAreInconclusive());
}

QTEST_MAIN(PortableSecretStoreTest)
#include "PortableSecretStoreTest.moc"
