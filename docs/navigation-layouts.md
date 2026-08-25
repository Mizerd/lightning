# Room navigation: the Spaces rail, Classic, and Channels

**Landed 2026-08-23, substantially reworked 2026-08-25.** Live GUI validation:
partial — see §11 for exactly what has been seen and what has not.

Three things live here and they are deliberately different systems. Confusing
any two of them is the failure mode this document exists to prevent.

| | What it is | Where it lives |
|---|---|---|
| **The Spaces rail** | The 68 px column: Home, your Spaces, and your own *local* folders over them | `qml/SpacesRail.qml`, `RailEntryModel`, `RailLayoutStore` |
| **Classic** | One activity-ordered conversation list | `qml/RoomListClassicPresenter.qml` |
| **Channels** | Every joined Space as a flat collapsible folder of rooms | `qml/RoomChannelsPresenter.qml`, `SpaceChannelModel` |

## 1. Three kinds of grouping, and none of them are the same thing

* A **local folder** is client-side organisation the user makes by dragging one
  Space onto another. It is device-local, it holds Space ids, and it touches no
  Matrix state whatsoever.
* A **Matrix subspace** is real hierarchy: `m.space.child` / `m.space.parent`.
  Lightning renders it and never invents it.
* A **Space folder in Channels** is a *presentation* of one Space's direct
  child rooms. It is not a container the user can edit.

`NavigationLayoutContractTest::aLocalFolderNeverTouchesMatrixState` bans
`m.space.child`, `m.space.parent`, `addRoomToSpace`, `setSpaceChildSuggested`
and `sendStateEvent` from both `RailLayoutStore` and `RailEntryModel`. Ordering
and grouping the rail are view preferences, like a window width: Matrix has no
standard for either, so anything stored on the server would be a private
invention only Lightning could read.

## 2. The rail's drag

The reported problem was *"it works, but it isn't greatly polished — it's kinda
hard to tell exactly where you are moving them"*, with Element cited for smooth
real-time movement and Sable for a line showing the destination. Both were
built; on testing the line was cut and the smooth movement kept (see below).

### Why the drag needed a model, not a tweak

The rail used to bind its `ListView` to a **JavaScript array** rebuilt on every
change. Assigning a new array is a model **reset**: no `move` transition, no
`displaced` transition, every delegate torn down and rebuilt. So a reorder
could not animate at all, and during a drag the delegate holding the gesture
was destroyed the moment anything refreshed it. Both halves of the report have
that one cause.

`RailEntryModel` is a real `QAbstractListModel` that emits `beginMoveRows` for
the preview position. That is what lets QML animate the neighbours out of the
way while the pointer is still down.

### Three states, kept apart

* **Durable** — `RailLayoutStore`, on disk.
* **Transient preview** — the model's row order while a drag is live. Never
  written anywhere.
* **The gesture's facts** — which entry is moving, and whether releasing now
  would REORDER or GROUP.

Nothing is saved until the gesture ends, so a drag is **one** settings write
rather than one per pointer sample. `aPreviewDragMovesRowsWithoutWritingAnything`
asserts both halves: a real `rowsMoved` during the drag, zero `layoutChanged`
until release.

### What is on screen

**The tile itself moves.** It follows the pointer at full opacity while its
neighbours animate around it, and where it currently sits is where it will land.

A first revision drew three things at once — a dimmed gap where the tile would
land, an accent insertion line at that gap, and a floating copy of the tile
under the pointer. All three are gone, on the maintainer's testing: *"spaces
should always be their normal image and move freely without a line appearing
between them"*. The tile is the feedback; a line claiming the same thing was
noise on 68 px of chrome, and dimming the tile made the one thing being looked
at the hardest to see.

The one thing still drawn on top of the movement is the GROUP target: a ring on
the Space or folder a release would file into.

| Gesture | Feedback |
|---|---|
| Between two Spaces → **reorder** | the tile is there |
| Held over a Space or folder → **group** | the target ringed |

**A released tile stops rendering as dragged immediately.** `endDrag` announces
the cleared `DraggedRole`/`DropTargetRole` explicitly, because `refresh()` is
allowed to find the rows identical and emit nothing at all — which is right for
the row data and catastrophic for the drag flags. Without it the tile stayed
dimmed until some unrelated room update happened to refresh the model, reported
as *"their icons get darkened after moved and let go and only clear up after
entering a room"*.

