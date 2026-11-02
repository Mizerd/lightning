
#include "updater/ArtifactDigest.h"
#include "updater/AtomicReplace.h"
#include "updater/InstallStrategies.h"
#include "updater/ProcessWaiter.h"
#include "updater/RelaunchEnvironment.h"
#include "updater/SafeArchive.h"
#include "storage/PortableMode.h"
#include "updater/UpdaterArgs.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTextStream>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

enum ExitCode {
    ExitSuccess = 0,
    ExitInvalidArguments = 2,
    ExitProcessWaitFailed = 3,
    ExitNotSelfInstallable = 4,
    ExitNoPackageManager = 5,
    ExitArchiveFailed = 6,
    ExitReplaceFailed = 7,
    ExitInstallerFailed = 8,
    ExitInternalError = 9,
    ExitArtifactDigestMismatch = 10,
};

const QString kDigestMismatchStatus = QStringLiteral("artifact-digest-mismatch");

const QString kPortableExecutableName = QStringLiteral("Lightning.exe");

constexpr int kInstallerTimeoutMs = 15 * 60 * 1000;

void writeStderr(const QString &line)
{
    QTextStream stream(stderr);
    stream << line << Qt::endl;
}

// Exactly four fields, all from our own enums or fixed strings: never a path,
// token or process output.
void writeStatus(const QString &statusPath, bool ok, const QString &mode,
                 const QString &error)
{
    if (statusPath.isEmpty())
        return;

    QJsonObject object;
    object.insert(QStringLiteral("ok"), ok);
    object.insert(QStringLiteral("mode"), mode);
    object.insert(QStringLiteral("error"), error);
    object.insert(QStringLiteral("timestamp"),
                  QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

    QSaveFile file(statusPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        writeStderr(QStringLiteral("lightning-updater: cannot write the status file"));
        return;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    if (!file.commit())
        writeStderr(QStringLiteral("lightning-updater: cannot commit the status file"));
}

constexpr int kInstallerDidNotStart = -1;
constexpr int kInstallerTimedOut = -2;
constexpr int kInstallerCrashed = -3;
constexpr int kElevationDeclined = -4;
constexpr int kLockedArtifactMismatch = -5;
constexpr int kLockedArtifactPathUnresolved = -6;

#ifdef Q_OS_WIN

// Opens `path` with FILE_SHARE_READ only, so it cannot be written, deleted or
// renamed (nor its directory renamed) while the handle lives, and hashes it
// through that handle. Returns the handle only when the digest matches. The
// elevated reader opens it with read sharing, which this permits.
HANDLE lockVerifiedArtifact(const QString &path, const QString &expectedSha256)
{
    const std::wstring native = path.toStdWString();
    HANDLE file = CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return INVALID_HANDLE_VALUE;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray chunk(1024 * 1024, Qt::Uninitialized);
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file, chunk.data(), DWORD(chunk.size()), &got, nullptr)) {
            CloseHandle(file);
            return INVALID_HANDLE_VALUE;
        }
        if (got == 0)
            break;
        hash.addData(QByteArrayView(chunk.constData(), qsizetype(got)));
    }
    if (QString::fromLatin1(hash.result().toHex()) != expectedSha256) {
        CloseHandle(file);
        return INVALID_HANDLE_VALUE;
    }
    return file;
}

int launchElevatedAndWait(const updater::InstallPlan &plan)
{
    bool ok = false;
    const QString parameters = updater::windowsCommandLine(plan.arguments, &ok);
    if (!ok)
        return kInstallerDidNotStart;
    const std::wstring file = plan.program.toStdWString();
    const std::wstring params = parameters.toStdWString();
    const std::wstring directory = plan.workingDirectory.toStdWString();

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = file.c_str();
    info.lpParameters = params.c_str();
    info.lpDirectory = directory.empty() ? nullptr : directory.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        return GetLastError() == ERROR_CANCELLED ? kElevationDeclined
                                                 : kInstallerDidNotStart;
    }
    if (!info.hProcess)
        return kInstallerDidNotStart;
    const DWORD waited = WaitForSingleObject(info.hProcess, DWORD(kInstallerTimeoutMs));
    if (waited != WAIT_OBJECT_0) {
        CloseHandle(info.hProcess);
        return kInstallerTimedOut;
    }
    DWORD exitCode = 0;
    const BOOL gotCode = GetExitCodeProcess(info.hProcess, &exitCode);
    CloseHandle(info.hProcess);
    if (!gotCode || exitCode > 0x7fffffffUL)
        return kInstallerCrashed;
    return int(exitCode);
}

