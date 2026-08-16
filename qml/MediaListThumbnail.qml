import QtQuick
import MatrixClient

// Lazy compact preview for Room Information → Media. Requests the bridge's
// list-thumbnail class only while its virtualized delegate intersects the
// viewport. That class uses Matrix thumbnails and never substitutes an
// encrypted full attachment merely to fill this tile.
Rectangle {
    id: root

    required property string mediaKey
    property bool visual: false
    property bool video: false
    property bool onScreen: false
    property string resolvedSource: ""
    property bool failed: false

    implicitWidth: 42
    implicitHeight: 34
    radius: AppTheme.radiusSm
    color: AppTheme.inputBackground
    border.width: 1
    border.color: AppTheme.border
    clip: true

    function refresh() {
        if (!visual || !onScreen || mediaKey.length === 0
                || !app.mediaBridge.supported || failed)
            return
        var value = app.mediaBridge.mediaSource(mediaKey, "list_thumb")
        if (value.length > 0) resolvedSource = value
    }

    onOnScreenChanged: refresh()
    onMediaKeyChanged: {
        resolvedSource = ""
        failed = false
        refresh()
    }
    Component.onCompleted: refresh()

    Connections {
        target: app.mediaBridge
        function onMediaCached(cacheKey) {
            if (cacheKey === "listthumb:" + root.mediaKey)
                root.refresh()
        }
        function onMediaFetchFailed(cacheKey, category) {
            if (cacheKey === "listthumb:" + root.mediaKey)
                root.failed = true
        }
        function onMediaRetryable(cacheKey) {
            if (cacheKey === "listthumb:" + root.mediaKey) {
                root.failed = false
                root.refresh()
            }
        }
    }

    Image {
        anchors.fill: parent
        visible: root.resolvedSource.length > 0
        source: visible ? root.resolvedSource + "|shape:round:120" : ""
        sourceSize: Qt.size(96, 72)
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        cache: true
    }

    Icon {
        anchors.centerIn: parent
        visible: root.resolvedSource.length === 0
        name: root.failed ? "description" : root.video ? "videocam" : "image"
        size: 17
        color: root.failed ? AppTheme.textDisabled : AppTheme.textMuted
    }
}
