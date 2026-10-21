import QtQuick
import QtQuick.Layouts
import MatrixClient

// A miniature, entirely fake Lightning window: the live preview in the custom
// theme editor. Every name, message and count is a literal here (no `app.`,
// models, controllers or media), so it renders while signed out and can never
// leak a real conversation into a screenshot. Colours come from `pal`
// (AppTheme.paletteForTheme(id)), not the live AppTheme, so it can show a theme
// that is not applied; non-colour tokens still come from AppTheme. Every region
// is clickable and reports the role it paints, so a user can point at what to
// recolour.
//
// One element, one role (2026-10-13). Each Region names the per-element role
// the real view of that element reads (CustomThemeStore's element roles), never
// a shared group such as "Panels and cards": a click opens that element alone
// and outlines nothing else, and editing it changes nothing else. Two Regions
// share a role only when they are the same element drawn twice (two rows of
// one list); those say `repeated: true`, and the suite holds every other role
// to a single Region (`everyElementOfThePreviewIsItsOwnClickTarget`).
//
// Every editable role must be painted here, by a Region of its own or (for a
// group) through its elements (`everyEditableRoleHasARegionInThePreview`).
// Avatars and sender names wear each person's identity colour, derived from
// the theme as the app derives it; they are content, like a picture, not a
// theme role, so they are drawn but are not targets.
Item {
    id: root

    // role -> colour, from AppTheme.paletteForTheme(id). Not named `palette`,
    // which would shadow QQuickItem's own property.
    required property var pal
    // Which navigation layout the room-list column shows, bound to the user's
    // own choice so the preview shows where colours actually land.
    property bool channels: false
    // The role open in the picker, outlined here.
    property string highlightRole: ""
    // The role the pointer is over in the editor's list: outlined here too,
    // so a row can be traced to where it paints without opening it.
    property string hoverRole: ""
    // { role: true } for roles that currently follow the outlined one (an
    // unset child takes its parent's colour), outlined more lightly: editing
    // the parent recolours them too.
    property var linkedRoles: ({})
    // { role: true } for the roles the readability audit flags: each region
    // painting one carries a small amber dot, so a problem is on the thing
    // rather than in a report. Decoration only; it takes no input.
    property var problemRoles: ({})
    // The ring's stroke in this item's own pixels. The editor scales the
    // preview, so it passes 2 / scale to keep the ring 2px on screen.
    property real outlineWidth: 2
    // Surface gradients to paint: role -> { type, angle, stops }, the theme's
    // own (CustomThemeStore.gradients), an element's inherited one, and, while
    // Depth is on, Depth's for the grounds the theme leaves flat. A role
    // without one stays flat.
    property var gradients: ({})

    // ---- keyboard (2026-10-14) ----
    //
    // Tab or the arrow keys walk the regions in reading order (top to bottom in
    // bands, then left to right, a container before what it holds); the focus
    // ring is the region's own, drawn by the Region. Enter or Space opens the
    // element in the inspector (regionActivated), Escape hands focus back to
    // the editor's list (keyboardEscape).
    //
    // Real focus moves from region to region (each is a focus target, but not
    // a Tab stop, so Tab does not visit a hundred of them): that is what lets
    // an assistive technology read the region's name when it arrives. The
    // names come from the editor, which knows the roles' labels.
    // role -> label (a function the editor supplies), and a one-line state
    // ("Own colour", "Linked to Panels and cards").
    property var roleLabel: null
    property var roleState: null
    // The region the keyboard is on, remembered when focus leaves.
    property Item kbRegion: null
    // The role it is on while it has focus, else "".
    readonly property string keyboardRole:
        root.kbRegion && root.kbRegion.activeFocus ? root.kbRegion.role : ""
    // Set when focus leaves backwards, so the entry stop does not pull it back.
    property bool kbLeaving: false
    signal keyboardEscape()

    function labelOf(role) {
        var v = root.roleLabel ? root.roleLabel(role) : ""
        return v && v.length > 0 ? v : role
    }
    function stateOf(role) {
        return root.roleState ? root.roleState(role) : ""
    }

    // Every region on screen, in reading order.
    function orderedRegions() {
        var found = []
        var walk = function (node) {
            var kids = node.children
            for (var i = 0; i < kids.length; ++i) {
                var k = kids[i]
                if (!k.visible)
                    continue
                if (k.isThemeRegion === true && k.width > 0 && k.height > 0) {
                    var p = k.mapToItem(root, 0, 0)
                    found.push({ item: k, band: Math.round(p.y / 10),
                                 x: p.x, area: k.width * k.height })
                }
                walk(k)
            }
        }
        walk(root)
        found.sort(function (a, b) {
            if (a.band !== b.band)
                return a.band - b.band
            if (Math.abs(a.x - b.x) > 0.5)
                return a.x - b.x
            return b.area - a.area
        })
        var out = []
        for (var j = 0; j < found.length; ++j)
            out.push(found[j].item)
        return out
    }
    function focusRegion(region) {
        root.kbRegion = region
        region.forceActiveFocus()
    }
    // Moves one region on. Tab leaves at the ends (false: not handled), the
    // arrow keys stop there.
    function stepFocus(dir, tabbing) {
        var list = root.orderedRegions()
        if (list.length === 0)
            return false
        var i = list.indexOf(root.kbRegion)
        var n = i < 0 ? (dir > 0 ? 0 : list.length - 1) : i + dir
        if (n < 0 || n >= list.length) {
            if (tabbing)
                return false
            n = Math.max(0, Math.min(list.length - 1, n))
        }
        root.focusRegion(list[n])
        return true
    }
    function jumpFocus(last) {
        var list = root.orderedRegions()
        if (list.length > 0)
            root.focusRegion(list[last ? list.length - 1 : 0])
    }
    // Opens the focused element, as a click on it would.
    function activateFocused() {
        if (root.kbRegion && root.kbRegion.activeFocus)
            root.regionActivated(root.kbRegion.role, [root.kbRegion.role])
    }
    // Tab arrived at the entry stop: hand focus to the region the keyboard was
    // on, else the one open in the picker, else the first.
    function enterFromTab() {
        if (root.kbLeaving) {
            root.kbLeaving = false
            var prev = kbEntry.nextItemInFocusChain(false)
            if (prev && prev !== kbEntry)
                prev.forceActiveFocus()
            return
        }
        var list = root.orderedRegions()
        if (list.length === 0)
            return
        var target = list.indexOf(root.kbRegion) >= 0 ? root.kbRegion : null
        for (var i = 0; !target && i < list.length; ++i) {
            if (root.highlightRole.length > 0
                    && list[i].role === root.highlightRole)
                target = list[i]
        }
        root.focusRegion(target ? target : list[0])
    }

    // The one Tab stop of the whole sample window.
    Item {
        id: kbEntry
        objectName: "themePreviewKeyboardEntry"
        width: 1
        height: 1
        activeFocusOnTab: true
        Accessible.role: Accessible.Pane
        Accessible.name: qsTr("Sample window. Tab or the arrow keys move between its parts, Enter edits one, Escape goes back to the list.")
        onActiveFocusChanged: if (activeFocus) root.enterFromTab()
    }

    Keys.onPressed: (e) => {
        switch (e.key) {
        case Qt.Key_Right:
        case Qt.Key_Down:
            root.stepFocus(1, false)
            e.accepted = true
            break
        case Qt.Key_Left:
        case Qt.Key_Up:
            root.stepFocus(-1, false)
            e.accepted = true
            break
        case Qt.Key_Home:
            root.jumpFocus(false)
            e.accepted = true
            break
        case Qt.Key_End:
            root.jumpFocus(true)
            e.accepted = true
            break
        case Qt.Key_Return:
        case Qt.Key_Enter:
        case Qt.Key_Space:
            root.activateFocused()
            e.accepted = true
            break
        }
    }
    Keys.onTabPressed: (e) => { e.accepted = root.stepFocus(1, true) }
    Keys.onBacktabPressed: (e) => {
        e.accepted = root.stepFocus(-1, true)
        if (!e.accepted)
            root.kbLeaving = true
    }
    Keys.onEscapePressed: (e) => {
        root.keyboardEscape()
        e.accepted = true
    }
    // Escape here is the preview's: without this the editor's Escape shortcut
    // wins before the key reaches the item.
    Keys.onShortcutOverride: (e) => {
        if (e.key === Qt.Key_Escape)
            e.accepted = true
    }

    function gradientSpec(role) {
        var g = root.gradients ? root.gradients[role] : undefined
        return g && g.stops && g.stops.length >= 2 ? g : null
    }

    // A region was clicked; the editor opens that role. `stack` lists every
    // role painted under the pointer, the clicked one first and then each
    // surface behind it, so text and the fill under it are both reachable.
    signal regionActivated(string role, var stack)

    // The roles under a clicked region: the region itself, then any region
    // filling one of its ancestors (a region fills its parent element, so
    // an ancestor's region covers the point too).
    function regionStack(region, x, y) {
        var out = [region.role]
        var item = region.parent
        while (item && item !== root) {
            var kids = item.children
            for (var i = 0; i < kids.length; ++i) {
                var k = kids[i]
                if (k === region || k.isThemeRegion !== true || !k.visible
                        || out.indexOf(k.role) >= 0)
                    continue
                // A padded or offset region (a halo) may not cover the point.
                if (!k.contains(k.mapFromItem(region, x, y)))
                    continue
                out.push(k.role)
            }
            item = item.parent
        }
        return out
    }

    // Natural size. The editor scales it uniformly to fit (up to 1.6x; text
    // here is distance-field rendered and scales cleanly).
    implicitWidth: 880
    implicitHeight: 560
    clip: true

    function c(role, fallback) {
        var p = root.pal
        if (p && p[role] !== undefined)
            return p[role]
        return p && p[fallback] !== undefined ? p[fallback] : "transparent"
    }

    // A palette entry at a given alpha (status chips are a tint of their ink).
    // Goes through Qt.color because a custom palette carries "#RRGGBB" strings.
    function tone(role, fallback, alpha) {
        return Qt.alpha(Qt.color(String(root.c(role, fallback))), alpha)
    }

    // The identity colours, derived from this palette by AppTheme's own rule
    // (identityAnchor): avatar discs and sender names follow the theme without
    // being a role of it.
    readonly property color identityAnchor: {
        var bg = Qt.color(String(root.c("background", "background")))
        var ink = Qt.color(String(root.c("accent", "accent")))
        if (bg.hslSaturation < 0.20)
            return ink
        var gap = Math.abs(bg.hslHue - ink.hslHue)
        if (gap > 0.5)
            gap = 1.0 - gap
        return gap > (60.0 / 360.0) ? bg : ink
    }
    function disc(key) {
        return IdentityPalette.disc(AppTheme.identityIndex(key), root.identityAnchor)
    }
    function nameInk(key) {
        return IdentityPalette.nameInk(
            AppTheme.identityIndex(key), root.identityAnchor,
            [Qt.color(String(root.c("background", "background"))),
             Qt.color(String(root.c("surface", "surface"))),
             Qt.color(String(root.c("cardElevated", "surface"))),
             Qt.color(String(root.c("otherBubble", "surface")))])
    }

    // Fixture rows. `state` shows selected and hovered rows at once.
    readonly property var fakeRooms: [
        { name: qsTr("Design"),    preview: qsTr("Shipped the new palette"), badge: 0, mention: 0, state: "normal" },
        { name: qsTr("Lightning"), preview: qsTr("Storm looks good now"),    badge: 0, mention: 0, state: "selected" },
        { name: qsTr("Alex"),      preview: qsTr("See you at six"),          badge: 3, mention: 0, state: "hovered" },
        { name: qsTr("Releases"),  preview: qsTr("New release is out"),      badge: 0, mention: 1, state: "normal" },
        { name: qsTr("Support"),   preview: qsTr("Open, pointer on it"),     badge: 0, mention: 0, state: "selectedHover" }
    ]

    // The Channels shape: navigation rows, a group, and a Space folder with
    // rooms. Literals only.
    readonly property var fakeChannelRows: [
        { kind: "nav",    name: qsTr("Lobby"),          state: "selected" },
        { kind: "nav",    name: qsTr("Message Search"), state: "normal" },
        { kind: "folder", name: qsTr("Rooms"),          state: "normal" },
        { kind: "folder", name: qsTr("Creative Studio"), state: "normal" },
        { kind: "room",   name: qsTr("Design"),         state: "normal" },
        { kind: "room",   name: qsTr("Lightning"),      state: "hovered" },
        { kind: "room",   name: qsTr("Releases"),       state: "normal" }
    ]

    readonly property var fakeMembers: [
        qsTr("Sam"), qsTr("Alex"), qsTr("Robin"), qsTr("Kim")
    ]

    // One click target and edit outline per region. A MouseArea, not a
    // TapHandler: pointer handlers are non-exclusive across subtrees, so a row
    // click would also hit the container behind it. Containers are declared
    // first, beneath their content, so leaf regions win.
    component Region: MouseArea {
        id: region
        required property string role
        // Marks a region for regionStack(); other children of an element
        // are skipped.
        readonly property bool isThemeRegion: true
        // The same element drawn more than once (two rows of one list): the
        // only case in which two Regions may name one role.
        property bool repeated: false
        // Grows the hit area and the outline past a thin element (a hairline,
        // a scrollbar) so it can be pointed at.
        property real pad: 0
        // How loudly this region is outlined: 3 open in the picker, 2 pointed
        // at in the list, 1 recoloured along with either, 0.5 under the
        // pointer here, 0 not at all.
        readonly property real emphasis:
            region.activeFocus ? 3
            : root.highlightRole === region.role ? 3
            : root.hoverRole === region.role ? 2
            : root.linkedRoles[region.role] === true ? 1
            : region.containsMouse ? 0.5 : 0
        anchors.fill: parent
        anchors.margins: -region.pad
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        // Reachable by the keyboard and named for assistive technology: what
        // it is, and whether its colour is its own or linked.
        Accessible.role: Accessible.Button
        Accessible.name: root.labelOf(region.role)
        Accessible.description: root.stateOf(region.role)
        Accessible.focusable: true
        Accessible.onPressAction: root.regionActivated(region.role, [region.role])
        onClicked: (mouse) => {
            // The keyboard carries on from where the pointer last was.
            root.kbRegion = region
            root.regionActivated(
                region.role, root.regionStack(region, mouse.x, mouse.y))
        }

        // The readability mark: inside the region's top-right corner, 8px on
        // screen at any scale (outlineWidth is 2px on screen).
        Rectangle {
            objectName: "themeProblemDot"
            visible: root.problemRoles[region.role] === true
            enabled: false
            readonly property real d: root.outlineWidth * 4
            x: region.width - d - root.outlineWidth
            y: root.outlineWidth
            width: d
            height: d
            radius: d / 2
            color: "#F2A93B"
            border.width: root.outlineWidth * 0.5
            border.color: "#B0000000"
        }

        // Built only while shown: there are about a hundred regions.
        Loader {
            anchors.fill: parent
            // A text element's box hugs its glyphs, so the ring would stroke
            // through the letters; it stands off by one ring width. (Text has
            // no `radius`; every Rectangle has one, 0 or not.)
            anchors.margins: region.parent && region.parent.radius === undefined
                             ? -root.outlineWidth * 1.5 : 0
            active: region.emphasis > 0
            sourceComponent: Component {
                // Three strokes so the ring reads on any colour the theme
                // can paint, including one close to the accent: a dark halo
                // outside, the accent, and a light keyline inside. Rounded
                // to the element's own corners.
                Item {
                    id: ring
                    readonly property real w: root.outlineWidth
                    readonly property real r:
                        region.parent && region.parent.radius !== undefined
                        ? region.parent.radius + region.pad
                        : AppTheme.radiusSm
                    readonly property bool strong: region.emphasis >= 2
                    // A linked region (recoloured along with the traced
                    // role) is visibly quieter than the role itself, which
                    // at 0.9 read as the same thing.
                    opacity: region.emphasis >= 2 ? 1.0
                             : region.emphasis >= 1 ? 0.6 : 0.5
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -ring.w * 0.5
                        radius: ring.r > 0 ? ring.r + ring.w * 0.5 : 0
                        color: "transparent"
                        border.width: ring.w * 0.5
                        border.color: "#B0000000"
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: ring.r
                        color: "transparent"
                        border.width: ring.strong ? ring.w : ring.w * 0.5
                        border.color: AppTheme.editorAccent
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: ring.strong ? ring.w : ring.w * 0.5
                        visible: region.emphasis >= 1
                        radius: Math.max(0, ring.r - anchors.margins)
                        color: "transparent"
                        border.width: ring.w * 0.5
                        border.color: "#E6FFFFFF"
                    }
                    // The keyboard's own mark: a white band standing off the
                    // ring, so keyboard focus is not mistaken for the element
                    // that is merely open in the picker.
                    Rectangle {
                        objectName: "themeKeyboardRing"
                        visible: region.activeFocus
                        anchors.fill: parent
                        anchors.margins: -ring.w * 2
                        radius: ring.r + ring.w * 2
                        color: "transparent"
                        border.width: ring.w * 0.75
                        border.color: "#FFFFFFFF"
                    }
                }
            }
        }
    }

    // A role's gradient over its element's flat fill, beneath the element's
    // Region and content so the outline and the click target are unchanged.
    // Draws nothing for a flat role.
    component GradientLayer: ThemedSurface {
        anchors.fill: parent
        flatFill: false
        specOverride: root.gradientSpec(role)
    }

    // A labelled strip for a state the main window shows only in one layout or
    // one dialog. Its label is drawn in the ink the real element uses on that
    // state, and is a target of its own.
    component Sample: Rectangle {
        id: smp
        required property string role
        property string label
        required property string inkRole
        property color fill: "transparent"
        property color ink: root.c("textPrimary", "textPrimary")
        Layout.fillWidth: true
        implicitHeight: 22
        radius: AppTheme.radiusSm
        color: smp.fill
        Region { role: smp.role }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: AppTheme.spacing8
            width: Math.min(implicitWidth, parent.width - AppTheme.spacing8 * 2)
            text: smp.label
            textFormat: Text.PlainText
            color: smp.ink
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
            elide: Text.ElideRight
            Region { role: smp.inkRole; pad: 1 }
        }
    }

    // A 1px line of its own, so a border is a click target separate from the
    // box it frames (a Region over the box would hand the line and the fill to
    // one role). Lies along the bottom edge of its parent.
    component EdgeRegion: Item {
        id: edge
        required property string role
        property bool repeated: false
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        Region { role: edge.role; pad: 3; repeated: edge.repeated }
    }

    // An avatar disc in a person's (or a room's) identity colour: content, not
    // a target.
    component IdentityDisc: Rectangle {
        required property string key
        property bool square: false
        implicitWidth: 22
        implicitHeight: 22
        radius: square ? AppTheme.radiusSm : width / 2
        color: root.disc(key)
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // Spaces rail
        Rectangle {
            Layout.preferredWidth: 60
            Layout.fillHeight: true
            color: root.c("rail", "sidebar")

            GradientLayer { role: "rail" }
            Region { role: "rail" }
            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: AppTheme.spacing12
                anchors.bottomMargin: AppTheme.spacing12
                spacing: AppTheme.spacing12

                // Home: a resting tile, with something unread in it.
                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    implicitWidth: 34
                    implicitHeight: 34
                    radius: AppTheme.radiusMd
                    color: root.c("railTile", "cardElevated")
                    Region { role: "railTile" }
                    Rectangle {
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.rightMargin: -2
                        anchors.bottomMargin: -2
                        width: 10
                        height: 10
                        radius: 5
                        color: root.c("railUnreadDot", "unreadBadge")
                        border.width: 2
                        border.color: root.c("railUnreadDotRing", "railUnreadDot")
                        Region { role: "railUnreadDot"; pad: 2 }
                    }
                }

                // The open Space: a soft wash inside an accent ring drawn
                // outside the tile. The ring is declared first, so the wash
                // wins inside it and the ring keeps the band around it.
                Item {
                    Layout.alignment: Qt.AlignHCenter
                    implicitWidth: 34
                    implicitHeight: 34
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -3
                        radius: AppTheme.radiusMd + 3
                        color: "transparent"
                        border.width: 2
                        border.color: root.c("railActiveRing", "accent")
                        Region { role: "railActiveRing" }
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: AppTheme.radiusMd
                        color: root.c("railTileActive", "accentSoft")
                        Region { role: "railTileActive" }
                    }
                }

                // A Space with a picture of its own.
                IdentityDisc {
                    Layout.alignment: Qt.AlignHCenter
                    key: "#design:example.org"
                    square: true
                    implicitWidth: 34
                    implicitHeight: 34
                    radius: AppTheme.radiusMd
                }

                Item { Layout.fillHeight: true }
            }
        }

        // Room list
        Rectangle {
            Layout.preferredWidth: 236
            Layout.fillHeight: true
            color: root.c("roomList", "sidebar")
            clip: true

            GradientLayer { role: "roomList" }
            Region { role: "roomList" }
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: AppTheme.spacing12
                spacing: AppTheme.spacing8

                Text {
                    text: qsTr("Lightning")
                    color: root.c("roomListTitle", "textPrimary")
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightStrong
                    elide: Text.ElideRight
                    Region { role: "roomListTitle" }
                }

                // Search field.
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 28
                    radius: AppTheme.radiusMd
                    color: root.c("searchField", "surface")
                    border.width: 1
                    border.color: root.c("searchFieldBorder", "border")
                    Region { role: "searchField" }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: AppTheme.spacing8
                        text: qsTr("Search")
                        color: root.c("placeholderInk", "textMuted")
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                        Region { role: "placeholderInk" }
                    }
                    EdgeRegion { role: "searchFieldBorder" }
                }

                Text {
                    visible: !root.channels
                    text: qsTr("Favourites")
                    color: root.c("roomListSection", "textMuted")
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                    Region { role: "roomListSection" }
                }

                // The Channels shape: a folder header, then indented rooms.
                Repeater {
                    model: root.channels ? root.fakeChannelRows : []
                    delegate: Rectangle {
                        id: fakeChannelRow
                        required property var modelData
                        readonly property bool isRoom: modelData.kind === "room"
                        readonly property bool isFolder: modelData.kind === "folder"
                        readonly property bool isSelected: modelData.state === "selected"
                        readonly property bool isHovered: modelData.state === "hovered"
                        Layout.fillWidth: true
                        Layout.leftMargin: isRoom ? AppTheme.spacing8 : 0
                        implicitHeight: 26
                        radius: AppTheme.radiusSm
                        color: isSelected ? root.c("channelSelected", "selected")
                             : isHovered ? root.c("channelHover", "hover")
                             : "transparent"
                        border.width: fakeChannelRow.isFolder ? 1 : 0
                        border.color: root.c("channelFolderBorder", "border")

                        // A resting row is the column itself: no target of
                        // its own, a click reaches the room list.
                        Region {
                            visible: fakeChannelRow.isSelected || fakeChannelRow.isHovered
                            enabled: visible
                            role: fakeChannelRow.isSelected ? "channelSelected" : "channelHover"
                        }
                        EdgeRegion {
                            visible: fakeChannelRow.isFolder
                            role: "channelFolderBorder"
                            repeated: true
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: AppTheme.spacing6
                            anchors.rightMargin: AppTheme.spacing8
                            spacing: AppTheme.spacing6

                            // The folder's arrow.
                            Rectangle {
                                visible: fakeChannelRow.isFolder
                                implicitWidth: 8
                                implicitHeight: 8
                                radius: 1
                                color: root.c("channelChevron", "textMuted")
                                Region { role: "channelChevron"; repeated: true; pad: 2 }
                            }
                            // A room's avatar.
                            IdentityDisc {
                                visible: fakeChannelRow.isRoom
                                key: fakeChannelRow.modelData.name
                                square: true
                                implicitWidth: 16
                                implicitHeight: 16
                            }
                            Text {
                                text: fakeChannelRow.modelData.name
                                textFormat: Text.PlainText
                                color: fakeChannelRow.isSelected
                                       ? root.c("channelSelectedText", "selectedText")
                                       : fakeChannelRow.isFolder
                                         ? root.c("channelCategoryText", "textMuted")
                                         : root.c("channelText", "textSecondary")
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMeta
                                font.weight: fakeChannelRow.isFolder ? AppTheme.weightStrong : AppTheme.weightBody
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                                Region {
                                    role: fakeChannelRow.isSelected ? "channelSelectedText"
                                          : fakeChannelRow.isFolder ? "channelCategoryText"
                                          : "channelText"
                                    repeated: true
                                }
                            }
                        }
                    }
                }

                Repeater {
                    model: root.channels ? [] : root.fakeRooms
                    delegate: Rectangle {
                        id: fakeRoomRow
                        required property var modelData
                        // The open room, with or without the pointer on it.
                        readonly property bool isSelected:
                            modelData.state === "selected"
                            || modelData.state === "selectedHover"
                        readonly property bool isUnread:
                            modelData.badge > 0 || modelData.mention > 0
                        Layout.fillWidth: true
                        implicitHeight: 42
                        radius: AppTheme.radiusMd
                        color: modelData.state === "selectedHover"
                               ? root.c("roomSelectedHover", "selectedHover")
                             : isSelected ? root.c("roomSelected", "selected")
                             : modelData.state === "hovered"
                               ? root.c("roomHover", "hover")
                               : "transparent"

                        // Beneath the content, so each ink names itself. A
                        // resting row is the column itself: no target.
                        Region {
                            visible: fakeRoomRow.modelData.state !== "normal"
                            enabled: visible
                            role: fakeRoomRow.modelData.state === "selectedHover"
                                  ? "roomSelectedHover"
                                  : fakeRoomRow.isSelected
                                    ? "roomSelected" : "roomHover"
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: AppTheme.spacing8
                            anchors.rightMargin: AppTheme.spacing8
                            spacing: AppTheme.spacing8

                            IdentityDisc {
                                key: fakeRoomRow.modelData.name
                                square: true
                                implicitWidth: 26
                                implicitHeight: 26
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Text {
                                    text: fakeRoomRow.modelData.name
                                    textFormat: Text.PlainText
                                    color: fakeRoomRow.isSelected
                                           ? root.c("roomSelectedName", "selectedText")
                                           : fakeRoomRow.isUnread
                                             ? root.c("roomNameUnread", "textPrimary")
                                             : root.c("roomName", "textSecondary")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    font.weight: AppTheme.weightStrong
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                    Region {
                                        role: fakeRoomRow.isSelected ? "roomSelectedName"
                                              : fakeRoomRow.isUnread ? "roomNameUnread"
                                              : "roomName"
                                        repeated: true
                                    }
                                }
                                Text {
                                    // Untrusted text: never markup.
                                    textFormat: Text.PlainText
                                    text: fakeRoomRow.modelData.preview
                                    color: fakeRoomRow.isSelected
                                           ? root.c("roomSelectedPreview", "selectedText")
                                           : root.c("roomPreview", "textMuted")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                    Region {
                                        role: fakeRoomRow.isSelected ? "roomSelectedPreview"
                                                                     : "roomPreview"
                                        repeated: true
                                    }
                                }
                            }

                            // Unread count.
                            Rectangle {
                                visible: fakeRoomRow.modelData.badge > 0
                                implicitWidth: 20
                                implicitHeight: 18
                                radius: AppTheme.radiusPill
                                color: root.c("unreadCountFill", "roomUnreadBadge")
                                Region { role: "roomUnreadBadge" }
                                Text {
                                    anchors.centerIn: parent
                                    text: fakeRoomRow.modelData.badge
                                    color: root.c("unreadCountInk", "unreadBadgeText")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMicro
                                    font.weight: AppTheme.weightStrong
                                    Region { role: "unreadBadgeText"; pad: 1 }
                                }
                            }

                            // Mention count: the pill that says someone named you.
                            Rectangle {
                                visible: fakeRoomRow.modelData.mention > 0
                                implicitWidth: 20
                                implicitHeight: 18
                                radius: AppTheme.radiusPill
                                color: root.c("roomMentionBadge", "mentionBadge")
                                Region { role: "roomMentionBadge" }
                                Text {
                                    anchors.centerIn: parent
                                    text: fakeRoomRow.modelData.mention
                                    color: root.c("mentionBadgeText", "mentionBadgeText")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMicro
                                    font.weight: AppTheme.weightStrong
                                    Region { role: "mentionBadgeText"; pad: 1 }
                                }
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true }
            }
        }

        // Timeline + composer
        Rectangle {
            id: timelineArea
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: root.c("timelineBackground", "background")
            clip: true

            GradientLayer { role: "timelineBackground" }
            Region { role: "timelineBackground" }
            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                // Room header.
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 44
                    color: root.c("roomHeader", "sidebar")
                    Region { role: "roomHeader" }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: AppTheme.spacing12
                        text: qsTr("Lightning")
                        color: root.c("roomHeaderTitle", "textPrimary")
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textBody
                        font.weight: AppTheme.weightStrong
                        Region { role: "roomHeaderTitle" }
                    }
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: root.c("headerDivider", "border")
                        // A one pixel line is not clickable; the region is
                        // widened around it.
                        Region { role: "headerDivider"; pad: 3 }
                    }
                }

                // A call going on in the room.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.leftMargin: AppTheme.spacing12
                    Layout.rightMargin: AppTheme.spacing12
                    Layout.topMargin: AppTheme.spacing8
                    implicitHeight: 26
                    radius: AppTheme.radiusMd
                    color: root.c("callBanner", "accentSoft")
                    border.width: 1
                    border.color: root.c("accentBorder", "borderStrong")
                    Region { role: "callBanner" }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: AppTheme.spacing8
                        text: qsTr("Call in progress")
                        color: root.c("callBannerText", "textPrimary")
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                        font.weight: AppTheme.weightStrong
                        Region { role: "callBannerText" }
                    }
                    EdgeRegion { role: "accentBorder" }
                }

                // Messages.
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing8

                    // Incoming.
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.maximumWidth: 320
                        implicitHeight: incomingCol.implicitHeight
                                        + AppTheme.spacing8 * 2
                        radius: AppTheme.radiusMd
                        color: root.c("otherBubble", "surface")
                        // A message that is selected (to forward) or was just
                        // jumped to: a wash painted behind its row.
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: -4
                            z: -1
                            radius: AppTheme.radiusMd + 4
                            color: root.c("messageHighlight", "selected")
                            opacity: 0.85
                            Region { role: "messageHighlight" }
                        }
                        Region { role: "otherBubble"; repeated: true }
                        ColumnLayout {
                            id: incomingCol
                            anchors.fill: parent
                            anchors.margins: AppTheme.spacing8
                            spacing: 2
                            RowLayout {
                                spacing: AppTheme.spacing6
                                // The sender in their identity colour.
                                Text {
                                    text: qsTr("Sam")
                                    color: root.nameInk("@sam:example.org")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    font.weight: AppTheme.weightStrong
                                }
                                Text {
                                    text: qsTr("12:41")
                                    color: root.c("timestampInk", "textMuted")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMicro
                                    Region { role: "timestampInk" }
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("The new ladder reads much better.")
                                color: root.c("otherBubbleText", "textPrimary")
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMeta
                                wrapMode: Text.WordWrap
                                Region { role: "otherBubbleText"; repeated: true }
                            }
                        }
                    }

                    // Outgoing.
                    Rectangle {
                        Layout.alignment: Qt.AlignRight
                        implicitWidth: Math.min(300, outgoingText.implicitWidth
                                                + AppTheme.spacing8 * 2)
                        implicitHeight: outgoingText.implicitHeight
                                        + AppTheme.spacing8 * 2
                        radius: AppTheme.radiusMd
                        color: root.c("ownBubble", "accent")
                        Region { role: "ownBubble" }
                        Text {
                            id: outgoingText
                            anchors.fill: parent
                            anchors.margins: AppTheme.spacing8
                            text: qsTr("Agreed — shipping it.")
                            color: root.c("ownBubbleText", "accentText")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            wrapMode: Text.WordWrap
                            Region { role: "ownBubbleText" }
                        }
                    }

                    // Incoming, carrying a link, a mention of you, code, a
                    // link preview and reactions.
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.maximumWidth: 360
                        implicitHeight: richCol.implicitHeight
                                        + AppTheme.spacing8 * 2
                        radius: AppTheme.radiusMd
                        color: root.c("otherBubble", "surface")
                        Region { role: "otherBubble"; repeated: true }
                        ColumnLayout {
                            id: richCol
                            anchors.fill: parent
                            anchors.margins: AppTheme.spacing8
                            spacing: AppTheme.spacing6

                            RowLayout {
                                spacing: AppTheme.spacing6
                                Text {
                                    text: qsTr("See")
                                    color: root.c("otherBubbleText", "textPrimary")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    Region { role: "otherBubbleText"; repeated: true }
                                }
                                Text {
                                    text: qsTr("the notes")
                                    color: root.c("link", "accent")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    font.underline: true
                                    Region { role: "link" }
                                }
                                // A mention of you: bold ink, no chip, as the
                                // timeline draws it.
                                Text {
                                    text: qsTr("@you")
                                    color: root.c("selfMentionInk", "accent")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    font.weight: AppTheme.weightBold
                                    Region { role: "selfMentionInk" }
                                }
                            }

                            // Code.
                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 26
                                radius: AppTheme.radiusSm
                                color: root.c("codeBlock", "cardElevated")
                                border.width: 1
                                border.color: root.c("codeBlockBorder", "border")
                                Region { role: "codeBlock" }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.leftMargin: AppTheme.spacing6
                                    text: qsTr("git push")
                                    color: root.c("codeText", "textPrimary")
                                    font.family: AppTheme.monoFont
                                    font.pixelSize: AppTheme.textMeta
                                    Region { role: "codeText" }
                                }
                                EdgeRegion { role: "codeBlockBorder" }
                            }

                            // A link preview.
                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 26
                                radius: AppTheme.radiusSm
                                color: root.c("embedSurface", "cardElevated")
                                border.width: 1
                                border.color: root.c("embedBorder", "border")
                                Region { role: "embedSurface" }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.leftMargin: AppTheme.spacing6
                                    text: qsTr("Release notes")
                                    color: root.c("embedText", "textPrimary")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    elide: Text.ElideRight
                                    Region { role: "embedText" }
                                }
                                EdgeRegion { role: "embedBorder" }
                            }

                            // Reactions: someone else's, then your own. Each
                            // is its edge with the fill inside it, so the edge
                            // is a target of its own.
                            RowLayout {
                                spacing: AppTheme.spacing6

                                Rectangle {
                                    implicitWidth: 44
                                    implicitHeight: 22
                                    radius: AppTheme.radiusPill
                                    color: root.c("reactionBorder", "border")
                                    Region { role: "reactionBorder"; pad: 2 }
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: 1
                                        radius: AppTheme.radiusPill
                                        color: root.c("reactionBackground", "cardElevated")
                                        Region { role: "reaction" }
                                        Text {
                                            anchors.centerIn: parent
                                            // Not an emoji: emoji literals are
                                            // banned in row delegates here.
                                            text: qsTr("+2")
                                            color: root.c("reactionInk", "textSecondary")
                                            font.family: AppTheme.uiFont
                                            font.pixelSize: AppTheme.textMeta
                                            Region { role: "reactionInk" }
                                        }
                                    }
                                }

                                Rectangle {
                                    implicitWidth: 44
                                    implicitHeight: 22
                                    radius: AppTheme.radiusPill
                                    color: root.c("reactionSelectedBorder", "accent")
                                    Region { role: "reactionSelectedBorder"; pad: 2 }
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: 2
                                        radius: AppTheme.radiusPill
                                        color: root.c("reactionSelectedBackground",
                                                      "accentSoft")
                                        Region { role: "reactionSelectedBackground" }
                                        Text {
                                            anchors.centerIn: parent
                                            text: qsTr("+1")
                                            color: root.c("reactionSelectedInk", "textPrimary")
                                            font.family: AppTheme.uiFont
                                            font.pixelSize: AppTheme.textMeta
                                            Region { role: "reactionSelectedInk" }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }
                }

                // Composer.
                RowLayout {
                    Layout.fillWidth: true
                    Layout.margins: AppTheme.spacing12
                    Layout.topMargin: 0
                    spacing: AppTheme.spacing8

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 36
                        radius: AppTheme.radiusMd
                        color: root.c("composerField", "surface")
                        border.width: 1
                        border.color: root.c("composerBorder", "border")
                        Region { role: "composerField" }
                        // Draft text with part of it selected: the message box
                        // selects in the system selection colours.
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: AppTheme.spacing12
                            Text {
                                text: qsTr("Typing ")
                                color: root.c("composerText", "textPrimary")
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMeta
                                Region { role: "composerText" }
                            }
                            Rectangle {
                                width: selectedWord.implicitWidth
                                height: selectedWord.implicitHeight + 2
                                color: root.c("paletteHighlight", "selected")
                                Region { role: "paletteHighlight"; pad: 2 }
                                Text {
                                    id: selectedWord
                                    anchors.centerIn: parent
                                    text: qsTr("this")
                                    color: root.c("paletteHighlightText", "selectedText")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    Region { role: "paletteHighlightText" }
                                }
                            }
                        }
                        EdgeRegion { role: "composerBorder" }
                    }

                    Rectangle {
                        implicitWidth: 64
                        implicitHeight: 36
                        radius: AppTheme.radiusMd
                        color: root.c("sendButton", "accent")

                        // The keyboard focus ring, drawn as it is on a focused
                        // control: outside the control, with a gap. Declared
                        // first so the button's own region wins inside it.
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: -4
                            radius: AppTheme.radiusMd + 4
                            color: "transparent"
                            border.width: 2
                            border.color: root.c("focusRing", "accent")
                            Region { role: "focusRing" }
                        }
                        Rectangle {
                            anchors.fill: parent
                            radius: AppTheme.radiusMd
                            color: parent.color
                            Region { role: "sendButton" }
                            Text {
                                anchors.centerIn: parent
                                text: qsTr("Send")
                                color: root.c("sendButtonInk", "accentText")
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMeta
                                font.weight: AppTheme.weightStrong
                                Region { role: "sendButtonInk" }
                            }
                        }
                    }
                }
            }

            // A scrollbar handle, as it sits at the timeline's edge.
            Rectangle {
                anchors.right: parent.right
                anchors.rightMargin: 3
                anchors.top: parent.top
                anchors.topMargin: 90
                width: 6
                height: 96
                radius: 3
                color: root.c("scrollbarHandle", "borderStrong")
                Region { role: "scrollbarHandle"; pad: 4 }
            }

            // A context menu, open over the timeline.
            Rectangle {
                anchors.right: parent.right
                anchors.rightMargin: AppTheme.spacing20
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 64
                width: 140
                height: menuColumn.implicitHeight + AppTheme.spacing6 * 2
                radius: AppTheme.radiusMd
                color: root.c("popoverSurface", "surface")
                border.width: 1
                border.color: root.c("menuBorder", "borderStrong")
                Region { role: "popoverSurface" }
                EdgeRegion { role: "menuBorder" }

                Column {
                    id: menuColumn
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacing6
                    spacing: 2

                    Rectangle {
                        width: parent.width
                        height: 24
                        radius: AppTheme.radiusSm
                        // The highlighted item.
                        color: root.c("menuHighlight", "hover")
                        Region { role: "menuHighlight" }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: AppTheme.spacing8
                            text: qsTr("Reply")
                            color: root.c("menuItemActiveText", "textPrimary")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            Region { role: "menuItemActiveText" }
                        }
                    }
                    Rectangle {
                        width: parent.width
                        height: 24
                        radius: AppTheme.radiusSm
                        color: "transparent"
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: AppTheme.spacing8
                            text: qsTr("Forward")
                            color: root.c("menuItemText", "textSecondary")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            Region { role: "menuItemText" }
                        }
                    }
                    Rectangle {
                        width: parent.width
                        height: 24
                        radius: AppTheme.radiusSm
                        // A destructive item under the pointer: a tint of the
                        // danger family.
                        color: root.tone("menuDangerTint", "dangerTint", 0.10)
                        Region { role: "menuDangerTint" }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: AppTheme.spacing8
                            text: qsTr("Delete")
                            color: root.c("menuDangerText", "danger")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            Region { role: "menuDangerText" }
                        }
                    }
                }
            }
        }

        // Side panel: the people in the room.
        Rectangle {
            Layout.preferredWidth: 168
            Layout.fillHeight: true
            color: root.c("sidePanel", "sidebar")

            GradientLayer { role: "sidePanel" }
            Region { role: "sidePanel" }
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: AppTheme.spacing12
                spacing: AppTheme.spacing6

                // A text field with what was typed in it selected.
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 26
                    radius: AppTheme.radiusMd
                    color: root.c("inputBackground", "surface")
                    border.width: 1
                    border.color: root.c("fieldBorder", "border")
                    Region { role: "inputBg" }
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        x: AppTheme.spacing8
                        width: typedPeople.implicitWidth
                        height: typedPeople.implicitHeight + 2
                        color: root.c("textSelection", "selectedHover")
                        Region { role: "textSelection"; pad: 2 }
                        Text {
                            id: typedPeople
                            anchors.centerIn: parent
                            text: qsTr("Sam")
                            color: root.c("fieldText", "textPrimary")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            Region { role: "fieldText" }
                        }
                    }
                    EdgeRegion { role: "fieldBorder" }
                }

                Text {
                    text: qsTr("Admins")
                    color: root.c("sidePanelSection", "textMuted")
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                    Region { role: "sidePanelSection" }
                }

                Repeater {
                    model: root.fakeMembers
                    delegate: Rectangle {
                        id: memberRow
                        required property string modelData
                        required property int index
                        // The third person is under the pointer.
                        readonly property bool hovered: index === 2
                        Layout.fillWidth: true
                        implicitHeight: 24
                        radius: AppTheme.radiusSm
                        color: hovered ? root.c("memberRowHover", "hover") : "transparent"
                        Region {
                            visible: memberRow.hovered
                            enabled: visible
                            role: "memberRowHover"
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: AppTheme.spacing4
                            anchors.rightMargin: AppTheme.spacing4
                            spacing: AppTheme.spacing8

                            IdentityDisc {
                                key: memberRow.modelData
                                implicitWidth: 18
                                implicitHeight: 18
                                // Presence: the first two are online.
                                Rectangle {
                                    visible: memberRow.index < 2
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    anchors.rightMargin: -2
                                    anchors.bottomMargin: -2
                                    width: 9
                                    height: 9
                                    radius: 4.5
                                    color: root.c("presenceDot", "presenceOnline")
                                    border.width: 2
                                    border.color: root.c("sidePanel", "sidebar")
                                    Region { role: "presenceDot"; repeated: true; pad: 2 }
                                }
                            }
                            // Names wear their owner's identity colour.
                            Text {
                                Layout.fillWidth: true
                                text: memberRow.modelData
                                color: root.nameInk(memberRow.modelData)
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMeta
                                elide: Text.ElideRight
                            }
                        }
                    }
                }

                // Status chips: a tint of their own colour.
                Flow {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing4

                    Repeater {
                        model: [
                            { label: qsTr("Verified"), role: "chipSuccess", group: "presenceOnline" },
                            { label: qsTr("Invited"),  role: "chipWarning", group: "warning" },
                            { label: qsTr("Banned"),   role: "chipDanger",  group: "dangerTint" }
                        ]
                        delegate: Rectangle {
                            id: statusChip
                            required property var modelData
                            implicitWidth: statusText.implicitWidth
                                           + AppTheme.spacing6 * 2
                            implicitHeight: 18
                            radius: AppTheme.radiusPill
                            color: root.tone(modelData.role, modelData.group, 0.14)
                            border.width: 1
                            border.color: root.tone(modelData.role, modelData.group, 0.32)
                            Region { role: statusChip.modelData.role }
                            Text {
                                id: statusText
                                anchors.centerIn: parent
                                textFormat: Text.PlainText
                                text: statusChip.modelData.label
                                color: root.c(statusChip.modelData.role,
                                              statusChip.modelData.group)
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMicro
                                font.weight: AppTheme.weightStrong
                            }
                        }
                    }
                }

                // States the window above shows only in the other layout or
                // another dialog. In the room-list layout: Channels rows; in
                // the Channels layout: room-list rows.
                Sample {
                    visible: !root.channels
                    role: "channelSelected"
                    label: qsTr("Open channel")
                    fill: root.c("channelSelected", "selected")
                    inkRole: "channelSelectedText"
                    ink: root.c("channelSelectedText", "selectedText")
                }
                Sample {
                    visible: !root.channels
                    role: "channelHover"
                    label: qsTr("Channel, hovered")
                    fill: root.c("channelHover", "hover")
                    inkRole: "channelText"
                    ink: root.c("channelText", "textSecondary")
                }
                // An open Channels folder: its outline, arrow and name.
                Rectangle {
                    visible: !root.channels
                    Layout.fillWidth: true
                    implicitHeight: 22
                    radius: AppTheme.radiusSm
                    color: "transparent"
                    border.width: 1
                    border.color: root.c("channelFolderBorder", "border")
                    EdgeRegion { role: "channelFolderBorder" }
                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        x: AppTheme.spacing6
                        spacing: AppTheme.spacing6
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 8
                            height: 8
                            radius: 1
                            color: root.c("channelChevron", "textMuted")
                            Region { role: "channelChevron"; pad: 2 }
                        }
                        Text {
                            text: qsTr("Creative Studio")
                            color: root.c("channelCategoryText", "textMuted")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            font.weight: AppTheme.weightStrong
                            Region { role: "channelCategoryText" }
                        }
                    }
                }
                Sample {
                    visible: root.channels
                    role: "roomSelected"
                    label: qsTr("Open room")
                    fill: root.c("roomSelected", "selected")
                    inkRole: "roomSelectedName"
                    ink: root.c("roomSelectedName", "selectedText")
                }
                Sample {
                    visible: root.channels
                    role: "roomHover"
                    label: qsTr("Room, hovered")
                    fill: root.c("roomHover", "hover")
                    inkRole: "roomName"
                    ink: root.c("roomName", "textSecondary")
                }
                Sample {
                    role: "buttonGhostHover"
                    label: qsTr("Quiet button")
                    fill: root.c("buttonGhostHover", "hover")
                    inkRole: "buttonGhostInk"
                    ink: root.c("buttonGhostInk", "textSecondary")
                }

                // Settings in miniature: its navigation column (the open
                // section marked, two more labels) beside its page (a heading
                // and two lines), split by Settings' divider.
                Row {
                    Layout.fillWidth: true
                    height: 40
                    Rectangle {
                        width: 44
                        height: parent.height
                        color: root.c("settingsNav", "background")
                        GradientLayer { role: "settingsNav" }
                        Region { role: "settingsNav" }
                        Column {
                            x: 4
                            y: 5
                            spacing: 4
                            Rectangle {
                                width: 36
                                height: 9
                                radius: 2
                                color: root.c("settingsNavSelected", "selectedHover")
                                Region { role: "settingsNavSelected" }
                            }
                            Repeater {
                                model: 2
                                delegate: Rectangle {
                                    x: 2
                                    width: 24
                                    height: 4
                                    radius: 2
                                    color: root.tone("settingsNavText", "textSecondary", 0.55)
                                    Region { role: "settingsNavText"; repeated: true; pad: 2 }
                                }
                            }
                        }
                    }
                    Rectangle {
                        width: 1
                        height: parent.height
                        color: root.c("settingsDivider", "border")
                        Region { role: "settingsDivider"; pad: 2 }
                    }
                    Rectangle {
                        width: parent.width - 45
                        height: parent.height
                        color: root.c("settingsPage", "background")
                        GradientLayer { role: "settingsPage" }
                        Region { role: "settingsPage" }
                        Text {
                            x: 6
                            y: 4
                            text: qsTr("Settings")
                            color: root.c("settingsHeading", "textPrimary")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMicro
                            font.weight: AppTheme.weightStrong
                            Region { role: "settingsHeading" }
                        }
                        Column {
                            x: 6
                            y: 22
                            spacing: 4
                            Repeater {
                                model: [0.8, 0.55]
                                delegate: Rectangle {
                                    required property real modelData
                                    width: 92 * modelData
                                    height: 4
                                    radius: 2
                                    color: root.tone("settingsText", "textMuted", 0.55)
                                    Region { role: "settingsText"; repeated: true; pad: 2 }
                                }
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                // A primary button at rest, under the pointer and held down.
                Row {
                    spacing: AppTheme.spacing4

                    Rectangle {
                        width: 44
                        height: 22
                        radius: AppTheme.radiusSm
                        color: root.c("buttonPrimaryFill", "accent")
                        Region { role: "buttonPrimaryFill" }
                        Text {
                            anchors.centerIn: parent
                            text: qsTr("Rest")
                            color: root.c("buttonPrimaryInk", "accentText")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMicro
                            Region { role: "buttonPrimaryInk"; repeated: true; pad: 1 }
                        }
                    }
                    Rectangle {
                        width: 44
                        height: 22
                        radius: AppTheme.radiusSm
                        color: root.c("accentHover", "accent")
                        Region { role: "accentHover" }
                        Text {
                            anchors.centerIn: parent
                            text: qsTr("Hover")
                            color: root.c("buttonPrimaryInk", "accentText")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMicro
                            Region { role: "buttonPrimaryInk"; repeated: true; pad: 1 }
                        }
                    }
                    Rectangle {
                        width: 44
                        height: 22
                        radius: AppTheme.radiusSm
                        color: root.c("accentPressed", "accent")
                        Region { role: "accentPressed" }
                        Text {
                            anchors.centerIn: parent
                            text: qsTr("Held")
                            color: root.c("buttonPrimaryInk", "accentText")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMicro
                            Region { role: "buttonPrimaryInk"; repeated: true; pad: 1 }
                        }
                    }
                }

                // A disabled button, so the disabled ink and edge are visible.
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 26
                    radius: AppTheme.radiusMd
                    color: "transparent"
                    border.width: 1
                    border.color: root.c("buttonDisabledBorder", "border")
                    EdgeRegion { role: "buttonDisabledBorder" }
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Invite")
                        color: root.c("buttonDisabledInk", "textDisabled")
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                        Region { role: "buttonDisabledInk" }
                    }
                }
            }
        }
    }
}
