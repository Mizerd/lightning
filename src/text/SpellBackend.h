#pragma once

#include <QString>
#include <QStringList>

#include <memory>

class SpellBackend
{
public:
    virtual ~SpellBackend() = default;

    virtual bool isCorrect(const QString &word) const = 0;
    virtual QStringList suggest(const QString &word) const = 0;
    virtual QString language() const = 0;
    virtual QStringList availableLanguages() const = 0;
    virtual QString name() const = 0;
    virtual QString autoCorrection(const QString &word) const
    {
        Q_UNUSED(word);
        return {};
    }
    // Whether autoCorrection() is the platform's own answer, so SpellChecker
    // must not second-guess it with a heuristic.
    virtual bool hasNativeAutoCorrection() const { return false; }
};

using SpellBackendFailure = QString;

std::unique_ptr<SpellBackend> createPlatformSpellBackend(
    const QString &preferredLanguage, SpellBackendFailure *failure,
    QStringList *availableLanguages);

inline std::unique_ptr<SpellBackend> createPlatformSpellBackend(
    const QString &preferredLanguage, SpellBackendFailure *failure)
{
    return createPlatformSpellBackend(preferredLanguage, failure, nullptr);
}

inline std::unique_ptr<SpellBackend> createPlatformSpellBackend(
    const QString &preferredLanguage)
{
    return createPlatformSpellBackend(preferredLanguage, nullptr, nullptr);
}

QString spellTagToBcp47(const QString &tag);
QString spellTagToPosix(const QString &tag);

bool platformAutocorrectDefault();

bool platformSpellEngineCompiledIn();

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
QStringList spellDictionaryDirectories();
QStringList bundledSpellDictionaryDirectories();
std::unique_ptr<SpellBackend> createHunspellSpellBackend(
    const QString &preferredLanguage, const QStringList &dictionaryDirectories,
    SpellBackendFailure *failure, QStringList *availableLanguages);
#endif
