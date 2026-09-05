# Implemented feature contracts

This is section 7 of CLAUDE.md, moved out of that file on 2026-08-28 because
CLAUDE.md had grown to 199,237 characters against a 150,000 limit and was being
SILENTLY TRUNCATED — which meant its own tail (the completion-report rules, the
review protocol and the continuity rules) was not reaching agents at all.
Nothing here was edited; it is the same text, one file away.

**Read this before changing any feature it describes.** These are contracts, not
descriptions: most entries record a decision that cost something to learn, and
several say plainly what must never be done again.

Treat the following as implemented in the current repository, while preserving
backend capability checks and honest live-test status.

### Authentication and lifecycle

- Password login, persistent SDK session/store, session restoration, logout,
  sync/initial-sync state, and account-scoped local reset paths
- **OAuth 2.0 / OIDC browser sign-in** through `Client::oauth()` on
  matrix-sdk 0.18 (`rust/src/oauth.rs`). PKCE, CSRF `state`, code exchange
  and the refresh REQUEST are SDK-owned; Lightning implements no OAuth
  primitive. Two things the SDK does NOT do itself and must not be dropped:
  * `ClientBuilder::handle_refresh_tokens()` **defaults to FALSE** — without
    it a 401 is forwarded rather than renewed and a saved refresh token is
    inert.
  * The ROTATED pair must be written back (`oauth::spawn_token_persistence`
    on `SessionChange::TokensRefreshed` →
    `SettingsManager::updateSessionTokens`). Skipping it leaves a CONSUMED
    refresh token in the store, and an OAuth 2.1 server treats its reuse as
    compromise.
  `SessionChange::UnknownToken` surfaces as the existing `AccessTokenRevoked`
  state, not an endless sync-failure loop. Added surface is only the
  system-browser launch and `src/auth/OAuthCallbackServer.*` — loopback-only
  (127.0.0.1), ephemeral port, single-shot, size-bounded, timed out; hand-
  rolled because matrix-sdk's `local-server` helper needs `sso-login`/`axum`,
  not vendored in this offline `--locked` build. Costs no dependency change.
  **Two-phase store lifecycle, mandatory.** OAuth learns the user id only
  from `whoami` after the code exchange, so phase A authenticates on a
  bootstrap handle with NO persistent store (in-memory default; it must
  never sync, or it would upload device keys phase B would contradict), and
  phase B derives `AccountIdentity`, applies
  `rust_session::oauthLoginBlockReason()`, then opens the account's sqlite
  store and `oauth().restore_session()`. Rule: a device the server just
  issued must never adopt a store belonging to a different device. That
  block deliberately does NOT suggest a local reset — the store belongs to a
  live device whose keys are still valid.
  Sessions carry an `authType` discriminator in **QSettings, not the
  SecretStore**, so restore routes correctly even with a locked keyring:
  `password` → `matrix_auth()`, `oauth` → `oauth()`. Refresh tokens and the
  dynamic-registration client id are CREDENTIALS in the SecretStore, never
  in QSettings, never exposed to QML, never logged. Legacy Matrix SSO is
  detected and disclosed as unsupported, never offered. Live validation
  (2026-08-15): **PASSED against matrix.org** (MAS/OIDC) end-to-end incl.
  Google-IdP registration, refresh, restart restoration and sign-out — the
  OAuth path is fully live-validated.
- `restore_client()` previously hardcoded `refresh_token: None`, discarding
  a saved refresh token on every restore so an expired access token surfaced
  as `M_UNKNOWN_TOKEN`. Fixed for password sessions as well as OAuth
- Persistent multi-account support: records under `accounts/<slug>/` in
  QSettings, tokens per-user-id in the SecretStore, accessors are views of
  the active account. `AppController::switchToAccount` detaches the local
  session (`MatrixClient::detachSession` emits `loggedOut` for model cleanup
  WITHOUT server logout or store deletion), repoints settings, restores
  normally. Only the active account syncs; removal/logout are scoped to one
  account and logout continues with the most recently added remaining one. A
  failed activation falls back once
- Secret Service/libsecret token storage when available, with an explicit
  insecure QSettings fallback warning
- Rust-backed unified sync/Sliding Sync behavior with compatibility fallback

### Rooms and navigation

- Joined rooms, DM detection from `m.direct`, invites, Space hierarchy, room
  membership/actions, room information, and room creation
