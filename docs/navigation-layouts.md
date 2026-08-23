# Room navigation layouts: Classic and Channels

**Landed 2026-08-23.** Live GUI validation: **NOT TESTED**.

The room-list column can be organised two ways, chosen per account in
Settings → Appearance → Conversation list.

## Why two, rather than one better one

They answer different questions and neither answer generalises.

**Classic** — what Lightning shipped through 0.7.6, and the default — answers
*"what has been happening?"* One activity-ordered list: invites, then
favourites, then DMs, then rooms, each row carrying a preview line and a
timestamp. It works in every account, including one with no Spaces at all,
which is why it is the default and the clamp target for an out-of-range
stored value.

**Channels** answers *"where is the thing I am looking for?"* The active
Space's own hierarchy: its direct rooms first, then each direct child Space as
a collapsible category with its own rooms nested under it, in the order the
Space's admin built. Rows are 32 px, a glyph and a name, nothing else.

The trade is explicit. Classic tells you what is new and cannot tell you what
exists; Channels tells you what exists and stays put while it does. A channel
list whose rows move when someone speaks is not a channel list — the whole
point is that a member learns where things are. So in Channels, unread changes
a row's **weight**, never its position.

## Design reference

Sable first, as directed: hierarchy order, categories as quiet all-caps
headers, single-line channel rows. Cinny informed the density. Discord
informed only the *interaction* — the chevron rotating, and a collapsed group
still reporting the activity inside it.

No Discord, Sable or Cinny code, asset, sound, trademark or wording is used.
Every colour comes from AppTheme.

## Host and presenter

`RoomsPanel.qml` is now a **host**. It owns the workspace header, the search
field with its Ctrl-K hint, the create/discover dialogs and the Voice
Connected footer. Its list body is one slot filled by whichever presenter the
setting chooses:

* `RoomListClassicPresenter.qml` — extracted verbatim from RoomsPanel.
* `RoomChannelsPresenter.qml` — new.

Three rules the contract suite enforces, each one a defect that would ship
silently:

1. **The host owns the chrome.** A presenter that grows its own header means
   the two layouts fork and one stops getting fixes.
2. **A presenter never reaches up into the host by id.** The extracted Classic
   list originally called `leaveRoomConfirm.openFor(...)` and
   `newConversationDialog.openDialog()` — resolved by scope, from inside a
   delegate, through a parent chain. That is exactly how the reader popover's
   click ended up silently dead in the 2026-08-19 round, and it breaks with no
   warning the moment the component is instantiated anywhere else. Signals
   only.
3. **Exactly one presenter is instantiated.** `Loader.active`, not `visible`:
   two live room lists is two sets of avatar fetches for one visible column.

## The model, and the trap it exists to avoid

`SpaceChannelModel` reads the **direct** hierarchy. This is deliberately not
`RoomListModel` with a Space filter, and it is deliberately not built on the
pre-existing `SpaceManager::childRoomsDetailed()`.

`SpaceManager::rebuild()` flattens a subspace's rooms into **every ancestor's**
membership — right for "show me everything in this Space", wrong for a channel
list. Building on it showed every subspace room twice: once at the top level
and again under its own category, with the structure invisible. So
`directChildRoomsDetailed()` was added, reading the Space's own
`m.space.child` order and keeping only joined non-Space children.

Nine of the fifteen model tests fail if that call is swapped back to the
transitive one.

Other decisions:

* **Depth stops at one.** A Space tree can nest arbitrarily and a sidebar that
  nests arbitrarily is unreadable by about three levels. A deeper subspace is
  offered as a category the user can open, which re-roots the model at it —
  the same way Space Home already works.
* **Uncategorised channels come first**, as in Element and Sable. With
  categories first, a Space whose only uncategorised channel is at the bottom
  looks empty until you scroll.
* **A collapsed category still reports what it hides** — a mention count, or a
  dot for plain unread. Collapsing to save space must not silently mute the
  group. Expanded, the header reports nothing: the rows carry their own
  badges, and a total on top of them would double-count what is already
  visible.
