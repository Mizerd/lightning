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

    property var roomData: ({})
    signal closeRequested()
    // v0.5.9: Media & Files entries delegate viewing/saving to the pane
    // that owns the viewer and the save dialog (TimelinePane).
    signal openImageRequested(string mediaKey, var httpUrl)
    signal saveMediaRequested(string mediaKey, string filename)

    // "overview" | "pinned" | "people" | "media"
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
    // Jump to a pinned event in the timeline. The panel does not own
    // navigation; TimelinePane does, exactly as it does for search results
    // and permalinks, so out-of-window pins hydrate through the ONE
    // existing path rather than a second one built here.
    signal jumpToEventRequested(string eventId)

    // Looks up avatar/name/topic itself (rather than taking a caller-passed
    // snapshot) so it stays live: an avatar that arrives asynchronously
    // after resolveMissingDirectAvatars() completes — or any other room
    // change — refreshes here exactly like the timeline header does,
    // instead of freezing on whatever was known at the moment the panel
    // opened.
    function openForRoom(roomId) {
        app.roomInfo.roomId = roomId
        section = "overview"
        memberFilter = ""
        memberSearch.text = ""
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
        onAccepted: app.roomInfo.setRoomAvatar(selectedFile)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Header ────────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: headerRow.implicitHeight + AppTheme.spacing12 * 2
            color: AppTheme.surface
            RowLayout {
                id: headerRow
                anchors.fill: parent
                anchors.margins: AppTheme.spacing12
                spacing: AppTheme.spacing8
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Room information")
                    color: AppTheme.textPrimary
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
            Layout.margins: AppTheme.spacing8
            // The Pinned tab appears only when the backend supports pinned
            // messages AND the panel is showing the room the pin controller
            // is tracking (the panel can be opened for a Space home, which
            // is not the active room).
            model: root.pinnedAvailable
                ? [
                    { label: qsTr("Overview"), value: "overview" },
                    { label: qsTr("Pinned"), value: "pinned" },
                    { label: qsTr("People"), value: "people" },
                    { label: qsTr("Media"), value: "media" },
                  ]
                : [
                    { label: qsTr("Overview"), value: "overview" },
                    { label: qsTr("People"), value: "people" },
                    { label: qsTr("Media"), value: "media" },
                  ]
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
                    RowLayout {
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
                        Item { Layout.fillWidth: true }
                    }
                    RowLayout {
                        visible: app.roomInfo.canEditName
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        AppTextField {
                            id: editName
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
                        Label {
                            Layout.fillWidth: true
                            visible: parent.restricted
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            wrapMode: Text.WordWrap
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("Members of a space can join. "
                                       + "Lightning can't change "
                                       + "space-restricted access yet.")
                        }
                    }

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
        ColumnLayout {
            visible: root.section === "people"
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: AppTheme.spacing8

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: AppTheme.spacing8
                spacing: AppTheme.spacing8
                AppTextField {
                    id: memberSearch
                    Layout.fillWidth: true
                    searchIcon: true
                    clearButton: true
                    placeholderText: qsTr("Search members…")
                    onTextChanged: root.memberFilter = text
                }
                AppButton {
                    kind: "primary"
                    visible: app.roomInfo.canInvite
                    text: qsTr("Invite")
                    Accessible.name: qsTr("Invite people to this room")
                    onClicked: inviteDialog.openFor(app.roomInfo.roomId)
                }
            }

            Label {
                visible: app.roomInfo.loading
                Layout.leftMargin: AppTheme.spacing12
                text: qsTr("Loading members…")
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textBody
            }
            Label {
                visible: app.roomInfo.truncated
                Layout.leftMargin: AppTheme.spacing12
                Layout.fillWidth: true
                text: qsTr("Showing the first %1 members of %2.")
                      .arg(app.roomInfo.members.length)
                      .arg(app.roomInfo.joinedCount + app.roomInfo.invitedCount)
                color: AppTheme.textMuted
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                wrapMode: Text.WordWrap
                font.pixelSize: AppTheme.textMeta
            }

            ListView {
                id: memberList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                // Reading `members` makes this binding re-evaluate when the
                // snapshot updates (invites landing, refreshes), not only
                // when the filter text changes.
                model: {
                    var snapshot = app.roomInfo.members // dependency only
                    return root.memberFilter.length > 0
                            ? app.roomInfo.filterMembers(root.memberFilter)
                            : snapshot
                }
                ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
                // Same wheel/touchpad feel as the room timeline; see
                // qml/SmoothWheelArea.qml.
                SmoothWheelArea {}

                delegate: ItemDelegate {
                    width: ListView.view.width
                    Accessible.name: modelData.displayName.length > 0
                                     ? qsTr("%1 (%2)").arg(modelData.displayName)
                                                      .arg(modelData.userId)
                                     : modelData.userId
                    onClicked: memberProfile.openFor(modelData)
                    contentItem: RowLayout {
                        spacing: AppTheme.spacing8
                        Avatar {
                            width: 32; height: 32
                            size: 32
                            name: modelData.displayName.length > 0
                                  ? modelData.displayName : modelData.userId
                            mxc: modelData.avatarUrl || ""
                            colorKey: modelData.userId || ""

                            // v0.7.x Matrix presence. Banned members are
                            // not IN the room — polling them would be
                            // noise, so they get no dot. The userId is
                            // additionally gated on the panel actually
                            // showing People (the MemberProfilePopover
                            // `opened` idiom): this panel is never behind
                            // a Loader and the ListView keeps cached
                            // delegates alive, so without the gate a
                            // closed panel kept watching members
                            // (review L4).
                            PresenceDot {
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.margins: -1
                                dotSize: 10
                                ring: AppTheme.sidebar
                                userId: root.visible
                                        && root.section === "people"
                                        && modelData.membership !== "banned"
                                        ? (modelData.userId || "") : ""
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: AppTheme.spacing4
                                Label {
                                    text: modelData.displayName.length > 0
                                          ? modelData.displayName
                                          : modelData.userId
                                    // Identity ink — the member list is the
                                    // one place a reader scans for a
                                    // specific person, and it was a column
                                    // of identical grey.
                                    color: AppTheme.userColor(modelData.userId || "")
                                    font.pixelSize: AppTheme.textSubtitle
                                    font.weight: AppTheme.weightStrong
                                    elide: Label.ElideRight
                                }
                                Label {
                                    visible: modelData.ambiguous === true
                                             && modelData.displayName.length > 0
                                    text: modelData.userId
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.textMeta
                                    elide: Label.ElideMiddle
                                    Layout.fillWidth: true
                                }
                            }
                            Label {
                                visible: modelData.displayName.length > 0
                                         && modelData.ambiguous !== true
                                text: modelData.userId
                                color: AppTheme.textMuted
                                font.pixelSize: AppTheme.textMeta
                                elide: Label.ElideMiddle
                                Layout.fillWidth: true
                            }
                        }
                        // These four were bare coloured words, two of them
                        // (Admin, Mod) in the SAME accent — different powers
                        // rendered identically. StatusChip carries the six
                        // tone families and keeps the pill geometry shared
                        // with every other status chip in the app.
                        StatusChip {
                            visible: modelData.membership === "invited"
                            tone: "warning"
                            label: qsTr("Invited")
                        }
                        // Banned members ride the snapshot since the unban
                        // round (sorted last) — mark them so the list
                        // doesn't read as "still here".
                        StatusChip {
                            visible: modelData.membership === "banned"
                            tone: "danger"
                            label: qsTr("Banned")
                        }
                        StatusChip {
                            visible: modelData.role === "administrator"
                                     || modelData.role === "creator"
                            tone: "accent"
                            label: qsTr("Admin")
                        }
                        StatusChip {
                            visible: modelData.role === "moderator"
                            tone: "info"
                            label: qsTr("Mod")
                        }
                    }
                    ToolTip.text: modelData.userId
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                }
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
                                Layout.fillWidth: true
                                text: modelData.filename || qsTr("(unnamed)")
                                color: AppTheme.textPrimary
                                font.pixelSize: AppTheme.textBody
                                elide: Label.ElideMiddle
                            }
                            Label {
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
