import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The call surface — Discord's layout and interaction, Lightning's tokens.
//
// Shown whenever `app.groupCall` has a live call. It replaces the timeline
// rather than floating over it: a call is the thing the user is doing, and
// the persistent "Voice Connected" bar is what lets them leave it visually
// without leaving it actually.
//
// Layout follows the participant count rather than a mode the user has to
// choose, with a manual override for the cases where the automatic answer is
// wrong (someone pinned, or a screen share you want to step out of).
Rectangle {
    id: root

    objectName: "callStage"
    color: AppTheme.stormCanvas

    /// Manually spotlighted participant identity; empty follows the
    /// automatic layout.
    property string focusedIdentity: ""
    /// "auto" | "grid" | "spotlight"
    property string layoutMode: "auto"

    // app.groupCall's participant list is a C++ function call, which Qt
    // cannot observe for change, so a tick is bumped by the signal and READ
    // by the bindings. Never assign over a binding — bump a counter it
    // reads (the media-cache lesson).
    property int refreshTick: 0
    readonly property var people: {
        var _ = root.refreshTick
        return app.groupCall.participants()
    }
    readonly property int peopleCount: {
        var _ = root.refreshTick
        return app.groupCall.participantCount
    }
    readonly property bool someoneSharing: {
        var _ = root.refreshTick
        for (var i = 0; i < people.length; ++i) {
            if (people[i].screenSharing)
                return true
        }
        return false
    }

    Connections {
        target: app.groupCall
        function onParticipantsChanged() {
            root.refreshTick = root.refreshTick + 1
        }
        function onMediaStateChanged() {
            root.refreshTick = root.refreshTick + 1
        }
    }

    readonly property string effectiveLayout: {
        if (root.layoutMode !== "auto")
            return root.layoutMode
        // A screen share is the reason everyone is looking, so it takes the
        // stage automatically; likewise a manual pin.
        if (root.someoneSharing || root.focusedIdentity.length > 0)
            return "spotlight"
        return "grid"
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: AppTheme.spacing16
        spacing: AppTheme.spacing12

        // Header: who and where, plus the connection state in plain words.
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            Icon {
                name: "call"
                size: 18
                color: AppTheme.accent
            }
            Text {
                Layout.fillWidth: true
                text: {
                    var _ = root.refreshTick
                    var n = root.peopleCount
                    if (n <= 0)
                        return qsTr("Connecting…")
                    return n === 1 ? qsTr("1 person in call")
                                   : qsTr("%1 people in call").arg(n)
                }
                color: AppTheme.stormText
                font.pixelSize: 14
                font.weight: Font.Medium
                elide: Text.ElideRight
            }
            // Reconnecting and degraded states are SHOWN, never left as a
            // frozen picture — a call that looks fine while it is not is
            // the failure users report as "it just stopped".
            Loader {
                active: app.groupCall.state === SfuCallController.Reconnecting
                visible: active
                sourceComponent: Text {
                    text: qsTr("Reconnecting…")
                    color: AppTheme.warning
                    font.pixelSize: 12
                }
            }
            Loader {
                active: app.groupCall.mediaEncrypted
                visible: active
                sourceComponent: Icon {
                    name: "lock"
                    size: 14
                    color: AppTheme.success
                }
            }
        }

        // The stage itself.
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            // GRID — everyone the same size, wrapping responsively. The
            // column count follows the count and the available width, so a
            // narrow window with a side panel open does not explode.
            Loader {
                anchors.fill: parent
                active: root.effectiveLayout === "grid"
                visible: active
                sourceComponent: GridView {
                    id: grid
                    objectName: "callGrid"
                    clip: true
                    model: root.people
                    readonly property int columns: {
                        var n = Math.max(1, root.peopleCount)
                        var byWidth = Math.max(1, Math.floor(width / 200))
                        return Math.max(1, Math.min(byWidth,
                                                    Math.ceil(Math.sqrt(n))))
                    }
                    cellWidth: Math.floor(width / columns)
                    cellHeight: Math.min(cellWidth * 0.72, height)
                    delegate: Item {
                        required property var modelData
                        width: grid.cellWidth
                        height: grid.cellHeight
                        CallParticipantTile {
                            anchors.fill: parent
                            anchors.margins: 6
                            userId: parent.modelData.userId
                            // Routes video. The identity, not userId+deviceId:
                            // the sticky format's identity is a hash.
                            identity: parent.modelData.identity
                            displayName: app.displayNameFor
                                         ? app.displayNameFor(parent.modelData.userId)
                                         : parent.modelData.userId
                            micKnown: parent.modelData.micKnown
                            micMuted: parent.modelData.micMuted
                            cameraKnown: parent.modelData.cameraKnown
                            cameraOn: parent.modelData.cameraOn
                            screenSharing: parent.modelData.screenSharing
                            speaking: parent.modelData.speaking
                            local: parent.modelData.local
                            onActivated: root.focusedIdentity = parent.modelData.identity
                        }
                    }
                }
            }

            // SPOTLIGHT — one large surface (a screen share, or a pinned
            // person) with everyone else in a strip beneath, which is the
            // arrangement that makes shared content readable.
            Loader {
                anchors.fill: parent
                active: root.effectiveLayout === "spotlight"
                visible: active
                sourceComponent: ColumnLayout {
                    spacing: AppTheme.spacing8

                    Rectangle {
                        objectName: "callSpotlight"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: AppTheme.radiusTile
                        color: AppTheme.stormInset
                        border.width: 1
                        border.color: AppTheme.stormBorder

                        // Video rendering is not attached yet, so this says
                        // WHO holds the stage rather than drawing a black
                        // rectangle that looks like a broken stream.
                        ColumnLayout {
                            anchors.centerIn: parent
                            spacing: AppTheme.spacing8
                            Icon {
                                Layout.alignment: Qt.AlignHCenter
                                name: root.someoneSharing ? "screen_share" : "person"
                                size: 32
                                color: AppTheme.stormTextMuted
                            }
                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: root.someoneSharing
                                      ? qsTr("Someone is sharing their screen")
                                      : qsTr("Spotlight")
                                color: AppTheme.stormTextSecondary
                                font.pixelSize: 13
                            }
                        }

                        AppButton {
                            objectName: "callBackToGridButton"
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.margins: AppTheme.spacing8
                            size: "sm"
                            text: qsTr("Back to grid")
                            onClicked: {
                                root.focusedIdentity = ""
                                root.layoutMode = "grid"
                            }
                        }
                    }

                    // Strip of everyone else, compact.
                    ListView {
                        objectName: "callStrip"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 96
                        orientation: ListView.Horizontal
                        spacing: AppTheme.spacing8
                        clip: true
                        model: root.people
                        delegate: CallParticipantTile {
                            required property var modelData
                            compact: true
                            height: 88
                            width: 140
                            userId: modelData.userId
                            identity: modelData.identity
                            displayName: app.displayNameFor
                                         ? app.displayNameFor(modelData.userId)
                                         : modelData.userId
                            micKnown: modelData.micKnown
                            micMuted: modelData.micMuted
                            cameraKnown: modelData.cameraKnown
                            cameraOn: modelData.cameraOn
                            screenSharing: modelData.screenSharing
                            speaking: modelData.speaking
                            local: modelData.local
                            focused: modelData.identity === root.focusedIdentity
                            onActivated: root.focusedIdentity = modelData.identity
                        }
                    }
                }
            }
        }

        // Control bar, centred.
        CallControlBar {
            objectName: "callStageControls"
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            participantCount: root.peopleCount
            onLayoutCycleRequested: {
                root.layoutMode = root.effectiveLayout === "grid"
                                  ? "spotlight" : "grid"
            }
            onHangUpRequested: app.groupCall.leave()
        }
    }
}
