#include "i18n/LocalizationManager.h"

#include "app/SettingsManager.h"
#include "i18n/TimeLocale.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJSEngine>
#include <QLocale>
#include <QTranslator>
#include <QVariantMap>
#include <QtGlobal>

#include <iterator>

namespace {

constexpr auto kSystemPolicy = "system";

struct Language {
    const char *code;
    const char *english;
    const char *endonym;
    bool rtl;
};

constexpr Language kLanguages[] = {
    { "en",    "English",                    "English",          false },
    { "zh_CN", "Chinese (Simplified)",       "中文（简体）", false },
    { "hi",    "Hindi",                      "हिन्दी", false },
    { "es",    "Spanish",                    "Español",     false },
    { "ar",    "Arabic",                     "العربية", true },
    { "fr",    "French",                     "Français",    false },
    { "bn",    "Bengali",                    "বাংলা",       false },
    { "pt",    "Portuguese",                 "Português",   false },
    { "ru",    "Russian",                    "Русский", false },
    { "id",    "Indonesian",                 "Bahasa Indonesia", false },
    { "lt",    "Lithuanian",                 "Lietuvių",        false },
    { "de",    "German",                     "Deutsch",         false },
};

const Language *lookup(const QString &code)
{
    for (const Language &l : kLanguages) {
        if (code == QLatin1String(l.code))
            return &l;
    }
    return nullptr;
}

QString canonical(const QString &tag)
{
    return QString(tag).replace(QLatin1Char('-'), QLatin1Char('_'));
}

}

AppLocale *AppLocale::instance()
{
    // Never destroyed: QML engines and backends may outlive any owner we could
    // pick, and it holds no resources.
    static AppLocale *const self = new AppLocale;
    return self;
}

AppLocale *AppLocale::create(QQmlEngine *, QJSEngine *)
{
    AppLocale *self = instance();
    QJSEngine::setObjectOwnership(self, QJSEngine::CppOwnership);
    return self;
}

LocalizationManager::LocalizationManager(SettingsManager *settings,
                                         QObject *parent)
    : QObject(parent)
    , m_settings(settings)
{
}

LocalizationManager::~LocalizationManager() = default;

QStringList LocalizationManager::supportedCodes()
{
    QStringList out;
    out.reserve(int(std::size(kLanguages)));
    for (const Language &l : kLanguages)
        out << QLatin1String(l.code);
    return out;
}

bool LocalizationManager::isSupported(const QString &code)
{
    return lookup(code) != nullptr;
}

bool LocalizationManager::isRightToLeft(const QString &code)
{
    const Language *l = lookup(code);
    return l && l->rtl;
}

QString LocalizationManager::endonym(const QString &code)
{
    const Language *l = lookup(code);
    return l ? QString::fromUtf8(l->endonym) : QString();
}

QString LocalizationManager::englishName(const QString &code)
{
    const Language *l = lookup(code);
    return l ? QString::fromUtf8(l->english) : QString();
}

