
#include "updater/AtomicReplace.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#ifndef Q_OS_WIN
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace updater;

namespace {

const QString kExe = QStringLiteral("Lightning.exe");

bool writeFile(const QString &path, const QByteArray &contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return file.write(contents) == contents.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

bool buildInstallation(const QString &root, const QByteArray &marker)
{
    return writeFile(QDir(root).absoluteFilePath(kExe), marker)
        && writeFile(QDir(root).absoluteFilePath(QStringLiteral("qt.conf")),
                     marker + QByteArray("-conf"))
        && writeFile(QDir(root).absoluteFilePath(
                         QStringLiteral("plugins/platforms/qwindows.dll")),
                     marker + QByteArray("-dll"));
}

bool installationHasMarker(const QString &root, const QByteArray &marker)
{
    const QDir dir(root);
    return readFile(dir.absoluteFilePath(kExe)) == marker
        && readFile(dir.absoluteFilePath(QStringLiteral("qt.conf")))
               == marker + QByteArray("-conf")
        && readFile(dir.absoluteFilePath(
               QStringLiteral("plugins/platforms/qwindows.dll")))
               == marker + QByteArray("-dll");
}

}

class UpdaterPortableSwapTest : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void resolvesAFlatStagedTree();
    void resolvesASingleTopLevelFolder();
    void refusesAnAmbiguousStagedTree();
    void refusesAStagedTreeWithoutTheExecutable();

    void swapsTheInstallationAtomically();
    void preservedPortableDataSurvivesTheSwap();
    void preservedPortableDataIsNeverPromotedOver();
    void swapFromASingleTopLevelFolder();
    void refusesAnInvalidLayoutBeforeTouchingTheTarget();
    void rollsBackWhenPromotionFails();
    void rollsBackWhenTheBackupRenameFails();
    void refusesANonWritableTargetWithADistinctError();
    void keepsTheInstallationDirectoryItself();
    void clearsAStaleBackupDirectoryAndProceeds();
    void refusesABackupPathThatIsAFile();
    void refusesABackupPathInsideTheTarget();
    void theBackupPathTheHelperUsesIsAcceptedBySwapDirectory();
    void aFailedPromoteRollsBackWithoutTouchingPreservedState();
    void copyFallbackStillSwapsAndRollsBack();

    void replacesAFileAndPreservesTheExecutableBit();
    void fileReplaceRollsBackOnPromotionFailure();
    void fileReplaceRefusesAMissingTarget();
    void fileReplaceRefusesAMissingSource();
    void fileReplaceUsesTheCopyFallbackWhenRenameCannotBeUsed();

private:
    QTemporaryDir m_dir;
    QString m_staged;
    QString m_target;
    QString m_backup;
};

void UpdaterPortableSwapTest::init()
{
    QVERIFY(m_dir.isValid());
    const QDir root(m_dir.path());
    m_staged = root.absoluteFilePath(QStringLiteral("staged"));
    m_target = root.absoluteFilePath(QStringLiteral("Lightning"));
    m_backup = root.absoluteFilePath(QStringLiteral("Lightning.previous"));

    QDir(m_staged).removeRecursively();
    QDir(m_target).removeRecursively();
    QDir(m_backup).removeRecursively();
    QFile::remove(m_backup);
}

void UpdaterPortableSwapTest::resolvesAFlatStagedTree()
{
    QVERIFY(buildInstallation(m_staged, QByteArray("new")));
    QCOMPARE(resolveStagedRoot(m_staged, kExe),
             QDir(m_staged).absolutePath());
}

void UpdaterPortableSwapTest::resolvesASingleTopLevelFolder()
{
    const QString inner =
        QDir(m_staged).absoluteFilePath(QStringLiteral("Lightning-0.8.0"));
    QVERIFY(buildInstallation(inner, QByteArray("new")));
    QCOMPARE(resolveStagedRoot(m_staged, kExe), QDir(inner).absolutePath());
}

void UpdaterPortableSwapTest::refusesAnAmbiguousStagedTree()
{
    QVERIFY(buildInstallation(QDir(m_staged).absoluteFilePath(QStringLiteral("a")),
                              QByteArray("a")));
    QVERIFY(buildInstallation(QDir(m_staged).absoluteFilePath(QStringLiteral("b")),
                              QByteArray("b")));
    QVERIFY(resolveStagedRoot(m_staged, kExe).isEmpty());
}