"Between" and "onto" are a few pixels apart and mean completely different
things, so the group gesture needs both a **dead zone** (12 px at each edge of
the tile band) and a **dwell** (320 ms in one tile's centre). Without the dwell,
reordering *through* a Space would make a folder out of it on the way past.

The band is measured against the **tile**, not the row: an expanded Space's row
is much taller than its tile, and centring on that would put the group band
over its revealed rooms.

### Auto-scroll

While dragging, the pointer within 44 px of either end scrolls the rail
progressively (faster the closer to the edge), and the row under the stationary
pointer is re-evaluated as the content moves. Nobody should have to drop, scroll
and start a second drag.

### Where a dropped Space lands

Decided by the row it landed **after**, and only that:

* after a folder's header, or after one of its members → **inside** that folder;
* anywhere else → **top level**.

Dropping just past a folder's last member therefore **appends** to the folder
rather than leaving it. The rule has to choose, and appending is the choice that
makes the end of a folder reachable at all; leaving is a drop above the header, a
drop past whatever follows the folder, or the context menu's "Move out of
folder".

A **folder** moves as a block — header plus its open members — and may only land
at a top-level boundary. Folders do not nest: dropping a folder onto a folder is
refused (`aFolderIsNeverOfferedAsSomethingToDropAFolderInto`), and dropping a
Space onto an already-filed Space joins *that* folder rather than creating a
nested one.

### What the store guarantees

`applyArrangement(topLevel, folderMembers)` is one atomic write of the whole
picture, which is what a finished drag actually produces. Its one subtlety is
load-bearing: a folder **left out** of `folderMembers` keeps its members. The
rail only renders an *open* folder's members, so a collapsed folder can never be
emptied by a drag that never showed its contents
(`aCollapsedFolderIsNotEmptiedByADragThatNeverShowedIt`).

## 3. Local folders, Discord-style

Dropping one Space onto another creates a folder containing both, **where the
target was** — so the gesture reads as the two tiles merging rather than as one
being moved somewhere. No dialog: naming is a rename in the context menu
afterwards, exactly as Discord does it.

The collapsed folder's tile is a **composite of the Spaces inside it**
(`qml/FolderTile.qml`): up to four member avatars in a 2×2 grid, one larger
avatar when there is only one member. This is not decoration. A collapsed folder
raises exactly one question — *which* folder is this — and a generic letter tile
answers the one question the user already knows the answer to. (There is also no
folder glyph available: the bundled Material Symbols subset does not carry one,
and regenerating it needs the network.)

An **open** folder draws one container behind its header and its members, with
the last member carrying the rounded bottom (`folderLast`). That container is the
whole difference between a folder and several adjacent Spaces.

A collapsed folder still reports the unread and mention totals of the Spaces
inside it. Collapsing to save space must not silently mute a group.

**Muting a whole Space** is on the rail's context menu. Matrix has no primitive
for it — a Space is a room with no timeline, so muting it silences nothing — so
`AppController::setSpaceMuted` does what a person would otherwise do by hand and
sets each member room's notification mode, through the one per-room path so its
writes report and retry like every other one. Unmute restores *follow the
account default* rather than *all messages*: asserting the loud mode for rooms
that never asked for it is a different choice from undoing a mute.

**The stored format is additive.** A layout written by 0.7.6 has `folders` and
`order` and no `expanded` key; it loads with its folders, its membership, its
order and its collapse state intact, and nothing expanded — which is exactly
what it meant. `anOlderStoredLayoutKeepsItsFoldersAndOrder` pins it. Losing
someone's grouping to a format change is the one unrecoverable failure in this
area.

## 4. Matrix subspaces in the rail

Only **root** Spaces sit at the top level. A subspace appears underneath its
parent when that parent is expanded, indented by its real depth, recursively —
which is what Element Classic's Space panel does. The expansion is **persisted**
(`RailLayoutStore::expandedSpaceIds`), as Element persists its own: an expansion
is how someone wants to navigate, not a glance.

A subspace row is presentation of Matrix state, so it is **not draggable and not
a group target**. Its position is the hierarchy's, and a local folder must never
look like it can change it.

### Three things Matrix permits that a tree does not

`SpaceManager::resolveHierarchy` handles each explicitly:

* **Several parents.** A subspace is nested under exactly ONE of them — whichever
  the breadth-first walk reaches first — so it appears once, in a place that does
  not move between syncs. The other parent still contains its rooms transitively;
  only the nesting is exclusive.
* **Cycles.** `A → B → A` is legal state. Every Space is assigned at most once, so
  a cycle simply stops; anything the walk never reaches becomes a ROOT rather than
  being dropped, because a Space the user has joined must stay reachable whatever
  its state says.
* **Parent links the account cannot see.** `parentSpaceIds` may name an unjoined
  Space, and on some backends it is not populated at all. Parents are therefore
  the UNION of the child's own parent list (restricted to joined Spaces) and the
  inverse of every joined Space's own `m.space.child` list, so the hierarchy
  resolves identically whether the backend reports edges from above, below or both.

`level` used to be `parentSpaceIds.isEmpty() ? 0 : 1` — a two-level
approximation that rendered a three-deep tree as a flat pair of indents. It is
real depth now.

### A backend defect this round fixed

`RoomInfo::childRoomIds` is documented as the Space's DIRECT children in
`m.space.child` order, and it was that on the mock and HTTP backends. The **Rust**
backend filled it from its payload's `descendants` array — the *transitive*
closure. So every consumer that needed the structure the Space's admin built saw
one flat run of the whole tree: `directChildRoomsDetailed` was not direct, and the
Channels layout listed a subspace's rooms twice with no structure visible. The
mock's own tests could never catch it, because the mock was right.

`enqueue_spaces` now reads each Space's own `m.space.child` state
(`direct_children_of`), ordered by the spec's comparator (`order` key first,
room id as the tiebreak, empty-`via` removals skipped), and emits it as
`children`; `descendants` remains as a fallback for any producer that does not
send it.

