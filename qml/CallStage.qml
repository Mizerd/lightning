import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import MatrixClient

Rectangle {
    id: root

    objectName: "callStage"
    color: AppTheme.stormCanvas

    StreamInfoOverlay {
        objectName: "callStageStreamInfo"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: AppTheme.spacing12
        anchors.topMargin: stageColumn.y + callHeaderRow.y + callHeaderRow.height
                           + AppTheme.spacing8
        width: Math.max(0, Math.min(480, parent.width - 2 * AppTheme.spacing12))
        height: Math.max(0, Math.min(implicitHeight, parent.height
                                     - anchors.topMargin - AppTheme.spacing12))
        z: 100
        visible: app.groupCall.streamInfoVisible && !root.fullScreenActive
        controller: app.groupCall
        stats: app.groupCall.streamInfo
    }

    signal participantsRequested()

    property bool collapsed: false
    signal collapseToggled()

    // ── Data layer ──
    // Models are bound directly, never copied into a JS array: reassigning an
    // array is a model reset, which would rebuild every tile (and its
    // VideoOutput) on every speaker-level update.
    readonly property var participantModel: app.groupCall.participantModel
    readonly property var shareModel: app.groupCall.shareModel
    readonly property var stageState: app.groupCall.stageState

    readonly property int peopleCount:
        root.participantModel ? root.participantModel.count : 0
    readonly property int shareCount:
        root.shareModel ? root.shareModel.count : 0

    // Number of live cameras. A JS scan isn't a binding, so the model's change
    // signals drive an explicit recount. dataChanged also fires for speaker
    // levels; the walk is cheap for call-sized lists.
    property int camerasOn: 0
    function recountCameras() {
        var n = 0;
        if (root.participantModel) {
            for (var i = 0; i < root.participantModel.count; ++i) {
                var row = root.participantModel.get(i);
                if (row && row.cameraOn === true)
                    ++n;
            }
        }
        root.camerasOn = n;
    }
    Connections {
        target: root.participantModel
        function onDataChanged() { root.recountCameras() }
        function onCountChanged() { root.recountCameras() }
    }
    Component.onCompleted: {
        root.recountCameras();
        // The host's Loader rebuilds the stage on every room change while the
        // call outlives it, so full screen may already be on at creation.
        root.syncFullScreenWindow();
    }

    readonly property bool voiceOnly:
        root.shareCount === 0 && root.camerasOn === 0

    readonly property int spotlightShareRow: {
        var _ = root.shareCount;
        if (!root.shareModel || !root.stageState
                || root.stageState.spotlightShareId.length === 0)
            return -1;
        return root.shareModel.indexOfShare(root.stageState.spotlightShareId);
    }
    readonly property int pinnedRow: {
        var _ = root.peopleCount;
        if (!root.participantModel || !root.stageState
                || root.stageState.pinnedIdentity.length === 0)
            return -1;
        return root.participantModel.indexOfIdentity(
                    root.stageState.pinnedIdentity);
    }
    readonly property bool spotlightHasSurface:
        root.spotlightShareRow >= 0 || root.pinnedRow >= 0

    readonly property string effectiveLayout: {
        if (!root.stageState)
            return "grid";
        if (root.stageState.layoutPreference === "grid")
            return "grid";
        if (root.stageState.layoutPreference === "spotlight")
            return "spotlight";
        return (root.stageState.spotlightShareId.length > 0
                || root.stageState.pinnedIdentity.length > 0)
                ? "spotlight" : "grid";
    }

    readonly property int minimumTileStrip: 80
    readonly property int bubbleStripHeight: 44

    readonly property int spotlightOverlayHeight: 30 + 2 * AppTheme.spacing8

    readonly property int minimumPictureHeight: 132
    readonly property int minimumUsefulHeight:
        2 * AppTheme.spacing12
        + 48 + 2 * AppTheme.spacing8 + AppTheme.spacing8
        + root.minimumPictureHeight

    function stripModeForStage(available) {
        if (!available || available <= 0)
            return "tiles";
        return Math.floor((available - AppTheme.spacing8) * 0.4)
                >= root.minimumTileStrip ? "tiles" : "bubbles";
    }

    readonly property bool bubblesInHeader:
        !root.collapsed && !root.fullScreenActive
        && root.effectiveLayout === "spotlight"
        && root.width >= root.headerFacesMinimumWidth
    readonly property int headerFacesMinimumWidth: 640

    function stripHeightForStage(available) {
        if (!available || available <= 0)
            return 96;
        if (root.stripModeForStage(available) !== "tiles")
            return root.bubbleStripHeight;
        return Math.min(96,
                        Math.floor((available - AppTheme.spacing8) * 0.4));
    }

    function leaveSpotlight() {
        if (!root.stageState)
            return;
        if (root.stageState.spotlightShareId.length > 0)
            root.stageState.dismissShare(root.stageState.spotlightShareId);
        root.stageState.clearPin();
    }

    readonly property int idleTickMs: 500
    readonly property int idleTicksToHide: 6

    property int stageIdleTicks: 0
    readonly property bool stageChromeIdle:
        root.spotlightHasSurface && !root.fullScreenActive
        && root.stageIdleTicks >= root.idleTicksToHide

    readonly property bool fullScreenActive:
        root.stageState ? (root.stageState.fullScreen
                           && root.spotlightHasSurface)
                        : false

    function enterFullScreen() {
        if (root.stageState)
            root.stageState.setFullScreen(true);
    }
    function exitFullScreen() {
        if (root.stageState)
            root.stageState.setFullScreen(false);
    }

    function focusedSurfaceActivated() {
        if (root.fullScreenActive)
            root.exitFullScreen();
        else
            root.leaveSpotlight();
    }

    function syncFullScreenWindow() {
        if (!fullScreenWindow)
            return;
        if (root.fullScreenActive) {
            root.placeOnThisApplicationsScreen();
            fullScreenWindow.showFullScreen();
            fullScreenSurface.forceActiveFocus();
            if (root.stageState && root.stageState.traceEnabled) {
                console.info("call-fullscreen landed on="
                             + (fullScreenWindow.screen
                                ? fullScreenWindow.screen.name : "?"));
            }
        } else if (fullScreenWindow.visible) {
            fullScreenWindow.hide();
        }
    }

    Timer {
        id: stageIdleTimer
        objectName: "stageIdleTimer"
        interval: root.idleTickMs
        repeat: true
        running: root.spotlightHasSurface && !root.fullScreenActive
        onTriggered: root.stageIdleTicks += 1
    }

    property real lastPointerX: -1
    property real lastPointerY: -1
    HoverHandler {
        id: stageHover
        onPointChanged: {
            const p = stageHover.point.scenePosition;
            if (Math.abs(p.x - root.lastPointerX) < 1
                && Math.abs(p.y - root.lastPointerY) < 1)
                return;
            root.lastPointerX = p.x;
            root.lastPointerY = p.y;
            root.stageIdleTicks = 0;
        }
    }

    function placeOnThisApplicationsScreen() {
        var target = root.Screen;
        if (!target)
            return;
        fullScreenWindow.screen = target;
        fullScreenWindow.x = target.virtualX;
        fullScreenWindow.y = target.virtualY;
        fullScreenWindow.width = target.width;
        fullScreenWindow.height = target.height;
        if (root.stageState && root.stageState.traceEnabled) {
            console.info("call-fullscreen"
                         + " platform=" + Qt.platform.pluginName
                         + " appScreen=" + target.name
                         + " virtualX=" + target.virtualX
                         + " virtualY=" + target.virtualY
                         + " size=" + target.width + "x" + target.height);
        }
    }
    onFullScreenActiveChanged: root.syncFullScreenWindow()

    // ── The focused surface, defined once ──
    // Hosted by the spotlight or the full-screen window, never both: the router
    // holds one sink per key, so two surfaces would fight over it. Going full
    // screen rebuilds the video item (items can't move between scene graphs);
    // SfuVideoRouter's ownership rule (new surface claims the key, old one
    // releases by sink) keeps that rebuild from blanking the video.
    Component {
        id: focusedSurface

        Item {
            Repeater {
                model: root.shareModel
                delegate: Loader {
                    id: spotShare
                    required property string shareId
                    required property string ownerIdentity
                    required property string ownerDisplayName
                    required property string trackKey
                    required property bool local

                    anchors.fill: parent
                    active: root.stageState
                            && spotShare.shareId
                               === root.stageState.spotlightShareId
                    visible: active
                    sourceComponent: CallShareTile {
                        shareId: spotShare.shareId
                        ownerIdentity: spotShare.ownerIdentity
                        ownerDisplayName: spotShare.ownerDisplayName
                        trackKey: spotShare.trackKey
                        local: spotShare.local
                        focused: true
                        onActivated: root.focusedSurfaceActivated()
                    }
                }
            }

            Repeater {
                model: root.participantModel
                delegate: Loader {
                    id: spotPerson
                    required property string identity
                    required property string userId
                    required property string deviceId
                    required property string displayName
                    required property string avatarMxc
                    required property bool local
                    required property bool micKnown
                    required property bool micMuted
                    required property bool cameraKnown
                    required property bool cameraOn
                    required property string cameraTrackKey
                    required property bool screenSharing
                    required property bool speaking
                    required property real speakingLevel
                    required property bool handRaised
                    required property string reactionEmoji
                    required property string connectionQuality

                    anchors.fill: parent
                    active: root.stageState
                            && root.spotlightShareRow < 0
                            && spotPerson.identity
                               === root.stageState.pinnedIdentity
                    visible: active
                    sourceComponent: CallParticipantTile {
                        identity: spotPerson.identity
                        userId: spotPerson.userId
                        deviceId: spotPerson.deviceId
                        displayName: spotPerson.displayName
                        avatarMxc: spotPerson.avatarMxc
                        local: spotPerson.local
                        micKnown: spotPerson.micKnown
                        micMuted: spotPerson.micMuted
                        cameraKnown: spotPerson.cameraKnown
                        cameraOn: spotPerson.cameraOn
                        cameraTrackKey: spotPerson.cameraTrackKey
                        screenSharing: spotPerson.screenSharing
                        mediaKind: "camera"
                        speaking: spotPerson.speaking
                        speakingLevel: spotPerson.speakingLevel
                        handRaised: spotPerson.handRaised
                        reactionEmoji: spotPerson.reactionEmoji
                        connectionQuality: spotPerson.connectionQuality
                        focused: true
                        onActivated: root.focusedSurfaceActivated()
                    }
                }
            }

            Loader {
                anchors.centerIn: parent
                active: !root.spotlightHasSurface
                visible: active
                sourceComponent: Text {
                    text: qsTr("Nobody to show here yet")
                    color: AppTheme.stormTextSecondary
                    font.pixelSize: 13
                }
            }
        }
    }

    ColumnLayout {
        id: stageColumn
        anchors.fill: parent
        anchors.margins: root.collapsed ? AppTheme.spacing8 : AppTheme.spacing12
        spacing: AppTheme.spacing8

        RowLayout {
            id: callHeaderRow
            objectName: "callHeaderRow"
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            Icon {
                name: "call"
                size: 18
                color: AppTheme.accent
            }
            Text {
                Layout.fillWidth: true
                Layout.maximumWidth: root.collapsed ? implicitWidth : Number.POSITIVE_INFINITY
                Layout.preferredWidth: root.collapsed ? implicitWidth : 1
                text: {
                    var n = root.peopleCount;
                    if (n <= 0)
                        return qsTr("Connecting…");
                    return n === 1 ? qsTr("1 person in call")
                                   : qsTr("%1 people in call").arg(n);
                }
                color: AppTheme.stormText
                font.pixelSize: 14
                font.weight: Font.Medium
                elide: Text.ElideRight
            }
            Text {
                objectName: "callStageFormatLabel"
                visible: root.width >= root.headerFacesMinimumWidth
                text: qsTr("MatrixRTC")
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                HoverHandler { id: stageFormatHover }
                ToolTip.text: qsTr("MatrixRTC call: media goes through a call server")
                ToolTip.visible: stageFormatHover.hovered
                ToolTip.delay: 500
            }
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
                    name: app.groupCall.remoteMediaBlocked
                          ? "warning" : "lock"
                    size: 14
                    color: app.groupCall.remoteMediaBlocked
                           ? AppTheme.warning : AppTheme.success
                    ToolTip.visible: lockHover.hovered
                    ToolTip.delay: 400
                    ToolTip.text: app.groupCall.remoteMediaBlocked
                        ? qsTr("This call is encrypted, but media from "
                               + "someone here cannot be decrypted and is "
                               + "being dropped. You will not hear or see "
                               + "them.")
                        : qsTr("This call is end-to-end encrypted.")
                    HoverHandler { id: lockHover }
                }
            }

            Loader {
                objectName: "callHeaderMicSilentBadge"
                active: app.groupCall.microphoneSilent
                visible: active
                sourceComponent: Icon {
                    name: "warning"
                    size: 14
                    color: AppTheme.warning
                    ToolTip.visible: micSilentHover.hovered
                    ToolTip.delay: 400
                    ToolTip.text: qsTr("Your microphone is not picking "
                                       + "anything up, so nobody here can "
                                       + "hear you. Check which microphone "
                                       + "is selected in call settings.")
                    HoverHandler { id: micSilentHover }
                }
            }

            Loader {
                objectName: "callHeaderBubblesHost"
                Layout.fillWidth: root.collapsed
                Layout.preferredHeight: active ? implicitHeight : 0
                Layout.alignment: Qt.AlignVCenter
                active: (root.collapsed
                         && root.width >= root.headerFacesMinimumWidth)
                        || root.bubblesInHeader
                visible: active
                sourceComponent: CallSpeakerBubbles {
                    objectName: "callHeaderBubbles"
                    model: root.participantModel
                    onActivated: identity => {
                        if (root.stageState)
                            root.stageState.pin(identity);
                    }
                }
            }

            Loader {
                id: controlsHost
                active: true
                visible: true
                Layout.preferredHeight: implicitHeight
                Layout.minimumWidth: root.collapsed ? implicitWidth : 0
                Layout.fillWidth: true
                Layout.maximumWidth: implicitWidth
                Layout.alignment: Qt.AlignVCenter

                property bool cramped: false
                property real expandedNeed: 0

                function roomAvailable() {
                    return callHeaderRow.width
                           - (callHeaderRow.implicitWidth - implicitWidth);
                }

                Connections {
                    target: callHeaderRow
                    function onWidthChanged() {
                        controlsHost.reassessControlRoom();
                    }
                    function onImplicitWidthChanged() {
                        controlsHost.reassessControlRoom();
                    }
                }

                function reassessControlRoom() {
                    if (!item)
                        return;
                    if (!cramped) {
                        if (!root.collapsed)
                            expandedNeed = implicitWidth;
                        if (roomAvailable() + 0.5 < implicitWidth)
                            cramped = true;
                    } else if (expandedNeed > 0
                               && roomAvailable() + 0.5 >= expandedNeed) {
                        cramped = false;
                    }
                }
                onWidthChanged: reassessControlRoom()
                onImplicitWidthChanged: reassessControlRoom()
                Component.onCompleted: reassessControlRoom()
                sourceComponent: CallHeaderBar {
                    objectName: "callStageControls"
                    placement: "dock"
                    compact: root.collapsed || controlsHost.cramped
                    onParticipantsRequested: root.participantsRequested()
                }
            }

            Loader {
                active: !root.collapsed && root.stageState
                        && root.stageState.restorableShareAvailable
                visible: active
                sourceComponent: AppButton {
                    objectName: "callRestoreShareButton"
                    size: "sm"
                    kind: "primary"
                    storm: true
                    text: root.stageState.dismissedShareCount > 1
                          ? qsTr("Show screen shares (%1)")
                            .arg(root.stageState.dismissedShareCount)
                          : qsTr("Show screen share")
                    onClicked: root.stageState.restoreAllShares()
                }
            }

            CallControlButton {
                objectName: "callCollapseButton"
                iconName: root.collapsed ? "expand_more" : "expand_less"
                diameter: 30
                glyphSize: 16
                tooltip: root.collapsed ? qsTr("Expand the call")
                                        : qsTr("Collapse the call")
                onClicked: root.collapseToggled()
            }
        }

        // ── Why there is no picture ── With Qt Quick's software renderer there
        // is no video node, so calls carry audio but draw no frames. The tiles
        // stay in their placeholder state (see `softwareRendererHidesVideo`)
        // and this one line explains why. Zero height on machines with a usable
        // GPU.
        Rectangle {
            objectName: "callSoftwareRendererNotice"
            visible: !root.collapsed
                     && typeof app !== "undefined" && app
                     && app.softwareRenderer === true
            Layout.fillWidth: true
            implicitHeight: visible ? noticeRow.implicitHeight
                                      + AppTheme.spacing8 * 2 : 0
            color: AppTheme.warningFill
            radius: AppTheme.radiusSm

            RowLayout {
                id: noticeRow
                anchors.fill: parent
                anchors.margins: AppTheme.spacing8
                spacing: AppTheme.spacing8

                Icon {
                    name: "warning"
                    size: 16
                    color: AppTheme.warning
                    Layout.alignment: Qt.AlignVCenter
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: AppTheme.stormText
                    font.pixelSize: 12
                    // One string literal: lupdate cannot extract qsTr("a" +
                    // "b").
                    text: qsTr("Video can't be shown on this computer — there is no working graphics acceleration, so cameras and shared screens won't appear. Audio is unaffected.")
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !root.collapsed
            clip: true

            // Grid: every surface the same size, shares first. Stood down in
            // full screen so two tiles never compete for one routing key.
            Loader {
                anchors.fill: parent
                active: !root.collapsed && !root.fullScreenActive
                        && root.effectiveLayout === "grid"
                visible: active
                sourceComponent: CallTileGrid {
                    objectName: "callGrid"
                    shareModel: root.shareModel
                    participantModel: root.participantModel
                    voiceOnly: root.voiceOnly
                    onShareActivated: shareId => {
                        if (root.stageState)
                            root.stageState.restoreShare(shareId);
                    }
                    onParticipantActivated: identity => {
                        if (root.stageState)
                            root.stageState.pin(identity);
                    }
                }
            }

            Loader {
                anchors.fill: parent
                active: !root.collapsed && !root.fullScreenActive
                        && root.effectiveLayout === "spotlight"
                visible: active
                sourceComponent: ColumnLayout {
                    id: spotlightColumn
                    spacing: AppTheme.spacing8

                    readonly property string stripMode:
                        root.stripModeForStage(spotlightColumn.height)
                    readonly property int stripHeight:
                        root.stripHeightForStage(spotlightColumn.height)
                    readonly property int stripTileHeight:
                        spotlightColumn.stripHeight - AppTheme.spacing8
                    readonly property int stripTileWidth:
                        Math.round(spotlightColumn.stripTileHeight * 140 / 88)

                    Rectangle {
                        objectName: "callSpotlight"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: AppTheme.radiusTile
                        color: AppTheme.stormInset
                        border.width: 1
                        border.color: AppTheme.stormBorder
                        clip: true

                        Loader {
                            anchors.fill: parent
                            anchors.margins: 1
                            sourceComponent: focusedSurface
                        }

                        MouseArea {
                            objectName: "callSpotlightCursor"
                            anchors.fill: parent
                            z: 100
                            visible: root.stageChromeIdle
                            acceptedButtons: Qt.NoButton
                            cursorShape: Qt.BlankCursor
                        }

                        RowLayout {
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.margins: AppTheme.spacing8
                            spacing: AppTheme.spacing8
                            visible: parent.height
                                     >= root.spotlightOverlayHeight
                                        + AppTheme.spacing8
                            opacity: root.stageChromeIdle ? 0 : 1
                            enabled: !root.stageChromeIdle
                            Behavior on opacity {
                                enabled: !AppTheme.reducedMotion
                                NumberAnimation { duration: 180 }
                            }

                            CallControlButton {
                                objectName: "callStreamInfoButton"
                                iconName: "speed"
                                diameter: 30
                                glyphSize: 16
                                role: app.groupCall.streamInfoVisible ? "active" : "neutral"
                                tooltip: app.groupCall.streamInfoVisible
                                         ? qsTr("Hide Stream Info") : qsTr("Show Stream Info")
                                onClicked: app.groupCall.streamInfoVisible
                                           = !app.groupCall.streamInfoVisible
                            }

                            CallControlButton {
                                objectName: "callFullScreenButton"
                                iconName: "fit_screen"
                                diameter: 30
                                glyphSize: 16
                                tooltip: qsTr("Full screen")
                                onClicked: root.enterFullScreen()
                            }

                            AppButton {
                                objectName: "callBackToGridButton"
                                storm: true
                                size: "sm"
                                text: qsTr("Back to grid")
                                onClicked: root.leaveSpotlight()
                            }
                        }
                    }

                    Flickable {
                        objectName: "callStrip"
                        Layout.fillWidth: true
                        visible: spotlightColumn.stripMode === "tiles"
                                 && !root.bubblesInHeader
                        Layout.preferredHeight: visible
                                                ? spotlightColumn.stripHeight
                                                : 0
                        contentWidth: stripRow.width
                        contentHeight: height
                        flickableDirection: Flickable.HorizontalFlick
                        boundsBehavior: Flickable.StopAtBounds
                        clip: true

                        Row {
                            id: stripRow
                            height: spotlightColumn.stripTileHeight
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: AppTheme.spacing8

                            Repeater {
                                model: root.shareModel
                                delegate: Loader {
                                    id: stripShare
                                    required property string shareId
                                    required property string ownerIdentity
                                    required property string ownerDisplayName
                                    required property string trackKey
                                    required property bool local

                                    height: spotlightColumn.stripTileHeight
                                    width: active
                                           ? spotlightColumn.stripTileWidth : 0
                                    active: spotlightColumn.stripMode === "tiles"
                                            && (!root.stageState
                                                || stripShare.shareId
                                                   !== root.stageState.spotlightShareId)
                                    visible: active
                                    sourceComponent: CallShareTile {
                                        compact: true
                                        shareId: stripShare.shareId
                                        ownerIdentity: stripShare.ownerIdentity
                                        ownerDisplayName: stripShare.ownerDisplayName
                                        trackKey: stripShare.trackKey
                                        local: stripShare.local
                                        onActivated: {
                                            if (root.stageState)
                                                root.stageState.restoreShare(
                                                    stripShare.shareId);
                                        }
                                    }
                                }
                            }

                            Repeater {
                                model: root.participantModel
                                delegate: Loader {
                                    id: stripPerson
                                    required property string identity
                                    required property string userId
                                    required property string deviceId
                                    required property string displayName
                                    required property string avatarMxc
                                    required property bool local
                                    required property bool micKnown
                                    required property bool micMuted
                                    required property bool cameraKnown
                                    required property bool cameraOn
                                    required property string cameraTrackKey
                                    required property bool screenSharing
                                    required property bool speaking
                                    required property real speakingLevel
                                    required property bool handRaised
                                    required property string reactionEmoji
                                    required property string connectionQuality

                                    height: spotlightColumn.stripTileHeight
                                    width: active
                                           ? spotlightColumn.stripTileWidth : 0
                                    active: spotlightColumn.stripMode === "tiles"
                                            && (root.spotlightShareRow >= 0
                                                || !root.stageState
                                                || stripPerson.identity
                                                   !== root.stageState.pinnedIdentity)
                                    visible: active
                                    sourceComponent: CallParticipantTile {
                                        compact: true
                                        identity: stripPerson.identity
                                        userId: stripPerson.userId
                                        deviceId: stripPerson.deviceId
                                        displayName: stripPerson.displayName
                                        avatarMxc: stripPerson.avatarMxc
                                        local: stripPerson.local
                                        micKnown: stripPerson.micKnown
                                        micMuted: stripPerson.micMuted
                                        cameraKnown: stripPerson.cameraKnown
                                        cameraOn: stripPerson.cameraOn
                                        cameraTrackKey: stripPerson.cameraTrackKey
                                        screenSharing: stripPerson.screenSharing
                                        mediaKind: "camera"
                                        speaking: stripPerson.speaking
                                        speakingLevel: stripPerson.speakingLevel
                                        handRaised: stripPerson.handRaised
                                        reactionEmoji: stripPerson.reactionEmoji
                                        connectionQuality: stripPerson.connectionQuality
                                        onActivated: {
                                            if (root.stageState)
                                                root.stageState.pin(
                                                    stripPerson.identity);
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Loader {
                        objectName: "callStripBubblesHost"
                        Layout.fillWidth: true
                        active: spotlightColumn.stripMode !== "tiles"
                                && !root.bubblesInHeader
                        visible: active
                        Layout.preferredHeight: active ? implicitHeight : 0
                        sourceComponent: CallSpeakerBubbles {
                            objectName: "callStripBubbles"
                            model: root.participantModel
                            onActivated: identity => {
                                if (root.stageState)
                                    root.stageState.pin(identity);
                            }
                        }
                    }
                }
            }

            Loader {
                anchors.fill: parent
                active: !root.collapsed && root.fullScreenActive
                visible: active
                sourceComponent: Item {
                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: AppTheme.spacing8

                        Icon {
                            Layout.alignment: Qt.AlignHCenter
                            name: "fit_screen"
                            size: 28
                            color: AppTheme.stormTextSecondary
                        }
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("Playing full screen")
                            color: AppTheme.stormTextSecondary
                            font.pixelSize: 13
                        }
                        AppButton {
                            objectName: "callExitFullScreenFromStageButton"
                            Layout.alignment: Qt.AlignHCenter
                            storm: true
                            size: "sm"
                            text: qsTr("Exit full screen")
                            onClicked: root.exitFullScreen()
                        }
                    }
                }
            }
        }
    }

    Window {
        id: fullScreenWindow

        objectName: "callFullScreenWindow"
        title: qsTr("Lightning — full screen")
        color: "#000000"
        width: 1280
        height: 720

        onClosing: root.exitFullScreen()

        Item {
            id: fullScreenSurface
            objectName: "fullScreenSurface"
            anchors.fill: parent

            property bool overlaysIdle: false
            property int idleTicks: 0
            readonly property int idleTicksToHide: 6
            Timer {
                id: fullScreenIdleTimer
                objectName: "fullScreenIdleTimer"
                interval: 500
                repeat: true
                running: root.fullScreenActive
                onTriggered: {
                    fullScreenSurface.idleTicks += 1;
                    if (fullScreenSurface.idleTicks >= fullScreenSurface.idleTicksToHide)
                        fullScreenSurface.overlaysIdle = true;
                }
            }
            MouseArea {
                objectName: "callFullScreenCursor"
                anchors.fill: parent
                z: 100
                visible: root.fullScreenActive
                         && fullScreenSurface.overlaysIdle
                acceptedButtons: Qt.NoButton
                cursorShape: Qt.BlankCursor
            }
            property real lastPointerX: -1
            property real lastPointerY: -1
            HoverHandler {
                id: fullScreenHover
                enabled: root.fullScreenActive
                onPointChanged: {
                    const p = fullScreenHover.point.scenePosition;
                    if (Math.abs(p.x - fullScreenSurface.lastPointerX) < 1
                        && Math.abs(p.y - fullScreenSurface.lastPointerY) < 1)
                        return;
                    fullScreenSurface.lastPointerX = p.x;
                    fullScreenSurface.lastPointerY = p.y;
                    fullScreenSurface.idleTicks = 0;
                    fullScreenSurface.overlaysIdle = false;
                }
            }
            Connections {
                target: root
                function onFullScreenActiveChanged() {
                    fullScreenSurface.overlaysIdle = false
                    fullScreenSurface.idleTicks = 0
                }
            }
            focus: true

            // A Keys handler, not a Shortcut: Escape is reserved in
            // ShortcutRegistry and two enabled Shortcuts on one sequence fire
            // neither. A handler in a different window can't collide.
            Keys.onPressed: function (event) {
                if (event.key === Qt.Key_Escape) {
                    root.exitFullScreen();
                    event.accepted = true;
                }
            }

            Loader {
                anchors.fill: parent
                // Only while shown: a hidden Window still instantiates
                // children, and an unseen VideoOutput would hold a frame and a
                // router key.
                active: root.fullScreenActive
                visible: active
                sourceComponent: focusedSurface
            }

            StreamInfoOverlay {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: AppTheme.spacing12
                width: Math.max(0, Math.min(480, parent.width - 2 * AppTheme.spacing12))
                height: Math.max(0, Math.min(implicitHeight, parent.height - 2 * AppTheme.spacing12))
                z: 100
                visible: app.groupCall.streamInfoVisible && root.fullScreenActive
                controller: app.groupCall
                stats: app.groupCall.streamInfo
            }

            RowLayout {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: AppTheme.spacing12
                spacing: AppTheme.spacing8
                opacity: fullScreenSurface.overlaysIdle ? 0 : 1
                enabled: !fullScreenSurface.overlaysIdle
                Behavior on opacity {
                    enabled: !AppTheme.reducedMotion
                    NumberAnimation { duration: 180 }
                }

                CallControlButton {
                    objectName: "callFullScreenStreamInfoButton"
                    iconName: "speed"
                    diameter: 36
                    glyphSize: 18
                    role: app.groupCall.streamInfoVisible ? "active" : "neutral"
                    tooltip: app.groupCall.streamInfoVisible
                             ? qsTr("Hide Stream Info") : qsTr("Show Stream Info")
                    onClicked: app.groupCall.streamInfoVisible
                               = !app.groupCall.streamInfoVisible
                }

                CallControlButton {
                    objectName: "callExitFullScreenButton"
                    iconName: "close_fullscreen"
                    diameter: 36
                    glyphSize: 18
                    tooltip: qsTr("Exit full screen (Esc)")
                    onClicked: root.exitFullScreen()
                }
            }

            Loader {
                anchors.bottom: parent.bottom
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottomMargin: AppTheme.spacing16
                active: root.fullScreenActive
                visible: active
                opacity: fullScreenSurface.overlaysIdle ? 0 : 1
                enabled: !fullScreenSurface.overlaysIdle
                Behavior on opacity {
                    enabled: !AppTheme.reducedMotion
                    NumberAnimation { duration: 180 }
                }
                sourceComponent: CallHeaderBar {
                    objectName: "callFullScreenDock"
                    placement: "dock"
                    onParticipantsRequested: root.participantsRequested()
                }
            }
        }
    }
}
