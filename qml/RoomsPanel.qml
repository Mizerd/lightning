import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: root
    color: AppTheme.roomList
    readonly property bool compact: width < app.settings.roomListExpandedMinWidth
    function focusNavigation() {
        if (compact)
            compactRoomSearch.forceActiveFocus()
        else
            roomSearch.forceActiveFocus()
    }
    onCompactChanged: if (compact && app.roomBatch) app.roomBatch.exitSelection()
    Connections {
        target: app.settings
        function onRoomNavigationLayoutChanged() {
            if (app.roomBatch) app.roomBatch.exitSelection()
        }
        function onCompactRoomListChanged() {
            if (app.roomBatch) app.roomBatch.exitSelection()
        }
    }
    Shortcut {
        sequence: "Escape"
        enabled: root.visible && app.roomBatch.selecting
                 && !app.roomBatch.awaitingLeaveConfirmation
        onActivated: app.roomBatch.exitSelection()
    }
    function knownLastAdminCount() {
        if (!app.roomInfo || app.roomInfo.truncated || app.roomInfo.loading
            || app.roomInfo.ownPowerLevel < 100 || app.roomInfo.roomId.length === 0
            || !app.roomBatch.isSelected(app.roomInfo.roomId)) return 0
        var admins = 0
        const members = app.roomInfo.joinedMembers
        for (var i = 0; i < members.length; ++i)
            if (members[i].powerLevel !== undefined && members[i].powerLevel >= 100)
                ++admins
        return admins === 1 ? 1 : 0
    }
    function collapse() {
        if (!compact)
            app.settings.setRoomListExpandedWidth(Math.round(width))
        app.settings.roomListWidth = app.settings.roomListMinWidth
        // A handle press can replace SplitView's preferredWidth binding.
        // A click clears the drag saver, so reattach it here as well.
        root.SplitView.preferredWidth = Qt.binding(
            function() { return app.settings.roomListWidth })
    }
    function expand() {
        app.settings.roomListWidth = app.settings.roomListExpandedWidth()
        root.SplitView.preferredWidth = Qt.binding(
            function() { return app.settings.roomListWidth })
    }

    ThemedSurface {
        id: sidebarGround
        anchors.fill: parent
        role: "roomList"
        flatFill: false
        innerShadowEdge: "left"
    }

    function startConversation(mode, options) {
        newConversationDialog.openDialog(mode, options)
    }

    function openActivityCenter() {
        activityPanel.openPanel()
    }

    function openDiscover(startMode) {
        discoverJoinDialog.openDialog(startMode)
    }

    function releaseHeldOrder() {
        if (app.roomList)
            app.roomList.releaseOrder()
        if (app.spaceChannels)
            app.spaceChannels.releaseOrder()
    }
    readonly property bool orderHeld:
        (app.roomList && app.roomList.orderHeld)
        || (app.spaceChannels && app.spaceChannels.orderHeld)
    Connections {
        target: Qt.application
        function onStateChanged() {
            if (Qt.application.state === Qt.ApplicationActive)
                root.releaseHeldOrder()
        }
    }

    signal messageSearchRequested()

    function openDiscoverForLink(link) {
        discoverJoinDialog.openForLink(link)
    }

    function findDemoDescendant(obj, name) {
        if (!obj) return null
        if (obj.objectName === name) return obj
        if (obj.contentItem) {
            var viaContent = findDemoDescendant(obj.contentItem, name)
            if (viaContent) return viaContent
        }
        var kids = obj.children || []
        for (var i = 0; i < kids.length; ++i) {
            var found = findDemoDescendant(kids[i], name)
            if (found) return found
        }
        var data = obj.data || []
        for (var j = 0; j < data.length; ++j) {
            var found2 = findDemoDescendant(data[j], name)
            if (found2) return found2
        }
        return null
    }

    Connections {
        target: app.demo
        enabled: app.screenshotDemoActive
        function onDemoOpenRoomContextMenu() {
            if (!roomList.contentItem) return
            var kids = roomList.contentItem.children
            for (var i = 0; i < kids.length; ++i) {
                if (kids[i] && kids[i].selected === true) {
                    var menu = root.findDemoDescendant(kids[i], "roomContextMenu")
                    if (menu && menu.popup) menu.popup()
                    return
                }
            }
        }
        function onDemoOpenNewConversation() {
            newConversationDialog.openDialog()
            Qt.callLater(function() {
                var picker = root.findDemoDescendant(newConversationDialog, "dmUserPicker")
                if (picker)
                    picker.searchText = "launch-crew"
            })
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        visible: !root.compact

        Rectangle {
            Layout.fillWidth: true
            color: sidebarGround.hasGradient ? "transparent" : AppTheme.roomList
            implicitHeight: Math.max(AppTheme.headerBandHeight,
                headerRow.implicitHeight + AppTheme.spacing12 * 2)

            RowLayout {
                id: headerRow
                anchors {
                    left: parent.left
                    verticalCenter: parent.verticalCenter
                    leftMargin: AppTheme.spacing12
                }
                spacing: AppTheme.spacing6

                Label {
                    id: workspaceLabel
                    objectName: "workspaceLabel"
                    Layout.alignment: Qt.AlignVCenter
                    Layout.maximumWidth: Math.max(0, root.width
                        - AppTheme.spacing12 * 2 - headerRow.spacing
                        - wordmarkBolt.implicitWidth
                        - (railHiddenActions.visible
                           ? railHiddenActions.implicitWidth
                             + AppTheme.spacing8 : 0))
                    readonly property bool channelsLayout:
                        app.settings && app.settings.roomNavigationLayout === 1
                    property int spacesRevision: 0
                    Connections {
                        target: app.spaces
                        function onSpacesChanged() {
                            workspaceLabel.spacesRevision++
                        }
                    }
                    text: {
                        void spacesRevision
                        if (!app.spaces)
                            return qsTr("Lightning")
                        var id = app.spaces.activeSpaceId
                        if (id === "" || id === undefined)
                            return channelsLayout ? qsTr("Home") : qsTr("Lightning")
                        if (id === "@people")
                            return qsTr("Direct Messages")
                        if (id === "@orphans")
                            return qsTr("Other rooms")
                        return app.spaces.spaceName(id) || qsTr("Lightning")
                    }
                    color: AppTheme.roomListTitle
                    font.pixelSize: AppTheme.textTitle
                    font.weight: AppTheme.weightBold
                    elide: Label.ElideRight

                    readonly property bool spaceLink:
                        app.spaces !== null
                        && app.spaces.activeSpaceId.length > 0
                        && app.spaces.activeSpaceId.charAt(0) === "!"
                    HoverHandler {
                        id: workspaceHover
                        enabled: workspaceLabel.spaceLink
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        enabled: workspaceLabel.spaceLink
                        onTapped: app.openSpaceHome(app.spaces.activeSpaceId)
                    }
                    ToolTip.visible: workspaceHover.hovered
                    ToolTip.text: qsTr("Open Space overview")
                    ToolTip.delay: 500
                }

                Icon {
                    id: wordmarkBolt
                    objectName: "workspaceBoltMark"
                    Layout.alignment: Qt.AlignVCenter
                    name: "bolt"
                    size: 15
                    color: AppTheme.wordmarkBolt
                    Accessible.ignored: true
                }
            }

            RailHiddenActions {
                id: railHiddenActions
                anchors {
                    right: parent.right
                    verticalCenter: parent.verticalCenter
                    rightMargin: AppTheme.spacing12
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: AppTheme.headerDivider
        }

        Rectangle {
            id: searchHeader
            Layout.fillWidth: true
            color: sidebarGround.hasGradient ? "transparent" : AppTheme.roomList
            implicitHeight: searchRow.implicitHeight + AppTheme.spacing8 * 2

            readonly property real headerOneRowFloor:
                AppTheme.spacing12 * 2 + AppTheme.spacing8
                + searchCard.Layout.minimumWidth + headerActions.implicitWidth
            readonly property bool headerStacked: width < headerOneRowFloor

            GridLayout {
                id: searchRow
                anchors {
                    left: parent.left; right: parent.right
                    verticalCenter: parent.verticalCenter
                    leftMargin: AppTheme.spacing12
                    rightMargin: AppTheme.spacing12
                }
                columns: searchHeader.headerStacked ? 1 : 2
                columnSpacing: AppTheme.spacing8
                rowSpacing: AppTheme.spacing8

                Rectangle {
                    id: searchCard
                    objectName: "roomSearchCard"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 120
                    implicitHeight: 34
                    radius: AppTheme.radiusMd
                    color: AppTheme.searchField
                    border.width: roomSearch.activeFocus ? 2 : 1
                    border.color: roomSearch.activeFocus ? AppTheme.focusRing
                                  : searchCardHover.hovered ? AppTheme.borderStrong
                                  : AppTheme.searchFieldBorder
                    HoverHandler { id: searchCardHover }
                    TapHandler {
                        onTapped: roomSearch.forceActiveFocus()
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: AppTheme.spacing8 + 2
                        anchors.rightMargin: AppTheme.spacing6
                        spacing: AppTheme.spacing6

                        Icon {
                            name: "search"
                            size: 16
                            color: AppTheme.textMuted
                        }
                        TextField {
                            id: roomSearch
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            placeholderText: qsTr("Search")
                            Accessible.name: qsTr("Search rooms")
                            Keys.onEscapePressed: (event) => {
                                if (roomSearch.text.length > 0)
                                    roomSearch.clear()
                                else
                                    root.forceActiveFocus(Qt.OtherFocusReason)
                                event.accepted = true
                            }
                            onTextChanged: app.roomList.searchQuery = text
                            font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                            color: AppTheme.textPrimary
                            placeholderTextColor: AppTheme.placeholderInk
                            selectionColor: AppTheme.accentSoft
                            selectedTextColor: AppTheme.textPrimary
                            verticalAlignment: TextInput.AlignVCenter
                            padding: 0
                            leftPadding: 0
                            rightPadding: 0
                            background: null
                        }
                        MenuKeycap {
                            keys: "Ctrl+K"
                            storm: false
                            visible: !roomSearch.activeFocus
                                     && searchCard.width >= 220
                        }
                    }
                }

                RowLayout {
                    id: headerActions
                    spacing: AppTheme.spacing8
                    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter

                    IconButton {
                        objectName: "favouritesShortcut"
                        visible: app.loggedIn && app.roomList
                                 && app.roomList.roomFavouritesSupported
                        implicitWidth: 30; implicitHeight: 30
                        radius: AppTheme.radiusMd
                        iconName: "star"
                        iconSize: 18
                        active: app.settings.roomFilterMode === 4
                        Accessible.name: qsTr("Favourites")
                        ToolTip.text: qsTr("Favourites")
                        ToolTip.visible: hovered
                        onClicked: app.settings.roomFilterMode = active ? 0 : 4
                    }

                    IconButton {
                        id: sortBtn
                        objectName: "roomListOptionsButton"
                        visible: app.loggedIn
                        implicitWidth: 30; implicitHeight: 30
                        radius: AppTheme.radiusMd
                        iconName: "more_horiz"
                        iconSize: 18
                        active: roomListSortMenu.opened
                        Accessible.name: qsTr("Room list options")
                        ToolTip.text: qsTr("Room list options")
                        ToolTip.visible: hovered && !roomListSortMenu.opened
                        ToolTip.delay: 500
                        Keys.onMenuPressed: (event) => { sortBtn.clicked(); event.accepted = true }
                        Keys.onPressed: (event) => {
                            if (event.key === Qt.Key_F10
                                    && (event.modifiers & Qt.ShiftModifier)) {
                                sortBtn.clicked()
                                event.accepted = true
                            }
                        }
                        property bool openedByKeyboard: false
                        onClicked: {
                            openedByKeyboard = visualFocus
                            roomListSortMenu.open()
                        }
                        RoomListSortMenu {
                            id: roomListSortMenu
                            parent: sortBtn
                            x: sortBtn.width - width
                            y: sortBtn.height + 4
                            onClosed: if (sortBtn.openedByKeyboard) {
                                // forceActiveFocus() is a no-op on an item the
                                // popup already refocused, so the reason is set
                                // directly: visualFocus reads it.
                                sortBtn.focusReason = Qt.TabFocusReason
                                Qt.callLater(function() {
                                    if (!sortBtn.activeFocus)
                                        sortBtn.forceActiveFocus(Qt.TabFocusReason)
                                    sortBtn.focusReason = Qt.TabFocusReason
                                })
                            }
                        }
                    }
                    IconButton {
                        id: newConversationBtn
                        objectName: "roomListAddButton"
                        visible: app.loggedIn
                                 && (app.conversations.supported
                                     || app.discovery.supported)
                        implicitWidth: 30; implicitHeight: 30
                        radius: AppTheme.radiusMd
                        iconName: "add"
                        iconSize: 18
                        active: roomListAddMenu.opened
                        Accessible.name: qsTr("Start or join a conversation")
                        ToolTip.text: qsTr("New message, room or Space")
                        ToolTip.visible: hovered && !roomListAddMenu.opened
                        ToolTip.delay: 500
                        Keys.onMenuPressed: (event) => { newConversationBtn.clicked(); event.accepted = true }
                        Keys.onPressed: (event) => {
                            if (event.key === Qt.Key_F10
                                    && (event.modifiers & Qt.ShiftModifier)) {
                                newConversationBtn.clicked()
                                event.accepted = true
                            }
                        }
                        property bool openedByKeyboard: false
                        onClicked: {
                            openedByKeyboard = visualFocus
                            roomListAddMenu.popup(
                                newConversationBtn, 0,
                                newConversationBtn.height + AppTheme.spacing4)
                        }
                        AppMenu {
                            id: roomListAddMenu
                            objectName: "roomListAddMenu"
                            onClosed: if (newConversationBtn.openedByKeyboard) {
                                newConversationBtn.focusReason = Qt.TabFocusReason
                                Qt.callLater(function() {
                                    if (!newConversationBtn.activeFocus)
                                        newConversationBtn.forceActiveFocus(Qt.TabFocusReason)
                                    newConversationBtn.focusReason = Qt.TabFocusReason
                                })
                            }
                            AppMenuItem {
                                objectName: "roomListAddMessage"
                                visible: app.conversations.supported
                                iconName: "chat_bubble"
                                text: qsTr("New message")
                                onTriggered: newConversationDialog.openDialog("dm")
                            }
                            AppMenuItem {
                                objectName: "roomListAddRoom"
                                visible: app.conversations.supported
                                iconName: "group_add"
                                text: qsTr("New room")
                                onTriggered: newConversationDialog.openDialog("room")
                            }
                            AppMenuItem {
                                objectName: "roomListAddSpace"
                                visible: app.conversations.supported
                                iconName: "workspaces"
                                text: qsTr("New Space")
                                onTriggered: newConversationDialog.openDialog("space")
                            }
                            AppMenuSeparator {
                                visible: app.conversations.supported
                                         && app.discovery.supported
                            }
                            AppMenuItem {
                                objectName: "roomListAddExplore"
                                visible: app.discovery.supported
                                iconName: "explore"
                                text: qsTr("Explore public rooms")
                                onTriggered: discoverJoinDialog.openDialog("browse")
                            }
                            AppMenuItem {
                                objectName: "roomListAddJoin"
                                visible: app.discovery.supported
                                iconName: "tag"
                                text: qsTr("Join by address…")
                                onTriggered: discoverJoinDialog.openDialog("address")
                            }
                        }
                    }
                    IconButton {
                        id: activityBtn
                        objectName: "activityCenterButton"
                        visible: app.loggedIn && !!app.activity
                        implicitWidth: 30; implicitHeight: 30
                        radius: AppTheme.radiusMd
                        iconName: "notifications"
                        iconSize: 18
                        active: !!app.activity && app.activity.unseenCount > 0
                        Accessible.name: (!!app.activity && app.activity.unseenCount > 0)
                                         ? qsTr("Activity, %n unseen", "", app.activity.unseenCount)
                                         : qsTr("Activity")
                        ToolTip.text: qsTr("Activity")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: activityPanel.openPanel()
                        Rectangle {
                            objectName: "activityCenterBadge"
                            visible: !!app.activity && app.activity.unseenCount > 0
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.topMargin: -3
                            anchors.rightMargin: -5
                            height: 14
                            width: Math.max(14, activityBadgeLabel.implicitWidth + 6)
                            radius: 7
                            color: AppTheme.bolt
                            Label {
                                id: activityBadgeLabel
                                anchors.centerIn: parent
                                text: !!app.activity
                                      ? (app.activity.unseenCount > 99
                                         ? "99+" : String(app.activity.unseenCount))
                                      : ""
                                color: AppTheme.stormDeep
                                font.pixelSize: 9
                                font.weight: AppTheme.weightBold
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            color: sidebarGround.hasGradient ? "transparent" : AppTheme.roomList
            implicitHeight: filterChips.implicitHeight + AppTheme.spacing6 * 2
            Flickable {
                id: chipsFlick
                objectName: "roomFilterChipsFlick"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: AppTheme.spacing12
                anchors.rightMargin: AppTheme.spacing12
                anchors.verticalCenter: parent.verticalCenter
                height: filterChips.implicitHeight
                contentWidth: Math.max(filterChips.implicitWidth, width)
                contentHeight: height
                flickableDirection: Flickable.HorizontalFlick
                boundsBehavior: Flickable.StopAtBounds
                clip: true
                function revealSelected() {
                    for (let i = 0; i < filterChips.children.length; ++i) {
                        const c = filterChips.children[i]
                        if (c.selected === true) {
                            if (c.x < contentX)
                                contentX = c.x
                            else if (c.x + c.width > contentX + width)
                                contentX = c.x + c.width - width
                            return
                        }
                    }
                }
                function showStart() {
                    contentX = app.localization.rightToLeft
                             ? Math.max(0, contentWidth - width) : 0
                    revealSelected()
                }
                onWidthChanged: showStart()
                onContentWidthChanged: showStart()
                Connections {
                    target: app.localization
                    function onLanguageChanged() {
                        chipsFlick.showStart()
                    }
                }
                WheelHandler {
                    acceptedDevices: PointerDevice.Mouse
                    onWheel: (event) => {
                        const d = event.angleDelta.x !== 0 ? event.angleDelta.x
                                                           : event.angleDelta.y
                        chipsFlick.contentX = Math.max(0, Math.min(
                            chipsFlick.contentWidth - chipsFlick.width,
                            chipsFlick.contentX - d / 2))
                    }
                }
            SegmentedControl {
                id: filterChips
                objectName: "roomFilterChips"
                width: chipsFlick.contentWidth
                dense: true
                onCurrentChanged: Qt.callLater(chipsFlick.revealSelected)
                readonly property bool channelsLayout:
                    app.settings && app.settings.roomNavigationLayout === 1
                readonly property bool spaceView:
                    app.spaceChannels
                    && app.spaceChannels.viewKind === "space"
                readonly property bool homeView:
                    app.spaceChannels
                    && app.spaceChannels.viewKind === "home"
                readonly property int channelsFilterMode: {
                    const stored = app.settings.roomFilterMode
                    if (stored === 3 || stored === 4)
                        return stored
                    if (stored === 1 && (filterChips.spaceView || filterChips.homeView))
                        return 1
                    return 0
                }
                model: channelsLayout
                       ? ((spaceView || homeView)
                          ? [ { label: qsTr("All"), value: 0 },
                              { label: qsTr("People"), value: 1 },
                              { label: qsTr("Favourites"), value: 4 },
                              { label: qsTr("Unreads"), value: 3 } ]
                          : [ { label: qsTr("All"), value: 0 },
                              { label: qsTr("Favourites"), value: 4 },
                              { label: qsTr("Unreads"), value: 3 } ])
                       : [ { label: qsTr("All"), value: 0 },
                           { label: qsTr("People"), value: 1 },
                           { label: qsTr("Rooms"), value: 2 },
                           { label: qsTr("Favourites"), value: 4 },
                           { label: qsTr("Unreads"), value: 3 } ]
                current: channelsLayout
                         ? filterChips.channelsFilterMode
                         : app.settings.roomFilterMode
                onActivated: (value) => {
                    app.settings.roomFilterMode = value
                }
            }
            }
            Repeater {
                model: [ { side: "left" }, { side: "right" } ]
                delegate: Item {
                    id: chipEdge
                    required property var modelData
                    readonly property bool leftEdge: modelData.side === "left"
                    objectName: leftEdge ? "roomFilterChipsMoreLeft"
                                         : "roomFilterChipsMoreRight"
                    visible: leftEdge ? chipsFlick.contentX > 1
                                      : chipsFlick.contentX + chipsFlick.width
                                        < chipsFlick.contentWidth - 1
                    width: AppTheme.spacing24
                    height: chipsFlick.height
                    anchors.verticalCenter: chipsFlick.verticalCenter
                    x: leftEdge ? chipsFlick.x : chipsFlick.x + chipsFlick.width - width
                    z: 2
                    Rectangle {
                        anchors.fill: parent
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop {
                                position: 0.0
                                color: chipEdge.leftEdge ? AppTheme.roomList : "transparent"
                            }
                            GradientStop {
                                position: 1.0
                                color: chipEdge.leftEdge ? "transparent" : AppTheme.roomList
                            }
                        }
                    }
                    Icon {
                        anchors.verticalCenter: parent.verticalCenter
                        x: chipEdge.leftEdge ? 0 : parent.width - width
                        name: chipEdge.leftEdge ? "chevron_left" : "chevron_right"
                        size: 16
                        color: AppTheme.textSecondary
                    }
                    TapHandler {
                        onTapped: chipsFlick.contentX = Math.max(0, Math.min(
                            chipsFlick.contentWidth - chipsFlick.width,
                            chipsFlick.contentX
                            + (chipEdge.leftEdge ? -1 : 1) * chipsFlick.width / 2))
                    }
                }
            }
        }
        Binding {
            target: app.roomList
            property: "filterMode"
            value: app.settings.roomFilterMode
        }
        Binding {
            target: app.spaceChannels
            property: "filterMode"
            value: filterChips.channelsFilterMode
        }
        Binding {
            target: app.spaceChannels
            property: "searchQuery"
            value: roomSearch.text
        }
        Binding {
            target: app.spaceChannels
            property: "messageSearchSupported"
            value: app.loggedIn && app.messageSearch.supported
        }
        Binding {
            target: app.spaceChannels
            property: "scopeSpaceId"
            value: app.spaces ? app.spaces.activeSpaceId : ""
        }

        NewConversationDialog {
            id: newConversationDialog
            parent: Overlay.overlay
        }

        ActivityCenterPanel { id: activityPanel }
        InvitePeopleDialog {
            id: roomInviteDialog
            objectName: "roomListInviteDialog"
            parent: Overlay.overlay
        }

        ChatBackgroundDialog {
            id: roomChatBackgroundDialog
            objectName: "roomListChatBackgroundDialog"
        }
        DiscoverJoinDialog {
            id: discoverJoinDialog
            parent: Overlay.overlay
        }

        TextEdit {
            id: roomLinkClipboard
            visible: false
            width: 0
            height: 0
        }

        LeaveRoomConfirmDialog {
            id: leaveRoomConfirm
            objectName: "leaveRoomConfirmDialog"
            onConfirmed: (roomId) => app.roomInfo.leaveRoom(roomId)

            function openForRoom(roomId, name) {
                var members = -1
                var rule = ""
                var invited = -1
                var soleAdmin = false
                if (app.roomInfo && app.roomInfo.roomId === roomId) {
                    soleAdmin = soleAdminOf(app.roomInfo, roomId)
                    members = app.roomInfo.joinedCount
                    rule = app.roomInfo.joinRule
                    invited = app.roomInfo.invitedCount
                } else {
                    var row = app.roomList.findRoom(roomId)
                    // RoomInfo.members holds only the member events seen so
                    // far, so the list's own count is never a head count;
                    // only the SDK's joined count may say "only member".
                    // findRoom omits each key the SDK has not reported.
                    if (row && row.joinedMemberCount !== undefined)
                        members = row.joinedMemberCount
                    if (row && row.joinRule !== undefined)
                        rule = row.joinRule
                    if (row && row.invitedCount !== undefined)
                        invited = row.invitedCount
                }
                openFor(roomId, name, members, rule, invited, soleAdmin)
            }
        }

        Dialog {
            id: leaveRoomFailedDialog
            objectName: "leaveRoomFailedDialog"
            parent: Overlay.overlay
            anchors.centerIn: parent
            width: Math.max(240, Math.min(400, parent ? parent.width - 32 : 400))
            modal: true
            standardButtons: Dialog.NoButton
            closePolicy: Popup.CloseOnEscape
            property string roomLabel: ""
            property string messageText: ""
            title: qsTr("Couldn't leave \"%1\"").arg(leaveRoomFailedDialog.roomLabel)

            background: Rectangle {
                color: AppTheme.surface
                border.color: AppTheme.border
                radius: AppTheme.radiusLg
            }

            contentItem: ColumnLayout {
                spacing: AppTheme.spacing12
                Label {
                    Layout.fillWidth: true
                    text: leaveRoomFailedDialog.messageText
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    color: AppTheme.textPrimary
                }
                RowLayout {
                    Layout.fillWidth: true
                    Item { Layout.fillWidth: true }
                    AppButton {
                        text: qsTr("Close")
                        focus: true
                        onClicked: leaveRoomFailedDialog.close()
                    }
                }
            }
        }
        Connections {
            target: app.roomInfo
            function onRoomLeaveFailed(roomId, message) {
                var info = app.roomList.findRoom(roomId)
                leaveRoomFailedDialog.roomLabel =
                    (info && info.name) ? info.name : roomId
                leaveRoomFailedDialog.messageText = message
                leaveRoomFailedDialog.open()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: AppTheme.stormBorderStrong
        }

        AppDialog {
            id: batchLeaveDialog
            objectName: "batchLeaveConfirmDialog"
            modal: true
            title: qsTr("Leave %n room(s)?", "", app.roomBatch.selectionCount)
            parent: Overlay.overlay
            visible: app.roomBatch.awaitingLeaveConfirmation
            anchors.centerIn: parent
            width: Math.min(400, root.width - 20)
            onRejected: app.roomBatch.cancelLeave()
            onClosed: if (app.roomBatch.awaitingLeaveConfirmation)
                app.roomBatch.cancelLeave()
            contentItem: ColumnLayout {
                spacing: AppTheme.spacing12
                Label {
                    Layout.fillWidth: true
                    color: AppTheme.stormText
                    wrapMode: Text.WordWrap
                    text: qsTr("You will leave all selected rooms. You may need an invitation to rejoin private rooms.")
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.knownLastAdminCount() > 0
                    color: AppTheme.stormText
                    wrapMode: Text.WordWrap
                    text: qsTr("You are the last known admin in one selected room. Assign another admin before leaving if the room needs one.")
                }
                RowLayout {
                    Item { Layout.fillWidth: true }
                    AppButton { text: qsTr("Cancel"); storm: true; onClicked: app.roomBatch.cancelLeave() }
                    AppButton { text: qsTr("Leave rooms"); storm: true; kind: "dangerPrimary"; onClicked: app.roomBatch.confirmLeave() }
                }
            }
        }

        Item {
            id: listBody
            Layout.fillWidth: true
            Layout.fillHeight: true

            Rectangle {
                id: batchOverlay
                z: 40
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: batchOverlayContent.implicitHeight
                visible: !root.compact && (app.roomBatch.selecting
                                           || app.roomBatch.outcomes.length > 0)
                color: AppTheme.roomList
                Column {
                    id: batchOverlayContent
                    anchors.fill: parent
                    Rectangle {
                        visible: app.roomBatch.selecting
                        width: parent.width
                        height: visible ? batchActions.implicitHeight + AppTheme.spacing8 * 2 : 0
                        color: AppTheme.roomList
                        Flow {
                            id: batchActions
                            objectName: "roomBatchActionBar"
                            width: parent.width - AppTheme.spacing8 * 2
                            x: AppTheme.spacing8
                            spacing: AppTheme.spacingXS
                            AppButton { text: qsTr("%n selected", "", app.roomBatch.selectionCount); enabled: false }
                            AppButton { text: qsTr("Read"); enabled: app.roomBatch.markReadSupported && app.roomBatch.selectionCount > 0 && !app.roomBatch.busy; onClicked: app.roomBatch.run("read") }
                            AppButton { text: qsTr("Favourite"); enabled: app.roomList.roomFavouritesSupported && app.roomBatch.selectionCount > 0 && !app.roomBatch.busy; onClicked: app.roomBatch.run("favourite", 1) }
                            AppButton { text: qsTr("Unfavourite"); enabled: app.roomList.roomFavouritesSupported && app.roomBatch.selectionCount > 0 && !app.roomBatch.busy; onClicked: app.roomBatch.run("favourite", 0) }
                            AppButton { text: qsTr("Low priority"); enabled: app.roomList.roomFavouritesSupported && app.roomBatch.selectionCount > 0 && !app.roomBatch.busy; onClicked: app.roomBatch.run("low_priority", 1) }
                            AppButton { text: qsTr("Remove low priority"); enabled: app.roomList.roomFavouritesSupported && app.roomBatch.selectionCount > 0 && !app.roomBatch.busy; onClicked: app.roomBatch.run("low_priority", 0) }
                            AppButton { text: qsTr("Notify all"); enabled: app.roomBatch.selectionCount > 0 && !app.roomBatch.busy; onClicked: app.roomBatch.run("notification", 0) }
                            AppButton { text: qsTr("Mentions"); enabled: app.roomBatch.selectionCount > 0 && !app.roomBatch.busy; onClicked: app.roomBatch.run("notification", 1) }
                            AppButton { text: qsTr("Mute"); enabled: app.roomBatch.selectionCount > 0 && !app.roomBatch.busy; onClicked: app.roomBatch.run("notification", 2) }
                            AppButton { text: qsTr("Default"); enabled: app.roomBatch.selectionCount > 0 && !app.roomBatch.busy; onClicked: app.roomBatch.run("notification", 3) }
                            AppButton { text: qsTr("Leave…"); enabled: app.roomBatch.selectionCount > 0 && !app.roomBatch.busy; onClicked: app.roomBatch.requestLeave() }
                            AppButton { text: qsTr("Retry failed"); visible: app.roomBatch.canRetry; enabled: !app.roomBatch.busy; onClicked: app.roomBatch.retryFailed() }
                            AppButton { text: qsTr("Done"); onClicked: app.roomBatch.exitSelection() }
                            property bool summaryDismissed: false
                            readonly property bool allDone:
                                app.roomBatch.outcomes.length > 0
                                && app.roomBatch.failed === 0
                                && app.roomBatch.pending === 0
                            onAllDoneChanged: {
                                summaryDismissed = false
                                if (allDone)
                                    summaryTimer.restart()
                            }
                            Timer {
                                id: summaryTimer
                                interval: 4000
                                onTriggered: batchActions.summaryDismissed = true
                            }
                            Label {
                                objectName: "roomBatchSummary"
                                visible: app.roomBatch.outcomes.length > 0
                                         && !batchActions.summaryDismissed
                                text: batchActions.allDone
                                      ? qsTr("%n room(s) updated", "", app.roomBatch.succeeded)
                                      : qsTr("%1 succeeded · %2 failed · %3 pending")
                                        .arg(app.roomBatch.succeeded).arg(app.roomBatch.failed).arg(app.roomBatch.pending)
                                color: app.roomBatch.failed > 0 ? AppTheme.error : AppTheme.textMuted
                            }
                        }
                    }
                    ListView {
                        objectName: "roomBatchOutcomes"
                        x: AppTheme.spacing8
                        width: parent.width - AppTheme.spacing8 * 2
                        height: Math.min(contentHeight, 96)
                        visible: model.length > 0
                        clip: true
                        model: app.roomBatch.outcomes.filter(
                                   (o) => o.status === "failed")
                        delegate: Label {
                            required property var modelData
                            width: ListView.view.width
                            leftPadding: AppTheme.spacing8
                            color: modelData.status === "failed" ? AppTheme.error : AppTheme.textMuted
                            text: qsTr("%1: %2").arg(modelData.name || modelData.roomId)
                                  .arg(modelData.status === "succeeded" ? qsTr("Succeeded")
                                       : modelData.status === "failed" ? qsTr("Failed")
                                       : qsTr("Pending"))
                            elide: Text.ElideRight
                        }
                    }

                    AppButton {
                        visible: !app.roomBatch.selecting && app.roomBatch.outcomes.length > 0
                        text: qsTr("Done")
                        onClicked: app.roomBatch.exitSelection()
                    }
                }
            }

            readonly property bool channelsChosen:
                app.settings && app.settings.roomNavigationLayout === 1
            readonly property bool channelsUsable: channelsChosen

            Loader {
                id: classicLoader
                anchors.fill: parent
                anchors.bottomMargin: batchOverlay.visible ? batchOverlay.height : 0
                active: !root.compact && !parent.channelsUsable
                visible: active
                sourceComponent: RoomListClassicPresenter {
                    currentRoomId: app.currentRoomId
                    onRoomActivated: (roomId) => app.openRoom(roomId)
                    onRoomOpenedByPointer: (roomId) => app.requestComposerFocus(roomId)
                    onCreateRequested: newConversationDialog.openDialog()
                    onDiscoverRequested: discoverJoinDialog.openDialog()
                    onClearSearchRequested: roomSearch.clear()
                    onRoomLinkCopyRequested: (roomId) => {
                        var row = app.roomList.findRoom(roomId)
                        var link = app.roomList.roomPermalink(
                            roomId, (row && row.canonicalAlias) || "")
                        if (link.length > 0) {
                            roomLinkClipboard.text = link
                            roomLinkClipboard.selectAll()
                            roomLinkClipboard.copy()
                            roomLinkClipboard.text = ""
                        }
                    }
                    onLeaveRoomRequested: (roomId, roomName) =>
                        leaveRoomConfirm.openForRoom(roomId, roomName)
                    onChatBackgroundRequested: (roomId, roomName) =>
                        roomChatBackgroundDialog.openForRoom(roomId, roomName)
                    onInviteRequested: (roomId) =>
                        roomInviteDialog.openFor(roomId)
                }
            }

            Loader {
                id: channelsLoader
                anchors.fill: parent
                anchors.bottomMargin: batchOverlay.visible ? batchOverlay.height : 0
                active: !root.compact && parent.channelsUsable
                visible: active
                sourceComponent: RoomChannelsPresenter {
                    currentRoomId: app.currentRoomId
                    onRoomActivated: (roomId) => app.openRoom(roomId)
                    onRoomOpenedByPointer: (roomId) => app.requestComposerFocus(roomId)
                    onLobbyActivated: app.openLobby()
                    onMessageSearchRequested: root.messageSearchRequested()
                    onCreateRoomRequested: newConversationDialog.openDialog("room")
                    onCreateChatRequested: newConversationDialog.openDialog("dm")
                    onJoinAddressRequested: discoverJoinDialog.openDialog("address")
                    onExploreSpacesRequested: discoverJoinDialog.openDialog("browse")
                    onRoomLinkCopyRequested: (roomId) => {
                        var row = app.roomList.findRoom(roomId)
                        var link = app.roomList.roomPermalink(
                            roomId, (row && row.canonicalAlias) || "")
                        if (link.length > 0) {
                            roomLinkClipboard.text = link
                            roomLinkClipboard.selectAll()
                            roomLinkClipboard.copy()
                            roomLinkClipboard.text = ""
                        }
                    }
                    onLeaveRoomRequested: (roomId, roomName) =>
                        leaveRoomConfirm.openForRoom(roomId, roomName)
                    onChatBackgroundRequested: (roomId, roomName) =>
                        roomChatBackgroundDialog.openForRoom(roomId, roomName)
                    onInviteRequested: (roomId) =>
                        roomInviteDialog.openFor(roomId)
                }
            }
        }

        VoiceConnectedBar {
            objectName: "roomsPanelVoiceBar"
            Layout.fillWidth: true
            Layout.margins: visible ? AppTheme.spacing8 : 0
            onReturnToCallRequested: {
                if (app.groupCall.roomId.length > 0)
                    // openRoom(), not a currentRoomId write: the write only
                    // selects, and openRoom() is the only caller of
                    // openRoomTimeline(). Its alreadyOpen guard would then skip
                    // the real open.
                    app.openRoom(app.groupCall.roomId)
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        visible: root.compact
        spacing: 0
        IconButton {
            objectName: "compactRoomListHeader"
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: AppTheme.spacing8
            iconName: app.spaces && app.spaces.activeSpaceId ? "workspaces" : "home"
            Accessible.name: qsTr("Expand %1 room list").arg(workspaceLabel.text)
            ToolTip.text: workspaceLabel.text
            ToolTip.visible: hovered
            onClicked: root.expand()
        }
        IconButton {
            id: compactRoomSearch
            objectName: "compactRoomListSearch"
            Layout.alignment: Qt.AlignHCenter
            iconName: "search"
            Accessible.name: qsTr("Search rooms")
            ToolTip.text: qsTr("Search rooms")
            ToolTip.visible: hovered
            onClicked: {
                root.expand()
                Qt.callLater(function() { roomSearch.forceActiveFocus() })
            }
        }
        IconButton {
            objectName: "compactRoomListFilter"
            Layout.alignment: Qt.AlignHCenter
            iconName: "format_list_bulleted"
            Accessible.name: app.settings.roomFilterMode === 3
                             ? qsTr("Show all rooms") : qsTr("Show unread rooms")
            ToolTip.text: Accessible.name
            ToolTip.visible: hovered
            onClicked: app.settings.roomFilterMode =
                       app.settings.roomFilterMode === 3 ? 0 : 3
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: AppTheme.spacing8
            height: 1
            color: AppTheme.headerDivider
        }
        Loader {
            active: root.compact
            Layout.fillWidth: true
            Layout.fillHeight: true
            sourceComponent: RoomListCompactPresenter {
                onRoomActivated: (roomId) => app.openRoom(roomId)
                onLobbyActivated: app.openLobby()
                onMessageSearchRequested: root.messageSearchRequested()
                onExpandRequested: root.expand()
                onActionRequested: (actionId) => {
                    if (actionId === "@new-room")
                        newConversationDialog.openDialog("room")
                    else if (actionId === "@new-chat")
                        newConversationDialog.openDialog("dm")
                    else if (actionId === "@join-address")
                        discoverJoinDialog.openDialog("address")
                    else if (actionId === "@explore")
                        discoverJoinDialog.openDialog("browse")
                }
            }
        }
    }
}