QString LocalizationManager::matchLanguageTag(const QString &tag)
{
    const QString t = canonical(tag.trimmed());
    if (t.isEmpty())
        return {};

    if (isSupported(t))
        return t;

    const QStringList parts = t.split(QLatin1Char('_'), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return {};
    const QString lang = parts.first().toLower();

    if (lang == QLatin1String("zh")) {
        for (int i = 1; i < parts.size(); ++i) {
            const QString p = parts.at(i);
            if (p.compare(QLatin1String("Hant"), Qt::CaseInsensitive) == 0
                || p.compare(QLatin1String("TW"), Qt::CaseInsensitive) == 0
                || p.compare(QLatin1String("HK"), Qt::CaseInsensitive) == 0
                || p.compare(QLatin1String("MO"), Qt::CaseInsensitive) == 0) {
                return {};
            }
        }
        return QStringLiteral("zh_CN");
    }

    if (isSupported(lang))
        return lang;
    return {};
}

QString LocalizationManager::matchPreferenceList(const QStringList &uiLanguages)
{
    for (const QString &tag : uiLanguages) {
        const QString match = matchLanguageTag(tag);
        if (!match.isEmpty())
            return match;
    }
    return QStringLiteral("en");
}

QString LocalizationManager::systemLanguage()
{
    return matchPreferenceList(QLocale::system().uiLanguages());
}

QString LocalizationManager::systemLanguage(const QLocale &locale)
{
    return matchPreferenceList(locale.uiLanguages());
}

QLocale LocalizationManager::formattingLocale(const QString &uiCode,
                                              const QLocale &system,
                                              const QLocale &timeCategory)
{
    const QLocale ui(uiCode);
    if (timeCategory.language() == ui.language())
        return timeCategory;
    if (ui.language() != system.language())
        return ui;
    const QLocale regional(ui.language(), ui.script(), system.territory());
    return regional.language() == ui.language() ? regional : ui;
}

bool LocalizationManager::translationsAvailable()
{
#ifdef LIGHTNING_HAS_TRANSLATIONS
    return true;
#else
    return false;
#endif
}

QString LocalizationManager::language() const
{
    if (!m_settings)
        return QString::fromLatin1(kSystemPolicy);
    const QString stored = m_settings->language();
    if (stored == QLatin1String(kSystemPolicy) || isSupported(stored))
        return stored;
    return QString::fromLatin1(kSystemPolicy);
}

QVariantList LocalizationManager::languages() const
{
    QVariantList out;
    QVariantMap system;
    system.insert(QStringLiteral("code"), QString::fromLatin1(kSystemPolicy));
    system.insert(QStringLiteral("name"), tr("System default"));
    system.insert(QStringLiteral("endonym"), tr("System default"));
    system.insert(QStringLiteral("rightToLeft"), false);
    out.append(system);

    for (const Language &l : kLanguages) {
        QVariantMap entry;
        entry.insert(QStringLiteral("code"), QLatin1String(l.code));
        entry.insert(QStringLiteral("name"), QString::fromUtf8(l.english));
        entry.insert(QStringLiteral("endonym"), QString::fromUtf8(l.endonym));
        entry.insert(QStringLiteral("rightToLeft"), l.rtl);
        out.append(entry);
    }
    return out;
}

bool LocalizationManager::rightToLeft() const
{
    return isRightToLeft(m_effective);
}

void LocalizationManager::setLanguage(const QString &policy)
{
    const QString wanted = (policy == QLatin1String(kSystemPolicy)
                            || isSupported(policy))
                               ? policy
                               : QString::fromLatin1(kSystemPolicy);
    if (wanted == language())
        return;
    if (m_settings)
        m_settings->setLanguage(wanted);
    applyLanguage(wanted);
}

void LocalizationManager::applyStoredLanguage()
{
    applyLanguage(language());
}

void LocalizationManager::applyLanguage(const QString &policy)
{
    const QString code = policy == QLatin1String(kSystemPolicy)
                             ? systemLanguage()
                             : policy;

    removeTranslators();
    m_effective = code;
    if (!installCatalog(code)) {
        m_effective = QStringLiteral("en");
        installCatalog(m_effective);
    }

    QLocale::setDefault(formattingLocale(m_effective, QLocale::system(),
                                         i18n::timeCategoryLocale()));

    if (auto *gui = qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        gui->setLayoutDirection(isRightToLeft(m_effective) ? Qt::RightToLeft
                                                           : Qt::LeftToRight);
    }

    Q_EMIT languageChanged();
    Q_EMIT retranslateRequested();
    AppLocale::instance()->notifyChanged();
}

bool LocalizationManager::installCatalog(const QString &code)
{
    if (code.isEmpty())
        return true;

    const bool isSource = code == QLatin1String("en");

    auto translator = std::make_unique<QTranslator>();
    const QString file = QStringLiteral("lightning_%1").arg(code);
    if (!translator->load(file, QStringLiteral(":/i18n"))) {
        if (isSource)
            return true;
        qWarning("LocalizationManager: no catalog for '%s' — falling back to "
                 "English", qPrintable(code));
        return false;
    }
    if (!QCoreApplication::installTranslator(translator.get()))
        return false;
    m_appTranslator = std::move(translator);
    return true;
}

void LocalizationManager::removeTranslators()
{
    if (!m_appTranslator)
        return;
    QCoreApplication::removeTranslator(m_appTranslator.get());
    m_appTranslator.reset();
}
