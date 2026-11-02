import QtQuick
import QtQuick.Layouts
import MatrixClient

Item {
    id: root

    property var shareModel: null
    property var participantModel: null

    property bool voiceOnly: false

    property bool compact: false

    property string focusedShareId: ""
    property string focusedIdentity: ""

    signal shareActivated(string shareId)
    signal participantActivated(string identity)

    readonly property int shareCount: root.shareModel ? root.shareModel.count : 0
    readonly property int peopleCount: root.participantModel ? root.participantModel.count : 0
    readonly property int total: root.shareCount + root.peopleCount

    readonly property int gap: AppTheme.spacing8
    readonly property real cellAspect: root.voiceOnly ? 1.0 : (16 / 9)

    readonly property int columns: {
        var n = Math.max(1, root.total);
        if (root.width <= 0 || root.height <= 0)
            return 1;
        var best = 1;
        var bestArea = -1;
        for (var c = 1; c <= n; ++c) {
            var rows = Math.ceil(n / c);
            var w = (root.width - (c - 1) * root.gap) / c;
            var h = (root.height - (rows - 1) * root.gap) / rows;
            if (w <= 0 || h <= 0)
                continue;
            var usableW = Math.min(w, h * root.cellAspect);
            var area = usableW * (usableW / root.cellAspect);
            if (area > bestArea) {
                bestArea = area;
                best = c;
            }
        }
        return best;
    }
    readonly property int rows: Math.max(1, Math.ceil(Math.max(1, root.total) / root.columns))
    readonly property real cellWidth: {
        if (root.width <= 0 || root.height <= 0)
            return 0;
        var w = (root.width - (root.columns - 1) * root.gap) / root.columns;
        var h = (root.height - (root.rows - 1) * root.gap) / root.rows;
        return Math.max(0, Math.min(w, h * root.cellAspect));
    }
    readonly property real cellHeight: root.cellWidth / root.cellAspect

    readonly property real _blockHeight:
        root.rows * root.cellHeight + (root.rows - 1) * root.gap
    readonly property real _topOffset:
        Math.max(0, (root.height - root._blockHeight) / 2)

    function cellX(i) {
        if (root.columns <= 0 || root.cellWidth <= 0)
            return 0;
        var r = Math.floor(i / root.columns);
        var inRow = Math.min(root.columns, root.total - r * root.columns);
        var rowWidth = inRow * root.cellWidth + (inRow - 1) * root.gap;
        var startX = Math.max(0, (root.width - rowWidth) / 2);
        return startX + (i - r * root.columns) * (root.cellWidth + root.gap);
    }
    function cellY(i) {
        if (root.columns <= 0)
            return 0;
        return root._topOffset
                + Math.floor(i / root.columns) * (root.cellHeight + root.gap);
    }

    Repeater {
        id: shareTiles
        model: root.shareModel
        delegate: Item {
            id: shareCell
            required property int index
            required property string shareId
            required property string ownerIdentity
            required property string ownerDisplayName
            required property string trackKey
            required property bool local

            x: root.cellX(shareCell.index)
            y: root.cellY(shareCell.index)
            width: root.cellWidth
            height: root.cellHeight

            CallShareTile {
                anchors.fill: parent
                shareId: shareCell.shareId
                ownerIdentity: shareCell.ownerIdentity
                ownerDisplayName: shareCell.ownerDisplayName
                trackKey: shareCell.trackKey
                local: shareCell.local
                compact: root.compact
                focused: shareCell.shareId === root.focusedShareId
                onActivated: root.shareActivated(shareCell.shareId)
            }
        }
    }

    Repeater {
        id: personTiles
        model: root.participantModel
        delegate: Item {
            id: personCell
            required property int index
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
            // element-call's transient reaction, empty while none is
            // playing. Required, so a model lacking it fails at load.
            required property string reactionEmoji
            required property string connectionQuality

            x: root.cellX(root.shareCount + personCell.index)
            y: root.cellY(root.shareCount + personCell.index)
            width: root.cellWidth
            height: root.cellHeight

            CallParticipantTile {
                anchors.fill: parent
                identity: personCell.identity
                userId: personCell.userId
                deviceId: personCell.deviceId
                displayName: personCell.displayName
                avatarMxc: personCell.avatarMxc
                local: personCell.local
                micKnown: personCell.micKnown
                micMuted: personCell.micMuted
                cameraKnown: personCell.cameraKnown
                cameraOn: personCell.cameraOn
                cameraTrackKey: personCell.cameraTrackKey
                screenSharing: personCell.screenSharing
                mediaKind: "camera"
                speaking: personCell.speaking
                speakingLevel: personCell.speakingLevel
                handRaised: personCell.handRaised
                reactionEmoji: personCell.reactionEmoji
                connectionQuality: personCell.connectionQuality
                bare: root.voiceOnly
                compact: root.compact
                focused: personCell.identity === root.focusedIdentity
                onActivated: root.participantActivated(personCell.identity)
            }
        }
    }
}
