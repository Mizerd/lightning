#pragma once

#include <QString>
#include <QStringList>

#include <functional>

// Transactional replacement with rollback. Verify everything before touching
// anything, perform the fewest renames possible, and undo on failure: the
// target is always entirely old or entirely new, never mixed or missing.
// Nothing outside the named paths is deleted or recursed into.

namespace updater {

enum class ReplaceError {
    None = 0,
    SourceMissing,
    SourceNotAFile,
    SourceNotADirectory,
    TargetMissing,
    TargetNotAFile,
    TargetNotADirectory,
    TargetNotWritable,
    BackupPathUnusable,
    LayoutInvalid,
    PromoteFailed,
    BackupFailed,
    CopyFailed,
    RollbackFailed,
    RefusedUnsafePath,
};

const char *replaceErrorName(ReplaceError error);

struct ReplaceResult {
    ReplaceError error = ReplaceError::None;
    QString message;
    bool rolledBack = false;
    bool usedCopyFallback = false;
    QString backupPath;

    bool ok() const { return error == ReplaceError::None; }
};

struct ReplaceHooks {
    std::function<bool()> beforeBackupRename;
    std::function<bool()> beforePromoteRename;
    bool forceCopyFallback = false;
};

ReplaceResult replaceFileAtomically(const QString &newFile,
                                    const QString &targetPath,
                                    const QString &backupPath,
                                    const ReplaceHooks &hooks = ReplaceHooks());

ReplaceResult swapDirectory(const QString &stagedDir, const QString &targetDir,
                            const QString &backupDir,
                            const QString &expectedExecutableName,
                            const QStringList &preserveNames = QStringList(),
                            const ReplaceHooks &hooks = ReplaceHooks());

QString resolveStagedRoot(const QString &stagedDir,
                          const QString &expectedExecutableName);

bool directoryIsWritable(const QString &directory);

// Where a portable swap puts the outgoing build: a sibling of the
// installation, never inside it (swapDirectory refuses an overlapping backup).
// Sharing the parent keeps promotion a same-filesystem rename. Shared with the
// helper so tests assert the rule it actually uses.
QString portableBackupPath(const QString &targetDir);

}
