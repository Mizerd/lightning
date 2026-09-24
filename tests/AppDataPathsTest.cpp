#include "storage/AppDataPaths.h"
#include "storage/PortableMode.h"

#include <QDir>
#include <QFile>
#include <QDirIterator>
#include <QTemporaryDir>
#include <QtTest>

class AppDataPathsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void resolveAppDataBase_data();
    void resolveAppDataBase();
    void resolveAppDataBasePortableRootWins();
    void composeAppDataRoot_data();
    void composeAppDataRoot();
    void canonicalIdentity_data();
    void canonicalIdentity();
    void rejectsInvalidIdentity_data();
    void rejectsInvalidIdentity();
    void preservesExistingSlugCompatibility();
    void removesOnlySelectedRustState();
    void resetIsIdempotent();
    void refusesUnsafeParentPath();
    void removesStoreSymlinkWithoutFollowingIt();
    void reportsPartialRemovalFailure();
    void removalTakesEveryStoreThatHoldsMessageText();

private:
    bool writeFile(const QString &path, const QByteArray &contents = "fixture");

    QTemporaryDir m_dataHome;
};

void AppDataPathsTest::initTestCase()
{
    QVERIFY(m_dataHome.isValid());
    qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
    // Isolate every other variable resolveAppDataBase() can read too, so no
    // case can write into a real data directory.
    qputenv("HOME", m_dataHome.path().toUtf8());
    qputenv("LOCALAPPDATA", m_dataHome.path().toUtf8() + "/AppData/Local");
    qputenv("USERPROFILE", m_dataHome.path().toUtf8());
    qunsetenv("LIGHTNING_PORTABLE");

    // Pin installed mode: primaryRoot() consults lightning::portable, and a
    // stray portable.marker in the build tree would move every path.
    lightning::portable::setPortableOverrideForTest(false, QString());
    QVERIFY(!lightning::portable::isPortable());
}

void AppDataPathsTest::cleanupTestCase()
{
    lightning::portable::clearPortableOverrideForTest();
}

void AppDataPathsTest::resolveAppDataBasePortableRootWins()
{
    // The portable root is an override, not a fallback: an inherited
    // environment cannot steer a portable copy back into AppData.
    // PortableModeTest covers the full precedence; this pins the signature.
    QCOMPARE(matrix::app_data::resolveAppDataBase(
                 true, QStringLiteral("D:/xdg"),
                 QStringLiteral("C:/Users/X/AppData/Local"),
                 QStringLiteral("C:/Users/X"), QStringLiteral("/home/x"),
                 QStringLiteral("E:/Lightning/data")),
             QStringLiteral("E:/Lightning/data"));
    // Omitting it keeps the five-argument behaviour exactly.
    QCOMPARE(matrix::app_data::resolveAppDataBase(
                 true, QString(), QStringLiteral("C:/Users/X/AppData/Local"),
                 QStringLiteral("C:/Users/X"), QStringLiteral("/home/x")),
             QStringLiteral("C:/Users/X/AppData/Local"));
}

void AppDataPathsTest::composeAppDataRoot_data()
{
    QTest::addColumn<QString>("base");
    QTest::addColumn<bool>("portable");
    QTest::addColumn<QString>("expected");

    // Installed: unchanged, so an MSI/Setup install keeps reading its data.
    QTest::newRow("installed-linux")
        << QStringLiteral("/home/x/.local/share") << false
        << QStringLiteral("/home/x/.local/share/MatrixClient/matrix-client");
    QTest::newRow("installed-windows")
        << QStringLiteral("C:/Users/X/AppData/Local") << false
        << QStringLiteral("C:/Users/X/AppData/Local/MatrixClient/matrix-client");
    // Portable: no vendor/app segments; <program dir>/data is already
    // Lightning's.
    QTest::newRow("portable")
        << QStringLiteral("E:/Lightning/data") << true
        << QStringLiteral("E:/Lightning/data/matrix");
    QTest::newRow("portable-spaces")
        << QStringLiteral("E:/Portable Apps/Lightning/data") << true
        << QStringLiteral("E:/Portable Apps/Lightning/data/matrix");
    // No resolvable base means empty, never a bogus root, in either mode.
    QTest::newRow("empty-installed") << QString() << false << QString();
    QTest::newRow("empty-portable") << QString() << true << QString();
}

void AppDataPathsTest::composeAppDataRoot()
{
    QFETCH(QString, base);
    QFETCH(bool, portable);
    QFETCH(QString, expected);
    QCOMPARE(matrix::app_data::composeAppDataRoot(base, portable), expected);
}

