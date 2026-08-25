import QtQuick
import QtQuick.Layouts
import MatrixClient

// The call grid, built over SURFACES rather than over people.
//
// One entry per SHARE and one entry per PARTICIPANT, in that order. A person
// sharing with their camera on occupies two cells; two people sharing occupy
// two cells. This is the fix for "make sure multiple users can screen share":
// the previous grid was a GridView over the participant list alone, whose
// delegate never set `mediaKind`, so a screen share was not merely
// un-spotlighted — it was not rendered ANYWHERE, and only the first sharer was
// reachable at all.
//
// Shares come FIRST because that is what everyone is looking at. The order is
// applied HERE and not in `CallParticipantModel`: reordering the source model
// would churn `beginMoveRows` on every speaker change, and the rows move for
// reasons that have nothing to do with the view.
//
// TWO REPEATERS, NOT A VIEW. A GridView takes one model, and building a merged
// JS array out of two models is the exact defect CallParticipantModel exists to
// remove: a JS array reassigned is a MODEL RESET, so every tile — and with it
// every VideoOutput and its attach()/detach() pair — would be destroyed and
// rebuilt on every speaker update. Two Repeaters bound to the two real models
// give per-row `dataChanged`, which is what makes an amplitude-driven speaking
// ring possible at all. A call has tens of participants, not thousands, so the
// virtualization a view would have brought is worth nothing here.
//
// Positioning is computed rather than laid out, for one reason a Flow cannot
// give: the LAST ROW IS CENTRED. Three tiles read as 2-over-1-centred, which
// is what a call client looks like; a Flow would left-align the odd one.
Item {
    id: root

    /// `app.groupCall.shareModel` and `app.groupCall.participantModel`. Bound
    /// as models directly — never copied into an array.
    property var shareModel: null
    property var participantModel: null

    /// Nobody in this call is sending video: draw circular avatars on the
    /// canvas instead of a grid of empty panels.
    property bool voiceOnly: false

    /// Compact tiles — smaller glyphs and type, for the strip beside a
    /// spotlight.
    property bool compact: false

    /// Highlight state, for the strip form where one surface is on the
    /// spotlight. Empty in the ordinary grid.
    property string focusedShareId: ""
    property string focusedIdentity: ""

    signal shareActivated(string shareId)
    signal participantActivated(string identity)

    readonly property int shareCount: root.shareModel ? root.shareModel.count : 0
    readonly property int peopleCount: root.participantModel ? root.participantModel.count : 0
    readonly property int total: root.shareCount + root.peopleCount

    readonly property int gap: AppTheme.spacing8
    /// Cell width divided by cell height. 16:9 for video tiles — the shape a
    /// camera frame and a screen both fit into. A voice-only cell is square:
    /// it holds a circle and a name, and a 16:9 box around a circle is mostly
    /// empty canvas.
    readonly property real cellAspect: root.voiceOnly ? 1.0 : (16 / 9)

    /// Column count that maximises the area of one cell.
    ///
    /// No Discord documentation states the reflow rule for N participants and
    /// none of the numbers here are measurements of Discord — this is the
    /// standard "try every column count, keep the best" search, which
    /// reproduces the arrangements everyone recognises: 2 side by side, 3 as
    /// 2-over-1, 4 as 2x2.
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
            // Strictly greater, so a tie keeps the SMALLER column count and
            // therefore the smaller row count — a 2x2 rather than a 4x1.
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

    /// x of cell `i` (0-based over shares then participants). Reads only
    /// declared properties, so a binding calling it re-evaluates when any of
    /// them changes — QML captures property reads THROUGH a function call.
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

    // ── Shares first ─────────────────────────────────────────────────────
    //
    // The delegate root is a plain Item that RECEIVES the roles as required
    // properties and the tile reads them from it. Marking the tile's own
    // same-named properties required would be shorter, but that syntax is
    // used nowhere else in this tree and this round cannot build to find out
    // — the wrapper is the pattern the old stage already used.
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

    // ── Then the people ──────────────────────────────────────────────────
    Repeater {
        id: personTiles
        model: root.participantModel
        delegate: Item {
            id: personCell
            required property int index
            required property string identity
            required property string userId
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
            required property string connectionQuality

            // Offset by the share count: shares occupy the first cells.
            x: root.cellX(root.shareCount + personCell.index)
            y: root.cellY(root.shareCount + personCell.index)
            width: root.cellWidth
            height: root.cellHeight

            CallParticipantTile {
                anchors.fill: parent
                identity: personCell.identity
                userId: personCell.userId
                displayName: personCell.displayName
                avatarMxc: personCell.avatarMxc
                local: personCell.local
                micKnown: personCell.micKnown
                micMuted: personCell.micMuted
                cameraKnown: personCell.cameraKnown
                cameraOn: personCell.cameraOn
                cameraTrackKey: personCell.cameraTrackKey
                // The screen-share BADGE only. This tile never renders the
                // person's screen: the share has its own tile above, and two
                // surfaces asking the router for one participant's screen
                // would blank each other.
                screenSharing: personCell.screenSharing
                mediaKind: "camera"
                speaking: personCell.speaking
                speakingLevel: personCell.speakingLevel
                handRaised: personCell.handRaised
                connectionQuality: personCell.connectionQuality
                bare: root.voiceOnly
                compact: root.compact
                focused: personCell.identity === root.focusedIdentity
                onActivated: root.participantActivated(personCell.identity)
            }
        }
    }
}
