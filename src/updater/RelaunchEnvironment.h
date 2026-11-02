#pragma once

#include <QList>
#include <QString>

#include <functional>

// Corrections to the environment a relaunched Lightning inherits from the
// helper (and so from whatever originally launched it):
//
// 1. A private temporary directory that no longer exists. nix-shell,
//    systemd's PrivateTmp and some sandboxes point TMPDIR at a directory they
//    delete when Lightning exits. The AppImage runtime mounts under TMPDIR
//    (failing with "create mount dir error"), and any Qt temporary file would
//    fail too, so this applies to every install mode.
//
// 2. An AppImage that cannot mount itself. When it was started through an
//    extractor (appimage-run on NixOS, Gearlever, --appimage-extract-and-run),
//    the helper is still inside that wrapper's bubblewrap sandbox, where
//    no_new_privs stops the setuid fusermount3 ("No suitable fusermount binary
//    found", exit 127). Relaunching with APPIMAGE_EXTRACT_AND_RUN=1 works.
//    Extraction is requested only when the running instance was itself
//    extracted; a normally mounted AppImage has APPDIR at its own
//    `.mount_XXXXXX`, proving FUSE works, and keeps the cheaper path.
//
// Pure decisions over a lookup, tested by calling them.
namespace updater {

struct RelaunchEnvironmentChange {
    QString name;
    QString value;
    bool remove = false;
    QString reason;
};

QList<RelaunchEnvironmentChange> relaunchEnvironmentChanges(
    bool appImageInstall,
    const std::function<QString(const QString &)> &value,
    const std::function<bool(const QString &)> &directoryExists);

}
