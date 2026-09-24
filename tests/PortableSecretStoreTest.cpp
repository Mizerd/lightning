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

// Hermetic, platform-neutral coverage for the portable secret store.
//
// Central property (relocatesToADifferentAbsoluteRoot): a sign-in saved under
// one absolute path must open under a completely different one, since the
// ZIP is extracted to different places on different PCs. The rest guards
// against an absolute path in a file, a machine-bound key, or a damaged file
// being replaced by a fresh one.
//
// An unreadable secret must never be treated as an absent one, so every
// corruption case asserts that the store refuses, says why, and leaves the
// bytes on disk bit-identical.
class PortableSecretStoreTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void freshDirectoryIsUsableAndReportsNothingStored();
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
    // InsecureFallbackSecretStore default-constructs QSettings, which needs
    // organization and application names; without them every read fails for
    // an unrelated reason.
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
    // A just-extracted portable folder has no secrets. That is an ordinary
    // answer, not a failure, and nothing is minted merely by looking.
    QVERIFY(store.isAvailable());
    QVERIFY(store.lastError().isEmpty());
    QCOMPARE(store.readSecret(QStringLiteral("@a:example.org"),
                              QStringLiteral("accessToken")),
             QString());
    QVERIFY(!store.lastReadFailed());
    QVERIFY(!QFile::exists(dataPathIn(dir)));
    QVERIFY(!QFile::exists(keyPathIn(dir)));
}

void PortableSecretStoreTest::roundTripsIncludingUnicodeAndLargeValues()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString dir = secretsDirIn(root);

    // Unicode in the user id, key and value: a UTF-8 round trip breaks if
    // anything on the path uses Latin-1.
    const QString user = QStringLiteral("@ąžuolas:例え.example.org");
    const QString key = QStringLiteral("accessToken-Ω");
    const QString value = QStringLiteral("syt_ĄČĘ-ẞ-😀-\u00a0-token");
    // A value large enough to exceed one cipher block many times over, so a
    // buffer-sizing mistake in the seal/open helpers cannot hide.
    const QString bulk = QString(200000, QLatin1Char('x'));

    PortableSecretStore store(dir);
    QVERIFY(store.isAvailable());
    QVERIFY(store.storeSecret(user, key, value));
    QVERIFY(store.storeSecret(user, QStringLiteral("refreshToken"), bulk));
    QCOMPARE(store.readSecret(user, key), value);
    QCOMPARE(store.readSecret(user, QStringLiteral("refreshToken")), bulk);
    QVERIFY(!store.lastReadFailed());

    // An empty value is a legitimate stored value and must not read back as
    // "absent". lastReadFailed() is the only thing that separates the two.
    QVERIFY(store.storeSecret(user, QStringLiteral("empty"), QString()));
    QCOMPARE(store.readSecret(user, QStringLiteral("empty")), QString());
    QVERIFY(!store.lastReadFailed());

    // Exactly two files, and no per-account file NAME: a Matrix user id must
    // not become directory metadata in a folder that travels.
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
        // A second process over the same folder must reuse the existing key
        // rather than mint a second one, or the first sign-in is lost.
        PortableSecretStore second(dir);
        QVERIFY(second.isAvailable());
        QCOMPARE(second.readSecret(user, QStringLiteral("accessToken")),
                 QStringLiteral("syt_first"));
    }
}

void PortableSecretStoreTest::relocatesToADifferentAbsoluteRoot()
{
    // Write under one absolute path, move the two files to an unrelated
    // absolute path, and read them there.
    QTemporaryDir rootA;
    QTemporaryDir rootB;
    QVERIFY(rootA.isValid());
    QVERIFY(rootB.isValid());
    const QString dirA = secretsDirIn(rootA);
    // A different depth AND non-ASCII characters and a space, because a real
    // relocation is "D:\Portable Apps" -> "C:\Users\Ąžuolas\Desktop\lt".
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
    // Remove the origin entirely, so nothing can be silently reading through
    // to it. On the user's machine the old PC is simply not there.
    QVERIFY(QFile::remove(dataPathIn(dirA)));
    QVERIFY(QFile::remove(keyPathIn(dirA)));

    PortableSecretStore moved(dirB);
    QVERIFY2(moved.isAvailable(), qPrintable(moved.lastError()));
    QCOMPARE(moved.readSecret(user, QStringLiteral("accessToken")), value);
    // The device id is what makes this the SAME Matrix device after the move
    // rather than a second one, which is the user-visible half of the goal.
    QCOMPARE(moved.readSecret(user, QStringLiteral("deviceId")),
             QStringLiteral("LIGHTNINGDEV"));
    QVERIFY(!moved.lastReadFailed());

    // And it stays writable at the new location.
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

    // No plaintext: the token and user id must not appear in the file.
    QVERIFY(!data.contains(value.toUtf8()));
    QVERIFY(!data.contains(user.toUtf8()));
    QVERIFY(!keyFile.contains(value.toUtf8()));
    QVERIFY(!keyFile.contains(user.toUtf8()));

    // No absolute path: a recorded path is what would stop the folder being
    // portable.
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

    // Two independently minted keys, so one folder's key cannot open the
    // other's document even though both are on this same machine.
    QVERIFY(readAll(keyPathIn(dirA)) != readAll(keyPathIn(dirB)));
}

