import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

// v0.7 outgoing @-mentions: the composer autocomplete popup. A flat Lightning
// surface (like AppMenu) that floats ABOVE the composer input while the user
// types an @-token. It deliberately does NOT take focus — the TextArea keeps
// focus and forwards Up/Down/Tab/Return/Escape to the functions below — so the
// caret never leaves the editor. It presents ONLY current-room members from
// the shared MentionSuggestionModel; it never queries the server directory and
// owns no Matrix protocol logic. No message text is ever logged.
//
// v0.6.5 (SPEC §1q): a mono "MENTION · MATCHING "…"" header, a matched-prefix
// tint on the row name, a role chip (ADMIN/MOD, when the member snapshot
// carries a recognized power-level role), a tinted "return" keycap on the
// selected row, and row accessibility. The @room row and presence dots are
// omitted — the model has no data to honestly back either.
//
// Storm skin (SPEC-storm-language §4/2h): stormPanel chrome, faint-mono
// header, stormSelection selected row with a bolt matched prefix, bolt MOD
// chip and active ↵ keycap. Colors/fonts only — behavior unchanged.
Popup {
    id: root
    objectName: "mentionPopup"

    // The MentionSuggestionModel (set by the composer). Its `query`/`roomId`
    // are driven by the composer; this popup only presents + selects.
    property var suggestions: null
    // The composer's live mention-token query text, set by the host alongside
    // suggestions.query. Kept as its own property (rather than read back off
    // the shared model) because both the room and thread hosts bind the same
    // MentionSuggestionModel instance and each owns its own popup's header.
    property string query: ""
    // Top-left of the composer input in overlay coordinates; the popup is
    // placed above it.
    property point anchorInputTop: Qt.point(0, 0)
    property real anchorWidth: 320
    property int currentIndex: 0

    signal chosen(string userId, string displayName)

    // A member with no display name must read as their LOCALPART, never as
    // the full MXID. The row already shows the MXID on its second line, so
    // the MXID fallback printed the same string twice and made a nameless
    // user look like the app was showing usernames on purpose — which is
    // exactly how it was reported.
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
    // The composer drives open/close; auto-close (focus/press-outside) would
    // fight the editor keeping focus.
    closePolicy: Popup.NoAutoClose
    padding: AppTheme.menuPadding

    readonly property int count: suggestions ? suggestions.count : 0
    readonly property int rowH: AppTheme.scaled(42)
    readonly property int headerH: AppTheme.scaled(24)
    readonly property int visibleRows: Math.max(1, Math.min(count, 6))

    width: Math.max(240, Math.min(anchorWidth, 380))
    height: headerH + visibleRows * rowH + padding * 2
    // Clamped inside the overlay (the sibling pickers' placeInsideWindow()
    // convention): the 240px width floor can exceed a narrow thread
    // composer, and a short window would otherwise push the popup's top
    // above the window edge.
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

    // administrator/creator -> ADMIN, moderator -> MOD, everything else
    // (user/default/empty) -> no chip. `role` is the power-level-derived
    // snapshot field (MentionSuggestionModel::RoleRole) — real SDK data on
    // the Rust backend, always "default" (no chip) on the mock.
    function roleChipLabel(role) {
        if (role === "administrator" || role === "creator")
            return qsTr("ADMIN")
        if (role === "moderator")
            return qsTr("MOD")
        return ""
    }

    // Minimal HTML-escape for the matched-prefix rich-text highlight below —
    // display names are untrusted user content and must never be interpreted
    // as markup.
    function escapeHtml(s) {
        return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;")
                        .replace(/>/g, "&gt;")
    }

    // Highlights the query as a prefix of the display name only (the simple,
    // common case matchScore ranks highest); a word-start-only or
    // localpart/MXID-only match still shows the plain, unhighlighted name.
    // `ink` is the highlight colour: bolt on THE selected row only (yellow
    // discipline), a brightened stormText prefix on resting rows.
    function highlightedName(name, q, ink) {
        // A row can legitimately arrive without a display name (the model
        // falls back to the localpart elsewhere); calling String methods on
        // undefined threw a TypeError that killed the whole binding, so the
        // row rendered nothing at all.
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
        // Elevation. This popup floats over the composer CARD, which carries
        // a shadow of its own, and the two were separated by nothing but a
        // 1px border — with the emoji picker or a context menu also up, the
        // stacking order was unreadable. The effect is a SIBLING behind the
        // surface (z: -1), which is what keeps the popup's measured geometry
        // untouched: the documented reason context menus stay border-only is
        // that a shadow ON the background inflates the implicit size their
        // anchor maths depend on. Same construction as the composer card.
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
            // Storm §2: faint mono section-header ink (the storm header
            // vocabulary — deliberately dim decorative mono).
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
                        // Muted MXID under the name (always shown when there is a
                        // display name, and required when the name is ambiguous).
                        Label {
                            // Remote or externally chosen text: never markup.
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            visible: (model.ambiguous === true)
                                     || (model.displayName
                                         && model.displayName.length > 0)
                            // The whole-room row says what it DOES. "@room"
                            // under "room" would just be the same word twice,
                            // and this is the one entry whose consequence —
                            // notifying everybody — is worth spelling out
                            // before it is pressed.
                            text: model.isRoom === true
                                  ? qsTr("Notify everyone in this room")
                                  : model.userId
                            font.family: model.isRoom === true
                                         ? AppTheme.uiFont : AppTheme.monoFont
                            font.pixelSize: AppTheme.scaled(AppTheme.fontMonoXS)
                            // AA on the selection fill: the selected row's
                            // MXID brightens one step.
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
