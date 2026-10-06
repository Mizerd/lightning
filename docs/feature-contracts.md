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
  (127.0.0.1), ephemeral port, consumes exactly one request on the exact
  `/callback/<nonce>` path (up to 16 connections wait at once, 10 s each for a
  complete head of at most 16 KiB — Chromium-family browsers send the callback
  on a spare connection, 8483516f), 15-minute attempt timeout; hand-
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
  `password` and `sso` → `matrix_auth()`, `oauth` → `oauth()`. Refresh tokens
  and the dynamic-registration client id are CREDENTIALS in the SecretStore,
  never in QSettings, never exposed to QML, never logged. **Legacy Matrix SSO
  (`m.login.sso`) IS offered** (`rust/src/sso.rs`, on the ungated
  `get_sso_login_url` / `login_token` primitives), on servers without their
  own sign-in page; the single-use `loginToken` is never logged and shares
  OAuth's two-phase store lifecycle. Live: debian.social's "Salsa" provider
  and Synapse SSO in Vivaldi, Chromium and Firefox PASS (8483516f); the
  Firefox + matrix.org failures a user reported were UNEXPLAINED at that
  commit and later explained by a server typed without `https://` (fixed by
  the address normalisation below, `docs/open-items.md`). Live validation
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

### Sign-in screen (2026-09-29, Rokas chose option A)

- **The typed server address** is normalised once, in
  `AuthManager::normalizedServerAddress()`, for discovery, password, OAuth
  and SSO alike: optional scheme (none means https, a typed `http://` is
  kept, never upgraded or downgraded), case, spaces, trailing slash, a pasted
  `/_matrix/...` or `/.well-known/...` path and a pasted `@you:server` are
  accepted. Without a scheme the text must be a Matrix server name, so a
  password typed into the field by mistake is not looked up; text with an
  `@` (name:password@host), another or a doubled scheme, or a bad host is
  refused and never sent anywhere. The result still goes through
  `server_name_or_homeserver_url()`, which strips the scheme again; it is
  never a guessed client API URL (CLAUDE.md §16, "A restore must not need a
  live server").
- **Refuse before the browser, explain after it.** Whatever the account
  record at the end of a browser sign-in would refuse is refused BEFORE the
  browser opens (`AuthManager::refuseBeforeTheBrowser`). Before this, a bare
  `matrix.org` / `sk.community` completed sign-in in the browser, the server
  issued a device, and Phase B refused with a message about "saved account
  details" (reported by darkcoffee, 2026-09-29). What can only be known after
  (the user id the server returns, a device the store belongs to) is said in
  plain words, never a generic failure. The issued device is still not
  revoked on such a refusal: open item.
- **A failed restore leaves nothing behind (D6).** Phase B writes the account
  record before its restore (so a crash leaves a store with its record), and
  opening the handle creates the store directory. When the restore then fails,
  the store and the record THIS attempt created are taken back
  (`m_freshLoginIdentity` / `m_freshBrowserRecord`): the store through
  `removeAttemptRustStore`, which removes that one directory and never a
  quarantined `.orphaned-*` sibling (the account-wide `removeAccountRustState`
  is for sign-out; used here it deleted "moved aside, never deleted" copies,
  on the password path too), after waiting for the retiring handle; the
  record only on an exact match, and only when `saveSession()` really wrote
  it. A record or store that existed before is never touched, and the message
  says honestly whether the rollback completed. A browser sign-in whose
  account's storage name collides with another saved one is refused before
  anything is written, as `login()` does. Before, they refused every later
  browser sign-in as "already signed in on this device" with no way out
  (reported live 2026-09-29). A session that really is there and blocks a
  sign-in as a new device is reported with the account
  (`existing_store_requires_restore`), and the login screen's card offers
  "Open it" and "Remove this account" (with its confirmation). A removal that
  could not delete every file turns the card into "removal_incomplete": it
  lists what is still here and "Try again" deletes from the identity resolved
  before the record went (`AppController::retryAccountRemoval`, never a
  re-derived path); the card goes only when nothing is left.
- **No dead end on the sign-in screen.** A store with no saved account beside
  it (a rollback or removal that could not finish, a crash before the save,
  an older build) is moved aside exactly as `login()` does, never deleted, and
  the browser sign-in goes on: a server with its own sign-in page shows no
  password form, so `login()`'s own quarantine was unreachable and the old
  "Sign in again to continue" refusal could never help. It is left alone only
  when a saved account differs from this one by case alone or records this
  store (on a case-insensitive file system it may be theirs); that refusal
  says to remove the other account. Every remaining Phase B refusal names an
  exact saved record and carries it to the login screen's card (open or
  remove it; rebuild a record with no device). Tests:
  `browser-sign-in-rollback`, `account-removal-retry`, `login-screen-qml`.
- **Layout (option A).** The server first, as one row: the field while it is
  typed or asked, then a summary ("matrix.org · Change") with a status line
  that always keeps at least one line ("Checking…", "Found"); a failure
  ("Can't reach this server. Check the address.", "This server has no sign-in
  method Lightning supports.") wraps to at most three, and nothing is shown
  below it then. Then
  only the ways that server lets people in, the most suitable first:
  * a server with its own sign-in page (OAuth 2.0 / MAS, e.g. matrix.org):
    ONLY "Continue with <server>", as Element Web does (its `Login.ts`
    `getFlows()` keeps only the OAuth flow when the server publishes one), plus
    "Create account" when the metadata lists `prompt=create` (Element Web
    `isUserRegistrationSupported.ts`, Element X iOS
    `AuthenticationService.swift`) and "Forgot password?", which opens the
    metadata's `account_management_uri` (https only);
  * a password server: the form, then "Or" and its single sign-on providers,
    as Element shows both; three or more providers become a two-column grid;
  * single sign-on only: the provider button(s), no password form.
  Lightning has no registration or password reset of its own, so a server
  without its own page shows neither link.
- **Provider logos** come from the IdP's MSC2858 `brand` (apple, facebook,
  github, gitlab, google, twitter/x), drawn from Simple Icons paths (CC0-1.0,
  `docs/third-party-notices.md`) in the button's ink; any other provider gets
  a neutral glyph, never a letter. A server-supplied `icon` (mxc) is NOT
  fetched: nothing can fetch media before sign-in (MediaBridge needs a
  session, and authenticated media needs a token).
- **"Not encrypted"** shows when the base URL discovery RESOLVED is plain http
  and not loopback (`AuthManager::isInsecureRemoteUrl`). A warning only: a
  development server needs http. It follows the resolved URL, not the typed
  scheme: `http://matrix.org` redirects to https.
- **Nothing moves under the cursor.** The card is anchored near the top, never
  centred on its height, and the server row keeps its height in every state
  (the status line only grows on a failure, when no field is below it), so a
  server answering cannot slide a field (a password landed in the clear-text
  server field that way, twice).
