#pragma once

#include <QDate>
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

class AppLocale : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QString name READ name NOTIFY changed)
    Q_PROPERTY(double dayStartMs READ dayStartMs NOTIFY todayChanged)

public:
    static AppLocale *instance();
    static AppLocale *create(QQmlEngine *, QJSEngine *);

    QString name() const { return QLocale().bcp47Name(); }

    void notifyChanged() { Q_EMIT changed(); }

    // Local midnight of the current day; relative dates in QML bind to it so
    // they re-evaluate when the date changes.
    double dayStartMs() const;
    void setToday(const QDate &today);

Q_SIGNALS:
    void changed();
    void todayChanged();

private:
    AppLocale() = default;

    QDate m_today = QDate::currentDate();
};

class LocalizationManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString language READ language WRITE setLanguage
                   NOTIFY languageChanged)
    Q_PROPERTY(QString effectiveLanguage READ effectiveLanguage
                   NOTIFY languageChanged)
    Q_PROPERTY(QVariantList languages READ languages NOTIFY languageChanged)
    Q_PROPERTY(bool rightToLeft READ rightToLeft NOTIFY languageChanged)
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

    Q_INVOKABLE QString endonymOf(const QString &code) const
    { return endonym(code); }
    Q_INVOKABLE QString englishNameOf(const QString &code) const
    { return englishName(code); }

    void applyStoredLanguage();

    static QStringList supportedCodes();
    static bool isSupported(const QString &code);
    static bool isRightToLeft(const QString &code);
    static QString endonym(const QString &code);
    static QString englishName(const QString &code);

    // Maps one locale tag ("es_MX", "pt-BR", "zh-Hans-CN") to a supported code,
    // or "". Regions collapse to their language; script matters only for
    // Chinese.
    static QString matchLanguageTag(const QString &tag);
    static QString matchPreferenceList(const QStringList &uiLanguages);
    static QString systemLanguage();
    static QString systemLanguage(const QLocale &locale);
    static QLocale formattingLocale(const QString &uiCode, const QLocale &system,
                                    const QLocale &timeCategory);

Q_SIGNALS:
    void languageChanged();
    void retranslateRequested();

private:
    void applyLanguage(const QString &policy);
    bool installCatalog(const QString &code);
    void removeTranslators();

    SettingsManager *m_settings = nullptr;
    std::unique_ptr<QTranslator> m_appTranslator;
    QString m_effective = QStringLiteral("en");
};
