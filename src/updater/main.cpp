// lightning-updater — the standalone update helper.
//
// This binary knows NOTHING about Matrix, accounts, tokens, stores, or the
// network. It never downloads anything and it never opens a socket. It is
// handed a local artifact that Lightning has already verified (Ed25519
// manifest signature -> SHA-256 of the exact bytes), waits for Lightning to
// exit, performs exactly one install operation, writes a small non-sensitive
// status file, and — only when --relaunch was supplied — starts the
// application again. Lightning reads that status file on its next launch and
// reports the outcome; without it, every post-handoff failure is invisible.
//
// Its argument vector is fixed and fully enumerated (UPDATE-SPEC §11). It
// accepts no command strings from anywhere, and every external process it
// starts is launched with a program path plus a QStringList argument vector —
// never a command line, never a shell.

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

// Exit codes. 0 is the only success. Everything else is a specific, typed
// failure so a log or a support report can name what went wrong.
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
    // The artifact on disk no longer hashes to what the signed manifest said
    // it should. Nothing was installed.
    ExitArtifactDigestMismatch = 10,
};

// The status token the application reads back for that refusal. Kept as one
// literal here and in UpdateManager, which writes the same token itself when
// ITS pre-launch re-hash fails on the install-on-quit path.
const QString kDigestMismatchStatus = QStringLiteral("artifact-digest-mismatch");

// The executable the portable Windows archive must contain. The build produces
// lightning-matrix.exe, but packaging renames it on staging -- see
// lightning-deploy scripts/stage-windows-runtime.py:79, which copies it to
// Lightning.exe, and build-windows.sh:249, which zips the staged tree from a
// single top-level "Lightning" directory (swapDirectory unwraps that).
// Getting this wrong makes every portable update refuse with LayoutInvalid.
const QString kPortableExecutableName = QStringLiteral("Lightning.exe");

// How long an installer may run before we give up waiting on it. An MSI or
// NSIS run on a slow disk can legitimately take minutes.
constexpr int kInstallerTimeoutMs = 15 * 60 * 1000;

void writeStderr(const QString &line)
{
    QTextStream stream(stderr);
    stream << line << Qt::endl;
}

// The status file carries exactly four fields, all of them derived from our
// own enums or from a fixed string. It never contains a path, a token, a
// process output, or anything else that would be unsafe to share.
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

// runExternalInstaller's own negative codes. Anything >= 0 is the installer's
// exit code.
constexpr int kInstallerDidNotStart = -1;
constexpr int kInstallerTimedOut = -2;
constexpr int kInstallerCrashed = -3;
// The UAC prompt for a per-machine upgrade was declined (or could not be
// answered: a standard account with no administrator to type a password).
constexpr int kElevationDeclined = -4;
// The artifact could not be locked, or its bytes read through the lock no
// longer match the signed digest. Nothing was launched.
constexpr int kLockedArtifactMismatch = -5;
// The locked file's final path could not be resolved into one the installer
// can be launched with. Nothing was launched.
constexpr int kLockedArtifactPathUnresolved = -6;

#ifdef Q_OS_WIN
// A PER-MACHINE upgrade, and nothing else, comes through here. CreateProcess
// (what QProcess uses) cannot raise a UAC prompt; ShellExecuteEx with "runas"
// is the documented way to start a program elevated, and it is the ONE place
// in this helper where the argument vector has to become a single string --
// built by windowsCommandLine(), which refuses rather than escapes anything a
// path cannot contain.
//
// COM is deliberately not initialised: it is needed when ShellExecuteEx may
// hand the verb to a shell extension or DDE, and neither applies to "runas" on
// an .exe (msiexec or the NSIS setup).

// Opens `path` so that nobody can write, delete or rename it while the handle
// lives (FILE_SHARE_READ only), and hashes it THROUGH that handle. Returns the
// handle only when the digest matches; INVALID_HANDLE_VALUE otherwise. While
// the file is open its directory cannot be renamed either, so the path keeps
// naming these bytes. The elevated reader (msiexec, or the loader mapping the
// setup EXE) opens it for reading with read sharing, which this permits.
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
        // An elevated process cannot be terminated from this unelevated one,
        // so a timeout here only stops WAITING; it is reported as a failure
        // exactly like the unelevated path's timeout.
        CloseHandle(info.hProcess);
        return kInstallerTimedOut;
    }
    DWORD exitCode = 0;
    const BOOL gotCode = GetExitCodeProcess(info.hProcess, &exitCode);
    CloseHandle(info.hProcess);
    // An NTSTATUS crash code (0xC0000005 &c.) does not fit a positive int and
    // must not read as one of the negative codes above.
    if (!gotCode || exitCode > 0x7fffffffUL)
        return kInstallerCrashed;
    return int(exitCode);
}

