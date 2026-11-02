import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: view
    objectName: "contextView"
    color: AppTheme.background
    visible: app.eventContext.active
    // Swallow input so nothing reaches the live timeline underneath. The list
    // takes wheel events over its own area; this takes the rest (top bar,
    // list edges) and accepts them, so the hidden timeline never scrolls.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }

    property var openImage: function(mediaKey, httpUrl) {}
    property var openArtwork: function(source, title) {}
    property var saveMedia: function(mediaKey, filename, mime, ask) {}
    property var currentRoom: ({})

    property string highlightEventId: ""
    property bool targetPositioned: false
    property bool targetBuilt: false
    property bool landingPending: false
    property bool barDismissed: false
    function loadTowards(delta) {
        if (delta < 0 && !app.eventContext.olderFailed
                && list.contentY - list.originY + list.topMargin < list.height)
            app.eventContext.loadOlder()
        else if (delta > 0 && !app.eventContext.newerFailed
                 && list.originY + list.contentHeight + list.bottomMargin
                    - list.contentY < 2 * list.height)
            app.eventContext.loadNewer()
    }
    Timer {
        id: highlightTimer
        interval: 2400
        onTriggered: view.highlightEventId = ""
    }

    function placeTarget(row) {
        if (row < 0 || row >= list.count)
            return
        view.highlightEventId = app.eventContext.eventId
        highlightTimer.restart()
        list.positionViewAtIndex(row, ListView.Center)
        view.targetPositioned = true
        view.landingPending = view.targetBuilt
    }
    Connections {
        target: app.eventContext
        function onStateChanged() {
            if (app.eventContext.state === ContextController.Opening) {
                view.targetPositioned = false
                view.targetBuilt = false
                view.landingPending = false
                view.barDismissed = false
                list.lastSeenRow = -1
                list.lastContentY = 0
            }
        }
        function onTargetLocated(row) {
            Qt.callLater(function() { view.placeTarget(row) })
        }
    }
    Connections {
        target: view.Window.window
        enabled: view.landingPending
        function onFrameSwapped() {
            if (!view.landingPending || !view.targetPositioned || !view.targetBuilt)
                return
            view.landingPending = false
            app.eventContext.tracePhase("landed")
        }
    }
    Connections {
        target: app.eventContext.model
        function onOlderPrepended(count) {
            if (list.lastSeenRow >= 0)
                list.lastSeenRow += count
            Qt.callLater(function() {
                // ListView keeps a reader who is past the first row in place;
                // only one sitting on that row is pushed down by the insert.
                var first = list.itemAtIndex(count)
                if (!first || first.y < list.contentY)
                    return
                contextWheel.stopGlide()
                list.positionViewAtIndex(count, ListView.Beginning)
            })
        }
    }
    function takeFocus() {
        if (view.visible && app.eventContext.state === ContextController.Ready)
            list.forceActiveFocus(Qt.TabFocusReason)
    }
    onVisibleChanged: Qt.callLater(view.takeFocus)
    Connections {
        target: app.eventContext
        function onStateChanged() { Qt.callLater(view.takeFocus) }
    }
    Shortcut {
        sequence: "Escape"
        enabled: view.visible
        onActivated: app.eventContext.jumpToLatest()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            objectName: "contextTopBar"
            visible: !view.barDismissed
            Layout.fillWidth: true
            implicitHeight: barRow.implicitHeight + AppTheme.spacing12 * 2
            color: AppTheme.surface
            RowLayout {
                id: barRow
                anchors.fill: parent
                anchors.leftMargin: AppTheme.spacing12
                anchors.rightMargin: AppTheme.spacing12
                spacing: AppTheme.spacing12
                Icon {
                    name: "info"
                    size: 18
                    color: AppTheme.textMuted
                }
                Label {
                    objectName: "contextBarTitle"
                    Layout.fillWidth: true
                    text: qsTr("Viewing an older message")
                    color: AppTheme.textPrimary
                    font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                    font.weight: AppTheme.weightBold
                    elide: Text.ElideRight
                }
                AppButton {
                    objectName: "contextJumpToLatest"
                    kind: "primary"
                    size: "sm"
                    text: qsTr("Jump to latest")
                    onClicked: app.eventContext.jumpToLatest()
                }
                IconButton {
                    objectName: "contextBarDismiss"
                    iconName: "close"
                    size: "md"
                    Accessible.name: qsTr("Dismiss")
                    ToolTip.text: qsTr("Dismiss")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: view.barDismissed = true
                }
            }
        }
        Rectangle {
            visible: !view.barDismissed
            Layout.fillWidth: true
            implicitHeight: 1
            color: AppTheme.border
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            AppBusyIndicator {
                anchors.centerIn: parent
                visible: app.eventContext.state === ContextController.Opening
                running: visible
                size: 24
            }

            ListView {
                id: list
                objectName: "contextList"
                anchors.fill: parent
                visible: app.eventContext.state === ContextController.Ready
                onVisibleChanged: {
                    if (visible)
                        app.eventContext.tracePhase("view-swap")
                }
                clip: true
                spacing: 0
                model: app.eventContext.model
                topMargin: AppTheme.spacingS
                bottomMargin: AppTheme.spacingS
                leftMargin: AppTheme.spacing12
                rightMargin: AppTheme.spacing12
                boundsBehavior: Flickable.StopAtBounds

                SmoothWheelArea {
                    id: contextWheel
                    onWheel: (event) => view.loadTowards(
                        -(event.pixelDelta.y || event.angleDelta.y))
                }
                property real lastContentY: 0
                property int lastSeenRow: -1
                onContentYChanged: {
                    var delta = contentY - lastContentY
                    lastContentY = contentY
                    if (moving || contextWheel.active || contextWheel.gliding)
                        view.loadTowards(delta)
                    var row = indexAt(width / 2, contentY + height / 2)
                    if (row >= 0)
                        lastSeenRow = row
                    emptyViewportCheck.restart()
                }
                // Written directly, contentY can land in estimated height that
                // holds no rows, and ListView does not bring it back.
                Timer {
                    id: emptyViewportCheck
                    interval: 120
                    onTriggered: {
                        if (list.indexAt(list.width / 2,
                                         list.contentY + list.height / 2) >= 0
                                || list.lastSeenRow < 0
                                || list.lastSeenRow >= list.count)
                            return
                        contextWheel.stopGlide()
                        list.positionViewAtIndex(list.lastSeenRow, ListView.Center)
                    }
                }

                activeFocusOnTab: true
                Keys.onPressed: (event) => {
                    var step = 48
                    var page = Math.max(step, list.height - step)
                    var top = list.originY - list.topMargin
                    var bottom = list.originY + list.contentHeight
                                 + list.bottomMargin - list.height
                    function go(y) {
                        var next = Math.max(top, Math.min(bottom, y))
                        view.loadTowards(next - list.contentY)
                        list.contentY = next
                        event.accepted = true
                    }
                    if (event.key === Qt.Key_Down) go(list.contentY + step)
                    else if (event.key === Qt.Key_Up) go(list.contentY - step)
                    else if (event.key === Qt.Key_PageDown)
                        go(list.contentY + page)
                    else if (event.key === Qt.Key_PageUp)
                        go(list.contentY - page)
                    else if (event.key === Qt.Key_Home) go(top)
                    else if (event.key === Qt.Key_End) go(bottom)
                }

                property var timelineModel: app.eventContext.model
                property bool readOnlyContext: true
                property string suppressRootEventId: ""
                property bool threadContext: false
                property string pinnedActionsKey: ""
                property string hoveredActionsKey: ""
                property bool emojiPickerOpen: false
                property string transientInteractionOwner: ""
                function claimTransientInteraction(owner) {}
                function releaseTransientInteraction(owner, fallback) {}
                property bool roomEncrypted: view.currentRoom
                                             && view.currentRoom.encrypted === true
                property bool isDirectRoom: view.currentRoom
                                            && view.currentRoom.isDirect === true
                property string roomJoinRule: view.currentRoom
                                              ? (view.currentRoom.joinRule || "") : ""
                function stateGroupExpanded(groupId) { return true }
                function toggleStateGroup(groupId) {}
                property var openImage: view.openImage
                property var openArtwork: view.openArtwork
                property var saveMedia: view.saveMedia
                property string downloadSurface: "context"
                property var openReactionPicker: function(eventId, point, expanded) {}
                property var openSenderProfile: function(member) {}
                property string navigationHighlightEventId: view.highlightEventId
                property var navigateToEvent: function(eventId) {
                    if (!eventId)
                        return
                    var row = app.eventContext.model.rowForStableId(eventId)
                    if (row >= 0) {
                        view.highlightEventId = eventId
                        highlightTimer.restart()
                        list.positionViewAtIndex(row, ListView.Center)
                    } else {
                        app.pagination.jumpToEvent(eventId)
                    }
                }

                delegate: MessageDelegate {
                    Component.onCompleted: {
                        if (app.eventContext.traceEnabled
                                && app.eventContext.state === ContextController.Ready
                                && index === app.eventContext.targetRow()) {
                            view.targetBuilt = true
                            app.eventContext.tracePhase("first-target-row-built")
                            view.landingPending = view.targetPositioned
                        }
                    }
                    width: {
                        var available = ListView.view
                                      ? ListView.view.width
                                        - AppTheme.spacing12 * 2 : 0
                        return available > 0 ? available : 316
                    }
                }

                header: Item {
                    width: list.width
                    height: olderRow.implicitHeight + AppTheme.spacing12
                    visible: !app.eventContext.reachedStart
                    Row {
                        id: olderRow
                        anchors.centerIn: parent
                        spacing: 8
                        AppBusyIndicator {
                            size: 14
                            visible: app.eventContext.loadingOlder
                            running: visible
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        AppButton {
                            objectName: "contextLoadOlder"
                            kind: "ghost"
                            size: "sm"
                            visible: !app.eventContext.loadingOlder
                            enabled: app.eventContext.canLoadOlder
                            text: app.eventContext.olderFailed
                                  ? qsTr("Could not load. Try again")
                                  : app.eventContext.canLoadOlder
                                    ? qsTr("Load older messages")
                                    : qsTr("No more older messages here")
                            onClicked: app.eventContext.loadOlder()
                        }
                    }
                }
                footer: Item {
                    width: list.width
                    height: newerRow.implicitHeight + AppTheme.spacing12
                    visible: !app.eventContext.reachedEnd
                    Row {
                        id: newerRow
                        anchors.centerIn: parent
                        spacing: 8
                        AppBusyIndicator {
                            size: 14
                            visible: app.eventContext.loadingNewer
                            running: visible
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        AppButton {
                            objectName: "contextLoadNewer"
                            kind: "ghost"
                            size: "sm"
                            visible: !app.eventContext.loadingNewer
                            text: app.eventContext.newerFailed
                                  ? qsTr("Could not load. Try again")
                                  : app.eventContext.canLoadNewer
                                    ? qsTr("Load newer messages")
                                    : qsTr("Jump to latest")
                            onClicked: app.eventContext.canLoadNewer
                                       ? app.eventContext.loadNewer()
                                       : app.eventContext.jumpToLatest()
                        }
                    }
                }

                ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
            }
        }
    }
}
