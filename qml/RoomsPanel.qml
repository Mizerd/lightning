import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.7 design shell: the room-list column. Workspace header (active Space
// name), search with a ⌘K hint, DM / Room sections, room rows. The account
// entry point lives on the SpacesRail; this column has no footer.
Rectangle {
    id: root
    color: AppTheme.sidebar

    // v0.7.1: entry point for the Home surface's create actions, routed here
    // by MainScreen so they reuse this column's shared new-conversation
    // dialog. mode is "dm" or "room".
    function startConversation(mode) {
        newConversationDialog.openDialog(mode)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Workspace header ─────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            color: AppTheme.sidebar
            implicitHeight: workspaceLabel.implicitHeight + AppTheme.spacing12 * 2

            Label {
                id: workspaceLabel
                anchors {
                    left: parent.left; right: parent.right
                    verticalCenter: parent.verticalCenter
                    leftMargin: AppTheme.spacing12
                    rightMargin: AppTheme.spacing12
                }
                text: {
                    if (!app.spaces)
                        return qsTr("Lightning")
                    var id = app.spaces.activeSpaceId
                    if (id === "" || id === undefined)
                        return qsTr("Lightning")
                    if (id === "@orphans")
                        return qsTr("Other rooms")
                    return app.spaces.spaceName(id) || qsTr("Lightning")
                }
                color: AppTheme.textPrimary
                font.pixelSize: AppTheme.fontRoomTitle
                font.weight: Font.ExtraBold
                elide: Label.ElideRight
            }
        }

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
                    placeholderText: qsTr("Search")
                    onTextChanged: app.roomList.searchQuery = text
                    font.pixelSize: AppTheme.fontSizeS
                    leftPadding: searchIcon.width + AppTheme.spacing12
                    rightPadding: kbdHint.width + AppTheme.spacing12
                    background: Rectangle {
                        color: AppTheme.hover
                        border.color: roomSearch.activeFocus ? AppTheme.focusRing : "transparent"
                        border.width: roomSearch.activeFocus ? 2 : 1
                        radius: AppTheme.radiusMd
                    }

                    // Leading search glyph (handoff §2 search field).
                    Icon {
                        id: searchIcon
                        anchors.left: parent.left
                        anchors.leftMargin: AppTheme.spacing8
                        anchors.verticalCenter: parent.verticalCenter
                        name: "search"
                        size: 18
                        color: AppTheme.textMuted
                    }

                    // ⌘K-style keycap hint for the quick switcher.
                    Rectangle {
                        id: kbdHint
                        anchors.right: parent.right
                        anchors.rightMargin: AppTheme.spacing6
                        anchors.verticalCenter: parent.verticalCenter
                        width: kbdLabel.implicitWidth + AppTheme.spacing8
                        height: kbdLabel.implicitHeight + AppTheme.spacing4
                        radius: AppTheme.radiusSm
                        color: AppTheme.sidebar
                        border.color: AppTheme.border
                        visible: !roomSearch.activeFocus
                        Label {
                            id: kbdLabel
                            anchors.centerIn: parent
                            text: qsTr("Ctrl K")
                            font.family: AppTheme.monoFont
                            font.pixelSize: AppTheme.fontCaption
                            color: AppTheme.textMuted
                        }
                    }
                }

                // v0.5.9: start a DM or create a room (Rust backend only —
                // the controller reports unsupported backends itself).
                IconButton {
                    id: newConversationBtn
                    visible: app.loggedIn && app.conversations.supported
                    implicitWidth: 30; implicitHeight: 30
                    radius: AppTheme.radiusMd
                    iconName: "add"
                    iconSize: 18
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
            // Fast-scroll: recycle row delegates instead of re-creating
            // them (RoomDelegate keeps no per-instance state that could
            // leak across model rows).
            reuseItems: true

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
                          : section === "dm" ? qsTr("PEOPLE") : qsTr("ROOMS")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.fontSizeXS
                    font.weight: Font.ExtraBold
                    font.letterSpacing: 1.2
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

        // The account entry point lives on the SpacesRail (design shell);
        // this column intentionally has no footer.
    }
}