QString finalPathOf(HANDLE file)
{
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    std::wstring buffer(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 2; ++attempt) {
        const DWORD length = GetFinalPathNameByHandleW(file, buffer.data(),
                                                       DWORD(buffer.size()), flags);
        if (length == 0)
            return QString();
        if (length < buffer.size())
            return QString::fromWCharArray(buffer.data(), int(length));
        buffer.assign(length + 1, L'\0');
    }
    return QString();
}

int runElevatedWindowsInstaller(const updater::InstallPlan &plan,
                                const QString &expectedSha256)
{
    const HANDLE locked = lockVerifiedArtifact(plan.lockedArtifact, expectedSha256);
    if (locked == INVALID_HANDLE_VALUE)
        return kLockedArtifactMismatch;
    // Launch the locked file's resolved path, not the original string: a
    // junction in a parent directory could be re-pointed after locking.
    updater::InstallPlan retargeted = plan;
    const QString launchable = updater::launchablePathFromFinal(finalPathOf(locked));
    int result = kLockedArtifactPathUnresolved;
    if (updater::retargetPlanToLockedFile(retargeted, launchable))
        result = launchElevatedAndWait(retargeted);
    CloseHandle(locked);
    return result;
}
#endif

int runExternalInstaller(const updater::InstallPlan &plan, const QString &expectedSha256)
{
#ifdef Q_OS_WIN
    if (plan.elevateOnWindows)
        return runElevatedWindowsInstaller(plan, expectedSha256);
#else
    Q_UNUSED(expectedSha256);
#endif
    QProcess process;
    process.setProgram(plan.program);
    process.setArguments(plan.arguments);
    if (!plan.workingDirectory.isEmpty())
        process.setWorkingDirectory(plan.workingDirectory);
    process.setProcessChannelMode(QProcess::ForwardedChannels);

    process.start();
    if (!process.waitForStarted(30000))
        return kInstallerDidNotStart;
    if (!process.waitForFinished(kInstallerTimeoutMs)) {
        process.kill();
        process.waitForFinished(5000);
        return kInstallerTimedOut;
    }
    if (process.exitStatus() != QProcess::NormalExit)
        return kInstallerCrashed;
    return process.exitCode();
}

updater::ReplaceResult runPortableSwap(const updater::UpdaterArguments &args,
                                       QString *archiveError)
{
    const QString workRoot = QDir(args.targetPath).absoluteFilePath(
        QString::fromLatin1(lightning::portable::kDataDirName)
        + QStringLiteral("/update-work"));
    if (!QDir().mkpath(workRoot)) {
        *archiveError = QStringLiteral("work-dir-unusable");
        updater::ReplaceResult failure;
        failure.error = updater::ReplaceError::TargetNotWritable;
        failure.message = *archiveError;
        return failure;
    }
    QDir stale(updater::portableBackupPath(args.targetPath));
    if (stale.exists())
        stale.removeRecursively();

    QTemporaryDir staging(QDir(workRoot).absoluteFilePath(
        QStringLiteral("staging-")));
    staging.setAutoRemove(true);
    if (!staging.isValid()) {
        *archiveError = QStringLiteral("staging-dir-unusable");
        updater::ReplaceResult failure;
        failure.error = updater::ReplaceError::TargetNotWritable;
        failure.message = *archiveError;
        return failure;
    }

    const updater::ArchiveResult extracted =
        updater::extractZipSafely(args.artifactPath, staging.path());
    if (!extracted.ok()) {
        *archiveError = QString::fromLatin1(updater::archiveErrorName(extracted.error));
        updater::ReplaceResult failure;
        failure.error = updater::ReplaceError::LayoutInvalid;
        failure.message = extracted.message;
        return failure;
    }

    const QString backup = updater::portableBackupPath(args.targetPath);
    return updater::swapDirectory(staging.path(), args.targetPath, backup,
                                  kPortableExecutableName,
                                  QStringList{
                                      QString::fromLatin1(
                                          lightning::portable::kDataDirName)});
}