// The path of the object `file` is open on, with every junction and link on
// the way resolved. Empty on failure.
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
        buffer.assign(length + 1, L'\0');   // too small: `length` is what it needs
    }
    return QString();
}

int runElevatedWindowsInstaller(const updater::InstallPlan &plan,
                                const QString &expectedSha256)
{
    // Held from BEFORE the UAC prompt until the elevated process has exited.
    const HANDLE locked = lockVerifiedArtifact(plan.lockedArtifact, expectedSha256);
    if (locked == INVALID_HANDLE_VALUE)
        return kLockedArtifactMismatch;
    // Launch the file that is LOCKED, not the string that found it: a junction
    // in a parent directory can be re-pointed after the lock is taken, and the
    // elevated installer would open whatever the string names by then.
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
    process.setArguments(plan.arguments);   // argument VECTOR, never a string
    if (!plan.workingDirectory.isEmpty())
        process.setWorkingDirectory(plan.workingDirectory);
    // The installer's own output is not captured into anything we persist.
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
    // Extract into a private staging directory INSIDE the portable tree.
    //
    // This used to live in the PARENT of the folder, together with the
    // backup below, which broke the one promise portable mode makes: after a
    // successful update a USB stick was left holding
    //   USB:\Lightning\  and  USB:\lightning-previous-version\
    // and the update required the PARENT to be writable, not just the folder.
    //
    // `data` is the single name swapDirectory PRESERVES, so a directory under
    // it is not moved into the backup by step 2 and its path stays valid for
    // the whole swap. It is also inside the target, so the promote is still a
    // same-filesystem rename.
    const QString workRoot = QDir(args.targetPath).absoluteFilePath(
        QString::fromLatin1(lightning::portable::kDataDirName)
        + QStringLiteral("/update-work"));
    if (!QDir().mkpath(workRoot)) {
        // A TOKEN, not a sentence. This field is written verbatim into the
        // status file's `error`, which the UI renders as "Code: …" — a
        // sentence there reads as a code and has no explanation attached.
        *archiveError = QStringLiteral("work-dir-unusable");
        updater::ReplaceResult failure;
        failure.error = updater::ReplaceError::TargetNotWritable;
        failure.message = *archiveError;
        return failure;
    }
    // A previous-version directory left by the LAST update. On Windows step 4
    // cannot delete it while the updater's own moved DLLs are mapped, so it is
    // deliberately left behind — which means clearing it is this run's job.
    // swapDirectory refuses to start when its backup path already exists, so
    // without this the SECOND update always fails.
    //
    // It has to be the path the swap ACTUALLY uses. This cleared
    // `<target>/data/update-work/previous-version` for a while after the
    // backup had moved to a sibling of the installation, so it swept a
    // location nothing writes any more and left the real backup in place —
    // the second update then failed on a directory this code believed it had
    // already removed. swapDirectory clears a stale backup itself, so this is
    // belt and braces; it is kept because a leftover under the user's own
    // folder is worth removing early, and it now names one source of truth.
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

    // A SIBLING of the installation. It cannot live under `workRoot`, which
    // is inside the target: swapDirectory refuses an overlapping backup
    // before it moves anything, so that placement did not merely risk the
    // swap, it guaranteed the refusal. Reported as an "unknown error" on
    // restart when updating Windows portable 0.9.0 to 0.9.1, and reproduced
    // against the shipped helper: exit 7, `refused-unsafe-path`, the
    // executable untouched and no relaunch. The rule lives in AtomicReplace
    // so a test can assert the one the helper really uses.
    const QString backup = updater::portableBackupPath(args.targetPath);
    // The portable data directory NEVER takes part in the swap. It holds the
    // user's settings, their sealed Matrix session, the Rust SDK store and the
    // E2EE crypto store, all of which live inside the installation precisely
    // so the folder can be copied to another machine. Without this the swap
    // moves it into the backup and step 4 deletes it, and the promoted build
    // starts with no state: a fresh login and a NEW Matrix device, losing
    // access to everything encrypted to the old one.
    return updater::swapDirectory(staging.path(), args.targetPath, backup,
                                  kPortableExecutableName,
                                  QStringList{
                                      QString::fromLatin1(
                                          lightning::portable::kDataDirName)});
}

