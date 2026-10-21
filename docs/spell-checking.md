# Spell checking

Lightning checks spelling in every message-entry surface — the room composer
(new messages, replies, edits), the thread composer, in Markdown mode and in
Rich Text mode — using the operating system's own spell checker (Windows,
macOS) or Hunspell with system and bundled dictionaries (Linux). Several
languages can be checked at once, an optional autocorrect fixes clear typos
as you type, and the personal dictionary is Lightning's own. Nothing is
downloaded at runtime, and no draft text ever leaves the machine.

## Architecture

| Layer | File | Owns |
|---|---|---|
| Backend interface | `src/text/SpellBackend.h` | one engine, ONE language: `isCorrect`, `suggest`, `language`, `availableLanguages`, and the platform's own `autoCorrection` where it has one |
| Windows | `src/text/SpellBackend.cpp` (`Q_OS_WIN`) | `ISpellCheckerFactory` / `ISpellChecker` from `spellcheck.h`; BCP-47 tags; `get_SupportedLanguages`; autocorrect = an error whose corrective action is `CORRECTIVE_ACTION_REPLACE` (Windows' own list) |
| macOS | `src/text/SpellBackendMac.mm` (Objective-C++, compiled only on Apple) | `NSSpellChecker`: `availableLanguages`, `userPreferredLanguages`, `checkSpellingOfString:…`, `guessesForWordRange:…`, `correctionForWordRange:…`; the autocorrect default follows `isAutomaticSpellingCorrectionEnabled` |
| Linux / BSD | `src/text/SpellBackend.cpp` (`Q_OS_UNIX`) | enchant-2 first, then Hunspell directly, both `dlopen`ed: libhunspell from `<app>/../lib` (AppImage, Snap), the build's `LIGHTNING_HUNSPELL_LIBRARY` (Nix), then the soname. Dictionaries from `$DICPATH`, the XDG data dirs, `/usr/share/{hunspell,myspell}`, the NixOS profiles and the package's own bundle (`<app>/../share/hunspell`, `/app/share/hunspell`, `LIGHTNING_SPELL_DICTIONARY_DIRS`). `LIGHTNING_SPELL_ENGINE=enchant\|hunspell` pins one engine |
| Policy | `src/text/SpellChecker.*` | what is a word, what is skipped, the caret rule, the bounded cache, session ignores, the languages, the personal dictionary, autocorrect and the visible-window bound |
| Composer glue | `qml/MessageComposerBar.qml`, `qml/ThreadPanel.qml`, `qml/SpellAutocorrect.qml`, `src/app/RichComposerBridge.*`, `src/models/RichComposition.*` | underline geometry, the context menu, autocorrect and its undo, rich-mode skip ranges and format-preserving replacement |
| Settings | `src/app/SettingsManager.*` (`composer/spellCheck`, `composer/spellLanguage`, `composer/spellExtraLanguages`, `composer/autocorrect`), `qml/SettingsScreen.qml` | on/off, the languages, autocorrect, the personal dictionary list — application settings, never account data |

QML never sees a platform API: it talks to `app.spell` (a `SpellChecker`)
and `app.richComposer`.

### Why the underline is drawn by the composer, not by a QTextCharFormat

`QTextCharFormat::SpellCheckUnderline` does not render in Qt Quick: the scene
graph's text node builds its decorations from the glyph run's boolean
underline flag, so every style other than `SingleUnderline` paints nothing,
and even that would be drawn in the text's own colour. The composers
therefore ask `SpellChecker::misspelledRanges` for `{start, length}` ranges
(document positions, i.e. UTF-16 units — the same units QML's `TextEdit`
uses) and draw one two-pixel rectangle per line-run of each range, as
children of the editor so they scroll with it. Nothing is written into the
document: the decoration cannot enter `formatted_body`, Markdown, the
clipboard, undo or a draft.

### Word rules (`SpellChecker::forEachWord`)

Never asked about: whitespace chunks carrying `://`, `@`, a backtick, `/`,
`\`, `_`, a digit, or a dot between letters (domains, file names); chunks
opening with `#`, `!`, `:` or `~/` (aliases, room ids, shortcodes, paths);
ALL-CAPS runs and words with an interior capital; single letters; everything
inside a fenced ` ``` ` block, an inline code span (spaces included) or a
Markdown link destination `](…)`; the word the caret is in; and any caller
skip range — mention pills in both modes, plus code fragments and code
blocks in rich mode (`RichComposition::spellSkipRanges`). Link anchor text is
checked; the destination is not in the text. `~~struck~~`, `**bold**` and
`*italic*` words are checked. Words are runs of letters and combining
marks with inner apostrophes, so Lithuanian, Cyrillic, accented and
decomposed characters stay whole; surrogate pairs are not letters and cannot
shift a range. The composers pass the visible part of the editor plus one
viewport either side as a window, so only those words reach a dictionary
(up to a 400 000-character draft, at most 300 underlines); without a window
a draft beyond 20 000 characters is not checked.

### Timing

Every keystroke and every scroll restarts a 150 ms timer; the pass runs on the
GUI thread over the visible window with a 4096-word answer cache, so a
dictionary is consulted once per distinct word. Results are synchronous —
there is no asynchronous answer to arrive late for a different draft,
room or account.

### Languages

`Automatic` (the default) is the system language — the Windows user language,
the macOS preferred spelling languages (with AppKit's own identification), or
the Linux locale, with `en_US`/`en` as the last resort — plus the app's UI
language when a dictionary exists for it (a Lithuanian UI on an English system
checks both). An explicit primary language never falls back, and "Also check"
adds more: a word is right when ANY active dictionary accepts it, suggestions
interleave each language's best guesses, and an explicitly chosen language
with no dictionary is named in Settings rather than dropped. The picker lists
what the platform can check right now, each dictionary once (distribution
aliases such as `es_MX -> es_ES` collapse; a regional request still resolves
through its language).

A dictionary that accepts a nonsense probe word (a provider with a dictionary
file but no word data, e.g. hspell for Hebrew on some hosts) is never
activated, whether it is the primary, the interface language or an extra one:
a word is correct when ANY active language accepts it, so one such dictionary
would silence all checking. A refused extra or explicit primary is listed as
missing; a refused automatic primary reports `no-dictionary` when nothing else
is active.

### Personal dictionary and Ignore

`Add to dictionary` writes Lightning's own word list,
`<app-data root>/spelling/personal-dictionary.txt` (`matrix::app_data::primaryRoot()`,
so a portable Windows install keeps it in its own folder; UTF-8, one word per line, per
operating-system user, at most 5000 words), accepted in every language and
every engine, and listed in Settings where each word can be removed. A
lowercase entry also covers its capitalised form. The platform dictionary is
no longer written; words it already learned keep being accepted because the
engine still consults it. `Ignore` is per session and is written nowhere.

### Autocorrect

Off by default everywhere except macOS, where the default follows the
system's "Correct spelling automatically" (Discord, Element desktop, Slack and
Telegram desktop do not autocorrect, and Windows leaves it off for hardware
keyboards). Setting `composer/autocorrect`: 0 = platform default, 1 = on,
2 = off. It acts only when ONE character was typed and it is whitespace or
`, ; ! ? ) "` or a closing curly quote (not `.`, which starts domains and
file names, nor `:`, which starts
shortcodes), on the word ending there, when every active dictionary rejects
it, never inside code, URLs, paths, mentions, shortcodes or a `/command`
draft, never on Ignored, personal or previously reverted words. Sources, in
order: an English typo table (only with an English dictionary active, and
only when the dictionary accepts the replacement), the platform's own
correction (Windows, macOS), else -- only for a 4–24-letter all-lowercase word
-- the ONE swap of two neighbouring letters that the dictionaries accept
(`wrold` -> `world`). Never a Capitalised word (usually a name) and never
another kind of edit: the engine's best one-edit suggestion was measured
turning haha into aha, async into sync, hmmm into hmm, lolz into loll, Lukas
into Lucas and Matrixy into Matrix. The swaps are checked with the dictionary's
spell check, never `suggest()` (18–200 ms per dictionary on the GUI thread),
and the answer, "no correction" included, is cached per word until the
dictionaries change. A lone `i` (and `i'm`, `i've`, `i'll`, `i'd`)
becomes `I…` only when every active language is English — `i` is Lithuanian.
Backspace or Ctrl+Z (⌘Z) as the very next key restores the typed word, keeps
the boundary character, and the word is not corrected again that session. A
paste, a restored draft or an IME commit of several characters is never
corrected.

