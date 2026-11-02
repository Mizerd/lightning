#include "app/FontManager.h"

#include "app/SettingsManager.h"
#include "storage/AppDataPaths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QRawFont>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QSet>
#include <QVariantMap>

Q_LOGGING_CATEGORY(lcFonts, "lightning.fonts")

namespace {

const QString &defaultUiFamily()
{
    static const QString family = QStringLiteral("Manrope");
    return family;
}

const QString &defaultMonospaceFamily()
{
    static const QString family = QStringLiteral("JetBrains Mono");
    return family;
}

QStringList bundled()
{
    return { QStringLiteral("Manrope"),
             QStringLiteral("Inter"),
             QStringLiteral("IBM Plex Sans"),
             QStringLiteral("Source Sans 3"),
             QStringLiteral("Plus Jakarta Sans"),
             QStringLiteral("Space Grotesk"),
             QStringLiteral("JetBrains Mono") };
}

bool isOwnImportName(const QString &name)
{
    if (name.size() != 64 + 4)
        return false;
    if (!name.endsWith(QLatin1String(".ttf")) && !name.endsWith(QLatin1String(".otf")))
        return false;
    for (int i = 0; i < 64; ++i) {
        const QChar c = name.at(i);
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9'))
              || (c >= QLatin1Char('a') && c <= QLatin1Char('f'))))
            return false;
    }
    return true;
}

}

