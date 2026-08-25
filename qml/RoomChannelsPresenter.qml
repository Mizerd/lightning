import QtQuick
import QtQuick.Controls
import MatrixClient

// The Channels navigation layout's list body.
//
// A PRESENTER: it owns no header, no search field and no dialogs — RoomsPanel
// (the host) owns those and swaps this in place of the Classic list. The split
// exists so the two layouts cannot fork the surrounding chrome; every user of
// either layout gets the same workspace header and the same ⌘K hint.
//
// Sable's model, as directed: a Lobby entry, a Message Search entry, a group
// for the rooms that belong to no Space, then EVERY joined Space as a flat
// collapsible folder of its rooms. No Discord, Sable or Cinny code, asset,
// sound, trademark or wording is used; every colour comes from AppTheme.
//
// What changed from the first version of this layout, and why it is not a
// tweak: it used to show the ACTIVE Space's hierarchy, with child Spaces as
// nested categories, and the host silently fell back to Classic at Home
// because there was nothing to show. A navigation layout that becomes the
// other layout depending on where you are is not a navigation layout.
//
// So the layout always exists, and the rail's selection NARROWS it instead of
// deciding whether it works: pick a Space and the column becomes that Space and
// its subspaces (still flat folders, never nested); Lobby clears the selection
// and the column is the whole account again. Both states are this layout, and
// Lobby is always one row away from either.
Item {
    id: root

    /// The room the timeline is showing, so the open room's row can be marked.
    property string currentRoomId: ""
    /// True while the shell is on the home surface, which is what Lobby marks.
    ///
    /// The same condition TimelinePane uses to choose HomePane over a
    /// timeline, so the two cannot disagree about where the user is. Computed
    /// here rather than passed in because it is a fact about the shell, not
    /// about this column, and the host would only be forwarding it.
    readonly property bool lobbyActive:
        app.currentRoomId === ""
        && !(app.spaces && app.spaces.activeSpaceId.length > 0
             && app.spaces.activeSpaceId.charAt(0) === "!")

    signal roomActivated(string roomId)
    /// Lobby: the home / all-conversations surface. Navigation only — there is
    /// no Matrix room behind it and nothing is persisted for it.
    signal lobbyActivated()
    /// Message Search: the existing global server-side search dialog, which
    /// the host owns. Never a filter over this list — Sable's row opens a
    /// search experience, and so does this one.
    signal messageSearchRequested()
    // The host owns the clipboard proxy and the leave-confirm dialog, exactly
    // as it does for the Classic list — a presenter that reached up into the
    // host by id is how the reader popover's click ended up silently dead.
    signal roomLinkCopyRequested(string roomId)
    signal leaveRoomRequested(string roomId, string roomName)

    // Empty state. Only when the ACCOUNT has nothing — a filter or a search
    // that matched nothing is a fact about the filter, and saying the first
    // when the second is true sends the user looking for a problem that is not
    // there.
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
        // Rows are 32px, so one extra screen is a much smaller number of
        // delegates than the Classic list needs.
        cacheBuffer: 400
        // Recycling is safe: ChannelDelegate keeps no per-instance state that
        // outlives its roomId, and it re-queries the mute mode on every id
        // change precisely so a recycled row cannot inherit one.
        reuseItems: true

        ScrollBar.vertical: AppScrollBar {
            policy: ScrollBar.AsNeeded
        }

        // No section.property. The MODEL is already ordered and grouped, and a
        // ListView section header on top of the folder rows would draw the
        // same grouping twice.
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

            // One Loader choosing between components, rather than one delegate
            // with everything in it behind visibility flags: the row kinds
            // share no geometry and no controls, and a combined delegate would
            // instantiate all of them for every row.
            //
            // The chooser must name EVERY kind the model can produce. It once
            // named two of three, so a group label fell through to the
            // channel-row component and rendered as a room row with an empty
            // room id — clickable-looking, opening nothing, and carrying a
            // room's context menu over a heading.
            sourceComponent: rowLoader.model.kind === "lobby" ? lobbyComponent : (rowLoader.model.kind === "search" ? searchComponent : (rowLoader.model.kind === "space" ? spaceComponent : (rowLoader.model.kind === "group" ? groupComponent : channelComponent)))

            Component {
                id: lobbyComponent
                ChannelNavRow {
                    width: channelList.width
                    label: rowLoader.model.name
                    iconName: "home"
                    current: root.lobbyActive
                    onClicked: root.lobbyActivated()
                }
            }

            Component {
                id: searchComponent
                ChannelNavRow {
                    width: channelList.width
                    label: rowLoader.model.name
                    iconName: "search"
                    onClicked: root.messageSearchRequested()
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
                    // The same mutations the Classic host performs, so the
                    // two layouts cannot disagree about what a menu row does.
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
                    // The primary action is COLLAPSE, not "open this space".
                    // A folder header that navigated on click would make every
                    // attempt to tidy the column also change rooms.
                    onClicked: app.spaceChannels.toggleCollapsed(rowLoader.model.roomId)
                    // Opening the Space itself is the secondary action, on its
                    // own gesture.
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: app.openSpaceHome(rowLoader.model.roomId)
                    }
                }
            }
        }
    }
}
