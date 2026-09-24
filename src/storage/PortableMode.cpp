#include "storage/PortableMode.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLatin1String>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>
#include <QUuid>
#include <QDateTime>
#include <QLockFile>
#include <QTimeZone>
#include <QtGlobal>
#include <memory>
#include <unordered_map>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

#include <optional>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <mach-o/dyld.h>
#include <cstdlib>
#include <limits.h>
#include <vector>
#else
#include <limits.h>
#include <unistd.h>
#include <vector>
#endif

// PATH_MAX is not guaranteed by the C++ standard headers on every libc; the
// loops below grow past it anyway, so this is only a starting size.
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

namespace lightning::portable {

namespace {

// One mutex for all module state: isPortable() is also reached via
// primaryRoot() from the Rust backend's threads.
QMutex &stateMutex()
{
    static QMutex mutex;
    return mutex;
}

// Test override: sits IN FRONT of the real decision (see the header).
bool g_overrideActive = false;
bool g_overridePortable = false;
QString g_overrideExecutableDir;

// The real, once-per-process decision.
bool g_decided = false;
bool g_portable = false;

// Resolve the running executable's own path from the platform API. Returns an
// empty string on any failure; there is deliberately no argv[0] fallback.
QString resolveExecutablePath()
{
#if defined(Q_OS_WIN)
    // GetModuleFileNameW truncates rather than failing, so grow until the
    // result fits strictly inside the buffer.
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = ::GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0)
            return {};
        if (written < buffer.size()) {
            return QString::fromWCharArray(buffer.data(),
                                           static_cast<qsizetype>(written));
        }
        if (buffer.size() >= 32768) // Windows' own extended-path ceiling.
            return {};
        buffer.resize(buffer.size() * 2, L'\0');
    }
#elif defined(Q_OS_MACOS)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size); // Asks for the required size.
    if (size == 0)
        return {};
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    // realpath resolves symlinks and "..", so the directory compares with the
    // marker's resolved location.
    char resolved[PATH_MAX] = {};
    if (::realpath(buffer.data(), resolved) == nullptr)
        return QString::fromLocal8Bit(buffer.data());
    return QString::fromLocal8Bit(resolved);
#else
    // /proc/self/exe is resolved by the kernel. readlink() neither terminates
    // nor reports the needed size, so grow until the result fits.
    std::vector<char> buffer(PATH_MAX, '\0');
    for (;;) {
        const ssize_t written =
            ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (written < 0)
            return {};
        if (static_cast<size_t>(written) < buffer.size()) {
            return QString::fromLocal8Bit(buffer.data(),
                                          static_cast<qsizetype>(written));
        }
        if (buffer.size() >= 65536)
            return {};
        buffer.resize(buffer.size() * 2, '\0');
    }
#endif
}

// Cached: isPortable()/dataRoot() are on the startup path.
QString realExecutableDir()
{
    static const QString cached = [] {
        const QString exe = resolveExecutablePath();
        if (exe.isEmpty())
            return QString();
        const QString dir = QFileInfo(exe).absolutePath();
        return dir.isEmpty() ? QString() : QDir::cleanPath(dir);
    }();
    return cached;
}

// Development-only forcing. nullopt when unset or unrecognised: an unknown
// value must not silently mean "off".
std::optional<bool> environmentOverride()
{
    const QByteArray raw = qgetenv(kPortableEnvVar);
    if (raw.isEmpty())
        return std::nullopt;
    const QByteArray value = raw.trimmed().toLower();
    if (value == "1" || value == "true" || value == "on" || value == "yes")
        return true;
    if (value == "0" || value == "false" || value == "off" || value == "no") {
        // Accepted, but it can only stop a dev build from opting in; it never
        // turns a packaged portable installation off (see isPortable()).
        return false;
    }
    qWarning("LIGHTNING_PORTABLE is set to an unrecognised value; "
             "ignoring it and using the packaging marker instead.");
    return std::nullopt;
}

// executableDir(), honouring a test override. Caller holds stateMutex().
QString executableDirLocked()
{
    if (g_overrideActive)
        return g_overrideExecutableDir;
    return realExecutableDir();
}

} // namespace

QString executableDir()
{
    QMutexLocker locker(&stateMutex());
    return executableDirLocked();
}

bool markerPresentIn(const QString &dir)
{
    if (dir.trimmed().isEmpty())
        return false;
    const QFileInfo marker(QDir(dir).absoluteFilePath(
        QLatin1String(kMarkerFileName)));
    // isFile(), not exists(): a directory with that name is not the marker.
    return marker.isFile();
}

