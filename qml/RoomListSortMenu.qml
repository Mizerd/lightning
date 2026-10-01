import QtQuick
import QtQuick.Controls
import MatrixClient

// The "..." menu beside the room-list search field: how each group of the
// conversation list is ordered. The groups themselves (invitations,
// favourites, People, Rooms) are not affected; only the order inside one.
//
// The mode is the persisted device setting `app.settings.roomListSort`
// (0 = Activity, 1 = A-Z). Both layouts and the Space channel list follow it
// through AppController, so this menu only writes the setting and reads it
// back; there is no second copy of the choice here.
AppMenu {
    id: root
    objectName: "roomListSortMenu"

    menuWidth: 200

    // The two values mirror conversation::SortMode (models/ConversationOrder.h).
    readonly property int sortActivity: 0
    readonly property int sortName: 1
    readonly property int currentSort: app.settings.roomListSort

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
}
