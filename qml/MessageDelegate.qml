import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    implicitHeight: layout.implicitHeight + AppTheme.spacingXS

    // Hardcoded reaction palette (v0.3). Real emoji picker is v0.5+.
    readonly property var reactionPalette: ["👍", "❤️", "😂", "🎉", "😢"]

    ColumnLayout {
        id: layout
        width: parent.width
        spacing: 2

        // Message bubble line + hover actions in a horizontal row.
        Row {
            id: bubbleRow
            Layout.alignment: model.isOwn ? Qt.AlignRight : Qt.AlignLeft
            spacing: AppTheme.spacingXS

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
                    return Math.min(natural, root.width * 0.78)
                }
                implicitHeight: bubbleContent.implicitHeight + AppTheme.spacingM
                color: model.isOwn ? AppTheme.ownBubble : AppTheme.otherBubble
                radius: AppTheme.radius
                opacity: model.redacted ? 0.65 : 1.0

                MouseArea {
                    id: bubbleHover
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.NoButton
                    propagateComposedEvents: true
                }

                ColumnLayout {
                    id: bubbleContent
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacingS + 2
                    spacing: 2

                    Label {
                        id: nameLabel
                        visible: !model.isOwn
                        text: model.senderDisplayName || model.sender
                        color: AppTheme.text
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }

                    // Reply preview
                    Rectangle {
                        id: replyBox
                        visible: model.replyToEventId && model.replyToEventId.length > 0
                                 && !model.redacted
                        Layout.fillWidth: true
                        implicitHeight: replyLayout.implicitHeight + 6
                        color: Qt.rgba(0, 0, 0, model.isOwn ? 0.18 : 0.06)
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
                                color: model.isOwn ? Qt.rgba(1,1,1,0.85) : AppTheme.textMuted
                                font.pixelSize: 11
                                font.italic: true
                            }
                            Label {
                                text: model.replyToPreview || qsTr("(original message not loaded)")
                                color: model.isOwn ? Qt.rgba(1,1,1,0.85) : AppTheme.textMuted
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
                            color: model.isOwn ? Qt.rgba(1, 1, 1, 0.75) : AppTheme.textMuted
                            font.pixelSize: 10
                        }
                        // v0.4.1: thread indicator on the root event.
                        Label {
                            visible: model.isThreadRoot === true
                            text: qsTr("· %n reply(s) in thread", "",
                                       model.threadReplyCount || 0)
                            color: model.isOwn ? Qt.rgba(1, 1, 1, 0.75) : AppTheme.accent
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
                            color: model.isOwn ? Qt.rgba(1, 1, 1, 0.75) : AppTheme.textMuted
                            font.pixelSize: 10
                            font.italic: true
                        }
                    }
                }
            }

            // Hover action buttons.
            //
            // v0.4.8: replaced an inner `MouseArea { anchors.fill: parent }`
            // with a HoverHandler. Column does not allow anchors on its
            // direct children (`anchors.fill/top/bottom/verticalCenter/
            // centerIn`) and QML logged that warning on every message.
            // HoverHandler hovers over its containing Item without needing
            // explicit geometry and works fine inside a Column.
            Column {
                spacing: 2
                visible: bubbleHover.containsMouse
                         || actionsHover.hovered
                         || moreMenu.opened

                HoverHandler {
                    id: actionsHover
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                }

                Row {
                    spacing: 2
                    ToolButton {
                        text: "\u{1F60A}"
                        ToolTip.text: qsTr("React")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: reactionMenu.open()
                        Menu {
                            id: reactionMenu
                            Row {
                                padding: 4
                                spacing: 2
                                Repeater {
                                    model: root.reactionPalette
                                    ToolButton {
                                        text: modelData
                                        font.pixelSize: 16
                                        onClicked: {
                                            app.composer.reactTo(root.eventIdForActions(), modelData)
                                            reactionMenu.close()
                                        }
                                    }
                                }
                            }
                        }
                    }
                    ToolButton {
                        text: "↰"
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
                    color: modelData.byMe ? AppTheme.accent : AppTheme.surfaceAlt
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
                            color: modelData.byMe ? AppTheme.accentText : AppTheme.textMuted
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
            width: parent.width
            property real ratio: (model.mediaWidth > 0 && model.mediaHeight > 0)
                                 ? (model.mediaHeight / model.mediaWidth) : 0.6
            implicitHeight: img.status === Image.Ready
                            ? Math.min(root.width * 0.5, img.paintedHeight + 4)
                            : Math.min(320 * ratio, 240)
            Image {
                id: img
                anchors.centerIn: parent
                width: Math.min(parent.width, 320)
                height: Math.min(width * (ratio > 0 ? ratio : 0.6), 240)
                fillMode: Image.PreserveAspectFit
                source: model.mediaThumbUrl && model.mediaThumbUrl.toString().length > 0
                        ? model.mediaThumbUrl
                        : model.mediaUrl
                asynchronous: true
                cache: true

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (model.mediaUrl && model.mediaUrl.toString().length > 0)
                            app.media.openExternal(model.mediaUrl)
                    }
                }

                BusyIndicator {
                    anchors.centerIn: parent
                    running: img.status === Image.Loading
                    visible: img.status === Image.Loading
                }
                Label {
                    anchors.centerIn: parent
                    text: qsTr("(image unavailable)")
                    color: AppTheme.textMuted
                    font.pixelSize: 11
                    visible: img.status === Image.Error
                }
            }
        }
    }

    Component {
        id: fileComponent
        Rectangle {
            width: parent.width
            implicitHeight: fileRow.implicitHeight + 10
            color: Qt.rgba(0, 0, 0, model.isOwn ? 0.15 : 0.05)
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
                        color: model.isOwn ? Qt.rgba(1,1,1,0.75) : AppTheme.textMuted
                        font.pixelSize: 10
                    }
                }
                ToolButton {
                    text: qsTr("Open")
                    onClicked: {
                        if (model.mediaUrl && model.mediaUrl.toString().length > 0)
                            app.media.openExternal(model.mediaUrl)
                    }
                }
            }
        }
    }
}
