#pragma once

#include <QLocale>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <memory>

class QJSEngine;
class QQmlEngine;
class QTranslator;
class SettingsManager;

// The locale every date and number on screen is formatted with, for QML and
// C++ alike, and the one signal that says the UI language changed.
//
// QML: `Qt.locale(AppLocale.name)`. Reading `name` is what makes a binding
// re-run on a switch: `Qt.locale()` with no argument returns the right locale
// but notifies nothing, so a date label kept the old language until its
// delegate was rebuilt. A QML singleton rather than `app.localization`, so a
// component works under every test harness's stand-in `app`.
//
// C++: a backend that keeps text worded in the UI language (room-list
// previews) re-words it on `changed()`. Process-wide, like the default
// QLocale and the installed translators it describes; LocalizationManager is
// the only writer.
class AppLocale : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    // The formatting locale's BCP 47 name ("de-DE", "en-GB"), which
    // Qt.locale() accepts.
    Q_PROPERTY(QString name READ name NOTIFY changed)

public:
    static AppLocale *instance();
    // The QML singleton is the process-wide instance, owned by C++.
    static AppLocale *create(QQmlEngine *, QJSEngine *);

    QString name() const { return QLocale().bcp47Name(); }

    // Announces a new UI language (translators and default QLocale already in
    // place). LocalizationManager calls it; tests may.
    void notifyChanged() { Q_EMIT changed(); }

Q_SIGNALS:
    void changed();

private:
    AppLocale() = default;
};

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
    // The locale dates and numbers are formatted with for the UI language
    // `uiCode`: the desktop's time category (i18n::timeCategoryLocale) when
    // it speaks that language (LC_TIME=en_GB in an English UI), else the UI
    // language's own CLDR locale, in the system's territory when the system
    // speaks the same language. Never QLocale::system() itself: on Unix that
    // answers month names from LC_TIME and separators from LC_NUMERIC, so
    // LANG=en_US with LC_TIME=lt_LT gave "10 spal." in an English UI.
    static QLocale formattingLocale(const QString &uiCode, const QLocale &system,
                                    const QLocale &timeCategory);

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
