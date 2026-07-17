import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.6.1: keyboard-driven quick switcher (Ctrl+K). A modal overlay over the
// already-available local room/DM/Space/invite presentation data
// (app.quickSwitcher). No network search, no persisted message text.
// Selecting a result routes through the app's normal navigation.
Popup {
    id: switcher
    parent: Overlay.overlay
    modal: true
    dim: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: Math.min(560, parent ? parent.width - 80 : 560)
    height: Math.min(440, parent ? parent.height - 120 : 440)
    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? Math.max(60, parent.height * 0.12) : 0
    padding: 0

    onOpened: {
        app.quickSwitcher.query = ""
        app.quickSwitcher.refresh()
        queryField.text = ""
        queryField.forceActiveFocus()
        resultList.currentIndex = app.quickSwitcher.count > 0 ? 0 : -1
    }
    onClosed: app.quickSwitcher.reset()

    function activate(row) {
        if (row < 0 || row >= app.quickSwitcher.count) return
        var r = app.quickSwitcher.resultAt(row)
        if (!r.roomId) return
        if (r.isSpace) {
            if (app.spaces) app.spaces.activeSpaceId = r.roomId
        } else {
            // Rooms, DMs, and invites all open the room context; an invite
            // opens its accept/decline view — never auto-accepted here.
            app.openRoom(r.roomId)
        }
        switcher.close()
    }

    background: Rectangle {
        color: AppTheme.surface
        radius: AppTheme.radius
        border.color: AppTheme.border
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: 0

        TextField {
            id: queryField
            objectName: "quickSwitcherField"
            Layout.fillWidth: true
            Layout.margins: AppTheme.spacingS
            placeholderText: qsTr("Jump to a room, person, or Space…")
            onTextChanged: {
                app.quickSwitcher.query = text
                resultList.currentIndex = app.quickSwitcher.count > 0 ? 0 : -1
            }
            Keys.onDownPressed: {
                if (app.quickSwitcher.count > 0)
                    resultList.currentIndex =
                        Math.min(resultList.currentIndex + 1,
                                 app.quickSwitcher.count - 1)
            }
            Keys.onUpPressed: {
                if (resultList.currentIndex > 0)
                    resultList.currentIndex = resultList.currentIndex - 1
            }
            Keys.onReturnPressed: switcher.activate(resultList.currentIndex)
            Keys.onEnterPressed: switcher.activate(resultList.currentIndex)
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: AppTheme.border
        }

        Label {
            visible: app.quickSwitcher.count === 0
            Layout.fillWidth: true
            Layout.margins: AppTheme.spacingM
            horizontalAlignment: Text.AlignHCenter
            text: queryField.text.length > 0
                  ? qsTr("No matching rooms")
                  : qsTr("Type to search your rooms")
            color: AppTheme.textMuted
            font.pixelSize: 13
        }

        ListView {
            id: resultList
            objectName: "quickSwitcherList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: app.quickSwitcher
            currentIndex: -1
            keyNavigationEnabled: false
            ScrollBar.vertical: ScrollBar {}

            delegate: ItemDelegate {
                width: ListView.view.width
                height: 52
                highlighted: ListView.isCurrentItem
                onClicked: switcher.activate(index)
                Accessible.name: model.name + " " + (model.subtitle || "")

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: AppTheme.spacingM
                    anchors.rightMargin: AppTheme.spacingM
                    spacing: AppTheme.spacingS

                    Avatar {
                        size: 34
                        mxc: model.avatarUrl
                        name: model.name
                        circle: model.category === "dm"
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label {
                            Layout.fillWidth: true
                            text: model.name
                            color: AppTheme.textPrimary
                            font.pixelSize: 13
                            elide: Label.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: (model.subtitle || "").length > 0
                            text: model.subtitle
                            color: AppTheme.textMuted
                            font.pixelSize: 11
                            elide: Label.ElideRight
                        }
                    }
                    // Category / state chips.
                    Label {
                        visible: model.isInvite
                        text: qsTr("Invite")
                        color: AppTheme.accent
                        font.pixelSize: 10
                    }
                    Label {
                        visible: model.isSpace
                        text: qsTr("Space")
                        color: AppTheme.textMuted
                        font.pixelSize: 10
                    }
                    Rectangle {
                        visible: model.hasUnread && !model.isSpace
                        implicitWidth: 9
                        implicitHeight: 9
                        radius: 4.5
                        color: model.highlight > 0 ? AppTheme.danger
                                                   : AppTheme.accent
                    }
                }
            }
        }
    }
}
