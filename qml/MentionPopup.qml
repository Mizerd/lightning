import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

// Composer @-mention autocomplete: a flat surface above the input while an
// @-token is typed. It never takes focus: the TextArea keeps the caret and
// forwards Up/Down/Tab/Return/Escape. Presents only current-room members from
// the shared MentionSuggestionModel; no directory queries, no protocol logic,
// no message text logged. Header, matched-prefix tint, ADMIN/MOD role chip when
// the snapshot has a recognised role, a Return keycap on the selected row, and
// row accessibility. Presence dots are omitted: the model has no data for them.
// Storm skin.
Popup {
    id: root
    objectName: "mentionPopup"

    // The MentionSuggestionModel; the composer drives its query and room.
    property var suggestions: null
    // The composer's live query, kept here because the room and thread hosts
    // share one model instance and each owns its popup's header.
    property string query: ""
    // Top-left of the composer input in overlay coordinates; the popup sits
    // above it.
    property point anchorInputTop: Qt.point(0, 0)
    property real anchorWidth: 320
    property int currentIndex: 0

    signal chosen(string userId, string displayName)

    // A member without a display name reads as the localpart; the MXID is
    // already on the second line.
    function localpartOf(userId) {
        var s = String(userId)
        if (s.charAt(0) === "@")
            s = s.substring(1)
        var colon = s.indexOf(":")
        return colon > 0 ? s.substring(0, colon) : s
    }
    function nameFor(m) {
        return (m && m.displayName && m.displayName.length > 0)
               ? m.displayName : root.localpartOf(m ? m.userId : "")
    }

    parent: Overlay.overlay
    focus: false
    // The composer drives open/close; auto-close would fight the editor's
    // focus.
    closePolicy: Popup.NoAutoClose
    padding: AppTheme.menuPadding

    readonly property int count: suggestions ? suggestions.count : 0
    readonly property int rowH: AppTheme.scaled(42)
    readonly property int headerH: AppTheme.scaled(24)
    readonly property int visibleRows: Math.max(1, Math.min(count, 6))

    width: Math.max(240, Math.min(anchorWidth, 380))
    height: headerH + visibleRows * rowH + padding * 2
    // Clamped inside the overlay (as placeInsideWindow() in the pickers): the
    // 240px floor can exceed a narrow thread composer.
    x: parent ? Math.max(AppTheme.spacing4,
                         Math.min(anchorInputTop.x,
                                  parent.width - width - AppTheme.spacing4))
              : anchorInputTop.x
    y: Math.max(AppTheme.spacing4,
                anchorInputTop.y - height - AppTheme.spacing4)

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
        root.chosen(m.userId, root.nameFor(m))
    }

    // administrator/creator -> ADMIN, moderator -> MOD, anything else -> no
    // chip. `role` is MentionSuggestionModel::RoleRole ("default" on the mock).
    function roleChipLabel(role) {
        if (role === "administrator" || role === "creator")
            return qsTr("ADMIN")
        if (role === "moderator")
            return qsTr("MOD")
        return ""
    }

    // Display names are untrusted; escape before the rich-text highlight.
    function escapeHtml(s) {
        return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;")
                        .replace(/>/g, "&gt;")
    }

    // Highlights the query as a display-name prefix only; other matches show
    // the plain name. `ink`: bolt on the selected row only, brightened
    // stormText on others.
    function highlightedName(name, q, ink) {
        // A row can arrive without a display name; String methods on undefined
        // would kill the binding.
        const safeName = name === undefined || name === null ? "" : String(name)
        const escaped = escapeHtml(safeName)
        if (!q || q.length === 0
                || safeName.toLowerCase().indexOf(String(q).toLowerCase()) !== 0)
            return escaped
        const prefix = escapeHtml(safeName.substring(0, q.length))
        const rest = escapeHtml(safeName.substring(q.length))
        return "<font color=\"%1\">%2</font>%3".arg(ink)
                                                .arg(prefix).arg(rest)
    }

    background: Item {
        // Elevation, as a sibling behind the surface (z: -1) so the popup's
        // geometry is unchanged (a shadow on the background would inflate the
        // implicit size the anchor maths use). Same construction as the
        // composer card.
        MultiEffect {
            source: mentionSurface
            anchors.fill: mentionSurface
            z: -1
            shadowEnabled: true
            shadowColor: AppTheme.shadowSoft
            shadowBlur: 0.9
            shadowVerticalOffset: AppTheme.elevationPopoverY
            shadowHorizontalOffset: 0
        }
        Rectangle {
            id: mentionSurface
            objectName: "mentionPopupSurface"
            anchors.fill: parent
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            border.width: 1
            radius: AppTheme.menuRadius
        }
    }

    contentItem: Column {
        spacing: 0

        Label {
            objectName: "mentionPopupHeader"
            width: parent.width
            height: root.headerH
            verticalAlignment: Text.AlignVCenter
            textFormat: Text.PlainText
            elide: Label.ElideRight
            text: qsTr("Mention · Matching \"%1\"").arg(root.query)
            font.family: AppTheme.monoFont
            font.pixelSize: AppTheme.fontChip
            font.weight: Font.DemiBold
            font.letterSpacing: AppTheme.trackingStorm
            font.capitalization: Font.AllUppercase
            // Faint mono section-header ink.
            color: AppTheme.stormTextFaint
        }

        ListView {
            id: listView
            objectName: "mentionListView"
            width: parent.width
            height: root.visibleRows * root.rowH
            clip: true
            model: root.suggestions
            currentIndex: root.currentIndex
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentHeight > height
            Accessible.role: Accessible.List

            delegate: Rectangle {
                id: rowDelegate
                objectName: "mentionRow_" + index
                width: ListView.view ? ListView.view.width : 0
                height: root.rowH
                readonly property bool isSelected: index === root.currentIndex
                readonly property string roleChip: root.roleChipLabel(model.role)
                radius: AppTheme.menuItemRadius
                color: isSelected ? AppTheme.stormSelection : "transparent"
                border.width: isSelected ? 1 : 0
                border.color: AppTheme.stormBorderStrong

                Accessible.role: Accessible.Button
                Accessible.name: model.displayName + ", " + model.userId
                                 + (roleChip.length > 0 ? ", " + roleChip : "")
                Accessible.selected: isSelected

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

                RowLayout {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: AppTheme.spacing8
                    anchors.rightMargin: AppTheme.spacing8
                    spacing: AppTheme.spacing8

                    Avatar {
                        Layout.alignment: Qt.AlignVCenter
                        size: AppTheme.scaled(28)
                        circle: true
                        name: root.nameFor(model)
                        mxc: model.avatarMxc
                        colorKey: model.userId
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        spacing: 0
                        Label {
                            Layout.fillWidth: true
                            textFormat: Text.RichText
                            text: root.highlightedName(
                                root.nameFor(model), root.query,
                                rowDelegate.isSelected ? AppTheme.bolt
                                                       : AppTheme.stormText)
                            color: rowDelegate.isSelected
                                   ? AppTheme.stormText
                                   : AppTheme.stormTextSecondary
                            font.family: AppTheme.menuFont
                            font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                            font.weight: rowDelegate.isSelected
                                         ? AppTheme.weightBold
                                         : AppTheme.weightStrong
                            elide: Label.ElideRight
                        }
                        // The MXID under the name.
                        Label {
                            // Untrusted text: never markup.
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            visible: (model.ambiguous === true)
                                     || (model.displayName
                                         && model.displayName.length > 0)
                            // The whole-room row says what it does: it notifies
                            // everyone.
                            text: model.isRoom === true
                                  ? qsTr("Notify everyone in this room")
                                  : model.userId
                            font.family: model.isRoom === true
                                         ? AppTheme.uiFont : AppTheme.monoFont
                            font.pixelSize: AppTheme.scaled(AppTheme.fontMonoXS)
                            // AA on the selection fill.
                            color: rowDelegate.isSelected
                                   ? AppTheme.stormTextSecondary
                                   : AppTheme.stormTextMuted
                            elide: Label.ElideRight
                        }
                    }
                    StatusChip {
                        objectName: "mentionRoleChip"
                        Layout.alignment: Qt.AlignVCenter
                        visible: rowDelegate.roleChip.length > 0
                        label: rowDelegate.roleChip
                        storm: true
                        tone: "bolt"
                    }
                    MenuKeycap {
                        objectName: "mentionSelectedKeycap"
                        Layout.alignment: Qt.AlignVCenter
                        visible: rowDelegate.isSelected
                        iconName: "keyboard_return"
                        active: true
                    }
                }
            }
            ScrollBar.vertical: AppScrollBar { thin: true; policy: ScrollBar.AsNeeded }
        }
    }
}
