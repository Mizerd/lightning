import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

// v0.6.5 (SPEC 1p): reusable member profile popover — content redesign only.
// Presentation (centred-modal Popup, `openFor()` contract) is unchanged
// (R12): callers still do `parent: Overlay.overlay; anchors.centerIn: parent`.
// Shows the member's avatar, display name, full Matrix ID, membership/role,
// and offers Start/Open Direct Message (existing DMs are reused, never
// silently duplicated) + Copy ID. Deliberately OMITS call/videocam, a
// Verified chip, a status line, presence, the SHARED-rooms section, View
// full profile, and Ignore — none of those have a real backend today, and
// this component never renders a disabled placeholder in their place.
Popup {
    id: root
    modal: true
    width: 296
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    property string userId: ""
    property string displayName: ""
    property string membership: ""
    property string role: ""
    property string avatarMxc: ""
    property bool isOwn: false

    // The same room-member profile the list row shows — one resolution
    // path, no popover-local reinvention.
    readonly property string visibleName:
        displayName.length > 0
        ? displayName
        : (userId.length > 1
           ? userId.slice(1).split(":")[0] : userId)

    // NOTE: Accessible attaches to the contentItem below — Popup itself is
    // not an Item, and attaching here only logs a warning at load.

    function openFor(member) {
        userId = member.userId || ""
        displayName = member.displayName || ""
        membership = member.membership || ""
        role = member.role || ""
        avatarMxc = member.avatarUrl || ""
        isOwn = member.isOwn === true
        open()
    }

    function startOrOpenDm() {
        // Reuse an existing DM when the SDK's m.direct projection has one;
        // otherwise create a new encrypted DM (opened via conversationReady).
        app.conversations.checkExistingDm(userId)
        var existing = app.conversations.existingDms
        if (existing.length > 0)
            app.openRoom(existing[0].roomId)
        else
            app.conversations.startDirectMessage(userId)
        close()
    }

    // v0.6.5 (R14): this popover is already centred-modal (R12) — the
    // themed scrim override applies here (AccountMenu is an anchored
    // popover, not a centred modal, and deliberately keeps the default).
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }

    // Sanctioned shadow (R3), composer-card pattern: the effect and its
    // source Rectangle must be SIBLINGS inside one background Item —
    // MultiEffect cannot anchor across the Popup background boundary.
    background: Item {
        Rectangle {
            id: popoverBackground
            anchors.fill: parent
            color: AppTheme.surface
            border.color: AppTheme.border
            border.width: 1
            radius: AppTheme.radiusLg
        }
        MultiEffect {
            source: popoverBackground
            anchors.fill: popoverBackground
            z: -1
            shadowEnabled: true
            shadowColor: AppTheme.shadow
            shadowBlur: 0.6
            shadowVerticalOffset: 2
            shadowHorizontalOffset: 0
        }
    }

    contentItem: ColumnLayout {
        spacing: 0
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Profile for %1").arg(root.visibleName)

        Item {
            id: header
            Layout.fillWidth: true
            // Cover the avatar's real bottom edge — the halo disc hangs 34px
            // below the banner, so banner.height + 28 alone left it painting
            // 6px outside this Item's box.
            implicitHeight: Math.max(banner.height + 28,
                                     avatarWrap.y + avatarWrap.height)

            // Banner gradient, Canvas-painted (theme-reactive colour
            // properties) so the 120° angle and the ~70% stop can be
            // expressed exactly — a plain vertical Gradient cannot.
            Canvas {
                id: banner
                objectName: "profileBanner"
                width: parent.width
                height: 64
                property color c1: AppTheme.accentSoft
                property color c2: AppTheme.surface
                property int cornerRadius: AppTheme.radiusLg
                onC1Changed: requestPaint()
                onC2Changed: requestPaint()
                onCornerRadiusChanged: requestPaint()
                onWidthChanged: requestPaint()
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()
                    var w = width, h = height
                    var rr = Math.max(0, Math.min(cornerRadius, w / 2, h))
                    ctx.beginPath()
                    ctx.moveTo(0, h)
                    ctx.lineTo(0, rr)
                    ctx.quadraticCurveTo(0, 0, rr, 0)
                    ctx.lineTo(w - rr, 0)
                    ctx.quadraticCurveTo(w, 0, w, rr)
                    ctx.lineTo(w, h)
                    ctx.closePath()
                    // CSS-style angle (0deg = up, clockwise) converted to a
                    // canvas linear-gradient direction spanning the box.
                    var angle = 120 * Math.PI / 180
                    var dx = Math.sin(angle)
                    var dy = -Math.cos(angle)
                    var length = Math.abs(w * dx) + Math.abs(h * dy)
                    var cx = w / 2, cy = h / 2
                    var grad = ctx.createLinearGradient(
                        cx - dx * length / 2, cy - dy * length / 2,
                        cx + dx * length / 2, cy + dy * length / 2)
                    grad.addColorStop(0, c1)
                    // Stopping ~70%: the destination tone takes over before
                    // the gradient reaches the bottom edge.
                    grad.addColorStop(0.7, c2)
                    ctx.fillStyle = grad
                    ctx.fill()
                }
            }

            // 56px avatar overlapping the banner's bottom-left, with a 3px
            // surface ring (a solid backing disc slightly larger than the
            // avatar, painted after the banner so the ring reads as a halo).
            Item {
                id: avatarWrap
                x: AppTheme.spacing16
                y: banner.height - 28
                width: 62
                height: 62
                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color: AppTheme.surface
                }
                Avatar {
                    anchors.centerIn: parent
                    size: 56
                    mxc: root.avatarMxc
                    name: root.visibleName
                    colorKey: root.userId
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing16
            Layout.rightMargin: AppTheme.spacing16
            Layout.topMargin: AppTheme.spacing8
            Layout.bottomMargin: AppTheme.spacing16
            spacing: AppTheme.spacing8

            Label {
                Layout.fillWidth: true
                text: root.visibleName
                color: AppTheme.textPrimary
                font.pixelSize: 16
                font.weight: Font.ExtraBold
                elide: Label.ElideRight
            }
            Label {
                Layout.fillWidth: true
                text: root.userId
                color: AppTheme.monoIdentityColor
                font.family: AppTheme.monoFont
                font.pixelSize: AppTheme.fontMonoXS
                elide: Label.ElideMiddle
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                Label {
                    visible: root.membership.length > 0
                    text: root.membership === "invited" ? qsTr("Invited")
                        : root.membership === "joined" ? qsTr("Member")
                        : root.membership
                    color: root.membership === "invited" ? AppTheme.warning
                                                         : AppTheme.textSecondary
                    font.pixelSize: AppTheme.fontSecondary
                }
                StatusChip {
                    visible: root.role === "administrator" || root.role === "creator"
                    tone: "accent"
                    solid: true
                    label: qsTr("Administrator")
                }
                StatusChip {
                    visible: root.role === "moderator"
                    tone: "neutral"
                    solid: true
                    label: qsTr("Moderator")
                }
                Label {
                    visible: root.isOwn
                    text: qsTr("(you)")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.fontSecondary
                }
                Item { Layout.fillWidth: true }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8

                // Primary Message action — AppButton has no icon slot or a
                // parametrized radius, so this mirrors its accent styling
                // locally with the chat_bubble icon the spec asks for.
                AbstractButton {
                    id: messageButton
                    text: qsTr("Message")
                    visible: !root.isOwn && app.conversations.supported
                    Layout.fillWidth: true
                    implicitHeight: 32
                    hoverEnabled: true
                    focusPolicy: Qt.TabFocus
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Start or open a direct message with %1")
                                     .arg(root.displayName.length > 0
                                          ? root.displayName : root.userId)
                    // Spacer-centred (the copyIdButton idiom): a control
                    // stretches its contentItem to the full button width, so
                    // anchors.centerIn on the layout is a no-op and the
                    // icon+label would sit hard against the left edge.
                    contentItem: RowLayout {
                        spacing: AppTheme.spacing6
                        Item { Layout.fillWidth: true }
                        Icon { name: "chat_bubble"; size: 16; color: AppTheme.accentText }
                        Label {
                            text: qsTr("Message")
                            color: AppTheme.accentText
                            font.pixelSize: 13
                            font.weight: Font.Bold
                        }
                        Item { Layout.fillWidth: true }
                    }
                    background: Rectangle {
                        radius: AppTheme.radiusTile
                        color: !messageButton.enabled ? AppTheme.cardElevated
                               : messageButton.down ? AppTheme.accentPressed
                               : messageButton.hovered ? AppTheme.accentHover
                               : AppTheme.accent
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -4
                        radius: AppTheme.radiusTile + 4
                        color: "transparent"
                        border.width: 2
                        border.color: AppTheme.focusRing
                        visible: messageButton.visualFocus
                    }
                    onClicked: root.startOrOpenDm()
                }
                AbstractButton {
                    id: copyIdButton
                    objectName: "profileCopyIdButton"
                    text: qsTr("Copy ID")
                    // When Message is unavailable (own profile, or a backend
                    // without DM support) the lone icon square would orphan
                    // hard-left — expand into a labelled, row-filling button
                    // so the action row keeps its designed weight.
                    readonly property bool expanded: !messageButton.visible
                    Layout.fillWidth: expanded
                    implicitWidth: expanded ? copyIdContent.implicitWidth + 28
                                            : 32
                    implicitHeight: 32
                    hoverEnabled: true
                    focusPolicy: Qt.TabFocus
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Copy Matrix ID %1").arg(root.userId)
                    contentItem: RowLayout {
                        id: copyIdContent
                        spacing: AppTheme.spacing6
                        Item { Layout.fillWidth: true }
                        Icon {
                            name: "content_copy"
                            size: 16
                            color: AppTheme.textSecondary
                        }
                        Label {
                            visible: copyIdButton.expanded
                            text: copyIdButton.text
                            color: AppTheme.textPrimary
                            font.pixelSize: 13
                            font.weight: Font.Bold
                        }
                        Item { Layout.fillWidth: true }
                    }
                    background: Rectangle {
                        radius: AppTheme.radiusTile
                        color: (copyIdButton.hovered || copyIdButton.down)
                               ? AppTheme.hover : AppTheme.cardElevated
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -4
                        radius: AppTheme.radiusTile + 4
                        color: "transparent"
                        border.width: 2
                        border.color: AppTheme.focusRing
                        visible: copyIdButton.visualFocus
                    }
                    onClicked: {
                        idClipboard.text = root.userId
                        idClipboard.selectAll()
                        idClipboard.copy()
                        idClipboard.text = ""
                        copiedNotice.visible = true
                        copiedTimer.restart()
                    }
                }
            }
            Label {
                id: copiedNotice
                visible: false
                text: qsTr("Matrix ID copied")
                color: AppTheme.textMuted
                font.pixelSize: 11
                Accessible.name: text
                Timer {
                    id: copiedTimer
                    interval: 1500
                    onTriggered: copiedNotice.visible = false
                }
            }
            TextEdit {
                id: idClipboard
                visible: false
                width: 0; height: 0
            }
        }
    }
}