## 5. Channels: Sable's model

```text
Lobby
Message Search

Invites            (only when there are any)
Rooms       >      every joined room no Space folder will list
Space A     v
  room 1
  room 2
Space B     >
Space C     >
```

### What the previous design got wrong, structurally

Channels used to show the **active Space's** hierarchy, with its child Spaces as
nested categories. Two problems, and neither was fixable by tuning:

* **It could not exist at Home.** With no Space selected there was no hierarchy,
  so the host silently fell back to Classic. The user chose a navigation layout
  and got the other one, with nothing saying why.
* **Nesting duplicated.** A subspace's rooms appeared under the subspace's
  category and again (transitively) under the top-level Space.

So the visual structure is now **flat by Space**, and the layout always exists:
`RoomsPanel`'s `channelsUsable` is the user's choice and nothing else.

**The rail's selection NARROWS it; it does not decide whether it works.** Those
are different things and the difference is the point. Picking a Space in the rail
sets `scopeSpaceId`, and the column becomes that Space and its subspaces — still
flat folders, never nested — with the two account-wide groups dropped, because
"every joined room no Space folder lists" is a statement about the whole account
and repeating it under a Space the user just selected is what
*"clicking a space basically does nothing"* was. Lobby clears the selection and
the column is the whole account again; it is always the first row, so the scope
is one click from escapable.

A pseudo rail row ("" for Home, `@orphans`) is not a Space and scopes nothing. A
scope on a Space the account no longer has falls back to everything — an empty
column would look like the account had nothing in it.

### Rows

A room row is the room's **avatar** and its name. Sable's own column shows a
picture per room, and the first revision of this layout drew a hash glyph
instead, which made every room in a Space look identical. The glyph survives as
a small ringed badge on the avatar's corner for the two things a picture cannot
say: this room is a DM, or this room is encrypted (and the lock is still a
CLAIM — only for encryption the client knows about).

### Membership rules

A Space folder lists that Space's **direct** child rooms. A subspace is a joined
Space like any other and gets its own folder at the same level. Consequences,
each one tested:

* A room that is a child of **two** Spaces appears under **both** folders. That
  is what "this Space contains it" means; a first-parent-wins rule would make one
  of the two Spaces look incomplete.
* A room whose only Space parents are Spaces the account has not **joined** has no
  folder to appear in, so it is in "Rooms". Nothing joined is unreachable.
