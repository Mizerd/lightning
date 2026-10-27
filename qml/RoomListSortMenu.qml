import QtQuick
import QtQuick.Controls
import MatrixClient

// The "..." menu beside the room-list search field: how each group of the
// conversation list is ordered. The groups themselves (invitations,
// favourites, People, Rooms) are not affected; only the order inside one.
//
// The mode is the persisted device setting `app.settings.roomListSort`
// (0 = Activity, 1 = A-Z, 2 = Space order, which is Activity outside a Space,
// 3 = Unread first). Until one is picked the models run unset (-1): Space order
// in a real Space, Activity elsewhere. Both layouts and the Space channel list follow it
// through AppController, so this menu only writes the setting and reads it
// back; there is no second copy of the choice here.
AppMenu {
    id: root
    objectName: "roomListSortMenu"

    menuWidth: 280

    // The values mirror conversation::SortMode (models/ConversationOrder.h).
    readonly property int sortActivity: 0
    readonly property int sortName: 1
    readonly property int sortSpace: 2
    readonly property int sortUnread: 3
    // The mode the list is ordered by NOW. AppController hands the models -1
    // while the user has never picked a sort; that is Space order inside a real
    // Space (room ids start with "!") and Activity everywhere else, and the
    // menu has to check what the list is actually doing, not the stored
    // default.
    readonly property int currentSort: {
        const mode = app.roomList ? app.roomList.sortMode
                                  : app.settings.roomListSort
        if (mode !== -1)
            return mode
        const space = app.spaces ? app.spaces.activeSpaceId : ""
        return space.length > 0 && space.charAt(0) === "!" ? root.sortSpace
                                                           : root.sortActivity
    }

    MenuSectionLabel { text: qsTr("Sort rooms by") }

    AppMenuItem {
        objectName: "roomSortActivityItem"
        radio: true
        radioSelected: root.currentSort === root.sortActivity
        text: qsTr("Activity")
        onTriggered: app.settings.roomListSort = root.sortActivity
    }
    AppMenuItem {
        objectName: "roomSortNameItem"
        radio: true
        radioSelected: root.currentSort === root.sortName
        text: qsTr("A–Z")
        onTriggered: app.settings.roomListSort = root.sortName
    }
    AppMenuItem {
        objectName: "roomSortSpaceItem"
        radio: true
        radioSelected: root.currentSort === root.sortSpace
        text: qsTr("Space order")
        onTriggered: app.settings.roomListSort = root.sortSpace
    }
    AppMenuItem {
        objectName: "roomSortUnreadItem"
        radio: true
        radioSelected: root.currentSort === root.sortUnread
        text: qsTr("Unread first")
        onTriggered: app.settings.roomListSort = root.sortUnread
    }

    MenuSectionLabel {
        visible: app.settings.roomNavigationLayout === 1
        text: qsTr("Collapsed groups")
    }
    AppMenuItem {
        objectName: "keepImportantRoomsItem"
        visible: app.settings.roomNavigationLayout === 1
        radio: true
        radioSelected: app.spaceChannels.keepImportantRoomsVisible
        text: qsTr("Keep important rooms visible")
        onTriggered: app.spaceChannels.keepImportantRoomsVisible = !app.spaceChannels.keepImportantRoomsVisible
    }
}
