#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>

class SettingsManager;

// Resolves the UI and monospace typefaces and manages user-imported fonts.
//
// Kept out of SettingsManager, which many test targets link against Qt6::Core
// alone. SettingsManager persists a family name; FontManager resolves it
// against the host's fonts. A family that disappears renders as the bundled
// default and the stored choice is left alone, so reinstalling it restores it.
//
// Font import is the narrowest useful shape, since FreeType is attack surface:
//   * the only entry point is importFontFile() with a user-picked local file;
//     nothing reads a path from Matrix events, URLs or remote input;
//   * .ttf/.otf extension AND an sfnt signature; the name alone never decides;
//   * size- and count-bounded before anything is read;
//   * QFontDatabase must accept it and report a family, or it is discarded;
//   * the bytes are copied into Lightning's app-data directory and only that
//     private copy is ever loaded, so an import can never be read from a
//     network share or another user's directory.
// A user who can be talked into picking a hostile font could equally install
// it system-wide; the import must never become a path something else can name.
class FontManager : public QObject
{
    Q_OBJECT
    // The families Lightning ships (data/fonts, loaded in main.cpp).
    Q_PROPERTY(QStringList bundledFamilies READ bundledFamilies CONSTANT)
    // Bundled families first, then the host's, then imported ones.
    Q_PROPERTY(QStringList uiFamilies READ uiFamilies NOTIFY familiesChanged)
    Q_PROPERTY(QStringList monospaceFamilies READ monospaceFamilies
                   NOTIFY familiesChanged)
    // What to render: the stored choice if usable, else the bundled default.
    Q_PROPERTY(QString uiFamily READ uiFamily NOTIFY selectionChanged)
    Q_PROPERTY(QString monospaceFamily READ monospaceFamily
                   NOTIFY selectionChanged)
    // What the user chose, installed or not, so the picker keeps it selected.
    Q_PROPERTY(QString storedUiFamily READ storedUiFamily NOTIFY selectionChanged)
    Q_PROPERTY(QString storedMonospaceFamily READ storedMonospaceFamily
                   NOTIFY selectionChanged)
    // Discloses why the stored choice is not being rendered.
    Q_PROPERTY(QString uiFamilyUnavailableReason READ uiFamilyUnavailableReason
                   NOTIFY selectionChanged)
    Q_PROPERTY(QString monospaceFamilyUnavailableReason
                   READ monospaceFamilyUnavailableReason NOTIFY selectionChanged)
    Q_PROPERTY(bool uiFamilyAvailable READ uiFamilyAvailable
                   NOTIFY selectionChanged)
    Q_PROPERTY(bool monospaceFamilyAvailable READ monospaceFamilyAvailable
                   NOTIFY selectionChanged)
    // One map per imported font: fileName, families (QStringList), available.
    Q_PROPERTY(QVariantList importedFonts READ importedFonts
                   NOTIFY importedFontsChanged)
    Q_PROPERTY(int importedFontLimit READ importedFontLimit CONSTANT)
    // Category of the last refused import; never a path or file name.
    Q_PROPERTY(QString lastImportError READ lastImportError
                   NOTIFY lastImportErrorChanged)

public:
    // Room for a full CJK face (~20 MB) while bounding memory and disk use.
    static constexpr qint64 kMaxFontFileBytes = 32LL * 1024 * 1024;
    static constexpr int kMaxImportedFonts = 16;

    // The host's colour emoji face ("" when none), resolved once. Named
    // explicitly because Qt's automatic fallback differs by version (6.8
    // prefers a monochrome face that claims the codepoint).
    static QString emojiFamily();
    // `family` first, the emoji face second (when there is one).
    static QFont withEmojiFallback(const QString &family, int pixelSize);
    // Makes `family` Qt's fallback for Common-script characters (where emoji
    // live) the requested font lacks. This covers QML text that sets a single
    // `font.family`, which replaces any per-surface family list. Returns false
    // when `family` is empty or Qt predates the API (6.8).
    static bool installEmojiFallback(const QString &family);

    explicit FontManager(SettingsManager *settings, QObject *parent = nullptr);

    // Registers every stored import. Call once after the bundled faces are
    // loaded and before the window font is applied.
    void loadImportedFonts();

    QStringList bundledFamilies() const;
    QStringList uiFamilies() const;
    QStringList monospaceFamilies() const;

    QString uiFamily() const;
    QString monospaceFamily() const;
    QString storedUiFamily() const;
    QString storedMonospaceFamily() const;
    bool uiFamilyAvailable() const;
    // "" when the stored family is in use, "missing" when not installed,
    // "unusable" when installed but not a text face for this surface.
    QString uiFamilyUnavailableReason() const;
    QString monospaceFamilyUnavailableReason() const;
    bool monospaceFamilyAvailable() const;

    QVariantList importedFonts() const;
    int importedFontLimit() const { return kMaxImportedFonts; }
    QString lastImportError() const { return m_lastImportError; }

    // Case-insensitive: a stored value from another platform may differ in
    // case from QFontDatabase's canonical name.
    Q_INVOKABLE bool hasFamily(const QString &family) const;

    // Stored verbatim after a syntactic check; need not be installed now.
    Q_INVOKABLE void setUiFamily(const QString &family);
    Q_INVOKABLE void setMonospaceFamily(const QString &family);

    // Returns false and sets lastImportError on refusal.
    Q_INVOKABLE bool importFontFile(const QUrl &fileUrl);
    // Deletes the private copy. A selection naming it is not rewritten.
    Q_INVOKABLE bool removeImportedFont(const QString &fileName);

    // ---- Validators, public for tests ----

    // 0x00010000 (TrueType), "true" (legacy Apple) or "OTTO" (CFF). Refuses
    // "ttcf" collections and the "wOFF"/"wOF2" web wrappers.
    static bool looksLikeSfnt(const QByteArray &head);
    // Whether the face itself (no font merging) draws Latin letters and
    // digits. isFixedPitch() is true for emoji faces, which also carry digit
    // glyphs as keycap bases. Used for the monospace list only: UI faces for
    // non-Latin locales may carry no Latin at all.
    static bool facesLatinText(const QString &family);
    static bool isNonTextFace(const QString &family);
    // Necessary, never sufficient: looksLikeSfnt() decides.
    static bool hasFontExtension(const QString &fileName);

Q_SIGNALS:
    void familiesChanged();
    void selectionChanged();
    void importedFontsChanged();
    void lastImportErrorChanged();

private:
    struct Imported {
        QString fileName;      // basename inside importedFontsDir()
        QStringList families;
        bool available = false;
        int handle = -1;       // QFontDatabase application font id
    };

    // <app data root>/fonts; copies inside are written owner-only.
    static QString importedFontsDir();
    void refreshFamilyCache();
    void setImportError(const QString &category);
    QStringList storedImportFileNames() const;
    void writeImportFileNames(const QStringList &names);

    SettingsManager *m_settings = nullptr;
    QList<Imported> m_imported;
    // Picker lists, rebuilt only when the family set changes: building the
    // mono list resolves a font per family, too costly per binding read.
    QStringList m_uiFamilies;
    QStringList m_monoFamilies;
    // Lower-cased installed families; QFontDatabase::families() is costly.
    QStringList m_familyCache;
    QString m_lastImportError;
};
