#pragma once

#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <functional>

namespace lightning::accounts {

// What a store path IS on disk, as opposed to how it was spelled: two paths
// that differ only in case on a case-insensitive filesystem, or reach the same
// directory through different parents, have one identity. Phase 0 records it
// (StoreOwnershipRegistry) and refuses nothing.
struct FileIdentity {
    quint64 volume = 0;   // POSIX st_dev; Windows dwVolumeSerialNumber
    quint64 file = 0;     // POSIX st_ino; Windows (nFileIndexHigh<<32)|nFileIndexLow
    bool valid = false;   // false: missing/unreadable. Two invalid ids are NEVER the same store.
    friend bool operator==(const FileIdentity &, const FileIdentity &) = default;
};

size_t qHash(const FileIdentity &id, size_t seed = 0) noexcept;

// stat() (follows links; store symlinks are already refused). Windows:
// CreateFileW(..., FILE_FLAG_BACKUP_SEMANTICS, share R|W|D) +
// GetFileInformationByHandle, handle closed at once. Never logs the path,
// which contains the Matrix localpart.
FileIdentity fileIdentityOf(const QString &path);

using FileIdentityResolver = std::function<FileIdentity(const QString &path)>;

} // namespace lightning::accounts