void UpdaterPortableSwapTest::refusesAStagedTreeWithoutTheExecutable()
{
    QVERIFY(writeFile(QDir(m_staged).absoluteFilePath(QStringLiteral("README.txt")),
                      QByteArray("nothing here")));
    QVERIFY(resolveStagedRoot(m_staged, kExe).isEmpty());
}

void UpdaterPortableSwapTest::swapsTheInstallationAtomically()
{
    QVERIFY(buildInstallation(m_staged, QByteArray("new")));
    QVERIFY(buildInstallation(m_target, QByteArray("old")));

    const ReplaceResult result = swapDirectory(m_staged, m_target, m_backup, kExe);
    QVERIFY2(result.ok(), replaceErrorName(result.error));
    QVERIFY(installationHasMarker(m_target, QByteArray("new")));
    QVERIFY(!installationHasMarker(m_target, QByteArray("old")));
    QVERIFY(!QFileInfo::exists(m_backup));
    QVERIFY(QFileInfo::exists(m_dir.path()));
}

void UpdaterPortableSwapTest::preservedPortableDataSurvivesTheSwap()
{
    QVERIFY(buildInstallation(m_staged, QByteArray("new")));
    QVERIFY(buildInstallation(m_target, QByteArray("old")));

    const QString secrets =
        QDir(m_target).absoluteFilePath(QStringLiteral("data/secrets"));
    QVERIFY(QDir().mkpath(secrets));
    const QString keyPath =
        QDir(secrets).absoluteFilePath(QStringLiteral("secrets.key"));
    const QByteArray keyBytes("sealed-session-key-bytes");
    QVERIFY(writeFile(keyPath, keyBytes));
    const QString storePath = QDir(m_target).absoluteFilePath(
        QStringLiteral("data/matrix/alice_example.org/store.sqlite"));
    QVERIFY(QDir().mkpath(QFileInfo(storePath).absolutePath()));
    QVERIFY(writeFile(storePath, QByteArray("crypto-store")));

    const ReplaceResult result =
        swapDirectory(m_staged, m_target, m_backup, kExe,
                      QStringList{QStringLiteral("data")});
    QVERIFY2(result.ok(), replaceErrorName(result.error));

    QVERIFY(installationHasMarker(m_target, QByteArray("new")));
    QFile key(keyPath);
    QVERIFY2(key.exists(), "the portable secret key was destroyed by the swap");
    QVERIFY(key.open(QIODevice::ReadOnly));
    QCOMPARE(key.readAll(), keyBytes);
    QVERIFY2(QFileInfo::exists(storePath),
             "the portable Matrix/crypto store was destroyed by the swap");

    // It must never have entered the backup either — step 4 deletes that, and
    // on Windows a surviving backup is cleared by the NEXT update.
    QVERIFY(!QFileInfo::exists(
        QDir(m_backup).absoluteFilePath(QStringLiteral("data"))));
}

void UpdaterPortableSwapTest::preservedPortableDataIsNeverPromotedOver()
{
    QVERIFY(buildInstallation(m_staged, QByteArray("new")));
    QVERIFY(writeFile(QDir(m_staged).absoluteFilePath(
                          QStringLiteral("data/config/settings.ini")),
                      QByteArray("shipped-by-mistake")));
    QVERIFY(buildInstallation(m_target, QByteArray("old")));
    const QString live = QDir(m_target).absoluteFilePath(
        QStringLiteral("data/config/settings.ini"));
    QVERIFY(QDir().mkpath(QFileInfo(live).absolutePath()));
    QVERIFY(writeFile(live, QByteArray("the-users-real-settings")));

    const ReplaceResult result =
        swapDirectory(m_staged, m_target, m_backup, kExe,
                      QStringList{QStringLiteral("data")});
    QVERIFY2(result.ok(), replaceErrorName(result.error));

    QFile settings(live);
    QVERIFY(settings.open(QIODevice::ReadOnly));
    QCOMPARE(settings.readAll(), QByteArray("the-users-real-settings"));
}

