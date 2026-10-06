#include "app/SaveNaming.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QSet>

#if defined(Q_OS_WIN)
#include <qt_windows.h>

#include <string>
#endif

namespace savenaming {

namespace {

// Bounded below 255 bytes for every filesystem, with room for " (999)".
constexpr int kMaxLeafChars = 120;

const QHash<QString, QString> &mimeTable()
{
    // The types Matrix clients actually send. Fixed, so a save on Windows (Qt's
    // built-in MIME database) and on Linux (the system's shared-mime-info)
    // suggest the same name.
    static const QHash<QString, QString> table = {
        {QStringLiteral("image/jpeg"), QStringLiteral("jpg")},
        {QStringLiteral("image/jpg"), QStringLiteral("jpg")},
        {QStringLiteral("image/pjpeg"), QStringLiteral("jpg")},
        {QStringLiteral("image/png"), QStringLiteral("png")},
        {QStringLiteral("image/apng"), QStringLiteral("png")},
        {QStringLiteral("image/gif"), QStringLiteral("gif")},
        {QStringLiteral("image/webp"), QStringLiteral("webp")},
        {QStringLiteral("image/avif"), QStringLiteral("avif")},
        {QStringLiteral("image/heic"), QStringLiteral("heic")},
        {QStringLiteral("image/heif"), QStringLiteral("heif")},
        {QStringLiteral("image/bmp"), QStringLiteral("bmp")},
        {QStringLiteral("image/tiff"), QStringLiteral("tiff")},
        {QStringLiteral("image/svg+xml"), QStringLiteral("svg")},
        {QStringLiteral("image/x-icon"), QStringLiteral("ico")},
        {QStringLiteral("image/jxl"), QStringLiteral("jxl")},
        {QStringLiteral("video/mp4"), QStringLiteral("mp4")},
        {QStringLiteral("video/webm"), QStringLiteral("webm")},
        {QStringLiteral("video/quicktime"), QStringLiteral("mov")},
        {QStringLiteral("video/x-matroska"), QStringLiteral("mkv")},
        {QStringLiteral("video/ogg"), QStringLiteral("ogv")},
        {QStringLiteral("video/x-msvideo"), QStringLiteral("avi")},
        {QStringLiteral("audio/ogg"), QStringLiteral("ogg")},
        {QStringLiteral("audio/opus"), QStringLiteral("opus")},
        {QStringLiteral("audio/mpeg"), QStringLiteral("mp3")},
        {QStringLiteral("audio/mp3"), QStringLiteral("mp3")},
        {QStringLiteral("audio/mp4"), QStringLiteral("m4a")},
        {QStringLiteral("audio/x-m4a"), QStringLiteral("m4a")},
        {QStringLiteral("audio/aac"), QStringLiteral("aac")},
        {QStringLiteral("audio/wav"), QStringLiteral("wav")},
        {QStringLiteral("audio/x-wav"), QStringLiteral("wav")},
        {QStringLiteral("audio/wave"), QStringLiteral("wav")},
        {QStringLiteral("audio/flac"), QStringLiteral("flac")},
        {QStringLiteral("audio/x-flac"), QStringLiteral("flac")},
        {QStringLiteral("audio/webm"), QStringLiteral("weba")},
        {QStringLiteral("application/pdf"), QStringLiteral("pdf")},
        {QStringLiteral("application/zip"), QStringLiteral("zip")},
        {QStringLiteral("application/x-7z-compressed"), QStringLiteral("7z")},
        {QStringLiteral("application/gzip"), QStringLiteral("gz")},
        {QStringLiteral("application/x-tar"), QStringLiteral("tar")},
        {QStringLiteral("application/x-rar-compressed"), QStringLiteral("rar")},
        {QStringLiteral("application/vnd.rar"), QStringLiteral("rar")},
        {QStringLiteral("application/json"), QStringLiteral("json")},
        {QStringLiteral("application/xml"), QStringLiteral("xml")},
        {QStringLiteral("text/plain"), QStringLiteral("txt")},
        {QStringLiteral("text/markdown"), QStringLiteral("md")},
        {QStringLiteral("text/html"), QStringLiteral("html")},
        {QStringLiteral("text/csv"), QStringLiteral("csv")},
        {QStringLiteral("text/calendar"), QStringLiteral("ics")},
        {QStringLiteral("text/vcard"), QStringLiteral("vcf")},
        {QStringLiteral("application/msword"), QStringLiteral("doc")},
        {QStringLiteral("application/vnd.openxmlformats-officedocument."
                        "wordprocessingml.document"),
         QStringLiteral("docx")},
        {QStringLiteral("application/vnd.ms-excel"), QStringLiteral("xls")},
        {QStringLiteral("application/vnd.openxmlformats-officedocument."
                        "spreadsheetml.sheet"),
         QStringLiteral("xlsx")},
        {QStringLiteral("application/vnd.ms-powerpoint"), QStringLiteral("ppt")},
        {QStringLiteral("application/vnd.openxmlformats-officedocument."
                        "presentationml.presentation"),
         QStringLiteral("pptx")},
        {QStringLiteral("application/vnd.oasis.opendocument.text"),
         QStringLiteral("odt")},
        {QStringLiteral("application/vnd.oasis.opendocument.spreadsheet"),
         QStringLiteral("ods")},
        {QStringLiteral("application/vnd.oasis.opendocument.presentation"),
         QStringLiteral("odp")},
        {QStringLiteral("application/epub+zip"), QStringLiteral("epub")},
        {QStringLiteral("application/rtf"), QStringLiteral("rtf")},
        {QStringLiteral("font/ttf"), QStringLiteral("ttf")},
        {QStringLiteral("font/otf"), QStringLiteral("otf")},
        {QStringLiteral("font/woff2"), QStringLiteral("woff2")},
    };
    return table;
}

const QSet<QString> &tableExtensions()
{
    static const QSet<QString> set = [] {
        QSet<QString> s;
        for (const QString &ext : mimeTable())
            s.insert(ext);
        // Extensions that are not any table entry's preferred one but are
        // every bit as common.
        for (const char *ext : {"jpeg", "tif", "htm", "mpeg", "mpg", "m4v",
                                "oga", "tgz", "bz2", "xz", "zst"})
            s.insert(QString::fromLatin1(ext));
        return s;
    }();
    return set;
}

const QStringList &compoundSuffixes()
{
    static const QStringList list = {
        QStringLiteral("tar.gz"), QStringLiteral("tar.bz2"),
        QStringLiteral("tar.xz"), QStringLiteral("tar.zst"),
    };
    return list;
}

// Extensions whose opening can run code: native executables, installers and
// packages, scripts, shortcuts and launchers on Windows, macOS and Linux.
const QSet<QString> &riskyExtensions()
{
    static const QSet<QString> set = [] {
        QSet<QString> s;
        for (const char *ext : {
                 // Windows executables, installers and script hosts
                 "exe", "com", "scr", "pif", "cpl", "msi", "msix", "msixbundle",
                 "appx", "appxbundle", "msp", "mst", "bat", "cmd", "ps1",
                 "psm1", "psd1", "ps1xml", "psc1", "vbs", "vbe", "vb", "js",
                 "jse", "wsf", "wsh", "wsc", "ws", "hta", "sct", "shb", "shs",
                 "msc", "reg", "inf", "ins", "isp", "chm", "gadget", "lnk",
                 "url", "website", "appref-ms", "application", "settingcontent-ms",
                 "library-ms", "searchconnector-ms", "xbap", "xll", "dll", "sys",
                 "drv", "ocx", "jar", "jnlp", "hlp",
                 // macOS
                 "app", "dmg", "pkg", "mpkg", "command", "tool", "terminal",
                 "workflow", "action", "scpt", "applescript", "webloc",
                 "fileloc", "inetloc",
                 // Linux and Unix
                 "sh", "bash", "zsh", "csh", "ksh", "fish", "run", "bin",
                 "out", "elf", "so", "appimage", "desktop", "deb", "rpm",
                 "snap", "flatpak", "flatpakref", "flatpakrepo", "apk", "xapk",
                 "kwinscript", "plasmoid",
                 // Interpreted scripts a desktop may hand to an interpreter
                 "py", "pyw", "pyc", "pl", "rb", "php", "lua", "tcl",
                 // Documents a browser opens with active content (script),
                 // outside any Matrix client's sandbox
                 "html", "htm", "xhtml", "shtml", "xht", "mht", "mhtml",
                 "svg", "svgz",
                 // Windows disk images and containers: what is inside them
                 // loses the mark-of-the-web, so SmartScreen never sees it
                 "iso", "img", "vhd", "vhdx",
             })
            s.insert(QString::fromLatin1(ext));
        return s;
    }();
    return set;
}

const QSet<QString> &riskyMimeTypes()
{
    static const QSet<QString> set = {
        QStringLiteral("application/x-executable"),
        QStringLiteral("application/x-sharedlib"),
        QStringLiteral("application/x-pie-executable"),
        QStringLiteral("application/x-elf"),
        QStringLiteral("application/x-msdownload"),
        QStringLiteral("application/x-msdos-program"),
        QStringLiteral("application/x-ms-dos-executable"),
        QStringLiteral("application/x-dosexec"),
        QStringLiteral("application/vnd.microsoft.portable-executable"),
        QStringLiteral("application/x-msi"),
        QStringLiteral("application/x-ms-installer"),
        QStringLiteral("application/x-ms-shortcut"),
        QStringLiteral("application/x-sh"),
        QStringLiteral("application/x-shellscript"),
        QStringLiteral("text/x-shellscript"),
        QStringLiteral("application/x-bat"),
        QStringLiteral("application/x-desktop"),
        QStringLiteral("application/x-appimage"),
        QStringLiteral("application/vnd.appimage"),
        QStringLiteral("application/java-archive"),
        QStringLiteral("application/x-java-archive"),
        QStringLiteral("application/x-java-jnlp-file"),
        QStringLiteral("application/vnd.android.package-archive"),
        QStringLiteral("application/x-apple-diskimage"),
        QStringLiteral("application/x-debian-package"),
        QStringLiteral("application/vnd.debian.binary-package"),
        QStringLiteral("application/x-rpm"),
        QStringLiteral("application/x-redhat-package-manager"),
        QStringLiteral("application/javascript"),
        QStringLiteral("text/javascript"),
        QStringLiteral("application/x-python"),
        QStringLiteral("text/x-python"),
        QStringLiteral("application/x-perl"),
        QStringLiteral("application/hta"),
        QStringLiteral("application/x-ms-application"),
        QStringLiteral("text/html"),
        QStringLiteral("application/xhtml+xml"),
        QStringLiteral("multipart/related"),
        QStringLiteral("message/rfc822"),
        QStringLiteral("application/x-mimearchive"),
        QStringLiteral("image/svg+xml"),
        QStringLiteral("application/x-iso9660-image"),
        QStringLiteral("application/x-raw-disk-image"),
        QStringLiteral("application/x-vhd"),
        QStringLiteral("application/x-vhdx"),
    };
    return set;
}

QString lowerMime(const QString &mime)
{
    // Parameters ("text/plain; charset=utf-8") are not part of the type.
    return mime.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
}

bool mimeDatabaseKnowsSuffix(const QString &suffix)
{
    static const QMimeDatabase db;
    const QList<QMimeType> types =
        db.mimeTypesForFileName(QStringLiteral("x.") + suffix);
    for (const QMimeType &t : types) {
        if (t.isValid() && !t.isDefault())
            return true;
    }
    return false;
}

} // namespace

QString sanitizeLeaf(const QString &raw)
{
    QString out = raw;
    // Both separators, whatever the platform: `..\..\x` is one leaf on Unix.
    const QStringList parts = out.split(
        QRegularExpression(QStringLiteral("[\\\\/]")), Qt::SkipEmptyParts);
    out = parts.isEmpty() ? QString() : parts.last();
    for (QChar &c : out) {
        const ushort u = c.unicode();
        if (u < 0x20 || u == 0x7f || u == '<' || u == '>' || u == ':'
            || u == '"' || u == '|' || u == '?' || u == '*')
            c = QLatin1Char('_');
    }
    out = out.trimmed();
    while (out.startsWith(QLatin1Char('.')))
        out.remove(0, 1);
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' ')))
        out.chop(1);
    out = out.trimmed();
    if (out.isEmpty())
        return {};
    // Windows reserved device names, whatever the extension.
    static const QSet<QString> reserved = {
        QStringLiteral("con"), QStringLiteral("prn"), QStringLiteral("aux"),
        QStringLiteral("nul"), QStringLiteral("com1"), QStringLiteral("com2"),
        QStringLiteral("com3"), QStringLiteral("com4"), QStringLiteral("com5"),
        QStringLiteral("com6"), QStringLiteral("com7"), QStringLiteral("com8"),
        QStringLiteral("com9"), QStringLiteral("lpt1"), QStringLiteral("lpt2"),
        QStringLiteral("lpt3"), QStringLiteral("lpt4"), QStringLiteral("lpt5"),
        QStringLiteral("lpt6"), QStringLiteral("lpt7"), QStringLiteral("lpt8"),
        QStringLiteral("lpt9"), QStringLiteral("conin$"),
        QStringLiteral("conout$"),
    };
    if (reserved.contains(out.section(QLatin1Char('.'), 0, 0).toLower()))
        out.prepend(QStringLiteral("file-"));
    if (out.size() > kMaxLeafChars) {
        const QString ext = extensionOf(out);
        const QString suffix = ext.isEmpty() ? QString()
                                             : QStringLiteral(".") + ext;
        // Keep the original spelling of the suffix.
        const QString kept = out.right(suffix.size());
        out = out.left(kMaxLeafChars - kept.size()).trimmed() + kept;
    }
    return out;
}

