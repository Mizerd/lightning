#include "app/FontManager.h"

#include "app/SettingsManager.h"
#include "storage/AppDataPaths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QSet>
#include <QVariantMap>

Q_LOGGING_CATEGORY(lcFonts, "lightning.fonts")

namespace {

// The default family, and the one every fallback lands on. It is bundled, so
// it is present in every build on every platform.
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

// The bundled faces, in the order main.cpp registers them. Material Symbols
// (an icon subset) and the emoji face are deliberately absent: neither is a
// text face and offering either would produce an unreadable UI.
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

// A file name we generated: 64 lowercase hex characters plus the extension we
// chose. Checked on the way OUT of settings, so a hand-edited config cannot
// make this class read or delete a path of someone else's choosing.
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

} // namespace

FontManager::FontManager(SettingsManager *settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
{
    refreshFamilyCache();
    if (m_settings) {
        // The selection is per-account, so switching accounts changes what
        // the resolved properties answer.
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
    // 0x00010000 — TrueType outlines.
    if (b0 == 0x00 && b1 == 0x01 && b2 == 0x00 && b3 == 0x00)
        return true;
    // "true" — the legacy Apple tag, still shipped by some foundries.
    if (head.startsWith(QByteArrayLiteral("true")))
        return true;
    // "OTTO" — CFF outlines.
    if (head.startsWith(QByteArrayLiteral("OTTO")))
        return true;
    // "ttcf" (a collection) and "wOFF"/"wOF2" (web transport wrappers) fall
    // through on purpose; see the header.
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

void FontManager::refreshFamilyCache()
{
    m_familyCache.clear();
    const QStringList installed = QFontDatabase::families();
    m_familyCache.reserve(installed.size());
    for (const QString &family : installed)
        m_familyCache.append(family.toLower());

    // Bundled first — they are the ones this design was drawn against and the
    // ones guaranteed present — then everything else the host has, in
    // QFontDatabase's own order.
    m_uiFamilies.clear();
    QSet<QString> seen;
    for (const QString &family : bundled()) {
        if (hasFamily(family) && !seen.contains(family.toLower())) {
            seen.insert(family.toLower());
            m_uiFamilies.append(family);
        }
    }
    if (m_uiFamilies.isEmpty()) {
        // A build whose resources failed to load must still offer something.
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
        // A private/aliased face, and an ICON SUBSET, are not text faces a
        // person picks a user interface in.
        if (family.startsWith(QLatin1Char('.')))
            continue;
        if (family.compare(QLatin1String("Material Symbols Rounded"),
                           Qt::CaseInsensitive) != 0
            && !seen.contains(family.toLower())) {
            seen.insert(family.toLower());
            m_uiFamilies.append(family);
        }
        if (!monoSeen.contains(family.toLower())
            && QFontDatabase::isFixedPitch(family)) {
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

bool FontManager::uiFamilyAvailable() const
{
    return hasFamily(storedUiFamily());
}

bool FontManager::monospaceFamilyAvailable() const
{
    return hasFamily(storedMonospaceFamily());
}

QString FontManager::uiFamily() const
{
    // THE fallback rule. The stored value is read, not written: a font that is
    // missing today may be installed again tomorrow, and rewriting the setting
    // would destroy the user's choice on its behalf.
    const QString stored = storedUiFamily();
    return hasFamily(stored) ? stored : defaultUiFamily();
}

QString FontManager::monospaceFamily() const
{
    const QString stored = storedMonospaceFamily();
    return hasFamily(stored) ? stored : defaultMonospaceFamily();
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
        // Every gate the import applied is applied AGAIN here. The copy lives
        // in our own directory, but "our own directory" is a claim about a
        // filesystem, and a record that outlived its file must degrade to
        // "unavailable" rather than to a font database call on whatever is
        // there now.
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
    // A LOCAL file the user picked, and nothing else. A remote URL here would
    // be a download path this class deliberately does not have.
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
    // Bounded BEFORE anything is read.
    if (info.size() <= 0 || info.size() > kMaxFontFileBytes) {
        setImportError(info.size() <= 0 ? QStringLiteral("empty")
                                        : QStringLiteral("too_large"));
        return false;
    }
    QStringList names = storedImportFileNames();
    if (names.size() >= kMaxImportedFonts) {
        // Refusal, never eviction: a full store must not silently discard a
        // font the user asked to keep. Same rule as the saved-media store.
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
    // The name is never the decision.
    if (!looksLikeSfnt(bytes.left(4))) {
        setImportError(QStringLiteral("not_a_font"));
        return false;
    }

    const QString dir = importedFontsDir();
    if (dir.isEmpty() || !QDir().mkpath(dir)) {
        setImportError(QStringLiteral("no_store"));
        return false;
    }
    // Content-addressed, like the saved-media store: importing the same file
    // twice is one entry, and the recorded name can never carry anything the
    // user typed.
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

    // Only now does FreeType see it, and only from OUR copy. A file the font
    // database refuses, or that carries no family name, is not recorded: an
    // entry the user could select and never see applied would be a worse
    // outcome than a refusal.
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
    // The selection is deliberately NOT rewritten. It falls back exactly as an
    // uninstalled system font does, and re-importing brings it back.
    Q_EMIT familiesChanged();
    Q_EMIT importedFontsChanged();
    Q_EMIT selectionChanged();
    return true;
}
