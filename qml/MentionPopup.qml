import QtQuick
import QtQuick.Controls.Basic
import MatrixClient

// v0.7 outgoing @-mentions: the composer autocomplete popup. A flat Lightning
// surface (like AppMenu) that floats ABOVE the composer input while the user
// types an @-token. It deliberately does NOT take focus — the TextArea keeps
// focus and forwards Up/Down/Tab/Return/Escape to the functions below — so the
// caret never leaves the editor. It presents ONLY current-room members from
// the shared MentionSuggestionModel; it never queries the server directory and
// owns no Matrix protocol logic. No message text is ever logged.
Popup {
    id: root
    objectName: "mentionPopup"

    // The MentionSuggestionModel (set by the composer). Its `query`/`roomId`
    // are driven by the composer; this popup only presents + selects.
    property var suggestions: null
    // Top-left of the composer input in overlay coordinates; the popup is
    // placed above it.
    property point anchorInputTop: Qt.point(0, 0)
    property real anchorWidth: 320
    property int currentIndex: 0

    signal chosen(string userId, string displayName)

    parent: Overlay.overlay
    focus: false
    // The composer drives open/close; auto-close (focus/press-outside) would
    // fight the editor keeping focus.
    closePolicy: Popup.NoAutoClose
    padding: AppTheme.spacing4

    readonly property int count: suggestions ? suggestions.count : 0
    readonly property int rowH: 42
    readonly property int visibleRows: Math.max(1, Math.min(count, 6))

    width: Math.max(240, Math.min(anchorWidth, 380))
    height: visibleRows * rowH + padding * 2
    x: anchorInputTop.x
    y: anchorInputTop.y - height - AppTheme.spacing4

    onCountChanged: {
        if (currentIndex >= count)
            currentIndex = Math.max(0, count - 1)
        if (currentIndex < 0)
            currentIndex = 0
        if (visible && count === 0)
            close()
    }
    onOpened: currentIndex = 0

    function moveDown() {
        if (count > 0)
            currentIndex = (currentIndex + 1) % count
    }
    function moveUp() {
        if (count > 0)
            currentIndex = (currentIndex - 1 + count) % count
    }
    function accept() {
        if (!suggestions || count === 0)
            return
        var m = suggestions.get(currentIndex)
        if (!m || !m.userId)
            return
        var name = (m.displayName && m.displayName.length > 0)
                   ? m.displayName : m.userId
        root.chosen(m.userId, name)
    }

    background: Rectangle {
        objectName: "mentionPopupSurface"
        color: AppTheme.surface
        border.color: AppTheme.borderStrong
        border.width: 1
        radius: AppTheme.radiusMd
    }

    contentItem: ListView {
        id: listView
        objectName: "mentionListView"
        clip: true
        implicitHeight: contentHeight
        model: root.suggestions
        currentIndex: root.currentIndex
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height

        delegate: Rectangle {
            objectName: "mentionRow_" + index
            width: ListView.view ? ListView.view.width : 0
            height: root.rowH
            radius: AppTheme.radiusSm
            color: index === root.currentIndex ? AppTheme.hover : "transparent"

            MouseArea {
                objectName: "mentionRowMouse_" + index
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onEntered: root.currentIndex = index
                onClicked: {
                    root.currentIndex = index
                    root.accept()
                }
            }

            Row {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: AppTheme.spacing8
                anchors.rightMargin: AppTheme.spacing8
                spacing: AppTheme.spacing8

                Avatar {
                    anchors.verticalCenter: parent.verticalCenter
                    size: 28
                    circle: true
                    name: (model.displayName && model.displayName.length > 0)
                          ? model.displayName : model.userId
                    mxc: model.avatarMxc
                    colorKey: model.userId
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 0
                    Label {
                        text: (model.displayName && model.displayName.length > 0)
                              ? model.displayName : model.userId
                        color: AppTheme.textPrimary
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        elide: Label.ElideRight
                        width: Math.max(0, root.width - 28
                                        - AppTheme.spacing8 * 3
                                        - root.padding * 2)
                    }
                    // Muted MXID under the name (always shown when there is a
                    // display name, and required when the name is ambiguous).
                    Label {
                        visible: (model.ambiguous === true)
                                 || (model.displayName
                                     && model.displayName.length > 0)
                        text: model.userId
                        color: AppTheme.textMuted
                        font.pixelSize: 11
                        elide: Label.ElideRight
                        width: Math.max(0, root.width - 28
                                        - AppTheme.spacing8 * 3
                                        - root.padding * 2)
                    }
                }
            }
        }
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
    }
}
