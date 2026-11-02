import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: root
    color: AppTheme.rail

    ThemedSurface {
        anchors.fill: parent
        role: "rail"
        flatFill: false
    }

    signal createSpaceRequested()

    readonly property bool peopleTabVisible:
        app.settings && app.settings.roomNavigationLayout === 1
    Binding {
        target: app.railEntries
        property: "peopleEntryVisible"
        value: root.peopleTabVisible
    }
    Binding {
        target: app.railEntries
        property: "orphansEntryVisible"
        value: !root.peopleTabVisible
    }
    Binding {
        target: app.spaces
        property: "directMessagesHaveOwnTile"
        value: root.peopleTabVisible
    }
    onPeopleTabVisibleChanged: {
        if (!peopleTabVisible && app.spaces
            && app.spaces.activeSpaceId === "@people")
            app.spaces.activeSpaceId = ""
        if (peopleTabVisible && app.spaces
            && app.spaces.activeSpaceId === "@orphans")
            app.spaces.activeSpaceId = ""
    }

    property var railReveal: ({})
    property int spacesRevision: 0

    /// A row's height: tile plus air above and below. The divider row adds its
    /// own band. One source, because the drag's row arithmetic and the delegate
    /// must agree at every interface size.
    readonly property int normalRowBand: railTileSize + 2 * rowPad
    readonly property int dividerRowBand: normalRowBand + AppTheme.scaled(10)
    readonly property int railRoomRowBand: railRoomTileSize + 2 * rowPad

    readonly property int chevronGlyphSize: AppTheme.scaled(16)
    readonly property int chevronInkWidth: Math.round(chevronGlyphSize * 0.36)
    readonly property int chevronTileGap: AppTheme.scaled(2)
    readonly property int tileRingOutset: AppTheme.scaled(2)

    readonly property int railTipFloor: AppTheme.scaled(150)
    readonly property real railTipWidth:
        ToolTip.toolTip ? ToolTip.toolTip.implicitWidth : 0
    readonly property int chevronSlotLeft: bandInset(maxBandLayers)
    readonly property int selfDotSize: AppTheme.scaled(13)
    readonly property int selfDotRing: 2
    readonly property int otherAccountsDotSize: AppTheme.scaled(10)
    readonly property real selfDotMargin: rimDotMargin(selfDotSize)
    readonly property real otherAccountsDotMargin:
        rimDotMargin(otherAccountsDotSize)
    function rimDotMargin(dotSize) {
        return railTileSize
            - (railTileSize / 2
               + (railTileSize / 2 + dotSize / 2 - selfDotRing) / Math.SQRT2
               + dotSize / 2)
    }
    readonly property int chevronPlatePad:
        AppTheme.scaled(AppTheme.compactDensity ? 0 : 2)
    readonly property int chevronInkLongest: AppTheme.scaled(8)
    readonly property int chevronPlateWidth:
        chevronInkLongest + 2 * chevronPlatePad
    readonly property int chevronPlateHeight: chevronPlateWidth
    readonly property int chevronInkRise: AppTheme.scaled(1)
    readonly property real chevronPlateLeft:
        Math.max(chevronSlotLeft,
                 tileColumnX - tileRingOutset - chevronTileGap
                 - chevronPlateWidth)
    readonly property real chevronInkLeft:
        chevronPlateLeft + (chevronPlateWidth - chevronInkWidth) / 2
    readonly property int railSideMargin:
        root.classicDepth ? AppTheme.scaled(10)
                          : AppTheme.scaled(AppTheme.compactDensity ? 15 : 19)

    readonly property bool classicDepth:
        app.settings ? app.settings.spacesRailDepthStyle === 1 : false
    readonly property int railTileSize:
        Math.min(AppTheme.scaled(AppTheme.railTileMax),
                 Math.max(AppTheme.scaled(AppTheme.railTileMin),
                          width - 2 * railSideMargin))
    readonly property int railTileRadius: tileRadiusFor(railTileSize)
    function tileRadiusFor(size) { return Math.round(size * 0.30) }
    readonly property int railChipIconSize: Math.round(railTileSize * 0.5)
    readonly property int railDividerWidth: Math.round(railTileSize * 0.8)
    readonly property int railDividerX:
        tileColumnX + Math.round((railTileSize - railDividerWidth) / 2)
    readonly property int tileColumnX: Math.round((width - railTileSize) / 2)
    readonly property int railNestedTileSize:
        railTileSize - 2 * Math.round((railTileSize
                                       - railTileSize * 0.833) / 2)
    readonly property int railRoomTileSize:
        railTileSize - 2 * Math.round((railTileSize
                                       - railTileSize * 0.7) / 2)
    readonly property int rowPad: AppTheme.scaled(AppTheme.railRowPadding)
    readonly property int railTileGap: 2 * rowPad + AppTheme.spacing4
    readonly property int groupGap: AppTheme.scaled(AppTheme.railGroupGap)
    readonly property int groupBreakGap: AppTheme.scaled(AppTheme.railGroupBreakGap)
    readonly property int maxBandLayers: 3
    readonly property int bandInsetBase: AppTheme.scaled(1)
    readonly property int bandInsetStep: AppTheme.scaled(1)
    function bandInset(depth) {
        return bandInsetBase
               + (Math.min(depth, maxBandLayers) - 1) * bandInsetStep
    }
    function bandRadius(depth) {
        return Math.max(AppTheme.scaled(4),
                        AppTheme.scaled(16)
                        - Math.min(depth, maxBandLayers) * bandInsetStep)
    }

    readonly property int minRailWidth:
        AppTheme.scaled(AppTheme.railTileMin) + 2 * railSideMargin
    readonly property int maxRailWidth:
        AppTheme.scaled(AppTheme.railTileMax) + 2 * railSideMargin

    function revealCount(spaceId) {
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
    function topRoomsInSpace(spaceId) {
        void spacesRevision
        if (!app.spaces || !spaceId || spaceId.charAt(0) !== "!")
            return []
        var rooms = app.spaces.directChildRoomsDetailed(spaceId)
        var sortMode = app.settings ? app.settings.railSort : 0
        if (sortMode === 1) {
            rooms.sort(function(a, b) {
                var c = (a.name || "").localeCompare(b.name || "")
                return c !== 0 ? c : (a.roomId < b.roomId ? -1 : 1)
            })
            return rooms
        }
        if (sortMode === 2) {
            rooms.sort(function(a, b) {
                return (b.lastActivity || 0) - (a.lastActivity || 0)
            })
            return rooms
        }
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

    readonly property bool dragging: app.railEntries.dragging
    property real dragViewportY: 0
    property real dragContentY: 0

    function cancelTileDrag() {
        if (!root.dragging)
            return false
        app.railEntries.endDrag(false)
        return true
    }
    Keys.onEscapePressed: (event) => { event.accepted = root.cancelTileDrag() }

    property Item focusBeforeDrag: null
    // From the model's own flag, not the delegate's handler: a drop reorders
    // rows and the delegate that began the drag may be gone before its
    // handler reports the end.
    onDraggingChanged: if (!dragging) restoreFocusAfterDrag()
    function restoreFocusAfterDrag() {
        var prev = root.focusBeforeDrag
        root.focusBeforeDrag = null
        if (prev && prev !== root && prev.visible && prev.enabled)
            prev.forceActiveFocus()
    }

    function beginTileDrag(entryId, sceneY) {
        if (!app.railEntries.beginDrag(entryId))
            return false
        dragViewportY = list.mapFromItem(null, 0, sceneY).y
        dragContentY = dragViewportY + list.contentY
        return true
    }

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
    readonly property int tileGroupInset: 12
    readonly property int tileGroupBand: 24
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
    function applyPointerReading(contentY) {
        if (!root.dragging)
            return
        var reading = readingAt(contentY)
        if (reading.row !== undefined && !draggedCanGroup()) {
            var rowMid = rowTop(reading.row) + rowBand(reading.row) / 2
            app.railEntries.hoverGap(contentY < rowMid ? reading.row
                                                       : reading.row + 1)
            return
        }
        if (reading.row !== undefined) {
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

    readonly property real groupAnchorY: {
        if (!root.dragging || !app.railEntries.grouping)
            return -1
        var r = app.railEntries.rowForEntry(app.railEntries.dropTargetId)
        return r < 0 ? -1 : rowTop(r) + rowBand(r) / 2
    }

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
            root.dragContentY = root.dragViewportY + list.contentY
            root.applyPointerReading(root.dragContentY)
        }
    }

    // Whether Create a Space is offered, and the height of its cell. Not
    // readonly so a test can offer it: the mock backend never manages rooms.
    property bool railAddVisible:
        app.loggedIn && app.conversations && app.conversations.supported
    readonly property int railAddCellHeight: railTileSize + 2 * rowPad
    property bool railAddPinned: false
    property bool railAddPinQueued: false
    function queuePinAdd() {
        if (railAddPinQueued)
            return
        railAddPinQueued = true
        Qt.callLater(pinAddIfNeeded)
    }
    function pinAddIfNeeded() {
        railAddPinQueued = false
        if (!list)
            return
        const footerHeight = list.footerItem ? list.footerItem.height : 0
        const tiles = list.contentHeight - footerHeight
        const room = list.height + (railAddPinned ? railAddCellHeight : 0)
        railAddPinned = railAddVisible && tiles + railAddCellHeight > room
    }
    onRailAddVisibleChanged: queuePinAdd()
    Component.onCompleted: queuePinAdd()
    component RailAddSpaceCell: Item {
        property string buttonName: ""
        width: root.width
        height: root.railAddCellHeight
        IconButton {
            id: addButton
            objectName: parent.buttonName
            y: root.rowPad
            x: root.tileColumnX
            implicitWidth: root.railTileSize
            implicitHeight: root.railTileSize
            radius: root.railTileRadius
            iconName: "add"
            iconSize: root.railChipIconSize
            visible: root.railAddVisible
            Accessible.name: qsTr("Create a Space")
            onClicked: root.createSpaceRequested()
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
        Item {
            x: root.width
            y: addButton.y + addButton.height
            width: Math.max(root.railTipFloor, root.railTipWidth)
            height: 1
            ToolTip.visible: addButton.hovered
            ToolTip.text: qsTr("Create a Space")
            ToolTip.delay: 500
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: AppTheme.spacing12
        anchors.bottomMargin: AppTheme.spacing12
        spacing: 0

        ListView {
            id: list
            objectName: "spacesRailList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.bottomMargin: AppTheme.scaled(16)
            model: app.railEntries
            clip: true
            spacing: AppTheme.spacing4
            onContentHeightChanged: root.queuePinAdd()
            onHeightChanged: root.queuePinAdd()
            // No recycling: few rows, and a recycled delegate mid-drag loses
            // the tile.
            reuseItems: false

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: AppTheme.scaled(16)
                z: 5
                visible: list.contentHeight > list.height
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "transparent" }
                    GradientStop {
                        position: 1.0
                        color: AppTheme.surfaceColorAt("rail", 1)
                    }
                }
            }

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
                required property bool hasUnread
                required property int mentionCount
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
                readonly property bool carriesDivider:
                    root.peopleTabVisible ? isPeople : isHome
                readonly property int tileBandHeight:
                    carriesDivider
                    ? root.dividerRowBand
                    : spaceItem.rowTileSize + 2 * root.rowPad
                readonly property int rowTileSize:
                    (spaceItem.hierarchyChild && !root.classicDepth)
                    ? root.railNestedTileSize : root.railTileSize

                readonly property int bandLayers:
                    Math.min(root.maxBandLayers,
                             Math.max(0, spaceItem.level)
                             + (spaceItem.ownsRegion ? 1 : 0))
                readonly property int plateRung: {
                    var last = AppTheme.railNestSurfaces.length - 1
                    var up = spaceItem.innermostTint + 2
                    return up <= last ? up
                                      : Math.max(0, spaceItem.innermostTint - 2)
                }
                readonly property int innermostTint:
                    spaceItem.bandLayers > 0
                    ? spaceItem.bandTint(spaceItem.bandLayers) : 0
                readonly property bool ownsRegion:
                    spaceItem.bandNextLevel > spaceItem.level
                    || expansionCol.visible
                readonly property int trueBandDepth:
                    Math.max(0, spaceItem.level)
                    + (spaceItem.ownsRegion ? 1 : 0)
                function bandTint(depth) {
                    if (depth < spaceItem.bandLayers)
                        return depth
                    var overflow = spaceItem.trueBandDepth - root.maxBandLayers
                    if (overflow <= 0)
                        return depth
                    return root.maxBandLayers + (overflow % 2)
                }
                readonly property int capSeamSize: root.groupGap
                readonly property bool capOpens:
                    spaceItem.ownsRegion
                    && spaceItem.trueBandDepth > root.maxBandLayers
                    && spaceItem.bandPrevLevel >= root.maxBandLayers
                readonly property bool capCloses:
                    spaceItem.trueBandDepth > root.maxBandLayers
                    && spaceItem.bandNextLevel < spaceItem.trueBandDepth
                    && spaceItem.bandNextLevel >= root.maxBandLayers
                readonly property int capSeamTop: capOpens ? capSeamSize : 0
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
                height: tileBandHeight
                        + (expansionCol.visible ? expansionCol.height + 2 : 0)
                        + spaceItem.trailingGap
                        + spaceItem.capSeamTop + spaceItem.capSeamBottom

                property bool isActive: app.spaces && !isFolder
                                        && app.spaces.activeSpaceId === spaceItem.spaceId
                readonly property real dragLift:
                    !spaceItem.dragged
                    ? 0
                    : (root.groupAnchorY >= 0 ? root.groupAnchorY
                                              : root.dragContentY)
                      - (spaceItem.y + spaceItem.tileBandHeight / 2)
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
                property var roomPreview: null
                property int roomDragIndex: -1
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

                activeFocusOnTab: true
                objectName: "railSpaceDelegate"
                function activateTile() {
                    if (spaceItem.isFolder) {
                        app.railLayout.setFolderCollapsed(
                            spaceItem.entryId, !spaceItem.collapsed)
                        return
                    }
                    if (spaceItem.isRealSpace)
                        app.openSpaceHome(spaceItem.spaceId)
                    else if (spaceItem.isHome && spaceItem.isActive)
                        app.openSpaceHome("")
                    else if (app.spaces)
                        app.spaces.activeSpaceId = spaceItem.spaceId
                }
                function openContextMenu(x, y) {
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
                    railMenu.spaceUnread = spaceItem.hasUnread
                                           || spaceItem.mentionCount > 0
                                           || spaceItem.unreadTotal > 0
                    var p = spaceItem.mapToItem(Overlay.overlay, x, y)
                    railMenu.popup(Overlay.overlay, p.x, p.y)
                }
                Keys.onPressed: (event) => {
                    if (root.dragging)
                        return
                    var k = event.key
                    if (k === Qt.Key_Return || k === Qt.Key_Enter
                            || k === Qt.Key_Space) {
                        if (!event.isAutoRepeat)
                            spaceItem.activateTile()
                        event.accepted = true
                    } else if ((k === Qt.Key_Right || k === Qt.Key_Left)
                               && spaceItem.isRealSpace
                               && spaceItem.expandable) {
                        if (!event.isAutoRepeat
                                && (k === Qt.Key_Right) !== spaceItem.expanded)
                            app.railLayout.toggleSpaceExpanded(
                                spaceItem.spaceId)
                        event.accepted = true
                    } else if (k === Qt.Key_Menu
                               || (k === Qt.Key_F10
                                   && (event.modifiers & Qt.ShiftModifier))) {
                        if (!event.isAutoRepeat)
                            spaceItem.openContextMenu(
                            spaceTile.x + spaceTile.width,
                            spaceTile.y + spaceTile.height / 2)
                        event.accepted = true
                    }
                }

                Accessible.role: Accessible.Button
                readonly property bool folderNamed:
                    isFolder && spaceItem.name.length > 0
                    && spaceItem.name !== qsTr("Folder")
                Accessible.name: isFolder
                                 ? (folderNamed
                                    ? qsTr("Folder: %1").arg(spaceItem.name)
                                    : qsTr("Folder"))
                                 : isHome ? qsTr("All rooms")
                                 : isPeople ? qsTr("Direct Messages")
                                 : spaceItem.spaceId === "@orphans"
                                   ? qsTr("Other rooms") : spaceItem.name
                Accessible.description:
                    spaceItem.mentionCount > 0
                    ? qsTr("Mentions: %1").arg(spaceItem.mentionCount)
                    : spaceItem.hasUnread ? qsTr("Unread messages") : ""

                Rectangle {
                    visible: (spaceItem.isFolder && !spaceItem.collapsed)
                             || spaceItem.inFolder
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

                Rectangle {
                    objectName: "railSpaceActiveRing"
                    anchors.fill: spaceTile
                    anchors.margins: -root.tileRingOutset
                    radius: root.railTileRadius + root.tileRingOutset
                    color: "transparent"
                    border.color: AppTheme.railActiveRing
                    border.width: 2
                    visible: spaceItem.isActive
                }

                Rectangle {
                    visible: spaceItem.carriesDivider
                    width: root.railDividerWidth
                    height: 2; radius: 1
                    color: AppTheme.border
                    x: root.railDividerX
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 2
                }

                Rectangle {
                    objectName: "railCapBackdrop"
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
                    color: AppTheme.railNestSurfaces[
                        Math.min(AppTheme.railNestSurfaces.length - 1,
                                 root.maxBandLayers
                                 + ((spaceItem.trueBandDepth
                                     - root.maxBandLayers + 1) % 2))]
                }

                Repeater {
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
                        readonly property int bandDepth:
                            depth === root.maxBandLayers
                            ? root.maxBandLayers : depth
                        readonly property bool isOwnerLayer:
                            spaceItem.ownsRegion
                            && depth === Math.min(spaceItem.trueBandDepth,
                                                  root.maxBandLayers)
                        readonly property bool opensHere:
                            isOwnerLayer
                            || spaceItem.bandPrevLevel < bandDepth - 1
                        readonly property bool closesHere:
                            spaceItem.bandNextLevel
                            < (depth === root.maxBandLayers
                               ? spaceItem.trueBandDepth : bandDepth)
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: root.bandInset(depth)
                        anchors.rightMargin: root.bandInset(depth)
                        y: depth === root.maxBandLayers
                           ? spaceItem.capSeamTop : 0
                        height: spaceItem.height - spaceItem.trailingGap
                                - (depth === root.maxBandLayers
                                   ? spaceItem.capSeamTop
                                     + spaceItem.capSeamBottom : 0)
                                + (closesHere
                                   ? 0
                                   : list.spacing + spaceItem.trailingGap)
                        z: -20 + depth
                        radius: root.bandRadius(depth)
                        color: AppTheme.railNestSurfaces[
                            Math.min(AppTheme.railNestSurfaces.length - 1,
                                     spaceItem.bandTint(depth))]
                        Rectangle {
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

                Item {
                    id: expandChevronArea
                    objectName: "railSpaceExpandChevron"
                    visible: spaceItem.isRealSpace && spaceItem.expandable
                             && !root.dragging
                    width: root.tileColumnX
                    height: spaceItem.tileBandHeight
                    x: 0
                    y: spaceItem.contentTop
                    Icon {
                        id: expandGlyph
                        objectName: "railSpaceExpandGlyph"
                        x: root.chevronInkLeft
                           - (width - root.chevronInkWidth) / 2
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.verticalCenterOffset:
                            (spaceItem.expanded ? 1.33 : 0.67)
                            * root.chevronInkRise
                        name: spaceItem.expanded ? "expand_more"
                                                 : "chevron_right"
                        size: root.chevronGlyphSize
                        color: chevronHover.hovered ? AppTheme.text
                                                    : AppTheme.textSecondary
                    }
                    Rectangle {
                        objectName: "railSpaceExpandPlate"
                        visible: !root.classicDepth
                        x: root.chevronPlateLeft
                        width: root.chevronPlateWidth
                        height: root.chevronPlateHeight
                        anchors.verticalCenter: parent.verticalCenter
                        radius: AppTheme.radiusSm
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
                    x: root.tileColumnX
                       + Math.round((root.railTileSize
                                     - spaceItem.rowTileSize) / 2)
                    y: spaceItem.contentTop + root.rowPad
                       + spaceItem.dragLift
                    radius: root.tileRadiusFor(spaceItem.rowTileSize)
                    color: spaceItem.dropTarget ? AppTheme.accentSoft
                           : spaceItem.isActive ? AppTheme.railTileActive
                           : spaceItem.isFolder ? AppTheme.railTile
                           : spaceItem.pseudo ? AppTheme.railTile
                                              : "transparent"
                    scale: spaceItem.dragged
                           ? (root.groupAnchorY >= 0 ? 0.56 : 1.06)
                           : spaceItem.dropTarget ? 1.08 : 1.0

                    Behavior on color { ColorAnimation { duration: 120 } }
                    Behavior on scale { NumberAnimation { duration: 90 } }

                    Icon {
                        anchors.centerIn: parent
                        visible: spaceItem.pseudo
                        name: spaceItem.isHome ? "home"
                              : spaceItem.isPeople ? "person" : "tag"
                        size: root.railChipIconSize
                        color: spaceItem.isActive ? AppTheme.text
                                                  : AppTheme.textSecondary
                    }

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

                    Rectangle {
                        objectName: "railMentionBadge"
                        visible: spaceItem.mentionCount > 0
                                 && !spaceItem.isActive
                        width: Math.max(AppTheme.scaled(18),
                                        badgeLabel.implicitWidth
                                        + AppTheme.scaled(6))
                        height: AppTheme.scaled(18)
                        radius: height / 2
                        color: AppTheme.mentionBadgeFill
                        border.color: AppTheme.rail
                        border.width: 2
                        anchors.bottom: parent.bottom
                        anchors.right: parent.right
                        anchors.bottomMargin:
                            -AppTheme.scaled(AppTheme.compactDensity ? 2 : 5)
                        anchors.rightMargin: -AppTheme.scaled(5)

                        Label {
                            id: badgeLabel
                            anchors.centerIn: parent
                            text: spaceItem.mentionCount > 99
                                  ? "99+" : spaceItem.mentionCount.toString()
                            font.pixelSize: AppTheme.textMicro
                            font.weight: AppTheme.weightBold
                            color: AppTheme.mentionBadgeInk
                        }
                    }
                    Rectangle {
                        objectName: "railUnreadDot"
                        visible: spaceItem.hasUnread
                                 && spaceItem.mentionCount === 0
                                 && !spaceItem.isActive
                        width: AppTheme.scaled(10) + 2
                        height: width
                        radius: height / 2
                        color: AppTheme.rail
                        anchors.bottom: parent.bottom
                        anchors.right: parent.right
                        anchors.bottomMargin: AppTheme.compactDensity ? -2 : -3
                        anchors.rightMargin: -3
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 2
                            radius: height / 2
                            color: AppTheme.railUnreadDot
                            border.color: AppTheme.railUnreadDotRing
                            border.width: AppTheme.badgeDotRingWidth
                        }
                    }

                    Rectangle {
                        id: spaceDropRing
                        objectName: "railSpaceDropRing"
                        readonly property int stroke: AppTheme.scaled(3)
                        anchors.fill: parent
                        anchors.margins: -stroke
                        radius: spaceTile.radius + stroke
                        color: "transparent"
                        border.color: AppTheme.accent
                        border.width: stroke
                        visible: spaceItem.dropTarget
                    }
                    Rectangle {
                        objectName: "railSpaceFocusRing"
                        anchors.fill: parent
                        anchors.margins: -root.tileRingOutset
                        radius: spaceTile.radius + root.tileRingOutset
                        color: "transparent"
                        border.color: AppTheme.focusRing
                        border.width: 2
                        visible: spaceItem.activeFocus
                    }
                }

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
                            if (root.beginTileDrag(spaceItem.entryId,
                                                   centroid.scenePosition.y)) {
                                var w = root.Window.window
                                root.focusBeforeDrag =
                                    w ? w.activeFocusItem : null
                                root.forceActiveFocus()
                            }
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
                HoverHandler { id: spaceHover }
                }

                TapHandler {
                    acceptedButtons: Qt.RightButton
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    enabled: !root.dragging
                    onTapped: (eventPoint) => spaceItem.openContextMenu(
                                  eventPoint.position.x,
                                  eventPoint.position.y)
                }

                Rectangle {
                    anchors.fill: spaceTile
                    anchors.margins: -root.tileRingOutset
                    radius: spaceTile.radius + root.tileRingOutset
                    color: AppTheme.hover
                    visible: spaceHover.hovered && !spaceItem.isActive
                             && !root.dragging
                    z: -2
                }

                TapHandler {
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
                        spaceItem.activateTile()
                    }
                }

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
                        enabled: app.settings.railSort === 0
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
                            readonly property string roomName:
                                modelData.name || modelData.roomId
                            readonly property string roomLabel:
                                (modelData.highlightCount || 0) > 0
                                ? qsTr("%1, %n mention(s)", "",
                                       modelData.highlightCount).arg(roomName)
                                : ((modelData.hasUnread === true
                                    || (modelData.unreadCount || 0) > 0)
                                   ? qsTr("%1, unread", "room row").arg(roomName)
                                   : roomName)
                            width: expansionCol.width
                            height: root.railRoomRowBand
                            Rectangle {
                                anchors.fill: roomTile
                                anchors.margins: -root.tileRingOutset
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
                                             || (expansionRoomRow.modelData
                                                     .unreadCount || 0) > 0
                                             || (expansionRoomRow.modelData
                                                     .highlightCount || 0) > 0
                                    width: AppTheme.scaled(10) + 2
                                    height: width
                                    radius: height / 2
                                    color: AppTheme.rail
                                    anchors.bottom: parent.bottom
                                    anchors.right: parent.right
                                    anchors.bottomMargin: -3
                                    anchors.rightMargin: -3
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: 2
                                        radius: height / 2
                                        color: (expansionRoomRow.modelData
                                                    .highlightCount || 0) > 0
                                               ? AppTheme.mentionDot
                                               : AppTheme.railUnreadDot
                                        border.color: (expansionRoomRow.modelData
                                                           .highlightCount || 0) > 0
                                                      ? AppTheme.mentionDotRing
                                                      : AppTheme.railUnreadDotRing
                                        border.width: AppTheme.badgeDotRingWidth
                                    }
                                }
                            }
                            HoverHandler { id: roomHover }
                            TapHandler {
                                onTapped: {
                                    if (app.spaces)
                                        app.spaces.activeSpaceId =
                                            spaceItem.spaceId
                                    app.openRoom(
                                        expansionRoomRow.modelData.roomId)
                                    app.requestComposerFocus(
                                        expansionRoomRow.modelData.roomId)
                                }
                            }
                            Item {
                                x: expansionRoomRow.width
                                   - expansionRoomRow.x
                                y: expansionRoomRow.height
                                width: Math.max(root.railTipFloor,
                                                root.railTipWidth)
                                height: 1
                                ToolTip.visible: roomHover.hovered
                                ToolTip.text: expansionRoomRow.roomLabel
                                ToolTip.delay: 300
                            }
                            Accessible.role: Accessible.Button
                            Accessible.name: expansionRoomRow.roomLabel
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

            footer: Item {
                width: list.width
                height: root.railAddVisible && !root.railAddPinned
                        ? root.railAddCellHeight : 0
                visible: height > 0
                RailAddSpaceCell { buttonName: "railAddSpaceButton" }
            }
        }

        Item {
            id: railAddPinnedSlot
            Layout.fillWidth: true
            Layout.preferredHeight: root.railAddVisible && root.railAddPinned
                                    ? root.railAddCellHeight : 0
            visible: Layout.preferredHeight > 0
            onHeightChanged: root.queuePinAdd()
            RailAddSpaceCell { buttonName: "railAddSpaceButtonPinned" }
        }

        Rectangle {
            Layout.alignment: Qt.AlignLeft
            Layout.leftMargin: root.railDividerX
            implicitWidth: root.railDividerWidth
            implicitHeight: 2
            radius: 1
            color: AppTheme.separator
            visible: app.loggedIn
        }

        Item { implicitHeight: root.railTileGap; visible: app.loggedIn }

        IconButton {
            id: railSettingsButton
            objectName: "railSettingsButton"
            Layout.alignment: Qt.AlignLeft
            Layout.leftMargin: root.tileColumnX
            implicitWidth: root.railTileSize
            implicitHeight: root.railTileSize
            radius: root.railTileRadius
            restingColor: AppTheme.cardElevated
            iconName: "settings"
            iconSize: root.railChipIconSize
            active: app.currentScreen === 2
            visible: app.loggedIn
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

            Item {
                x: root.width - railSettingsButton.x
                y: railSettingsButton.height
                width: Math.max(root.railTipFloor, root.railTipWidth)
                height: 1
                ToolTip.visible: railSettingsButton.hovered
                ToolTip.text: railSettingsButton._attentionText
                ToolTip.delay: 500
            }

            Rectangle {
                objectName: "railSettingsAlertBadge"
                visible: app.sessionVerificationWarning || parent._updateBadge
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins:
                    Math.round((root.railTileSize - root.railChipIconSize) / 2)
                    - AppTheme.scaled(3)
                width: AppTheme.scaled(10)
                height: AppTheme.scaled(10)
                radius: height / 2
                color: app.sessionVerificationWarning ? AppTheme.dangerFill
                                                      : AppTheme.warningFill
                border.color: AppTheme.rail
                border.width: 2
            }
        }

        Item { implicitHeight: root.railTileGap; visible: app.loggedIn }

        Item {
            id: railAccount
            objectName: "railAccountTile"
            Layout.alignment: Qt.AlignLeft
            Layout.leftMargin: root.tileColumnX
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

            activeFocusOnTab: true
            Keys.onPressed: (event) => {
                var k = event.key
                if (k === Qt.Key_Return || k === Qt.Key_Enter
                        || k === Qt.Key_Space || k === Qt.Key_Menu) {
                    railAccountMenu.open()
                    event.accepted = true
                }
            }
            Rectangle {
                objectName: "railAccountFocusRing"
                anchors.fill: parent
                anchors.margins: -3
                radius: AppTheme.radiusPill
                color: "transparent"
                border.color: AppTheme.focusRing
                border.width: 2
                visible: railAccount.activeFocus
            }

            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Account menu for %1")
                             .arg(app.accounts ? app.accounts.activeUserId : "")

            Avatar {
                id: railAvatar
                anchors.fill: parent
                size: root.railTileSize
                circle: true
                name: railAccount.activeAccount.displayName
                      || railAccount.localpart
                mxc: railAccount.activeAccount.avatarUrl || ""
                colorKey: app.accounts ? app.accounts.activeUserId : ""
            }
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

            readonly property int otherAccountsWithActivity:
                app.accountActivity
                ? app.accountActivity.otherAccountsWithActivity : 0
            Rectangle {
                id: railOtherAccountsDot
                objectName: "railOtherAccountsDot"
                visible: railAccount.otherAccountsWithActivity > 0
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.rightMargin: root.otherAccountsDotMargin
                anchors.topMargin: root.otherAccountsDotMargin
                width: root.otherAccountsDotSize
                height: root.otherAccountsDotSize
                radius: height / 2
                color: AppTheme.unreadBadge
                border.color: AppTheme.rail
                border.width: root.selfDotRing
                Accessible.role: Accessible.Indicator
                Accessible.ignored: !visible
                Accessible.name: qsTr("Accounts — new messages on %n other account(s)",
                                      "", railAccount.otherAccountsWithActivity)
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
            AccountWheelSwitch {
                objectName: "railAccountWheel"
                accounts: app.accounts ? app.accounts.accounts : []
                activeUserId: app.accounts ? (app.accounts.activeUserId || "") : ""
                busy: app.accountSwitching
                      || !!(app.auth && app.auth.logoutPending)
                onSwitchRequested: (userId) => app.switchToAccount(userId)
            }
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
                margins: AppTheme.spacing12
                y: parent.height - implicitHeight
            }
            Connections {
                target: app.demo
                enabled: app.screenshotDemoActive
                function onAccountSwitcherRequested() { railAccountMenu.open() }
            }
        }
    }

    AppMenu {
        id: railMenu
        objectName: "railContextMenu"
        property string entryId: ""
        property string spaceId: ""
        property string inFolder: ""
        property string folderName: ""
        property bool isFolder: false
        property bool collapsed: false
        property bool hierarchyChild: false
        readonly property bool isRealSpace:
            !isFolder && spaceId.charAt(0) === "!"
        property bool spaceMuted: false
        property bool spaceUnread: false
        contextLabel: railMenu.isFolder
                      ? (railMenu.folderName.length > 0 ? railMenu.folderName
                                                        : qsTr("Folder"))
                      : (app.spaces ? app.spaces.spaceName(railMenu.spaceId)
                                    : "")

        AppMenuItem {
            iconName: railMenu.collapsed ? "expand_more" : "expand_less"
            text: railMenu.collapsed ? qsTr("Expand folder")
                                     : qsTr("Collapse folder")
            visible: railMenu.isFolder
            // A hidden row must not take the keyboard: an enabled one absorbs Down
            // presses and Return would trigger it unseen.
            enabled: visible
            onTriggered: app.railLayout.setFolderCollapsed(railMenu.entryId,
                                                           !railMenu.collapsed)
        }
        AppMenuItem {
            iconName: "edit_square"
            text: qsTr("Rename folder…")
            visible: railMenu.isFolder
            enabled: visible
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
            enabled: visible
            onTriggered: app.railLayout.deleteFolder(railMenu.entryId)
        }

        AppMenuSeparator { visible: railMenu.isFolder }

        AppMenuItem {
            objectName: "railMarkAllRoomsRead"
            iconName: "done_all"
            text: qsTr("Mark all rooms read")
            visible: !railMenu.isFolder && railMenu.spaceId === ""
            enabled: visible && (railMenu.spaceUnread)
            onTriggered: app.roomList.markAllRoomsRead()
        }
        AppMenuItem {
            objectName: "railMarkSpaceRead"
            iconName: "done_all"
            text: qsTr("Mark as read")
            visible: railMenu.isRealSpace
            enabled: visible && (railMenu.spaceUnread)
            onTriggered: app.markSpaceRead(railMenu.spaceId)
        }
        AppMenuItem {
            objectName: "railMuteSpace"
            iconName: railMenu.spaceMuted ? "notifications" : "notifications_off"
            text: railMenu.spaceMuted ? qsTr("Unmute space")
                                      : qsTr("Mute space")
            visible: railMenu.isRealSpace
            enabled: visible
            onTriggered: app.setSpaceMuted(railMenu.spaceId,
                                           !railMenu.spaceMuted)
        }
        AppMenuItem {
            objectName: "railSpaceInvite"
            iconName: "person_add"
            text: qsTr("Invite")
            visible: railMenu.isRealSpace
            enabled: visible
            onTriggered: spaceInviteDialog.openFor(railMenu.spaceId)
        }
        AppMenuItem {
            objectName: "railSpaceCopyLink"
            iconName: "content_copy"
            text: qsTr("Copy link")
            visible: railMenu.isRealSpace
            enabled: visible
            onTriggered: root.copySpaceLink(railMenu.spaceId)
        }
        AppMenuItem {
            objectName: "railSpaceShareLink"
            // Every glyph must exist in the bundled Material Symbols subset, or
            // it renders as tofu. IconChromeTest pins the set.
            iconName: "link"
            text: qsTr("Share link…")
            visible: railMenu.isRealSpace
            enabled: visible
            onTriggered: shareLinkDialog.openFor(
                             railMenu.spaceId,
                             app.spaces.spaceName(railMenu.spaceId))
        }
        AppMenuItem {
            objectName: "railSpaceSettings"
            iconName: "settings"
            text: qsTr("Space settings")
            visible: railMenu.isRealSpace
            enabled: visible
            onTriggered: {
                spaceSettingsLoader.active = true
                spaceSettingsLoader.item.openFor(railMenu.spaceId)
            }
        }

        AppMenuSeparator { visible: railMenu.isRealSpace }

        AppMenuItem {
            objectName: "railNewFolder"
            iconName: "add"
            text: qsTr("New folder…")
            visible: !railMenu.hierarchyChild
            enabled: visible
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
            enabled: visible
            onTriggered: app.railLayout.setSpaceFolder(railMenu.spaceId, "")
        }
        Repeater {
            model: (railMenu.isFolder || railMenu.hierarchyChild
                    || !railMenu.isRealSpace)
                   ? [] : app.railLayout.folders
            delegate: AppMenuItem {
                required property var modelData
                objectName: "railMoveToItem"
                iconName: "workspaces"
                text: qsTr("Move to “%1”").arg(modelData.name && modelData.name.length > 0
                                         ? modelData.name : qsTr("Folder"))
                visible: modelData.id !== railMenu.inFolder
                enabled: visible
                onTriggered: {
                    // Close first: the move rebuilds this Repeater, destroying
                    // the item that was triggered before the menu dismissed.
                    railMenu.close()
                    app.railLayout.setSpaceFolder(railMenu.spaceId,
                                                  modelData.id)
                }
            }
        }
        AppMenuSeparator {}
        MenuSectionLabel { text: qsTr("Order Spaces by") }
        AppMenuItem {
            objectName: "railSortManual"
            radio: true
            radioSelected: app.settings.railSort === 0
            text: qsTr("Manual")
            onTriggered: app.settings.railSort = 0
        }
        AppMenuItem {
            objectName: "railSortName"
            radio: true
            radioSelected: app.settings.railSort === 1
            text: qsTr("A–Z")
            onTriggered: app.settings.railSort = 1
        }
        AppMenuItem {
            objectName: "railSortActivity"
            radio: true
            radioSelected: app.settings.railSort === 2
            text: qsTr("Recent activity")
            onTriggered: app.settings.railSort = 2
        }
    }

    TextEdit {
        id: railLinkClipboard
        visible: false
        width: 0
        height: 0
    }

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
        linkCopiedTip.show(qsTr("Link copied"))
    }

    ToolTip {
        id: linkCopiedTip
        objectName: "railLinkCopiedTip"
        parent: Overlay.overlay
        x: root.width + AppTheme.spacing8
        y: AppTheme.spacing8
        timeout: 1800
    }

    InvitePeopleDialog {
        id: spaceInviteDialog
        parent: Overlay.overlay
    }

    Loader {
        id: spaceSettingsLoader
        active: false
        sourceComponent: SpaceSettingsDialog {
            onInviteRequested: (spaceId) => spaceInviteDialog.openFor(spaceId)
        }
    }

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
