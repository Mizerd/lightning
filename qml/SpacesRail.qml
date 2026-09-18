import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.7 design shell: the far-left rail (68 px). Top-to-bottom: Home ("all
// rooms"), Space avatars (40×40, radius 12, active = accent outline), then a
// bottom cluster with Settings and the account avatar that opens the account
// switcher popover. The rail is always visible — it is the primary
// navigation column, not a Spaces-only affordance.
//
// ── Dragging ──────────────────────────────────────────────────────────────
//
// The rows come from `app.railEntries` (a real QAbstractListModel), NOT from a
// JavaScript array. That is what makes the drag feel like Element's: the model
// emits a genuine `beginMoveRows` for the preview position, so the neighbours
// ANIMATE out of the way while the pointer is still down, and the delegate
// holding the gesture is never destroyed by a refresh underneath it. The
// previous version rebuilt a JS array on every change, which is a model reset:
// no move transition, every delegate torn down, and the resulting position
// only discoverable after release — reported as "kinda hard to tell exactly
// where you are moving them".
//
// What a drag looks like: THE TILE ITSELF MOVES. It follows the pointer at
// full opacity and its neighbours animate out of the way around it, which is
// Element's behaviour and what was asked for in those words — "spaces should
// always be their normal image and move freely without a line appearing
// between them".
//
// An earlier revision drew three things at once (a dimmed gap where the tile
// would land, an accent insertion line at that gap, and a floating copy of the
// tile under the pointer). All three are gone: the tile IS the feedback, and
// where it currently sits IS where it will land, so a separate line claiming
// the same thing was noise on 68 px of chrome.
//
// The one thing still drawn on top of the movement is the GROUP target: a ring
// on the Space or folder a release would file into. It arms the moment the
// pointer is on that tile and needs no dwell to be safe, because NOTHING MOVES
// while the pointer is on a tile — dragging THROUGH one on the way somewhere
// else changes the order not at all. See `readingAt` below for why that rule
// replaced two earlier ones that made grouping unreachable.
Rectangle {
    id: root
    color: AppTheme.rail

    // Emitted by the Add Space tile below the Space list; MainScreen routes
    // it into the creation dialog's Space mode.
    signal createSpaceRequested()

    // The Direct Messages tab, CHANNELS ONLY. Classic reaches DMs through its
    // People filter chip over one activity-ordered list; a tab there would be
    // a second route to the same rows in a layout the maintainer asked to
    // leave untouched.
    readonly property bool peopleTabVisible:
        app.settings && app.settings.roomNavigationLayout === 1
    Binding {
        target: app.railEntries
        property: "peopleEntryVisible"
        value: root.peopleTabVisible
    }
    // "Other rooms" is the mirror image: CLASSIC only. The tile narrows a Home
    // that shows everything, which is what Classic's Home is; Channels' Home
    // already lists exactly the rooms in no Space, so the two tiles opened the
    // same page. Reported live 2026-09-03.
    Binding {
        target: app.railEntries
        property: "orphansEntryVisible"
        value: !root.peopleTabVisible
    }
    // The same condition, told to the model that computes the badges. A
    // tile's badge counts what that tile's view lists, and Home lists
    // different things in the two layouts: in Channels the DMs are on the
    // tile above and every Space's rooms are on its own tile, so Home
    // counting them pointed at a message that could not be there. That is how
    // two direct messages were missed on 2026-08-31.
    Binding {
        target: app.spaces
        property: "directMessagesHaveOwnTile"
        value: root.peopleTabVisible
    }
    // A SELECTION THAT NO LONGER HAS A TILE MUST NOT SURVIVE. Switching to
    // Classic removes the People tab, and leaving the selection on it would
    // leave the whole shell scoped to a tab with nothing rendering it — the
    // room list scoped to DMs with no visible control saying so, and no way
    // back but to click a Space.
    onPeopleTabVisibleChanged: {
        if (!peopleTabVisible && app.spaces
            && app.spaces.activeSpaceId === "@people")
            app.spaces.activeSpaceId = ""
        // And the same rescue the other way: switching to Channels removes
        // the "Other rooms" tile, and a selection left on it would scope the
        // shell to a tile with nothing rendering it. Home is where it lands,
        // which in Channels shows the very same rooms.
        if (peopleTabVisible && app.spaces
            && app.spaces.activeSpaceId === "@orphans")
            app.spaces.activeSpaceId = ""
    }

    // How many of a Space's rooms are revealed under its tile. SESSION state,
    // unlike the expansion itself: "show me five more" is a momentary
    // request, where "this Space is open" is how the user wants to navigate
    // and is persisted by RailLayoutStore. Held on the rail root so ListView
    // recycling and model resets never forget it.
    property var railReveal: ({})
    // Bumped on every SpaceManager change so the revealed-rooms bindings
    // (function calls, which QML tracks through this read) re-evaluate.
    property int spacesRevision: 0

    /// A row's height — its tile plus air above and below. The divider row
    /// (Home, or People when it is shown) adds its own band for the handoff
    /// rule beneath it.
    ///
    /// ONE SOURCE, because the drag's row arithmetic accumulates these and
    /// the delegate draws them: a literal in either place is a mis-drop at
    /// any interface size but the one it was written at.
    readonly property int normalRowBand: railTileSize + 2 * rowPad
    readonly property int dividerRowBand: normalRowBand + AppTheme.scaled(10)
    readonly property int railRoomRowBand: railRoomTileSize + 2 * rowPad

    /// ── THE TILES DO NOT MOVE, AND NOTHING IS DRAWN BETWEEN THEM ───────
    ///
    /// Indenting per level was the first answer and it was reported as "they
    /// keep sticking out more and more and create like a wave pattern". Moving
    /// the depth into connector lanes was the second, and a visual audit
    /// measured what that cost: 59% of a 116px rail spent on line-work and
    /// void, with depth encoded as the LENGTH of a horizontal rule — 10px per
    /// level at the widest stop and FOUR at the default width — and lanes that
    /// began in empty background, never attaching to the parent they stood
    /// for. It read as a wiring diagram with avatars stapled to one edge.
    ///
    /// So there are no lines. Containment is a tinted REGION behind the run,
    /// which is what Discord does, what this rail's own folders already did,
    /// and the one device that costs no horizontal space at all. Depth beyond
    /// the first step is carried by SIZE — a Space, a subspace, a room — which
    /// is Element's answer and is also free.
    ///
    /// THE GUTTER IS SIZED BY WHAT IT HOLDS, and it was not: this margin was
    /// a literal 14 with nothing tying it to the expander it has to carry.
    /// At the narrowest rail that left the chevron 2.8px from the rail's own
    /// outer edge and 4px from its tile — hard against the window frame, and
    /// reading as something that had fallen off the column rather than as
    /// something belonging to the tile beside it.
    ///
    /// IT WAS NOT CLIPPED, and the first version of this comment said it was.
    /// That came from eyeballing a 2x upscale and assuming the glyph was as
    /// wide as the size it is given; an `Icon` sizes by FONT PIXEL SIZE and a
    /// chevron's advance is 7.2px of the 12 it was asked for, so the arithmetic
    /// that predicted x = -2 was out by the difference. Measure the item.
    ///
    /// AND IT HOLDS THE EXPANDER *INSIDE THE INNERMOST REGION*, which is a
    /// stricter requirement and the one that sets this number. Reported as
    /// "chevrons should be repositioned so they are in the color shape, not
    /// in between them": a deep row's innermost region starts at
    /// `bandInset(maxBandLayers)`, and a glyph right-anchored to the tile
    /// begins at `railLeftGutter - chevronInset - its own advance`. With a
    /// 20px gutter that put the glyph at 8.8 and the depth-3 region's edge
    /// at 10, so the chevron sat in the PARENT's band beside its own box.
    ///
    /// The two are one constraint, so the margin is derived from it rather
    /// than nudged until it looked right:
    ///   bandInset(maxBandLayers) + clearance <= margin - chevronInset - advance
    /// A chevron's advance is 7.2px of the 12 it is given (an `Icon` sizes by
    /// FONT PIXEL SIZE), so three inset steps of 4 leave 2.8px of clearance
    /// here. Moving the chevron per level instead was considered and
    /// refused: it is what "the chevrons are unevenly distanced" already
    /// asked to have removed once.
    readonly property int chevronGlyphSize: AppTheme.scaled(12)
    readonly property int chevronInset: AppTheme.scaled(4)
    /// ── THE TWO MARGINS ARE NOT THE SAME, AND THEY WERE ───────────────
    ///
    /// One `railSideMargin` was mirrored on both edges, so the RIGHT side of
    /// the rail was as wide as the chevron column on the left while holding
    /// nothing at all — reported as "the space bar is a bit too wide for
    /// comfort, especially on the right side, it's just empty space there",
    /// and it was: 24px of it at every width.
    ///
    /// The left gutter is a COLUMN with a control in it and is sized by that
    /// control. The right margin is air, so it is sized like air. The rail
    /// loses 16px at every stop and the tiles keep every pixel of theirs.
    readonly property int railLeftGutter: chevronGlyphSize + 3 * chevronInset
    readonly property int railRightMargin: AppTheme.scaled(8)
    /// THE WIDTH NOW BUYS SOMETHING. It used to buy indent, then lanes; both
    /// were spent on structure rather than on content, so dragging the rail
    /// wider changed the tiles by nothing at all. Past the default the tile
    /// itself grows, up to a ceiling, and then the margins take the rest.
    readonly property int railTileSize:
        Math.min(AppTheme.scaled(56),
                 Math.max(AppTheme.scaled(40),
                          width - railLeftGutter - railRightMargin))
    /// ONE x for every tile — the audit's one unambiguous keep. It is the
    /// gutter's own width now rather than a centring calculation, because
    /// the two margins differ: a tile centred in an asymmetric rail would sit
    /// half-way into the column that holds its chevron.
    readonly property int tileColumnX: railLeftGutter
    /// ONE step down for anything nested, however deep — the same decision
    /// Element makes with its 32 -> 24 avatar, and for the same reason: a
    /// per-level shrink runs out after three steps.
    readonly property int railNestedTileSize: Math.round(railTileSize * 0.85)
    /// A REVEALED ROOM'S tile. 0.7 of the Space tile, which is what the
    /// literal 28 was at the size it was written at.
    readonly property int railRoomTileSize: Math.round(railTileSize * 0.7)
    /// Air above and below a tile inside a run, and the extra a run adds
    /// after its last row so the next group reads as a separate thing.
    readonly property int rowPad: AppTheme.scaled(4)
    /// TWO SIZES, BECAUSE THERE ARE TWO MEANINGS. One gap ran every break in
    /// the rail, and measured against the rows' own heights that made
    /// "leaving a nested region" and "an entirely different top-level Space"
    /// land 2.3px apart — the largest semantic break in the column did not
    /// read as one. A nested close needs only enough air to show the
    /// parent's tint through it; a top-level break has nothing behind it and
    /// has to carry on its own.
    readonly property int groupGap: AppTheme.scaled(8)
    readonly property int groupBreakGap: AppTheme.scaled(18)
    /// ── THE REGIONS STACK, one per ANCESTOR ───────────────────────────
    ///
    /// The first version drew ONE region per row, tinted by that row's own
    /// depth, and a depth-2 run therefore REPLACED its parent's tint for the
    /// rows it covered: three runs under one Space read as three unrelated
    /// bands stacked vertically rather than as two things inside a third.
    ///
    /// Every row now draws a region for each of its ancestors, outermost
    /// first, each one inset and a step further from the rail than the one
    /// containing it. A parent's region therefore runs unbroken behind every
    /// descendant it owns, and the nesting is visible as LAYERS.
    ///
    /// CAPPED AT FOUR, and it was three. Asked directly — "are these
    /// supposed to be the same color?" — of two regions that are nested one
    /// inside the other and wear the same tint, because both sit at or past
    /// the cap. They are, and that is the cap admitting it cannot say
    /// "deeper" any more; the honest fix is to make the cap deeper rather
    /// than to explain it.
    ///
    /// THE WIDTH FOR IT CAME FROM THE INSET, not from the rail. Each layer
    /// costs `2 * bandInsetStep` and pushes the innermost edge right, and the
    /// expander has to stay inside that edge (see `railLeftGutter`) — so the
    /// step is 2 where it was 3, which buys a fourth rung AND leaves more
    /// clearance than three rungs had. That is the review's point, taken: the
    /// inset was never the legible part. Measured across the whole stack it
    /// changed the region's width by 9px on a 94px rail, while one tint step
    /// is visible everywhere at once. Depth is carried by TONE, and the inset
    /// is only there so an edge exists to see the tone against.
    readonly property int maxBandLayers: 4
    readonly property int bandInsetBase: AppTheme.scaled(3)
    readonly property int bandInsetStep: AppTheme.scaled(2)
    /// Rung 0 is a FOLDER's container, which sits one step OUTSIDE hierarchy
    /// depth 1 because it contains it.
    function bandInset(depth) {
        return bandInsetBase
               + (Math.min(depth, maxBandLayers) - 1) * bandInsetStep
    }
    /// CONCENTRIC, and it was not. The radius stepped by one while the inset
    /// stepped by three, so the corners of two nested regions were not
    /// parallel: the visible band between them pinched from 3px to about 2.4
    /// at every corner. A rounded rectangle inset by N inside another is
    /// concentric only when its radius is smaller by exactly N.
    ///
    /// AND IT WAS A RAW LITERAL. `AppTheme.radiusMd` does not scale, so at
    /// 140% every other thing in this rail grew and the corners did not —
    /// the bands read measurably boxier against the tiles they hold.
    function bandRadius(depth) {
        return Math.max(AppTheme.scaled(2),
                        AppTheme.scaled(9)
                        - Math.min(depth, maxBandLayers) * bandInsetStep)
    }

    /// Both stops are the tile's own range plus the gutter, so the gutter is
    /// a CONSTANT across the whole range a reader can drag to — which is why
    /// the tile grows in the middle of it and the margins take the rest only
    /// once the tile has stopped.
    readonly property int minRailWidth:
        AppTheme.scaled(40) + railLeftGutter + railRightMargin
    readonly property int maxRailWidth:
        AppTheme.scaled(56) + railLeftGutter + railRightMargin

    function revealCount(spaceId) {
        if (!app.railLayout || !app.railLayout.spaceExpanded(spaceId))
            return 0
        var explicitCount = railReveal[spaceId]
        return explicitCount ? explicitCount : 5
    }
    function showMoreRooms(spaceId) {
        var next = {}
        for (var k in railReveal)
            next[k] = railReveal[k]
        next[spaceId] = revealCount(spaceId) + 5
        railReveal = next
    }
    // The space's DIRECT joined child rooms, most recently active first — the
    // quick-access reading of "top rooms". Unjoined children are join
    // offers, not rooms this rail can open; they live on Space Home.
    //
    // DIRECT, NOT TRANSITIVE, AND THAT IS THE WHOLE BUG.
    //
    // This called `childRoomsDetailed`, which returns the entire subtree, so
    // every room of every descendant Space was listed under every ancestor
    // tile — a Discord bridge with server spaces and category subspaces drew
    // each channel under its category, its server AND the umbrella above
    // them. Reported 2026-09-17 with a tree diagram; the reporter's words
    // were "the hierarchy is like… out of order".
    //
    // The call site went stale rather than being wrong when written: it was
    // added when the rail had no subspace nesting at all, where transitive
    // was a defensible reading of "what this Space contains". Six days later
    // the rail gained real nesting, and the commit that added it fixed this
    // exact flattening in every other surface and missed this one line. The
    // Channels column is protected from the same mistake by a contract test
    // that bans the accessor BY NAME; the rail had no such test.
    //
    // `directChildRoomsDetailed` had to gain `lastActivity` for this — see
    // SpaceManager.cpp. Without it the sort below silently degrades to state
    // order (the `|| 0` guards keep it from being NaN, which is worse: it
    // would not have looked broken).
    function topRoomsInSpace(spaceId) {
        void spacesRevision
        if (!app.spaces || !spaceId || spaceId.charAt(0) !== "!")
            return []
        var rooms = app.spaces.directChildRoomsDetailed(spaceId)
        rooms.sort(function(a, b) {
            return (b.lastActivity || 0) - (a.lastActivity || 0)
        })
        return rooms
    }
    Connections {
        target: app.spaces
        function onSpacesChanged() { root.spacesRevision++ }
    }
    // The expansion lives in the store, so a toggle has to re-evaluate the
    // reveal bindings too.
    Connections {
        target: app.railLayout
        function onLayoutChanged() { root.spacesRevision++ }
    }
    Connections {
        target: app
        function onAccountSwitchingChanged() {
            if (app.accountSwitching)
                root.railReveal = ({})
        }
    }

    // ── Drag state that belongs to the VIEW ──────────────────────────────
    // The order, the drop target and the reorder/group decision all live in
    // the model (see RailEntryModel). What is left here is what only the view
    // can know: where the pointer is, and what the proxy should look like.
    readonly property bool dragging: app.railEntries.dragging
    property real dragViewportY: 0
    // The pointer in CONTENT coordinates. The dragged tile centres itself on
    // this, which is what makes it follow the pointer rather than snap between
    // slots — the model reorder underneath moves its neighbours.
    property real dragContentY: 0

    function beginTileDrag(entryId, sceneY) {
        if (!app.railEntries.beginDrag(entryId))
            return false
        dragViewportY = list.mapFromItem(null, 0, sceneY).y
        dragContentY = dragViewportY + list.contentY
        return true
    }

    // A ROW IS NOT ONE HEIGHT, and this used to assume it was.
    //
    // It returned `normalRowBand` for every row but the first, with the first
    // SAMPLED at the start of a gesture because the handoff divider makes it
    // taller. That was exactly true while every tile was `railTileSize`, and
    // stopped being true when a nested Space's tile became a step smaller
    // (2026-09-18) and the last row of a group started carrying the gap
    // below it: 6px per nested row and 8px per group ABOVE the pointer,
    // which is invisible on a shallow rail and drops a slot off on a deep
    // one. Every row derives its own band now, the divider included, so
    // there is nothing left to sample.
    //
    // Read from the MODEL rather than from the delegate: `rowTop` exists
    // precisely because a delegate's `y` is being interpolated by the move
    // and displaced transitions while a drag is live.
    function rowBand(index) {
        var e = app.railEntries.entryAt(index)
        if (!e)
            return normalRowBand
        var isHome = e.pseudo === true && (e.spaceId || "") === ""
        var isPeople = e.pseudo === true && (e.spaceId || "") === "@people"
        if (peopleTabVisible ? isPeople : isHome)
            return dividerRowBand
        var tile = e.hierarchyChild === true ? railNestedTileSize
                                             : railTileSize
        // The gap belongs to the row whose innermost run ends, matching the
        // delegate's own `trailingGap` — the two are one number and a
        // literal in either place is a mis-drop.
        //
        // `ownsRegion` reduces to "has child ROWS" here, and so does the
        // delegate's: its other arm is `expansionCol.visible`, which is false
        // for the whole of a drag. This function is only ever asked during
        // one, so the two agree exactly when it matters.
        var owns = e.bandNextLevel > e.level
        var layers = Math.min(maxBandLayers,
                              Math.max(0, e.level) + (owns ? 1 : 0))
        var gap = layers > 0 && e.bandNextLevel >= 0
                  && e.bandNextLevel < layers
                  ? (e.bandNextLevel < 1 ? groupBreakGap : groupGap) : 0
        return tile + 2 * rowPad + gap
    }
    // DERIVED from the row heights, not read off `itemAtIndex(i).y`.
    //
    // Every row is exactly its tile band tall while a drag is live — the
    // revealed-rooms columns are hidden for the duration — so accumulating is
    // exact. And it is the only way to be animation-independent: the move and
    // displaced transitions interpolate `y` for 140 ms, so a pointer held
    // still would map to one row, then to its neighbour, then back, and the
    // dragged entry would oscillate between two slots.
    function rowTop(index) {
        var y = 0
        for (var i = 0; i < index && i < list.count; ++i)
            y += rowBand(i) + list.spacing
        return y
    }
    // THE TILE IS THE GROUP TARGET; THE GAP BETWEEN TILES IS THE REORDER
    // TARGET. This is Discord's rule and it is the third attempt at this
    // decision, because the first two were both structurally unreachable.
    //
    //   v1: "the middle 24 px of a row is the group zone". Reaching that
    //   middle means first crossing the row's near edge, which REORDERED —
    //   so the tile being aimed at stepped aside and the row under the
    //   pointer became the dragged entry, which is never a group target.
    //
    //   v2: "short of the row's midpoint you are resting, past it you have
    //   pushed through". The geometry was right and the dispatch was not:
    //   the resting branch ended in `updateDrag(row, !dwellTimer.running)`,
    //   and `running` is TRUE for the whole 250 ms the dwell is being
    //   served — so the second pointer sample inside the target's near half
    //   reordered anyway, and the branch that then fired stopped the very
    //   dwell it was waiting for. Grouping needed a frozen mouse to happen
    //   at all.
    //
    // Both had the same shape: a reading that MOVES THINGS while the user is
    // still aiming. So nothing moves while the pointer is on a tile, full
    // stop. A gesture that never disturbs its own target cannot fail the way
    // those two did, and it needs no dwell to compensate — a pointer sweeping
    // across a tile on the way somewhere else changes the order not at all,
    // which is what the dwell was standing in for.
    //
    // The bands, measured off the TILE and not off the row band: every tile
    // is 40 px at y = 4 inside its row, so a 24 px group band centred on the
    // tile's own centre leaves 12 px of dead space at each end. Between two
    // adjacent 48 px rows that is a 28 px reorder gap (12 + 4 spacing + 12),
    // which is a comfortable target, and it means the visual centre of a tile
    // — where a person aims — is now the middle of the group band instead of
    // sitting exactly on the old boundary.
    readonly property int tileGroupInset: 12
    readonly property int tileGroupBand: 24
    // ONE total, monotone reading of the pointer. Returns either { row: i }
    // (the pointer is on row i's tile) or { gap: g } (the pointer is in the
    // gap before row g; gaps run 0..count, so `count` is the end of the rail).
    function readingAt(contentY) {
        for (var i = 0; i < list.count; ++i) {
            var top = rowTop(i)
            if (contentY < top + tileGroupInset)
                return { gap: i }
            if (contentY < top + tileGroupInset + tileGroupBand)
                return { row: i }
        }
        return { gap: list.count }
    }
    // The dragged block's own slot is the GAP its tile came out of — the tile
    // is drawn under the pointer, not here. There is nothing to group with and
    // nowhere new to move, so a pointer over it holds everything still.
    function rowIsDraggedBlock(row) {
        var entry = app.railEntries.entryAt(row)
        if (!entry)
            return false
        var held = app.railEntries.draggingEntryId
        return entry.entryId === held
               || (entry.folderId !== undefined && entry.folderId === held)
    }
    // ONE dispatch, called by the pointer AND by the auto-scroll. The
    // auto-scroll used to end in its own unconditional reorder, which meant
    // any auto-scroll step disarmed a grouping the user had just aimed —
    // every 16 ms, for as long as the pointer was within 44 px of an edge.
    function applyPointerReading(contentY) {
        if (!root.dragging)
            return
        var reading = readingAt(contentY)
        if (reading.row !== undefined) {
            // A target the pointer has LEFT must stop being lit AND stop
            // being armed: `endDrag` groups on the flag, not on where the
            // pointer is, so a stale one would make a folder out of a
            // release over the gap.
            if (rowIsDraggedBlock(reading.row))
                app.railEntries.clearDropTarget()
            else
                app.railEntries.hoverGroup(reading.row)
            return
        }
        app.railEntries.hoverGap(reading.gap)
    }
    function updateTileDrag(sceneY) {
        if (!root.dragging)
            return
        var local = list.mapFromItem(null, 0, sceneY)
        dragViewportY = local.y
        dragContentY = local.y + list.contentY
        applyPointerReading(dragContentY)
    }

    // Where the dragged tile parks once a release would GROUP: the centre of
    // the target's own row. The tile stops following the pointer and shrinks
    // onto the target, so the two are visibly about to become one thing —
    // and, just as importantly, the full-size dragged tile stops covering the
    // ring that says which Space it would land in. -1 while reordering, where
    // the tile follows the pointer exactly.
    readonly property real groupAnchorY: {
        if (!root.dragging || !app.railEntries.grouping)
            return -1
        var r = app.railEntries.rowForEntry(app.railEntries.dropTargetId)
        return r < 0 ? -1 : rowTop(r) + rowBand(r) / 2
    }

    // Auto-scroll while dragging near either end, so a rail longer than the
    // window does not force the user to drop, scroll and start again.
    Timer {
        id: autoScroll
        interval: 16
        repeat: true
        running: root.dragging && list.contentHeight > list.height
        onTriggered: {
            var zone = 44
            var maxY = Math.max(0, list.contentHeight - list.height)
            var step = 0
            if (root.dragViewportY < zone) {
                // Progressive: the closer to the edge, the faster.
                step = -Math.ceil((zone - root.dragViewportY) / 4)
            } else if (root.dragViewportY > list.height - zone) {
                step = Math.ceil(
                    (root.dragViewportY - (list.height - zone)) / 4)
            }
            if (step === 0)
                return
            var next = Math.max(0, Math.min(maxY, list.contentY + step))
            if (next === list.contentY)
                return
            list.contentY = next
            // The pointer did not move, but the row under it did — so this
            // goes through the SAME dispatch a pointer move does. It used to
            // end in its own unconditional reorder, which cancelled an armed
            // grouping every 16 ms near either edge.
            root.dragContentY = root.dragViewportY + list.contentY
            root.applyPointerReading(root.dragContentY)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: AppTheme.spacing12 + 2
        anchors.bottomMargin: AppTheme.spacing12
        spacing: 0

        ListView {
            id: list
            objectName: "spacesRailList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            // A real model, so a preview reorder is a MOVE and not a reset.
            model: app.railEntries
            clip: true
            spacing: AppTheme.spacing4
            // Recycling is off on purpose: a rail holds a handful of rows, and
            // a recycled delegate mid-drag is how the gesture loses its own
            // tile.
            reuseItems: false

            ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }

            // What makes the rearrangement read as movement rather than as a
            // jump. `displaced` covers the rows the moved one pushed past.
            move: Transition {
                NumberAnimation {
                    properties: "y"
                    duration: 140
                    easing.type: Easing.OutCubic
                }
            }
            displaced: Transition {
                NumberAnimation {
                    properties: "y"
                    duration: 140
                    easing.type: Easing.OutCubic
                }
            }

            delegate: Item {
                id: spaceItem
                required property int index
                required property string entryId
                required property string kind
                required property string spaceId
                required property string name
                required property string avatarUrl
                required property int unreadTotal
                required property int highlightTotal
                required property int level
                required property string folderId
                required property bool collapsed
                required property int childCount
                required property var memberPreview
                required property bool pseudo
                required property bool hierarchyChild
                required property bool expandable
                required property bool expanded
                required property bool dragged
                required property bool dropTarget
                required property bool folderLast
                required property bool draggable
                // DECLARED, or they are `undefined` and everything that
                // reads them quietly takes the other branch — QML does not
                // complain about an unknown property on a delegate, it just
                // hands back undefined. Both of these shipped unread for
                // several hours: the field's corner-squaring children were
                // `!undefined` (so a run was always square at both ends) and
                // the group gap was never added. Caught by a geometric case
                // comparing `rowTop()` against the delegates, not by looking.
                required property int bandPrevLevel
                required property int bandNextLevel

                readonly property bool isFolder: kind === "folder"
                readonly property bool isHome: pseudo && spaceId === ""
                readonly property bool isPeople: pseudo && spaceId === "@people"
                readonly property bool isRealSpace:
                    !pseudo && !isFolder && spaceId.charAt(0) === "!"
                readonly property bool inFolder:
                    folderId.length > 0 && !isFolder

                width: list.width
                // 48 = 40px tile + 4px on each side so the active accent
                // outline (drawn at -4px margins) is never clipped by the
                // list bounds — this was the Home-icon clipping defect.
                // Home carries the handoff divider (32×2) below its tile.
                // The handoff divider sits under the LAST navigation tab, so
                // it separates the tabs from the Spaces rather than splitting
                // the tabs from each other.
                readonly property bool carriesDivider:
                    root.peopleTabVisible ? isPeople : isHome
                // SCALED, WITH THE TILE IT HOLDS. Measured on a real build:
                // the rail widened from 112 to 152 between 100% and 140%
                // interface and the tile stayed exactly 38px of colour — all
                // the extra width went into the gutters. Every calculation
                // around it already used `railTileSize`, which IS scaled, so
                // the drawn tile was the one thing that did not move. The
                // band is `tile + 8` (4px above and below) and the divider
                // row adds its own 10.
                //
                // NOTE for the next reader: the 2026-09-17 note on this file
                // said "at 140% the tiles grew and the indent did not". The
                // tiles did not grow either; both were unscaled, and only
                // the indent was fixed at the time.
                readonly property int tileBandHeight:
                    carriesDivider
                    ? root.dividerRowBand
                    : spaceItem.rowTileSize + 2 * root.rowPad
                /// Anything nested is drawn one step smaller, at any depth.
                readonly property int rowTileSize:
                    spaceItem.hierarchyChild ? root.railNestedTileSize
                                             : root.railTileSize
                /// How many nested regions this row draws, capped: one per
                /// ancestor, plus its own when it owns the run below it.
                readonly property int bandLayers:
                    Math.min(root.maxBandLayers,
                             Math.max(0, spaceItem.level)
                             + (spaceItem.ownsRegion ? 1 : 0))
                /// Does a run hang off this tile — subspaces as model rows,
                /// or rooms revealed inside this delegate?
                readonly property bool ownsRegion:
                    spaceItem.bandNextLevel > spaceItem.level
                    || expansionCol.visible
                /// How deep this row's own region really is, uncapped.
                readonly property int trueBandDepth:
                    Math.max(0, spaceItem.level)
                    + (spaceItem.ownsRegion ? 1 : 0)
                /// ── NO TWO REGIONS THAT TOUCH WEAR ONE TINT ───────────
                ///
                /// The stack is capped, so every row past the cap draws its
                /// own region as "the innermost layer" — which meant a
                /// depth-5 region was drawn directly inside a depth-4 one at
                /// the same inset AND the same tint, and the two were the
                /// same picture. Asked in those words: "are these supposed to
                /// be the same color?" They were, and the honest answer is
                /// that the cap had stopped distinguishing.
                ///
                /// Past the cap the innermost layer ALTERNATES between the
                /// last two rungs, so a parent and the child drawn on top of
                /// it always differ. The ancestor layers are untouched, which
                /// is what keeps the alternation from colliding with the
                /// depth-(cap-1) region on the same row.
                function bandTint(depth) {
                    if (depth < spaceItem.bandLayers)
                        return depth
                    var overflow = spaceItem.trueBandDepth - root.maxBandLayers
                    if (overflow <= 0)
                        return depth
                    return root.maxBandLayers + (overflow % 2)
                }
                /// The air after the last row of a run, added by that row.
                ///
                /// KEYED ON THE ROW'S INNERMOST REGION, and it used to be
                /// keyed on depth 1 — the top-level group. That left every
                /// INNER run butting straight into the next one with only
                /// `ListView.spacing` between them: reported as "the lowest
                /// level runs out of color, there are small gaps between them
                /// where the color should end, we want to separate it
                /// cleanly", with two runs of the same tint reading as one
                /// shape with a hairline notch through it.
                ///
                /// The gap is not a hole in the parent, because a layer whose
                /// own run continues BRIDGES it (see the Repeater's height).
                /// So what a reader sees between two sibling runs is the
                /// parent's tint, at full height, which is the separation.
                readonly property int trailingGap:
                    spaceItem.bandLayers > 0
                    && spaceItem.bandNextLevel >= 0
                    && spaceItem.bandNextLevel < spaceItem.bandLayers
                    ? (spaceItem.bandNextLevel < 1 ? root.groupBreakGap
                                                   : root.groupGap)
                    : 0
                height: tileBandHeight
                        + (expansionCol.visible ? expansionCol.height + 2 : 0)
                        + spaceItem.trailingGap

                property bool isActive: app.spaces && !isFolder
                                        && app.spaces.activeSpaceId === spaceItem.spaceId
                // How far the tile is lifted from its own slot to sit under
                // the pointer. Zero for every row but the one being dragged,
                // so nothing else pays for it.
                readonly property real dragLift:
                    !spaceItem.dragged
                    ? 0
                    : (root.groupAnchorY >= 0 ? root.groupAnchorY
                                              : root.dragContentY)
                      - (spaceItem.y + spaceItem.tileBandHeight / 2)
                // Above its neighbours while it travels over them.
                z: spaceItem.dragged ? 10 : 0
                // Indentation: a filed Space steps in a little, a subspace
                // steps in per level, and the whole thing is clamped to
                // whatever the rail's CURRENT width can carry.
                //
                // The clamp used to be the literal 14, which was right for
                // exactly one rail width because the rail had exactly one
                // width. Now it is `root.indentBudget`, so widening the rail
                // reveals more depth and narrowing it back hides it again,
                // with no mode to switch and no level to count.
                // GATED ON THE `expanded` ROLE, NOT ON A FUNCTION CALL.
                //
                // `revealCount()` asks `app.railLayout.spaceExpanded(id)`,
                // which is a Q_INVOKABLE — so a binding that reaches the
                // expansion state only through it records NO dependency on
                // it and never re-evaluates when it changes. Expanding a
                // Space that HAS subspaces inserts model rows, which rebuilds
                // the delegate and hides that; a LEAF category inserts none,
                // so its chevron flipped open (that reads the role, which
                // does update) and its rooms stayed hidden until an unrelated
                // toggle or an app restart rebuilt the rail.
                //
                // Reported as "a room did not appear under its space until
                // Lightning restarted" and filed as sync staleness. It was
                // not: Space Home listed the rooms the whole time. Same shape
                // as `root.info` in Space settings — a CALL where a binding
                // was needed.
                readonly property int revealed:
                    (isRealSpace && spaceItem.expanded)
                        ? root.revealCount(spaceItem.spaceId) : 0
                readonly property var revealedRooms:
                    revealed > 0 ? root.topRoomsInSpace(spaceItem.spaceId) : []

                Accessible.role: Accessible.Button
                Accessible.name: isFolder
                                 ? qsTr("Folder: %1").arg(spaceItem.name)
                                 : isHome ? qsTr("All rooms")
                                 : isPeople ? qsTr("Direct Messages")
                                 : spaceItem.spaceId === "@orphans"
                                   ? qsTr("Other rooms") : spaceItem.name

                // ── The open folder's container ─────────────────────────────
                // One surface behind the header and its members, which is the
                // whole difference between a folder and several adjacent
                // Spaces. Drawn per row and squared off between them, so the
                // run reads as continuous however long it is.
                Rectangle {
                    visible: (spaceItem.isFolder && !spaceItem.collapsed)
                             || spaceItem.inFolder
                    // ── ON THE SAME LADDER AS THE HIERARCHY REGIONS ──
                    //
                    // It was not, and the two defects that came out of that
                    // were both invisible to every check here.
                    //
                    // FIRST, `z: -2` put this container ON TOP of the region
                    // stack (z -21..-17), in a colour those regions already
                    // used — so a Space tree filed into a folder was painted
                    // flat and lost its nesting entirely. Measured on a
                    // capture: inside a folder, exactly ONE region tint
                    // appeared in the whole rail. Nothing was wrong with the
                    // model; the picture was drawn over.
                    //
                    // SECOND, the margins were a RAW 6 against a ladder in
                    // SCALED units — 6 lands between depth 1 and depth 2, so
                    // a container was drawn NARROWER than the regions it
                    // contains, and the boundary between two groups became
                    // four corner arcs and a hairline inside 20px.
                    //
                    // A folder and a hierarchy region say the same thing, so
                    // they are one device: this is rung 0, outside depth 1,
                    // behind everything.
                    anchors.left: parent.left
                    anchors.right: parent.right
                    objectName: "railFolderContainer"
                    anchors.leftMargin: root.bandInset(0)
                    anchors.rightMargin: root.bandInset(0)
                    y: 0
                    height: spaceItem.isFolder
                            ? spaceItem.tileBandHeight + list.spacing
                            : spaceItem.height
                              + (spaceItem.folderLast ? 0 : list.spacing)
                    z: -22
                    color: AppTheme.railNestSurfaces[0]
                    radius: root.bandRadius(0)
                    // Square off the joins so the container is one shape.
                    Rectangle {
                        visible: spaceItem.isFolder
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: parent.radius
                        color: parent.color
                    }
                    Rectangle {
                        visible: spaceItem.inFolder
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        height: parent.radius
                        color: parent.color
                    }
                    Rectangle {
                        visible: spaceItem.inFolder && !spaceItem.folderLast
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: parent.radius
                        color: parent.color
                    }
                }

                // Active outline: 2 px accent ring offset from the tile.
                Rectangle {
                    anchors.fill: spaceTile
                    anchors.margins: -4
                    radius: AppTheme.radiusLg + 3
                    color: "transparent"
                    border.color: AppTheme.accent
                    border.width: 2
                    visible: spaceItem.isActive
                }

                // Handoff divider between Home and the Space tiles: a rule
                // four fifths of a tile wide, so it stays narrower than the
                // tiles it separates at every interface size.
                Rectangle {
                    visible: spaceItem.carriesDivider
                    width: Math.round(root.railTileSize * 0.8)
                    height: 2; radius: 2
                    color: AppTheme.border
                    x: root.tileColumnX
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 2
                }

                // ── The group field ─────────────────────────────────────
                //
                // What a run of nested rows sits ON, instead of what used to
                // be drawn BETWEEN them. Common region: the Gestalt cue that
                // needs no horizontal space, which is the whole reason it is
                // the one every narrow rail converges on — Discord tints a
                // pill behind a folder's servers, and this rail's own folder
                // container has done exactly this since it shipped.
                Repeater {
                    // ── ONE REGION PER ANCESTOR, OUTERMOST FIRST ────────
                    //
                    // `model` is the row's own depth, so a depth-3 row draws
                    // three rectangles: its top-level Space's region, its
                    // parent's inside that, and its own inside that. The
                    // parent's region therefore runs unbroken behind every
                    // descendant instead of being replaced by the deeper
                    // one's tint, which is what made three runs under one
                    // Space read as three unrelated bands.
                    //
                    // NOT hidden during a drag, and the lanes this replaced
                    // were. `stampGroupField` runs inside `applyRows`, the
                    // one chokepoint every row set passes through — the drag
                    // PREVIEW included — so the layers a reader sees
                    // mid-gesture are the layers the release will produce.
                    // That is the whole answer to "can these be rearranged
                    // cleanly": the drop target is drawn, not imagined.
                    // …AND THE OWNER IS INSIDE ITS OWN REGION. A row at
                    // depth L draws L layers as a member, plus ONE MORE when
                    // it is the tile that owns the run below it — so the
                    // region reads as "this Space and everything in it"
                    // rather than as a band that begins under it. A
                    // top-level Space is at depth 0 and draws exactly that
                    // one, which is why an expanded Space is boxed and a
                    // collapsed one sits on bare rail.
                    model: Math.min(root.maxBandLayers,
                                    Math.max(0, spaceItem.level)
                                    + (spaceItem.ownsRegion ? 1 : 0))
                    delegate: Rectangle {
                        id: bandLayer
                        objectName: "railGroupField"
                        required property int index
                        readonly property int depth: index + 1
                        // THE INNERMOST DRAWN LAYER SPEAKS FOR EVERYTHING
                        // BELOW IT. Past the cap a region would be narrower
                        // than the tile it contains, so a depth-6 row joins
                        // the depth-4 layer rather than getting two nobody
                        // can see — and its bounds are then "depth >= 4",
                        // which is what `bandDepth` says.
                        readonly property int bandDepth:
                            depth === root.maxBandLayers
                            ? root.maxBandLayers : depth
                        // Compared against the NEIGHBOURS' depths, per layer.
                        // A pair of booleans on the row could only ever have
                        // described the innermost one.
                        //
                        // The owner's layer opens ON THE OWNER, by
                        // definition. For every other layer the row above is
                        // either another member (deeper or equal) or this
                        // region's own owner (exactly one shallower), and
                        // neither of those is an opening — so a layer opens
                        // only where the row above is outside it altogether.
                        readonly property bool isOwnerLayer:
                            depth === spaceItem.level + 1
                        readonly property bool opensHere:
                            isOwnerLayer
                            || spaceItem.bandPrevLevel < bandDepth - 1
                        readonly property bool closesHere:
                            spaceItem.bandNextLevel < bandDepth
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: root.bandInset(depth)
                        anchors.rightMargin: root.bandInset(depth)
                        y: 0
                        // PLUS THE LIST'S OWN SPACING while this layer's run
                        // continues, or the layer is not one shape at all:
                        // `ListView.spacing` puts 4px of rail background
                        // between consecutive delegates, and a per-row
                        // rectangle that stops at its own delegate leaves
                        // that seam showing through the middle of the group.
                        // Measured on a capture — two bands with a 4px dark
                        // line between them — not reasoned about.
                        // …AND THE TRAILING GAP TOO, for a layer whose own
                        // run carries on past it: the gap belongs to the run
                        // that ENDED, so a region still open across it has to
                        // cover it or the parent gets a notch where its child
                        // happened to stop.
                        height: spaceItem.height - spaceItem.trailingGap
                                + (closesHere
                                   ? 0
                                   : list.spacing + spaceItem.trailingGap)
                        // Outermost furthest back, so each layer is drawn ON
                        // the one containing it.
                        z: -20 + depth
                        radius: root.bandRadius(depth)
                        // INDEX `depth`, NOT `depth - 1`. Rung 0 of the
                        // ladder is a FOLDER's container, which sits outside
                        // hierarchy depth 1 — so reading it here shifted the
                        // whole ramp one rung down and made the first
                        // boundary the quietest instead of the evenest.
                        // Caught by measuring a capture, not by reading this
                        // line, which is correct-looking either way.
                        color: AppTheme.railNestSurfaces[
                            Math.min(AppTheme.railNestSurfaces.length - 1,
                                     spaceItem.bandTint(depth))]
                        Rectangle {
                            // Square off the top when this layer's run
                            // continues above, so a run of any length reads
                            // as one shape.
                            visible: !bandLayer.opensHere
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            height: parent.radius
                            color: parent.color
                        }
                        Rectangle {
                            visible: !bandLayer.closesHere
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: parent.radius
                            color: parent.color
                        }
                    }
                }

                // The expander: a quiet tree glyph living ENTIRELY in the
                // gutter left of the tile, never touching the active accent
                // outline. Right-pointing when closed, down when open — the
                // tree convention. It reveals BOTH the Space's subspaces (as
                // real hierarchy rows, inserted by the model) and its top
                // rooms, which is the whole of what the Space contains.
                Item {
                    id: expandChevronArea
                    objectName: "railSpaceExpandChevron"
                    // ── ON THE TILE, not in the gutter ──────────────────
                    //
                    // The gutter belongs to the lanes now, and a lane is four
                    // to ten pixels wide — a twelve-pixel glyph cannot stand
                    // in one without covering its neighbours. It also has no
                    // business there: the lanes say what CONTAINS this Space,
                    // and the chevron says what this Space is DOING, which is
                    // a fact about the tile.
                    //
                    // So it is a badge in the tile's bottom-left corner, at
                    // the same place on every tile at every depth. It is
                    // PERMANENT for a Space with something to open — hover-only
                    // was tried for one revision and the report was immediate,
                    // "I don't see how to collapse it" — and it never touches a
                    // line, so the tree behind it stays unbroken.
                    //
                    // ONLY WHEN THERE IS SOMETHING TO EXPAND. It used to
                    // appear on hover over ANY real Space, so a Space with no
                    // joined subspaces offered a control that opened nothing
                    // — reported as "remove this small arrow left of space,
                    // it does nothing now". `expandable` is the model's own
                    // answer (childSpaceCount > 0) and was already computed;
                    // the chevron simply never read it.
                    visible: spaceItem.isRealSpace && spaceItem.expandable
                             && !root.dragging
                    // ── IN THE GUTTER, AND THE TILE IS LEFT WHOLE ───────
                    //
                    // Third home, and the first one that costs nothing. A
                    // notch straddling the tile's corner was measured biting
                    // a 10x14px hole out of a 40px tile and filling it with
                    // rail background — a quarter of that corner, on ten of
                    // the twenty tiles, so the column's own silhouette came
                    // out eroded. Its disc was 1.07:1 against the rail, which
                    // is to say invisible, and the only thing anyone could
                    // actually see was a 5x3px tick sitting in a dent.
                    //
                    // The gutter is empty now that nothing is drawn in it, so
                    // the mark that lost to the lanes wins by default. It has
                    // the gutter to itself, so it keeps ONE x at every depth
                    // and needs no disc: there is no longer a line for it to
                    // interrupt, and nothing for it to be read against.
                    width: root.tileColumnX
                    height: spaceItem.tileBandHeight
                    x: 0
                    y: 0
                    Icon {
                        id: expandGlyph
                        objectName: "railSpaceExpandGlyph"
                        anchors.right: parent.right
                        anchors.rightMargin: root.chevronInset
                        anchors.verticalCenter: parent.verticalCenter
                        // ONE MEANING: open or closed. A separate glyph for
                        // "this one dives" was tried twice and rejected both
                        // times — as `chevron_right` it was indistinguishable
                        // from "collapsed", and as an arrow it read as a stray
                        // mark in the gutter.
                        name: spaceItem.expanded ? "expand_more"
                                                 : "chevron_right"
                        size: root.chevronGlyphSize
                        // STEPPED WITH THE REGION UNDER IT. The glyph's
                        // colour was constant while the region it sits on
                        // gets a tint step lighter each level, so the same
                        // control measured 4.37:1 at depth 1 and 3.01:1 at
                        // depth 3 — a third of its contrast lost, at the
                        // depth where the rail is busiest. It moves with its
                        // background now, in whichever direction the theme
                        // takes: `text` is dark on a light preset, so the
                        // same tint darkens there.
                        color: chevronHover.hovered
                               ? AppTheme.text
                               : Qt.tint(AppTheme.textMuted,
                                         Qt.rgba(AppTheme.text.r,
                                                 AppTheme.text.g,
                                                 AppTheme.text.b,
                                                 0.16 * Math.max(
                                                     0, spaceItem.bandLayers - 1)))
                    }
                    HoverHandler { id: chevronHover }
                    TapHandler {
                        gesturePolicy: TapHandler.WithinBounds
                        onTapped: app.railLayout.toggleSpaceExpanded(
                                      spaceItem.spaceId)
                    }
                    Accessible.role: Accessible.Button
                    Accessible.name: spaceItem.expanded
                                     ? qsTr("Collapse space")
                                     : qsTr("Expand space")
                }

                Rectangle {
                    id: spaceTile
                    objectName: "railSpaceTile"
                    width: spaceItem.rowTileSize
                    height: spaceItem.rowTileSize
                    // CENTRED on the column, not left-aligned to it: a smaller
                    // tile inset on one side only reads as misaligned, where
                    // the same tile inset equally on both reads as smaller.
                    x: root.tileColumnX
                       + Math.round((root.railTileSize
                                     - spaceItem.rowTileSize) / 2)
                    y: AppTheme.scaled(4) + spaceItem.dragLift
                    radius: AppTheme.radiusLg
                    // ACTIVE is ONE language for every tile in the rail: the
                    // accent ring above, plus a soft accent WASH here (a tint,
                    // not a block). A solid bolt fill four pixels inside a bolt
                    // ring reads as one yellow blob rather than as "you are
                    // here".
                    color: spaceItem.dropTarget ? AppTheme.accentSoft
                           : spaceItem.isActive ? AppTheme.accentSoft
                           : spaceItem.isFolder ? AppTheme.cardElevated
                           : spaceItem.pseudo ? AppTheme.cardElevated
                                              : "transparent"
                    // The GROUP target wears the rail's own "you are here"
                    // colour, thicker: a release here merges the two into a
                    // folder, and the accent is the one ink this column uses
                    // to say "this tile is the one that matters".
                    border.width: spaceItem.dropTarget ? 3 : 0
                    border.color: AppTheme.accent
                    // Full opacity, always. The tile keeps its normal image
                    // while it is dragged; dimming it made the one thing the
                    // user is looking at the hardest thing to see.
                    //
                    // Scale carries the merge: the dragged tile shrinks onto
                    // the target it has parked on (see dragLift), and the
                    // target opens up a little to receive it, so the two read
                    // as about to become one thing rather than as one tile
                    // sitting on another.
                    scale: spaceItem.dragged
                           ? (root.groupAnchorY >= 0 ? 0.56 : 1.06)
                           : spaceItem.dropTarget ? 1.08 : 1.0

                    Behavior on color { ColorAnimation { duration: 120 } }
                    Behavior on scale { NumberAnimation { duration: 90 } }

                    // Pseudo rows use monochrome icons; real Spaces show
                    // palette initials until the avatar loads.
                    Icon {
                        anchors.centerIn: parent
                        visible: spaceItem.pseudo
                        name: spaceItem.isHome ? "home"
                              : spaceItem.isPeople ? "person" : "workspaces"
                        size: spaceItem.isHome || spaceItem.isPeople ? 22 : 20
                        // Follows the tile: accent ink on the active wash, the
                        // plain icon ink otherwise. accentText was the ink for
                        // a solid fill that no longer exists, and on a soft
                        // wash it is unreadable.
                        color: spaceItem.isActive ? AppTheme.accent
                                                  : AppTheme.textSecondary
                    }

                    // The folder tile: a COMPOSITE of the Spaces inside it,
                    // the way Discord's is. A generic letter tile tells the
                    // user the one thing they already know ("this is a
                    // folder"); the member avatars tell them which folder,
                    // which is the only question a collapsed folder raises.
                    Loader {
                        anchors.fill: parent
                        active: spaceItem.isFolder
                        visible: active
                        sourceComponent: FolderTile {
                            members: spaceItem.memberPreview
                            fallbackName: spaceItem.name
                            colorKey: spaceItem.entryId
                            highlighted: spaceItem.dropTarget
                        }
                    }

                    Avatar {
                        anchors.fill: parent
                        visible: !spaceItem.pseudo && !spaceItem.isFolder
                        // MUST equal the rendered edge: Avatar bakes the
                        // rounded-square mask as `radius * 1000 / size`
                        // permille of the bitmap, so a stale 40 here gave a
                        // scaled tile a corner 40% too round.
                        size: root.railTileSize
                        circle: false
                        squareRadius: AppTheme.radiusLg
                        labelSize: AppTheme.scaled(15)
                        name: spaceItem.name
                        colorKey: spaceItem.spaceId
                        mxc: spaceItem.avatarUrl
                    }

                    // Unread count badge (rail-coloured ring per design).
                    Rectangle {
                        visible: spaceItem.unreadTotal > 0
                                 && !spaceItem.isActive
                        width: Math.max(AppTheme.scaled(18),
                                        badgeLabel.implicitWidth
                                        + AppTheme.scaled(6))
                        height: AppTheme.scaled(18)
                        radius: height / 2
                        color: spaceItem.highlightTotal > 0
                               ? AppTheme.mentionBadge : AppTheme.unreadBadge
                        border.color: AppTheme.rail
                        border.width: 2
                        anchors.top: parent.top
                        anchors.right: parent.right
                        anchors.topMargin: -AppTheme.scaled(5)
                        anchors.rightMargin: -AppTheme.scaled(5)

                        Label {
                            id: badgeLabel
                            anchors.centerIn: parent
                            text: spaceItem.unreadTotal > 99
                                  ? "99+" : spaceItem.unreadTotal.toString()
                            font.pixelSize: AppTheme.textMicro
                            font.weight: AppTheme.weightBold
                            color: spaceItem.highlightTotal > 0
                                   ? AppTheme.dangerText : AppTheme.accentText
                        }
                    }
                }

                // Drag to rearrange. Vertical only — the rail is a column, and
                // a sideways twitch is not a reorder. Pseudo rows and subspace
                // rows are excluded: "All rooms" is a view of everything, and a
                // subspace's position belongs to Matrix.
                //
                // The gesture's state lives in the MODEL, not here.
                DragHandler {
                    id: tileDrag
                    enabled: spaceItem.draggable
                    target: null
                    xAxis.enabled: false
                    onActiveChanged: {
                        if (active) {
                            root.beginTileDrag(spaceItem.entryId,
                                               centroid.scenePosition.y)
                        } else if (app.railEntries.draggingEntryId
                                   === spaceItem.entryId) {
                            app.railEntries.endDrag(true)
                        }
                    }
                    onCentroidChanged: {
                        if (active)
                            root.updateTileDrag(centroid.scenePosition.y)
                    }
                }

                // Right-click: folders are renamed and unmade here, and a
                // Space can be filed without a drag. The primary way to MAKE
                // one is dropping a Space onto another Space.
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    enabled: !root.dragging
                    onTapped: (eventPoint) => {
                        railMenu.entryId = spaceItem.entryId
                        railMenu.isFolder = spaceItem.isFolder
                        railMenu.spaceId = spaceItem.spaceId
                        railMenu.inFolder = spaceItem.inFolder
                                            ? spaceItem.folderId : ""
                        railMenu.collapsed = spaceItem.collapsed
                        railMenu.hierarchyChild = spaceItem.hierarchyChild
                        railMenu.folderName = spaceItem.isFolder
                                              ? spaceItem.name : ""
                        railMenu.spaceMuted =
                            spaceItem.isRealSpace
                            && app.spaceIsMuted(spaceItem.spaceId)
                        railMenu.spaceUnread = spaceItem.unreadTotal
                        var p = spaceItem.mapToItem(Overlay.overlay,
                                                    eventPoint.position.x,
                                                    eventPoint.position.y)
                        railMenu.popup(Overlay.overlay, p.x, p.y)
                    }
                }

                HoverHandler { id: spaceHover }
                Rectangle {
                    anchors.fill: spaceTile
                    anchors.margins: -3
                    radius: spaceTile.radius + 3
                    color: AppTheme.hover
                    visible: spaceHover.hovered && !spaceItem.isActive
                             && !root.dragging
                    // BELOW the tree, not level with it. At equal z the later
                    // sibling wins, so the halo painted over the last three
                    // pixels of the elbow and the line stopped on the halo
                    // instead of on the tile — only while hovered, which is
                    // exactly when the reader is looking at that row.
                    z: -2
                }

                TapHandler {
                    // A single tap on a REAL Space opens its overview — the
                    // unified rooms-and-spaces list REPLACES the chat view
                    // (openSpaceHome also activates the space, so the
                    // room-list column follows). The pseudo tiles only filter:
                    // they have no overview to open and must not tear down the
                    // open room. There is deliberately NO double-tap; the
                    // chevron is the one expansion trigger. Scoped to the tile
                    // band (the expansion rows below carry their own handlers)
                    // and excluding the chevron's gutter — TapHandlers are
                    // non-exclusive across subtrees, so without the exclusion a
                    // chevron click would also navigate.
                    enabled: !root.dragging
                    function pointOnChevron(eventPoint) {
                        if (!expandChevronArea.visible)
                            return false
                        var cp = spaceItem.mapToItem(expandChevronArea,
                                                     eventPoint.position.x,
                                                     eventPoint.position.y)
                        return cp.x >= 0 && cp.x <= expandChevronArea.width
                               && cp.y >= 0
                               && cp.y <= expandChevronArea.height
                    }
                    onTapped: (eventPoint) => {
                        if (eventPoint.position.y > spaceItem.tileBandHeight)
                            return
                        if (pointOnChevron(eventPoint))
                            return
                        if (spaceItem.isFolder) {
                            app.railLayout.setFolderCollapsed(
                                spaceItem.entryId, !spaceItem.collapsed)
                            return
                        }
                        if (spaceItem.isRealSpace)
                            app.openSpaceHome(spaceItem.spaceId)
                        else if (app.spaces)
                            app.spaces.activeSpaceId = spaceItem.spaceId
                    }
                }

                // ── The tooltip hangs off the rail, not over it ─────────
                //
                // Attached, not a declared child: a declared ToolTip is a full
                // Popup (background + Label) instantiated PER ROW, and the
                // attached form reuses the one shared instance Main.qml
                // hardens to plain text. A Space name is remote text, so that
                // hardening is not optional and a per-row ToolTip must not be
                // introduced to move this.
                //
                // Qt centres an attached tooltip on the item it is attached to
                // and puts it ABOVE. Attached to the row, that is the middle
                // of the rail one row up — measured covering 15 of the 28
                // pixels of the tile above, in a column whose only identity
                // cue is a two-letter avatar, while ~37px of rail sat unused
                // beside it. So it is attached to an invisible anchor that
                // starts at the rail's right edge and hangs BELOW the row:
                // centred there the tooltip clears the rail entirely, and
                // "above the anchor" puts it beside the row it describes
                // rather than over the one before it.
                Item {
                    objectName: "railSpaceTipAnchor"
                    x: spaceItem.width
                    y: spaceItem.tileBandHeight
                    width: AppTheme.scaled(150)
                    height: 1
                    ToolTip.visible: spaceHover.hovered && !root.dragging
                    ToolTip.text: spaceItem.Accessible.name
                    ToolTip.delay: 500
                }

                // THE REVEALED ROOMS NEED NO FIELD OF THEIR OWN.
                //
                // They used to get a separate rectangle, because a nested
                // Space run sat on a tint while a room run floated on bare
                // rail though both are the same statement about the same
                // tile. Once a tile draws the region it OWNS, that rectangle
                // is the owner's own layer: it already spans this delegate,
                // tile band and revealed rooms together, at the inset and
                // tint one step in from this row's own. Two things drawing
                // one region is how they drift apart.

                // Inline expansion: up to `revealed` of the space's top rooms
                // as 28px tiles, then a "+N" pill revealing 5 more. Tiles
                // indent one step past the owning tile so the hierarchy reads.
                Column {
                    id: expansionCol
                    visible: spaceItem.revealed > 0
                             && spaceItem.revealedRooms.length > 0
                             && !root.dragging
                    y: spaceItem.tileBandHeight
                    width: parent.width
                    spacing: 2

                    Repeater {
                        model: expansionCol.visible
                               ? spaceItem.revealedRooms.slice(
                                     0, spaceItem.revealed)
                               : []
                        delegate: Item {
                            id: expansionRoomRow
                            required property var modelData
                            width: expansionCol.width
                            height: root.railRoomRowBand
                            Rectangle {
                                anchors.fill: roomTile
                                anchors.margins: -2
                                radius: 10
                                color: AppTheme.hover
                                visible: roomHover.hovered
                            }
                            Rectangle {
                                id: roomTile
                                objectName: "railRevealedRoomTile"
                                width: root.railRoomTileSize
                                height: root.railRoomTileSize
                                radius: AppTheme.radiusMd
                                color: "transparent"
                                // THE SAME x AS EVERY OTHER TILE. A room used
                                // to sit half a step further in, giving the
                                // expansion column an offset that lined up
                                // with nothing; now its 28px size is what
                                // says it is a room, exactly as Element's own
                                // one-step avatar shrink does, and the column
                                // stays a column.
                                x: root.tileColumnX
                                   + Math.round((root.railTileSize
                                                 - root.railRoomTileSize) / 2)
                                anchors.verticalCenter: parent.verticalCenter
                                Avatar {
                                    anchors.fill: parent
                                    size: root.railRoomTileSize
                                    circle: expansionRoomRow.modelData
                                                .isDirect === true
                                    squareRadius: AppTheme.radiusMd
                                    labelSize: AppTheme.scaled(11)
                                    name: expansionRoomRow.modelData.name || ""
                                    colorKey: expansionRoomRow.modelData
                                                  .identityColorKey
                                              || expansionRoomRow.modelData
                                                     .roomId
                                    mxc: expansionRoomRow.modelData
                                             .avatarUrl || ""
                                }
                                Rectangle {
                                    visible: expansionRoomRow.modelData
                                                 .hasUnread === true
                                    width: AppTheme.scaled(10)
                                    height: AppTheme.scaled(10)
                                    radius: height / 2
                                    color: (expansionRoomRow.modelData
                                                .highlightCount || 0) > 0
                                           ? AppTheme.mentionBadge
                                           : AppTheme.unreadBadge
                                    border.color: AppTheme.rail
                                    border.width: 2
                                    anchors.top: parent.top
                                    anchors.right: parent.right
                                    anchors.topMargin: -2
                                    anchors.rightMargin: -2
                                }
                            }
                            HoverHandler { id: roomHover }
                            TapHandler {
                                // Opening from the rail also activates the
                                // space so the room-list column follows —
                                // openRoom itself never touches activeSpaceId.
                                onTapped: {
                                    if (app.spaces)
                                        app.spaces.activeSpaceId =
                                            spaceItem.spaceId
                                    app.openRoom(
                                        expansionRoomRow.modelData.roomId)
                                }
                            }
                            // Off the rail, for the reason the Space tile's
                            // own anchor carries: centred on a 28px tile a
                            // tooltip covers its neighbours, and the rail has
                            // nothing else to identify them by.
                            Item {
                                x: expansionRoomRow.width
                                   - expansionRoomRow.x
                                y: expansionRoomRow.height
                                width: AppTheme.scaled(150)
                                height: 1
                                ToolTip.visible: roomHover.hovered
                                ToolTip.text: expansionRoomRow.modelData.name
                                              || expansionRoomRow.modelData
                                                     .roomId
                                ToolTip.delay: 300
                            }
                            Accessible.role: Accessible.Button
                            Accessible.name: expansionRoomRow.modelData.name
                                             || expansionRoomRow.modelData
                                                    .roomId
                        }
                    }

                    Item {
                        visible: spaceItem.revealedRooms.length
                                 > spaceItem.revealed
                        width: expansionCol.width
                        height: AppTheme.scaled(22)
                        Rectangle {
                            id: morePill
                            objectName: "railSpaceMoreButton"
                            width: AppTheme.scaled(32)
                            height: AppTheme.scaled(18)
                            radius: height / 2
                            color: moreHover.hovered ? AppTheme.hover
                                                     : AppTheme.cardElevated
                            border.color: AppTheme.border
                            border.width: 1
                            x: root.tileColumnX
                               + Math.round((root.railTileSize - morePill.width) / 2)
                            anchors.verticalCenter: parent.verticalCenter
                            Label {
                                anchors.centerIn: parent
                                text: "+" + Math.min(
                                          5,
                                          spaceItem.revealedRooms.length
                                          - spaceItem.revealed)
                                font.pixelSize: AppTheme.textMicro
                                font.weight: AppTheme.weightBold
                                color: AppTheme.textSecondary
                            }
                        }
                        HoverHandler { id: moreHover }
                        TapHandler {
                            onTapped: root.showMoreRooms(spaceItem.spaceId)
                        }
                        Item {
                            x: morePill.width
                            y: morePill.height
                            width: AppTheme.scaled(150)
                            height: 1
                            ToolTip.visible: moreHover.hovered
                            ToolTip.text: qsTr("Show more rooms")
                            ToolTip.delay: 300
                        }
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Show more rooms")
                    }
                }
            }

            // Add Space: part of the list CONTENT, so it sits directly below
            // the last Space tile, scrolls with the tiles, and never overlaps
            // the pinned Settings/account cluster.
            footer: Item {
                width: list.width
                height: railAddSpaceButton.visible ? 48 : 0
                IconButton {
                    id: railAddSpaceButton
                    objectName: "railAddSpaceButton"
                    y: AppTheme.scaled(4)
                    x: root.tileColumnX
                    implicitWidth: root.railTileSize
                    implicitHeight: root.railTileSize
                    radius: AppTheme.radiusLg
                    iconName: "add"
                    iconSize: AppTheme.scaled(22)
                    visible: app.loggedIn && app.conversations
                             && app.conversations.supported
                    Accessible.name: qsTr("Create a Space")
                    ToolTip.text: qsTr("Create a Space")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: root.createSpaceRequested()
                    // Soft dashed-affordance treatment: a quiet outline
                    // distinguishes "add" from real Space tiles.
                    Rectangle {
                        anchors.fill: parent
                        z: -1
                        radius: AppTheme.radiusLg
                        color: "transparent"
                        border.width: 1
                        border.color: AppTheme.borderStrong
                    }
                }
            }
        }

        // ── Bottom cluster: settings + account ─────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing12
            Layout.rightMargin: AppTheme.spacing12
            implicitHeight: 1
            color: AppTheme.separator
            visible: app.loggedIn
        }

        Item { implicitHeight: AppTheme.spacing12; visible: app.loggedIn }

        IconButton {
            id: railSettingsButton
            objectName: "railSettingsButton"
            // LEFT, with the trunk. The tiles above are no longer centred,
            // so a centred cog and avatar would be the only two things in the
            // rail that move when it is widened.
            Layout.alignment: Qt.AlignLeft
            Layout.leftMargin: root.tileColumnX
            implicitWidth: root.railTileSize
            implicitHeight: root.railTileSize
            radius: AppTheme.radiusLg
            iconName: "settings"
            iconSize: AppTheme.scaled(22)
            // Accent chip while the in-shell Settings view is open;
            // clicking again returns to chat.
            active: app.currentScreen === 2
            visible: app.loggedIn
            // v0.7.x: the badge names WHY the cog wants attention, so a
            // screen reader is not left with a bare "Settings" while a red
            // dot sits on it.
            // ONE badge, never two dots on 68px of chrome. Verification
            // outranks an update: a security state the user must act on is
            // not the same class of thing as a release being available, and
            // colouring them alike would devalue the red one.
            // The PERSISTENT fact, not the dismissible card: dismissing the
            // corner prompt stops the interruption, and the badge is what is
            // left to say an update is still waiting.
            readonly property bool _updateBadge:
                app.updateManager && app.updateManager.updateAvailable
            readonly property string _attentionText:
                app.sessionVerificationWarning
                    ? qsTr("Settings — this session is not verified")
                    : (_updateBadge
                       ? qsTr("Settings — a Lightning update is available")
                       : qsTr("Settings"))
            Accessible.name: _attentionText
            ToolTip.text: _attentionText
            ToolTip.visible: hovered
            ToolTip.delay: 500
            onClicked: app.currentScreen === 2 ? app.showMain()
                                               : app.showSettings()

            // Attention badge: this session is not verified AND the user
            // has not dismissed the reminder. A dot, not a full "!" glyph —
            // the rail is 68px of chrome and the tooltip carries the words.
            // Ringed in the rail colour so it reads as a badge sitting ON
            // the cog rather than part of the glyph.
            Rectangle {
                objectName: "railSettingsAlertBadge"
                visible: app.sessionVerificationWarning || parent._updateBadge
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: AppTheme.scaled(6)
                width: AppTheme.scaled(10)
                height: AppTheme.scaled(10)
                radius: height / 2
                // `danger`/`warning` became INK-ONLY roles on 2026-08-21
                // (they route light on dark themes so they stay AA as text);
                // a badge is a FILL and must ask for the saturated fill by
                // name, or the dot renders as a pale rose smudge on the rail.
                color: app.sessionVerificationWarning ? AppTheme.dangerFill
                                                      : AppTheme.warningFill
                border.color: AppTheme.rail
                border.width: 2
            }
        }

        Item { implicitHeight: AppTheme.spacing8; visible: app.loggedIn }

        // Account avatar (40 px circle) with presence dot; opens the
        // account switcher popover.
        Item {
            id: railAccount
            objectName: "railAccountTile"
            // LEFT, with the trunk. The tiles above are no longer centred,
            // so a centred cog and avatar would be the only two things in the
            // rail that move when it is widened.
            Layout.alignment: Qt.AlignLeft
            Layout.leftMargin: root.tileColumnX
            // implicitWidth, NOT width: this is a ColumnLayout child, and a
            // layout takes an aligned item's PREFERRED size — which falls
            // back to whatever `width` happened to be at the first pass and
            // then never looks again. Written as `width` it stayed 40px at
            // every interface size while the cog above it (already an
            // implicit size) scaled correctly.
            implicitWidth: root.railTileSize
            implicitHeight: root.railTileSize
            visible: app.loggedIn

            // Invokable results do not re-evaluate on signals; refresh the
            // record whenever the registry or selection changes.
            property var activeAccount: ({})
            function refreshAccount() {
                activeAccount = app.accounts
                    ? app.accounts.account(app.accounts.activeUserId) : ({})
            }
            Component.onCompleted: refreshAccount()
            Connections {
                target: app.accounts
                function onAccountsChanged() { railAccount.refreshAccount() }
                function onActiveUserIdChanged() { railAccount.refreshAccount() }
            }
            readonly property string localpart: {
                var uid = app.accounts ? (app.accounts.activeUserId || "") : ""
                if (uid.startsWith("@")) uid = uid.slice(1)
                var colon = uid.indexOf(":")
                return colon > 0 ? uid.slice(0, colon) : uid
            }

            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Account menu for %1")
                             .arg(app.accounts ? app.accounts.activeUserId : "")

            Avatar {
                id: railAvatar
                anchors.fill: parent
                // Same rule as the Space tiles: Avatar's mask is a permille
                // of `size`, so it must be the rendered edge.
                size: root.railTileSize
                circle: true
                name: railAccount.activeAccount.displayName
                      || railAccount.localpart
                mxc: railAccount.activeAccount.avatarUrl || ""
                // Key by the MXID like every other self-avatar surface —
                // a display-name key gave the rail its own colour and
                // recoloured on rename.
                colorKey: app.accounts ? app.accounts.activeUserId : ""
            }
            // LOCAL CONNECTIVITY, not Matrix presence. Matrix presence
            // landed in v0.7.x (PresenceDot.qml) and is shown for OTHER
            // users — on DM rows, the People list and the profile popover.
            // This dot deliberately stays a sync-connection indicator: it
            // answers "is this client talking to the homeserver", which is
            // a different question from "what state has this account
            // published", and conflating the two would let a network blip
            // read as the user going away. It uses the presence palette
            // only because those are the app's online/away inks.
            Rectangle {
                objectName: "railConnectionDot"
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                width: AppTheme.scaled(11)
                height: AppTheme.scaled(11)
                radius: height / 2
                border.color: AppTheme.rail
                border.width: 2
                color: app.connectionStatus === qsTr("Connected")
                       ? AppTheme.presenceOnline : AppTheme.presenceAway
                Accessible.role: Accessible.Indicator
                Accessible.name: app.connectionStatus === qsTr("Connected")
                                 ? qsTr("Connected to your homeserver")
                                 : qsTr("Not connected to your homeserver")
            }

            HoverHandler { id: accountHover }
            Rectangle {
                anchors.fill: parent
                anchors.margins: -3
                radius: AppTheme.radiusPill
                color: AppTheme.hover
                visible: accountHover.hovered
                z: -1
            }
            TapHandler { onTapped: railAccountMenu.open() }
            ToolTip.visible: accountHover.hovered
            ToolTip.text: app.accounts ? (app.accounts.activeUserId || "") : ""
            ToolTip.delay: 500

            AccountMenu {
                id: railAccountMenu
                x: parent.width + AppTheme.spacing8
                // v0.6.5: the vertical identity-card stack can grow far
                // taller than the old single-header popover. By default it
                // still grows upward from the rail avatar's bottom (unchanged
                // behavior), but it must never push its top above the
                // window's top edge. `root.height` is read only to force
                // this binding to re-evaluate on a window resize —
                // Item.mapFromItem() results are not tracked reactively by
                // QML's binding engine on their own.
                readonly property real _windowTopLocalY: {
                    var _dep = root.height
                    return parent ? parent.mapFromItem(null, 0, 0).y : 0
                }
                y: Math.max(_windowTopLocalY + AppTheme.spacing12,
                            parent.height - implicitHeight)
            }
            // Development-only: the screenshot-demo "account-switching" scenario
            // opens the real account switcher popover. Null target in a
            // non-demo build makes this an inert no-op.
            Connections {
                target: app.demo
                enabled: app.screenshotDemoActive
                function onAccountSwitcherRequested() { railAccountMenu.open() }
            }
        }
    }

    // ── Folders ──────────────────────────────────────────────────────────
    // Ordering and grouping the rail is DEVICE-LOCAL, deliberately: Matrix
    // has no standard for either, so anything stored on the server would be a
    // private invention only Lightning could read. See RailLayoutStore.
    AppMenu {
        id: railMenu
        objectName: "railContextMenu"
        property string entryId: ""
        property string spaceId: ""
        property string inFolder: ""
        property string folderName: ""
        property bool isFolder: false
        property bool collapsed: false
        // A subspace is Matrix's arrangement. It can be expanded and opened;
        // it cannot be filed or ordered, and offering to would promise
        // something this layer cannot deliver.
        property bool hierarchyChild: false
        // A REAL Space (not Home, not "Other rooms", not a local folder), so
        // the Space-only actions are offered only where they mean something.
        readonly property bool isRealSpace:
            !isFolder && spaceId.charAt(0) === "!"
        // Sampled when the menu opens, not bound: spaceIsMuted() is a call
        // over every room in the Space and carries no NOTIFY of its own.
        property bool spaceMuted: false
        // Same treatment, same reason: read once from the row that was
        // right-clicked, so "Mark as read" can be honestly disabled when
        // there is nothing to mark.
        property int spaceUnread: 0
        // Sable's menu names the Space it belongs to at the top. AppMenu's
        // own context header does that here — the row it was opened from is
        // no longer under the pointer once the menu is up.
        contextLabel: railMenu.isFolder
                      ? railMenu.folderName
                      : (app.spaces ? app.spaces.spaceName(railMenu.spaceId)
                                    : "")

        AppMenuItem {
            iconName: railMenu.collapsed ? "expand_more" : "expand_less"
            text: railMenu.collapsed ? qsTr("Expand folder")
                                     : qsTr("Collapse folder")
            visible: railMenu.isFolder
            onTriggered: app.railLayout.setFolderCollapsed(railMenu.entryId,
                                                           !railMenu.collapsed)
        }
        AppMenuItem {
            iconName: "edit_square"
            text: qsTr("Rename folder…")
            visible: railMenu.isFolder
            onTriggered: {
                folderNameDialog.folderId = railMenu.entryId
                folderNameDialog.openFor(railMenu.folderName)
            }
        }
        AppMenuItem {
            objectName: "railDeleteFolder"
            iconName: "delete"
            text: qsTr("Delete folder")
            visible: railMenu.isFolder
            // The Spaces inside come back to the top level where the folder
            // was; nothing is left and nothing is removed from the account,
            // so this needs no confirmation.
            onTriggered: app.railLayout.deleteFolder(railMenu.entryId)
        }

        AppMenuSeparator { visible: railMenu.isFolder }

        // Mute the whole Space. Matrix has no "mute a Space" primitive — a
        // Space is a room with no timeline, so muting it silences nothing —
        // so this does what a person would otherwise do by hand to each room
        // inside it. Unmute restores "follow the account default", the state
        // a room is in before anyone touched it, rather than asserting "all
        // messages" for rooms that never asked for it.
        // ── Home: mark EVERY room read ───────────────────────────────
        //
        // Per-room and per-Space have existed for a while and the sweep did
        // not, so an account that had drifted could only be caught up one
        // room at a time. It lives on the Home tile because Home is the one
        // that means "everything"; the Space tile keeps its own scoped
        // version directly below.
        //
        // It clears the bell too: the receipt this sends is what
        // ActivityModel::markRoomReadUpTo listens for, so the room list and
        // the Activity badge come down together rather than one of them
        // being left behind.
        AppMenuItem {
            objectName: "railMarkAllRoomsRead"
            iconName: "done_all"
            text: qsTr("Mark all rooms read")
            visible: !railMenu.isFolder && railMenu.spaceId === ""
            // Nothing unread is nothing to do, and a control that would do
            // nothing should say so — the same restraint the Space item
            // below applies.
            enabled: railMenu.spaceUnread > 0
            onTriggered: app.roomList.markAllRoomsRead()
        }
        AppMenuItem {
            objectName: "railMarkSpaceRead"
            iconName: "done_all"
            text: qsTr("Mark as read")
            visible: railMenu.isRealSpace
            // A Space with nothing unread has nothing to mark, and a control
            // that would do nothing should say so rather than pretend.
            enabled: railMenu.spaceUnread > 0
            onTriggered: app.markSpaceRead(railMenu.spaceId)
        }
        AppMenuItem {
            objectName: "railMuteSpace"
            iconName: railMenu.spaceMuted ? "notifications" : "notifications_off"
            text: railMenu.spaceMuted ? qsTr("Unmute space")
                                      : qsTr("Mute space")
            visible: railMenu.isRealSpace
            onTriggered: app.setSpaceMuted(railMenu.spaceId,
                                           !railMenu.spaceMuted)
        }
        AppMenuItem {
            objectName: "railSpaceInvite"
            iconName: "person_add"
            text: qsTr("Invite")
            // Deliberately NOT gated on canInvite. That gate reads
            // app.roomInfo, which follows whatever surface last pointed it
            // somewhere — usually the open room, not this Space — so gating on
            // it would grey the row out because nobody has LOOKED, which is a
            // different and worse lie than offering something the server may
            // refuse. The invite dialog reports the server's answer honestly.
            visible: railMenu.isRealSpace
            onTriggered: spaceInviteDialog.openFor(railMenu.spaceId)
        }
        AppMenuItem {
            objectName: "railSpaceCopyLink"
            iconName: "content_copy"
            text: qsTr("Copy link")
            visible: railMenu.isRealSpace
            onTriggered: root.copySpaceLink(railMenu.spaceId)
        }
        AppMenuItem {
            objectName: "railSpaceShareLink"
            // Every glyph here has to exist in the bundled Material Symbols
            // SUBSET, which carries exactly the icons the app already uses —
            // a name the subset does not cover renders as tofu, not as a
            // missing icon. IconChromeTest pins the whole set.
            iconName: "link"
            text: qsTr("Share link…")
            visible: railMenu.isRealSpace
            onTriggered: shareLinkDialog.openFor(
                             railMenu.spaceId,
                             app.spaces.spaceName(railMenu.spaceId))
        }
        AppMenuItem {
            objectName: "railSpaceSettings"
            iconName: "settings"
            text: qsTr("Space settings")
            visible: railMenu.isRealSpace
            onTriggered: spaceSettings.openFor(railMenu.spaceId)
        }

        AppMenuSeparator { visible: railMenu.isRealSpace }

        AppMenuItem {
            objectName: "railNewFolder"
            iconName: "add"
            text: qsTr("New folder…")
            visible: !railMenu.hierarchyChild
            onTriggered: {
                folderNameDialog.folderId = ""
                folderNameDialog.pendingSpaceId =
                    railMenu.isFolder ? "" : railMenu.spaceId
                folderNameDialog.openFor("")
            }
        }
        AppMenuItem {
            iconName: "logout"
            text: qsTr("Move out of folder")
            visible: !railMenu.isFolder && railMenu.inFolder.length > 0
            onTriggered: app.railLayout.setSpaceFolder(railMenu.spaceId, "")
        }
        // One entry per existing folder, so filing a Space never requires a
        // drag — a rail with twenty Spaces is a long way to drag one.
        Repeater {
            model: (railMenu.isFolder || railMenu.hierarchyChild)
                   ? [] : app.railLayout.folders
            delegate: AppMenuItem {
                required property var modelData
                iconName: "workspaces"
                text: qsTr("Move to “%1”").arg(modelData.name || "")
                visible: modelData.id !== railMenu.inFolder
                onTriggered: app.railLayout.setSpaceFolder(railMenu.spaceId,
                                                           modelData.id)
            }
        }
    }

    // ── What the Space menu opens ────────────────────────────────────────
    // Hosted here, exactly as the folder-name dialog is: a delegate that
    // reached up into its host by id is how the reader popover's click ended
    // up silently dead, so the rows stay signal-only and every shared surface
    // is ONE instance owned by the view.

    // The clipboard write. A hidden TextEdit is the app's existing proxy for
    // this (RoomsPanel uses the same one for room links); text is cleared
    // immediately after the copy so a permalink does not sit in a live item.
    TextEdit {
        id: railLinkClipboard
        visible: false
        width: 0
        height: 0
    }

    // matrix.to, the public link — never an authenticated media or client URL.
    // Alias-preferred, id fallback, which is RoomListModel's own convention.
    function spaceLink(spaceId) {
        if (!app.roomList || !spaceId || spaceId.length === 0)
            return ""
        var row = app.roomList.findRoom(spaceId)
        return app.roomList.roomPermalink(
            spaceId, (row && row.canonicalAlias) || "")
    }
    function copySpaceLink(spaceId) {
        var link = root.spaceLink(spaceId)
        if (link.length === 0)
            return
        railLinkClipboard.text = link
        railLinkClipboard.selectAll()
        railLinkClipboard.copy()
        railLinkClipboard.text = ""
    }

    InvitePeopleDialog {
        id: spaceInviteDialog
        parent: Overlay.overlay
    }

    SpaceSettingsDialog {
        id: spaceSettings
        onInviteRequested: (spaceId) => spaceInviteDialog.openFor(spaceId)
    }

    // Share: the link itself, selectable, with the two things anyone actually
    // does with it. Lightning has no OS share sheet, and inventing one that
    // silently copied would make "Share" and "Copy link" the same control
    // under two names.
    AppDialog {
        id: shareLinkDialog
        objectName: "railShareLinkDialog"
        property string targetId: ""
        property string targetName: ""
        readonly property string link: root.spaceLink(targetId)
        title: qsTr("Share space")
        standardButtons: Dialog.Close
        parent: Overlay.overlay
        anchors.centerIn: parent

        function openFor(spaceId, name) {
            targetId = spaceId
            targetName = name || ""
            open()
        }

        contentItem: ColumnLayout {
            spacing: AppTheme.spacing8
            Label {
                // Remote or externally chosen text: never markup.
                textFormat: Text.PlainText
                Layout.fillWidth: true
                Layout.maximumWidth: 380
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: shareLinkDialog.link.length > 0
                      ? qsTr("Anyone with this link can find “%1”. Whether "
                             + "they can join still depends on the space's "
                             + "access setting.").arg(
                            shareLinkDialog.targetName)
                      : qsTr("This space has no shareable address yet.")
            }
            AppTextField {
                id: shareLinkField
                objectName: "railShareLinkField"
                storm: true
                Layout.fillWidth: true
                Layout.minimumWidth: 340
                readOnly: true
                text: shareLinkDialog.link
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                AppButton {
                    storm: true
                    kind: "primary"
                    iconName: "content_copy"
                    text: qsTr("Copy link")
                    enabled: shareLinkDialog.link.length > 0
                    onClicked: root.copySpaceLink(shareLinkDialog.targetId)
                }
                AppButton {
                    storm: true
                    iconName: "explore"
                    text: qsTr("Open in browser")
                    enabled: shareLinkDialog.link.length > 0
                    onClicked: Qt.openUrlExternally(shareLinkDialog.link)
                }
                Item { Layout.fillWidth: true }
            }
        }
    }

    AppDialog {
        id: folderNameDialog
        objectName: "railFolderNameDialog"
        property string folderId: ""
        // Set when the folder is being created FROM a Space's menu: that
        // Space goes straight into it, which is what "new folder" means when
        // you asked for it while pointing at something.
        property string pendingSpaceId: ""
        title: folderId.length > 0 ? qsTr("Rename folder") : qsTr("New folder")
        standardButtons: Dialog.Ok | Dialog.Cancel
        parent: Overlay.overlay
        anchors.centerIn: parent

        function openFor(name) {
            folderNameField.text = name
            open()
            folderNameField.forceActiveFocus()
            folderNameField.selectAll()
        }

        onAccepted: {
            var name = folderNameField.text
            if (folderId.length > 0) {
                app.railLayout.renameFolder(folderId, name)
            } else {
                var created = app.railLayout.createFolder(name)
                if (created.length > 0 && pendingSpaceId.length > 0)
                    app.railLayout.setSpaceFolder(pendingSpaceId, created)
            }
            pendingSpaceId = ""
        }
        onRejected: pendingSpaceId = ""

        // A LAID-OUT content item, not a bare child. A raw Item dropped into
        // a Dialog's contentData is positioned by nothing: it sat at the
        // bottom of the panel, off-centre, with the header's space above it —
        // reported as "the text to create a folder is not centered and sitting
        // on the bottom of the bubble". AppDialog's own usage note says
        // ColumnLayout for exactly this reason.
        contentItem: ColumnLayout {
            spacing: AppTheme.spacing8

            Label {
                Layout.fillWidth: true
                Layout.maximumWidth: 300
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                // Says the one thing a person needs to know before naming it:
                // this is theirs, and nobody else will ever see it.
                text: qsTr("Folders are only on this device. Other clients, "
                           + "and everyone else, see your spaces unchanged.")
            }
            AppTextField {
                id: folderNameField
                objectName: "railFolderNameField"
                storm: true
                Layout.fillWidth: true
                Layout.minimumWidth: 260
                maximumLength: 40
                placeholderText: qsTr("Folder name")
                onAccepted: folderNameDialog.accept()
            }
        }
    }
}