void AppDataPathsTest::resolveAppDataBase_data()
{
    QTest::addColumn<bool>("windows");
    QTest::addColumn<QString>("xdg");
    QTest::addColumn<QString>("localAppData");
    QTest::addColumn<QString>("userProfile");
    QTest::addColumn<QString>("home");
    QTest::addColumn<QString>("expected");

    // XDG_DATA_HOME always wins, on either platform.
    QTest::newRow("xdg-wins-linux")
        << false << QStringLiteral("/xdg/data") << QString() << QString()
        << QStringLiteral("/home/x") << QStringLiteral("/xdg/data");
    QTest::newRow("xdg-wins-windows")
        << true << QStringLiteral("D:/xdg") << QStringLiteral("C:/Users/X/AppData/Local")
        << QString() << QString() << QStringLiteral("D:/xdg");

    // Windows: LOCALAPPDATA, then USERPROFILE\AppData\Local. Drive letters,
    // spaces and Unicode profile names must survive.
    QTest::newRow("win-localappdata")
        << true << QString() << QStringLiteral("C:/Users/Test/AppData/Local")
        << QStringLiteral("C:/Users/Test") << QStringLiteral("/ignored")
        << QStringLiteral("C:/Users/Test/AppData/Local");
    QTest::newRow("win-userprofile-fallback")
        << true << QString() << QString()
        << QStringLiteral("C:/Users/Rokas Name") << QString()
        << QStringLiteral("C:/Users/Rokas Name/AppData/Local");
    QTest::newRow("win-unicode-profile")
        << true << QString() << QString()
        << QStringLiteral("C:/Users/Žmogus") << QString()
        << QStringLiteral("C:/Users/Žmogus/AppData/Local");

    // The Windows variables are ignored off Windows even when set.
    QTest::newRow("linux-ignores-localappdata")
        << false << QString() << QStringLiteral("C:/Users/X/AppData/Local")
        << QString() << QStringLiteral("/home/y")
        << QStringLiteral("/home/y/.local/share");

    // POSIX HOME fallback.
    QTest::newRow("linux-home")
        << false << QString() << QString() << QString()
        << QStringLiteral("/home/z") << QStringLiteral("/home/z/.local/share");

    // Nothing resolvable means empty, never a bogus root.
    QTest::newRow("nothing-linux")
        << false << QString() << QString() << QString() << QString() << QString();
    QTest::newRow("nothing-windows")
        << true << QString() << QString() << QString() << QString() << QString();
}

void AppDataPathsTest::resolveAppDataBase()
{
    QFETCH(bool, windows);
    QFETCH(QString, xdg);
    QFETCH(QString, localAppData);
    QFETCH(QString, userProfile);
    QFETCH(QString, home);
    QFETCH(QString, expected);
    QCOMPARE(matrix::app_data::resolveAppDataBase(windows, xdg, localAppData,
                                                  userProfile, home),
             expected);
}

void AppDataPathsTest::canonicalIdentity_data()
{
    QTest::addColumn<QString>("homeserver");
    QTest::addColumn<QString>("user");
    QTest::addColumn<QString>("expectedHomeserver");
    QTest::addColumn<QString>("expectedUserId");

    QTest::newRow("full-mxid")
        << QStringLiteral(" https://MATRIX.SMETONIS.NET/ ")
        << QStringLiteral(" @test:Matrix.Smetonis.Net ")
        << QStringLiteral("https://matrix.smetonis.net")
        << QStringLiteral("@test:matrix.smetonis.net");
    QTest::newRow("localpart")
        << QStringLiteral("https://matrix.smetonis.net")
        << QStringLiteral("test")
        << QStringLiteral("https://matrix.smetonis.net")
        << QStringLiteral("@test:matrix.smetonis.net");
    // The server name is lowercased; the localpart is not. Uppercase
    // localparts are legal, so folding would alias two accounts onto one SDK
    // store. Typed-vs-canonical differences are handled by
    // SettingsManager::canonicalUserIdForTypedIdentity and by recording the
    // store location (bindStoreSlug / storeSlugFor), never by mangling ids or
    // moving stores.
    QTest::newRow("uppercase-localpart-preserved")
        << QStringLiteral("https://matrix.smetonis.net")
        << QStringLiteral("@Mizerd:Matrix.Smetonis.Net")
        << QStringLiteral("https://matrix.smetonis.net")
        << QStringLiteral("@Mizerd:matrix.smetonis.net");
    QTest::newRow("trailing-slash-and-path")
        << QStringLiteral("https://matrix.example/proxy///")
        << QStringLiteral(" alice ")
        << QStringLiteral("https://matrix.example/proxy")
        << QStringLiteral("@alice:matrix.example");
}

