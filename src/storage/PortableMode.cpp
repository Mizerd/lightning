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
#include <QtGlobal>

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

// One mutex for every piece of module state. isPortable() is reached from the
// preflight (single-threaded) but also, indirectly, from
// matrix::app_data::primaryRoot(), which the Rust backend calls off the GUI
// thread. A torn read of the cache would be a storage-location race.
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
    // GetModuleFileNameW truncates rather than failing when the buffer is too
    // small (and on pre-2000 builds does not null-terminate), so the only safe
    // loop is "grow until the result fits strictly inside the buffer".
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
    // _NSGetExecutablePath may return a path containing symlinks or "..";
    // realpath is what makes "the directory containing the executable"
    // comparable with a marker file's own resolved location.
    char resolved[PATH_MAX] = {};
    if (::realpath(buffer.data(), resolved) == nullptr)
        return QString::fromLocal8Bit(buffer.data());
    return QString::fromLocal8Bit(resolved);
#else
    // /proc/self/exe is already fully resolved by the kernel. readlink() never
    // null-terminates and never reports the required size, so grow until the
    // result fits strictly inside the buffer.
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

// The real executable directory, cached: the platform call is a syscall and
// isPortable()/dataRoot() are on the startup path.
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

// Development-only forcing. Returns nullopt when the variable is unset or
// carries a value we refuse to interpret — an unrecognised value must not
// silently mean "off", because that would be a silent departure from portable
// mode, which is exactly the failure class this module forbids.
std::optional<bool> environmentOverride()
{
    const QByteArray raw = qgetenv(kPortableEnvVar);
    if (raw.isEmpty())
        return std::nullopt;
    const QByteArray value = raw.trimmed().toLower();
    if (value == "1" || value == "true" || value == "on" || value == "yes")
        return true;
    if (value == "0" || value == "false" || value == "off" || value == "no") {
        // Accepted as a value, but see the caller: it can only ever prevent a
        // dev build from opting IN. It cannot switch a PACKAGED portable
        // installation off, because an environment variable that silently
        // moves a portable copy's session, secrets and crypto store back to
        // %LOCALAPPDATA%, the registry and the Credential Manager is the exact
        // failure this module exists to prevent — and it would surface to the
        // user only as "why am I being asked to sign in again?".
        return false;
    }
    qWarning("LIGHTNING_PORTABLE is set to an unrecognised value; "
             "ignoring it and using the packaging marker instead.");
    return std::nullopt;
}

// Directory executableDir() should report, honouring an installed override.
// Caller holds stateMutex().
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
    // isFile() and not merely exists(): a DIRECTORY named portable.marker is
    // not the packaging pipeline's marker, and treating it as one would move
    // a normal installation's storage.
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
        // An unresolvable executable directory is "not portable": we have
        // nowhere to anchor a portable tree, and inventing one from the
        // working directory would put the user's session wherever the
        // shortcut happened to point.
        const bool marked = markerPresentIn(realExecutableDir());
        // THE MARKER WINS. The environment variable can only ever opt a
        // build IN (for development against a tree that was never packaged);
        // it can NOT opt a packaged portable installation OUT.
        //
        // That asymmetry is deliberate. A stray LIGHTNING_PORTABLE=0 in a
        // user's environment would otherwise send a portable copy's settings,
        // sealed session and crypto store back to %LOCALAPPDATA%, the registry
        // and the Credential Manager — silently, and visible to the user only
        // as being asked to sign in again after moving the folder, with the
        // old machine still holding the only copy of the device keys. An
        // environment variable must not be able to cause that.
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
    // Deliberately does NOT reset g_decided: the real decision is still the
    // real decision, and re-deciding here would give this function the power
    // to change a running process's storage location, which is precisely what
    // the caching exists to prevent.
}

QString prepareDataRoot()
{
    if (!isPortable()) {
        // A programming error rather than a user-fixable condition, so it does
        // not get the user-facing wording.
        return QStringLiteral(
            "prepareDataRoot() called while not in portable mode.");
    }

    const QString root = dataRoot();
    if (root.isEmpty()) {
        return QStringLiteral(
            "Lightning portable could not determine its own program "
            "directory, so it does not know where to keep your data.");
    }

    // matrix/ is matrix::app_data::primaryRoot() in portable mode; the other
    // four are lightning::portable's own named subdirectories. Creating them
    // up front means every later writer finds its directory already there,
    // and it makes an extracted-but-never-run folder visibly complete.
    const QStringList wanted = {
        root,
        root + QLatin1String("/config"),
        root + QLatin1String("/secrets"),
        root + QLatin1String("/cache"),
        root + QLatin1String("/logs"),
        root + QLatin1String("/matrix"),
    };
    for (const QString &dir : wanted) {
        if (!QDir().mkpath(dir)) {
            return QStringLiteral(
                "Lightning portable could not create its data directory "
                "inside the extracted folder (%1).").arg(QDir::toNativeSeparators(dir));
        }
    }

    // mkpath() succeeding is NOT evidence of writability: the directory may
    // already exist on a read-only mount, inside a still-mounted ISO, or in a
    // ZIP browsed through a shell namespace extension. Prove it by writing.
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
    // Read it back: a network share or a filter driver can accept a write and
    // discard it, and discovering that here is much cheaper than discovering
    // it after a sign-in that then fails to persist.
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

} // namespace lightning::portable