- **Two navigation layouts, chosen per account** (Settings → Appearance →
  Conversation list), plus the Spaces rail above both. Full contract in
  `docs/navigation-layouts.md`; read it before touching any of this. The
  load-bearing parts:
  * **Classic** — one activity-ordered conversation list. The default and the
    clamp target for an out-of-range stored value, because it works in an
    account with no Spaces at all.
  * **Channels** — Sable's model, reworked 2026-08-26 into THREE VIEWS the
    rail chooses between: **Home** (Create Room / Join with Address / Explore
    Spaces / Message Search, then the room invites and the rooms in no Space),
    **Direct Messages** (Create Chat, the DM invites and the DMs) and **one
    per Space** (Lobby / Message Search, its own DIRECT child rooms, its
    subspaces as sibling folders). It no longer falls back to Classic at
    Home — a layout that becomes the other layout depending on where you are
    is not a layout.
    The selection is written to `scopeSpaceId` VERBATIM and CLASSIFIED there;
    it used to collapse every non-`!` value to `""`, which left the rail one
    way to say anything that was not a Space, so a People tab could not be
    expressed and DMs had to ride along inside every view to stay reachable.
    **A DM is never a Space CHILD** — Matrix gives no way for a DM to be a
    Space's child, so a DM under a Space heading as a *child* is a claim the
    state does not make. This used to read "a DM is in the People tab and
    nowhere else"; AMENDED 2026-08-29 at Rokas's explicit request that the
    people filter follow the selected Space in BOTH layouts. A Space view now
    carries a **People group** listing DMs with people who are joined or
    invited to that SPACE ROOM — a claim about Space MEMBERSHIP, which the
    state does make, and Element's own reading. The literal invariant above is
    intact: nothing is presented as a Space child. The **@people tab stays
    ACCOUNT-WIDE**, which is what keeps every DM reachable, and Classic's
    scope predicate FAILS OPEN (an unknown roster moves nothing) while the
    Channels group FAILS CLOSED (no roster, no group) — opposite directions on
    purpose, because one filters a list that already has rows and the other
    adds rows to a view that has none. **The People tab is CHANNELS ONLY** and the rail
    resets a selection left on it when the layout changes. **The People/Rooms
    filter chips are dropped in Channels** (the tabs are that split); the
    stored value is MAPPED on the way in, never rewritten, so Classic gets the
    user's own chip back. A selection on a Space the account no longer has
    STAYS that Space and renders its own emptiness — falling back to
    "everything" would silently be a different Space's view under a tile that
    is gone. **Lobby is the HEAD of a SPACE'S view and only that one**; Home
    and People have no overview of themselves to open. Subspaces are NOT
    nested; a subspace is a Space folder at the same level. A room in two
    Spaces appears under both. Rows carry the room's AVATAR (the lock/DM glyph
    is a corner badge, not a replacement), and a DM's face comes from
    `DirectAvatarResolver` — ONE derivation shared with the Classic list,
    because `RoomInfo::avatarUrl` is empty on most DMs and this column drew
    initials next to a Home strip showing the real pictures. Order is the
    rail's arrangement for subspaces and `m.space.child` for rooms — never
    activity. Command rows carry a synthetic `@` id the presenter must name
    (contract-pinned: an unnamed one renders as a control that does nothing)
    and the MODEL names each row's glyph, because the icon font is a SUBSET
    and the ordinary icon sweep only sees a literal beside an `Icon { name: }`.
    Live GUI validation of the three-view rework: **NOT TESTED**.
  * **The rail's drag** lives in `RailEntryModel`, a real QAbstractListModel
    emitting `beginMoveRows`, so a preview reorder ANIMATES and the delegate
    holding the gesture survives a refresh. A JS array rebuilt per change is a
    model reset and could do neither. Nothing is written until release. THE
    TILE ITSELF MOVES — full opacity, following the pointer, neighbours
    animating around it; the first revision's dimmed gap, insertion line and
    floating proxy were all cut on testing ("spaces should always be their
    normal image and move freely without a line appearing between them").
    `endDrag` ANNOUNCES the cleared drag flags: `refresh()` may find the rows
    identical and emit nothing, which left a released tile dimmed until an
    unrelated room update happened along.
    **REORDER vs GROUP is measured from the side the pointer arrived from.**
    Short of a row's MIDPOINT the pointer is RESTING on that tile — nothing
    moves, and it is what a release groups with; past the midpoint it has
    PUSHED THROUGH and the dragged block takes the row. The previous rule
    ("the middle 24 px is the group zone") could never fire: reaching that
    middle means crossing the near edge first, which reordered, so the tile
    being aimed at stepped aside and the row under the pointer became the
    DRAGGED entry — never a group target. **No drop ever created a folder, and
    every model test passed throughout**, because they hand the model the
    target's row directly. Resting needs its own verb (`clearDropTarget()`);
    `updateDrag(row, false)` reorders. The 250 ms dwell is now a SECOND guard,
    not the only one. On a group the target lights accent with a 3 px ring and
    the dragged tile PARKS on it at 0.56 scale, so a full-size tile no longer
    covers the ring that says where it would land.
  * **The rail's Space menu** carries Sable's set and names its Space in
    AppMenu's context header: Mark as read, Mute/Unmute, Invite, Copy link,
    Share link…, Space settings. Matrix has no "mark a Space read" and no "mute
    a Space" primitive — a Space is a room with no timeline — so both do what a
    person would do by hand to each room inside it, bounded by the Space's own
    transitive membership, through the ONE per-room path. Unmute restores
    FOLLOW THE ACCOUNT DEFAULT, never "all messages"; Mark as read routes to
    `RoomListModel::markRoomRead`. Links are the PUBLIC `matrix.to` permalink.
    **Invite is deliberately NOT gated on `canInvite`**: that reads
    `app.roomInfo`, which follows whatever surface last pointed it somewhere,
    so gating would grey the row out because nobody LOOKED — a worse lie than
    offering something the server may refuse.
  * **`SpaceSettingsDialog`** (General / Members / Permissions / Developer
    tools) is `RoomInfoController` behind a Space-shaped surface: a Space IS a
    Matrix room, so name/topic/avatar/join rule/alias/power levels are ordinary
    room state, gated on the room's REAL required level and never applied
    optimistically. Fields are EXPLICIT MIRRORS (a keystroke destroys a
    binding; a rejection must snap back; the dialog reopens on other Spaces),
    and it restores `app.roomInfo` to wherever it was pointing on close.
    Sable's Cosmetics / Abbreviations / Emojis & Stickers / Appearance pages
    are deliberately ABSENT — none is Matrix state, so they would be private
    storage only Lightning could read presented as part of the Space. Four dead
    tabs are worse than four missing ones; a contract test bans `app.settings`
    and `app.railLayout` from the file.
  * **A hidden `AppMenuSeparator` now takes NO height.** QQuickMenu lays rows
    out in a ListView that honours each item's height, and a separator's height
    comes from its contentItem plus padding whether it is visible or not — so
    the rail's Space menu opened with a 13 px band above its first row, left by
    the divider belonging to the folder-only rows. AppMenuItem already did this.
  * **Local Space folders** are device-local organisation and touch NO Matrix
    state — banned by contract test, not by convention. Dropping one Space
    onto another creates a folder where the target was; folders never nest.
    The stored format is ADDITIVE, so a 0.7.6 layout loads with its folders,
    membership, order and collapse intact.
  * **Matrix subspaces** are the real hierarchy: only ROOT Spaces sit at the
    rail's top level, a subspace nests under its expanded parent at its REAL
    depth (was a hardcoded 0-or-1 approximation), and a subspace row is not
    draggable because its position is Matrix's. Several parents → nested under
    exactly one, deterministically; cycles → every Space stays reachable as a
    root; parent links only one side reports → resolved from the union.
- **`RoomInfo::childRoomIds` is DIRECT children in `m.space.child` order** on
  every backend. The Rust backend used to fill it from its payload's
  `descendants` (the TRANSITIVE closure), so everything that needed the
  admin's structure saw one flat run of the whole tree — the mock and HTTP
  backends were right, which is exactly why no test caught it.
  `enqueue_spaces` now emits `children` read from each Space's own state,
  ordered by the spec's comparator; `descendants` remains a fallback.
- Quick switching across rooms, direct messages, Spaces, invites, threads
- Activity ordering, unread state/navigation, first-unread and latest jumps,
  threaded receipts, and local marked-unread behavior
- Matrix presence indicators on unambiguous 1:1 DM rows, the People list and
  the member profile popover, via bounded client polling — Sliding Sync has
  no presence extension. §16 carries the mechanism and honesty rules; live
  validation NOT TESTED
- **Member power levels** via `Room::update_power_levels`, which preserves
  every other user's value including arbitrary custom numbers. OFFER policy
  is `RoomInfoController::canSetPowerLevel` (§5), applying what the server
  applies anyway: never above the viewer's own level, never against a peer
  at or above it, self-DEMOTION only, and an unknown target **FAILS CLOSED**
  — levels may legitimately be NEGATIVE (Element's "Restricted" is -1), so
  absence of the roster row, never a sentinel, is the unknown state.
  `roleLabelForLevel` renders 100/50/users_default as
  Administrator/Moderator/Member and **anything else as its number**: a room
  using 42 must not be relabelled 50 and must not be SAVED as 50. Nothing is
  applied optimistically — the write completes, the roster is re-read, so a
  rejection cannot leave a value the room does not have.
  `own_can_change_power_levels` is the SDK's `can_send_state`, never a role
  label. Live homeserver validation NOT TESTED
- **Join rule and canonical alias** in Room Information → Overview, each
  gated on the room's REAL required level for that state event. Only
  `invite`/`public`/`knock` are settable: restricted rules carry an
  allow-rule list this surface cannot build, and sending one with an empty
  list would silently lock the room to invite-only while claiming otherwise
  — a restricted room is displayed honestly and left alone. The alias path
  publishes the directory mapping first (`Client::create_room_alias`) when
  the alias does not already resolve to this room, because a server rejects
  a canonical alias it cannot resolve; clearing sends the state event with
  no alias and deliberately does NOT delete the directory mapping. Both ride
  the MEMBER snapshot, so a successful write must ask for a roster refresh
  explicitly. NOT TESTED
- **Room upgrades / tombstones**: banner-and-link, deliberately **NOT
  auto-follow**. The old room stays open and readable; the successor is
  OFFERED. Security reason: a transition discards navigation and draft
  context, and `m.room.tombstone` is state anyone with the power level can
  send — it NAMES the room you would be moved into. No code path changes the
  current room, joins, or leaves except as the direct result of the user
  pressing the banner. Room ids come ONLY from the SDK's
  `Room::successor_room()` / `predecessor_room()` (ruma `OwnedRoomId`);
  nothing hand-parses `m.room.tombstone` or `m.room.create`. The tombstone's
  `body` **NEVER crosses the FFI** (free text chosen by whoever sent the
  event, on a control the user is invited to click), so the banner uses
  Lightning's own wording. Joined successor → navigate, no join. Invited or
  UNKNOWN → join through `RoomDiscoveryController::join` (so error
  categories and wait-for-room settling cannot drift from Discover),
  navigate once settled; refused → stay put, reason inline. A successor we
  HOLD but cannot enter is the one case reported inaccessible; one never
  heard of is **Unknown**. `chainVerified` requires the successor's
  predecessor to point BACK; false-because-unknown means "not established
  yet", and only a CONTRADICTED chain withholds the room list's de-emphasis
  — a demotion WITHIN the room's own category, never a filter. Permalinks
  untouched. NOT TESTED
- **Unverified-session prompts**: `sessionVerificationNeeded` is true for
  exactly one actionable state — signed in, crypto-capable backend,
  `sessionTrustState == "Not verified"`. "Unknown" and "Cross-signing
  unavailable" deliberately do NOT prompt. `sessionVerificationWarning` adds
  the per-account dismissal and ONLY the badges read it — the Sessions page
  states the fact from the undismissible property, so silencing the reminder
  never hides the truth. The dismissal is strictly account-scoped (NOT
  `appearanceValue`, which mirrors into a shared global fallback) and clears
  on verification, so it can never silence a later unverified session

### Timeline and media

- **Pinned messages** (`m.room.pinned_events`). Lightning invents NO storage
  format: **the list IS the state event**, read via
  `Room::pinned_event_ids()` with `Room::load_pinned_events()` as the
  `/state` fallback (probe spent once per room per session), written via
  `Room::pin_event()`/`unpin_event()`, **which do the read-modify-send
  themselves** — a concurrent change can never be clobbered by a stale list
  of ours. Each pinned id resolves through `Room::load_or_fetch_event()`
  (cache-first, one bounded `/event` on a miss, SDK-decrypted), fan-out
  bounded at `PINNED_RESOLVE_CAP` (32) sequential resolutions, 10 s no-retry
  each; longer lists report `truncated`. **The COMPLETE id list crosses
  uncapped**, because it answers "is this pinned?" for the message menu — a
  capped answer there would be a WRONG answer, not a partial one.
  `PinnedMessagesController` tracks the ACTIVE room (not the Room
  Information panel's room, which may be a Space home), never applies a pin
  optimistically (re-reads the authoritative list on success AND rejection),
  and a failed READ keeps the last known list — a flaky connection must not
  read as "nothing is pinned any more". A remote change arrives as a
  payload-free `room_pinned_changed` poke answered by re-reading, so remote
  and local converge on one path. Entry previews are decrypted text in an
  encrypted room: **MEMORY ONLY, never CacheStore**. NOT TESTED
- SDK-backed live timelines and local echoes
- Text, rich replies, edits, reactions, redactions, typing indicators, read
  receipts, mentions, and room-state activity rows
- Element-style read-receipt chips on live-room rows: newest 16 receipts
  cross the bridge with a truthful uncapped total, and **ONLY the local user
  is excluded** — a user's marker renders even on their own message, as in
  real Element; the earlier extra sender-exclusion made receipts vanish
  asymmetrically when the other side sent (docs/receipt-semantics.md).
  Thread timeline builders deliberately keep receipt tracking **Disabled** —
  SDK receipts are not thread-aware
- Images, files, clipboard images, encrypted attachments, media
  viewing/saving, animated GIF attachments, validated direct-raster previews
- Inline video/audio playback materializes the decrypted payload as a
  session-scoped 0600 temp file (wiped on sign-out/switch/exit); a BOUNDED
  speculative prefetch for on-screen video/audio rows (≤ 32 MiB declared,
  lowest priority, dropped on room switch) governed by the SAME preference
  as GIF autoplay ("never" disables all passive media downloads); and a
  locally extracted first-frame poster for videos without a Matrix thumbnail
  (JPEG, RAM image cache only — never disk). In-flight fetches are
  cancellable end-to-end (QML card → MediaBridge → `mx_rust_media_cancel`),
  and the SDK media store runs a retention policy (max_file_size 24 MiB) so
  large payloads no longer enter or stall matrix-sdk-media.sqlite3
- **Outgoing videos carry a real poster thumbnail.** `AttachmentQueueModel`
  drives the same `VideoPosterExtractor` the receive side uses; the decoded
  frame is also the only honest source of the video's width/height and
  duration on the send side. Dispatch waits for the poster and nothing else;
  extraction failure is NOT send failure. Bytes cross
  `mx_rust_timeline_send_video`/`mx_rust_thread_send_video`, are re-validated
  by magic sniffing (`rooms::PosterBytes`, ≤ 2 MiB, SVG and every non-raster
  refused; a refusal degrades to no thumbnail), and become
  `AttachmentConfig::thumbnail`. **The SDK owns everything after that** —
  upload, encryption alongside the payload, the thumbnail fields on the
  `m.video` event. Nothing in C++ builds thumbnail content or encrypts
  anything. Live Element interop of sent posters: NOT TESTED
- **Element-style Hide image / Show image**, on image and sticker rows only.
  PURELY LOCAL: nothing is redacted, edited, deleted or sent, no other client
  sees anything, and `MediaVisibilityStore` reaches no MatrixClient, no
  SettingsManager and no QSettings (asserted). THE contract is GEOMETRY —
  `MediaHiddenPlaceholder` fills the media box and contributes no implicit
  size, so the row keeps the exact rectangle the picture reserved and the
  timeline does not move; a text row in its place would jump every message
  above it. State is keyed by media identity in the STORE, never in the
  delegate (a timeline row is destroyed the moment it leaves the cache
  buffer). **Session-only, deliberately**: no Matrix standard exists, a hidden
  image the user has forgotten is content they cannot find, and there is no
  hidden-media list to un-hide from — bounded at 4096 keys, and the cap
  releases the OLDEST rather than refusing the newest. Hiding starts NO fetch
  and removes nothing from the cache; the `Image` source is CLEARED (an Image
  with a source still holds the decoded pixmap) and a hidden GIF stops
  animating. Hide is on the action bar and in the menu; once hidden the
  placeholder's Show image is the only control, because a second control
  offering to hide what is already hidden is noise. Live validation NOT TESTED
- Backward pagination and retry, stable navigation, loaded-timeline search,
  message links/permalinks, message details, context menus, sender profiles
- Link previews with encrypted-room privacy controls and security validation
- Smooth mouse-wheel motion, touchpad pixel scrolling, configurable wheel
  speed, keyboard scrolling, and per-room position preservation

### Threads

- SDK `TimelineFocus::Thread` timelines and `ThreadListService`
- Thread panel and per-room Threads view, real `m.thread` text/rich replies,
  follow/unfollow where MSC4306 is supported, threaded read receipts, and
  pagination
- Thread image/file/clipboard attachments through the SDK including
  encrypted rooms, with local echoes, send state, retry/failure handling.
  Thread video sends carry the same locally extracted poster
  (`mx_rust_thread_send_video`), still routed through the thread-focused SDK
  timeline so the `m.thread` relation and encryption stay SDK-owned
- Element-style root summary cards with server reply counts, latest
  metadata, live updates, conservative unread indication
- **Thread voice messages.** Same mic, pill, waveform, cancel and send as
  the room composer, reusing the ONE shared `VoiceRecorder`;
  `rooms::send_thread_voice_path` builds the same `AttachmentInfo::Voice`
  and routes through `mx_rust_thread_send_voice`. Invariants:
  * **NO room-send fallback, ever** — a thread voice message that cannot
    reach its thread must fail, never land in the main timeline.
  * It hands over **BYTES, not a path**. The SDK resolves
    `AttachmentSource::File` with `fs::read` INSIDE its spawned task, so
    reclaiming the recording when the panel closes (one click after Send)
    could delete it before it was read, and the advanced thread generation
    would suppress the failure report. Do not switch back to `File`.
  * Recorder ownership is ONE authoritative value
    (`AppController::voiceOwner`), never two per-composer flags: with two,
    recording in the room composer and then in a thread (opening a thread
    does not change `currentRoomId`, so cancel-on-room-change never fires)
    left both armed and one `ready()` sent the same file to BOTH.
  * Ownership is taken only AFTER a successful start and is NEVER stolen
    from a live recorder — `VoiceRecorder::start()` refuses while
    Recording/Processing and returns false WITHOUT emitting `failed()`, so
    moving ownership first orphaned the microphone with no pill and no
    owner, for up to 15 minutes and across sign-out.
  Live mic capture and Element interop: NOT TESTED
- **Thread participant facepiles.** matrix-sdk-ui 0.18 exposes NO
  participant list: `ThreadSummary`/`ThreadListItem` carry only the root
  sender, the latest reply's sender and a count of REPLIES — `num_replies`
  is not a participant count. Participants therefore come from the thread's
  own events via `Room::load_or_fetch_event_with_relations` (cache-first),
  deduped by user id in Rust, root sender first then first-appearance order.
  Only user id, display name and avatar mxc cross the FFI — never event
  content. `ThreadManager` caches per (roomId, rootEventId), cleared on
  sign-out; requests are idempotent per root and an unanswered one is
  released after 60 s so a root never becomes permanently un-retryable.
  **An empty list means UNKNOWN, never "nobody"** — a FAILED lookup is
  deliberately NOT cached, and the card falls back to the latest sender's
  avatar. No "+N" badge: the distinct total beyond the cap is not known.
  Fan-out is BOUNDED (the timeline is not virtualized, so every root's card
  calls `requestParticipants` on the same frame):
  `kMaxConcurrentParticipantFetches` (4) concurrent + a FIFO queue capped at
  64, beyond which a root is DROPPED, keeping it genuinely retryable rather
  than queued forever. A slot is released by the answer, by the 60 s
  timeout, **and by a FAILED (empty) answer** — otherwise one failure per
  round would shrink the pool permanently. Dedup covers cached, in-flight
  AND queued roots. `setActiveRoom()` discards QUEUED work for other rooms
  but deliberately leaves IN-FLIGHT work running: the cache key is
  `(roomId, rootEventId)`, so a late answer can only populate its own room
- True thread-reply filtering from the live main timeline, cold-cache
  initial loading, stable per-thread scrolling, quick-switch navigation, and
  in-place thread E2EE recovery

### E2EE

- SDK-owned encrypted sending/receiving and persistent crypto store
- Crypto readiness/health model and sanitized recovery diagnostics
- Automatic room-key requests and SDK backup download after decryption failure
- Late in-place decryption updates, manual bounded retry, key import, and
  recovery-key/passphrase backup restore controls
- SAS emoji device verification in both directions, show-QR verification
  (Lightning displays a code the other device scans; SDK-owned reciprocate
  flow, SAS fallback, **never scans** — live Element interop NOT TESTED),
  session/device trust UI, cross-signing/backup state, and
  generation-isolated callbacks

These mechanisms cannot guarantee recovery of historical messages whose keys
were never backed up or shared.

### Calls, screen sharing and MatrixRTC

The deep contract is `docs/matrixrtc.md` and `docs/voice-calls.md`; §16 carries
the lane's refuted hypotheses and must be read before touching any of it. What
EXISTS, so nobody rebuilds it:

- **MatrixRTC group calls over LiveKit**, with audio, camera and screen share
  working in both directions against Element (live-validated 2026-08-25).
  Per-participant frame encryption, raised hands interoperating with Element,
  per-participant volume, a spotlight/grid stage, and a device picker.
- **Legacy 1:1 `m.call.*` signalling** plus a GStreamer/webrtcbin media
  backend behind a build-time seam, re-probed at runtime, with a
  `LIGHTNING_DISABLE_WEBRTC=1` kill switch and an honest signalling-only
  refusal when no engine is present.
- **Screen sharing per platform.** Linux goes through the xdg-desktop-portal,
  which owns its own picker and hands back a PipeWire node — Lightning
  enumerates nothing there. Windows and macOS have no such broker, so
  Lightning draws the picker: displays on both, and on Windows also SINGLE
  WINDOWS through `src/calls/WindowCaptureSrc.*`, an element Lightning
  compiles in itself because nothing shippable captures a window
  (`d3d11screencapturesrc` has `window-handle` but its plugin does not load in
  this toolchain). It asks the window to render itself via `PrintWindow`
  rather than cropping the screen, DELIBERATELY: cropping would share whatever
  is stacked on top, and a share that can leak a window the user did not
  choose is not a feature. The honest cost is that a window drawing through
  its own swapchain prints blank, which is reported as a black frame and NOT
  worked around.
- **Publish ceilings match livekit-client's presets** — 1920x1080/30 for a
  screen share, 1280x720/30 for a camera — with `pixel-aspect-ratio` pinned to
  1/1 at BOTH the ceiling and the source, so a non-16:9 source arrives the
  right shape instead of carrying a PAR that VP8 discards (§16).
- Windows and macOS packages BUNDLE GStreamer and each artifact proves itself
  with `--call-media-status`; Linux packages declare no runtime dependency and
  a distro without GStreamer keeps the honest refusal.

Live status, and do not inflate it: **audio, camera and screen share are
live-confirmed** — against Element on Linux, and on a packaged Windows build
(2026-08-27). Group-call behaviour on macOS, an ANSWERED legacy 1:1 call, and
most failure branches are **NOT TESTED**. The full inventory is at the end of
§16.

### Notifications

- Native freedesktop notifications when Qt DBus and a notification service
  are available
- SDK-derived mention metadata, direct-message and per-room local modes,
  privacy modes, active-room suppression, invite and verification notices
- Cold-start/backlog suppression, bounded click routing to room/event/thread,
  configurable sounds, and burst coalescing
- Per-room notification modes synchronize to server push rules on the Rust
  backend (SDK-managed; user-defined-rule reports reconcile a device-local
  cache that keeps policy working offline, and a failed write is disclosed
  in the UI as kept-on-this-device). Non-Rust backends remain device-local.
  Live homeserver/Element interoperability of the rules: NOT TESTED
- **"Follow account default" and retry on reconnect.** Matrix has no
  follow-default rule — it has the ABSENCE of a room override — so mode 3
  routes to `clearRoomNotificationMode` →
  `mx_rust_clear_room_notification_mode` → the SDK's
  `delete_user_defined_room_rules`, and `setRoomNotificationMode` still
  refuses 3 toward the FFI so an invalid `RoomNotificationMode` can never
  cross. Success reports on its OWN `roomNotificationModeCleared` signal:
  the absence of a rule is not a rule's value, and routing it through
  `roomNotificationModeChanged` DROPPED a successful clear, so a clear that
  failed once claimed "couldn't save" for the whole session and was
  re-issued on every reconnect. Mode 3 is stored EXPLICITLY, not as a
  missing key — an absent key already reads back as 0, so absence cannot
  distinguish "following the default" from "never configured". Clamps are
  0..3 in `SettingsManager` only; other mode settings stay 0..2.
  `NotificationManager` branches only on Muted/MentionsOnly, so mode 3
  notifies locally, and the UI discloses that the SERVER applies the account
  default while THIS DEVICE notifies for all messages — the resolved default
  is not known here and is not fabricated. Offered only on a backend that
  owns server push rules. A failed offline write is retried on the EDGE into
  Syncing (not on every status change), and a room leaves the failed set
  ONLY when the server acknowledges it, never merely because a retry was
  attempted. Live homeserver validation: NOT TESTED

### Settings, usability, and accessibility

- **Media browser tiles fetch through the media registry** (2026-09-05).
  The history scanner registers every attachment's sources under its event
  id — the same `StoredMedia` record and key the timeline registers for its
  own rows (`mediahistory::stored_media_from_event`, `TimelineRegistry::
  remember_media`) — and the row carries `mediaKey`, which the tile hands to
  `mediaBridge.mediaSource(key, "list_thumb")`. That is the only path that
  can decrypt an encrypted attachment's thumbnail: the old tile asked the
  server to thumbnail an encrypted mxc, which is impossible, and every tile
  in an encrypted room failed with `category=network` (reported with a
  screenshot from the 0.9.0 AppImage and reproduced in the source build).
  The plain mxc route stays for a row the scanner could not register.
- **A custom display-name colour** (2026-09-05): a tenth swatch beside the
  nine theme inks opens the theme editor's `ColorPickerPanel` inline; the
  picker reports every drag step, so nothing is written until **Apply**, the
  one `setOwnColor` call. Readers still see it through
  `IdentityPalette.legibleChoice`, adjusted for their theme.
- **Settings is built once and kept** (2026-09-05). `Main.qml`'s loader used
  to follow the current screen, so the 7,000-line screen was instantiated on
  every open and destroyed on every close — measured with
  `LIGHTNING_GUI_STALL_TRACE=100` as a 428 ms GUI-thread block per open,
  reported as "takes like a second". The first build sticks (`warm`), a close
  merely hides it, and the first build is started asynchronously a moment
  after the main screen shows. A section requested while the screen is alive
  (`showSettingsSection`) reaches it through `settingsSectionRequested`,
  since `Component.onCompleted` now runs once. The settings header is
  `AppTheme.headerBandHeight`, the same 60 px as the room header it replaces
  on screen, so opening Settings no longer changes the top band's height.
- **Read receipts are hosted by the nearest row that draws a body.** The
  SDK attaches a reader's receipt to the newest event they read, which
  during a call is a call-membership update — a row that draws nothing —
  and the chips vanished (reported: "when call event read receipts
  disappear"). `TimelineModel::rowHostsReceipts` decides who draws (a
  message, a call card, or a state-run leader with at least one entry the
  activity settings show); `receiptHostRow` walks up to that row, and its
  ReadReceipts roles merge every row it hosts, one entry per reader, newest
  first, with the host re-announced whenever a hosted row changes. The strip
  itself now lives outside MessageDelegate's message-only layout so call
  cards and run leaders can paint it. Regression: `timeline-model-diff`.
- **The encryption lock sits beside the room name** (non-fill label under a
  half-header cap), and the call popout (`CallPipWindow.qml`) hosts the
  stage's `CallTileGrid` — every participant a tile, every screen share a
  tile of its own — instead of one chosen surface.
- **A participant volume chosen before their track arrives lands when it
  does.** `SfuMediaEngine` remembers the wanted value per stream/track key
  and applies it when the receive bin is built; the "nowhere to land"
  diagnostic fires once per key instead of per attempt (a live log carried
  hundreds). Regression: `sfu-media-engine`
  `aVolumeChosenBeforeTheTrackArrivesLandsWhenItDoes`. Whether this is the
  whole of the Windows "volume 0 does not mute" report is NOT TESTED live.
- **Settings keeps its state between opens** (last section, search text)
  and its two window-level Shortcuts are enabled ONLY while it is visible —
  found in review: kept alive, an unconditional Escape would have made
  Qt's ambiguous-shortcut rule swallow Escape on the main screen. Backup
  progress and profile banners are re-requested on every open, not once at
  creation.
- **Widgets: Remove names the exact state key** (`stateKey` on the row) and
  is offered only for rows the reader could name exactly and that carry the
  `im.vector.modular.widgets` type (`removable`); a failed write is shown in
  the tab (`lastWriteError`); a room switch resets the write and the
  permission claim. The Rust write validation is one function,
  `validate_widget_write`, pinned by its test.
- **Room information tabs wrap.** The tab strip is kept as one row while it
  fits and becomes two rows (split at the midpoint, one shared `current`)
  when it overflows — driven by the strip's own `overflowing`, which only
  works because the strip's `Layout.minimumWidth` is pinned to 0: a RowLayout
  of non-fill children reports their SUM as its minimum, the panel inherited
  it, and TimelinePane's row pushed the whole panel off the window instead of
  narrowing it (reported as "widgets go off screen").
- **Light themes are not pure white anywhere.** Lightning Light's and Moss
  Light's card surface (`_cardLight`, `_mosCard`) carry a whisper of their own
  hue (`#F5F9FE`, `#F6FBF7`) — the search field, composer bar, status strip
  and the Classic card were the only `#FFFFFF` on a tinted canvas.
- **The GIF picker re-runs the typed search when the provider changes**;
  it used to fall back to trending until a keystroke.
- **The Home "no key backup" banner** says "Set up backup" and opens the
  Sessions section, which is where backup is set up; it opened Privacy.
- Eleven complete semantic themes (ids 1–11): Lightning Light, Lightning
  Dark, Graphite, Midnight, Nordic, Purple Dusk, Warm, the design-handoff
  Moss Light / Indigo Night / Deep Teal, and Storm (11), the brand theme.
  **Indigo Night is the flagship** (2026-08-25, maintainer's call): it leads
  the featured cards and System (0) resolves to Moss Light / Indigo Night.
  Storm stays a featured card, fourth, and stays the shell's own chrome. An
  explicitly persisted id is never rerouted, so changing what System means
  touches nobody's stored choice. The identity discs damp the magenta wedge
  (290-350 degrees, saturation x0.55) so a cool accent stops producing pink
  fallback avatars; hues are unmoved, so per-theme families and the dE 19.7
  all-pairs separation are unchanged.
  AppTheme.qml is the sole token source; the theme test enforces palette
  completeness, routing, and WCAG AA pairs. The storm* namespace (menus,
  popovers, Settings) is theme-ROUTED: Storm literals under theme 11, each
  legacy theme's own semantic tones otherwise. There is NO invariant
  exception left — the Sessions trust card was the last one, and 2026-08-26
  deleted its ten pinned `trust*` tokens and routed it here too, because a
  brand-navy card sitting between themed SettingsCards was the one surface
  on the page that read as foreign. Ink on a bolt/accent fill uses boltInk,
  never stormPanel — the trust card's complete-node glyph was the case that
  proves it: it only looked right as `trustNavy` because the pinned fill
  happened to be navy, and routed unchanged it would have painted the page
  ground onto a yellow disc
- The four-pane design shell: 68 px spaces rail (home, Spaces, settings,
  account avatar + switcher popover), 300 px room list with workspace header
  and Ctrl-K hint, timeline with members/threads side panel, card composer;
  bundled Manrope/JetBrains Mono fonts; application icon and desktop entry
  installed by CMake (data/, scripts/generate-icons.sh)
- The full-view Settings screen covers the whole content area (chat shell
  loaded but hidden — no rail, room list, timeline, composer or right panel
  while open; closing restores the selected room with the right panel
  remaining None), 60 px header above 260 px navigation (Account,
  Appearance, Notifications, Privacy & security, Sessions, Labs; About
  pinned bottom). Appearance carries featured theme cards, a match-system
  switch, a FUNCTIONAL message-layout selector (Modern / Bubbles for DMs /
  Compact) and a text-size slider (90-140%) — theme, layout and text scale
  persist per account with a global fallback. Avatar shapes are baked into
  the cached bitmap by MediaImageProvider ("|shape:" suffix) instead of
  per-item MultiEffect masks. Headless/offscreen runs force stderr logging
  in main.cpp because Qt otherwise routes category logs to the journal when
  stderr is no TTY
- Room-activity visibility, link/GIF preview policy, notification privacy
  and sound, per-room notification mode, and wheel-speed settings
- The media autoplay control is labelled **"Autoplay and prefetch media"**
  because that is what it governs: GIF animation, the picker's autoplay, AND
  the speculative video/audio prefetch. The stored key stays `gif/autoplay`
  and the property stays `gifAutoplay` **ON PURPOSE** — renaming the key
  would silently reset every existing user's preference, which is worse than
  a stale identifier
- **Pre-send upload-limit preflight** against the homeserver's advertised
  `m.upload.size` ONLY; both fabricated 100 MiB ceilings are gone (the Rust
  one reported an invented value as though the server had advertised it).
  **0 means UNKNOWN** — never "unlimited", never replaced by a client
  default — and suppresses local rejection entirely rather than refusing
  files the server would have accepted. Voice messages share
  `AttachmentQueueModel::exceedsUploadLimit` so the check cannot drift
  between composers. **Exactly-at-limit is allowed** (`>`, not `>=`):
  `m.upload.size` is the largest ACCEPTED payload. Consequence: with no
  advertised limit there is no client-side ceiling at all; re-adding a bound
  would have to be worded plainly as a CLIENT safety limit, never presented
  as the server's
- **Send failures are scoped to where they happened.**
  `onAttachmentQueueFinished` received a `roomId` and discarded it, so a
  late voice-send failure surfaced over whatever room the composer had since
  moved to. Ops now carry their target room (and thread root). Cleanup of
  the recording stays UNCONDITIONAL so nothing is orphaned on disk; only the
  NOTICE is scoped — deliberately not the same decision
- Unicode emoji picker with search, tones, and bounded local recents
- Keyboard quick switch/search/navigation, accessible labels/roles/actions,
  focus handling, and keyboard-operable message/thread actions

### Display-name colours

Every name in the client is coloured from a nine-slot family DERIVED FROM THE
ACTIVE THEME, and a user may override their own with a colour other Lightning
clients see.

* **Derived, not tabled.** `lightning::theme::nameInk` builds the family from
  the theme's own anchor and solves each ink against that theme's real
  grounds (background, card, elevated card, other-party bubble) to 4.5:1. It
  lives in C++ beside the avatar-disc arithmetic so the two cannot drift, and
  so the desktop-notification painter can reach it with no QML engine. Custom
  themes get real colours from their own two colours.
* **The inks spend 340 degrees of the wheel where the discs spend 190**, and
  that is measured rather than chosen: nine text inks are not separable in
  190 degrees (worst pair dE 3.3), and no lightness pattern rescues it. A
  filled disc can afford a tight family; thin text cannot. The family stays
  centred on the anchor, which is what makes it the theme's.
* **The separation floor is dE 9.0**, below the 12 the old hand-picked tables
  met. That is the cost of matching the theme — those tables cleared 18
  because they walked 321 degrees and belonged to no theme.
* **A user-chosen colour** lives in the Matrix profile as
  `org.lightning.name_color` over MSC4133 extended profile fields, so it is
  global, readable by anyone who can see the profile, and changed in one
  write. Account data would be private to one account; a state event would
  mean a different colour per room and a write to every room per change.
* **The choice is clamped, not obeyed.** `legibleChoice()` keeps the hue and
  saturation exactly and moves only lightness, far enough to clear 4.5:1 on
  the viewer's worst ground. A colour already legible there is returned
  untouched. The field is written by its owner and read by everybody else, so
  painting it verbatim would let anyone hand every other user a name they
  cannot read on a theme the sender has never seen.
* **Validated twice** — in Rust leaving the profile field, in C++ reaching a
  QML colour property — because that is remote text becoming a paint
  instruction. Anything that is not exactly `#rrggbb` is dropped, never
  repaired.
* **One fetch per user per session.** `colorFor()` is called from a binding
  that re-evaluates for every name on screen, so the guard is set when the
  request is SENT; "this user has no colour" is stored as an ANSWER so the
  commonest case is not re-asked forever. A homeserver without extended
  profile fields stops the asking and hides the control.

LIVE-VALIDATED against `matrix.smetonis.net`: picking a swatch wrote
`{"org.lightning.name_color":"#fbe7ed"}` to the real profile field; and a
colour set on a SECOND account's profile rendered on that account's name in
the first account's client, which is the cross-user claim.

### Mention pills and unknown users (2026-09-05)

- A `matrix.to` user link renders as a pill whose label is, in order: the
  room's own member name for that user, the user's global profile display
  name (`app.userProfiles`, asked once per user per session), and the bare
  localpart until one of those exists. The text the sender wrote inside the
  anchor is never the label: a pill reading "@admin" that links to another
  user is a spoof, and the localpart fallback is what prevents it.
- `app.userProfiles` (`UserProfileResolver`) answers `lookup(userId)` from a
  session cache and asks the homeserver's `/profile` once per unknown user;
  a refused answer is remembered and re-asked only after five minutes. Its
  `resolved` signal re-renders exactly the timeline rows whose pill used that
  user, through the per-event name record every render keeps.
- The member profile popover fills from the room roster first and from the
  resolver second, so a mention of someone who is not in the room opens with
  their name and picture. A room nick still wins where one exists.
- Member hydration (`membersChanged`) re-announces the identity roles on every
  row and the BODY roles only on rows whose recorded names changed; a
  hydration that changes nothing re-renders nothing.

### Media rows and the viewport band (2026-09-05)

- An image row asks the media bridge for its picture only while it lies inside
  the pane's media band: content-coordinate bounds published by
  `TimelinePane` (2.5 viewports towards the newest end, 1.5 towards history)
  and moved only at load, model reset, viewport resize, and the settle after a
  gesture. A row entering the band asks then; a row that already holds its
  picture is left alone. Fixtures and the thread panel, which publish no
  band, are permissive.

### The hover action bar and the call popout (2026-09-05)

- The message hover bar offers Edit (before More) under exactly the context
  menu's gate — own, editable, not a local echo — and starts the composer's
  edit for that message.
- The call popout offers "Fill this window with the share": the spotlighted
  (or first) share alone, edge to edge, the tile grid hidden; the same button
  restores everyone, and the mode drops on its own when the share ends. It is
  local to the popout window and does not touch the stage's spotlight.

### Local message search

Server search (`POST /_matrix/client/v3/search`) can only search what the
SERVER can read, so it returns nothing for an encrypted room — which for most
people is most of their conversations. The local index is the other half: a
SQLite FTS5 database of the plaintext this client already holds, so an
encrypted room searches exactly like a public one.

It does not introduce decrypted text to disk. The SDK's own event cache already
persists it (`encode_event` serializes the whole `TimelineEvent` including its
`Decrypted` variant, `encode_value` is a no-op with no cypher configured, and
Lightning opens `sqlite_store(path, None)`), so the index is a second copy of
something already present, in a form that is faster to query. **Encrypting the
store at rest is a separate, open decision** — `sqlite_store` builds ONE config
for the state, event-cache, media and crypto stores, and matrix-sdk-sqlite
mints a new cipher when it finds none, so a passphrase would leave every
existing install unable to decode its own account pickle.

* **The index lives in the account's own store directory**, so it is deleted
  with the account and inherits the same 0700 protection as the SDK store.
* **`bundled-sqlite` adds nothing to package.** It compiles the amalgamation
  into the Rust staticlib, so SQLite is linked STATICALLY — measured on the
  built binary: `ldd` names no `libsqlite3`, and 430 FTS5 symbols are present.
  No packaging list on any platform needs a new entry, which is the question
  a per-platform shared-library dependency would otherwise have raised
  thirty minutes into a release pipeline.
* **Tokenizer: trigram**, because `unicode61` cannot segment CJK — a Chinese
  sentence becomes one token and nothing inside it is findable, a silent total
  failure for a language Lightning ships. trigram's cost is a hard
  three-character minimum, reported as its own state so the user can act on it
  rather than reading "no results". `remove_diacritics 2` folds case and
  accents on both the stored text and the query.
* **The query is text, not a language.** FTS5 has operators; a user typing
  `AND` or a quotation mark means the characters. The whole query becomes one
  quoted phrase.
* **Edits overwrite, redactions delete.** An `m.replace` is filed on the event
  it REPLACES using `m.new_content`, never under its own id with the
  "* fallback" body. A redacted message is removed outright — leaving it
  findable by its own text would be the worst thing this index could do.
* **Fed from the event cache**, never the live timeline (which exists only for
  the open room). A sweep runs on sync and every five minutes; "Index this
  room" pages history in, bounded at 50 pages, indexing after EVERY page
  because `events()` returns the in-memory chunk and the history trim shrinks
  it back.
* **Bounded at 250,000 rows**, evicting oldest first.
* The find bar prefers local and offers server as an explicit CHOICE where the
  server can read the room — they answer different questions, and switching
  silently would make "no results" mean two things on consecutive keystrokes.
  The coverage line says how many messages are being searched.

Live-validated against a real homeserver, including a Megolm-encrypted message
this client sent and then found.

### Widgets

See `docs/widgets.md` for the whole contract and the evidence behind it.
Lightning LISTS a room's widgets, validates their addresses, discloses what
each will learn about the user, and opens it in the user's BROWSER. It does not
embed them: Windows cannot build Qt WebEngine, Flatpak could only ship Chromium
unsandboxed beside Megolm keys, and initialising it would force the whole
application's scenegraph to OpenGL.

Since 0.9.0 it also ADDS and REMOVES them (`docs/widgets.md`, "Adding and
removing"): the Widgets tab offers **Add widget…** and a per-row **Remove**
only when the SDK's `can_send_state` for `im.vector.modular.widgets` says so
(`can_manage` on every `room_widgets` answer, never inferred from a role).
Adding writes Element's event shape under a fresh UUID state key; removing
writes an empty object, which is Element's tombstone and what the reader
already reads as "no widget". A `url` that is not https with a host and no
credentials is refused in the dialog AND in `write_room_widget`, so a widget
this client would refuse to open is never published. Success re-reads the
list rather than applying optimistically.

### Navigating a room's history

- **Jump to first unread.** The SDK places a read-marker virtual row from
  `m.fully_read` (which Lightning writes with every read receipt) and
  MessageDelegate draws it as the "New messages" divider; the pill at the TOP
  of the timeline scrolls to it. THE MARKER HAS NO EVENT ID, so this is a ROW
  (`TimelineModel::firstUnreadRow`, NOTIFY countChanged — a virtual row can
  only move by being inserted or removed) handed to
  `beginNavigationLanding()`, which holds a target by stable id across
  paginations landing while it waits. Offered only when the marker is loaded;
  a reader further back than the window pages toward it, bounded at 8 pages
  and one per 240 ms, and the reader taking hold of the view cancels the hunt.
- **Jump to date** (MSC3030 `timestamp_to_event`, stable since Matrix 1.6),
  from the find bar. FORWARD from local midnight of the chosen day, so a date
  lands on its FIRST message; a day with no messages lands on the next message
  after it rather than refusing to move. There is deliberately NO client-side
  fallback — paginating backwards until the dates look right is an unbounded
  walk through a room's whole history to answer a question one request
  answers, and it would be slowest in exactly the rooms the feature is for. A
  homeserver without the endpoint arrives as `not_found` and the dialog says
  so; it STAYS OPEN until the answer arrives, because a dialog that closes on
  click and then does nothing is the failure this surface exists to avoid.
  An answer whose room is no longer the open one is dropped as `stale`.
- **Mark all rooms read**, on the rail's Home menu beside the Space tile's own
  scoped version. `RoomListModel::markAllRoomsRead()` — that model owns "mark
  a room read", so it owns marking them all. Only rooms that are actually
  unread; invites are skipped (a decision, not unread mail), `markedUnread` is
  skipped (the user's own "leave this for later"), and Spaces are skipped (a
  room with no timeline). It clears the bell too, because the receipt it sends
  per room is what `ActivityModel::markRoomReadUpTo` listens for.
- **"Other rooms" is a CLASSIC tile.** It narrows a Home that shows
  everything, which is Classic's Home; Channels' Home already lists exactly
  the rooms in no Space, so the two opened the same page and the tile is not
  offered there (`RailEntryModel::orphansEntryVisible`, the mirror of
  `peopleEntryVisible`, selection rescue included).

### Export a room

Writes the room's LOADED timeline to a file the user picks, as plain text or
JSON. It does not paginate: walking a room's whole history to build a file is
an unbounded job whose only honest progress report is "still going", and a
partial export presented as a complete one is a lie about a conversation. The
count is on screen before the user picks a file and repeated inside the file,
because the person who reads it later may not be the person who made it.

NO MEDIA. An attachment exports as its filename and type with a sentence
saying the bytes are absent, and never as an `mxc:` — a reader of the file
cannot resolve one without the account's token, so printing it offers a
live-looking dead link.

**This is the one place in Lightning that writes encrypted-room plaintext to
disk, and it is an explicit user-chosen exception to §6, not a hole.** The
checkbox is off by default and worded as a consequence rather than an option
("Write the message text into this file in the clear"); declining still
produces a usable export with every body replaced by a withheld marker, so the
shape of the conversation survives and none of its text does. An UNKNOWN
encryption state counts as encrypted, the same way the draft store fails
closed. Nothing else is relaxed: `CacheStore` still refuses encrypted rows and
this path never writes to a cache.

The renderers are pure (events in, string out), so the whole shape of the file
is unit-tested without a filesystem and the one place that touches disk is
four lines. The suggested filename is a LEAF — a room name is chosen by
somebody else and this string is handed to a file dialog.

### The message box (composer) controls

Left to right: attach, formatting, the text field, emoji, GIFs and stickers,
voice message, send, send options. **2026-09-03, from tester feedback:**

- **One button for GIFs and stickers, and one window.** They were two buttons
  opening two popups. The two pickers are still two components — a pack is not
  a GIF: it has an owner, an attribution, a room it may belong to, and failure
  states (no packs, a pack of emoticons only) the GIF grid has no words for —
  and the merge is that both now carry the same GIFs/Stickers strip and the
  HOST swaps them in place. They already shared the anchor, the chrome and the
  remembered size (`sizeSettingsKey: "picker"`), and neither declares an enter
  or exit transition, so the swap reads as the window changing tab. A picker
  never opens its sibling itself: it emits `kindRequested` and the composer
  that owns both does it, because only the host knows its own anchor item.
  When only one kind is available the strip is absent; when NEITHER is, the
  button stays present and disabled with a tooltip that says why.
- **A chevron on the right of Send, not a clock in the glyph row.** "Send
  later" was an unrelated-looking icon among emoji and GIF, and it vanished
  entirely in a narrow window with no menu entry standing in for it. The
  split-button shape says "another way to send THIS", and it rides beside a
  button that is always present. The menu carries Send later (available only
  with something to send) and Scheduled messages (always, with the room's
  pending count). The scheduler itself is unchanged — see "Send later".
- **The buttons can be switched off**, in Settings › Appearance › Message box
  › Message box buttons: formatting, emoji, GIFs and stickers, voice message,
  send options. **Attach is deliberately not offered**: it carries files and
  polls, and in a narrow window the emoji and media actions are displaced INTO
  its menu, so hiding it could stand between the user and an action the user
  had not hidden. Stored as `SettingsManager::hiddenComposerButtons`, a list
  of what is HIDDEN rather than what is shown, so a button added in a later
  release appears for everyone instead of being hidden from every existing
  user. Both composers honour it; a recording in flight keeps its controls
  whatever the setting says, because the pill is the way to stop it.

A picker button TOGGLES. The pickers carry `Popup.CloseOnPressOutside` and the
icon that opens them is outside, so a second press closed the panel and the
button's own click — which arrives on the RELEASE — opened it again. The popup
layer sees the press first, so `opened` already reads false in `onClicked`;
what identifies the gesture is that a panel of that kind was dismissed a
moment ago and the very next thing is a click on its own button.

### GIF provider integration

Implemented: strict GIPHY and KLIPY parsing behind a shared provider
interface (provider-specific endpoint/key/rating/pagination, attribution), a
provider-agnostic search controller and result model with stale-response
rejection and deduplication, and bounded redirect-validated HTTPS transport
through the Rust backend. The user-facing browser is implemented too: shared
room/thread picker with provider tabs, trending, debounced search,
client-side categories, pagination, attribution, favorites, bounded local
recents, safe-search rating, configurable autoplay, accessible
keyboard-navigable tiles. The safe validated download pipeline (HTTPS-only,
revalidated redirects, bounded size, GIF magic and dimension validation) and
the send path into a room or a real Matrix thread — uploading through the SDK
media path, with SDK media encryption in encrypted rooms — are implemented.
Existing GIF attachment/direct-media playback remains separate and
implemented. Live Element interop of provider GIF sends: to be tested
honestly rather than assumed.

**Saving GIFs** is implemented; the star accepts every safe static raster the
timeline shows (GIF, PNG, JPEG, WebP). Bytes are validated by magic sniffing
— never a claimed MIME or file name; SVG and everything else refused —
stored in their ORIGINAL format as `<sha256>.<ext>` (no transcoding), and
re-sent with a truthful MIME and dimensions. Legacy index entries without a
format field load as GIF, so existing saved GIFs survive with no migration.
The store is account-scoped and content-addressed, bounded at 200 items /
64 MiB by **refusal, never eviction** — a full store must not silently
discard what the user asked to keep. Sends go from local bytes.

A star means exactly one thing everywhere — "save this GIF" — with one
destination: the picker's **Saved** tab, which renders `GifSavedModel`, a
presentation-only `QConcatenateTablesProxyModel` merge of the local byte
store and the provider favorites. The two **stores stay separate**, because
only one of them holds decrypted media. Each tile carries its own source tag
(GIPHY/KLIPY/LOCAL). Saved and Recent issue **no provider API request** — no
search, trending, pagination or category call is reachable from either — but
they are not offline: a saved *provider bookmark* is a link, so its tile
still loads its preview from that provider's CDN. Only locally-saved rows
are pure device-local content; do not describe the Saved tab as having "no
provider traffic".

Never read `GifResultModel::FavoriteRole` from a `GifStoredModel` as an "is
this saved" oracle: that role is a constant `true` for every stored
collection — honest for favorites and local-saved rows, a lie for Recents.
Ask the collection (`GifFavoritesModel::isFavorite`).

This is a deliberate, documented exception to the §6 rule against persisting
decrypted media, on explicit-export semantics: the user chooses to save one
image, exactly as Save-As already allows. It is only defensible because
deletion is REAL — the store is removed on sign-out and on account removal
through a shared path helper with tri-state deleted/absent/failed reporting
(an earlier version *claimed* this cleanup and did not have it; decrypted
media would have survived sign-out indefinitely). Settings → Privacy &
security discloses the store and offers Clear All. The index records **no
provenance**: no room, event, or sender. Do not weaken any of that, and do
not extend the exception to other media without the same guarantees.


### Stickers and custom emoji (MSC2545 image packs)

Lightning invents NO storage format here. The three events are MSC2545's own,
transcribed from ruma's `ruma_events::image_pack` (ruma-events 0.34.0) without
enabling its `unstable-msc2545` feature — that would mean taking ruma-events as
a DIRECT dependency, and dependencies are lock-file controlled. All of it lives
in `rust/src/stickers.rs`:

- **`im.ponies.user_emotes`** — global account data, the account's own pack.
- **`im.ponies.room_emotes`** — ROOM STATE, and **the state key IS the pack
  id**, so one room may publish several packs; the empty state key is the
  room's default pack.
- **`im.ponies.emote_rooms`** — global account data,
  `{ "rooms": { room_id: { state_key: {} } } }`, selecting which ROOM packs are
  active outside their own room. A room's packs are always usable INSIDE that
  room whatever this holds.

**A pack is remote, author-chosen content and is validated in Rust before it
crosses the FFI.** A `url` that is not a syntactically valid `mxc://` is
DROPPED, not merely unrendered — an `https://` on a picker tile is a beacon
that fires once per pack listing. A DECLARED `info.mimetype` outside the five
raster types is refused (`image/svg+xml` above all — §6); an ABSENT one is
UNKNOWN, which is a different fact, so it passes and the bytes are sniffed on
arrival. Shortcodes are repaired to the MSC's own `[a-zA-Z0-9-_]` alphabet
rather than rejected (packs in the wild carry illegal ones, and dropping them
would make another client's pack look empty); bodies, pack names and
attribution are stripped of control characters and bounded. Pack and image
counts are capped. Everything that crosses is a LABEL and is never rendered as
rich text.

**Sending** is an `m.sticker` carrying the pack's own `mxc`
(`mx_rust_stickers_send` → the SDK timeline, so local echo, send queue and
Retry all work, and the SDK attaches any `m.thread` relation — §8). There is no
uploader: a pack image is already Matrix media. **Stated plainly rather than
glossed: in an encrypted room the EVENT is SDK-encrypted like every other
event, but the BITMAP is ordinary unencrypted media, because that is what a
shared pack IS.** Inherent to the MSC, true of every client that implements it,
and the reason a pack sticker is never presented as private content. A thread
send has NO room fallback.

**"Add to my stickers"** writes one image into `im.ponies.user_emotes`
(read-modify-write against the SERVER copy, so another device's edit is not
clobbered). Taken from Sable's PR #107: the destination pack, dedupe by MXC,
`usage: ["sticker"]`. Deliberately DIVERGED: the shortcode comes from the
sticker's BODY sanitized to the MSC's alphabet, where Sable uses
`sticker-$eventId` — illegal under the MSC twice over and unusable in another
client's `:shortcode:` completion; a name collision gets a numeric suffix; a
duplicate is refused in Rust rather than only hidden in the UI. It is NOT gated
on "already saved" (that needs a fetched pack, and greying the row out because
nothing LOOKED is the worse lie); an ENCRYPTED sticker has an `EncryptedFile`
and no mxc, so it structurally cannot go in a pack and the action is absent.

**"Add to this room's stickers"** is the one write that is ROOM STATE, so it is
POWER-LEVEL GATED on the room's OWN required level for `im.ponies.room_emotes`,
asked of the SDK (`can_send_state`) — never a role label, FALSE when unknown,
checked in Rust before anything is sent. That permission is reported on the
SNAPSHOT (`room_can_manage`), not per pack, because a room with no pack yet has
no pack row to carry it and its first pack could otherwise never be created.
The action is ABSENT rather than greyed when the permission is unknown — the
opposite decision from the account-pack row above, deliberately, since the
account-pack row still works and the sticker is never unsaveable. Both writers
share ONE transform (`add_image_to_pack_content`), so duplicate policy, the 512
cap and the collision rule cannot drift; a room pack is never renamed by an add
(MSC2545 defaults a nameless room pack to the ROOM's name).

Nothing is applied optimistically anywhere: every write completes, the
authoritative snapshot is re-read, and a refusal cannot leave a surface showing
state the account does not have.

**The picker is its own popup** (`qml/StickerPicker.qml`), not a tab on the
emoji picker: a pack is remote content with an owner, an attribution and a room
it may belong to, and its failure modes need words the emoji grid has no place
for. It follows GifPicker's chrome, remembered size, press-sink and one-shot
activation latch. Its sticker button's glyph is `emoji_symbols` only because
the bundled Material Symbols font is a SUBSET and every mapped name is already
spoken for; swap it when the subset can be rebuilt.

**Nothing polls.** A refresh costs one global-account-data read plus a bounded
`/state` read per room pack, so it happens when a surface asks (the picker on
open, the reaction picker on open) and after the account's own pack is written.
Room navigation only MARKS the snapshot stale.

**Custom emoji** are the same packs' `emoticon`-usage images. Implemented as
REACTIONS: an `m.reaction` whose key is the image's `mxc://` — the convention
read out of Sable's own `Reaction.tsx`, not inferred. A reaction chip whose key
is a plain mxc renders the image; its accessible name is the shortcode when a
pack is loaded and "custom emoji" otherwise, never a raw mxc. The reaction
picker gains a "Custom emoji" strip; composer mode does not, and the strip
never records into the Unicode recents.

**Custom emoji INSIDE a message body are IMPLEMENTED (v0.9.0).** Both halves
landed in one round, because sending before the receive side existed would
have emitted messages this client could not display.

Wire format: `formatted_body` carries
`<img data-mx-emoticon src="mxc://…" alt=":code:" height="20" width="20" />`
and the plain `body` carries `:code:`.

Three things that are not obvious and cost time to find:

* **matrix-sdk-ui sanitises INCOMING html itself**, with a hard-coded
  `HtmlSanitizerMode::Compat` `const` that is not configurable, and it strips
  `data-mx-emoticon`. Lightning therefore reads the RAW event's
  `formatted_body` rather than the SDK's cleaned copy
  (`restore_raw_formatted_body`), and must call it AFTER
  `fill_message_content` — an earlier ordering made its guard always return
  early and the whole feature silently did nothing.
* **Allowing that attribute DISABLES ruma's own img-src scheme check.** Its
  loop returns on the first attribute with no scheme rules, so permitting
  `data-mx-emoticon` stops it ever reaching `src`. mxc-only is therefore
  enforced by Lightning's own `strip_non_mxc_images`, not by the sanitizer.
* **The `<img>` is REBUILT from validated parts**, never passed through: it
  is allowed only when it carries BOTH `data-mx-emoticon` and an `mxc://`
  src, and `height`/`width` are forced to 20. A pass-through would let a
  sender choose the dimensions and paint over the surrounding message.

Shortcode completion (`:blob` → the installed packs' matches) is
`EmojiCompletionPopup.qml`, driven by `MessageComposer::emojiCompletionsAt`
— CURSOR-driven rather than a NOTIFY property, because a shortcode can be
anywhere in the text while a slash command is always at position 0.

**Pack management (v0.9.0):** remove an image, rename its shortcode, rename
the pack, empty the pack — `StickerPackEditor.qml`, reached from the picker.
One `PackEdit` enum and one shared writer serve BOTH the account pack
(account data) and a room pack (room state, power-level gated exactly as
adding is). Decisions worth keeping:

* Removing the last image leaves an EMPTY pack that keeps its name; deleting
  the pack drops the name too. MSC2545 says a room pack with no
  `display_name` falls back to the ROOM's name, so keeping an empty pack's
  name is what stops "I removed my last sticker" from renaming it.
* A rename COLLISION is refused, not suffixed. `add` may invent `blob-2`
  because any name will do for something new; someone deliberately renaming
  meant the name they typed.
* Renaming to the SAME name succeeds — the pack ends how the user asked.
* An empty pack name is SENT, not refused: clearing it is a real state, and
  a local guard would make the room-name fallback unreachable.
* A rename is ONE transform, not remove-then-add: a remove that succeeded
  followed by an add that failed would delete the image being renamed.

Live validation: the SEND and RECEIVE of inline custom emoji, and shortcode
completion, are **PASS** (a real `im.ponies.user_emotes` pack, 2026-09-05).
Pack management is **NOT TESTED** on screen.

### Browsing a room's media, files and links

`Room Information → Media`. Walks the room's WHOLE accessible history on its
own `/messages` cursor (`rust/src/mediahistory.rs`), never the timeline's
loaded window — the tab it replaced showed whatever the timeline happened to
hold, so finding an image from March meant scrolling the conversation back to
March.

**Completeness is SHOWN, not implied.** The strip under the toolbar always
says how much history has been examined and whether the start was reached.
"No images" after 60 events and "no images in 12,000 events, all of history"
are different answers, and a browser rendering both as an empty grid is lying
about the second. `undecryptableCount` is the third state: history that
exists and cannot be read, which in an encrypted room would otherwise look
like less media.

A link event contributes one row PER URL, so the event id alone cannot be the
identity — the (event, url) pair is. Pages can overlap at a boundary, which
is what the dedupe is for. A page that matched nothing is NOT completeness,
and a FAILED page never is: it leaves the rest of history unknown, so
reporting it as complete would turn a server error into "that is everything".

Categories are a `Flow`, not a horizontal scroller: this lives in a side
panel whose width the user controls, and a scroller hid Files and Links
behind a gesture nobody would guess was there.

Live validation: **PASS** (2026-09-05). Three defects only GUI testing found:
an `"op"`/`"op_id"` payload-key mismatch that meant pages never arrived;
`available` never re-emitted after login, so the browser said "cannot browse
room history" forever; and the initial category never pushed to the model
(`onCategoryChanged` fires only on a CHANGE), so the Media tab listed files
and links.

### Forwarding a selection

Multi-select in the timeline, multi-destination picker
(`ForwardSelectionDialog.qml`). N messages to M destinations, with two modes:
"just the message" (a clean copy) and "with original sender" (a copy naming
the sender, the source room and the time). The mode is a CONSCIOUS choice and
the dialog says what it discloses, because that context goes to whoever
receives the copy — who may not be in the source room.

The dialog stays open through the send and reports PER PAIR, with a retry for
the failures. N×M sends can partially fail, and "sent" because one of twelve
worked is a lie the user would act on. Capped at 50 messages.

Attachments in a bulk selection are REPORTED as needing the single-message
path rather than silently skipped: unbounded parallel uploads are forbidden
by design.

Live validation: **PASS** (2026-09-05, 2 messages × 2 rooms = "Sent 4
copies", both rooms verified server-side).

### Per-room profiles

`Room Information → Your profile in this room`. A display name and avatar
that apply in ONE room.

The display name uses the SDK's own `Room::set_own_member_display_name`.
There is **no avatar equivalent**, so the avatar is a RAW read-modify-write
of the member event that preserves `join_authorised_via_users_server` (note
the JSON spelling, with the s), `blurhash`, `reason`, `is_direct` and
`third_party_invite` — dropping any of them would rewrite state the server
put there.

It uploads through `client.media().upload()`, **never** `upload_avatar`,
which would rewrite the account's GLOBAL avatar — the exact opposite of what
a per-room override is for. Refuses a membership that is not `join`, and a
stripped one.

Live validation: **PASS** (2026-09-05, verified server-side: the override
applied in one room while another kept the global name).

### Desktop integration (notification actions, call keys, picture-in-picture)

**Notification actions.** Mark as read, and an inline Reply where the daemon
advertises `inline-reply` (queried in the same `GetCapabilities` round trip
as `body-markup`). Offered only on a card with a real event id, so an invite
— which cannot be marked read and cannot be replied into — is not given two
buttons that fail.

**A notification card OUTLIVES the account that raised it.** The user can
switch accounts, or sign out, while it is on screen, and the desktop
delivers the action minutes later. Acting under whichever account is current
would mark another account's room read, or send a reply from the wrong
identity into a room the current account may not even be in — and it would
SUCCEED, so nothing would report it. Every payload is stamped with the
account it was raised for, and a mismatch is refused with a notice that says
why rather than silently switching who you are signed in as.

`inline-reply` arrives in TWO parts on some daemons (ActionInvoked, then
NotificationReplied), so the payload survives the first and is consumed by
the second. A threaded message is answered in its thread.

**Encrypted rooms get their own preview level**, defaulting to "same as other
rooms". A notification body is written to the desktop daemon and its log in
plaintext, outside everything the room's encryption guarantees. When
encryption is not yet KNOWN — a real state during hydration — the STRICTER of
the two wins: guessing "unencrypted" would put a body on screen the user
asked to withhold, and nobody would learn it had happened.

**Mute and deafen keys** are window-global (Ctrl+Shift+U, Ctrl+Shift+H) and
follow the same lane selection the call bar's buttons use. Discord's own keys
were both unavailable: Ctrl+Shift+M is `room.markRead` and Ctrl+Shift+D is in
the reserved table. They are inert outside a call rather than absent — a key
that quietly does nothing is better than one that takes the sequence away
from something else while a call is up.

**Picture-in-picture** (`CallPipWindow.qml`) is a small always-on-top window
opened from the call bar, and — only when the "automatic pop-out" setting is
on, which it is NOT by default since 2026-09-05 — opened by itself while the
main window is minimised or in the tray. Shipped on, it popped the call out
on every minimise, a window the reader had not asked for. It is a REPLACEMENT, never a duplicate:
`SfuVideoRouter` holds ONE sink per track and the last attach owns it, so two
surfaces on the same participant means one goes black. Hence the flag is
mutually exclusive with full screen in `CallStageState`, the window's
surfaces are built only while it is really showing, and its tiles are
Repeaters over the LIVE models rather than `get(row)` snapshots (a share's
track key fills in late; a snapshot never attaches a sink). Unlike full
screen it is NOT refused without a focused surface — a voice-only call is
when a floating window is most useful.

Live validation of the actions, the keys and the PiP window: **NOT TESTED**.

### Reading and typing privacy

`Settings → Privacy & security → Reading and typing`.

**Read receipts**: public (as before), private (MSC2285 `m.read.private` —
the server records it so this account's OTHER devices still clear their
badge, and no other member sees it), or off. The mode is stored ONCE on the
bridge rather than passed per call, because THREE Rust paths send a receipt —
reading a room, marking one read from the room list, and the thread panel's —
and a rule honoured by two of three is not a rule.

**The fully-read marker is sent in every mode.** It is account data only this
user can read, and it carries their place in the conversation between their
own devices and across a reinstall; losing it is not what a privacy setting
should cost. Receipts already sent cannot be retracted — the protocol has no
un-send — and the UI says so.

**Typing notices** can be switched off. They are the highest-frequency thing
a client discloses — every few keystrokes, to every member — and they say
when you are at the keyboard, not only what you send. Turning it off MID-NOTICE
sends the stop immediately: the server would time it out eventually, but "you
stop appearing to type within thirty seconds" is not what the switch says.

Live validation: **NOT TESTED**.

### Sign in another device with a code (MSC4108)

`Settings → Sessions → Sign in another device…`. Lightning implements the
two combinations where THIS device is the one already signed in: it shows a
code a new device scans, or takes the TEXT of a code a new device shows.
Either way the new device ends up signed in AND cross-signed — the SDK moves
the private cross-signing keys and the backup key across the channel, which
is the value of it and why the two confirmation digits matter.

**The other direction is deliberately absent.** Signing THIS device in from a
code requires the OAuth device-code grant in the client metadata, and
`rust/src/oauth.rs` does not request it — there is a test asserting so with a
comment saying the omission is on purpose. If that is ever revisited, the
route is clean: `login_with_qr_code` takes its OWN `ClientRegistrationData`,
so a separate metadata can request device_code while the ordinary
authorization-code flow keeps not to, and that test stays true.

**There is no camera.** Lightning bundles no camera-frame decoder, so the
scanning leg takes the code's base64 text. The dialog says so rather than
showing a viewfinder that will never fill.

Two rules, both tested:

* A progress step for a SUPERSEDED flow is ignored. Applying it would drive
  the current flow with the previous one's input, and for a check code that
  means comparing digits from a channel that no longer exists — skipping the
  comparison the digits exist to force.
* The rendered code does not outlive its flow, by ANY exit (cancel,
  sign-out, failure, success). The grid store is shared with device
  verification and served over an `image://` URL, so a code left behind is
  one a stale URL can still fetch.

The progress stream is not optional: the check code and the verification URL
arrive only through it, so the task always spawns consumer-plus-future.

Live validation: **NOT TESTED** (needs a homeserver with the MSC4108
rendezvous endpoint and a second device).

### Invisible crypto (MSC4153)

`Settings → Privacy & security → Device trust`. ONE switch driving BOTH SDK
knobs, because setting one alone gives an asymmetric client: refusing to
share room keys with devices that are not cross-signed while still decrypting
what those devices send, or the reverse.

* send: `CollectStrategy::IdentityBasedStrategy`
* receive: `TrustRequirement::CrossSignedOrLegacy` — **never**
  `CrossSigned`, which refuses legacy Megolm sessions (those created before
  clients collected trust information) and would turn existing history into
  undecryptable events the moment someone enabled a privacy setting.

**Default OFF**, deliberately: enabling it makes anyone who has not
cross-signed their own devices unreadable, and doing that to an existing
install unasked reads as the client breaking.

**RESTART TO APPLY**, said plainly in the UI. matrix-sdk 0.18 exposes no
runtime setter for either half — `decryption_settings()` is read-only and
there is no recipient-strategy setter — so the alternative would be
rebuilding the client, store and timeline registry underneath the user. It is
a process global read at client-BUILD time for the same reason:
`build_client` is the one path for password login, OAuth and the auth probe.

Live validation: **NOT TESTED**.

### Policy lists (Mjolnir-style moderation)

`Room Information → Moderation rules…`. Reads a policy room's
`m.policy.rule.{user,server,room}` state, publishes and removes rules where
the account has the power level, and keeps the list of rooms this account
follows (its own account data, `org.lightning_matrix.policy_lists`).

**Following a list does NOT act on it, and that is the contract.** A
subscribed list is somebody else's judgement; hiding people on the strength
of it — with no way to see that it happened or why — is a different feature
from showing that a list covers someone and offering to act. Lightning
already has ignore (`m.ignored_user_list`, server-side and account-wide) and
kick/ban/unban; this feeds them. The dialog says so, and a test pins it.

WHERE it tells you: the member profile popover asks the controller on open
(`app.policy.check("user", …)`) and, when a followed list covers the person,
shows "On a moderation list you follow — <the list's reason>" beside the
existing ignore report. The action is the popover's ordinary Ignore item,
deliberately unchanged. Until 2026-09-05 the check existed with NO caller —
the README's "Lightning tells you" was not true on screen — which is worth
remembering as a shape: a controller method proves nothing about a surface.

Details that decide whether a REAL list is readable at all:

* `recommendation` crosses as a STRING, not an enum. ruma models
  `Recommendation` with one known variant (`m.ban`) plus an open `_Custom`,
  so Mjolnir's legacy `org.matrix.mjolnir.ban` lands in the latter — and a
  client comparing the enum reads a real ban list as EMPTY.
* The legacy `org.matrix.mjolnir.rule.*` type names are read too.
* A rule recommending something OTHER than a ban matches nothing: the spec
  allows other recommendations and acting on them would be acting on advice
  nobody gave. Such rules are still SHOWN, and marked.
* A SERVER rule covers everyone on that server — the point of one.
* Removal is an EMPTY content object, so a removed rule must not parse as a
  rule with an empty entity, which would match nothing under a careful
  matcher and EVERYTHING under a careless one.
* The glob is a GLOB: `*` and `?` only. `.` and `+` are literal, because a
  Matrix localpart can contain both.

Reading uses the store-then-raw-`/state` pattern `widgets.rs` documents (the
SDK's state store is empty for uncommon types), bounded at 2000 rules — and
the bound is REPORTED, because a partial list that does not say so reads as
complete, and for a ban list "complete" means "this person is not on it".

Live validation: **NOT TESTED**.

### Sharing a place (m.location, live beacons)

RECEIVING both kinds is implemented in full: static `m.location` (MSC3488)
and live beacons (MSC3672), each rendered as its own card with the place, the
coordinates, the sender's stated accuracy, and — for a live share — whether
it is STILL CURRENT (`is_live()` checks the flag AND `ts + timeout`; showing
an expired share as live tells the reader somebody is somewhere they may have
left).

**SENDING is deliberately not implemented, in either form.** A static send
from a desktop is "paste a map link", which an ordinary message already is;
wrapping it in an `m.location` bought a native pin on phones and cost a
dialog, a menu item and a code path, and the maintainer judged that not worth
it (2026-09-05 — a first version with coordinate fields was built and
removed the same day). A LIVE send would be worse than absent: a desktop has
no position source, `send_location_beacon` exists to be called repeatedly
with new positions, and a "live" share that never moves is a lie told to
everyone in the room under a banner saying otherwise.

Two rules on the receive side:

* **An unreadable or out-of-range point leaves the coordinates ABSENT, never
  0,0.** A geo URI is a field of a message anyone can send. Zero is a real
  spot in the Atlantic, and a UI reading it would draw a confident link to
  the wrong place. `locationHasPoint` is the flag that distinguishes absence
  from a genuine 0,0, which is the equator at the prime meridian and must
  still render.
* **No embedded map, and no widening of the URL allowlist.** A map widget
  means tiles, and tiles mean every reader's IP address reaching a tile
  server the moment a message renders. The card builds an
  `https://www.openstreetmap.org/...` link from the PARSED NUMBERS;
  `UrlLauncher`'s allowlist (http/https/mailto) is untouched, because
  widening it to `geo:` would hand an attacker-controlled string to
  `xdg-open`.

ruma parses no geo URIs (`LocationContent::new` takes a `String`), so
`rust/src/location.rs` owns the parser.

Live validation: **NOT TESTED**.
