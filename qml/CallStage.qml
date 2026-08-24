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

    /// The participant list was asked for from the dock. The host (the
    /// timeline pane) decides where it opens — the stage has no side panel of
    /// its own to put it in.
    signal participantsRequested()

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
        var _ = root.refreshTick;
        return app.groupCall.participants();
    }
    readonly property int peopleCount: {
        var _ = root.refreshTick;
        return app.groupCall.participantCount;
    }
    /// True when nothing in this call is sending video at all — the ordinary
    /// voice call, which is drawn as circular avatars on the canvas rather
    /// than a grid of empty panels.
    readonly property bool voiceOnly: {
        var _ = root.refreshTick;
        for (var i = 0; i < people.length; ++i) {
            if (people[i].screenSharing
                || (people[i].cameraKnown && people[i].cameraOn))
                return false;
        }
        return true;
    }
    readonly property bool someoneSharing: {
        var _ = root.refreshTick;
        for (var i = 0; i < people.length; ++i) {
            if (people[i].screenSharing)
                return true;
        }
        return false;
    }

    Connections {
        target: app.groupCall
        function onParticipantsChanged() {
            root.refreshTick = root.refreshTick + 1;
        }
        function onMediaStateChanged() {
            root.refreshTick = root.refreshTick + 1;
        }
    }

    /// The participant whose SCREEN is on the spotlight, if anyone's is.
    /// A share is why everyone is looking, so it outranks a manual pin.
    readonly property var sharingPerson: {
        var _ = root.refreshTick;
        for (var i = 0; i < people.length; ++i) {
            if (people[i].screenSharing)
                return people[i];
        }
        return null;
    }
    /// The manually pinned participant, if they are still in the call.
    readonly property var focusedPerson: {
        var _ = root.refreshTick;
        if (root.focusedIdentity.length === 0)
            return null;
        for (var i = 0; i < people.length; ++i) {
            if (people[i].identity === root.focusedIdentity)
                return people[i];
        }
        return null;
    }
    /// Whoever the spotlight is actually showing, and which of their tracks.
    readonly property var spotlightPerson:
        root.sharingPerson !== null ? root.sharingPerson : root.focusedPerson
    readonly property string spotlightKind:
        root.sharingPerson !== null ? "screen" : "camera"
    /// The strip below the spotlight, with the spotlighted person removed
    /// WHEN the spotlight is showing their camera.
    ///
    /// Not cosmetic de-duplication: the video router holds ONE sink per track
    /// key, so two surfaces asking for the same camera means the second attach
    /// replaces the first and the first destruction detaches the survivor —
    /// one of the two goes blank. A screen share has its own track key, so the
    /// sharer legitimately stays in the strip (as their camera or avatar),
    /// which is also where Discord leaves them.
    readonly property var stripPeople: {
        var _ = root.refreshTick;
        if (root.spotlightKind !== "camera" || root.spotlightPerson === null)
            return people;
        var out = [];
        for (var i = 0; i < people.length; ++i) {
            if (people[i].identity !== root.spotlightPerson.identity)
                out.push(people[i]);
        }
        return out;
    }

    readonly property string effectiveLayout: {
        if (root.layoutMode !== "auto")
            return root.layoutMode;
        // A screen share is the reason everyone is looking, so it takes the
        // stage automatically; likewise a manual pin.
        if (root.someoneSharing || root.focusedIdentity.length > 0)
            return "spotlight";
        return "grid";
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
                    var _ = root.refreshTick;
                    var n = root.peopleCount;
                    if (n <= 0)
                        return qsTr("Connecting…");
                    return n === 1 ? qsTr("1 person in call") : qsTr("%1 people in call").arg(n);
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

        // Who is here and who is talking, at a glance. Above the stage
        // because it answers a different question from it: the stage shows
        // what is being shown, this shows who is present.
        CallSpeakerBubbles {
            Layout.fillWidth: true
            people: root.people
            refreshTick: root.refreshTick
            onActivated: identity => root.focusedIdentity = identity
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
                        var n = Math.max(1, root.peopleCount);
                        var byWidth = Math.max(1, Math.floor(width / 200));
                        return Math.max(1, Math.min(byWidth, Math.ceil(Math.sqrt(n))));
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
                            // The MODEL resolved this from the MatrixRTC membership. Asking
                            // displayNameFor(userId) again was a second source of
                            // truth that returned nothing for a hashed identity.
                            displayName: parent.modelData.displayName || ""
                            avatarMxc: parent.modelData.avatarMxc || ""
                            micKnown: parent.modelData.micKnown
                            micMuted: parent.modelData.micMuted
                            cameraKnown: parent.modelData.cameraKnown
                            cameraOn: parent.modelData.cameraOn
                            screenSharing: parent.modelData.screenSharing
                            speaking: parent.modelData.speaking
                            local: parent.modelData.local
                            cameraTrackKey: parent.modelData.cameraTrackKey || ""
                            screenTrackKey: parent.modelData.screenTrackKey || ""
                            bare: root.voiceOnly
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

                        // The spotlight RENDERS. It used to draw a glyph and
                        // the words "someone is sharing their screen" over an
                        // empty rectangle — so a screen share was announced
                        // and never shown, which is exactly what "I did not
                        // see their screenshare" was. CallParticipantTile
                        // already owns video attachment, the avatar fallback
                        // and the honesty rules, so the stage reuses it here
                        // rather than growing a second one.
                        CallParticipantTile {
                            anchors.fill: parent
                            anchors.margins: 1
                            visible: root.spotlightPerson !== null
                            mediaKind: root.spotlightKind
                            userId: root.spotlightPerson
                                    ? (root.spotlightPerson.userId || "") : ""
                            identity: root.spotlightPerson
                                      ? (root.spotlightPerson.identity || "") : ""
                            displayName: root.spotlightPerson
                                         ? (root.spotlightPerson.displayName || "") : ""
                            avatarMxc: root.spotlightPerson
                                       ? (root.spotlightPerson.avatarMxc || "") : ""
                            micKnown: root.spotlightPerson
                                      ? root.spotlightPerson.micKnown === true : false
                            micMuted: root.spotlightPerson
                                      ? root.spotlightPerson.micMuted === true : false
                            cameraKnown: root.spotlightPerson
                                         ? root.spotlightPerson.cameraKnown === true : false
                            cameraOn: root.spotlightPerson
                                      ? root.spotlightPerson.cameraOn === true : false
                            screenSharing: root.spotlightPerson
                                           ? root.spotlightPerson.screenSharing === true : false
                            speaking: root.spotlightPerson
                                      ? root.spotlightPerson.speaking === true : false
                            local: root.spotlightPerson
                                   ? root.spotlightPerson.local === true : false
                            cameraTrackKey: root.spotlightPerson
                                            ? (root.spotlightPerson.cameraTrackKey || "") : ""
                            screenTrackKey: root.spotlightPerson
                                            ? (root.spotlightPerson.screenTrackKey || "") : ""
                        }

                        // Only when there is genuinely nobody to spotlight —
                        // a pinned participant who has left, for instance.
                        Loader {
                            anchors.centerIn: parent
                            active: root.spotlightPerson === null
                            visible: active
                            sourceComponent: Text {
                                text: qsTr("Nobody to show here yet")
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
                                root.focusedIdentity = "";
                                root.layoutMode = "grid";
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
                        model: root.stripPeople
                        delegate: CallParticipantTile {
                            required property var modelData
                            compact: true
                            height: 88
                            width: 140
                            userId: modelData.userId
                            identity: modelData.identity
                            displayName: modelData.displayName || ""
                            avatarMxc: modelData.avatarMxc || ""
                            micKnown: modelData.micKnown
                            micMuted: modelData.micMuted
                            cameraKnown: modelData.cameraKnown
                            cameraOn: modelData.cameraOn
                            screenSharing: modelData.screenSharing
                            speaking: modelData.speaking
                            local: modelData.local
                            cameraTrackKey: modelData.cameraTrackKey || ""
                            screenTrackKey: modelData.screenTrackKey || ""
                            focused: modelData.identity === root.focusedIdentity
                            onActivated: root.focusedIdentity = modelData.identity
                        }
                    }
                }
            }
        }

        // The control dock: ONE control surface, at the bottom of the stage,
        // where a call client puts it.
        //
        // It is the SAME component as the header bar, in its "dock"
        // placement — not a second control bar. That distinction is the whole
        // lesson of this surface: the stage used to carry a partial bar of
        // its own, and when the media controls moved to the header what was
        // left were two orphan buttons floating under the call UI. There is
        // one definition of the control set; the header instance hides itself
        // while this stage is showing (CallHeaderBar.stageOwnsControls), so
        // the controls are never drawn twice.
        CallHeaderBar {
            objectName: "callStageDock"
            placement: "dock"
            Layout.fillWidth: true
            Layout.topMargin: AppTheme.spacing4
            onParticipantsRequested: root.participantsRequested()
        }
    }
}
