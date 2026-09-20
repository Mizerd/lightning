import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// One account ROW in the account switcher.
//
// 2026-09-19 (reported: "scrolling in it feels kind of dumb since you can
// even scroll about with one account since it doesn't fit"). This used to be
// a ~180 px CARD, and the ACTIVE variant a ~235 px one carrying a meta line,
// an E2EE badge and a trust meter. Three accounts overflowed a popover that
// was sized from an off-layout PROBE of this component — and the probe
// carried neither the trust meter nor the E2EE badge, so it under-measured
// the real active card by exactly 23 px (measured: short 113, tall probe
// 136, real active 159, at the 296 px content width). That 23 px is why one
// single account could scroll.
//
// The cure is structural, not arithmetic: this is now a FIXED-HEIGHT row
// whose height is handed in by the list (`rowHeight`), with no optional row
// that can grow it. Everything the row cannot carry for every account moved
// to AccountMenu's one status strip — per-row crypto state was only ever
// available for the ACTIVE account anyway, so it is a property of one row,
// never a column.
//
// Density is the product's existing person-row ladder (MentionPopup's
// suggestion row): 28 px avatar, textBody name, mono fontMonoXS id beneath
// it. Selection is the room list's existing "this is the current one"
// vocabulary: a rounded `selected` chip inset in a 4 px gutter plus a 3 px
// bolt left edge.
//
// No access token, device secret, or local path is ever displayed.
Item {
    id: root
    objectName: "identityCard"

    // NO property named `name` (IconChromeTest repo-wide scan / R16).
    property bool active: false
    property string displayName: ""
    property string userId: ""
    property string avatarMxc: ""
    // No real per-account unread source exists today; callers leave this at
    // its default 0 rather than fabricate a count. It costs no height — the
    // chip rides in the row's right-hand slot.
    property int unreadCount: 0
    property bool needsSignIn: false
    property bool healthWarning: false

    // THE row height, handed in by the list that lays these out so the
    // viewport and the content can never disagree by construction. Nothing
    // inside this component may grow it; the content is centred and elided
    // inside it instead.
    property int rowHeight: AppTheme.scaled(44)

    signal activated()
    signal removeRequested()

    // The pointer is "within" the row while it is over the row OR over a
    // control on it. MouseArea.containsMouse alone is not that: a
    // hover-enabled control above the MouseArea takes the hover the moment
    // the pointer reaches it, and an affordance revealed by containsMouse
    // then hides under the pointer that came for it. A HoverHandler does
    // not compete with children, and the remove button's own `hovered`
    // covers the button whatever the delivery order.
    readonly property bool pointerWithin: cardHover.hovered
                                          || cardMouse.containsMouse
                                          || removeButton.hovered
    HoverHandler { id: cardHover }

    readonly property string localpart: {
        var uid = root.userId
        if (uid.indexOf("@") === 0) uid = uid.slice(1)
        var colon = uid.indexOf(":")
        return colon > 0 ? uid.slice(0, colon) : uid
    }
    readonly property string visibleName:
        root.displayName.length > 0 ? root.displayName : root.localpart

    // Two accounts can share a display name on different homeservers (the
    // reported case: two "Mizerd"s), so the id under the name is the only
    // thing telling them apart. Exposed so a test can assert it is not
    // truncated at the popover's width.
    readonly property alias identityLabel: idLabel

    implicitWidth: 280
    implicitHeight: rowHeight
    // The text column is measured against the row so a scale the ladder
    // cannot carry is visible to a test rather than silently clipped.
    readonly property real textColumnHeight: textColumn.implicitHeight

    Accessible.role: Accessible.Button
    Accessible.focusable: true
    Accessible.name: root.active
        ? qsTr("Active account, %1, %2").arg(root.visibleName).arg(root.userId)
        : qsTr("Switch to %1, %2").arg(root.visibleName).arg(root.userId)
    Accessible.description: root.active ? qsTr("This is the active account")
        : (root.needsSignIn ? qsTr("Needs sign-in")
           : (root.healthWarning ? qsTr("Encryption needs attention") : ""))
    Accessible.onPressAction: root.activated()

    activeFocusOnTab: true
    Keys.onReturnPressed: root.activated()
    Keys.onEnterPressed: root.activated()
    Keys.onSpacePressed: root.activated()

    // ── Selection / hover chip — the room list's own idiom (RoomDelegate):
    // a rounded chip inset in a 4 px gutter, never a full-bleed square.
    // `selected` / `selectedHover` / `hover` are three distinct values in
    // every palette, so a hovered inactive row can never be mistaken for
    // the active one.
    Rectangle {
        id: rowChip
        objectName: "identityCardRowChip"
        anchors.fill: parent
        anchors.leftMargin: AppTheme.spacing4
        anchors.rightMargin: AppTheme.spacing4
        radius: AppTheme.radiusMd
        color: root.active
               ? (root.pointerWithin ? AppTheme.selectedHover
                                     : AppTheme.selected)
               : (root.pointerWithin ? AppTheme.hover : "transparent")
    }

    // Active marker 1 of 3 — the room list's bolt edge bar, in the 4 px
    // gutter the chip above already leaves.
    Rectangle {
        objectName: "identityCardActiveEdge"
        visible: root.active
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: 3
        height: Math.max(16, parent.height - AppTheme.spacing12)
        radius: width / 2
        color: AppTheme.bolt
    }

    Rectangle {
        visible: root.activeFocus
        anchors.fill: rowChip
        radius: rowChip.radius
        color: "transparent"
        border.width: 2
        border.color: AppTheme.bolt
    }

    MouseArea {
        id: cardMouse
        anchors.fill: parent
        z: -1
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        acceptedButtons: Qt.LeftButton
        onClicked: {
            root.forceActiveFocus()
            root.activated()
        }
    }

    RowLayout {
        id: body
        anchors.fill: parent
        anchors.leftMargin: AppTheme.spacing12
        anchors.rightMargin: AppTheme.spacing10
        spacing: AppTheme.spacing8

        Item {
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: AppTheme.scaled(28)
            implicitHeight: AppTheme.scaled(28)
            Avatar {
                anchors.centerIn: parent
                size: AppTheme.scaled(28)
                circle: true
                name: root.visibleName
                mxc: root.avatarMxc
                // THE ONLY PLACE IN THE APPLICATION THAT SETS THIS, and the
                // reason it exists: MediaBridge fetches through whichever
                // client is ACTIVE, so an inactive account's avatar cannot
                // be fetched here at all — its bytes are on that account's
                // homeserver. Every row but one showed initials for ever.
                // `avatarUrlFor` returns "" when nothing was ever stored,
                // which leaves the honest initials behind.
                fallbackSource: (typeof app !== "undefined" && app
                                 && app.accountAvatars && root.userId.length > 0)
                                ? app.accountAvatars.avatarUrlFor(root.userId)
                                : ""
                colorKey: root.userId
            }
            // Active marker 2 of 3 — Storm §3.5's yellow-ring identity
            // marker, tightened to a 2 px gap for the row ladder.
            Rectangle {
                objectName: "identityCardAvatarRing"
                visible: root.active
                anchors.fill: parent
                anchors.margins: -3
                radius: width / 2
                color: "transparent"
                border.width: 2
                border.color: AppTheme.bolt
            }
        }

        ColumnLayout {
            id: textColumn
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: 0

            Label {
                objectName: "identityCardName"
                // Remote or externally chosen text: never markup.
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: root.visibleName
                // MEASURED across all eleven palettes, not chosen. The
                // inactive name used to ink `stormTextSecondary`, which in
                // the LIGHT palettes is all but the same colour as the id's
                // `stormTextMuted` beneath it — Lightning Light #4c5661 vs
                // #525c68, 5.63:1 against 5.12:1 on the popover canvas, a
                // ratio of 1.10, where Storm reads 11.00 against 6.75. The
                // two lines stopped being a hierarchy and read as one block
                // of grey. With `stormText` the name/id ratio is >= 1.85 on
                // every palette (worst: Nordic 1.85) and the id is
                // untouched. The active row is not told apart by this ink —
                // it never was; it has the selected chip, the bolt edge,
                // the avatar ring, the tick and the heavier weight.
                color: AppTheme.stormText
                font.family: AppTheme.menuFont
                font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                font.weight: root.active ? AppTheme.weightBold
                                         : AppTheme.weightStrong
                elide: Label.ElideRight
            }
            Label {
                id: idLabel
                objectName: "identityCardUserId"
                // Remote or externally chosen text: never markup.
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: root.userId
                // MEASURED, not chosen. `stormTextFaint` under the row
                // (the old card's ink) resolves to `textDisabled` in the
                // light palette: #A8B3C4 on the popover's #D3E1F2 is
                // 1.60:1, and this line is the ONLY thing separating two
                // accounts that share a display name. These are
                // MentionPopup's own MXID inks and they measure 5.05:1
                // light / 6.80:1 Storm inactive, and 4.81:1 / 7.14:1 on the
                // active row's fill.
                color: root.active ? AppTheme.stormTextSecondary
                                   : AppTheme.stormTextMuted
                // Mono is right for a Matrix ID.
                font.family: AppTheme.monoFont
                font.pixelSize: AppTheme.scaled(AppTheme.fontMonoXS)
                elide: Label.ElideMiddle
            }
        }

        Icon {
            objectName: "identityCardHealthIcon"
            visible: root.needsSignIn || root.healthWarning
            name: root.needsSignIn ? "error" : "warning"
            size: AppTheme.scaled(15)
            color: AppTheme.stormDanger
            Layout.alignment: Qt.AlignVCenter
            Accessible.ignored: true
        }
        // Storm §3.7: unread badges are bolt-on-dark, replacing red.
        StatusChip {
            visible: !root.active && !root.pointerWithin
                     && root.unreadCount > 0
            Layout.alignment: Qt.AlignVCenter
            storm: true
            tone: "bolt"
            label: root.unreadCount > 99 ? "99+" : String(root.unreadCount)
        }
        // Active marker 3 of 3 — the "you are here" tick. A chip reading
        // ACTIVE cost more width than the id beneath the name can spare.
        Icon {
            objectName: "identityCardActiveTick"
            visible: root.active
            name: "check_circle"
            size: AppTheme.scaled(17)
            color: AppTheme.bolt
            Layout.alignment: Qt.AlignVCenter
            Accessible.ignored: true
        }
        ToolButton {
            id: removeButton
            objectName: "identityCardRemoveButton"
            // Keyboard parity with hover: the affordance reveals when the
            // row OR the button itself holds focus, and the button is a
            // real tab stop while revealed.
            readonly property bool revealed:
                !root.active && (root.pointerWithin || root.activeFocus
                                 || removeButton.activeFocus)
            // THE SLOT IS HELD ON EVERY INACTIVE ROW, AND FADING IS WHY.
            // This is a real RowLayout child, so revealing it with
            // `visible` took 30 px + 8 px of spacing out of the text column
            // the moment the pointer arrived, and the id beneath the name
            // re-elided under the cursor: measured, `@dave:chat.very…
            // ame.example.net` at rest became `@dave:chat.v….example.net`
            // on hover. Five characters of the one string that tells two
            // accounts sharing a display name apart, removed at exactly the
            // moment someone is reading it. The row now reserves the slot
            // whenever it could ever show the button, and only the paint
            // changes — same lesson as AppMenuItem's constant content
            // inset: nothing under the pointer may move because the pointer
            // arrived.
            visible: !root.active
            opacity: revealed ? 1 : 0
            // Not merely invisible: a transparent button that still took
            // clicks would remove an account nobody aimed at, and a
            // transparent tab stop would be a focus trap with nothing on
            // screen.
            enabled: revealed
            activeFocusOnTab: !root.active
            Accessible.ignored: !revealed
            Layout.alignment: Qt.AlignVCenter
            // 30 px, not 22: "make the x hitbox bigger" (2026-09-06). The
            // row is 44 px tall, so this costs no height.
            implicitWidth: 30
            implicitHeight: 30
            Accessible.name: qsTr("Remove account %1").arg(root.userId)
            ToolTip.text: qsTr("Remove from this device")
            ToolTip.visible: hovered
            ToolTip.delay: 500
            contentItem: Icon {
                name: "close"
                size: 15
                color: AppTheme.stormDanger
            }
            background: Rectangle {
                radius: AppTheme.radiusSm
                color: removeButton.hovered ? AppTheme.stormSelection
                                            : "transparent"
            }
            onClicked: root.removeRequested()
        }
    }
}