void PortableSecretStoreTest::aForeignDocumentFailsInsteadOfReadingEmpty()
{
    // A document sealed with a different key (half a folder copied, or files
    // from two installs paired) must be refused by GCM authentication, not
    // read as empty, which would look like "no saved sign-in".
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

    // B's key, A's document.
    QVERIFY(copyFile(dataPathIn(dirA), dataPathIn(dirB)));
    const QByteArray before = readAll(dataPathIn(dirB));

    PortableSecretStore mismatched(dirB);
    QVERIFY(!mismatched.isAvailable());
    QVERIFY(!mismatched.lastError().isEmpty());
    QCOMPARE(mismatched.readSecret(user, QStringLiteral("accessToken")), QString());
    QVERIFY(mismatched.lastReadFailed());

    // And every write refuses, leaving the bytes exactly as found.
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
        // Nothing minted over it, and the document untouched.
        QCOMPARE(QFileInfo(keyPathIn(dir)).size(), qint64(0));
        QCOMPARE(readAll(dataPathIn(dir)), dataBefore);
    }

    // Restoring the key from a backup of the folder recovers the session,
    // which is only true because nothing overwrote it above.
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
    // The message must name the missing file: this is the one failure the user
    // can actually repair, by restoring secrets.key from a copy of the folder.
    QVERIFY(orphaned.lastError().contains(PortableSecretStore::keyFileName()));
    QVERIFY(orphaned.readSecret(user, QStringLiteral("accessToken")).isEmpty());
    QVERIFY(orphaned.lastReadFailed());
    QVERIFY(!orphaned.storeSecret(user, QStringLiteral("accessToken"),
                                  QStringLiteral("nope")));
    // No key minted, no document rewritten.
    QVERIFY(!QFile::exists(keyPathIn(dir)));
    QCOMPARE(readAll(dataPathIn(dir)), dataBefore);
}

void PortableSecretStoreTest::flippedCiphertextByteFailsAuthentication()
{
    // One bit flipped inside the ciphertext: AEAD turns this into a hard
    // refusal, not garbage.
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

    // Deleting nothing is reported as nothing, never as a successful removal.
    QVERIFY(!store.deleteSecret(user, QStringLiteral("accessToken")));

    // The deletion reached the disk, not just the in-memory view.
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
    // Nonce reuse under one key is fatal in GCM: the nonce is fresh per write,
    // so the same plaintext never seals to the same bytes twice.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString dir = secretsDirIn(root);
    const QString user = QStringLiteral("@alice:example.org");
    const QString value = QStringLiteral("syt_identical_every_time");

    PortableSecretStore store(dir);
    QSet<QString> nonces;
    QSet<QByteArray> ciphertexts;
    for (int i = 0; i < 8; ++i) {
        // Alternate so the plaintext returns to the SAME content repeatedly.
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
    // FAT32/exFAT have no ownership bits and Windows maps QFileDevice
    // permissions loosely, so owner-only permissions are not asserted here.
    QSKIP("owner-only permissions are not enforceable on this platform");
#endif
}

void PortableSecretStoreTest::neverClaimsToBeSecure()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    PortableSecretStore store(secretsDirIn(root));

    // isSecure() is false: the key sits beside the ciphertext so the folder
    // can move, and possession of the folder is access to the session.
    QVERIFY(!store.isSecure());
    const QString name = store.backendName();
    QVERIFY(!name.isEmpty());
    QVERIFY(name.contains(QStringLiteral("folder")));
}

// ── The insecure fallback's one predicate ───────────────────────────────
//
// Destructive decisions key on lastReadFailed(). In substituted mode (a
// native backend compiled in but unavailable, e.g. no keyring daemon) it
// decides whether a working install is told it cannot read its sign-ins.

