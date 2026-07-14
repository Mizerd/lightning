# Current state (v0.5.17)

## v0.5.18 development — automatic initial history

Room entry now distinguishes a requested Rust timeline from a pagination-ready
timeline whose initial SDK snapshot and generation have been adopted. Initial
viewport fill remains pending across that snapshot reset and starts
automatically once readiness is signalled, so the normal opening race no
longer becomes a user-facing Retry failure. Transient initial failures use a
generation-scoped, cancellable three-attempt exponential backoff; terminal or
exhausted failures still expose manual Retry. Completion is accepted only
after the backend has reported the batch loading, preventing an accepted Rust
dispatch from being mistaken for an immediate empty completion.

## v0.5.18 development — room activity visibility

General settings now include **Show room activity**, enabled by default and
persisted globally. Disabling it collapses routine membership, profile, and
room-state annotations at presentation time only; the SDK timeline and C++
model retain the events, and re-enabling the setting restores them without a
resync. Ordinary messages, media, send failures, and undecryptable-event
warnings are not classified as room activity and remain visible.

## v0.5.18 development — unread navigation

The matrix-sdk-ui read-marker virtual item now renders as a stable **New
messages** divider between the fully-read event and the first unread event; it
is absent when the SDK places the fully-read marker at the end. A floating
**Jump to latest** control appears only while the reader is meaningfully away
from the bottom. New events preserve that historical reading position, while
the explicit control resumes bottom-following and the existing read-receipt
policy.

## v0.5.18 development — stable timeline navigation

Reply previews now navigate by the Matrix target event ID. Targets already in
the live SDK timeline are centered immediately; older targets use the existing
single-flight pagination controller for at most eight backward batches, and a
room change cancels the search. Located replies receive a short highlight and
unavailable targets report a compact safe fallback without fetching arbitrary
event data through QML.

Each room also keeps a bounded, in-memory presentation anchor consisting only
of its room ID, visible event ID, pixel offset, and whether it followed the
latest event. Returning to a room restores that stable anchor (or latest), and
logout clears all anchors. No message body or decrypted content is persisted.

## v0.5.17 — timeline runtime hotfix

Live investigation of the affected encrypted room classified the reported
window disappearance as a live-process UI hang in the Qt Quick timeline,
not a Rust panic or a cryptographic error. The pagination presentation header
changed `ListView.contentHeight`, whose synchronous geometry handler dispatched
another viewport-fill request while the header state binding was evaluating.
Viewport-fill geometry checks are now queued and coalesced, and the header
reads the controller's semantic presentation state directly. Loading, Retry,
reached-start, automatic fill, and near-top pagination remain controller-owned.

The remaining affected-room hang was a QML delegate-incubation failure exposed
by a long decrypted message, not an encryption failure. Before the 0.5.16
manual content-column layout received its real ListView width, wrapped text was
measured against a one-pixel clamp. A multi-thousand-character body therefore
reported a transient height of tens of thousands of pixels; ListView repeatedly
discarded and recreated the same visible delegates and starved the GUI event
loop. Delegates and wrapped bodies now use bounded startup widths until the
real responsive width is available. The regression fixture also covers safe
undecryptable, missing-profile, missing-reply, and encrypted-media-pending
states plus resize and room-switch transitions.

## v0.5.16 — timeline presentation polish

### Compact sender rows (checkpoint 1)

Sender-group leaders retain the fixed left avatar gutter and compact
name/timestamp header, while continuation rows no longer reserve an empty
avatar-height block or a permanent timestamp line. Continuation timestamps
appear subtly in the gutter on hover, group leaders provide the inter-group
spacing, and the timeline no longer inserts the same global gap between every
event. The action toolbar floats at the row's right edge rather than consuming
message-column layout width. Selectable text, rich links, replies, reactions,
send state, retry, and message actions remain on the same event delegate.

### Message-column previews and media (checkpoint 2)

Link previews, Matrix image thumbnails, and file attachments now take bounded
left-aligned widths from the message content column instead of stretching
their loaders across the timeline row. Preview cards use a compact elevated
surface, a small accent edge, clipped thumbnail framing, and tighter internal
insets. Matrix image sizing is capped at 360×320 logical pixels while
preserving intrinsic aspect ratio and avoiding upscaling; file attachments use
a compact bordered 320-pixel maximum. Original link/media activation and all
controlled-source security boundaries are unchanged.

### Direct GIFs as inline media (checkpoint 3)

The controller already distinguished validated direct raster responses from
HTML metadata, but QML still routed both through the same generic embed card.
Loaded direct images and GIFs now select a dedicated inline-media component:
it uses intrinsic aspect ratio within a 360×300 bound, clips to the message
column, shows the existing GIF badge, and uses the controlled static or
animated source according to settings. It has no article accent edge, title,
description, or host footer. Activation still passes only the original
validated HTTP/HTTPS URL to `MediaManager`; the controlled local source is
render-only and never becomes an external target.

## v0.5.15 — expanded room activity

Room-activity expansion now renders the actual SDK-derived child events.
The previous nested `Repeater` accidentally resolved `model` to its own
model property instead of the outer timeline delegate, so the C++ group had
entries while QML always repeated an empty value. The shared
`RoomActivityDelegate.qml` receives typed child maps directly from
`TimelineModel`, including stable event identity, event kind, actor, affected
member where applicable, safe description, and timestamp. It remains a
compact annotation with keyboard-accessible Expand/Collapse behavior and no
message actions or preview requests.

Expansion keys now include both room ID and stable group identity. Group-role
notifications refresh existing leaders after incremental appends, prepends,
changes, and removals, so pagination and live updates cannot leave a stale
count or inaccessible children. A runtime-facing QML test instantiates the
actual activity component and verifies that expansion creates visible,
non-zero-height child rows.

### Secure direct image and GIF previews

Direct passive raster responses are now classified from the final bounded
response and recognized magic bytes, not a URL suffix. A declared image MIME
must agree with the bytes; safely recognized image bytes may recover a generic
or mislabeled CDN response, while real HTML stays on the metadata path and SVG
remains inactive. Direct results carry an explicit `direct_media` kind, so a
GIF cannot be substituted by an unrelated title/description card.

Redirects for both the original preview URL and HTML metadata images are
manually followed through the same DNS resolution, public-IP policy, pinned
connection, timeout, and byte limits on every hop. Validated static bytes are
served to QML through the bounded in-memory image provider; validated GIFs use
the existing short-lived controlled animation file. Neither controlled source
is used for browser activation: clicking always opens the original visible
HTTP/HTTPS link through `MediaManager` validation.

### Timeline sender identity and grouping roles

`TimelineModel` now exposes normalized sender avatar MXC, initials fallback,
stable event identity, and explicit begin/continue/end/show-identity roles for
normal message and media rows. The avatar identifier comes from the Rust SDK's
timeline sender profile (or the account-scoped member cache) and is resolved by
the existing Rust media bridge; no authenticated download URL or local path is
exposed as a model role.

Visual groups use a documented five-minute threshold and break on sender,
date, visible room activity, timeline-start/date rows, room reset, and event
presentation boundaries. Hidden SDK read markers do not fragment a group.
Incremental diffs, pagination prepends, local-echo replacement, and late member
profile updates all refresh the affected grouping and identity roles.

### Left-aligned message presentation

Normal text and media events now share one Element/Discord-inspired,
left-aligned timeline flow for both the current user and remote participants.
The first event in a visual sender group shows the SDK-derived avatar, sender
name, and timestamp; continuation rows keep the same content indent without
repeating identity. Ordinary incoming/outgoing colored bubbles have been
removed in favor of transparent content and a subtle full-row hover surface.

Reply context, selectable rich text, independently activated links, inline
Matrix media, secure link previews, reactions, thread metadata, send status,
retry, and message actions remain attached to the same stable event. Room
activity stays a separate compact annotation and never receives sender-message
styling.

## v0.5.14 — pagination presentation, room activity, avatars and previews

### Pagination QML exposure and stale-state fix (checkpoint 1)

`PaginationController` is now registered as an uncreatable QML type
(`QML_ELEMENT` / `QML_UNCREATABLE`) so `TimelinePane.qml` can actually resolve
`PaginationController.Hidden/Loading/Failed` — in 0.5.13 the type was never
registered, so every load of the pane threw
`ReferenceError: PaginationController is not defined`.

Separately, `PaginationController::finishBatch()` now emits `stateChanged()`
once a batch completes. It previously dropped `m_requestActive` to `false`
(the input to `busy()`/`presentationState()`) without notifying, so any QML
binding on those properties — the pagination header included — froze on
whatever state it last observed (typically "Loading") even though a fresh
C++ read already reported "Hidden". A new runtime test
(`tests/TimelinePaneQmlTest.cpp`) loads the real compiled QML module through
a real `AppController` on the mock backend specifically to catch this class
of defect, which a text-scan or isolated-controller unit test cannot see.

### Room-activity Expand/Collapse and compact presentation (checkpoint 2)

Clicking Expand on a collapsed room-activity group did nothing: the summary
row's `TapHandler`/`Keys.onPressed` referenced the bare `ListView.view`
attached property, which Qt Quick only populates on the delegate's own root
item — not on a nested child — so it silently resolved to `null`. Every
other action in `MessageDelegate.qml` already qualified this correctly as
`root.ListView.view`; the state-activity block (added in 0.5.13) was the one
place that didn't. Fixed, and pinned by a text-scan regression test
(`QmlBindingContractTest::stateActivityQualifiesListViewViewOnNestedControls`).

Adjacent state-change groups were also splitting apart around invisible SDK
bookkeeping rows (date dividers, read markers, the timeline-start marker) —
`matrix-sdk-ui` freely interleaves those between real events, and
`TimelineModel`'s old grouping only looked at strictly consecutive
`StateChange` rows. Grouping is now transparent through virtual rows: only a
visible message/media event ends a group. Covered by
`tests/StateActivityGroupingTest.cpp`.

The collapsed presentation is now a single compact, discreet row (a chevron
+ "N room updates", Element-style) instead of a bordered, card-like
Rectangle — the whole row is the Expand/Collapse control, keyboard-activates
via Space/Enter, and grants focus on click. Expanded rows are plain compact
text lines with no message-bubble styling, actions, or previews.

### Effective direct-message avatar resolution (checkpoint 3)

One-to-one direct rooms kept showing initials everywhere (room list, timeline
header, Room Information) even after 0.5.13's `resolveMissingDirectAvatars()`
correctly fetched and cached the other member's profile avatar. Root cause:
`RoomListModel::effectiveAvatarUrl()` required the per-room member snapshot
(`RoomInfo::members`) to identify "the other participant" before it would
ever consult that cache — but the Rust backend never populates `members` on
the main room list at all (it is fetched separately, on demand, only for the
Room Information "People" tab, and that result never flowed back into
`RoomInfo`). The member-scan therefore always fell through empty, and the
fetched avatar was silently unreachable. Automated tests didn't catch this
because they artificially populated `members` to make the assertions pass —
a shape the live Rust backend never actually produces.

