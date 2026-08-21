#include "storage/AppDataPaths.h"
#include "storage/PortableMode.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

// Hermetic, platform-neutral coverage for portable mode.
//
// Everything here runs through the PURE helpers (markerPresentIn,
// dataRootFor, resolveAppDataBase, composeAppDataRoot) and the documented test
// seam. Nothing writes a portable.marker beside the TEST EXECUTABLE — that
// would leave a file in the build tree that silently turns every other binary
// in it portable if a run ever aborted, which is a far worse failure than a
// slightly indirect test.
class PortableModeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanup();

    void realDecisionIsCachedAndDoesNotFlipMidProcess();
    void markerFilePresenceIsTheOnlySignal();
    void dataRootComposesFromTheExecutableDirectory_data();
    void dataRootComposesFromTheExecutableDirectory();
    void namedSubdirectoriesAreAllUnderTheDataRoot();
    void installedModeExposesNoPortablePaths();
    void portableAndInstalledRootsAreNeverEqual();
    void resolveAppDataBasePortableRootBeatsEveryEnvironmentSource_data();
    void resolveAppDataBasePortableRootBeatsEveryEnvironmentSource();
    void appDataRootFollowsThePortableTree();
    void legacyRootsAreEmptyInPortableMode();
    void unresolvableExecutableDirectoryYieldsNoRootRatherThanAppData();
    void cacheRootFollowsThePortableTree();
    void prepareDataRootCreatesTheWholeTree();
    void everyScratchAndUpdatePathStaysInsideTheFolder();
    void prepareDataRootRefusesAnUnwritableFolderWithoutFallingBack();
    void relocationKeepsSettingsWithNoReferenceToTheOldRoot();

private:
    static bool copyTree(const QString &from, const QString &to);
    static QString grepForPath(const QString &dir, const QString &needle);

    QTemporaryDir m_home;
};

void PortableModeTest::initTestCase()
{
    QVERIFY(m_home.isValid());
    // Isolate EVERY variable the non-portable paths can reach. A previous test
    // in this project isolated only some of them and wrote into the
    // maintainer's real data directory; the cost of over-isolating is nil.
    const QByteArray home = m_home.path().toUtf8();
    qputenv("HOME", home);
    qputenv("XDG_DATA_HOME", home + "/xdg-data");
    qputenv("XDG_CONFIG_HOME", home + "/xdg-config");
    qputenv("XDG_CACHE_HOME", home + "/xdg-cache");
    qputenv("XDG_STATE_HOME", home + "/xdg-state");
    qputenv("XDG_RUNTIME_DIR", home + "/xdg-runtime");
    qputenv("LOCALAPPDATA", home + "/AppData/Local");
    qputenv("USERPROFILE", home);
    qputenv("APPDATA", home + "/AppData/Roaming");
    qunsetenv("LIGHTNING_PORTABLE");

    QCoreApplication::setOrganizationName(QStringLiteral("MatrixClient"));
    QCoreApplication::setApplicationName(QStringLiteral("matrix-client"));

    // Latch the REAL decision now, with no override and no environment
    // forcing, so the caching test below measures the genuine mechanism.
    // The test executable has no portable.marker beside it, so this is false.
    QVERIFY(!lightning::portable::isPortable());
}

void PortableModeTest::cleanup()
{
    lightning::portable::clearPortableOverrideForTest();
}

void PortableModeTest::realDecisionIsCachedAndDoesNotFlipMidProcess()
{
    // initTestCase() already forced the real decision. Turning the
    // development override ON afterwards must NOT change the answer: a
    // running process's storage location cannot be allowed to move, and a
    // marker file that appears (or a variable that is set) mid-run is exactly
    // the situation the cache exists to survive.
    qputenv("LIGHTNING_PORTABLE", "1");
    QVERIFY(!lightning::portable::isPortable());
    QVERIFY(lightning::portable::dataRoot().isEmpty());
    qunsetenv("LIGHTNING_PORTABLE");
    QVERIFY(!lightning::portable::isPortable());

    // The test seam, by contrast, is explicitly allowed to answer differently
    // — that is its whole purpose — and clearing it restores the real answer
    // rather than latching the fiction.
    lightning::portable::setPortableOverrideForTest(
        true, QStringLiteral("/opt/lightning"));
    QVERIFY(lightning::portable::isPortable());
    lightning::portable::clearPortableOverrideForTest();
    QVERIFY(!lightning::portable::isPortable());
}

