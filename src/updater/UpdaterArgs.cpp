#include "updater/UpdaterArgs.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

namespace updater {
namespace {

// A generous but finite ceiling. Real paths are far shorter; anything past
// this is either a bug or an attempt to overflow something downstream.
constexpr int kMaxValueLength = 4096;

ArgsParseResult fail(ArgsError error, const QString &message,
                     const QString &option = QString())
{
    ArgsParseResult result;
    result.error = error;
    result.message = message;
    result.offendingOption = option;
    return result;
}

} // namespace

QString modeToString(UpdaterMode mode)
{
    for (const detail::ModeName &entry : detail::kModeNames) {
        if (entry.mode == mode)
            return QString::fromLatin1(entry.name);
    }
    return QStringLiteral("invalid");
}

bool isInProcessMode(UpdaterMode mode)
{
    return mode == UpdaterMode::WindowsPortable
        || mode == UpdaterMode::LinuxAppImage;
}

bool valueIsUnsafe(const QString &value)
{
    if (value.isEmpty())
        return true;
    if (value.size() > kMaxValueLength)
        return true;
    for (const QChar ch : value) {
        // Rejects NUL, tab, newline, carriage return, and every other C0/C1
        // control character. A legitimate filesystem path never needs one.
        if (ch.unicode() < 0x20 || ch.unicode() == 0x7F)
            return true;
        if (ch.unicode() >= 0x80 && ch.unicode() <= 0x9F)
            return true;
    }
    return false;
}

bool pathIsAbsolute(const QString &path)
{
    if (path.isEmpty())
        return false;
    return QFileInfo(path).isAbsolute();
}

bool pathContainsParentComponent(const QString &path)
{
    const QStringList components =
        path.split(QRegularExpression(QStringLiteral("[/\\\\]")));
    return components.contains(QStringLiteral(".."));
}

QString parseErrorName(ArgsError error)
{
    switch (error) {
    case ArgsError::None: return QStringLiteral("none");
    case ArgsError::EmptyArguments: return QStringLiteral("empty-arguments");
    case ArgsError::UnknownOption: return QStringLiteral("unknown-option");
    case ArgsError::PositionalArgument: return QStringLiteral("positional-argument");
    case ArgsError::DuplicateOption: return QStringLiteral("duplicate-option");
    case ArgsError::MissingValue: return QStringLiteral("missing-value");
    case ArgsError::MissingRequiredOption: return QStringLiteral("missing-required-option");
    case ArgsError::EmptyValue: return QStringLiteral("empty-value");
    case ArgsError::UnsafeValue: return QStringLiteral("unsafe-value");
    case ArgsError::UnknownMode: return QStringLiteral("unknown-mode");
    case ArgsError::ModeNotSelfInstallable: return QStringLiteral("mode-not-self-installable");
    case ArgsError::PathNotAbsolute: return QStringLiteral("path-not-absolute");
    case ArgsError::PathTraversal: return QStringLiteral("path-traversal");
    case ArgsError::PathDoesNotExist: return QStringLiteral("path-does-not-exist");
    case ArgsError::PathNotAFile: return QStringLiteral("path-not-a-file");
    case ArgsError::PathNotADirectory: return QStringLiteral("path-not-a-directory");
    case ArgsError::PathIsSymlink: return QStringLiteral("path-is-symlink");
    case ArgsError::ArtifactEmpty: return QStringLiteral("artifact-empty");
    case ArgsError::StatusParentMissing: return QStringLiteral("status-parent-missing");
    case ArgsError::InvalidPid: return QStringLiteral("invalid-pid");
    }
    return QStringLiteral("unknown");
}

QString preScanStatusPath(const QStringList &arguments)
{
    QString found;
    int seen = 0;
    for (int i = 0; i + 1 < arguments.size(); ++i) {
        if (arguments.at(i) != QLatin1String("--status"))
            continue;
        ++seen;
        found = arguments.at(i + 1);
    }
    if (seen != 1)
        return QString();
    if (valueIsUnsafe(found) || !pathIsAbsolute(found)
        || pathContainsParentComponent(found))
        return QString();
    const QFileInfo info(found);
    const QFileInfo parent(info.absolutePath());
    if (!parent.exists() || !parent.isDir())
        return QString();
    if (info.isDir() || info.isSymLink())
        return QString();
    return info.absoluteFilePath();
}

ArgsParseResult parseUpdaterArgs(const QStringList &arguments)
{
    if (arguments.isEmpty())
        return fail(ArgsError::EmptyArguments,
                    QStringLiteral("no arguments supplied"));

    static const QStringList kKnownOptions = {
        QStringLiteral("--mode"),   QStringLiteral("--artifact"),
        QStringLiteral("--pid"),    QStringLiteral("--target"),
        QStringLiteral("--relaunch"), QStringLiteral("--status"),
    };
    // --relaunch is the only optional option: absent means "do not relaunch".
    // Everything else must be supplied exactly once.
    static const QStringList kRequiredOptions = {
        QStringLiteral("--mode"),   QStringLiteral("--artifact"),
        QStringLiteral("--pid"),    QStringLiteral("--target"),
        QStringLiteral("--status"),
    };

    QHash<QString, QString> values;

    for (int i = 0; i < arguments.size(); ++i) {
        const QString &token = arguments.at(i);
        if (!token.startsWith(QLatin1String("--")))
            return fail(ArgsError::PositionalArgument,
                        QStringLiteral("unexpected positional argument"),
                        token.left(64));
        if (!kKnownOptions.contains(token))
            return fail(ArgsError::UnknownOption,
                        QStringLiteral("unrecognised option"), token.left(64));
        if (values.contains(token))
            return fail(ArgsError::DuplicateOption,
                        QStringLiteral("option supplied more than once"), token);
        if (i + 1 >= arguments.size())
            return fail(ArgsError::MissingValue,
                        QStringLiteral("option requires a value"), token);

        const QString value = arguments.at(i + 1);
        ++i;

        // A value that itself looks like an option is always a mistake here:
        // every value in this contract is either an absolute path or a
        // decimal integer, and neither can start with "--".
        if (value.startsWith(QLatin1String("--")))
            return fail(ArgsError::MissingValue,
                        QStringLiteral("option value looks like another option"),
                        token);
        if (value.isEmpty())
            return fail(ArgsError::EmptyValue,
                        QStringLiteral("option value is empty"), token);
        if (valueIsUnsafe(value))
            return fail(ArgsError::UnsafeValue,
                        QStringLiteral("option value contains control characters "
                                       "or is too long"),
                        token);
        values.insert(token, value);
    }

    for (const QString &option : kRequiredOptions) {
        if (!values.contains(option))
            return fail(ArgsError::MissingRequiredOption,
                        QStringLiteral("required option missing"), option);
    }

    // No path in this contract may carry a ".." component. Lightning always
    // passes a fully resolved path, and without this a value such as
    // "/staging/../../etc/passwd" would silently clean itself into a
    // directory that does exist and then pass every later check.
    for (const QString &option :
         {QStringLiteral("--artifact"), QStringLiteral("--target"),
          QStringLiteral("--relaunch"), QStringLiteral("--status")}) {
        if (!values.contains(option))
            continue; // only --relaunch can be absent here
        if (pathContainsParentComponent(values.value(option)))
            return fail(ArgsError::PathTraversal,
                        QStringLiteral("path contains a \"..\" component"), option);
    }

    UpdaterArguments args;

    // --mode
    args.modeString = values.value(QStringLiteral("--mode"));
    args.mode = modeFromString(args.modeString);
    if (args.mode == UpdaterMode::Invalid)
        return fail(ArgsError::UnknownMode,
                    QStringLiteral("mode is not a known install-type identifier"),
                    QStringLiteral("--mode"));
    if (!isSelfInstallable(args.mode))
        return fail(ArgsError::ModeNotSelfInstallable,
                    QStringLiteral("this install type is never installed by the "
                                   "helper"),
                    QStringLiteral("--mode"));

    // --pid: strictly decimal digits, no sign, no whitespace, > 1.
    const QString pidValue = values.value(QStringLiteral("--pid"));
    for (const QChar ch : pidValue) {
        if (ch < QLatin1Char('0') || ch > QLatin1Char('9'))
            return fail(ArgsError::InvalidPid,
                        QStringLiteral("pid must be decimal digits only"),
                        QStringLiteral("--pid"));
    }
    if (pidValue.size() > 19)
        return fail(ArgsError::InvalidPid, QStringLiteral("pid is out of range"),
                    QStringLiteral("--pid"));
    bool pidOk = false;
    const qint64 pid = pidValue.toLongLong(&pidOk);
    if (!pidOk || pid <= 1)
        return fail(ArgsError::InvalidPid,
                    QStringLiteral("pid must be a positive process id"),
                    QStringLiteral("--pid"));
    args.pid = pid;

    // --artifact: absolute, exists, regular file, non-empty.
    args.artifactPath = values.value(QStringLiteral("--artifact"));
    if (!pathIsAbsolute(args.artifactPath))
        return fail(ArgsError::PathNotAbsolute,
                    QStringLiteral("artifact path must be absolute"),
                    QStringLiteral("--artifact"));
    {
        const QFileInfo info(args.artifactPath);
        if (!info.exists())
            return fail(ArgsError::PathDoesNotExist,
                        QStringLiteral("artifact does not exist"),
                        QStringLiteral("--artifact"));
        if (!info.isFile())
            return fail(ArgsError::PathNotAFile,
                        QStringLiteral("artifact is not a regular file"),
                        QStringLiteral("--artifact"));
        if (info.size() <= 0)
            return fail(ArgsError::ArtifactEmpty,
                        QStringLiteral("artifact is empty"),
                        QStringLiteral("--artifact"));
        args.artifactPath = info.absoluteFilePath();
    }

    // --target: absolute and existing. A portable swap targets a directory;
    // an AppImage replacement targets the running AppImage file. For the
    // installer-driven modes the target is informational, but the contract is
    // fixed so it is still validated rather than ignored.
    args.targetPath = values.value(QStringLiteral("--target"));
    if (!pathIsAbsolute(args.targetPath))
        return fail(ArgsError::PathNotAbsolute,
                    QStringLiteral("target path must be absolute"),
                    QStringLiteral("--target"));
    {
        const QFileInfo info(args.targetPath);
        if (!info.exists())
            return fail(ArgsError::PathDoesNotExist,
                        QStringLiteral("target does not exist"),
                        QStringLiteral("--target"));
        if (args.mode == UpdaterMode::WindowsPortable && !info.isDir())
            return fail(ArgsError::PathNotADirectory,
                        QStringLiteral("portable target must be a directory"),
                        QStringLiteral("--target"));
        if (args.mode == UpdaterMode::LinuxAppImage && !info.isFile())
            return fail(ArgsError::PathNotAFile,
                        QStringLiteral("AppImage target must be a regular file"),
                        QStringLiteral("--target"));
        args.targetPath = info.absoluteFilePath();
    }

    // --relaunch: OPTIONAL. Absent leaves relaunchPath empty, which the helper
    // reads as "install, then start nothing". When supplied it is validated
    // exactly as strictly as the required paths -- absolute, existing, and a
    // regular file -- so making it optional relaxes no check on the value.
    if (values.contains(QStringLiteral("--relaunch"))) {
        args.relaunchPath = values.value(QStringLiteral("--relaunch"));
        if (!pathIsAbsolute(args.relaunchPath))
            return fail(ArgsError::PathNotAbsolute,
                        QStringLiteral("relaunch path must be absolute"),
                        QStringLiteral("--relaunch"));
        const QFileInfo info(args.relaunchPath);
        if (!info.exists())
            return fail(ArgsError::PathDoesNotExist,
                        QStringLiteral("relaunch target does not exist"),
                        QStringLiteral("--relaunch"));
        if (!info.isFile())
            return fail(ArgsError::PathNotAFile,
                        QStringLiteral("relaunch target is not a regular file"),
                        QStringLiteral("--relaunch"));
        args.relaunchPath = info.absoluteFilePath();
    }

    // --status: absolute, parent exists, and the path itself must not already
    // be a directory or a symlink (a symlink would redirect the write).
    args.statusPath = values.value(QStringLiteral("--status"));
    if (!pathIsAbsolute(args.statusPath))
        return fail(ArgsError::PathNotAbsolute,
                    QStringLiteral("status path must be absolute"),
                    QStringLiteral("--status"));
    {
        const QFileInfo info(args.statusPath);
        const QFileInfo parent(info.absolutePath());
        if (!parent.exists() || !parent.isDir())
            return fail(ArgsError::StatusParentMissing,
                        QStringLiteral("status directory does not exist"),
                        QStringLiteral("--status"));
        if (info.isDir())
            return fail(ArgsError::PathNotAFile,
                        QStringLiteral("status path is a directory"),
                        QStringLiteral("--status"));
        if (info.isSymLink())
            return fail(ArgsError::PathIsSymlink,
                        QStringLiteral("status path is a symbolic link"),
                        QStringLiteral("--status"));
        args.statusPath = QDir(parent.absoluteFilePath())
                              .absoluteFilePath(info.fileName());
    }

    ArgsParseResult result;
    result.args = args;
    return result;
}

} // namespace updater
