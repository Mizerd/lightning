import QtQuick
import QtQuick.Controls
import MatrixClient

Rectangle {
    id: dot

    // Resolved through one guarded property (as Avatar.qml's `bridge`): a
    // delegate created inside a property-change handler can see its first
    // unqualified `app` lookup resolve undefined. Must stay the first `app`
    // reference in this file; re-resolved from _sync(). Unresolved means no
    // watch and no render.
    property var presenceService:
        (typeof app !== "undefined" && app) ? app.presence : null

    function resolvePresence() {
        if (!presenceService && typeof app !== "undefined" && app)
            presenceService = app.presence
    }

    property string userId: ""
    property color ring: AppTheme.surface
    property int dotSize: 10

    readonly property string presenceState:
        userId !== "" && presenceService && presenceService.revision >= 0
            ? presenceService.stateFor(userId) : ""

    readonly property string statusText: {
        if (userId === "" || !presenceService)
            return ""
        var rev = presenceService.revision
        var info = presenceService.infoFor(userId)
        if (!info || info.state === undefined)
            return ""
        var status = (info.statusMsg && info.statusMsg.length > 0)
                     ? " \u00b7 " + info.statusMsg : ""
        if (info.state === "online")
            return qsTr("Online") + status
        if (info.state === "unavailable")
            return qsTr("Away") + status
        if (info.state !== "offline")
            return ""
        var ago = info.lastActiveAgoMs
        if (ago === undefined || ago < 0)
            return qsTr("Offline") + status
        var mins = Math.floor(ago / 60000)
        if (mins < 1)
            return qsTr("Offline \u2014 active just now") + status
        if (mins < 60)
            return qsTr("Offline \u2014 active %1 min ago").arg(mins) + status
        var hours = Math.floor(mins / 60)
        if (hours < 24)
            return qsTr("Offline \u2014 active %1 h ago").arg(hours) + status
        return qsTr("Offline \u2014 active %1 d ago").arg(Math.floor(hours / 24)) + status
    }

    property bool hoverStatus: false

    property string _watched: ""

    readonly property int _epoch:
        presenceService ? presenceService.sessionEpoch : 0
    on_EpochChanged: {
        _watched = ""
        _sync()
    }

    function _sync() {
        resolvePresence()
        if (_watched === userId)
            return;
        if (_watched !== "" && presenceService)
            presenceService.unwatch(_watched);
        _watched = "";
        if (userId !== "" && presenceService) {
            presenceService.watch(userId);
            _watched = userId;
        }
    }

    onUserIdChanged: _sync()
    Component.onCompleted: _sync()
    Component.onDestruction: {
        if (_watched !== "" && presenceService)
            presenceService.unwatch(_watched);
        _watched = "";
    }

    visible: presenceState !== ""
    width: dotSize
    height: dotSize
    radius: dotSize / 2
    color: ring

    Rectangle {
        objectName: "presenceDotDisc"
        anchors.fill: parent
        anchors.margins: 2
        radius: width / 2
        readonly property bool _offline:
            dot.presenceState !== "online"
            && dot.presenceState !== "unavailable"
        color: dot.presenceState === "online" ? AppTheme.presenceDot
             : dot.presenceState === "unavailable" ? AppTheme.presenceAway
                                                   : "transparent"
        border.width: _offline ? Math.max(1, Math.round(dot.dotSize * 0.2)) : 0
        border.color: AppTheme.presenceOffline
    }

    HoverHandler {
        id: statusHover
        enabled: dot.hoverStatus && dot.visible
    }
    ToolTip.text: dot.statusText
    ToolTip.visible: dot.hoverStatus && statusHover.hovered
                     && dot.statusText.length > 0
    ToolTip.delay: 300
    Accessible.role: Accessible.StaticText
    Accessible.ignored: !dot.hoverStatus || dot.statusText.length === 0
    Accessible.name: dot.statusText
}
