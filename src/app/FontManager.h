#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>

class SettingsManager;

// The UI and monospace typeface the application renders with, and the small
// store of fonts the user has imported by hand.
//
// WHY THIS IS A SEPARATE CLASS AND NOT MORE SettingsManager.
// SettingsManager is linked against Qt6::Core ALONE by about twenty test
// targets — that is why QScreen was kept out of it, and QFontDatabase is in
// exactly the same position (Qt6::Gui). So the split is:
//   * SettingsManager PERSISTS a family name. It validates the string
//     syntactically and nothing else; it has no way to ask whether a font
//     exists and must not pretend to.
//   * FontManager RESOLVES that name against the fonts this host actually
//     has, and answers what to render.
// The consequence is the behaviour that matters: a family that disappears
// between two launches (an uninstalled system font, an imported file the user
// deleted) renders as the bundled default, and the STORED CHOICE IS LEFT
// ALONE. Reinstall the font and it comes back. Rewriting the setting on the
// user's behalf would silently destroy a preference because a font was
// temporarily absent.
//
// IMPORTING A FONT FILE — the security position, stated rather than implied.
// A font file is parsed by FreeType, which is a real attack surface, so this
// is deliberately the narrowest useful shape:
//   * the ONLY entry point is importFontFile(), taking a local file: URL that
//     the user picked in a file dialog. Nothing here reads a path out of a
//     Matrix event, a URL, a setting written by anything but this class, or
//     any other remote input, and this class holds no MatrixClient and no
//     network object at all — the test target links neither;
//   * the extension must be .ttf or .otf AND the first bytes must carry an
//     sfnt signature. The name alone is never the decision, exactly as the
//     custom-app-icon import sniffs magic instead of trusting a file name;
//   * the file is bounded at kMaxFontFileBytes before a byte is read, and at
//     most kMaxImportedFonts may be held;
//   * QFontDatabase must ACTUALLY accept it and report at least one family.
//     A file FreeType refuses is discarded, not recorded — an entry the user
//     could select and never see applied is worse than a refusal;
//   * the accepted bytes are COPIED into Lightning's own app-data directory
//     and every later launch loads that private copy. The path the user
//     picked is never stored and never read again, so an imported font can
//     never be loaded from a network share, a removable disk, or a directory
//     another user can write — which is the "never auto-load from a shared or
//     network location" rule enforced by construction rather than by a
//     blocklist of path shapes.
// This is judged shippable, not waved through: a user who can be talked into
// picking a hostile font file here could equally be talked into installing it
// system-wide, where fontconfig hands the same bytes to the same FreeType.
// The import adds no reach that a user-chosen font did not already have. What
// it must never become is a path that something else can name.
class FontManager : public QObject
{
    Q_OBJECT
    // The families Lightning ships (data/fonts, loaded in main.cpp). Always
    // present, so the picker always has a working answer.
    Q_PROPERTY(QStringList bundledFamilies READ bundledFamilies CONSTANT)
    // Everything selectable: the bundled set first, then every other family
    // the host has, then the imported ones.
    Q_PROPERTY(QStringList uiFamilies READ uiFamilies NOTIFY familiesChanged)
    // The fixed-pitch subset, for the code/monospace role.
    Q_PROPERTY(QStringList monospaceFamilies READ monospaceFamilies
                   NOTIFY familiesChanged)
    // What to RENDER. Resolved: the stored choice when the host has it, the
    // bundled default when it does not.
    Q_PROPERTY(QString uiFamily READ uiFamily NOTIFY selectionChanged)
    Q_PROPERTY(QString monospaceFamily READ monospaceFamily
                   NOTIFY selectionChanged)
    // What the user CHOSE, installed or not. The picker reads this so a row
    // stays selected while the font is missing, instead of appearing to have
    // silently reset itself.
    Q_PROPERTY(QString storedUiFamily READ storedUiFamily NOTIFY selectionChanged)
    Q_PROPERTY(QString storedMonospaceFamily READ storedMonospaceFamily
                   NOTIFY selectionChanged)
    // False when the stored choice is not on this host — the disclosure that
    // keeps the fallback from being a silent lie.
    Q_PROPERTY(bool uiFamilyAvailable READ uiFamilyAvailable
                   NOTIFY selectionChanged)
    Q_PROPERTY(bool monospaceFamilyAvailable READ monospaceFamilyAvailable
                   NOTIFY selectionChanged)
    // One map per imported font: fileName, families (QStringList), available.
    Q_PROPERTY(QVariantList importedFonts READ importedFonts
                   NOTIFY importedFontsChanged)
    Q_PROPERTY(int importedFontLimit READ importedFontLimit CONSTANT)
    // Machine-readable category for the last refused import, empty after a
    // successful one. Never a path and never the picked file's name.
    Q_PROPERTY(QString lastImportError READ lastImportError
                   NOTIFY lastImportErrorChanged)

public:
    // 32 MiB is generous for a font (a full CJK face is ~20 MB) and small
    // enough that a hostile file cannot be used to exhaust memory or the
    // app-data directory. The same ceiling the icon import uses.
    static constexpr qint64 kMaxFontFileBytes = 32LL * 1024 * 1024;
    static constexpr int kMaxImportedFonts = 16;