- A password sign-in's common failures read in words ("Wrong username or
  password.", "Too many attempts…", "This account has been deactivated.",
  "Can't reach the server…") from a fixed reason token rust/src/lib.rs
  attaches; anything else keeps the backend's message.
- Tests: `server-address-input` (AuthManager), `login-screen-qml` (the screen
  loaded for real). Live: NOT TESTED against a real MAS sign-up.

### Rooms and navigation

- Joined rooms, DM detection from `m.direct`, invites, Space hierarchy, room
  membership/actions, room information, and room creation
- **Two navigation layouts, chosen per account** (Settings → Appearance →
  Conversation list), plus the Spaces rail above both. Full contract in
  `docs/navigation-layouts.md`; read it before touching any of this. The
  load-bearing parts:
  * **Classic** — one activity-ordered conversation list: invites, then
    **Favourites under a header of their own** (Element's shape, restored
    2026-09-05 at Rokas's request after the 2026-08 round had retired the
    group so a star would not buy rank), then the feed. The default and the
    clamp target for an out-of-range stored value, because it works in an
    account with no Spaces at all.
  * **Channels** — Sable's model, reworked 2026-08-26 into THREE VIEWS the
    rail chooses between: **Home** (Create Room / Join with Address / Explore
    Spaces / Message Search, then the room invites, the rooms in no Space,
    and — since 2026-09-05, at Rokas's request — a **Direct Messages** group
    of the joined DMs after Rooms, with its own collapse key; the DM invites
    stay in the tab, the People chip is offered at Home too, where it
    narrows the view to that group, and the rail's Home badge counts those
    DMs as well as the unparented rooms),
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
- **Space Home's lobby is SECTIONED (2026-09-23, Sable's shape)**, replacing
  the 2026-08-19 flat "Rooms and spaces" list, which read the TRANSITIVE
  `childRoomsDetailed()` so a subspace's rooms ran together with the Space's
  own ("you can't tell which rooms belong to each space"). The Space's own
  DIRECT rooms come first ("Rooms"), then ONE collapsible section per joined
  direct child Space listing THAT Space's direct children. ONE level of
  sections: a grandchild Space is a ROW in its parent's section and drills
  into its own Home; its rooms are never flattened into the section. An
  UNJOINED child Space is a row with Join in the section of its parent.
  Every row shows the room's topic as a second line (plain text, one elided
  line; an unjoined room's topic comes from `/hierarchy`, which is also the
  only source of member counts and `suggested`). Order inside a section is
  the parent's own `m.space.child` order (`RoomInfo::childRoomIds`), then any
  row only `/hierarchy` knows, in the SDK's spec order. The grouping, order,
  search (names AND topics; a subspace whose own name matches keeps all its
  rows; empty filtered sections are dropped; a search overrides folding) and
  the folded state are `SpaceManager::lobbySections()` — the view
  (`SpaceLobby.qml`) draws data and never hides rows with `visible:`. Folded
  sections are SESSION state per Space, cleared on sign-out: persisting them
  would write Matrix room ids into settings and need the account-removal
  sweep. `/hierarchy` is asked once per section Space (the SDK's listing is
  max_depth 1; `RoomDiscoveryController` is single-flight per Space).
  Kept from the flat list: search, the Suggested badge, Join / Ask to join /
  Request pending, the unread and mention badges, and the manager's
  multi-select Remove / Mark as suggested — now offered ONLY on the Home's
  DIRECT children (its rooms and its subspace sections, the latter also via
  the section's ⋮ menu), because those are the only children its
  m.space.child events can change. `removeRoomFromSpace` checks the DIRECT
  child list too: its old transitive pre-check sent an empty-via
  m.space.child into the Home for a subspace's room and reported "removed",
  and reported child Spaces and unjoined children "removed" without sending
  anything. **Removal sends `{}`, not `{"via": []}`** (2026-09-24, measured live
  on a throwaway Space): the spec calls both "not a child", but matrix-sdk-ui's
  space graph keeps an edge for `via: []` (it drops only content that fails to
  parse), so a removed child stayed in the Space's rooms, rail and room list.
  `enqueue_spaces` also filters children a Space's own state unlinked (empty
  via, unparsable, or redacted), which covers Spaces removed from the old way
  and a child SPACE whose own `m.space.parent` still points back (an
  event unparsable for any other reason keeps its link unless its `via` is
  missing or empty); and an empty
  `children` list is an answer, never a reason to fall back to the SDK's
  `descendants`. A JOINED room or subspace the parent's synced state no
  longer lists is never drawn from a cached `/hierarchy` answer. While `/hierarchy` has not
  answered, a section says "Loading rooms…", not "No rooms yet". The **"Joined" chip is gone** from rows: beside every room it
  read as the user's ROLE (Portuguese renders it "Membro"), and the row's
  action — Join, or the open arrow — already says it; the accessible name
  carries "not joined".
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
  and the SDK media store runs a retention policy (100 MiB of downloaded
  payload, 1 GiB in total since 2026-10-01; see the size rule below) so
  larger payloads do not enter or stall matrix-sdk-media.sqlite3
