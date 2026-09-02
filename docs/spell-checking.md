# Spell checking

Lightning checks spelling in every message-entry surface — the room composer
(new messages, replies, edits), the thread composer, in Markdown mode and in
Rich Text mode — using the operating system's own spell checker. Nothing is
bundled, nothing is downloaded, and no draft text ever leaves the machine.

## Architecture

| Layer | File | Owns |
|---|---|---|
| Backend interface | `src/text/SpellBackend.h` | one word in, one answer out: `isCorrect`, `suggest`, `addToPersonalDictionary`, `language`, `availableLanguages` |
| Windows | `src/text/SpellBackend.cpp` (`Q_OS_WIN`) | `ISpellCheckerFactory` / `ISpellChecker` from `spellcheck.h`; BCP-47 tags; `get_SupportedLanguages`; `Add` writes the user's own Windows custom dictionary |
| macOS | `src/text/SpellBackendMac.mm` (Objective-C++, compiled only on Apple) | `NSSpellChecker`: `availableLanguages`, `userPreferredLanguages`, `checkSpellingOfString:…`, `guessesForWordRange:…`, `learnWord:` |
| Linux / BSD | `src/text/SpellBackend.cpp` (`Q_OS_UNIX`) | enchant-2, resolved with `dlopen` at runtime: `enchant_broker_*`, `enchant_dict_*`, `enchant_broker_list_dicts`; the user's `~/.config/enchant` word list |
| Policy | `src/text/SpellChecker.*` | what is a word, what is skipped, the caret rule, the bounded cache, session ignores, the language preference and the picker rows |
| Composer glue | `qml/MessageComposerBar.qml`, `qml/ThreadPanel.qml`, `src/app/RichComposerBridge.*`, `src/models/RichComposition.*` | underline geometry, the context menu, rich-mode skip ranges and format-preserving replacement |
| Settings | `src/app/SettingsManager.*` (`composer/spellCheck`, `composer/spellLanguage`), `qml/SettingsScreen.qml` | on/off and the language preference — application settings, never account data |

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
shift a range. A draft beyond 20 000 characters is not checked.

### Timing

Every keystroke restarts a 150 ms timer; the pass runs on the GUI thread
over the whole (bounded) draft with a 4096-word answer cache, so a
dictionary is consulted once per distinct word. Results are synchronous —
there is no asynchronous answer to arrive late for a different draft,
room or account.

### Language

`Automatic` (the default) asks the system: the Windows user language, the
macOS preferred spelling languages (with AppKit's own identification when
no explicit language is set), or the Linux locale, with `en_US`/`en` as the
last resort only when the system language has no dictionary. An explicit
choice never falls back. The picker lists what the platform can check right
now (`availableLanguages`), labelled in English through `QLocale`
("Lithuanian (Lithuania)"). The UI language and the spelling language are
independent settings.

`Add to dictionary` writes the platform's user dictionary (Windows custom
dictionary, macOS learned words, enchant's personal word list), so other
applications see it too. `Ignore` is per session and is written nowhere.

## Packaging

* **Windows**: `spellcheck.h` is part of the Windows SDK mingw-w64 ships;
  nothing is staged. A missing proofing language is reported as
  "no dictionary" with a pointer to Windows Settings. Running the portable
  build installs nothing and registers nothing.
* **macOS**: AppKit is linked (`-framework AppKit`); nothing else.
* **Linux**: the deb and rpm recommend the enchant-2 broker; dictionaries are
  the distribution's (`hunspell-*`). The AppImage does not bundle enchant and
  `dlopen`s the host's `libenchant-2.so.2`, so a host without enchant gets
  an honest "unavailable". Flatpak and Snap see only the dictionaries their
  runtime carries; host dictionaries are not visible inside confinement —
  reported as a limitation, not hidden. `lightning-matrix --spell-status`
  prints which backend and dictionary a build resolved on a machine.

## Privacy

Local only. No cloud spelling or grammar service, no telemetry; drafts,
words, room names and Matrix ids never leave the process. Misspelled words
are message content and are never logged.
