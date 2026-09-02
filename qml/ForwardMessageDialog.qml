import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.7.x message forwarding: the ONE room picker (opens itself off
// ForwardController's `active` state, exactly like ReportMessageDialog
// opens off ModerationController's pending-report state — see
//). Styled on DiscoverJoinDialog's Storm shape.
//
// Scope: rooms only, never a thread — Lightning has no
// "forward into this thread" concept, and offering thread targets here
// would need its own root-selection UI. Spaces are excluded: a Space is
// not a chat target. Only ONE message forwards at a time; there is no
// multi-select.
Dialog {
    id: root
    objectName: "forwardMessageDialog"
    modal: true
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    width: Math.min(440, parent ? parent.width - AppTheme.spacing24 * 2 : 440)
    height: Math.min(520, parent ? parent.height - AppTheme.spacing24 * 2 : 520)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    property string searchText: ""

    readonly property bool active: app.forward.active
    onActiveChanged: {
        if (active) {
            root.searchText = ""
            open()
            Qt.callLater(function() { searchField.forceActiveFocus() })
        } else if (opened) {
            close()
        }
    }
    onClosed: {
        root.searchText = ""
        if (app.forward.active)
            app.forward.cancel()
    }

    background: Rectangle {
        radius: AppTheme.radiusLg
        color: AppTheme.stormPanel
        border.color: AppTheme.stormBorder
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Label {
                text: qsTr("Forward message")
                color: AppTheme.stormText
                font.family: AppTheme.menuFont
                font.pixelSize: AppTheme.textTitle
                font.weight: AppTheme.weightBold
                Layout.fillWidth: true
            }
            IconButton {
                // 28/18 is the dialog close-button size used by every other
                // dialog in this family; IconButton's own 34/21 default made
                // this one visibly larger than the identical control in the
                // dialog opened right beside it.
                storm: true
                iconName: "close"
                iconSize: 18
                implicitWidth: 28; implicitHeight: 28
                Accessible.name: qsTr("Close")
                onClicked: root.close()
            }
        }

        Label {
            // Remote or externally chosen text: never markup.
            textFormat: Text.PlainText
            objectName: "forwardPreviewLabel"
            Layout.fillWidth: true
            text: app.forward.previewText
            color: AppTheme.stormTextSecondary
            font.pixelSize: AppTheme.textMeta
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.Wrap
            elide: Text.ElideRight
            maximumLineCount: 2
            Accessible.name: qsTr("Forwarding: %1").arg(text)
        }

        Label {
            objectName: "forwardErrorLabel"
            Layout.fillWidth: true
            visible: app.forward.error.length > 0
            text: app.forward.error
            color: AppTheme.danger
            font.pixelSize: AppTheme.textMeta
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.Wrap
            Accessible.name: text
        }

        AppTextField {
            id: searchField
            objectName: "forwardRoomSearchField"
            Layout.fillWidth: true
            storm: true
            searchIcon: true
            clearButton: true
            enabled: !app.forward.busy
            placeholderText: qsTr("Search rooms…")
            text: root.searchText
            onTextChanged: root.searchText = text
            Accessible.name: qsTr("Search rooms")
        }

        ListView {
            id: targetList
            objectName: "forwardTargetRoomList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: AppTheme.spacing4
            model: app.roomList
            ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                id: targetRow
                required property string roomId
                required property string name
                required property string avatarUrl
                required property bool encrypted
                required property bool isSpace
                required property string membership

                // Rooms only (never a Space or a thread — see class
                // comment) and only rooms already joined: an invite or a
                // knock is not somewhere a message can land.
                readonly property bool eligible:
                    !isSpace && membership === "joined"
                readonly property string visibleName:
                    name.length > 0 ? name : roomId
                readonly property bool matchesSearch:
                    root.searchText.trim().length === 0
                    || visibleName.toLowerCase().indexOf(
                           root.searchText.trim().toLowerCase()) !== -1

                visible: eligible && matchesSearch
                width: targetList.width
                height: visible ? 56 : 0
                radius: AppTheme.radiusMd
                color: rowHover.hovered ? AppTheme.stormSelection
                                        : "transparent"
                HoverHandler { id: rowHover; enabled: !app.forward.busy }

                Accessible.role: Accessible.ListItem
                Accessible.name: targetRow.visibleName
                Accessible.onPressAction: forwardButton.clicked()

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacing8
                    spacing: AppTheme.spacing8

                    Avatar {
                        mxc: targetRow.avatarUrl
                        name: targetRow.visibleName
                        size: 40
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            // Remote or externally chosen text: never markup.
                            textFormat: Text.PlainText
                            text: targetRow.visibleName
                            color: AppTheme.stormText
                            font.family: AppTheme.menuFont
                            font.pixelSize: AppTheme.textBody
                            font.weight: AppTheme.weightStrong
                            elide: Label.ElideRight
                            Layout.fillWidth: true
                        }
                        // Encryption is a positive state, and it was the only
                        // thing on this row with nothing to say it: a bare
                        // 10px grey word. A lock in the success ink reads at
                        // a glance and matches the room header's own lock.
                        RowLayout {
                            visible: targetRow.encrypted
                            spacing: AppTheme.spacing4
                            Icon {
                                name: "lock"
                                size: 12
                                color: AppTheme.stormSuccess
                            }
                            Label {
                                text: qsTr("Encrypted")
                                color: AppTheme.stormTextMuted
                                font.pixelSize: AppTheme.textMeta
                            }
                        }
                    }
                    AppButton {
                        id: forwardButton
                        objectName: "forwardToRoomButton"
                        storm: true
                        kind: "primary"
                        enabled: !app.forward.busy
                        text: qsTr("Send")
                        Accessible.name: qsTr("Forward to %1")
                                        .arg(targetRow.visibleName)
                        onClicked: app.forward.forwardTo(targetRow.roomId)
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: targetList.count === 0
                text: qsTr("No rooms")
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textBody
            }
        }
    }

    Connections {
        target: app.forward
        function onForwarded() { root.close() }
    }
}
