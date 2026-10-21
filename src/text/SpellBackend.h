#pragma once

#include <QString>
#include <QStringList>

#include <memory>

// The platform's own spell checker behind the narrowest interface a composer
// needs. Qt ships no spell checker:
//
//  * Windows: `ISpellChecker` (Windows 8+), using the user's installed
//    proofing languages.
//  * macOS: `NSSpellChecker` (SpellBackendMac.mm).
//  * Linux/BSD: enchant-2 when the machine has it, else Hunspell directly.
//    Both are loaded with dlopen at runtime, so the build gains no
//    dependency. Hunspell finds the system's dictionaries and the ones the
//    AppImage, Snap, Flatpak and Nix packages bundle (see
//    spellDictionaryDirectories()).
//  * Everything else: no backend.
//
// One backend checks ONE language; SpellChecker combines several. A backend
// only answers questions about a single word; all policy (what is a word,
// what is skipped or cached, the personal dictionary, autocorrect) lives in
// SpellChecker. Every backend is local, so nothing about a draft leaves the
// process.
class SpellBackend
{
public:
    virtual ~SpellBackend() = default;

    // True when the platform's dictionary accepts the word as spelled.
    virtual bool isCorrect(const QString &word) const = 0;
    // Replacements, best first. May be empty; never a guess of our own.
    virtual QStringList suggest(const QString &word) const = 0;
    // The resolved dictionary as a BCP-47 tag ("en-US").
    virtual QString language() const = 0;
    // BCP-47 tags in platform order; empty when the platform cannot
    // enumerate.
    virtual QStringList availableLanguages() const = 0;
    // A short backend name for --spell-status ("windows", "macos", "enchant",
    // "hunspell").
    virtual QString name() const = 0;
    // The platform's OWN high-confidence correction for a word it rejects
    // (Windows: a CORRECTIVE_ACTION_REPLACE error; macOS:
    // correctionForWordRange), or "" when it offers none. Engines with no
    // such notion (enchant, Hunspell) return "" and SpellChecker applies its
    // own conservative rule instead.
    virtual QString autoCorrection(const QString &word) const
    {
        Q_UNUSED(word);
        return {};
    }
    // Whether autoCorrection() is the platform's own answer, so SpellChecker
    // must not second-guess it with a heuristic.
    virtual bool hasNativeAutoCorrection() const { return false; }
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

// Windows and the picker use BCP-47 ("en-US"); enchant, Hunspell and macOS use
// POSIX names ("en_US").
QString spellTagToBcp47(const QString &tag);
QString spellTagToPosix(const QString &tag);

// Whether autocorrect is on when the user never chose: the operating
// system's own default. True only on macOS when the system's "Correct
// spelling automatically" is on; Windows (hardware keyboard), Linux and the
// mainstream desktop chat clients do not autocorrect by default.
bool platformAutocorrectDefault();

// Whether this build carries the platform engine at all (Windows: compiled
// against <spellcheck.h>; macOS: AppKit; Linux: the dlopen loaders). A
// compile-time fact, so it is meaningful even where the engine cannot run
// (Windows under Wine).
bool platformSpellEngineCompiledIn();

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
// Directories Hunspell dictionaries are looked up in, in priority order,
// existing absolute ones only: $DICPATH; $XDG_DATA_HOME/hunspell; each
// $XDG_DATA_DIRS entry, then /usr/local/share and /usr/share, as
// {hunspell, myspell, myspell/dicts}; the NixOS profiles; and LAST the
// package's own bundle (bundledSpellDictionaryDirectories()), so a host's
// newer word list wins and the bundle fills only what the host lacks.
QStringList spellDictionaryDirectories();
// Only the directories that ship INSIDE this package (AppImage, Snap,
// Flatpak, Nix), for `--spell-selftest`.
QStringList bundledSpellDictionaryDirectories();
// The Hunspell backend alone, for `LIGHTNING_SPELL_ENGINE=hunspell` and the
// tests. `dictionaryDirectories` empty = spellDictionaryDirectories().
std::unique_ptr<SpellBackend> createHunspellSpellBackend(
    const QString &preferredLanguage, const QStringList &dictionaryDirectories,
    SpellBackendFailure *failure, QStringList *availableLanguages);
#endif