void UpdaterPortableSwapTest::swapFromASingleTopLevelFolder()
{
    const QString inner =
        QDir(m_staged).absoluteFilePath(QStringLiteral("Lightning-0.8.0"));
    QVERIFY(buildInstallation(inner, QByteArray("new")));
    QVERIFY(buildInstallation(m_target, QByteArray("old")));

    const ReplaceResult result = swapDirectory(m_staged, m_target, m_backup, kExe);
    QVERIFY2(result.ok(), replaceErrorName(result.error));
    QVERIFY(installationHasMarker(m_target, QByteArray("new")));
    QVERIFY(!QFileInfo::exists(
        QDir(m_target).absoluteFilePath(QStringLiteral("Lightning-0.8.0"))));
}

void UpdaterPortableSwapTest::refusesAnInvalidLayoutBeforeTouchingTheTarget()
{
    QVERIFY(writeFile(QDir(m_staged).absoluteFilePath(QStringLiteral("README.txt")),
                      QByteArray("not an installation")));
    QVERIFY(buildInstallation(m_target, QByteArray("old")));

    const ReplaceResult result = swapDirectory(m_staged, m_target, m_backup, kExe);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, ReplaceError::LayoutInvalid);
    QVERIFY(installationHasMarker(m_target, QByteArray("old")));
    QVERIFY(!QFileInfo::exists(m_backup));
}

void UpdaterPortableSwapTest::rollsBackWhenPromotionFails()
{
    QVERIFY(buildInstallation(m_staged, QByteArray("new")));
    QVERIFY(buildInstallation(m_target, QByteArray("old")));

    ReplaceHooks hooks;
    hooks.beforePromoteRename = [] { return false; };

    const ReplaceResult result =
        swapDirectory(m_staged, m_target, m_backup, kExe, QStringList(), hooks);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, ReplaceError::PromoteFailed);
    QVERIFY(result.rolledBack);

    QVERIFY(installationHasMarker(m_target, QByteArray("old")));
    QVERIFY(!QFileInfo::exists(m_backup));
    const QStringList leftovers =
        QDir(m_dir.path()).entryList(QStringList(QStringLiteral(".lightning-new-*")),
                                     QDir::Dirs | QDir::Hidden);
    QVERIFY2(leftovers.isEmpty(), qPrintable(leftovers.join(QLatin1Char(','))));
}

void UpdaterPortableSwapTest::rollsBackWhenTheBackupRenameFails()
{
    QVERIFY(buildInstallation(m_staged, QByteArray("new")));
    QVERIFY(buildInstallation(m_target, QByteArray("old")));

    ReplaceHooks hooks;
    hooks.beforeBackupRename = [] { return false; };

    const ReplaceResult result =
        swapDirectory(m_staged, m_target, m_backup, kExe, QStringList(), hooks);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, ReplaceError::BackupFailed);
    QVERIFY(result.rolledBack);
    QVERIFY(installationHasMarker(m_target, QByteArray("old")));
    QVERIFY(!QFileInfo::exists(m_backup));
}

void UpdaterPortableSwapTest::refusesANonWritableTargetWithADistinctError()
{
#ifdef Q_OS_WIN
    QSKIP("POSIX directory permissions are not the Windows write model");
#else
    if (::geteuid() == 0)
        QSKIP("running as root: directory permissions do not deny writes");

    QVERIFY(buildInstallation(m_staged, QByteArray("new")));
    QVERIFY(buildInstallation(m_target, QByteArray("old")));

    QVERIFY(QFile::setPermissions(m_target,
                                  QFile::ReadOwner | QFile::ExeOwner
                                      | QFile::ReadUser | QFile::ExeUser));

    const ReplaceResult result = swapDirectory(m_staged, m_target, m_backup, kExe);

    QVERIFY(QFile::setPermissions(m_target,
                                  QFile::ReadOwner | QFile::WriteOwner
                                      | QFile::ExeOwner | QFile::ReadUser
                                      | QFile::WriteUser | QFile::ExeUser));

    QVERIFY(!result.ok());
    QCOMPARE(result.error, ReplaceError::TargetNotWritable);
    QVERIFY(installationHasMarker(m_target, QByteArray("old")));
    QVERIFY(!QFileInfo::exists(m_backup));
#endif
}

