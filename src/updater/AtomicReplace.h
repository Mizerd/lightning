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
    TargetNotWritable,      // distinct on purpose: nothing was changed
    BackupPathUnusable,
    LayoutInvalid,          // staged tree does not contain the expected binary
    PromoteFailed,          // could not move the new content into place
    BackupFailed,           // could not move the old content aside
    CopyFailed,             // cross-filesystem fallback failed
    RollbackFailed,         // the worst case: report it loudly
    RefusedUnsafePath,      // e.g. backup dir is an ancestor of the target
};

const char *replaceErrorName(ReplaceError error);

struct ReplaceResult {
    ReplaceError error = ReplaceError::None;
    QString message;
    bool rolledBack = false;      // a failure was fully undone
    bool usedCopyFallback = false; // crossed a filesystem boundary
    QString backupPath;            // where the previous version went, if kept

    bool ok() const { return error == ReplaceError::None; }
};

// Test seams: a hook returning false fails that step as if the OS refused,
// exercising the rollback paths.
struct ReplaceHooks {
    std::function<bool()> beforeBackupRename;  // fails before the target moves
    std::function<bool()> beforePromoteRename; // fails AFTER the target moved
    bool forceCopyFallback = false;            // pretend rename() hit EXDEV
};

// AppImage case. Replaces `targetPath` (an existing regular file) with
// `newFile`, preserving the target's executable bit and permissions.
//
// `backupPath` must be a helper-private path. It is removed on success; on
// failure it is restored over the target and reported.
//
// The staged file is usually on another mount (EXDEV), so it is first copied
// and flushed into the target's own directory; the renames always happen on
// one filesystem.
ReplaceResult replaceFileAtomically(const QString &newFile,
                                    const QString &targetPath,
                                    const QString &backupPath,
                                    const ReplaceHooks &hooks = ReplaceHooks());

// Portable-Windows case. Swaps `targetDir` for `stagedDir`.
//
// `expectedExecutableName` is validated to exist inside the staged tree
// BEFORE anything is touched. A ZIP that unpacks into a single top-level
// folder is handled: if the executable is not directly in `stagedDir` but
// exactly one immediate subdirectory contains it, that subdirectory becomes
// the effective source.
//
// On success the previous directory is removed; on failure it is restored and
// the target left as found.
//
// `preserveNames` are top-level entries of `targetDir` that are never moved,
// backed up, promoted over or deleted. A portable installation keeps its
// settings, sealed session and SDK/crypto stores inside the installation; the
// swap would otherwise delete them, leaving a fresh login as a new Matrix
// device without access to old encrypted history. Installed (MSI/setup) builds
// pass an empty set.
ReplaceResult swapDirectory(const QString &stagedDir, const QString &targetDir,
                            const QString &backupDir,
                            const QString &expectedExecutableName,
                            const QStringList &preserveNames = QStringList(),
                            const ReplaceHooks &hooks = ReplaceHooks());

// Resolves the effective source root inside a staged extraction, applying the
// single-top-level-folder rule. Returns an empty string when the expected
// executable is not found. Exposed for tests.
QString resolveStagedRoot(const QString &stagedDir,
                          const QString &expectedExecutableName);

// True when a probe file can be created and removed inside `directory`.
bool directoryIsWritable(const QString &directory);

// Where a portable swap puts the outgoing build: a sibling of the
// installation, never inside it (swapDirectory refuses an overlapping backup).
// Sharing the parent keeps promotion a same-filesystem rename. Shared with the
// helper so tests assert the rule it actually uses.
QString portableBackupPath(const QString &targetDir);

} // namespace updater
