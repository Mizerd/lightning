#pragma once

#include <QString>
#include <QStringList>

#include <memory>

// ONE PLATFORM SPELL CHECKER, behind the narrowest interface a composer needs.
//
// Why this exists at all: Qt ships NO spell checker. Measured over all 1626
// headers of qtbase 6.11.1, the only spelling-related symbols in the whole of
// Qt are `QTextCharFormat::SpellCheckUnderline` (a DRAWING style),
// `QPlatformTheme::SpellCheckUnderlineStyle` (which underline that style
// resolves to), `QIcon::ToolsCheckSpelling` (an icon name) and the Wayland
// text-input protocol's `content_hint_spellcheck` (a hint handed to an
// on-screen keyboard). Not one of them checks a word. So no Qt application
// gets a squiggle for free and the engine has to be supplied.
//
// The engine supplied here is deliberately THE PLATFORM'S OWN, never a
// bundled dictionary:
//
//  * Windows -> `ISpellChecker` (Windows 8+). It uses the dictionaries the
//    user's own Windows language settings installed, and "Add to dictionary"
//    writes into the user's own Windows custom dictionary, which every other
//    Windows application then honours. Costs the installers NOTHING: no
//    dictionary is packaged, because the operating system already has them.
//  * macOS -> `NSSpellChecker` (SpellBackendMac.mm, an Objective-C++
//    translation unit compiled only on Apple). The system dictionaries, the
//    user's learned words and the user's preferred spelling languages are
//    all AppKit's; "Add to dictionary" is `learnWord:`.
//  * Linux/BSD -> enchant-2, loaded with `dlopen` at RUNTIME. Enchant is the
//    same broker KDE, GNOME, LibreOffice and Firefox use, so it resolves the
//    distribution's hunspell/aspell/nuspell dictionaries and the user's own
//    `~/.config/enchant` personal word list. dlopen rather than a link so the
//    build gains no dependency and a machine without enchant reports an
//    honest "unavailable" instead of failing to start.
//  * Everything else -> no backend.
//
// A backend is a value-free lookup: it is handed a WORD and answers. All
// policy — what counts as a word, what is skipped, what is cached, what the
// user chose to ignore — is SpellChecker's, so a second platform cannot
// quietly acquire a different idea of what a word is.
//
// PRIVACY: every backend here is local (a COM object, an AppKit singleton, a
// shared library). Nothing about a draft leaves the process.
class SpellBackend
{
public:
    virtual ~SpellBackend() = default;

    // True when the platform's dictionary accepts the word as spelled.
    virtual bool isCorrect(const QString &word) const = 0;
    // Replacements, best first. May be empty; never a guess of our own.
    virtual QStringList suggest(const QString &word) const = 0;
    // Adds the word to the USER'S OWN platform dictionary, which is where a
    // user expects it to end up: the same list their other applications read.
    virtual void addToPersonalDictionary(const QString &word) = 0;
    // The dictionary actually resolved, as a BCP-47 tag ("en-US"), for
    // honest reporting and for the language picker's current value.
    virtual QString language() const = 0;
    // Every language this platform can check right now, as BCP-47 tags, in
    // the platform's own order. Drives the language picker; empty when the
    // platform cannot enumerate.
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

// The backend for THIS platform, or nullptr when the platform has none or
// when the platform has one but this machine cannot provide a dictionary.
// `preferredLanguage` is a BCP-47 tag ("en-US", "lt-LT"); empty means "the
// system's own preference". `failure`, when given, says why nullptr came
// back, and `availableLanguages` still receives what the platform CAN check
// on a "no-dictionary" failure — the language picker must keep offering
// those, or a stored preference the machine no longer has would leave the
// user with no way to pick another.
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

// Tag helpers shared by the backends and the policy layer. Windows and the
// picker speak BCP-47 ("en-US"); enchant and macOS dictionaries are named the
// POSIX way ("en_US"). One direction each, no guessing.
QString spellTagToBcp47(const QString &tag);
QString spellTagToPosix(const QString &tag);
