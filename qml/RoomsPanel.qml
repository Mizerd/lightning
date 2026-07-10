import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.5.4: Rooms column with DM / Room sections and a user footer.
// Section grouping uses RoomListModel's "category" role ("dm" | "room");
// the C++ refresh() sorts DMs before groups so section headers appear in order.
// The user footer (avatar + userId + ⚙ + ↪) replaces the old sidebar gear.
Rectangle {
    id: root
    color: AppTheme.sidebar

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Search bar ────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            color: AppTheme.sidebar
            implicitHeight: roomSearch.implicitHeight + AppTheme.spacing8 * 2

            TextField {
                id: roomSearch
                anchors {
                    left: parent.left; right: parent.right
                    verticalCenter: parent.verticalCenter
                    leftMargin: AppTheme.spacing8; rightMargin: AppTheme.spacing8
                }
                placeholderText: qsTr("Search rooms…")
                onTextChanged: app.roomList.searchQuery = text
                font.pixelSize: AppTheme.fontSizeS
                background: Rectangle {
                    color: AppTheme.inputBackground
                    border.color: roomSearch.activeFocus ? AppTheme.focusRing : AppTheme.inputBorder
                    border.width: roomSearch.activeFocus ? 2 : 1
                    radius: AppTheme.radiusSm
                }
            }
        }

        // ── Room list with DM / ROOMS section headers ─────────────────────
        ListView {
            id: roomList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: app.roomList
            currentIndex: -1
            spacing: 0

            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            // Section grouping driven by the "category" role from RoomListModel.
            // C++ sorts DMs first so "dm" section appears above "room" section.
            section.property: "category"
            section.criteria: ViewSection.FullString
            section.delegate: Rectangle {
                required property string section
                width: roomList.width
                height: 28
                color: AppTheme.sidebar

                Label {
                    anchors {
                        left: parent.left; verticalCenter: parent.verticalCenter
                        leftMargin: AppTheme.spacing12
                    }
                    text: section === "invite" ? qsTr("INVITES")
                          : section === "dm" ? qsTr("DIRECT MESSAGES") : qsTr("ROOMS")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.fontSizeXS
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.0
                }
            }

            delegate: RoomDelegate {
                width: ListView.view.width
                selected: model.roomId === app.currentRoomId
                onClicked: if (model.membership === "joined") app.openRoom(model.roomId)
                onAcceptInvite: app.roomList.acceptInvite(model.roomId)
                onRejectInvite: app.roomList.rejectInvite(model.roomId)
                onMarkRead: app.roomList.markRoomRead(model.roomId)
                onMarkUnread: app.roomList.markRoomUnread(model.roomId)

            }

            // Empty / loading state
            Label {
                anchors.centerIn: parent
                width: parent.width - AppTheme.spacing24 * 2
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                visible: roomList.count === 0
                text: {
                    if (!app.loggedIn) return qsTr("Sign in to see rooms")
                    if (!app.initialSyncDone) return qsTr("Loading rooms…")
                    if (app.spaces && app.spaces.activeSpaceId &&
                            app.spaces.activeSpaceId !== "" &&
                            app.spaces.activeSpaceId !== "@orphans")
                        return qsTr("No rooms in this Space")
                    return qsTr("No joined rooms")
                }
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.fontSizeS
            }
        }

        // ── User footer ───────────────────────────────────────────────────
        // Shows avatar + userId + Settings gear + Sign-out button.
        // Hidden when not logged in.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: footerRow.implicitHeight + AppTheme.spacing8 * 2
            color: AppTheme.sidebar
            visible: app.loggedIn

            // Top separator line
            Rectangle {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 1
                color: AppTheme.separator
            }

            RowLayout {
                id: footerRow
                anchors {
                    fill: parent
                    topMargin: AppTheme.spacing8 + 1   // +1 for separator
                    bottomMargin: AppTheme.spacing8
                    leftMargin: AppTheme.spacing8
                    rightMargin: AppTheme.spacing4
                }
                spacing: AppTheme.spacing8

                // Avatar circle — first letter of local part of the MXID
                Rectangle {
                    width: 32; height: 32
                    radius: AppTheme.radiusPill
                    color: AppTheme.accent

                    Label {
                        anchors.centerIn: parent
                        text: {
                            var uid = app.accounts ? app.accounts.activeUserId : ""
                            if (!uid || uid.length === 0) return "?"
                            var local = uid.startsWith("@") ? uid.slice(1) : uid
                            return local.length > 0 ? local[0].toUpperCase() : "?"
                        }
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        color: AppTheme.accentText
                    }
                }

                // Truncated user ID
                Label {
                    Layout.fillWidth: true
                    text: app.accounts ? app.accounts.activeUserId : ""
                    color: AppTheme.textSecondary
                    font.pixelSize: AppTheme.fontSizeS
                    elide: Label.ElideRight
                }

                // Settings gear ⚙
                ToolButton {
                    id: settingsBtn
                    implicitWidth: 30; implicitHeight: 30
                    contentItem: Label {
                        text: "⚙"
                        font.pixelSize: 16
                        color: AppTheme.textSecondary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: AppTheme.radiusSm
                        color: settingsBtn.hovered ? AppTheme.hover : "transparent"
                    }
                    onClicked: app.showSettings()
                    ToolTip { text: qsTr("Settings"); visible: parent.hovered; delay: 500 }
                }

                // Sign-out ↪
                ToolButton {
                    id: signOutBtn
                    implicitWidth: 30; implicitHeight: 30
                    contentItem: Label {
                        text: "↪"
                        font.pixelSize: 16
                        color: AppTheme.textSecondary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: AppTheme.radiusSm
                        color: signOutBtn.hovered ? AppTheme.hover : "transparent"
                    }
                    onClicked: app.auth.logout()
                    ToolTip { text: qsTr("Sign out"); visible: parent.hovered; delay: 500 }
                }
            }
        }
    }
}
