#include "stickers/StickerPackArchive.h"

#include "updater/SafeArchive.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

namespace stickers::archive {
namespace {

constexpr auto kFormat = "lightning.sticker-pack";
constexpr int kVersion = 1;
constexpr int kMaxShortcodeChars = 100;  // Matrix v1.19 grammar bound
constexpr int kMaxBodyChars = 160;
constexpr int kMaxNameChars = 80;
constexpr int kMaxAttributionChars = 160;

quint32 crc32(const QByteArray &data)
{
    static quint32 table[256];
    static bool ready = false;
    if (!ready) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    quint32 c = 0xFFFFFFFFu;
    for (const char ch : data)
        c = table[(c ^ quint8(ch)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void le16(QByteArray &b, quint16 v)
{
    b.append(char(v & 0xFF));
    b.append(char(v >> 8));
}
void le32(QByteArray &b, quint32 v)
{
    le16(b, quint16(v & 0xFFFF));
    le16(b, quint16(v >> 16));
}

struct ZipEntry {
    QByteArray name;
    QByteArray data;
};

// A stored ZIP. Names are ASCII by construction (the caller builds them from a
// fixed alphabet), the UTF-8 flag is set anyway.
QByteArray writeStoredZip(const QList<ZipEntry> &entries)
{
    QByteArray out;
    QByteArray central;
    for (const ZipEntry &e : entries) {
        const quint32 offset = quint32(out.size());
        const quint32 crc = crc32(e.data);
        // Local file header.
        le32(out, 0x04034b50);
        le16(out, 20);          // version needed
        le16(out, 0x0800);      // UTF-8 names
        le16(out, 0);           // stored
        le16(out, 0);           // time
        le16(out, 0x0021);      // date: 1980-01-01
        le32(out, crc);
        le32(out, quint32(e.data.size()));
        le32(out, quint32(e.data.size()));
        le16(out, quint16(e.name.size()));
        le16(out, 0);
        out.append(e.name);
        out.append(e.data);
        // Central directory record.
        le32(central, 0x02014b50);
        le16(central, 20);      // version made by
        le16(central, 20);
        le16(central, 0x0800);
        le16(central, 0);
        le16(central, 0);
        le16(central, 0x0021);
        le32(central, crc);
        le32(central, quint32(e.data.size()));
        le32(central, quint32(e.data.size()));
        le16(central, quint16(e.name.size()));
        le16(central, 0);       // extra
        le16(central, 0);       // comment
        le16(central, 0);       // disk
        le16(central, 0);       // internal attributes
        le32(central, 0);       // external attributes
        le32(central, offset);
        central.append(e.name);
    }
    const quint32 centralOffset = quint32(out.size());
    out.append(central);
    le32(out, 0x06054b50);
    le16(out, 0);
    le16(out, 0);
    le16(out, quint16(entries.size()));
    le16(out, quint16(entries.size()));
    le32(out, quint32(central.size()));
    le32(out, centralOffset);
    le16(out, 0);
    return out;
}

QString oneLine(const QString &text, int maxChars)
{
    QString collapsed;
    collapsed.reserve(text.size());
    for (const QChar c : text)
        collapsed += (c.isNull() || c.category() == QChar::Other_Control)
                         ? QLatin1Char(' ') : c;
    return collapsed.simplified().left(maxChars);
}

QString normalizeUsage(const QString &usage)
{
    if (usage == QLatin1String("emoticon") || usage == QLatin1String("both"))
        return usage;
    return QStringLiteral("sticker");
}

}  // namespace

bool isValidShortcode(const QString &key)
{
    if (key.isEmpty() || key.size() > kMaxShortcodeChars)
        return false;
    for (const QChar c : key) {
        const ushort u = c.unicode();
        const bool ok = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z')
                        || (u >= 'A' && u <= 'Z') || u == '-' || u == '_';
        if (!ok)
            return false;
    }
    return true;
}

QString proposeShortcode(const QString &requested)
{
    const QString trimmed = requested.trimmed();
    return isValidShortcode(trimmed) ? trimmed : sanitizeShortcode(trimmed);
}

QString sanitizeShortcode(const QString &raw)
{
    QString out;
    for (const QChar c : raw.trimmed()) {
        const ushort u = c.unicode();
        const bool alnum = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z')
                           || (u >= 'A' && u <= 'Z');
        if (alnum || u == '-') {
            out += c;
        } else if (!out.endsWith(QLatin1Char('_'))) {
            out += QLatin1Char('_');
        }
        if (out.size() >= kMaxShortcodeChars)
            break;
    }
    int begin = 0;
    int end = out.size();
    while (begin < end && (out[begin] == QLatin1Char('-')
                           || out[begin] == QLatin1Char('_')))
        ++begin;
    while (end > begin && (out[end - 1] == QLatin1Char('-')
                           || out[end - 1] == QLatin1Char('_')))
        --end;
    return out.mid(begin, end - begin);
}

QString extensionForBytes(const QByteArray &b)
{
    if (b.size() >= 8 && b.startsWith("\x89PNG\r\n\x1A\n"))
        return QStringLiteral("png");
    if (b.size() >= 3 && quint8(b[0]) == 0xFF && quint8(b[1]) == 0xD8
        && quint8(b[2]) == 0xFF)
        return QStringLiteral("jpg");
    if (b.size() >= 12 && b.startsWith("RIFF") && b.mid(8, 4) == "WEBP")
        return QStringLiteral("webp");
    if (b.size() >= 6 && (b.startsWith("GIF87a") || b.startsWith("GIF89a")))
        return QStringLiteral("gif");
    return {};
}

QString mimeForExtension(const QString &ext)
{
    if (ext == QLatin1String("png")) return QStringLiteral("image/png");
    if (ext == QLatin1String("jpg")) return QStringLiteral("image/jpeg");
    if (ext == QLatin1String("webp")) return QStringLiteral("image/webp");
    if (ext == QLatin1String("gif")) return QStringLiteral("image/gif");
    return {};
}

QByteArray buildArchive(const Pack &pack, int *skipped)
{
    int skippedCount = 0;
    QList<ZipEntry> entries;
    QJsonArray images;
    int index = 0;
    for (const Image &image : pack.images) {
        const QString ext = extensionForBytes(image.data);
        if (ext.isEmpty() || image.data.isEmpty()
            || image.data.size() > kMaxImageBytes || index >= kMaxImages) {
            ++skippedCount;
            continue;
        }
        // A valid key travels exactly (case, `__`, 65-100 bytes); only a
        // malformed one is turned into a suggestion.
        const QString code = proposeShortcode(image.shortcode);
        if (code.isEmpty()) {
            ++skippedCount;
            continue;
        }
        ++index;
        // The file name is a locator only (the manifest carries the exact
        // shortcode), so it stays short enough for the reader's name rule.
        const QString file = QStringLiteral("images/%1-%2.%3")
                                 .arg(index, 4, 10, QLatin1Char('0'))
                                 .arg(code.left(64), ext);
        QJsonObject obj;
        obj.insert(QStringLiteral("shortcode"), code);
        obj.insert(QStringLiteral("file"), file);
        obj.insert(QStringLiteral("mimetype"), mimeForExtension(ext));
        obj.insert(QStringLiteral("usage"), normalizeUsage(image.usage));
        const QString body = oneLine(image.body, kMaxBodyChars);
        if (!body.isEmpty() && body != code)
            obj.insert(QStringLiteral("body"), body);
        images.append(obj);
        entries.append({file.toUtf8(), image.data});
    }
    QJsonObject manifest;
    manifest.insert(QStringLiteral("format"), QLatin1String(kFormat));
    manifest.insert(QStringLiteral("version"), kVersion);
    manifest.insert(QStringLiteral("name"), oneLine(pack.name, kMaxNameChars));
    if (!pack.attribution.isEmpty())
        manifest.insert(QStringLiteral("attribution"),
                        oneLine(pack.attribution, kMaxAttributionChars));
    manifest.insert(QStringLiteral("images"), images);
    entries.prepend({QByteArrayLiteral("pack.json"),
                     QJsonDocument(manifest).toJson(QJsonDocument::Indented)});
    if (skipped)
        *skipped = skippedCount;
    return writeStoredZip(entries);
}

ReadResult readArchive(const QString &zipPath, const QString &workDir)
{
    ReadResult result;
    updater::ArchiveLimits limits;
    limits.maxEntries = kMaxImages + 8;
    limits.maxEntryUncompressedBytes = kMaxImageBytes;
    limits.maxTotalUncompressedBytes = 96LL * 1024 * 1024;
    limits.maxArchiveBytes = 96LL * 1024 * 1024;
    limits.maxPathDepth = 4;
    const updater::ArchiveResult extracted =
        updater::extractZipSafely(zipPath, workDir, limits);
    if (!extracted.ok()) {
        result.error = ReadError::Unreadable;
        return result;
    }

    QFile manifestFile(QDir(workDir).filePath(QStringLiteral("pack.json")));
    if (!manifestFile.exists()) {
        result.error = ReadError::NoManifest;
        return result;
    }
    if (!manifestFile.open(QIODevice::ReadOnly)
        || manifestFile.size() > kMaxManifestBytes) {
        result.error = ReadError::BadManifest;
        return result;
    }
    QJsonParseError parse;
    const QJsonDocument doc =
        QJsonDocument::fromJson(manifestFile.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        result.error = ReadError::BadManifest;
        return result;
    }
    const QJsonObject manifest = doc.object();
    if (manifest.value(QStringLiteral("format")).toString()
            != QLatin1String(kFormat)
        || manifest.value(QStringLiteral("version")).toInt() != kVersion) {
        result.error = ReadError::BadManifest;
        return result;
    }
    result.pack.name = oneLine(
        manifest.value(QStringLiteral("name")).toString(), kMaxNameChars);
    result.pack.attribution = oneLine(
        manifest.value(QStringLiteral("attribution")).toString(),
        kMaxAttributionChars);

    static const QRegularExpression fileName(
        QStringLiteral("^images/[A-Za-z0-9][A-Za-z0-9._-]{0,99}$"));
    const QJsonArray entries = manifest.value(QStringLiteral("images")).toArray();
    QSet<QString> seen;
    for (const QJsonValue &value : entries) {
        const QJsonObject obj = value.toObject();
        const QString file = obj.value(QStringLiteral("file")).toString();
        if (result.pack.images.size() >= kMaxImages
            || !fileName.match(file).hasMatch()
            || file.contains(QLatin1String(".."))) {
            ++result.refused;
            continue;
        }
        const QString path = QDir(workDir).filePath(file);
        const QFileInfo info(path);
        if (!info.isFile() || info.isSymLink() || info.size() <= 0
            || info.size() > kMaxImageBytes) {
            ++result.refused;
            continue;
        }
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            ++result.refused;
            continue;
        }
        // The type comes from the bytes, never from the name or the manifest.
        // SVG (and everything not a plain raster) is refused here.
        const QString ext = extensionForBytes(f.read(16));
        if (ext.isEmpty()) {
            ++result.refused;
            continue;
        }
        QString code = proposeShortcode(
            obj.value(QStringLiteral("shortcode")).toString());
        if (code.isEmpty())
            code = sanitizeShortcode(QFileInfo(file).completeBaseName());
        if (code.isEmpty())
            code = QStringLiteral("sticker");
        // Exact, case-sensitive: `Cat` and `cat` are two images.
        if (seen.contains(code)) {
            ++result.refused;
            continue;
        }
        seen.insert(code);
        Image image;
        image.shortcode = code;
        image.body = oneLine(obj.value(QStringLiteral("body")).toString(),
                             kMaxBodyChars);
        image.mimetype = mimeForExtension(ext);
        image.usage = normalizeUsage(
            obj.value(QStringLiteral("usage")).toString());
        image.path = path;
        result.pack.images.append(image);
    }
    if (result.pack.images.isEmpty())
        result.error = ReadError::NoImages;
    return result;
}

}  // namespace stickers::archive