// The installation directory itself survives the swap as the same directory
// (checked by inode): contents move out and in. Windows refuses to rename a
// directory while the running helper and its DLLs are held inside it.
void UpdaterPortableSwapTest::keepsTheInstallationDirectoryItself()
{
#ifndef Q_OS_WIN
    QVERIFY(buildInstallation(m_staged, QByteArray("new")));
    QVERIFY(buildInstallation(m_target, QByteArray("old")));

    struct stat before {};
    QCOMPARE(::stat(QFile::encodeName(m_target).constData(), &before), 0);

    const ReplaceResult result = swapDirectory(m_staged, m_target, m_backup, kExe);
    QVERIFY(result.ok());
    QVERIFY(installationHasMarker(m_target, QByteArray("new")));

    struct stat after {};
    QCOMPARE(::stat(QFile::encodeName(m_target).constData(), &after), 0);
    QCOMPARE(after.st_ino, before.st_ino);
    QCOMPARE(after.st_dev, before.st_dev);
#else
    QSKIP("inode identity is checked on the POSIX host that runs the suite");
#endif
}

// A leftover backup directory is cleared and the swap proceeds. On Windows
// the backup holding the still-mapped helper survives the run that created
// it, so refusing here would make every later update fail.
void UpdaterPortableSwapTest::clearsAStaleBackupDirectoryAndProceeds()
{
    QVERIFY(buildInstallation(m_staged, QByteArray("new")));
    QVERIFY(buildInstallation(m_target, QByteArray("old")));
    QVERIFY(QDir().mkpath(m_backup));
    QFile stale(QDir(m_backup).absoluteFilePath(QStringLiteral("stale.txt")));
    QVERIFY(stale.open(QIODevice::WriteOnly));
    stale.write("left over from the previous update");
    stale.close();

    const ReplaceResult result = swapDirectory(m_staged, m_target, m_backup, kExe);
    QVERIFY(result.ok());
    QVERIFY(installationHasMarker(m_target, QByteArray("new")));
    QVERIFY(!QFileInfo::exists(
        QDir(m_backup).absoluteFilePath(QStringLiteral("stale.txt"))));
}

void UpdaterPortableSwapTest::refusesABackupPathThatIsAFile()
{
    QVERIFY(buildInstallation(m_staged, QByteArray("new")));
    QVERIFY(buildInstallation(m_target, QByteArray("old")));
    QFile blocker(m_backup);
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.write("not a directory");
    blocker.close();

    const ReplaceResult result = swapDirectory(m_staged, m_target, m_backup, kExe);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, ReplaceError::BackupPathUnusable);
    QVERIFY(installationHasMarker(m_target, QByteArray("old")));
}

void UpdaterPortableSwapTest::aFailedPromoteRollsBackWithoutTouchingPreservedState()
{
    QVERIFY(buildInstallation(m_staged, QByteArray("new")));
    QVERIFY(buildInstallation(m_target, QByteArray("old")));
    const QString dataDir = m_target + QStringLiteral("/data");
    QVERIFY(QDir().mkpath(dataDir));
    QFile session(dataDir + QStringLiteral("/session.json"));
    QVERIFY(session.open(QIODevice::WriteOnly));
    session.write(QByteArray("sealed-session"));
    session.close();
    QVERIFY(QDir().mkpath(dataDir + QStringLiteral("/crypto")));
    QFile keys(dataDir + QStringLiteral("/crypto/megolm.db"));
    QVERIFY(keys.open(QIODevice::WriteOnly));
    keys.write(QByteArray("device-keys"));
    keys.close();

    updater::ReplaceHooks hooks;
    hooks.beforePromoteRename = [] { return false; };
    const updater::ReplaceResult result = updater::swapDirectory(
        m_staged, m_target, updater::portableBackupPath(m_target),
        QStringLiteral("Lightning.exe"),
        QStringList{ QStringLiteral("data") }, hooks);
    QVERIFY2(!result.ok(), "the promote was supposed to fail");
    QVERIFY2(result.error != updater::ReplaceError::RollbackFailed,
             qPrintable(QStringLiteral("rollback reported failure: %1")
                            .arg(result.message)));

    QFile restored(m_target + QStringLiteral("/Lightning.exe"));
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), QByteArray("old"));
    restored.close();

    QFile survivedSession(dataDir + QStringLiteral("/session.json"));
    QVERIFY2(survivedSession.exists(),
             "the rollback deleted the portable session");
    QVERIFY(survivedSession.open(QIODevice::ReadOnly));
    QCOMPARE(survivedSession.readAll(), QByteArray("sealed-session"));
    QFile survivedKeys(dataDir + QStringLiteral("/crypto/megolm.db"));
    QVERIFY2(survivedKeys.exists(),
             "the rollback deleted the portable crypto store");
    QVERIFY(survivedKeys.open(QIODevice::ReadOnly));
    QCOMPARE(survivedKeys.readAll(), QByteArray("device-keys"));
}

