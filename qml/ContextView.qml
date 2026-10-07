import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Read-only context view: one message and the messages around it, fetched with
// /context when a search result or reply target is not in the loaded timeline.
// Backed by app.eventContext (ContextController); its rows are an ordinary
// TimelineModel rendered by MessageDelegate with the read-only contract
// (`readOnlyContext`): no hover actions, no reactions, no reply, no menu.
// It sits over the timeline area and never touches the live timeline.
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

    // Media entry points and room facts supplied by TimelinePane.
    property var openImage: function(mediaKey, httpUrl) {}
    // (mediaKey, filename, mimetype, ask): ask = "Save as…", otherwise
    // Download (app.downloads).
    property var saveMedia: function(mediaKey, filename, mime, ask) {}
    property var currentRoom: ({})

    property string highlightEventId: ""
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
    }
    Connections {
        target: app.eventContext
        function onTargetLocated(row) {
            // The ListView needs a turn to build delegates for the new model.
            Qt.callLater(function() { view.placeTarget(row) })
        }
    }
    Connections {
        target: app.eventContext.model
        function onOlderPrepended(count) {
            // Keep the reader on the row they were on: the new rows are above.
            Qt.callLater(function() {
                list.positionViewAtIndex(count, ListView.Beginning)
            })
        }
    }
    Shortcut {
        sequence: "Escape"
        enabled: view.visible
        onActivated: app.eventContext.jumpToLatest()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Top bar
        Rectangle {
            objectName: "contextTopBar"
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
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

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
                clip: true
                spacing: 0
                model: app.eventContext.model
                topMargin: AppTheme.spacingS
                bottomMargin: AppTheme.spacingS
                leftMargin: AppTheme.spacing12
                rightMargin: AppTheme.spacing12
                boundsBehavior: Flickable.StopAtBounds

                // MessageDelegate view contract (see ThreadPanel). Read-only:
                // the delegate hides its action bar, reactions, reply and menu.
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
                function stateGroupExpanded(groupId) { return true }
                function toggleStateGroup(groupId) {}
                property var openImage: view.openImage
                property var saveMedia: view.saveMedia
                property var openReactionPicker: function(eventId, point, expanded) {}
                property var openSenderProfile: function(member) {}
                property string navigationHighlightEventId: view.highlightEventId
                // A reply quote inside the view jumps within it when loaded;
                // otherwise it re-routes through the one navigation path,
                // which reopens the view on that message.
                property var navigateToEvent: function(eventId) {
                    if (!eventId)
                        return
                    var row = app.eventContext.model.rowForStableId(eventId)
                    if (row >= 0) {
                        view.highlightEventId = eventId
                        highlightTimer.restart()
                        list.positionViewAtIndex(row, ListView.Center)
                    } else {
                        // The single navigation path: it replaces this view
                        // with one on the new target (or falls back).
                        app.pagination.jumpToEvent(eventId)
                    }
                }

                delegate: MessageDelegate {
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
                            enabled: app.eventContext.canLoadNewer
                            text: app.eventContext.newerFailed
                                  ? qsTr("Could not load. Try again")
                                  : app.eventContext.canLoadNewer
                                    ? qsTr("Load newer messages")
                                    : qsTr("Use Jump to latest for newer messages")
                            onClicked: app.eventContext.loadNewer()
                        }
                    }
                }

                ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
            }
        }
    }
}
