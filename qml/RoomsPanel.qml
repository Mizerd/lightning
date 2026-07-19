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

        // ── Search bar + new-conversation button ─────────────────────────
        Rectangle {
            Layout.fillWidth: true
            color: AppTheme.sidebar
            implicitHeight: searchRow.implicitHeight + AppTheme.spacing8 * 2

            RowLayout {
                id: searchRow
                anchors {
                    left: parent.left; right: parent.right
                    verticalCenter: parent.verticalCenter
                    leftMargin: AppTheme.spacing8; rightMargin: AppTheme.spacing8
                }
                spacing: AppTheme.spacing8

                TextField {
                    id: roomSearch
                    Layout.fillWidth: true
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

                // v0.5.9: start a DM or create a room (Rust backend only —
                // the controller reports unsupported backends itself).
                ToolButton {
                    id: newConversationBtn
                    visible: app.loggedIn && app.conversations.supported
                    implicitWidth: 30; implicitHeight: 30
                    contentItem: Label {
                        text: "＋"
                        font.pixelSize: 18
                        color: AppTheme.textSecondary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: AppTheme.radiusSm
                        color: newConversationBtn.hovered ? AppTheme.hover : "transparent"
                    }
                    Accessible.name: qsTr("Start a new conversation")
                    ToolTip.text: qsTr("New conversation")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: newConversationDialog.openDialog()
                }
            }
        }

        NewConversationDialog {
            id: newConversationDialog
            parent: Overlay.overlay
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
            // Instantiate delegates a little past the viewport so their
            // avatars start fetching before the row scrolls into view.
            // Bounded prefetch: roughly one extra screen of rows.
            cacheBuffer: 600

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

        // ── Account footer ───────────────────────────────────────────────
        // v0.5.9: one compact account button (avatar, clean name,
        // abbreviated MXID, connection dot). Clicking opens the account
        // menu — Settings, Security & Recovery, About, and the only Sign
        // out in the app (danger-styled, confirmed). Hidden when signed out.
        Rectangle {
            id: accountFooter
            Layout.fillWidth: true
            implicitHeight: footerRow.implicitHeight + AppTheme.spacing8 * 2
            color: footerHover.hovered ? AppTheme.hover : AppTheme.sidebar
            visible: app.loggedIn
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Account menu for %1")
                             .arg(app.accounts ? app.accounts.activeUserId : "")

            readonly property string userId:
                app.accounts ? (app.accounts.activeUserId || "") : ""
            readonly property string localpart: {
                var uid = userId
                if (uid.startsWith("@")) uid = uid.slice(1)
                var colon = uid.indexOf(":")
                return colon > 0 ? uid.slice(0, colon) : uid
            }
            readonly property string server: {
                var colon = userId.indexOf(":")
                return colon > 0 ? userId.slice(colon + 1) : ""
            }

            // Top separator line
            Rectangle {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 1
                color: AppTheme.separator
            }

            HoverHandler { id: footerHover }
            TapHandler { onTapped: accountMenu.open() }

            RowLayout {
                id: footerRow
                anchors {
                    fill: parent
                    topMargin: AppTheme.spacing8 + 1   // +1 for separator
                    bottomMargin: AppTheme.spacing8
                    leftMargin: AppTheme.spacing8
                    rightMargin: AppTheme.spacing8
                }
                spacing: AppTheme.spacing8

                // Avatar circle with connection indicator.
                Item {
                    width: 32; height: 32
                    Rectangle {
                        anchors.fill: parent
                        radius: AppTheme.radiusPill
                        color: AppTheme.accent
                        Label {
                            anchors.centerIn: parent
                            text: accountFooter.localpart.length > 0
                                  ? accountFooter.localpart[0].toUpperCase() : "?"
                            font.pixelSize: AppTheme.fontBody
                            font.weight: Font.DemiBold
                            color: AppTheme.accentText
                        }
                    }
                    Rectangle {
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        width: 10; height: 10; radius: 5
                        border.color: AppTheme.sidebar
                        border.width: 2
                        color: app.connectionStatus === qsTr("Connected")
                               ? AppTheme.success : AppTheme.warning
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Label {
                        Layout.fillWidth: true
                        text: accountFooter.localpart
                        color: AppTheme.textPrimary
                        font.pixelSize: AppTheme.fontSecondary
                        font.weight: Font.DemiBold
                        elide: Label.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        text: accountFooter.server
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.fontCaption
                        elide: Label.ElideRight
                    }
                }

                Label {
                    text: "⋯"
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.fontRoomTitle
                }
            }

            AccountMenu {
                id: accountMenu
                // Anchor above the footer, inside the window.
                x: 0
                y: -implicitHeight - AppTheme.spacing4
            }
        }
    }
}
