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
// Every editable role must be painted by at least one Region below (the suite
// `everyEditableRoleHasARegionInThePreview` scans for `role: "<key>"`), or the
// role would be editable but impossible to see change. Add the surface here
// first when adding a role to CustomThemeStore.
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
    // The ring's stroke in this item's own pixels. The editor scales the
    // preview, so it passes 2 / scale to keep the ring 2px on screen.
    property real outlineWidth: 2
    // Surface gradients to paint: role -> { type, angle, stops }, the theme's
    // own (CustomThemeStore.gradients) and, while Depth is on, Depth's for the
    // grounds the theme leaves flat. A role without one stays flat.
    property var gradients: ({})

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
        // Grows the hit area and the outline past a thin element (a hairline,
        // a scrollbar) so it can be pointed at.
        property real pad: 0
        // How loudly this region is outlined: 3 open in the picker, 2 pointed
        // at in the list, 1 recoloured along with either, 0.5 under the
        // pointer here, 0 not at all.
        readonly property real emphasis:
            root.highlightRole === region.role ? 3
            : root.hoverRole === region.role ? 2
            : root.linkedRoles[region.role] === true ? 1
            : region.containsMouse ? 0.5 : 0
        anchors.fill: parent
        anchors.margins: -region.pad
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: (mouse) => root.regionActivated(
            region.role, root.regionStack(region, mouse.x, mouse.y))

        // Built only while shown: there are about ninety regions.
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
    // one dialog: a Channels row, a standard control, a quiet button.
    component Sample: Rectangle {
        id: smp
        required property string role
        property string label
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
            anchors.right: parent.right
            anchors.rightMargin: AppTheme.spacing8
            text: smp.label
            textFormat: Text.PlainText
            color: smp.ink
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
            elide: Text.ElideRight
        }
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
                spacing: AppTheme.spacing8

                Repeater {
                    model: 3
                    delegate: Rectangle {
                        id: railTile
                        required property int index
                        Layout.alignment: Qt.AlignHCenter
                        implicitWidth: 34
                        implicitHeight: 34
                        radius: AppTheme.radiusMd
                        // The middle tile stands for the open Space.
                        color: index === 1 ? root.c("selected", "hover")
                                           : root.c("cardElevated", "surface")
                        border.width: index === 1 ? 1 : 0
                        border.color: root.c("accent", "border")

                        Region { role: railTile.index === 1 ? "selected" : "cardElevated" }

                        // An unread count on the first Space.
                        Rectangle {
                            visible: railTile.index === 0
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.rightMargin: -4
                            anchors.topMargin: -4
                            width: 14
                            height: 14
                            radius: 7
                            color: root.c("unreadBadge", "accent")
                            border.width: 2
                            border.color: root.c("rail", "sidebar")
                            Region { role: "unreadBadge" }
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                // Account avatar: a circle, as people are in this shell.
                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    implicitWidth: 30
                    implicitHeight: 30
                    radius: width / 2
                    color: root.c("accent", "border")
                    Region { role: "accent" }
                }
            }

        }

        // Room list
        Rectangle {
            Layout.preferredWidth: 236
            Layout.fillHeight: true
            color: root.c("sidebar", "background")
            clip: true

            GradientLayer { role: "sidebar" }
            Region { role: "sidebar" }
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: AppTheme.spacing12
                spacing: AppTheme.spacing8

                Text {
                    text: qsTr("Lightning")
                    color: root.c("textPrimary", "textPrimary")
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightStrong
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    Region { role: "textPrimary" }
                }

                // Search field: the input surface.
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 28
                    radius: AppTheme.radiusMd
                    color: root.c("inputBackground", "surface")
                    border.width: 1
                    border.color: root.c("border", "border")
                    Region { role: "inputBg" }
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
                }

                Text {
                    visible: !root.channels
                    text: qsTr("Rooms")
                    color: root.c("sectionLabelColor", "textMuted")
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                    Layout.fillWidth: true
                    Region { role: "textMuted" }
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
                        Layout.fillWidth: true
                        Layout.leftMargin: isRoom ? AppTheme.spacing8 : 0
                        implicitHeight: 26
                        radius: AppTheme.radiusSm
                        color: isSelected ? root.c("channelSelected", "selected") : (modelData.state === "hovered" ? root.c("channelHover", "hover") : "transparent")
                        border.width: fakeChannelRow.isFolder ? 1 : 0
                        border.color: root.c("border", "border")

                        // Beneath the text, so the text names its own ink.
                        Region {
                            role: fakeChannelRow.isSelected ? "channelSelected" : (fakeChannelRow.modelData.state === "hovered" ? "channelHover" : "sidebar")
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: AppTheme.spacing6
                            anchors.rightMargin: AppTheme.spacing8
                            spacing: AppTheme.spacing6

                            // The folder's chevron, or the room's avatar.
                            Rectangle {
                                implicitWidth: fakeChannelRow.isFolder ? 8 : 16
                                implicitHeight: fakeChannelRow.isFolder ? 8 : 16
                                radius: fakeChannelRow.isFolder ? 1 : AppTheme.radiusSm
                                color: fakeChannelRow.isFolder ? root.c("textMuted", "textSecondary") : root.c("cardElevated", "surface")
                            }
                            Text {
                                text: fakeChannelRow.modelData.name
                                textFormat: Text.PlainText
                                color: fakeChannelRow.isSelected ? root.c("selectedText", "textPrimary") : (fakeChannelRow.isFolder ? root.c("textMuted", "textSecondary") : root.c("textSecondary", "textSecondary"))
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMeta
                                font.weight: fakeChannelRow.isFolder ? AppTheme.weightStrong : AppTheme.weightBody
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                                Region {
                                    role: fakeChannelRow.isSelected ? "selectedText" : (fakeChannelRow.isFolder ? "textMuted" : "textSecondary")
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
                        Layout.fillWidth: true
                        implicitHeight: 42
                        radius: AppTheme.radiusMd
                        color: modelData.state === "selectedHover"
                               ? root.c("selectedHover", "selected")
                             : isSelected ? root.c("roomSelected", "selected")
                             : modelData.state === "hovered"
                               ? root.c("roomHover", "hover")
                               : "transparent"

                        // Beneath the content, so each ink names itself.
                        Region {
                            role: fakeRoomRow.modelData.state === "selectedHover"
                                  ? "selectedHover"
                                  : fakeRoomRow.isSelected
                                    ? "roomSelected"
                                    : fakeRoomRow.modelData.state === "hovered"
                                      ? "roomHover" : "sidebar"
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: AppTheme.spacing8
                            anchors.rightMargin: AppTheme.spacing8
                            spacing: AppTheme.spacing8

                            Rectangle {
                                implicitWidth: 26
                                implicitHeight: 26
                                radius: AppTheme.radiusSm
                                color: root.c("cardElevated", "surface")
                                Region { role: "cardElevated" }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Text {
                                    text: fakeRoomRow.modelData.name
                                    textFormat: Text.PlainText
                                    color: fakeRoomRow.isSelected
                                           ? root.c("selectedText", "textPrimary")
                                           : root.c("textPrimary", "textPrimary")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    font.weight: AppTheme.weightStrong
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                    Region {
                                        role: fakeRoomRow.isSelected ? "selectedText" : "textPrimary"
                                    }
                                }
                                Text {
                                    // Untrusted text: never markup.
                                    textFormat: Text.PlainText
                                    text: fakeRoomRow.modelData.preview
                                    color: fakeRoomRow.isSelected
                                           ? root.c("selectedText", "textPrimary")
                                           : root.c("textMuted", "textSecondary")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                    Region {
                                        role: fakeRoomRow.isSelected ? "selectedText" : "textMuted"
                                    }
                                }
                            }

                            // Unread count.
                            Rectangle {
                                visible: fakeRoomRow.modelData.badge > 0
                                implicitWidth: 20
                                implicitHeight: 18
                                radius: AppTheme.radiusPill
                                color: root.c("unreadBadge", "accent")
                                Region { role: "unreadBadge" }
                                Text {
                                    anchors.centerIn: parent
                                    text: fakeRoomRow.modelData.badge
                                    color: root.c("accentText", "textPrimary")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMicro
                                    font.weight: AppTheme.weightStrong
                                }
                            }

                            // Mention count: the pill that says someone named you.
                            Rectangle {
                                visible: fakeRoomRow.modelData.mention > 0
                                implicitWidth: 20
                                implicitHeight: 18
                                radius: AppTheme.radiusPill
                                color: root.c("mentionBadge", "accent")
                                Region { role: "mention" }
                                Text {
                                    anchors.centerIn: parent
                                    text: fakeRoomRow.modelData.mention
                                    color: AppTheme.dangerText
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMicro
                                    font.weight: AppTheme.weightStrong
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
            color: root.c("background", "background")
            clip: true

            GradientLayer { role: "background" }
            Region { role: "background" }
            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                // Room header.
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 44
                    color: root.c("surface", "background")
                    GradientLayer { role: "surface" }
                    Region { role: "surface" }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: AppTheme.spacing12
                        text: qsTr("Lightning")
                        color: root.c("textPrimary", "textPrimary")
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textBody
                        font.weight: AppTheme.weightStrong
                        Region { role: "textPrimary" }
                    }
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: root.c("border", "border")
                        // A one pixel line is not clickable; the region is
                        // widened around it.
                        Region { role: "border"; pad: 3 }
                    }
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
                        Region { role: "otherBubble" }
                        ColumnLayout {
                            id: incomingCol
                            anchors.fill: parent
                            anchors.margins: AppTheme.spacing8
                            spacing: 2
                            RowLayout {
                                spacing: AppTheme.spacing6
                                Text {
                                    text: qsTr("Sam")
                                    color: root.c("textMuted", "textSecondary")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    font.weight: AppTheme.weightStrong
                                    Region { role: "textMuted" }
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
                                Region { role: "textPrimary" }
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

                    // Incoming, carrying a link, a mention, a raised chip and a
                    // reaction pill: roles shown nowhere else in the preview.
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.maximumWidth: 360
                        implicitHeight: richCol.implicitHeight
                                        + AppTheme.spacing8 * 2
                        radius: AppTheme.radiusMd
                        color: root.c("otherBubble", "surface")
                        Region { role: "otherBubble" }
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
                                    Region { role: "textPrimary" }
                                }
                                Text {
                                    text: qsTr("the notes")
                                    color: root.c("link", "accent")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    font.underline: true
                                    Region { role: "link" }
                                }
                                // The inline "@you" chip: a wash of the accent,
                                // body ink (the pill with a count is the room
                                // list's, under "Mentions").
                                Rectangle {
                                    implicitWidth: youText.implicitWidth
                                                   + AppTheme.spacing6 * 2
                                    implicitHeight: youText.implicitHeight + 2
                                    radius: AppTheme.radiusSm
                                    color: root.tone("mentionHighlight",
                                                     "accent", 0.16)
                                    Text {
                                        id: youText
                                        anchors.centerIn: parent
                                        text: qsTr("@you")
                                        color: root.c("textPrimary", "textPrimary")
                                        font.family: AppTheme.uiFont
                                        font.pixelSize: AppTheme.textMeta
                                        font.weight: AppTheme.weightStrong
                                    }
                                    Region { role: "accent" }
                                }
                            }

                            // Code, a surface that is neither bubble nor chip.
                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 26
                                radius: AppTheme.radiusSm
                                color: root.c("codeBlock", "cardElevated")
                                Region { role: "codeBlock" }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.leftMargin: AppTheme.spacing6
                                    text: qsTr("git push")
                                    color: root.c("textSecondary", "textPrimary")
                                    font.family: AppTheme.monoFont
                                    font.pixelSize: AppTheme.textMeta
                                    Region { role: "textSecondary" }
                                }
                            }

                            // Raised chip: the surface every embed uses.
                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 26
                                radius: AppTheme.radiusSm
                                color: root.c("cardElevated", "surface")
                                border.width: 1
                                border.color: root.c("border", "border")
                                Region { role: "cardElevated" }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.leftMargin: AppTheme.spacing6
                                    text: qsTr("Release notes")
                                    color: root.c("textSecondary", "textPrimary")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    elide: Text.ElideRight
                                    Region { role: "textSecondary" }
                                }
                            }

                            // Reactions: someone else's, then your own.
                            RowLayout {
                                spacing: AppTheme.spacing6

                                Rectangle {
                                    implicitWidth: 44
                                    implicitHeight: 22
                                    radius: AppTheme.radiusPill
                                    color: root.c("reactionBackground", "cardElevated")
                                    border.width: 1
                                    border.color: root.c("reactionBorder", "border")
                                    Region { role: "reaction" }
                                    Text {
                                        anchors.centerIn: parent
                                        // Not an emoji: emoji literals are banned
                                        // in row delegates here.
                                        text: qsTr("+2")
                                        color: root.c("reactionInk", "textSecondary")
                                        font.family: AppTheme.uiFont
                                        font.pixelSize: AppTheme.textMeta
                                    }
                                }

                                // Yours: soft accent fill inside its own edge.
                                Rectangle {
                                    implicitWidth: 44
                                    implicitHeight: 22
                                    radius: AppTheme.radiusPill
                                    color: root.c("accentBorder", "borderStrong")
                                    Region { role: "accentBorder" }
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: 1
                                        radius: AppTheme.radiusPill
                                        color: root.c("reactionSelectedBackground",
                                                      "accentSoft")
                                        Region { role: "reactionSelectedBackground" }
                                        Text {
                                            anchors.centerIn: parent
                                            text: qsTr("+1")
                                            color: root.c("textPrimary", "textPrimary")
                                            font.family: AppTheme.uiFont
                                            font.pixelSize: AppTheme.textMeta
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
                        color: root.c("inputBackground", "surface")
                        border.width: 1
                        border.color: root.c("border", "border")
                        Region { role: "inputBg" }
                        // Draft text with part of it selected.
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: AppTheme.spacing12
                            Text {
                                text: qsTr("Typing ")
                                color: root.c("textPrimary", "textPrimary")
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMeta
                                Region { role: "textPrimary" }
                            }
                            Rectangle {
                                width: selectedWord.implicitWidth
                                height: selectedWord.implicitHeight + 2
                                color: root.c("textSelection", "selectedHover")
                                Text {
                                    id: selectedWord
                                    anchors.centerIn: parent
                                    text: qsTr("this")
                                    color: root.c("textPrimary", "textPrimary")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                }
                                Region { role: "textSelection" }
                            }
                        }
                    }

                    Rectangle {
                        implicitWidth: 64
                        implicitHeight: 36
                        radius: AppTheme.radiusMd
                        color: root.c("accent", "accent")

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
                            Region { role: "accent" }
                            Text {
                                anchors.centerIn: parent
                                text: qsTr("Send")
                                color: root.c("accentText", "textPrimary")
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMeta
                                font.weight: AppTheme.weightStrong
                                Region { role: "accentText" }
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
                anchors.topMargin: 60
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
                border.color: root.c("borderStrong", "border")
                Region { role: "popoverSurface" }

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
                            color: root.c("textPrimary", "textPrimary")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            Region { role: "textPrimary" }
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
                            color: root.c("textPrimary", "textPrimary")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            Region { role: "textPrimary" }
                        }
                    }
                    Rectangle {
                        width: parent.width
                        height: 24
                        radius: AppTheme.radiusSm
                        // A destructive row under the pointer: a tint of the
                        // danger family.
                        color: root.tone("dangerTint", "mentionBadge", 0.10)
                        Region { role: "dangerTint" }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: AppTheme.spacing8
                            text: qsTr("Delete")
                            color: root.c("danger", "textPrimary")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            Region { role: "danger" }
                        }
                    }
                }
            }

        }

        // Member list: shows a plain panel surface next to the timeline ground.
        Rectangle {
            Layout.preferredWidth: 168
            Layout.fillHeight: true
            color: root.c("surface", "background")

            GradientLayer { role: "surface" }
            Region { role: "surface" }
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: AppTheme.spacing12
                spacing: AppTheme.spacing8

                Text {
                    text: qsTr("People")
                    color: root.c("sectionLabelColor", "textMuted")
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                    Region { role: "textMuted" }
                }

                Repeater {
                    model: root.fakeMembers
                    delegate: RowLayout {
                        required property string modelData
                        required property int index
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8

                        Rectangle {
                            implicitWidth: 22
                            implicitHeight: 22
                            radius: width / 2
                            color: root.c("cardElevated", "surface")
                            Region { role: "cardElevated" }

                            // Presence: the first two are online.
                            Rectangle {
                                visible: index < 2
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.rightMargin: -2
                                anchors.bottomMargin: -2
                                width: 9
                                height: 9
                                radius: 4.5
                                color: root.c("presenceOnline", "success")
                                border.width: 2
                                border.color: root.c("surface", "background")
                                Region { role: "presenceOnline"; pad: 2 }
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: modelData
                            color: index === 0
                                   ? root.c("textPrimary", "textPrimary")
                                   : root.c("textSecondary", "textPrimary")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            elide: Text.ElideRight
                            Region {
                                role: index === 0 ? "textPrimary" : "textSecondary"
                            }
                        }
                    }
                }

                // A soft accent chip, as on a power-level mark or an active
                // icon: tinted fill inside its own edge.
                Rectangle {
                    implicitWidth: 56
                    implicitHeight: 22
                    radius: AppTheme.radiusPill
                    color: root.c("accentBorder", "borderStrong")
                    Region { role: "accentBorder" }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 1
                        radius: AppTheme.radiusPill
                        color: root.c("accentSoft", "selected")
                        Region { role: "accentSoft" }
                        Text {
                            anchors.centerIn: parent
                            text: qsTr("Admin")
                            color: root.c("textPrimary", "textPrimary")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMicro
                        }
                    }
                }

                // Status chips: a tint of their own ink.
                Flow {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing4

                    Repeater {
                        model: [
                            { label: qsTr("Verified"), role: "success" },
                            { label: qsTr("Slow"),     role: "warning" },
                            { label: qsTr("Failed"),   role: "danger" }
                        ]
                        delegate: Rectangle {
                            id: statusChip
                            required property var modelData
                            implicitWidth: statusText.implicitWidth
                                           + AppTheme.spacing6 * 2
                            implicitHeight: 18
                            radius: AppTheme.radiusPill
                            color: root.tone(modelData.role, "textMuted", 0.14)
                            border.width: 1
                            border.color: root.tone(modelData.role, "textMuted", 0.32)
                            Region { role: statusChip.modelData.role }
                            Text {
                                id: statusText
                                anchors.centerIn: parent
                                textFormat: Text.PlainText
                                text: statusChip.modelData.label
                                color: root.c(statusChip.modelData.role, "textMuted")
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMicro
                                font.weight: AppTheme.weightStrong
                            }
                        }
                    }
                }

                // States the window above shows only in another layout or
                // another dialog.
                Sample {
                    role: "hover"
                    label: qsTr("Row, hovered")
                    fill: root.c("hover", "surface")
                }
                Sample {
                    role: "channelSelected"
                    label: qsTr("Open channel")
                    fill: root.c("channelSelected", "selected")
                    ink: root.c("selectedText", "textPrimary")
                }
                Sample {
                    role: "channelHover"
                    label: qsTr("Channel, hovered")
                    fill: root.c("channelHover", "hover")
                }
                Sample {
                    role: "paletteHighlight"
                    label: qsTr("Standard control")
                    fill: root.c("paletteHighlight", "selected")
                    ink: root.c("selectedText", "textPrimary")
                }
                Sample {
                    role: "buttonGhostHover"
                    label: qsTr("Quiet button")
                    fill: root.c("buttonGhostHover", "hover")
                }
                // Settings in miniature: its navigation column (three
                // entries, the open one marked) beside its page (a heading
                // and two lines). Two bare boxes read as broken rectangles.
                Row {
                    Layout.fillWidth: true
                    height: 40
                    Rectangle {
                        width: 44
                        height: parent.height
                        color: root.c("settingsNav", "background")
                        border.width: 1
                        border.color: root.c("border", "border")
                        // Inside the 1px border.
                        GradientLayer { role: "settingsNav"; anchors.margins: 1 }
                        Region { role: "settingsNav" }
                        Column {
                            x: 6
                            y: 7
                            spacing: 4
                            Repeater {
                                model: 3
                                delegate: Rectangle {
                                    required property int index
                                    width: index === 0 ? 32 : 24
                                    height: 5
                                    radius: 2
                                    color: index === 0
                                           ? root.c("selected", "hover")
                                           : root.tone("textMuted", "textSecondary", 0.45)
                                }
                            }
                        }
                    }
                    Rectangle {
                        width: parent.width - 44
                        height: parent.height
                        color: root.c("settingsPage", "background")
                        border.width: 1
                        border.color: root.c("border", "border")
                        GradientLayer { role: "settingsPage"; anchors.margins: 1 }
                        Region { role: "settingsPage" }
                        Text {
                            x: 6
                            y: 4
                            text: qsTr("Settings")
                            color: root.c("textPrimary", "textPrimary")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMicro
                            font.weight: AppTheme.weightStrong
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
                                    color: root.tone("textMuted", "textSecondary", 0.45)
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
                        color: root.c("accent", "accent")
                        Region { role: "accent" }
                        Text {
                            anchors.centerIn: parent
                            text: qsTr("Rest")
                            color: root.c("accentText", "textPrimary")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMicro
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
                            color: root.c("accentText", "textPrimary")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMicro
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
                            color: root.c("accentText", "textPrimary")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMicro
                        }
                    }
                }

                // A disabled control, so the disabled ink is visible.
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 30
                    radius: AppTheme.radiusMd
                    color: "transparent"
                    border.width: 1
                    border.color: root.c("borderStrong", "border")
                    Region { role: "borderStrong" }
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Invite")
                        color: root.c("textDisabled", "textMuted")
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                        Region { role: "textDisabled" }
                    }
                }
            }

        }
    }
}