void PortableModeTest::markerFilePresenceIsTheOnlySignal()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // Absent -> installed.
    QVERIFY(!lightning::portable::markerPresentIn(dir.path()));

    // Present -> portable.
    const QString marker = QDir(dir.path()).absoluteFilePath(
        QString::fromLatin1(lightning::portable::kMarkerFileName));
    QFile file(marker);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    QVERIFY(lightning::portable::markerPresentIn(dir.path()));

    // A DIRECTORY of that name is not the packaging pipeline's marker.
    // Treating it as one would relocate a normal installation's storage.
    QVERIFY(file.remove());
    QVERIFY(QDir(dir.path()).mkdir(
        QString::fromLatin1(lightning::portable::kMarkerFileName)));
    QVERIFY(!lightning::portable::markerPresentIn(dir.path()));

    // No directory at all is never portable — there is nowhere to anchor a
    // portable tree, and inventing one from the working directory would put
    // the user's session wherever the shortcut happened to point.
    QVERIFY(!lightning::portable::markerPresentIn(QString()));
    QVERIFY(!lightning::portable::markerPresentIn(QStringLiteral("   ")));
}

void PortableModeTest::dataRootComposesFromTheExecutableDirectory_data()
{
    QTest::addColumn<QString>("executableDir");
    QTest::addColumn<QString>("expected");

    QTest::newRow("plain")
        << QStringLiteral("/opt/lightning")
        << QStringLiteral("/opt/lightning/data");
    QTest::newRow("windows-drive")
        << QStringLiteral("C:/Lightning")
        << QStringLiteral("C:/Lightning/data");
    // A portable folder normally lives somewhere a human chose, so spaces and
    // non-ASCII are the common case, not the edge case.
    QTest::newRow("spaces")
        << QStringLiteral("D:/Portable Apps/Lightning 0.7.4")
        << QStringLiteral("D:/Portable Apps/Lightning 0.7.4/data");
    QTest::newRow("non-ascii")
        << QStringLiteral("E:/Žaibas/Žmogaus programos")
        << QStringLiteral("E:/Žaibas/Žmogaus programos/data");
    QTest::newRow("cjk")
        << QStringLiteral("/media/usb/闪电/便携版")
        << QStringLiteral("/media/usb/闪电/便携版/data");
    QTest::newRow("trailing-slash-normalised")
        << QStringLiteral("/opt/lightning/")
        << QStringLiteral("/opt/lightning/data");
    QTest::newRow("dot-segments-normalised")
        << QStringLiteral("/opt/lightning/bin/..")
        << QStringLiteral("/opt/lightning/data");

    // Unresolvable executable directory -> no data root. Never a fallback.
    QTest::newRow("empty") << QString() << QString();
    QTest::newRow("blank") << QStringLiteral("  ") << QString();
}

void PortableModeTest::dataRootComposesFromTheExecutableDirectory()
{
    QFETCH(QString, executableDir);
    QFETCH(QString, expected);
    QCOMPARE(lightning::portable::dataRootFor(executableDir), expected);
}