QString extensionForMime(const QString &mime)
{
    const QString type = lowerMime(mime);
    if (type.isEmpty())
        return {};
    const auto it = mimeTable().constFind(type);
    if (it != mimeTable().constEnd())
        return it.value();
    if (type == QLatin1String("application/octet-stream"))
        return {};
    static const QMimeDatabase db;
    const QMimeType t = db.mimeTypeForName(type);
    if (!t.isValid() || t.isDefault())
        return {};
    const QString preferred = t.preferredSuffix().toLower();
    return looksLikeExtension(preferred) ? preferred : QString();
}

bool looksLikeExtension(const QString &suffix)
{
    if (suffix.isEmpty() || suffix.size() > 10)
        return false;
    bool letter = false;
    for (const QChar c : suffix) {
        if (c.isLetter() && c.unicode() < 0x80)
            letter = true;
        else if (!(c.isDigit() && c.unicode() < 0x80) && c != QLatin1Char('+')
                 && c != QLatin1Char('-') && c != QLatin1Char('_'))
            return false;
    }
    return letter;
}

QString extensionOf(const QString &leaf)
{
    const QString lower = leaf.toLower();
    for (const QString &compound : compoundSuffixes()) {
        if (lower.endsWith(QLatin1Char('.') + compound)
            && lower.size() > compound.size() + 1)
            return compound;
    }
    const int dot = lower.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot == lower.size() - 1)
        return {};
    const QString suffix = lower.mid(dot + 1);
    if (!looksLikeExtension(suffix))
        return {};
    if (tableExtensions().contains(suffix) || riskyExtensions().contains(suffix)
        || mimeDatabaseKnowsSuffix(suffix))
        return suffix;
    return {};
}

