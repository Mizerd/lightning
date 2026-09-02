#include "updater/ArtifactDigest.h"

#include <QCryptographicHash>
#include <QFile>

namespace updater {

QString sha256HexOfFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();

    QCryptographicHash hash(QCryptographicHash::Sha256);
    // 1 MiB at a time: an artifact is up to 1 GiB (UpdateManifest's ceiling)
    // and must never be pulled into memory whole.
    constexpr qint64 kChunk = qint64(1024) * 1024;
    QByteArray chunk;
    chunk.resize(static_cast<int>(kChunk));
    for (;;) {
        const qint64 got = file.read(chunk.data(), kChunk);
        if (got < 0)
            return QString();
        if (got == 0)
            break;
        hash.addData(QByteArrayView(chunk.constData(), static_cast<qsizetype>(got)));
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool isSha256Hex(const QString &value)
{
    if (value.size() != 64)
        return false;
    for (const QChar ch : value) {
        const bool digit = ch >= QLatin1Char('0') && ch <= QLatin1Char('9');
        const bool lowerHex = ch >= QLatin1Char('a') && ch <= QLatin1Char('f');
        if (!digit && !lowerHex)
            return false;
    }
    return true;
}

} // namespace updater
