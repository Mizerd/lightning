# Localization

Lightning uses Qt's own translation system — `qsTr()` in QML, `tr()` /
`QCoreApplication::translate()` in C++, Qt Linguist `.ts` catalogs, `lrelease`,
and `QTranslator`. There is no bespoke dictionary and there must not be one.

English is the **source language**. Every string in the tree is written in
English at its call site, and the catalogs translate away from it.

## Shipped languages

| Code | Language | Endonym |
|---|---|---|
| `en` | English | English |
| `zh_CN` | Chinese (Simplified) | 中文（简体） |
| `hi` | Hindi | हिन्दी |
| `es` | Spanish | Español |
| `ar` | Arabic | العربية |
| `fr` | French | Français |
| `bn` | Bengali | বাংলা |
| `pt` | Portuguese | Português |
| `ru` | Russian | Русский |
| `id` | Indonesian | Bahasa Indonesia |
| `lt` | Lithuanian | Lietuvių |

The table lives in exactly one place in code — `kLanguages` in
`src/i18n/LocalizationManager.cpp`. `tests/LocalizationTest.cpp` asserts that
it agrees with `LIGHTNING_LANGUAGE_CODES` in `CMakeLists.txt` and with the
files in `i18n/`, in both directions, so a language cannot be offered without
a catalog or compiled without being offered.

### Why English has a catalog

English is the source language, so nearly every entry in `lightning_en.ts` is
identical to its source and `lrelease -removeidentical` throws it away. The
catalog still exists, and it is about 3 KB, because of **plural forms**: a
source string written `%n room(s)` renders that `(s)` **literally** when no
catalog is loaded, so without it the English UI says "1 room(s)". The English
catalog carries the twenty numerus entries and essentially nothing else.

## Selecting a language

`Settings → Appearance → Language`. The stored value is a **policy**, not a
language: `"system"` means "resolve against the desktop every time we start",
and any other value is an explicit choice stored verbatim. Those are different
states — storing the resolved code instead would freeze a user's language the
first time they opened Settings, and moving the machine to another locale
would stop following.

