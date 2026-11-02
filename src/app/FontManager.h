#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVersionNumber>

class SettingsManager;

class FontManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList bundledFamilies READ bundledFamilies CONSTANT)
    Q_PROPERTY(QStringList uiFamilies READ uiFamilies NOTIFY familiesChanged)
    Q_PROPERTY(QStringList monospaceFamilies READ monospaceFamilies
                   NOTIFY familiesChanged)
    Q_PROPERTY(QString uiFamily READ uiFamily NOTIFY selectionChanged)
    Q_PROPERTY(QString monospaceFamily READ monospaceFamily
                   NOTIFY selectionChanged)
    Q_PROPERTY(QString storedUiFamily READ storedUiFamily NOTIFY selectionChanged)
    Q_PROPERTY(QString storedMonospaceFamily READ storedMonospaceFamily
                   NOTIFY selectionChanged)
    Q_PROPERTY(QString uiFamilyUnavailableReason READ uiFamilyUnavailableReason
                   NOTIFY selectionChanged)
    Q_PROPERTY(QString monospaceFamilyUnavailableReason
                   READ monospaceFamilyUnavailableReason NOTIFY selectionChanged)
    Q_PROPERTY(bool uiFamilyAvailable READ uiFamilyAvailable
                   NOTIFY selectionChanged)
    Q_PROPERTY(bool monospaceFamilyAvailable READ monospaceFamilyAvailable
                   NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList importedFonts READ importedFonts
                   NOTIFY importedFontsChanged)
    Q_PROPERTY(int importedFontLimit READ importedFontLimit CONSTANT)
    Q_PROPERTY(QString lastImportError READ lastImportError
                   NOTIFY lastImportErrorChanged)

public:
    static constexpr qint64 kMaxFontFileBytes = 32LL * 1024 * 1024;
    static constexpr int kMaxImportedFonts = 16;

    static QString emojiFamily();
    static QFont withEmojiFallback(const QString &family, int pixelSize);
    static bool installEmojiFallback(const QString &family);
    // Whether the Qt running this process needs installEmojiFallback(): only
    // 6.8, which picks a monochrome face on its own. Every Qt up to 6.11
    // reads an uninitialised FcCharSet in its fontconfig fallback check when
    // a fallback family's match has no charset, so the extra family crashed a
    // Flatpak user's 0.10.0 at the first label (reported 2026-10-01); newer
    // Qt draws colour emoji without it.
    static bool emojiFallbackNeeded(const QVersionNumber &runningQt);

    explicit FontManager(SettingsManager *settings, QObject *parent = nullptr);

    void loadImportedFonts();

    QStringList bundledFamilies() const;
    QStringList uiFamilies() const;
    QStringList monospaceFamilies() const;

    QString uiFamily() const;
    QString monospaceFamily() const;
    QString storedUiFamily() const;
    QString storedMonospaceFamily() const;
    bool uiFamilyAvailable() const;
    QString uiFamilyUnavailableReason() const;
    QString monospaceFamilyUnavailableReason() const;
    bool monospaceFamilyAvailable() const;

    QVariantList importedFonts() const;
    int importedFontLimit() const { return kMaxImportedFonts; }
    QString lastImportError() const { return m_lastImportError; }

    Q_INVOKABLE bool hasFamily(const QString &family) const;

    Q_INVOKABLE void setUiFamily(const QString &family);
    Q_INVOKABLE void setMonospaceFamily(const QString &family);

    Q_INVOKABLE bool importFontFile(const QUrl &fileUrl);
    Q_INVOKABLE bool removeImportedFont(const QString &fileName);

    static bool looksLikeSfnt(const QByteArray &head);
    // Whether the face itself (no font merging) draws Latin letters and
    // digits. isFixedPitch() is true for emoji faces, which also carry digit
    // glyphs as keycap bases. Used for the monospace list only: UI faces for
    // non-Latin locales may carry no Latin at all.
    static bool facesLatinText(const QString &family);
    static bool isNonTextFace(const QString &family);
    static bool isUsableUiFace(const QString &family);
    static bool hasFontExtension(const QString &fileName);

Q_SIGNALS:
    void familiesChanged();
    void selectionChanged();
    void importedFontsChanged();
    void lastImportErrorChanged();

private:
    struct Imported {
        QString fileName;
        QStringList families;
        bool available = false;
        int handle = -1;
    };

    static QString importedFontsDir();
    void refreshFamilyCache();
    void setImportError(const QString &category);
    QStringList storedImportFileNames() const;
    void writeImportFileNames(const QStringList &names);

    SettingsManager *m_settings = nullptr;
    QList<Imported> m_imported;
    QStringList m_uiFamilies;
    QStringList m_monoFamilies;
    QStringList m_familyCache;
    QString m_lastImportError;
};