QString suggestedFileName(const QString &rawName, const QString &mime)
{
    const QString ext = extensionForMime(mime);
    QString leaf = sanitizeLeaf(rawName);
    if (leaf.isEmpty()) {
        const QString type = lowerMime(mime);
        if (type.startsWith(QLatin1String("image/")))
            leaf = QStringLiteral("image");
        else if (type.startsWith(QLatin1String("video/")))
            leaf = QStringLiteral("video");
        else if (type.startsWith(QLatin1String("audio/")))
            leaf = QStringLiteral("audio");
        else
            leaf = QStringLiteral("file");
    }
    if (!ext.isEmpty() && extensionOf(leaf).isEmpty()) {
        if (leaf.size() + ext.size() + 1 > kMaxLeafChars)
            leaf = leaf.left(kMaxLeafChars - ext.size() - 1).trimmed();
        leaf += QLatin1Char('.') + ext;
    }
    return leaf;
}

QString ensureExtension(const QString &chosenLeaf,
                        const QString &expectedExtension)
{
    if (chosenLeaf.isEmpty() || expectedExtension.isEmpty())
        return chosenLeaf;
    if (!extensionOf(chosenLeaf).isEmpty())
        return chosenLeaf;
    QString leaf = chosenLeaf;
    while (leaf.endsWith(QLatin1Char('.')))
        leaf.chop(1);
    return leaf + QLatin1Char('.') + expectedExtension;
}