## Packaging

Every package asks itself with `lightning-matrix --spell-selftest` (yes/no
lines, the bundled-dictionary count and a VERDICT; `assert_spell_selftest` in
`packaging-ci/scripts/lib.sh`). In `bundled` mode EVERY bundled dictionary must
open (`bundled languages usable: N/N`, at least 13) and the ISO-8859-13
Lithuanian one must answer a real word correctly, so a dictionary this Qt
cannot convert cannot hide behind an English pass.

A required entry comes second (CLAUDE.md §16): Windows and macOS start as
`warn`, where even a missing transcript only warns. Promote Windows to
`compiled` in `smoke-windows-wine.sh` once a non-publishing rehearsal pipeline
shows `spell engine compiled in: yes` and a VERDICT line in
`reports/wine-portable-spell-selftest.log`; promote macOS to `system` in
`validate-macos-artifacts.sh` once the macOS job's `reports/spell-selftest.txt`
shows `VERDICT: pass`.

| Package | Engine | Dictionaries | Validator |
|---|---|---|---|
| .deb | enchant (Recommends libenchant-2-2) or Hunspell | Debian's `libenchant-2-2` Depends `hunspell-en-us \| hunspell-dictionary`; more via `hunspell-<lang>` | `system`: must pass |
| .rpm | Fedora: enchant2 (pulls hunspell + hunspell-en-US); openSUSE: Recommends libhunspell + `(hunspell-en-US or myspell-en_US)` | the distribution's | `system`: must pass on every lane |
| AppImage | bundled `usr/lib/libhunspell-1.7.so.0` | bundled `usr/share/hunspell`: en_US en_GB ar bn_BD de_DE es_ES fr hi_IN id_ID lt_LT pt_PT pt_BR ru_RU (Debian 13 packages, copyrights in `usr/share/licenses/hunspell-dictionaries`); host dictionaries too | `bundled`: must pass with the bundle alone |
| Snap | inherits the AppDir | inherits the AppDir (confinement hides the host's) | `bundled` |
| Flatpak | the KDE runtime's libhunspell (it has no enchant) | module `hunspell-dictionaries`: the same 13 languages, but from LibreOffice's dictionaries at the commit tagged libreoffice-25.2.3.1, in `/app/share/hunspell` | `bundled` + licence check |
| Nix package / dev shell | store path compiled in (`LIGHTNING_HUNSPELL_LIBRARY`) / `LD_LIBRARY_PATH` | `nix/spell-dictionaries.nix` (same LibreOffice files), compiled in / `DICPATH`; plus the NixOS profiles | — |
| Windows | `ISpellChecker` (mingw-w64's `spellcheck.h`) | the user's installed proofing languages | `warn` under Wine (no spell service there); promote to `compiled` -- see below |
| macOS | `NSSpellChecker` | the system's | `warn`; promote to `system` -- see below |

The AppImage/Snap word lists (Debian packages) and the Flatpak/Nix ones
(LibreOffice) cover the same languages but are not byte-identical: Debian's
en_US (SCOWL 2020), de_DE (igerman98), fr and ar come from other upstreams
than LibreOffice's, so a few words are judged differently between formats.
Chinese has no Hunspell dictionary (word segmentation, not spelling).
`lightning-matrix --spell-status` prints the engine, the active dictionary,
the languages and the directories searched.

## Privacy

Local only. No cloud spelling or grammar service, no telemetry; drafts,
words, room names and Matrix ids never leave the process. Misspelled words
are message content and are never logged. The personal dictionary file holds only the
words the user explicitly adds with "Add to dictionary", never anything read
from a message; it stays on this computer and is removable word by word.
