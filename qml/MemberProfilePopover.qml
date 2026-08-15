import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

// v0.6.5 (SPEC 1p): reusable member profile popover — content redesign only.
// Presentation (centred-modal Popup, `openFor()` contract) is unchanged
// (R12): callers still do `parent: Overlay.overlay; anchors.centerIn: parent`.
// Shows the member's avatar, display name, full Matrix ID, membership/role,
// real Matrix presence (dot + status line, v0.7.x — unknown renders
// nothing), and the member's real power level (v0.7.x — rendered as its
// number when the room uses an unconventional value, changeable only to
// levels the viewer may actually set),
// and offers Start/Open Direct Message (existing DMs are reused,
// never silently duplicated) + Copy ID. Deliberately OMITS call/videocam, a
// Verified chip, the SHARED-rooms section, View full profile, and Ignore —
// none of those have a real backend today, and this component never
// renders a disabled placeholder in their place.
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

    // --- Moderation (kick / ban) state ---
    // "" | "kick" | "ban" — which confirm surface is showing.
    property string modAction: ""
    property string modError: ""
    // Unban confirm option: send a normal invite after a successful
    // unban (maintainer request — one step back in instead of two).
    property bool inviteBackChecked: true
    // The offer policy is the CONTROLLER'S (RoomInfoController::
    // canModerate: SDK permission flag, loaded snapshot row for the
    // target — unknown fails closed — non-self, strictly below the
    // viewer's own power level). Captured at openFor() time; the
    // controller re-checks at dispatch, so this is presentation only.
    // Scoped to the room-info snapshot being for the room currently open
    // (the panel may show a Space's settings instead).
    property bool showKick: false
    property bool showBan: false
    property bool showUnban: false

    // v0.7.x room administration: the member's real numeric power level and
    // the role changes the viewer may actually make. Same discipline as the
    // moderation flags — captured at openFor()/refresh time for
    // presentation, re-checked by the controller at dispatch.
    property string roleLabel: ""
    property var roleOptions: []
    property string roleError: ""

    function _refreshModeration() {
        var scoped = app.roomInfo
                     && app.roomInfo.roomId === app.currentRoomId
                     && userId.length > 0
        showKick = scoped ? app.roomInfo.canModerate(userId, "kick") : false
        showBan = scoped ? app.roomInfo.canModerate(userId, "ban") : false
        showUnban = scoped ? app.roomInfo.canModerate(userId, "unban") : false
        _refreshRole(scoped)
    }

    function _refreshRole(scoped) {
        if (!scoped) {
            roleLabel = ""
            roleOptions = []
            return
        }
        var current = app.roomInfo.powerLevelFor(userId)
        roleLabel = app.roomInfo.roleLabelForLevel(current)
        // The conventional presets plus the room's own default. A room may
        // set users_default to anything, so it is included rather than
        // assumed to be 0 — and duplicates collapse.
        var candidates = [app.roomInfo.usersDefaultPowerLevel, 50, 100]
        var seen = {}
        var out = []
        for (var i = 0; i < candidates.length; ++i) {
            var level = candidates[i]
            if (seen[level] === true)
                continue
            seen[level] = true
            // Offer ONLY what the viewer may actually set: canSetPowerLevel
            // applies the room's real permission plus the Matrix rules
            // about acting on peers at or above your own level.
            if (!app.roomInfo.canSetPowerLevel(userId, level))
                continue
            out.push({ level: level,
                       label: app.roomInfo.roleLabelForLevel(level) })
        }
        roleOptions = out
    }

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
        modAction = ""
        modError = ""
        roleError = ""
        ignoreNotice = ""
        modReasonField.text = ""
        inviteBackChecked = true
        _refreshModeration()
        _refreshIgnored()
        open()
    }

    // v0.7.x account-wide ignore (m.ignored_user_list). Recomputed on the
    // controller's revision because a plain binding cannot observe a
    // Q_INVOKABLE's inputs; remote changes land while the popover is open.
    property bool userIgnored: false
    property string ignoreNotice: ""
    property bool ignoreNoticeError: false
    function _refreshIgnored() {
        userIgnored = !isOwn && app.moderation.supported
                      && userId.length > 0
                      && app.moderation.isIgnored(userId)
    }
    Connections {
        target: app.moderation
        function onStateChanged() { root._refreshIgnored() }
        function onIgnoreActionFinished(userId, ignored, ok, message) {
            if (userId !== root.userId)
                return
            root.ignoreNotice = message
            root.ignoreNoticeError = !ok
            root._refreshIgnored()
        }
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

    // v0.7.x Matrix presence status line. Pure reads re-evaluated on
    // PresenceManager's revision; unknown presence yields "" and the label
    // collapses — never a placeholder.
    readonly property string presenceLine: {
        if (!root.opened || root.userId === "" || !app.presence)
            return ""
        var rev = app.presence.revision // re-evaluation dependency
        var info = app.presence.infoFor(root.userId)
        if (!info || info.state === undefined)
            return ""
        if (info.state === "online")
            return qsTr("Online")
        if (info.state === "unavailable")
            return qsTr("Away")
        if (info.state !== "offline")
            return ""
        var ago = info.lastActiveAgoMs
        if (ago === undefined || ago < 0)
            return qsTr("Offline")
        var mins = Math.floor(ago / 60000)
        if (mins < 1)
            return qsTr("Offline — active just now")
        if (mins < 60)
            return qsTr("Offline — active %1 min ago").arg(mins)
        var hours = Math.floor(mins / 60)
        if (hours < 24)
            return qsTr("Offline — active %1 h ago").arg(hours)
        return qsTr("Offline — active %1 d ago").arg(Math.floor(hours / 24))
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
            // Storm chrome (SPEC-storm-language §3.1 / mock 2g).
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
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
                // Storm 2g banner: stormSelection → stormPanel at 120°.
                property color c1: AppTheme.stormSelection
                property color c2: AppTheme.stormPanel
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

            // Storm §3.6 corner watermark: ONE oversized outline bolt
            // overflowing the banner's top-right, cropped by its own
            // banner-sized clip container (nothing else in this popover
            // clips; the mock's overflow:hidden lives here). Painted after
            // the Canvas, before the avatar.
            Item {
                anchors.fill: banner
                clip: true
                Icon {
                    name: "bolt"
                    size: 80
                    color: AppTheme.stormWatermark
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.topMargin: -16
                    anchors.rightMargin: -18
                }
            }

            // 56px avatar overlapping the banner's bottom-left, with a 3px
            // panel ring (a solid backing disc slightly larger than the
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
                    color: AppTheme.stormPanel
                }
                Avatar {
                    anchors.centerIn: parent
                    size: 56
                    mxc: root.avatarMxc
                    name: root.visibleName
                    colorKey: root.userId
                }
                // v0.7.x Matrix presence. Watched only while the popover
                // is open (the userId gate), so a closed popover holds no
                // polling reference.
                PresenceDot {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 2
                    dotSize: 12
                    ring: AppTheme.stormPanel
                    userId: root.opened ? root.userId : ""
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
                color: AppTheme.stormText
                font.family: AppTheme.menuFont
                font.pixelSize: 16
                font.weight: Font.Bold
                elide: Label.ElideRight
            }
            Label {
                Layout.fillWidth: true
                text: root.userId
                color: AppTheme.stormTextMuted
                font.family: AppTheme.monoFont
                font.pixelSize: AppTheme.fontMonoXS
                elide: Label.ElideMiddle
            }
            Label {
                Layout.fillWidth: true
                visible: root.presenceLine.length > 0
                text: root.presenceLine
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.fontSecondary
                elide: Label.ElideRight
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                Label {
                    visible: root.membership.length > 0
                    text: root.membership === "invited" ? qsTr("Invited")
                        : root.membership === "joined" ? qsTr("Member")
                        : root.membership === "banned" ? qsTr("Banned")
                        : root.membership
                    // Reading text on the panel rides the muted ink —
                    // faint stays reserved for decorative mono (AA note in
                    // AppTheme).
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.fontSecondary
                }
                // Outline chips (§1 yellow discipline): the bolt fill on
                // this surface belongs to the Message primary alone.
                StatusChip {
                    visible: root.role === "administrator" || root.role === "creator"
                    storm: true
                    tone: "neutral"
                    label: qsTr("Administrator")
                }
                StatusChip {
                    visible: root.role === "moderator"
                    storm: true
                    tone: "neutral"
                    label: qsTr("Moderator")
                }
                Label {
                    visible: root.isOwn
                    text: qsTr("(you)")
                    color: AppTheme.stormTextMuted
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
                    // Storm §3.9 primary: THE one bolt fill on this surface.
                    // boltInk is the ink on that fill (Storm: deep canvas
                    // navy; legacy: accentText) — stays readable once bolt
                    // routes to each legacy theme's own accent.
                    contentItem: RowLayout {
                        spacing: AppTheme.spacing6
                        Item { Layout.fillWidth: true }
                        Icon { name: "chat_bubble"; size: 16; color: AppTheme.boltInk }
                        Label {
                            text: qsTr("Message")
                            color: AppTheme.boltInk
                            font.family: AppTheme.menuFont
                            font.pixelSize: 13
                            font.weight: Font.Bold
                        }
                        Item { Layout.fillWidth: true }
                    }
                    background: Rectangle {
                        radius: AppTheme.radiusTile
                        color: !messageButton.enabled ? AppTheme.stormInset
                               : messageButton.down ? Qt.darker(AppTheme.bolt, 1.12)
                               : messageButton.hovered ? Qt.darker(AppTheme.bolt, 1.05)
                               : AppTheme.bolt
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -4
                        radius: AppTheme.radiusTile + 4
                        color: "transparent"
                        border.width: 2
                        border.color: AppTheme.bolt
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
                    // Storm §3.9 secondary: stormBorderStrong outline,
                    // stormTextSecondary ink (mock 2g outline squares).
                    contentItem: RowLayout {
                        id: copyIdContent
                        spacing: AppTheme.spacing6
                        Item { Layout.fillWidth: true }
                        Icon {
                            name: "content_copy"
                            size: 16
                            color: AppTheme.stormTextSecondary
                        }
                        Label {
                            visible: copyIdButton.expanded
                            text: copyIdButton.text
                            color: AppTheme.stormTextSecondary
                            font.family: AppTheme.menuFont
                            font.pixelSize: 13
                            font.weight: Font.Bold
                        }
                        Item { Layout.fillWidth: true }
                    }
                    background: Rectangle {
                        radius: AppTheme.radiusTile
                        color: (copyIdButton.hovered || copyIdButton.down)
                               ? AppTheme.stormSelection : "transparent"
                        border.width: 1
                        border.color: AppTheme.stormBorderStrong
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -4
                        radius: AppTheme.radiusTile + 4
                        color: "transparent"
                        border.width: 2
                        border.color: AppTheme.bolt
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
            // --- Role / power level (v0.7.x) ---
            // The label always shows the member's REAL level: a room using
            // a value that is not one of the conventional presets renders
            // as "Custom (N)", never rounded into a preset. The buttons
            // offer only levels the viewer may actually set, and a
            // rejection from the server surfaces here rather than being
            // swallowed by an optimistic repaint.
            ColumnLayout {
                objectName: "profileRoleBlock"
                visible: root.roleLabel.length > 0 && root.modAction === ""
                Layout.fillWidth: true
                spacing: AppTheme.spacing6

                RowLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing6
                    Label {
                        text: qsTr("Role")
                        color: AppTheme.stormTextMuted
                        font.pixelSize: 11
                        font.weight: Font.Bold
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.roleLabel
                        color: AppTheme.stormText
                        font.pixelSize: AppTheme.fontSecondary
                        font.weight: Font.Bold
                        elide: Label.ElideRight
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing8
                    visible: root.roleOptions.length > 0

                    Repeater {
                        model: root.roleOptions
                        delegate: AbstractButton {
                            id: roleButton
                            required property var modelData
                            objectName: "profileRoleButton_"
                                        + modelData.level
                            Layout.fillWidth: true
                            implicitHeight: 30
                            hoverEnabled: true
                            focusPolicy: Qt.TabFocus
                            enabled: app.roomInfo
                                     && !app.roomInfo.powerLevelPending
                            Accessible.role: Accessible.Button
                            Accessible.name:
                                qsTr("Set %1 to %2")
                                    .arg(root.visibleName)
                                    .arg(modelData.label)
                            contentItem: Label {
                                text: roleButton.modelData.label
                                horizontalAlignment: Text.AlignHCenter
                                // A bare Label contentItem fills the button,
                                // and Text defaults to top alignment — the
                                // label rides high without this.
                                verticalAlignment: Text.AlignVCenter
                                color: roleButton.enabled
                                       ? AppTheme.stormTextSecondary
                                       : AppTheme.stormTextFaint
                                font.family: AppTheme.menuFont
                                font.pixelSize: 12
                                font.weight: Font.Bold
                                elide: Label.ElideRight
                            }
                            background: Rectangle {
                                radius: AppTheme.radiusTile
                                color: (roleButton.hovered || roleButton.down)
                                       ? AppTheme.stormSelection : "transparent"
                                border.width: 1
                                border.color: AppTheme.stormBorderStrong
                            }
                            onClicked: {
                                root.roleError = ""
                                app.roomInfo.setMemberPowerLevel(
                                    root.userId, modelData.level)
                            }
                        }
                    }
                }

                Label {
                    visible: root.roleError.length > 0
                    Layout.fillWidth: true
                    text: root.roleError
                    color: AppTheme.danger
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                    Accessible.name: text
                }
            }

            // --- Moderation row (kick / ban / unban) ---
            // Kick/ban are outline-danger secondaries; unban is an
            // ordinary outline secondary (it restores access, it does not
            // remove it). The bolt fill stays reserved for the Message
            // primary. Hidden entirely when the viewer lacks the power
            // (or it cannot be established) — never a disabled
            // placeholder.
            RowLayout {
                visible: (root.showKick || root.showBan || root.showUnban)
                         && root.modAction === ""
                Layout.fillWidth: true
                spacing: AppTheme.spacing8

                Repeater {
                    model: [
                        { op: "kick", label: qsTr("Remove"),
                          icon: "person_remove", danger: true,
                          show: root.showKick },
                        { op: "ban", label: qsTr("Ban"),
                          icon: "block", danger: true,
                          show: root.showBan },
                        { op: "unban", label: qsTr("Unban"),
                          icon: "undo", danger: false,
                          show: root.showUnban }
                    ]
                    delegate: AbstractButton {
                        id: modButton
                        required property var modelData
                        readonly property color modInk:
                            modelData.danger ? AppTheme.danger
                                             : AppTheme.stormTextSecondary
                        objectName: "profileModButton_" + modelData.op
                        visible: modelData.show
                        Layout.fillWidth: true
                        implicitHeight: 32
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.role: Accessible.Button
                        Accessible.name: modelData.op === "kick"
                            ? qsTr("Remove %1 from the room").arg(root.visibleName)
                            : modelData.op === "ban"
                              ? qsTr("Ban %1 from the room").arg(root.visibleName)
                              : qsTr("Unban %1").arg(root.visibleName)
                        contentItem: RowLayout {
                            spacing: AppTheme.spacing6
                            Item { Layout.fillWidth: true }
                            Icon {
                                name: modButton.modelData.icon
                                size: 16
                                color: modButton.modInk
                            }
                            Label {
                                text: modButton.modelData.label
                                color: modButton.modInk
                                font.family: AppTheme.menuFont
                                font.pixelSize: 13
                                font.weight: Font.Bold
                            }
                            Item { Layout.fillWidth: true }
                        }
                        background: Rectangle {
                            radius: AppTheme.radiusTile
                            color: (modButton.hovered || modButton.down)
                                   ? AppTheme.stormSelection : "transparent"
                            border.width: 1
                            border.color: modButton.modelData.danger
                                          ? AppTheme.danger
                                          : AppTheme.stormBorderStrong
                        }
                        onClicked: {
                            root.modError = ""
                            root.modAction = modelData.op
                        }
                    }
                }
            }

            // v0.7.x: account-wide ignore. Visually separate from the
            // room-scoped moderation row above — ignoring needs no power
            // level and applies to every room. One click, reversible.
            ColumnLayout {
                visible: !root.isOwn && app.moderation.supported
                         && root.modAction === ""
                Layout.fillWidth: true
                spacing: AppTheme.spacing6

                AbstractButton {
                    id: ignoreButton
                    objectName: "profileIgnoreButton"
                    Layout.fillWidth: true
                    implicitHeight: 32
                    hoverEnabled: true
                    focusPolicy: Qt.TabFocus
                    enabled: !app.moderation.busy
                    Accessible.role: Accessible.Button
                    Accessible.name: root.userIgnored
                        ? qsTr("Stop ignoring %1").arg(root.visibleName)
                        : qsTr("Ignore %1 everywhere").arg(root.visibleName)
                    contentItem: RowLayout {
                        spacing: AppTheme.spacing6
                        Item { Layout.fillWidth: true }
                        Icon {
                            name: root.userIgnored ? "visibility" : "visibility_off"
                            size: 16
                            color: root.userIgnored ? AppTheme.stormTextSecondary
                                                    : AppTheme.danger
                        }
                        Label {
                            text: root.userIgnored ? qsTr("Stop ignoring")
                                                   : qsTr("Ignore")
                            color: root.userIgnored ? AppTheme.stormTextSecondary
                                                    : AppTheme.danger
                            font.family: AppTheme.menuFont
                            font.pixelSize: 13
                            font.weight: Font.Bold
                        }
                        Item { Layout.fillWidth: true }
                    }
                    background: Rectangle {
                        radius: AppTheme.radiusTile
                        color: (ignoreButton.hovered || ignoreButton.down)
                               ? AppTheme.stormSelection : "transparent"
                        border.width: 1
                        border.color: root.userIgnored
                                      ? AppTheme.stormBorderStrong
                                      : AppTheme.danger
                    }
                    onClicked: {
                        root.ignoreNotice = ""
                        if (root.userIgnored)
                            app.moderation.unignoreUser(root.userId)
                        else
                            app.moderation.ignoreUser(root.userId)
                    }
                }
                Label {
                    visible: root.ignoreNotice.length > 0
                    Layout.fillWidth: true
                    text: root.ignoreNotice
                    color: root.ignoreNoticeError ? AppTheme.danger
                                                  : AppTheme.stormTextMuted
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                    Accessible.name: text
                }
            }

            // Inline confirm surface: reason (optional) + Confirm/Cancel.
            ColumnLayout {
                visible: root.modAction !== ""
                Layout.fillWidth: true
                spacing: AppTheme.spacing6

                Label {
                    Layout.fillWidth: true
                    text: root.modAction === "kick"
                          ? qsTr("Remove %1 from this room?").arg(root.visibleName)
                          : root.modAction === "ban"
                            ? qsTr("Ban %1 from this room?").arg(root.visibleName)
                            : qsTr("Unban %1? They will be able to join again.")
                                  .arg(root.visibleName)
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.fontSecondary
                    font.weight: Font.Bold
                    wrapMode: Text.Wrap
                }
                AppTextField {
                    id: modReasonField
                    Layout.fillWidth: true
                    storm: true
                    placeholderText: qsTr("Reason (optional)")
                }
                RowLayout {
                    visible: root.modAction === "unban"
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing8
                    AppSwitch {
                        checked: root.inviteBackChecked
                        onToggled: root.inviteBackChecked
                                   = !root.inviteBackChecked
                        Accessible.name: qsTr("Invite back after unbanning")
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Invite back after unbanning")
                        color: AppTheme.stormTextSecondary
                        font.pixelSize: AppTheme.fontSecondary
                        wrapMode: Text.Wrap
                        TapHandler {
                            onTapped: root.inviteBackChecked
                                      = !root.inviteBackChecked
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing8
                    AppButton {
                        objectName: "profileModConfirmButton"
                        Layout.fillWidth: true
                        text: root.modAction === "kick" ? qsTr("Remove")
                            : root.modAction === "ban" ? qsTr("Ban")
                            : qsTr("Unban")
                        kind: root.modAction === "unban" ? "secondary"
                                                         : "danger"
                        storm: true
                        enabled: app.roomInfo
                                 && !app.roomInfo.moderationPending
                        onClicked: {
                            root.modError = ""
                            if (root.modAction === "kick")
                                app.roomInfo.kickMember(root.userId,
                                                        modReasonField.text)
                            else if (root.modAction === "ban")
                                app.roomInfo.banMember(root.userId,
                                                       modReasonField.text)
                            else
                                app.roomInfo.unbanMember(
                                    root.userId, modReasonField.text,
                                    root.inviteBackChecked)
                        }
                    }
                    AppButton {
                        Layout.fillWidth: true
                        text: qsTr("Cancel")
                        storm: true
                        enabled: app.roomInfo
                                 && !app.roomInfo.moderationPending
                        onClicked: root.modAction = ""
                    }
                }
                Label {
                    visible: root.modError.length > 0
                    Layout.fillWidth: true
                    text: root.modError
                    color: AppTheme.danger
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                    Accessible.name: text
                }
            }

            Connections {
                target: app.roomInfo
                enabled: root.visible
                function onModerationActionFinished(roomId, userId, op, ok,
                                                    message) {
                    if (userId !== root.userId)
                        return
                    if (ok)
                        root.close()
                    else
                        root.modError = message
                }
                // A roster refresh while open (e.g. after a successful
                // action elsewhere) can change what may be offered.
                function onMembersChanged() {
                    root._refreshModeration()
                }
                // v0.7.x: a power-level write finished. The authoritative
                // level arrives with the roster refresh the controller
                // triggers; this only reports a failure. The popover stays
                // OPEN on success — unlike kick/ban, the member is still
                // here and the user may well want to see the new role.
                function onPowerLevelActionFinished(roomId, userId, level, ok,
                                                    message) {
                    if (userId !== root.userId)
                        return
                    root.roleError = ok ? "" : message
                }
            }

            Label {
                id: copiedNotice
                visible: false
                text: qsTr("Matrix ID copied")
                color: AppTheme.stormTextMuted
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
