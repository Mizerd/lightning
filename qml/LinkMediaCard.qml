import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// A link that is itself a video, or an image too large to show inline
// (LinkPreviewController: mediaKind "video", or "image" without mediaHeld).
// A cover shaped like the Matrix video card, and nothing is fetched until the
// reader asks: Play swaps VideoPlayerCard into the same box, View opens the
// image viewer. Both fetch through MediaBridge's "link:" key, so through the
// Rust safe fetch with its size cap (rust/src/linkmedia.rs). No URL ever
// reaches an Image or a MediaPlayer here. A played video gets its first frame
// as the cover, from the file Play materialized; a viewed image shows itself.
Item {
    id: root
    objectName: "linkMediaCard"

    // The row's loaded preview (LinkPreviewController::previewFor()).
    property var preview: ({})
    property real maximumWidth: 360
    // Row identity for app.playback (one audible card at a time).
    property string ownerKey: ""
    property bool rowOnScreen: true
    // False while the row must not react (multi-select mode).
    property bool interactive: true

    signal viewRequested()
    signal openInBrowserRequested()
    signal dismissRequested()
    // Play/View pressed on a card this row had not allowed itself; emitted
    // before the fetch starts, so the delegate records the row's consent.
    signal consentGiven()

    readonly property bool isVideo: (root.preview.mediaKind || "") === "video"
    readonly property string mediaKey: root.preview.mediaKey || ""
    readonly property real mediaSize: root.preview.mediaSize || 0
    // The fetch found it over the cap (the size was not declared up front).
    property bool fetchTooLarge: false
    readonly property bool tooLarge:
        root.preview.mediaTooLarge === true || root.fetchTooLarge
    property bool playerActive: false
    // The preview is per URL, so this row may show a card another room
    // loaded. Contacting the site from here needs this row's own consent, and
    // the card says what that costs before the press that gives it.
    readonly property bool needsConsent:
        root.preview.mediaAllowed !== true && !root.tooLarge
    readonly property string consentText:
        root.isVideo
        ? qsTr("Plays from %1, which will see your IP address")
              .arg(root.preview.host || "")
        : qsTr("Opens from %1, which will see your IP address")
              .arg(root.preview.host || "")
    // image://lightning-media/ sources only, from MediaBridge's cache.
    property string coverSource: ""

    function sizeText(bytes) {
        if (!bytes || bytes <= 0) return ""
        if (bytes < 1024) return qsTr("%1 B").arg(bytes)
        if (bytes < 1024 * 1024)
            return qsTr("%1 KB").arg(Math.round(bytes / 1024))
        return qsTr("%1 MB").arg((bytes / (1024 * 1024)).toFixed(1))
    }
    readonly property string kindText: root.isVideo ? qsTr("Video") : qsTr("Image")
    // "Video · 12.4 MB", or the kind alone when the size is unknown.
    readonly property string chipText: {
        var size = root.sizeText(root.mediaSize)
        return size.length > 0 ? root.kindText + " · " + size : root.kindText
    }
    readonly property string captionText:
        (root.preview.fileName || "").length > 0 ? root.preview.fileName
                                                 : (root.preview.host || "")
    readonly property string limitText: {
        var size = root.sizeText(root.mediaSize)
        if (size.length > 0)
            return size
        var cap = root.isVideo ? app.linkPreviews.linkVideoMaxBytes
                               : app.linkPreviews.linkImageMaxBytes
        return qsTr("Over %1").arg(root.sizeText(cap))
    }

    readonly property real boxWidth:
        Math.max(160, Math.min(360, root.maximumWidth > 0 ? root.maximumWidth
                                                          : 360))
    // An image keeps its own shape within bounds; a video's is unknown until
    // it plays, and the player letterboxes.
    readonly property real aspect:
        !root.isVideo && root.preview.imageWidth > 0 && root.preview.imageHeight > 0
        ? Math.min(1.25, Math.max(0.5, root.preview.imageHeight
                                        / root.preview.imageWidth))
        : 9 / 16
    implicitWidth: root.boxWidth
    // A floor so the caption, the affordance and the chip never overlap in a
    // narrow column; the player letterboxes.
    implicitHeight: Math.max(150, Math.round(root.boxWidth * root.aspect))

    function refreshCover() {
        if (root.mediaKey.length === 0) {
            root.coverSource = ""
            return
        }
        // Never dispatches: a cached poster (video) or the image the viewer
        // already fetched.
        root.coverSource = app.mediaBridge.cachedSource(
            (root.isVideo ? "thumb:" : "full:") + root.mediaKey)
    }
    // A pooled delegate may hand this card another link.
    onMediaKeyChanged: {
        root.playerActive = false
        root.fetchTooLarge = false
        root.refreshCover()
    }
    Component.onCompleted: root.refreshCover()

    function activate() {
        if (!root.interactive)
            return
        // Over the cap, or the preview behind this row has left the cache
        // (its byte budget): the browser, never a dead click.
        if (root.tooLarge || !app.linkPreviews.linkMediaAvailable(root.mediaKey)) {
            root.openInBrowserRequested()
            return
        }
        if (root.needsConsent)
            root.consentGiven()
        if (root.isVideo)
            root.playerActive = true
        else
            root.viewRequested()
    }

    Connections {
        target: app.mediaBridge
        function onMediaFetchFailed(cacheKey, category) {
            if (cacheKey !== "full:" + root.mediaKey)
                return
            if (category === "too_large") {
                root.fetchTooLarge = true
                root.playerActive = false
            }
        }
        function onPlayableMediaReady(cacheKey) {
            // The file Play wrote can give the cover its first frame.
            if (root.isVideo && cacheKey === "full:" + root.mediaKey
                && root.coverSource.length === 0)
                root.coverSource =
                    app.mediaBridge.videoPosterSource(root.mediaKey, 0)
        }
        function onMediaCached(cacheKey) {
            if (cacheKey === (root.isVideo ? "thumb:" : "full:") + root.mediaKey)
                root.coverSource = app.mediaBridge.cachedSource(cacheKey)
        }
    }

    Rectangle {
        id: cover
        objectName: "linkMediaCover"
        anchors.fill: parent
        radius: AppTheme.radiusSm
        color: AppTheme.embedSurface
        border.color: AppTheme.border
        border.width: 1
        clip: true
        visible: !root.playerActive

        Image {
            id: coverImage
            objectName: "linkMediaCoverImage"
            anchors.fill: parent
            anchors.margins: 1
            fillMode: root.isVideo ? Image.PreserveAspectCrop
                                   : Image.PreserveAspectFit
            source: root.coverSource
            sourceSize.width: 720
            asynchronous: true
            cache: true
            // The consent notice needs a plain surface to be read on.
            visible: status === Image.Ready && !root.needsConsent
        }

        // Identity while there is no picture: the kind and the file name.
        ColumnLayout {
            anchors.centerIn: parent
            anchors.verticalCenterOffset:
                root.tooLarge ? 0 : (root.needsConsent ? -34 : -26)
            width: parent.width - AppTheme.spacing8 * 4
            spacing: AppTheme.spacing4
            visible: !coverImage.visible || root.tooLarge
            Icon {
                name: root.tooLarge ? "link"
                                    : (root.isVideo ? "videocam" : "image")
                size: 26
                color: AppTheme.textMuted
                Layout.alignment: Qt.AlignHCenter
                visible: !coverImage.visible
            }
            Label {
                objectName: "linkMediaCaption"
                // Sender-chosen text: never markup.
                textFormat: Text.PlainText
                text: root.captionText
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                elide: Label.ElideMiddle
                horizontalAlignment: Text.AlignHCenter
                Layout.fillWidth: true
                visible: text.length > 0 && !coverImage.visible
            }
            Label {
                objectName: "linkMediaConsentNotice"
                visible: root.needsConsent
                textFormat: Text.PlainText
                text: root.consentText
                color: AppTheme.warning
                font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                // Never elided: this is what the press agrees to.
                elide: Label.ElideNone
                Layout.fillWidth: true
            }
            // Over the cap: said plainly, and the card opens the browser.
            Label {
                objectName: "linkMediaTooLarge"
                visible: root.tooLarge
                text: root.isVideo ? qsTr("Too large to play here")
                                   : qsTr("Too large to show here")
                color: AppTheme.text
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                font.weight: AppTheme.weightStrong
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Label {
                visible: root.tooLarge
                text: qsTr("%1 · Open in browser").arg(root.limitText)
                color: AppTheme.link
                font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        // Play or View.
        Rectangle {
            objectName: "linkMediaPlayAffordance"
            visible: !root.tooLarge
            anchors.centerIn: parent
            anchors.verticalCenterOffset:
                coverImage.visible ? 0 : (root.needsConsent ? 40 : 22)
            width: 44; height: 44; radius: 22
            color: AppTheme.overlayScrim
            Icon {
                anchors.centerIn: parent
                name: root.isVideo ? "play_arrow" : "zoom_in"
                size: 26
                color: AppTheme.scrimInk
            }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: 5
            radius: 3
            color: AppTheme.overlayScrim
            width: chipRow.implicitWidth + 10
            height: chipRow.implicitHeight + 4
            Row {
                id: chipRow
                anchors.centerIn: parent
                spacing: 4
                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: root.isVideo ? "videocam" : "image"
                    size: 11
                    color: AppTheme.scrimInk
                }
                Label {
                    objectName: "linkMediaChip"
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.chipText
                    color: AppTheme.scrimInk
                    font.pixelSize: AppTheme.fontMicro
                    font.weight: Font.Bold
                }
            }
        }

        MouseArea {
            objectName: "linkMediaActivate"
            anchors.fill: parent
            enabled: root.interactive && root.mediaKey.length > 0
            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: root.activate()
        }

        HoverHandler { id: coverHover }

        // On a scrim like the chip: over a poster the theme's own icon ink can
        // vanish into the picture.
        Rectangle {
            z: 3
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 4
            radius: AppTheme.radiusSm
            color: AppTheme.overlayScrim
            width: actionsRow.implicitWidth
            height: actionsRow.implicitHeight
            opacity: coverHover.hovered || openButton.hovered
                     || dismissButton.hovered || openButton.activeFocus
                     || dismissButton.activeFocus ? 1.0 : 0.6
            Row {
                id: actionsRow
                spacing: 0
                IconButton {
                    id: openButton
                    objectName: "linkMediaOpenButton"
                    size: "sm"
                    iconName: "link"
                    iconColorOverride: AppTheme.scrimInk
                    enabled: root.interactive
                    Accessible.name: qsTr("Open link in browser")
                    ToolTip.text: qsTr("Open in browser")
                    ToolTip.visible: openButton.hovered
                    ToolTip.delay: 400
                    onClicked: root.openInBrowserRequested()
                }
                IconButton {
                    id: dismissButton
                    objectName: "linkPreviewDismissButton"
                    size: "sm"
                    iconName: "close"
                    iconColorOverride: AppTheme.scrimInk
                    enabled: root.interactive
                    Accessible.name: qsTr("Dismiss link preview")
                    ToolTip.text: qsTr("Dismiss preview")
                    ToolTip.visible: dismissButton.hovered
                    ToolTip.delay: 400
                    onClicked: root.dismissRequested()
                }
            }
        }
    }

    // Keyboard: the card is one control.
    activeFocusOnTab: root.interactive && !root.playerActive
    Keys.onReturnPressed: root.activate()
    Keys.onEnterPressed: root.activate()
    Keys.onSpacePressed: root.activate()
    Accessible.role: Accessible.Button
    Accessible.name: root.tooLarge
        ? qsTr("%1 too large to show here, open %2 in browser")
              .arg(root.kindText).arg(root.preview.host || "")
        : (root.isVideo
           ? qsTr("Play video %1").arg(root.captionText)
           : qsTr("View image %1").arg(root.captionText))
          + (root.needsConsent ? ". " + root.consentText : "")
    Rectangle {
        anchors.fill: parent
        radius: AppTheme.radiusSm
        color: "transparent"
        border.width: root.activeFocus ? 2 : 0
        border.color: AppTheme.focusRing
        visible: root.activeFocus
    }

    // The inline player replaces the cover in the same box.
    Loader {
        objectName: "linkMediaPlayerLoader"
        anchors.fill: parent
        active: root.playerActive && root.isVideo && !root.tooLarge
        visible: active
        sourceComponent: VideoPlayerCard {
            mediaKey: root.mediaKey
            ownerKey: root.ownerKey
            filename: root.captionText
            rowOnScreen: root.rowOnScreen
            onCloseRequested: root.playerActive = false
        }
    }
}
