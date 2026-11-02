#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace stickers::archive {

constexpr int kMaxImages = 512;
constexpr qint64 kMaxImageBytes = 4 * 1024 * 1024;
constexpr qint64 kMaxManifestBytes = 1024 * 1024;

struct Image {
    QString shortcode;
    QString body;
    QString mimetype;
    QString usage = QStringLiteral("sticker");
    QByteArray data;
    QString path;
};

struct Pack {
    QString name;
    QString attribution;
    QList<Image> images;
};

// File extension (without dot) for raster image bytes, or "" when the bytes
// are not PNG / JPEG / WebP / GIF. SVG and anything else are "".
QString extensionForBytes(const QByteArray &bytes);
QString mimeForExtension(const QString &extension);

QByteArray buildArchive(const Pack &pack, int *skipped = nullptr);

enum class ReadError {
    None,
    Unreadable,
    NoManifest,
    BadManifest,
    NoImages,
};

struct ReadResult {
    ReadError error = ReadError::None;
    Pack pack;
    int refused = 0;
};

ReadResult readArchive(const QString &zipPath, const QString &workDir);

bool isValidShortcode(const QString &key);

QString proposeShortcode(const QString &requested);

QString sanitizeShortcode(const QString &raw);

}
