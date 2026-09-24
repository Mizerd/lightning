import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

// Member profile card, a centred modal: callers use `parent: Overlay.overlay;
// anchors.centerIn: parent` and openFor().
//
// Shows banner, avatar, display name in identity ink, full MXID, bio (MSC4440
// over MSC4133), chips (homeserver, Share, membership with the presence dot,
// power-level chips), Message, an overflow menu and room-scoped moderation.
//   * Presence is the dot, which carries its own sentence on hover
//     (PresenceDot.statusText). The membership chip carries the one fact the
//     dot cannot: presence known to be unavailable.
//   * The bio is remote free text: always Text.PlainText, on top of the
//     stripping in rust/src/bio.rs. Never rich text or a link target (§6): an
//     embedded <img> would be fetched for every viewer.
//   * The badge is decoration from a fixed local table: not Matrix state,
//     moderation or verification, and its accessible description says so.
// Features without a backend are omitted rather than shown disabled (calls,
// Verified chip, shared rooms, nickname). A mutual-rooms count would need a
// /members request per joined room.
Popup {
    id: root
    modal: true
    width: 296
    padding: 0

    // Scale the whole card on large displays so its proportions survive; a
    // non-integer item scale renders Text via distance fields, so it stays
    // sharp. Bounded, and exactly 1.0 on an ordinary window. Scales around the
    // centre, so anchors.centerIn still centres it.
    readonly property real cardScale: {
        var w = parent ? parent.width : 0
        var h = parent ? parent.height : 0
        if (w <= 0 || h <= 0)
            return 1.0
        return Math.max(1.0, Math.min(1.5, Math.min(w / 1100, h / 720)))
    }
    scale: cardScale
    transformOrigin: Item.Center
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    property string userId: ""
    property string displayName: ""
    property string membership: ""
    property string role: ""
    property string avatarMxc: ""
    property bool isOwn: false

    // Moderation state: "" | "kick" | "ban", the confirm surface showing.
    property string modAction: ""
    property string modError: ""
    // Unban option: send an invite after a successful unban.
    property bool inviteBackChecked: true
    // The offer policy is RoomInfoController::canModerate (SDK permission,
    // target row known, not self, strictly below the viewer's level). Captured
    // at openFor(); the controller re-checks at dispatch. Scoped to the
    // snapshot being for the room currently open.
    property bool showKick: false
    property bool showBan: false
    property bool showUnban: false

    // The member's power level and the role changes the viewer may make.
    // Captured for presentation; re-checked by the controller at dispatch.
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
        // Conventional presets plus the room's own users_default; duplicates
        // collapse.
        var candidates = [app.roomInfo.usersDefaultPowerLevel, 50, 100]
        var seen = {}
        var out = []
        for (var i = 0; i < candidates.length; ++i) {
            var level = candidates[i]
            if (seen[level] === true)
                continue
            seen[level] = true
            // Offer only what the viewer may set (canSetPowerLevel applies the
            // room's permission and the rules about peers at or above your
            // level).
            if (!app.roomInfo.canSetPowerLevel(userId, level))
                continue
            out.push({ level: level,
                       label: app.roomInfo.roleLabelForLevel(level) })
        }
        roleOptions = out
    }

    // The same member profile the list row shows. A display name with no
    // visible character (spaces, format characters, Hangul or Braille
    // fillers, variation selectors, tag characters) would draw an empty title,
    // so it falls back to the localpart like an absent one.
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

    // Accessible attaches to the contentItem: Popup is not an Item.

    // Fill in what the caller did not know from the room's roster, so every
    // call site (including a mention link with only a user id) gets name and
    // avatar. Nothing is fabricated: an unknown user keeps an empty name
    // (visibleName falls back to the localpart) and an empty avatar (initials).
    function _fillFromRoster() {
        if (userId.length === 0)
            return
        if (displayName.length > 0 && avatarMxc.length > 0
            && membership.length > 0)
            return
        if (!app.roomInfo)
            return
        // The reader's room explicitly: app.roomInfo.roomId may point
        // elsewhere.
        var row = app.roomInfo.memberFor(userId, app.currentRoomId)
        if (!row || !row.userId)
            return
        if (displayName.length === 0)
            displayName = row.displayName || ""
        if (avatarMxc.length === 0)
            avatarMxc = row.avatarUrl || ""
        if (membership.length === 0)
            membership = row.membership || ""
        // `role` is not derived here; the role block below renders the
        // controller's own reading of the power level.
    }

    // For a user the roster cannot name, ask the global profile. A per-room
    // name still wins when there is one.
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

    function openFor(member) {
        userId = member.userId || ""
        displayName = member.displayName || ""
        membership = member.membership || ""
        role = member.role || ""
        avatarMxc = member.avatarUrl || ""
        isOwn = member.isOwn === true
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
        // Scope the room-info controller to the reader's room.
        // _refreshModeration() rightly refuses admin controls unless it is
        // scoped here, and otherwise only Room Information, People, Pinned and
        // Search set that scope, so a card opened from the timeline showed no
        // Kick/Ban/Role. The answer arrives through the existing
        // onMembersChanged connection.
        if (app.roomInfo && app.currentRoomId.length > 0
            && app.roomInfo.roomId !== app.currentRoomId)
            app.roomInfo.roomId = app.currentRoomId
        _refreshModeration()
        _refreshIgnored()
        open()
    }

    // Opens straight onto the confirm step for `op` ("kick", "ban" or
    // "unban") when the viewer may do it; otherwise the plain card, whose
    // controls say what is possible.
    function openForAction(member, op) {
        openFor(member)
        if ((op === "kick" && showKick) || (op === "ban" && showBan)
                || (op === "unban" && showUnban))
            modAction = op
    }

    // Policy lists: whether a list this account follows covers the person,
    // asked on open. Acting on it is the ordinary Ignore item; nothing acts
    // automatically (docs/feature-contracts.md, "Policy lists").
    property bool policyMatched: false
    property string policyReason: ""

    // Account-wide ignore (m.ignored_user_list), recomputed on the controller's
    // revision since a binding cannot observe a Q_INVOKABLE.
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
        // Reuse an existing DM from the SDK's m.direct projection, otherwise
        // create a new encrypted DM.
        app.conversations.checkExistingDm(userId)
        var existing = app.conversations.existingDms
        if (existing.length > 0)
            app.openRoom(existing[0].roomId)
        else
            app.conversations.startDirectMessage(userId)
        close()
    }

    // Presence known to be unavailable: the backend has none, or the server
    // refused it (PresenceManager's latch). Unanswered is unknown and renders
    // nothing, never a fabricated Offline.
    readonly property bool presenceUnavailable:
        root.opened && root.userId !== "" && app.presence
            ? app.presence.unavailable : false

    // Derived facts the card renders

    // The homeserver half of the address.
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

    // The bio. "" covers no bio, not asked yet and a server without extended
    // profiles, and all render as nothing (M_NOT_FOUND is not an error). A pure
    // read on the manager's revision; asking happens on the open edge.
    readonly property string bioText: {
        if (!root.opened || root.userId === "" || !app.bio)
            return ""
        var rev = app.bio.revision
        return app.bio.bioFor(root.userId)
    }

    // Decorative thank-you badge; not Matrix state, a permission or
    // verification.
    readonly property string badgeLabel:
        app.badges && root.userId !== ""
            ? app.badges.labelFor(root.userId) : ""
    readonly property string badgeDescription:
        app.badges && root.userId !== ""
            ? app.badges.descriptionFor(root.userId) : ""

    // Clipboard actions

    function _copy(text, notice) {
        if (!text || text.length === 0)
            return
        idClipboard.text = text
        idClipboard.selectAll()
        idClipboard.copy()
        // Cleared immediately (RoomsPanel/SpacesRail convention).
        idClipboard.text = ""
        copiedNotice.text = notice
        copiedNotice.visible = true
        copiedTimer.restart()
    }

    function copyUserId() {
        root._copy(root.userId, qsTr("Matrix ID copied"))
    }

    // The public matrix.to profile link, percent-encoded like
    // MentionTokenizer::matrixToUrl so it matches a sent mention.
    function copyProfileLink() {
        if (root.userId.length === 0)
            return
        root._copy("https://matrix.to/#/" + encodeURIComponent(root.userId),
                   qsTr("Profile link copied"))
    }

    // One chip-shaped button for Share and the overflow.
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
        // Centred in a wrapper: a Control stretches its contentItem, and a Row
        // lays out from x=0, so the content hugged the left edge.
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

    // Themed scrim for this centred modal.
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }

    // Shadow: the effect and its source must be siblings inside one background
    // Item; MultiEffect cannot anchor across the Popup background boundary.
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

    // Asking is a side effect, so it happens on the open edge.
    // ProfileBannerManager deduplicates per user per session.
    onOpenedChanged: {
        if (!opened || userId.length === 0)
            return
        if (app.banners)
            app.banners.request(userId)
        // Same for the bio; ProfileBioManager deduplicates and refuses on a
        // server without extended profiles.
        if (app.bio)
            app.bio.request(userId)
        // A policy check reads every followed list, so it happens on the open
        // edge.
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
            // Cover the avatar's bottom edge: the halo hangs 34px below the
            // banner.
            implicitHeight: Math.max(banner.height + 28,
                                     avatarWrap.y + avatarWrap.height)

            // Canvas-painted so the 120° angle and ~70% stop can be expressed
            // exactly.
            Canvas {
                id: banner
                objectName: "profileBanner"
                // Antialias the painted rounded corners.
                antialiasing: true
                // Inset by the popover's border so the frame draws around the
                // banner.
                readonly property int frame: popoverBackground.border.width
                x: frame
                y: frame
                width: parent.width - 2 * frame
                // 3:1, the ratio the cropper produces (ImageCropDialog.aspect,
                // via ProfileBannerManager), so the crop the user chose is
                // exactly what shows.
                height: Math.round(width / 3)
                // Banner gradient: stormSelection → stormPanel at 120°.
                property color c1: AppTheme.stormSelection
                property color c2: AppTheme.stormPanel
                property int cornerRadius: Math.max(0, AppTheme.radiusLg - frame)
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
                    // CSS-style angle (0deg = up, clockwise) as a canvas
                    // gradient direction.
                    var angle = 120 * Math.PI / 180
                    var dx = Math.sin(angle)
                    var dy = -Math.cos(angle)
                    var length = Math.abs(w * dx) + Math.abs(h * dy)
                    var cx = w / 2, cy = h / 2
                    var grad = ctx.createLinearGradient(
                        cx - dx * length / 2, cy - dy * length / 2,
                        cx + dx * length / 2, cy + dy * length / 2)
                    grad.addColorStop(0, c1)
                    // Stop at ~70% so the destination tone takes over before
                    // the bottom.
                    grad.addColorStop(0.7, c2)
                    ctx.fillStyle = grad
                    ctx.fill()
                }
            }

            // The user's own banner (MSC4427) over the gradient, shown only
            // when it resolves. Fetched through the media bridge; the value is
            // always an mxc URI (Rust drops anything else), so it cannot point
            // at an arbitrary host. The mask rounds only the top corners
            // (`clip` cannot); the bottom meets the card body.
            Item {
                id: bannerCornerMask
                anchors.fill: banner
                visible: false
                layer.enabled: true
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
                anchors.fill: banner
                clip: true
                layer.enabled: true
                layer.effect: MultiEffect {
                    maskEnabled: true
                    maskSource: bannerCornerMask
                    // The ramp that maps mask alpha 0 -> 0 and 1 -> 1 is
                    // threshold 0.5 with spread 1 (per qquickmultieffect.cpp,
                    // the low ramp runs from (t - 1)(1 + s) + 1 to t(1 + s)).
                    // The defaults give a hard step and jagged corners.
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
                    readonly property string mxc: {
                        if (!root.opened || !app.banners)
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
                            // Cache keys end with the mxc; only this banner's
                            // completion matters.
                            if (key.endsWith(":" + bannerImage.mxc)
                                && bannerImage.source.toString().length === 0)
                                bannerImage.resolveTick++
                        }
                        // An expired transient failure mark: re-ask, as
                        // Avatar.qml does. The bridge re-arms the mark on
                        // failure, so this cannot hammer the backend.
                        function onMediaRetryable(key) {
                            if (key.endsWith(":" + bannerImage.mxc)
                                && bannerImage.source.toString().length === 0)
                                bannerImage.resolveTick++
                        }
                    }
                }
            }

            // Storm §3.6 corner watermark: one oversized outline bolt, clipped
            // by its own banner-sized container. Only over the gradient, never
            // over the user's own banner; bound to the image's readiness so it
            // does not blink out early.
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

            // 56px avatar overlapping the banner's bottom-left, on a slightly
            // larger panel-coloured disc that reads as a ring.
            Item {
                id: avatarWrap
                x: AppTheme.spacing16
                y: banner.height - 28
                // A 2px ring.
                width: 60
                height: 60
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
                // Watched only while open (the userId gate).
                PresenceDot {
                    id: avatarPresenceDot
                    objectName: "profileAvatarPresenceDot"
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 2
                    dotSize: 12
                    ring: AppTheme.stormPanel
                    userId: root.opened ? root.userId : ""
                    // The dot's own tooltip would open above it, over the
                    // avatar, so a custom tip sits to the right instead.
                    hoverStatus: false
                    HoverHandler {
                        id: avatarDotHover
                        enabled: avatarPresenceDot.visible
                    }
                    // A tip of our own: a ToolTip cannot declare a text format,
                    // and the status is member-written text that must render as
                    // plain text.
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
                        // Beside the dot it explains.
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

            // The person's status text (presence status_msg) when the last poll
            // carried one. Remote text, bounded in Rust, rendered plain.
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

            // Identity row: name (+ badge) and MXID.
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
                            // Untrusted text: never markup.
                            textFormat: Text.PlainText
                            objectName: "profileDisplayName"
                            Layout.fillWidth: true
                            // No minimum width: a Layout whose children's
                            // minimums exceed the parent overflows rather than
                            // shrinking them. The name has the whole line since
                            // Message moved to its own row.
                            text: root.visibleName
                            // Identity ink, hashed from the MXID like the
                            // timeline name and avatar.
                            color: AppTheme.userColor(root.userId)
                            font.family: AppTheme.menuFont
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
                            elide: Label.ElideRight
                        }

                        // Thank-you badge (decorative, local): tinted with the
                        // holder's own identity ink, with no verification or
                        // moderation vocabulary, and an accessible description
                        // that says so. A Loader: almost nobody has one, and a
                        // Label born with "" stays a viewport observer.
                        Loader {
                            active: root.badgeLabel.length > 0
                            visible: active
                            // The badge gives way: capped, and its text elides.
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
                                // Soft-chip treatment: the ink at 14% with a
                                // 32% border.
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
                        // Remote text: never markup.
                        textFormat: Text.PlainText
                        objectName: "profileUserId"
                        Layout.fillWidth: true
                        text: root.userId
                        color: AppTheme.stormTextMuted
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.fontMonoXS
                        // Wrapped, not elided: the MXID must stay exact.
                        // WrapAnywhere because it has no spaces.
                        wrapMode: Text.WrapAnywhere
                        maximumLineCount: 2
                        elide: Label.ElideRight
                    }
                }

            }
            // Primary Message action, full width under the identity (as in
            // Sable and Discord), so the name does not compete with it for the
            // line. Mirrors AppButton's accent styling locally for the
            // chat_bubble icon.
            AbstractButton {
                id: messageButton
                text: qsTr("Message")
                visible: !root.isOwn && app.conversations.supported
                // Centred on the identity block.
                Layout.fillWidth: true
                // Sized to its own content.
                implicitWidth: messageContent.implicitWidth
                               + 2 * AppTheme.spacing12
                implicitHeight: 32
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Start or open a direct message with %1")
                                 .arg(root.visibleName)
                onClicked: root.startOrOpenDm()

                // Spacer-centred: a control stretches its contentItem, so
                // centerIn on the layout does nothing. The one bolt fill on
                // this surface; boltInk is its ink.
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

            // Bio card (MSC4440 over MSC4133): remote free text, bounded and
            // stripped in Rust and rendered as PlainText only (§6). No bio, not
            // asked yet and no server support all render nothing. A Loader for
            // the same reason as the badge.
            Loader {
                Layout.fillWidth: true
                active: root.bioText.length > 0
                visible: active
                sourceComponent: Rectangle {
                    implicitHeight: bioLabel.implicitHeight + 2 * AppTheme.spacing12
                    radius: AppTheme.radiusMd
                    color: AppTheme.stormInset
                    border.width: 1
                    border.color: AppTheme.stormBorder
                    Label {
                        // Not `bioText`, the popover property this reads.
                        id: bioLabel
                        objectName: "profileBioText"
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: AppTheme.spacing12
                        anchors.rightMargin: AppTheme.spacing12
                        text: root.bioText
                        // Never StyledText or RichText.
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: AppTheme.stormTextSecondary
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textBody
                        // A second bound on top of Rust's, for bios stored
                        // before bounding existed.
                        maximumLineCount: 12
                        elide: Text.ElideRight
                    }
                    Accessible.role: Accessible.StaticText
                    Accessible.name: qsTr("Bio")
                    Accessible.description: root.bioText
                }
            }

            // Chip row: facts and light actions, wrapping rather than eliding,
            // since a clipped domain misleads.
            Flow {
                // Not `chipRow`: that id exists inside ProfileChipButton and
                // would shadow this one.
                id: profileChipRow
                Layout.fillWidth: true
                spacing: AppTheme.spacing6
                // One height for every chip: a Flow top-aligns, and StatusChip
                // and ProfileChipButton differ by a few pixels.
                readonly property int uniformHeight: AppTheme.chipHeight + 6

                // The homeserver half of the address; informational.
                StatusChip {
                    objectName: "profileHomeserverChip"
                    height: profileChipRow.uniformHeight
                    visible: root.homeserver.length > 0
                    storm: true
                    tone: "neutral"
                    iconName: "alternate_email"
                    label: root.homeserver
                }

                // Copies the public matrix.to profile link; the notice names
                // what was copied.
                ProfileChipButton {
                    height: profileChipRow.uniformHeight
                    objectName: "profileShareButton"
                    iconName: "link"
                    label: qsTr("Share")
                    accessibleName: qsTr("Copy profile link")
                    onClicked: root.copyProfileLink()
                }

                // Membership with the shared presence dot; unknown presence
                // hides the dot.
                Rectangle {
                    objectName: "profileMembershipChip"
                    visible: root.membershipLabel.length > 0
                    // Same height as the other chips.
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
                    // When presence is known to be unavailable the dot renders
                    // nothing; say so here, on the always-present chip.
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

                // Administrator / Moderator keep their tones: real power
                // levels.
                StatusChip {
                    height: profileChipRow.uniformHeight
                    visible: root.role === "administrator" || root.role === "creator"
                    storm: true
                    tone: "accent"
                    label: qsTr("Administrator")
                }
                StatusChip {
                    height: profileChipRow.uniformHeight
                    visible: root.role === "moderator"
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

                // Overflow: only actions that exist (ignore, copy id).
                ProfileChipButton {
                    height: profileChipRow.uniformHeight
                    objectName: "profileOverflowButton"
                    iconName: "more_horiz"
                    label: ""
                    accessibleName: qsTr("More actions")
                    onClicked: {
                        // Asked for on open: cached membership only, but still
                        // an FFI call.
                        app.roomInfo.requestMutualRooms(root.userId)
                        profileOverflowMenu.popup(0, height + 2)
                    }
                    AppMenu {
                        id: profileOverflowMenu
                        menuWidth: 240
                        // Bounded so the menu never exceeds the screen; Qt's
                        // Menu scrolls once it has a height. Rust also caps the
                        // list at MAX_MUTUAL_ROOMS.
                        height: Math.min(implicitHeight, 420)

                        // Rooms in common. Absent rather than empty: it reads
                        // only what the store holds, so "none known" is not
                        // "none". A Loader: a hidden Menu row must take no
                        // space, and `height: visible ? implicitHeight : 0` on
                        // a wrapping Label is a binding loop.
                        Loader {
                            active: !root.isOwn
                                    && app.roomInfo.mutualRooms.length > 0
                            visible: active
                            sourceComponent: MenuSectionLabel {
                                objectName: "profileMutualRoomsHeader"
                                // Matches AppMenuItem's left padding.
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
                                }
                            }
                        }
                        // AppMenuSeparator collapses its height when hidden.
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
                            // A write is already in flight.
                            enabled: !app.moderation.busy
                            danger: !root.userIgnored
                            text: root.userIgnored ? qsTr("Unignore user")
                                                   : qsTr("Ignore user")
                            // The scope: m.ignored_user_list is account-wide,
                            // not this room.
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
            // Role / power level. Always the member's real level (a non-preset
            // value is "Custom (N)"). Buttons offer only levels the viewer may
            // set; a server rejection is shown rather than hidden by an
            // optimistic repaint.
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
                        // Untrusted text: never markup.
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
                                // A bare Label contentItem fills the button and
                                // Text top-aligns by default.
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
                                border.width: 1
                                border.color: AppTheme.stormBorderStrong
                            }
                            // Confirm first: promoting someone to your own
                            // level cannot be undone, since Matrix refuses
                            // changes at or above your own level.
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
                            Button {
                                objectName: "profileIgnoreConfirmCancel"
                                text: qsTr("Cancel")
                                onClicked: ignoreConfirm.close()
                            }
                            Button {
                                objectName: "profileIgnoreConfirmAccept"
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
                    title: qsTr("Change role?")

                    property int pendingLevel: 0
                    property string pendingLabel: ""
                    // A grant at or above your own level cannot be taken back;
                    // say so.
                    readonly property bool irreversible:
                        app.roomInfo
                        && roleConfirm.pendingLevel >= app.roomInfo.ownPowerLevel

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
                            // Chosen by its owner: never markup.
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
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing8
                            Item { Layout.fillWidth: true }
                            Button {
                                objectName: "profileRoleConfirmCancel"
                                text: qsTr("Cancel")
                                onClicked: roleConfirm.close()
                            }
                            Button {
                                objectName: "profileRoleConfirmAccept"
                                text: qsTr("Change role")
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

            // Moderation row: kick/ban are outline-danger, unban an ordinary
            // outline. Hidden entirely when the viewer lacks the power or it
            // cannot be established.
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
                                // Untrusted text: never markup.
                                textFormat: Text.PlainText
                                text: modButton.modelData.label
                                color: modButton.modInk
                                font.family: AppTheme.menuFont
                                font.pixelSize: 13
                                font.weight: AppTheme.weightBold
                            }
                            Item { Layout.fillWidth: true }
                        }
                        // Matches AppButton's storm danger treatment
                        // (stormDangerSoft hover on a 30% outline).
                        background: Rectangle {
                            radius: AppTheme.radiusTile
                            color: (modButton.hovered || modButton.down)
                                   ? (modButton.modelData.danger
                                      ? AppTheme.stormDangerSoft
                                      : AppTheme.stormSelection)
                                   : "transparent"
                            border.width: 1
                            border.color: modButton.modelData.danger
                                          ? AppTheme.stormDangerBorder
                                          : AppTheme.stormBorderStrong
                        }
                        onClicked: {
                            root.modError = ""
                            root.modAction = modelData.op
                        }
                    }
                }
            }

            // A followed moderation list covers this person: shown as
            // information with the list's reason (plain text). Ignore is the
            // user's decision.
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

            // Account-wide ignore report. The action lives in the overflow
            // menu; the report must be on the card, since the menu closes on
            // selection.
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

            // Inline confirm surface: optional reason + Confirm/Cancel.
            ColumnLayout {
                visible: root.modAction !== ""
                Layout.fillWidth: true
                spacing: AppTheme.spacing6

                Label {
                    // Untrusted text: never markup.
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
                // A roster refresh while open can change what may be offered.
                function onMembersChanged() {
                    root._refreshModeration()
                }
                // The new level arrives with the roster refresh; this only
                // reports a failure. Stays open on success so the new role is
                // visible.
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
                // Set by _copy(): names what went to the clipboard.
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
