#pragma once

#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <functional>

namespace lightning::accounts {

struct FileIdentity {
    quint64 volume = 0;
    quint64 file = 0;     // POSIX st_ino; Windows (nFileIndexHigh<<32)|nFileIndexLow
    bool valid = false;
    friend bool operator==(const FileIdentity &, const FileIdentity &) = default;
};

size_t qHash(const FileIdentity &id, size_t seed = 0) noexcept;

FileIdentity fileIdentityOf(const QString &path);

using FileIdentityResolver = std::function<FileIdentity(const QString &path)>;

}
