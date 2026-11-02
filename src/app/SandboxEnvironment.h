#pragma once

#include <QByteArray>
#include <QByteArrayList>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace sandboxenv {

struct Environment
{
    QString flatpakId;
    QString container;
    QString snap;
    QString snapName;
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

inline QString flatpakInfoPath()
{
    return QStringLiteral("/.flatpak-info");
}

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
    QString appId;
    QString flatpakFilesystems;
};

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

inline Detection detectHost()
{
#if defined(Q_OS_LINUX)
    const Environment env = processEnvironment();
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

}
