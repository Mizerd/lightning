import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

Popup {
    id: root
    modal: false
    dim: false
    width: 340
    padding: 0

    readonly property int anchorGap: 8
    readonly property int edgeMargin: 8
    property var anchorRect: null
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    // A popup sees Escape only while it holds focus, and nothing on the card
    // takes it when it opens.
    focus: true

    function _rectOf(anchor) {
        if (!anchor)
            return null
        var ov = root.parent
        if (anchor.mapToItem !== undefined && ov) {
            var tl = anchor.mapToItem(ov, 0, 0)
            return { x: tl.x, y: tl.y, width: anchor.width,
                     height: anchor.height }
        }
        if (anchor.x !== undefined && anchor.y !== undefined)
            return { x: anchor.x, y: anchor.y, width: anchor.width || 1,
                     height: anchor.height || 1 }
        return null
    }

    function place() {
        var ov = root.parent
        if (!ov)
            return
        var W = ov.width, H = ov.height
        var m = root.edgeMargin
        var w = root.width, h = root.height
        var r = root.anchorRect
        var nx, ny
        if (!r) {
            nx = (W - w) / 2
            ny = (H - h) / 2
        } else {
            if (r.x + r.width + root.anchorGap + w <= W - m)
                nx = r.x + r.width + root.anchorGap
            else if (r.x - root.anchorGap - w >= m)
                nx = r.x - root.anchorGap - w
            else
                nx = r.x + r.width / 2 - w / 2
            ny = r.y
        }
        root.x = Math.round(Math.max(m, Math.min(nx, W - w - m)))
        root.y = Math.round(Math.max(m, Math.min(ny, H - h - m)))
    }
    onHeightChanged: if (visible) place()
    Connections {
        target: root.parent
        function onWidthChanged() { if (root.visible) root.place() }
        function onHeightChanged() { if (root.visible) root.place() }
    }

    property string userId: ""
    property string displayName: ""
    property string membership: ""
    property string role: ""
    property string avatarMxc: ""
    property bool isOwn: false
    // False when opened outside a joined room (an invite's inviter): no
    // roster, role or moderation for whichever room happens to be current.
    property bool roomScoped: true
    // MSC4278 invite_avatars off: a stranger's picture and banner stay unloaded.
    property bool withholdMedia: false

    property string modAction: ""
    property string modError: ""
    property bool inviteBackChecked: true
    property bool showKick: false
    property bool showBan: false
    property bool showUnban: false

    property string roleLabel: ""
    property var roleOptions: []
    property string roleError: ""
    property string liveRole: ""
    readonly property string badgeRole: liveRole.length > 0 ? liveRole : role

    function _refreshModeration() {
        var scoped = root.roomScoped && app.roomInfo
                     && app.roomInfo.roomId === app.currentRoomId
                     && userId.length > 0
        showKick = scoped ? app.roomInfo.canModerate(userId, "kick") : false
        showBan = scoped ? app.roomInfo.canModerate(userId, "ban") : false
        showUnban = scoped ? app.roomInfo.canModerate(userId, "unban") : false
        _refreshRole(scoped)
    }

    function _roleForLevel(level) {
        return level >= 9007199254740992 ? "creator"
               : level >= 100 ? "administrator"
               : level >= 50 ? "moderator" : "member"
    }

    function _refreshRole(scoped) {
        if (!scoped) {
            roleLabel = ""
            roleOptions = []
            liveRole = ""
            return
        }
        if (membership === "banned" || membership === "left") {
            roleLabel = ""
            roleOptions = []
            liveRole = ""
            return
        }
        var current = app.roomInfo.powerLevelFor(userId)
        liveRole = _roleForLevel(current)
        roleLabel = app.roomInfo.roleLabelForLevel(current)
        var candidates = [app.roomInfo.usersDefaultPowerLevel, 50, 100]
        var seen = {}
        var out = []
        for (var i = 0; i < candidates.length; ++i) {
            var level = candidates[i]
            if (seen[level] === true)
                continue
            seen[level] = true
            if (!app.roomInfo.canSetPowerLevel(userId, level))
                continue
            out.push({ level: level,
                       label: app.roomInfo.roleLabelForLevel(level) })
        }
        roleOptions = out
    }

    readonly property string visibleName:
        hasVisibleText(displayName)
        ? displayName
        : (userId.length > 1
           ? userId.slice(1).split(":")[0] : userId)

    function hasVisibleText(text) {
        if (!text || text.length === 0)
            return false
        var stripped = text
            .replace(/[\s\u00AD\u034F\u061C\u115F\u1160\u17B4\u17B5\u180B-\u180F\u200B-\u200F\u202A-\u202E\u2060-\u206F\u2800\u3164\uFE00-\uFE0F\uFEFF\uFFA0\uFFF9-\uFFFB]/g, "")
            .replace(/\uDB40[\uDC00-\uDDEF]/g, "")
        return stripped.length > 0
    }

    function _fillFromRoster() {
        if (userId.length === 0)
            return
        if (displayName.length > 0 && avatarMxc.length > 0
            && membership.length > 0)
            return
        if (!app.roomInfo)
            return
        var row = app.roomInfo.memberFor(userId, app.currentRoomId)
        if (!row || !row.userId)
            return
        if (displayName.length === 0)
            displayName = row.displayName || ""
        if (avatarMxc.length === 0)
            avatarMxc = row.avatarUrl || ""
        if (membership.length === 0)
            membership = row.membership || ""
    }

    function _fillFromServer() {
        if (userId.length === 0 || typeof app === "undefined" || !app.userProfiles)
            return
        if (displayName.length > 0 && avatarMxc.length > 0)
            return
        var p = app.userProfiles.lookup(userId)
        if (!p || !p.known)
            return
        if (displayName.length === 0)
            displayName = p.displayName || ""
        if (avatarMxc.length === 0)
            avatarMxc = p.avatarUrl || ""
    }
    Connections {
        target: (typeof app !== "undefined" && app.userProfiles)
                ? app.userProfiles : null
        function onResolved(uid, name, avatar) {
            if (uid !== root.userId || !root.opened)
                return
            if (root.displayName.length === 0)
                root.displayName = name || ""
            if (root.avatarMxc.length === 0)
                root.avatarMxc = avatar || ""
        }
    }

    function openFor(member, anchor) {
        anchorRect = _rectOf(anchor)
        userId = member.userId || ""
        displayName = member.displayName || ""
        membership = member.membership || ""
        role = member.role || ""
        liveRole = ""
        avatarMxc = member.avatarUrl || ""
        isOwn = member.isOwn === true
        roomScoped = member.roomScoped !== false
        withholdMedia = member.withholdMedia === true
        if (roomScoped)
            _fillFromRoster()
        _fillFromServer()
        modAction = ""
        modError = ""
        roleError = ""
        ignoreNotice = ""
        modReasonField.text = ""
        inviteBackChecked = true
        policyReason = ""
        policyMatched = false
        if (roomScoped && app.roomInfo && app.currentRoomId.length > 0
            && app.roomInfo.roomId !== app.currentRoomId)
            app.roomInfo.roomId = app.currentRoomId
        _refreshModeration()
        _refreshIgnored()
        place()
        open()
        place()
        // The tap that opened the card can also reach a TapHandler above its
        // caller (the timeline's takes focus for keyboard scrolling), after
        // this one: take focus once that tap is delivered, or Escape goes there.
        Qt.callLater(root._takeFocus)
    }
    function _takeFocus() {
        if (root.visible)
            root.forceActiveFocus(Qt.PopupFocusReason)
    }

    function openForAction(member, op, anchor) {
        openFor(member, anchor)
        if ((op === "kick" && showKick) || (op === "ban" && showBan)
                || (op === "unban" && showUnban))
            modAction = op
    }

    property bool policyMatched: false
    property string policyReason: ""

    property bool userIgnored: false
    property string ignoreNotice: ""
    property bool ignoreNoticeError: false
    function _refreshIgnored() {
        userIgnored = !isOwn && app.moderation.supported
                      && userId.length > 0
                      && app.moderation.isIgnored(userId)
    }
    Connections {
        target: app.policy
        function onCheckFinished(entity, matched, detail) {
            if (entity !== root.userId)
                return
            root.policyMatched = matched
            root.policyReason = matched && detail && detail.reason
                                ? detail.reason : ""
        }
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
        app.conversations.checkExistingDm(userId)
        var existing = app.conversations.existingDms
        if (existing.length > 0) {
            app.openRoom(existing[0].roomId)
            app.requestComposerFocus(existing[0].roomId)
        } else
            app.conversations.startDirectMessageAnyway(userId)
        close()
    }

    readonly property bool presenceUnavailable:
        root.opened && root.userId !== "" && app.presence
            ? app.presence.unavailable : false

    readonly property string homeserver: {
        var i = root.userId.indexOf(":")
        return i >= 0 && i + 1 < root.userId.length
               ? root.userId.slice(i + 1) : ""
    }

    readonly property string membershipLabel:
        root.membership === "invited" ? qsTr("Invited")
        : root.membership === "joined" ? qsTr("Member")
        : root.membership === "banned" ? qsTr("Banned")
        : root.membership

    readonly property string bioText: {
        if (!root.opened || root.userId === "" || !app.bio)
            return ""
        var rev = app.bio.revision
        return app.bio.bioFor(root.userId)
    }

    readonly property string badgeLabel:
        app.badges && root.userId !== ""
            ? app.badges.labelFor(root.userId) : ""
    readonly property string badgeDescription:
        app.badges && root.userId !== ""
            ? app.badges.descriptionFor(root.userId) : ""

    function _copy(text, notice) {
        if (!text || text.length === 0)
            return
        idClipboard.text = text
        idClipboard.selectAll()
        idClipboard.copy()
        idClipboard.text = ""
        copiedNotice.text = notice
        copiedNotice.visible = true
        copiedTimer.restart()
    }

    function copyUserId() {
        root._copy(root.userId, qsTr("Matrix ID copied"))
    }

    function copyProfileLink() {
        if (root.userId.length === 0)
            return
        root._copy("https://matrix.to/#/" + encodeURIComponent(root.userId),
                   qsTr("Profile link copied"))
    }

    component ProfileChipButton: AbstractButton {
        id: chipButton
        property string iconName: ""
        property string label: ""
        property string accessibleName: ""
        implicitWidth: chipRow.implicitWidth + 2 * AppTheme.chipPaddingH
        implicitHeight: AppTheme.chipHeight + 6
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        Accessible.role: Accessible.Button
        Accessible.name: accessibleName.length > 0 ? accessibleName : label
        Keys.onReturnPressed: (event) => { event.accepted = true; if (!event.isAutoRepeat) clicked() }
        Keys.onEnterPressed: (event) => { event.accepted = true; if (!event.isAutoRepeat) clicked() }
        ToolTip.text: accessibleName
        ToolTip.visible: hovered && label.length === 0 && accessibleName.length > 0
        ToolTip.delay: 500
        contentItem: Item {
            implicitWidth: chipRow.implicitWidth
            implicitHeight: chipRow.implicitHeight
            Row {
                id: chipRow
                anchors.centerIn: parent
                spacing: AppTheme.spacing2
            Icon {
                visible: chipButton.iconName.length > 0
                name: chipButton.iconName
                size: AppTheme.textMicro + 3
                color: AppTheme.stormLink
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                // Untrusted text: never markup.
                textFormat: Text.PlainText
                visible: chipButton.label.length > 0
                text: chipButton.label
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textMicro
                font.weight: AppTheme.weightBold
                color: AppTheme.stormLink
                anchors.verticalCenter: parent.verticalCenter
                }
            }
        }
        background: Rectangle {
            radius: AppTheme.chipRadius
            color: (chipButton.hovered || chipButton.down)
                   ? Qt.alpha(AppTheme.stormLink, 0.24)
                   : Qt.alpha(AppTheme.stormLink, 0.14)
            border.width: 1
            border.color: chipButton.visualFocus
                          ? AppTheme.focusRing
                          : Qt.alpha(AppTheme.stormLink, 0.32)
        }
    }

    background: Item {
        Rectangle {
            id: popoverBackground
            anchors.fill: parent
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

    onOpenedChanged: {
        if (opened && contentItem)
            contentItem.forceActiveFocus()
        if (!opened || userId.length === 0)
            return
        if (app.banners && !withholdMedia)
            app.banners.request(userId)
        if (app.bio)
            app.bio.request(userId)
        if (app.policy && app.policy.available && !isOwn)
            app.policy.check("user", userId)
    }

    contentItem: ColumnLayout {
        spacing: 0
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Profile for %1").arg(root.visibleName)

        Item {
            id: header
            Layout.fillWidth: true
            implicitHeight: Math.max(banner.height + 28,
                                     avatarWrap.y + avatarWrap.height)

            Canvas {
                id: banner
                objectName: "profileBanner"
                antialiasing: true
                readonly property int frame: popoverBackground.border.width
                x: frame
                y: frame
                width: parent.width - 2 * frame
                height: Math.round(width / 3)
                property color c1: AppTheme.stormSelection
                property color c2: AppTheme.stormPanel
                property int cornerRadius: Math.max(0, AppTheme.radiusLg - frame)
                // The corner mask is a MultiEffect over a layer, and the
                // software scene graph draws neither: there the layered Item
                // below renders as nothing at all, banner included. Without a
                // mask the picture is clipped square instead.
                property bool maskable:
                    banner.GraphicsInfo.api !== GraphicsInfo.Software
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
                    var angle = 120 * Math.PI / 180
                    var dx = Math.sin(angle)
                    var dy = -Math.cos(angle)
                    var length = Math.abs(w * dx) + Math.abs(h * dy)
                    var cx = w / 2, cy = h / 2
                    var grad = ctx.createLinearGradient(
                        cx - dx * length / 2, cy - dy * length / 2,
                        cx + dx * length / 2, cy + dy * length / 2)
                    grad.addColorStop(0, c1)
                    grad.addColorStop(0.7, c2)
                    ctx.fillStyle = grad
                    ctx.fill()
                }
            }

            Item {
                id: bannerCornerMask
                anchors.fill: banner
                visible: false
                layer.enabled: banner.maskable
                Rectangle {
                    anchors.fill: parent
                    radius: banner.cornerRadius
                    color: "white"
                }
                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: banner.cornerRadius
                    color: "white"
                }
            }

            Item {
                objectName: "profileBannerClip"
                anchors.fill: banner
                clip: true
                layer.enabled: banner.maskable
                layer.effect: MultiEffect {
                    maskEnabled: true
                    maskSource: bannerCornerMask
                    maskThresholdMin: 0.5
                    maskSpreadAtMin: 1.0
                    maskThresholdMax: 1.0
                    maskSpreadAtMax: 0.0
                }
                Image {
                    id: bannerImage
                    objectName: "profileBannerImage"
                    anchors.fill: parent
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    cache: true
                    visible: status === Image.Ready
                    opacity: bannerMotion.shown ? 0 : 1
                    readonly property string mxc: {
                        if (!root.opened || !app.banners || root.withholdMedia)
                            return ""
                        var _dep = app.banners.revision
                        return app.banners.bannerFor(root.userId)
                    }
                    // Re-resolve counter: wideImageSource() returns "" on a
                    // cache miss and nothing else here changes when the bytes
                    // land. A counter rather than assigning `source`, which
                    // would destroy the binding and keep the first banner ever
                    // loaded.
                    property int resolveTick: 0
                    source: {
                        var _tick = resolveTick
                        return mxc.length > 0 && app.mediaBridge.supported
                               ? app.mediaBridge.wideImageSource(mxc) : ""
                    }
                    Connections {
                        target: app.mediaBridge
                        enabled: bannerImage.mxc.length > 0
                        function onMediaCached(key) {
                            if (key.endsWith(":" + bannerImage.mxc)
                                && bannerImage.source.toString().length === 0)
                                bannerImage.resolveTick++
                        }
                        function onMediaRetryable(key) {
                            if (key.endsWith(":" + bannerImage.mxc)
                                && bannerImage.source.toString().length === 0)
                                bannerImage.resolveTick++
                        }
                    }
                }
                BannerMotion {
                    id: bannerMotion
                    objectName: "profileBannerMotion"
                    anchors.fill: parent
                    mxc: bannerImage.mxc
                    stillReady: bannerImage.status === Image.Ready
                }
            }

            Item {
                anchors.fill: banner
                clip: true
                visible: !bannerImage.visible
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

            Item {
                id: avatarWrap
                x: AppTheme.spacing16
                y: banner.height - 28
                width: 60
                height: 60
                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color: AppTheme.stormPanel
                }
                Avatar {
                    objectName: "profileCardAvatar"
                    anchors.centerIn: parent
                    size: 56
                    mxc: root.withholdMedia ? "" : root.avatarMxc
                    name: root.visibleName
                    colorKey: root.userId
                    prominent: true
                    AvatarViewAction {}
                }
                PresenceDot {
                    id: avatarPresenceDot
                    objectName: "profileAvatarPresenceDot"
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 2
                    dotSize: 12
                    ring: AppTheme.stormPanel
                    userId: root.opened ? root.userId : ""
                    hoverStatus: false
                    HoverHandler {
                        id: avatarDotHover
                        enabled: avatarPresenceDot.visible
                    }
                    Timer {
                        id: avatarTipDelay
                        interval: 300
                        repeat: false
                        running: avatarDotHover.hovered
                    }
                    Rectangle {
                        id: avatarPresenceTip
                        objectName: "profileAvatarPresenceTip"
                        parent: avatarWrap
                        x: avatarPresenceDot.x + avatarPresenceDot.width
                           + AppTheme.spacing4
                        y: Math.round(avatarPresenceDot.y
                                      + (avatarPresenceDot.height - height) / 2)
                        z: 20
                        visible: avatarDotHover.hovered && !avatarTipDelay.running
                                 && (avatarPresenceDot.statusText.length > 0
                                     || root.presenceUnavailable)
                        implicitWidth: avatarPresenceTipLabel.implicitWidth
                                       + 2 * AppTheme.spacing8
                        implicitHeight: avatarPresenceTipLabel.implicitHeight
                                        + 2 * AppTheme.spacing4
                        radius: AppTheme.radiusSm
                        color: AppTheme.stormPanel
                        border.width: 1
                        border.color: AppTheme.stormBorder
                        Label {
                            id: avatarPresenceTipLabel
                            anchors.centerIn: parent
                            text: avatarPresenceDot.statusText.length > 0
                                  ? avatarPresenceDot.statusText
                                  : qsTr("Presence unavailable")
                            textFormat: Text.PlainText
                            color: AppTheme.textPrimary
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                        }
                    }
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
                objectName: "profileStatusMessage"
                Layout.fillWidth: true
                readonly property string statusMsg: {
                    if (!root.opened || root.userId === "" || !app.presence)
                        return ""
                    var rev = app.presence.revision
                    return app.presence.statusMessageFor(root.userId)
                }
                visible: statusMsg.length > 0
                text: statusMsg
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
                color: AppTheme.stormTextSecondary
                font: app.textFontWithEmoji(AppTheme.uiFont, AppTheme.scaled(13))
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing2

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing6

                        Label {
                            textFormat: Text.PlainText
                            objectName: "profileDisplayName"
                            Layout.fillWidth: true
                            text: root.visibleName
                            color: AppTheme.userColor(root.userId)
                            font.family: AppTheme.menuFont
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
                            elide: Label.ElideRight
                            HoverHandler { id: nameHover }
                            ToolTip.text: root.visibleName
                            ToolTip.visible: nameHover.hovered && truncated
                            ToolTip.delay: 500
                        }

                        Loader {
                            active: root.badgeLabel.length > 0
                            visible: active
                            Layout.maximumWidth: AppTheme.scaled(160)
                            sourceComponent: Rectangle {
                                readonly property color ink:
                                    AppTheme.userColor(root.userId)
                                implicitWidth: Math.min(
                                    badgeText.implicitWidth + 2 * AppTheme.spacing6,
                                    AppTheme.scaled(160))
                                implicitHeight: Math.max(
                                    AppTheme.chipHeight,
                                    badgeText.implicitHeight + 2 * AppTheme.spacing2)
                                radius: AppTheme.chipRadius
                                color: Qt.alpha(ink, 0.14)
                                border.width: 1
                                border.color: Qt.alpha(ink, 0.32)
                                Label {
                                    id: badgeText
                                    anchors.centerIn: parent
                                    width: Math.min(implicitWidth,
                                                    parent.width - 2 * AppTheme.spacing6)
                                    elide: Label.ElideRight
                                    horizontalAlignment: Text.AlignHCenter
                                    text: root.badgeLabel
                                    color: parent.ink
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMicro
                                    font.weight: AppTheme.weightStrong
                                    textFormat: Text.PlainText
                                }
                                Accessible.role: Accessible.StaticText
                                Accessible.name: root.badgeLabel
                                Accessible.description: root.badgeDescription
                                ToolTip.text: root.badgeDescription
                                ToolTip.visible: badgeHover.hovered
                                                 && root.badgeDescription.length > 0
                                ToolTip.delay: 300
                                HoverHandler { id: badgeHover }
                            }
                        }
                    }

                    Label {
                        textFormat: Text.PlainText
                        objectName: "profileUserId"
                        Layout.fillWidth: true
                        text: root.userId
                        color: AppTheme.stormTextMuted
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.fontMonoXS
                        wrapMode: Text.WrapAnywhere
                        maximumLineCount: 2
                        elide: Label.ElideRight
                        MouseArea {
                            objectName: "profileUserIdCopyArea"
                            anchors.fill: parent
                            acceptedButtons: Qt.LeftButton
                            cursorShape: Qt.PointingHandCursor
                            onDoubleClicked: root.copyUserId()
                        }
                        HoverHandler { id: userIdHover }
                        ToolTip.text: qsTr("Double-click to copy")
                        ToolTip.visible: userIdHover.hovered
                        ToolTip.delay: 800
                    }
                }

            }
            AbstractButton {
                id: messageButton
                text: qsTr("Message")
                visible: !root.isOwn && app.conversations.supported
                Layout.fillWidth: true
                implicitWidth: messageContent.implicitWidth
                               + 2 * AppTheme.spacing12
                implicitHeight: 32
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Start or open a direct message with %1")
                                 .arg(root.visibleName)
                onClicked: root.startOrOpenDm()
                Keys.onReturnPressed: (event) => { event.accepted = true; if (!event.isAutoRepeat) clicked() }
                Keys.onEnterPressed: (event) => { event.accepted = true; if (!event.isAutoRepeat) clicked() }

                contentItem: RowLayout {
                    id: messageContent
                    spacing: AppTheme.spacing6
                    Item { Layout.fillWidth: true }
                    Icon {
                        name: "chat_bubble"
                        size: 16
                        color: AppTheme.boltInk
                    }
                    Label {
                        text: messageButton.text
                        color: AppTheme.boltInk
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textBody
                        font.weight: AppTheme.weightStrong
                    }
                    Item { Layout.fillWidth: true }
                }
                background: Rectangle {
                    radius: AppTheme.radiusMd
                    color: !messageButton.enabled ? AppTheme.stormInset
                           : messageButton.down ? Qt.darker(AppTheme.bolt, 1.12)
                           : messageButton.hovered ? Qt.darker(AppTheme.bolt, 1.05)
                           : AppTheme.bolt
                }
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -2
                    radius: AppTheme.radiusMd + 2
                    color: "transparent"
                    border.width: 2
                    border.color: AppTheme.focusRing
                    visible: messageButton.visualFocus
                }
            }

            Loader {
                Layout.fillWidth: true
                active: root.bioText.length > 0
                visible: active
                sourceComponent: Rectangle {
                    objectName: "profileBioBox"
                    implicitHeight: Math.min(bioLabel.implicitHeight,
                                             bioMetrics.lineSpacing * 12)
                                    + 2 * AppTheme.spacing12
                    radius: AppTheme.radiusMd
                    color: AppTheme.stormInset
                    border.width: 1
                    border.color: bioLabel.activeFocus ? AppTheme.focusRing
                                                       : AppTheme.stormBorder
                    clip: true
                    FontMetrics {
                        id: bioMetrics
                        font: bioLabel.font
                    }
                    Flickable {
                        id: bioFlick
                        objectName: "profileBioFlick"
                        anchors.fill: parent
                        anchors.topMargin: AppTheme.spacing12
                        anchors.bottomMargin: AppTheme.spacing12
                        anchors.leftMargin: AppTheme.spacing12
                        anchors.rightMargin: AppTheme.spacing4
                        clip: true
                        contentWidth: width
                        contentHeight: bioLabel.implicitHeight
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: AppScrollBar {
                            objectName: "profileBioScrollBar"
                            thin: true
                            policy: bioFlick.contentHeight > bioFlick.height
                                    ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
                        }
                        function scrollBy(dy) {
                            contentY = Math.max(0, Math.min(contentY + dy,
                                                            contentHeight - height))
                        }
                        TextEdit {
                            id: bioLabel
                            objectName: "profileBioText"
                            width: bioFlick.width
                            rightPadding: AppTheme.spacing8
                            text: root.bioText
                            textFormat: Text.PlainText
                            readOnly: true
                            activeFocusOnTab: true
                            selectByMouse: true
                            selectByKeyboard: true
                            selectionColor: AppTheme.textSelection
                            selectedTextColor: AppTheme.selectedText
                            wrapMode: TextEdit.Wrap
                            color: AppTheme.stormTextSecondary
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textBody
                            // The caret of a read-only edit is invisible, so
                            // unmodified paging keys scroll the view instead.
                            Keys.onPressed: (event) => {
                                if (event.modifiers !== Qt.NoModifier)
                                    return
                                var line = bioMetrics.lineSpacing
                                var page = bioFlick.height - line
                                var dy = event.key === Qt.Key_Down ? line
                                       : event.key === Qt.Key_Up ? -line
                                       : event.key === Qt.Key_PageDown ? page
                                       : event.key === Qt.Key_PageUp ? -page
                                       : event.key === Qt.Key_End ? bioFlick.contentHeight
                                       : event.key === Qt.Key_Home ? -bioFlick.contentHeight
                                       : 0
                                if (dy === 0)
                                    return
                                bioFlick.scrollBy(dy)
                                event.accepted = true
                            }
                            onCursorRectangleChanged: {
                                var r = cursorRectangle
                                if (r.y < bioFlick.contentY)
                                    bioFlick.contentY = r.y
                                else if (r.y + r.height
                                         > bioFlick.contentY + bioFlick.height)
                                    bioFlick.contentY = r.y + r.height - bioFlick.height
                            }
                        }
                    }
                    Accessible.role: Accessible.StaticText
                    Accessible.name: qsTr("Bio")
                    Accessible.description: root.bioText
                }
            }

            UserClientsInfo {
                userId: root.userId
            }

            Flow {
                id: profileChipRow
                objectName: "profileActionRow"
                Layout.fillWidth: true
                spacing: AppTheme.spacing6
                readonly property int uniformHeight: AppTheme.chipHeight + 6

                StatusChip {
                    objectName: "profileHomeserverChip"
                    height: profileChipRow.uniformHeight
                    visible: root.homeserver.length > 0
                    storm: true
                    tone: "neutral"
                    iconName: "alternate_email"
                    label: root.homeserver
                }

                ProfileChipButton {
                    height: profileChipRow.uniformHeight
                    objectName: "profileShareButton"
                    iconName: "link"
                    label: qsTr("Share")
                    accessibleName: qsTr("Copy profile link")
                    onClicked: root.copyProfileLink()
                }

                Rectangle {
                    objectName: "profileMembershipChip"
                    visible: root.membershipLabel.length > 0
                    height: profileChipRow.uniformHeight
                    implicitWidth: membershipRow.implicitWidth
                                   + 2 * AppTheme.chipPaddingH
                    implicitHeight: Math.max(AppTheme.chipHeight,
                                             membershipRow.implicitHeight
                                             + 2 * AppTheme.spacing2)
                    radius: AppTheme.chipRadius
                    color: Qt.alpha(AppTheme.stormTextMuted, 0.14)
                    border.width: 1
                    border.color: Qt.alpha(AppTheme.stormTextMuted, 0.32)
                    Row {
                        id: membershipRow
                        anchors.centerIn: parent
                        spacing: AppTheme.spacing6
                        PresenceDot {
                            id: chipPresenceDot
                            anchors.verticalCenter: parent.verticalCenter
                            dotSize: 10
                            ring: "transparent"
                            userId: root.opened ? root.userId : ""
                            hoverStatus: true
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.membershipLabel
                            color: AppTheme.stormTextMuted
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMicro
                            font.weight: AppTheme.weightStrong
                            textFormat: Text.PlainText
                        }
                    }
                    ToolTip.text: chipPresenceDot.statusText.length > 0
                                  ? chipPresenceDot.statusText
                                  : qsTr("Presence unavailable")
                    ToolTip.visible: membershipHover.hovered
                                     && (chipPresenceDot.statusText.length > 0
                                         || root.presenceUnavailable)
                    ToolTip.delay: 300
                    HoverHandler { id: membershipHover }
                    Accessible.role: Accessible.StaticText
                    Accessible.name: root.membershipLabel
                    Accessible.description:
                        chipPresenceDot.statusText.length > 0
                        ? chipPresenceDot.statusText
                        : (root.presenceUnavailable
                           ? qsTr("Presence unavailable") : "")
                }

                StatusChip {
                    height: profileChipRow.uniformHeight
                    visible: root.badgeRole === "administrator" || root.badgeRole === "creator"
                    storm: true
                    tone: "accent"
                    label: root.badgeRole === "creator" ? qsTr("Creator")
                                                        : qsTr("Administrator")
                }
                StatusChip {
                    height: profileChipRow.uniformHeight
                    visible: root.badgeRole === "moderator"
                    storm: true
                    tone: "info"
                    label: qsTr("Moderator")
                }
                StatusChip {
                    height: profileChipRow.uniformHeight
                    visible: root.isOwn
                    storm: true
                    tone: "neutral"
                    label: qsTr("You")
                }

                ProfileChipButton {
                    height: profileChipRow.uniformHeight
                    objectName: "profileOverflowButton"
                    iconName: "more_horiz"
                    label: ""
                    accessibleName: qsTr("More actions")
                    onClicked: {
                        app.roomInfo.requestMutualRooms(root.userId)
                        profileOverflowMenu.popup(0, height + 2)
                    }
                    AppMenu {
                        id: profileOverflowMenu
                        menuWidth: 240
                        height: Math.min(implicitHeight, 420)

                        Loader {
                            active: !root.isOwn
                                    && app.roomInfo.mutualRooms.length > 0
                            visible: active
                            sourceComponent: MenuSectionLabel {
                                objectName: "profileMutualRoomsHeader"
                                leftPadding: AppTheme.menuItemPadding + 6
                                rightPadding: AppTheme.menuItemPadding
                                text: qsTr("Rooms in common")
                            }
                        }
                        Repeater {
                            model: root.isOwn ? [] : app.roomInfo.mutualRooms
                            delegate: AppMenuItem {
                                required property var modelData
                                objectName: "profileMutualRoom"
                                text: modelData.name.length > 0
                                    ? modelData.name : modelData.roomId
                                iconName: modelData.isDirect
                                    ? "person" : "tag"
                                onTriggered: {
                                    root.close()
                                    app.openRoom(modelData.roomId)
                                    app.requestComposerFocus(modelData.roomId)
                                }
                            }
                        }
                        AppMenuSeparator {
                            visible: !root.isOwn
                                     && app.roomInfo.mutualRooms.length > 0
                        }

                        AppMenuItem {
                            objectName: "profileCopyIdButton"
                            text: qsTr("Copy user ID")
                            Accessible.description:
                                qsTr("Copy Matrix ID %1").arg(root.userId)
                            iconName: "content_copy"
                            onTriggered: root.copyUserId()
                        }
                        AppMenuItem {
                            objectName: "profileIgnoreButton"
                            visible: !root.isOwn && app.moderation.supported
                            enabled: !app.moderation.busy
                            danger: !root.userIgnored
                            text: root.userIgnored ? qsTr("Unignore user")
                                                   : qsTr("Ignore user")
                            Accessible.description: root.userIgnored
                                ? qsTr("Stop ignoring %1 in every room")
                                  .arg(root.visibleName)
                                : qsTr("Ignore %1 in every room")
                                  .arg(root.visibleName)
                            iconName: "block"
                            // Ignoring is account-wide and hides existing
                            // messages on every device, so confirm. Un-ignoring
                            // asks nothing.
                            onTriggered: {
                                root.ignoreNotice = ""
                                if (root.userIgnored) {
                                    app.moderation.unignoreUser(root.userId)
                                    return
                                }
                                ignoreConfirm.open()
                            }
                        }
                    }
                }
            }
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
                        font.pixelSize: AppTheme.textMeta
                        font.weight: AppTheme.weightBold
                    }
                    Label {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: root.roleLabel
                        color: AppTheme.stormText
                        font.pixelSize: AppTheme.textBody
                        font.weight: AppTheme.weightBold
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
                                textFormat: Text.PlainText
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                color: roleButton.enabled
                                       ? AppTheme.stormTextSecondary
                                       : AppTheme.stormTextFaint
                                font.family: AppTheme.menuFont
                                font.pixelSize: AppTheme.textMeta
                                font.weight: AppTheme.weightBold
                                elide: Label.ElideRight
                            }
                            background: Rectangle {
                                radius: AppTheme.radiusTile
                                color: (roleButton.hovered || roleButton.down)
                                       ? AppTheme.stormSelection : "transparent"
                                border.width: roleButton.visualFocus ? 2 : 1
                                border.color: roleButton.visualFocus
                                              ? AppTheme.focusRing
                                              : AppTheme.stormBorderStrong
                            }
                            Keys.onReturnPressed: (event) => { event.accepted = true; if (!event.isAutoRepeat) clicked() }
                            Keys.onEnterPressed: (event) => { event.accepted = true; if (!event.isAutoRepeat) clicked() }
                            onClicked: {
                                root.roleError = ""
                                roleConfirm.openFor(modelData.level,
                                                    modelData.label)
                            }
                        }
                    }
                }

                Dialog {
                    id: ignoreConfirm
                    objectName: "profileIgnoreConfirmDialog"
                    parent: Overlay.overlay
                    anchors.centerIn: parent
                    width: Math.max(240,
                                    Math.min(400,
                                             parent ? parent.width - 32 : 400))
                    modal: true
                    standardButtons: Dialog.NoButton
                    closePolicy: Popup.CloseOnEscape
                    title: qsTr("Ignore this person?")
                    background: Rectangle {
                        color: AppTheme.surface
                        border.color: AppTheme.border
                        radius: AppTheme.radiusLg
                    }
                    contentItem: ColumnLayout {
                        spacing: AppTheme.spacing12
                        Label {
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            wrapMode: Text.WordWrap
                            text: qsTr("Ignore %1 in every room, on every "
                                       + "device? Their existing messages "
                                       + "are hidden too. You can undo this "
                                       + "from their profile.")
                                    .arg(root.visibleName)
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing8
                            Item { Layout.fillWidth: true }
                            AppButton {
                                objectName: "profileIgnoreConfirmCancel"
                                storm: true
                                kind: "secondary"
                                text: qsTr("Cancel")
                                onClicked: ignoreConfirm.close()
                            }
                            AppButton {
                                objectName: "profileIgnoreConfirmAccept"
                                storm: true
                                kind: "danger"
                                text: qsTr("Ignore")
                                onClicked: {
                                    ignoreConfirm.close()
                                    app.moderation.ignoreUser(root.userId)
                                }
                            }
                        }
                    }
                }

                Dialog {
                    id: roleConfirm
                    objectName: "profileRoleConfirmDialog"
                    parent: Overlay.overlay
                    anchors.centerIn: parent
                    width: Math.max(240,
                                    Math.min(400,
                                             parent ? parent.width - 32 : 400))
                    modal: true
                    standardButtons: Dialog.NoButton
                    closePolicy: Popup.CloseOnEscape
                    title: roleConfirm.selfDemotion
                           ? qsTr("Lower your own role?")
                           : qsTr("Change role?")

                    property int pendingLevel: 0
                    property string pendingLabel: ""
                    readonly property bool irreversible:
                        app.roomInfo && !root.isOwn
                        && roleConfirm.pendingLevel >= app.roomInfo.ownPowerLevel
                    readonly property bool selfDemotion:
                        app.roomInfo && root.isOwn
                        && roleConfirm.pendingLevel < app.roomInfo.ownPowerLevel
                        && app.roomInfo.ownPowerLevel < 9007199254740992
                    readonly property bool losesRoleControl: {
                        var _roster = app.roomInfo ? app.roomInfo.ownPowerLevel : 0
                        return roleConfirm.selfDemotion
                               && app.roomInfo.powerLevelKnown("m.room.power_levels")
                               && roleConfirm.pendingLevel
                                  < app.roomInfo.powerLevelForKey("m.room.power_levels")
                    }

                    function openFor(level, label) {
                        pendingLevel = level
                        pendingLabel = label
                        open()
                    }

                    background: Rectangle {
                        color: AppTheme.surface
                        border.color: AppTheme.border
                        radius: AppTheme.radiusLg
                    }
                    contentItem: ColumnLayout {
                        spacing: AppTheme.spacing12
                        Label {
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            wrapMode: Text.WordWrap
                            text: qsTr("Set %1 to %2 in this room?")
                                    .arg(root.visibleName)
                                    .arg(roleConfirm.pendingLabel)
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: roleConfirm.irreversible
                            wrapMode: Text.WordWrap
                            color: AppTheme.danger
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("This gives them your own level or "
                                       + "higher. You will not be able to "
                                       + "change it back.")
                        }
                        Label {
                            objectName: "profileSelfDemotionWarning"
                            Layout.fillWidth: true
                            visible: roleConfirm.selfDemotion
                            wrapMode: Text.WordWrap
                            color: AppTheme.danger
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("You are lowering your own role. You "
                                       + "will not be able to raise it back "
                                       + "yourself: only someone whose role "
                                       + "is above your new one can, and if "
                                       + "nobody else holds a role as high "
                                       + "as yours, nobody can.")
                        }
                        Label {
                            objectName: "profileSelfDemotionLosesControl"
                            Layout.fillWidth: true
                            visible: roleConfirm.losesRoleControl
                            wrapMode: Text.WordWrap
                            color: AppTheme.danger
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("You will also no longer be able to "
                                       + "change roles or permissions in "
                                       + "this room.")
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing8
                            Item { Layout.fillWidth: true }
                            AppButton {
                                objectName: "profileRoleConfirmCancel"
                                storm: true
                                kind: "secondary"
                                text: qsTr("Cancel")
                                onClicked: roleConfirm.close()
                            }
                            AppButton {
                                objectName: "profileRoleConfirmAccept"
                                storm: true
                                kind: roleConfirm.selfDemotion ? "danger" : "primary"
                                text: roleConfirm.selfDemotion
                                      ? qsTr("Lower my role")
                                      : qsTr("Change role")
                                onClicked: {
                                    roleConfirm.close()
                                    app.roomInfo.setMemberPowerLevel(
                                        root.userId, roleConfirm.pendingLevel)
                                }
                            }
                        }
                    }
                }

                Label {
                    visible: root.roleError.length > 0
                    Layout.fillWidth: true
                    text: root.roleError
                    color: AppTheme.danger
                    font.pixelSize: AppTheme.textMeta
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    wrapMode: Text.Wrap
                    Accessible.name: text
                }
            }

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
                                textFormat: Text.PlainText
                                text: modButton.modelData.label
                                color: modButton.modInk
                                font.family: AppTheme.menuFont
                                font.pixelSize: 13
                                font.weight: AppTheme.weightBold
                            }
                            Item { Layout.fillWidth: true }
                        }
                        background: Rectangle {
                            radius: AppTheme.radiusTile
                            color: (modButton.hovered || modButton.down)
                                   ? (modButton.modelData.danger
                                      ? AppTheme.stormDangerSoft
                                      : AppTheme.stormSelection)
                                   : "transparent"
                            border.width: modButton.visualFocus ? 2 : 1
                            border.color: modButton.visualFocus
                                          ? AppTheme.focusRing
                                          : (modButton.modelData.danger
                                             ? AppTheme.stormDangerBorder
                                             : AppTheme.stormBorderStrong)
                        }
                        Keys.onReturnPressed: (event) => { event.accepted = true; if (!event.isAutoRepeat) clicked() }
                        Keys.onEnterPressed: (event) => { event.accepted = true; if (!event.isAutoRepeat) clicked() }
                        onClicked: {
                            root.modError = ""
                            root.modAction = modelData.op
                        }
                    }
                }
            }

            Label {
                objectName: "profilePolicyNotice"
                visible: root.policyMatched && !root.isOwn
                Layout.fillWidth: true
                textFormat: Text.PlainText
                text: root.policyReason.length > 0
                      ? qsTr("On a moderation list you follow — %1")
                            .arg(root.policyReason)
                      : qsTr("On a moderation list you follow.")
                color: AppTheme.warning
                font.pixelSize: AppTheme.textMeta
                font.weight: AppTheme.weightStrong
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                wrapMode: Text.Wrap
            }

            Label {
                visible: root.ignoreNotice.length > 0 && root.modAction === ""
                Layout.fillWidth: true
                text: root.ignoreNotice
                color: root.ignoreNoticeError ? AppTheme.danger
                                              : AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                wrapMode: Text.Wrap
                Accessible.name: text
            }

            ColumnLayout {
                visible: root.modAction !== ""
                Layout.fillWidth: true
                spacing: AppTheme.spacing6

                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: root.modAction === "kick"
                          ? qsTr("Remove %1 from this room?").arg(root.visibleName)
                          : root.modAction === "ban"
                            ? qsTr("Ban %1 from this room?").arg(root.visibleName)
                            : qsTr("Unban %1? They will be able to join again.")
                                  .arg(root.visibleName)
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightBold
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    wrapMode: Text.Wrap
                }
                AppTextField {
                    id: modReasonField
                    Layout.fillWidth: true
                    storm: true
                    placeholderText: qsTr("Reason (optional)")
                    onAccepted: if (modConfirmButton.enabled) modConfirmButton.clicked()
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
                        font.pixelSize: AppTheme.textBody
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
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
                        id: modConfirmButton
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
                    font.pixelSize: AppTheme.textMeta
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
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
                function onMembersChanged() {
                    root._refreshModeration()
                }
                function onPowerLevelActionFinished(roomId, userId, level, ok,
                                                    message) {
                    if (userId !== root.userId)
                        return
                    root.roleError = ok ? "" : message
                }
            }

            Label {
                id: copiedNotice
                objectName: "profileCopiedNotice"
                visible: false
                text: ""
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
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
