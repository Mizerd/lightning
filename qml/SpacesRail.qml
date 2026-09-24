import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The far-left rail: Home, Space tiles, then a bottom cluster with Settings and
// the account avatar (opens the account switcher). Always visible.
//
// Dragging: rows come from app.railEntries, a real QAbstractListModel, so a
// preview position is a genuine beginMoveRows. Neighbours animate aside while
// the pointer is down and the dragged delegate is never destroyed. The tile
// itself follows the pointer, and where it sits is where it lands; no separate
// insertion line. The only overlay is a ring on the Space or folder a release
// would file into. Nothing moves while the pointer is on a tile (see
// `readingAt`), so that target needs no dwell.
Rectangle {
    id: root
    color: AppTheme.rail

    // Emitted by the Add Space tile; MainScreen opens the creation dialog in
    // Space mode.
    signal createSpaceRequested()

    // The Direct Messages tab, Channels only. Classic reaches DMs through its
    // People filter chip.
    readonly property bool peopleTabVisible:
        app.settings && app.settings.roomNavigationLayout === 1
    Binding {
        target: app.railEntries
        property: "peopleEntryVisible"
        value: root.peopleTabVisible
    }
    // "Other rooms", Classic only: in Channels, Home already lists exactly the
    // rooms in no Space.
    Binding {
        target: app.railEntries
        property: "orphansEntryVisible"
        value: !root.peopleTabVisible
    }
    // Tell the badge model too: a tile's badge counts what its view lists, and
    // Home lists different things in the two layouts.
    Binding {
        target: app.spaces
        property: "directMessagesHaveOwnTile"
        value: root.peopleTabVisible
    }
    // A selection whose tile disappears (People tab when switching to Classic)
    // must not survive, or the shell stays scoped to an invisible tab.
    onPeopleTabVisibleChanged: {
        if (!peopleTabVisible && app.spaces
            && app.spaces.activeSpaceId === "@people")
            app.spaces.activeSpaceId = ""
        // Likewise the "Other rooms" tile when switching to Channels; land on
        // Home, which shows the same rooms there.
        if (peopleTabVisible && app.spaces
            && app.spaces.activeSpaceId === "@orphans")
            app.spaces.activeSpaceId = ""
    }

    // How many of a Space's rooms are revealed under its tile. Session state,
    // unlike the expansion (persisted by RailLayoutStore). Held on the rail
    // root so recycling and model resets do not lose it.
    property var railReveal: ({})
    // Bumped on every SpaceManager change so the revealed-rooms bindings
    // (function calls) re-evaluate.
    property int spacesRevision: 0

    /// A row's height: tile plus air above and below. The divider row adds its
    /// own band. One source, because the drag's row arithmetic and the delegate
    /// must agree at every interface size.
    readonly property int normalRowBand: railTileSize + 2 * rowPad
    readonly property int dividerRowBand: normalRowBand + AppTheme.scaled(10)
    readonly property int railRoomRowBand: railRoomTileSize + 2 * rowPad

    /// Tiles do not indent and nothing is drawn between them: containment is a
    /// tinted region behind the run (as in Discord and this rail's folders), and
    /// depth beyond the first step is carried by tile size. Neither costs width.
    /// The chevron is positioned by its ink, not its box (the glyph's advance is
    /// wider than its ink, and a right-anchored box leaves side bearing in the
    /// gutter).
    readonly property int chevronGlyphSize: AppTheme.scaled(16)
    /// Approximate ink width of the chevron glyph relative to its font size;
    /// used to place the ink rather than the box.
    readonly property int chevronInkWidth: Math.round(chevronGlyphSize * 0.36)
    /// Ink to tile: a control sits against the thing it acts on.
    readonly property int chevronTileGap: AppTheme.scaled(2)
    /// How far the active ring reaches outside its tile. The chevron's gap is
    /// measured from the ring, not the tile, or the glyph overlaps the ring on
    /// the selected tile. The gutter's budget is written only at railSideMargin.
    readonly property int tileRingOutset: AppTheme.scaled(2)

    /// Tooltips hang off an invisible anchor at the rail's right edge. The Basic
    /// style centres a tooltip on its anchor, so a tip wider than the anchor
    /// spills back over the rail. The anchor is therefore at least as wide as
    /// the shared tooltip (railTipWidth, the one instance Main.qml configures),
    /// with 150 as a floor. Read as a property so the binding tracks it. No
    /// loop: the tip's implicitWidth depends only on its text and font.
    readonly property int railTipFloor: AppTheme.scaled(150)
    readonly property real railTipWidth:
        ToolTip.toolTip ? ToolTip.toolTip.implicitWidth : 0
    /// Left wall of the chevron's slot: the deepest region inset any row can
    /// carry, so the glyph never leaves the innermost region. Derived from the
    /// cap, not the row's depth, so chevrons align across levels.
    readonly property int chevronSlotLeft: bandInset(maxBandLayers)
    /// The self badge sits on the avatar's edge: its centre is placed at avatarR
    /// + dotR - ring along the 45° diagonal, so only its rail-coloured ring
    /// overlaps the picture.
    readonly property int selfDotSize: AppTheme.scaled(13)
    /// Matches PresenceDot's 2px inset of its state disc.
    readonly property int selfDotRing: 2
    readonly property real selfDotMargin:
        railTileSize
        - (railTileSize / 2
           + (railTileSize / 2 + selfDotSize / 2 - selfDotRing) / Math.SQRT2
           + selfDotSize / 2)
    /// A plate behind the chevron, resting and hovered, so it reads as a control
    /// rather than a stray mark. Square and sized for the wider of the two
    /// glyphs: expand_more and chevron_right have transposed ink boxes, so any
    /// other shape looks like it changes size between states.
    readonly property int chevronPlatePad: AppTheme.scaled(2)
    readonly property int chevronInkLongest: AppTheme.scaled(8)
    readonly property int chevronPlateWidth:
        chevronInkLongest + 2 * chevronPlatePad
    readonly property int chevronPlateHeight: chevronPlateWidth
    /// expand_more's ink sits about 1px high in its box, so nudge it down; place
    /// the ink, never the box.
    readonly property int chevronInkRise: AppTheme.scaled(1)
    /// One place decides where the control goes. The plate is the control's real
    /// edge, so the gap to the ring is measured from the plate.
    readonly property real chevronPlateLeft:
        Math.max(chevronSlotLeft,
                 tileColumnX - tileRingOutset - chevronTileGap
                 - chevronPlateWidth)
    readonly property real chevronInkLeft:
        chevronPlateLeft + (chevronPlateWidth - chevronInkWidth) / 2
    /// One margin on both sides, so the tile column stays centred on the rail.
    /// The left gutter holds the chevron control: inset 3 + plate 12 + gap 2 +
    /// ring 2 = 19. The same air appears on the right. If the rail must narrow,
    /// shrink the plate. The Icon's box width is its advance and its ink centres
    /// in it, so the placement is written at the Icon itself.
    readonly property int railSideMargin:
        root.classicDepth ? AppTheme.scaled(10) : AppTheme.scaled(19)

    /// Classic depth: a plain top-level Space list, as before the hierarchy.
    /// RailEntryModel::setFlat drops subspaces and expansion from the rows; this
    /// component then draws no regions, cap backdrops or chevron plates, one
    /// tile size, and the narrower margin. `app.settings` is read defensively
    /// because suites load this component standalone. Nothing is hidden:
    /// subspaces stay joined and reachable via their parent's room list, search
    /// and permalinks. Expansion state is kept, so switching back restores the
    /// rail exactly.
    readonly property bool classicDepth:
        app.settings ? app.settings.spacesRailDepthStyle === 1 : false
    /// Past the default width the tile grows up to a ceiling, then the margins
    /// take the rest. 40..48: a denser column than Discord's (tint layers,
    /// nested steps, revealed rooms), so a smaller unit. Derived tiers stay
    /// legible (a room tile is 28 at the 40 floor).
    readonly property int railTileSize:
        Math.min(AppTheme.scaled(48),
                 Math.max(AppTheme.scaled(40),
                          width - 2 * railSideMargin))
    /// Proportional corners (0.30 of the tile) at every tier via
    /// tileRadiusFor(), so the size ladder is not undone by flatter corners.
    readonly property int railTileRadius: tileRadiusFor(railTileSize)
    function tileRadiusFor(size) { return Math.round(size * 0.30) }
    /// Glyph size inside a pseudo tile (Home, People, cog, "+"): half the tile.
    readonly property int railChipIconSize: Math.round(railTileSize * 0.5)
    /// Both dividers, one rule.
    readonly property int railDividerWidth: Math.round(railTileSize * 0.8)
    readonly property int railDividerX:
        tileColumnX + Math.round((railTileSize - railDividerWidth) / 2)
    /// One centred x for every tile. Extra width splits evenly.
    readonly property int tileColumnX: Math.round((width - railTileSize) / 2)
    /// One step down for anything nested, however deep (as Element does). The
    /// derived size must have the same parity as railTileSize, or the halved
    /// difference is .5 and the tile sits half a pixel off the shared axis; so
    /// round the half difference and double it. Written out at both tiers rather
    /// than through a helper function, which the binding would not track.
    readonly property int railNestedTileSize:
        railTileSize - 2 * Math.round((railTileSize
                                       - railTileSize * 0.833) / 2)
    /// A revealed room's tile: 0.7 of the Space tile, paired for parity as
    /// above.
    readonly property int railRoomTileSize:
        railTileSize - 2 * Math.round((railTileSize
                                       - railTileSize * 0.7) / 2)
    /// Air above and below a tile in a run, and the extra after a run's last
    /// row.
    readonly property int rowPad: AppTheme.scaled(4)
    /// The gap between two tiles. One number, shared with the bottom cluster.
    readonly property int railTileGap: 2 * rowPad + AppTheme.spacing4
    /// Two gap sizes for two meanings: leaving a nested region needs only enough
    /// air to show the parent's tint; a top-level break must carry on its own.
    readonly property int groupGap: AppTheme.scaled(8)
    readonly property int groupBreakGap: AppTheme.scaled(18)
    /// Regions stack, one per ancestor, outermost first, each inset a step
    /// further, so a parent's region runs unbroken behind all its descendants.
    /// Depth is carried mainly by tone (AppTheme.railNestSurfaces); the inset
    /// provides an edge to see it against. Capped at three: the rail must stay
    /// dark and receding, and more rungs than that are not distinguishable. The
    /// deepest inset, bandInset(maxBandLayers), is the chevron slot's left wall
    /// (chevronSlotLeft); change them together.
    readonly property int maxBandLayers: 3
    /// Base 1, step 1: insets 1/2/3. Moving the deepest inset moves the wall the
    /// expander is clamped against.
    readonly property int bandInsetBase: AppTheme.scaled(1)
    /// 1px: the tone ladder steps evenly in lightness, so the inset only needs
    /// to provide an edge.
    readonly property int bandInsetStep: AppTheme.scaled(1)
    /// Rung 0 is a folder's container, one step outside hierarchy depth 1.
    function bandInset(depth) {
        return bandInsetBase
               + (Math.min(depth, maxBandLayers) - 1) * bandInsetStep
    }
    /// Concentric: an inset of N inside a rounded rectangle is concentric only
    /// if its radius is smaller by exactly N. Scaled, so corners grow with the
    /// interface.
    function bandRadius(depth) {
        // Rounder corners, still concentric: the radius steps by bandInsetStep,
        // exactly as the inset does.
        return Math.max(AppTheme.scaled(4),
                        AppTheme.scaled(16)
                        - Math.min(depth, maxBandLayers) * bandInsetStep)
    }

    /// Both stops are the tile range plus the gutter, so the gutter is constant
    /// across the draggable range.
    readonly property int minRailWidth:
        AppTheme.scaled(40) + 2 * railSideMargin
    readonly property int maxRailWidth:
        AppTheme.scaled(48) + 2 * railSideMargin

    function revealCount(spaceId) {
        // No revealed rooms in Classic. The model drops nested Spaces, but
        // revealed rooms come from railLayout's expansion state, which Classic
        // keeps so that switching back is lossless; so the rail declines to
        // read it.
        if (root.classicDepth)
            return 0
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
    // The Space's direct joined child rooms, most recently active first.
    // Unjoined children are join offers and live on Space Home. Direct, not
    // transitive (childRoomsDetailed returns the whole subtree, which listed
    // every descendant's rooms under every ancestor). directChildRoomsDetailed
    // carries lastActivity for the sort; the `|| 0` guards keep it from being
    // NaN.
    function topRoomsInSpace(spaceId) {
        void spacesRevision
        if (!app.spaces || !spaceId || spaceId.charAt(0) !== "!")
            return []
        var rooms = app.spaces.directChildRoomsDetailed(spaceId)
        rooms.sort(function(a, b) {
            return (b.lastActivity || 0) - (a.lastActivity || 0)
        })
        // Then the user's own arrangement from the store, if any. Activity
        // order is the default; unknown rooms keep their place at the end.
        if (!app.railLayout)
            return rooms
        var ids = []
        for (var i = 0; i < rooms.length; ++i)
            ids.push(rooms[i].roomId)
        var wanted = app.railLayout.orderedRooms(spaceId, ids)
        if (wanted.length !== ids.length)
            return rooms
        var byId = {}
        for (var j = 0; j < rooms.length; ++j)
            byId[rooms[j].roomId] = rooms[j]
        var out = []
        for (var k = 0; k < wanted.length; ++k) {
            if (byId[wanted[k]] !== undefined)
                out.push(byId[wanted[k]])
        }
        return out.length === rooms.length ? out : rooms
    }
    Connections {
        target: app.spaces
        function onSpacesChanged() { root.spacesRevision++ }
    }
    // Scroll a requested Space into view. The model has already expanded and
    // rebuilt the rows; Qt.callLater because the ListView has not laid them out
    // yet.
    Connections {
        target: app.railEntries
        function onRevealRequested(spaceId) {
            Qt.callLater(function() {
                if (!app.railEntries)
                    return
                var row = app.railEntries.rowForEntry(spaceId)
                if (row < 0 || row >= list.count)
                    return
                list.positionViewAtIndex(row, ListView.Contain)
            })
        }
    }
    // Expansion lives in the store, so a toggle re-evaluates the reveal
    // bindings.
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

    // View-side drag state. Order, drop target and the reorder/group decision
    // live in RailEntryModel; the view only knows the pointer and the proxy's
    // look.
    readonly property bool dragging: app.railEntries.dragging
    property real dragViewportY: 0
    // The pointer in content coordinates. The dragged tile centres on this so
    // it follows the pointer; the model reorder moves its neighbours.
    property real dragContentY: 0

    function beginTileDrag(entryId, sceneY) {
        if (!app.railEntries.beginDrag(entryId))
            return false
        dragViewportY = list.mapFromItem(null, 0, sceneY).y
        dragContentY = dragViewportY + list.contentY
        return true
    }

    // Each row derives its own band (tile size, trailing gap, divider). Read
    // from the model rather than the delegate, whose `y` is being animated by
    // move/displaced transitions during a drag.
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
        // delegate's trailingGap. `ownsRegion` reduces to "has child rows" here
        // and in the delegate, since the expansion column is hidden during a
        // drag. Uses the same uncapped depth as the delegate.
        var owns = e.bandNextLevel > e.level
        var depth = Math.max(0, e.level) + (owns ? 1 : 0)
        var gap = depth > 0 && e.bandNextLevel >= 0
                  && e.bandNextLevel < depth
                  ? (e.bandNextLevel < 1 ? groupBreakGap : groupGap) : 0
        // The cap seam too, which the delegate adds to its height. Any term in
        // one and not the other maps the pointer to the wrong row.
        var seam = groupGap
        var capOpens = owns && depth > maxBandLayers
                       && e.bandPrevLevel >= maxBandLayers
        var capCloses = depth > maxBandLayers
                        && e.bandNextLevel < depth
                        && e.bandNextLevel >= maxBandLayers
        return tile + 2 * rowPad + gap
               + (capOpens ? seam : 0) + (capCloses ? seam : 0)
    }
    // Derived from row heights, not itemAtIndex(i).y: transitions interpolate y
    // for 140 ms, so a still pointer would oscillate between two slots. Rows
    // are exactly their tile band tall during a drag (revealed rooms are
    // hidden).
    function rowTop(index) {
        var y = 0
        for (var i = 0; i < index && i < list.count; ++i)
            y += rowBand(i) + list.spacing
        return y
    }
    // The tile is the group target; the gap between tiles is the reorder target
    // (Discord's rule). Nothing moves while the pointer is on a tile, so aiming
    // at a tile never disturbs it and no dwell is needed. A 24px group band
    // centred on each tile leaves a 28px reorder gap between adjacent rows.
    readonly property int tileGroupInset: 12
    readonly property int tileGroupBand: 24
    // A total, monotone reading of the pointer: { row: i } (on row i's tile) or
    // { gap: g } (in the gap before row g; `count` is the end).
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
    // The dragged block's own slot holds everything still.
    /// Only a top-level entry can be dropped onto a tile (folders are
    /// top-level). The model refuses otherwise; asking here keeps the view from
    /// sending a gesture that would do nothing.
    function draggedCanGroup() {
        var held = app.railEntries ? app.railEntries.draggingEntryId : ""
        if (!held)
            return false
        var row = app.railEntries.rowForEntry(held)
        if (row < 0)
            return false
        var entry = app.railEntries.entryAt(row)
        return !!entry && entry.hierarchyChild !== true
    }
    function rowIsDraggedBlock(row) {
        var entry = app.railEntries.entryAt(row)
        if (!entry)
            return false
        var held = app.railEntries.draggingEntryId
        return entry.entryId === held
               || (entry.folderId !== undefined && entry.folderId === held)
    }
    // One dispatch for the pointer and the auto-scroll, so auto-scroll steps do
    // not disarm a grouping the user is aiming at.
    function applyPointerReading(contentY) {
        if (!root.dragging)
            return
        var reading = readingAt(contentY)
        // A drag that cannot group (a subspace) treats a tile as a position:
        // above its midpoint is before it, below is after. legalGap() snaps
        // that to a boundary between the dragged row's own siblings, so it
        // cannot reparent.
        if (reading.row !== undefined && !draggedCanGroup()) {
            var rowMid = rowTop(reading.row) + rowBand(reading.row) / 2
            app.railEntries.hoverGap(contentY < rowMid ? reading.row
                                                       : reading.row + 1)
            return
        }
        if (reading.row !== undefined) {
            // A target the pointer has left must also be disarmed: endDrag
            // groups on the flag, not the pointer position.
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

    // Where the dragged tile parks when a release would group: the target row's
    // centre. It shrinks onto the target and stops covering the target ring. -1
    // while reordering.
    readonly property real groupAnchorY: {
        if (!root.dragging || !app.railEntries.grouping)
            return -1
        var r = app.railEntries.rowForEntry(app.railEntries.dropTargetId)
        return r < 0 ? -1 : rowTop(r) + rowBand(r) / 2
    }

    // Auto-scroll while dragging near either end.
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
                // Progressive: faster closer to the edge.
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
            // The row under a still pointer changed, so go through the same
            // dispatch as a pointer move.
            root.dragContentY = root.dragViewportY + list.contentY
            root.applyPointerReading(root.dragContentY)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        // Symmetric with the bottom inset.
        anchors.topMargin: AppTheme.spacing12
        anchors.bottomMargin: AppTheme.spacing12
        spacing: 0

        ListView {
            id: list
            objectName: "spacesRailList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            // Air before the divider, enough that the bottom fade only covers
            // empty rail rather than dimming the last tile.
            Layout.bottomMargin: AppTheme.scaled(16)
            // A real model, so a preview reorder is a move, not a reset.
            model: app.railEntries
            clip: true
            spacing: AppTheme.spacing4
            // No recycling: few rows, and a recycled delegate mid-drag loses
            // the tile.
            reuseItems: false

            // No scrollbar: there is no room beside the tiles, and the fade
            // shows the column continues. Wheel, drag and keyboard still
            // scroll.

            // Bottom fade: a partly scrolled tile dissolves instead of being
            // cut flat, and it is the only scroll indicator. A direct child of
            // the ListView, not its contentItem, so it does not scroll away.
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: AppTheme.scaled(16)
                z: 5
                visible: list.contentHeight > list.height
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "transparent" }
                    GradientStop { position: 1.0; color: AppTheme.rail }
                }
            }

            // Animated moves; `displaced` covers the rows the moved one pushed
            // past.
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
                // Must be declared: an undeclared role reads as undefined
                // silently.
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
                // The handoff divider sits under the last navigation tab,
                // separating the tabs from the Spaces.
                readonly property bool carriesDivider:
                    root.peopleTabVisible ? isPeople : isHome
                // Scaled with its tile: tile + 8 (4px above and below); the
                // divider row adds its own 10.
                readonly property int tileBandHeight:
                    carriesDivider
                    ? root.dividerRowBand
                    : spaceItem.rowTileSize + 2 * root.rowPad
                /// Anything nested is one step smaller, at any depth. Every row
                /// is the same size in Classic, where there are no regions to
                /// explain the step.
                readonly property int rowTileSize:
                    (spaceItem.hierarchyChild && !root.classicDepth)
                    ? root.railNestedTileSize : root.railTileSize

                /// Nested regions this row draws, capped: one per ancestor, plus
                /// its own when it owns the run below.
                readonly property int bandLayers:
                    Math.min(root.maxBandLayers,
                             Math.max(0, spaceItem.level)
                             + (spaceItem.ownsRegion ? 1 : 0))
                /// The ladder rung for the expander's plate: two rungs above the
                /// region it sits on, or two below where that runs past the top
                /// of the ladder (clamping would make the plate the band's own
                /// colour). A row with no region counts as rung 0, so the
                /// plate's contrast is the same collapsed or expanded.
                readonly property int plateRung: {
                    var last = AppTheme.railNestSurfaces.length - 1
                    var up = spaceItem.innermostTint + 2
                    return up <= last ? up
                                      : Math.max(0, spaceItem.innermostTint - 2)
                }
                /// `bandLayers`, not `bandLayers - 1`: the layer at index i
                /// draws depth i + 1, so the innermost layer's depth is
                /// bandLayers.
                readonly property int innermostTint:
                    spaceItem.bandLayers > 0
                    ? spaceItem.bandTint(spaceItem.bandLayers) : 0
                /// Whether a run hangs off this tile: subspace rows or revealed
                /// rooms.
                readonly property bool ownsRegion:
                    spaceItem.bandNextLevel > spaceItem.level
                    || expansionCol.visible
                /// This row's own region depth, uncapped.
                readonly property int trueBandDepth:
                    Math.max(0, spaceItem.level)
                    + (spaceItem.ownsRegion ? 1 : 0)
                /// Past the cap, the innermost layer alternates between the last
                /// two rungs so a parent and the child drawn on it always
                /// differ. Ancestor layers are unchanged.
                function bandTint(depth) {
                    if (depth < spaceItem.bandLayers)
                        return depth
                    var overflow = spaceItem.trueBandDepth - root.maxBandLayers
                    if (overflow <= 0)
                        return depth
                    return root.maxBandLayers + (overflow % 2)
                }
                /// The air after the last row of a run, keyed on the row's
                /// innermost region at its true (uncapped) depth, so sibling
                /// runs never touch and two same-tinted regions cannot merge. A
                /// layer whose own run continues bridges the gap, so the reader
                /// sees the parent's tint between siblings. Past the cap a child
                /// region cannot be inset further, so parent and child would
                /// share an edge. The cap seam is air in the parent's tone, with
                /// the radius keeping it from reading as a slot: a run that ends
                /// shows 8px of the grandparent; a child that begins shows the
                /// parent. Eight, the same as a run ending, so every region
                /// boundary uses one number.
                readonly property int capSeamSize: root.groupGap
                /// Only when the parent's band is present at this inset, so a
                /// cap seam never stacks with a run's end gap.
                readonly property bool capOpens:
                    spaceItem.ownsRegion
                    && spaceItem.trueBandDepth > root.maxBandLayers
                    && spaceItem.bandPrevLevel >= root.maxBandLayers
                readonly property bool capCloses:
                    spaceItem.trueBandDepth > root.maxBandLayers
                    && spaceItem.bandNextLevel < spaceItem.trueBandDepth
                    && spaceItem.bandNextLevel >= root.maxBandLayers
                readonly property int capSeamTop: capOpens ? capSeamSize : 0
                /// Where this row's content starts: the cap seam moves the band
                /// down, and the tile, chevron and revealed rooms must move with
                /// it.
                readonly property int contentTop: capSeamTop
                readonly property int capSeamBottom:
                    capCloses ? capSeamSize : 0
                readonly property int trailingGap:
                    spaceItem.trueBandDepth > 0
                    && spaceItem.bandNextLevel >= 0
                    && spaceItem.bandNextLevel < spaceItem.trueBandDepth
                    ? (spaceItem.bandNextLevel < 1 ? root.groupBreakGap
                                                   : root.groupGap)
                    : 0
                // The seam comes out of the row, not the band, which has only
                // 4px above the tile.
                height: tileBandHeight
                        + (expansionCol.visible ? expansionCol.height + 2 : 0)
                        + spaceItem.trailingGap
                        + spaceItem.capSeamTop + spaceItem.capSeamBottom

                property bool isActive: app.spaces && !isFolder
                                        && app.spaces.activeSpaceId === spaceItem.spaceId
                // Lift from the tile's own slot to sit under the pointer; zero
                // for other rows.
                readonly property real dragLift:
                    !spaceItem.dragged
                    ? 0
                    : (root.groupAnchorY >= 0 ? root.groupAnchorY
                                              : root.dragContentY)
                      - (spaceItem.y + spaceItem.tileBandHeight / 2)
                // Above its neighbours while it travels.
                z: spaceItem.dragged ? 10 : 0
                // Gated on the `expanded` role, not revealCount(), which calls
                // a Q_INVOKABLE and records no dependency: a leaf Space's
                // chevron would open without its rooms appearing until the rail
                // was rebuilt.
                readonly property int revealed:
                    (isRealSpace && spaceItem.expanded)
                        ? root.revealCount(spaceItem.spaceId) : 0
                readonly property var revealedRooms:
                    revealed > 0 ? root.topRoomsInSpace(spaceItem.spaceId) : []
                /// The revealed rooms as the live gesture has arranged them, or
                /// null. While dragging this is the Repeater's model, so what
                /// the reader sees is what the release writes.
                property var roomPreview: null
                property int roomDragIndex: -1
                /// Moves the dragged room to `to`, clamped to the ends.
                function moveRoomPreview(to) {
                    if (!spaceItem.roomPreview || spaceItem.roomDragIndex < 0)
                        return
                    var list = spaceItem.roomPreview
                    var target = Math.max(0, Math.min(list.length - 1, to))
                    if (target === spaceItem.roomDragIndex)
                        return
                    var next = list.slice()
                    next.splice(target, 0, next.splice(
                        spaceItem.roomDragIndex, 1)[0])
                    spaceItem.roomDragIndex = target
                    // A new array: `var` properties compare by reference, so an
                    // in-place splice notifies nothing.
                    spaceItem.roomPreview = next
                }
                function commitRoomOrder() {
                    var list = spaceItem.roomPreview
                    spaceItem.roomPreview = null
                    spaceItem.roomDragIndex = -1
                    if (!list || !app.railLayout || !spaceItem.spaceId)
                        return
                    var ids = []
                    for (var i = 0; i < list.length; ++i)
                        ids.push(list[i].roomId)
                    app.railLayout.setRoomOrder(spaceItem.spaceId, ids)
                }

                Accessible.role: Accessible.Button
                Accessible.name: isFolder
                                 ? qsTr("Folder: %1").arg(spaceItem.name)
                                 : isHome ? qsTr("All rooms")
                                 : isPeople ? qsTr("Direct Messages")
                                 : spaceItem.spaceId === "@orphans"
                                   ? qsTr("Other rooms") : spaceItem.name

                // The open folder's container: one surface behind the header
                // and members, squared off between rows so the run reads as
                // continuous.
                Rectangle {
                    visible: (spaceItem.isFolder && !spaceItem.collapsed)
                             || spaceItem.inFolder
                    // On the same ladder as the hierarchy regions: rung 0,
                    // outside depth 1, behind everything, with scaled margins.
                    // Drawn above the regions or at a raw inset, it flattened a
                    // Space tree filed into a folder.
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

                // Active outline: 2px accent ring offset by tileRingOutset,
                // which the chevron's placement is measured from.
                Rectangle {
                    objectName: "railSpaceActiveRing"
                    anchors.fill: spaceTile
                    anchors.margins: -root.tileRingOutset
                    // Concentric: an outset of N needs a radius larger by
                    // exactly N.
                    radius: root.railTileRadius + root.tileRingOutset
                    color: "transparent"
                    border.color: AppTheme.accent
                    border.width: 2
                    visible: spaceItem.isActive
                }

                // Handoff divider between Home and the Space tiles, four fifths
                // of a tile wide.
                Rectangle {
                    visible: spaceItem.carriesDivider
                    // Centred under the tile; shares its rule with the bottom
                    // separator.
                    width: root.railDividerWidth
                    height: 2; radius: 1
                    color: AppTheme.border
                    x: root.railDividerX
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 2
                }

                // The group field: nested rows sit on a tinted region instead
                // of anything drawn between them (no horizontal cost). At a cap
                // seam this supplies the parent's tone; without it the seam
                // would show the grandparent's band behind the cap layer. Its
                // own objectName (railGroupField) because a geometric test
                // requires each region layer to be narrower than its container,
                // and this one shares the inset by design. A fractional z so
                // paint order does not depend on document order.
                Rectangle {
                    objectName: "railCapBackdrop"
                    // Visible on every row past the cap, not only where a seam
                    // opens: the cap layer's rounded corners would otherwise
                    // expose the grandparent's band. The seam asks whether
                    // there is air; this answers what colour is behind the
                    // child's corners (always its parent). Off in Classic,
                    // which draws no regions.
                    visible: !root.classicDepth
                             && spaceItem.trueBandDepth > root.maxBandLayers
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: root.bandInset(root.maxBandLayers)
                    anchors.rightMargin: root.bandInset(root.maxBandLayers)
                    y: 0
                    height: spaceItem.height - spaceItem.trailingGap
                            + (spaceItem.bandNextLevel >= root.maxBandLayers
                               ? list.spacing + spaceItem.trailingGap : 0)
                    z: -20 + root.maxBandLayers - 0.5
                    // Square by necessity: this fills the notch a child's
                    // rounded corner opens with the parent's colour. A radius
                    // would round it away from the notch and show the
                    // grandparent. A rounded run end would need a separate
                    // rectangle.
                    radius: 0
                    // The other parity: the rung the child is not wearing.
                    color: AppTheme.railNestSurfaces[
                        Math.min(AppTheme.railNestSurfaces.length - 1,
                                 root.maxBandLayers
                                 + ((spaceItem.trueBandDepth
                                     - root.maxBandLayers + 1) % 2))]
                }

                Repeater {
                    // One region per ancestor, outermost first: `model` is the
                    // row's depth, so a depth-3 row draws three rectangles and
                    // a parent's region runs unbroken behind its descendants.
                    // The row that owns the run below draws one more, so an
                    // expanded Space is boxed with its contents. Not hidden
                    // during a drag: stampGroupField runs in applyRows, which
                    // the drag preview also passes through, so the layers shown
                    // are what the release produces. Zero in Classic: a model
                    // of 0 instantiates nothing, where `visible` on each
                    // delegate would build and hide them.
                    model: root.classicDepth
                           ? 0
                           : Math.min(root.maxBandLayers,
                                      Math.max(0, spaceItem.level)
                                      + (spaceItem.ownsRegion ? 1 : 0))
                    delegate: Rectangle {
                        id: bandLayer
                        objectName: "railGroupField"
                        required property int index
                        readonly property int depth: index + 1
                        // Past the cap a row joins the innermost drawn layer,
                        // whose bounds are then "depth >= cap".
                        readonly property int bandDepth:
                            depth === root.maxBandLayers
                            ? root.maxBandLayers : depth
                        // Compared against the neighbours' depths per layer.
                        // The owner's layer opens on the owner; any other layer
                        // opens only where the row above is outside it. Must
                        // survive the cap: `depth === level + 1` is already
                        // clipped away for an over-cap owner.
                        readonly property bool isOwnerLayer:
                            spaceItem.ownsRegion
                            && depth === Math.min(spaceItem.trueBandDepth,
                                                  root.maxBandLayers)
                        readonly property bool opensHere:
                            isOwnerLayer
                            || spaceItem.bandPrevLevel < bandDepth - 1
                        // True depth on the cap layer; the capped depth would
                        // never close.
                        readonly property bool closesHere:
                            spaceItem.bandNextLevel
                            < (depth === root.maxBandLayers
                               ? spaceItem.trueBandDepth : bandDepth)
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: root.bandInset(depth)
                        anchors.rightMargin: root.bandInset(depth)
                        // The cap layer starts below the seam; the seam belongs
                        // to the child that opens.
                        y: depth === root.maxBandLayers
                           ? spaceItem.capSeamTop : 0
                        // Plus the list's spacing while this layer's run
                        // continues, or a 4px strip of rail shows between rows.
                        // Plus the trailing gap for a layer whose run carries
                        // on, since that gap belongs to the run that ended.
                        height: spaceItem.height - spaceItem.trailingGap
                                - (depth === root.maxBandLayers
                                   ? spaceItem.capSeamTop
                                     + spaceItem.capSeamBottom : 0)
                                + (closesHere
                                   ? 0
                                   : list.spacing + spaceItem.trailingGap)
                        // Outermost furthest back.
                        z: -20 + depth
                        radius: root.bandRadius(depth)
                        // Index `depth`, not `depth - 1`: rung 0 is a folder's
                        // container, outside hierarchy depth 1.
                        color: AppTheme.railNestSurfaces[
                            Math.min(AppTheme.railNestSurfaces.length - 1,
                                     spaceItem.bandTint(depth))]
                        Rectangle {
                            // Square off the top while the run continues above.
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

                // The expander, in the gutter left of the tile, never touching
                // the active ring. Right when closed, down when open. Reveals
                // the Space's subspaces (as model rows) and its top rooms.
                Item {
                    id: expandChevronArea
                    objectName: "railSpaceExpandChevron"
                    // Only when there is something to expand (`expandable`,
                    // from the model), and permanent rather than hover-only so
                    // it is discoverable.
                    visible: spaceItem.isRealSpace && spaceItem.expandable
                             && !root.dragging
                    // In the gutter, leaving the tile whole, at one x for every
                    // depth.
                    width: root.tileColumnX
                    height: spaceItem.tileBandHeight
                    x: 0
                    y: spaceItem.contentTop
                    Icon {
                        id: expandGlyph
                        objectName: "railSpaceExpandGlyph"
                        // Placed by its ink: `width` is the glyph's advance
                        // with the ink centred in it, so the box is offset by
                        // half the side bearing. Not a loop: an Icon's width
                        // depends only on font metrics. Tracks its tile,
                        // clamped into the innermost region. Not rounded:
                        // rounding the box moves the ink, and the gutter is
                        // exactly full.
                        x: root.chevronInkLeft
                           - (width - root.chevronInkWidth) / 2
                        anchors.verticalCenter: parent.verticalCenter
                        // Both glyphs sit high in their box by different
                        // amounts (expand_more 1.33, chevron_right 0.67 of the
                        // rise). Not rounded, like x.
                        anchors.verticalCenterOffset:
                            (spaceItem.expanded ? 1.33 : 0.67)
                            * root.chevronInkRise
                        // One meaning: open or closed.
                        name: spaceItem.expanded ? "expand_more"
                                                 : "chevron_right"
                        size: root.chevronGlyphSize
                        // One ink at every depth: the region ladder is narrow
                        // enough that a single colour clears its contrast floor
                        // everywhere.
                        color: chevronHover.hovered ? AppTheme.text
                                                    : AppTheme.textSecondary
                    }
                    // The plate: always drawn, positioned from the same place
                    // as the ink; hover brightens it.
                    Rectangle {
                        objectName: "railSpaceExpandPlate"
                        // Not drawn in Classic: its job is to lift the chevron
                        // off a region, and there are none.
                        visible: !root.classicDepth
                        x: root.chevronPlateLeft
                        width: root.chevronPlateWidth
                        height: root.chevronPlateHeight
                        // Centred on the row; the glyph moves to meet it.
                        anchors.verticalCenter: parent.verticalCenter
                        radius: AppTheme.radiusSm
                        // Two rungs up the rail's own ladder, not `hover`,
                        // which would be brighter than the ladder's ceiling and
                        // the room list. One rung is the step between regions;
                        // two reads as a control.
                        color: chevronHover.hovered
                               ? AppTheme.hover
                               : AppTheme.railNestSurfaces[spaceItem.plateRung]
                        Behavior on color { ColorAnimation { duration: 90 } }
                        z: -1
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
                    // Centred on the column, so a smaller tile reads as
                    // smaller, not misaligned.
                    x: root.tileColumnX
                       + Math.round((root.railTileSize
                                     - spaceItem.rowTileSize) / 2)
                    y: spaceItem.contentTop + AppTheme.scaled(4)
                       + spaceItem.dragLift
                    // This row's own size: a nested tile at the full tile's
                    // radius would look rounder than its parent.
                    radius: root.tileRadiusFor(spaceItem.rowTileSize)
                    // Active: the accent ring plus a soft accent wash.
                    color: spaceItem.dropTarget ? AppTheme.accentSoft
                           : spaceItem.isActive ? AppTheme.accentSoft
                           : spaceItem.isFolder ? AppTheme.cardElevated
                           : spaceItem.pseudo ? AppTheme.cardElevated
                                              : "transparent"
                    // No border here (see railSpaceDropRing): a Qt border
                    // paints inside the bounds, under the children. Full
                    // opacity while dragged. Scale carries the merge: the
                    // dragged tile shrinks onto the target and the target grows
                    // slightly to receive it.
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
                        // Half the tile.
                        size: root.railChipIconSize
                        // Plain text ink when selected: the accent on an accent
                        // wash is unreadable. The wash carries the state.
                        color: spaceItem.isActive ? AppTheme.text
                                                  : AppTheme.textSecondary
                    }

                    // The folder tile is a composite of its members' avatars,
                    // which identifies the folder.
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
                        // Must equal the rendered edge: Avatar bakes the mask
                        // as radius * 1000 / size permille, so a different size
                        // gives the wrong corner.
                        size: spaceItem.rowTileSize
                        circle: false
                        squareRadius: root.tileRadiusFor(spaceItem.rowTileSize)
                        labelSize: AppTheme.scaled(15)
                        name: spaceItem.name
                        colorKey: spaceItem.spaceId
                        mxc: spaceItem.avatarUrl
                    }

                    // Unread count badge with a rail-coloured ring.
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

                    // The group ring, drawn wholly outside the tile (outset by
                    // its own stroke): a border inside the tile would be
                    // covered by the Avatar. A child rather than a sibling so
                    // it rides the tile's scale transform. 3px against the
                    // active ring's 2px so "you are here" and "a release merges
                    // these" differ. It may exceed tileRingOutset because the
                    // chevron is hidden during drags, the only time a tile is a
                    // drop target.
                    Rectangle {
                        id: spaceDropRing
                        objectName: "railSpaceDropRing"
                        readonly property int stroke: AppTheme.scaled(3)
                        anchors.fill: parent
                        anchors.margins: -stroke
                        // Concentric with this row's tile radius.
                        radius: spaceTile.radius + stroke
                        color: "transparent"
                        border.color: AppTheme.accent
                        border.width: stroke
                        visible: spaceItem.dropTarget
                    }
                }

                // Drag to rearrange, vertical only; pseudo rows are excluded.
                // Gesture state lives in the model. Parented to an item
                // covering only the tile band: a handler acts within its
                // parent, and on the whole delegate a press on a revealed room
                // would arm the Space's drag instead.
                Item {
                    id: tileDragArea
                    width: parent.width
                    height: spaceItem.tileBandHeight
                    y: spaceItem.contentTop
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
                // Hover over the tile band only, for the same reason as the
                // DragHandler: on the whole delegate it would report hover over
                // revealed rooms too, lighting the wrong ring and showing the
                // parent's tooltip for a room.
                HoverHandler { id: spaceHover }
                }

                // Right-click: rename or unmake folders, or file a Space
                // without a drag. Dropping a Space onto another is the primary
                // way to make a folder.
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

                // Same outset as the ring; the chevron's gap is budgeted
                // against it.
                Rectangle {
                    anchors.fill: spaceTile
                    anchors.margins: -root.tileRingOutset
                    radius: spaceTile.radius + root.tileRingOutset
                    color: AppTheme.hover
                    visible: spaceHover.hovered && !spaceItem.isActive
                             && !root.dragging
                    // Below the tree, so the halo does not paint over the
                    // elbow.
                    z: -2
                }

                TapHandler {
                    // A tap on a real Space opens its overview (openSpaceHome
                    // also activates the Space). Pseudo tiles only filter and
                    // must not close the open room. No double-tap: the chevron
                    // is the one expansion trigger. Scoped to the tile band and
                    // excluding the chevron gutter, since TapHandlers are
                    // non-exclusive across subtrees.
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

                // Tooltip anchor. Attached rather than a declared ToolTip,
                // which would build a Popup per row; the shared instance in
                // Main.qml is hardened to plain text, which a remote Space name
                // requires. Qt centres an attached tooltip on its item and
                // places it above, which covered the tile above; so it hangs
                // off an invisible anchor starting at the rail's right edge,
                // below the row.
                Item {
                    objectName: "railSpaceTipAnchor"
                    x: spaceItem.width
                    y: spaceItem.contentTop + spaceItem.tileBandHeight
                    width: Math.max(root.railTipFloor, root.railTipWidth)
                    height: 1
                    ToolTip.visible: spaceHover.hovered && !root.dragging
                    ToolTip.text: spaceItem.Accessible.name
                    ToolTip.delay: 500
                }

                // Inline expansion: up to `revealed` top rooms as small tiles,
                // then a "+N" pill revealing 5 more. The reorder gesture is on
                // an item of its own (see below).
                Item {
                    id: roomDragArea
                    x: 0
                    y: expansionCol.y
                    width: parent.width
                    height: expansionCol.height
                    visible: expansionCol.visible
                    z: 1
                    function roomIndexAt(py) {
                        var pitch = root.railRoomRowBand + expansionCol.spacing
                        return Math.floor(Math.max(0, py) / pitch)
                    }
                    DragHandler {
                        target: null
                        xAxis.enabled: false
                        onActiveChanged: {
                            if (active) {
                                spaceItem.roomDragIndex =
                                    roomDragArea.roomIndexAt(
                                        centroid.pressPosition.y)
                                spaceItem.roomPreview =
                                    spaceItem.revealedRooms.slice(
                                        0, spaceItem.revealed)
                            } else {
                                spaceItem.commitRoomOrder()
                            }
                        }
                        onCentroidChanged: {
                            if (active) {
                                spaceItem.moveRoomPreview(
                                    roomDragArea.roomIndexAt(
                                        centroid.position.y))
                            }
                        }
                    }
                }

                Column {
                    id: expansionCol
                    // Keep `visible`, `y`, `width` and `spacing`: without them
                    // the Column lays out nothing, and a default visible: true
                    // adds height rowBand() does not count, so drops land on
                    // the wrong row. The mock fixture reveals no rooms, so
                    // tests cannot see this.
                    visible: spaceItem.revealed > 0
                             && spaceItem.revealedRooms.length > 0
                             && !root.dragging
                    y: spaceItem.contentTop + spaceItem.tileBandHeight
                    width: parent.width
                    spacing: 2

                    Repeater {
                        id: roomRepeater
                        model: !expansionCol.visible
                               ? []
                               : (spaceItem.roomPreview
                                  ? spaceItem.roomPreview
                                  : spaceItem.revealedRooms.slice(
                                        0, spaceItem.revealed))
                        delegate: Item {
                            id: expansionRoomRow
                            required property var modelData
                            required property int index
                            width: expansionCol.width
                            height: root.railRoomRowBand
                            Rectangle {
                                anchors.fill: roomTile
                                anchors.margins: -root.tileRingOutset
                                // Concentric: the tile's radius plus the
                                // outset, at every size.
                                radius: roomTile.radius + root.tileRingOutset
                                color: AppTheme.hover
                                visible: roomHover.hovered
                            }
                            Rectangle {
                                id: roomTile
                                objectName: "railRevealedRoomTile"
                                width: root.railRoomTileSize
                                height: root.railRoomTileSize
                                radius: root.tileRadiusFor(
                                            root.railRoomTileSize)
                                color: "transparent"
                                // The same x as every other tile; the smaller
                                // size says it is a room.
                                x: root.tileColumnX
                                   + Math.round((root.railTileSize
                                                 - root.railRoomTileSize) / 2)
                                anchors.verticalCenter: parent.verticalCenter
                                Avatar {
                                    anchors.fill: parent
                                    size: root.railRoomTileSize
                                    circle: expansionRoomRow.modelData
                                                .isDirect === true
                                    squareRadius: root.tileRadiusFor(
                                                      root.railRoomTileSize)
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
                                // Space so the room list follows; openRoom
                                // never touches activeSpaceId.
                                onTapped: {
                                    if (app.spaces)
                                        app.spaces.activeSpaceId =
                                            spaceItem.spaceId
                                    app.openRoom(
                                        expansionRoomRow.modelData.roomId)
                                }
                            }
                            // Off the rail, like the Space tile's anchor.
                            Item {
                                x: expansionRoomRow.width
                                   - expansionRoomRow.x
                                y: expansionRoomRow.height
                                width: Math.max(root.railTipFloor,
                                                root.railTipWidth)
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
                        // Off the rail: `root.width` is this row's right edge;
                        // `morePill.y` because the pill is centred in a taller
                        // row.
                        Item {
                            x: root.width
                            y: morePill.y + morePill.height
                            width: Math.max(root.railTipFloor,
                                            root.railTipWidth)
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

            // Add Space, part of the list content: it follows the last tile,
            // scrolls with the list and never overlaps the pinned bottom
            // cluster.
            footer: Item {
                width: list.width
                // The tile's own height, so contentHeight is correct.
                height: railAddSpaceButton.visible
                        ? root.railTileSize + 2 * root.rowPad : 0
                IconButton {
                    id: railAddSpaceButton
                    objectName: "railAddSpaceButton"
                    y: AppTheme.scaled(4)
                    x: root.tileColumnX
                    implicitWidth: root.railTileSize
                    implicitHeight: root.railTileSize
                    radius: root.railTileRadius
                    iconName: "add"
                    iconSize: root.railChipIconSize
                    visible: app.loggedIn && app.conversations
                             && app.conversations.supported
                    Accessible.name: qsTr("Create a Space")
                    onClicked: root.createSpaceRequested()
                    // Filled at 40% so it reads as the lightest tile rather
                    // than a gap.
                    Rectangle {
                        anchors.fill: parent
                        z: -1
                        radius: root.railTileRadius
                        color: Qt.rgba(AppTheme.cardElevated.r,
                                       AppTheme.cardElevated.g,
                                       AppTheme.cardElevated.b, 0.4)
                        border.width: 1
                        border.color: AppTheme.borderStrong
                    }
                }
                // Off the rail, like the Space tile's anchor. A sibling of the
                // button, so x is in the footer's full-width coordinates.
                Item {
                    x: root.width
                    y: railAddSpaceButton.y + railAddSpaceButton.height
                    width: Math.max(root.railTipFloor, root.railTipWidth)
                    height: 1
                    ToolTip.visible: railAddSpaceButton.hovered
                    ToolTip.text: qsTr("Create a Space")
                    ToolTip.delay: 500
                }
            }
        }

        // Bottom cluster: settings and account. The same divider as the handoff
        // row, centred on the tile column.
        Rectangle {
            Layout.alignment: Qt.AlignLeft
            Layout.leftMargin: root.railDividerX
            implicitWidth: root.railDividerWidth
            implicitHeight: 2
            radius: 1
            color: AppTheme.separator
            visible: app.loggedIn
        }

        // The column's own gap.
        Item { implicitHeight: root.railTileGap; visible: app.loggedIn }

        IconButton {
            id: railSettingsButton
            objectName: "railSettingsButton"
            // On the column, which is the rail's centre.
            Layout.alignment: Qt.AlignLeft
            Layout.leftMargin: root.tileColumnX
            implicitWidth: root.railTileSize
            implicitHeight: root.railTileSize
            radius: root.railTileRadius
            // A fill, like the Home and People chips, so it sits with the
            // avatar below.
            restingColor: AppTheme.cardElevated
            iconName: "settings"
            iconSize: root.railChipIconSize
            // Accent chip while in-shell Settings is open; clicking again
            // returns to chat.
            active: app.currentScreen === 2
            visible: app.loggedIn
            // The badge says why the cog wants attention (for screen readers
            // too). One badge: verification outranks an update. Shows the
            // persistent state, not the dismissible prompt.
            readonly property bool _updateBadge:
                app.updateManager && app.updateManager.updateAvailable
            readonly property string _attentionText:
                app.sessionVerificationWarning
                    ? qsTr("Settings — this session is not verified")
                    : (_updateBadge
                       ? qsTr("Settings — a Lightning update is available")
                       : qsTr("Settings"))
            Accessible.name: _attentionText
            onClicked: app.currentScreen === 2 ? app.showMain()
                                               : app.showSettings()

            // Off the rail, following the cog's layout x; hangs off its bottom
            // so the tip sits beside the cog.
            Item {
                x: root.width - railSettingsButton.x
                y: railSettingsButton.height
                width: Math.max(root.railTipFloor, root.railTipWidth)
                height: 1
                ToolTip.visible: railSettingsButton.hovered
                ToolTip.text: railSettingsButton._attentionText
                ToolTip.delay: 500
            }

            // Attention badge: this session is unverified and the reminder has
            // not been dismissed. A dot; the tooltip carries the words.
            // Rail-coloured ring.
            Rectangle {
                objectName: "railSettingsAlertBadge"
                visible: app.sessionVerificationWarning || parent._updateBadge
                anchors.right: parent.right
                anchors.top: parent.top
                // On the gear's shoulder, relative to the glyph rather than the
                // button.
                anchors.margins:
                    Math.round((root.railTileSize - root.railChipIconSize) / 2)
                    - AppTheme.scaled(3)
                width: AppTheme.scaled(10)
                height: AppTheme.scaled(10)
                radius: height / 2
                // danger/warning are ink-only roles; a badge is a fill and asks
                // for the saturated fill by name.
                color: app.sessionVerificationWarning ? AppTheme.dangerFill
                                                      : AppTheme.warningFill
                border.color: AppTheme.rail
                border.width: 2
            }
        }

        Item { implicitHeight: root.railTileGap; visible: app.loggedIn }

        // Account avatar with presence; opens the account switcher.
        Item {
            id: railAccount
            objectName: "railAccountTile"
            // On the column, like the cog.
            Layout.alignment: Qt.AlignLeft
            Layout.leftMargin: root.tileColumnX
            // implicitWidth, not width: a layout takes the preferred size, and
            // `width` would stick at its first value instead of scaling.
            implicitWidth: root.railTileSize
            implicitHeight: root.railTileSize
            visible: app.loggedIn

            // Invokable results do not re-evaluate on signals; refresh on
            // registry or selection changes.
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
                // Avatar's mask is a permille of `size`, so it must be the
                // rendered edge.
                size: root.railTileSize
                circle: true
                name: railAccount.activeAccount.displayName
                      || railAccount.localpart
                mxc: railAccount.activeAccount.avatarUrl || ""
                // Keyed by MXID like every self-avatar.
                colorKey: app.accounts ? app.accounts.activeUserId : ""
            }
            // The account's real presence via PresenceDot (unknown renders
            // nothing, offline is a hollow ring). Connection state goes in the
            // tooltip rather than a second colour.
            PresenceDot {
                id: railSelfPresence
                objectName: "railSelfPresenceDot"
                userId: app.accounts ? (app.accounts.activeUserId || "") : ""
                ring: AppTheme.rail
                dotSize: root.selfDotSize
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.rightMargin: root.selfDotMargin
                anchors.bottomMargin: root.selfDotMargin
                Accessible.role: Accessible.Indicator
                Accessible.name: statusText
            }
            // Fallback when the server has no presence to give: the connection
            // dot, visible only then, so the user's own tile always has an
            // indicator.
            Rectangle {
                objectName: "railConnectionDot"
                visible: !railSelfPresence.visible
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.rightMargin: root.selfDotMargin
                anchors.bottomMargin: root.selfDotMargin
                width: root.selfDotSize
                height: root.selfDotSize
                radius: height / 2
                border.color: AppTheme.rail
                border.width: root.selfDotRing
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
            // Off the rail: the user id is the widest tooltip and would cover
            // the cog.
            Item {
                x: root.width - railAccount.x
                y: railAccount.height
                width: Math.max(root.railTipFloor, root.railTipWidth)
                height: 1
                ToolTip.visible: accountHover.hovered
                ToolTip.text: app.accounts
                              ? (app.accounts.activeUserId || "") : ""
                ToolTip.delay: 500
            }

            AccountMenu {
                id: railAccountMenu
                x: parent.width + AppTheme.spacing8
                // The popover grows upward from the tile's bottom; keeping it
                // inside the window is left to `margins`, which QQuickPopup
                // enforces at every reposition. A binding using mapFromItem()
                // is not reactive and was cached before first layout, pushing
                // the popover off-screen.
                margins: AppTheme.spacing12
                y: parent.height - implicitHeight
            }
            // Screenshot demo: the account-switching scenario opens the real
            // popover. Inert in a non-demo build.
            Connections {
                target: app.demo
                enabled: app.screenshotDemoActive
                function onAccountSwitcherRequested() { railAccountMenu.open() }
            }
        }
    }

    // Folders. Ordering and grouping are device-local: Matrix has no standard
    // for either (see RailLayoutStore).
    AppMenu {
        id: railMenu
        objectName: "railContextMenu"
        property string entryId: ""
        property string spaceId: ""
        property string inFolder: ""
        property string folderName: ""
        property bool isFolder: false
        property bool collapsed: false
        // A subspace is Matrix's arrangement: it can be expanded and opened,
        // not filed or ordered.
        property bool hierarchyChild: false
        // Space-only actions are offered only on a real Space.
        readonly property bool isRealSpace:
            !isFolder && spaceId.charAt(0) === "!"
        // Sampled when the menu opens: spaceIsMuted() has no NOTIFY.
        property bool spaceMuted: false
        // Read once so "Mark as read" can be disabled when there is nothing to
        // mark.
        property int spaceUnread: 0
        // Names the Space the menu belongs to, since the row is no longer under
        // the pointer.
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
            // The Spaces inside return to the top level; nothing is removed
            // from the account, so no confirmation.
            onTriggered: app.railLayout.deleteFolder(railMenu.entryId)
        }

        AppMenuSeparator { visible: railMenu.isFolder }

        // Home: mark every room read. It clears the bell too, since the receipt
        // is what ActivityModel::markRoomReadUpTo listens for.
        AppMenuItem {
            objectName: "railMarkAllRoomsRead"
            iconName: "done_all"
            text: qsTr("Mark all rooms read")
            visible: !railMenu.isFolder && railMenu.spaceId === ""
            // Disabled when nothing is unread.
            enabled: railMenu.spaceUnread > 0
            onTriggered: app.roomList.markAllRoomsRead()
        }
        AppMenuItem {
            objectName: "railMarkSpaceRead"
            iconName: "done_all"
            text: qsTr("Mark as read")
            visible: railMenu.isRealSpace
            // Disabled when nothing is unread.
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
            // Not gated on canInvite: app.roomInfo usually points at the open
            // room, not this Space. The invite dialog reports the server's
            // answer.
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
            // Every glyph must exist in the bundled Material Symbols subset, or
            // it renders as tofu. IconChromeTest pins the set.
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
        // One entry per folder, so filing a Space never requires a drag.
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

    // Shared surfaces for the Space menu, one instance owned by the view; rows
    // only emit signals.

    // Hidden TextEdit clipboard proxy (as in RoomsPanel), cleared immediately.
    TextEdit {
        id: railLinkClipboard
        visible: false
        width: 0
        height: 0
    }

    // matrix.to public link; alias preferred, id fallback (RoomListModel's
    // convention).
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

    // Share: the link, selectable, with its two actions. There is no OS share
    // sheet, so no fake one.
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
                // Untrusted text: never markup.
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
        // Set when creating the folder from a Space's menu: that Space goes
        // straight in.
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

        // A laid-out content item: a raw Item in a Dialog's contentData is
        // positioned by nothing (see AppDialog's usage note).
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
                // Folders are local to this device.
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