void PortableModeTest::namedSubdirectoriesAreAllUnderTheDataRoot()
{
    const QString exeDir = QStringLiteral("/media/usb/Lightning Portable");
    lightning::portable::setPortableOverrideForTest(true, exeDir);

    const QString root = lightning::portable::dataRoot();
    QCOMPARE(root, QStringLiteral("/media/usb/Lightning Portable/data"));
    QCOMPARE(lightning::portable::executableDir(), exeDir);

    const QStringList named = {
        lightning::portable::configDir(),
        lightning::portable::secretsDir(),
        lightning::portable::cacheDir(),
        lightning::portable::logsDir(),
    };
    QCOMPARE(named.at(0), root + QStringLiteral("/config"));
    QCOMPARE(named.at(1), root + QStringLiteral("/secrets"));
    QCOMPARE(named.at(2), root + QStringLiteral("/cache"));
    QCOMPARE(named.at(3), root + QStringLiteral("/logs"));
    for (const QString &dir : named) {
        QVERIFY2(dir.startsWith(root + QLatin1Char('/')),
                 qPrintable(QStringLiteral("escaped the data root: %1").arg(dir)));
    }
    // Distinct directories: a shared one would put the sealed secret document
    // in the same folder as evictable cache files.
    QCOMPARE(QSet<QString>(named.begin(), named.end()).size(), named.size());
}

void PortableModeTest::installedModeExposesNoPortablePaths()
{
    lightning::portable::setPortableOverrideForTest(
        false, QStringLiteral("/opt/lightning"));

    QVERIFY(!lightning::portable::isPortable());
    QVERIFY(lightning::portable::dataRoot().isEmpty());
    QVERIFY(lightning::portable::configDir().isEmpty());
    QVERIFY(lightning::portable::secretsDir().isEmpty());
    QVERIFY(lightning::portable::cacheDir().isEmpty());
    QVERIFY(lightning::portable::logsDir().isEmpty());

    // And the normal locations still resolve exactly as before.
    const QString root = matrix::app_data::primaryRoot();
    QVERIFY(!root.isEmpty());
    QVERIFY2(root.startsWith(m_home.path()),
             qPrintable(QStringLiteral("installed root escaped the test home: %1")
                            .arg(root)));
    QCOMPARE(root, m_home.path()
                       + QStringLiteral("/xdg-data/MatrixClient/matrix-client"));
}

void PortableModeTest::portableAndInstalledRootsAreNeverEqual()
{
    lightning::portable::setPortableOverrideForTest(
        false, QStringLiteral("/opt/lightning"));
    const QString installed = matrix::app_data::primaryRoot();
    QVERIFY(!installed.isEmpty());

    lightning::portable::setPortableOverrideForTest(
        true, QStringLiteral("/opt/lightning"));
    const QString portable = matrix::app_data::primaryRoot();
    QCOMPARE(portable, QStringLiteral("/opt/lightning/data/matrix"));

    QVERIFY2(portable != installed,
             "a portable install that resolved to the installed root would "
             "share the crypto store with an installed copy");
}

void PortableModeTest::resolveAppDataBasePortableRootBeatsEveryEnvironmentSource_data()
{
    QTest::addColumn<bool>("windows");
    QTest::addColumn<QString>("xdg");
    QTest::addColumn<QString>("localAppData");
    QTest::addColumn<QString>("userProfile");
    QTest::addColumn<QString>("home");
    QTest::addColumn<QString>("portableRoot");
    QTest::addColumn<QString>("expected");

    QTest::newRow("beats-xdg-linux")
        << false << QStringLiteral("/xdg/data") << QString() << QString()
        << QStringLiteral("/home/x") << QStringLiteral("/opt/lightning/data")
        << QStringLiteral("/opt/lightning/data");
    QTest::newRow("beats-localappdata")
        << true << QString() << QStringLiteral("C:/Users/X/AppData/Local")
        << QStringLiteral("C:/Users/X") << QString()
        << QStringLiteral("D:/Lightning/data")
        << QStringLiteral("D:/Lightning/data");
    QTest::newRow("beats-everything-at-once")
        << true << QStringLiteral("D:/xdg")
        << QStringLiteral("C:/Users/X/AppData/Local")
        << QStringLiteral("C:/Users/X") << QStringLiteral("/home/x")
        << QStringLiteral("E:/Portable Apps/Lightning/data")
        << QStringLiteral("E:/Portable Apps/Lightning/data");
    // An empty portable root is "not portable", and must leave the existing
    // precedence completely untouched.
    QTest::newRow("empty-portable-root-changes-nothing")
        << false << QStringLiteral("/xdg/data") << QString() << QString()
        << QStringLiteral("/home/x") << QString() << QStringLiteral("/xdg/data");
}

