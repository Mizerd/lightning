#include "accounts/FileIdentity.h"

#include <QHashFunctions>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <QDir>
#  include <windows.h>
#else
#  include <QFile>
#  include <sys/stat.h>
#endif

namespace lightning::accounts {

size_t qHash(const FileIdentity &id, size_t seed) noexcept
{
    return qHashMulti(seed, id.volume, id.file, id.valid);
}

FileIdentity fileIdentityOf(const QString &path)
{
    FileIdentity id;
    if (path.isEmpty())
        return id;
#ifdef Q_OS_WIN
    // BACKUP_SEMANTICS is what lets CreateFileW open a DIRECTORY; the store
    // is one. Attribute access only, and every share mode, so this never
    // contends with the SQLite files a running client holds open below it.
    const QString native = QDir::toNativeSeparators(path);
    HANDLE h = CreateFileW(reinterpret_cast<const wchar_t *>(native.utf16()),
                           FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return id;
    BY_HANDLE_FILE_INFORMATION info;
    const BOOL ok = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    if (!ok)
        return id;
    id.volume = static_cast<quint64>(info.dwVolumeSerialNumber);
    id.file = (static_cast<quint64>(info.nFileIndexHigh) << 32)
        | static_cast<quint64>(info.nFileIndexLow);
    id.valid = true;
#else
    struct stat st {};
    const QByteArray encoded = QFile::encodeName(path);
    if (::stat(encoded.constData(), &st) != 0)
        return id;
    id.volume = static_cast<quint64>(st.st_dev);
    id.file = static_cast<quint64>(st.st_ino);
    id.valid = true;
#endif
    return id;
}

} // namespace lightning::accounts