- **The SDK media store is encrypted at rest, and encrypted-room media is
  kept in it (2026-09-30).** `rust/src/mediastore.rs` opens the media store
  on its own, in `lightning-media-store/` inside the account's store
  directory, with a 32-byte per-account key that `src/matrix/MediaStoreKey.cpp`
  keeps in the SecretStore beside the tokens (so sign-out and account removal
  delete it with them). The state, event-cache and crypto stores are opened
  exactly as `sqlite_store(path, None)` opened them; only the media store has
  a key. Binding rules:
  * a key is made only when its record is PROVABLY absent — the read
    succeeded, the store can vouch for that miss, and the account's own
    access token reads back from the same store. A locked keyring, a damaged
    value, or a key that could not be written and read back all give NO key;
  * the plaintext fallback standing in for a keyring that did not answer
    (2026-10-01: a Linux desktop with NO Secret Service, e.g. COSMIC from the
    COPR) vouches for a miss only for an account it already holds secrets for
    — one signed in while no keyring answered. Its key is made THERE, exactly
    as on macOS and in a portable folder, and admits no encrypted-room media.
    For an account whose secrets are in a merely LOCKED keyring the fallback
    holds nothing, the miss stays inconclusive and no key is made
    (`MediaStoreKey::mayCreate`, `read`). Before this, such a desktop kept
    nothing at all, unencrypted-room media included. Residual (sign-in itself
    is refused during a keyring outage, so it cannot be the trigger): a
    fallback group that SURVIVES `migrateInsecureSecretsGroup` (a partial
    migration, or a group kept whole for a key it does not know) still holds
    the account's token; at a LATER outage the fallback answers for that
    account, a miss for the media key there is taken as absent, a fallback
    key is made, and the keyring-keyed store is wiped at that start, unsent
    attachments in it included. After a PARTIAL migration the fallback group
    wins the next one, so the keys agree from then on; a group KEPT whole
    never overwrites the keyring's value, so each switch between an outage
    and a working keyring hands the store the other key and wipes it again
    (a cache, plus unsent attachments). Rare: it needs a group kept for a key
    a newer build wrote;
  * no key means an IN-MEMORY media store for the session: nothing kept,
    nothing on disk, never a plaintext file. The encrypted store on disk is
    left exactly as it is for the next start;
  * a store recorded for another key (`lightning-key-id`, a one-way id) is
    unreadable and is deleted: it is only a cache;
  * encrypted-room media is cached only in that store and only when the key
    is in a secure keyring (`isSecure()`: Secret Service, Credential
    Manager). Never as a kept file (`lightning-media-files/`), which is plain;
  * the old plaintext `matrix-sdk-media.sqlite3` is deleted (database first,
    then -wal/-shm) at the first start where no row is pinned by the send
    queue (`ignore_policy`); while one is, it is used as before for that
    session so the upload can finish, and new attachments sent then are
    written to it unencrypted. BOUNDED: pins not accessed for 7 days
    (`ABANDONED_AFTER`) count as abandoned, and a file that is not a readable
    database is deleted at once;
  * nothing from an encrypted room becomes a plain kept file, judged by the
    ROOM (unknown counts as encrypted), not by the media's own encryption;
  * the key is read through `SettingsManager::secretStore()` on every handle,
    never cached: `setSecretStore()` can swap it after a keyring outage, and
    no key is made while `secretBackendUnavailable()`, nor while
    `secretMissesAreInconclusive()` on a SECURE store. A session already open
    keeps the media store it opened; the swap applies from the next handle;
  * SIZE: 100 MiB (`MEDIA_STORE_MAX_FILE_BYTES`) of DOWNLOADED payload is the
    one cap (Rokas, 2026-10-01, raised from 24 MiB so a phone video from an
    encrypted room is kept). Lightning applies it itself before writing
    (`mediafetch::cache_put`), and an unencrypted-room file above it is a kept
    file, so no size falls between the two. The SDK's `max_file_size` is
    measured on the ENCODED row, which the keyed store makes 1.5x larger
    (ciphertext serialized as a msgpack array of integers; measured 1.5000x),
    so the policy is `MEDIA_STORE_ENCODED_MAX_BYTES` (151 MiB). The total
    (`max_cache_size`, also encoded bytes) is 1 GiB, about 680 MiB of media.
    Until 2026-10-01 the policy WAS 24 MiB, which kept only ~16 MiB, and an
    unencrypted-room file of 16-24 MiB was kept nowhere;
  * LIMIT: encrypted-room media over 100 MiB is never kept (a kept file is
    plain on disk), so it is downloaded and decrypted again every session.
    Cost of the size: a keyed write or read of a payload holds ~3.5x it in
    memory transiently (payload, ciphertext, encoded row), and the INSERT
    holds the store's single write connection while it runs;
  * Settings shows what THIS session keeps beside the checkbox
    (`MatrixClient::mediaKeepState`, read from `mx_rust_media_store_state`
    once the client is built): kept / kept except encrypted rooms (key not in
    a secure keyring) / kept except encrypted rooms this session (the old
    plaintext store, kept for an unsent attachment) / not kept (first session
    after sign-in, keyring locked or its key unreadable, store would not
    open);
  * `mediaStoreKey` is one of the keys `migrateInsecureSecretsGroup` moves, so
    a fallback group holding it still migrates whole. A key the fallback made
    is thereby PROMOTED: once a secure keyring holds it, the same store opens
    with `admits_encrypted` and encrypted-room media is kept from then on.
    What the store already holds then is unencrypted-room media and the send
    queue's attachments, written under a key that was on the same disk.
  On an insecure store (macOS, portable, a desktop with no Secret Service or
  Credential Manager) sent attachments, encrypted rooms
  included, are still kept by the send queue, encrypted with a key on the same
  disk; disclosed in docs/privacy.md and the Settings note. A first sign-in has
  no saved record yet, so its first session runs in memory and the key is made
  at the next start; a memory session loses an upload still running at quit.
  Live validation: NOT TESTED.
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
- **A link that is itself an image or a video shows as that media** (setting
  "Show images and videos from links inline", `previews/inlineMedia`, ON; it
  is presentation and loads nothing itself, and has its own NOTIFY so a
  toggle does not make rows re-request evicted previews). A direct image
  draws inline and a click opens the in-app
  viewer (zoom, copy, save, Open in browser); a video, or an image over the
  5 MiB inline cap, is a cover that fetches NOTHING until Play or View. The
  preview reads a 64 KiB head and describes a video from it, so the body is
  never downloaded to preview it (it used to read 5 MiB of an mp4 and show
  nothing). Invariants: every byte goes through `rooms::safe_open` (https,
  public DNS pinned, no proxy, every redirect re-validated) and is validated
  by magic (`rust/src/linkmedia.rs`, caps 25 MiB / 50 MP image, 100 MiB
  video); MediaBridge reaches a link only through a `link:<sha256>` key that
  `LinkPreviewController::resolveLinkMedia` answers, and only for a preview
  that LOADED, with inline media on, and not when the preview already knows
  it is over the cap; loaded is per URL but contacting the site is per ROW
  (`mediaAllowed` = auto-load for the row's room class or its own consent), so
  a card that came from another room's preview states that the site sees the
  IP before the Play/View press that records the row's consent; a row whose
  cache entry was evicted (64 MiB budget) opens the browser on click, never a
  dead click; an image the preview holds is served from those bytes
  and a server-route image from the homeserver's mxc, so opening the viewer
  contacts the site again only for an over-5 MiB image; a link leaves no
  learned size or dimensions on disk; no QML Image or MediaPlayer ever gets
  the URL. A server answer for a direct image (mxc plus a raster type, no
  title) is now used instead of falling back to a direct fetch. Preview
  images are held within a 64 MiB budget. og:video is NOT supported: a page's
  video is a second host and a second policy question. Live validation NOT
  TESTED.
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
- A backup key download pass per room (Lightning's own; deduplicated per room
  per session lifecycle) plus verified-session secret gossip. NOT automatic
  SDK room-key requests and NOT SDK backup download after decryption failure:
  this line claimed both until 2026-09-15 and the tree has neither —
  `automatic-room-key-forwarding` is not a requested feature and
  `BackupDownloadStrategy::OneShot` installs no UTD handler. See CLAUDE.md §9.
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

- **Call sounds (2026-09-23).** Lightning's own synthesised set
  (`scripts/generate-call-sounds.py` renders `data/sounds/*.wav`,
  GPL-3.0-or-later; nothing sampled from another product). The RULES are
  `src/calls/CallSoundPolicy.*` and are binding: every cue is LOCAL; the room
  already present at connect (and after a reconnect) is a silent baseline for
  2 s; join/leave throttled to one per 500 ms and silent above 8 people;
  deafened, nothing OTHER people do makes a sound while your own actions still
  confirm; a join that never connected and an unanswered ring end silently;
  no cue plays while a share captures the whole output mix. The incoming ring
  is Lightning's own looping ringer once it has loaded (the notification card
  then goes silent) and the desktop's themed call sound only as the fallback;
  it plays on the system default output, in-call sounds on the call's chosen
  speaker. `--call-sounds-status` asks a package whether the sounds load.
  Live: loading and playback measured on the laptop; in-call triggering in a
  real call and audibility are NOT TESTED.
