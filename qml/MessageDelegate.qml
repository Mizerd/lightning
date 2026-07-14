import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    // v0.5.7: virtual SDK timeline rows (date divider / read marker /
    // timeline start) render as thin separators instead of messages.
    // eventType: 7 = DateDivider, 8 = ReadMarker, 9 = TimelineStart.
    readonly property bool isVirtualRow: model.isVirtual === true
    readonly property bool isStateActivity: model.isStateActivity === true
    readonly property bool isRoutineActivity: model.isRoutineActivity === true
    // State events remain in the authoritative timeline model. This is only
    // a zero-height presentation filter, so toggling the setting restores the
    // same delegates without a resync or a second timeline.
    readonly property bool roomActivityVisible:
        !isRoutineActivity || app.settings.showRoomActivity
    readonly property var stateActivityEntries: model.stateGroupEntries || []
    readonly property bool showsIdentity: model.showSenderIdentity === true
    readonly property real messageTopSpacing: showsIdentity
                                               ? AppTheme.spacingS : 1
    readonly property real avatarGutterWidth: 40
    visible: roomActivityVisible
    implicitHeight: !roomActivityVisible ? 0
                    : isVirtualRow ? virtualRow.implicitHeight
                    : isStateActivity ? stateActivity.implicitHeight
                    : layout.implicitHeight + messageTopSpacing

    // Stable key for the pin-one-toolbar-at-a-time state on the ListView.
    // Prefer the SDK item id; fall back to the event id for backends that
    // don't set it. Empty for virtual rows (they have no actions).
    readonly property string actionKey: (model.itemId && model.itemId.length > 0)
                                        ? model.itemId
                                        : (model.eventId || "")
    readonly property string previewRoomId: app.currentRoomId || ""
    readonly property string previewOwnerKey:
        previewRoomId.length > 0 && actionKey.length > 0
        ? previewRoomId + "\u001f" + actionKey : ""
    readonly property bool actionsPinned: ListView.view
            && ListView.view.pinnedActionsKey !== ""
            && ListView.view.pinnedActionsKey === actionKey
    function toggleActionsPin() {
        if (!ListView.view || actionKey === "") return
        ListView.view.pinnedActionsKey =
            actionsPinned ? "" : actionKey
    }

    // v0.5.11: link-preview state for this row, resolved by
    // LinkPreviewController. Calling previewFor() may dispatch an automatic
    // request (unencrypted rooms with auto-load on); encrypted rooms stay in
    // "requires_action" until the explicit Load action.
    property var preview: ({ state: "none" })
    readonly property bool roomEncrypted:
        ListView.view ? ListView.view.roomEncrypted === true : false
    function refreshPreview() {
        if (isVirtualRow || isStateActivity || model.redacted || model.isImage || model.isFile
            || actionKey === "" || !app.linkPreviews.supported) {
            preview = ({ state: "none" })
            return
        }
        preview = app.linkPreviews.previewForEvent(previewRoomId, actionKey,
                                                   model.body || "",
                                                   roomEncrypted)
    }
    Component.onCompleted: refreshPreview()
    onActionKeyChanged: refreshPreview()
    onPreviewRoomIdChanged: {
        preview = ({ state: "none" })
        refreshPreview()
    }
    Connections {
        target: app.linkPreviews
        function onPreviewChanged(itemKey) {
            if (itemKey === root.previewOwnerKey) root.refreshPreview()
        }
        function onPolicyChanged() { root.refreshPreview() }
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

    // Compact, discreet room-activity summary (Element-style) — never a
    // message-bubble-like card. Collapsed by default; the whole row (not
    // just the chevron) is the Expand/Collapse control.
    RoomActivityDelegate {
        id: stateActivity
        objectName: "stateActivityGroup"
        visible: root.isStateActivity && model.stateGroupLeader === true
        width: parent.width
        groupId: model.stateGroupId || ""
        entries: root.stateActivityEntries
        expanded: root.ListView.view
                  ? root.ListView.view.stateGroupExpanded(groupId)
                  : false
        onToggleRequested: {
            if (root.ListView.view)
                root.ListView.view.toggleStateGroup(groupId)
        }
    }

    Rectangle {
        visible: !root.isVirtualRow && !root.isStateActivity
                 && (rowHover.hovered || root.actionsPinned)
        x: -AppTheme.spacingXS
        y: layout.y
        width: root.width + AppTheme.spacingXS * 2
        height: layout.height
        color: AppTheme.bubbleOverlaySubtle
        radius: AppTheme.radiusSm
        z: 0
    }

    ColumnLayout {
        id: layout
        visible: !root.isVirtualRow && !root.isStateActivity
        y: root.messageTopSpacing
        width: parent.width
        spacing: 2
        z: 1

        // One left-aligned sender timeline for every participant. Identity is
        // shown once at the start of a model-defined sender group; continuation
        // rows retain the same content indent without repeating the avatar.
        Item {
            id: bubbleRow
            objectName: "messagePresentationRow"
            Layout.fillWidth: true
            implicitHeight: Math.max(avatarSlot.implicitHeight,
                                     bubble.implicitHeight)

            HoverHandler { id: rowHover }

            Item {
                id: avatarSlot
                objectName: "senderAvatarSlot"
                x: 0
                width: root.avatarGutterWidth
                height: parent.height
                implicitHeight: root.showsIdentity ? 34 : bodyLabel.implicitHeight

                Avatar {
                    objectName: "senderAvatar"
                    anchors.top: parent.top
                    visible: root.showsIdentity
                    size: 32
                    mxc: model.senderAvatarMxc || ""
                    name: model.senderDisplayName || model.senderInitials
                    Accessible.name: qsTr("Avatar for %1").arg(
                                         model.senderDisplayName || model.sender)
                }

                // Continuations keep Discord's stable gutter without paying
                // for another avatar-height row. The timestamp is available
                // on hover in that gutter instead of consuming a metadata
                // line beneath every short message.
                Label {
                    objectName: "continuationTimestamp"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.rightMargin: 5
                    visible: !root.showsIdentity && rowHover.hovered
                    text: Qt.formatDateTime(model.timestamp, "hh:mm")
                    horizontalAlignment: Text.AlignRight
                    color: AppTheme.textMuted
                    font.pixelSize: 9
                    Accessible.name: qsTr("Sent at %1").arg(text)
                }
            }

            // Transparent content column: ordinary messages are rows, not
            // incoming/outgoing speech bubbles.
            Rectangle {
                id: bubble
                objectName: "messageContentColumn"
                x: root.avatarGutterWidth
                width: Math.max(1, parent.width - x)
                height: implicitHeight
                implicitHeight: bubbleContent.implicitHeight
                color: "transparent"
                radius: 0
                opacity: model.redacted ? 0.65 : 1.0

                // Click the message content to pin the action toolbar (click again
                // or press Escape to close). Does not consume media/link taps,
                // which have their own handlers on top.
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    onTapped: root.toggleActionsPin()
                }

                ColumnLayout {
                    id: bubbleContent
                    anchors.fill: parent
                    anchors.margins: 0
                    spacing: 2

                    RowLayout {
                        id: identityHeader
                        objectName: "senderIdentityHeader"
                        visible: root.showsIdentity
                        spacing: 6
                        Layout.maximumWidth: Math.max(1, bubble.width - 112)
                        Label {
                            id: nameLabel
                            objectName: "senderName"
                            text: model.senderDisplayName || model.sender
                            color: AppTheme.text
                            font.pixelSize: AppTheme.fontSizeM
                            font.weight: Font.DemiBold
                            elide: Label.ElideRight
                            Layout.maximumWidth: 320
                            Accessible.name: qsTr("Sender: %1").arg(text)
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
                            color: AppTheme.textMuted
                            font.pixelSize: 10
                            elide: Label.ElideMiddle
                            Layout.maximumWidth: 180
                        }
                        Label {
                            objectName: "senderTimestamp"
                            text: Qt.formatDateTime(model.timestamp, "hh:mm")
                            color: AppTheme.textMuted
                            font.pixelSize: 10
                            Accessible.name: qsTr("Sent at %1").arg(text)
                        }
                    }

                    // Reply preview
                    Rectangle {
                        id: replyBox
                        visible: model.replyToEventId && model.replyToEventId.length > 0
                                 && !model.redacted
                        Layout.fillWidth: true
                        implicitHeight: replyLayout.implicitHeight + 6
                        color: AppTheme.bubbleOverlaySubtle
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
                                color: AppTheme.textMuted
                                font.pixelSize: 11
                                font.italic: true
                            }
                            Label {
                                text: model.replyToPreview || qsTr("(original message not loaded)")
                                color: AppTheme.textMuted
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
                        Layout.alignment: Qt.AlignLeft
                        Layout.preferredWidth: Math.min(bubble.width,
                                                        implicitWidth)
                        Layout.maximumWidth: bubble.width
                        // v0.5.11: contribute a real implicit width so an
                        // image-only row grows to the media size instead of
                        // collapsing to the timestamp width (which made images
                        // render avatar-sized). Files stay compact.
                        implicitWidth: mediaLoader.item
                                       ? mediaLoader.item.implicitWidth : 0
                        implicitHeight: mediaLoader.item
                                        ? mediaLoader.item.implicitHeight : 0
                        Loader {
                            id: mediaLoader
                            anchors.left: parent.left
                            width: Math.min(bubble.width,
                                            item ? item.implicitWidth : 0)
                            sourceComponent: model.isImage ? imageComponent
                                            : model.isFile  ? fileComponent
                                            : null
                        }
                    }

                    // Body text (hidden for image-only messages when body == filename)
                    TextEdit {
                        id: bodyLabel
                        objectName: "messageBody"
                        visible: text.length > 0
                        text: app.linkPreviews.linkifiedBody((function() {
                            if (model.redacted) return qsTr("[message deleted]")
                            // For images, skip re-showing the filename we already show in mediaBox.
                            if (model.isImage && model.body === model.mediaFilename) return ""
                            return model.body || ""
                        })())
                        color: model.undecryptable === true
                               ? AppTheme.muted : AppTheme.text
                        font.pixelSize: AppTheme.fontSizeM
                        font.italic: model.redacted || model.undecryptable === true
                        wrapMode: Text.Wrap
                        readOnly: true
                        // ColumnLayout incubates children before bubbleRow has
                        // received its final layout width. Measuring wrapped
                        // text against the old 1px clamp can turn a large body
                        // into a transient tens-of-thousands-pixel delegate,
                        // causing ListView to discard and recreate it forever.
                        // Use a normal column width during that brief startup
                        // phase, then follow the actual responsive width.
                        Layout.maximumWidth: bubble.width > 8
                                             ? Math.min(720, bubble.width - 8)
                                             : 560
                        textFormat: Text.RichText
                        selectByMouse: true
                        Accessible.name: model.body || ""
                        onLinkActivated: function(link) { app.media.openWebUrl(link) }

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

                    // v0.5.11: rich link-preview card. Backed by
                    // LinkPreviewController — Rust performs the protected
                    // outbound fetch; QML only renders whitelisted fields.
                    // Encrypted rooms default to click-to-load (privacy).
                    Loader {
                        id: previewLoader
                        Layout.alignment: Qt.AlignLeft
                        Layout.preferredWidth: Math.min(
                            bubble.width - 8,
                            item ? item.implicitWidth : 400)
                        Layout.maximumWidth: bubble.width
                        active: root.preview.state !== undefined
                                && root.preview.state !== "none"
                                && !model.redacted
                                && !model.isImage && !model.isFile
                        visible: active
                        sourceComponent: root.preview.state === "loaded"
                                         && root.preview.isDirectMedia === true
                                         && root.preview.gifOversized !== true
                                         ? directMediaPreviewComponent
                                         : linkPreviewComponent
                    }

                    RowLayout {
                        id: metaRow
                        Layout.fillWidth: true
                        spacing: AppTheme.spacingXS
                        Label {
                            id: metaLabel
                            visible: text.length > 0
                            text: {
                                var ts = Qt.formatDateTime(model.timestamp, "hh:mm")
                                // Status: 0=Sent, 1=Sending, 2=Failed
                                if (model.isOwn && model.status === 1) return ts + " • " + qsTr("sending…")
                                if (model.isOwn && model.status === 2) return ts + " • " + qsTr("failed")
                                if (model.edited) return qsTr("edited")
                                return ""
                            }
                            color: AppTheme.textMuted
                            font.pixelSize: 10
                            Accessible.name: text
                        }
                        // v0.5.7: retry action for failed local echoes. The
                        // SDK send queue re-attempts the same queued item,
                        // so retrying never duplicates the message.
                        Label {
                            visible: model.isOwn && model.status === 2
                            text: qsTr("Retry")
                            color: AppTheme.accent
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
                            color: AppTheme.accent
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
                            color: AppTheme.textMuted
                            font.pixelSize: 10
                            font.italic: true
                        }
                    }
                }
            }

            // Action toolbar. Visible while the shared row hover is active,
            // while it is pinned open by a click, or while one of its menus
            // is open — so it never vanishes as the pointer travels from the
            // message to the buttons. Subtle AppTheme surface/border framing.
            Rectangle {
                id: actionBar
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: -3
                anchors.rightMargin: 2
                visible: rowHover.hovered || root.actionsPinned
                         || reactionPicker.opened || moreMenu.opened
                z: 3
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
            Layout.alignment: Qt.AlignLeft
            Layout.leftMargin: 36 + AppTheme.spacingXS
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

    // ---- link preview card ----

    // A validated direct raster response is media, not article metadata.
    // It receives its own compact renderer so GIFs and images never inherit
    // the generic embed card's accent edge, host footer, or fixed card width.
    Component {
        id: directMediaPreviewComponent
        Rectangle {
            id: directMedia
            objectName: "directMediaPreview"
            readonly property var p: root.preview
            readonly property real naturalWidth: p.imageWidth > 0
                                                  ? p.imageWidth : 0
            readonly property real naturalHeight: p.imageHeight > 0
                                                   ? p.imageHeight : 0
            readonly property real aspectRatio:
                naturalWidth > 0 && naturalHeight > 0
                ? naturalHeight / naturalWidth : 0.75
            readonly property real maxWidth: Math.min(360, bubble.width - 8)
            readonly property real maxHeight: 300
            readonly property real displayWidth: {
                var widthHint = naturalWidth > 0
                                ? Math.min(Math.max(160, naturalWidth), maxWidth)
                                : maxWidth
                if (widthHint * aspectRatio > maxHeight)
                    widthHint = maxHeight / aspectRatio
                return Math.max(1, Math.min(widthHint, maxWidth))
            }
            readonly property string animatedSource:
                p.isGif === true && app.settings.animateGifPreviews
                ? app.mediaBridge.previewAnimatedSource(p.imageSource || "",
                                                        p.imageMime || "") : ""
            readonly property string staticSource:
                (p.imageSource || "").length > 0
                ? app.mediaBridge.previewImageSource(p.imageSource || "",
                                                     p.imageMime || "") : ""

            implicitWidth: displayWidth
            implicitHeight: Math.max(1, displayWidth * aspectRatio)
            color: AppTheme.cardElevated
            radius: AppTheme.radiusSm
            clip: true

            Image {
                anchors.fill: parent
                visible: directMedia.animatedSource.length === 0
                source: directMedia.staticSource
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: true
            }
            AnimatedImage {
                anchors.fill: parent
                visible: directMedia.animatedSource.length > 0
                source: visible ? directMedia.animatedSource : ""
                fillMode: Image.PreserveAspectFit
                playing: visible
                asynchronous: true
                cache: false
            }
            Rectangle {
                visible: directMedia.p.isGif === true
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 5
                radius: 3
                color: AppTheme.overlayScrim
                width: directGifLabel.implicitWidth + 8
                height: directGifLabel.implicitHeight + 4
                Label {
                    id: directGifLabel
                    anchors.centerIn: parent
                    text: "GIF"
                    color: AppTheme.accentText
                    font.pixelSize: 9
                    font.weight: Font.Bold
                }
            }
            MouseArea {
                anchors.fill: parent
                enabled: (directMedia.p.url || "").length > 0
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: app.media.openWebUrl(directMedia.p.url)
            }
        }
    }

    Component {
        id: linkPreviewComponent
        Rectangle {
            id: card
            readonly property var p: root.preview
            readonly property string st: p.state || "none"
            readonly property string previewAnimation:
                p.isGif === true && app.settings.animateGifPreviews
                ? app.mediaBridge.previewAnimatedSource(p.imageSource || "",
                                                        p.imageMime || "") : ""
            readonly property string previewStatic:
                (p.imageSource || "").length > 0
                ? app.mediaBridge.previewImageSource(p.imageSource || "",
                                                     p.imageMime || "") : ""
            implicitWidth: Math.min(400, bubble.width - 8)
            implicitHeight: cardCol.implicitHeight + AppTheme.spacingS * 2
            color: AppTheme.surfaceElevated
            radius: AppTheme.radiusSm
            border.color: AppTheme.border
            border.width: 1

            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 3
                color: AppTheme.accent
                radius: AppTheme.radiusSm
            }

            // Whole card opens the URL (loaded state only).
            MouseArea {
                anchors.fill: parent
                enabled: card.st === "loaded" && (card.p.url || "").length > 0
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: app.media.openWebUrl(card.p.url)
            }

            ColumnLayout {
                id: cardCol
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: AppTheme.spacingS
                anchors.bottomMargin: AppTheme.spacingS
                anchors.leftMargin: AppTheme.spacingM
                anchors.rightMargin: AppTheme.spacingS
                spacing: 4

                // Consent / privacy gate (encrypted rooms, or auto-load off).
                ColumnLayout {
                    visible: card.st === "requires_action"
                    Layout.fillWidth: true
                    spacing: 4
                    Label {
                        text: qsTr("Link preview")
                        color: AppTheme.textMuted
                        font.pixelSize: 10
                        font.weight: Font.DemiBold
                    }
                    Label {
                        text: card.p.host || ""
                        color: AppTheme.link
                        font.pixelSize: 12
                        elide: Label.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        visible: root.roomEncrypted
                        text: qsTr("Loading this preview contacts the linked "
                                   + "website directly and may reveal your IP address.")
                        color: AppTheme.warning
                        font.pixelSize: 10
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    Button {
                        text: qsTr("Load link preview")
                        font.pixelSize: 11
                        Layout.topMargin: 2
                        onClicked: app.linkPreviews.requestPreviewForEvent(
                                       root.previewRoomId, root.actionKey)
                    }
                }

                // Loading.
                RowLayout {
                    visible: card.st === "loading"
                    spacing: AppTheme.spacingS
                    BusyIndicator {
                        width: 16; height: 16
                        running: card.st === "loading"
                    }
                    Label {
                        text: qsTr("Loading preview…")
                        color: AppTheme.textMuted
                        font.pixelSize: 11
                    }
                }

                // Failed.
                ColumnLayout {
                    visible: card.st === "failed"
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        text: qsTr("Preview unavailable")
                        color: AppTheme.textMuted
                        font.pixelSize: 11
                    }
                    Label {
                        visible: card.p.retryable === true
                        text: qsTr("Retry")
                        color: AppTheme.link
                        font.pixelSize: 11
                        font.underline: true
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: app.linkPreviews.retryForEvent(
                                           root.previewRoomId, root.actionKey)
                        }
                    }
                }

                // Loaded.
                ColumnLayout {
                    visible: card.st === "loaded"
                    Layout.fillWidth: true
                    spacing: 3

                    // Thumbnail bytes were fetched and validated by Rust.
                    Rectangle {
                        visible: ((card.p.imageMxc || "").length > 0
                                  || (card.p.imageSource || "").length > 0)
                                 && !(card.p.gifOversized === true)
                        Layout.fillWidth: true
                        Layout.preferredHeight: visible ? Math.min(180,
                            (card.p.imageHeight > 0 && card.p.imageWidth > 0)
                            ? width * (card.p.imageHeight / card.p.imageWidth)
                            : 140) : 0
                        color: AppTheme.cardElevated
                        radius: AppTheme.radiusSm
                        clip: true
                        Image {
                            id: thumb
                            anchors.fill: parent
                            visible: card.previewAnimation.length === 0
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            cache: true
                            source: card.previewStatic.length > 0
                                    ? card.previewStatic
                                    : (card.p.imageMxc || "").length > 0
                                    && app.mediaBridge.supported
                                    ? app.mediaBridge.avatarSource(card.p.imageMxc, 480)
                                    : ""
                            Connections {
                                target: app.mediaBridge
                                enabled: (card.p.imageMxc || "").length > 0
                                function onMediaCached(cacheKey) {
                                    thumb.source = app.mediaBridge.avatarSource(
                                        card.p.imageMxc, 480)
                                }
                            }
                        }
                        AnimatedImage {
                            anchors.fill: parent
                            visible: card.previewAnimation.length > 0
                            source: visible ? card.previewAnimation : ""
                            fillMode: Image.PreserveAspectFit
                            playing: visible
                            asynchronous: true
                            cache: false
                        }
                        // GIF badge.
                        Rectangle {
                            visible: card.p.isGif === true
                            anchors.left: parent.left
                            anchors.bottom: parent.bottom
                            anchors.margins: 4
                            radius: 3
                            color: AppTheme.overlayScrim
                            width: gifLabel.implicitWidth + 8
                            height: gifLabel.implicitHeight + 4
                            Label {
                                id: gifLabel
                                anchors.centerIn: parent
                                text: "GIF"
                                color: AppTheme.accentText
                                font.pixelSize: 9
                                font.weight: Font.Bold
                            }
                        }
                    }

                    Label {
                        visible: card.p.isDirectMedia !== true
                                 && (card.p.siteName || "").length > 0
                        text: card.p.siteName || ""
                        color: AppTheme.textMuted
                        font.pixelSize: 10
                        elide: Label.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        visible: card.p.isDirectMedia !== true
                                 && (card.p.title || "").length > 0
                        text: card.p.title || ""
                        color: AppTheme.text
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        wrapMode: Text.WordWrap
                        maximumLineCount: 2
                        elide: Label.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        visible: card.p.isDirectMedia !== true
                                 && (card.p.description || "").length > 0
                        text: card.p.description || ""
                        color: AppTheme.textMuted
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                        maximumLineCount: 3
                        elide: Label.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        text: card.p.host || ""
                        color: AppTheme.link
                        font.pixelSize: 10
                        elide: Label.ElideRight
                        Layout.fillWidth: true
                    }
                }
            }
        }
    }

    // ---- media sub-components ----

    Component {
        id: imageComponent
        Item {
            id: imageBox

            // v0.5.11: responsive timeline-image sizing. The display box is
            // derived from the intrinsic media dimensions and a responsive
            // bound (never avatar-sized, never overflowing the column, never
            // upscaling a tiny image beyond its natural size). This box's
            // implicitWidth/Height flow up into the row so the row grows
            // to the picture instead of collapsing to the timestamp width.
            readonly property real maxW: Math.min(360, bubble.width)
            readonly property real maxH: 320
            readonly property real natW: model.mediaWidth > 0 ? model.mediaWidth : 0
            readonly property real natH: model.mediaHeight > 0 ? model.mediaHeight : 0
            readonly property real ratio: (natW > 0 && natH > 0)
                                          ? (natH / natW) : 0.66
            // Never upscale media with known intrinsic dimensions. Unknown
            // dimensions use the responsive bound until the image metadata is
            // available from a later timeline update.
            readonly property real dispW: {
                var w = natW > 0 ? Math.min(natW, maxW) : maxW
                if (w * ratio > maxH) w = maxH / ratio
                return Math.max(1, Math.min(w, maxW))
            }
            readonly property real dispH: Math.max(1, dispW * ratio)

            implicitWidth: dispW
            implicitHeight: dispH

            readonly property bool isGif:
                (model.mediaMimetype || "").toLowerCase() === "image/gif"
            readonly property bool pendingMedia:
                (model.eventId || "").startsWith("local:")
                || (model.mediaUrl && model.mediaUrl.toString()
                    .indexOf("send-queue.localhost") >= 0)
            property string animatedSource: ""
            readonly property bool animateGif:
                isGif && app.settings.animateGifPreviews
                && !pendingMedia && animatedSource.length > 0

            // v0.5.9: prefer the media bridge (works for encrypted rooms —
            // the SDK decrypts inside Rust); HTTP-backend URLs remain the
            // fallback. An empty bridgeSource means "fetch in flight".
            readonly property bool usesBridge:
                model.mediaSourceAvailable === true && app.mediaBridge.supported
            readonly property string mediaIdentity: root.actionKey + "\u001f"
                                                    + (model.mediaKey || "")
            readonly property string bridgeCacheKey:
                (model.mediaThumbAvailable ? "thumb:" : "full:") + (model.mediaKey || "")
            property string bridgeSource: ""
            property bool bridgeFailed: false

            function refreshBridgeSource() {
                if (!usesBridge || !model.mediaKey) return
                if (isGif && app.settings.animateGifPreviews && !pendingMedia) {
                    animatedSource = app.mediaBridge.animatedSource(model.mediaKey)
                    return
                }
                if (bridgeFailed)
                    app.mediaBridge.retry(bridgeCacheKey)
                bridgeFailed = false
                bridgeSource = app.mediaBridge.mediaSource(
                    model.mediaKey,
                    model.mediaThumbAvailable ? "thumb" : "full")
            }
            Component.onCompleted: refreshBridgeSource()
            onMediaIdentityChanged: {
                animatedSource = ""
                bridgeSource = ""
                bridgeFailed = false
                refreshBridgeSource()
            }
            Connections {
                target: app.mediaBridge
                enabled: imageBox.usesBridge
                function onMediaCached(cacheKey) {
                    if (cacheKey === imageBox.bridgeCacheKey)
                        imageBox.bridgeSource = app.mediaBridge.cachedSource(cacheKey)
                }
                function onAnimatedMediaReady(cacheKey) {
                    if (cacheKey === "full:" + (model.mediaKey || ""))
                        imageBox.animatedSource = app.mediaBridge.animatedSource(model.mediaKey)
                }
                function onMediaFetchFailed(cacheKey, category) {
                    if (cacheKey === imageBox.bridgeCacheKey)
                        imageBox.bridgeFailed = true
                }
            }

            readonly property string resolvedSource:
                usesBridge ? bridgeSource
                           : (model.mediaThumbUrl
                              && model.mediaThumbUrl.toString().length > 0
                              ? model.mediaThumbUrl : model.mediaUrl)

            // Placeholder frame keeps a stable size while loading/failed so
            // the timeline does not jump when the bitmap arrives.
            Rectangle {
                anchors.fill: parent
                radius: AppTheme.radiusSm
                color: AppTheme.cardElevated
                visible: img.status !== Image.Ready
                         && animatedImg.status !== AnimatedImage.Ready
            }

            // Static path (default; also the frame for non-animated GIFs).
            Image {
                id: img
                anchors.fill: parent
                visible: !imageBox.animateGif
                fillMode: Image.PreserveAspectFit
                source: imageBox.animateGif ? "" : imageBox.resolvedSource
                sourceSize.width: 640
                asynchronous: true
                cache: true
            }

            // Animated path — only when the message is a confirmed GIF and the
            // "Animate GIF previews" setting is on. Paused while off-screen to
            // avoid burning CPU on scrolled-away rows.
            AnimatedImage {
                id: animatedImg
                anchors.fill: parent
                visible: imageBox.animateGif
                fillMode: Image.PreserveAspectFit
                source: imageBox.animateGif ? imageBox.animatedSource : ""
                asynchronous: true
                cache: true
                playing: imageBox.animateGif
                         && root.ListView.view
                         && (root.y + root.height) > root.ListView.view.contentY
                         && root.y < (root.ListView.view.contentY
                                      + root.ListView.view.height)
            }

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
                running: imageBox.animateGif
                         ? animatedImg.status === AnimatedImage.Loading
                         : (img.status === Image.Loading
                            || (imageBox.usesBridge
                                && imageBox.bridgeSource === ""
                                && !imageBox.bridgeFailed))
                visible: running
            }
            Label {
                anchors.centerIn: parent
                width: parent.width - 12
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: imageBox.bridgeFailed
                      ? qsTr("Image failed to load — click to retry")
                      : qsTr("(image unavailable)")
                color: AppTheme.textMuted
                font.pixelSize: 11
                visible: img.status === Image.Error || imageBox.bridgeFailed
            }
        }
    }

    Component {
        id: fileComponent
        Rectangle {
            width: parent.width
            implicitWidth: Math.min(320, bubble.width)
            implicitHeight: fileRow.implicitHeight + 10
            color: AppTheme.surfaceElevated
            radius: AppTheme.radiusSm
            border.color: AppTheme.border
            border.width: 1
            RowLayout {
                id: fileRow
                anchors.fill: parent
                anchors.margins: 6
                spacing: 8
                Label {
                    text: "📎"
                    font.pixelSize: 20
                    color: AppTheme.text
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Label {
                        text: model.mediaFilename || model.body || qsTr("File")
                        color: AppTheme.text
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
