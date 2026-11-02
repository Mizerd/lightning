import QtQuick
import QtQuick.Controls
import MatrixClient

AppMenu {
    id: root
    objectName: "roomListSortMenu"

    menuWidth: 280

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
    Label {
        objectName: "roomOrderExplanation"
        width: root.menuWidth - 24
        padding: 8
        wrapMode: Text.WordWrap
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        text: app.settings.roomNavigationLayout === 1
            ? qsTr("Rooms stay in their groups. Within each group: current call, favourites, other rooms, then low priority.")
            : qsTr("Current call first, then invitations, favourites, other conversations, and low priority.")
    }
    Label {
        objectName: "roomSortExplanation"
        width: root.menuWidth - 24
        padding: 8
        wrapMode: Text.WordWrap
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        text: root.currentSort === root.sortName
            ? qsTr("Within each section: alphabetical order.")
            : root.currentSort === root.sortUnread
              ? qsTr("Within each section: unread rooms first, then newest activity.")
              : root.currentSort === root.sortSpace && app.spaces.activeSpaceId.charAt(0) === "!"
                ? qsTr("Within each section: your room arrangement, then the space's default order.")
                : qsTr("Within each section: newest activity first. Unread status does not change the order.")
    }

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