Fixed by deriving "is this an unambiguous 1:1 DM" from the room's
authoritative `m.direct` target list (`RoomInfo::directUserIds`, newly
parsed from the Rust bridge's `direct_user_ids`) when available, instead of
requiring a member-list fetch; `effectiveAvatarUrl()` now falls back to the
profile cache directly. An explicit room name no longer has any bearing on
this (it never did, but is now explicitly tested). Also fixed
`RoomInfoPanel.qml`, which captured its room's avatar as a one-time snapshot
at open time and never refreshed — it now looks up the room live from
`RoomListModel`, the same as the timeline header, and re-reads on every
`dataChanged`.

### Link-preview compatibility restored (checkpoint 4)

Generic website previews (NYT and most non-YouTube sites) regressed in
0.5.13 relative to 0.5.12. Root cause was in `rust/src/rooms.rs`: 0.5.12's
redirect-following fetch of the page itself used the generous
`MAX_IMAGE_BYTES` (5 MiB) budget, but 0.5.13 narrowed it to the much smaller
`MAX_HTML_BYTES` (2 MiB), so any HTML response larger than that (routine for
a modern news homepage's initial document) was truncated mid-download and
failed before OpenGraph metadata could even be parsed. The fetch now uses its
own `MAX_INITIAL_FETCH_BYTES` budget (equal to `MAX_IMAGE_BYTES`), restoring
both HTML pages and direct-image previews without raising the hard cap used
for the (separate) secondary og:image fetch. Confirmed live against
nytimes.com, bbc.com, wikipedia.org, and example.org, each returning a
successful title/image preview against this build.

Other concrete gaps found and fixed in the same pass:

* No `Accept-Language` header was sent, which increases the odds of unusual
  interstitial/challenge responses from sites that vary behavior by locale
  guesswork. A conservative default (`en-US,en;q=0.9`) is now sent.
* `reqwest`'s feature set only enabled `gzip`; a `deflate`-compressed response
  decoded as garbage before metadata parsing. `deflate` is now enabled
  alongside `gzip` (`brotli` remains unavailable in this offline build
  environment — documented in `rust/Cargo.toml`).
* WebP dimension parsing only handled the extended `VP8X` chunk; the simple
  lossy `VP8` and lossless `VP8L` chunk layouts were unhandled, silently
  dropping width/height (and therefore GIF-class oversized checks and
  preview-card sizing) for a large share of real-world WebP images. All three
  chunk layouts are now parsed, covered by
  `preview_webp_dimensions_cover_all_three_chunk_types`.
* A wrong or generic Content-Type header (some CDNs serve images as
  `application/octet-stream`) is now corrected via a magic-byte sniff
  (`sniff_image_mime`) before MIME-based rejection, instead of trusting the
  header outright.
* The secondary og:image fetch's failure previously propagated via `?` and
  failed the entire preview even though the page metadata (title/description)
  had already been successfully extracted; it is now non-fatal.
* SVG remains explicitly rejected as a non-previewable, non-retryable direct
  image (`preview_image_fields_rejects_unsupported_mime_like_svg`); YouTube's
  existing oEmbed-based path is untouched.

`fetch_url_preview()`'s failure JSON, `MatrixClient::urlPreviewFinished`, and
`LinkPreviewController::onPreviewFinished` now carry two additional sanitized
diagnostic fields — coarse HTTP status and redirect count (0/0 when the
failure never reached an HTTP response, e.g. DNS or timeout) — logged
alongside the existing sanitized hostname and failure category. These are
diagnostic-only: `LinkPreviewController::stateFor()` does not add them to the
QVariantMap QML reads, which
`LinkPreviewTest::diagnosticFieldsAreNotExposedToQmlState` pins directly (and
which was confirmed, via a temporary deliberate leak, to actually fail
without that guarantee).

All SSRF/redirect/DNS-pinning/public-IP protections, the credential-URL
rejection, and the terminal-vs-retryable failure-category split are
unchanged. No NYT-specific bypass or scraping logic was added; any residual
NYT-specific failure that traces to that site's own bot-detection or
geo-blocking policy is a live-server behavior, not something fixable in this
codebase, and is reported as such rather than claimed fixed.

## v0.5.13 — runtime reliability and state activity

### Pagination status lifecycle (0.5.13)

Pagination loading and failure presentation now consumes an explicit logical
state from `PaginationController`. Header geometry and viewport-fill geometry
no longer participate in the binding that decides whether that header exists.

Link-preview ownership is scoped to the room and immutable SDK timeline-item
identity. Delegate reuse, room switches, prepends, and edited URLs revalidate
that ownership before any asynchronous result is displayed.

Preview failures now distinguish transient network/server failures from
terminal policy, content-type, size, and metadata failures. Retry starts a
fresh single-flight request only for transient failures; unsupported direct
media and metadata-free pages do not leave a misleading Retry card.

Animated GIF loading now follows the active `AnimatedImage` lifecycle instead
of the intentionally unused static bridge source. Ready, failed, and reused
delegates therefore stop or reset the loading indicator consistently.

One-to-one direct rooms with a missing member avatar now trigger a deduplicated
SDK profile lookup during room-model refresh. The resulting account-scoped MXC
updates the room-list role and open-room header without requiring user search.

Plain message bodies render independently clickable HTTP/HTTPS anchors while
remaining escaped, selectable text. Browser activation and loaded preview
cards share a credential-free HTTP/HTTPS-only validation boundary.

Rust SDK membership, profile, and common room-state variants now cross the
timeline bridge as typed activity with readable descriptions. Consecutive
activity is rendered as a neutral stable group, collapsed by default with
accessible Expand/Collapse controls and without message actions or previews.

## v0.5.12 — avatars, previews, animated media and pagination

Direct-message room avatars now use the other joined/invited member's profile
avatar when the room has no explicit avatar, but only for authoritative
one-to-one `m.direct` rooms. Explicit room avatars take precedence, group DMs
do not select an arbitrary member, and the value refreshes with membership,
classification, and account lifecycle changes. Room-list, open-room, Room
Information, and member-list avatars share the account-scoped media bridge.
The account footer still lacks an exposed own-profile avatar, and message
sender avatars remain pending a dedicated timeline role.

Confirmation dialogs now use explicit viewport-bounded widths with wrapping
content, and the timeline pagination header separates logical visibility from
fixed geometry. This removes the Room Information, account-menu, and
pagination-status binding loops while preserving narrow-window behavior.

Link previews are now generated locally by Lightning's Rust layer instead of
the homeserver `preview_url` endpoint. HTTPS destinations and every redirect
are DNS-resolved and pinned only after rejecting credentials, local names,
loopback/private/link-local/CGNAT/ULA/multicast/unspecified addresses; request,
redirect, HTML, image-byte, and decoded-dimension limits are centralized.
Only inert metadata and validated images reach QML: no JavaScript, CSS, fonts,
cookies, Matrix credentials, Referer, or page body is exposed. Preview cache
state remains bounded, memory-only, request-deduplicated, account-scoped, and
cleared on logout. Encrypted rooms remain click-to-load by default and warn
that direct website contact can reveal the user's IP address and timing.

Confirmed Matrix GIFs now download and decrypt through the Rust SDK media
path. `MediaBridge` validates the original GIF bytes, writes them atomically
under a short-lived hashed session directory, and exposes only a local
`file://` source to `AnimatedImage`; the timeline and viewer share it. The
in-memory/file cache is bounded by the existing media cache and a 20 MiB GIF
entry cap, and logout removes the temporary directory. Pending send-queue
echoes never start `AnimatedImage`, while disabling animation uses the static
image-provider frame and off-screen timeline animations pause.

Timeline pagination now defers viewport-fill requests until the active Rust
timeline is initialized instead of treating startup as a dispatch failure.
Completion settles after queued SDK diffs, counts unique stable prepended
events (including index-zero inserts), retains the request reason through
logging/callbacks, and preserves the existing budget, duplicate, stale-result,
and no-progress protections. Scroll anchors are cleared on room/reset/logout,
failure/cancellation, generation changes, and reached-start without insertion.

## v0.5.11 — pagination, media, avatars, themes, link previews

This release completes the QML/C++ integration on top of the 0.5.11 backend
foundations (pagination policy, read-receipt coordinator, bare-localpart user
lookup, shared media pipeline, link-preview backend, GIF classification and
preview settings).

### Automatic backward history + scroll preservation

`PaginationController` (`app.pagination`) owns the request policy over the
Rust single-flight `paginate_backwards`. The timeline asks for a viewport
fill whenever `contentHeight < height`, so a short initial snapshot that can
never scroll still fetches older batches until the viewport fills, the SDK
reports the start of history, a bounded automatic-fill budget is spent, or
no-progress is detected. Approaching the top (within half a viewport) triggers
a near-top request before the exact zero position. The header shows only
transient loading / failure (with Retry) states — there is no permanent
"scroll up" placeholder; the start of history is the virtual
"Beginning of conversation" row.

Backward prepends preserve position: when a request starts and the user is
not following the bottom, the timeline records the first visible event's
stable id (`TimelineModel::stableIdAt`) and its pixel offset, then on
`paginationCompleted` re-aligns to that event (`rowForStableId` +
`positionViewAtIndex`), falling back to a content-height delta if the anchor
scrolled out of the created range.

### Responsive timeline media

Timeline images derive their display box from the intrinsic media dimensions
and a responsive bound (max ≈ 400×360 logical px, capped to the column
width), never upscaling tiny images and never collapsing to the timestamp
width. The media box contributes a real implicit width so the bubble grows to
the picture. Loading/failure/retry states and click-to-open are retained, and
media is fetched once through the shared `MediaBridge` even as delegates are
recycled. Confirmed GIFs animate only when a direct http(s) URL is available
and the "Animate GIF previews" setting is on (the bridge serves single frames
through an image provider, so bridge/encrypted GIFs show a static frame and
open full-size on click).

### Avatars

A shared `Avatar.qml` resolves an mxc URI through `MediaBridge` and shows a
stable initial placeholder until a bitmap is fully decoded (masked to the
avatar shape with `MultiEffect`), so no broken-image glyph or stale-avatar
flash occurs on delegate reuse, account switch or logout. It is wired into the
room list, Space rail (a Space is a room), room header, Room Information and
invite results. Per-message sender avatars remain a known gap: `TimelineModel`
does not yet expose a sender-avatar role.

### Bare-localpart invite search

`UserPicker` shows avatars and per-row provenance ("From your server" for a
homeserver-confirmed exact local lookup, "Exact Matrix ID" for a typed full
id) using the backend `source` role. Typing `admin`, `@admin`,
`admin:server` or `@admin:server` resolves through the confirmed exact
profile lookup; a bare localpart appears only once the homeserver confirms it.

### Appearance themes

`AppTheme.qml` resolves one of six presets (System, Light, Graphite, Midnight
Blue, Nord, Purple Dusk) to a full semantic palette; components never branch
on the theme. System follows the platform colour scheme
(`AppController.systemDarkMode` from `QStyleHints::colorScheme`). The Light and
Midnight palettes are byte-for-byte the previous Light/Dark values, so
existing views are unchanged under them. `SettingsManager` validates an
out-of-range stored theme back to System.

### Link previews and GIFs

`LinkPreviewController` (`app.linkPreviews`) drives a timeline preview card
using the protected Rust client-side fetcher described in the 0.5.12 section.
Unencrypted rooms auto-load per the `autoLoadLinkPreviews` setting.
**Encrypted rooms default to explicit click-to-load** and direct website/IP
disclosure is stated before loading. Preview metadata stays memory-only and a
failed preview never alters the underlying message row.

### Settings, login, toolbar, receipts

Settings gains Theme (six presets), "Automatically load previews in
unencrypted rooms", "Load previews in encrypted rooms" (off by default, under
a privacy note) and "Animate GIF previews". The login form was rebuilt with
`ColumnLayout`/`RowLayout` inside a `Flickable` so it fits from narrow to wide
windows and high-DPI scaling without clipping, with a password reveal toggle,
Enter-to-submit and tab order. The message action toolbar shares one hover
region across the bubble and buttons (no gap dropout) and stays visible while
its reaction picker or menu is open. Read receipts are sent by
`ReadReceiptCoordinator` only when the room is open, the window is active, the
timeline is visible, the user is near the bottom, and the newest eligible
event has a real remote id — after an 800 ms debounce with full re-validation.

## v0.5.10 — reactions and composer emoji picker

One reusable `EmojiPicker.qml` now serves reaction and composer modes. Its
immutable C++ `EmojiCatalog` parses the embedded generated TSV once, while
filtered views retain catalogue indexes and cap global search at 512 results.
The `GridView` creates only visible cells; a 150 ms UI debounce avoids work for
every keystroke. Search is case-insensitive across CLDR English names, group
names and GitHub/gemoji aliases. Categories are Recently Used, Smileys &
Emotion, People & Body, Animals & Nature, Food & Drink, Travel & Places,
Activities, Objects, Symbols and Flags.

The committed 3,943-sequence dataset is generated from `emojis 0.8.2`, already
pinned by `matrix-sdk-ui 0.18.0` and now an exact direct development-time
dependency. It uses Unicode Emoji 17.0 CLDR order and contains flags, ZWJ
families/professions and validated default/single/mixed skin-tone sequences.
Licensing is `(MIT OR Apache-2.0) AND Unicode-3.0`; only metadata and Unicode
text are bundled, never artwork. Runtime loading is local and never accesses
the network or Matrix FFI.

Recent emoji are a shared, deduplicated, most-recent-first QSettings list
bounded to 32 sequences; invalid persisted values are ignored by the model.
The optional preferred tone is also local. Right-click, long-press or Alt+V
opens only dataset-provided variants, including the base/default choice.

The reaction picker preserves the stable item/event keyed toolbar pin and
calls the existing `MessageComposer::reactTo` / `MatrixClient::toggleReaction`
path exactly once. Matrix reaction keys remain exact Unicode sequences. The
composer saves the QML TextArea's UTF-16 selection/cursor, replaces the
selection or inserts at that boundary, advances by the chosen QString length,
and restores focus; it never sends or touches attachments. Popups live in the
window overlay, clamp on every edge, close on outside press/Escape, expose
accessible names/tooltips, and use a virtualized keyboard-navigable grid.

## v0.5.9 — user search, DMs, room creation, invites, room info; media FFI foundation

Scope note: this release delivers the conversation-creation and membership
milestone (the original plan's Phases 7–10), the full media pipeline
(Phases 12–15: composer attachments, received media, image viewer), the
semantic theme system (Phase 2), the account menu with the relocated Sign
out (Phase 3) and the two-pane Settings redesign (Phase 4). The emoji picker
was delivered in the following v0.5.10 release.
`docs/matrix-feature-status.md` reflects exactly what is wired.

### Theme system (Phase 2)

- `AppTheme.qml` now defines the full semantic token set — surfaces
  (window/nav/panel/surface/elevated/hover/selected/selected-hover/input),
  borders (subtle/strong), text (primary/secondary/muted/disabled), accent
  (base/hover/pressed/on-accent), status (success/warning/danger/info),
  messaging (incoming/outgoing bubble, on-accent muted ink, bubble
  overlays, code block, reaction backgrounds, unread/mention badges),
  focus ring and overlay scrim — plus semantic typography roles
  (fontPageTitle/SectionTitle/RoomTitle/MessageSender/Body/Secondary/
  Caption/Mono) on the existing spacing/radius scale. Legacy aliases keep
  older QML compiling.
- Contrast: the light theme got its own muted/secondary inks (the old
  palette reused the dark theme's grey at 2.2:1); outgoing bubbles use a
  deeper blue than the control accent so white body text is ≈ 6.2:1 and
  muted meta ink ≈ 4.9:1; selected room rows (and their hover states) stay
  ≥ 4.5:1 in both themes. `tests/ThemeTokensTest.cpp` asserts required
  tokens exist, computes WCAG contrast for every critical pair, verifies
  the light palette is not inverted-dark, and fails if core view QML
  contains any hex colour or `Qt.rgba` literal — RoomDelegate and
  MessageDelegate were swept onto tokens (the image viewer's dark overlay
  chrome is a deliberate committed-dark exception).

### Account menu and Sign out (Phase 3)

- The sidebar footer is now one compact account button: avatar initial
  with a connection dot, clean localpart, homeserver as secondary text.
  It opens `AccountMenu.qml` — Settings, Security & Recovery, About
  (each landing on the matching Settings category via
  `AppController::showSettingsSection`), and at the bottom the only Sign
  out in the application: danger-styled, behind a confirmation dialog
  whose focused default is Cancel and whose copy explains the local
  account-store removal without exposing paths. Escape/click-outside
  close; no second Sign out exists anywhere (the old footer button and
  the Settings-page button are gone).

### Settings redesign (Phase 4)

- `SettingsScreen.qml` is now a two-pane layout: left category navigation
  (General, Appearance, Notifications, Account, Security & Recovery,
  Advanced, About), right an independently scrolling pane of grouped
  cards with a page heading. Categories switch by visibility — never a
  Loader — so an in-flight verification or key import survives navigating
  away and back.
- Only implemented controls appear: General (start minimized), Appearance
  (theme with a live palette preview, language), Notifications (the
  existing enable toggle plus an honest "push registration not
  implemented" note), Account (identity, device, trust state, homeserver
  URL, a pointer to the account menu for Sign out — no duplicate button),
  Security & Recovery (everything from 0.5.6–0.5.8: secret/crypto backend
  status, session verification with the full SAS card, recovery key,
  encrypted room-key import, and the destructive local reset moved into a
  collapsed, danger-bordered Danger Zone), Advanced (backend, sync mode,
  connection, refresh current room), About (version, description, pinned
  SDK versions, licence).

### User search (Phase 7)

- `mx_rust_search_users` wraps the pinned `Client::search_users` (user
  directory). `UserSearchModel` (C++) debounces at 300 ms, requires 2+
  characters, caps at 20 results, and accepts complete `@user:server`
  Matrix IDs directly — the typed MXID is always offered as the first row
  even when the directory does not return it. Stale completions are
  rejected by operation id (only the most recent dispatch may populate the
  model), duplicates are removed by user id, and the current user is
  excluded. Query text and result payloads are never logged or persisted.
- One reusable `UserPicker.qml` (input + results + keyboard navigation:
  arrows, Enter selects, loading/no-results/error states) is used by the
  New Conversation and Invite People dialogs.

### Start a Direct Message (Phase 8)

- The sidebar `＋` button opens `NewConversationDialog` (Direct Message /
  New Room). Selecting a user first runs the existing-DM check through
  `Client::get_dm_rooms` — the SDK's authoritative `m.direct` projection —
  and offers **Open existing conversation** before any creation; creating
  a duplicate requires an explicit "start a new conversation anyway".
- Creation uses the pinned `Client::create_dm`: `TrustedPrivateChat`
  preset, `is_direct`, encryption initial state
  (`RoomEncryptionEventContent::with_recommended_defaults()`), server-
  chosen room version, and the SDK's `Account::mark_as_dm` — a mutex-held
  fetch-merge-store of `m.direct` that preserves every other mapping.
  Lightning never composes account data itself.
- Self-DMs are rejected; `busy` single-flighting blocks double-clicks; the
  room opens only after it appears in the authoritative room list (with a
  bounded 10 s fallback); sign-out clears the pending operation so a stale
  completion can never open a room in a new session.

### Create room (Phase 9)

- `mx_rust_create_room` builds the pinned ruma `create_room::v3::Request`
  from validated options: name (required), topic, private/public preset +
  visibility, optional local alias (public only), optional encryption
  initial state, deduplicated initial invites, `is_direct: false`, no
  locally generated room id or pinned room version. Unit-tested in Rust
  (`rooms::tests`).
- Dialog defaults: encryption ON for private rooms with an explicit
  warning when disabled; public rooms are created unencrypted with a note.
- Optional placement into the currently active Space sends `m.space.child`
  via `send_state_event_for_key`. Placement failure is reported as a
  warning (`spacePlacementFailed` → status-bar message) and never reads as
  a failed room creation.

### Invite people (Phase 10)

- Room Information → People → **Invite** (visible only when the SDK's
  `RoomMember::can_invite()` allows it — never guessed from role labels).
  Invites go through `Room::invite_user_by_id`, sequentially, with
  per-user pending/ok/failed rows (coarse categories: forbidden,
  rate_limited, network), deduplication, an already-joined/invited
  pre-check against the loaded member snapshot, and a batch summary.
  One user's failure never discards the others' results. Membership
  refreshes through authoritative sync (`membersChanged`).

### Room Information panel + membership

- `RoomInfoPanel.qml` (right side of the chat column, toggled from the
  room header or the ⓘ button; Escape closes; collapses below 700 px):
  - **Overview** — avatar initial, name, topic, encryption state, member
    counts, Copy room ID, permission-gated name/topic editing
    (`Room::set_name` / `set_room_topic`, gated on
    `RoomMember::can_send_state`; no optimistic writes — sync is the
    authority), and **Leave room** behind a confirmation dialog whose safe
    default is Cancel. Leaving closes the open timeline and returns to the
    no-room state; the list entry disappears via the authoritative room
    list, and nothing is forgotten server-side.
  - **People** — member search, joined/invited state, Admin/Mod role
    labels from `suggested_role_for_power_level`, full MXID as secondary
    text (primary when the SDK flags the display name ambiguous), and the
    Invite button. Clicking a member opens `MemberProfilePopover` (avatar
    initial, display name, full MXID, membership state, role, and
    **Message** — which reuses an existing DM via m.direct or creates a
    new encrypted one; no moderation actions in this release).
  - **Media & Files** — media shared in the *loaded* timeline
    (`TimelineModel::mediaEntries()`, newest first; no automatic history
    fetch). Images open in the in-app viewer; every entry offers Save As
    through the media bridge.
  - Room avatar editing (permission-gated): Change avatar… (image picker →
    `mx_rust_set_room_avatar`, magic-byte sniffed in Rust) and Remove
    avatar, alongside the name/topic editors.
- `mx_rust_room_members` returns a bounded snapshot (500 rows, truncation
  flagged, joined before invited, then power, then name) plus the caller's
  own permission flags (`can_invite`, `can_send_state` for
  RoomName/RoomTopic/RoomAvatar). Member data stays in memory only — it is
  never written to CacheStore.
- Avatar editing FFI exists (`mx_rust_set_room_avatar` reads the file in
  Rust, sniffs PNG/JPEG/GIF/WebP/BMP magic bytes, ≤ 8 MB, uploads via
  `Room::upload_avatar`; `mx_rust_remove_room_avatar`); the Overview UI
  for it lands with the media UI pass.

### Media sending (Phases 12–13)

- Send: `mx_rust_timeline_send_attachment` /
  `_bytes` → `Timeline::send_attachment(...).use_send_queue()` — SDK-owned
  local echo through the existing diff stream, transparent E2EE for
  encrypted rooms, retry via the existing unwedge path. The bytes variant
  exists so clipboard images never touch a temporary file.
- Composer: unified bar (attach `＋`, expanding multiline editor capped at
  ~6 lines, Send; Enter sends, Shift+Enter newline). The attachment tray
  (`AttachmentQueueModel`) holds prepared files with name/size/thumbnail,
  per-entry queued → dispatching → failed(+retry) state, and remove
  buttons; nothing uploads until the user sends. Validation on add:
  directories, unreadable and empty files rejected; MIME detected from
  *content* (`QMimeDatabase::MatchContent` — a mislabelled extension cannot
  pick the send path); size gated against the server's `m.upload.size`
  (fetched once per session; conservative 100 MB fallback). Image
  dimensions come from a header-only `QImageReader` probe.
- Drag-and-drop (`text/uri-list` only) highlights the composer; clipboard
  paste intercepts Ctrl+V — images become bounded in-memory PNG attachments
  (scaled ≤ 4096 px, sent via the bytes FFI, no temp file), file-manager
  URL lists become attachments, and plain text always pastes as text (a
  pasted path string is never treated as a file). Queued attachments are
  dropped on room switch and sign-out; already-dispatched uploads belong to
  the SDK send queue. Attachment sends go first, then the composed text as
  its own message. Covered by `tests/AttachmentQueueTest.cpp`.

### Received media and the image viewer (Phases 14–15)

- Receive: the timeline serializer now records each media item's
  `MediaSource` (including encrypted sources, which never cross the FFI)
  in a per-room Rust-side registry keyed by `media_key` (event id, or SDK
  unique id for local echoes) and adds `media_key` /
  `media_source_available` / `media_thumb_available` to item payloads.
  `mx_rust_media_fetch` retrieves (and for encrypted rooms decrypts)
  through `Media::get_media_content`; bytes are parked in Rust and handed
  over through a dedicated `mx_rust_media_take` / `mx_rust_media_free`
  binary bridge — never the JSON queue — into `MediaBridge`'s 64 MB LRU
  in-memory cache serving QML via `image://lightning-media/`
  (`MediaImageProvider`, decode bounded at 4096 px). `saveAs` writes
  atomically (QSaveFile) to the user-chosen destination with a
  re-sanitized file name and never opens the file. The cache and parked
  payloads are cleared on sign-out. `mx_rust_media_fetch_mxc` serves
  plain-mxc avatar thumbnails; `mx_rust_fetch_upload_limit` supplies the
  composer's upload-size gate.
- Timeline UI: image messages render bounded aspect-ratio thumbnails
  through the bridge (encrypted rooms included — the SDK decrypts inside
  Rust); loading and failed-with-retry placeholders; the HTTP backend keeps
  its URL path. File messages show icon/name/size/MIME with an explicit
  **Save** (bridge Save As dialog) on the Rust backend and the old external
  Open on HTTP. Files are never executed or auto-opened.
- `ImageViewerOverlay` (in-app, hosted by TimelinePane — the SDK timeline
  is untouched): full-window scrim, fit-to-window, wheel/keyboard zoom
  (20 %–800 %) with pan, previous/next across the images currently loaded
  in the timeline (`TimelineModel::imageEntries()`; no history fetched),
  filename + sender/timestamp captions, Save As, loading/error states with
  retry, Escape/scrim-click close, animated GIF playback via
  `AnimatedImage`.
- Cache security: `LIGHTNING_MEDIA_CACHE_TEST_059` in
  `tests/CacheStoreSecurityTest.cpp` proves encrypted-room media metadata
  (filename/media key/MIME) never reaches raw `cache.sqlite` bytes; media
  payloads themselves exist only in the in-memory bridge cache.

### Identity groundwork

- Timeline items now carry `sender_name_ambiguous` (SDK profile
  ambiguity) and `TimelineModel` adds `sameSenderAsPrevious` (same sender
  within 5 minutes, virtual rows break runs) — consumed by the deferred
  message-grouping UI pass.

### Lifecycle and safety

- Every new Rust command is a managed `spawn_room_action` task (joined on
  shutdown), stamps the lifecycle generation, and is dropped when stale.
  C++ controllers match completions by operation id and reset on
  `loggedOut`, so no late callback can mutate a new session; tested in
  `tests/ConversationFlowTest.cpp` (stale search results, stale DM
  completions after sign-out, duplicate submissions, partial invite
  success).
- HTTP/Mock builds keep working: the new `MatrixClient` virtuals default
  to inert no-ops, `supportsRoomManagement()` gates every new UI entry
  point, and both build trees pass all 8 CTest suites.

## v0.5.8 — Modern room list, Spaces, DMs, invites, unread, receipts and typing

### Authoritative sync ownership

- Login/restore still creates exactly one `matrix_sdk::Client` and installs
  the existing timeline, E2EE and verification handlers once.
- `mx_rust_start_sync` enters `probing` and calls the pinned SDK's
  `Client::supported_versions()`. `FeatureFlag::Msc4186` is the exact signal
  for ruma 0.24's
  `/_matrix/client/unstable/org.matrix.simplified_msc3575/sync` endpoint.
- When supported, `matrix_sdk_ui::sync_service::SyncService` owns the
  `RoomListService` and `EncryptionSyncService`. Its single
  `EncryptionSyncPermit` is the proof that no second encryption sync runs.
  Stop awaits the supervisor, which stops and joins both child streams.
- Only an authoritative absent capability, `M_UNRECOGNIZED`, or endpoint
  not-found selects `classic_fallback`. Temporary network/TLS failures stay
  offline/retrying in the selected mode. Unknown-token/forbidden remains a
  fatal authentication error. Store/setup failures are never compatibility
  fallback signals.
- Compatibility mode uses the existing one `Client::sync_with_callback`
  loop, including its crypto/to-device processing. It lacks Sliding Sync
  range loading and unified offline supervision, but emits the same room,
  Space, DM, invite, unread and typing contract as modern mode.

### Reactive room and Space state

- Modern `RoomList::entries_with_dynamic_adapters` uses the SDK non-left
  filter and recommended latest-event/recency/name sorting. Every pinned
  `VectorDiff` variant crosses FFI as a bounded safe envelope. C++ validates
  indexes and duplicate room IDs before updating its ordered registry.
  `RoomListModel` then reconciles by stable room ID with insert/remove/move/
  data-change notifications; an index never identifies the selected room.
- `Room::direct_targets()`—the SDK projection of global `m.direct` account
  data—is authoritative. No mapping means no DMs; malformed/stale entries are
  ignored by SDK deserialization; two-member rooms receive no special case.
- `SpaceService::space_filters()` supplies joined, ordered, cycle-pruned Space
  graph data. Lightning combines the SDK's two presentation levels into a
  transitive descendant set, deduplicates Home and per-Space results, accepts
  multiple parents, ignores inaccessible/unjoined descendants, and enforces a
  defensive traversal depth of 64 in C++.
- Invitations are `RoomState::Invited` rows. Accept calls `Room::join`; reject
  calls `Room::leave`. Buttons become pending immediately, duplicate clicks
  are disabled, and lifecycle generation rejection prevents an old action
  from reaching a new session.

### Unread, receipts and typing

- Room payloads contain SDK client unread messages/notifications/mentions,
  server notification/highlight counts, and `Room::is_marked_unread()`.
  Space counts sum each deduplicated joined descendant once.
- Mark unread calls `Room::set_unread_flag(true)`. A legitimate read uses
  `Room::send_multiple_receipts(Receipts::fully_read_marker(...)
  .public_read_receipt(...))`, which also clears marked-unread through the
  SDK. C++ deduplicates the last event ID. QML only requests a read while the
  application is active and the timeline is at/near the bottom; pagination
  and older-history inspection do not mark read.
- Incoming SDK `SyncTypingEvent` lists replace the previous room set and omit
  the current user. Display names are resolved from bounded room membership
  lookups. The composer sends transitions only (not keystrokes), renews every
  3 seconds for the SDK's 4-second notice, and sends false on clear, send,
  room/screen switch and sign-out. Composer text is never logged or forwarded.

### Privacy and lifecycle

- Room-state command tasks are owned join handles. Shutdown order is active
  timeline, room-state commands, room-key import, unified/classic sync, then
  client/store release. C++ lifecycle generations still reject queued events
  after sign-out.
- Encrypted latest-event preview plaintext is memory-only. Even if an
  encrypted `RoomInfo` reaches `CacheStore::saveRoom`, the preview column is
  forced empty. `LIGHTNING_ROOM_PREVIEW_CACHE_TEST_058` is scanned from raw
  `cache.sqlite` bytes by `cache-store-security`.

## v0.5.7 — Live SDK timeline, decryption retry, pagination, local echoes

### Why

v0.5.6 proved that encrypted room-key import works (keys persisted; a full
restart made old messages decrypt) but the loaded timeline never
reprocessed existing undecryptable events. Root cause: the Rust backend's
history path was a snapshot (`Room::messages`) fed into an append-only,
event-id-deduplicated C++ list — a re-fetched, now-decryptable event was
*skipped* because its event id already existed, so the placeholder row
could never be replaced without rebuilding everything from scratch
(i.e. restarting).

### Timeline architecture

- The Rust bridge now owns persistent `matrix_sdk_ui::Timeline` objects
  (`matrix-sdk-ui 0.18.0`, the exact release matching the pinned
  `matrix-sdk 0.18.0`; `matrix-sdk` itself was **not** upgraded).
- `rust/src/timeline.rs` hosts a `TimelineRegistry` with a single active
  room timeline (simplest safe design): opening a room advances the
  **room generation**, aborts + drops the previous subscription/timeline,
  builds a fresh `TimelineBuilder::new(&room).build()`, and calls
  `Timeline::subscribe()` — the SDK returns the initial item vector and
  the diff stream atomically, so no live event can fall in between.
- A shared multi-thread Tokio runtime (2 workers) now lives on the
  bridge. Timeline tasks, the SDK event cache, the send queue, and
  room-key import all run there; the old per-FFI-call throwaway runtimes
  could not host anything that outlives one call.
- Every `VectorDiff` variant is forwarded over the existing poll queue as
  a `timeline_diff` JSON envelope: `append`, `push_back`, `push_front`,
  `insert`, `set`, `remove`, `pop_front`, `pop_back`, `clear`,
  `truncate`, `reset` — each stamped with `room_generation` + `lifecycle`.
- C++ (`src/matrix/RustTimelineIngest.{h,cpp}`) validates each envelope
  against a mirrored list before mutating it; malformed or out-of-range
  diffs are rejected (mirror untouched) and recovered with one fresh
  snapshot. `TimelineGenerationTracker` adopts the generation from the
  most recently *requested* room's `timeline_reset` and rejects
  everything stale — diffs from a previous room, a previous open of the
  same room, or a signed-out lifecycle can never mutate visible state.
- `TimelineModel` gained index-based diff application
  (`eventInsertedAt` / `eventChangedAt` / `eventRemovedAt` /
  `eventsTruncatedTo` interface signals) with correct
  `beginInsertRows`/`beginRemoveRows`/`dataChanged` notifications and
  defensive re-validation (invalid index → full reload, never
  corruption). Full model resets happen only for the initial snapshot
  and the SDK's own `Clear`/`Reset` diffs.
- Item identity is the SDK's `TimelineUniqueId` (`itemId` role), so an
  undecryptable placeholder becomes its decrypted form **in place**, a
  local echo becomes the remote event in place, and edits/redactions/
  reactions update the original row. Virtual items (date divider, read
  marker, timeline start) are forwarded as rows so diff indices stay
  aligned; QML renders them as thin separators.
- Live-sync `timeline_event`s for the open room now only refresh the
  room-list preview — the SDK timeline is the single source of truth for
  visible rows, eliminating the duplicate-source problem
  (Room::messages + live sync + C++ echoes all appending to one list).

### Immediate decryption retry (the 0.5.7 acceptance fix)

- `mx_rust_import_room_keys` keeps the `RoomKeyImportResult.keys` map
  in Rust, flattens it to `(room_id, [megolm session ids])` pairs
  (sender keys dropped, key material never present), and — right after
  emitting `room_key_import_done` — awaits the pinned SDK's
  `Timeline::retry_decryption(session_ids)` on the open room's timeline
  when it appears in the import result.
- The SDK emits in-place `Set` diffs for every item it can now decrypt;
  they flow through the normal subscription channel. No restart, no
  manual "Refresh current room", no room switch, no duplicate rows, no
  scroll jump (a `Set` does not change the row count), composer text
  untouched.
- The UI shows "Imported room keys applied to the open timeline." —
  keys applied, deliberately not "everything decrypted".
- Import still never changes verification state. Re-importing the same
  export reports 0 sessions and is a no-op.
- Rooms not currently open decrypt naturally when opened later (their
  timeline is rebuilt with the imported keys already in the store).

### Backward pagination

- Scrolling near the top triggers `Timeline::paginate_backwards(20)`
  (`PAGINATION_BATCH`, documented constant on both sides).
- Single-flight per room (atomic guard in Rust); states are
  idle / loading / failed / end-reached, surfaced to QML as
  `canPaginate` / `paginating` / `paginationFailed`.
- Failure shows "Could not load older messages — Retry"; end of history
  removes the header entirely; prepends keep the visible scroll anchor.
- Stale pagination completions (room switched / signed out mid-flight)
  are dropped by generation checks on both sides.

### Local echoes and send state

- Text sends in rooms with a live timeline go through `Timeline::send`
  (send queue): the SDK creates the local echo instantly, drives
  sending → sent/failed, and reconciles the remote echo in place — the
  old C++-built `local:` echo path is no longer used on this route.
- Failed sends show a coarse safe category ("network" / "rejected") and
  a **Retry** action that calls `SendHandle::unwedge()` — the same
  queued item is re-attempted, so retries cannot duplicate.
- Replies / edits / reactions / redactions on the Rust backend now use
  the official timeline APIs (`send_reply`, `edit`, `toggle_reaction`,
  `redact`) — no hand-built relation JSON. The HTTP backend keeps its
  existing behavior; encrypted sends remain blocked there.

### Lifecycle and shutdown

- New `mx_rust_shutdown_tasks`: advances the lifecycle generation,
  aborts + joins the timeline subscription, **joins** an in-flight
  room-key import (a bounded 15 s timeout remains only as a last-resort
  error boundary after the deterministic join), then stops sync. Called
  on sign-out before server logout and store cleanup, and again on
  handle release — the crypto store can no longer be deleted while a
  task still owns it. This replaces the v0.5.6 ~5 s `import_active`
  poll loop.
- Room switching, sign-out during pagination/import/retry, and app
  shutdown all resolve through the same generation + abort/join path.

### Security invariants (unchanged, now regression-tested)

- Decrypted encrypted-room plaintext stays memory-only; `CacheStore`
  refuses `isEncrypted` rows on every write path
  (`tests/CacheStoreSecurityTest.cpp` proves the unique marker never
  reaches `cache.sqlite`, including local echoes, edits, replies, and
  replacements). The Rust backend does not use `CacheStore` at all.
- No Megolm/Olm/session-key material crosses the FFI; retry uses session
  *identifiers* kept in Rust. Logs carry counts and coarse categories,
  never message bodies or key material.

### Known limitations

- Sliding Sync is not adopted; classic sync remains.
- The room list is not on `RoomListService`.
- One active room timeline (no LRU cache of recently open rooms yet);
  switching rooms rebuilds from the SDK event cache.
- Media on the Rust backend: plain (unencrypted) mxc sources only;
  encrypted media download/decrypt is future work.
- Stickers, polls, live location render as safe placeholders.
- QR verification and full device management remain unimplemented.

## v0.5.6 — Lightning-initiated SAS and encrypted room-key import

Two conceptually distinct security capabilities landed in this release, and
the Settings UI now enforces the distinction:

1. **Session (device) verification** — Lightning can initiate a Matrix SAS
   verification against another session belonging to the same account.
   Element (or any other cross-signed session) receives the request and
   accepts it; both sides then display and confirm the same seven emojis.
   After the flow completes, Lightning re-queries the SDK's cross-signing
   state and only labels the session **Verified** when
   `Device::is_cross_signed_by_owner()` returns true.

2. **Encrypted Megolm room-key import** — Lightning can decrypt a
   passphrase-protected Element/Matrix-SDK-compatible room-key export and
   import its inbound room sessions into the local Rust SDK crypto store.
   Imported keys may unlock older encrypted messages but **do not** verify
   the session, do not cross-sign the current device, and do not replace
   SAS verification.

Neither operation is Secure Backup restoration — the existing recovery-key
button is preserved and clearly separated in the UI ("Recovery key" → Secure
Backup; "Import room keys" → local file; "Verify this session" → SAS).

### Rust FFI additions

`rust/src/lib.rs` / `rust/include/matrix_rust.h`:

- `mx_rust_start_own_verification` — dispatches
  `UserIdentity::request_verification_with_methods(vec![VerificationMethod::SasV1])`
  against the account's own identity, then drives the SAS state machine.
  Reuses the existing `active_request` / `active_sas` slots and the same
  `verification_ready` / `verification_sas_ready` / `verification_done`
  events, so inbound and outbound flows share every downstream handler.
  Emits `verification_request_started` synchronously so the UI can flip to
  "Waiting for the other session…" immediately.
- `mx_rust_query_own_device_status` — returns a JSON snapshot with
  `device_id`, `own_identity_available`, `own_identity_verified`,
  `device_cross_signed`, `has_master`, `has_self_signing`,
  `has_user_signing`. `device_cross_signed` (from
  `Encryption::get_own_device()::is_cross_signed_by_owner()`) is the sole
  source of truth for the UI's "Verified" label.
- `mx_rust_import_room_keys` — delegates to
  `Encryption::import_room_keys(path, passphrase)`. Passphrase and decrypted
  key material stay inside the SDK's Zeroizing buffer; the FFI only forwards
  aggregate counts and (already-public) affected room IDs back to C++.
- `mx_rust_room_key_import_active` — atomic flag that gates duplicate
  imports and lets the sign-out path wait for an in-flight import to finish
  before dropping the SDK store.
- `classify_import_error` — pure classifier for wrong-passphrase / invalid
  file / read-failed / import-failed. Rust unit-tested in `#[cfg(test)]`.

### C++ layer

`src/matrix/RustSdkMatrixClient`:

- Adds `startOwnVerification`, `refreshOwnDeviceStatus`, `importRoomKeys`,
  `roomKeyImportActive`. Adds the matching signals:
  `verificationRequestStarted`, `ownDeviceStatusUpdated`,
  `roomKeyImportStarted/Progress/Done/Failed`.
- `logout()` waits up to ~5 s for an active import to complete before
  releasing the Rust client and deleting the store. Sign-out remains
  authoritative and bounded — a stuck import cannot pin the app.

`src/app/AppController`:

- Adds an application-facing security state: `sessionTrustState`
  (`Unknown` / `Not verified` / `Verified` / `Cross-signing unavailable`),
  `sessionDeviceId`, `ownIdentityAvailable`, `crossSigningAvailable`.
- Adds room-key import state: `roomKeyImportState`,
  `roomKeyImportImportedCount`, `roomKeyImportTotalCount`,
  `roomKeyImportAffectedRoomCount`, `roomKeyImportLastMessage`,
  `roomKeyImportRunning`, plus `roomKeyImportCompleted(imported, total,
  affected)`.
- Adds `Q_INVOKABLE`s `startOwnVerification`, `refreshSessionTrustState`,
  `importRoomKeys(fileUrl, passphrase)`. The passphrase is never stored in
  a member field; it flows straight through to Rust.
- Verification-done handler re-queries the SDK trust state. It never
  promotes the label to Verified locally.
- Logout clears the verification / security / room-key-import caches so a
  freshly logged-in session cannot inherit stale results.

### QML

`qml/SettingsScreen.qml`:

- Renames the Rust encryption pane to **Security & Recovery** and lays it
  out as three explicit blocks:
  - **Current session** — device ID + `Status:` label (Verified / Not
    verified / Unknown / Cross-signing unavailable).
  - **Verify this session** — button; shows waiting state and the seven
    emojis once the SDK reaches `KeysExchanged`. Confirm / mismatch /
    cancel are shared with the receive-first flow.
  - **Recovery key** — existing Secure Backup restore, unchanged.
  - **Import room keys** — file picker + password-echo passphrase field +
    progress bar + aggregate result summary. Passphrase is cleared on
    dispatch, on success, on failure, and on Clear.
- Explanatory copy under the section explicitly disambiguates SAS
  verification, Secure Backup recovery, and encrypted room-key import.

### Threat model

- Passphrase lifetime is bounded to one dispatch: QML wipes the field
  after `app.importRoomKeys(...)`, C++ forwards a stack `QByteArray` and
  zeroes it before return, Rust wraps it in `zeroize::Zeroizing` inside
  `Encryption::import_room_keys`. No `QSettings` / `SecretStore` /
  SQLite / log ever holds it.
- Decrypted key material never crosses the FFI. `mx_rust_import_room_keys`
  returns only `{imported, total, affected_rooms, room_ids}`.
- No plaintext temporary file — the decrypt path is streamed inside the
  SDK.
- The source export file is opened read-only by `matrix-sdk`. Lightning
  never modifies, renames, or deletes it.
- `CacheStore` continues to refuse encrypted `TimelineEvent` rows, so
  decrypted encrypted-room plaintext still remains memory-only even after
  imported keys unlock older messages.
- Verification trust promotion depends on
  `Device::is_cross_signed_by_owner()`, not on a local
  "user pressed 'They match'" flag.

Automated coverage: existing tests still pass in both build trees plus new
`#[cfg(test)]` unit tests inside the Rust crate for the import-error
classifier.

---

# Current state (v0.5.5 — Rust sign-out session reset)

## v0.5.5 — coordinated Rust sign-out and account-scoped reset

The Rust backend previously stopped sync by setting a flag and launched
logout on a detached thread. It did not join sync, destroy the Rust client,
or delete the SDK store. All old tasks shared one event queue, so a sync
request could report `M_UNKNOWN_TOKEN` after logout and change the footer to
Error. `logged_out` later cleared QSettings/SecretStore metadata, but the old
device's SDK crypto store remained. A password login then created a new
device and tried to attach it to that retained store, which matrix-sdk
correctly rejected as an account/device ownership mismatch. The Login reset
also depended on already-saved `SettingsManager::userId()`, which logout had
just cleared.

The corrected order is:

1. Capture normalized homeserver, full MXID, device id, canonical slug,
   account root, Rust store, and existing smoke-session paths.
2. Mark intentional sign-out and advance the lifecycle generation.
3. Cancel the Rust sync future and join its owned thread.
4. Reject every event from the invalidated handle except its `logged_out`
   completion.
5. Attempt Matrix SDK logout. `M_UNKNOWN_TOKEN` here means the session is
   already logged out; other server failures are logged safely and do not
   prevent local cleanup.
6. Release the Rust client handle, clear in-memory account/room/timeline/send
   state, clear only that MXID's SettingsManager/SecretStore session, and
   remove only `<account>/matrix-rust-sdk-store/`,
   `<account>/matrix-rust-sdk-smoke-session.json`, and its `.tmp` form.
7. Return to Login with a clean footer. Partial local cleanup exposes the
   reset action and a safe failure message.

`matrix::app_data::resolveAccountIdentity()` is now the canonical derivation
for full MXIDs and localparts plus homeserver. It normalizes whitespace,
scheme/host casing, and trailing slashes; rejects malformed/traversal input;
and validates every deletion target beneath one direct account child of the
application-data root. `CacheStore` also uses `AppDataPaths::accountRoot()`;
QML never builds a slug or filesystem path.

Password login refuses to open any existing account store because password
login can create a new Matrix device; an existing store must instead be
opened by a complete saved session/device restore. Missing metadata, missing
device id, another account's metadata, missing store during restore, and the
SDK's narrow ownership-mismatch error all produce the same reset-required
state. Unrelated password/network failures remain ordinary login failures.

The Login-screen reset passes its current homeserver and user fields into
C++, works while signed out, is idempotent, and leaves both fields populated.
It preserves `cache.sqlite`, unrelated account-local files, other Lightning
accounts, Element data, and server messages. A fresh password login may create
a new Matrix device, so Secure Backup recovery and/or SAS verification may be
needed again. Recovery can restore only backed-up Megolm room keys. Stale
callbacks are ignored, and decrypted encrypted-room plaintext remains
memory-only because `CacheStore` still refuses encrypted events.

Automated coverage is in `tests/AppDataPathsTest.cpp`,
`tests/RustSessionLifecycleTest.cpp`, and `tests/SettingsSessionTest.cpp`.

---

# Current state (v0.5.4 — 3-column navigation + room grouping)

## v0.5.4 — 3-column navigation layout and room grouping

Third slice of the UI redesign. Converts the sidebar into a proper
3-column layout (Spaces rail + Rooms column + Chat area), splits the
room list into "DIRECT MESSAGES" and "ROOMS" sections, softens the
light palette, moves Settings/Sign-out to a user footer in the sidebar,
and cleans up the app header.

Concrete changes:

- **`qml/SpacesRail.qml`** (new, replaces SpacesPanel): narrow 56 px
  fixed-width rail. Each row is a 40×40 circle avatar (pill when
  inactive → rounded-square when active, animated). A 3 px left-edge
  accent bar marks the active space. Unread count badge on inactive
  items. ToolTip shows full space name on hover. Pseudo-rows (All rooms,
  Other rooms) render ⊞/◦ symbols. Hidden entirely when there are no
  real Matrix Spaces (`app.spaces.hasSpaces` = false). **Superseded in
  v0.5.8:** Home is now always visible.

- **`qml/RoomsPanel.qml`** (rewritten): search bar at the top;
  `ListView` with `section.property: "category"` that shows
  "DIRECT MESSAGES" and "ROOMS" section headers driven by the new
  `CategoryRole` from `RoomListModel`; `RoomDelegate` delegates with
  height-collapse filter; user footer at the bottom (accent avatar circle
  with first letter of MXID, truncated user ID label, ⚙ Settings
  ToolButton, ↪ Sign-out ToolButton) — replaces the old bottom-left gear.

- **`qml/MainScreen.qml`** (rewritten): `SplitView` with 3 columns:
  `SpacesRail` (56 px fixed), `RoomsPanel` (200–360 px), `TimelinePane`
  (fills remainder). SpacesRail collapses automatically when not visible.

- **`src/models/RoomListModel.h/.cpp`**: two new roles —
  `MemberCountRole` ("memberCount", returns `r.members.size()`) and
  `CategoryRole` (historically used a member-count heuristic).
  **Superseded in v0.5.8:** `m.direct` is authoritative and member count
  is display-only. `refresh()` now `std::stable_sort`s the visible
  rooms so DMs appear before groups (required for Qt's section grouping
  to produce two contiguous sections rather than interleaving).

- **`qml/AppTheme.qml`**: softer light palette —
  background #EBF0F7, sidebar #F4F8FC, cardElevated #E8EEF7,
  hover #DCE8FF, selected #BEDBFF, border #C4D2E7.

- **`qml/Main.qml`**: header simplified to branding label only —
  Rooms/Settings/Sign-out ToolButtons removed (now in sidebar footer).

- **`qml/SettingsScreen.qml`**: Connection and Appearance split into
  separate panes; Sign out button added to the footer row.

- **`CMakeLists.txt`**: version 0.5.4; SpacesRail.qml added;
  SpacesPanel.qml and RoomListPane.qml removed from QML_FILES.

All E2EE / SAS / recovery / backend behaviour unchanged.

---

## v0.5.3 — split navigation sidebar foundation

Second slice of the UI redesign. Replaces the single
`RoomListPane` + horizontal chip strip with a proper split left
sidebar that mirrors the three-panel layout of Discord, Slack,
Element X, and Telegram Desktop.

Concrete changes:

- **`qml/SpacesPanel.qml`** (new): top section of the sidebar.
  Header "SPACES" with a collapse toggle (▼/▶). Search field
  ("Search spaces") filters real Spaces case-insensitively; the
  "All rooms" and "Other rooms" pseudo-rows always pass the
  filter. First-letter avatar rectangle for each real Space.
  Selecting a row still calls `app.spaces.activeSpaceId = ...`
  which triggers the existing `RoomListModel` filter downstream.
  When no real Matrix Spaces exist, shows a compact "No spaces"
  label instead of search + list. `collapsed: false` default;
  toggling hides search + list and shrinks the panel to
  header-height only so `RoomsPanel` absorbs the freed space.
  Uses `AppTheme.sidebar/hover/selected/selectedText/accent/
  cardElevated/textPrimary/textMuted/textSecondary/fontSizeXS/S/
  spacing4/8/12/radiusSm` tokens.

- **`qml/RoomsPanel.qml`** (new): bottom section of the sidebar.
  Header "ROOMS" with live room count. Search field ("Search
  rooms") filters the visible delegates case-insensitively by
  display name without mutating the backend `RoomListModel`.
  Filtered delegates collapse to `height: 0` so the ListView
  layout remains continuous. `RoomDelegate` is reused for each
  row — encrypted lock icon, first-letter avatar, unread badge,
  last message preview, and selected highlight all preserved.
  `app.openRoom(roomId)` is still the click target.
  Independent `ScrollBar.vertical` so rooms scroll without moving
  the Spaces list.

- **`qml/MainScreen.qml`** (rewritten): outer `SplitView`
  unchanged (horizontal, sidebar left / chat right). The old
  `RoomListPane` reference is replaced with a sidebar
  `Rectangle` (min 240 px, preferred 300 px, max 360 px)
  containing a `ColumnLayout`:
  - `SpacesPanel` (no `Layout.fillHeight` — height from
    `implicitHeight`, capped at `min(280, sidebar.height × 0.42)`)
  - 1 px separator (hidden when no Spaces)
  - `RoomsPanel` (`Layout.fillHeight: true`)
  - Bottom footer — a `ToolButton` with `⚙` text, tooltip
    "Settings", opens `app.showSettings()`.
  `TimelinePane` on the right is unchanged.

- **`qml/RoomListPane.qml`**: still registered in
  `APP_QML_FILES` and intact on disk; no longer referenced
  by `MainScreen`.

- **`CMakeLists.txt`**: `APP_VERSION_LABEL` → `"0.5.3"`.
  `SpacesPanel.qml` and `RoomsPanel.qml` added to
  `APP_QML_FILES`.

- **Top-right Settings link** in `qml/Main.qml` header: still
  present as a fallback. Removal is follow-up work.

Scroll independence:
  - `SpacesPanel` has its own `ListView` with `ScrollBar.vertical`.
  - `RoomsPanel` has its own `ListView` with `ScrollBar.vertical`.
  - The two lists scroll entirely independently.

Search behaviour:
  - Spaces search: in-delegate filter; `matchesFilter` on each
    delegate. `height: 0` when filtered, preventing gaps.
  - Rooms search: same in-delegate pattern on `RoomDelegate`
    instances. Backend model untouched.

Backend / E2EE behaviour — **unchanged**:
  - Rust backend connection, `supportsE2ee`, HTTP blocked sends.
  - SAS receive-first verification UI (`SettingsScreen.qml`).
  - Recovery-key restore flow.
  - Post-verification room reload.
  - Timeline reload after room open.
  - Store/device mismatch reset.
  - `CacheStore` encrypted plaintext guard.

Known limitations and follow-up work:
  - Top-right Settings button in the header not yet removed.
  - "No search results" feedback when a room search yields no
    visible delegates — the empty label shows only when the
    model is truly empty (follow-up).
  - `RoomListPane.qml` still registered in QML module (harmless
    dead file — can be removed in a cleanup pass).
  - Timeline, composer, and login form redesign are future passes.

# Current state (v0.5.2 — design-token foundation)

## v0.5.2 — design tokens + palette foundation

First slice of the UI redesign, deliberately scoped to design
tokens only. **Layout, sidebar structure, timeline, composer,
Settings placement, and login form are unchanged.** The larger
split Spaces + Rooms navigation and the bottom-left gear rework
are the next pass.

Concrete change:

- `qml/AppTheme.qml` rewritten around a semantic-token model.
  Consumers now reach `background`, `sidebar`, `surface`, `card`,
  `cardElevated`, `hover`, `selected`, `selectedText`, `accent`,
  `success`, `warning`, `danger`, `textPrimary`, `textSecondary`,
  `textMuted`, `border`, `separator`, `inputBackground`,
  `inputBorder`, `focusRing`, `ownMessageBubble`,
  `otherMessageBubble`, `undecryptableText`.
- Full spacing scale: `spacing2 / 4 / 6 / 8 / 12 / 16 / 20 / 24`.
- Radii: `radiusSm=4`, `radiusMd=8`, `radiusLg=12`, `radiusPill=999`.
- Typography: `fontSizeXS=11`, `fontSizeS=13` (up from 12),
  `fontSizeM=14`, `fontSizeRoom=16`, `fontSizeHeader=18`,
  `fontSizePageTitle=24`.
- Font stacks: `uiFontFamilies = [Inter, SF Pro Display, Segoe
  UI Variable, Segoe UI, system-ui, sans-serif]` and
  `monoFontFamilies = [JetBrains Mono, Fira Mono, SF Mono,
  Consolas, monospace]`. Consumers can now bind
  `Label { font.families: AppTheme.uiFontFamilies }` for real
  cross-platform fallback.
- Palette values updated in place to the redesign spec:
  Light — background `#F6F8FC`, sidebar/card `#FFFFFF`,
    accent `#4F7CFF`, hover `#EDF3FF`, selection `#DCE8FF`,
    text `#1E293B / #64748B / #94A3B8`, border `#E2E8F0`.
  Dark — background `#0F172A`, sidebar `#111827`, card `#1E293B`,
    accent `#4F7CFF`, hover `#243B6B`, selection `#2D4FA8`,
    text `#F8FAFC / #CBD5E1 / #94A3B8`, border `#334155`.
- Legacy aliases (`text`, `textMuted`, `surfaceAlt`,
  `spacingXS..XL`, `radius`, `fontSizeL/XL`, `error`, `ownBubble`,
  `otherBubble`, `selectedBg`, `muted`) all preserved so every
  existing QML file compiles and renders unchanged.
- Selected-item contrast rule enforced at token level:
  `selectedText` is near-white in dark mode (`#F8FAFC`) and the
  main dark ink in light mode (`#1E293B`); no consumer needs to
  branch on theme when they're already using `selectedText`.

Accessibility notes:

- Light: `#1E293B` on `#F6F8FC` primary text ≈ 13.6 : 1 contrast.
- Light: `#64748B` secondary on `#F6F8FC` ≈ 5.1 : 1.
- Dark: `#F8FAFC` on `#0F172A` ≈ 15.9 : 1.
- Dark: `#CBD5E1` secondary on `#0F172A` ≈ 10.9 : 1.
- Dark: `selectedText` `#F8FAFC` on `selected` `#2D4FA8` ≈ 7.1 : 1
  (fixes the previously-flagged grey-on-blue readability risk).
- `focusRing = accent = #4F7CFF` is visible on both `#F6F8FC` and
  `#0F172A`.

No backend / Matrix protocol / Rust FFI / E2EE / SAS / recovery
change. `CryptoManager::supportsE2ee()` still true for Rust only.
`CacheStore` still refuses encrypted `TimelineEvent` rows.

# Current state (v0.5.1 — post-verification retry decryption)

## v0.5.1 — retry decryption after verification

Reported after v0.5.0: even after Element Classic marked the
Lightning session verified, historical `[unable to decrypt yet]`
placeholders in the Lightning timeline remained.

Research pass in this cycle:

- Read `~/.cargo/registry/src/index.crates.io-1949cf8c6b5b557f/matrix-sdk-0.18.0/src/event_cache/redecryptor.rs`.
- matrix-sdk 0.18 has NO public per-event "request room key" or
  "retry decryption" method on `Client`. The
  `event_cache/redecryptor.rs` module runs internally and
  re-decrypts as room keys arrive on the sync loop.
- Related lower-level APIs exist on `OlmMachine`
  (`query_missing_secrets_from_other_sessions`,
  `get_missing_sessions`) but are not exposed at the public
  `Client` surface in 0.18.
- Practical action from the public API surface: reload the room
  via `Room::messages` after key material has (hopefully) arrived.
  matrix-sdk will attempt decryption again on the returned events.

Concrete change:

- `AppController::verificationDone` handler (bridged from
  `RustSdkMatrixClient::verificationDone`) now calls
  `reloadCurrentRoomTimeline(50)` when a room is open. Idempotent
  by `event_id` in `handleTimelineEvent`.
- Settings verification card status message updated: after Done
  the card now says *"Verification complete. Refreshing current
  room… Some old messages may remain undecryptable until another
  verified session shares their room keys."* — sets accurate
  expectations without pretending automatic decryption.
- `matrix.app: verification=done; reloading current room …` INFO
  log so a stalled retry is visible.
- Existing "Refresh current room" Settings button already handles
  the same reload; the post-verification path just triggers it
  automatically.

Retained invariants:

- `CryptoManager::supportsE2ee()` still true for Rust only.
- `RUST_SDK_E2EE_WIRED` unchanged.
- `CacheStore` still refuses encrypted `TimelineEvent` rows —
  decrypted plaintext stays memory-only.
- Passwords / access tokens / recovery keys / crypto keys /
  decrypted bodies never logged. Room ids appear in the reload
  log at INFO with only the last 12 characters shown.

What still remains undecryptable, and why:

- Events sent to the room BEFORE any of your currently-verified
  devices joined and received the associated Megolm session key
  simply cannot be recovered from client-side APIs. matrix-sdk
  0.18's public surface has no "please share key X from device Y"
  action; that gossip happens automatically at the lower level
  when both devices trust each other and are online at the same
  time. If the sending device is offline / has since forgotten
  the key / never shared it forward, the event will stay
  `[unable to decrypt yet]` forever unless recovered from the
  backup you already imported via `Restore keys`.

# Current state (v0.5.0 — SAS verification receive-first)

**The `prep+N` nomenclature is retired.** The prep series ended at
`0.5.0-prep+13`. Active line is now `0.5.x`; the next bug-fix pass
should use `0.5.1`.

## v0.5.0 — Matrix SAS emoji verification (receive-first)

Landed the SAS state machine using the source-verified matrix-sdk
0.18 API from prep+13's research pass.

Rust FFI additions (`rust/src/lib.rs` / `rust/include/matrix_rust.h`):

- Event handler for `ToDeviceKeyVerificationRequestEvent` installed
  during `install_event_handlers`. When a request arrives, the
  handler hydrates the request via
  `client.encryption().get_verification_request(user, flow_id)` and
  stores it in `RustClient::active_request` (single-flow policy —
  matches the "one dialog at a time" GUI intent). Emits
  `verification_request_received` on the FFI queue.
- `mx_rust_accept_verification(flow_id)` — drives
  `request.accept()`, polls for the SDK to transition into
  `Verification::SasV1(...)` via `get_verification`, calls
  `sas.accept()`, then polls `sas.state()` at 500 ms cadence for up
  to 120 s. Emits `verification_sas_ready` with the seven emojis +
  three-decimal fallback when `SasState::KeysExchanged` is reached,
  `verification_done` on `SasState::Done`, or
  `verification_cancelled` on `SasState::Cancelled`. Poll-based on
  purpose so we don't have to introduce a futures-util dependency.
- `mx_rust_confirm_verification(flow_id)` → `sas.confirm().await`.
- `mx_rust_mismatch_verification(flow_id)` → `sas.mismatch().await`.
- `mx_rust_cancel_verification(flow_id)` — SAS-level cancel if a
  SAS is active, else request-level cancel. Clears both slots.

C++ wrapper (`src/matrix/RustSdkMatrixClient.{h,cpp}`):

- New methods: `acceptVerification`, `confirmVerification`,
  `mismatchVerification`, `cancelVerification`.
- New signals: `verificationRequestReceived(flowId, otherUser,
  otherDevice, isSelfVerification)`, `verificationSasReady(flowId,
  emojis, decimals)`, `verificationDone(flowId)`,
  `verificationCancelled(flowId, message)`,
  `verificationFailed(flowId, message)`.
- Rust `verification_*` JSON queue events dispatched into those
  signals in `handleRustEvent`.

AppController (`src/app/AppController.{h,cpp}`):

- New properties (all `NOTIFY verificationStateChanged`):
  `verificationActive`, `verificationFlowId`,
  `verificationOtherUser`, `verificationOtherDevice`,
  `verificationIsSelfVerification`, `verificationState`,
  `verificationEmojis` (QVariantList of {symbol, description}),
  `verificationDecimals` (QVariantList of int).
- New invocables: `acceptVerification()`, `confirmVerification()`,
  `mismatchVerification()`, `cancelVerification()` — all no-op on
  non-Rust backends.
- Bridges `RustSdkMatrixClient::verification*` signals into
  `verificationStateChanged`; caches the state so QML doesn't
  need to hold onto anything.
- `cancelVerification()` clears the local state after the FFI
  call so the dialog dismisses cleanly.

QML (`qml/SettingsScreen.qml`):

- New verification card inside the Encryption pane, visible only
  while `app.verificationActive`. Shows the incoming user,
  contextual status text, a Flow of seven emoji tiles with
  descriptions when `verificationState === "sas_ready"`, and
  action buttons (`Accept`, `They match`, `They do not match`,
  `Cancel`, `Dismiss`) whose visibility follows the state.
- The pane's static status line switches from "not implemented
  yet" to "Session (SAS emoji) verification: receive-first flow
  implemented".

### matrix-sdk 0.18 quirks worked around

- `SasVerification` does NOT expose `flow_id()` on 0.18. We track
  the flow id externally in `active_sas: Mutex<Option<(String,
  SasVerification)>>`.
- No public `recv_verification_requests()` stream on 0.18 — the
  install_event_handlers path uses `add_event_handler` for
  `ToDeviceKeyVerificationRequestEvent` (research pass confirmed
  this in prep+13).
- No `futures-util` direct dep added; the SAS state watcher polls
  `sas.state()` at 500 ms cadence instead of consuming the
  `.changes()` Stream.

### Current verification limitations

- **Receive-first only.** Lightning can accept a SAS request that
  Element Classic (or any other client) initiates. Initiating a
  verification from Lightning is a follow-up prompt.
- Single active flow at a time (a second incoming request while
  one is in progress overwrites the slot).
- No in-room verification path — only to-device requests are
  observed.
- Cross-signing UI: not implemented.
- Key backup management UI: not implemented.

### Cache / security invariants (preserved)

- `CryptoManager::supportsE2ee()` still returns `true` for Rust
  only; `RUST_SDK_E2EE_WIRED` still defined only under
  `ENABLE_RUST_SDK_BACKEND`.
- `CacheStore` still refuses encrypted `TimelineEvent` rows.
- Emojis + descriptions are safe to display by SAS design. Flow
  ids, user ids, and device ids appear in logs at INFO; passwords
  / access tokens / recovery keys / crypto keys / decrypted
  message bodies never appear anywhere in logs.

# Current state (v0.5.0-prep+13, SAS API surface research)

## v0.5.0-prep+13 — SAS emoji verification API research

Concrete SAS implementation was NOT attempted this pass. Rather
than land half-wired scaffolding that could leave a real
verification request stranded on the server, this pass records
the exact locked matrix-sdk 0.18 API surface for the next agent
to implement cleanly, and updates the Settings wording to remain
honest ("Session (SAS emoji) verification UI: not implemented yet").

The full API reference — including which methods on
`VerificationRequest` vs `SasVerification` do what, the concrete
enum variants of `VerificationRequestState` and `SasState`, and
the fact that matrix-sdk 0.18 has **no** public
`recv_verification_requests()` (event-handler subscription only)
— is documented in `docs/next-prompts.md` under
"Prompt — Implement Matrix SAS emoji verification UI".

Key finding that would have broken a rushed implementation:
`Client::add_event_handler` for the appropriate `to_device` /
`OriginalSyncKeyVerificationRequestEvent` type is the canonical way
to notice incoming requests on 0.18; polling / `recv_verification_requests`
does not exist here. Once received, the handler must call
`client.encryption().get_verification_request(user, flow_id)` to
hydrate the `VerificationRequest` and drive it.

**No code changes shipped in this pass** beyond
`CMakeLists.txt APP_VERSION_LABEL` bump to `0.5.0-prep+13` and
this section + the enhanced next-prompt. All invariants from
prep+12 preserved: `CryptoManager::supportsE2ee()` still `true`
for Rust only, `CacheStore` still refuses encrypted rows, GUI
recovery-key restore + timeline reload + store mismatch reset all
work.

# Current state (v0.5.0-prep+11, timeline reload + store mismatch guard)

## v0.5.0-prep+11 additions

Two reported bugs land in one pass:

### Bug 1: encrypted rooms empty after restart

`CacheStore` refuses encrypted `TimelineEvent` rows (E2E plaintext
never persisted), so after quitting the Rust GUI the timeline was
empty until new live events arrived. Fix:

- New Rust FFI `mx_rust_reload_room_timeline(room_id, limit)`
  calls matrix-sdk 0.18 `Room::messages(MessagesOptions::backward())`
  and re-emits each event through the same `timeline_event`
  shape live sync uses. `handleTimelineEvent` dedupes by
  `event_id`, so double-emission is harmless. Body plaintext is
  only forwarded when the SDK actually decrypted the event;
  ciphertext is never forwarded (undecryptable rows get empty
  body + `undecryptable=true`).
- `RustSdkMatrixClient::reloadRoomTimeline(roomId, limit=30)`
  wraps the FFI + emits `roomTimelineReloaded(roomId, total,
  decrypted, undecryptable)` when the SDK finishes.
- `AppController::openRoom` now calls `reloadCurrentRoomTimeline(30)`
  automatically on the Rust backend when a room is selected.
- After `keyBackupResult("ok", ...)` (successful recovery-key
  restore), AppController also triggers a current-room reload so
  previously undecryptable events get another chance to decrypt.
- New Q_INVOKABLE `reloadCurrentRoomTimeline(limit)` + signal
  `currentRoomTimelineReloaded(t, d, u)` for future QML wiring.
- Settings pane adds a **Refresh current room** button and the
  Rust bridge logs `matrix.rust: reload_timeline start room=…
  limit=…` at INFO. No bodies, no keys.
- `CacheStore` guard preserved — decrypted E2E plaintext still
  memory-only.

### Bug 2: store/device mismatch on fresh login

If the on-disk Rust SDK store belongs to a different device (e.g.
the user deleted their session server-side then password-logs-in
fresh), matrix-sdk rejects the login with
`"the account in the store doesn't match the account in the
constructor"`. Fix:

- `AppController::onLoginFailed` handler now watches for that
  specific reason string and emits new signal
  `storeDeviceMismatchDetected(displayMessage)`.
- New `Q_INVOKABLE resetLocalRustStore()` on AppController
  computes the account's paths via `matrix::app_data::rustSdkStorePath`
  + `rustSdkSmokeSessionPath`, stops sync, calls `logout()`,
  and removes only those paths. Emits
  `localRustStoreResetResult(ok, message)`. Never touches other
  accounts, other backends, `cache.sqlite`, SecretStore, or
  server data. Safe INFO log: `matrix.app: reset_local_rust_store
  deleted=… failed=…` — paths only.
- `qml/LoginScreen.qml` binds `Connections { target: app;
  onStoreDeviceMismatchDetected() }` to reveal a red banner + a
  "Reset local Lightning session" button + a result label. The
  banner hides itself after `localRustStoreResetResult(ok=true)`.
- `qml/SettingsScreen.qml` gets an equivalent "Reset local
  Lightning session" button in the Encryption pane, guarded by a
  confirmation Dialog.
- `--reset-crypto-store` CLI (prep+5 layout) still works and
  still scans both roots.

### Not changed

- `CryptoManager::supportsE2ee()` returns `true` for Rust only.
- `RUST_SDK_E2EE_WIRED` still defined only under
  `ENABLE_RUST_SDK_BACKEND`.
- `CacheStore` still refuses encrypted rows.
- HTTP / Mock backends untouched.
- Smoke harness / persistent-store / recovery-key / probe FFI /
  encrypted-send / expect-text wait / shutdown-leak all
  preserved.

### Known limitations

- No SAS emoji verification UI yet (see next-prompts.md).
- Reload uses `Room::messages` (server round-trip). A pure
  local-cache read on top of matrix-sdk's event cache is a
  possible future optimisation.
- No incremental pagination beyond the single reload window;
  `loadOlderMessages` is still a no-op on the Rust backend.
- Interactive GUI shutdown still uses `mx_rust_destroy` — the
  deadpool-sqlite drop path from prep+8 remains a follow-up.

# Current state (v0.5.0-prep+10, GUI recovery + honest E2EE settings)

## v0.5.0-prep+10 — GUI E2EE controls

Reported symptoms after prep+9 landed:
- Rust GUI worked for sending, but the footer displayed
  "HTTP backend • Connected" even though the app was launched
  with `--backend=rust`.
- Timeline still showed many `[unable to decrypt yet]`
  placeholders for messages sent before the current Lightning
  device was created — expected, because the GUI had no way to
  restore the recovery key.
- No visible option in the GUI to verify the Lightning session or
  paste a recovery key.

Fixes in this pass:

- **Footer label.** `qml/Main.qml` now picks per backend:
  `Matrix Rust SDK • <status>`, `Mock backend • <status>`, or
  `HTTP backend • <status>`.
- **Settings E2EE panel.** New `Pane` in `qml/SettingsScreen.qml`,
  visible only when `app.backendName === "rust"`. Shows the
  redacted device id, a status line calling out what is / isn't
  implemented ("initial verified" for send + receive, "not
  implemented yet" for SAS emoji UI and cross-signing UI), and a
  recovery-key entry.
- **Recovery-key restore.** `AppController::requestRecoverFromBackup(QString)`
  invocable routes into `RustSdkMatrixClient::recoverFromBackup`.
  Recovery key TextField is `Password`-echo, wiped the moment the
  button is pressed, never held in a QML property beyond the
  invocation. Status flows back through
  `AppController::recoveryStateChanged(state, message)` which the
  Settings panel binds to via `Connections { target: app; … }`.
  States: `attempted` (button disables, shows "Recovery started"),
  `ok` (green "Recovery complete …" text), `failed` (red
  "Recovery failed: <safe reason>").
- **Local reset.** GUI reset button deliberately NOT implemented
  in this pass; the Settings panel points users at
  `matrix-client --reset-crypto-store` on the CLI, which already
  scans the correct roots (v0.5.0-prep+5). A dedicated GUI reset
  is a later step so we don't build a half-safe destroy path.
- **Undecryptable hint.** Settings panel now includes an inline
  note: "Some old messages may show '[unable to decrypt yet]'
  until you restore your recovery key here, or until another
  verified device shares the room keys." Placeholder rendering in
  the timeline is unchanged.

Not changed and preserved:
- `CryptoManager::supportsE2ee()` still returns `true` for Rust
  only. `RUST_SDK_E2EE_WIRED` still defined only under
  `ENABLE_RUST_SDK_BACKEND`.
- `CacheStore` still refuses encrypted `TimelineEvent` rows.
- Encrypted send/receive still route through matrix-sdk. C++
  never sees ciphertext or keys. Recovery key never logged.
- Smoke harness / persistent-store mode / `--reset-crypto-store`
  / encrypted-send probe FFI all untouched.

Known limitations (still):
- No SAS emoji verification UI (Settings panel says so).
- No GUI reset button (CLI works).
- No cross-signing management UI.
- No "Copy device ID" button (device id is displayed as
  redacted only; full id not yet exposed to QML).
- Interactive GUI shutdown still uses `mx_rust_destroy` — the
  deadpool-sqlite drop path from prep+8 is a follow-up.

# Current state (v0.5.0-prep+9, initial E2EE support enabled for Rust backend)

## v0.5.0-prep+9 — initial E2EE gate open

Both directions of the Rust SDK E2EE path have been verified live
against `matrix.smetonis.net`:

- **Element Classic → Lightning encrypted receive** (from the last
  smoke run):

  ```
  smoke: first_timeline_after_expect=yes
  smoke: expect_text=seen
  smoke: summary ...
         timeline_events_since_expect=1
         encrypted_events_since_expect=1
         decrypted_events_since_expect=1
         undecryptable_since_expect=0
         first_timeline_after_expect=yes
         supports_e2ee=false
  exit=0
  ```

- **Lightning → Element Classic encrypted send** (from the earlier
  prep+6 verification): `encrypted_send=ok marker=SMK-… event_id=$…`
  and Element Classic displayed the Lightning encrypted-send probe
  as readable text.

Concrete changes flipping the gate this pass:

- `CMakeLists.txt` defines `RUST_SDK_E2EE_WIRED=1` inside the
  `ENABLE_RUST_SDK_BACKEND` branch. HTTP and Mock builds do NOT
  define it.
- `CryptoManager::supportsE2ee()` returns `true` for the Rust
  backend only (both compile-time defines set AND the active
  backend name is `"rust"`).
- Rust FFI `mx_rust_supports_e2ee` returns `1`.
- Rust `mx_rust_send_text` no longer refuses encrypted rooms.
  matrix-sdk auto-encrypts via its `e2e-encryption + sqlite`
  features. C++ still gates the UI via
  `RustSdkMatrixClient::sendTextMessage`, which now passes the send
  through because `rustSupportsE2ee()` is true.
- `CryptoManager` status text / description updated to speak
  honestly: "Initial E2EE support (v0.5.0-prep+9): encrypted send
  + receive verified against Element Classic. SAS emoji UI, GUI
  recovery-key flow, cross-signing, and key backup management are
  not implemented yet."

`CacheStore` unchanged: encrypted `TimelineEvent` rows are still
refused, so decrypted encrypted-room plaintext remains memory-only.
`--reset-crypto-store`, persistent-store smoke mode, recovery-key
env var, and the encrypted-send probe are all preserved.

Known limitations (documented for honesty):
- No SAS emoji verification UI. Device verification currently
  happens externally through Element Classic ("Yes, it was me" +
  cross-signing propagation).
- No GUI recovery-key flow. Recovery is only exercised via
  `LIGHTNING_TEST_RECOVERY_KEY` in the smoke harness.
- No cross-signing management UI.
- No key backup management UI.
- Interactive GUI shutdown still uses the deadpool-sqlite drop
  path (only the smoke harness leaks). A clean GUI shutdown
  redesign is a follow-up.

# Current state (v0.5.0-prep+8, receive smoke reliability)

## v0.5.0-prep+8 additions

Fixes uncovered by the first live `EXPECT_TEXT` run against the
persistent store:

- **Dynamic global budget.** The old hard 60 s kill silently
  overrode `LIGHTNING_TEST_EXPECT_WAIT_SECONDS=180`, so the marker
  test never actually waited 3 minutes. Budget now scales:
  `max(60, expect_wait + 30)` when a marker is set, plus another
  30 s of headroom when a recovery key is set. Clamped to
  `[1, 3660]`. Printed once at startup as `smoke: budget timeout_s=N`,
  and the timeout emits `smoke: budget=exhausted timeout_s=N` before
  finalising cleanly.
- **Clean process exit / no deadpool panic.** matrix-sdk 0.18 uses
  `deadpool-sqlite` internally, whose async-drop paths require a
  live Tokio runtime when a `Client` is dropped. The smoke harness
  used per-call `current_thread` runtimes that were long gone by
  the time C++ tore down the wrapper — dropping the SDK Client from
  the main thread panicked with `there is no reactor running`
  AFTER the summary line had already been emitted. Fix: after
  `QCoreApplication::exec()` returns, the smoke harness now stops
  sync, sleeps 300 ms, prints `shutdown=leaked_for_process_exit`,
  and releases the unique_ptr without destroying it. The OS
  reclaims memory / FDs at process exit; no destructor path runs.
  Only smoke leaks — the interactive GUI still calls
  `mx_rust_destroy` normally (its own clean-shutdown story is
  documented as a known follow-up).
- **No more room-list spam.** The Rust SDK's sync callback fires
  many `rooms` events during a long wait; the harness now only
  prints `rooms joined=N encrypted=M spaces=S` when the counts
  actually change.
- **Duplicate `key_backup=attempted` gone.** The C++ side now
  relies on the `keyBackupResult` event handler to print the state
  (Rust bridge already emits `state=attempted` first, then `ok`/
  `failed`).
- **Long-wait heartbeat + since-counters.** During
  `expect_text=waiting`, a heartbeat every 30 s prints
  `sync=alive elapsed_s=N` plus the running
  `timeline_events_since_expect` / `encrypted_events_since_expect` /
  `decrypted_events_since_expect` / `undecryptable_since_expect`.
  New `timeline_events_since_expect` counter joins the existing
  three in the summary. `first_timeline_after_expect=yes` is
  printed the moment the first timeline event arrives during the
  wait.

## matrix-sdk 0.18 research findings (do not remove)

Confirmed by reading the locked source at
`~/.cargo/registry/src/index.crates.io-1949cf8c6b5b557f/matrix-sdk-0.18.0/`:

- `client.encryption().recovery().recover(key)` does exactly
  `secret_storage().open_secret_store(key)` → `import_secrets()` →
  `update_recovery_state()`. It imports **secrets** (including the
  backup recovery key + cross-signing seeds if present) into the
  local store. It does **not** synchronously download or import
  room keys.
- Room keys are then fetched **lazily** as encrypted events
  arrive on `/sync` — matrix-sdk detects a missing session and
  requests it from server-side backup / from other devices in
  the background. The smoke harness must therefore keep sync
  running long enough for that dance to complete.
- `client.encryption().backups().wait_for_steady_state()` is for
  **upload** progress after enabling backup, not for download.
  No public 0.18 API waits for room-key downloads to finish.
- `client.encryption().recovery().state()` returns a
  `RecoveryState` (Unknown / Disabled / Enabled / Incomplete).
  Useful to log post-recover, TODO in a future pass.
- No `recover_and_fix_backup` in 0.18 — that's newer. Available
  API used here is just `recover(&str)`.

Practical implication for `LIGHTNING_TEST_EXPECT_TEXT`: sending
the marker from Element **after** the Lightning smoke run has
called `recover()` is required. If the marker was sent long
before, the SDK will still try to fetch keys via backup + device
key requests, but only when a new sync response references those
events. Increase `LIGHTNING_TEST_EXPECT_WAIT_SECONDS` and/or send
a fresh marker after `key_backup=ok` prints.

## v0.5.0-prep+7 additions (retained)

# Current state (v0.5.0-prep+7, Rust SDK key backup probe + EXPECT_TEXT wait loop)

## v0.5.0-prep+7 additions

- New Rust FFI `mx_rust_recover_from_backup(recovery_key)` calling
  matrix-sdk 0.18 `client.encryption().recovery().recover(...)` to
  import backed-up room keys from server-side secret storage. FFI
  never logs the key or the imported material. Result flows through
  `key_backup_status` events on the poll queue.
- New C++ `RustSdkMatrixClient::recoverFromBackup(recoveryKey)` +
  signal `keyBackupResult(state, message)`.
- Smoke harness reads `LIGHTNING_TEST_RECOVERY_KEY` (base58 recovery
  key) after `initial_sync=done` and only when
  `LIGHTNING_TEST_PERSISTENT_STORE=1`. `LIGHTNING_TEST_RECOVERY_PASSPHRASE`
  is reserved but reports `passphrase_not_supported` — matrix-sdk
  0.18's fast-path recovery API takes a key.
- Smoke harness EXPECT_TEXT no longer finalises the moment
  `initial_sync=done` fires. When a marker is configured, the
  harness enters a bounded wait phase after any send/probe step
  finishes. Default 90 s, override via
  `LIGHTNING_TEST_EXPECT_WAIT_SECONDS`. New "since expect" counters
  (`encrypted_events_since_expect`, `decrypted_events_since_expect`,
  `undecryptable_since_expect`, `first_timeline_after_expect`) tell
  you whether new events arrived during the wait.
- SAS verification NOT implemented in this pass. Attempting it
  headlessly against matrix-sdk 0.18's async verification handshake
  risks unfinished state that the operator can't easily undo, so it
  is deferred to a session with a full token budget and interactive
  Element Classic driving.
- `CryptoManager::supportsE2ee()` unchanged: still `false`.
  `RUST_SDK_E2EE_WIRED` still undefined. Interactive UI encrypted
  sends remain blocked. `CacheStore` still refuses encrypted rows.


Last updated: 2026-07-05 (Rust SDK backend live-verified against
matrix.smetonis.net; encrypted-send probe verified one-way in Element
Classic; persistent Rust SDK smoke store/session support added so
encrypted receive can be tested with a stable device).

## Live verification status (v0.5.0-prep+6)

The maintainer ran the headless smoke harness twice against
`@test:matrix.smetonis.net` after the v0.5.0-prep+5 store-isolation
fix landed. Both runs exited 0 from a fresh QTemporaryDir SDK store.

Run 1 (no send):

```
smoke: rooms joined=2 encrypted=2 spaces=1
smoke: initial_sync=done
smoke: summary login=ok sync=ok rooms=2 encrypted_rooms=2 spaces=1
       timeline_events=4 undecryptable=4 send=n/a supports_e2ee=false
```

Run 2 (LIGHTNING_TEST_SEND=1):

```
smoke: send=skipped reason=no_unencrypted_room
smoke: summary … send=skipped(no_unencrypted_room) …
```

**What this proves:**

- Rust backend live login works (via matrix-sdk password login).
- Rust backend live joined-room sync works.
- Room list delivery works (2 rooms).
- Space detection works (1 Space).
- Timeline event delivery works (4 events observed).
- All observed events on this account are encrypted → the
  encrypted-timeline dispatch path is exercised.
- No decryption is possible from a fresh temp store — expected
  behaviour, not a bug.
- The plain-text send path safely skips when no unencrypted room
  exists.
- Smoke store isolation holds across back-to-back runs (no crypto
  store account/device mismatch).

Additional verified smoke result after prep+6:

```
smoke: encrypted_send=ok marker=SMK-1783280632 event_id=$NIP0ZhOlSs-NMUudtW_a3m45JmkxOeQZcsDks-mW3jQ
smoke: summary login=ok sync=ok rooms=2 encrypted_rooms=2 spaces=1
       timeline_events=4 encrypted_events=4 decrypted_events=0
       undecryptable=4 send=n/a encrypted_send=ok expect_text=n/a
       supports_e2ee=false
```

The maintainer confirmed in Element Classic that the Lightning
encrypted-send probe appeared as normal readable text. This proves
Lightning → Element Classic encrypted send one-way through matrix-sdk.

**What this does NOT prove yet:**

- Element Classic → Lightning encrypted receive. A fresh temp store
  still reports `expect_text=not_seen`, `decrypted_events=0`, and
  exits 14 when `LIGHTNING_TEST_REQUIRE_EXPECT=1`.
- Full E2EE support. Interactive UI encrypted sends remain blocked,
  SAS verification, key backup, and cross-signing are missing, and
  `RUST_SDK_E2EE_WIRED` remains undefined.

`CryptoManager::supportsE2ee()` remains **false**. Flipping it
requires Element Classic → Lightning `expect_text=seen` on a real
marker and Lightning → Element `encrypted_send=ok`. The second is now
verified one-way; the first is still pending.

This is the "where the repo actually is" doc. Treat it as ground truth
for a fresh LLM continuation session — read this before
`docs/matrix-feature-status.md`, before `README.md`, before touching
code.

## Repository

- Origin: `https://gitlab.smetonis.net/Mizerd/lightning.git`
- Local path this doc was written from: `/home/roksme/git/lightning/`
- Branch: `main`
- Most recent commits (newest last):
  - `c522f5d` Codex setup
  - `4310913` Fix Nix dev shell: drop stale qtquickcontrols2 attr, add .gitignore
  - `d251948` v0.4: SecretStore + backend CLI cleanup + Rust SDK backend scaffold
  - `13adf73` v0.4.1: Spaces + Threads foundations, SSO/OIDC flags, continuation docs
  - `1a5adba` v0.4.2: HTTP Spaces parsing + no-display preflight hardening
  - `2230bbe` v0.4.3: Nix Qt platform runtime fix + --http/--rust CLI hint
  - `da9f331` v0.4.4: real HTTP m.thread relation send + parse
  - `dbb28e0` v0.4.5: HTTP login transition fix + Lightning branding + Space/thread cache persistence
  - `31cbc22` v0.4.6: HTTP /sync bring-up polish + initialSyncDone capability + docs sweep
  - `50d4a6e` v0.4.7: HTTP restore recovers from stale Space-only cache
  - `41a9f69` v0.4.8: cache NOT NULL repair, delegate anchor warning, Connected status
  - `6f389aa` v0.5.0-prep: C++ groundwork for E2EE via matrix-sdk (crate not linked yet)
  - `9ade51b` v0.5.0-prep+1: --reset-crypto-store shows resolved paths; matrix-sdk still blocked at classifier layer
  - `8205606` rust: pull matrix-sdk deps for offline builds
  - `9eaa488` Wire Matrix Rust SDK backend foundation (Codex)
  - `4c9d4f5` Harden Matrix Rust SDK backend foundation (v0.5.0-prep+3)
  - `8d6f436` Harden Matrix Rust SDK backend testing (v0.5.0-prep+4)
  - `9bf3c83` Fix Rust SDK smoke store isolation (v0.5.0-prep+5)
  - HEAD after this pass: persistent Rust SDK smoke store for receive verification
  - Branch: `main`

## Layered architecture (unchanged from v0.4)

```
Qt/QML UI   →  qml/*.qml
App layer   →  src/app/{AppController,SettingsManager}
Auth        →  src/auth/{AuthManager,AccountManager}
UI models   →  src/models/{RoomListModel,TimelineModel,MessageComposer}
              src/spaces/SpaceManager        ← QAbstractListModel of Spaces
              src/threads/ThreadManager      ← thread aggregation helper
Backend iface: src/matrix/MatrixClient.h      ← the swap seam
Backends:     src/matrix/MockMatrixClient.{h,cpp}      --backend=mock
              src/matrix/CppHttpMatrixClient.{h,cpp}   --backend=http (default)
              src/matrix/RustSdkMatrixClient.{h,cpp}   --backend=rust
                                                       (only when compiled with
                                                        -DENABLE_RUST_SDK_BACKEND=ON)
Rust crate:  rust/                                    static lib + C ABI shim
Storage:     QSettings (prefs + non-secret session metadata)
             src/storage/SecretStore (LibSecret or InsecureFallback)
             src/storage/CacheStore  (SQLite: rooms/events/members)
Platform:    src/notifications/NotificationManager (stub)
             src/media/MediaManager (send/receive + open-external)
Crypto:      src/crypto/CryptoManager (capability surface only, no crypto)
```

## What is *implemented* right now

- **Product name**: **Lightning**. Window title / header / login-screen
  heading all say "Lightning". The executable is still `matrix-client`
  for build-system compatibility (Q_APPLICATION_NAME too — keeps the
  QSettings scope stable across the rename). The login-screen
  sub-heading is backend-aware (v0.4.5).
- **Persistent Rust SDK smoke store/session (this pass)**:
  `LIGHTNING_TEST_PERSISTENT_STORE=1` switches the headless Rust smoke
  harness from a fresh `QTemporaryDir` to the same account-specific
  Rust SDK store path used by the interactive Rust backend:
  `matrix::app_data::primaryRoot()/<safeUserId>/matrix-rust-sdk-store/`
  (normally
  `${XDG_DATA_HOME}/MatrixClient/matrix-client/<safeUserId>/matrix-rust-sdk-store/`).
  The default smoke mode is unchanged and still uses a temporary store.

  Persistent smoke mode configures a smoke-only MatrixSession sidecar:
  `matrix-rust-sdk-smoke-session.json` next to the account store. Rust
  writes it after password login and reads it through
  `mx_rust_restore_from_file` on the next run, so the SDK restores the
  same device without writing to the interactive QSettings/SecretStore
  session. The sidecar contains an access token, is 0600 on Unix, is
  never printed, and must not be committed.

  Smoke output now includes `restore=...`,
  `store_account_match=yes|no|unknown`, and a redacted `device_id`.
  If matrix-sdk reports the known "account in the store doesn't match
  the account in the constructor" error, the harness destroys the Rust
  handle, deletes only that account's `matrix-rust-sdk-store/` plus
  the smoke session sidecar, prints
  `store_reset=account_device_mismatch`, and retries password login
  once. It never deletes `cache.sqlite`, QSettings, or SecretStore
  entries.

  This implements the required workflow for encrypted receive
  verification:
  first persistent run creates/restores a stable Lightning SDK device;
  the user approves the new login in Element Classic if prompted; the
  user sends a harmless marker from Element Classic; the second
  persistent run uses `LIGHTNING_TEST_EXPECT_TEXT=<marker>` and
  `LIGHTNING_TEST_REQUIRE_EXPECT=1`. Success is `expect_text=seen`,
  `decrypted_events>0`, and exit 0. Until that happens, encrypted
  receive remains unverified.

  `CacheStore` now refuses to persist encrypted `TimelineEvent` rows:
  decrypted encrypted-room plaintext can be displayed in memory by the
  Rust backend, but is not written into `cache.sqlite` yet.
- **v0.5.0-prep+6 encrypted-receive diagnostics + encrypted-send
  probe (this pass)**: five connected changes wired end to end
  around real, safe E2EE plumbing — but no E2EE claim is made
  from the UI yet.

  1. `TimelineEvent` (`src/matrix/TimelineEvent.h`) gains
     `isEncrypted`, `isDecrypted`, `undecryptable`, `errorKind`.
     Defaults keep HTTP and Mock unchanged. All plumbing is
     metadata-only — the C++ layer never derives plaintext from
     these fields.
  2. `rust/src/lib.rs` `install_event_handlers` splits the two
     paths precisely:
     - `OriginalSyncRoomMessageEvent` (plaintext or SDK-decrypted)
       emits `is_encrypted = encryption_info.is_some()`,
       `is_decrypted = encryption_info.is_some()`,
       `undecryptable = false`.
     - `OriginalSyncRoomEncryptedEvent` (undecryptable) emits
       `is_encrypted = true, is_decrypted = false,
       undecryptable = true, error_kind = "no_key"`, empty body.
     The legacy `decrypted` field is still emitted for one
     release for backward compat.
  3. New Rust FFI `mx_rust_probe_encrypted_send` — mirror of
     `mx_rust_send_text` that ONLY accepts encrypted rooms
     (refuses non-encrypted with `encrypted_send_failed`).
     matrix-sdk does the encryption end-to-end via its
     `e2e-encryption + sqlite` features; the FFI never sees
     ciphertext, keys, or session material. On success the SDK
     returns a real server event id (safe to log).
  4. `RustSdkMatrixClient::probeEncryptedSend(room, body, marker)`
     wraps that FFI, tracks the txn id in a new `m_pendingProbes`
     map, and emits a new signal
     `encryptedSendProbeResult(room, marker, ok, serverEventId,
     message)`. Deliberately NOT wired into QML — the interactive
     UI send path stays gated on `CryptoManager::supportsE2ee()`.
     `handleEncryptedSendOk` / `handleEncryptedSendFailed` bridge
     the FFI events to that signal.
  5. Smoke harness (`src/smoke/RustSdkSmokeTest.cpp`) uses the new
     `TimelineEvent` flags to break `undecryptable` out of the
     total timeline event count and add `encrypted_events` and
     `decrypted_events`. Two new env vars land:
     - `LIGHTNING_TEST_EXPECT_TEXT` — a marker sent from Element
       Classic. The harness watches decrypted bodies for a match
       but never prints the marker itself; only `expect_text=seen`
       or `not_seen` is emitted. Combined with
       `LIGHTNING_TEST_REQUIRE_EXPECT=1` a missing marker becomes
       exit code 14.
     - `LIGHTNING_TEST_SEND_ENCRYPTED=1` — drives
       `probeEncryptedSend` against the first encrypted joined
       room (or `LIGHTNING_TEST_ROOM_ID`). Prints
       `encrypted_send=ok marker=<short-id> event_id=<id>` on
       success. Real send failures / timeouts return exit 15;
       "no encrypted room" and "target not encrypted" are
       `skipped` and remain exit 0.
     The summary line now includes `encrypted_events=N`,
     `decrypted_events=D`, `encrypted_send=<status>`,
     `expect_text=<status>`, and `supports_e2ee=<bool>`.

  `mx_rust_supports_e2ee()` still returns 0.
  `CryptoManager::supportsE2ee()` still returns false. The
  compile-time gate `RUST_SDK_E2EE_WIRED` is deliberately NOT
  defined this pass — flipping it requires both
  `expect_text=seen` AND `encrypted_send=ok` verified against
  Element Classic on a device with the room keys.

- **v0.5.0-prep+5 store-path consistency**: three
  connected fixes that closed the "SDK still opens an old crypto
  store after --reset-crypto-store" surprise reported after the
  headless smoke harness landed:
  1. New helper `matrix::app_data` at
     `src/storage/AppDataPaths.{h,cpp}` computes the same
     `QStandardPaths::AppLocalDataLocation`-style root as the
     runtime (`<XDG_DATA_HOME>/MatrixClient/matrix-client`) plus a
     list of legacy roots earlier v0.5.0-prep builds may have
     written to. Safe to call before `QCoreApplication` exists,
     which is exactly what `--reset-crypto-store` needs.
  2. `main.cpp --reset-crypto-store` now iterates
     `matrix::app_data::allRoots()` and lists every scanned root
     in its stdout, so users see exactly which layouts were
     inspected. It never touches `cache.sqlite`, QSettings, or
     the SecretStore. Bug fixed: pre-v0.5.0-prep+5 the scanner
     computed `<XDG_DATA_HOME>/matrix-client` directly, missing
     the `MatrixClient/` organisation-name segment Qt puts in
     `AppLocalDataLocation`. Any store the runtime created was
     invisible to reset, so `--reset-crypto-store` produced
     "No Rust SDK store directories found" while the SDK still
     opened the same (mismatched) store on the next login.
  3. `RustSdkMatrixClient` now uses the same helper for its
     per-account store, adds a `setStorePathOverride(QString)`
     testing hook for the smoke harness, exposes `rustStorePath()`
     and `rustStorePathIsOverride()` for diagnostics, and logs
     `base`, `slug`, `store`, `exists`, `mode` at INFO on the
     `matrix.rust` category. Path-only — no tokens, keys, or
     bodies.

  The smoke harness (`src/smoke/RustSdkSmokeTest.cpp`) now creates
  a `QTemporaryDir` under `/tmp/lightning-rust-sdk-smoke-XXXXXX/`
  and calls `setStorePathOverride` before login, so consecutive
  smoke runs never inherit a stale device id. The harness prints
  `smoke: store=temporary`, `smoke: store_path=<abs>`, and
  `smoke: store_exists=yes|no` up front and `supports_e2ee=…` in
  both the header and the summary line.

  Send outcome semantics also relaxed: `send=skipped` (no
  unencrypted room found) and `send=blocked` (target is
  encrypted / a Space / not in synced rooms) are now non-fatal
  and exit code stays 0. Only real send failures / timeouts
  return exit code 13.

- **v0.5.0-prep+4 verification harness**: a new headless
  smoke-test CLI mode for the Rust backend, gated to
  `ENABLE_RUST_SDK_BACKEND` and only accepted alongside
  `--backend=rust`. Sources at `src/smoke/RustSdkSmokeTest.{h,cpp}`;
  entry point invoked from `src/main.cpp` after preflight, before
  `QGuiApplication`.
  - Reads credentials only from environment variables
    (`LIGHTNING_TEST_HOMESERVER`, `LIGHTNING_TEST_USER`,
    `LIGHTNING_TEST_PASSWORD`, optional `LIGHTNING_TEST_SEND=1`,
    optional `LIGHTNING_TEST_ROOM_ID`). No creds ever land on a
    command line.
  - Constructs `RustSdkMatrixClient(nullptr, &app)` on purpose — a
    null `SettingsManager` prevents the smoke test from ever
    overwriting the interactive user's cached access token,
    syncToken, or homeserver. The Rust SDK store *is* still created
    under the test account's slug and can be wiped with
    `--reset-crypto-store`.
  - Prints `smoke: …` lines with counts (joined room count,
    encrypted room count, Space count, timeline event count,
    undecryptable event count) and statuses (`login=ok/failed`,
    `initial_sync=done`, `send=ok/failed/timeout`). Never prints
    message bodies, tokens, passwords, or crypto keys.
  - 60 s wall-clock budget with intermediate 30 s post-login sync
    guard and 15 s post-send confirmation guard. Exit codes: 0 on
    success, 10/11/12 for login/sync/room-count failures, 13 for
    send failure (only when `LIGHTNING_TEST_SEND=1`), 2 for
    missing env / wrong backend / wrong build.
  - `--help` output documents the flag. The preflight rejects it in
    a non-Rust build with exit 2 and a clean pointer to
    `-DENABLE_RUST_SDK_BACKEND=ON`.
  - See `docs/build-and-test.md` for exact usage and safety notes.

- **v0.5.0-prep+3 hardening**: three targeted fixes on
  top of Codex's `9eaa488` foundation.
  - **Bounded event queue.** The Rust-side `VecDeque<String>` used
    to accept unbounded pushes; a stalled C++ poll timer would
    grow it forever. Now capped at `EVENT_QUEUE_CAP = 4096` — on
    overflow we drop the oldest event and emit a single
    `queue_overflow` marker so the C++ side can log/surface it as
    a warning banner (`errorOccurred`).
  - **Sync-start race fixed.** `mx_rust_start_sync` used to check
    `sync_stop.is_some()`, release the lock, spawn the thread,
    then install the `stop` slot — two rapid `startSync()` calls
    could both see `None` and both spawn. Now the "already
    running?" check and slot reservation happen atomically under
    the same lock guard, before `thread::spawn`. No more leaked
    sync loops.
  - **Undecryptable encrypted events surface as a placeholder.**
    Codex's `install_event_handlers` only handled
    `OriginalSyncRoomMessageEvent` (plaintext or SDK-decrypted).
    Events the SDK could not decrypt silently disappeared,
    leaving encrypted rooms visually empty. Added a second
    handler for `OriginalSyncRoomEncryptedEvent` that emits a
    `timeline_event` with `undecryptable: true` and an empty
    body. `RustSdkMatrixClient::handleTimelineEvent` renders
    that as `[unable to decrypt yet]` (`TimelineEvent::Notice`).
    The ciphertext itself is deliberately NOT included in the
    FFI payload — C++ never needs it.
  - Comment on `login_ok` handling in
    `src/matrix/RustSdkMatrixClient.cpp` marks the payload
    sensitive: the `access_token` field must flow only into
    `SettingsManager::saveSession` (which routes to SecretStore)
    and must never appear in any log line.

- **v0.5.0-prep+2 foundation**: the optional Rust backend is no longer just a
  scaffold. `matrix-sdk` v0.18 is in `rust/Cargo.toml`, `Cargo.lock`
  is committed, and the Rust crate builds offline. The Rust FFI now
  owns a Matrix SDK client, SDK SQLite store, async work threads, and
  a JSON event queue drained by C++ on a `QTimer`.

  Implemented through the Rust path:
  - password login via `matrix_auth().login_username(...)`;
  - session restore via `MatrixSession` and `restore_session(...)`;
  - joined-room sync via `sync_with_callback(...)`;
  - room list events, including room name/topic/avatar/encrypted/Space
    flags where the SDK exposes them;
  - basic text/notice/emote timeline events through SDK event handlers;
  - plain text sends into unencrypted rooms, with C++ local echo
    reconciliation.

  Still not claimed:
  - E2EE is not enabled. `mx_rust_supports_e2ee()` returns 0 and
    `CryptoManager::supportsE2ee()` remains false.
  - Encrypted sends are blocked until encrypted read and send are
    verified end to end.
  - Rich timeline features in the Rust backend (pagination, replies,
    edits, reactions, media, typing, read receipts, Space child
    hierarchy) are still missing or partial.

  Store path: `${XDG_DATA_HOME}/MatrixClient/matrix-client/<safeUserId>/matrix-rust-sdk-store/`.
  This is separate from the C++ `cache.sqlite` and never stores access
  tokens; tokens remain in `SecretStore`.

- **v0.5.0-prep+1**: added --reset-crypto-store path resolution
  and documented the classifier block at the settings layer.
  Details in the git log.
- **v0.5.0-prep**: original C++ groundwork pass. `CMakeLists.txt`
  `PROJECT_VERSION` → `0.5.0`; `APP_VERSION_LABEL` → `"0.5.0-prep"`.
  `--reset-crypto-store` added as a pre-flight-recognised flag.
  Full write-up in `git show 6f389aa`.
- **Stabilisation (v0.4.8)**: three targeted fixes on top of v0.4.7:
  - **CacheStore NOT NULL repair**: previous versions bound null
    `QString` values to `rooms.child_room_ids` / `events.thread_root_id`
    (NOT NULL columns added in v0.4.5), which Qt's QSQLITE driver
    writes as SQL `NULL` and triggered `NOT NULL constraint failed`
    on every save. Fix: `textNonNull()` helper in
    `src/storage/CacheStore.cpp` coerces empty/null strings to a
    non-null empty QString at bind time, plus an idempotent repair
    (`UPDATE … SET col = '' WHERE col IS NULL`) on schema-ensure.
    No cache wipe.
  - **QML Column anchor warning**: `qml/MessageDelegate.qml`
    replaced an inner `MouseArea { anchors.fill: parent }` (invalid
    as a direct Column child) with a `HoverHandler`. Warning gone;
    hover-off behaviour unchanged.
  - **Status text `Connected`**: `AppController` reports
    `Connected` once the initial `/sync` response has been parsed
    and long-poll is the steady state, instead of continuing to
    say `Syncing`. Loading catch-up still says `Loading rooms…`.
- **HTTP `/sync` bring-up (v0.4.7)**: the initial `/sync` (no
  `since` token) uses `timeout=0&full_state=true`; the server
  returns current state immediately instead of long-polling.
  Follow-ups long-poll with `timeout=30000`. Request transfer
  timeout is 30s / 60s respectively — comfortably above the 30s
  server-side wait so we don't false-time-out.
  On session restore, a stored `syncToken` is discarded when the
  SQLite cache has no visible non-Space rooms, because an incremental
  token without visible cached room state cannot reconstruct the room
  list. This specifically handles the observed broken state where the
  cache held the Space room and `m.space.child` ids but no joined child
  room rows yet, causing the UI to render an empty list while
  incremental syncs had no reason to resend the full room snapshot.
  Fresh login also clears any persisted `syncToken` for the same MXID
  before starting sync.
  `MatrixClient::initialSyncDone()` is a new capability on the
  interface (default `true`; only `CppHttpMatrixClient` overrides
  and toggles it) so QML can distinguish "still waiting for the
  first response" from "sync loop live, no rooms". The room list
  header now shows the model count only after the first sync
  response lands, and the empty-state label under the list is
  state-aware: sign-in / loading / no joined rooms / no rooms in
  the selected Space.
  Non-secret sync diagnostics: `matrix.http:` log lines announce
  each request kind, HTTP status, response body size, and the
  joined / invited / left counts parsed. Tokens are never logged.
- **HTTP login → main-screen transition (v0.4.5)**: the `Loader` in
  `qml/Main.qml` used to pick the current page via
  `switch (app.currentScreen) { case app.LoginScreen: … }`. That
  pattern turned out to be fragile when the enum is exposed via
  `setContextProperty` (not registered as a QML type) and the QML
  files are AOT-compiled by the Qt Quick compiler — the case values
  could resolve to `undefined`, no case matched, and the switch fell
  through to `loginComponent`. Fresh HTTP login therefore logged
  "login ok" but the UI stayed on the login screen.
  Fix: integer-literal comparisons against the well-known
  `AppController::Screen` values in a `pickComponent()` helper, plus
  an explicit `Connections { onCurrentScreenChanged … }` re-trigger.
  Diagnostic `qCInfo(lcApp)` lines in
  `AppController::setCurrentScreen` and `onLoginSucceeded` make any
  future regression obvious in the terminal.
- **Backend selection**: `--backend={mock,http,rust}` plus legacy `--mock`.
  Pre-flight validation runs *before* `QGuiApplication` so bad args exit
  cleanly with exit code 2 even without a display. **v0.4.2**: a second
  preflight check refuses to construct `QGuiApplication` when neither
  `DISPLAY` nor `WAYLAND_DISPLAY` is set and `QT_QPA_PLATFORM` is not
  forced — exits 3 with a clear message instead of Qt's `qFatal`
  abort(). **v0.4.3**: `--http` and `--rust` are rejected pre-flight
  with a message pointing at `--backend=http` / `--backend=rust`.
- **Nix dev-shell runtime (v0.4.3)**: `flake.nix` / `shell.nix`
  `shellHook` now purges `QT_PLUGIN_PATH`, `QT_QPA_PLATFORM_PLUGIN_PATH`,
  `QML_IMPORT_PATH`, `QML2_IMPORT_PATH`, `QT_QUICK_CONTROLS_STYLE`,
  `QT_QUICK_CONTROLS_STYLE_PATH`, and `QT_QPA_PLATFORMTHEME` inherited
  from the outer KDE / GNOME session, then exports flake-consistent
  values against `${qt.qtbase}` and `${qt.qtwayland}` plus
  `QT_XKB_CONFIG_ROOT`. This fixes the reported crash where a KDE
  Plasma session's qtbase 6.11.0 helper plugin was being loaded into a
  6.11.1 executable and aborting at plugin init. Details in
  `docs/build-and-test.md`.
- **Mock backend** (`--backend=mock`): hardcoded rooms, one Space
  containing two rooms, one standalone room, one threaded conversation,
  synthetic reactions/edits/redactions/media/pagination.
- **HTTP backend** (`--backend=http`, default): password login,
  `/whoami` restore, long-poll `/sync`, room list, text messages,
  replies, edits, redactions, reactions, typing, read receipts,
  pagination via `/messages?dir=b`, member cache, media send/receive
  via legacy `/_matrix/media/v3/*`, local SQLite cache. **No E2EE.**
  Encrypted rooms are read-only placeholders; sends into encrypted
  rooms are blocked with a clear error.
- **Rust backend** (`--backend=rust`, only with
  `-DENABLE_RUST_SDK_BACKEND=ON`): Matrix Rust SDK backend
  foundation. Rust owns the SDK client/runtime/store and pushes JSON
  events through a C ABI queue. C++ `RustSdkMatrixClient` keeps the UI
  isolated from Rust and emits the existing `MatrixClient` signals.
  Login, restore, joined-room sync, basic text timeline events, and
  plain text send are wired. E2EE is still disabled; interactive
  encrypted sends are blocked honestly while the smoke-only encrypted
  send probe remains available for verification.
- **SecretStore**: libsecret (Secret Service via glib) backend when
  available; `InsecureFallbackSecretStore` (QSettings under `secrets/*`)
  when the session bus is unreachable. Legacy plaintext
  `session/accessToken` in QSettings is auto-migrated into the store
  on first launch of v0.4+.
- **SQLite cache**: `${XDG_DATA_HOME}/MatrixClient/matrix-client/<safeUserId>/cache.sqlite`.
  Rooms + last 200 non-encrypted events per room + members. Access
  tokens and decrypted encrypted-room bodies are **not** cached here.
  `SettingsManager::clearSession()` wipes both the QSettings session
  metadata and the SecretStore entry for that user.
- **Spaces (v0.4.1 + v0.4.2)**: `SpaceManager` is a `QAbstractListModel`
  bound to the active `MatrixClient`. Row 0 is a synthetic "All rooms";
  if any Space has children, an "Other rooms" row follows; then real
  Spaces. QML `RoomListPane` renders a chip strip when at least one
  real Space exists. `RoomListModel` applies the active-space filter.
  **v0.4.2**: `CppHttpMatrixClient::processStateEvent` now recognises
  `m.room.create` (`content.type == "m.space"` → `RoomInfo::isSpace`)
  and `m.space.child` (state key = child room id, `via[]` non-empty =
  active edge; empty = unlinked). Both events are handled from the
  `state.events` bucket AND from timeline state events. The
  `RoomInfo::spaceId` "primary parent" hint is intentionally not set on
  children — SpaceManager builds membership strictly from
  `Space.childRoomIds`, so rooms in multiple Spaces stay consistent.
  **v0.4.5**: `CacheStore` now persists `isSpace` and `childRoomIds`.
  On relaunch the Space chip strip renders immediately from cache —
  no more blank-until-first-`/sync` window.
- **Threads (v0.4.1 + v0.4.4)**: `MessageComposer` gains thread-reply
  mode via `beginThreadReply(rootId, preview)`. `TimelineModel`
  exposes `threadRootId`, `isThreadRoot`, `threadReplyCount` roles.
  Mock backend seeds a threaded conversation.
  `MatrixClient::sendThreadReply` is a virtual with a default that
  falls back to `sendReply`. **v0.4.4**: `CppHttpMatrixClient` now
  overrides `sendThreadReply` and emits a real `m.thread` relation:

  ```json
  { "m.relates_to": {
      "rel_type": "m.thread",
      "event_id": "$root",
      "is_falling_back": true,
      "m.in_reply_to": { "event_id": "$latest-or-root" }
  } }
  ```

  `processTimelineEvent` (live `/sync`) and the pagination path
  (`/messages?dir=b`) both recognise `rel_type == "m.thread"` and set
  `TimelineEvent::threadRootId`. When the same event carries an
  `m.in_reply_to` (fallback for non-thread-aware clients), the
  `replyToEventId` field is intentionally cleared — QML would
  otherwise render both the "in thread" chip AND the reply preview
  strip, which is noise. Local echo is set with `threadRootId` so the
  chip shows immediately; the existing txnId dedup + `eventReplaced`
  path already reconciles it with the server-confirmed event.

  Known limitations documented in `docs/matrix-feature-status.md`:
  * `unsigned["m.relations"]["m.thread"]` server aggregation (latest
    event, reply count) is not read yet. Reply counts are computed
    locally by scanning the loaded timeline (v0.4.1 behaviour).
  * Thread replies still appear inline in the main timeline (marked
    "in thread") — a dedicated thread side-panel is v0.5+.
  * `CacheStore` now persists `threadRootId` (v0.4.5) — the "in
    thread" chip renders immediately after restart for cached events.
- **SSO/OIDC capability flags (v0.4.1)**: `AuthManager` exposes
  `supportsPasswordLogin`, `supportsSsoLogin` (false), `supportsOidcLogin`
  (false), plus placeholder `beginSsoLogin` / `beginOidcLogin` that
  emit a clean "not implemented" error. QML is not wired to these yet
  (Settings screen is the natural spot in a follow-up).

## What is *stubbed or partial* — code exists but does not do the work

- `NotificationManager`: logs "notify" via QLoggingCategory. No tray, no
  native notify.
- `CryptoManager`: capability surface only. `supportsE2ee` is a pure
  compile-time expression — it becomes true only if
  `ENABLE_RUST_SDK_BACKEND` **and** `RUST_SDK_E2EE_WIRED` are both
  defined, AND the active backend is "rust". `RUST_SDK_E2EE_WIRED` is
  not defined in this pass.
- `RustSdkMatrixClient` rich operations: pagination, replies, edits,
  redactions, reactions, media send/receive, typing, read receipts,
  and Space child hierarchy are not wired through Rust yet.
- `AccountManager`: tracks the single active user id from the session.
  Multi-account (per-account SecretStore keyspace, cache path, sync
  loop) is not implemented — foundation described in
  `docs/next-prompts.md`.

## What is *intentionally missing*

- Verified E2EE. The Rust backend links the SDK but encrypted
  read/send have not been manually verified end to end, so Lightning
  still reports no E2EE support and will not hand-roll cryptography in
  C++.
- Authenticated media (`/_matrix/client/v1/media/*`) — v0.3/v0.4 use
  legacy `/_matrix/media/v3/*`.
- Full SSO / OIDC / MAS login flow.
- Multi-account UI switching. `AccountManager` API is single-active.
- Sliding sync.
- Windows / macOS SecretStore backends (they fall back to the insecure
  store with a warning until v0.5+).
- Own-profile lookup, other-user read-receipt display, member list UI.
- Thread panel / per-thread timeline model. Current thread UI is a
  chip on the root event + a composer mode.

## Build & smoke summary (see `docs/build-and-test.md` for details)

- Default: `nix develop -c cmake -S . -B build -G Ninja && nix develop -c cmake --build build`.
- With Rust: `nix develop -c cmake -S . -B build-rust -G Ninja -DENABLE_RUST_SDK_BACKEND=ON && nix develop -c cmake --build build-rust`.
- Rust-only offline: `cd rust && nix develop -c cargo build --release --offline`.
- Smoke: `QT_QPA_PLATFORM=offscreen timeout 3 ./build/matrix-client --mock`
  should exit 124 with no QML warnings and no crashes.
- Rejection: `QT_QPA_PLATFORM=offscreen ./build/matrix-client --backend=bogus`
  and `./build/matrix-client --backend=rust` (in the non-Rust build)
  both exit 2 with a clear stderr message. In the Rust build,
  `QT_QPA_PLATFORM=offscreen timeout 3 ./build-rust/matrix-client --backend=rust`
  should start without crashing.

## Rules for continuation

1. Do not rewrite from scratch. Keep the file layout.
2. Do not fake E2EE. `CryptoManager::supportsE2ee` is the single source
   of truth for the UI.
3. Do not remove the mock or HTTP backends.
4. Do not use Electron, Tauri, WebEngine chat UI, or Element Web.
5. Prefer C++ for anything that is not a Matrix cryptographic primitive
   or a place where the Matrix Rust SDK is the objectively correct
   dependency.
6. Any `Q_PROPERTY(T*)` in a header must have `T` fully defined in that
   header (moc reads `QMetaType::fromType<T>()` which requires
   completeness — we've been bitten by this twice, see the includes
   at the top of `src/app/AppController.h`).
7. Bad `--backend=…` values must never crash — pre-flight validation
   in `src/main.cpp` catches them before Qt starts.
