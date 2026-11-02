import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    objectName: "linkMediaCard"

    property var preview: ({})
    property real maximumWidth: 360
    property string ownerKey: ""
    property bool rowOnScreen: true
    property bool interactive: true

    signal viewRequested()
    signal openInBrowserRequested()
    signal dismissRequested()
    signal consentGiven()

    readonly property bool isVideo: (root.preview.mediaKind || "") === "video"
    readonly property string mediaKey: root.preview.mediaKey || ""
    readonly property real resumeMs: {
        var revision = app.playback.resumeRevision
        return root.isVideo && app.playback.videoResumePosition
               ? app.playback.videoResumePosition(root.mediaKey) : 0
    }
    readonly property bool watched: {
        var revision = app.playback.resumeRevision
        return root.isVideo && app.playback.videoFinished
               ? app.playback.videoFinished(root.mediaKey) : false
    }
    readonly property real mediaSize: root.preview.mediaSize || 0
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
    property string coverSource: ""

    function sizeText(bytes) {
        if (!bytes || bytes <= 0) return ""
        if (bytes < 1024) return qsTr("%1 B").arg(bytes)
        if (bytes < 1024 * 1024)
            return qsTr("%1 KB").arg(Math.round(bytes / 1024))
        return qsTr("%1 MB").arg(Number(bytes / (1024 * 1024)).toLocaleString(Qt.locale(AppLocale.name), "f", 1))
    }
    readonly property string kindText: root.isVideo ? qsTr("Video") : qsTr("Image")
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
    readonly property real aspect:
        !root.isVideo && root.preview.imageWidth > 0 && root.preview.imageHeight > 0
        ? Math.min(1.25, Math.max(0.5, root.preview.imageHeight
                                        / root.preview.imageWidth))
        : 9 / 16
    implicitWidth: root.boxWidth
    implicitHeight: Math.max(150, Math.round(root.boxWidth * root.aspect))

    function refreshCover() {
        if (root.mediaKey.length === 0) {
            root.coverSource = ""
            return
        }
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
        border.color: AppTheme.embedBorder
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
            visible: status === Image.Ready && !root.needsConsent
        }

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
                elide: Label.ElideNone
                Layout.fillWidth: true
            }
            Label {
                objectName: "linkMediaTooLarge"
                visible: root.tooLarge
                text: root.isVideo ? qsTr("Too large to play here")
                                   : qsTr("Too large to show here")
                color: AppTheme.embedText
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
                font.underline: AppTheme.underlineLinks
                font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        Rectangle {
            id: playAffordance
            objectName: "linkMediaPlayAffordance"
            visible: !root.tooLarge
            anchors.centerIn: parent
            anchors.verticalCenterOffset:
                coverImage.visible ? 0 : (root.needsConsent ? 40 : 22)
            width: 44; height: 44; radius: 22
            color: AppTheme.overlayScrim
            Icon {
                anchors.centerIn: parent
                name: !root.isVideo ? "zoom_in"
                      : root.watched ? "refresh" : "play_arrow"
                size: 26
                color: AppTheme.scrimInk
            }
        }
        Rectangle {
            objectName: "linkMediaResumeChip"
            visible: playAffordance.visible && root.resumeMs > 0
                     && !root.playerActive
            anchors.top: playAffordance.bottom
            anchors.topMargin: AppTheme.spacing6
            anchors.horizontalCenter: parent.horizontalCenter
            radius: 3
            color: AppTheme.overlayScrim
            width: Math.min(linkResumeLabel.implicitWidth + 12,
                            parent.width - 12)
            height: linkResumeLabel.implicitHeight + 4
            Label {
                id: linkResumeLabel
                anchors.centerIn: parent
                width: Math.min(implicitWidth, parent.width - 12)
                text: qsTr("Resume from %1").arg(AppTheme.clockPosition(root.resumeMs))
                color: AppTheme.scrimInk
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                font.weight: AppTheme.weightStrong
                elide: Label.ElideRight
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

    activeFocusOnTab: root.interactive && !root.playerActive
    Keys.onReturnPressed: root.activate()
    Keys.onEnterPressed: root.activate()
    Keys.onSpacePressed: root.activate()
    Accessible.role: Accessible.Button
    Accessible.onPressAction: root.activate()
    Accessible.name: root.tooLarge
        ? qsTr("%1 too large to show here, open %2 in browser")
              .arg(root.kindText).arg(root.preview.host || "")
        : (root.isVideo
           ? (root.watched
              ? qsTr("Replay video %1").arg(root.captionText)
              : root.resumeMs > 0
                ? qsTr("Resume video %1 from %2").arg(root.captionText)
                      .arg(AppTheme.clockPosition(root.resumeMs))
                : qsTr("Play video %1").arg(root.captionText))
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