updater::ReplaceResult runAppImageReplace(const updater::UpdaterArguments &args)
{
    // The AppImage must be executable before it becomes the target; the
    // replace routine also enforces this, but setting it on the staged file
    // first means the target is never briefly non-executable.
    QFile::Permissions permissions = QFile::permissions(args.artifactPath);
    permissions |= QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
                 | QFile::ReadUser | QFile::WriteUser | QFile::ExeUser;
    QFile::setPermissions(args.artifactPath, permissions);

    const QString backup = args.targetPath + QStringLiteral(".lightning-previous");
    return updater::replaceFileAtomically(args.artifactPath, args.targetPath,
                                          backup);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("lightning-updater"));

    const QStringList arguments = QCoreApplication::arguments().mid(1);

    const updater::ArgsParseResult parsed = updater::parseUpdaterArgs(arguments);
    if (!parsed.ok()) {
        // Best effort: if a usable --status path was supplied, record the
        // refusal there as well so the application can show it.
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

    // Wait for Lightning to exit. Installing over a running application is
    // the failure this whole helper exists to avoid, so a timeout here is
    // terminal: we refuse rather than proceed.
    const updater::WaitResult waited =
        updater::waitForProcessExit(args.pid, updater::kDefaultWaitTimeoutMs);
    if (waited != updater::WaitResult::Exited) {
        writeStatus(args.statusPath, false, mode,
                    QString::fromLatin1(updater::waitResultName(waited)));
        writeStderr(QStringLiteral("lightning-updater: the application did not exit"));
        return ExitProcessWaitFailed;
    }

    // The application verified these bytes as it downloaded them, and it
    // re-hashed them before starting us -- but between that and here the
    // file has been sitting at a predictable path, and what follows is a
    // chmod, an archive extraction, or `pkexec dpkg -i <path>` as root. This
    // is the last moment before the bytes are consumed, so it is where the
    // digest is taken again. A mismatch installs nothing and says so.
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
        // 3010 and 1641 are SUCCESSES that ask for a restart, and 1602 is the
    // user cancelling. Treating every non-zero code as a refusal told people
    // the installer had rejected an update it had in fact applied, then
    // skipped the relaunch and offered the same update again.
    const bool installerRebootPending =
        installerExit == 3010 || installerExit == 1641;
    if (installerExit != 0 && !installerRebootPending) {
            failureExit = ExitInstallerFailed;
            // A declined UAC prompt is its own outcome: nothing is broken and
            // nothing was changed, and the person can act on it -- approve
            // the prompt next time, or ask an administrator. The setup EXE
            // reports the same thing as 1223 (ERROR_CANCELLED) when it had to
            // elevate itself, so both read the same to the application.
            if (installerExit == kElevationDeclined) {
                failureCode = QStringLiteral("elevation-declined");
            } else if (installerExit == kLockedArtifactPathUnresolved) {
                // A safety refusal ("unsafe-" is explained as one): the
                // locked file's real location is not one to launch.
                failureCode = QStringLiteral("unsafe-artifact-path");
            } else if (installerExit == kLockedArtifactMismatch) {
                // Same refusal, and same token, as the path-based re-hash
                // above: the bytes are not the signed ones (or could not be
                // held still to prove it). Nothing ran.
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

    // Relaunch ONLY when Lightning asked for it by passing --relaunch. A
    // plain "install when I quit" must not start the application back up.
    if (args.relaunchRequested()) {
        // The child inherits THIS process's environment, which is the one
        // Lightning was started with, and two things in it are wrong by now:
        // a private temporary directory that died with the shell that
        // launched us, and an AppImage that cannot mount itself where it is
        // about to be restarted. See RelaunchEnvironment.h — both were
        // measured, not assumed.
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
        // Empty argument vector — the helper never forwards anything it was
        // given to the application.
        if (!QProcess::startDetached(args.relaunchPath, QStringList(),
                                     QFileInfo(args.relaunchPath).absolutePath())) {
            writeStderr(QStringLiteral("lightning-updater: the update succeeded but "
                                       "Lightning could not be relaunched"));
        }
    }
    return ExitSuccess;
}
