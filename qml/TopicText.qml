import QtQuick
import QtQuick.Controls.Basic
import MatrixClient

TextEdit {
    id: root

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
    font.family: AppTheme.uiFont
    font.pixelSize: AppTheme.textBody

    Accessible.role: Accessible.StaticText
    Accessible.name: root.plainText

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

    property string contextLink: ""

    TapHandler {
        acceptedButtons: Qt.LeftButton
        onPressedChanged: if (pressed) root.forceActiveFocus(Qt.MouseFocusReason)
    }

    TapHandler {
        objectName: "topicContextTap"
        acceptedButtons: Qt.RightButton
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onTapped: (point) => {
            root.contextLink = root.linkAt(point.position.x, point.position.y)
            contextMenu.popup(point.position.x, point.position.y)
        }
    }

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
