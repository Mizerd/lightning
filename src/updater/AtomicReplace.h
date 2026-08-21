#pragma once

#include <QString>
#include <QStringList>

#include <functional>

// Transactional replacement with rollback.
//
// Both routines here have the same shape: verify everything BEFORE touching
// anything, then perform the smallest possible sequence of renames, and undo
// them if any step fails. The invariant that matters is that the target is
// either entirely the old version or entirely the new one — never a mixture,
// and never missing.
//
// Nothing here deletes a path the caller did not name, and nothing here
// recurses outside the directory it was given.

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

// Test seams. Each hook, when set and returning false, makes the
// corresponding step fail as though the operating system had refused it —
// which is how the rollback paths get real coverage.
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
// Cross-filesystem: rename(2) fails with EXDEV when the staged file lives on
// another mount, which it usually does (the download staging directory is
// under the user's data dir). The fallback copies the file into the TARGET's
// own directory first, flushes it to disk, and only then does the two renames
// — so the atomic swap always happens within one filesystem.
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
// On success the previous directory is removed. On any failure the previous
// directory is restored and the target is left exactly as it was found.
// `preserveNames` are top-level entries of `targetDir` that the swap must
// leave exactly where they are: never moved into the backup, never promoted
// over, never deleted. It exists for ONE case and it is a data-loss guard.
//
// A PORTABLE installation keeps its entire persistent state — settings, the
// sealed Matrix session, the Rust SDK store and the E2EE crypto store — in a
// directory INSIDE the installation, because that is what makes the folder
// copyable to another machine. The swap below moves every top-level entry of
// the installation into the backup and then deletes the backup, so without
// this the first in-app update would take the user's session and Megolm keys
// with it. The promoted installation would then start with no state at all:
// a fresh login, and a NEW Matrix device issued by the server, losing access
// to history that was encrypted to the old one.
//
// An installed (MSI / Setup) build passes an empty set — its state lives in
// %LOCALAPPDATA% and the registry and was never inside the install directory.
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

} // namespace updater
