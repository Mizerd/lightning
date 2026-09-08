#pragma once

#include <QList>
#include <QString>

#include <functional>

// What the environment of a RELAUNCHED Lightning has to be corrected for.
//
// The helper starts the application again by inheriting its own environment,
// which is the application's environment, which is whatever launched the
// application in the first place. Two things in there are wrong by the time
// the relaunch happens, and both were MEASURED on 2026-09-08 against a real
// 0.9.3 AppImage on NixOS rather than reasoned about:
//
// 1. A PRIVATE TEMPORARY DIRECTORY THAT NO LONGER EXISTS. `nix-shell` (and
//    systemd's PrivateTmp, and several sandboxes) point TMPDIR at a directory
//    they delete when they exit — and they exit when Lightning does, which is
//    exactly when the helper starts working. The AppImage runtime creates its
//    mount point under TMPDIR, so the relaunched process died on
//    `create mount dir error: No such file or directory` before it had run a
//    single instruction of its own. It is not AppImage-specific: any Qt
//    temporary file in the new process would fail the same way, so the
//    correction applies to every install mode.
//
// 2. AN APPIMAGE THAT CANNOT MOUNT ITSELF WHERE IT IS BEING RELAUNCHED. When
//    the AppImage was started through an EXTRACTOR rather than by its own
//    runtime — `appimage-run` on NixOS, Gearlever, or a previous
//    `--appimage-extract-and-run` — the process lives inside that wrapper's
//    sandbox, and the helper, a detached child, is still inside it when it
//    relaunches. bubblewrap sets `no_new_privs`, so the setuid `fusermount3`
//    on the host cannot gain privilege there: the runtime answers
//    `No suitable fusermount binary found on the $PATH` and exits 127. The
//    same AppImage relaunched with APPIMAGE_EXTRACT_AND_RUN=1 in that same
//    sandbox printed `Lightning 0.9.3` and exited 0.
//
//    So the relaunch asks for extraction ONLY when the running instance was
//    itself extracted. A normally mounted AppImage keeps the cheap path: it
//    sets APPDIR to its own mount point (`…/.mount_XXXXXX`), which is the
//    evidence that fuse works here, and extraction would cost a needless copy
//    of the whole payload on every update.
//
// Everything here is a pure decision over a lookup, so it is tested by
// calling it rather than by performing an update.
namespace updater {

/// One correction to apply before the application is started again.
struct RelaunchEnvironmentChange {
    QString name;
    /// The value to set. Meaningless when `remove` is true.
    QString value;
    /// True: unset the variable. False: set it to `value`.
    bool remove = false;
    /// One short sentence for the helper's stderr, so a future failure has a
    /// record of what was changed and why.
    QString reason;
};

/// The corrections for a relaunch, in the order they should be applied.
///
/// `value` answers what a variable holds, empty when it is unset.
/// `directoryExists` answers whether a path names a directory that exists NOW
/// — the whole point of the first rule is that it did exist a moment ago.
///
/// Nothing here overrides a value the user set deliberately: an
/// APPIMAGE_EXTRACT_AND_RUN that is already present is left exactly as it is,
/// including when it says 0.
QList<RelaunchEnvironmentChange> relaunchEnvironmentChanges(
    bool appImageInstall,
    const std::function<QString(const QString &)> &value,
    const std::function<bool(const QString &)> &directoryExists);

} // namespace updater
