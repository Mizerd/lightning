import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

// Right-side Room Information panel. Overview: identity, encryption state,
// permission-gated name/topic editing, leave with confirmation. People: member
// search, membership state, roles, Invite when the SDK allows. Member data is a
// bounded in-memory snapshot from RoomInfoController; nothing is persisted.
Rectangle {
    id: root

    // The section tabs wrap to two rows when they cannot fit the panel (whose
    // floor is 260). The decision comes from tabsProbe, an off-layout copy
    // whose width the result cannot move, so the layout never chases itself.
    // Applied deferred rather than as a direct binding: changing the wrapped
    // Repeater's model from inside the layout pass (overflowing depends on
    // widths that pass is settling) rebuilt delegate trees every frame and hung
    // the GUI thread. Qt.callLater moves the change outside the pass and
    // coalesces bursts. The offscreen harness cannot reproduce the hang.
    property bool tabsWrap: false
    // Hysteresis: wrap as soon as the strip does not fit, unwrap only with real
    // room to spare, so a width at the threshold cannot flip every turn.
    readonly property real tabsUnwrapSlack: 12
    function applyTabsWrap() {
        if (!tabsWrap) {
            tabsWrap = tabsProbe.overflowing
        } else if (tabsProbe.width > 0
                   && tabsProbe.implicitWidth
                      <= tabsProbe.width - root.tabsUnwrapSlack) {
            tabsWrap = false
        }
    }
    readonly property var tabModel: {
        const tabs = [{ label: qsTr("Overview"), value: "overview" }]
        if (root.pinnedAvailable)
            tabs.push({ label: qsTr("Pinned"), value: "pinned" })
        tabs.push({ label: qsTr("People"), value: "people" })
        tabs.push({ label: qsTr("Media"), value: "media" })
        if (root.widgetsAvailable)
            tabs.push({ label: qsTr("Widgets"), value: "widgets" })
        return tabs
    }
    color: AppTheme.sidebar
    visible: width > 0
    // A last line of defence: stops a control that fails to wrap or elide from
    // painting over the timeline or off the window.
    clip: true

    property var roomData: ({})
    signal closeRequested()
    // Media & Files hands viewing/saving to TimelinePane. Pass the list and the
    // index, never a key alone: the picture may be absent from the loaded
    // timeline (see MediaBrowser's signal).
    signal openImagesRequested(var entries, int index)
    signal saveMediaRequested(string mediaKey, string filename)

    // "overview" | "pinned" | "people" | "media" | "widgets"
    property string section: "overview"
    property string memberFilter: ""

    // PinnedMessagesController follows the active room, while this panel can
    // show another room (a Space home); the tab exists only when the two agree.
    readonly property bool pinnedAvailable:
        app.pinned && app.pinned.supported
        && app.roomInfo.roomId !== ""
        && app.roomInfo.roomId === app.pinned.roomId
    // A disappearing tab must not leave a blank section.
    onPinnedAvailableChanged: {
        if (!pinnedAvailable && section === "pinned")
            section = "overview"
    }
    // Widgets get their own tab because Overview is already the fullest
    // section. Absent only when the backend cannot read widgets; present (and
    // saying so) for a room with none.
    readonly property bool widgetsAvailable:
        app.widgets && app.widgets.supported
    onWidgetsAvailableChanged: {
        if (!widgetsAvailable && section === "widgets")
            section = "overview"
    }
    // Jump to a pinned event. TimelinePane owns navigation, so out-of-window
    // pins use the single existing path.
    signal jumpToEventRequested(string eventId)
    /// Export this room's loaded messages; the host owns the dialog. This panel
    /// is signal-only.
    signal exportRoomRequested()

    // Looks up avatar/name/topic itself so late updates (e.g. an avatar
    // resolved after opening) refresh here, as in the timeline header.
    function openForRoom(roomId) {
        app.roomInfo.roomId = roomId
        // Widgets are read on demand: a /state read for a type sliding sync
        // does not carry, so not for every room the user passes through.
        if (app.widgets.supported) {
            app.widgets.roomId = roomId
            app.widgets.refresh()
        }
        // MSC2346 bridge state, also a /state read. Opening this panel is an
        // explicit action on one room, so it always reads (once per room per
        // session); the room list never triggers one (see
        // AppController::requestRoomBridgeInfo).
        app.requestRoomBridgeInfo(roomId, true)
        section = "overview"
        memberFilter = ""
        memberSearch.text = ""
        // The membership filter and sort reset with the room, so a new room is
        // not shown under the previous room's filter.
        memberSection.membership = "joined"
        memberSection.alphabetical = false
        refreshRoomData()
        // Re-query the room's server push-rule mode on open, so changes from
        // other clients land. A no-op on backends without server push rules.
        app.requestRoomNotificationMode(roomId)
    }
    function refreshRoomData() {
        roomData = app.roomInfo.roomId !== ""
                   ? app.roomList.findRoom(app.roomInfo.roomId) : ({})
    }
    Connections {
        target: app.roomList
        function onDataChanged() { root.refreshRoomData() }
        function onModelReset() { root.refreshRoomData() }
    }

    InvitePeopleDialog {
        id: inviteDialog
        parent: Overlay.overlay
    }

    MemberProfilePopover {
        id: memberProfile
        parent: Overlay.overlay
        anchors.centerIn: parent
    }

    // The People list's context menu. What it offers is read from the
    // controller at open (canModerate is a method, so a binding would not
    // follow the roster); the popover's confirm step re-checks and sends.
    function openMemberMenu(member, anchor, x, y) {
        if (!member || !member.userId || !app.roomInfo)
            return
        memberMenu.member = member
        memberMenu.canKick = app.roomInfo.canModerate(member.userId, "kick")
        memberMenu.canBan = app.roomInfo.canModerate(member.userId, "ban")
        memberMenu.canUnban = app.roomInfo.canModerate(member.userId, "unban")
        memberMenu.popup(anchor, x, y)
    }

    AppMenu {
        id: memberMenu
        objectName: "roomMemberMenu"
        menuWidth: 210
        property var member: ({})
        property bool canKick: false
        property bool canBan: false
        property bool canUnban: false
        contextLabel: member && member.userId
                      ? (member.displayName || member.userId) : ""

        AppMenuItem {
            objectName: "roomMemberMenuProfile"
            iconName: "person"
            text: qsTr("View profile")
            onTriggered: memberProfile.openFor(memberMenu.member)
        }
        AppMenuSeparator {
            visible: memberMenu.canKick || memberMenu.canBan
                     || memberMenu.canUnban
        }
        AppMenuItem {
            objectName: "roomMemberMenuKick"
            visible: memberMenu.canKick
            danger: true
            iconName: "person_remove"
            text: qsTr("Remove from room…")
            onTriggered: memberProfile.openForAction(memberMenu.member, "kick")
        }
        AppMenuItem {
            objectName: "roomMemberMenuBan"
            visible: memberMenu.canBan
            danger: true
            iconName: "block"
            text: qsTr("Ban from room…")
            onTriggered: memberProfile.openForAction(memberMenu.member, "ban")
        }
        AppMenuItem {
            objectName: "roomMemberMenuUnban"
            visible: memberMenu.canUnban
            iconName: "undo"
            text: qsTr("Unban…")
            onTriggered: memberProfile.openForAction(memberMenu.member, "unban")
        }
    }

    // Development-only: find a descendant by objectName through children and
    // the default data list.
    function findDemoDescendant(obj, name) {
        if (!obj) return null
        if (obj.objectName === name) return obj
        // Dialogs/Popups are not Items; their subtree hangs off contentItem.
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

    // Screenshot-demo hooks; inert in a non-demo build.
    Connections {
        target: app.demo
        enabled: app.screenshotDemoActive
        function onDemoOpenInvitePeople() {
            inviteDialog.openFor(app.currentRoomId)
            // Seed the search so real results render.
            Qt.callLater(function() {
                var picker = root.findDemoDescendant(inviteDialog, "invitePeoplePicker")
                if (picker)
                    picker.searchText = "ma"
            })
        }
    }

    FileDialog {
        id: avatarDialog
        title: qsTr("Choose room avatar")
        fileMode: FileDialog.OpenFile
        nameFilters: [ qsTr("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp)") ]
        // The picker only chooses; the crop dialog is the gate that refuses SVG
        // before anything renders it (CLAUDE.md §6).
        onAccepted: avatarCrop.openFor(selectedFile)
    }

    FileDialog {
        id: myAvatarDialog
        title: qsTr("Choose your avatar for this room")
        fileMode: FileDialog.OpenFile
        nameFilters: [ qsTr("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp)") ]
        // The crop dialog is the gate that refuses SVG (CLAUDE.md §6).
        onAccepted: myAvatarCrop.openFor(selectedFile)
    }
    ImageCropDialog {
        id: myAvatarCrop
        role: "avatar"
        onCropped: function (file) { app.roomInfo.setMyRoomAvatar(file) }
    }

    ImageCropDialog {
        id: avatarCrop
        role: "avatar"
        // The cropped temp file is a local path, which this sink already takes.
        onCropped: function (file) { app.roomInfo.setRoomAvatar(file) }
    }

    // Measuring probe: reads `overflowing` from a control whose width cannot
    // depend on the answer. Not in the layout; given the same content,
    // compaction and width as the real strip. Never draws or takes focus.
    SegmentedControl {
        id: tabsProbe
        objectName: "roomInfoTabsProbe"
        visible: false
        enabled: false
        storm: true
        dense: true
        fitWidth: true
        model: root.tabModel
        width: Math.max(0, root.width - AppTheme.spacing6 * 2)
        onOverflowingChanged: Qt.callLater(root.applyTabsWrap)
        onWidthChanged: Qt.callLater(root.applyTabsWrap)
        onImplicitWidthChanged: Qt.callLater(root.applyTabsWrap)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Header, floored at the shared top-strip height so its rule lines up
        // with the room header beside it.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Math.max(AppTheme.headerBandHeight,
                headerRow.implicitHeight + AppTheme.spacing12 * 2)
            color: AppTheme.surface
            RowLayout {
                id: headerRow
                anchors.fill: parent
                anchors.margins: AppTheme.spacing12
                spacing: AppTheme.spacing8
                Label {
                    Layout.fillWidth: true
                    // The People section names its subject and size. Branched
                    // rather than %n, which renders "(s)" literally without a
                    // translation.
                    text: {
                        if (root.section !== "people")
                            return qsTr("Room information");
                        var n = app.roomInfo.joinedCount;
                        if (n <= 0)
                            return qsTr("Members");
                        return n === 1 ? qsTr("1 member")
                                       : qsTr("%1 members").arg(n);
                    }
                    color: AppTheme.textPrimary
                    elide: Label.ElideRight
                    // Pane-header size, matching the room-list header.
                    font.pixelSize: AppTheme.textTitle
                    font.weight: AppTheme.weightDisplay
                }
                IconButton {
                    implicitWidth: 30; implicitHeight: 30
                    radius: AppTheme.radiusControl
                    iconName: "close"
                    iconSize: 18
                    Accessible.name: qsTr("Close room information")
                    onClicked: root.closeRequested()
                }
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

        SegmentedControl {
            id: roomInfoTabs
            objectName: "roomInfoTabs"
            storm: true
            // Horizontal margins never follow tabsWrap, or they would change
            // the width `overflowing` is measured against.
            Layout.leftMargin: AppTheme.spacing6
            Layout.rightMargin: AppTheme.spacing6
            Layout.topMargin: AppTheme.spacing6
            Layout.bottomMargin: AppTheme.spacing6
            // Plain `visible`: keeping it in the layout at zero height bound to
            // its own implicit height made the ColumnLayout re-polish
            // endlessly. Safe because the wrap is decided by tabsProbe.
            visible: !root.tabsWrap
            // Zero minimum: otherwise the RowLayout's minimum is the sum of its
            // segments, which propagates up and pushes the panel off the window
            // instead of letting the strip overflow and wrap.
            Layout.minimumWidth: 0
            clip: true
            // fitWidth needs a width from its host.
            Layout.fillWidth: true
            // One size in every section, so the tab strip does not change size
            // with the selected tab. Dense plus fitWidth is what lets
            // translated labels fit a resizable panel.
            dense: true
            fitWidth: true
            // Pinned appears only when supported and the panel shows the room
            // the pin controller tracks. Built up rather than one array per
            // combination.
            model: root.tabModel
            current: root.section
            onActivated: (value) => root.section = value
        }
        // Wrapped form: the same tabs split across two rows, sharing `current`.
        // Not fitWidth, so both rows read at the same size.
        Repeater {
            model: root.tabsWrap ? 2 : 0
            SegmentedControl {
                required property int index
                objectName: "roomInfoTabsRow" + index
                storm: true
                dense: true
                Layout.leftMargin: AppTheme.spacing6
                Layout.rightMargin: AppTheme.spacing6
                Layout.topMargin: index === 0 ? AppTheme.spacing6 : 0
                Layout.bottomMargin: index === 1 ? AppTheme.spacing6 : 2
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                model: {
                    const all = root.tabModel
                    const cut = Math.ceil(all.length / 2)
                    return index === 0 ? all.slice(0, cut) : all.slice(cut)
                }
                current: root.section
                onActivated: (value) => root.section = value
            }
        }

        // Overview. A Flickable rather than ScrollView so it can carry a
        // SmoothWheelArea (see SmoothWheelArea.qml).
        Flickable {
            id: overviewFlick
            visible: root.section === "overview"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: overviewColumn.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
            SmoothWheelArea {}

            ColumnLayout {
                id: overviewColumn
                width: parent.width
                spacing: AppTheme.spacing12

                // Per-room notification mode. On the Rust backend written to
                // the account's server push rules; other backends keep it on
                // this device.
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: AppTheme.spacing12
                    Layout.rightMargin: AppTheme.spacing12
                    Layout.topMargin: AppTheme.spacing12
                    spacing: 4
                    Label {
                        text: app.serverRoomNotificationModes
                              ? qsTr("Notifications")
                              : qsTr("Notifications (this device)")
                        color: AppTheme.textSecondary
                        font.pixelSize: AppTheme.textBody
                        font.weight: AppTheme.weightStrong
                    }
                    AppComboBox {
                        id: notificationModeCombo
                        objectName: "roomNotificationModeCombo"
                        Layout.fillWidth: true
                        // Index === mode, so "Follow account default" (3) stays
                        // last. Offered only where the backend owns server push
                        // rules; locally mode 3 would simply notify for
                        // everything.
                        model: app.serverRoomNotificationModes
                            ? [
                                qsTr("All messages"),
                                // The SDK's MentionsAndKeywordsOnly mode keeps
                                // keyword rules firing.
                                qsTr("Mentions & keywords"),
                                qsTr("Mute"),
                                qsTr("Follow account default")
                              ]
                            : [
                                qsTr("All messages"),
                                qsTr("Mentions & keywords"),
                                qsTr("Mute")
                              ]
                        // Explicit mirrors: both getters are Q_INVOKABLEs, so
                        // refreshMode() re-runs from change signals and on room
                        // switches.
                        property int displayedMode: 0
                        property bool syncFailed: false
                        function refreshMode() {
                            displayedMode = app.roomInfo.roomId !== ""
                                ? app.settings.roomNotificationMode(
                                      app.roomInfo.roomId)
                                : 0
                            syncFailed = app.roomInfo.roomId !== ""
                                && app.roomNotificationModeSyncFailed(
                                       app.roomInfo.roomId)
                        }
                        Component.onCompleted: refreshMode()
                        // A server-backend mode 3 on a device-local backend
                        // clamps to 0 ("All messages"), which matches what the
                        // device does; the last row is Mute.
                        currentIndex: (displayedMode >= 0
                                       && displayedMode < count)
                                      ? displayedMode : 0
                        Connections {
                            target: app.settings
                            function onRoomNotificationModeChanged(roomId) {
                                if (roomId === app.roomInfo.roomId)
                                    notificationModeCombo.refreshMode()
                            }
                        }
                        Connections {
                            target: app
                            function onRoomNotificationModeSyncStateChanged(roomId) {
                                if (roomId === app.roomInfo.roomId)
                                    notificationModeCombo.refreshMode()
                            }
                        }
                        Connections {
                            target: app.roomInfo
                            function onRoomIdChanged() {
                                notificationModeCombo.refreshMode()
                            }
                        }
                        onActivated: (index) => {
                            if (app.roomInfo.roomId !== "")
                                app.setRoomNotificationMode(
                                    app.roomInfo.roomId, index)
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                        wrapMode: Text.WordWrap
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.textMeta
                        // Backend-honest, phrased like the room context-menu
                        // disclaimer.
                        text: app.serverRoomNotificationModes
                              ? (notificationModeCombo.syncFailed
                                 ? qsTr("Couldn't save to the server — "
                                        + "kept on this device. "
                                        + "Retried when you reconnect.")
                                 : (notificationModeCombo.displayedMode === 3
                                    // The server applies the account default to
                                    // pushes, but this device does not know
                                    // what it resolves to and notifies for
                                    // everything.
                                    ? qsTr("This room has no override — your "
                                           + "account's settings apply on the "
                                           + "server. This device notifies "
                                           + "for all messages.")
                                    : qsTr("Saved to your account's notification "
                                           + "settings (server push rules).")))
                              : qsTr("Local setting: it does not change this "
                                     + "room's server push rules.")
                    }
                }

                // Identity block
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing8

                    RowLayout {
                        spacing: AppTheme.spacing12
                        Avatar {
                            objectName: "roomInfoAvatar"
                            size: 56
                            name: root.roomData.name || ""
                            mxc: root.roomData.avatarUrl || ""
                            colorKey: root.roomData.identityColorKey || ""
                            circle: root.roomData.isDirect === true

                            // As in the room header: unambiguous 1:1 DMs only,
                            // inside the avatar's bounds. Gated on the panel
                            // showing Overview, since the panel is not behind a
                            // Loader and would otherwise keep a watch while
                            // closed.
                            PresenceDot {
                                objectName: "roomInfoPresenceDot"
                                anchors.top: parent.top
                                anchors.right: parent.right
                                dotSize: 16
                                ring: root.color
                                hoverStatus: true
                                userId: root.visible
                                        && root.section === "overview"
                                        && root.roomData.isDirect === true
                                        && (root.roomData.identityColorKey || "")
                                               .charAt(0) === "@"
                                        ? root.roomData.identityColorKey : ""
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                Layout.fillWidth: true
                                text: root.roomData.name || qsTr("(unnamed room)")
                                textFormat: Text.PlainText
                                color: AppTheme.textPrimary
                                font.pixelSize: AppTheme.textTitle
                                font.weight: AppTheme.weightStrong
                                lineHeight: AppTheme.lineHeightBody
                                lineHeightMode: Text.ProportionalHeight
                                wrapMode: Text.Wrap
                            }
                            RowLayout {
                                visible: root.roomData.encrypted === true
                                spacing: 4
                                Icon {
                                    name: "verified_user"
                                    size: 14
                                    color: AppTheme.accent
                                }
                                Label {
                                    text: qsTr("End-to-end encrypted")
                                    color: AppTheme.textSecondary
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                }
                            }
                            Label {
                                visible: root.roomData.encrypted !== true
                                text: qsTr("Not encrypted")
                                color: AppTheme.textMuted
                                font.pixelSize: AppTheme.textBody
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: (root.roomData.topic || "").length > 0
                        text: root.roomData.topic || ""
                        // Unsanitized server text; never AutoText.
                        textFormat: Text.PlainText
                        color: AppTheme.textSecondary
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                        wrapMode: Text.Wrap
                        font.pixelSize: AppTheme.textBody
                    }

                    Label {
                        text: qsTr("%1 members (%2 invited)")
                              .arg(app.roomInfo.joinedCount)
                              .arg(app.roomInfo.invitedCount)
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.textBody
                    }

                    // Which network this room is bridged to. The label is our
                    // curated name for a known MSC2346 protocol; otherwise the
                    // bridge's own text, sanitized and bounded in Rust and
                    // rendered as plain text. No bridge user ids are shown. A
                    // plain `visible` is fine here: the empty-Label concern is
                    // for per-row timeline delegates.
                    RowLayout {
                        objectName: "roomInfoBridgeRow"
                        visible: (root.roomData.bridgeLabel || "").length > 0
                        spacing: 4
                        Icon {
                            name: "link"
                            size: 14
                            color: AppTheme.textMuted
                        }
                        Label {
                            objectName: "roomInfoBridgeLabel"
                            Layout.fillWidth: true
                            text: qsTr("Bridged via %1")
                                  .arg(root.roomData.bridgeLabel || "")
                            // Bridge-chosen text in the fallback case; never
                            // AutoText.
                            textFormat: Text.PlainText
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.textBody
                            elide: Text.ElideRight
                        }
                    }


                    // Export sits with the room's details. Moderation rules are
                    // offered for every room, since a policy room exists to
                    // hold rules; publishing is gated by the room's power
                    // levels.
                    AppButton {
                        objectName: "roomInfoPolicyButton"
                        visible: app.policy && app.policy.available
                        text: qsTr("Moderation rules…")
                        onClicked: policyListDialog.openFor(
                            app.roomInfo.roomId, root.roomData.name || "")
                        ToolTip.text: qsTr("Ban lists this room publishes")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                    }
                    AppButton {
                        objectName: "roomInfoExportButton"
                        text: qsTr("Export room…")
                        onClicked: root.exportRoomRequested()
                        ToolTip.text: qsTr("Save this room's loaded messages "
                                           + "to a file")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                    }
                    AppButton {
                        text: qsTr("Copy room ID")
                        onClicked: {
                            copyHelper.text = app.roomInfo.roomId
                            copyHelper.selectAll()
                            copyHelper.copy()
                        }
                        ToolTip.text: app.roomInfo.roomId
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                    }
                    // Hidden helper for clipboard copy.
                    TextEdit {
                        id: copyHelper
                        visible: false
                        width: 0; height: 0
                    }
                }

                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

                // Permission-gated editing (name / topic).
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing8
                    visible: app.roomInfo.canEditName || app.roomInfo.canEditTopic
                             || app.roomInfo.canEditAvatar

                    Label {
                        text: qsTr("Edit room")
                        color: AppTheme.textSecondary
                        font.pixelSize: AppTheme.textBody
                        font.weight: AppTheme.weightStrong
                    }
                    // A Flow so the second button wraps instead of overflowing
                    // a narrow panel.
                    Flow {
                        visible: app.roomInfo.canEditAvatar
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        AppButton {
                            text: qsTr("Change avatar…")
                            enabled: !app.roomInfo.editPending
                            onClicked: avatarDialog.open()
                        }
                        AppButton {
                            kind: "danger"
                            text: qsTr("Remove avatar")
                            enabled: !app.roomInfo.editPending
                            onClicked: app.roomInfo.removeRoomAvatar()
                        }
                    }
                    RowLayout {
                        visible: app.roomInfo.canEditName
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        AppTextField {
                            id: editName
                            // Yields so the Save button keeps its size.
                            Layout.minimumWidth: 60
                            Layout.fillWidth: true
                            placeholderText: qsTr("Room name")
                            // Show the start of a long value; a TextField parks
                            // the cursor at the end when its text is set. Only
                            // while not typing.
                            onTextChanged: if (!activeFocus) cursorPosition = 0
                            Component.onCompleted: cursorPosition = 0
                            text: root.roomData.name || ""
                        }
                        AppButton {
                            kind: "primary"
                            text: qsTr("Save")
                            enabled: !app.roomInfo.editPending
                                     && editName.text.trim().length > 0
                                     && editName.text !== (root.roomData.name || "")
                            onClicked: app.roomInfo.setRoomName(editName.text)
                        }
                    }
                    RowLayout {
                        visible: app.roomInfo.canEditTopic
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        AppTextField {
                            id: editTopic
                            Layout.minimumWidth: 60
                            Layout.fillWidth: true
                            placeholderText: qsTr("Topic")
                            // Show the start of a long value; a TextField parks
                            // the cursor at the end when its text is set. Only
                            // while not typing.
                            onTextChanged: if (!activeFocus) cursorPosition = 0
                            Component.onCompleted: cursorPosition = 0
                            text: root.roomData.topic || ""
                        }
                        AppButton {
                            kind: "primary"
                            text: qsTr("Save")
                            enabled: !app.roomInfo.editPending
                                     && editTopic.text !== (root.roomData.topic || "")
                            onClicked: app.roomInfo.setRoomTopic(editTopic.text)
                        }
                    }
                    Label {
                        visible: app.roomInfo.editError.length > 0
                        Layout.fillWidth: true
                        text: app.roomInfo.editError
                        color: AppTheme.danger
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                        wrapMode: Text.WordWrap
                        font.pixelSize: AppTheme.textBody
                    }
                    AppBusyIndicator {
                        visible: app.roomInfo.editPending
                        running: visible
                        size: 18
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: AppTheme.border
                    visible: roomAdminBlock.visible
                }

                // Room administration: join rule and published address. Both
                // are room state, gated on the SDK's power-level check for that
                // event.
                ColumnLayout {
                    id: roomAdminBlock
                    objectName: "roomAdminBlock"
                    Layout.fillWidth: true
                    Layout.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing8
                    visible: app.roomInfo.canChangeJoinRule
                             || app.roomInfo.canChangeAlias
                             || app.roomInfo.canChangeHistoryVisibility
                             || app.roomInfo.canChangeGuestAccess
                    // Directory visibility is not room state: ask the server
                    // while shown.
                    onVisibleChanged: if (visible) app.roomInfo.requestDirectoryVisibility()
                    Connections {
                        target: app.roomInfo
                        function onRoomIdChanged() {
                            if (roomAdminBlock.visible)
                                app.roomInfo.requestDirectoryVisibility()
                        }
                    }

                // Your profile in this room: the per-room member name and
                // avatar overriding the global ones. Empty restores the global
                // profile. Built like the "Edit room" group (ColumnLayout with
                // margins), which stays inside its background at narrow widths.
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: AppTheme.border
                    visible: app.roomInfo.roomProfilesSupported
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing8
                    visible: app.roomInfo.roomProfilesSupported

                    Label {
                        text: qsTr("Your profile in this room")
                        color: AppTheme.textSecondary
                        font.pixelSize: AppTheme.textBody
                        font.weight: AppTheme.weightStrong
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                        text: qsTr("Only affects this room. Everyone here sees "
                                   + "it; other rooms keep your usual name and "
                                   + "picture.")
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.textMeta
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        AppTextField {
                            id: myRoomNameField
                            objectName: "roomProfileNameField"
                            Layout.fillWidth: true
                            // Otherwise the field's implicit width is a floor
                            // and squeezes Save.
                            Layout.minimumWidth: 0
                            enabled: !app.roomInfo.roomProfilePending
                            placeholderText: qsTr("Your name in this room")
                            // Show the start of a long value; a TextField parks
                            // the cursor at the end when its text is set. Only
                            // while not typing.
                            onTextChanged: if (!activeFocus) cursorPosition = 0
                            Component.onCompleted: cursorPosition = 0
                        }
                        AppButton {
                            text: qsTr("Save")
                            enabled: !app.roomInfo.roomProfilePending
                                     && myRoomNameField.text.length > 0
                            onClicked: app.roomInfo.setMyRoomDisplayName(
                                myRoomNameField.text)
                        }
                    }
                    // A Flow so the second button wraps instead of overflowing.
                    Flow {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        AppButton {
                            text: qsTr("Change picture…")
                            kind: "ghost"
                            size: "sm"
                            enabled: !app.roomInfo.roomProfilePending
                            onClicked: myAvatarDialog.open()
                        }
                        AppButton {
                            objectName: "roomProfileResetButton"
                            text: qsTr("Reset to global")
                            kind: "ghost"
                            size: "sm"
                            enabled: !app.roomInfo.roomProfilePending
                            onClicked: {
                                myRoomNameField.text = ""
                                app.roomInfo.setMyRoomDisplayName("")
                                app.roomInfo.clearMyRoomAvatar()
                            }
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: app.roomInfo.roomProfileError.length > 0
                        wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                        text: app.roomInfo.roomProfileError
                        color: AppTheme.danger
                        font.pixelSize: AppTheme.textMeta
                    }
                }

                    Label {
                        text: qsTr("Access")
                        color: AppTheme.textSecondary
                        font.pixelSize: AppTheme.textBody
                        font.weight: AppTheme.weightStrong
                    }

                    // Join rule. The settable rules are those needing no extra
                    // configuration; a space-restricted rule is shown as-is
                    // (see below).
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        visible: app.roomInfo.canChangeJoinRule
                        readonly property bool restricted:
                            app.roomInfo.joinRule === "restricted"
                            || app.roomInfo.joinRule === "knock_restricted"
                        Label {
                            text: qsTr("Who can join")
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.textMeta
                        }
                        AppComboBox {
                            id: joinRuleCombo
                            objectName: "roomJoinRuleCombo"
                            Layout.fillWidth: true
                            visible: !parent.restricted
                            // Index order must match ruleValues below.
                            model: [
                                qsTr("Invited people only"),
                                qsTr("Anyone with the link"),
                                qsTr("Ask to join (knock)")
                            ]
                            readonly property var ruleValues:
                                ["invite", "public", "knock"]
                            // An explicit mirror, so a rejected write snaps
                            // back to what the room holds; a binding broken by
                            // the user's selection could not.
                            property int displayedIndex: 0
                            function refreshRule() {
                                var idx = ruleValues.indexOf(
                                    app.roomInfo.joinRule)
                                displayedIndex = idx >= 0 ? idx : 0
                            }
                            Component.onCompleted: refreshRule()
                            currentIndex: displayedIndex
                            enabled: !app.roomInfo.editPending
                            Connections {
                                target: app.roomInfo
                                function onMembersChanged() {
                                    joinRuleCombo.refreshRule()
                                }
                            }
                            onActivated: (index) => {
                                app.roomInfo.setJoinRule(
                                    joinRuleCombo.ruleValues[index])
                            }
                        }
                        // Space-restricted access. The kind (restricted or
                        // knock + restricted) and the allowed spaces are one
                        // write; allow rules this client cannot show are
                        // preserved on save and disclosed. `checked` inside the
                        // toggled handler is the value before the flip.
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing8
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Members of selected spaces can join")
                                color: AppTheme.text
                                font.pixelSize: AppTheme.textBody
                                wrapMode: Text.WordWrap
                            }
                            AppSwitch {
                                id: restrictedSwitch
                                objectName: "roomRestrictedSwitch"
                                checked: restrictedSwitch.parent.parent.restricted
                                enabled: !app.roomInfo.editPending
                                onToggled: {
                                    if (!checked) {
                                        var ids = restrictedPicker.selectedIds()
                                        if (ids.length === 0) {
                                            // Nothing chosen yet: open the picker
                                            // first.
                                            restrictedPicker.expanded = true
                                            return
                                        }
                                        app.roomInfo.setRestrictedJoinRule(
                                            "restricted", ids)
                                    } else {
                                        app.roomInfo.setJoinRule("invite")
                                    }
                                }
                            }
                        }
                        ColumnLayout {
                            id: restrictedPicker
                            objectName: "roomRestrictedPicker"
                            Layout.fillWidth: true
                            spacing: 2
                            property bool expanded: false
                            visible: parent.restricted || expanded
                            function selectedIds() {
                                var ids = []
                                for (var i = 0; i < spaceRepeater.count; ++i) {
                                    var row = spaceRepeater.itemAt(i)
                                    if (row && row.checked)
                                        ids.push(row.spaceId)
                                }
                                return ids
                            }
                            function apply(kind) {
                                var ids = selectedIds()
                                if (ids.length === 0)
                                    return
                                app.roomInfo.setRestrictedJoinRule(kind, ids)
                            }
                            Label {
                                text: qsTr("Spaces whose members may join")
                                color: AppTheme.textMuted
                                font.pixelSize: AppTheme.textMeta
                            }
                            Repeater {
                                id: spaceRepeater
                                model: app.roomInfo.joinedSpaces
                                delegate: RowLayout {
                                    id: spaceRow
                                    required property var modelData
                                    readonly property string spaceId: modelData.roomId
                                    // Local selection, seeded from the server's
                                    // list and re-seeded after every roster
                                    // refresh.
                                    property bool checked:
                                        app.roomInfo.restrictedAllowedRooms
                                            .indexOf(modelData.roomId) >= 0
                                    Connections {
                                        target: app.roomInfo
                                        function onMembersChanged() {
                                            spaceRow.checked =
                                                app.roomInfo.restrictedAllowedRooms
                                                    .indexOf(spaceRow.spaceId) >= 0
                                        }
                                    }
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    Label {
                                        Layout.fillWidth: true
                                        text: spaceRow.modelData.name
                                        textFormat: Text.PlainText
                                        color: AppTheme.text
                                        font.pixelSize: AppTheme.textBody
                                        elide: Label.ElideRight
                                    }
                                    AppSwitch {
                                        checked: spaceRow.checked
                                        enabled: !app.roomInfo.editPending
                                        onToggled: {
                                            spaceRow.checked = !spaceRow.checked
                                            if (restrictedPicker.parent.restricted)
                                                restrictedPicker.apply(
                                                    app.roomInfo.joinRule)
                                        }
                                    }
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: app.roomInfo.joinedSpaces.length === 0
                                wrapMode: Text.WordWrap
                                color: AppTheme.textMuted
                                font.pixelSize: AppTheme.textMeta
                                text: qsTr("Join a space first; only spaces "
                                           + "you are in can be chosen.")
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: app.roomInfo.restrictedHasUnknownRules
                                wrapMode: Text.WordWrap
                                color: AppTheme.textMuted
                                font.pixelSize: AppTheme.textMeta
                                text: qsTr("This room also allows joins by a "
                                           + "rule Lightning can't show. It "
                                           + "is kept when you save.")
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                visible: restrictedPicker.parent.restricted
                                spacing: AppTheme.spacing8
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Others may ask to join (knock)")
                                    color: AppTheme.text
                                    font.pixelSize: AppTheme.textBody
                                    wrapMode: Text.WordWrap
                                }
                                AppSwitch {
                                    objectName: "roomKnockRestrictedSwitch"
                                    checked: app.roomInfo.joinRule === "knock_restricted"
                                    enabled: !app.roomInfo.editPending
                                    onToggled: restrictedPicker.apply(
                                                   !checked ? "knock_restricted"
                                                            : "restricted")
                                }
                            }
                            AppButton {
                                visible: !restrictedPicker.parent.restricted
                                kind: "primary"
                                size: "sm"
                                text: qsTr("Restrict to these spaces")
                                enabled: !app.roomInfo.editPending
                                         && restrictedPicker.selectedIds().length > 0
                                onClicked: restrictedPicker.apply("restricted")
                            }
                        }
                    }

                    // History visibility (m.room.history_visibility).
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        visible: app.roomInfo.canChangeHistoryVisibility
                        Label {
                            text: qsTr("Who can read history")
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.textMeta
                        }
                        AppComboBox {
                            id: historyCombo
                            objectName: "roomHistoryVisibilityCombo"
                            Layout.fillWidth: true
                            model: [
                                qsTr("Members, from when they were invited"),
                                qsTr("Members, from when they joined"),
                                qsTr("Members, everything"),
                                qsTr("Anyone, even without joining")
                            ]
                            readonly property var values:
                                ["invited", "joined", "shared", "world_readable"]
                            property int displayedIndex: 2
                            function refresh() {
                                var idx = values.indexOf(
                                    app.roomInfo.historyVisibility)
                                displayedIndex = idx >= 0 ? idx : 2
                            }
                            Component.onCompleted: refresh()
                            currentIndex: displayedIndex
                            enabled: !app.roomInfo.editPending
                            Connections {
                                target: app.roomInfo
                                function onMembersChanged() { historyCombo.refresh() }
                            }
                            onActivated: (index) =>
                                app.roomInfo.setHistoryVisibility(
                                    historyCombo.values[index])
                        }
                    }

                    // Guest access (m.room.guest_access).
                    RowLayout {
                        Layout.fillWidth: true
                        visible: app.roomInfo.canChangeGuestAccess
                        spacing: AppTheme.spacing8
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Guests can join")
                            color: AppTheme.text
                            font.pixelSize: AppTheme.textBody
                        }
                        AppSwitch {
                            objectName: "roomGuestAccessSwitch"
                            checked: app.roomInfo.guestAccess === "can_join"
                            enabled: !app.roomInfo.editPending
                            onToggled: app.roomInfo.setGuestAccess(
                                           !checked ? "can_join" : "forbidden")
                        }
                    }

                    // Directory visibility (the server's public room list).
                    RowLayout {
                        Layout.fillWidth: true
                        visible: app.roomInfo.canChangeAlias
                        spacing: AppTheme.spacing8
                        Label {
                            Layout.fillWidth: true
                            text: app.roomInfo.directoryPublished < 0
                                  ? qsTr("Listed in the room directory (checking…)")
                                  : qsTr("Listed in the room directory")
                            color: AppTheme.text
                            font.pixelSize: AppTheme.textBody
                            wrapMode: Text.WordWrap
                        }
                        AppSwitch {
                            objectName: "roomDirectorySwitch"
                            checked: app.roomInfo.directoryPublished === 1
                            enabled: !app.roomInfo.editPending
                                     && app.roomInfo.directoryPublished >= 0
                            onToggled: app.roomInfo.setDirectoryPublished(!checked)
                        }
                    }

                    // Room version and upgrade. The Upgrade control appears
                    // only for someone allowed to send m.room.tombstone, and
                    // opens a confirmation.
                    RowLayout {
                        Layout.fillWidth: true
                        visible: app.roomInfo.roomVersion.length > 0
                        spacing: AppTheme.spacing8
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Room version %1").arg(app.roomInfo.roomVersion)
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.textMeta
                        }
                        AppButton {
                            objectName: "roomUpgradeButton"
                            visible: app.roomInfo.canUpgradeRoom
                                     && !app.roomUpgrade.upgraded
                            kind: "secondary"
                            size: "sm"
                            text: qsTr("Upgrade room…")
                            onClicked: {
                                roomUpgradeDialog.kind = "room"
                                // The inspected room, which may not be the open
                                // one (a Space home).
                                roomUpgradeDialog.openFor(app.roomInfo.roomId)
                            }
                        }
                    }
                    RoomUpgradeDialog { id: roomUpgradeDialog }

                    // Canonical alias; the controller completes a bare
                    // localpart with the account's server.
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        visible: app.roomInfo.canChangeAlias
                        Label {
                            text: qsTr("Published address")
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.textMeta
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing8
                            AppTextField {
                                id: editAlias
                                Layout.minimumWidth: 60
                                objectName: "roomAliasField"
                                Layout.fillWidth: true
                                placeholderText: qsTr("#room-name")
                                // An explicit mirror, not a binding to the
                                // controller: the first keystroke would break
                                // the binding, and this panel outlives a room
                                // change, so text typed for one room could be
                                // saved to another.
                                property string authoritative: ""
                                // A room change always wins, even mid-edit.
                                function resetForRoom() {
                                    authoritative =
                                        app.roomInfo.canonicalAlias
                                    text = authoritative
                                }
                                // A roster refresh resnaps only when the user
                                // has not edited.
                                function refreshAlias() {
                                    var next = app.roomInfo.canonicalAlias
                                    if (text === authoritative)
                                        text = next
                                    authoritative = next
                                }
                                Component.onCompleted: resetForRoom()
                                Connections {
                                    target: app.roomInfo
                                    function onRoomIdChanged() {
                                        editAlias.resetForRoom()
                                    }
                                    function onMembersChanged() {
                                        editAlias.refreshAlias()
                                    }
                                }
                            }
                            AppButton {
                                kind: "primary"
                                text: qsTr("Save")
                                enabled: !app.roomInfo.editPending
                                         && editAlias.text.trim()
                                            !== app.roomInfo.canonicalAlias
                                onClicked:
                                    app.roomInfo.setCanonicalAlias(
                                        editAlias.text)
                            }
                        }
                        // Alternative addresses, written as one list. Removing
                        // one drops it from room state but keeps its directory
                        // mapping (see set_room_alt_aliases).
                        Label {
                            Layout.topMargin: AppTheme.spacing4
                            text: qsTr("Alternative addresses")
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.textMeta
                        }
                        Repeater {
                            model: app.roomInfo.altAliases
                            delegate: RowLayout {
                                required property string modelData
                                Layout.fillWidth: true
                                spacing: AppTheme.spacing8
                                Label {
                                    // Untrusted text: never markup.
                                    textFormat: Text.PlainText
                                    Layout.fillWidth: true
                                    text: modelData
                                    color: AppTheme.text
                                    font.pixelSize: AppTheme.textBody
                                    elide: Label.ElideMiddle
                                }
                                AppButton {
                                    kind: "ghost"
                                    size: "sm"
                                    text: qsTr("Remove")
                                    enabled: !app.roomInfo.editPending
                                    onClicked: {
                                        var next = app.roomInfo.altAliases
                                                       .filter(function (a) {
                                                           return a !== modelData
                                                       })
                                        app.roomInfo.setAltAliases(next)
                                    }
                                }
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing8
                            AppTextField {
                                id: newAltAlias
                                objectName: "roomAltAliasField"
                                Layout.fillWidth: true
                                Layout.minimumWidth: 60
                                placeholderText: qsTr("#another-name")
                                onAccepted: addAltAlias.clicked()
                            }
                            AppButton {
                                id: addAltAlias
                                kind: "secondary"
                                size: "sm"
                                text: qsTr("Add")
                                enabled: !app.roomInfo.editPending
                                         && newAltAlias.text.trim().length > 0
                                onClicked: {
                                    var next = app.roomInfo.altAliases.slice()
                                    next.push(newAltAlias.text.trim())
                                    app.roomInfo.setAltAliases(next)
                                    newAltAlias.text = ""
                                }
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            wrapMode: Text.WordWrap
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("Publishing an address lets people "
                                       + "find and join this room by name. "
                                       + "Leave it empty to remove it.")
                        }
                    }
                }

                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

                // Leave room.
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing8

                    AppButton {
                        kind: "danger"
                        text: qsTr("Leave room")
                        enabled: !app.roomInfo.leavePending
                        onClicked: leaveConfirm.open()
                    }
                    Label {
                        visible: app.roomInfo.leaveError.length > 0
                        Layout.fillWidth: true
                        text: app.roomInfo.leaveError
                        color: AppTheme.danger
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                        wrapMode: Text.WordWrap
                        font.pixelSize: AppTheme.textBody
                    }
                }
                Item { Layout.preferredHeight: AppTheme.spacing16 }
            }
        }

        // Pinned. The list is m.room.pinned_events: nothing stored locally, and
        // an unresolvable entry renders as an unavailable row.
        ColumnLayout {
            visible: root.section === "pinned" && root.pinnedAvailable
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: AppTheme.spacing8

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: AppTheme.spacing12
                Layout.bottomMargin: 0
                spacing: AppTheme.spacing8
                Label {
                    Layout.fillWidth: true
                    text: app.pinned.total === 0
                          ? qsTr("No pinned messages")
                          : qsTr("%n pinned message(s)", "", app.pinned.total)
                    color: AppTheme.textSecondary
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightStrong
                }
                AppBusyIndicator {
                    visible: app.pinned.loading || app.pinned.pending
                    running: visible
                    size: 16
                }
            }

            Label {
                Layout.fillWidth: true
                Layout.leftMargin: AppTheme.spacing12
                Layout.rightMargin: AppTheme.spacing12
                visible: app.pinned.truncated
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                wrapMode: Text.WordWrap
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
                text: qsTr("Showing the most recent pins. This room pins "
                           + "more than Lightning loads at once.")
            }

            Label {
                Layout.fillWidth: true
                Layout.leftMargin: AppTheme.spacing12
                Layout.rightMargin: AppTheme.spacing12
                visible: app.pinned.error.length > 0
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                wrapMode: Text.WordWrap
                text: app.pinned.error
                color: AppTheme.danger
                font.pixelSize: AppTheme.textBody
            }

            ListView {
                objectName: "pinnedMessagesList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 2
                // Same wheel/touchpad feel as the timeline.
                SmoothWheelArea {}
                // Newest pin first: Matrix appends, so the tail is the useful
                // end.
                model: {
                    var out = []
                    var src = app.pinned.entries
                    for (var i = src.length - 1; i >= 0; --i)
                        out.push(src[i])
                    return out
                }
                delegate: Item {
                    id: pinDelegate
                    width: ListView.view.width
                    height: pinRow.implicitHeight + AppTheme.spacing16

                    required property var modelData

                    readonly property bool resolved:
                        pinDelegate.modelData.available === true
                    readonly property string senderName:
                        pinDelegate.modelData.senderDisplayName
                        || (pinDelegate.modelData.sender || "")
                    readonly property string kind:
                        pinDelegate.modelData.kind || ""

                    HoverHandler { id: pinHover }
                    Rectangle {
                        anchors.fill: parent
                        color: AppTheme.hover
                        visible: pinHover.hovered && pinDelegate.resolved
                    }
                    // Only a resolved pin navigates.
                    TapHandler {
                        enabled: pinDelegate.resolved
                        onTapped: root.jumpToEventRequested(
                                      pinDelegate.modelData.eventId)
                    }

                    RowLayout {
                        id: pinRow
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: AppTheme.spacing12
                        anchors.rightMargin: AppTheme.spacing8
                        spacing: AppTheme.spacing8

                        Avatar {
                            size: 28
                            circle: true
                            visible: pinDelegate.resolved
                            name: pinDelegate.senderName
                            mxc: pinDelegate.modelData.senderAvatarUrl || ""
                            colorKey: pinDelegate.modelData.sender || ""
                        }
                        Icon {
                            visible: !pinDelegate.resolved
                            name: "warning"
                            size: 20
                            color: AppTheme.textMuted
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: AppTheme.spacing8
                                Label {
                                    Layout.fillWidth: true
                                    elide: Label.ElideRight
                                    text: pinDelegate.resolved
                                          ? pinDelegate.senderName
                                          : qsTr("Message unavailable")
                                    textFormat: Text.PlainText
                                    // Identity ink; an unresolved pin has no
                                    // sender and keeps the neutral ink.
                                    color: pinDelegate.resolved
                                           ? AppTheme.userColor(
                                                 pinDelegate.modelData.sender || "")
                                           : AppTheme.textPrimary
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                }
                                Label {
                                    visible: pinDelegate.resolved
                                             && pinDelegate.modelData
                                                .timestampMs > 0
                                    text: new Date(pinDelegate.modelData
                                                   .timestampMs)
                                          .toLocaleDateString(
                                              Qt.locale(), Locale.ShortFormat)
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.textMeta
                                }
                            }
                            Label {
                                // Untrusted text: never markup.
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                elide: Label.ElideRight
                                maximumLineCount: 2
                                lineHeight: AppTheme.lineHeightBody
                                lineHeightMode: Text.ProportionalHeight
                                wrapMode: Text.WordWrap
                                color: AppTheme.textSecondary
                                font.pixelSize: AppTheme.textBody
                                // Typed labels for media and non-text pins;
                                // deleted or still-encrypted pins say so.
                                text: {
                                    if (!pinDelegate.resolved) {
                                        return qsTr("It may have been deleted, "
                                                    + "or this account cannot "
                                                    + "see it.")
                                    }
                                    var k = pinDelegate.kind
                                    if (k === "redacted")
                                        return qsTr("Message deleted")
                                    if (k === "encrypted")
                                        return qsTr("Can't decrypt this yet")
                                    var p = pinDelegate.modelData.preview || ""
                                    if (k === "image")
                                        return p.length > 0
                                            ? qsTr("Image · %1").arg(p)
                                            : qsTr("Image")
                                    if (k === "video")
                                        return p.length > 0
                                            ? qsTr("Video · %1").arg(p)
                                            : qsTr("Video")
                                    if (k === "audio")
                                        return p.length > 0
                                            ? qsTr("Audio · %1").arg(p)
                                            : qsTr("Audio")
                                    if (k === "file")
                                        return p.length > 0
                                            ? qsTr("File · %1").arg(p)
                                            : qsTr("File")
                                    if (k === "sticker")
                                        return p.length > 0
                                            ? qsTr("Sticker · %1").arg(p)
                                            : qsTr("Sticker")
                                    return p.length > 0 ? p : qsTr("Message")
                                }
                            }
                        }

                        // Unpin stays available for an unavailable pin: it
                        // needs only the id.
                        IconButton {
                            iconName: "close"
                            iconSize: 18
                            implicitWidth: 28
                            implicitHeight: 28
                            visible: app.pinned.canPin
                                     && (pinHover.hovered
                                         || !pinDelegate.resolved)
                            enabled: !app.pinned.pending
                            Accessible.name: qsTr("Unpin this message")
                            ToolTip.text: qsTr("Unpin")
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            onClicked: app.pinned.unpin(
                                           pinDelegate.modelData.eventId)
                        }
                    }
                }
            }
        }

        // People: a count, membership filter, A-to-Z toggle and search, then
        // the roster grouped by power level. Groups come from C++
        // (memberRoleRows): which roles a room has is a model fact (a custom
        // level gets its own heading), and the flat list lets the view
        // virtualise large rosters.
        ColumnLayout {
            id: memberSection
            visible: root.section === "people"
            Layout.fillWidth: true
            Layout.fillHeight: true
            // Tight: every point of spacing is one fewer person on screen.
            spacing: AppTheme.spacing4

            // Calling memberRoleRows() creates no dependency; reading this
            // counter makes a new roster reach the list.
            property int rosterTick: 0
            Connections {
                target: app.roomInfo
                function onMembersChanged() { memberSection.rosterTick++ }
            }

            /// "" (everyone) | "joined" | "invited" | "banned". Anything else
            /// matches nothing.
            property string membership: "joined"
            property bool alphabetical: false

            // One row of chrome: search, filter, sort and Invite, with the
            // toggles as icons so the roster starts near the top.
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: AppTheme.spacing8
                Layout.rightMargin: AppTheme.spacing8
                Layout.topMargin: AppTheme.spacing6
                spacing: AppTheme.spacing4

                AppTextField {
                    id: memberSearch
                    Layout.fillWidth: true
                    // The field yields; the icon buttons keep their size.
                    Layout.minimumWidth: 40
                    // Follows the text-size setting.
                    implicitHeight: Math.max(30, AppTheme.scaled(AppTheme.textBody) + 16)
                    searchIcon: true
                    clearButton: true
                    placeholderText: qsTr("Type name…")
                    onTextChanged: root.memberFilter = text
                }
                // Membership filter as a cycling toggle (no room for a
                // dropdown). `active` marks a non-default filter.
                IconButton {
                    size: "sm"
                    iconName: "person_search"
                    active: memberSection.membership !== "joined"
                    Accessible.name: qsTr("Filter members by membership")
                    ToolTip.text: memberSection.membership === "joined"
                                  ? qsTr("Showing joined members — click for invited")
                                  : memberSection.membership === "invited"
                                    ? qsTr("Showing invited members — click for banned")
                                    : memberSection.membership === "banned"
                                      ? qsTr("Showing banned members — click for everyone")
                                      : qsTr("Showing everyone — click for joined")
                    ToolTip.visible: hovered
                    ToolTip.delay: 400
                    onClicked: {
                        memberSection.membership =
                            memberSection.membership === "joined" ? "invited"
                            : memberSection.membership === "invited" ? "banned"
                            : memberSection.membership === "banned" ? ""
                            : "joined"
                    }
                }
                // A-to-Z, off by default. The snapshot is sorted by power level
                // and then capped, so an alphabetical view of a truncated
                // roster is missing names from the middle; `truncated` says so.
                IconButton {
                    size: "sm"
                    iconName: "unfold_more"
                    active: memberSection.alphabetical
                    Accessible.name: qsTr("Sort members alphabetically")
                    ToolTip.text: memberSection.alphabetical
                                  ? qsTr("Sorted A to Z — click for role order")
                                  : qsTr("Sorted by role — click for A to Z")
                    ToolTip.visible: hovered
                    ToolTip.delay: 400
                    onClicked: memberSection.alphabetical = !memberSection.alphabetical
                }
                IconButton {
                    size: "sm"
                    iconName: "person_add"
                    visible: app.roomInfo.canInvite
                    Accessible.name: qsTr("Invite people to this room")
                    ToolTip.text: qsTr("Invite people")
                    ToolTip.visible: hovered
                    ToolTip.delay: 400
                    onClicked: inviteDialog.openFor(app.roomInfo.roomId)
                }
            }

            // Loaders: an empty Text that is never laid out keeps
            // ItemObservesViewport, and these are usually empty.
            Loader {
                active: app.roomInfo.loading
                visible: active
                Layout.leftMargin: AppTheme.spacing12
                sourceComponent: Label {
                    text: qsTr("Loading members…")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                    wrapMode: Text.WordWrap
                }
            }
            Loader {
                active: app.roomInfo.truncated
                visible: active
                Layout.leftMargin: AppTheme.spacing12
                Layout.rightMargin: AppTheme.spacing12
                Layout.fillWidth: true
                sourceComponent: Label {
                    text: qsTr("Showing the first %1 members of %2.")
                          .arg(app.roomInfo.members.length)
                          .arg(app.roomInfo.joinedCount + app.roomInfo.invitedCount)
                    color: AppTheme.textMuted
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    wrapMode: Text.WordWrap
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                }
            }

            ListView {
                id: memberList
                objectName: "memberRoleList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                // Air between names so rows do not merge.
                spacing: 3
                // One flat list of headers and members, so it virtualises.
                model: {
                    var _t = memberSection.rosterTick
                    return app.roomInfo.memberRoleRows(root.memberFilter,
                                                       memberSection.membership,
                                                       memberSection.alphabetical)
                }
                ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
                // Same wheel/touchpad feel as the timeline.
                SmoothWheelArea {}

                delegate: Loader {
                    id: memberLoader
                    width: memberList.width
                    required property var modelData
                    sourceComponent: modelData.kind === "header"
                                     ? roleHeaderComponent : memberRowComponent

                    Component {
                        id: roleHeaderComponent
                        Item {
                            width: memberList.width
                            // Group heading, sized to its text.
                            height: Math.max(24, AppTheme.scaled(AppTheme.textSubtitle) + 10)
                            MenuSectionLabel {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.leftMargin: AppTheme.spacing12
                                anchors.rightMargin: AppTheme.spacing12
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 2
                                // Follows the text size, one step under the
                                // names.
                                font.pixelSize: AppTheme.scaled(AppTheme.textSubtitle)
                                text: qsTr("%1 — %2")
                                          .arg(memberLoader.modelData.label)
                                          .arg(memberLoader.modelData.count)
                            }
                        }
                    }

                    Component {
                        id: memberRowComponent
                        ItemDelegate {
                            width: memberList.width
                            // Sized to the text so the row follows the
                            // text-size setting.
                            height: Math.max(34, AppTheme.scaled(AppTheme.textTitle) + 16)
                            padding: 0
                            hoverEnabled: true
                            readonly property var member: memberLoader.modelData
                            Accessible.name: member.displayName.length > 0
                                             ? qsTr("%1 (%2), %3")
                                                   .arg(member.displayName)
                                                   .arg(member.userId)
                                                   .arg(member.roleLabel)
                                             : member.userId
                            onClicked: memberProfile.openFor(member)
                            // Right-click: the moderation actions without
                            // opening the profile first.
                            TapHandler {
                                acceptedButtons: Qt.RightButton
                                onTapped: (eventPoint) => root.openMemberMenu(
                                    member, parent, eventPoint.position.x,
                                    eventPoint.position.y)
                            }
                            background: Rectangle {
                                anchors.fill: parent
                                anchors.leftMargin: AppTheme.spacing8
                                anchors.rightMargin: AppTheme.spacing8
                                radius: AppTheme.radiusSm
                                color: parent.hovered ? AppTheme.hover
                                                      : "transparent"
                            }
                            contentItem: RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: AppTheme.spacing12
                                anchors.rightMargin: AppTheme.spacing12
                                spacing: AppTheme.spacing8
                                Avatar {
                                    // Follows the text size.
                                    readonly property int px:
                                        Math.max(24, AppTheme.scaled(AppTheme.textTitle) + 8)
                                    width: px; height: px
                                    size: px
                                    name: member.displayName.length > 0
                                          ? member.displayName : member.userId
                                    mxc: member.avatarUrl || ""
                                    colorKey: member.userId || ""

                                    // Banned members get no dot. The userId is
                                    // gated on the People section being shown:
                                    // this panel is not behind a Loader and
                                    // cached delegates would keep watching
                                    // presence.
                                    PresenceDot {
                                        anchors.right: parent.right
                                        anchors.bottom: parent.bottom
                                        anchors.margins: -1
                                        dotSize: 8
                                        ring: AppTheme.sidebar
                                        userId: root.visible
                                                && root.section === "people"
                                                && member.membership !== "banned"
                                                ? (member.userId || "") : ""
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: member.displayName.length > 0
                                          ? member.displayName : member.userId
                                    textFormat: Text.PlainText
                                    // Identity ink.
                                    color: AppTheme.userColor(member.userId || "")
                                    // Scaled like every other size in this
                                    // panel; textTitle is one step above body,
                                    // since a name is what this list is scanned
                                    // for.
                                    font.pixelSize: AppTheme.scaled(AppTheme.textTitle)
                                    elide: Label.ElideRight
                                }
                                // Invited and banned rows are listed (a hidden
                                // ban cannot be lifted) and marked. The role is
                                // the group heading, not repeated per row.
                                StatusChip {
                                    visible: member.membership === "invited"
                                    tone: "warning"
                                    label: qsTr("Invited")
                                }
                                StatusChip {
                                    visible: member.membership === "banned"
                                    tone: "danger"
                                    label: qsTr("Banned")
                                }
                            }
                            ToolTip.text: member.userId
                            ToolTip.visible: hovered
                            ToolTip.delay: 600
                        }
                    }
                }
            }
        }

        // ── Widgets ──────────────────────────────────────────────
        // In their own tab (Overview is already full).
        Flickable {
            visible: root.section === "widgets"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: widgetCol.implicitHeight + AppTheme.spacing16 * 2
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: AppScrollBar {}
            // No anchors: SmoothWheelArea attaches to its parent Flickable.
            SmoothWheelArea {}
            ColumnLayout {
                id: widgetCol
                x: AppTheme.spacing12
                y: AppTheme.spacing12
                width: parent.width - AppTheme.spacing12 * 2
                spacing: AppTheme.spacing8

                // Lightning lists widgets and opens them in the browser; it
                // does not embed them (see docs/widgets.md). The empty state is
                // an answer, distinct from a missing tab.
                Label {
                    visible: app.widgets.supported && app.widgets.count === 0
                    Layout.fillWidth: true
                    text: qsTr("No widgets in this room.")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    wrapMode: Text.Wrap
                }
                // Adding a widget: offered only when the room's power levels
                // allow writing widget state, and not while a write is in
                // flight. Opened in the browser like any other. Failures are
                // reported here.
                Label {
                    objectName: "roomInfoWidgetWriteError"
                    Layout.fillWidth: true
                    visible: app.widgets.lastWriteError.length > 0
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    color: AppTheme.danger
                    font.pixelSize: AppTheme.textMeta
                    text: app.widgets.lastWriteError === "forbidden"
                          ? qsTr("You do not have permission to change widgets here.")
                          : qsTr("Could not change the widgets (%1).").arg(app.widgets.lastWriteError)
                }
                AppButton {
                    objectName: "roomInfoAddWidgetButton"
                    visible: app.widgets.supported && app.widgets.canManage
                    enabled: !app.widgets.writing
                    text: qsTr("Add widget…")
                    iconName: "add"
                    onClicked: addWidgetDialog.openDialog()
                }
                Repeater {
                    model: app.widgets.supported ? app.widgets : null
                    RowLayout {
                        id: widgetRow
                        required property int index
                        required property string name
                        required property string kind
                        required property string refusal
                        required property bool openable
                        required property bool removable
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        Icon {
                            name: "explore"
                            size: 18
                            color: widgetRow.openable ? AppTheme.icon
                                                      : AppTheme.textDisabled
                            Layout.alignment: Qt.AlignTop
                            Layout.topMargin: 2
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label {
                                Layout.fillWidth: true
                                // A widget name is remote text.
                                textFormat: Text.PlainText
                                text: widgetRow.name
                                color: AppTheme.text
                                elide: Label.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                textFormat: Text.PlainText
                                // A refused widget still shows, with the
                                // reason.
                                text: widgetRow.openable
                                      ? widgetRow.kind
                                      : app.widgets.refusalText(widgetRow.refusal)
                                color: widgetRow.openable
                                       ? AppTheme.textMuted
                                       : AppTheme.danger
                                // Scaled like every other size in this panel.
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                            }
                        }
                        // Removed by row; QML never names a widget id.
                        AppButton {
                            objectName: "roomInfoRemoveWidgetButton"
                            visible: app.widgets.canManage && widgetRow.removable
                            enabled: !app.widgets.writing
                            kind: "ghost"
                            size: "sm"
                            text: qsTr("Remove")
                            Layout.alignment: Qt.AlignTop
                            // Removal writes a tombstone everyone sees and
                            // cannot be undone; confirm first.
                            onClicked: removeWidgetConfirm.openFor(
                                           widgetRow.index,
                                           widgetRow.name || "")
                        }
                        AppButton {
                            objectName: "roomInfoOpenWidget"
                            text: qsTr("Open")
                            enabled: widgetRow.openable
                            onClicked: widgetSheet.openFor(
                                widgetRow.index,
                                app.widgets.rowAt(widgetRow.index))
                        }
                    }
                }
                WidgetOpenSheet { id: widgetSheet }
            }
        }

        // Media, files and links: a history browser that walks /messages on its
        // own cursor. See MediaBrowser.qml and MediaHistoryModel.
        MediaBrowser {
            objectName: "mediaBrowser"
            visible: root.section === "media"
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: app.mediaHistory
            roomId: app.roomInfo.roomId
            onOpenImagesRequested: (entries, index) =>
                root.openImagesRequested(entries, index)
            // Navigation belongs to the host.
            onJumpToEventRequested: (eventId) =>
                root.jumpToEventRequested(eventId)
        }
    }

    // Leave confirmation; Cancel is the default. Widget removal confirmation
    // below.
    Dialog {
        id: removeWidgetConfirm
        objectName: "roomInfoRemoveWidgetConfirmDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.max(240, Math.min(400, parent ? parent.width - 32 : 400))
        modal: true
        standardButtons: Dialog.NoButton
        closePolicy: Popup.CloseOnEscape
        title: qsTr("Remove widget?")
        property int pendingIndex: -1
        property string pendingName: ""
        function openFor(index, name) {
            pendingIndex = index
            pendingName = name
            open()
        }
        background: Rectangle {
            color: AppTheme.surface
            border.color: AppTheme.border
            radius: AppTheme.radiusLg
        }
        contentItem: ColumnLayout {
            spacing: AppTheme.spacing12
            Label {
                Layout.fillWidth: true
                // Written by whoever added it: never markup.
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                text: removeWidgetConfirm.pendingName.length > 0
                      ? qsTr("Remove \"%1\" from this room? Everyone here loses it, and it cannot be undone.").arg(removeWidgetConfirm.pendingName)
                      : qsTr("Remove this widget from the room? Everyone here loses it, and it cannot be undone.")
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                Item { Layout.fillWidth: true }
                Button {
                    objectName: "roomInfoRemoveWidgetConfirmCancel"
                    text: qsTr("Cancel")
                    onClicked: removeWidgetConfirm.close()
                }
                Button {
                    objectName: "roomInfoRemoveWidgetConfirmAccept"
                    text: qsTr("Remove")
                    onClicked: {
                        removeWidgetConfirm.close()
                        app.widgets.removeWidget(removeWidgetConfirm.pendingIndex)
                    }
                }
            }
        }
    }
    Dialog {
        id: leaveConfirm
        parent: Overlay.overlay
        anchors.centerIn: parent
        // An explicit bounded width keeps the Dialog's sizing independent of
        // the wrapping content.
        width: Math.max(240, Math.min(400, parent ? parent.width - 32 : 400))
        modal: true
        title: qsTr("Leave room?")
        standardButtons: Dialog.NoButton
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            color: AppTheme.surface
            border.color: AppTheme.border
            radius: AppTheme.radiusLg
        }

        contentItem: ColumnLayout {
            spacing: AppTheme.spacing12
            Label {
                Layout.fillWidth: true
                text: qsTr("You will stop receiving messages from this room. "
                           + "Server history is not deleted, and you can be "
                           + "invited again later.")
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                wrapMode: Text.WordWrap
                color: AppTheme.textPrimary
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                AppButton {
                    text: qsTr("Cancel")
                    focus: true
                    onClicked: leaveConfirm.close()
                }
                AppButton {
                    kind: "danger"
                    text: qsTr("Leave room")
                    onClicked: {
                        leaveConfirm.close()
                        app.roomInfo.leaveRoom()
                    }
                }
            }
        }
    }

    PolicyListDialog {
        id: policyListDialog
    }
    AddWidgetDialog {
        id: addWidgetDialog
    }
}
