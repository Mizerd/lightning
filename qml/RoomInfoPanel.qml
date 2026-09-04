import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

// v0.5.9: right-side Room Information panel (Phase 6 surface for the
// Phase 10 invite entry point). Overview: identity, encryption state,
// permission-gated name/topic editing, Leave room with confirmation.
// People: member search, joined/invited state, roles, Invite button when
// the SDK says the user may invite. Member data is a bounded in-memory
// snapshot from RoomInfoController; nothing here is persisted.
Rectangle {
    id: root
    color: AppTheme.sidebar
    visible: width > 0
    // A LAST LINE OF DEFENCE, not a layout fix. Everything in here is meant
    // to fit the panel's width and wrap or elide when it cannot; clip is what
    // stops a control that gets that wrong from painting over the timeline —
    // or, when the panel is at the window edge, off the window entirely.
    clip: true

    property var roomData: ({})
    signal closeRequested()
    // v0.5.9: Media & Files entries delegate viewing/saving to the pane
    // that owns the viewer and the save dialog (TimelinePane).
    signal openImageRequested(string mediaKey, var httpUrl)
    signal saveMediaRequested(string mediaKey, string filename)

    // "overview" | "pinned" | "people" | "media" | "widgets"
    property string section: "overview"
    property string memberFilter: ""

    // v0.7.x pinned messages. PinnedMessagesController follows the ACTIVE
    // room, while this panel can be opened for another room entirely (a
    // Space home). Rendering its list under a different room's header would
    // be a lie, so the tab only exists when the two agree.
    readonly property bool pinnedAvailable:
        app.pinned && app.pinned.supported
        && app.roomInfo.roomId !== ""
        && app.roomInfo.roomId === app.pinned.roomId
    // A tab that disappears must not leave the panel on a blank section.
    onPinnedAvailableChanged: {
        if (!pinnedAvailable && section === "pinned")
            section = "overview"
    }
    // Widgets get their own TAB rather than a block in Overview. Overview
    // already carries notifications, the avatar and topic, the member count,
    // export, room id, the whole Edit room group and every Access control —
    // it is the fullest section in the panel, and a widget list with
    // multi-line refusal text pushed all of that further down.
    //
    // The tab is ABSENT only when the backend cannot read widgets at all —
    // that is an honest "this build cannot answer", and it hides a control
    // that could never work. It is PRESENT for a room with no widgets, which
    // it briefly was not: gating on `count > 0` meant the tab existed only in
    // rooms that happened to have one, so in every other room the feature
    // looked missing rather than empty, and it was reported as exactly that.
    // Pinned is the precedent in the other direction and People and Media in
    // this one — a section that can be empty says so on its own pane.
    readonly property bool widgetsAvailable:
        app.widgets && app.widgets.supported
    onWidgetsAvailableChanged: {
        if (!widgetsAvailable && section === "widgets")
            section = "overview"
    }
    // Jump to a pinned event in the timeline. The panel does not own
    // navigation; TimelinePane does, exactly as it does for search results
    // and permalinks, so out-of-window pins hydrate through the ONE
    // existing path rather than a second one built here.
    signal jumpToEventRequested(string eventId)
    /// Export this room's loaded messages. The host owns the dialog, exactly
    /// like every other action here: this panel is signal-only, so a refused
    /// write cannot leave the panel and the account disagreeing.
    signal exportRoomRequested()

    // Looks up avatar/name/topic itself (rather than taking a caller-passed
    // snapshot) so it stays live: an avatar that arrives asynchronously
    // after resolveMissingDirectAvatars() completes — or any other room
    // change — refreshes here exactly like the timeline header does,
    // instead of freezing on whatever was known at the moment the panel
    // opened.
    function openForRoom(roomId) {
        app.roomInfo.roomId = roomId
        // Widgets are read ON DEMAND, not on every room change: the read is a
        // /state fallback for a type sliding sync does not carry, and doing it
        // for every room the user passes through would be one request per
        // room for a panel most of them never open.
        if (app.widgets.supported) {
            app.widgets.roomId = roomId
            app.widgets.refresh()
        }
        section = "overview"
        memberFilter = ""
        memberSearch.text = ""
        // The membership filter and the sort are part of "what am I looking
        // at", so they reset with the room: opening a new room under the
        // previous room's Banned filter shows an empty roster with nothing
        // saying why.
        memberSection.membership = "joined"
        memberSection.alphabetical = false
        refreshRoomData()
        // Poll-on-open: re-query the room's server push-rule mode so a
        // change made in another client lands in the local cache (and the
        // notifications combo below). A guarded no-op on backends without
        // server push-rule support.
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

    // Development-only: locate a descendant by objectName across both the
    // visual children (Item-derived) and the default-property data list.
    function findDemoDescendant(obj, name) {
        if (!obj) return null
        if (obj.objectName === name) return obj
        // Dialogs/Popups are not Items: their subtree hangs off contentItem,
        // never children/data — without this branch a Dialog descendant is
        // silently unreachable.
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

    // Development-only: screenshot-demo popup hooks (see
    // ScreenshotDemoController and SpacesRail.qml:accountSwitcherRequested
    // for the pattern this mirrors). Null target / disabled in a non-demo
    // build makes this an inert no-op.
    Connections {
        target: app.demo
        enabled: app.screenshotDemoActive
        function onDemoOpenInvitePeople() {
            inviteDialog.openFor(app.currentRoomId)
            // Seed the token field so real search results (matching Maya
            // Chen) render instead of an empty starting state.
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
        // The picker CHOOSES; it never uploads. Every display image in this
        // app goes through the one crop dialog first, which is also the gate
        // that refuses an SVG before anything renders it (CLAUDE.md §6).
        onAccepted: avatarCrop.openFor(selectedFile)
    }

    ImageCropDialog {
        id: avatarCrop
        role: "avatar"
        // The cropped temp file is a local path, exactly what this sink
        // already took — so the sink is unchanged.
        onCropped: function (file) { app.roomInfo.setRoomAvatar(file) }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Header ────────────────────────────────────────────────────────
        // Floored at the shared top-strip height, like the room list's header
        // and the room header: this panel sized itself from its content
        // alone, came out ~7px shorter than the room header beside it, and
        // its 1px rule therefore crossed the divider at a different height.
        // The Math.max keeps a taller text scale from clipping the row.
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
                    // The People section names its own subject and its size,
                    // as Sable's member column does. A panel titled "Room
                    // information" over a roster is a header describing the
                    // tab strip rather than the thing under it.
                    // Branched explicitly rather than %n: without a loaded
                    // translation a %n source string renders its "(s)"
                    // literally (the same reason RoomCallBanner branches).
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
                    // The pane-header role: 16, matching the room-list
                    // header on the same shell row. It was 15 here and 16
                    // there, which is exactly the kind of one-pixel
                    // difference that reads as "nobody chose".
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

        // ── Section tabs: one coherent segmented row ─────────────────────
        SegmentedControl {
            objectName: "roomInfoTabs"
            storm: true
            Layout.margins: AppTheme.spacing6
            // fitWidth compacts the row into the width its HOST gives it, so
            // it needs to be given one. Without fillWidth this RowLayout takes
            // its own implicit width and simply overflows the panel — which
            // is what cut "Media" off the end in every section but People.
            Layout.fillWidth: true
            // ONE SIZE IN EVERY SECTION. These two used to be conditional on
            // `section === "people"`, so the tab strip visibly shrank the
            // moment People was selected and grew back on the way out —
            // reported as "when clicking people tab everything gets small".
            // A control that changes size depending on which of its own tabs
            // is active looks broken, and the compactness People wanted
            // belongs to the ROSTER, never to the chrome above every section.
            //
            // Dense and fitWidth together are also what makes four translated
            // labels fit a user-resizable panel at all: non-dense, the fourth
            // tab ran off the panel edge in every section but People.
            dense: true
            fitWidth: true
            // The Pinned tab appears only when the backend supports pinned
            // messages AND the panel is showing the room the pin controller
            // is tracking (the panel can be opened for a Space home, which
            // is not the active room).
            // Built up rather than spelled out per combination: two
            // conditional tabs would otherwise need four hard-coded arrays,
            // and the next one eight.
            model: {
                const tabs = [{ label: qsTr("Overview"), value: "overview" }]
                if (root.pinnedAvailable)
                    tabs.push({ label: qsTr("Pinned"), value: "pinned" })
                tabs.push({ label: qsTr("People"), value: "people" })
                tabs.push({ label: qsTr("Media"), value: "media" })
                if (root.widgetsAvailable)
                    tabs.push({ label: qsTr("Widgets"), value: "widgets" })
                return tabs
            }
            current: root.section
            onActivated: (value) => root.section = value
        }

        // ── Overview ─────────────────────────────────────────────────────
        // v0.7.x: an explicit Flickable (was ScrollView) so this pane can
        // carry a SmoothWheelArea the same way every other converted
        // pane does — ScrollView auto-wraps non-Flickable content in an
        // internal, unreachable Flickable, which a WheelHandler cannot be
        // attached to from here. Same wheel/touchpad feel as the room
        // timeline; see qml/SmoothWheelArea.qml.
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

                // v0.6.0 checkpoint 11: per-room notification mode. On the
                // Rust backend the choice is written to the account's server
                // push rules through the SDK (the local value is the
                // device cache); other backends stay this-device-only.
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
                        // Index === mode, so "Follow account default" (3)
                        // stays last. It is offered ONLY on a backend that
                        // owns server push rules: with a device-local
                        // backend there is no account rule to defer to, so
                        // the option would be a label with nothing behind
                        // it — locally mode 3 simply notifies, which would
                        // silently disagree with what it claims to do.
                        model: app.serverRoomNotificationModes
                            ? [
                                qsTr("All messages"),
                                // "& keywords" is what the rule actually
                                // does: the SDK's MentionsAndKeywordsOnly
                                // mode keeps keyword rules firing.
                                qsTr("Mentions & keywords"),
                                qsTr("Mute"),
                                qsTr("Follow account default")
                              ]
                            : [
                                qsTr("All messages"),
                                qsTr("Mentions & keywords"),
                                qsTr("Mute")
                              ]
                        // Explicit mirrors of the cached mode and the
                        // room's sync-failure state: both getters are
                        // Q_INVOKABLEs, so bindings cannot observe their
                        // changes — refreshMode() is re-run from the
                        // change signals and on room switches instead.
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
                        // A mode-3 value persisted under a server-capable
                        // backend must not select a non-existent row on a
                        // device-local one. Clamp to 0 ("All messages"),
                        // NOT to count-1: the last row is "Mute", and mode
                        // 3 locally notifies for everything, so clamping to
                        // the end would show the exact opposite of what the
                        // device does.
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
                        // Backend-honest, phrased exactly like the room
                        // context-menu flyout disclaimer ("saved", not
                        // continuously synced; a failed write is admitted).
                        text: app.serverRoomNotificationModes
                              ? (notificationModeCombo.syncFailed
                                 ? qsTr("Couldn't save to the server — "
                                        + "kept on this device. "
                                        + "Retried when you reconnect.")
                                 : (notificationModeCombo.displayedMode === 3
                                    // Honest about the split: the SERVER
                                    // applies the account default to
                                    // pushes, but Lightning does not know
                                    // what that default resolves to, so
                                    // this device notifies for everything.
                                    // Claiming "the account default
                                    // applies" without that caveat would
                                    // mislead anyone whose default is
                                    // mentions-only.
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
                            size: 56
                            name: root.roomData.name || ""
                            mxc: root.roomData.avatarUrl || ""
                            colorKey: root.roomData.identityColorKey || ""
                            circle: root.roomData.isDirect === true
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
                        // Unsanitized server text; never AutoText (§6).
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


                    // An export is a fact ABOUT the room, so it sits with
                    // the room's own details rather than in the timeline
                    // header — which already carries five icon buttons and is
                    // the first row to crowd at 125% scaling.
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
                    // Hidden helper for clipboard copy without C++ additions.
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
                    // A Flow, not a RowLayout: two buttons with minimum
                    // widths do not fit this panel at every width the user
                    // may drag it to, and a RowLayout answers that by running
                    // off the edge. A Flow puts the second one on the next
                    // line instead.
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
                            // Yields, so the Save button beside it keeps its
                            // size instead of the layout distributing the
                            // shortfall across both and overflowing the panel.
                            Layout.minimumWidth: 60
                            Layout.fillWidth: true
                            placeholderText: qsTr("Room name")
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

                // v0.7.x room administration: who may join, and the room's
                // published address. Both are ordinary Matrix room state and
                // both are gated on the SDK's own power-level check for that
                // state event — never on a role label.
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
                    // when this block is on screen for a room.
                    onVisibleChanged: if (visible) app.roomInfo.requestDirectoryVisibility()
                    Connections {
                        target: app.roomInfo
                        function onRoomIdChanged() {
                            if (roomAdminBlock.visible)
                                app.roomInfo.requestDirectoryVisibility()
                        }
                    }

                    Label {
                        text: qsTr("Access")
                        color: AppTheme.textSecondary
                        font.pixelSize: AppTheme.textBody
                        font.weight: AppTheme.weightStrong
                    }

                    // Join rule. The three settable rules are the ones that
                    // carry no extra configuration; a room already using a
                    // space-restricted rule is shown honestly and left
                    // alone, because changing it needs an allow-rule list
                    // this panel has no way to build.
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
                            // Explicit mirror rather than a two-way binding:
                            // a rejected write must snap back to what the
                            // room actually holds, and a binding that the
                            // user's own selection has already broken
                            // cannot do that.
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
                        // v0.9 (phase 4): space-restricted access is
                        // editable. The kind (restricted vs. knock +
                        // restricted) and the allowed spaces are ONE write,
                        // and a configuration another client wrote renders
                        // as-is: allow rules of a kind this client cannot
                        // show are preserved on save and disclosed below.
                        // AppSwitch is a bare toggle whose owner binds
                        // `checked` and flips it from toggled(); the label
                        // sits beside it, and `checked` inside a handler is
                        // the value BEFORE the flip.
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
                                            // Nothing chosen yet: open the
                                            // picker so a choice can be
                                            // made first.
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
                                    // Local selection: the server's list
                                    // seeds it and re-seeds it after every
                                    // roster refresh; a flip before saving
                                    // is what the picker reads.
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

                    // v0.9 (phase 8): room version + upgrade. The version
                    // is disclosed to anyone who can see this block; the
                    // Upgrade control only to someone allowed to send
                    // m.room.tombstone (own_can_upgrade), and it opens a
                    // confirmation, never acts on the click itself.
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
                                // The inspected room, which is not always the
                                // open one — Room Information can be showing a
                                // Space home.
                                roomUpgradeDialog.openFor(app.roomInfo.roomId)
                            }
                        }
                    }
                    RoomUpgradeDialog { id: roomUpgradeDialog }

                    // Canonical alias. A bare localpart is completed with
                    // the account's own server by the controller.
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
                                // Explicit mirror, NOT `text: app.roomInfo
                                // .canonicalAlias` — the first keystroke
                                // breaks that binding permanently, and this
                                // panel outlives a room change. Typed text
                                // from room A then sat in the field with
                                // Save enabled against room B's alias, one
                                // click from publishing A's address onto B.
                                // Same discipline as joinRuleCombo above.
                                property string authoritative: ""
                                // A room change ALWAYS wins, even mid-edit:
                                // the half-typed value belongs to the room
                                // that is no longer on screen.
                                function resetForRoom() {
                                    authoritative =
                                        app.roomInfo.canonicalAlias
                                    text = authoritative
                                }
                                // A roster refresh only resnaps when the
                                // user has not edited, so a remote change
                                // (or a rejected write) lands without
                                // destroying an edit in progress.
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
                        // v0.9 (phase 4): alternative addresses. The whole
                        // list is one write; removing one demotes it from
                        // the room's state and deliberately keeps its
                        // directory mapping (see set_room_alt_aliases).
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
                                    // Remote or externally chosen text: never markup.
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

        // ── Pinned ───────────────────────────────────────────────────────
        // v0.7.x. The list IS `m.room.pinned_events`: nothing is stored
        // locally, remote changes arrive through the controller's re-read,
        // and an entry the server could not resolve renders as an honest
        // unavailable row rather than being hidden or linked anywhere.
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
                // Same wheel/touchpad feel as the room timeline; see
                // qml/SmoothWheelArea.qml.
                SmoothWheelArea {}
                // Newest pin first: Matrix appends, so the bridge's list is
                // oldest-first and the useful end is the tail.
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
                    // Only a resolved pin is clickable. An unavailable one
                    // must never navigate: there is nothing to navigate to,
                    // and jumping "near" it would land on an unrelated
                    // message.
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
                                    // Identity ink, as in the timeline. An
                                    // UNRESOLVED pin has no sender to hash,
                                    // so it keeps the neutral ink — colouring
                                    // "Message unavailable" would imply a
                                    // person who is not there.
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
                                // Remote or externally chosen text: never markup.
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                elide: Label.ElideRight
                                maximumLineCount: 2
                                lineHeight: AppTheme.lineHeightBody
                                lineHeightMode: Text.ProportionalHeight
                                wrapMode: Text.WordWrap
                                color: AppTheme.textSecondary
                                font.pixelSize: AppTheme.textBody
                                // Media and non-text pins get a typed label
                                // instead of a body that would read as
                                // nothing; a deleted or still-encrypted pin
                                // says exactly that.
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

                        // Unpin stays available for an UNAVAILABLE pin too:
                        // a dangling id is exactly the entry a moderator
                        // most wants to remove, and unpinning it needs only
                        // the id, never the event.
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

        // ── People ───────────────────────────────────────────────────────
        // Sable's / Discord's shape: a count, a membership filter and an
        // A-to-Z toggle, a name search, then the roster GROUPED BY POWER
        // LEVEL with a heading per group.
        //
        // The buckets come from C++ (`memberRoleRows`), not from QML. Which
        // roles a room HAS is a model fact — a room using 42 gets its own
        // "Custom (42)" heading rather than being folded into Moderator,
        // which would misdescribe the room's own configuration in the one
        // place a person consults to understand it — and the flattening has
        // to be there too, or a nested Repeater instantiates every row of
        // every group at once in a room that may have thousands of members.
        ColumnLayout {
            id: memberSection
            visible: root.section === "people"
            Layout.fillWidth: true
            Layout.fillHeight: true
            // Tight: this column is a list of people and every point of
            // spacing above it is one fewer person on screen.
            spacing: AppTheme.spacing4

            // A method CALL creates no property dependency, so a binding that
            // calls memberRoleRows() never re-evaluates on its own. Reading
            // this counter inside it is what makes an arriving roster reach
            // the list — the same idiom SpaceSettingsDialog's rosterTick uses
            // and the media-cache handlers' resolveTick before it.
            property int rosterTick: 0
            Connections {
                target: app.roomInfo
                function onMembersChanged() { memberSection.rosterTick++ }
            }

            /// "" (everyone) | "joined" | "invited" | "banned". A closed set:
            /// the controller matches nothing for anything else rather than
            /// quietly meaning "all".
            property string membership: "joined"
            property bool alphabetical: false

            // ONE ROW OF CHROME, not two. The search field, the membership
            // filter, the sort and Invite used to take a row each above the
            // roster; with the panel header and the tab strip on top of them
            // that was ~190 px of chrome before the first member, in a panel
            // whose whole job is to list people. Reported as "the people tab
            // takes up way too much space".
            //
            // The two toggles are ICONS with tooltips rather than words: in a
            // column this narrow a labelled control is most of the row, and
            // both are states with an obvious mark (a funnel, an A-to-Z sort).
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: AppTheme.spacing8
                Layout.rightMargin: AppTheme.spacing8
                Layout.topMargin: AppTheme.spacing6
                spacing: AppTheme.spacing4

                AppTextField {
                    id: memberSearch
                    Layout.fillWidth: true
                    // The field YIELDS; the three icon buttons keep their
                    // size. Without a minimum the layout distributes the
                    // shortfall across every child instead, so dragging the
                    // panel narrow pushed the buttons off its edge rather
                    // than shrinking the box they sit next to.
                    Layout.minimumWidth: 40
                    // Follows the slider with everything else in the section.
                    implicitHeight: Math.max(30, AppTheme.scaled(AppTheme.textBody) + 16)
                    searchIcon: true
                    clearButton: true
                    placeholderText: qsTr("Type name…")
                    onTextChanged: root.memberFilter = text
                }
                // The membership filter, as a cycling toggle rather than a
                // combo: four values, and this row has no space for a
                // dropdown. `active` carries "this is not the default", so a
                // roster narrowed to Banned cannot look like the whole room.
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
                // A-to-Z, off by default. The snapshot arrives sorted by
                // power level DESCENDING and is only THEN capped, so an
                // alphabetical re-sort of a truncated roster is missing
                // names from the MIDDLE of the alphabet rather than its tail
                // — `truncated` below is what says so.
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

            // Both behind Loaders: a never-laid-out empty Text keeps
            // ItemObservesViewport forever, and these two are empty in the
            // state this panel is normally in.
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
                // Air between names. A directory is scanned by NAME, and rows
                // that touch make two of them read as one block of text.
                spacing: 3
                // ONE flat list of headers and members, so it virtualises.
                // The role groups are the model's, not this view's.
                model: {
                    var _t = memberSection.rosterTick   // dependency only
                    return app.roomInfo.memberRoleRows(root.memberFilter,
                                                       memberSection.membership,
                                                       memberSection.alphabetical)
                }
                ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
                // Same wheel/touchpad feel as the room timeline; see
                // qml/SmoothWheelArea.qml.
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
                            // Wayfinding between runs of people: legible, and
                            // costing as little of the column as it can.
                            // Sized to its own text for the slider's sake.
                            height: Math.max(24, AppTheme.scaled(AppTheme.textSubtitle) + 10)
                            MenuSectionLabel {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.leftMargin: AppTheme.spacing12
                                anchors.rightMargin: AppTheme.spacing12
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 2
                                // MenuSectionLabel pins 12px for popovers.
                                // This is a list heading in a resizable panel
                                // and has to follow the slider like the names
                                // under it — and stay a step UNDER them, or
                                // the wayfinding competes with the content.
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
                            // Sized to the TEXT, so the row follows the
                            // text-size slider instead of clipping its own
                            // contents at 140%. The floor is the old fixed
                            // height; the avatar and the padding are what the
                            // rest of it is.
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
                                    // Follows the text, so the row keeps its
                                    // proportions at every slider position.
                                    readonly property int px:
                                        Math.max(24, AppTheme.scaled(AppTheme.textTitle) + 8)
                                    width: px; height: px
                                    size: px
                                    name: member.displayName.length > 0
                                          ? member.displayName : member.userId
                                    mxc: member.avatarUrl || ""
                                    colorKey: member.userId || ""

                                    // Banned members are not IN the room —
                                    // polling them would be noise, so they
                                    // get no dot. The userId is additionally
                                    // gated on the panel actually showing
                                    // People: this panel is never behind a
                                    // Loader and the ListView keeps cached
                                    // delegates alive, so without the gate a
                                    // closed panel kept watching members.
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
                                    // Identity ink — the member list is the
                                    // one place a reader scans for a specific
                                    // person, and it was a column of
                                    // identical grey.
                                    color: AppTheme.userColor(member.userId || "")
                                    // THROUGH scaled(), which this whole
                                    // panel was not doing at all: it used
                                    // AppTheme.scaled zero times while the
                                    // room list uses it six, so the text-size
                                    // slider grew every other surface and
                                    // left this one behind. That is why the
                                    // roster read small however its literal
                                    // size was tuned.
                                    //
                                    // textTitle, one real step up the type
                                    // ladder from the body size — a name is
                                    // what a person scans this list for.
                                    //
                                    // It was briefly textDisplay (22), which
                                    // is the ladder's HERO size ("login hero,
                                    // empty-state hero, verification panel
                                    // headline") and too big for a directory
                                    // row. The ladder is five sizes on
                                    // purpose; this is the one above body.
                                    font.pixelSize: AppTheme.scaled(AppTheme.textTitle)
                                    elide: Label.ElideRight
                                }
                                // Invited and banned rows are in the list on
                                // purpose (a banned member you cannot see is
                                // a ban you cannot lift), so they have to be
                                // legible AS invited and banned. The ROLE is
                                // the group heading and is deliberately not
                                // repeated per row.
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

        // ── Widgets ──────────────────────────────────────────────────────
        //
        // Its own tab since 0.8.5, at Rokas's request: Overview already
        // carries notifications, avatar, topic, member count, export, room
        // id, the Edit room group and every Access control, and a widget
        // list whose refusal rows wrap to three lines pushed all of that
        // further down. The tab only exists when there is something in it
        // (root.widgetsAvailable), so it never invites a click that shows
        // nothing.
        Flickable {
            visible: root.section === "widgets"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: widgetCol.implicitHeight + AppTheme.spacing16 * 2
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: AppScrollBar {}
            // No anchors: SmoothWheelArea is a non-visual handler that
            // attaches to its parent Flickable, exactly as the Overview and
            // Pinned panes use it.
            SmoothWheelArea {}
            ColumnLayout {
                id: widgetCol
                x: AppTheme.spacing12
                y: AppTheme.spacing12
                width: parent.width - AppTheme.spacing12 * 2
                spacing: AppTheme.spacing8

                // ── Widgets ──────────────────────────────────────
                //
                // Lightning LISTS widgets and opens them in the user's
                // browser; it does not embed them. docs/widgets.md carries
                // the measurements — Windows cannot build Qt WebEngine at
                // all, Flatpak could only ship Chromium unsandboxed beside
                // Megolm keys, and initialising it would force the whole
                // application's scenegraph to OpenGL.
                //
                // No "Widgets" heading here: the TAB is the heading now, and
                // repeating it would be the only text on the pane saying what
                // the tab already says.
                //
                // The empty state is what lets the tab exist in a room with
                // no widgets: "none here" is an answer, and it is a different
                // one from a missing tab, which reads as a missing feature.
                Label {
                    visible: app.widgets.supported && app.widgets.count === 0
                    Layout.fillWidth: true
                    text: qsTr("No widgets in this room.")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    wrapMode: Text.Wrap
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
                                // A widget name is chosen by whoever added
                                // it — remote text in a Label that would
                                // otherwise auto-detect rich text.
                                textFormat: Text.PlainText
                                text: widgetRow.name
                                color: AppTheme.text
                                elide: Label.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                textFormat: Text.PlainText
                                // A REFUSED widget still shows, with the
                                // reason. Dropping it would make a widget
                                // Lightning will not open indistinguishable
                                // from a widget the room never had.
                                text: widgetRow.openable
                                      ? widgetRow.kind
                                      : app.widgets.refusalText(widgetRow.refusal)
                                color: widgetRow.openable
                                       ? AppTheme.textMuted
                                       : AppTheme.danger
                                // SCALED, like every other size in this panel.
                                // It was unscaled while the block lived in
                                // Overview and nothing noticed; moving it into
                                // the range CallUiContractTest scans is what
                                // surfaced a size that ignored the text-size
                                // slider.
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                            }
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

        // ── Media & Files ────────────────────────────────────────────────
        ColumnLayout {
            visible: root.section === "media"
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: AppTheme.spacing8

            Label {
                Layout.margins: AppTheme.spacing12
                Layout.fillWidth: true
                text: qsTr("Media and files shared in the loaded part of "
                           + "this conversation. Scroll the timeline up to "
                           + "load more history.")
                color: AppTheme.textMuted
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                wrapMode: Text.WordWrap
                font.pixelSize: AppTheme.textMeta
            }

            ListView {
                id: mediaList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                // Newest first; bound to the loaded timeline (count keeps
                // the binding fresh as history paginates in).
                model: {
                    var deps = app.timeline.count
                    return app.timeline.mediaEntries().slice().reverse()
                }
                ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
                // Same wheel/touchpad feel as the room timeline; see
                // qml/SmoothWheelArea.qml.
                SmoothWheelArea {}

                Label {
                    anchors.centerIn: parent
                    visible: mediaList.count === 0
                    text: qsTr("No media in the loaded history.")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.textBody
                }

                delegate: ItemDelegate {
                    id: mediaDelegate
                    width: ListView.view.width
                    readonly property bool rowOnScreen:
                        y + height >= mediaList.contentY
                        && y <= mediaList.contentY + mediaList.height
                    Accessible.name: modelData.filename
                    onClicked: {
                        if (modelData.isImage)
                            root.openImageRequested(modelData.mediaKey || "",
                                                    modelData.httpUrl)
                    }
                    contentItem: RowLayout {
                        spacing: AppTheme.spacing8
                        Icon {
                            name: modelData.isImage ? "image" : "attach_file"
                            size: 16
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label {
                                // Remote or externally chosen text: never markup.
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                text: modelData.filename || qsTr("(unnamed)")
                                color: AppTheme.textPrimary
                                font.pixelSize: AppTheme.textBody
                                elide: Label.ElideMiddle
                            }
                            Label {
                                // Remote or externally chosen text: never markup.
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                text: qsTr("%1 · %2")
                                      .arg(modelData.sender)
                                      .arg(Qt.formatDateTime(modelData.timestamp,
                                                             "d MMM hh:mm"))
                                color: AppTheme.textMuted
                                font.pixelSize: AppTheme.textMeta
                                elide: Label.ElideRight
                            }
                        }
                        MediaListThumbnail {
                            visible: modelData.isVisual === true
                            mediaKey: modelData.mediaKey || ""
                            // A VIDEO with no server thumbnail must not ask
                            // for one: the request falls back to the full
                            // attachment, so the list downloaded whole videos
                            // (2.5 MB, 4.9 MB, 5.4 MB in one capture) only to
                            // reject them as "thumbnail payload sniffs as A/V
                            // container" — megabytes fetched and discarded,
                            // which is the lag spike on opening the media
                            // tab. thumbAvailable is already published by
                            // TimelineModel; it just was not consulted.
                            // Such rows fall back to the placeholder box.
                            visual: modelData.isVisual === true
                                    && (modelData.isVideo !== true
                                        || modelData.thumbAvailable === true)
                            video: modelData.isVideo === true
                            onScreen: mediaDelegate.rowOnScreen
                            Layout.alignment: Qt.AlignVCenter
                        }
                        AppButton {
                            visible: (modelData.mediaKey || "").length > 0
                                     && app.mediaBridge.supported
                            implicitHeight: 26
                            leftPadding: 10
                            rightPadding: 10
                            text: qsTr("Save")
                            Accessible.name: qsTr("Save %1 as…").arg(modelData.filename)
                            onClicked: root.saveMediaRequested(modelData.mediaKey,
                                                               modelData.filename || "download")
                        }
                    }
                }
            }
        }
    }

    // Leave confirmation — Cancel is the default safe action.
    Dialog {
        id: leaveConfirm
        parent: Overlay.overlay
        anchors.centerIn: parent
        // An explicit viewport-bounded width keeps Dialog implicit sizing
        // independent from the wrapping label/layout inside it.
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
}
