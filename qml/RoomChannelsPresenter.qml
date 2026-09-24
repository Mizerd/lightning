import QtQuick
import QtQuick.Controls
import MatrixClient

// The Channels layout's list body. A presenter: RoomsPanel owns the header,
// search and dialogs and swaps this in place of the Classic list. The rail
// chooses one of three views and this column renders it: Home (command rows and
// rooms in no Space), Direct Messages (Create Chat and the DMs), or one Space
// (Lobby, Message Search, its rooms and subspace folders). Each view contains
// only what belongs to it. Every colour comes from AppTheme.
Item {
    id: root

    /// The room the timeline is showing, so its row can be marked.
    property string currentRoomId: ""
    /// True while the shell shows an overview (Space Home or Home) rather than a
    /// timeline: "no room open", the same condition TimelinePane uses.
    readonly property bool lobbyActive: app.currentRoomId === ""

    signal roomActivated(string roomId)
    /// Lobby: the selected Space's overview. Navigation only; nothing persisted.
    signal lobbyActivated()
    // Command rows. The model decides which exist in which view and the host
    // what they do; this presenter dispatches on the model's id
    // (everyChannelActionIsDispatched checks all four).
    signal createRoomRequested()
    signal joinAddressRequested()
    signal exploreSpacesRequested()
    signal createChatRequested()
    /// Message Search: opens the host's global search dialog, never a filter
    /// over this list.
    signal messageSearchRequested()
    // The host owns the clipboard proxy and leave confirmation, as for the
    // Classic list.
    signal roomLinkCopyRequested(string roomId)
    signal leaveRoomRequested(string roomId, string roomName)

    // Empty state, only when the account has nothing; a filter or search that
    // matched nothing is a different message (below).
    Loader {
        anchors.centerIn: parent
        width: parent.width - AppTheme.spacing24 * 2
        active: app.spaceChannels.empty
        visible: active
        sourceComponent: Label {
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textBody
            text: qsTr("No conversations yet. Rooms you join, and the spaces " + "they belong to, will show up here.")
            // Account-wide, so the wording does not vary by view.
        }
    }

    // Filter-miss state: the account has conversations but this view shows
    // none. Says which filter or search matched nothing, so a chip does not
    // look dead.
    Loader {
        anchors.centerIn: parent
        width: parent.width - AppTheme.spacing24 * 2
        active: app.spaceChannels.matchCount === 0 && !app.spaceChannels.empty
        visible: active
        sourceComponent: Label {
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textBody
            text: {
                // The search box wins: it is the most recent input.
                if (app.spaceChannels.searchQuery.trim().length > 0)
                    return qsTr("Nothing in this list matches \"%1\".").arg(app.spaceChannels.searchQuery);
                if (app.spaceChannels.filterMode === 3)
                    return qsTr("Nothing unread. Everything in this view has been read.");
                // No filter or search: the view is genuinely empty; name the
                // view, never claim the account is empty.
                if (app.spaceChannels.viewKind === "people")
                    return qsTr("No direct messages yet. Start one with Create Chat.");
                if (app.spaceChannels.viewKind === "space")
                    return qsTr("Nothing in this space yet. Open Lobby to add a room.");
                return qsTr("No rooms outside your spaces. Pick a space on the left, or create a room.");
            }
        }
    }

    ListView {
        id: channelList
        objectName: "channelList"
        anchors.fill: parent
        clip: true
        model: app.spaceChannels
        currentIndex: -1
        spacing: 0
        // 32px rows: one extra screen is few delegates.
        cacheBuffer: 400
        // Safe to recycle: ChannelDelegate re-queries its mute mode on every id
        // change.
        reuseItems: true

        ScrollBar.vertical: AppScrollBar {
            policy: ScrollBar.AsNeeded
        }

        // No section.property: the model is already grouped.
        header: Item {
            width: channelList.width
            height: AppTheme.spacing8
        }
        footer: Item {
            width: channelList.width
            height: AppTheme.spacing12
        }

        delegate: Loader {
            id: rowLoader
            width: channelList.width
            required property var model
            required property int index

            // One Loader choosing a component per row kind (the kinds share
            // nothing). The chooser must name every kind the model produces, or
            // a row falls through to the channel-row component.
            sourceComponent: rowLoader.model.kind === "lobby" ? lobbyComponent : (rowLoader.model.kind === "search" ? searchComponent : (rowLoader.model.kind === "action" ? actionComponent : (rowLoader.model.kind === "space" ? spaceComponent : (rowLoader.model.kind === "group" ? groupComponent : channelComponent))))

            Component {
                id: lobbyComponent
                ChannelNavRow {
                    width: channelList.width
                    label: rowLoader.model.name
                    // The model names the glyph; the icon font is a subset, so
                    // there is one place to pin names.
                    iconName: rowLoader.model.iconName
                    current: root.lobbyActive
                    onClicked: root.lobbyActivated()
                }
            }

            Component {
                id: searchComponent
                ChannelNavRow {
                    width: channelList.width
                    label: rowLoader.model.name
                    iconName: rowLoader.model.iconName
                    onClicked: root.messageSearchRequested()
                }
            }

            Component {
                id: actionComponent
                ChannelNavRow {
                    width: channelList.width
                    label: rowLoader.model.name
                    iconName: rowLoader.model.iconName
                    // Dispatch on the model's id; every id is named here.
                    onClicked: {
                        var id = rowLoader.model.roomId
                        if (id === "@new-room")
                            root.createRoomRequested()
                        else if (id === "@join-address")
                            root.joinAddressRequested()
                        else if (id === "@explore")
                            root.exploreSpacesRequested()
                        else if (id === "@new-chat")
                            root.createChatRequested()
                    }
                }
            }

            Component {
                id: channelComponent
                ChannelDelegate {
                    width: channelList.width
                    roomId: rowLoader.model.roomId
                    channelName: rowLoader.model.name
                    avatarUrl: rowLoader.model.avatarUrl
                    identityColorKey: rowLoader.model.identityColorKey
                    isDirect: rowLoader.model.isDirect
                    isInvite: rowLoader.model.isInvite
                    encrypted: rowLoader.model.encrypted
                    unreadCount: rowLoader.model.unreadCount
                    highlightCount: rowLoader.model.highlightCount
                    hasUnread: rowLoader.model.hasUnread
                    isFavourite: rowLoader.model.isFavourite
                    depth: rowLoader.model.depth
                    active: rowLoader.model.roomId === root.currentRoomId
                    onClicked: root.roomActivated(rowLoader.model.roomId)
                    // The same mutations as the Classic host.
                    onMarkRead: app.roomList.markRoomRead(rowLoader.model.roomId)
                    onMarkUnread: app.roomList.markRoomUnread(rowLoader.model.roomId)
                    onSetFavourite: on => app.roomList.setRoomFavourite(rowLoader.model.roomId, on)
                    onSetNotificationMode: mode => app.setRoomNotificationMode(rowLoader.model.roomId, mode)
                    onCopyRoomLink: root.roomLinkCopyRequested(rowLoader.model.roomId)
                    onLeaveRoomRequested: root.leaveRoomRequested(rowLoader.model.roomId, rowLoader.model.name)
                }
            }

            Component {
                id: groupComponent
                ChannelCategoryHeader {
                    width: channelList.width
                    headerId: rowLoader.model.roomId
                    headerName: rowLoader.model.name
                    showsAvatar: false
                    collapsed: rowLoader.model.collapsed
                    hiddenUnread: rowLoader.model.hiddenUnread
                    hiddenHighlight: rowLoader.model.hiddenHighlight
                    onClicked: app.spaceChannels.toggleCollapsed(rowLoader.model.roomId)
                }
            }

            Component {
                id: spaceComponent
                ChannelCategoryHeader {
                    width: channelList.width
                    headerId: rowLoader.model.roomId
                    headerName: rowLoader.model.name
                    avatarUrl: rowLoader.model.avatarUrl
                    identityColorKey: rowLoader.model.identityColorKey
                    showsAvatar: true
                    collapsed: rowLoader.model.collapsed
                    hiddenUnread: rowLoader.model.hiddenUnread
                    hiddenHighlight: rowLoader.model.hiddenHighlight
                    // The primary action collapses, so tidying the column never
                    // changes rooms.
                    onClicked: app.spaceChannels.toggleCollapsed(rowLoader.model.roomId)
                    // Opening the Space is the secondary action, on its own
                    // gesture.
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: app.openSpaceHome(rowLoader.model.roomId)
                    }
                }
            }
        }
    }
}
