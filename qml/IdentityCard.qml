import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.6.5 (SPEC 1h, modified — vertical identity cards): one account row in
// the redesigned account switcher. The active account renders as a themed
// accent-gradient card; every other saved account renders as a flat surface
// card. Health/removal affordances are the EXISTING per-row states from
// AccountMenu.qml, just translated into this layout — nothing here invents
// new account state.
//
// Deliberate API note: `connected` is one boolean beyond the literal
// property set specified for this component. The active card's meta row
// needs a live connectivity signal to color its presence dot (reusing the
// exact source SpacesRail.qml's self-avatar dot uses) and there is no other
// carrier for that in the given contract; adding one boolean is far safer
// than encoding colour state inside the `metaText` string. Disclosed as a
// deviation in the implementer's report.
Item {
    id: root
    objectName: "identityCard"

    // NO property named `name` (IconChromeTest repo-wide scan / R16).
    property bool active: false
    property string displayName: ""
    property string userId: ""
    property string avatarMxc: ""
    // Presence + real space-count text for the ACTIVE card only ("Connected"
    // or "Connected · 4 spaces"). Always empty for an inactive card: the SDK
    // only reports presence/space membership for the attached session, so
    // there is no real source for an inactive account's connection state.
    property string metaText: ""
    // No real per-account unread source exists today; callers leave this at
    // its default 0 rather than fabricate a count.
    property int unreadCount: 0
    // Storm §3.4 trust meter — shown only when the caller supplies a REAL
    // completed count (the SDK only reports crypto state for the attached
    // account, so in practice the active card alone carries it). -1 hides.
    property int trustCompleted: -1
    property int trustTotal: 3
    property bool e2eeReady: false
    property bool needsSignIn: false
    property bool healthWarning: false
    property bool connected: true

    signal activated()
    signal removeRequested()

    readonly property string localpart: {
        var uid = root.userId
        if (uid.indexOf("@") === 0) uid = uid.slice(1)
        var colon = uid.indexOf(":")
        return colon > 0 ? uid.slice(0, colon) : uid
    }
    readonly property string visibleName:
        root.displayName.length > 0 ? root.displayName : root.localpart

    implicitWidth: 280
    implicitHeight: body.implicitHeight + 2 * AppTheme.spacing14
    clip: true

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

    // ── Surface (inactive card) — Storm §4 2c: inset panel. ─────────────
    Rectangle {
        id: surface
        anchors.fill: parent
        radius: AppTheme.radiusCard
        visible: !root.active
        color: AppTheme.stormInset
        border.width: 1
        border.color: root.activeFocus ? AppTheme.bolt : AppTheme.stormBorder
    }

    // ── Active card fill — Storm §4 2c: flat stormPanel with the strong
    // border; the previous themed Canvas gradient retired with the storm
    // language. Since the 0.6.5 routing correction, stormPanel/
    // stormBorderStrong follow the user's selected theme again (Storm
    // itself is now a real selectable theme, id 11) — this card renders
    // navy only under Storm, and the active user's own theme otherwise.
    Rectangle {
        objectName: "identityCardActiveFill"
        anchors.fill: parent
        visible: root.active
        radius: AppTheme.radiusCard
        color: AppTheme.stormPanel
        border.width: 1
        border.color: AppTheme.stormBorderStrong
    }

    // Focus ring for the active card — its border is the resting stroke,
    // so focus needs a dedicated overlay.
    Rectangle {
        visible: root.active && root.activeFocus
        anchors.fill: parent
        radius: AppTheme.radiusCard
        color: "transparent"
        border.width: 2
        border.color: AppTheme.bolt
    }

    // Bolt watermark — active card only, overflowing the bottom-right
    // corner; `clip: true` on root crops it to the card's rectangular
    // bounds (Qt Quick clipping is always axis-aligned, so the extreme
    // corner pixel is a negligible, accepted approximation of the rounded
    // shape — the same trade-off TrustCard's watermark makes).
    Icon {
        name: "bolt"
        size: 76
        color: AppTheme.stormWatermark
        visible: root.active
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.bottomMargin: -14
        anchors.rightMargin: -16
    }

    Rectangle {
        id: hoverTint
        anchors.fill: parent
        radius: AppTheme.radiusCard
        color: AppTheme.stormSelection
        visible: !root.active && cardMouse.containsMouse
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

    ColumnLayout {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: AppTheme.spacing14
        spacing: AppTheme.spacing6

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            Item {
                implicitWidth: 34
                implicitHeight: 34
                Avatar {
                    anchors.centerIn: parent
                    size: 34
                    circle: true
                    name: root.visibleName
                    mxc: root.avatarMxc
                    colorKey: root.userId
                }
                // Storm §3.5 yellow-ring identity marker: 2px panel gap +
                // 2px bolt stroke — the ONE yellow-ring avatar this surface
                // shows (active card only).
                Rectangle {
                    visible: root.active
                    anchors.fill: parent
                    anchors.margins: -4
                    radius: width / 2
                    color: "transparent"
                    border.width: 2
                    border.color: AppTheme.bolt
                }
            }

            Item { Layout.fillWidth: true }

            Icon {
                visible: root.needsSignIn || root.healthWarning
                name: root.needsSignIn ? "error" : "warning"
                size: 14
                color: AppTheme.stormDanger
                Accessible.ignored: true
            }
            StatusChip {
                visible: root.active
                storm: true
                tone: "bolt"
                label: qsTr("ACTIVE")
            }
            // Storm §3.7: unread badges are bolt-on-dark, replacing red.
            StatusChip {
                visible: !root.active && !cardMouse.containsMouse
                         && root.unreadCount > 0
                storm: true
                tone: "bolt"
                label: root.unreadCount > 99 ? "99+" : String(root.unreadCount)
            }
            ToolButton {
                id: removeButton
                objectName: "identityCardRemoveButton"
                // Keyboard parity with hover: the affordance reveals when
                // the card OR the button itself holds focus, and the button
                // is a real tab stop while revealed.
                visible: !root.active
                         && (cardMouse.containsMouse || root.activeFocus
                             || removeButton.activeFocus)
                activeFocusOnTab: !root.active
                implicitWidth: 22
                implicitHeight: 22
                Accessible.name: qsTr("Remove account %1").arg(root.userId)
                ToolTip.text: qsTr("Remove from this device")
                ToolTip.visible: hovered
                ToolTip.delay: 500
                contentItem: Icon {
                    name: "close"
                    size: 13
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

        Label {
            Layout.fillWidth: true
            text: root.visibleName
            color: root.active ? AppTheme.stormText : AppTheme.stormTextSecondary
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.fontQuery
            font.weight: Font.Bold
            elide: Label.ElideRight
        }
        Label {
            Layout.fillWidth: true
            text: root.userId
            color: root.active ? AppTheme.stormTextMuted
                                : AppTheme.stormTextFaint
            font.family: AppTheme.monoFont
            font.pixelSize: AppTheme.fontChip
            elide: Label.ElideMiddle
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing6
            visible: root.metaText.length > 0

            // Storm: the connected dot is bolt (§4 2h presence idiom);
            // disconnected falls to the muted ink.
            Rectangle {
                visible: root.active
                width: 6
                height: 6
                radius: 3
                color: root.connected ? AppTheme.bolt
                                       : AppTheme.stormTextMuted
            }
            Label {
                Layout.fillWidth: true
                text: root.metaText
                color: AppTheme.stormTextMuted
                font.family: AppTheme.monoFont
                font.pixelSize: AppTheme.fontChip
                font.weight: Font.DemiBold
                elide: Label.ElideRight
            }
            RowLayout {
                visible: root.e2eeReady
                spacing: 2
                Icon {
                    name: "check"
                    size: AppTheme.fontMonoXS + 1
                    color: AppTheme.stormSuccess
                }
                Label {
                    text: qsTr("E2EE")
                    color: AppTheme.stormSuccess
                    font.family: AppTheme.monoFont
                    font.pixelSize: AppTheme.fontChip
                    font.weight: Font.DemiBold
                }
            }
        }

        TrustMeter {
            objectName: "identityCardTrustMeter"
            visible: root.trustCompleted >= 0
            Layout.fillWidth: true
            Layout.topMargin: AppTheme.spacing4
            completed: Math.max(0, root.trustCompleted)
            total: root.trustTotal
        }
    }
}
