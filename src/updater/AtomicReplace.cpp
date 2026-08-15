#include "updater/AtomicReplace.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSaveFile>

#ifndef Q_OS_WIN
#include <fcntl.h>
#include <unistd.h>
#endif

namespace updater {
namespace {

ReplaceResult replaceFail(ReplaceError error, const QString &message,
                          bool rolledBack = false)
{
    ReplaceResult result;
    result.error = error;
    result.message = message;
    result.rolledBack = rolledBack;
    return result;
}

QString siblingScratchPath(const QString &directory, const QString &prefix)
{
    for (int attempt = 0; attempt < 32; ++attempt) {
        const QString candidate =
            QDir(directory).absoluteFilePath(
                prefix
                + QString::number(QRandomGenerator::system()->generate64(), 16));
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
    return QString();
}

// Copies a file and flushes it to stable storage. QSaveFile already writes to
// a temporary and renames on commit; the explicit fsync of the directory
// afterwards is what makes the new name durable across a power loss.
bool copyFileDurably(const QString &source, const QString &destination)
{
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly))
        return false;
    QSaveFile out(destination);
    if (!out.open(QIODevice::WriteOnly))
        return false;

    constexpr qint64 kChunk = 1 << 20;
    while (!in.atEnd()) {
        const QByteArray chunk = in.read(kChunk);
        if (chunk.isEmpty() && !in.atEnd())
            return false;
        if (out.write(chunk) != chunk.size())
            return false;
    }
    if (!out.commit())
        return false;

#ifndef Q_OS_WIN
    const QByteArray dirPath =
        QFileInfo(destination).absolutePath().toLocal8Bit();
    const int fd = ::open(dirPath.constData(), O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#endif
    return true;
}

// Recursive copy used only as the cross-filesystem fallback. It refuses
// symbolic links outright rather than following or recreating them: the
// staged tree comes from our own extractor, which never writes one, so a
// symlink here means something unexpected touched the staging directory.
bool copyDirectoryTree(const QString &source, const QString &destination)
{
    const QFileInfo sourceInfo(source);
    if (sourceInfo.isSymLink() || !sourceInfo.isDir())
        return false;
    if (!QDir().mkpath(destination))
        return false;

    const QDir dir(sourceInfo.absoluteFilePath());
    const QFileInfoList entries =
        dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                          | QDir::System);
    for (const QFileInfo &entry : entries) {
        if (entry.isSymLink())
            return false;
        const QString child =
            QDir(destination).absoluteFilePath(entry.fileName());
        if (entry.isDir()) {
            if (!copyDirectoryTree(entry.absoluteFilePath(), child))
                return false;
        } else if (entry.isFile()) {
            if (!copyFileDurably(entry.absoluteFilePath(), child))
                return false;
            QFile::setPermissions(child,
                                  QFile::permissions(entry.absoluteFilePath()));
        } else {
            return false; // device nodes, fifos and the like are never copied
        }
    }
    return true;
}

// Recursive removal with hard guards. Refuses anything that is not an
// absolute path of reasonable depth, refuses a symlink, and refuses to touch
// a directory that is an ancestor of `mustNotContain`.
bool removeTreeGuarded(const QString &path, const QString &mustNotContain)
{
    const QFileInfo info(path);
    if (!info.isAbsolute() || !info.exists())
        return true;
    if (info.isSymLink())
        return false;
    if (!info.isDir())
        return QFile::remove(path);

    const QString clean = QDir::cleanPath(info.absoluteFilePath());
    if (clean.count(QLatin1Char('/')) < 2)
        return false; // far too close to the filesystem root
    if (!mustNotContain.isEmpty()) {
        const QString guard = QDir::cleanPath(mustNotContain);
        if (guard == clean
            || guard.startsWith(clean + QLatin1Char('/')))
            return false; // would delete the thing we are protecting
    }
    return QDir(clean).removeRecursively();
}

} // namespace