* DMs are in "Rooms" with every other unparented room. There is **no** Favourites
  group and **no** Direct messages group — Sable has neither, and the People
  filter chip still reaches DMs.

### Lobby and Message Search

**Lobby** maps onto the shell's existing home surface: no room open and no real
Space selected, which is exactly the condition `TimelinePane` already uses to
show `HomePane`. `AppController::openLobby()` reuses `openSpaceHome("")`'s
teardown ordering. No fake room, no persisted event.

**Message Search** opens the existing global `MessageSearchDialog` — the same one
`Ctrl+Shift+F` opens — by signal through the host, because the dialog is
`MainScreen`'s. It is not a filter over the list. The row is **absent** when the
homeserver cannot search, rather than offered as a dead entry.

**Invites** are not in Sable's own column and are here anyway: this layout is now
the whole navigation column, so leaving invites out would make an invite
unreachable for anyone who chose it.

### Ordering

Deterministic and stable everywhere, because a channel list whose rows move when
somebody speaks is not a channel list. Unread changes a row's **weight**, never
its position.

* **Spaces** follow the rail's own arrangement — `RailLayoutStore::orderedSpaceIds`,
  which is the user's order with each local folder's members inline where the
  folder sits, *regardless of whether it is collapsed*. `arrange()` cannot answer
  this: that is a presentation list and legitimately hides a collapsed folder's
  members, which for an ordering question would silently drop Spaces.
* **Rooms inside a Space** follow that Space's `m.space.child` order.
* **Rooms and Invites** are sorted by name, then by room id — two rooms may
  legitimately share a name, and a tie broken by nothing is a list that reorders
  itself between syncs.

### Collapse and search

Collapse is **persisted locally** (account-scoped appearance storage, never sent
to the server) and keyed by Space id or synthetic group id. It survives the
model's rebuilds, which happen on every arriving message. An account change drops
the in-memory copy and re-reads, so one account's collapsed folders never
describe another's rooms.

A non-empty search **opens every folder** and lists only matching rooms, with the
folder kept for context and empty folders dropped; Lobby and Message Search step
aside (they match nothing). The collapse **set** is untouched, so clearing the box
restores exactly what was collapsed before.

### Rebuilds are diffed

`applyRows` compares the new row list against the old: same ids and kinds → one
`dataChanged`; otherwise a reset. A reset on every arriving message tears down and
rebuilds every delegate — and its avatar fetch — which for a column this long is
visible. `RailEntryModel` does the same.

## 6. Host and presenter

`RoomsPanel.qml` is a **host**. It owns the workspace header, the search field
with its Ctrl-K hint, the filter chips, the create/discover dialogs and the Voice
Connected footer. Its list body is one slot filled by whichever presenter the
setting chooses.

Three rules the contract suite enforces, each one a defect that would ship
silently:

1. **The host owns the chrome.** A presenter that grows its own header means the
   two layouts fork and one stops getting fixes.
2. **A presenter never reaches up into the host by id.** Signals only. The
   extracted Classic list originally called `leaveRoomConfirm.openFor(...)`,
   resolved by scope from inside a delegate — exactly how the reader popover's
   click ended up silently dead in the 2026-08-19 round.
3. **Exactly one presenter is instantiated.** `Loader.active`, not `visible`: two
   live room lists is two sets of avatar fetches for one visible column.

The same chrome drives both layouts: the filter chips and the search box are
bound into `SpaceChannelModel` as well as into `RoomListModel`, or they would be
visible and inert in Channels.

The row **chooser** must name every kind the model can produce. It once named two
of three, so a group label fell through to the channel-row component and rendered
as a room row with an empty room id — clickable-looking, opening nothing, and
carrying a room's context menu over a heading. There are five kinds now (`lobby`,
`search`, `group`, `space`, `room`) and both the chooser and the model's closed
set are asserted.

## 7. Honesty rules in the rows

* **The lock glyph is a claim.** Drawn only for encryption the client *knows*
  about (`encrypted && encryptionKnown`); "not established yet" gets the plain
  hash.
* **A muted channel keeps its unread weight and loses its pill.** The user asked
  not to be counted at, not to be lied to about whether anything happened. A
  mention keeps its colour even when muted.
