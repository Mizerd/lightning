import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    // v0.5.7: virtual SDK timeline rows (date divider / read marker /
    // timeline start) render as thin separators instead of bubbles.
    // eventType: 7 = DateDivider, 8 = ReadMarker, 9 = TimelineStart.
    readonly property bool isVirtualRow: model.isVirtual === true
    implicitHeight: isVirtualRow ? virtualRow.implicitHeight
                                 : layout.implicitHeight + AppTheme.spacingXS

    // Stable key for the pin-one-toolbar-at-a-time state on the ListView.
    // Prefer the SDK item id; fall back to the event id for backends that
    // don't set it. Empty for virtual rows (they have no actions).
    readonly property string actionKey: (model.itemId && model.itemId.length > 0)
                                        ? model.itemId
                                        : (model.eventId || "")
    readonly property bool actionsPinned: ListView.view
            && ListView.view.pinnedActionsKey !== ""
            && ListView.view.pinnedActionsKey === actionKey
    function toggleActionsPin() {
        if (!ListView.view || actionKey === "") return
        ListView.view.pinnedActionsKey =
            actionsPinned ? "" : actionKey
    }

    Item {
        id: virtualRow
        visible: root.isVirtualRow
        width: parent.width
        implicitHeight: virtualLabel.visible
                        ? virtualLabel.implicitHeight + AppTheme.spacingS
                        : 0
        Label {
            id: virtualLabel
            anchors.centerIn: parent
            visible: root.isVirtualRow && model.eventType !== 8 // hide ReadMarker
            text: model.eventType === 7
                  ? Qt.locale().toString(model.timestamp, "dddd, d MMMM yyyy")
                  : (model.eventType === 9 ? qsTr("Beginning of conversation") : "")
            color: AppTheme.textMuted
            font.pixelSize: 11
        }
    }

    ColumnLayout {
        id: layout
        visible: !root.isVirtualRow
        width: parent.width
        spacing: 2

        // Message bubble line + action toolbar in a horizontal row.
        //
        // v0.5.8: one shared HoverHandler over the whole row (bubble +
        // spacing + toolbar) keeps the toolbar visible while the pointer
        // crosses the gap from the bubble toward the buttons. The old design
        // used a MouseArea on the bubble and a separate HoverHandler on the
        // toolbar; the gap between them registered as "not hovered" on either,
        // so the toolbar vanished before it could be clicked. layoutDirection
        // places the toolbar on the INNER side of the bubble (left of your own
        // right-aligned messages, right of others') so it never runs off the
        // right edge of the viewport.
        Row {
            id: bubbleRow
            Layout.alignment: model.isOwn ? Qt.AlignRight : Qt.AlignLeft
            layoutDirection: model.isOwn ? Qt.RightToLeft : Qt.LeftToRight
            spacing: AppTheme.spacingXS

            HoverHandler { id: rowHover }

            // Bubble
            Rectangle {
                id: bubble
                width: {
                    var natural = Math.max(nameLabel.implicitWidth,
                                           replyBox.implicitWidth,
                                           bodyLabel.implicitWidth,
                                           mediaBox.implicitWidth,
                                           metaLabel.implicitWidth)
                                  + AppTheme.spacingL
                    // Cap leaves room for the action toolbar so it stays inside
                    // the viewport instead of clipping at the right edge.
                    return Math.min(natural, root.width * 0.72)
                }
                implicitHeight: bubbleContent.implicitHeight + AppTheme.spacingM
                color: model.isOwn ? AppTheme.ownBubble : AppTheme.otherBubble
                radius: AppTheme.radius
                opacity: model.redacted ? 0.65 : 1.0

                // Click the bubble to pin the action toolbar open (click again
                // or press Escape to close). Does not consume media/link taps,
                // which have their own handlers on top.
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    onTapped: root.toggleActionsPin()
                }

                ColumnLayout {
                    id: bubbleContent
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacingS + 2
                    spacing: 2

                    RowLayout {
                        visible: !model.isOwn && !model.sameSenderAsPrevious
                        spacing: AppTheme.spacingXS
                        Label {
                            id: nameLabel
                            text: model.senderDisplayName || model.sender
                            color: AppTheme.text
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            // Full MXID on hover; always available even when
                            // the display name is shown.
                            ToolTip.text: model.sender
                            ToolTip.visible: nameHover.hovered
                            ToolTip.delay: 400
                            HoverHandler { id: nameHover }
                        }
                        // v0.5.9: compact disambiguator when the SDK reports
                        // two active members share this display name.
                        Label {
                            visible: model.senderNameAmbiguous === true
                                     && (model.senderDisplayName || "").length > 0
                            text: model.sender
                            color: model.isOwn ? AppTheme.ownBubbleText
                                               : AppTheme.textMuted
                            font.pixelSize: 10
                            elide: Label.ElideMiddle
                            Layout.maximumWidth: 180
                        }
                    }

                    // Reply preview
                    Rectangle {
                        id: replyBox
                        visible: model.replyToEventId && model.replyToEventId.length > 0
                                 && !model.redacted
                        Layout.fillWidth: true
                        implicitHeight: replyLayout.implicitHeight + 6
                        color: model.isOwn ? AppTheme.bubbleOverlay : AppTheme.bubbleOverlaySubtle
                        radius: 4
                        border.color: AppTheme.accent
                        border.width: 0
                        ColumnLayout {
                            id: replyLayout
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.margins: 4
                            spacing: 0
                            Label {
                                text: model.replyToSender
                                      ? qsTr("↰ %1").arg(model.replyToSender)
                                      : qsTr("↰ Reply")
                                color: model.isOwn ? AppTheme.onAccentMuted : AppTheme.textMuted
                                font.pixelSize: 11
                                font.italic: true
                            }
                            Label {
                                text: model.replyToPreview || qsTr("(original message not loaded)")
                                color: model.isOwn ? AppTheme.onAccentMuted : AppTheme.textMuted
                                font.pixelSize: 11
                                elide: Label.ElideRight
                                Layout.fillWidth: true
                                maximumLineCount: 1
                            }
                        }
                    }

                    // Media block (image or file)
                    Item {
                        id: mediaBox
                        visible: model.isImage || model.isFile
                        Layout.fillWidth: true
                        implicitHeight: mediaLoader.item ? mediaLoader.item.implicitHeight : 0
                        Loader {
                            id: mediaLoader
                            width: parent.width
                            sourceComponent: model.isImage ? imageComponent
                                            : model.isFile  ? fileComponent
                                            : null
                        }
                    }

                    // Body text (hidden for image-only messages when body == filename)
                    Label {
                        id: bodyLabel
                        visible: text.length > 0
                        text: {
                            if (model.redacted) return qsTr("[message deleted]")
                            // For images, skip re-showing the filename we already show in mediaBox.
                            if (model.isImage && model.body === model.mediaFilename) return ""
                            return model.body || ""
                        }
                        color: (model.undecryptable === true)
                               ? AppTheme.muted
                               : (model.isOwn ? AppTheme.ownBubbleText
                                              : AppTheme.otherBubbleText)
                        font.pixelSize: AppTheme.fontSizeM
                        font.italic: model.redacted || model.undecryptable === true
                        wrapMode: Text.Wrap
                        Layout.maximumWidth: root.width * 0.72
                        textFormat: Text.PlainText

                        // v0.5.0-prep+12: hover tooltip for undecryptable rows
                        // so the user knows why the body is a placeholder.
                        // Text is deliberately reassuring, not alarming.
                        HoverHandler {
                            id: undecryptHover
                            enabled: model.undecryptable === true
                        }
                        ToolTip {
                            visible: undecryptHover.hovered
                            delay: 400
                            text: qsTr(
                                "Missing room key. Restore your recovery key " +
                                "in Settings, or wait for another verified " +
                                "device to share the key.")
                        }
                    }

                    RowLayout {
                        id: metaRow
                        Layout.fillWidth: true
                        spacing: AppTheme.spacingXS
                        Label {
                            id: metaLabel
                            text: {
                                var ts = Qt.formatDateTime(model.timestamp, "hh:mm")
                                // Status: 0=Sent, 1=Sending, 2=Failed
                                if (model.isOwn && model.status === 1) return ts + " • " + qsTr("sending…")
                                if (model.isOwn && model.status === 2) return ts + " • " + qsTr("failed")
                                if (model.edited) return ts + " • " + qsTr("edited")
                                return ts
                            }
                            color: model.isOwn ? AppTheme.onAccentMuted : AppTheme.textMuted
                            font.pixelSize: 10
                        }
                        // v0.5.7: retry action for failed local echoes. The
                        // SDK send queue re-attempts the same queued item,
                        // so retrying never duplicates the message.
                        Label {
                            visible: model.isOwn && model.status === 2
                            text: qsTr("Retry")
                            color: model.isOwn ? AppTheme.ownBubbleText : AppTheme.accent
                            font.pixelSize: 10
                            font.underline: true
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: app.timeline.retrySend(index)
                            }
                        }
                        // v0.4.1: thread indicator on the root event.
                        Label {
                            visible: model.isThreadRoot === true
                            text: qsTr("· %n reply(s) in thread", "",
                                       model.threadReplyCount || 0)
                            color: model.isOwn ? AppTheme.onAccentMuted : AppTheme.accent
                            font.pixelSize: 10
                            font.underline: true
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    var previewText = model.body || ""
                                    app.composer.beginThreadReply(
                                        root.eventIdForActions(),
                                        previewText.substring(0, 80))
                                }
                            }
                        }
                        // v0.4.1: reply-in-thread mark on non-root thread events.
                        Label {
                            visible: (model.threadRootId || "").length > 0
                                     && !(model.isThreadRoot === true)
                            text: qsTr("· in thread")
                            color: model.isOwn ? AppTheme.onAccentMuted : AppTheme.textMuted
                            font.pixelSize: 10
                            font.italic: true
                        }
                    }
                }
            }

            // Action toolbar. Visible while the shared row hover is active,
            // while it is pinned open by a click, or while one of its menus
            // is open — so it never vanishes as the pointer travels from the
            // bubble to the buttons. Subtle AppTheme surface/border framing.
            Rectangle {
                id: actionBar
                visible: rowHover.hovered || root.actionsPinned
                         || reactionPicker.opened || moreMenu.opened
                radius: AppTheme.radiusSm
                color: AppTheme.surface
                border.color: AppTheme.border
                border.width: 1
                implicitWidth: actionRow.implicitWidth + 8
                implicitHeight: actionRow.implicitHeight + 6

                Row {
                    id: actionRow
                    anchors.centerIn: parent
                    spacing: 2
                    ToolButton {
                        id: reactButton
                        text: "\u{1F60A}"
                        Accessible.name: qsTr("React to message")
                        ToolTip.text: qsTr("React")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: {
                            if (ListView.view)
                                ListView.view.pinnedActionsKey = root.actionKey
                            var p = reactButton.mapToItem(Overlay.overlay,
                                                          reactButton.width / 2,
                                                          reactButton.height)
                            reactionPicker.anchorPoint = p
                            reactionPicker.open()
                        }
                    }
                    ToolButton {
                        text: "↰"
                        Accessible.name: qsTr("Reply to message")
                        ToolTip.text: qsTr("Reply")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: {
                            var previewText = model.body
                            if (model.isImage) previewText = qsTr("Image: %1").arg(model.mediaFilename || "")
                            else if (model.isFile) previewText = qsTr("File: %1").arg(model.mediaFilename || "")
                            app.composer.beginReply(root.eventIdForActions(),
                                                    model.senderDisplayName || model.sender,
                                                    (previewText || "").substring(0, 80))
                        }
                    }
                    ToolButton {
                        text: "…"
                        Accessible.name: qsTr("More message actions")
                        ToolTip.text: qsTr("More")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: moreMenu.open()
                        Menu {
                            id: moreMenu
                            MenuItem {
                                text: qsTr("Copy text")
                                enabled: !model.redacted && (model.body || "").length > 0
                                onTriggered: {
                                    // Simple: no clipboard access in QML by default; skip.
                                }
                                visible: false
                            }
                            MenuItem {
                                text: qsTr("Reply in thread")
                                enabled: !model.redacted
                                onTriggered: {
                                    var previewText = model.body
                                    if (model.isImage) previewText = qsTr("Image: %1").arg(model.mediaFilename || "")
                                    else if (model.isFile) previewText = qsTr("File: %1").arg(model.mediaFilename || "")
                                    // Threads are rooted at the root event id.
                                    // If this event is itself a thread reply, use its
                                    // root; otherwise use this event id as the root.
                                    var rootId = (model.threadRootId || "").length > 0
                                                     ? model.threadRootId
                                                     : root.eventIdForActions()
                                    app.composer.beginThreadReply(rootId,
                                                                  (previewText || "").substring(0, 80))
                                }
                            }
                            MenuItem {
                                text: qsTr("Edit")
                                enabled: model.isOwn && !model.redacted
                                         && model.eventType === 0    // TextMessage
                                onTriggered: app.composer.beginEdit(root.eventIdForActions(), model.body)
                            }
                            MenuItem {
                                text: qsTr("Delete")
                                enabled: model.isOwn && !model.redacted
                                onTriggered: app.composer.redact(root.eventIdForActions())
                            }
                            MenuItem {
                                visible: (model.isImage || model.isFile) && model.mediaUrl && model.mediaUrl.toString().length > 0
                                text: qsTr("Open externally")
                                onTriggered: app.media.openExternal(model.mediaUrl)
                            }
                        }
                    }
                }
            }
        }

        // Reactions row
        Flow {
            visible: !model.redacted && model.reactions && model.reactions.length > 0
            Layout.alignment: model.isOwn ? Qt.AlignRight : Qt.AlignLeft
            spacing: 4
            Repeater {
                model: root.reactionsList()
                Rectangle {
                    color: modelData.byMe ? AppTheme.reactionSelectedBackground : AppTheme.reactionBackground
                    radius: 10
                    border.color: AppTheme.border
                    border.width: 1
                    implicitWidth: reactionRow.implicitWidth + 8
                    implicitHeight: reactionRow.implicitHeight + 4
                    Row {
                        id: reactionRow
                        anchors.centerIn: parent
                        spacing: 3
                        Label {
                            text: modelData.key
                            font.pixelSize: 12
                        }
                        Label {
                            text: modelData.count
                            color: modelData.byMe ? AppTheme.selectedText : AppTheme.textMuted
                            font.pixelSize: 11
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: app.composer.reactTo(root.eventIdForActions(), modelData.key)
                    }
                }
            }
        }
    }

    EmojiPicker {
        id: reactionPicker
        mode: "reaction"
        onOpened: if (ListView.view) ListView.view.emojiPickerOpen = true
        onClosed: if (ListView.view) ListView.view.emojiPickerOpen = false
        onEmojiChosen: (emoji) =>
            app.composer.reactTo(root.eventIdForActions(), emoji)
    }

    // JS helper: models expose reactions as QVariantList. Return as-is for Repeater.
    function reactionsList() { return model.reactions || [] }

    // Local echoes carry "local:*" ids. QML actions should still work because
    // the backend keys pendingSends by txnId, but redact of a local echo will
    // fail server-side. We still allow it — the failure surfaces via the
    // status bar error signal.
    function eventIdForActions() { return model.eventId }

    // ---- media sub-components ----

    Component {
        id: imageComponent
        Item {
            id: imageBox
            width: parent.width
            property real ratio: (model.mediaWidth > 0 && model.mediaHeight > 0)
                                 ? (model.mediaHeight / model.mediaWidth) : 0.6

            // v0.5.9: prefer the media bridge (works for encrypted rooms —
            // the SDK decrypts inside Rust); HTTP-backend URLs remain the
            // fallback. An empty bridgeSource means "fetch in flight".
            readonly property bool usesBridge:
                model.mediaSourceAvailable === true && app.mediaBridge.supported
            readonly property string bridgeCacheKey:
                (model.mediaThumbAvailable ? "thumb:" : "full:") + (model.mediaKey || "")
            property string bridgeSource: ""
            property bool bridgeFailed: false

            function refreshBridgeSource() {
                if (!usesBridge || !model.mediaKey) return
                bridgeFailed = false
                bridgeSource = app.mediaBridge.mediaSource(
                    model.mediaKey,
                    model.mediaThumbAvailable ? "thumb" : "full")
            }
            Component.onCompleted: refreshBridgeSource()
            Connections {
                target: app.mediaBridge
                enabled: imageBox.usesBridge
                function onMediaCached(cacheKey) {
                    if (cacheKey === imageBox.bridgeCacheKey)
                        imageBox.bridgeSource = app.mediaBridge.cachedSource(cacheKey)
                }
                function onMediaFetchFailed(cacheKey, category) {
                    if (cacheKey === imageBox.bridgeCacheKey)
                        imageBox.bridgeFailed = true
                }
            }

            implicitHeight: img.status === Image.Ready
                            ? Math.min(root.width * 0.5, img.paintedHeight + 4)
                            : Math.min(320 * ratio, 240)
            Image {
                id: img
                anchors.centerIn: parent
                width: Math.min(parent.width, 320)
                height: Math.min(width * (imageBox.ratio > 0 ? imageBox.ratio : 0.6), 240)
                fillMode: Image.PreserveAspectFit
                source: imageBox.usesBridge
                        ? imageBox.bridgeSource
                        : (model.mediaThumbUrl && model.mediaThumbUrl.toString().length > 0
                           ? model.mediaThumbUrl
                           : model.mediaUrl)
                sourceSize.width: 640
                asynchronous: true
                cache: true

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (imageBox.bridgeFailed) {
                            imageBox.refreshBridgeSource()
                            return
                        }
                        if (root.ListView.view && root.ListView.view.openImage)
                            root.ListView.view.openImage(model.mediaKey || "",
                                                         model.mediaUrl)
                        else if (model.mediaUrl && model.mediaUrl.toString().length > 0)
                            app.media.openExternal(model.mediaUrl)
                    }
                }

                BusyIndicator {
                    anchors.centerIn: parent
                    running: img.status === Image.Loading
                             || (imageBox.usesBridge
                                 && imageBox.bridgeSource === ""
                                 && !imageBox.bridgeFailed)
                    visible: running
                }
                Label {
                    anchors.centerIn: parent
                    text: imageBox.bridgeFailed
                          ? qsTr("Image failed to load — click to retry")
                          : qsTr("(image unavailable)")
                    color: AppTheme.textMuted
                    font.pixelSize: 11
                    visible: img.status === Image.Error || imageBox.bridgeFailed
                }
            }
        }
    }

    Component {
        id: fileComponent
        Rectangle {
            width: parent.width
            implicitHeight: fileRow.implicitHeight + 10
            color: model.isOwn ? AppTheme.bubbleOverlay : AppTheme.bubbleOverlaySubtle
            radius: 4
            RowLayout {
                id: fileRow
                anchors.fill: parent
                anchors.margins: 6
                spacing: 8
                Label {
                    text: "📎"
                    font.pixelSize: 20
                    color: model.isOwn ? AppTheme.ownBubbleText : AppTheme.text
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Label {
                        text: model.mediaFilename || model.body || qsTr("File")
                        color: model.isOwn ? AppTheme.ownBubbleText : AppTheme.text
                        font.pixelSize: 12
                        font.weight: Font.Medium
                        elide: Label.ElideMiddle
                        Layout.fillWidth: true
                    }
                    Label {
                        text: {
                            var kb = model.mediaSize / 1024
                            var size = kb < 1024 ? kb.toFixed(1) + " KB"
                                                 : (kb / 1024).toFixed(1) + " MB"
                            return model.mediaMimetype
                                ? size + " • " + model.mediaMimetype
                                : size
                        }
                        color: model.isOwn ? AppTheme.onAccentMuted : AppTheme.textMuted
                        font.pixelSize: 10
                    }
                }
                // v0.5.9: explicit Save As through the media bridge (SDK
                // decrypts encrypted attachments; the file is written only
                // to the user-chosen destination and never opened).
                ToolButton {
                    visible: model.mediaSourceAvailable === true
                             && app.mediaBridge.supported
                    text: qsTr("Save")
                    Accessible.name: qsTr("Save %1 as…").arg(model.mediaFilename || qsTr("file"))
                    onClicked: {
                        if (root.ListView.view && root.ListView.view.saveMedia)
                            root.ListView.view.saveMedia(model.mediaKey || "",
                                                         model.mediaFilename || "download")
                    }
                }
                // HTTP backend keeps its external-open path (plain media).
                ToolButton {
                    visible: !(model.mediaSourceAvailable === true)
                             && model.mediaUrl
                             && model.mediaUrl.toString().length > 0
                    text: qsTr("Open")
                    onClicked: app.media.openExternal(model.mediaUrl)
                }
            }
        }
    }
}
