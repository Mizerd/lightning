import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Dialog {
    id: root
    objectName: "discoverJoinDialog"
    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: Math.min(560, parent ? parent.width - AppTheme.spacing24 * 2 : 560)
    height: Math.min(640,
                     parent ? parent.height - AppTheme.spacing24 * 2 : 640)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    property string mode: "browse"
    property string addressText: ""
    property string knockReasonText: ""
    property string pendingJoinRoomId: ""
    // A knock is a server request that RoomDiscoveryController.busy() does not
    // cover, so the dialog tracks it itself: in flight (button disabled, no
    // repeat) and sent (the row/preview reads "Request pending" at once, not
    // only after a later sync or lookup shows membership=knock). C12-F03.
    property bool knockInFlight: false
    property var knockedRoomIds: ({})
    property bool addressKnockSent: false
    readonly property string mrsServer: "matrixrooms.info"
    property bool otherDirectoryServer: false

    readonly property bool busy: app.discovery.busy
    readonly property var resolved: app.discovery.resolved
    readonly property bool resolvedOk:
        app.discovery.resolveState === "resolved"
        && resolved.ok === true
    readonly property bool resolvedPreview:
        resolvedOk && resolved.previewOk === true
    readonly property bool resolvedNotFound:
        resolvedOk && !resolvedPreview
        && resolved.previewCategory === "not_found"
    readonly property string resolvedMembership:
        resolvedPreview ? (resolved.membership || "") : ""
    readonly property string resolvedJoinRule:
        resolvedPreview ? (resolved.joinRule || "") : ""
    readonly property bool resolvedCanKnock:
        (resolvedJoinRule === "knock" || resolvedJoinRule === "knock_restricted")
        && resolvedMembership !== "joined" && resolvedMembership !== "invited"
        && resolvedMembership !== "knocked" && resolvedMembership !== "banned"

    property bool autoOpenJoined: false
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

    function openForLink(link) {
        resetAll()
        mode = "address"
        addressText = link
        autoOpenJoined = true
        activatedLink = link
        linkResolveGrace.restart()
        app.discovery.resolve(link)
    }

    function revealForLink() {
        linkResolveGrace.stop()
        if (root.autoOpenJoined && !root.visible) {
            root.open()
            Qt.callLater(focusCurrent)
        }
    }

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
        linkResolveGrace.stop()
        mode = "browse"
        addressText = ""
        knockReasonText = ""
        pendingJoinRoomId = ""
        knockInFlight = false
        knockedRoomIds = ({})
        addressKnockSent = false
        autoOpenJoined = false
        activatedLink = ""
        // Browse state must not outlive the dialog: a row that was joined
        // stayed "Join" and the old query/server came back on reopen (C12-F05).
        // Set the fields first; clear() then cancels the debounce they start.
        app.discovery.directory.query = ""
        app.discovery.directory.server = ""
        otherDirectoryServer = false
        directorySourceBox.currentIndex = 0
        app.discovery.directory.clear()
        app.discovery.clearResolved()
        app.discovery.clearError()
    }

    function focusCurrent() {
        if (mode === "address")
            addressField.forceActiveFocus()
        else
            directorySearchField.forceActiveFocus()
    }

    // 0 = the account's homeserver, 1 = matrixrooms.info, 2 = a typed server.
    function chooseDirectorySource(index) {
        otherDirectoryServer = index === 2
        app.discovery.directory.server = index === 1 ? mrsServer : ""
        if (otherDirectoryServer)
            Qt.callLater(directoryServerField.forceActiveFocus)
    }

    function joinRuleLabel(rule) {
        if (rule === "public") return qsTr("Public")
        if (rule === "knock" || rule === "knock_restricted")
            return qsTr("Ask to join")
        if (rule === "restricted") return qsTr("Restricted")
        if (rule === "invite" || rule === "private") return qsTr("Invite only")
        return ""
    }

    // RoomDiscoveryController::knock() returns SILENTLY when unsupported, busy
    // or a knock is already running, so the in-flight flag is only raised when
    // none of those hold, and a bounded timer releases it in case the answer
    // never arrives (logout zeroes the op without a signal).
    function startKnock(target, via, reason) {
        if (knockInFlight || !app.discovery.supported || app.discovery.busy)
            return false
        knockInFlight = true
        knockGuard.restart()
        app.discovery.knock(target, via, reason)
        return true
    }
    Timer {
        id: knockGuard
        interval: 30000
        onTriggered: root.knockInFlight = false
    }
    onKnockInFlightChanged: if (!knockInFlight) knockGuard.stop()

    onClosed: resetAll()

    Connections {
        target: app.discovery
        function onRoomJoined() { root.close() }
        function onSpaceJoined() { root.close() }
        function onKnockSent(roomId) {
            if (root.pendingJoinRoomId !== "" || root.knockInFlight) {
                var m = Object.assign({}, root.knockedRoomIds)
                if (root.pendingJoinRoomId !== "")
                    m[root.pendingJoinRoomId] = true
                if (roomId.length > 0)
                    m[roomId] = true
                root.knockedRoomIds = m
            }
            if (root.mode === "address" && root.knockInFlight)
                root.addressKnockSent = true
            root.pendingJoinRoomId = ""
            root.knockInFlight = false
        }
        function onErrorMessageChanged() {
            if (app.discovery.errorMessage.length > 0) {
                root.pendingJoinRoomId = ""
                root.knockInFlight = false
                if (root.autoOpenJoined)
                    root.revealForLink()
            }
        }
        function onResolveChanged() {
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
                font.pixelSize: AppTheme.textTitle
                font.weight: AppTheme.weightBold
                Layout.fillWidth: true
            }
            IconButton {
                storm: true
                iconName: "close"
                iconSize: 18
                implicitWidth: 28; implicitHeight: 28
                Accessible.name: qsTr("Close")
                ToolTip.text: Accessible.name
                ToolTip.visible: hovered
                ToolTip.delay: 500
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

        Label {
            Layout.fillWidth: true
            visible: app.discovery.errorMessage.length > 0
            text: app.discovery.errorMessage
            color: AppTheme.danger
            font.pixelSize: AppTheme.textBody
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            wrapMode: Text.Wrap
            Accessible.name: text
        }

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
                AppComboBox {
                    id: directorySourceBox
                    objectName: "directorySourceBox"
                    Layout.preferredWidth: 170
                    storm: true
                    model: [qsTr("Your server"), root.mrsServer,
                            qsTr("Other server…")]
                    Accessible.name: qsTr("Room directory")
                    onActivated: (index) => root.chooseDirectorySource(index)
                }
                AppTextField {
                    id: directoryServerField
                    objectName: "directoryServerField"
                    visible: root.otherDirectoryServer
                    Layout.preferredWidth: 150
                    storm: true
                    placeholderText: qsTr("Server (optional)")
                    text: app.discovery.directory.server
                    onEditingFinished:
                        app.discovery.directory.server = text
                }
            }

            Label {
                objectName: "directoryThirdPartyNote"
                Layout.fillWidth: true
                visible: app.discovery.directory.server === root.mrsServer
                text: qsTr("matrixrooms.info is a third-party directory: your homeserver sends it your search terms.")
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textMeta
                wrapMode: Text.Wrap
                Accessible.name: text
            }

            ListView {
                id: directoryList
                objectName: "directoryResultsList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: AppTheme.spacing4
                model: app.discovery.directory
                ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
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
                    required property var via

                    readonly property string visibleName:
                        name.length > 0 ? name
                                        : (alias.length > 0 ? alias : roomId)
                    readonly property bool joined: membership === "joined"
                    readonly property bool knocked: membership === "knocked"
                        || root.knockedRoomIds[roomId] === true
                    readonly property bool rowKnocks:
                        joinRule === "knock" || joinRule === "knock_restricted"
                    readonly property bool rowPending:
                        root.pendingJoinRoomId === roomId
                        && (root.busy || root.knockInFlight)
                    readonly property bool primaryAvailable:
                        joined || (!knocked && !root.busy
                                   && !root.knockInFlight)
                    function activate() {
                        if (joined) {
                            app.openRoom(roomId)
                            root.close()
                            return
                        }
                        if (!primaryAvailable)
                            return
                        root.pendingJoinRoomId = roomId
                        if (rowKnocks) {
                            if (!root.startKnock(roomId, via, ""))
                                root.pendingJoinRoomId = ""
                        } else {
                            app.discovery.join(roomId, via, isSpace)
                        }
                    }
                    activeFocusOnTab: true
                    onActiveFocusChanged: if (activeFocus && ListView.view)
                        ListView.view.positionViewAtIndex(index, ListView.Contain)
                    Accessible.role: Accessible.Button
                    Accessible.name: joined
                        ? qsTr("Open %1").arg(visibleName)
                        : (knocked ? qsTr("Request pending for %1").arg(visibleName)
                                   : (rowKnocks ? qsTr("Ask to join %1").arg(visibleName)
                                                : qsTr("Join %1").arg(visibleName)))
                    Keys.onReturnPressed: activate()
                    Keys.onEnterPressed: activate()
                    Keys.onSpacePressed: activate()
                    Rectangle {
                        objectName: "directoryRowFocusRing"
                        anchors.fill: parent
                        radius: directoryRow.radius
                        color: "transparent"
                        border.width: 2
                        border.color: AppTheme.bolt
                        visible: directoryRow.activeFocus
                    }

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
                                    textFormat: Text.PlainText
                                    text: directoryRow.visibleName
                                    color: AppTheme.stormText
                                    font.family: AppTheme.menuFont
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                    elide: Label.ElideRight
                                    Layout.maximumWidth: 240
                                }
                                StatusChip {
                                    storm: true
                                    visible: directoryRow.isSpace
                                    tone: "accent"
                                    label: qsTr("Space")
                                }
                                StatusChip {
                                    storm: true
                                    visible: !directoryRow.joined
                                             && root.joinRuleLabel(
                                                 directoryRow.joinRule).length > 0
                                    tone: directoryRow.rowKnocks ? "warning"
                                                                 : "neutral"
                                    label: root.joinRuleLabel(directoryRow.joinRule)
                                }
                            }
                            Label {
                                textFormat: Text.PlainText
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
                                font.pixelSize: AppTheme.textMeta
                                elide: Label.ElideRight
                                maximumLineCount: 1
                            }
                        }
                        Label {
                            visible: directoryRow.knocked
                            text: qsTr("Request pending")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textMeta
                        }
                        AppButton {
                            visible: directoryRow.joined
                            storm: true
                            focusPolicy: Qt.NoFocus
                            text: qsTr("Open")
                            onClicked: directoryRow.activate()
                        }
                        AppButton {
                            visible: !directoryRow.joined && !directoryRow.knocked
                            storm: true
                            kind: "primary"
                            focusPolicy: Qt.NoFocus
                            enabled: directoryRow.primaryAvailable
                            text: directoryRow.rowPending
                                  ? (directoryRow.rowKnocks ? qsTr("Requesting…")
                                                            : qsTr("Joining…"))
                                  : (directoryRow.rowKnocks ? qsTr("Ask to join")
                                                            : qsTr("Join"))
                            onClicked: directoryRow.activate()
                        }
                    }
                }

                Item {
                    anchors.fill: parent
                    visible: directoryList.count === 0

                    AppBusyIndicator {
                        anchors.centerIn: parent
                        running: app.discovery.directory.state === "loading"
                        visible: running
                        color: AppTheme.bolt
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: app.discovery.directory.state === "no_results"
                        text: qsTr("No rooms found")
                        color: AppTheme.stormTextMuted
                        font.pixelSize: AppTheme.textBody
                    }
                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: AppTheme.spacing8
                        visible: app.discovery.directory.state === "error"
                        Label {
                            textFormat: Text.PlainText
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
                font.pixelSize: AppTheme.textMeta
            }
        }

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
                    onTextChanged: {
                        if (root.addressText !== text)
                            root.addressKnockSent = false
                        root.addressText = text
                    }
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

            AppBusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                running: app.discovery.resolveState === "resolving"
                visible: running
                color: AppTheme.bolt
            }

            Label {
                Layout.fillWidth: true
                visible: app.discovery.resolveState === "failed"
                text: root.resolved.category === "not_a_room"
                      ? qsTr("That link points at a person, not a room.")
                      : qsTr("That is not a valid room address or link.")
                color: AppTheme.danger
                font.pixelSize: AppTheme.textBody
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                wrapMode: Text.Wrap
            }

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
                                textFormat: Text.PlainText
                                color: AppTheme.stormText
                                font.family: AppTheme.menuFont
                                font.pixelSize: AppTheme.textSubtitle
                                font.weight: AppTheme.weightBold
                                elide: Label.ElideRight
                                Layout.fillWidth: true
                            }
                            Label {
                                textFormat: Text.PlainText
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
                                font.pixelSize: AppTheme.textMeta
                                elide: Label.ElideRight
                            }
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: root.resolvedPreview
                                 && (root.resolved.topic || "").length > 0
                        text: root.resolved.topic || ""
                        textFormat: Text.PlainText
                        color: AppTheme.stormTextSecondary
                        font.pixelSize: AppTheme.textBody
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                        wrapMode: Text.Wrap
                        maximumLineCount: 3
                        elide: Label.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        objectName: "resolvedNoPreviewLabel"
                        visible: root.resolvedOk && !root.resolvedPreview
                                 && !root.resolvedNotFound
                        text: qsTr("No preview is available for this room — "
                                   + "the server does not allow previewing "
                                   + "it. You can still try to join.")
                        color: AppTheme.stormTextMuted
                        font.pixelSize: AppTheme.textBody
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                        wrapMode: Text.Wrap
                    }
                    Label {
                        objectName: "resolvedNotFoundLabel"
                        Layout.fillWidth: true
                        visible: root.resolvedNotFound
                        text: qsTr("No room with that address could be found.")
                        color: AppTheme.danger
                        font.pixelSize: AppTheme.textBody
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: root.resolvedMembership === "banned"
                        text: qsTr("You are banned from this room.")
                        color: AppTheme.danger
                        font.pixelSize: AppTheme.textBody
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
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
                                     || root.addressKnockSent
                            text: qsTr("Request pending")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textMeta
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
                            visible: !root.resolvedNotFound
                                     && root.resolvedMembership !== "joined"
                                     && root.resolvedMembership !== "knocked"
                                     && root.resolvedMembership !== "banned"
                                     && !root.addressKnockSent
                            storm: true
                            kind: "primary"
                            enabled: !root.busy && !root.knockInFlight
                            text: root.knockInFlight
                                  ? qsTr("Requesting…")
                                  : root.busy
                                  ? qsTr("Joining…")
                                  : (root.resolvedCanKnock
                                     ? qsTr("Ask to join")
                                     : (root.resolvedMembership === "invited"
                                        ? qsTr("Accept invite")
                                        : qsTr("Join room")))
                            onClicked: {
                                var target = root.resolved.target || ""
                                var via = root.resolved.via || []
                                if (root.resolvedCanKnock) {
                                    root.startKnock(target, via,
                                                    root.knockReasonText)
                                } else
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
