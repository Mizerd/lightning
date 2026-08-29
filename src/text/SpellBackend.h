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
//  * Linux/BSD -> enchant-2, loaded with `dlopen` at RUNTIME. Enchant is the
//    same broker KDE, GNOME, LibreOffice and Firefox use, so it resolves the
//    distribution's hunspell/aspell/nuspell dictionaries and the user's own
//    `~/.config/enchant` personal word list. dlopen rather than a link so the
//    build gains no dependency and a machine without enchant reports an
//    honest "unavailable" instead of failing to start.
//  * Everything else (macOS today) -> no backend. macOS's own checker is
//    `NSSpellChecker`, which needs an Objective-C++ translation unit and a
//    macOS host to compile and test on; claiming it without either would be
//    exactly the "assembled at package time, never asked whether it works"
//    trap this repository has paid for twice.
//
// A backend is a value-free lookup: it is handed a WORD and answers. All
// policy — what counts as a word, what is skipped, what is cached, what the
// user chose to ignore — is SpellChecker's, so a second platform cannot
// quietly acquire a different idea of what a word is.
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

    // The dictionary actually resolved ("en_US"), for honest reporting.
    virtual QString language() const = 0;
    // A short backend name for --spell-status ("windows", "enchant").
    virtual QString name() const = 0;
};

// The backend for THIS platform, or nullptr when the platform has none and
// when the platform has one but this machine cannot provide a dictionary.
// `preferredLanguage` is a BCP-47/POSIX-ish tag ("en_US", "lt_LT"); empty
// means "ask the system locale".
std::unique_ptr<SpellBackend> createPlatformSpellBackend(
    const QString &preferredLanguage);
