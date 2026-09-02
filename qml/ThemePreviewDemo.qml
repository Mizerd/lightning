import QtQuick
import QtQuick.Layouts
import MatrixClient

// A miniature, entirely FAKE Lightning window, used as the live preview in the
// custom-theme editor: pick a colour on the left, watch this repaint.
//
// It is NOT a real room and must never become one. Every name, message and
// count below is a literal in this file — no `app.` anything, no models, no
// controllers, no media. That is what lets it render in the Settings screen
// while signed out, and what stops a theme preview from leaking a real
// conversation into a screenshot.
//
// Colours come from `palette`, a role -> colour object produced by
// AppTheme.paletteForTheme(id) — NOT from the live AppTheme singleton. That is
// the whole reason this preview can show a theme the application is not
// currently running: the editor is usable before the theme is applied, and the
// editor's own chrome stays on the invariant brand navy while the palette
// under construction is confined to this rectangle. Non-colour tokens
// (spacing, radii, type) still come from AppTheme — those are not themed.
//
// Every region is clickable and reports the role it paints, so a person can
// point at the thing they want to recolour instead of hunting a list. That was
// the original request: "each spot like room list the menu with spaces on the
// left and top part and all else should have a option to select any color".
Item {
    id: root

    // role -> colour. Assign AppTheme.paletteForTheme(id).
    // NOT named `palette`: QQuickItem carries a `palette` property of its own
    // in Qt 6, and shadowing it silently gives every child a broken one.
    required property var pal
    // Which navigation layout the room-list column shows. The editor binds this
    // to the user's own choice, because a preview of a column they do not use
    // is a preview of the wrong thing: the Channels column has different rows,
    // different weights and a folder header the Classic one does not, and the
    // whole point of watching the preview is to see where a colour lands.
    property bool channels: false
    // The role currently open in the picker; outlined here so the list and
    // the preview agree about what is being edited.
    property string highlightRole: ""

    // A region was clicked. The editor opens that role in the picker.
    signal regionActivated(string role)

    // Natural size. The editor scales this to fit and never upscales past
    // 1.0 — Item.scale renders at the ORIGINAL resolution and then stretches,
    // so an upscaled preview is a blurry preview. Sized so a maximised window
    // shows it at 1:1.
    implicitWidth: 880
    implicitHeight: 560
    clip: true

    function c(role, fallback) {
        var p = root.pal
        if (p && p[role] !== undefined)
            return p[role]
        return p && p[fallback] !== undefined ? p[fallback] : "transparent"
    }

    // Fixture rows. `state` drives which row renders selected vs hovered, so
    // both states are on screen at once with no interaction — the user is
    // editing those colours and has to see them.
    readonly property var fakeRooms: [
        { name: qsTr("Design"),    preview: qsTr("Shipped the new palette"), badge: 0, state: "normal" },
        { name: qsTr("Lightning"), preview: qsTr("Storm looks good now"),    badge: 0, state: "selected" },
        { name: qsTr("Alex"),      preview: qsTr("See you at six"),          badge: 3, state: "hovered" },
        { name: qsTr("Releases"),  preview: qsTr("v0.7.4 is out"),           badge: 0, state: "normal" }
    ]

    // The Channels shape: two navigation rows, a group, and a Space folder
    // holding rooms. Literals like everything else here — no models, no `app.`.
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

    // One reusable click target + edit outline per region. Declared as a
    // component so a region is three lines at the call site and every region
    // behaves identically.
    // A MouseArea and deliberately NOT a TapHandler: pointer handlers are
    // non-exclusive across subtrees (the lesson this codebase has re-learned
    // four times), so a click on a room row would fire the row's region AND
    // the room list's region behind it. A MouseArea accepts the press and
    // stops there. Container regions are therefore declared FIRST, beneath
    // their content, and leaf regions inside it win by being on top.
    component Region: MouseArea {
        id: region
        required property string role
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.regionActivated(region.role)

        Rectangle {
            anchors.fill: parent
            color: "transparent"
            visible: root.highlightRole === region.role || region.containsMouse
            border.width: root.highlightRole === region.role ? 2 : 1
            border.color: AppTheme.editorAccent
            opacity: root.highlightRole === region.role ? 1.0 : 0.55
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ── Spaces rail ──────────────────────────────────────────────────
        Rectangle {
            Layout.preferredWidth: 60
            Layout.fillHeight: true
            color: root.c("rail", "sidebar")

            Region { role: "rail" }
            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: AppTheme.spacing12
                anchors.bottomMargin: AppTheme.spacing12
                spacing: AppTheme.spacing8

                Repeater {
                    model: 3
                    delegate: Rectangle {
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
                    }
                }

                Item { Layout.fillHeight: true }

                // Account avatar: a circle, because people are circles in
                // this shell and rooms are rounded squares.
                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    implicitWidth: 30
                    implicitHeight: 30
                    radius: width / 2
                    color: root.c("accent", "border")
                }
            }

        }

        // ── Room list ────────────────────────────────────────────────────
        Rectangle {
            Layout.preferredWidth: 236
            Layout.fillHeight: true
            color: root.c("sidebar", "background")
            clip: true

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
                }

                // Search field: the input surface, where a person expects to
                // find it.
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 28
                    radius: AppTheme.radiusMd
                    color: root.c("inputBackground", "surface")
                    border.width: 1
                    border.color: root.c("border", "border")
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: AppTheme.spacing8
                        text: qsTr("Search")
                        color: root.c("textMuted", "textSecondary")
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                    }
                    Region { role: "inputBg" }
                }

                Text {
                    visible: !root.channels
                    text: qsTr("Rooms")
                    color: root.c("sectionLabelColor", "textMuted")
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                    Layout.fillWidth: true
                }

                // ── The Channels shape ────────────────────────────────────
                // Same roles, different rows. A folder header, then indented
                // rooms with an avatar and a name — which is what the user
                // sees if that is the layout they chose.
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
                        color: isSelected ? root.c("selected", "hover") : (modelData.state === "hovered" ? root.c("hover", "surface") : "transparent")
                        border.width: fakeChannelRow.isFolder ? 1 : 0
                        border.color: root.c("border", "border")

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
                            }
                        }

                        Region {
                            role: fakeChannelRow.isSelected ? "selected" : (fakeChannelRow.modelData.state === "hovered" ? "hover" : "sidebar")
                        }
                    }
                }

                Repeater {
                    model: root.channels ? [] : root.fakeRooms
                    delegate: Rectangle {
                        id: fakeRoomRow
                        required property var modelData
                        readonly property bool isSelected: modelData.state === "selected"
                        Layout.fillWidth: true
                        implicitHeight: 42
                        radius: AppTheme.radiusMd
                        color: isSelected ? root.c("selected", "hover")
                             : modelData.state === "hovered"
                               ? root.c("hover", "surface")
                               : "transparent"

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
                                }
                                Text {
                                    text: fakeRoomRow.modelData.preview
                                    color: fakeRoomRow.isSelected
                                           ? root.c("selectedText", "textPrimary")
                                           : root.c("textMuted", "textSecondary")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }

                            Rectangle {
                                visible: fakeRoomRow.modelData.badge > 0
                                implicitWidth: 20
                                implicitHeight: 18
                                radius: AppTheme.radiusPill
                                color: root.c("unreadBadge", "accent")
                                Text {
                                    anchors.centerIn: parent
                                    text: fakeRoomRow.modelData.badge
                                    color: root.c("accentText", "textPrimary")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMicro
                                    font.weight: AppTheme.weightStrong
                                }
                            }
                        }

                        Region {
                            role: fakeRoomRow.isSelected
                                  ? "selected"
                                  : fakeRoomRow.modelData.state === "hovered"
                                    ? "hover" : "sidebar"
                        }
                    }
                }

                Item { Layout.fillHeight: true }
            }

        }

        // ── Timeline + composer ──────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: root.c("background", "background")
            clip: true

            Region { role: "background" }
            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                // Room header.
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 44
                    color: root.c("surface", "background")
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: AppTheme.spacing12
                        text: qsTr("Lightning")
                        color: root.c("textPrimary", "textPrimary")
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textBody
                        font.weight: AppTheme.weightStrong
                    }
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: root.c("border", "border")
                    }
                    Region { role: "surface" }
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
                        ColumnLayout {
                            id: incomingCol
                            anchors.fill: parent
                            anchors.margins: AppTheme.spacing8
                            spacing: 2
                            Text {
                                text: qsTr("Sam")
                                color: root.c("textMuted", "textSecondary")
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMeta
                                font.weight: AppTheme.weightStrong
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("The new ladder reads much better.")
                                color: root.c("otherBubbleText", "textPrimary")
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMeta
                                wrapMode: Text.WordWrap
                            }
                        }
                        Region { role: "otherBubble" }
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
                        Text {
                            id: outgoingText
                            anchors.fill: parent
                            anchors.margins: AppTheme.spacing8
                            text: qsTr("Agreed — shipping it.")
                            color: root.c("ownBubbleText", "accentText")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            wrapMode: Text.WordWrap
                        }
                        Region { role: "ownBubble" }
                    }

                    // Incoming carrying a link, a mention, a raised chip and a
                    // reaction pill — four editable roles that appear nowhere
                    // else in this preview.
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.maximumWidth: 360
                        implicitHeight: richCol.implicitHeight
                                        + AppTheme.spacing8 * 2
                        radius: AppTheme.radiusMd
                        color: root.c("otherBubble", "surface")
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
                                    color: root.c("mentionBadge", "accent")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    font.weight: AppTheme.weightStrong
                                    Region { role: "mention" }
                                }
                            }

                            // Code: the one surface a message can carry that
                            // is neither a bubble nor a chip.
                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 26
                                radius: AppTheme.radiusSm
                                color: root.c("codeBlock", "cardElevated")
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.leftMargin: AppTheme.spacing6
                                    text: qsTr("git push")
                                    color: root.c("textSecondary", "textPrimary")
                                    font.family: AppTheme.monoFont
                                    font.pixelSize: AppTheme.textMeta
                                }
                                Region { role: "codeBlock" }
                            }

                            // Raised chip: the surface every embed uses.
                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 26
                                radius: AppTheme.radiusSm
                                color: root.c("cardElevated", "surface")
                                border.width: 1
                                border.color: root.c("border", "border")
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.leftMargin: AppTheme.spacing6
                                    text: qsTr("Release notes")
                                    color: root.c("textSecondary", "textPrimary")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    elide: Text.ElideRight
                                }
                                Region { role: "cardElevated" }
                            }

                            Rectangle {
                                implicitWidth: 44
                                implicitHeight: 22
                                radius: AppTheme.radiusPill
                                color: root.c("reactionBackground", "cardElevated")
                                border.width: 1
                                border.color: root.c("reactionBorder", "border")
                                Text {
                                    anchors.centerIn: parent
                                    // Not an emoji: emoji literals are banned
                                    // in row delegates here, and a count with
                                    // a glyph name shows the same colours.
                                    text: qsTr("+2")
                                    color: root.c("reactionInk", "textSecondary")
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                }
                                Region { role: "reaction" }
                            }
                        }
                        Region { role: "otherBubble" }
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
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: AppTheme.spacing12
                            text: qsTr("Message")
                            color: root.c("textMuted", "textSecondary")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                        }
                        Region { role: "inputBg" }
                    }

                    Rectangle {
                        implicitWidth: 64
                        implicitHeight: 36
                        radius: AppTheme.radiusMd
                        color: root.c("accent", "accent")
                        Text {
                            anchors.centerIn: parent
                            text: qsTr("Send")
                            color: root.c("accentText", "textPrimary")
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            font.weight: AppTheme.weightStrong
                        }
                        Region { role: "accent" }
                    }
                }
            }

        }

        // ── Member list ──────────────────────────────────────────────────
        // The fourth pane. It is the one place a plain PANEL surface is shown
        // next to the timeline ground, which is how a person tells the two
        // apart while editing them.
        Rectangle {
            Layout.preferredWidth: 168
            Layout.fillHeight: true
            color: root.c("surface", "background")

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
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                // A disabled control, so the disabled ink is visible somewhere.
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 30
                    radius: AppTheme.radiusMd
                    color: "transparent"
                    border.width: 1
                    border.color: root.c("borderStrong", "border")
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Invite")
                        color: root.c("textDisabled", "textMuted")
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                    }
                    Region { role: "textDisabled" }
                }
            }

        }
    }
}