- **The speaking indicator uses RFC 6464 audio levels** (2026-09-26): the
  microphone track carries the `ssrc-audio-level` RTP header extension, which
  is what LiveKit's active-speaker detection reads. The level travels in the
  clear, even in an encrypted room: SRTP does not encrypt header extensions and
  LiveKit terminates SRTP, so the SFU learns each sender's per-packet level.
  Element Call and livekit-client expose exactly the same, and Opus packet
  sizes already leak similar activity. Nothing is sent while muted, and share
  audio carries no level.
- **A failed capture or playback device never ends the call** (2026-09-26): a
  receive sink that fails (a sound-server drop) is isolated and rebuilt on a
  fresh sound-server connection; a microphone that fails is restarted in place;
  a failed or refused camera never blocks a later video publish; a chosen camera
  that is missing is refused, never replaced by the default one (a portal camera
  is exempt: the portal picks it). Each gives a notice when it gives up.

Live status, and do not inflate it: **audio, camera and screen share are
live-confirmed** — against Element on Linux, and on a packaged Windows build
(2026-08-27). Group-call behaviour on macOS, an ANSWERED legacy 1:1 call, and
most failure branches are **NOT TESTED**. The full inventory is at the end of
§16.

### Notifications

- **Delivery, per platform (2026-09-05).** A build with QtDBus (Linux) talks
  to the freedesktop daemon and gets actions, inline reply and per-id
  withdrawal. A build without it (Windows, macOS) delivers through the tray
  icon's balloon — Windows shows it as a toast in the Action Centre, macOS
  as a user notification — which needs a visible icon, so there the icon
  shows while notifications are enabled (as well as with close-to-tray). One
  balloon at a time; a click opens the room it was raised for. A balloon
  cannot be withdrawn through Qt: read-dismissal on those platforms needs
  the native toast APIs and is a recorded follow-up.
- **Notification sound (2026-10-06).** A delivered notification makes
  LIGHTNING's chime (`data/sounds/message.wav`, `mention.wav`, rendered by
  `scripts/generate-call-sounds.py` in the call set's voice) through
  `CallSoundPlayer`, not the desktop's. `NotificationManager::planSound` is the
  one decision: Lightning source plays ours and sends `suppress-sound: true`
  with no `sound-name`; System default sends `sound-name` and plays nothing;
  a silent notification (sound off, mode, a burst already sounded within
  1.5 s) also sends `suppress-sound` so no daemon adds its default. Muted,
  active-room, DND-by-mode and ignored senders never reach delivery, so they
  never sound. `Policy::allowsNotificationSound` keeps the chime quiet in a
  call, while a call rings and while a share captures the output mix.
  Windows and macOS deliver through `QSystemTrayIcon::showMessage`, which
  has no sound parameter: the platform may still add its own sound there,
  and the tree has no native toast path. NOT LIVE-TESTED on any platform.
- **Reading a room withdraws its notifications.** `closeRoomNotifications`
  runs on Mark as read and on a reply, and it reaches the desktop's HISTORY:
  an expired popup (freedesktop reason 1) keeps its payload, since KDE and
  GNOME keep the entry, while a dismissed or closed one is forgotten. The
  room list's walk (`refreshTrayUnread`) goes through `observeRoomUnread`,
  which withdraws only once the list has shown the room UNREAD since its
  latest card (2026-09-29), or five seconds have passed: a card can come
  before the list's unread fields catch up, and that stale "read" withdrew
  the card 0.3-0.6 s after it appeared. The bound keeps a card whose room
  was read before the list ever showed it unread from staying for good.