void UpdaterPortableSwapTest::theBackupPathTheHelperUsesIsAcceptedBySwapDirectory()
{
    QVERIFY(buildInstallation(m_staged, QByteArray("new")));
    QVERIFY(buildInstallation(m_target, QByteArray("old")));
    const QString dataDir = m_target + QStringLiteral("/data");
    QVERIFY(QDir().mkpath(dataDir + QStringLiteral("/update-work")));
    QFile keep(dataDir + QStringLiteral("/session.json"));
    QVERIFY(keep.open(QIODevice::WriteOnly));
    keep.write(QByteArray("keep-me"));
    keep.close();

    const QString backup = updater::portableBackupPath(m_target);
    QVERIFY2(!backup.isEmpty(), "the helper must produce a backup path");
    const QString cleanTarget = QDir::cleanPath(QDir(m_target).absolutePath());
    QVERIFY2(!backup.startsWith(cleanTarget + QLatin1Char('/')),
             qPrintable(QStringLiteral("backup sits inside the target: %1")
                            .arg(backup)));
    QCOMPARE(QFileInfo(backup).absolutePath(),
             QFileInfo(cleanTarget).absolutePath());

    const updater::ReplaceResult result = updater::swapDirectory(
        m_staged, m_target, backup, QStringLiteral("Lightning.exe"),
        QStringList{ QStringLiteral("data") });
    QVERIFY2(result.ok(), qPrintable(result.message));

    QFile promoted(m_target + QStringLiteral("/Lightning.exe"));
    QVERIFY(promoted.open(QIODevice::ReadOnly));
    QCOMPARE(promoted.readAll(), QByteArray("new"));
    promoted.close();
    QFile survived(dataDir + QStringLiteral("/session.json"));
    QVERIFY2(survived.exists(), "the portable data directory was not preserved");
    QVERIFY(survived.open(QIODevice::ReadOnly));
    QCOMPARE(survived.readAll(), QByteArray("keep-me"));
}

void UpdaterPortableSwapTest::refusesABackupPathInsideTheTarget()
{
    QVERIFY(buildInstallation(m_staged, QByteArray("new")));
    QVERIFY(buildInstallation(m_target, QByteArray("old")));

    const QString inside =
        QDir(m_target).absoluteFilePath(QStringLiteral("previous"));
    const ReplaceResult result = swapDirectory(m_staged, m_target, inside, kExe);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, ReplaceError::RefusedUnsafePath);
    QVERIFY(installationHasMarker(m_target, QByteArray("old")));
}

void UpdaterPortableSwapTest::copyFallbackStillSwapsAndRollsBack()
{
    QVERIFY(buildInstallation(m_staged, QByteArray("new")));
    QVERIFY(buildInstallation(m_target, QByteArray("old")));

    ReplaceHooks hooks;
    hooks.forceCopyFallback = true;

    ReplaceResult result = swapDirectory(m_staged, m_target, m_backup, kExe, QStringList(), hooks);
    QVERIFY2(result.ok(), replaceErrorName(result.error));
    QVERIFY(result.usedCopyFallback);
    QVERIFY(installationHasMarker(m_target, QByteArray("new")));

    QDir(m_target).removeRecursively();
    QDir(m_staged).removeRecursively();
    QVERIFY(buildInstallation(m_staged, QByteArray("newer")));
    QVERIFY(buildInstallation(m_target, QByteArray("current")));
    hooks.beforePromoteRename = [] { return false; };

    result = swapDirectory(m_staged, m_target, m_backup, kExe, QStringList(), hooks);
    QVERIFY(!result.ok());
    QVERIFY(result.rolledBack);
    QVERIFY(installationHasMarker(m_target, QByteArray("current")));
}