* **An invite always reads as unread**, whatever its counters say: it is an action
  waiting on the user.
* **`reuseItems` is on**, so the row re-queries the notification mode on every id
  change. `roomNotificationMode` is `Q_INVOKABLE`, not a property, so it cannot be
  bound; a recycled row would otherwise inherit the previous room's mute state.

## 8. Theme tokens

Nine derived tokens: seven for Channels (`channelCategoryText`, `channelText`,
`channelTextUnread`, `channelSelected`, `channelSelectedText`, `channelHover`,
`channelUnreadMark`) and two for the rail (`railFolderSurface`,
`railInsertLine`). Every one uses the `_p.x !== undefined ? _p.x : fallback`
idiom AppTheme already uses.

Nothing was added to the eleven palettes. A new required key in eleven palettes
is how a theme ends up with one undefined colour and a transparent row, and the
only thing that catches it is the no-QML-warnings gate.

`CustomThemeTest::theNewNavigationTokensFollowAnEditableRole` additionally pins
that each token's fallback is a role the **Theme Editor** can change, so a custom
theme genuinely moves the new surfaces rather than leaving them on a private
literal.

**The editor's preview draws whichever layout the user runs.**
`ThemePreviewDemo` takes a `channels` flag and renders the Sable shape — Lobby,
Message Search, a group and a Space folder with rooms — instead of the Classic
rows; the editor binds it to `roomNavigationLayout`. Previewing the Classic
column to somebody who runs Channels shows them where a colour lands in a column
they never see. The preview stays entirely fake: no `app.` anything, no models,
no media, so it still renders while signed out and cannot leak a real
conversation into a screenshot.

**Indigo Night is the flagship** as of 2026-08-25, on the maintainer's call. It
leads the featured cards and System-dark resolves to it; Storm remains the brand
theme and the shell's own chrome, and stays a featured card in fourth place. An
explicitly persisted theme id is never rerouted, so changing what System means
cannot touch anybody's stored choice.

**The identity discs hold back the magenta wedge.** At the disc lightnesses,
magenta and hot pink are the loudest part of the wheel, and a cool accent puts
the arc's warm end squarely there — Indigo Night's accent sits at 239 degrees, so
its fallback avatars came out pink. Slots landing between 290 and 350 degrees
have their saturation damped to 0.55, which leaves every hue where it is: the
families stay distinct per theme and the worst all-pairs separation across all
eleven themes is unchanged at dE 19.7. Rotating or narrowing the arc instead
either collapses several dark themes onto one identical family or drops that
separation below the gate.

## 9. Tests

* `rail-layout` — the store and the drag. The arrangement, the atomic write, the
  older-format load, expansion persistence, folder previews; then the gesture:
  preview reorder without writing, abandoned drag, drop-onto-Space creating a
  folder where the target was, drop onto a filed Space joining that folder,
  reorder inside a folder, drag back out, a folder moving with its members, a
  folder refused as a nesting target, pseudo rows and subspaces undraggable,
  root-only top level with real depth, a cycle keeping every Space, a two-parent
  subspace nesting once and staying put, and a mid-drag refresh deferred.
* `space-channels` — the Channels model. Global with no active Space, flat by
  Space, subspaces not nested and not duplicated, unparented rooms and DMs
  reachable, a two-parent room in both folders, an unjoined-parent room still
  reachable, invites offered, rail-order Spaces, collapse hiding rows but not
  activity, collapse surviving a rebuild and a reload, search opening and
  restoring, filters not claiming an empty account, the search row gated on
  server support, navigation rows carrying no room id, `rowForRoom` never
  matching a header, unread arriving as `dataChanged` not a reset, known-only
  encryption, and an account change re-reading the collapse set.
* `navigation-layout-contract` — the separation and the bans. Host keeps the
  chrome, no presenter reaches up by id, one presenter instantiated, Channels
  global and never falling back, the model with no active Space left, subspaces
  never nested, the drag living in a model with move transitions, reorder and
  grouping visibly different, local folders never touching Matrix state, every
  token derived, every empty-capable Label behind a Loader, the pill and the
  folder tile shared, and the hide-image contract.
* `channel-row-geometry-qml` — every row kind reports a real height inside a
  width-assigned Loader, and both the channel row and the navigation rows are
  clickable across it.
