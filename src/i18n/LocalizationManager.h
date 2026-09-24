#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <memory>

class QTranslator;
class QLocale;
class SettingsManager;

// Application-wide UI language. Qt does the translation (qsTr()/tr(), .ts
// catalogs under i18n/ compiled to embedded .qm, a QTranslator); this class
// picks the catalog and tells QML to re-evaluate.
//
// The setting is a policy: "system" follows the desktop, anything else is an
// explicit choice. Resolving "system" at write time would freeze the language.
class LocalizationManager : public QObject
{
    Q_OBJECT

    // The stored policy: "system", or one of supportedCodes().
    Q_PROPERTY(QString language READ language WRITE setLanguage
                   NOTIFY languageChanged)
    // What that policy actually resolves to right now. Always a real code.
    Q_PROPERTY(QString effectiveLanguage READ effectiveLanguage
                   NOTIFY languageChanged)
    // [{ code, name, endonym, rightToLeft }] for the picker, "system" first.
    // The picker shows endonyms (a user who cannot read the UI cannot find
    // "Russian" either). Not constant: the "System default" row is itself
    // translated.
    Q_PROPERTY(QVariantList languages READ languages NOTIFY languageChanged)
    Q_PROPERTY(bool rightToLeft READ rightToLeft NOTIFY languageChanged)
    // False in a build without Linguist tools (every string is English), so
    // Settings can say so.
    Q_PROPERTY(bool translationsAvailable READ translationsAvailable CONSTANT)

public:
    explicit LocalizationManager(SettingsManager *settings,
                                 QObject *parent = nullptr);
    ~LocalizationManager() override;

    QString language() const;
    void setLanguage(const QString &policy);

    QString effectiveLanguage() const { return m_effective; }
    QVariantList languages() const;
    bool rightToLeft() const;
    static bool translationsAvailable();

    // QML views of the static table; Q_INVOKABLE cannot be static.
    Q_INVOKABLE QString endonymOf(const QString &code) const
    { return endonym(code); }
    Q_INVOKABLE QString englishNameOf(const QString &code) const
    { return englishName(code); }

    // Installs the stored policy's catalog. Call before QML loads.
    void applyStoredLanguage();

    // ---- Pure, testable policy -------------------------------------------

    // Every code Lightning ships a catalog for, English first.
    static QStringList supportedCodes();
    static bool isSupported(const QString &code);
    static bool isRightToLeft(const QString &code);
    static QString endonym(const QString &code);
    static QString englishName(const QString &code);

    // Maps one locale tag ("es_MX", "pt-BR", "zh-Hans-CN") to a supported code,
    // or "". Regions collapse to their language; script matters only for
    // Chinese.
    static QString matchLanguageTag(const QString &tag);
    // Walks a preference list in order (QLocale::uiLanguages()) and returns
    // the first supported match, or "en" when none match.
    static QString matchPreferenceList(const QStringList &uiLanguages);
    // The system's language, resolved through the two functions above.
    static QString systemLanguage();
    static QString systemLanguage(const QLocale &locale);

Q_SIGNALS:
    void languageChanged();
    // Emitted after the translators changed; main.cpp connects it to
    // QQmlApplicationEngine::retranslate(). A signal keeps this class headless-
    // testable.
    void retranslateRequested();

private:
    void applyLanguage(const QString &policy);
    bool installCatalog(const QString &code);
    void removeTranslators();

    SettingsManager *m_settings = nullptr;
    std::unique_ptr<QTranslator> m_appTranslator;
    QString m_effective = QStringLiteral("en");
};