* **Collapse state is per Space** and session-only. Keyed by Space so
  collapsing "General" in one workspace does not collapse a same-named
  category in another; not persisted, because restoring a collapse from a
  month ago hides channels the user has forgotten about.
* **Unjoined children are absent, never placeholder rows.** Space Home is
  where a join is offered; a row here would be a channel that cannot be
  opened.
* **`kind` is a string**, not the C++ enum. Exposing the enum would mean
  registering the type with the QML engine purely so a delegate can name a
  constant, and a bare integer comparison in QML silently stops matching when
  a value is inserted.

## Honesty rules in the rows

* **The lock glyph is a claim.** It is drawn only for encryption the client
  *knows* about (`encrypted && encryptionKnown`); "not established yet" gets
  the plain hash.
* **A muted channel keeps its unread weight and loses its pill.** The user
  asked not to be counted at, not to be lied to about whether anything
  happened. A mention keeps its colour even when muted — somebody naming you
  is not noise.
* **`reuseItems` is on**, so the row re-queries the notification mode on every
  id change. `roomNotificationMode` is `Q_INVOKABLE`, not a property, so it
  cannot be bound; a recycled row would otherwise inherit the previous room's
  mute state.

## What Channels cannot show

It is one Space's hierarchy, so DMs belonging to no Space, invites and
favourites are not in it — and at Home there is no hierarchy at all. The host
falls back to **Classic** at Home rather than rendering an empty column:
"this space has no channels" and "you are not in a space" are different facts,
and only the first is this layout's to state. Settings says so plainly rather
than leaving it to be discovered.

## Theme tokens

Seven new tokens (`channelCategoryText`, `channelText`, `channelTextUnread`,
`channelSelected`, `channelSelectedText`, `channelHover`,
`channelUnreadMark`), every one **derived** with the
`_p.x !== undefined ? _p.x : fallback` idiom AppTheme already uses.

Nothing was added to the eleven palettes. A new required key in eleven
palettes is how a theme ends up with one undefined colour and a transparent
row — and the only thing that catches it is the no-QML-warnings gate, which is
where `stormSelected` (a token that does not exist; the real one is
`stormSelection`) was caught during this round.

The Channels layout therefore works in all eleven themes on the day it ships,
and any palette can override a single tone later without touching AppTheme's
accessors.

## Also in this round

`UnreadBadge.qml` was extracted from RoomDelegate's inline pill. Two
hand-rolled pills is how one layout ends up saying "3" in danger ink and the
other in accent ink for the same room.

## Tests

* `space-channels` (15) — the model. Direct-not-transitive, hierarchy order
  under activity, collapse hiding rows but not activity, per-Space collapse
  scope, unjoined children absent, `rowForRoom` never matching a category,
  known-encryption only.
* `navigation-layout-contract` (17) — the separation. Host keeps the chrome,
  no presenter reaches up by id, one presenter instantiated, Channels never
  renders without a Space, Classic is the default and clamp target, the
  setting is account-scoped and re-announced on a switch, every token derived,
  every empty-capable Label behind a Loader, the pill shared.

Two pre-existing suites were **repointed**, not weakened:
`RoomStateModelTest`'s section-label and group-divider scans now read
`RoomListClassicPresenter.qml`, because that is where the code moved — the
invariants are unchanged.

`UpdatesUiContractTest` had two single-line source scans that `qmlformat`
legitimately broke by splitting an `if` from its `return`. They now match
against a whitespace-collapsed copy: the mapping is what matters, not how the
formatter chose to wrap it.

The contract suite strips comments before every **ban** assertion. A comment
that names the thing it is explaining we do not do is the documented way a ban
regex fires on prose — it has cost this repo real time twice, and it punishes
exactly the comments worth writing.

## NOT TESTED

Nothing in this round has been seen on a real desktop. There is no live
validation of the Channels layout against a real Space hierarchy on a
homeserver, no check of how it looks in any of the eleven themes, and no
confirmation that a collapse, a category open, or the Settings preview cards
behave on screen as the offscreen suites assert.
