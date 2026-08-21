#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <memory>

class QTranslator;
class QLocale;
class SettingsManager;

// Application-wide UI language.
//
// Qt owns the translation itself: strings are marked with qsTr()/tr(), the
// catalogs are Qt Linguist .ts files under i18n/, CMake compiles them to .qm
// and embeds them at :/i18n/, and a QTranslator does the lookup. This class
// only decides WHICH catalog is installed and tells the QML engine when to
// re-evaluate its bindings.
//
// The stored setting is a POLICY, not a language: "system" means "follow the
// desktop", and anything else is an explicit choice. Those are different
// states and must not be collapsed — resolving "system" to "en" at write time
// would freeze a user's language the first time they opened Settings.
class LocalizationManager : public QObject
{
    Q_OBJECT

    // The stored policy: "system", or one of supportedCodes().
    Q_PROPERTY(QString language READ language WRITE setLanguage
                   NOTIFY languageChanged)
    // What that policy actually resolves to right now. Always a real code.
    Q_PROPERTY(QString effectiveLanguage READ effectiveLanguage
                   NOTIFY languageChanged)
    // [{ code, name, endonym, rightToLeft }] for the Settings picker, with
    // "system" first. `name` is English, `endonym` is the language's own
    // name — the picker shows the endonym, because a user who cannot read
    // the current UI language cannot find "Russian" in a list either.
    // NOT constant: the "System default" row is itself a translated string,
    // so the list has to be re-read when the language changes or that one
    // row stays in the language the user just left.
    Q_PROPERTY(QVariantList languages READ languages NOTIFY languageChanged)
    Q_PROPERTY(bool rightToLeft READ rightToLeft NOTIFY languageChanged)
    // False in a build configured without Qt's Linguist tools: every string
    // then renders as its English source. Surfaced so the Settings page can
    // say so rather than offering ten languages that all look identical.
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

    // QML-reachable views of the static table below. The statics are the
    // testable form; these exist because Q_INVOKABLE cannot be static.
    Q_INVOKABLE QString endonymOf(const QString &code) const
    { return endonym(code); }
    Q_INVOKABLE QString englishNameOf(const QString &code) const
    { return englishName(code); }

    // Installs the catalog for the stored policy. Call once, before the QML
    // engine loads, so the first frame is already translated.
    void applyStoredLanguage();

    // ---- Pure, testable policy -------------------------------------------

    // Every code Lightning ships a catalog for, English first.
    static QStringList supportedCodes();
    static bool isSupported(const QString &code);
    static bool isRightToLeft(const QString &code);
    static QString endonym(const QString &code);
    static QString englishName(const QString &code);

    // Maps ONE locale tag ("es_MX", "pt-BR", "zh-Hans-CN", "fr_CA") onto a
    // supported code, or "" when nothing matches. Regional variants collapse
    // onto their language; script matters only for Chinese.
    static QString matchLanguageTag(const QString &tag);
    // Walks a preference list in order (QLocale::uiLanguages()) and returns
    // the first supported match, or "en" when none match.
    static QString matchPreferenceList(const QStringList &uiLanguages);
    // The system's language, resolved through the two functions above.
    static QString systemLanguage();
    static QString systemLanguage(const QLocale &locale);

Q_SIGNALS:
    void languageChanged();
    // Emitted after the translators have changed. main.cpp connects this to
    // QQmlApplicationEngine::retranslate(), which re-evaluates every binding
    // that reads qsTr(). Kept as a signal rather than an engine pointer so
    // this class stays constructible in a headless test.
    void retranslateRequested();

private:
    void applyLanguage(const QString &policy);
    bool installCatalog(const QString &code);
    void removeTranslators();

    SettingsManager *m_settings = nullptr;
    std::unique_ptr<QTranslator> m_appTranslator;
    QString m_effective = QStringLiteral("en");
};