void PortableSecretStoreTest::aSubstitutedFallbackVouchesForAReadThatFoundSomething()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    useTemporarySettings(root, QStringLiteral("fallback-vouches"));
    InsecureFallbackSecretStore store(nullptr, /*substitutedForNative=*/true);
    QVERIFY(store.isAvailable());
    QVERIFY(!store.isSecure());

    QVERIFY(store.storeSecret(QStringLiteral("@a:example.org"),
                              QStringLiteral("access_token"),
                              QStringLiteral("syt_real_token")));
    QCOMPARE(store.readSecret(QStringLiteral("@a:example.org"),
                              QStringLiteral("access_token")),
             QStringLiteral("syt_real_token"));
    // A substituted store that just read a secret back must vouch for it, or
    // every launch reports unreadable sign-ins and hides real expiries.
    QVERIFY2(!store.lastReadFailed(),
             "a substituted store refused to vouch for a secret it had just "
             "returned; substitution makes a MISS ambiguous, never a hit");
}

void PortableSecretStoreTest::aSubstitutedFallbackStillRefusesToVouchForAMiss()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    useTemporarySettings(root, QStringLiteral("fallback-miss"));
    InsecureFallbackSecretStore store(nullptr, /*substitutedForNative=*/true);

    // Nothing read yet: nothing proven yet.
    QVERIFY(store.lastReadFailed());
    // A miss is exactly what this store cannot speak to: the native store it
    // stood in for may hold the secret. "No readable token" is not "no
    // account".
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
    // No native backend was compiled in: this store IS the store, so its
    // misses are facts and it must not report a permanent failure.
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
    InsecureFallbackSecretStore store(nullptr, /*substitutedForNative=*/true);

    // This account's record demonstrably lives here (the store holds its
    // device id), so a missing access token is a fact and a genuinely expired
    // sign-in is reported as expired, not as "unlock the keyring".
    const QString known = QStringLiteral("@known:example.org");
    QVERIFY(store.storeSecret(known, QStringLiteral("device_id"),
                              QStringLiteral("ABCDEF")));
    QVERIFY(store.readSecret(known, QStringLiteral("access_token")).isEmpty());
    QVERIFY2(!store.lastReadFailed(),
             "a miss was called unknowable for an account this store already "
             "holds secrets for, so an expired sign-in could never be reported");

    // ...and a stranger is still unknowable: same store, different user id.
    QVERIFY(store.readSecret(QStringLiteral("@stranger:example.org"),
                             QStringLiteral("access_token")).isEmpty());
    QVERIFY2(store.lastReadFailed(),
             "a miss for an account this store has never held anything for was "
             "treated as authoritative; the native store may hold it");

    // The wipe takes the record with it, so the account becomes a stranger
    // again rather than staying permanently 'known'.
    QVERIFY(store.clearAccountSecrets(known));
    QVERIFY(store.readSecret(known, QStringLiteral("access_token")).isEmpty());
    QVERIFY(store.lastReadFailed());
}

void PortableSecretStoreTest::aSubstitutedStoreNeverSoftensTheStructuralVerdict()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    useTemporarySettings(root, QStringLiteral("fallback-structural"));

    // lastReadFailed() is per read and may soften; missesAreInconclusive() is
    // structural and must not, because the sign-in gate keys a destructive
    // local reset on it.
    InsecureFallbackSecretStore substituted(nullptr,
                                            /*substitutedForNative=*/true);
    QVERIFY(substituted.missesAreInconclusive());

    const QString user = QStringLiteral("@known:example.org");
    QVERIFY(substituted.storeSecret(user, QStringLiteral("access_token"),
                                    QStringLiteral("syt_real")));
    QCOMPARE(substituted.readSecret(user, QStringLiteral("access_token")),
             QStringLiteral("syt_real"));
    // The per-read verdict softened...
    QVERIFY(!substituted.lastReadFailed());
    // ...and the structural one did not.
    QVERIFY2(substituted.missesAreInconclusive(),
             "a successful read softened the STRUCTURAL verdict, which is what "
             "a destructive repair keys on");

    // A miss for an account it holds is conclusive per-read, and still does
    // not change the structural answer.
    QVERIFY(substituted.readSecret(user, QStringLiteral("refresh_token")).isEmpty());
    QVERIFY(!substituted.lastReadFailed());
    QVERIFY(substituted.missesAreInconclusive());

    // And a store that is NOT standing in for anything says so: its misses
    // are facts, so the destructive gate is not armed by it.
    InsecureFallbackSecretStore own(nullptr);
    QVERIFY(!own.missesAreInconclusive());
}

QTEST_MAIN(PortableSecretStoreTest)
#include "PortableSecretStoreTest.moc"
