import QtQuick
import QtQuick.Controls.Basic
import MatrixClient

// A room or Space topic: selectable, copyable, with its web links clickable.
//
// Built the way a message body is: a read-only TextEdit, fed escaped rich text
// from the linkifier plain message bodies use (LinkPreviewController::
// linkifiedTopic -> link_preview::linkifiedTopicHtml). The topic is untrusted
// server text, so nothing in it can become markup: everything is escaped
// except the anchors the linkifier builds, and each of those is a validated
// http(s) URL. There is no HTML-topic path; `plainText` is the plain topic.
//
// Mouse: a click on a link opens it. A drag selects and never opens, even when
// it starts on a link: Qt's text control activates a link only on a release
// with no selection. Keyboard: after a click, Ctrl+A and Ctrl+C.
// Wheel events are not taken, so the panel around it still scrolls.
//
// No lineHeight: it is a Text property, and assigning it on a TextEdit is a
// load-time error.
TextEdit {
    id: root

    // The topic as the server sent it.
    property string plainText: ""

    readOnly: true
    selectByMouse: true
    selectByKeyboard: true
    textFormat: Text.RichText
    wrapMode: TextEdit.Wrap
    text: app.linkPreviews ? app.linkPreviews.linkifiedTopic(root.plainText)
                           : ""
    color: AppTheme.textSecondary
    selectionColor: AppTheme.textSelection
    selectedTextColor: AppTheme.selectedText
    // TextEdit does not inherit the Controls font.
    font.family: AppTheme.uiFont
    font.pixelSize: AppTheme.textBody

    Accessible.role: Accessible.StaticText
    Accessible.name: root.plainText

    // The same routing as MessageDelegate.openMessageLink: a room-oriented
    // Matrix permalink opens in the app, anything else in the browser through
    // MediaManager::openWebUrl, which refuses every scheme but http(s).
    function openLink(link) {
        var lower = link.toLowerCase()
        var isMatrixLink = link.indexOf("matrix.to/#/") !== -1
        var isUserLink = link.indexOf("#/@") !== -1
                         || lower.indexOf("#/%40") !== -1
        if (isMatrixLink && !isUserLink) {
            app.openMatrixLink(link)
            return
        }
        app.media.openWebUrl(link)
    }

    onLinkActivated: (link) => root.openLink(link)

    // The link under the last right click ("" off a link).
    property string contextLink: ""

    // A left press focuses the topic. A TapHandler's default policy takes only
    // a passive grab, so the TextEdit still sees the press and its selection
    // and link handling are untouched.
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onPressedChanged: if (pressed) root.forceActiveFocus(Qt.MouseFocusReason)
    }

    TapHandler {
        objectName: "topicContextTap"
        acceptedButtons: Qt.RightButton
        // A touch tap is not a right-click.
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onTapped: (point) => {
            root.contextLink = root.linkAt(point.position.x, point.position.y)
            contextMenu.popup(point.position.x, point.position.y)
        }
    }

    // Same hidden-TextEdit clipboard route as MessageDelegate.copyToClipboard.
    TextEdit {
        id: clipboardHelper
        visible: false
    }
    function copyToClipboard(value) {
        if (!value || value.length === 0) return
        clipboardHelper.text = value
        clipboardHelper.selectAll()
        clipboardHelper.copy()
        clipboardHelper.text = ""
    }

    AppMenu {
        id: contextMenu
        objectName: "topicContextMenu"
        AppMenuItem {
            objectName: "topicCopyItem"
            iconName: "content_copy"
            text: qsTr("Copy")
            enabled: root.selectedText.length > 0
            onTriggered: root.copy()
        }
        AppMenuItem {
            objectName: "topicCopyLinkItem"
            iconName: "link"
            text: qsTr("Copy link")
            enabled: root.contextLink.length > 0
            onTriggered: root.copyToClipboard(root.contextLink)
        }
        AppMenuItem {
            objectName: "topicSelectAllItem"
            text: qsTr("Select all")
            onTriggered: {
                root.forceActiveFocus(Qt.MouseFocusReason)
                root.selectAll()
            }
        }
    }

    HoverHandler {
        cursorShape: root.hoveredLink.length > 0 ? Qt.PointingHandCursor
                                                 : Qt.IBeamCursor
    }
}