const char *replaceErrorName(ReplaceError error)
{
    switch (error) {
    case ReplaceError::None: return "none";
    case ReplaceError::SourceMissing: return "source-missing";
    case ReplaceError::SourceNotAFile: return "source-not-a-file";
    case ReplaceError::SourceNotADirectory: return "source-not-a-directory";
    case ReplaceError::TargetMissing: return "target-missing";
    case ReplaceError::TargetNotAFile: return "target-not-a-file";
    case ReplaceError::TargetNotADirectory: return "target-not-a-directory";
    case ReplaceError::TargetNotWritable: return "target-not-writable";
    case ReplaceError::BackupPathUnusable: return "backup-path-unusable";
    case ReplaceError::LayoutInvalid: return "layout-invalid";
    case ReplaceError::PromoteFailed: return "promote-failed";
    case ReplaceError::BackupFailed: return "backup-failed";
    case ReplaceError::CopyFailed: return "copy-failed";
    case ReplaceError::RollbackFailed: return "rollback-failed";
    case ReplaceError::RefusedUnsafePath: return "refused-unsafe-path";
    }
    return "unknown";
}

bool directoryIsWritable(const QString &directory)
{
    const QFileInfo info(directory);
    if (!info.exists() || !info.isDir())
        return false;
    const QString probe = siblingScratchPath(info.absoluteFilePath(),
                                             QStringLiteral(".lightning-probe-"));
    if (probe.isEmpty())
        return false;
    QFile file(probe);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.close();
    QFile::remove(probe);
    return true;
}

// ---------------------------------------------------------------------------
// File replacement (AppImage)
// ---------------------------------------------------------------------------

ReplaceResult replaceFileAtomically(const QString &newFile,
                                    const QString &targetPath,
                                    const QString &backupPath,
                                    const ReplaceHooks &hooks)
{
    const QFileInfo sourceInfo(newFile);
    if (!sourceInfo.exists())
        return replaceFail(ReplaceError::SourceMissing,
                           QStringLiteral("the staged file does not exist"));
    if (!sourceInfo.isFile())
        return replaceFail(ReplaceError::SourceNotAFile,
                           QStringLiteral("the staged path is not a regular file"));

    const QFileInfo targetInfo(targetPath);
    if (!targetInfo.exists())
        return replaceFail(ReplaceError::TargetMissing,
                           QStringLiteral("the target file does not exist"));
    if (!targetInfo.isFile())
        return replaceFail(ReplaceError::TargetNotAFile,
                           QStringLiteral("the target is not a regular file"));

    const QString targetDir = targetInfo.absolutePath();
    if (!directoryIsWritable(targetDir))
        return replaceFail(ReplaceError::TargetNotWritable,
                           QStringLiteral("the directory containing the target is "
                                          "not writable; nothing was changed"));

    const QFileInfo backupInfo(backupPath);
    if (!backupInfo.isAbsolute())
        return replaceFail(ReplaceError::BackupPathUnusable,
                           QStringLiteral("the backup path must be absolute"));
    if (backupInfo.exists()) {
        // A leftover from a crashed run must not block every future update,
        // but only a plain file is ever cleared away.
        if (backupInfo.isDir() || backupInfo.isSymLink()
            || !QFile::remove(backupPath))
            return replaceFail(ReplaceError::BackupPathUnusable,
                               QStringLiteral("the backup path already exists and "
                                              "could not be cleared"));
    }
    if (QDir::cleanPath(backupInfo.absoluteFilePath())
        == QDir::cleanPath(targetInfo.absoluteFilePath()))
        return replaceFail(ReplaceError::RefusedUnsafePath,
                           QStringLiteral("the backup path is the target path"));

    // Preserve the target's permissions, and guarantee the executable bit —
    // an AppImage that is not executable is a bricked installation.
    QFile::Permissions permissions = QFile::permissions(targetPath);
    permissions |= QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
                 | QFile::ReadUser | QFile::WriteUser | QFile::ExeUser;

    // Step 1: get the new bytes into the TARGET's own directory, so the
    // decisive rename never crosses a filesystem.
    ReplaceResult result;
    const QString scratch = siblingScratchPath(targetDir,
                                               QStringLiteral(".lightning-new-"));
    if (scratch.isEmpty())
        return replaceFail(ReplaceError::TargetNotWritable,
                           QStringLiteral("cannot allocate a scratch name beside "
                                          "the target"));

    bool moved = false;
    if (!hooks.forceCopyFallback)
        moved = QFile::rename(newFile, scratch);
    if (!moved) {
        if (!copyFileDurably(newFile, scratch)) {
            QFile::remove(scratch);
            return replaceFail(ReplaceError::CopyFailed,
                               QStringLiteral("could not stage the new file beside "
                                              "the target"));
        }
        result.usedCopyFallback = true;
    }
    QFile::setPermissions(scratch, permissions);

    if (hooks.beforeBackupRename && !hooks.beforeBackupRename()) {
        QFile::remove(scratch);
        return replaceFail(ReplaceError::BackupFailed,
                           QStringLiteral("simulated failure before the backup "
                                          "rename; nothing was changed"),
                           /*rolledBack=*/true);
    }

    // Step 2: move the current version aside.
    if (!QFile::rename(targetPath, backupPath)) {
        QFile::remove(scratch);
        return replaceFail(ReplaceError::BackupFailed,
                           QStringLiteral("could not move the current version "
                                          "aside; nothing was changed"),
                           /*rolledBack=*/true);
    }

    const auto rollback = [&]() -> bool {
        QFile::remove(scratch);
        return QFile::rename(backupPath, targetPath);
    };

    if (hooks.beforePromoteRename && !hooks.beforePromoteRename()) {
        if (!rollback())
            return replaceFail(ReplaceError::RollbackFailed,
                               QStringLiteral("the new version could not be "
                                              "promoted AND the previous version "
                                              "could not be restored"));
        return replaceFail(ReplaceError::PromoteFailed,
                           QStringLiteral("simulated failure before promoting the "
                                          "new version"),
                           /*rolledBack=*/true);
    }

    // Step 3: promote.
    if (!QFile::rename(scratch, targetPath)) {
        if (!rollback())
            return replaceFail(ReplaceError::RollbackFailed,
                               QStringLiteral("the new version could not be "
                                              "promoted AND the previous version "
                                              "could not be restored"));
        return replaceFail(ReplaceError::PromoteFailed,
                           QStringLiteral("could not promote the new version"),
                           /*rolledBack=*/true);
    }

    QFile::setPermissions(targetPath, permissions);
    // The previous version has done its job. Removing it keeps a 150 MB
    // AppImage from accumulating beside every install.
    QFile::remove(backupPath);
    return result;
}