    explicit FontManager(SettingsManager *settings, QObject *parent = nullptr);

    // Registers every stored import with QFontDatabase. Call once, after
    // QGuiApplication exists and the bundled faces are loaded, BEFORE the
    // first frame — an imported family must be resolvable by the time the
    // window font is applied.
    void loadImportedFonts();

    QStringList bundledFamilies() const;
    QStringList uiFamilies() const;
    QStringList monospaceFamilies() const;

    QString uiFamily() const;
    QString monospaceFamily() const;
    QString storedUiFamily() const;
    QString storedMonospaceFamily() const;
    bool uiFamilyAvailable() const;
    bool monospaceFamilyAvailable() const;

    QVariantList importedFonts() const;
    int importedFontLimit() const { return kMaxImportedFonts; }
    QString lastImportError() const { return m_lastImportError; }

    // Does this host have the family, under this exact name? Case-insensitive,
    // because QFontDatabase reports a canonical case that a stored value from
    // another platform need not match.
    Q_INVOKABLE bool hasFamily(const QString &family) const;

    // Persist a selection. The family is stored VERBATIM after a syntactic
    // check; it does not have to be installed right now.
    Q_INVOKABLE void setUiFamily(const QString &family);
    Q_INVOKABLE void setMonospaceFamily(const QString &family);

    // Import a font the user picked. Returns true when the file was accepted,
    // copied, registered and recorded; false sets lastImportError.
    Q_INVOKABLE bool importFontFile(const QUrl &fileUrl);
    // Forget an imported font. The private copy is deleted. A selection that
    // named one of its families is deliberately NOT rewritten — the same rule
    // as an uninstalled system font: it falls back and can come back.
    Q_INVOKABLE bool removeImportedFont(const QString &fileName);

    // ---- Pure validators, exposed so they can be tested without a font
    // database, a settings store or a window. ----

    // An sfnt wrapper Qt/FreeType will recognise: 0x00010000 (TrueType),
    // "true" (legacy Apple), or "OTTO" (CFF outlines). Deliberately REFUSES
    // "ttcf" collections (one file, several faces — out of scope for a picker
    // that names one family) and the "wOFF"/"wOF2" web wrappers, which are a
    // transport format and not what a desktop font picker offers.
    static bool looksLikeSfnt(const QByteArray &head);
    // .ttf / .otf only, case-insensitive. The extension is a necessary and
    // never a sufficient condition — looksLikeSfnt() decides.
    static bool hasFontExtension(const QString &fileName);

Q_SIGNALS:
    void familiesChanged();
    void selectionChanged();
    void importedFontsChanged();
    void lastImportErrorChanged();

private:
    struct Imported {
        QString fileName;      // basename inside importedFontsDir(), never a
                               // path the user typed or picked
        QStringList families;  // what QFontDatabase reported for it
        bool available = false;
        int handle = -1;       // QFontDatabase application font id
    };

    // <app data root>/fonts — Lightning's own directory, beside the branding
    // directory the custom app icon uses. Created with the platform default
    // for a user data directory (mkpath); the COPIES inside it are written
    // owner-only. Nothing here is ever a path the user typed.
    static QString importedFontsDir();
    void refreshFamilyCache();
    void setImportError(const QString &category);
    QStringList storedImportFileNames() const;
    void writeImportFileNames(const QStringList &names);

    SettingsManager *m_settings = nullptr;
    QList<Imported> m_imported;
    // The two picker lists, built once per family-set change rather than per
    // read. The mono list is the expensive one: QFontDatabase::isFixedPitch()
    // resolves a QFont per family, and on a machine with several hundred
    // installed faces that is a GUI-thread cost paid every time a binding
    // re-evaluates. Both are read from a QML `model:` binding.
    QStringList m_uiFamilies;
    QStringList m_monoFamilies;
    // Lower-cased family names present on this host, rebuilt whenever an
    // application font is added or removed. QFontDatabase::families() is not
    // cheap and this is read from a binding.
    QStringList m_familyCache;
    QString m_lastImportError;
};
