import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.5.3: left sidebar — Rooms section.
// Sits below SpacesPanel (or fills the sidebar when there are no Spaces).
// Has its own search bar and independent ScrollView. Room filtering is
// done in-delegate: delegates shrink to height 0 when the name does not
// match, so the backend model is never mutated by the search.
// Existing openRoom / currentRoomId / encrypted lock / avatar / unread
// behaviour is preserved unchanged.
Rectangle {
    id: root
    color: AppTheme.sidebar

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Header ───────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            color: AppTheme.sidebar
            implicitHeight: headerRow.implicitHeight + AppTheme.spacing8 * 2

            RowLayout {
                id: headerRow
                anchors.fill: parent
                anchors.leftMargin: AppTheme.spacing12
                anchors.rightMargin: AppTheme.spacing8
                anchors.topMargin: AppTheme.spacing8
                anchors.bottomMargin: AppTheme.spacing8
                spacing: AppTheme.spacing4

                Label {
                    text: qsTr("ROOMS")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.fontSizeXS
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.0
                }

                Item { Layout.fillWidth: true }

                // Show room count once the initial sync has landed.
                Label {
                    text: app.initialSyncDone ? roomList.count.toString() : ""
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.fontSizeXS
                }
            }
        }

        // ── Search ───────────────────────────────────────────────────────
        TextField {
            id: roomSearch
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing8
            Layout.rightMargin: AppTheme.spacing8
            Layout.bottomMargin: AppTheme.spacing4
            placeholderText: qsTr("Search rooms")
            font.pixelSize: AppTheme.fontSizeS
            background: Rectangle {
                color: AppTheme.inputBackground
                border.color: roomSearch.activeFocus ? AppTheme.focusRing
                                                     : AppTheme.inputBorder
                border.width: roomSearch.activeFocus ? 2 : 1
                radius: AppTheme.radiusSm
            }
        }

        // ── Rooms ListView ────────────────────────────────────────────────
        ListView {
            id: roomList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: app.roomList
            currentIndex: -1
            spacing: 0

            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            // Use RoomDelegate directly as the delegate. Two extra properties
            // are added to each instance: `visible` (filtered) and `height`
            // (collapses to 0 when filtered out). QML allows extending
            // component instances inline; `model.*` roles are still accessible
            // from the ListView context throughout the delegate tree.
            delegate: RoomDelegate {
                id: roomItem
                width: ListView.view.width
                selected: model.roomId === app.currentRoomId
                onClicked: app.openRoom(model.roomId)

                // Case-insensitive name filter. Empty search shows all rooms.
                visible: roomSearch.text === "" ||
                         (model.name &&
                          model.name.toLowerCase().indexOf(
                              roomSearch.text.toLowerCase()) >= 0)

                // Collapse vertical space when filtered out so ListView
                // layout is continuous. `implicitHeight` is RoomDelegate's
                // computed height (content + spacing); we keep it as-is when
                // visible and override to 0 when hidden.
                height: visible ? implicitHeight : 0
            }

            // ── Empty / loading state ─────────────────────────────────────
            Label {
                anchors.centerIn: parent
                width: parent.width - AppTheme.spacing24 * 2
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                visible: roomList.count === 0
                text: {
                    if (!app.loggedIn)
                        return qsTr("Sign in to see rooms")
                    if (!app.initialSyncDone)
                        return qsTr("Loading rooms…")
                    if (app.spaces && app.spaces.activeSpaceId
                            && app.spaces.activeSpaceId !== ""
                            && app.spaces.activeSpaceId !== "@orphans")
                        return qsTr("No rooms in this Space")
                    return qsTr("No joined rooms")
                }
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.fontSizeS
            }
        }
    }
}