System resolution walks `QLocale::system().uiLanguages()` **in order** (it is
the user's ordered preference list, not one locale) and takes the first
supported match. Regional variants collapse onto their language: `es_ES` and
`es_MX` both reach `es`, `pt_BR` and `pt_PT` both reach `pt`, `fr_CA` reaches
`fr`. Chinese is matched by **script**, not region — `zh_CN`, `zh_SG` and
anything tagged `Hans` reach the Simplified catalog; `zh_TW`, `zh_HK` and
anything tagged `Hant` deliberately do **not**, because Lightning ships no
Traditional catalog and serving Simplified to a Traditional reader while the
picker claims nothing of the sort is worse than the honest English fallback.
Anything else falls back to English.

Switching language takes effect **immediately**: `LocalizationManager` emits
`retranslateRequested()` and `main.cpp` calls `QQmlEngine::retranslate()`,
which re-evaluates every binding that reads `qsTr()`.

### Known limitation

`retranslate()` reaches declarative bindings. It does **not** reach a string
that a C++ model already turned into row data, nor a JavaScript variable
assigned once and never re-read. In practice that means a small number of
already-materialised strings — some model-provided labels, and text captured
into a local at component-creation time — keep the previous language until
their panel is next opened. Everything on the chat shell, the room list, the
composer, menus, dialogs and Settings retranslates in place. This is a real
gap, not a theoretical one; it is bounded and disclosed rather than papered
over with an application restart.

## Right-to-left

`LocalizationManager` sets `QGuiApplication::layoutDirection`, which Qt uses
for text alignment on its own. Anchors and layouts only mirror where
`LayoutMirroring` says so, and `qml/Main.qml` enables it once on the root
window with `childrenInherit: true`, so the whole shell mirrors from one
place.

`LayoutMirroring` resolves anchors and layout order — it never mirrors pixels,
so images, video and avatars are unaffected by construction. The timeline's
`rotation: 180` Flickable is likewise unaffected: its scroll axis is vertical
and mirroring is horizontal.

`qml/CodeBlock.qml` opts out (`LayoutMirroring.enabled: false`). Source code
reads left-to-right in every language.

**Arabic RTL has not been exercised on a real desktop.** The mechanism is in
place and the layout direction is applied; how it actually looks is NOT TESTED.

## Updating the catalogs

After adding or changing user-visible strings:

```sh
nix develop -c cmake --build build --target update-translations
```

That runs `lupdate` over `src/` and `qml/`, merging new strings into every
`i18n/*.ts` file **without discarding existing translations**. It is a manual
target and is deliberately not part of `all`: `lupdate` rewrites the tracked
`.ts` files, and doing that as a side effect of an ordinary build would edit
the working tree under whoever is building.

Translate the new entries (Qt Linguist, or by editing the XML), then build
normally — `lrelease` runs as part of the build and produces
`<build>/i18n/lightning_<code>.qm`, which is embedded into the binary at
`:/i18n/`.

`lrelease` is invoked with `-removeidentical -nounfinished`. An entry still
marked `type="unfinished"` is **left out of the compiled catalog**, so a
half-translated string falls back to English rather than shipping a draft.

### Adding a language

1. A row in `kLanguages` (`src/i18n/LocalizationManager.cpp`).
2. The code in `LIGHTNING_LANGUAGE_CODES` (`CMakeLists.txt`).
3. `nix develop -c lupdate -locations relative -no-obsolete src qml -ts i18n/lightning_<code>.ts`
4. Translate it.

`tests/LocalizationTest.cpp` fails if any of the first three are missed or if
the catalogs no longer match the strings currently extracted from the source.

**Every round that adds a `qsTr()` owes a catalog refresh.** That gate
(`catalogsMatchTheCurrentSource`) runs `lupdate` over `src` and `qml` at test
time and compares the result against all eleven catalogs in both directions,
so a new user-visible string breaks CTest until the catalogs carry it:

```sh
for c in en zh_CN hi es ar fr bn pt ru id lt; do
  nix develop -c lupdate -silent -locations relative -no-obsolete \
      src qml -ts i18n/lightning_$c.ts
done
```

The new entries land untranslated and fall back to English, which is the
honest state until somebody translates them. Check the plural forms after any
refresh (`grep -c '<numerusform>' i18n/lightning_ar.ts` and friends): an
earlier round lost them to a careless run, and the arity per language is
6 for `ar`, 3 for `ru`/`lt`, 2 for most, 1 for `zh_CN`/`id`.

## Rules for writing translatable strings

**Never build a sentence with `+`.** `qsTr("User ") + name + qsTr(" joined")`
cannot be translated into a language that orders those clauses differently.
Use a placeholder: `qsTr("%1 joined the room").arg(name)`. When a line is
assembled conditionally, each augmentation gets its own template rather than
a fragment glued on — see `ThreadSummaryCard.qml`'s accessible name.

**Use Qt's plurals, never a `count === 1` ternary.** `qsTr("%n room(s)", "", n)`.
Different languages have two, three or six plural forms; a ternary has one.

**Disambiguate where one English word carries two meanings.** The second
argument to `qsTr()` is a translator-visible comment:
`qsTr("Search", "verb: start searching")`. Words in this codebase that need it
include Search, Call, Room, Space, Leave, Open, Close and File.

**Do not translate protocol or content.** Matrix IDs, room aliases, server
names, event IDs, URLs, filenames, display names, room names, topics and
message bodies are data, not UI. Lightning's *description* of an event is UI
and is translated — `"%1 joined the room"` — with the name left as a
placeholder.

Product, protocol, library and file-format names also stay exact: `Lightning`,
`Matrix`, `MatrixRTC`, `Element`, `Rust`, `Qt`, `PNG`, `JPEG`, and similar
identifiers are not ordinary English words in this context. Translating or
transliterating them makes the UI name a different product or a file type that
does not exist. `LocalizationTest` checks the full protected-name list in every
finished translation.

**Do not translate logs or diagnostics.** They are read by developers, and a
translated log line is a log line nobody can grep for.

## Packaging

`.qm` files are compiled at build time and embedded into the executable's
resources, so a packaged application carries them with no data directory and
no install rule.

**`lrelease` lives in a different distro package from the Qt headers.** On
Debian/Ubuntu it is `qt6-tools-dev-tools`, not `qt6-base-dev`. CMake therefore
looks for `Qt6::LinguistTools` with `QUIET` and, when it is absent, configures
successfully and prints a warning: the build works and the application is
English-only, `LocalizationManager::translationsAvailable()` returns false, and
the Settings page says so instead of offering languages that all look identical.

> **Packaging action required.** The lightning-deploy CI images have not been
> checked for the Qt Linguist tools. Until `qt6-tools-dev-tools` (or each
> platform's equivalent) is present in the deb/rpm/AppImage/Windows/macOS build
> environments, **packaged builds will be English-only** — silently, apart from
> the configure-time warning. The configure log line to look for is
> `Localization: enabled (...)`.