FontManager::FontManager(SettingsManager *settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
{
    refreshFamilyCache();
    if (m_settings) {
        connect(m_settings, &SettingsManager::uiFontChanged,
                this, &FontManager::selectionChanged);
        connect(m_settings, &SettingsManager::monoFontChanged,
                this, &FontManager::selectionChanged);
    }
}

bool FontManager::looksLikeSfnt(const QByteArray &head)
{
    if (head.size() < 4)
        return false;
    const uchar b0 = uchar(head.at(0));
    const uchar b1 = uchar(head.at(1));
    const uchar b2 = uchar(head.at(2));
    const uchar b3 = uchar(head.at(3));
    if (b0 == 0x00 && b1 == 0x01 && b2 == 0x00 && b3 == 0x00)
        return true;
    if (head.startsWith(QByteArrayLiteral("true")))
        return true;
    if (head.startsWith(QByteArrayLiteral("OTTO")))
        return true;
    return false;
}

bool FontManager::hasFontExtension(const QString &fileName)
{
    return fileName.endsWith(QLatin1String(".ttf"), Qt::CaseInsensitive)
        || fileName.endsWith(QLatin1String(".otf"), Qt::CaseInsensitive);
}

QString FontManager::importedFontsDir()
{
    const QString root = matrix::app_data::primaryRoot();
    if (root.isEmpty())
        return {};
    return root + QLatin1String("/fonts");
}

bool FontManager::isNonTextFace(const QString &family)
{
    static const char *const kNames[] = {
        "Noto Color Emoji", "Apple Color Emoji", "Segoe UI Emoji", "Twemoji",
        "JoyPixels",        "EmojiOne Color",    "Noto Emoji",
        "Material Symbols Rounded",
    };
    for (const char *name : kNames) {
        if (family.compare(QLatin1String(name), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

bool FontManager::isUsableUiFace(const QString &family)
{
    if (family.isEmpty() || family.startsWith(QLatin1Char('.')) || isNonTextFace(family))
        return false;
    if (!QFontDatabase::isSmoothlyScalable(family))
        return false;
    if (family.contains(QLatin1String("Rotated"), Qt::CaseInsensitive))
        return false;
    return true;
}

bool FontManager::facesLatinText(const QString &family)
{
    QFont probe(family);
    // Without NoFontMerging the answer would come from a fallback face.
    probe.setStyleStrategy(QFont::NoFontMerging);
    const QRawFont raw = QRawFont::fromFont(probe);
    if (!raw.isValid())
        return false;
    for (const char32_t c : { U'A', U'a', U'0', U'9' }) {
        if (!raw.supportsCharacter(c))
            return false;
    }
    return true;
}

void FontManager::refreshFamilyCache()
{
    m_familyCache.clear();
    const QStringList installed = QFontDatabase::families();
    m_familyCache.reserve(installed.size());
    for (const QString &family : installed)
        m_familyCache.append(family.toLower());

    m_uiFamilies.clear();
    QSet<QString> seen;
    for (const QString &family : bundled()) {
        if (hasFamily(family) && !seen.contains(family.toLower())) {
            seen.insert(family.toLower());
            m_uiFamilies.append(family);
        }
    }
    if (m_uiFamilies.isEmpty()) {
        m_uiFamilies.append(defaultUiFamily());
        seen.insert(defaultUiFamily().toLower());
    }
    m_monoFamilies.clear();
    QSet<QString> monoSeen;
    if (hasFamily(defaultMonospaceFamily())) {
        m_monoFamilies.append(defaultMonospaceFamily());
        monoSeen.insert(defaultMonospaceFamily().toLower());
    }
    for (const QString &family : installed) {
        if (family.startsWith(QLatin1Char('.')))
            continue;
        if (!seen.contains(family.toLower()) && isUsableUiFace(family)) {
            seen.insert(family.toLower());
            m_uiFamilies.append(family);
        }
        // Fixed pitch AND able to draw Latin text; isFixedPitch() alone
        // admits emoji faces. The costly Latin probe runs last, only for
        // families that already claim fixed pitch.
        if (!monoSeen.contains(family.toLower())
            && QFontDatabase::isFixedPitch(family) && !isNonTextFace(family)
            && facesLatinText(family)) {
            monoSeen.insert(family.toLower());
            m_monoFamilies.append(family);
        }
    }
    if (m_monoFamilies.isEmpty())
        m_monoFamilies.append(defaultMonospaceFamily());
}

bool FontManager::hasFamily(const QString &family) const
{
    const QString needle = family.trimmed().toLower();
    return !needle.isEmpty() && m_familyCache.contains(needle);
}

QStringList FontManager::bundledFamilies() const
{
    QStringList out;
    for (const QString &family : bundled()) {
        if (hasFamily(family))
            out.append(family);
    }
    if (out.isEmpty())
        out.append(defaultUiFamily());
    return out;
}

QStringList FontManager::uiFamilies() const { return m_uiFamilies; }

QStringList FontManager::monospaceFamilies() const { return m_monoFamilies; }

QString FontManager::storedUiFamily() const
{
    return m_settings ? m_settings->uiFont() : defaultUiFamily();
}

QString FontManager::storedMonospaceFamily() const
{
    return m_settings ? m_settings->monoFont() : defaultMonospaceFamily();
}

namespace {
bool listHas(const QStringList &list, const QString &family)
{
    const QString needle = family.trimmed();
    if (needle.isEmpty())
        return false;
    for (const QString &entry : list) {
        if (entry.compare(needle, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}
}

bool FontManager::uiFamilyAvailable() const
{
    return listHas(m_uiFamilies, storedUiFamily());
}

bool FontManager::monospaceFamilyAvailable() const
{
    return listHas(m_monoFamilies, storedMonospaceFamily());
}

QString FontManager::uiFamilyUnavailableReason() const
{
    const QString stored = storedUiFamily();
    if (listHas(m_uiFamilies, stored))
        return {};
    return hasFamily(stored) ? QStringLiteral("unusable")
                             : QStringLiteral("missing");
}

QString FontManager::monospaceFamilyUnavailableReason() const
{
    const QString stored = storedMonospaceFamily();
    if (listHas(m_monoFamilies, stored))
        return {};
    return hasFamily(stored) ? QStringLiteral("unusable")
                             : QStringLiteral("missing");
}

QString FontManager::uiFamily() const
{
    const QString stored = storedUiFamily();
    return listHas(m_uiFamilies, stored) ? stored : defaultUiFamily();
}

QString FontManager::monospaceFamily() const
{
    const QString stored = storedMonospaceFamily();
    return listHas(m_monoFamilies, stored) ? stored : defaultMonospaceFamily();
}

void FontManager::setUiFamily(const QString &family)
{
    if (m_settings)
        m_settings->setUiFont(family);
}

void FontManager::setMonospaceFamily(const QString &family)
{
    if (m_settings)
        m_settings->setMonoFont(family);
}

QStringList FontManager::storedImportFileNames() const
{
    if (!m_settings)
        return {};
    QStringList out;
    for (const QString &name : m_settings->importedFontFiles()) {
        if (isOwnImportName(name) && !out.contains(name))
            out.append(name);
        if (out.size() >= kMaxImportedFonts)
            break;
    }
    return out;
}

void FontManager::writeImportFileNames(const QStringList &names)
{
    if (m_settings)
        m_settings->setImportedFontFiles(names);
}

void FontManager::loadImportedFonts()
{
    const QString dir = importedFontsDir();
    m_imported.clear();
    int loaded = 0;
    for (const QString &name : storedImportFileNames()) {
        Imported entry;
        entry.fileName = name;
        const QString path = dir.isEmpty() ? QString()
                                           : dir + QLatin1Char('/') + name;
        QFile file(path);
        if (!path.isEmpty() && QFileInfo(path).isFile()
            && QFileInfo(path).size() > 0
            && QFileInfo(path).size() <= kMaxFontFileBytes
            && file.open(QIODevice::ReadOnly)) {
            const QByteArray head = file.read(4);
            file.close();
            if (looksLikeSfnt(head)) {
                const int handle = QFontDatabase::addApplicationFont(path);
                if (handle >= 0) {
                    entry.families = QFontDatabase::applicationFontFamilies(handle);
                    entry.available = !entry.families.isEmpty();
                    entry.handle = handle;
                    if (!entry.available)
                        QFontDatabase::removeApplicationFont(handle);
                }
            }
        }
        if (entry.available)
            ++loaded;
        m_imported.append(entry);
    }
    refreshFamilyCache();
    qCInfo(lcFonts) << "imported fonts:" << m_imported.size() << "recorded,"
                    << loaded << "loaded";
    Q_EMIT familiesChanged();
    Q_EMIT importedFontsChanged();
    Q_EMIT selectionChanged();
}

QVariantList FontManager::importedFonts() const
{
    QVariantList out;
    for (const Imported &entry : m_imported) {
        out.append(QVariantMap{
            { QStringLiteral("fileName"), entry.fileName },
            { QStringLiteral("families"), entry.families },
            { QStringLiteral("available"), entry.available },
        });
    }
    return out;
}

void FontManager::setImportError(const QString &category)
{
    if (m_lastImportError == category)
        return;
    m_lastImportError = category;
    Q_EMIT lastImportErrorChanged();
}

bool FontManager::importFontFile(const QUrl &fileUrl)
{
    if (!fileUrl.isValid() || !fileUrl.isLocalFile()) {
        setImportError(QStringLiteral("not_a_local_file"));
        return false;
    }
    const QString source = fileUrl.toLocalFile();
    const QFileInfo info(source);
    if (!hasFontExtension(info.fileName())) {
        setImportError(QStringLiteral("unsupported_extension"));
        return false;
    }
    if (!info.isFile()) {
        setImportError(QStringLiteral("not_a_file"));
        return false;
    }
    if (info.size() <= 0 || info.size() > kMaxFontFileBytes) {
        setImportError(info.size() <= 0 ? QStringLiteral("empty")
                                        : QStringLiteral("too_large"));
        return false;
    }
    QStringList names = storedImportFileNames();
    if (names.size() >= kMaxImportedFonts) {
        setImportError(QStringLiteral("store_full"));
        return false;
    }
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly)) {
        setImportError(QStringLiteral("unreadable"));
        return false;
    }
    const QByteArray bytes = in.read(kMaxFontFileBytes + 1);
    in.close();
    if (bytes.size() <= 0 || bytes.size() > kMaxFontFileBytes) {
        setImportError(QStringLiteral("too_large"));
        return false;
    }
    if (!looksLikeSfnt(bytes.left(4))) {
        setImportError(QStringLiteral("not_a_font"));
        return false;
    }

    const QString dir = importedFontsDir();
    if (dir.isEmpty() || !QDir().mkpath(dir)) {
        setImportError(QStringLiteral("no_store"));
        return false;
    }
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    const QString suffix = info.fileName().endsWith(QLatin1String(".otf"),
                                                    Qt::CaseInsensitive)
        ? QStringLiteral(".otf") : QStringLiteral(".ttf");
    const QString fileName = digest + suffix;
    if (names.contains(fileName)) {
        setImportError(QStringLiteral("already_imported"));
        return false;
    }
    const QString target = dir + QLatin1Char('/') + fileName;
    {
        QSaveFile out(target);
        if (!out.open(QIODevice::WriteOnly)) {
            setImportError(QStringLiteral("copy_failed"));
            return false;
        }
        out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        if (out.write(bytes) != bytes.size() || !out.commit()) {
            setImportError(QStringLiteral("copy_failed"));
            return false;
        }
    }

    const int handle = QFontDatabase::addApplicationFont(target);
    const QStringList families = handle >= 0
        ? QFontDatabase::applicationFontFamilies(handle) : QStringList();
    if (handle < 0 || families.isEmpty()) {
        if (handle >= 0)
            QFontDatabase::removeApplicationFont(handle);
        QFile::remove(target);
        setImportError(QStringLiteral("rejected_by_font_database"));
        return false;
    }

    Imported entry;
    entry.fileName = fileName;
    entry.families = families;
    entry.available = true;
    entry.handle = handle;
    m_imported.append(entry);
    names.append(fileName);
    writeImportFileNames(names);
    refreshFamilyCache();
    setImportError(QString());
    qCInfo(lcFonts) << "imported a font file;" << families.size()
                    << "family/families";
    Q_EMIT familiesChanged();
    Q_EMIT importedFontsChanged();
    Q_EMIT selectionChanged();
    return true;
}

bool FontManager::removeImportedFont(const QString &fileName)
{
    if (!isOwnImportName(fileName))
        return false;
    int at = -1;
    for (int i = 0; i < m_imported.size(); ++i) {
        if (m_imported.at(i).fileName == fileName) {
            at = i;
            break;
        }
    }
    if (at < 0)
        return false;
    if (m_imported.at(at).handle >= 0)
        QFontDatabase::removeApplicationFont(m_imported.at(at).handle);
    const QString dir = importedFontsDir();
    if (!dir.isEmpty())
        QFile::remove(dir + QLatin1Char('/') + fileName);
    m_imported.removeAt(at);
    QStringList names = storedImportFileNames();
    names.removeAll(fileName);
    writeImportFileNames(names);
    refreshFamilyCache();
    Q_EMIT familiesChanged();
    Q_EMIT importedFontsChanged();
    Q_EMIT selectionChanged();
    return true;
}

QString FontManager::emojiFamily()
{
    static const QString family = [] {
        static const char *const kCandidates[] = {
            "Noto Color Emoji", "Apple Color Emoji", "Segoe UI Emoji",
            "Twemoji",          "JoyPixels",         "EmojiOne Color",
            "Noto Emoji",
        };
        const QStringList installed = QFontDatabase::families();
        for (const char *candidate : kCandidates) {
            const QString name = QString::fromLatin1(candidate);
            if (installed.contains(name, Qt::CaseInsensitive))
                return name;
        }
        return QString();
    }();
    return family;
}

bool FontManager::installEmojiFallback(const QString &family)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    if (family.isEmpty())
        return false;
    if (QFontDatabase::applicationFallbackFontFamilies(QChar::Script_Common)
            .contains(family))
        return true;
    QFontDatabase::addApplicationFallbackFontFamily(QChar::Script_Common, family);
    return true;
#else
    Q_UNUSED(family);
    return false;
#endif
}

bool FontManager::emojiFallbackNeeded(const QVersionNumber &runningQt)
{
    return runningQt < QVersionNumber(6, 9);
}

QFont FontManager::withEmojiFallback(const QString &family, int pixelSize)
{
    QFont font(family);
    const QString emoji = emojiFamily();
    if (!emoji.isEmpty() && emoji.compare(family, Qt::CaseInsensitive) != 0)
        font.setFamilies({ family, emoji });
    font.setPixelSize(pixelSize);
    return font;
}
