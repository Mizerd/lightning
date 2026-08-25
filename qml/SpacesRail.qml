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
// on the Space or folder a release would file into, armed only after a dwell,
// so dragging THROUGH a tile on the way somewhere else can never make a
// folder.
Rectangle {
    id: root
    color: AppTheme.rail

    // Emitted by the Add Space tile below the Space list; MainScreen routes
    // it into the creation dialog's Space mode.
    signal createSpaceRequested()

    // How many of a Space's rooms are revealed under its tile. SESSION state,
    // unlike the expansion itself: "show me five more" is a momentary
    // request, where "this Space is open" is how the user wants to navigate
    // and is persisted by RailLayoutStore. Held on the rail root so ListView
    // recycling and model resets never forget it.
    property var railReveal: ({})
    // Bumped on every SpaceManager change so the revealed-rooms bindings
    // (function calls, which QML tracks through this read) re-evaluate.
    property int spacesRevision: 0
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
    // The space's joined child rooms, most recently active first — the
    // quick-access reading of "top rooms". Unjoined children are join
    // offers, not rooms this rail can open; they live on Space Home.
    function topRoomsInSpace(spaceId) {
        void spacesRevision
        if (!app.spaces || !spaceId || spaceId.charAt(0) !== "!")
            return []
        var rooms = app.spaces.childRoomsDetailed(spaceId)
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
        var first = app.railEntries.entryAt(0)
        firstRowBand = (first && first.pseudo === true
                        && (first.spaceId || "") === "") ? 58 : 48
        dragViewportY = list.mapFromItem(null, 0, sceneY).y
        dragContentY = dragViewportY + list.contentY
        dwellTimer.stop()
        dwellRow = -1
        return true
    }

    // Which row the pointer is over, and whether it is held near that row's
    // centre (the GROUP band) rather than near its edges (the reorder bands).
    property int dwellRow: -1
    // Home's row is taller than the rest (it carries the handoff divider).
    // Sampled once per gesture rather than per pointer move.
    property int firstRowBand: 48
    function rowBand(index) {
        return index === 0 ? firstRowBand : 48
    }
    // DERIVED from the row heights, not read off `itemAtIndex(i).y`.
    //
    // Every row is exactly its tile band tall while a drag is live — the
    // revealed-rooms columns are hidden for the duration — so accumulating is
    // exact. And it is the only way to be animation-independent: the move and
    // displaced transitions interpolate `y` for 140 ms, so a pointer held
    // still would map to one row, then to its neighbour, then back, and the
    // dragged entry would oscillate between two slots.
    function rowAtContentY(contentY) {
        var y = 0
        for (var i = 0; i < list.count; ++i) {
            var band = rowBand(i) + list.spacing
            if (contentY < y + band)
                return i
            y += band
        }
        return -1
    }
    function rowTop(index) {
        var y = 0
        for (var i = 0; i < index && i < list.count; ++i)
            y += rowBand(i) + list.spacing
        return y
    }
    // 12 px of dead zone at each edge of the tile band, so passing through a
    // tile's edges is unambiguously a reorder.
    function pointerOverTileCentre(row, contentY) {
        var rel = contentY - rowTop(row)
        var band = rowBand(row)
        return rel >= 12 && rel <= band - 12
    }
    function updateTileDrag(sceneY) {
        if (!root.dragging)
            return
        var local = list.mapFromItem(null, 0, sceneY)
        dragViewportY = local.y
        var contentY = local.y + list.contentY
        dragContentY = contentY
        var row = rowAtContentY(contentY)
        if (row < 0) {
            app.railEntries.updateDrag(list.count - 1, false)
            return
        }
        var centred = pointerOverTileCentre(row, contentY)
        if (!centred) {
            dwellTimer.stop()
            dwellRow = -1
            app.railEntries.updateDrag(row, false)
            return
        }
        // The dwell: the group gesture arms only after the pointer has stayed
        // in one tile's centre band. Without it, reordering past a Space
        // creates a folder out of it on the way through.
        if (dwellRow !== row) {
            dwellRow = row
            dwellTimer.restart()
            app.railEntries.updateDrag(row, false)
            return
        }
        app.railEntries.updateDrag(row, !dwellTimer.running)
    }
    Timer {
        id: dwellTimer
        interval: 320
        onTriggered: {
            if (root.dragging && root.dwellRow >= 0)
                app.railEntries.updateDrag(root.dwellRow, true)
        }
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
            // The pointer did not move, but the row under it did.
            var contentY = root.dragViewportY + list.contentY
            root.dragContentY = contentY
            var row = root.rowAtContentY(contentY)
            if (row >= 0)
                app.railEntries.updateDrag(row, false)
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

                readonly property bool isFolder: kind === "folder"
                readonly property bool isHome: pseudo && spaceId === ""
                readonly property bool isRealSpace:
                    !pseudo && !isFolder && spaceId.charAt(0) === "!"
                readonly property bool inFolder:
                    folderId.length > 0 && !isFolder

                width: list.width
                // 48 = 40px tile + 4px on each side so the active accent
                // outline (drawn at -4px margins) is never clipped by the
                // list bounds — this was the Home-icon clipping defect.
                // Home carries the handoff divider (32×2) below its tile.
                readonly property int tileBandHeight: isHome ? 58 : 48
                height: tileBandHeight
                        + (expansionCol.visible ? expansionCol.height + 2 : 0)

                property bool isActive: app.spaces && !isFolder
                                        && app.spaces.activeSpaceId === spaceItem.spaceId
                // How far the tile is lifted from its own slot to sit under
                // the pointer. Zero for every row but the one being dragged,
                // so nothing else pays for it.
                readonly property real dragLift:
                    spaceItem.dragged
                    ? root.dragContentY - (spaceItem.y + spaceItem.tileBandHeight / 2)
                    : 0
                // Above its neighbours while it travels over them.
                z: spaceItem.dragged ? 10 : 0
                // Indentation: a filed Space steps in a little, a subspace
                // steps in per level. Capped so a deep tree cannot walk the
                // tile off a 68px rail.
                readonly property int tileIndent:
                    Math.min(14, (inFolder ? 7 : 0)
                                 + (hierarchyChild ? Math.min(2, level) * 6 : 0))
                readonly property int revealed:
                    isRealSpace ? root.revealCount(spaceItem.spaceId) : 0
                readonly property var revealedRooms:
                    revealed > 0 ? root.topRoomsInSpace(spaceItem.spaceId) : []

                Accessible.role: Accessible.Button
                Accessible.name: isFolder
                                 ? qsTr("Folder: %1").arg(spaceItem.name)
                                 : isHome ? qsTr("All rooms")
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
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 6
                    anchors.rightMargin: 6
                    y: 0
                    height: spaceItem.isFolder
                            ? spaceItem.tileBandHeight + list.spacing
                            : spaceItem.height
                              + (spaceItem.folderLast ? 0 : list.spacing)
                    z: -2
                    color: AppTheme.railFolderSurface
                    radius: AppTheme.radiusMd
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

                // Handoff divider between Home and the Space tiles (32×2).
                Rectangle {
                    visible: spaceItem.isHome
                    width: 32; height: 2; radius: 2
                    color: AppTheme.border
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 2
                }

                // Connector notch for a nested subspace, so the rail mirrors
                // the Matrix hierarchy rather than only indenting for it.
                Rectangle {
                    visible: spaceItem.hierarchyChild
                             && !expandChevronArea.visible
                    width: 6; height: 2; radius: 1
                    color: AppTheme.border
                    anchors.verticalCenter: spaceTile.verticalCenter
                    anchors.right: spaceTile.left
                    anchors.rightMargin: 1
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
                    visible: spaceItem.isRealSpace
                             && (spaceHover.hovered || spaceItem.expanded)
                             && !root.dragging
                    anchors.left: parent.left
                    // Up to the accent ring's outer edge (tile - 4px) — the
                    // glyph can never overlap it.
                    width: Math.max(0, spaceTile.x - 4)
                    height: 40
                    y: 4
                    Icon {
                        anchors.centerIn: parent
                        name: spaceItem.expanded ? "expand_more" : "chevron_right"
                        size: 10
                        color: chevronHover.hovered ? AppTheme.text
                                                    : AppTheme.textMuted
                    }
                    HoverHandler { id: chevronHover }
                    TapHandler {
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
                    width: 40; height: 40
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.horizontalCenterOffset: spaceItem.tileIndent
                    y: 4 + spaceItem.dragLift
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
                    border.width: spaceItem.dropTarget ? 2 : 0
                    border.color: AppTheme.accent
                    // Full opacity, always. The tile keeps its normal image
                    // while it is dragged; dimming it made the one thing the
                    // user is looking at the hardest thing to see.
                    scale: spaceItem.dragged ? 1.06 : 1.0

                    Behavior on color { ColorAnimation { duration: 120 } }
                    Behavior on scale { NumberAnimation { duration: 90 } }

                    // Pseudo rows use monochrome icons; real Spaces show
                    // palette initials until the avatar loads.
                    Icon {
                        anchors.centerIn: parent
                        visible: spaceItem.pseudo
                        name: spaceItem.isHome ? "home" : "workspaces"
                        size: spaceItem.isHome ? 22 : 20
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
                        size: 40
                        circle: false
                        squareRadius: AppTheme.radiusLg
                        labelSize: 15
                        name: spaceItem.name
                        colorKey: spaceItem.spaceId
                        mxc: spaceItem.avatarUrl
                    }

                    // Unread count badge (rail-coloured ring per design).
                    Rectangle {
                        visible: spaceItem.unreadTotal > 0
                                 && !spaceItem.isActive
                        width: Math.max(18, badgeLabel.implicitWidth + 6)
                        height: 18
                        radius: 9
                        color: spaceItem.highlightTotal > 0
                               ? AppTheme.mentionBadge : AppTheme.unreadBadge
                        border.color: AppTheme.rail
                        border.width: 2
                        anchors.top: parent.top
                        anchors.right: parent.right
                        anchors.topMargin: -5
                        anchors.rightMargin: -5

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
                            dwellTimer.stop()
                            root.dwellRow = -1
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
                    z: -1
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

                // Attached, not a declared child: a declared ToolTip is a full
                // Popup (background + Label) instantiated PER ROW, and the
                // attached form reuses the one shared instance Main.qml
                // hardens to plain text.
                ToolTip.visible: spaceHover.hovered && !root.dragging
                ToolTip.text: spaceItem.Accessible.name
                ToolTip.delay: 500

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
                            height: 32
                            Rectangle {
                                anchors.fill: roomTile
                                anchors.margins: -2
                                radius: 10
                                color: AppTheme.hover
                                visible: roomHover.hovered
                            }
                            Rectangle {
                                id: roomTile
                                width: 28; height: 28
                                radius: 8
                                color: "transparent"
                                anchors.horizontalCenter: parent.horizontalCenter
                                // One level deeper than the OWNING tile — a
                                // nested space's rooms step in further, not
                                // back to a flat offset.
                                anchors.horizontalCenterOffset:
                                    spaceItem.tileIndent + 5
                                anchors.verticalCenter: parent.verticalCenter
                                Avatar {
                                    anchors.fill: parent
                                    size: 28
                                    circle: expansionRoomRow.modelData
                                                .isDirect === true
                                    squareRadius: 8
                                    labelSize: 11
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
                                    width: 10; height: 10; radius: 5
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
                            ToolTip.visible: roomHover.hovered
                            ToolTip.text: expansionRoomRow.modelData.name
                                          || expansionRoomRow.modelData.roomId
                            ToolTip.delay: 300
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
                        height: 22
                        Rectangle {
                            id: morePill
                            objectName: "railSpaceMoreButton"
                            width: 32; height: 18; radius: 9
                            color: moreHover.hovered ? AppTheme.hover
                                                     : AppTheme.cardElevated
                            border.color: AppTheme.border
                            border.width: 1
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.horizontalCenterOffset:
                                spaceItem.tileIndent + 5
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
                        ToolTip.visible: moreHover.hovered
                        ToolTip.text: qsTr("Show more rooms")
                        ToolTip.delay: 300
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
                    y: 4
                    anchors.horizontalCenter: parent.horizontalCenter
                    implicitWidth: 40; implicitHeight: 40
                    radius: AppTheme.radiusLg
                    iconName: "add"
                    iconSize: 22
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
            Layout.alignment: Qt.AlignHCenter
            implicitWidth: 40; implicitHeight: 40
            radius: AppTheme.radiusLg
            iconName: "settings"
            iconSize: 22
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
                anchors.margins: 6
                width: 10
                height: 10
                radius: 5
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
            Layout.alignment: Qt.AlignHCenter
            width: 40; height: 40
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
                size: 40
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
                width: 11; height: 11; radius: 5.5
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
        AppMenuItem {
            objectName: "railMuteSpace"
            iconName: railMenu.spaceMuted ? "notifications" : "notifications_off"
            text: railMenu.spaceMuted ? qsTr("Unmute space")
                                      : qsTr("Mute space")
            visible: railMenu.isRealSpace
            onTriggered: app.setSpaceMuted(railMenu.spaceId,
                                           !railMenu.spaceMuted)
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