// ---------------------------------------------------------------------------
// Directory swap (portable Windows)
// ---------------------------------------------------------------------------

QString resolveStagedRoot(const QString &stagedDir,
                          const QString &expectedExecutableName)
{
    const QFileInfo info(stagedDir);
    if (!info.exists() || !info.isDir() || expectedExecutableName.isEmpty())
        return QString();

    const QDir dir(info.absoluteFilePath());
    if (QFileInfo(dir.absoluteFilePath(expectedExecutableName)).isFile())
        return dir.absolutePath();

    // A ZIP that unpacks into exactly one top-level folder is the normal
    // shape of a portable release. More than one candidate is ambiguous and
    // is refused rather than guessed at.
    const QStringList subdirs =
        dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (subdirs.size() != 1)
        return QString();
    const QDir inner(dir.absoluteFilePath(subdirs.first()));
    if (QFileInfo(inner.absoluteFilePath(expectedExecutableName)).isFile())
        return inner.absolutePath();
    return QString();
}

ReplaceResult swapDirectory(const QString &stagedDir, const QString &targetDir,
                            const QString &backupDir,
                            const QString &expectedExecutableName,
                            const ReplaceHooks &hooks)
{
    const QFileInfo stagedInfo(stagedDir);
    if (!stagedInfo.exists())
        return replaceFail(ReplaceError::SourceMissing,
                           QStringLiteral("the staged directory does not exist"));
    if (!stagedInfo.isDir())
        return replaceFail(ReplaceError::SourceNotADirectory,
                           QStringLiteral("the staged path is not a directory"));

    const QFileInfo targetInfo(targetDir);
    if (!targetInfo.exists())
        return replaceFail(ReplaceError::TargetMissing,
                           QStringLiteral("the target directory does not exist"));
    if (!targetInfo.isDir())
        return replaceFail(ReplaceError::TargetNotADirectory,
                           QStringLiteral("the target is not a directory"));

    const QString cleanTarget = QDir::cleanPath(targetInfo.absoluteFilePath());
    const QString parentDir = QFileInfo(cleanTarget).absolutePath();
    const QFileInfo backupInfo(backupDir);
    if (!backupInfo.isAbsolute())
        return replaceFail(ReplaceError::BackupPathUnusable,
                           QStringLiteral("the backup path must be absolute"));
    const QString cleanBackup = QDir::cleanPath(backupInfo.absoluteFilePath());
    if (cleanBackup == cleanTarget
        || cleanTarget.startsWith(cleanBackup + QLatin1Char('/'))
        || cleanBackup.startsWith(cleanTarget + QLatin1Char('/')))
        return replaceFail(ReplaceError::RefusedUnsafePath,
                           QStringLiteral("the backup path overlaps the target"));
    if (backupInfo.exists())
        return replaceFail(ReplaceError::BackupPathUnusable,
                           QStringLiteral("the backup path already exists"));

    // LAYOUT FIRST. Nothing is touched until we know the staged tree is a
    // real Lightning installation.
    const QString sourceRoot = resolveStagedRoot(stagedDir, expectedExecutableName);
    if (sourceRoot.isEmpty())
        return replaceFail(ReplaceError::LayoutInvalid,
                           QStringLiteral("the staged archive does not contain the "
                                          "expected Lightning executable"));

    // WRITABILITY NEXT, and as a distinct failure: a read-only install
    // directory must be reported as exactly that, not as a half-finished
    // swap. Both the target itself and its parent must be writable, because
    // the swap renames the target within its parent.
    if (!directoryIsWritable(parentDir) || !directoryIsWritable(cleanTarget))
        return replaceFail(ReplaceError::TargetNotWritable,
                           QStringLiteral("the installation directory is not "
                                          "writable; nothing was changed"));

    ReplaceResult result;

    // Step 1: bring the new tree next to the target, inside the same parent,
    // so the decisive renames cannot cross a filesystem.
    const QString scratch = siblingScratchPath(parentDir,
                                               QStringLiteral(".lightning-new-"));
    if (scratch.isEmpty())
        return replaceFail(ReplaceError::TargetNotWritable,
                           QStringLiteral("cannot allocate a scratch name beside "
                                          "the installation"));

    bool moved = false;
    if (!hooks.forceCopyFallback)
        moved = QDir().rename(sourceRoot, scratch);
    if (!moved) {
        if (!copyDirectoryTree(sourceRoot, scratch)) {
            removeTreeGuarded(scratch, cleanTarget);
            return replaceFail(ReplaceError::CopyFailed,
                               QStringLiteral("could not stage the new version "
                                              "beside the installation; nothing "
                                              "was changed"));
        }
        result.usedCopyFallback = true;
    }

    if (hooks.beforeBackupRename && !hooks.beforeBackupRename()) {
        removeTreeGuarded(scratch, cleanTarget);
        return replaceFail(ReplaceError::BackupFailed,
                           QStringLiteral("simulated failure before the backup "
                                          "rename; nothing was changed"),
                           /*rolledBack=*/true);
    }

    // Step 2: move the current installation aside.
    if (!QDir().rename(cleanTarget, cleanBackup)) {
        removeTreeGuarded(scratch, cleanTarget);
        return replaceFail(ReplaceError::BackupFailed,
                           QStringLiteral("could not move the current installation "
                                          "aside; nothing was changed"),
                           /*rolledBack=*/true);
    }

    const auto rollback = [&]() -> bool {
        removeTreeGuarded(scratch, cleanTarget);
        return QDir().rename(cleanBackup, cleanTarget);
    };

    if (hooks.beforePromoteRename && !hooks.beforePromoteRename()) {
        if (!rollback())
            return replaceFail(ReplaceError::RollbackFailed,
                               QStringLiteral("the new version could not be "
                                              "promoted AND the previous "
                                              "installation could not be restored; "
                                              "the previous version is at the "
                                              "backup path"));
        return replaceFail(ReplaceError::PromoteFailed,
                           QStringLiteral("simulated failure before promoting the "
                                          "new installation"),
                           /*rolledBack=*/true);
    }

    // Step 3: promote.
    if (!QDir().rename(scratch, cleanTarget)) {
        if (!rollback()) {
            ReplaceResult failure =
                replaceFail(ReplaceError::RollbackFailed,
                            QStringLiteral("the new version could not be promoted "
                                           "AND the previous installation could "
                                           "not be restored"));
            failure.backupPath = cleanBackup;
            return failure;
        }
        return replaceFail(ReplaceError::PromoteFailed,
                           QStringLiteral("could not promote the new installation"),
                           /*rolledBack=*/true);
    }

    // Step 4: the previous installation is now redundant. It is removed with
    // the guard above, which refuses anything that is not a plain directory
    // safely below the parent and refuses to delete an ancestor of the target.
    if (!removeTreeGuarded(cleanBackup, cleanTarget))
        result.backupPath = cleanBackup; // left behind; not a failure

    return result;
}

} // namespace updater