void PortableModeTest::resolveAppDataBasePortableRootBeatsEveryEnvironmentSource()
{
    QFETCH(bool, windows);
    QFETCH(QString, xdg);
    QFETCH(QString, localAppData);
    QFETCH(QString, userProfile);
    QFETCH(QString, home);
    QFETCH(QString, portableRoot);
    QFETCH(QString, expected);
    QCOMPARE(matrix::app_data::resolveAppDataBase(windows, xdg, localAppData,
                                                  userProfile, home,
                                                  portableRoot),
             expected);
}

void PortableModeTest::appDataRootFollowsThePortableTree()
{
    lightning::portable::setPortableOverrideForTest(
        true, QStringLiteral("E:/Portable Apps/Lightning"));

    const QString root = matrix::app_data::primaryRoot();
    QCOMPARE(root, QStringLiteral("E:/Portable Apps/Lightning/data/matrix"));

    // The account layout beneath it is unchanged — that is what keeps a moved
    // folder resolving the same recorded storeSlug, and therefore the same
    // Matrix device.
    QCOMPARE(matrix::app_data::accountRoot(QStringLiteral("@rokas:example.org")),
             root + QStringLiteral("/rokas_example.org"));
    QCOMPARE(matrix::app_data::rustSdkStorePath(QStringLiteral("@rokas:example.org")),
             root + QStringLiteral("/rokas_example.org/matrix-rust-sdk-store"));

    // No environment path may appear anywhere in the result.
    QVERIFY(!root.contains(m_home.path()));
}

void PortableModeTest::legacyRootsAreEmptyInPortableMode()
{
    lightning::portable::setPortableOverrideForTest(
        true, QStringLiteral("/opt/lightning"));
    // <dataRoot>/matrix-client never existed: the portable tree is created by
    // this feature, so no earlier build wrote into it. Handing an invented
    // directory to --reset-crypto-store's recursive scan is not acceptable.
    QVERIFY(matrix::app_data::legacyRoots().isEmpty());
    QCOMPARE(matrix::app_data::allRoots(),
             QStringList{QStringLiteral("/opt/lightning/data/matrix")});
}

void PortableModeTest::unresolvableExecutableDirectoryYieldsNoRootRatherThanAppData()
{
    // Portable, but the program directory could not be resolved (an empty
    // GetModuleFileNameW / readlink result). The answer is "no root" — never a
    // quiet fall-through to the environment, which would put the SDK store
    // outside the folder the user copies and reintroduce the very bug portable
    // mode fixes. main.cpp exits non-zero on this; nothing may paper over it.
    lightning::portable::setPortableOverrideForTest(true, QString());

    QVERIFY(lightning::portable::isPortable());
    QVERIFY(lightning::portable::dataRoot().isEmpty());
    QVERIFY(matrix::app_data::primaryRoot().isEmpty());
    QVERIFY(matrix::app_data::allRoots().isEmpty());
    QVERIFY(matrix::app_data::accountRoot(QStringLiteral("@rokas:example.org"))
                .isEmpty());
    QVERIFY(!lightning::portable::prepareDataRoot().isEmpty());
}