* `media-placeholder-qml` — the hide-image geometry contract (below).
* `custom-theme` — the new tokens follow an editable role.

## 10. Element-style hide image

A local **Hide image** control on image and sticker rows, matching Element's.

**The point of it is the geometry.** `MediaHiddenPlaceholder.qml` fills the media
box and contributes no implicit size of its own, so the row keeps the exact
rectangle the picture reserved: same width, same height, same reply and thread
positions, and the timeline does not move a pixel. Replacing a 360×270 picture
with a text row would jump every message above it, which for a
hide-this-image control is a worse outcome than the picture.
`hidingAnImageKeepsItsExactReservedGeometry` measures the box before and after.

**State lives in `MediaVisibilityStore`, keyed by media identity** — not in the
delegate. A timeline row is destroyed the moment it leaves the cache buffer, so a
flag inside one is gone by the time the reader scrolls back;
`aRebuiltRowComesBackHidden` pins that a freshly created row knows.

**Session-only**, deliberately, for three reasons in order of weight: there is no
Matrix standard for it, so persisting would mean an account-data key only
Lightning could read; a hidden image the user has forgotten about is content they
cannot find, and there is no list of hidden media and no "unhide everything"; and
Element's own persistence was not verified here, so inventing durable storage to
match a guess would be worse than a clean session-local implementation that says
so. The set is bounded at 4096 keys and the cap releases the **oldest** rather
than refusing the newest.

Purely local: nothing is redacted, edited, deleted or sent, and no other client
sees anything. The store reaches no `MatrixClient`, no `SettingsManager` and no
`QSettings`, which the contract suite asserts.

Behaviour details:

* Hiding starts **no** fetch, and reveals take the ordinary cache path. Bytes
  already cached stay cached; nothing is removed.
* A hidden GIF actually stops animating — leaving an `AnimatedImage` playing
  behind an opaque placeholder burns a decode per frame for something nobody can
  see.
* The `Image`'s source is **cleared**, not merely made invisible: an `Image` with
  a source still holds the decoded pixmap.
* **Hide** is on the message action bar (leading, where Element puts it) and in
  the context menu. Once hidden, the placeholder's **Show image** is the only
  control — the action-bar button is gone, because a second control offering to
  hide what is already hidden is noise. The menu row flips its label instead, so
  a keyboard user can go both ways.
* Images and stickers only. A video card has its own poster and controls, and
  extending this there without evidence anyone wants it would be adding a control
  to a surface that did not ask for one.

## 11. What has and has not been seen

**Seen on a real render** (the mock backend under the screenshot-demo build,
captured and read back): the Channels column's whole structure — Lobby, Message
Search, an Invites group, Space folders with their rooms indented under them —
with every room's avatar resolving, the open room's row marked and Lobby not
marked while a room is open; the rail's composite folder tile and the container
behind an open folder; both a light and a dark theme.

**NOT TESTED**, and each of these is the interaction rather than the picture:

* the drag's *feel* — the dwell, the auto-scroll, the animation timings, and
  whether the tile moving under the pointer reads right at pointer speed. Every
  assertion behind the drag is a MODEL assertion; nothing has driven it with a
  real pointer.
* creating a folder by dropping one Space on another, and the folder-name
  dialog's layout.
* muting a whole Space against a real homeserver's push rules.
* the Channels column against a real Space hierarchy — the demo backend has no
  subspaces, so nothing has confirmed a subspace appearing as its own flat
  folder, or the rail's selection narrowing the column, outside the model tests.
* hide/show on a real image, or the timeline genuinely not moving at a real DPI.
* the Rust backend's new `children` payload against a real homeserver: the
  direct-children fix is asserted only by the mock's own fixtures and by reading
  the SDK.

**One trap for whoever validates the rest.** These rows animate their
background (`Behavior on color`), and an offscreen `--demo-capture` at the
default 1400 ms settle grabs the frame *before* that animation has advanced —
so a row shows its creation-time colour and the capture reads as a selection
bug that is not there. Four rounds of probes were spent on exactly that. Use a
long settle (`--demo-capture=path.png,6000`), and if a property probe rendered
into a label disagrees with the pixel, believe the label.
