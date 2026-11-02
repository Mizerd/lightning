#include "updater/RelaunchEnvironment.h"

#include <QLatin1String>
#include <QStringList>

namespace updater {
namespace {

// Every temp-directory variable a program may read (Qt: TMPDIR on Unix,
// TEMP/TMP on Windows; the AppImage runtime: TMPDIR). Fixing one while another
// still names the dead directory solves half the problem.
constexpr const char *kTemporaryVariables[] = {"TMPDIR", "TMP", "TEMP", "TEMPDIR"};

constexpr QLatin1String kMountPrefix(".mount_");

QString lastPathSegment(const QString &path)
{
    QString trimmed = path;
    while (trimmed.size() > 1 && trimmed.endsWith(QLatin1Char('/')))
        trimmed.chop(1);
    const qsizetype slash = trimmed.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? trimmed : trimmed.mid(slash + 1);
}

}

QList<RelaunchEnvironmentChange> relaunchEnvironmentChanges(
    bool appImageInstall,
    const std::function<QString(const QString &)> &value,
    const std::function<bool(const QString &)> &directoryExists)
{
    QList<RelaunchEnvironmentChange> changes;
    if (!value || !directoryExists)
        return changes;

    for (const char *name : kTemporaryVariables) {
        const QString variable = QString::fromLatin1(name);
        const QString current = value(variable);
        if (current.isEmpty())
            continue;
        if (directoryExists(current))
            continue;
        changes.append(RelaunchEnvironmentChange{
            variable, QString(), true,
            QStringLiteral("%1 named a directory that no longer exists (%2); the "
                           "relaunched process falls back to the system default")
                .arg(variable, current)});
    }

    if (!appImageInstall)
        return changes;

    if (!value(QStringLiteral("APPIMAGE_EXTRACT_AND_RUN")).isEmpty())
        return changes;

    const QString appDir = value(QStringLiteral("APPDIR"));
    if (appDir.isEmpty())
        return changes;
    if (lastPathSegment(appDir).startsWith(kMountPrefix))
        return changes;

    changes.append(RelaunchEnvironmentChange{
        QStringLiteral("APPIMAGE_EXTRACT_AND_RUN"), QStringLiteral("1"), false,
        QStringLiteral("this AppImage was started by an extractor rather than by its "
                       "own runtime (APPDIR is %1), so the relaunch cannot rely on "
                       "being able to mount itself")
            .arg(appDir)});
    return changes;
}

}
