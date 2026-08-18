# Second 2026-08-18 tester report (0.7.3, Win11) — disposition

Reporter: RomanticAnimeGerl (via Rokas). Screenshots in the report showed
(a) the tone-popup/message-menu double-open and (b) Element rendering the
"Land of the Insane" space with nested subspaces and letter avatars.

## Fixed this round

| Item | Root cause | Fix |
|---|---|---|
| Two menus on skin-tone right-click | The emoji picker was a non-modal Popup floating over the message row; Qt Quick TapHandlers are non-exclusive across subtrees, so the tile's right-click ALSO fired the row's context-menu handler | Picker is modal (`dim: false` keeps the look); contract-tested |
| Copy-paste sends a link | There was no "Copy image" — the only affordances on an image were Copy text / Copy **message link** (visible in the screenshot). The paste path itself already prefers image data over remote URLs | Right-click → **Copy image**: decrypted bytes fetched via the star/save MediaBridge path (pending-key discipline), magic-sniffed (SVG refused), placed on the clipboard as raster + original bytes; transient export, Save-As precedent |
| GIF settings "reset" every launch | The stored values never reset — the three GIF combos were the only settings using a creation-time `indexOfValue()` binding, which evaluates before `valueRole`/`model` settle and returns -1 (masked to 0 by `Math.max`), so every relaunch *displayed* defaults | Explicit `Component.onCompleted` + change-signal sync; regression test runs first-open with non-default values |
| `#` room avatars | `roomGlyph: true` at 7 call sites (already inconsistent: Quick Switcher/Room Info showed initials) | Letter initials everywhere (Element style), identity colors unchanged |
| No invite on Space Home | The invite dialog was only reachable through the room-info panel, which a Space never opens | Invite button on Space Home, honestly gated on the roster's `canInvite`; same dialog, same server-side permission check |
| Subfolder spaces invisible | `SpaceManager` flattens subspace rooms into ancestors and the computed nesting `level` was rendered nowhere | Space Home gains "Spaces in this space" (joined subspaces → drill in; unjoined /hierarchy children labeled `Space · N rooms inside`, Join drills in on success); rail indents nested spaces |
| No reply-to-image thumbnail | Reply targets crossed as three text fields only | The embedded reply event's media registers in the Rust media registry under the reply target's event id (row mechanism, encrypted rooms included); thumbnails in the quote block and the composer banner |
| Read-by not clickable | No handler on the receipt strip | Click → shared reader-list popover: the delivered 16 newest readers + a truthful "…and N more (names not loaded)" — the bridge caps at 16 by design and names beyond it are not fabricated |

## Assessed and deferred (with reasons)

- **Spellcheck (MEDIUM)**: no engine dependency exists. The composer
  already has the exact hook needed (`MentionHighlighter` proves
  `TextArea.textDocument` + `QSyntaxHighlighter` works); the missing part
  is a spell engine (hunspell/nuspell) plus **dictionary packaging across
  the fleet** (Windows installers would need to ship dictionaries).
  Deserves its own round with a packaging plan, not a bolt-on.
- **Rearrangeable spaces rail / folders (LOW)**: deferred; the natural
  mechanism exists (account-scoped ordered `QStringList` in QSettings —
  the drafts-index precedent). Folders are a larger design (Discord-style
  groups) — noted for a future round.
- **Update dev/main channel toggle (LOW)**: the update pipeline has
  exactly one signed manifest slot (`lightning-update/latest`) and the
  channel concept in the manifest is server-declared. A dev channel needs
  lightning-deploy to publish a second signed slot first; client toggle
  is trivial after that. Deferred to release-infra work.
- **Emoji tofu on Win11 (LOW)**: Lightning bundles no emoji font; glyphs
  come from the OS (Segoe UI Emoji on Windows), so emoji newer than the
  installed Windows font update render as boxes. The honest fix is
  bundling a color-emoji font (Noto Color Emoji, ~10+ MB, COLRv1 —
  renderer support required) — a size/packaging decision for Rokas, not
  an incidental change. The catalogue itself is already Unicode 17.0.

## Everything user-visible above is NOT TESTED live on Windows —
offscreen suites cover the mechanics; the reporter's machine is the real
verifier.