void PortableModeTest::cacheRootFollowsThePortableTree()
{
    lightning::portable::setPortableOverrideForTest(
        true, QStringLiteral("/opt/lightning"));
    QCOMPARE(matrix::app_data::cacheRoot(),
             QStringLiteral("/opt/lightning/data/cache"));

    lightning::portable::setPortableOverrideForTest(
        false, QStringLiteral("/opt/lightning"));
    const QString installed = matrix::app_data::cacheRoot();
    QCOMPARE(installed,
             QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    QVERIFY(!installed.startsWith(QStringLiteral("/opt/lightning")));
}

void PortableModeTest::prepareDataRootCreatesTheWholeTree()
{
    QTemporaryDir exeDir;
    QVERIFY(exeDir.isValid());
    lightning::portable::setPortableOverrideForTest(true, exeDir.path());

    QCOMPARE(lightning::portable::prepareDataRoot(), QString());

    const QString root = lightning::portable::dataRoot();
    QVERIFY(QFileInfo(root).isDir());
    for (const QString &sub : {QStringLiteral("config"),
                               QStringLiteral("secrets"),
                               QStringLiteral("cache"),
                               QStringLiteral("logs"),
                               QStringLiteral("matrix"),
                               QStringLiteral("temp")}) {
        QVERIFY2(QFileInfo(root + QLatin1Char('/') + sub).isDir(),
                 qPrintable(sub));
    }
    // matrix/ is exactly what primaryRoot() resolves to, so the SDK store has
    // its parent already in place.
    QCOMPARE(matrix::app_data::primaryRoot(), root + QStringLiteral("/matrix"));

    // Idempotent: a second launch from the same folder must not fail.
    QCOMPARE(lightning::portable::prepareDataRoot(), QString());

    // The write probe cleans up after itself — a portable folder that
    // accumulated a probe file per launch would look broken to the user.
    const QStringList leftovers =
        QDir(root).entryList({QStringLiteral(".write-probe-*")},
                             QDir::Files | QDir::Hidden);
    QVERIFY2(leftovers.isEmpty(), qPrintable(leftovers.join(QLatin1Char(','))));

    // Calling it while NOT portable is a programming error, and it says so
    // rather than quietly creating a tree somewhere.
    lightning::portable::setPortableOverrideForTest(false, exeDir.path());
    QVERIFY(!lightning::portable::prepareDataRoot().isEmpty());
}

// 2026-08-21: an external audit found three paths that still left the folder,
// which is the one promise portable mode makes. Two were scratch
// (QDir::tempPath() for voice recordings and animated media), and the third
// was the UPDATER — it staged the new version and the displaced old one in
// the PARENT of the portable folder, so a successful update on a USB stick
// left `lightning-previous-version` sitting beside `Lightning\` and required
// the parent to be writable.
//
// This pins the contract as PATHS, which is what the audit actually checked.
void PortableModeTest::everyScratchAndUpdatePathStaysInsideTheFolder()
{
    QTemporaryDir exeDir;
    QVERIFY(exeDir.isValid());
    lightning::portable::setPortableOverrideForTest(true, exeDir.path());
    QCOMPARE(lightning::portable::prepareDataRoot(), QString());

    const QString root = QDir(lightning::portable::dataRoot()).canonicalPath();
    QVERIFY(!root.isEmpty());

    // Everything Lightning chooses to write lives under data/.
    for (const QString &path : {lightning::portable::configDir(),
                                lightning::portable::secretsDir(),
                                lightning::portable::cacheDir(),
                                lightning::portable::logsDir(),
                                lightning::portable::tempDir(),
                                lightning::portable::updateWorkDir(),
                                lightning::portable::mediaScratchRoot()}) {
        QVERIFY2(!path.isEmpty(), "a portable path resolved empty");
        QVERIFY2(QDir::cleanPath(path).startsWith(root),
                 qPrintable(QStringLiteral("escapes the folder: %1 (root %2)")
                                .arg(path, root)));
    }

    // The media scratch root IS the portable temp dir — not the OS one.
    QCOMPARE(lightning::portable::mediaScratchRoot(),
             lightning::portable::tempDir());
    // ...and NOT the OS temp directory, which is where it used to go. (The
    // containment loop above is the real guarantee; this names the specific
    // regression so a failure reads as what it is. Compared against the bare
    // path, since this fixture's own folder happens to live under /tmp.)
    QVERIFY2(lightning::portable::mediaScratchRoot() != QDir::tempPath(),
             "decrypted media scratch is still the OS temp directory");

    // Not portable: the scratch root goes back to the OS temp directory, so
    // an installed build is completely unaffected by any of this.
    lightning::portable::setPortableOverrideForTest(false, exeDir.path());
    QCOMPARE(lightning::portable::mediaScratchRoot(), QDir::tempPath());
    QVERIFY(lightning::portable::tempDir().isEmpty());
    QVERIFY(lightning::portable::updateWorkDir().isEmpty());

    // Stale scratch sweeping: ours by prefix, and NOTHING else. This runs
    // unattended at startup over a directory the user owns.
    lightning::portable::setPortableOverrideForTest(true, exeDir.path());
    const QString temp = lightning::portable::tempDir();
    QVERIFY(QDir().mkpath(temp + QStringLiteral("/lightning-voice-abc")));
    QVERIFY(QDir().mkpath(temp + QStringLiteral("/lightning-animated-xyz")));
    QVERIFY(QDir().mkpath(temp + QStringLiteral("/somebody-elses-data")));
    QFile keep(temp + QStringLiteral("/notes.txt"));
    QVERIFY(keep.open(QIODevice::WriteOnly));
    keep.write("keep me");
    keep.close();

    QCOMPARE(lightning::portable::cleanStaleTempDirs(), 2);
    QVERIFY(!QFileInfo::exists(temp + QStringLiteral("/lightning-voice-abc")));
    QVERIFY(!QFileInfo::exists(temp + QStringLiteral("/lightning-animated-xyz")));
    QVERIFY2(QFileInfo(temp + QStringLiteral("/somebody-elses-data")).isDir(),
             "the sweep removed a directory it did not create");
    QVERIFY2(QFileInfo::exists(temp + QStringLiteral("/notes.txt")),
             "the sweep removed a file");
}

void PortableModeTest::prepareDataRootRefusesAnUnwritableFolderWithoutFallingBack()
{
#ifndef Q_OS_UNIX
    QSKIP("needs POSIX permission bits to make a directory unwritable");
#else
    if (::geteuid() == 0)
        QSKIP("running as root: permission bits do not deny writes");

    QTemporaryDir exeDir;
    QVERIFY(exeDir.isValid());
    // r-x only: mkpath of <exeDir>/data cannot succeed.
    QVERIFY(QFile::setPermissions(exeDir.path(),
                                  QFileDevice::ReadOwner
                                      | QFileDevice::ExeOwner));

    lightning::portable::setPortableOverrideForTest(true, exeDir.path());
    const QString reason = lightning::portable::prepareDataRoot();

    // Restore before any assertion can abort the test and leave an
    // undeletable temporary directory behind.
    QVERIFY(QFile::setPermissions(exeDir.path(),
                                  QFileDevice::ReadOwner
                                      | QFileDevice::WriteOwner
                                      | QFileDevice::ExeOwner));

    QVERIFY2(!reason.isEmpty(), "an unwritable folder must be reported");
    QVERIFY(reason.contains(QStringLiteral("Lightning portable")));

    // The load-bearing assertion: it did NOT fall back. Portable mode is still
    // on, the data root is still inside the (unwritable) program folder, and
    // no AppData / XDG / registry-shaped location was produced.
    QVERIFY(lightning::portable::isPortable());
    QCOMPARE(lightning::portable::dataRoot(),
             QDir::cleanPath(exeDir.path()) + QStringLiteral("/data"));
    const QString root = matrix::app_data::primaryRoot();
    QVERIFY(root.startsWith(QDir::cleanPath(exeDir.path())));
    QVERIFY(!root.contains(m_home.path()));
    QVERIFY(!root.contains(QStringLiteral("AppData")));
    QVERIFY(!root.contains(QStringLiteral("xdg-data")));
#endif
}

void PortableModeTest::relocationKeepsSettingsWithNoReferenceToTheOldRoot()
{
    QTemporaryDir rootA;
    QTemporaryDir rootB;
    QVERIFY(rootA.isValid());
    QVERIFY(rootB.isValid());
    const QString dirA = QDir(rootA.path()).absoluteFilePath(
        QStringLiteral("Lightning Portable"));
    const QString dirB = QDir(rootB.path()).absoluteFilePath(
        QStringLiteral("Žaibas nešiojamas"));
    QVERIFY(QDir().mkpath(dirA));

    // ── Machine A: run portable, write a setting through the same mechanism
    // main.cpp installs (INI format + an explicit UserScope path).
    lightning::portable::setPortableOverrideForTest(true, dirA);
    QCOMPARE(lightning::portable::prepareDataRoot(), QString());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       lightning::portable::configDir());
    {
        QSettings settings;
        settings.setValue(QStringLiteral("accounts/rokas_example.org/userId"),
                          QStringLiteral("@rokas:example.org"));
        settings.setValue(QStringLiteral("accounts/rokas_example.org/storeSlug"),
                          QStringLiteral("rokas_example.org"));
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
    }
    const QString accountRootA =
        matrix::app_data::accountRoot(QStringLiteral("@rokas:example.org"));
    QVERIFY(QDir().mkpath(accountRootA
                          + QStringLiteral("/matrix-rust-sdk-store")));
    QVERIFY(QFileInfo(lightning::portable::configDir()
                      + QStringLiteral("/MatrixClient/matrix-client.ini"))
                .isFile());

    // ── Copy ONLY that directory to a different path (a different name, on a
    // different parent) — the user's "copy the folder to another PC" step.
    QVERIFY(copyTree(dirA, dirB));

    // ── Machine B: nothing may retain dirA.
    lightning::portable::setPortableOverrideForTest(true, dirB);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       lightning::portable::configDir());
    {
        QSettings settings;
        QCOMPARE(settings.value(
                     QStringLiteral("accounts/rokas_example.org/userId"))
                     .toString(),
                 QStringLiteral("@rokas:example.org"));
        // The RECORDED store slug is what keeps restore/logout/reset pointed
        // at one directory. It must survive the move verbatim — re-deriving it
        // is exactly the bug that once deleted the wrong account's store.
        QCOMPARE(settings.value(
                     QStringLiteral("accounts/rokas_example.org/storeSlug"))
                     .toString(),
                 QStringLiteral("rokas_example.org"));
    }

    const QString accountRootB =
        matrix::app_data::accountRoot(QStringLiteral("@rokas:example.org"));
    QVERIFY(accountRootB.startsWith(QDir::cleanPath(dirB)));
    QVERIFY(!accountRootB.contains(QDir::cleanPath(dirA)));
    // Same Matrix device after the move means the same store directory, found
    // by the same relative layout.
    QVERIFY(QFileInfo(accountRootB + QStringLiteral("/matrix-rust-sdk-store"))
                .isDir());
    QCOMPARE(accountRootB.mid(QDir::cleanPath(dirB).size()),
             accountRootA.mid(QDir::cleanPath(dirA).size()));

    // And nothing WRITTEN anywhere in the tree records the old absolute path.
    const QString offender = grepForPath(dirB, QDir::cleanPath(dirA));
    QVERIFY2(offender.isEmpty(),
             qPrintable(QStringLiteral("file records the old root: %1")
                            .arg(offender)));
}

bool PortableModeTest::copyTree(const QString &from, const QString &to)
{
    QDir source(from);
    if (!source.exists() || !QDir().mkpath(to))
        return false;
    const auto entries = source.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        const QString target = QDir(to).absoluteFilePath(entry.fileName());
        if (entry.isDir()) {
            if (!copyTree(entry.absoluteFilePath(), target))
                return false;
        } else if (!QFile::copy(entry.absoluteFilePath(), target)) {
            return false;
        }
    }
    return true;
}

QString PortableModeTest::grepForPath(const QString &dir, const QString &needle)
{
    QDirIterator it(dir, QDir::Files | QDir::Hidden,
                    QDirIterator::Subdirectories);
    const QByteArray raw = needle.toUtf8();
    while (it.hasNext()) {
        const QString path = it.next();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        if (file.readAll().contains(raw))
            return path;
    }
    return {};
}

QTEST_MAIN(PortableModeTest)
#include "PortableModeTest.moc"