void AppDataPathsTest::canonicalIdentity()
{
    QFETCH(QString, homeserver);
    QFETCH(QString, user);
    QFETCH(QString, expectedHomeserver);
    QFETCH(QString, expectedUserId);

    matrix::app_data::AccountIdentity identity;
    QVERIFY(matrix::app_data::resolveAccountIdentity(
        homeserver, user, &identity));
    QCOMPARE(identity.homeserver, expectedHomeserver);
    QCOMPARE(identity.userId, expectedUserId);
    QVERIFY(identity.isValid());
}

void AppDataPathsTest::rejectsInvalidIdentity_data()
{
    QTest::addColumn<QString>("homeserver");
    QTest::addColumn<QString>("user");

    QTest::newRow("empty") << QString{} << QString{};
    QTest::newRow("missing-host") << QStringLiteral("https:///")
                                   << QStringLiteral("alice");
    QTest::newRow("missing-scheme") << QStringLiteral("matrix.example")
                                     << QStringLiteral("alice");
    QTest::newRow("malformed-mxid") << QStringLiteral("https://matrix.example")
                                     << QStringLiteral("@alice");
    QTest::newRow("traversal-localpart")
        << QStringLiteral("https://matrix.example")
        << QStringLiteral("../alice");
    QTest::newRow("traversal-server")
        << QStringLiteral("https://matrix.example")
        << QStringLiteral("@alice:../matrix.example");
    QTest::newRow("query") << QStringLiteral("https://matrix.example/?x=1")
                            << QStringLiteral("alice");
}

void AppDataPathsTest::rejectsInvalidIdentity()
{
    QFETCH(QString, homeserver);
    QFETCH(QString, user);
    matrix::app_data::AccountIdentity identity;
    QVERIFY(!matrix::app_data::resolveAccountIdentity(
        homeserver, user, &identity));
    QVERIFY(!identity.isValid());
}

void AppDataPathsTest::preservesExistingSlugCompatibility()
{
    QCOMPARE(matrix::app_data::safeUserSlug(
                 QStringLiteral("@test:matrix.smetonis.net")),
             QStringLiteral("test_matrix.smetonis.net"));

    matrix::app_data::AccountIdentity identity;
    QVERIFY(matrix::app_data::resolveAccountIdentity(
        QStringLiteral("https://matrix.smetonis.net"),
        QStringLiteral("test"), &identity));
    QCOMPARE(identity.slug, QStringLiteral("test_matrix.smetonis.net"));
}

bool AppDataPathsTest::writeFile(const QString &path, const QByteArray &contents)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(contents) == contents.size();
}

void AppDataPathsTest::removesOnlySelectedRustState()
{
    matrix::app_data::AccountIdentity selected;
    matrix::app_data::AccountIdentity other;
    QVERIFY(matrix::app_data::resolveAccountIdentity(
        QStringLiteral("https://matrix.example"), QStringLiteral("alice"),
        &selected));
    QVERIFY(matrix::app_data::resolveAccountIdentity(
        QStringLiteral("https://other.example"), QStringLiteral("bob"),
        &other));

    QVERIFY(writeFile(selected.rustStorePath + QStringLiteral("/crypto.db")));
    QVERIFY(writeFile(selected.rustSmokeSessionPath, "session fixture"));
    QVERIFY(writeFile(
        matrix::app_data::rustSdkSmokeSessionTempPath(selected.userId),
        "temporary session fixture"));
    QVERIFY(writeFile(selected.accountRoot + QStringLiteral("/cache.sqlite")));
    QVERIFY(writeFile(selected.accountRoot + QStringLiteral("/notes.txt")));
    QVERIFY(writeFile(other.rustStorePath + QStringLiteral("/crypto.db")));

    const auto summary = matrix::app_data::removeAccountRustState(selected);
    QVERIFY(summary.ok());
    QCOMPARE(summary.deleted, 3);
    QCOMPARE(summary.failed, 0);
    QVERIFY(!QFileInfo::exists(selected.rustStorePath));
    QVERIFY(!QFileInfo::exists(selected.rustSmokeSessionPath));
    QVERIFY(QFileInfo::exists(selected.accountRoot + QStringLiteral("/cache.sqlite")));
    QVERIFY(QFileInfo::exists(selected.accountRoot + QStringLiteral("/notes.txt")));
    QVERIFY(QFileInfo::exists(other.rustStorePath + QStringLiteral("/crypto.db")));
}