- **A room's history re-delivered after the initial sync never notifies
  (2026-09-29).** Opening a room subscribes it with matrix-sdk-ui's
  `timeline_limit` 20; the server re-sends its last 20 events (`initial`,
  `num_live` 0, which matrix-sdk never reads), the event cache drops to its
  last chunk, and the open room's timeline clears and APPENDS them as live.
  A gap or an expired sync session produces the same batch, carrying NEW
  messages, so the mark is per EVENT (`push_verdict::BacklogMarker`, one per
  open room or thread timeline): after a `Clear` or `Reset` in a diff batch,
  an append is `backlog` when it reached the server before the timeline
  opened (`unsigned.age` against this device's clock, the timestamp without
  one); an event the timeline already forwarded is `backlog` reset or not.
  `decide` returns on it before the verdict. Measured before the fix: 20
  popups on opening a room on a new device or after a restart whenever the
  batch landed with the window unfocused or off the bottom, identical on the
  published 0.9.9 (not a #15 regression). A first version marked every append
  after a reset, which silenced the new messages of a gap in the open room
  (review, 2026-09-30). The sync handlers (unopened rooms) are unchanged; a
  fresh login with 24 rooms raised none. Live: the first version PASSED
  2026-09-29 (same procedure, 20 popups on the old build and 0 on the fixed
  one, the SDK's reset traced in both). The per-event rule PASSED live
  2026-09-30 on a 1.5 s link: a room's first open, 0; a forced gap in the
  open, unfocused room (30 new messages during a 30 s stall), 21 (the first
  before the stall and the 20 re-appended after the reset), where the
  per-batch rule gave 1; one message after it, 1. Against a daemon that never
  answers: no stall, and GetCapabilities asked again after the pause.
- **Every call to the notification daemon is asynchronous (2026-09-29).**
  `Notify` and `GetCapabilities` go through `QDBusPendingCallWatcher`
  (10 s and 3 s), `CloseNotification` is sent without waiting, and no
  `QDBusInterface` is built (its constructor introspects synchronously). A
  daemon that owned the name and never answered held the GUI thread for
  75 s per message (measured). Deliveries wait for the capabilities, which
  decide escaping; without an answer bodies are escaped and inline reply is
  off. A daemon not up yet or not answering (`ServiceUnknown`, `NoReply`) is
  asked again 30 s later, four times per session at most; any other error
  is its answer. The call card has one `Notify` in flight at a time, and a card raised
  for a call that ended meanwhile is closed when its id arrives. A daemon
  that is D-Bus-activatable but not running is now started by the call,
  where the old validity check wanted a current owner and fell back to the
  tray balloon (by reading; NOT TESTED live).

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
- **Desktop notifications follow the account's push rules as matrix-sdk
  evaluates them (GitHub issue #15, 2026-09-29).** That cache is written only
  by this device's picker and by a report that arrives when a picker OPENS,
  so a room set to "Mentions & keywords" on another client, and an account
  default of mentions-and-keywords, notified for every message — measured
  live, and opening the room's Notifications flyout was enough to make the
  same room go quiet. Every event payload now carries `push_notify` /
  `push_highlight` (`rust/src/push_verdict.rs`: the sync handlers' own
  actions — an empty list is a verdict only in a room whose own member the
  store holds, because that is exactly when the SDK builds a push context,
  looked up once per room and never re-evaluated per event — and one
  `PushContext` per batch of live-timeline appends), and
  `NotificationManager::decide` follows a known verdict: a local Muted still
  silences first, so a mute applies before the server echoes it; a local
  Mentions-only does NOT narrow a notifying verdict, because matrix-sdk
  writes keyword rules without a highlight; and while the server does not
  have a room's choice (`localModeUnsynced`) the local mode decides alone:
  a FAILED WRITE, persisted per account (SettingsManager
  `roomNotificationModeUnsynced`, with the mode the server held before it)
  so it survives a restart or an account switch, or a DEVICE-ONLY legacy
  mode — a 1 or 2 held only in the pre-0.6.6 device-global key, chosen as
  "Local setting: it does not change this room's server push rules". A
  device-only mode is NEVER sent to any account's rules and is not on the
  retry list (a first draft sent them, which made a laptop-only mute silence
  the phone and wrote one account's choice into another's rules). The retry
  READS before it writes: the server still holding the rule the failed
  choice replaced means send again; any other rule, or a replaced rule now
  gone, was set elsewhere since and is adopted. Reports keep the cache on the
  server's side once the account's rules are known (after the first sync): a
  user-defined rule is adopted, and "no rule" becomes "Follow account
  default", so the picker tells the truth; a refresh after each first sync
  asks about every room this account stored a mode for (reads only), which
  also ends a stale local mute. No verdict (another backend, a local echo, no
  push context yet) keeps the old local policy. `m.notice` and edits now stay
  quiet where the account's ruleset has `.m.rule.suppress_notices` /
  `.m.rule.suppress_edits` (the spec defaults do); both notified before —
  a release-notes item. A highlight counts as a mention for sound and
  urgency. Known gaps: the picker's disclaimer still says "Saved to your
  account's notification settings" for a device-only room (needs a new
  string), and CallController's ring gate reads the local mode. Live: the
  defect is measured (FAIL, pre-fix build, 2026-09-29), and the fix PASSED
  live the same day in 10 cases: a room rule and an account default set
  elsewhere, keywords, notices and edits, and an open unfocused room
  (unencrypted rooms, one account). Still NOT TESTED live: encrypted rooms,
  Element X interop, the failed-write retry paths, and legacy keys across
  several accounts.
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
  On the Rust backend mode 3 follows the account's rules through the push
  verdict above; without a verdict `NotificationManager` branches only on
  Muted/MentionsOnly and mode 3 notifies for everything. Offered only on a
  backend that
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
- **GIF picker previews never load a provider URL in Qt** (2026-09-25).
  `GifPreviewCache` fetches each tile through the same Rust path as a send
  (`gif_download`: https only, the provider host allowlist, redirect and
  DNS/IP checks, GIF magic, size and edge caps), re-checks the bytes in C++
  and hands QML a local file. The cache is a 0700 temporary directory, at most
  256 files / 64 MB, six fetches at a time, active only while a picker is open
  and cleared on sign-out. It holds public provider content only. KLIPY's JPG
  stills are no longer shown.
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
- Read receipts: a reader has ONE position. With receipt hosting (a host row
  draws the receipts of the bodiless rows after it), a receipt the backend
  still carries on an older row while the same reader is on a newer one is
  shown only at the newer one, and the host the reader left is re-announced
  so it stops drawing them.

### MatrixRTC membership events are not timeline items (2026-09-05)

- Every Lightning timeline is built with `lightning_event_filter`: the SDK's
  default filter minus MatrixRTC membership state
  (`org.matrix.msc3401.call.member`, `org.matrix.msc4143.rtc.member`,
  `m.call.member`, `m.rtc.member`). A call re-publishes one per participant
  per minute; a room that hosts calls carried thousands, each paginated,
  ingested, instantiated and counted. The "started a call" row is the
  notification event and stays; the call UI reads membership from room state;
  the collapsed activity group simply no longer lists them. The pagination
  controller allows twelve consecutive empty pages (was two) so the fill can
  walk through such a run; the pane's row cap bounds what is inserted.

### Media rows and the viewport band (2026-09-05)

- An image row asks the media bridge for its picture only while it lies inside
  the pane's media band: a ROW-INDEX range `TimelinePane` computes from the
  viewport (2.5 viewports towards the newest end, 1.5 towards history) at
  load, model reset, viewport resize, and 300 ms after the last content
  movement, and assigns to each row through the row Loader — the same shape
  as `rowOnScreen`. A row entering the band asks then; a row that already
  holds its picture is left alone. Fixtures and the thread panel, which
  assign no band, are permissive.
- The history fill spends at most 12 pages on rows that add no visible height
  (was 60 since `a5e64a6`); after that a wheel towards older history on
  content too short to scroll requests the next page, so a collapsed
  activity run never has to be expanded to reach older history.

### Call diagnostics: the RTP statistics trace (2026-09-05)

- `LIGHTNING_CALL_STATS_TRACE=<seconds>` (1/true/yes = 5) makes the media
  engine ask both webrtcbins for their statistics on that interval and log,
  per SSRC, packets, loss, jitter, the bitrate over the interval, PLI / NACK
  / FIR counts, and for what this client sends the remote side's round-trip
  time and fraction lost. Numbers and the media kind only, no identifiers.
  Off unless set; stops with the call.

### The hover action bar and the call popout (2026-09-05)

- The message hover bar offers Edit (before More) under exactly the context
  menu's gate — own, editable, not a local echo — and starts the composer's
  edit for that message.
- The call popout fills itself with a share when that share's tile is
  clicked (or through the bar's fill button): the share alone, no control
  bar, a small margin; clicking the filled share or pressing Escape brings
  the tiles back, and the mode drops on its own when the share ends. It is
  local to the popout window and does not touch the stage's spotlight.
- A share tile frames the PICTURE (the painted rectangle computed from the
  frame size, a 2px light edge with a small radius; the spotlight accent
  rides on it), not the tile bounds, and while the picture shows the tile's
  own edge steps aside so there is ONE frame. Only the keyboard focus ring
  still draws on the tile. On a surface 480px or wider the owner nameplate
  behaves like the full-screen controls: shown for three seconds after the
  tile appears or the pointer moves over it, then faded; small grid tiles
  keep it.
- The popout's fill tiles exist only while the window SHOWS, exactly like
  its tile grid: a share tile claims its track's sink when it is built and
  the main stage's tiles claim it back on pop-in, so a fill tile that
  outlived a pop-in came back as a grey box. The reader's fill choice is
  kept across pop-outs.
- The "Voice connected" bar in the navigation column keeps its four buttons
  inside itself at the column's floor: the status text is the part that
  yields (elides), never the hang-up button.
- In Channels a favourite sorts FIRST within its group — Home's Rooms and
  Direct Messages, the tab's Chats, a Space's rooms and People — and recency
  orders the rest; the group structure is untouched.
- A Channels room row carries a favourite star between the name and the
  call glyph: filled while the room is a favourite, an empty outline while
  the row is hovered, and a click toggles the `m.favourite` tag (the same
  write as the row's menu). Neither favourite nor hovered: no star, no
  reserved width, so the unread pill never moves.
- A mention query ends at its second space, and a completed rich-mode pill
  (a matrix.to anchor) is never re-read as a token; the markdown editor's
  recorded refs already refused that.
- The message context menu is closed by any timeline content movement: it is
  popped up at a fixed overlay point, so a scroll would otherwise leave it
  floating away from its row. The pane owns the one open menu.
- A name colour someone changes reaches other running clients within about
  20 seconds: `NameColorManager` re-asks on a read past the 20 s interval AND
  sweeps the recently read set on a 20 s timer, so propagation does not wait
  for a name to repaint. A refused call join is surfaced on the status bar
  (the controller's `callFailed` reason).
- The room header's title bound (`Layout.maximumWidth`) is ceiled: a Layout
  hands an item an integer width and the title's own implicitWidth is a
  fraction, so an unrounded bound elided every name by a quarter pixel
  ("Ho…" at Home).

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
(2026-09-30: the MEDIA store alone now has its own key, through
`ClientBuilder::store_config` — see "The SDK media store is encrypted at rest"
above. The event cache this index duplicates is unchanged.)

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
* **"Index all rooms"** (Settings → Privacy & security → Message search index,
  and a one-time offer after a sign-in made on this device) walks every joined
  room with the SAME bounded per-room walk, `deep_index_room_gated` — there is
  no second indexer and no second history bound. Strictly one room and one
  `/messages` page at a time, 2 s between pages and 2 s between rooms; a
  rate limit matrix-sdk's own retries did not absorb backs the whole queue off
  by the server's `retry_after_ms`. A call or a scrolling timeline HOLDS it
  between pages. The queue, its position and the rooms already done persist in
  `lightning-index-all.json` in the account's store directory (room ids and
  counters only; 0600; deleted with the account, reset when the index is
  cleared), so a restart continues and a finished room is skipped next time.
  Undecryptable history is never indexed; a room that still holds any is not
  recorded as done, so the next run revisits it — by then matrix-sdk's
  redecryptor has rewritten whatever keys arrived for, in the event-cache
  store the walk re-reads. "Finished" means every room was walked to that
  bound or its start, not that all history is searchable: failed rooms and
  rooms with undecryptable history are reported and revisited next run. A
  "Clear index" wins over any writer in flight — every writer (this pass,
  "Index this room", the sweep) captures the index generation before its
  first await and writes only while it matches, under the mutex `clear`
  holds. The offer is asked once per account record, never
  for a restored session, and waits while a verification or recovery prompt
  is up. Runs on the room-action pool and stops on sign-out, account switch or
  teardown through `index_shutdown` and the lifecycle generation. NOT
  live-tested.
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

The selection circle on each row follows `app.forward.selectedCount` — the
row's `isSelected()` check is a plain call Qt cannot observe, and on its own
it ran once (2026-09-05: the footer counted, the circles stayed empty). While
selecting, the gutter belongs to the circle: the hovered row's timestamp does
not draw there, the circle is solid so it reads over an avatar, and the row's
content shifts right so the circle has a column of its own. Only messages
select: a call card or a state row has an event id but is not forwardable.
A selection ends when the reader leaves the room it was started in.

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

### Space moderation (kick, ban, unban from a Space)

- From a Space's Members tab, Room Information's People list or a member's
  profile card. The dialog states the consequence, takes an optional reason
  and offers to cascade to the Space's rooms: the Space first, then joined
  subspaces and rooms depth-first (`SpaceManager::moderationScopeRoomIds`,
  cycle-safe, depth 64).
- The plan comes from Rust (`rooms::moderation_plan`), which SENDS NOTHING:
  store-first member reads, one bounded fetch per unsynced room (15 s), at
  most 100 rooms, six rooms at once, and 25 s for the whole plan
  (`assess_within_budget`). A room not assessed in time comes back
  `not_checked`: listed, never offered, and counted in the status line. The
  controller fails a plan that never answers after 40 s. A room is offered
  only when the action can succeed
  (`moderation_verdict`: kick needs join or invite, ban needs not banned,
  unban needs banned; the SDK's own `can_kick`/`can_ban`/`can_do(Unban)`; the
  target strictly below the viewer). Others are listed with their reason.
- `SpaceModerationController` dispatches ONLY plan-offered rooms, one at a
  time through the existing room kick/ban/unban path, and reports each room's
  own outcome. A failed step does not stop the rest, and "done" never hides a
  failure. Asking for a new flow while one runs is refused VISIBLY: the dialog
  opens on the running flow with a notice.
- Lowering your OWN power level is confirmed like a grant of your own level,
  with its own warning (you cannot raise yourself back; only someone above
  your new level can), plus a second line when the new level can no longer
  edit `m.room.power_levels`. In the Space settings member menu, the
  Permissions tab's role buttons (which used to apply with no confirmation at
  all) and the room member card. A v12 room CREATOR (MSC4289; the bridge
  reports 2^53, one above the largest finite level, and the UI labels it
  "Creator") is never offered a demotion: no power-level event can change
  a creator's level, so `canSetPowerLevel` refuses it for the viewer's own
  row.
- A display name with no visible character (fillers, zero-width, format or
  tag characters) falls back to the localpart on the profile card.

### Closing a room or Space, and deleting one as a server administrator

Matrix has no client-side room deletion: a room exists on every server that
took part, and no member can make other servers or other people's devices
forget it. Element, Cinny, FluffyChat and Nheko offer none (Element's
maintainers have declined Synapse admin APIs in the client; FluffyChat's
"archive" is leave). Discord deletes a channel outright and asks for the
server name before deleting a server. Lightning offers the two honest pieces.

- **Close** (anyone who may set the join rule). The plan
  (`rooms::closure_plan`, same budget and concurrency as the moderation plan,
  SENDS NOTHING) says per room whether it is offered (`not_joined`,
  `no_permission` = cannot set `m.room.join_rules`, `unknown`, `not_checked`)
  and whom closing removes: members strictly below the viewer, and only
  with the kick permission (joined, invited and knocking). Everyone else
  STAYS and is named in the row; without the kick permission the row says
  "you can't remove members here: all N stay", and a result names that as
  the reason. A world-readable room says its history stays readable by
  anyone. `rooms::close_room` then, in order: join rule to invite (a
  failure stops here, nothing else changed), out of the public directory if
  listed, out of the Spaces the caller names (one room only, optional), every
  removable member kicked one at a time with the optional reason (at most
  1000 per run), and LEAVES only when every step succeeded. The Spaces
  offered for unlisting are only the parents where the viewer may send
  `m.space.child` (the plan reads each parent too, from the store only, with
  no member load); the others are named as "stays listed" and do not count
  against the close. Anything less is
  "partly closed": the viewer stays so it can be run again, and the row says
  exactly what is left. It is never called a delete; the dialog says history
  stays on every server that took part, people keep what their apps
  downloaded, and anyone not below the viewer stays and can reopen it.
- **Close a Space** cascades through `SpaceManager::moderationScopeRoomIds`
  like Space moderation. Rooms run in reverse scope order, so a subspace's
  rooms go before the subspace, and the Space itself LAST. A Space or
  subspace is never LEFT while any room beneath it is partly closed, failed
  or not known: once out of a Space it cannot be cascaded again. The row
  says "you stayed: rooms inside it are not fully closed". A room another Space also lists
  starts UNSELECTED and says so. Cascade rooms are not unlisted from the Space.
- **Delete from server** (Synapse server administrators only). Offered only
  after `GET /_synapse/admin/v1/users/{self}/admin` answered 200 with an
  explicit `"admin": true`; 403, 404, a proxy hiding `/_synapse/admin`, MAS,
  or any other answer reads as "not an administrator" and the button never
  appears. Asked once per session, when Space settings or Room Information
  opens, never in the background. Confirmed by TYPING the room's name (its id
  when it has none). `DELETE /_synapse/admin/v2/rooms/{id}` with
  `{"block": <choice>, "purge": true}`, then the delete status is followed
  every 2 s for up to 10 minutes; past that the row says the server is still
  deleting it rather than guessing. The dialog states the limit: this removes
  the room from THIS homeserver; members on other servers keep it and its
  history. Every path segment is percent-encoded, `.`/`..` are refused, and a
  delete id other than letters, digits, `-` and `_` is not used. A COMPLETED
  delete marks the room left in the deleting admin's own store
  (`serveradmin::mark_deleted_room_left`, what matrix-sdk does to the room
  info after a leave; the crypto store is not touched):
  the purge removes the room before sync can deliver the forced leave, so
  without it the room stayed listed as joined, even after a restart (seen
  live 2026-09-29). It is not forgotten: that would clear the event cache
  under a timeline that may be open, for a room the server no longer has.
- The "also in another space" note on a row means a Space OTHER than the one
  being closed lists that room; a single room's own Spaces never count. It is
  added to the row's summary (whom closing removes, who stays), never shown
  instead of it.
- `RoomClosureController` (app.roomClosure) is the one flow; `RoomCloseDialog`
  renders it for both. The dialog can always be HIDDEN (a close can take
  many minutes); the controller keeps going and the next open shows where it
  is, or, once, how it ended. A step silent for 15 minutes (longer than every
  backend bound; a close reports after every step and removal) reads "not
  known yet" and the flow moves on. A flow, and the administrator answer,
  are PINNED to the account that started them: add-account resets both
  (`resetForAccountChange`, as Space moderation), and nothing more is sent
  once the client speaks for another account. A DELETE that got no answer
  (a timeout or lost response, not a refused connection) reads "not known:
  check the room before trying again", never "failed"; a 400 says a delete
  may already be running. A delete is confirmed by typing the name or the
  room id; a name with characters nobody can type asks for the id. A Space
  delete's title and text give the number of rooms it purges. Rooms the plan
  could not assess, rooms past its cap (no row at all) and rooms still
  waiting to run also keep the viewer in the Space. A 200 to the DELETE
  without a usable delete id, and a 502/503/504 from a proxy, read "not
  known" (the purge may be running), never "refused"; a delete whose status
  never answered reads "not known", not "still deleting"; a join-rule change
  that timed out reads "may or may not have changed". `serverAdmin` itself
  reads "unknown" for any account other than the one the answer was about,
  whatever reset did or did not run. Space moderation has a 3-minute step
  watchdog ("not known", the rest go on), so a lost answer cannot hold its
  modal dialog for the session. A 401 from the admin
  API says "sign in again"; an administrator answer that could not be obtained (or a question
  that could not be sent) is asked again after 10 minutes, a "not an administrator" answer is kept for the session. Entry points: Space settings > General > "Close or
  delete", and Room Information beside "Leave room". A flow requested while
  one runs is refused visibly, as in Space moderation.
- NOT TESTED live: closing against a real homeserver, the rate-limit
  behaviour of a large close (the SDK retries 429s within the 60 s per-request
  bound), and the admin delete against a real Synapse.

### SVG images (rasterised on send)

- §6 still holds: nothing RECEIVED is ever decoded as SVG. A received SVG
  image shows only the SENDER's raster thumbnail, with an "SVG" badge; without
  one it is a file card. Clicking saves; the viewer never opens it, and the row
  never falls back to an HTTP URL (which would bypass the media bridge's
  markup refusal). `rooms::media_fetch` refuses a declared SVG with no sender
  thumbnail before making any request. None of that changed.
- **RASTERIZE ON SEND (Rokas, 2026-10-06).** A user may PICK an SVG anywhere
  Lightning uploads a picture: message attachments (composer and thread),
  room, Space and own avatars, banners, chat backgrounds and sticker uploads.
  Lightning converts it LOCALLY to a PNG first and uploads only that PNG
  (`image/png`, with the PNG's own size and dimensions); a recipient never
  receives SVG from Lightning. The converter is `src/media/SvgRaster.h`; the
  job runner is `src/media/SvgRasterJob.h`.
- The file is hostile (it may have come from the web). `screen()` refuses,
  before QtSvg sees it: gzip/SVGZ (never inflated), over 2 MB, over 20000
  elements, over 1000 `<use>`, over 64 deep, external entities or an entity
  budget over 2 M characters, processing instructions, every `<image>` and
  `<feImage>` (QtSvg loads rasters from the local disk), any non-document
  `href`, external `url()` and `@import`. The render uses the SVG 1.2 Tiny
  subset with no animation (Qt 6.7+) and paints straight into a transparent
  QImage.
- It runs in a HELPER PROCESS: the app binary with the hidden
  `--rasterize-svg IN OUT MIN MAX` flag (preflight, exits, never reaches the
  Qt parser). The parent kills it after 10 s; the helper also has a CPU rlimit
  on Unix. Files, not pipes (a Windows GUI binary has no stdout). Up to three
  run at once. It runs under a QGuiApplication on every platform (the offscreen
  plugin on Linux, the native one on Windows and macOS; it never shows a
  window), so SVG text has fonts. Only if that app cannot start (the helper
  crashes) is the conversion retried once with `--no-text`, a core-only app
  where an SVG containing text is refused with a message saying to convert
  text to outlines.
- Size: the declared size (width/height with px, pt, pc, mm, cm, in, em, ex at
  96 dpi, else the viewBox, viewBox-only files included) sets the aspect ratio,
  which is kept to within a pixel. The longest side becomes the declared one
  raised to a floor and lowered to a ceiling, and never above 4096: attachments
  1024..4096, avatar and banner crop sources 1024..2048, chat backgrounds 2560,
  stickers 512..1024. A vector is drawn AT the target size, so a 24x24 icon is
  crisp, not an upscaled raster, and `width="100000"` is clamped.
- Needs the Qt SVG LIBRARY (`LIGHTNING_HAVE_QT_SVG`; packaging passes
  `LIGHTNING_REQUIRE_QT_SVG=ON`). It never needs the qsvg image-format PLUGIN,
  which the AppImage, macOS and Windows builds leave out and then assert
  absent: QSvgRenderer is the library. A build without it reports "This build
  of Lightning can't convert SVG pictures".
- UI: the pickers list `*.svg`. An attachment shows as the converted PNG
  (name, size and type are the PNG's) once ready; a refused one fails with the
  reason and Retry converts again. The crop dialog and the background editor
  say "Converting the SVG to a picture..." while the helper runs.

### Animated avatars and banners (GIF, animated WebP)

User avatars, room and Space avatars, profile banners (MSC4427) and Space
banners (`page.codeberg.everypizza.room.banner`) play when they are a GIF with
two or more frames or an animated WebP.

- **Thumbnails never animate.** Measured on Synapse 1.156: `animated=true`
  (MSC2705) is ignored, a GIF thumbnail comes back as PNG and a WebP one as
  JPEG. So the still thumbnail is always drawn first, and the ORIGINAL is
  fetched to find out (`MediaBridge::avatarAnimationSource`, "motion:" key):
  once per mxc per session, in the heavy lane.
- **What that costs.** The transfer is the whole original: matrix-sdk 0.18
  buffers media whole. Rust drops anything over 8 MiB (the motion-probe size
  class, `mxc_fetch_cap`) before the FFI copy, but after the download. The
  SDK media store keeps the original (`use_cache`, 100 MiB cap), so a
  later session re-reads it from disk, not the network. No verdict is
  persisted by Lightning: that would be a new on-disk record of the avatars a
  user has seen.
- **When it probes.** On hover and in a profile card (any format); otherwise,
  under autoplay "Always", only when the thumbnail is not a JPEG (a JPEG
  thumbnail is a JPEG or WebP source: every photo would be downloaded in full
  for nothing). So an animated WebP avatar plays in lists only once hovered.
- **Policy.** The "Autoplay and prefetch media" setting and Reduce motion. Never
  and Reduce motion fetch nothing extra. The avatar must also be on screen:
  `onScreen`, visible, a window that is not minimised or hidden, and inside
  its nearest Flickable's viewport (checked by the avatar itself, because
  Repeater sites cannot bind `onScreen`). Off on the software scene graph,
  where the shape mask cannot draw.
- **Bounds.** 24 playing avatars plus 4 for hover and the profile card; avatar
  canvas 512 x 512, banner 4 MP, 8 MiB; `cache: false`; the layer is torn down
  (`Loader active: false`) whenever it may not play.
- **Security.** Nothing reaches a decoder before the markup/SVG and A/V byte
  sniff. Files are 0600 in the 0700 scratch directory, wiped on sign-out, and
  read by the app alone.
- **Uploading.** Qt cannot encode an animation, so a crop flattens it. "Keep
  animation" in the crop dialog uploads the original frames uncropped (clients
  centre-crop), with metadata stripped first by a bounded parser that refuses
  anything malformed: GIF comments and every application extension except the
  looping ones (XMP), WebP EXIF and XMP chunks.

### Chat backgrounds and surface depth

A picture behind a room's timeline, from one of four places, resolved per room
(`backdrop::resolve`): this account's own picture for the room, the room's
SHARED picture, its Space's shared picture (nearest Space first; a room in two
Spaces inherits from the one being browsed), this account's own default, none.
Settings "Show backgrounds set by others" (default on) and the per-room "Hide
backgrounds others set for this room" remove the two shared levels only.

- **Shared = state.** `org.lightning_matrix.room.background`, state key "",
  schema version 1 (`rust/src/backdrop.rs`): an `mxc://` url only, advisory
  `info`, a dominant `color`, and `presentation` {dim, blur, tint, fit,
  align}, every field clamped on read AND write; `{}` clears. Unknown keys are
  ignored; a version above 1 is reported, never rendered. Set only when the
  SDK's `can_send_state` allows it; other clients ignore the type.
- **Privacy.** State is cleartext to the homeserver even in an encrypted room,
  so the picture is uploaded unencrypted and the editor says so first.
  Personal pictures are Lightning-encoded JPEG/PNG files under
  `<accountRoot>/backgrounds` (removed with the account), served to QML from
  memory via the staged-image store. "Use as my background here" is offered
  only in rooms known to be unencrypted: an encrypted room's picture kept as a
  file would be decrypted media at rest.
- **Pictures.** Shared ones only through `MediaBridge::wideImageSource`; picked
  files are sniffed by magic bytes (SVG refused), first frame only, scaled to
  2560 px and re-encoded with nothing of the original file kept (text chunks,
  EXIF, comments). Never animated.
- **Readability.** The scrim is the theme ground moved away from the ink
  (+8 L* light, -6 L* dark) at an opacity never below the floor at which
  textPrimary, textSecondary and textMuted all keep 4.5:1 over the picture's
  own MEASURED pixels (a 48 px decode; the worst 1% of samples are specks and
  are allowed to fail; the worst case, pure white and black, until measured).
  `dim` only adds. A fixed 50% scrim fails on all eleven presets (1.2-1.6:1),
  which ChatBackdropTest asserts. Blur is off on a software renderer.
- **Depth.** Settings -> Appearance -> Depth gives background, room list and
  rail a two-stop vertical gradient moved 4 L* AWAY from the ink, so text
  contrast only rises; every readability check passes at the worst stop on all
  eleven presets (ChatBackdropTest). Custom themes may also carry gradients
  for surface roles (`gradients` in the theme JSON, additive, sanitised like
  colours) and are graded at their worst stop
  (`CustomThemeStore::auditWithGradients`).
- **NOT TESTED live** (2026-10-06): two Lightning accounts seeing one shared
  background, Element ignoring the state event, Windows/macOS rendering.