updater::ReplaceResult runAppImageReplace(const updater::UpdaterArguments &args)
{
    QFile::Permissions permissions = QFile::permissions(args.artifactPath);
    permissions |= QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
                 | QFile::ReadUser | QFile::WriteUser | QFile::ExeUser;
    QFile::setPermissions(args.artifactPath, permissions);

    const QString backup = args.targetPath + QStringLiteral(".lightning-previous");
    return updater::replaceFileAtomically(args.artifactPath, args.targetPath,
                                          backup);
}

}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("lightning-updater"));

    const QStringList arguments = QCoreApplication::arguments().mid(1);

    const updater::ArgsParseResult parsed = updater::parseUpdaterArgs(arguments);
    if (!parsed.ok()) {
        const QString statusPath = updater::preScanStatusPath(arguments);
        writeStatus(statusPath, false, QStringLiteral("invalid"),
                    updater::parseErrorName(parsed.error));
        writeStderr(QStringLiteral("lightning-updater: %1 (%2 %3)")
                        .arg(parsed.message,
                             updater::parseErrorName(parsed.error),
                             parsed.offendingOption));
        return ExitInvalidArguments;
    }

    const updater::UpdaterArguments &args = parsed.args;
    const QString mode = args.modeString;

    const updater::WaitResult waited =
        updater::waitForProcessExit(args.pid, updater::kDefaultWaitTimeoutMs);
    if (waited != updater::WaitResult::Exited) {
        writeStatus(args.statusPath, false, mode,
                    QString::fromLatin1(updater::waitResultName(waited)));
        writeStderr(QStringLiteral("lightning-updater: the application did not exit"));
        return ExitProcessWaitFailed;
    }

    const QString digest = updater::sha256HexOfFile(args.artifactPath);
    if (digest.isEmpty() || digest != args.expectedSha256) {
        writeStatus(args.statusPath, false, mode, kDigestMismatchStatus);
        writeStderr(QStringLiteral(
            "lightning-updater: the update file no longer matches the signed release"));
        return ExitArtifactDigestMismatch;
    }

    const updater::StrategyResult strategy = updater::planForMode(args);
    if (!strategy.ok()) {
        writeStatus(args.statusPath, false, mode,
                    QString::fromLatin1(updater::strategyErrorName(strategy.error)));
        writeStderr(QStringLiteral("lightning-updater: %1").arg(strategy.message));
        return strategy.error == updater::StrategyError::NoPackageManagerFound
                       || strategy.error == updater::StrategyError::ElevationHelperMissing
                   ? ExitNoPackageManager
                   : ExitNotSelfInstallable;
    }

    int failureExit = ExitInternalError;
    QString failureCode;

    if (strategy.plan.requiresExternalProcess) {
        const int installerExit = runExternalInstaller(strategy.plan, args.expectedSha256);
    const bool installerRebootPending =
        installerExit == 3010 || installerExit == 1641;
    if (installerExit != 0 && !installerRebootPending) {
            failureExit = ExitInstallerFailed;
            if (installerExit == kElevationDeclined) {
                failureCode = QStringLiteral("elevation-declined");
            } else if (installerExit == kLockedArtifactPathUnresolved) {
                failureCode = QStringLiteral("unsafe-artifact-path");
            } else if (installerExit == kLockedArtifactMismatch) {
                failureExit = ExitArtifactDigestMismatch;
                failureCode = kDigestMismatchStatus;
            } else
                failureCode = installerExit < 0
                                  ? QStringLiteral("installer-did-not-run")
                                  : QStringLiteral("installer-exit-%1").arg(installerExit);
        }
    } else if (args.mode == updater::UpdaterMode::WindowsPortable) {
        QString archiveError;
        const updater::ReplaceResult swapped = runPortableSwap(args, &archiveError);
        if (!swapped.ok()) {
            failureExit = archiveError.isEmpty() ? ExitReplaceFailed
                                                 : ExitArchiveFailed;
            failureCode = archiveError.isEmpty()
                              ? QString::fromLatin1(
                                    updater::replaceErrorName(swapped.error))
                              : archiveError;
        }
    } else if (args.mode == updater::UpdaterMode::LinuxAppImage) {
        const updater::ReplaceResult replaced = runAppImageReplace(args);
        if (!replaced.ok()) {
            failureExit = ExitReplaceFailed;
            failureCode =
                QString::fromLatin1(updater::replaceErrorName(replaced.error));
        }
    } else {
        failureExit = ExitInternalError;
        failureCode = QStringLiteral("unhandled-mode");
    }

    if (!failureCode.isEmpty()) {
        writeStatus(args.statusPath, false, mode, failureCode);
        writeStderr(QStringLiteral("lightning-updater: install failed (%1)")
                        .arg(failureCode));
        return failureExit;
    }

    writeStatus(args.statusPath, true, mode, QString());

    if (args.relaunchRequested()) {
        const auto changes = updater::relaunchEnvironmentChanges(
            args.mode == updater::UpdaterMode::LinuxAppImage,
            [](const QString &name) { return qEnvironmentVariable(name.toLatin1().constData()); },
            [](const QString &path) { return QFileInfo(path).isDir(); });
        for (const auto &change : changes) {
            const QByteArray name = change.name.toLatin1();
            if (change.remove)
                qunsetenv(name.constData());
            else
                qputenv(name.constData(), change.value.toLocal8Bit());
            writeStderr(QStringLiteral("lightning-updater: relaunch environment: %1")
                            .arg(change.reason));
        }
        if (!QProcess::startDetached(args.relaunchPath, QStringList(),
                                     QFileInfo(args.relaunchPath).absolutePath())) {
            writeStderr(QStringLiteral("lightning-updater: the update succeeded but "
                                       "Lightning could not be relaunched"));
        }
    }
    return ExitSuccess;
}