// Message content is stored decrypted on disk (docs/privacy.md), and removing
// the account deletes it, so this names every store that holds message text:
// the SDK event cache, its state store and Lightning's search index,
// including SQLite WAL/SHM sidecars, which can hold plaintext the main
// database does not yet.
void AppDataPathsTest::removalTakesEveryStoreThatHoldsMessageText()
{
    matrix::app_data::AccountIdentity identity;
    QVERIFY(matrix::app_data::resolveAccountIdentity(
        QStringLiteral("https://plaintext.example"), QStringLiteral("alice"),
        &identity));

    const QByteArray marker = "PLAINTEXTPROBE-a-decrypted-message-body";
    const QStringList stores = {
        QStringLiteral("matrix-sdk-event-cache.sqlite3"),
        QStringLiteral("matrix-sdk-event-cache.sqlite3-wal"),
        QStringLiteral("matrix-sdk-event-cache.sqlite3-shm"),
        QStringLiteral("matrix-sdk-state.sqlite3"),
        QStringLiteral("matrix-sdk-state.sqlite3-wal"),
        QStringLiteral("lightning-search.sqlite3"),
        QStringLiteral("lightning-search.sqlite3-wal"),
        QStringLiteral("matrix-sdk-crypto.sqlite3"),
    };
    for (const QString &name : stores)
        QVERIFY(writeFile(identity.rustStorePath + QLatin1Char('/') + name,
                          marker));

    const auto summary = matrix::app_data::removeAccountRustState(identity);
    QVERIFY(summary.ok());
    QVERIFY(!QFileInfo::exists(identity.rustStorePath));
    for (const QString &name : stores) {
        const QString path = identity.rustStorePath + QLatin1Char('/') + name;
        QVERIFY2(!QFileInfo::exists(path), qPrintable(path));
    }

        // ...and nothing under the account root still contains the body, which
        // also catches stores this test does not know by name.
    QDirIterator it(identity.accountRoot, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile left(it.next());
        QVERIFY(left.open(QIODevice::ReadOnly));
        QVERIFY2(!left.readAll().contains(marker),
                 qPrintable(left.fileName()));
    }
}

void AppDataPathsTest::resetIsIdempotent()
{
    matrix::app_data::AccountIdentity identity;
    QVERIFY(matrix::app_data::resolveAccountIdentity(
        QStringLiteral("https://absent.example"), QStringLiteral("alice"),
        &identity));
    const auto summary = matrix::app_data::removeAccountRustState(identity);
    QVERIFY(summary.ok());
    QCOMPARE(summary.deleted, 0);
    QCOMPARE(summary.missing, 3);
}

void AppDataPathsTest::refusesUnsafeParentPath()
{
    matrix::app_data::AccountIdentity identity;
    QVERIFY(matrix::app_data::resolveAccountIdentity(
        QStringLiteral("https://unsafe.example"), QStringLiteral("alice"),
        &identity));
    const QString marker = matrix::app_data::primaryRoot()
        + QStringLiteral("/parent-marker");
    QVERIFY(writeFile(marker));

    identity.accountRoot = matrix::app_data::primaryRoot();
    identity.rustStorePath = identity.accountRoot;
    const auto summary = matrix::app_data::removeAccountRustState(identity);
    QVERIFY(!summary.ok());
    QCOMPARE(summary.failed, 1);
    QVERIFY(QFileInfo::exists(marker));
}

void AppDataPathsTest::removesStoreSymlinkWithoutFollowingIt()
{
    matrix::app_data::AccountIdentity identity;
    QVERIFY(matrix::app_data::resolveAccountIdentity(
        QStringLiteral("https://symlink.example"), QStringLiteral("alice"),
        &identity));
    const QString outside = m_dataHome.path() + QStringLiteral("/outside-store");
    const QString marker = outside + QStringLiteral("/marker");
    QVERIFY(writeFile(marker));
    QVERIFY(QDir().mkpath(identity.accountRoot));
    if (!QFile::link(outside, identity.rustStorePath))
        QSKIP("This platform/filesystem does not support directory symlinks");

    const auto summary = matrix::app_data::removeAccountRustState(identity);
    QVERIFY(summary.ok());
    QVERIFY(!QFileInfo(identity.rustStorePath).isSymLink());
    QVERIFY(QFileInfo::exists(marker));
}

void AppDataPathsTest::reportsPartialRemovalFailure()
{
    matrix::app_data::AccountIdentity identity;
    QVERIFY(matrix::app_data::resolveAccountIdentity(
        QStringLiteral("https://partial.example"), QStringLiteral("alice"),
        &identity));
    QVERIFY(QDir().mkpath(identity.rustSmokeSessionPath));
    QVERIFY(writeFile(identity.accountRoot + QStringLiteral("/cache.sqlite")));

    const auto summary = matrix::app_data::removeAccountRustState(identity);
    QVERIFY(!summary.ok());
    QCOMPARE(summary.failed, 1);
    QVERIFY(QFileInfo::exists(identity.accountRoot + QStringLiteral("/cache.sqlite")));
    QVERIFY(QDir(identity.rustSmokeSessionPath).removeRecursively());
}

QTEST_MAIN(AppDataPathsTest)
#include "AppDataPathsTest.moc"
