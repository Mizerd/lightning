#pragma once

#include <QString>
#include <QStringList>

#include <memory>

// The platform's own spell checker behind the narrowest interface a composer
// needs. Qt ships no spell checker, and no dictionary is bundled:
//
//  * Windows: `ISpellChecker` (Windows 8+), using the user's installed
//    dictionaries and custom word list.
//  * macOS: `NSSpellChecker` (SpellBackendMac.mm).
//  * Linux/BSD: enchant-2, loaded with dlopen at runtime so the build gains no
//    dependency and a machine without it reports "unavailable".
//  * Everything else: no backend.
//
// A backend only answers questions about a single word; all policy (what is a
// word, what is skipped or cached) lives in SpellChecker. Every backend is
// local, so nothing about a draft leaves the process.
class SpellBackend
{
public:
    virtual ~SpellBackend() = default;

    // True when the platform's dictionary accepts the word as spelled.
    virtual bool isCorrect(const QString &word) const = 0;
    // Replacements, best first. May be empty; never a guess of our own.
    virtual QStringList suggest(const QString &word) const = 0;
    // Adds the word to the user's platform dictionary, shared with other apps.
    virtual void addToPersonalDictionary(const QString &word) = 0;
    // The resolved dictionary as a BCP-47 tag ("en-US").
    virtual QString language() const = 0;
    // BCP-47 tags in platform order; empty when the platform cannot
    // enumerate.
    virtual QStringList availableLanguages() const = 0;
    // A short backend name for --spell-status ("windows", "macos", "enchant").
    virtual QString name() const = 0;
};

// Why createPlatformSpellBackend() came back empty, for the Settings copy:
//   ""              a backend was created;
//   "no-platform"   this build has no backend for this operating system;
//   "no-library"    the platform's engine could not be loaded (Linux: no
//                   libenchant-2 on the machine; Windows: the spell-check
//                   service is unavailable);
//   "no-dictionary" the engine is there but has no dictionary for the
//                   requested (or any candidate) language.
using SpellBackendFailure = QString;

// The backend for this platform, or nullptr with `failure` set. An empty
// `preferredLanguage` means the system's preference. `availableLanguages` is
// filled even on "no-dictionary", so the picker can still offer a language
// the machine does have.
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

// Windows and the picker use BCP-47 ("en-US"); enchant and macOS use POSIX
// names ("en_US").
QString spellTagToBcp47(const QString &tag);
QString spellTagToPosix(const QString &tag);
