import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Dialog {
    id: root
    objectName: "savedMessagesDialog"
    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    width: Math.min(620, parent ? parent.width - AppTheme.spacing24 * 2 : 620)
    height: Math.min(640, parent ? parent.height - AppTheme.spacing24 * 2 : 640)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    readonly property var saved: app.savedMessages
    function openPanel() {
        root.open()
    }
    onOpened: {
        if (saved) saved.resolveVisible()
        searchField.forceActiveFocus(Qt.PopupFocusReason)
    }
    Connections {
        target: root.saved
        function onChanged() {
            if (root.opened && root.saved)
                Qt.callLater(function() {
                    if (root.opened && root.saved)
                        root.saved.resolveVisible()
                })
        }
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
            Label {
                text: qsTr("Saved messages")
                color: AppTheme.stormText
                font.family: AppTheme.menuFont
                font.pixelSize: AppTheme.textTitle
                font.weight: AppTheme.weightBold
            }
            Item { Layout.fillWidth: true }
            Label {
                text: root.saved ? String(root.saved.count) : ""
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
            }
            AppButton {
                objectName: "undoSavedMessageRemoval"
                storm: true
                kind: "ghost"
                size: "sm"
                text: qsTr("Undo remove")
                visible: root.saved && root.saved.undoAvailable
                onClicked: root.saved.undoRemove()
            }
            IconButton {
                storm: true
                iconName: "close"
                size: "sm"
                Accessible.name: qsTr("Close")
                onClicked: root.close()
            }
        }
        TextField {
            id: searchField
            objectName: "savedMessagesSearch"
            Layout.fillWidth: true
            placeholderText: qsTr("Filter saved messages")
            text: root.saved ? root.saved.query : ""
            onTextEdited: if (root.saved) root.saved.query = text
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            ComboBox {
                objectName: "savedMessagesRoomFilter"
                Layout.fillWidth: true
                textRole: "name"
                valueRole: "roomId"
                model: [{ roomId: "", name: qsTr("All rooms") }]
                       .concat(root.saved ? root.saved.rooms : [])
                currentIndex: {
                    var selected = root.saved ? root.saved.roomFilter : ""
                    for (var i = 0; i < count; ++i)
                        if (valueAt(i) === selected) return i
                    return 0
                }
                onActivated: if (root.saved)
                    root.saved.roomFilter = currentValue
            }
            SegmentedControl {
                objectName: "savedMessagesTypeFilter"
                storm: true
                dense: true
                model: [
                    { value: "", label: qsTr("All") },
                    { value: "text", label: qsTr("Text") },
                    { value: "media", label: qsTr("Media") }
                ]
                current: root.saved ? root.saved.typeFilter : ""
                onActivated: (value) => {
                    if (root.saved) root.saved.typeFilter = value
                }
            }
        }
        Label {
            objectName: "savedMessagesError"
            visible: root.saved && root.saved.error !== ""
            text: root.saved ? root.saved.error : ""
            color: AppTheme.danger
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        AppButton {
            visible: root.saved && root.saved.supported
                     && !root.saved.loaded && root.saved.error !== ""
            storm: true
            kind: "secondary"
            size: "sm"
            text: qsTr("Retry loading")
            onClicked: root.saved.refresh()
        }
        Label {
            visible: root.saved && root.saved.supported
                     && !root.saved.loaded && root.saved.error === ""
            text: qsTr("Loading saved messages…")
            color: AppTheme.stormTextMuted
        }
        Label {
            visible: root.saved && !root.saved.supported
            text: qsTr("Saved messages are unavailable on this backend.")
            color: AppTheme.stormTextMuted
            wrapMode: Text.WordWrap
        }
        Label {
            visible: root.saved && root.saved.loaded && savedList.count === 0
            Layout.fillWidth: true
            Layout.topMargin: AppTheme.spacing24
            horizontalAlignment: Text.AlignHCenter
            color: AppTheme.stormTextMuted
            wrapMode: Text.WordWrap
            text: root.saved && root.saved.count > 0
                  ? qsTr("No saved messages match these filters.")
                  : qsTr("No saved messages yet. Use Save message on a message to add one.")
        }
        ListView {
            id: savedList
            objectName: "savedMessagesList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: AppTheme.spacing4
            model: root.saved ? root.saved.entries : []
            activeFocusOnTab: true
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Saved messages")
            Keys.onReturnPressed: openCurrent()
            Keys.onEnterPressed: openCurrent()
            function openCurrent() {
                if (currentItem) currentItem.openEvent()
            }
            ScrollBar.vertical: ScrollBar { }
            delegate: Rectangle {
                id: savedRow
                required property int index
                required property var modelData
                width: savedList.width
                implicitHeight: savedContent.implicitHeight + AppTheme.spacing12 * 2
                radius: AppTheme.radiusTile
                color: AppTheme.stormInset
                border.width: savedRow.ListView.isCurrentItem && savedList.activeFocus ? 2 : 1
                border.color: AppTheme.stormBorder
                function openEvent() {
                    if (!root.saved) return
                    savedList.currentIndex = index
                    root.saved.open(modelData.roomId, modelData.eventId)
                    root.close()
                }
                Accessible.role: Accessible.Grouping
                Accessible.name: modelData.roomName + ", "
                    + (modelData.available ? modelData.preview
                       : modelData.resolved ? qsTr("Message unavailable or redacted")
                                            : qsTr("Loading message"))
                RowLayout {
                    id: savedContent
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing8
                    Icon { name: "bookmark"; size: 18; color: AppTheme.stormText }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing2
                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: modelData.roomName
                                textFormat: Text.PlainText
                                color: AppTheme.stormText
                                font.weight: AppTheme.weightStrong
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }
                            Label {
                                text: Qt.locale(AppLocale.name).toString(
                                    new Date(modelData.savedAtMs), Locale.ShortFormat)
                                textFormat: Text.PlainText
                                color: AppTheme.stormTextMuted
                                font.pixelSize: AppTheme.textMeta
                            }
                        }
                        Label {
                            text: modelData.available
                                  ? (modelData.senderName
                                      ? modelData.senderName + ": " : "")
                                    + modelData.preview
                                  : modelData.resolved
                                    ? qsTr("Message unavailable or redacted")
                                    : qsTr("Loading message…")
                            textFormat: Text.PlainText
                            color: AppTheme.stormTextMuted
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            maximumLineCount: 2
                        }
                        Label {
                            visible: modelData.threadRootId !== ""
                            text: qsTr("In a thread")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textMeta
                        }
                    }
                    AppButton {
                        storm: true
                        kind: "ghost"
                        size: "sm"
                        text: qsTr("Open")
                        Accessible.name: qsTr("Open saved message")
                        onClicked: savedRow.openEvent()
                    }
                    IconButton {
                        storm: true
                        iconName: "forum"
                        size: "sm"
                        visible: modelData.threadRootId !== ""
                        Accessible.name: qsTr("Open thread root in room")
                        ToolTip.text: qsTr("Open thread root in room")
                        ToolTip.visible: hovered
                        onClicked: {
                            root.saved.openRoot(modelData.roomId, modelData.eventId)
                            root.close()
                        }
                    }
                    IconButton {
                        storm: true
                        iconName: "home"
                        size: "sm"
                        Accessible.name: qsTr("Open room")
                        ToolTip.text: qsTr("Open room")
                        ToolTip.visible: hovered
                        onClicked: {
                            root.saved.openRoom(modelData.roomId)
                            root.close()
                        }
                    }
                    IconButton {
                        storm: true
                        iconName: "close"
                        size: "sm"
                        enabled: root.saved && !root.saved.busy
                        Accessible.name: qsTr("Remove saved message")
                        onClicked: root.saved.remove(modelData.roomId, modelData.eventId)
                    }
                }
            }
        }
    }
}
