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
// Every colour is an AppTheme token. Not one hex literal: the whole point is
// that it repaints when a token changes, and `coreViewsUseTokensNotHex` bans
// stray hex in view QML anyway.
//
// The regions are laid out in the real app's order — rail, room list,
// timeline, composer — because the editor's job is to let someone point at a
// region and recolour it, which only works if the map matches the territory.
Item {
    id: root

    implicitWidth: 520
    implicitHeight: 320
    clip: true

    // Fixture rows. `state` drives which row renders selected vs hovered, so
    // both states are on screen at once with no interaction — the user is
    // editing those colours and has to see them.
    readonly property var fakeRooms: [
        { name: qsTr("Design"),    preview: qsTr("Shipped the new palette"), badge: 0, state: "normal" },
        { name: qsTr("Lightning"), preview: qsTr("Storm looks good now"),    badge: 0, state: "selected" },
        { name: qsTr("Alex"),      preview: qsTr("See you at six"),          badge: 3, state: "hovered" },
        { name: qsTr("Releases"),  preview: qsTr("v0.7.4 is out"),           badge: 0, state: "normal" }
    ]

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ── Spaces rail ──────────────────────────────────────────────────
        Rectangle {
            Layout.preferredWidth: 44
            Layout.fillHeight: true
            color: AppTheme.rail

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: AppTheme.spacing8
                anchors.bottomMargin: AppTheme.spacing8
                spacing: AppTheme.spacing6

                Repeater {
                    model: 3
                    delegate: Rectangle {
                        required property int index
                        Layout.alignment: Qt.AlignHCenter
                        implicitWidth: 24
                        implicitHeight: 24
                        radius: AppTheme.radiusSm
                        // The middle tile stands for the open Space.
                        color: index === 1 ? AppTheme.selected
                                           : AppTheme.cardElevated
                        border.width: index === 1 ? 1 : 0
                        border.color: AppTheme.accent
                    }
                }

                Item { Layout.fillHeight: true }

                // Account avatar: a circle, because people are circles in
                // this shell and rooms are rounded squares.
                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    implicitWidth: 24
                    implicitHeight: 24
                    radius: width / 2
                    color: AppTheme.accent
                }
            }
        }

        // ── Room list ────────────────────────────────────────────────────
        Rectangle {
            Layout.preferredWidth: 150
            Layout.fillHeight: true
            color: AppTheme.sidebar
            clip: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: AppTheme.spacing8
                spacing: AppTheme.spacing6

                Text {
                    text: qsTr("Lightning")
                    color: AppTheme.textPrimary
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                    font.weight: AppTheme.weightStrong
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                Text {
                    text: qsTr("Rooms")
                    color: AppTheme.sectionLabelColor
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMicro
                    Layout.fillWidth: true
                }

                Repeater {
                    model: root.fakeRooms
                    delegate: Rectangle {
                        id: fakeRoomRow
                        required property var modelData
                        readonly property bool isSelected: modelData.state === "selected"
                        Layout.fillWidth: true
                        implicitHeight: 30
                        radius: AppTheme.radiusSm
                        color: isSelected ? AppTheme.selected
                             : modelData.state === "hovered" ? AppTheme.hover
                                                             : "transparent"

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: AppTheme.spacing6
                            anchors.rightMargin: AppTheme.spacing6
                            spacing: AppTheme.spacing6

                            Rectangle {
                                implicitWidth: 18
                                implicitHeight: 18
                                radius: AppTheme.radiusSm
                                color: AppTheme.cardElevated
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Text {
                                    text: modelData.name
                                    color: fakeRoomRow.isSelected
                                           ? AppTheme.selectedText
                                           : AppTheme.textPrimary
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMicro
                                    font.weight: AppTheme.weightStrong
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Text {
                                    text: modelData.preview
                                    color: fakeRoomRow.isSelected
                                           ? AppTheme.selectedText
                                           : AppTheme.textMuted
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMicro
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }

                            Rectangle {
                                visible: modelData.badge > 0
                                implicitWidth: 16
                                implicitHeight: 14
                                radius: AppTheme.radiusPill
                                color: AppTheme.unreadBadge
                                Text {
                                    anchors.centerIn: parent
                                    text: modelData.badge
                                    color: AppTheme.accentText
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

        // ── Timeline + composer ──────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: AppTheme.background
            clip: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                // Room header.
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 28
                    color: AppTheme.surface
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: AppTheme.spacing8
                        text: qsTr("Lightning")
                        color: AppTheme.textPrimary
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                        font.weight: AppTheme.weightStrong
                    }
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: AppTheme.border
                    }
                }

                // Messages.
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.margins: AppTheme.spacing8
                    spacing: AppTheme.spacing6

                    // Incoming.
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.maximumWidth: 220
                        implicitHeight: incomingCol.implicitHeight
                                        + AppTheme.spacing6 * 2
                        radius: AppTheme.radiusMd
                        color: AppTheme.otherBubble
                        ColumnLayout {
                            id: incomingCol
                            anchors.fill: parent
                            anchors.margins: AppTheme.spacing6
                            spacing: 1
                            Text {
                                text: qsTr("Sam")
                                color: AppTheme.textMuted
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMicro
                                font.weight: AppTheme.weightStrong
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("The new ladder reads much better.")
                                color: AppTheme.textPrimary
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMicro
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                    // Outgoing.
                    Rectangle {
                        Layout.alignment: Qt.AlignRight
                        implicitWidth: Math.min(200, outgoingText.implicitWidth
                                                + AppTheme.spacing6 * 2)
                        implicitHeight: outgoingText.implicitHeight
                                        + AppTheme.spacing6 * 2
                        radius: AppTheme.radiusMd
                        color: AppTheme.ownBubble
                        Text {
                            id: outgoingText
                            anchors.fill: parent
                            anchors.margins: AppTheme.spacing6
                            text: qsTr("Agreed — shipping it.")
                            color: AppTheme.ownBubbleText
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMicro
                            wrapMode: Text.WordWrap
                        }
                    }

                    // Incoming carrying a link, a mention, a raised chip and a
                    // reaction pill — four editable roles that appear nowhere
                    // else in this preview.
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.maximumWidth: 240
                        implicitHeight: richCol.implicitHeight
                                        + AppTheme.spacing6 * 2
                        radius: AppTheme.radiusMd
                        color: AppTheme.otherBubble
                        ColumnLayout {
                            id: richCol
                            anchors.fill: parent
                            anchors.margins: AppTheme.spacing6
                            spacing: AppTheme.spacing4

                            RowLayout {
                                spacing: AppTheme.spacing4
                                Text {
                                    text: qsTr("See")
                                    color: AppTheme.textPrimary
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMicro
                                }
                                Text {
                                    text: qsTr("the notes")
                                    color: AppTheme.link
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMicro
                                    font.underline: true
                                }
                                Text {
                                    text: qsTr("@you")
                                    color: AppTheme.mentionBadge
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMicro
                                    font.weight: AppTheme.weightStrong
                                }
                            }

                            // Raised chip: the surface every embed uses.
                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 18
                                radius: AppTheme.radiusSm
                                color: AppTheme.cardElevated
                                border.width: 1
                                border.color: AppTheme.border
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.leftMargin: AppTheme.spacing4
                                    text: qsTr("Release notes")
                                    color: AppTheme.textSecondary
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMicro
                                    elide: Text.ElideRight
                                }
                            }

                            Rectangle {
                                implicitWidth: 32
                                implicitHeight: 16
                                radius: AppTheme.radiusPill
                                color: AppTheme.reactionBackground
                                border.width: 1
                                border.color: AppTheme.reactionBorder
                                Text {
                                    anchors.centerIn: parent
                                    // Not an emoji: emoji literals are banned
                                    // in row delegates here, and a count with
                                    // a glyph name shows the same colours.
                                    text: qsTr("+2")
                                    color: AppTheme.reactionInk
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMicro
                                }
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }
                }

                // Composer.
                RowLayout {
                    Layout.fillWidth: true
                    Layout.margins: AppTheme.spacing8
                    Layout.topMargin: 0
                    spacing: AppTheme.spacing6

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 26
                        radius: AppTheme.radiusMd
                        color: AppTheme.inputBackground
                        border.width: 1
                        border.color: AppTheme.border
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: AppTheme.spacing8
                            text: qsTr("Message")
                            color: AppTheme.textMuted
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMicro
                        }
                    }

                    Rectangle {
                        implicitWidth: 44
                        implicitHeight: 26
                        radius: AppTheme.radiusMd
                        color: AppTheme.accent
                        Text {
                            anchors.centerIn: parent
                            text: qsTr("Send")
                            color: AppTheme.accentText
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMicro
                            font.weight: AppTheme.weightStrong
                        }
                    }
                }
            }
        }
    }
}
