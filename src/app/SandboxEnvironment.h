#pragma once

// The one answer to "does this process run in a Flatpak or a Snap?".
//
// Header-only on purpose: the update lane, the call lane and the app lane all
// ask, and several test targets compile those sources one by one without the
// application library.
//
// /.flatpak-info MUST NOT be tested with QFileInfo::exists()/QFile::exists().
// Flatpak (bwrap) bind-mounts it from a temporary file it has already
// UNLINKED, so the inode has st_nlink == 0 (mountinfo reads
// `/bindfile… //deleted`), and Qt 6 reports a file with no links as absent
// even though it opens and reads normally. Measured live 2026-10-07 in the
// Flathub build with gdb: `QFile("/.flatpak-info").exists()` was false inside
// the sandbox, so Download wrote into the sandbox's private tmpfs home and
// reported "Saved" for a file that vanished when the app quit. The
// environment ($FLATPAK_ID, which every Flatpak since 0.6 sets) is asked
// first and the file is OPENED, never stat-gated.

#include <QByteArray>
#include <QByteArrayList>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace sandboxenv {

/// The environment variables the decision reads. Pure input, for tests.
struct Environment
{
    QString flatpakId; // $FLATPAK_ID
    QString container; // $container ("flatpak" inside a Flatpak)
    QString snap;      // $SNAP
    QString snapName;  // $SNAP_NAME
};

inline Environment processEnvironment()
{
    Environment env;
    env.flatpakId = qEnvironmentVariable("FLATPAK_ID");
    env.container = qEnvironmentVariable("container");
    env.snap = qEnvironmentVariable("SNAP");
    env.snapName = qEnvironmentVariable("SNAP_NAME");
    return env;
}

/// The default location of Flatpak's sandbox description.
inline QString flatpakInfoPath()
{
    return QStringLiteral("/.flatpak-info");
}

/// A file's contents, read without asking whether it "exists" first (see the
/// header comment). Empty when it cannot be opened. Bounded: the real file is
/// a few hundred bytes.
inline QByteArray readFileUngated(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.read(1 << 20);
}

/// True when `path` is present: QFileInfo::exists(), or, for a file Qt calls
/// absent because it has no links left (a bind-mounted unlinked file such as
/// /.flatpak-info), one that still opens.
inline bool pathPresent(const QString &path)
{
    if (path.isEmpty())
        return false;
    if (QFileInfo::exists(path))
        return true;
    QFile file(path);
    return file.open(QIODevice::ReadOnly);
}

/// `key=` in `[group]` of a GKeyFile-style text (the format of
/// /.flatpak-info). Empty when absent.
inline QString keyFileValue(const QByteArray &data, const QString &group,
                            const QString &key)
{
    const QString header = QLatin1Char('[') + group + QLatin1Char(']');
    const QString prefix = key + QLatin1Char('=');
    bool inGroup = false;
    const QList<QByteArray> lines = data.split('\n');
    for (const QByteArray &raw : lines) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            inGroup = line == header;
            continue;
        }
        if (inGroup && line.startsWith(prefix))
            return line.mid(prefix.size());
    }
    return {};
}

struct Detection
{
    bool flatpak = false;
    bool snap = false;
    /// The Flatpak application id ($FLATPAK_ID, else [Application] name=).
    QString appId;
    /// /.flatpak-info's [Context] filesystems= line ("" when unreadable).
    QString flatpakFilesystems;
};

/// Pure: the decision from the environment and /.flatpak-info's contents
/// (empty when it could not be read).
inline Detection detect(const Environment &env, const QByteArray &flatpakInfo)
{
    Detection d;
    d.flatpak = !env.flatpakId.isEmpty()
        || env.container == QLatin1String("flatpak") || !flatpakInfo.isEmpty();
    if (d.flatpak) {
        d.appId = !env.flatpakId.isEmpty()
            ? env.flatpakId
            : keyFileValue(flatpakInfo, QStringLiteral("Application"),
                           QStringLiteral("name"));
        d.flatpakFilesystems = keyFileValue(
            flatpakInfo, QStringLiteral("Context"), QStringLiteral("filesystems"));
    }
    d.snap = !d.flatpak && !env.snap.isEmpty() && !env.snapName.isEmpty();
    return d;
}

/// This process. Not cached: the environment is asked first, so outside a
/// Flatpak with $FLATPAK_ID set the file is read at most once per call, and
/// tests may change the environment between calls.
inline Detection detectHost()
{
#if defined(Q_OS_LINUX)
    const Environment env = processEnvironment();
    // The file only matters for its [Context] (and as a last-resort marker);
    // read it unconditionally so filesystems= is known in a real Flatpak.
    return detect(env, readFileUngated(flatpakInfoPath()));
#else
    return {};
#endif
}

inline bool isFlatpak()
{
#if defined(Q_OS_LINUX)
    const Environment env = processEnvironment();
    if (!env.flatpakId.isEmpty() || env.container == QLatin1String("flatpak"))
        return true;
    return !readFileUngated(flatpakInfoPath()).isEmpty();
#else
    return false;
#endif
}

inline bool isSnap()
{
#if defined(Q_OS_LINUX)
    return !isFlatpak() && !qEnvironmentVariableIsEmpty("SNAP")
        && !qEnvironmentVariableIsEmpty("SNAP_NAME");
#else
    return false;
#endif
}

inline bool isSandboxed()
{
    return isFlatpak() || isSnap();
}

} // namespace sandboxenv
