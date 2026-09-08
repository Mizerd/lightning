#include "updater/RelaunchEnvironment.h"

#include <QLatin1String>
#include <QStringList>

namespace updater {
namespace {

// The four spellings a program may consult for its temporary directory. Qt
// reads TMPDIR on Unix and TEMP/TMP on Windows; the AppImage runtime reads
// TMPDIR. Dropping one and leaving another pointing at the same dead
// directory would fix half the problem.
constexpr const char *kTemporaryVariables[] = {"TMPDIR", "TMP", "TEMP", "TEMPDIR"};

// The name an AppImage's own runtime gives its mount point. AppRun exports
// APPDIR as that directory, so the prefix is the difference between "mounted
// by the runtime" and "unpacked by something else".
constexpr QLatin1String kMountPrefix(".mount_");

QString lastPathSegment(const QString &path)
{
    QString trimmed = path;
    while (trimmed.size() > 1 && trimmed.endsWith(QLatin1Char('/')))
        trimmed.chop(1);
    const qsizetype slash = trimmed.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? trimmed : trimmed.mid(slash + 1);
}

} // namespace

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

    // Already decided by whoever started us. Their choice wins, either way.
    if (!value(QStringLiteral("APPIMAGE_EXTRACT_AND_RUN")).isEmpty())
        return changes;

    const QString appDir = value(QStringLiteral("APPDIR"));
    // No APPDIR is no evidence. Leave the relaunch exactly as it has always
    // been rather than imposing an extraction on a guess.
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

} // namespace updater
