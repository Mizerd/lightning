import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.7.x Discover / Join Room: one Lightning modal card for browsing the
// public room directory and joining by address or link.
//
// Browse tab: debounced directory search (RoomDirectorySearchModel) with
// server-token pagination and an optional remote directory server. Address
// tab: resolves #alias / !roomid / matrix: URIs / matrix.to permalinks
// through the SDK (never hand-parsed here) into a preview card, then joins
// or knocks. All server operations run through RoomDiscoveryController;
// duplicate submissions are blocked by its single-flight busy state, and a
// successful join closes the dialog — AppController navigates.
Dialog {
    id: root
    objectName: "discoverJoinDialog"
    modal: true
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: Math.min(560, parent ? parent.width - AppTheme.spacing24 * 2 : 560)
    height: Math.min(640,
                     parent ? parent.height - AppTheme.spacing24 * 2 : 640)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    // "browse" | "address"
    property string mode: "browse"
    property string addressText: ""
    property string knockReasonText: ""
    // Row currently asked to join from the Browse list ("" = none). Lets
    // exactly that row show its in-flight state.
    property string pendingJoinRoomId: ""

    readonly property bool busy: app.discovery.busy
    readonly property var resolved: app.discovery.resolved
    readonly property bool resolvedOk:
        app.discovery.resolveState === "resolved"
        && resolved.ok === true
    readonly property bool resolvedPreview:
        resolvedOk && resolved.previewOk === true
    readonly property string resolvedMembership:
        resolvedPreview ? (resolved.membership || "") : ""
    readonly property string resolvedJoinRule:
        resolvedPreview ? (resolved.joinRule || "") : ""
    readonly property bool resolvedCanKnock:
        (resolvedJoinRule === "knock" || resolvedJoinRule === "knock_restricted")
        && resolvedMembership !== "joined" && resolvedMembership !== "invited"
        && resolvedMembership !== "knocked" && resolvedMembership !== "banned"

    // When opened from an activated Matrix link: a link to an already
    // joined room should NOT park the user in a dialog — it auto-opens
    // (and jumps to the linked event) as soon as resolution confirms the
    // membership.
    property bool autoOpenJoined: false
    // The raw activated link, kept so a non-room resolution can fall back
    // to the browser (review L2) instead of stranding the user on an
    // error for a link they merely clicked.
    property string activatedLink: ""

    function openDialog(startMode) {
        resetAll()
        if (startMode === "address" || startMode === "browse")
            mode = startMode
        open()
        if (mode === "browse" && app.discovery.directory.count === 0)
            app.discovery.directory.refresh()
        Qt.callLater(focusCurrent)
    }

    // 2026-08-18 tester report ("kai spaudi link atsidaro pop up langas
    // milisekundei cant do anything with it"): a clicked room link opened
    // this dialog immediately and then closed it again the moment the link
    // resolved to a room the user is already in — a modal that flashed for a
    // frame and could not be used. The resolve now runs FIRST and the dialog
    // is only shown when it has something to ask the user: a room that needs
    // joining or knocking, a failure, or a resolve slow enough
    // (linkResolveGrace) that silence would look like a dead click.
    function openForLink(link) {
        resetAll()
        mode = "address"
        addressText = link
        autoOpenJoined = true
        activatedLink = link
        linkResolveGrace.restart()
        app.discovery.resolve(link)
    }

    // Show the dialog for a link only once it is clear the user is needed.
    function revealForLink() {
        linkResolveGrace.stop()
        if (root.autoOpenJoined && !root.visible) {
            root.open()
            Qt.callLater(focusCurrent)
        }
    }

    // A link flow that finished without needing the dialog: stop the grace
    // timer so it cannot pop the modal open after the fact, and clear the
    // link state by hand — onClosed does not run for a dialog that was
    // never shown.
    function finishLinkFlow() {
        linkResolveGrace.stop()
        var wasVisible = root.visible
        root.close()
        if (!wasVisible)
            resetAll()
    }

    Timer {
        id: linkResolveGrace
        interval: 400
        repeat: false
        onTriggered: root.revealForLink()
    }

    function resetAll() {
        // Any pending link flow ends here: a grace timer left running could
        // otherwise pop this dialog open over an unrelated later state.
        linkResolveGrace.stop()
        mode = "browse"
        addressText = ""
        knockReasonText = ""
        pendingJoinRoomId = ""
        autoOpenJoined = false
        activatedLink = ""
        app.discovery.clearResolved()
        app.discovery.clearError()
    }

    function focusCurrent() {
        if (mode === "address")
            addressField.forceActiveFocus()
        else
            directorySearchField.forceActiveFocus()
    }

    function joinRuleLabel(rule) {
        if (rule === "public") return qsTr("Public")
        if (rule === "knock" || rule === "knock_restricted")
            return qsTr("Ask to join")
        if (rule === "restricted") return qsTr("Restricted")
        if (rule === "invite" || rule === "private") return qsTr("Invite only")
        return ""
    }

    onClosed: resetAll()

    Connections {
        target: app.discovery
        // AppController navigates; this surface only needs to get out of
        // the way. Knocks stay open so the pending state is visible.
        function onRoomJoined() { root.close() }
        function onSpaceJoined() { root.close() }
        function onKnockSent() { root.pendingJoinRoomId = "" }
        function onErrorMessageChanged() {
            if (app.discovery.errorMessage.length > 0) {
                root.pendingJoinRoomId = ""
                // A link that failed must say so somewhere the user can see.
                if (root.autoOpenJoined)
                    root.revealForLink()
            }
        }
        function onResolveChanged() {
            // A clicked link that turns out not to be a room (e.g. an
            // encoded user permalink the interceptor missed) belongs in
            // the browser, exactly as before this round (review L2).
            if (root.autoOpenJoined
                && app.discovery.resolveState === "failed"
                && root.resolved.category === "not_a_room"
                && root.activatedLink.indexOf("matrix.to") !== -1) {
                app.media.openWebUrl(root.activatedLink)
                root.finishLinkFlow()
                return
            }
            if (!root.autoOpenJoined)
                return
            // Anything the user has to answer — a room to join or knock, or
            // a resolve that failed — needs the dialog on screen.
            if (!root.resolvedOk) {
                if (app.discovery.resolveState === "failed")
                    root.revealForLink()
                return
            }
            if (root.resolvedMembership !== "joined") {
                root.revealForLink()
                return
            }
            var roomId = root.resolved.roomId || ""
            if (roomId === "") {
                root.revealForLink()
                return
            }
            var eventId = root.resolved.eventId || ""
            app.openRoom(roomId)
            if (eventId !== "") {
                // Same shape as notification click routing (Main.qml): let
                // the room switch settle one event-loop turn, then jump.
                Qt.callLater(function() {
                    app.pagination.jumpToEvent(eventId)
                })
            }
            root.finishLinkFlow()
        }
    }

    background: Rectangle {
        radius: AppTheme.radiusLg
        color: AppTheme.stormPanel
        border.color: AppTheme.stormBorder
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Label {
                text: qsTr("Discover rooms")
                color: AppTheme.stormText
                font.family: AppTheme.menuFont
                font.pixelSize: 16
                font.weight: Font.Bold
                Layout.fillWidth: true
            }
            IconButton {
                iconName: "close"
                Accessible.name: qsTr("Close")
                onClicked: root.close()
            }
        }

        SegmentedControl {
            Layout.fillWidth: true
            storm: true
            model: [
                { label: qsTr("Browse"), value: "browse" },
                { label: qsTr("By address"), value: "address" }
            ]
            current: root.mode
            onActivated: (value) => {
                if (value === root.mode)
                    return
                app.discovery.clearError()
                root.mode = value
                if (value === "browse" && app.discovery.directory.count === 0)
                    app.discovery.directory.refresh()
                Qt.callLater(root.focusCurrent)
            }
        }

        // Shared error surface for join/knock failures from either tab.
        Label {
            Layout.fillWidth: true
            visible: app.discovery.errorMessage.length > 0
            text: app.discovery.errorMessage
            color: AppTheme.danger
            font.pixelSize: 12
            wrapMode: Text.Wrap
            Accessible.name: text
        }

        // ── Browse tab ──────────────────────────────────────────────────
        ColumnLayout {
            visible: root.mode === "browse"
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: AppTheme.spacing8

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                AppTextField {
                    id: directorySearchField
                    objectName: "directorySearchField"
                    Layout.fillWidth: true
                    storm: true
                    searchIcon: true
                    clearButton: true
                    placeholderText: qsTr("Search public rooms…")
                    text: app.discovery.directory.query
                    onTextChanged: app.discovery.directory.query = text
                }
                AppTextField {
                    id: directoryServerField
                    objectName: "directoryServerField"
                    Layout.preferredWidth: 150
                    storm: true
                    placeholderText: qsTr("Server (optional)")
                    text: app.discovery.directory.server
                    onEditingFinished:
                        app.discovery.directory.server = text
                }
            }

            ListView {
                id: directoryList
                objectName: "directoryResultsList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: AppTheme.spacing4
                model: app.discovery.directory
                ScrollBar.vertical: ScrollBar {}
                onAtYEndChanged: {
                    if (atYEnd && app.discovery.directory.canLoadMore)
                        app.discovery.directory.loadMore()
                }

                delegate: Rectangle {
                    id: directoryRow
                    required property string roomId
                    required property string name
                    required property string alias
                    required property string topic
                    required property string avatarUrl
                    required property var members
                    required property string joinRule
                    required property string membership
                    required property bool isSpace

                    readonly property string visibleName:
                        name.length > 0 ? name
                                        : (alias.length > 0 ? alias : roomId)
                    readonly property bool joined: membership === "joined"
                    readonly property bool knocked: membership === "knocked"
                    readonly property bool rowKnocks:
                        joinRule === "knock" || joinRule === "knock_restricted"
                    readonly property bool rowPending:
                        root.pendingJoinRoomId === roomId && root.busy

                    width: directoryList.width
                    height: 64
                    radius: AppTheme.radiusMd
                    color: rowHover.hovered ? AppTheme.stormSelection
                                            : "transparent"
                    HoverHandler { id: rowHover }

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: AppTheme.spacing8
                        spacing: AppTheme.spacing8
                        Avatar {
                            mxc: directoryRow.avatarUrl
                            name: directoryRow.visibleName
                            size: 40
                            circle: !directoryRow.isSpace
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: AppTheme.spacing6
                                Label {
                                    text: directoryRow.visibleName
                                    color: AppTheme.stormText
                                    font.family: AppTheme.menuFont
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                    elide: Label.ElideRight
                                    Layout.maximumWidth: 240
                                }
                                Label {
                                    visible: directoryRow.isSpace
                                    text: qsTr("Space")
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: 10
                                }
                                Label {
                                    visible: !directoryRow.joined
                                             && root.joinRuleLabel(
                                                 directoryRow.joinRule).length > 0
                                    text: root.joinRuleLabel(directoryRow.joinRule)
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: 10
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                text: {
                                    var bits = []
                                    if (directoryRow.alias.length > 0)
                                        bits.push(directoryRow.alias)
                                    bits.push(qsTr("%n member(s)", "",
                                                   Number(directoryRow.members)))
                                    if (directoryRow.topic.length > 0)
                                        bits.push(directoryRow.topic)
                                    return bits.join(" · ")
                                }
                                color: AppTheme.stormTextMuted
                                font.pixelSize: 11
                                elide: Label.ElideRight
                                maximumLineCount: 1
                            }
                        }
                        Label {
                            visible: directoryRow.knocked
                            text: qsTr("Request pending")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: 11
                        }
                        AppButton {
                            visible: directoryRow.joined
                            storm: true
                            text: qsTr("Open")
                            onClicked: {
                                app.openRoom(directoryRow.roomId)
                                root.close()
                            }
                        }
                        AppButton {
                            visible: !directoryRow.joined && !directoryRow.knocked
                            storm: true
                            kind: "primary"
                            enabled: !root.busy
                            text: directoryRow.rowPending
                                  ? qsTr("Joining…")
                                  : (directoryRow.rowKnocks ? qsTr("Ask to join")
                                                            : qsTr("Join"))
                            onClicked: {
                                root.pendingJoinRoomId = directoryRow.roomId
                                if (directoryRow.rowKnocks)
                                    app.discovery.knock(directoryRow.roomId, [], "")
                                else
                                    app.discovery.join(directoryRow.roomId, [],
                                                       directoryRow.isSpace)
                            }
                        }
                    }
                }

                // Empty / loading / error states live inside the list area.
                Item {
                    anchors.fill: parent
                    visible: directoryList.count === 0

                    BusyIndicator {
                        anchors.centerIn: parent
                        running: app.discovery.directory.state === "loading"
                        visible: running
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: app.discovery.directory.state === "no_results"
                        text: qsTr("No rooms found")
                        color: AppTheme.stormTextMuted
                    }
                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: AppTheme.spacing8
                        visible: app.discovery.directory.state === "error"
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: app.discovery.directory.errorCategory
                                  === "invalid_server"
                                  ? qsTr("That is not a valid directory "
                                         + "server name.")
                                  : qsTr("The room directory could not be "
                                         + "loaded.")
                            color: AppTheme.stormTextMuted
                        }
                        AppButton {
                            Layout.alignment: Qt.AlignHCenter
                            storm: true
                            text: qsTr("Retry")
                            onClicked: app.discovery.directory.refresh()
                        }
                    }
                }
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                visible: app.discovery.directory.state === "loading_more"
                text: qsTr("Loading more…")
                color: AppTheme.stormTextMuted
                font.pixelSize: 11
            }
        }

        // ── By address tab ──────────────────────────────────────────────
        ColumnLayout {
            visible: root.mode === "address"
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: AppTheme.spacing8

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                AppTextField {
                    id: addressField
                    objectName: "roomAddressField"
                    Layout.fillWidth: true
                    storm: true
                    clearButton: true
                    placeholderText:
                        qsTr("#room:server, !roomid:server, or a Matrix link")
                    text: root.addressText
                    onTextChanged: root.addressText = text
                    onAccepted: lookupButton.clicked()
                }
                AppButton {
                    id: lookupButton
                    objectName: "roomLookupButton"
                    storm: true
                    kind: "primary"
                    enabled: root.addressText.trim().length > 0
                             && app.discovery.resolveState !== "resolving"
                    text: qsTr("Look up")
                    onClicked: {
                        app.discovery.clearError()
                        app.discovery.resolve(root.addressText)
                    }
                }
            }

            BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                running: app.discovery.resolveState === "resolving"
                visible: running
            }

            Label {
                Layout.fillWidth: true
                visible: app.discovery.resolveState === "failed"
                text: root.resolved.category === "not_a_room"
                      ? qsTr("That link points at a person, not a room.")
                      : qsTr("That is not a valid room address or link.")
                color: AppTheme.danger
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }

            // Preview card (or the honest no-preview surface).
            Rectangle {
                Layout.fillWidth: true
                visible: root.resolvedOk
                radius: AppTheme.radiusMd
                color: AppTheme.stormInset
                border.color: AppTheme.stormBorder
                border.width: 1
                implicitHeight: previewColumn.implicitHeight
                                + AppTheme.spacing12 * 2

                ColumnLayout {
                    id: previewColumn
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        visible: root.resolvedPreview
                        Avatar {
                            mxc: root.resolved.avatarUrl || ""
                            name: root.resolved.name
                                  || root.resolved.alias
                                  || root.resolved.target || ""
                            size: 44
                            circle: root.resolved.isSpace !== true
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                text: root.resolved.name
                                      || root.resolved.alias
                                      || root.resolved.target || ""
                                color: AppTheme.stormText
                                font.family: AppTheme.menuFont
                                font.pixelSize: 14
                                font.weight: Font.Bold
                                elide: Label.ElideRight
                                Layout.fillWidth: true
                            }
                            Label {
                                Layout.fillWidth: true
                                text: {
                                    var bits = []
                                    if (root.resolved.alias)
                                        bits.push(root.resolved.alias)
                                    bits.push(qsTr("%n member(s)", "",
                                                   Number(root.resolved.members || 0)))
                                    var rule = root.joinRuleLabel(root.resolvedJoinRule)
                                    if (rule.length > 0)
                                        bits.push(rule)
                                    return bits.join(" · ")
                                }
                                color: AppTheme.stormTextMuted
                                font.pixelSize: 11
                                elide: Label.ElideRight
                            }
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: root.resolvedPreview
                                 && (root.resolved.topic || "").length > 0
                        text: root.resolved.topic || ""
                        color: AppTheme.stormTextSecondary
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                        maximumLineCount: 3
                        elide: Label.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: root.resolvedOk && !root.resolvedPreview
                        text: qsTr("No preview is available for this room — "
                                   + "the server does not allow previewing "
                                   + "it. You can still try to join.")
                        color: AppTheme.stormTextMuted
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: root.resolvedMembership === "banned"
                        text: qsTr("You are banned from this room.")
                        color: AppTheme.danger
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                    AppTextField {
                        Layout.fillWidth: true
                        visible: root.resolvedCanKnock
                        storm: true
                        placeholderText: qsTr("Reason (optional)")
                        text: root.knockReasonText
                        onTextChanged: root.knockReasonText = text
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        Item { Layout.fillWidth: true }
                        Label {
                            visible: root.resolvedMembership === "knocked"
                            text: qsTr("Request pending")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: 12
                        }
                        AppButton {
                            visible: root.resolvedMembership === "joined"
                            storm: true
                            text: qsTr("Open")
                            onClicked: {
                                app.openRoom(root.resolved.roomId
                                             || root.resolved.target)
                                root.close()
                            }
                        }
                        AppButton {
                            objectName: "resolvedJoinButton"
                            visible: root.resolvedMembership !== "joined"
                                     && root.resolvedMembership !== "knocked"
                                     && root.resolvedMembership !== "banned"
                            storm: true
                            kind: "primary"
                            enabled: !root.busy
                            text: root.busy
                                  ? qsTr("Joining…")
                                  : (root.resolvedCanKnock
                                     ? qsTr("Ask to join")
                                     : (root.resolvedMembership === "invited"
                                        ? qsTr("Accept invite")
                                        : qsTr("Join room")))
                            onClicked: {
                                var target = root.resolved.target || ""
                                var via = root.resolved.via || []
                                if (root.resolvedCanKnock)
                                    app.discovery.knock(target, via,
                                                        root.knockReasonText)
                                else
                                    app.discovery.join(
                                        target, via,
                                        root.resolved.isSpace === true)
                            }
                        }
                    }
                }
            }
            Item { Layout.fillHeight: true }
        }
    }
}