QString dataRootFor(const QString &executableDir)
{
    if (executableDir.trimmed().isEmpty())
        return {};
    return QDir::cleanPath(executableDir) + QLatin1Char('/')
           + QLatin1String(kDataDirName);
}

bool isPortable()
{
    QMutexLocker locker(&stateMutex());
    if (g_overrideActive)
        return g_overridePortable;
    if (!g_decided) {
        g_decided = true;
        // An unresolvable executable directory means "not portable", never the
        // working directory.
        const bool marked = markerPresentIn(realExecutableDir());
        // The marker wins. The environment can opt a development tree in, never
        // a packaged portable copy out: a stray LIGHTNING_PORTABLE=0 would
        // silently move its session, secrets and crypto store back to the
        // system locations.
        if (marked) {
            g_portable = true;
        } else if (const std::optional<bool> forced = environmentOverride()) {
            g_portable = *forced;
        } else {
            g_portable = false;
        }
    }
    return g_portable;
}

QString dataRoot()
{
    if (!isPortable())
        return {};
    QMutexLocker locker(&stateMutex());
    return dataRootFor(executableDirLocked());
}

namespace {

QString subdir(QLatin1String suffix)
{
    const QString root = dataRoot();
    if (root.isEmpty())
        return {};
    return root + suffix;
}

} // namespace

QString configDir()  { return subdir(QLatin1String("/config")); }
QString tempDir()    { return subdir(QLatin1String("/temp")); }
QString updateWorkDir() { return subdir(QLatin1String("/update-work")); }

namespace {
QString &scratchRootOverride()
{
    static QString root;
    return root;
}
} // namespace

void setMediaScratchRootOverrideForTest(const QString &root)
{
    QMutexLocker locker(&stateMutex());
    scratchRootOverride() = root;
}

QString mediaScratchRoot()
{
    {
        QMutexLocker locker(&stateMutex());
        if (!scratchRootOverride().isEmpty())
            return scratchRootOverride();
    }
    // One definition for every decrypted-media path, so nothing writes such
    // payloads outside a portable folder.
    if (isPortable()) {
        const QString dir = tempDir();
        if (!dir.isEmpty())
            return dir;
    }
    return QDir::tempPath();
}
QString secretsDir() { return subdir(QLatin1String("/secrets")); }
QString cacheDir()   { return subdir(QLatin1String("/cache")); }
QString logsDir()    { return subdir(QLatin1String("/logs")); }

void setPortableOverrideForTest(bool portable, const QString &executableDir)
{
    QMutexLocker locker(&stateMutex());
    g_overrideActive = true;
    g_overridePortable = portable;
    g_overrideExecutableDir =
        executableDir.trimmed().isEmpty() ? QString()
                                          : QDir::cleanPath(executableDir);
}

void clearPortableOverrideForTest()
{
    QMutexLocker locker(&stateMutex());
    g_overrideActive = false;
    g_overridePortable = false;
    g_overrideExecutableDir.clear();
    // Does not reset g_decided: nothing may change a running process's storage
    // location.
}

QString prepareDataRoot()
{
    if (!isPortable()) {
        // A programming error, not user-fixable; no user-facing wording.
        return QStringLiteral(
            "prepareDataRoot() called while not in portable mode.");
    }

    const QString root = dataRoot();
    if (root.isEmpty()) {
        return QStringLiteral(
            "Lightning portable could not determine its own program "
            "directory, so it does not know where to keep your data.");
    }

    // matrix/ is primaryRoot() in portable mode; the rest are this module's
    // subdirectories. Created up front so writers find them and a fresh folder
    // looks complete.
    const QStringList wanted = {
        root,
        root + QLatin1String("/config"),
        root + QLatin1String("/secrets"),
        root + QLatin1String("/cache"),
        root + QLatin1String("/logs"),
        root + QLatin1String("/matrix"),
        root + QLatin1String("/temp"),
    };
    for (const QString &dir : wanted) {
        if (!QDir().mkpath(dir)) {
            return QStringLiteral(
                "Lightning portable could not create its data directory "
                "inside the extracted folder (%1).").arg(QDir::toNativeSeparators(dir));
        }
    }

    // mkpath() succeeding does not prove writability (read-only mount, mounted
    // ISO, ZIP shell view). Write a probe.
    const QString probe = QDir(root).absoluteFilePath(
        QStringLiteral(".write-probe-%1")
            .arg(QUuid::createUuid().toString(QUuid::Id128)));
    {
        QFile file(probe);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return QStringLiteral(
                "Lightning portable cannot write inside its own folder (%1).")
                .arg(QDir::toNativeSeparators(root));
        }
        const QByteArray payload("lightning-portable-write-probe\n");
        if (file.write(payload) != payload.size() || !file.flush()) {
            file.close();
            QFile::remove(probe);
            return QStringLiteral(
                "Lightning portable could not finish writing a test file in "
                "its own folder (%1) — the drive may be full or read-only.")
                .arg(QDir::toNativeSeparators(root));
        }
    }
    // Read it back: network shares and filter drivers can discard writes.
    {
        QFile file(probe);
        const bool readable = file.open(QIODevice::ReadOnly)
                           && file.readAll().startsWith("lightning-portable");
        file.close();
        if (!readable) {
            QFile::remove(probe);
            return QStringLiteral(
                "Lightning portable wrote a test file in its own folder (%1) "
                "but could not read it back.")
                .arg(QDir::toNativeSeparators(root));
        }
    }
    QFile::remove(probe);
    return {};
}