QString numberedName(const QString &leaf, int n)
{
    if (n <= 0)
        return leaf;
    const QString ext = extensionOf(leaf);
    const QString stem =
        ext.isEmpty() ? leaf : leaf.left(leaf.size() - ext.size() - 1);
    const QString suffix =
        ext.isEmpty() ? QString() : leaf.right(ext.size() + 1);
    return QStringLiteral("%1 (%2)%3").arg(stem).arg(n).arg(suffix);
}

QString uniqueFileName(const QString &directory, const QString &leaf)
{
    const QDir dir(directory);
    for (int n = 0; n < 1000; ++n) {
        const QString candidate = numberedName(leaf, n);
        if (!QFileInfo::exists(dir.filePath(candidate)))
            return candidate;
    }
    return numberedName(leaf, 1000);
}

bool isRiskyToOpen(const QString &fileName, const QString &mime)
{
    if (riskyMimeTypes().contains(lowerMime(mime)))
        return true;
    QString leaf = QFileInfo(fileName).fileName();
    // What Windows would actually open: trailing dots and spaces dropped.
    while (leaf.endsWith(QLatin1Char('.')) || leaf.endsWith(QLatin1Char(' ')))
        leaf.chop(1);
    const int dot = leaf.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0)
        return true; // no extension: the desktop would decide by content
    const QString suffix = leaf.mid(dot + 1).toLower();
    if (suffix.isEmpty())
        return true;
    return riskyExtensions().contains(suffix);
}

