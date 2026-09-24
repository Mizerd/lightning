import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// One account row in the account switcher.
//
// A fixed-height row whose height is handed in by the list (`rowHeight`), so
// the list's viewport and content always agree. Per-account crypto state is
// only known for the active account, so it lives in AccountMenu's status
// strip, not here.
//
// Density follows MentionPopup's person row: 28 px avatar, name, mono id
// beneath. Selection uses the room list's idiom: a rounded `selected` chip in
// a 4 px gutter plus a 3 px bolt left edge.
//
// No access token, device secret or local path is ever displayed.
Item {
    id: root
    objectName: "identityCard"

    // No property named `name` (IconChromeTest's repo-wide scan).
    property bool active: false
    property string displayName: ""
    property string userId: ""
    property string avatarMxc: ""
    // No per-account unread source exists yet; callers leave this at 0 rather
    // than fabricate a count.
    property int unreadCount: 0
    property bool needsSignIn: false
    property bool healthWarning: false

    // The row height, set by the list so viewport and content can't disagree.
    // Nothing inside may grow it; content is centred and elided.
    property int rowHeight: AppTheme.scaled(44)

    signal activated()
    signal removeRequested()

    // The pointer is "within" the row when over it or over a control on it.
    // MouseArea.containsMouse alone loses hover to a control above it, which
    // would hide the affordance the pointer came for; a HoverHandler doesn't
    // compete with children.
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

    // Accounts can share a display name across homeservers, so the id is what
    // tells them apart. Exposed so a test can check it isn't truncated.
    readonly property alias identityLabel: idLabel

    implicitWidth: 280
    implicitHeight: rowHeight
    // Exposed so a test can see a scale the row can't carry, rather than it
    // being silently clipped.
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

    // The surface this row sits on (AccountMenu paints `stormCanvas`). The row
    // fill is composited onto it to derive text inks.
    property color hostSurface: AppTheme.stormCanvas

    // The pixels actually under the text: the chip over the host surface, with
    // the chip's own alpha (`hover` is translucent in some palettes).
    readonly property color rowFill: AppTheme.flatten(rowChip.color,
                                                      root.hostSurface)

    // ── Selection / hover chip (RoomDelegate's idiom): a rounded chip inset in
    // a 4 px gutter. selected / selectedHover / hover are distinct in every
    // palette, so a hovered row never looks active.
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

    // Active marker 1 of 3: the bolt edge bar in the chip's gutter.
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
                // The only place that sets this: MediaBridge fetches through
                // the active client, so an inactive account's avatar can't be
                // fetched here. `avatarUrlFor` returns "" when nothing was
                // stored, leaving initials.
                fallbackSource: (typeof app !== "undefined" && app
                                 && app.accountAvatars && root.userId.length > 0)
                                ? app.accountAvatars.avatarUrlFor(root.userId)
                                : ""
                colorKey: root.userId
            }
            // Active marker 2 of 3: the bolt avatar ring.
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
                // stormText so the name/id pair stays a clear hierarchy on
                // every palette (the secondary ink is nearly the id's colour on
                // light themes). The active row is distinguished by chip, edge,
                // ring, tick and weight, not ink. Derived against the actual
                // row fill, which only changes it where needed (hovered active
                // row on Nordic).
                color: AppTheme.legibleInkOn(AppTheme.stormText, root.rowFill)
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
                // Derived against the fill this row is actually painting (rest,
                // hover, selected or selectedHover), since the loud fills fail
                // AA with the plain token on several palettes. A single ink for
                // all four states would invert the name/id hierarchy (see
                // legibleInkOn in AppTheme). A token that already clears its
                // fill is returned unchanged. This line is the only thing
                // separating accounts that share a display name.
                color: AppTheme.legibleInkOn(root.active
                                             ? AppTheme.stormTextSecondary
                                             : AppTheme.stormTextMuted,
                                             root.rowFill)
                // Mono for a Matrix ID.
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
        // Unread badges are bolt-on-dark.
        StatusChip {
            visible: !root.active && !root.pointerWithin
                     && root.unreadCount > 0
            Layout.alignment: Qt.AlignVCenter
            storm: true
            tone: "bolt"
            label: root.unreadCount > 99 ? "99+" : String(root.unreadCount)
        }
        // Active marker 3 of 3: the tick (an "ACTIVE" chip would cost the id
        // width).
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
            // Revealed on hover or when the row or button has focus; a real tab
            // stop while revealed.
            readonly property bool revealed:
                !root.active && (root.pointerWithin || root.activeFocus
                                 || removeButton.activeFocus)
            // The slot is held on every inactive row and only the opacity
            // changes, so revealing the button doesn't re-elide the id under
            // the cursor.
            visible: !root.active
            opacity: revealed ? 1 : 0
            // Disabled while hidden, so an invisible button can't remove an
            // account or trap focus.
            enabled: revealed
            activeFocusOnTab: !root.active
            Accessible.ignored: !revealed
            Layout.alignment: Qt.AlignVCenter
            // A 30 px hit target; fits the 44 px row.
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
