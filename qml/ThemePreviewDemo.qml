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
    property bool channels: false
    property string highlightRole: ""
    property string hoverRole: ""
    property var linkedRoles: ({})
    property var problemRoles: ({})
    property real outlineWidth: 2
    property var gradients: ({})

    property var roleLabel: null
    property var roleState: null
    property Item kbRegion: null
    readonly property string keyboardRole:
        root.kbRegion && root.kbRegion.activeFocus ? root.kbRegion.role : ""
    property bool kbLeaving: false
    signal keyboardEscape()

    function labelOf(role) {
        var v = root.roleLabel ? root.roleLabel(role) : ""
        return v && v.length > 0 ? v : role
    }
    function stateOf(role) {
        return root.roleState ? root.roleState(role) : ""
    }

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
    function activateFocused() {
        if (root.kbRegion && root.kbRegion.activeFocus)
            root.regionActivated(root.kbRegion.role, [root.kbRegion.role])
    }
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

    signal regionActivated(string role, var stack)

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
                if (!k.contains(k.mapFromItem(region, x, y)))
                    continue
                out.push(k.role)
            }
            item = item.parent
        }
        return out
    }

    implicitWidth: 880
    implicitHeight: 560
    clip: true

    function c(role, fallback) {
        var p = root.pal
        if (p && p[role] !== undefined)
            return p[role]
        return p && p[fallback] !== undefined ? p[fallback] : "transparent"
    }

    function tone(role, fallback, alpha) {
        return Qt.alpha(Qt.color(String(root.c(role, fallback))), alpha)
    }

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

    readonly property var fakeRooms: [
        { name: qsTr("Design"),    preview: qsTr("Shipped the new palette"), badge: 0, mention: 0, state: "normal" },
        { name: qsTr("Lightning"), preview: qsTr("Storm looks good now"),    badge: 0, mention: 0, state: "selected" },
        { name: qsTr("Alex"),      preview: qsTr("See you at six"),          badge: 3, mention: 0, state: "hovered" },
        { name: qsTr("Releases"),  preview: qsTr("New release is out"),      badge: 0, mention: 1, state: "normal" },
        { name: qsTr("Support"),   preview: qsTr("Open, pointer on it"),     badge: 0, mention: 0, state: "selectedHover" }
    ]

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
        readonly property bool isThemeRegion: true
        property bool repeated: false
        property real pad: 0
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
        Accessible.role: Accessible.Button
        Accessible.name: root.labelOf(region.role)
        Accessible.description: root.stateOf(region.role)
        Accessible.focusable: true
        Accessible.onPressAction: root.regionActivated(region.role, [region.role])
        onClicked: (mouse) => {
            root.kbRegion = region
            root.regionActivated(
                region.role, root.regionStack(region, mouse.x, mouse.y))
        }

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

        Loader {
            anchors.fill: parent
            // A text element's box hugs its glyphs, so the ring would stroke
            // through the letters; it stands off by one ring width. (Text has
            // no `radius`; every Rectangle has one, 0 or not.)
            anchors.margins: region.parent && region.parent.radius === undefined
                             ? -root.outlineWidth * 1.5 : 0
            active: region.emphasis > 0
            sourceComponent: Component {
                Item {
                    id: ring
                    readonly property real w: root.outlineWidth
                    readonly property real r:
                        region.parent && region.parent.radius !== undefined
                        ? region.parent.radius + region.pad
                        : AppTheme.radiusSm
                    readonly property bool strong: region.emphasis >= 2
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

    component GradientLayer: ThemedSurface {
        anchors.fill: parent
        flatFill: false
        specOverride: root.gradientSpec(role)
    }

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

                            Rectangle {
                                visible: fakeChannelRow.isFolder
                                implicitWidth: 8
                                implicitHeight: 8
                                radius: 1
                                color: root.c("channelChevron", "textMuted")
                                Region { role: "channelChevron"; repeated: true; pad: 2 }
                            }
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
                        Region { role: "headerDivider"; pad: 3 }
                    }
                }

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

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing8

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.maximumWidth: 320
                        implicitHeight: incomingCol.implicitHeight
                                        + AppTheme.spacing8 * 2
                        radius: AppTheme.radiusMd
                        color: root.c("otherBubble", "surface")
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
                                Text {
                                    text: qsTr("@you")
                                    color: root.c("selfMentionInk", "accent")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    font.weight: AppTheme.weightBold
                                    Region { role: "selfMentionInk" }
                                }
                            }

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
