// lightning-updater: transactional replacement and rollback.
//
// The contract these tests defend is that the installation is either
// entirely the old version or entirely the new one. A failure at any step
// must leave the target exactly as it was found — never half-A-half-B, never
// missing — and a target we cannot write must be refused as exactly that
// BEFORE anything is touched.
//
// The mid-swap failures are injected through ReplaceHooks so the rollback
// paths are exercised for real rather than reasoned about.

#include "updater/AtomicReplace.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

using namespace updater;

namespace {

const QString kExe = QStringLiteral("matrix-client.exe");

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

// A minimal but realistic installation tree.
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

} // namespace

class UpdaterPortableSwapTest : public QObject
{
    Q_OBJECT

private slots:
    void init();

    // --- staged layout resolution ---
    void resolvesAFlatStagedTree();
    void resolvesASingleTopLevelFolder();
    void refusesAnAmbiguousStagedTree();
    void refusesAStagedTreeWithoutTheExecutable();

    // --- directory swap ---
    void swapsTheInstallationAtomically();
    void swapFromASingleTopLevelFolder();
    void refusesAnInvalidLayoutBeforeTouchingTheTarget();
    void rollsBackWhenPromotionFails();
    void rollsBackWhenTheBackupRenameFails();
    void refusesANonWritableTargetWithADistinctError();
    void refusesAnExistingBackupPath();
    void refusesABackupPathInsideTheTarget();
    void copyFallbackStillSwapsAndRollsBack();

    // --- file replacement (AppImage) ---
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

// ---------------------------------------------------------------------------

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
    // Two candidates is ambiguous; guessing would be worse than refusing.
    QVERIFY(resolveStagedRoot(m_staged, kExe).isEmpty());
}

void UpdaterPortableSwapTest::refusesAStagedTreeWithoutTheExecutable()
{
    QVERIFY(writeFile(QDir(m_staged).absoluteFilePath(QStringLiteral("README.txt")),
                      QByteArray("nothing here")));
    QVERIFY(resolveStagedRoot(m_staged, kExe).isEmpty());
}

// ---------------------------------------------------------------------------

void UpdaterPortableSwapTest::swapsTheInstallationAtomically()
{
    QVERIFY(buildInstallation(m_staged, QByteArray("new")));
    QVERIFY(buildInstallation(m_target, QByteArray("old")));

    const ReplaceResult result = swapDirectory(m_staged, m_target, m_backup, kExe);
    QVERIFY2(result.ok(), replaceErrorName(result.error));
    QVERIFY(installationHasMarker(m_target, QByteArray("new")));
    // Every file came from the new version — no mixture survived.
    QVERIFY(!installationHasMarker(m_target, QByteArray("old")));
    // The previous installation is cleaned up, and nothing outside was touched.
    QVERIFY(!QFileInfo::exists(m_backup));
    QVERIFY(QFileInfo::exists(m_dir.path()));
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
    // The wrapper folder must not have become the installation root.
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
    // Nothing moved.
    QVERIFY(installationHasMarker(m_target, QByteArray("old")));
    QVERIFY(!QFileInfo::exists(m_backup));
}

void UpdaterPortableSwapTest::rollsBackWhenPromotionFails()
{
    QVERIFY(buildInstallation(m_staged, QByteArray("new")));
    QVERIFY(buildInstallation(m_target, QByteArray("old")));

    // Fails AFTER the current installation has been moved aside — the worst
    // moment, and the one the rollback exists for.
    ReplaceHooks hooks;
    hooks.beforePromoteRename = [] { return false; };

    const ReplaceResult result =
        swapDirectory(m_staged, m_target, m_backup, kExe, hooks);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, ReplaceError::PromoteFailed);
    QVERIFY(result.rolledBack);

    // The original installation is back, complete and unmixed.
    QVERIFY(installationHasMarker(m_target, QByteArray("old")));
    QVERIFY(!QFileInfo::exists(m_backup));
    // No scratch directory was left beside it.
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
        swapDirectory(m_staged, m_target, m_backup, kExe, hooks);
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

    // Read + execute only: entries can be listed but nothing may be created.
    QVERIFY(QFile::setPermissions(m_target,
                                  QFile::ReadOwner | QFile::ExeOwner
                                      | QFile::ReadUser | QFile::ExeUser));

    const ReplaceResult result = swapDirectory(m_staged, m_target, m_backup, kExe);

    QVERIFY(QFile::setPermissions(m_target,
                                  QFile::ReadOwner | QFile::WriteOwner
                                      | QFile::ExeOwner | QFile::ReadUser
                                      | QFile::WriteUser | QFile::ExeUser));

    QVERIFY(!result.ok());
    // Distinct from every partial-progress error: nothing was attempted.
    QCOMPARE(result.error, ReplaceError::TargetNotWritable);
    QVERIFY(installationHasMarker(m_target, QByteArray("old")));
    QVERIFY(!QFileInfo::exists(m_backup));
#endif
}

void UpdaterPortableSwapTest::refusesAnExistingBackupPath()
{
    QVERIFY(buildInstallation(m_staged, QByteArray("new")));
    QVERIFY(buildInstallation(m_target, QByteArray("old")));
    QVERIFY(QDir().mkpath(m_backup));

    const ReplaceResult result = swapDirectory(m_staged, m_target, m_backup, kExe);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, ReplaceError::BackupPathUnusable);
    QVERIFY(installationHasMarker(m_target, QByteArray("old")));
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

    // Simulates the staged tree living on another filesystem, which is the
    // normal case: the download staging directory is under the user's data
    // directory, not beside the installation.
    ReplaceHooks hooks;
    hooks.forceCopyFallback = true;

    ReplaceResult result = swapDirectory(m_staged, m_target, m_backup, kExe, hooks);
    QVERIFY2(result.ok(), replaceErrorName(result.error));
    QVERIFY(result.usedCopyFallback);
    QVERIFY(installationHasMarker(m_target, QByteArray("new")));

    // And the same fallback still rolls back cleanly when promotion fails.
    QDir(m_target).removeRecursively();
    QDir(m_staged).removeRecursively();
    QVERIFY(buildInstallation(m_staged, QByteArray("newer")));
    QVERIFY(buildInstallation(m_target, QByteArray("current")));
    hooks.beforePromoteRename = [] { return false; };

    result = swapDirectory(m_staged, m_target, m_backup, kExe, hooks);
    QVERIFY(!result.ok());
    QVERIFY(result.rolledBack);
    QVERIFY(installationHasMarker(m_target, QByteArray("current")));
}

// ---------------------------------------------------------------------------
// File replacement (AppImage)
// ---------------------------------------------------------------------------

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
    // A non-executable AppImage is a bricked installation.
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
    // The original bytes are back and the backup is gone.
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
    // The staged file is untouched, so a retry is still possible.
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
    // The copy fallback leaves the source where it was — the caller owns it.
    QVERIFY(QFileInfo::exists(staged));
}

QTEST_GUILESS_MAIN(UpdaterPortableSwapTest)
#include "UpdaterPortableSwapTest.moc"