namespace {
QMutex &liveScratchMutex()
{
    static QMutex mutex;
    return mutex;
}
// Keyed by directory so a creator releases its own lock and entries for
// removed directories can be pruned. std::unordered_map because the value is
// move-only.
std::unordered_map<QString, std::unique_ptr<QLockFile>> &liveScratchLocks()
{
    static std::unordered_map<QString, std::unique_ptr<QLockFile>> locks;
    return locks;
}
void pruneDeadScratchLocksLocked()
{
    auto &locks = liveScratchLocks();
    for (auto it = locks.begin(); it != locks.end();) {
        if (!QFileInfo(it->first).isDir())
            it = locks.erase(it); // unlocks and closes the descriptor
        else
            ++it;
    }
}
// Unmarked directories are swept only past this age, so one whose creator has
// not registered its lock yet is not taken for a leftover.
constexpr qint64 kUnmarkedScratchMinAgeMs = 60 * 60 * 1000;
} // namespace

void holdScratchDirLive(const QString &dir)
{
    if (dir.isEmpty() || !QFileInfo(dir).isDir())
        return;
    const QString key = QDir(dir).absolutePath();
    auto lock = std::make_unique<QLockFile>(
        QDir(dir).absoluteFilePath(QLatin1String(kScratchLiveLockName)));
    // Stale only when the recorded pid is gone, never by age.
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0))
        return; // somebody else's directory after all; do not claim it
    QMutexLocker guard(&liveScratchMutex());
    pruneDeadScratchLocksLocked();
    liveScratchLocks()[key] = std::move(lock);
}

void releaseScratchDir(const QString &dir)
{
    QMutexLocker guard(&liveScratchMutex());
    liveScratchLocks().erase(QDir(dir).absolutePath());
    pruneDeadScratchLocksLocked();
}

int cleanStaleTempDirs()
{
    return cleanStaleTempDirs(QDateTime::currentDateTimeUtc());
}

int cleanStaleTempDirs(const QDateTime &now)
{
    // The scratch root, not the portable temp dir: decrypted payloads live
    // under mediaScratchRoot() on every install type, so crash leftovers must
    // be swept everywhere.
    const QString root = mediaScratchRoot();
    if (root.isEmpty())
        return 0;
    // Only our prefixes, only directories, only our own user's: /tmp is shared
    // and this runs unattended. The list is hand-maintained; every creator of a
    // scratch directory must use mediaScratchRoot() and appear here.
    static const QStringList kOurs = {
        QStringLiteral("lightning-voice-*"),
        QStringLiteral("lightning-animated-*"),
        QStringLiteral("lightning-playable-*"),
        QStringLiteral("lightning-crop-*"),
    };
    int removed = 0;
    QDir dir(root);
    const auto stale = dir.entryInfoList(kOurs, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &info : stale) {
        // Never follow a symlink, whatever its name.
        if (info.isSymLink())
            continue;
#ifndef Q_OS_WIN
        if (info.ownerId() != ::getuid())
            continue;
#endif
        // A held lock means the directory is in use (another instance or a
        // concurrent run). A lock whose holder died can be taken, making it a
        // leftover.
        const QString lockPath =
            QDir(info.absoluteFilePath()).absoluteFilePath(QLatin1String(kScratchLiveLockName));
        if (QFileInfo::exists(lockPath)) {
            QLockFile probe(lockPath);
            probe.setStaleLockTime(0);
            if (!probe.tryLock(0))
                continue;
            probe.unlock();
        } else if (info.lastModified(QTimeZone::UTC).msecsTo(now)
                   < kUnmarkedScratchMinAgeMs) {
            // Unmarked and recent: leave it for a later sweep.
            continue;
        }
        QDir victim(info.absoluteFilePath());
        if (victim.removeRecursively())
            ++removed;
    }
    return removed;
}

} // namespace lightning::portable