QByteArray zoneIdentifierContent()
{
    // What browsers write: Internet zone. No ReferrerUrl/HostUrl, which
    // would put an authenticated media URL on disk (CLAUDE.md §6).
    return QByteArrayLiteral("[ZoneTransfer]\r\nZoneId=3\r\n");
}

bool markAsDownloaded(const QString &path)
{
#if defined(Q_OS_WIN)
    if (path.isEmpty())
        return false;
    // An NTFS alternate data stream beside the file. Best effort: FAT/exFAT
    // and network shares have no streams, and a failure only means
    // SmartScreen treats the file as local, as before.
    const std::wstring stream =
        QDir::toNativeSeparators(path).toStdWString() + L":Zone.Identifier";
    HANDLE h = CreateFileW(stream.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    const QByteArray content = zoneIdentifierContent();
    DWORD written = 0;
    const BOOL ok = WriteFile(h, content.constData(),
                              static_cast<DWORD>(content.size()), &written,
                              nullptr);
    CloseHandle(h);
    return ok && written == static_cast<DWORD>(content.size());
#else
    Q_UNUSED(path);
    return false;
#endif
}

QStringList saveDialogFilters(const QString &leaf)
{
    QStringList filters;
    const QString ext = extensionOf(leaf);
    if (!ext.isEmpty()) {
        filters << QCoreApplication::translate("SaveNaming", "%1 file (*.%2)")
                       .arg(ext.toUpper(), ext);
    }
    filters << QCoreApplication::translate("SaveNaming", "All files (*)");
    return filters;
}

} // namespace savenaming
