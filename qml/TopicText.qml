import QtQuick
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
    selectionColor: AppTheme.selectedHover
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

    HoverHandler {
        cursorShape: root.hoveredLink.length > 0 ? Qt.PointingHandCursor
                                                 : Qt.IBeamCursor
    }
}
