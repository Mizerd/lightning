import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import MatrixClient

// v0.5.11: shared avatar element used across the whole UI (room list, Space
// rail, room header, Room Information and invite results). A Matrix Space is
// a room, so it uses the same account-scoped room-avatar mechanism.
//
// It resolves the mxc URI through the shared MediaBridge (dedup, bounded
// cache, account-separated, cleared on sign-out) and shows a stable initial
// placeholder until — and unless — a real bitmap is available. The bitmap is
// only shown once fully loaded, so a broken-image icon never appears and a
// stale avatar never flashes when the mxc changes (delegate reuse, account
// switch, logout, avatar-URL change). The bitmap is masked to the avatar
// shape with a MultiEffect (a Rectangle's `clip` ignores `radius`, which would
// otherwise leave square corners).
Rectangle {
    id: root

    // mxc:// avatar URI (empty → initials only).
    property string mxc: ""
    // Name used for the initial placeholder.
    property string name: ""
    property int size: 40
    property bool circle: true

    implicitWidth: size
    implicitHeight: size
    width: size
    height: size
    radius: circle ? size / 2 : AppTheme.radiusSm
    color: AppTheme.cardElevated

    readonly property bool hasImage:
        mxc.length > 0 && app.mediaBridge && app.mediaBridge.supported
    property string src: ""

    function refresh() {
        src = hasImage ? app.mediaBridge.avatarSource(mxc, Math.max(32, size * 2))
                       : ""
    }
    onMxcChanged: refresh()
    onSizeChanged: refresh()
    Component.onCompleted: refresh()

    Connections {
        target: app.mediaBridge
        enabled: root.hasImage
        function onMediaCached(cacheKey) { root.refresh() }
    }

    Label {
        anchors.centerIn: parent
        visible: img.status !== Image.Ready
        text: (root.name && root.name.length > 0)
              ? root.name.charAt(0).toUpperCase() : "?"
        color: AppTheme.textSecondary
        font.pixelSize: Math.max(10, Math.round(root.size * 0.42))
        font.weight: Font.DemiBold
    }

    Image {
        id: img
        anchors.fill: parent
        source: root.src
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        cache: true
        visible: false // shown via the masked MultiEffect below
    }

    // Rounded mask matching the avatar shape.
    Item {
        id: maskShape
        anchors.fill: parent
        visible: false
        layer.enabled: true
        Rectangle {
            anchors.fill: parent
            radius: root.radius
            color: "black"
        }
    }

    MultiEffect {
        anchors.fill: parent
        source: img
        maskEnabled: true
        maskSource: maskShape
        // Only shown once fully decoded — no broken-image glyph, no flash.
        visible: img.status === Image.Ready
    }
}