void UpdaterPortableSwapTest::replacesAFileAndPreservesTheExecutableBit()
{
    const QDir root(m_dir.path());
    const QString target = root.absoluteFilePath(QStringLiteral("Lightning.AppImage"));
    const QString staged = root.absoluteFilePath(QStringLiteral("staged.AppImage"));
    const QString backup = target + QStringLiteral(".previous");

    QVERIFY(writeFile(target, QByteArray("old-appimage")));
    QVERIFY(writeFile(staged, QByteArray("new-appimage")));
    QVERIFY(QFile::setPermissions(target,
                                  QFile::ReadOwner | QFile::WriteOwner
                                      | QFile::ExeOwner | QFile::ReadUser
                                      | QFile::WriteUser | QFile::ExeUser));

    const ReplaceResult result = replaceFileAtomically(staged, target, backup);
    QVERIFY2(result.ok(), replaceErrorName(result.error));
    QCOMPARE(readFile(target), QByteArray("new-appimage"));
#ifndef Q_OS_WIN
    QVERIFY(QFileInfo(target).isExecutable());
#endif
    QVERIFY(!QFileInfo::exists(backup));
    QVERIFY(!QFileInfo::exists(staged));
}

void UpdaterPortableSwapTest::fileReplaceRollsBackOnPromotionFailure()
{
    const QDir root(m_dir.path());
    const QString target = root.absoluteFilePath(QStringLiteral("Roll.AppImage"));
    const QString staged = root.absoluteFilePath(QStringLiteral("roll-staged.AppImage"));
    const QString backup = target + QStringLiteral(".previous");

    QVERIFY(writeFile(target, QByteArray("old-appimage")));
    QVERIFY(writeFile(staged, QByteArray("new-appimage")));

    ReplaceHooks hooks;
    hooks.beforePromoteRename = [] { return false; };

    const ReplaceResult result =
        replaceFileAtomically(staged, target, backup, hooks);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, ReplaceError::PromoteFailed);
    QVERIFY(result.rolledBack);
    QCOMPARE(readFile(target), QByteArray("old-appimage"));
    QVERIFY(!QFileInfo::exists(backup));
    const QStringList leftovers =
        root.entryList(QStringList(QStringLiteral(".lightning-new-*")),
                       QDir::Files | QDir::Hidden);
    QVERIFY2(leftovers.isEmpty(), qPrintable(leftovers.join(QLatin1Char(','))));
}

void UpdaterPortableSwapTest::fileReplaceRefusesAMissingTarget()
{
    const QDir root(m_dir.path());
    const QString staged = root.absoluteFilePath(QStringLiteral("only-staged"));
    QVERIFY(writeFile(staged, QByteArray("x")));

    const ReplaceResult result = replaceFileAtomically(
        staged, root.absoluteFilePath(QStringLiteral("absent")),
        root.absoluteFilePath(QStringLiteral("absent.previous")));
    QVERIFY(!result.ok());
    QCOMPARE(result.error, ReplaceError::TargetMissing);
    QVERIFY(QFileInfo::exists(staged));
}

void UpdaterPortableSwapTest::fileReplaceRefusesAMissingSource()
{
    const QDir root(m_dir.path());
    const QString target = root.absoluteFilePath(QStringLiteral("present"));
    QVERIFY(writeFile(target, QByteArray("old")));

    const ReplaceResult result = replaceFileAtomically(
        root.absoluteFilePath(QStringLiteral("nothing-here")), target,
        target + QStringLiteral(".previous"));
    QVERIFY(!result.ok());
    QCOMPARE(result.error, ReplaceError::SourceMissing);
    QCOMPARE(readFile(target), QByteArray("old"));
}

void UpdaterPortableSwapTest::fileReplaceUsesTheCopyFallbackWhenRenameCannotBeUsed()
{
    const QDir root(m_dir.path());
    const QString target = root.absoluteFilePath(QStringLiteral("Xdev.AppImage"));
    const QString staged = root.absoluteFilePath(QStringLiteral("xdev-staged.AppImage"));
    const QString backup = target + QStringLiteral(".previous");

    QVERIFY(writeFile(target, QByteArray("old-appimage")));
    QVERIFY(writeFile(staged, QByteArray("new-appimage-across-a-mount")));

    ReplaceHooks hooks;
    hooks.forceCopyFallback = true;

    const ReplaceResult result =
        replaceFileAtomically(staged, target, backup, hooks);
    QVERIFY2(result.ok(), replaceErrorName(result.error));
    QVERIFY(result.usedCopyFallback);
    QCOMPARE(readFile(target), QByteArray("new-appimage-across-a-mount"));
    QVERIFY(QFileInfo::exists(staged));
}

QTEST_GUILESS_MAIN(UpdaterPortableSwapTest)
#include "UpdaterPortableSwapTest.moc"
