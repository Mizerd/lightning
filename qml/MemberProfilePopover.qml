import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

// The member profile card. Presentation (centred-modal Popup, the
// `openFor()` contract) is unchanged since v0.6.5: callers still do
// `parent: Overlay.overlay; anchors.centerIn: parent`.
//
// Shows the member's banner and avatar, their display name in their own
// identity ink, the full Matrix ID, their BIO (MSC4440 over MSC4133, plain
// text only), a chip row (homeserver, Share, membership carrying the shared
// presence dot, the room's real power-level chips), the Message primary, an
// overflow menu, and the room-scoped moderation surface.
//
// 2026-08-28 rework, three things worth knowing before editing:
//
//   * THE STANDALONE PRESENCE LINE IS GONE. The dot is the indicator and it
//     carries its own sentence on hover (PresenceDot.statusText), so the
//     state is formatted in exactly one place in the application. The one
//     fact a dot structurally cannot report — presence KNOWN to be
//     unavailable, where the dot renders nothing and nothing is
//     indistinguishable from "not answered yet" — rides the membership chip.
//   * THE BIO IS REMOTE FREE TEXT. Text.PlainText, always, on top of the
//     bounding, control-stripping and markup-stripping rust/src/bio.rs
//     already does. Never StyledText, never RichText, never a link target
//     (§6): MSC4440's own example embeds an <img src="mxc://…">, which a
//     rich renderer would fetch for every viewer of the card.
//   * THE BADGE IS DECORATION. ProfileBadges is a fixed local table; a badge
//     is a thank-you, is the same for every viewer, is not Matrix state, and
//     is neither a moderation nor a verification signal. It carries none of
//     their vocabulary and its accessible description says so in words.
//
// Still deliberately OMITTED, because none has a backend and this component
// never renders a disabled placeholder: call/videocam, a Verified chip, the
// shared-rooms count, View full profile, and Set Nickname (Lightning has no
// per-user local nickname to set). A Mutual Rooms chip is NOT offered for a
// measured reason: the client holds no roster for a room it has not opened,
// so a count would cost one /members request per joined room on every open.
Popup {
    id: root
    modal: true
    width: 296
    padding: 0

    // On a large display this card is a 296px postage stamp in the middle of
    // a very big window ("in small windowed size its fine, but in 4k its
    // tiny"). Every size in it is chosen against every other one, so the card
    // is SCALED as a whole rather than re-laid-out at a second set of
    // measurements — the proportions survive, and a non-integer item scale
    // puts Text on the distance-field renderer, so it stays crisp instead of
    // being a stretched bitmap. Bounded, and exactly 1.0 on an ordinary
    // window. The scale is around the item's centre, so the caller's
    // anchors.centerIn still lands it in the middle.
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

    // Fill in what the CALLER did not know, from the room's own roster.
    //
    // The reported defect: clicking a `mention:` link in a message body
    // opened a card with no picture and an MXID where the display name
    // should be, while clicking the same person's avatar one line above was
    // correct — because that call site happens to pass senderDisplayName and
    // senderAvatarMxc and the mention path has only a user id to give. There
    // are five call sites and every one of them had to remember; resolving
    // HERE is what stops the sixth getting it wrong.
    //
    // Nothing is fabricated. A user the roster does not hold — someone who
    // has left, or a room whose members were never fetched — keeps an empty
    // display name, and `visibleName` falls back to the localpart exactly as
    // every other surface does. The avatar stays EMPTY rather than being
    // guessed at: Avatar draws its identity-inked initials, and a wrong face
    // is worse than no face.
    function _fillFromRoster() {
        if (userId.length === 0)
            return
        if (displayName.length > 0 && avatarMxc.length > 0
            && membership.length > 0)
            return
        if (!app.roomInfo)
            return
        // The ROOM the reader is in, explicitly — `app.roomInfo.roomId`
        // follows whatever surface last pointed it somewhere and may be a
        // Space rather than this room.
        var row = app.roomInfo.memberFor(userId, app.currentRoomId)
        if (!row || !row.userId)
            return
        if (displayName.length === 0)
            displayName = row.displayName || ""
        if (avatarMxc.length === 0)
            avatarMxc = row.avatarUrl || ""
        if (membership.length === 0)
            membership = row.membership || ""
        // `role` is deliberately NOT derived here. The room's real power
        // level is already rendered by the role block below, from
        // RoomInfoController's own reading of it; a second derivation of the
        // same fact is how the two come to disagree.
    }

    function openFor(member) {
        userId = member.userId || ""
        displayName = member.displayName || ""
        membership = member.membership || ""
        role = member.role || ""
        avatarMxc = member.avatarUrl || ""
        isOwn = member.isOwn === true
        _fillFromRoster()
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

    // Presence that the client KNOWS is not on offer: the backend cannot do
    // presence at all, or this session's server refused it for every user
    // (PresenceManager's latch). Those are the only two states we may
    // disclose. A lookup that simply has not been answered — the common
    // case for a popover opened a moment ago — is still UNKNOWN, and
    // unknown renders nothing at all: no dot, no line, and never a
    // fabricated Offline.
    readonly property bool presenceUnavailable:
        root.opened && root.userId !== "" && app.presence
            ? app.presence.unavailable : false

    // The presence SENTENCE is no longer formatted here: it belongs to
    // PresenceDot, which is the component that shows the state, and the
    // status line that used to sit beside the avatar is gone. One derivation,
    // one wording, and a card that says a thing once. The dot carries it on
    // hover (`PresenceDot.statusText`), and the membership chip carries the
    // one fact the dot structurally cannot — see `presenceUnavailable`.

    // --- Derived facts the card renders --------------------------------

    // The homeserver half of the address. Pure string work; no lookup.
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

    // The bio (MSC4440 over MSC4133). "" covers three different facts that
    // look identical on a card — no bio, not asked yet, and a homeserver
    // without extended profile fields — and all three render as NOTHING.
    // An absent field answers M_NOT_FOUND, which is a server working
    // correctly, so it is never reported as an error.
    //
    // A pure read re-evaluated on the manager's revision; the ASKING is a
    // side effect and happens on the open edge, never inside this binding.
    readonly property string bioText: {
        if (!root.opened || root.userId === "" || !app.bio)
            return ""
        var rev = app.bio.revision   // re-evaluation dependency
        return app.bio.bioFor(root.userId)
    }

    // Decorative thank-you badge. A fixed local table, identical for every
    // viewer, and NOT Matrix state, a permission, or a verification claim.
    readonly property string badgeLabel:
        app.badges && root.userId !== ""
            ? app.badges.labelFor(root.userId) : ""
    readonly property string badgeDescription:
        app.badges && root.userId !== ""
            ? app.badges.descriptionFor(root.userId) : ""

    // --- Clipboard actions ---------------------------------------------

    function _copy(text, notice) {
        if (!text || text.length === 0)
            return
        idClipboard.text = text
        idClipboard.selectAll()
        idClipboard.copy()
        // Cleared immediately: a permalink or an id must not sit in a live
        // item (the RoomsPanel / SpacesRail convention).
        idClipboard.text = ""
        copiedNotice.text = notice
        copiedNotice.visible = true
        copiedTimer.restart()
    }

    function copyUserId() {
        root._copy(root.userId, qsTr("Matrix ID copied"))
    }

    // The PUBLIC matrix.to profile link — never an authenticated media URL
    // and never a client-internal identifier. Percent-encoded exactly as
    // MentionTokenizer::matrixToUrl encodes it, so a link copied here and a
    // mention pill sent from the composer are the same string.
    function copyProfileLink() {
        if (root.userId.length === 0)
            return
        root._copy("https://matrix.to/#/" + encodeURIComponent(root.userId),
                   qsTr("Profile link copied"))
    }

    // One chip-shaped button, so Share and the overflow cannot drift apart.
    // AbstractButton rather than a Control: the chip vocabulary is a pill
    // with a soft tint, and the stock control padding fights it.
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
        contentItem: Row {
            id: chipRow
            spacing: AppTheme.spacing2
            Icon {
                visible: chipButton.iconName.length > 0
                name: chipButton.iconName
                size: AppTheme.textMicro + 3
                color: AppTheme.stormLink
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                visible: chipButton.label.length > 0
                text: chipButton.label
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textMicro
                font.weight: AppTheme.weightBold
                color: AppTheme.stormLink
                anchors.verticalCenter: parent.verticalCenter
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

    // Asking is a side effect, so it happens on the open EDGE rather than
    // inside a binding. ProfileBannerManager deduplicates per user per
    // session, so reopening the same card costs nothing.
    onOpenedChanged: {
        if (!opened || userId.length === 0)
            return
        if (app.banners)
            app.banners.request(userId)
        // Same discipline: asking is a SIDE EFFECT, so it happens on the
        // edge and never in the `bioText` binding. ProfileBioManager
        // deduplicates per user per session, so reopening costs nothing,
        // and it refuses outright on a server without extended profiles.
        if (app.bio)
            app.bio.request(userId)
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

            // The user's OWN banner (MSC4427), over the gradient. Asked for
            // only while the popover is open, and rendered only when it
            // actually resolves — an unanswered lookup, a server without
            // extended profile fields and a user with no banner all render
            // as the gradient, because they are the same thing to look at.
            //
            // Fetched through the media bridge like every other mxc: the
            // value is an mxc:// URI by construction (Rust drops anything
            // else), so no profile field can point this at an arbitrary host.
            Item {
                anchors.fill: banner
                clip: true      // the banner's rounded top corners
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
                    // Re-resolve counter. wideImageSource() returns "" on a
                    // cache MISS and dispatches; nothing else this binding
                    // depends on changes when the bytes land, so the arrival
                    // has to poke it.
                    //
                    // It is a COUNTER and not an assignment to `source`
                    // because assigning a bound property imperatively
                    // DESTROYS the binding. That is what made banners sticky:
                    // the first banner that ever finished loading unbound
                    // this Image from `mxc` for the rest of the session, so
                    // opening anyone else's card kept showing that first
                    // person's banner — including over a banner they really
                    // had. Bumping a counter keeps the binding alive, so a
                    // new user still replaces it, and a user with none still
                    // clears it back to the gradient.
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
                            // Cache keys end with the mxc ("mxc:<edge>:<uri>"),
                            // so only this banner's own completion is worth a
                            // re-resolve — not every byte fetched anywhere.
                            if (key.endsWith(":" + bannerImage.mxc)
                                && bannerImage.source.toString().length === 0)
                                bannerImage.resolveTick++
                        }
                    }
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
                    objectName: "profileAvatarPresenceDot"
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 2
                    dotSize: 12
                    ring: AppTheme.stormPanel
                    userId: root.opened ? root.userId : ""
                    // The bubble on the avatar IS the status now — the line
                    // of prose that used to sit under the name is gone, so
                    // this dot has to be able to say what it means.
                    hoverStatus: true
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

            // --- Identity row -------------------------------------------
            // Name (+ decorative badge) and MXID on the left, the primary
            // Message action on the right — the Sable reference's shape, and
            // it buys the card back the vertical band the old full-width
            // action row spent.
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
                            objectName: "profileDisplayName"
                            Layout.fillWidth: true
                            // A FLOOR, so the badge beside it cannot squeeze
                            // the name away. A RowLayout that cannot fit
                            // every child shrinks them all in proportion to
                            // their PREFERRED widths, and a display name's
                            // implicit width is much larger than a short
                            // badge pill's — so the name absorbed nearly the
                            // whole squeeze and rendered as "Roma…" beside a
                            // fully drawn "idea master". The person's name is
                            // the primary identity on a card that is ABOUT
                            // them; decoration yields to it, not the reverse.
                            Layout.minimumWidth: Math.min(
                                implicitWidth, AppTheme.scaled(140))
                            text: root.visibleName
                            // The identity ink, hashed from the MXID exactly
                            // as the timeline sender name and the avatar disc
                            // are. This popover is ABOUT one person;
                            // rendering their name in the same grey as the
                            // panel chrome threw away the one colour the app
                            // already knows is theirs.
                            color: AppTheme.userColor(root.userId)
                            font.family: AppTheme.menuFont
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
                            elide: Label.ElideRight
                        }

                        // --- Thank-you badge (decorative, local) ----------
                        // A fixed local table (ProfileBadges), the same for
                        // every viewer, tinted to the holder's OWN identity
                        // ink so it introduces no colour whose meaning a
                        // reader has to learn. It is NOT a verification or
                        // moderation signal and carries none of their
                        // vocabulary — no shield, no check, no lock, no trust
                        // palette — and the accessible description says so in
                        // words rather than leaving the treatment to imply it.
                        //
                        // A Loader, not a `visible: false` Rectangle: almost
                        // nobody has a badge, and a Label born holding "" keeps
                        // ItemObservesViewport for its whole life (§16).
                        Loader {
                            active: root.badgeLabel.length > 0
                            visible: active
                            // The badge is what gives way now. Capped so a
                            // long label cannot reclaim the name's floor, and
                            // its text elides rather than overflowing — a
                            // pill cannot elide on its own.
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
                                // The sanctioned soft-chip treatment: the ink
                                // at 14% with a 32% border, so the pill can
                                // never drift from the ink it is made of.
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
                        Layout.fillWidth: true
                        text: root.userId
                        color: AppTheme.stormTextMuted
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.fontMonoXS
                        elide: Label.ElideMiddle
                    }
                }

                // Primary Message action — AppButton has no icon slot or a
                // parametrized radius, so this mirrors its accent styling
                // locally with the chat_bubble icon the spec asks for.
                AbstractButton {
                    id: messageButton
                    text: qsTr("Message")
                    visible: !root.isOwn && app.conversations.supported
                    Layout.alignment: Qt.AlignTop
                    // Sized to its own content now that it shares the row
                    // with the identity block instead of filling it.
                    implicitWidth: messageContent.implicitWidth
                                   + 2 * AppTheme.spacing12
                    implicitHeight: 32
                    hoverEnabled: true
                    focusPolicy: Qt.TabFocus
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Start or open a direct message with %1")
                                     .arg(root.visibleName)
                    onClicked: root.startOrOpenDm()

                    // Spacer-centred: a control
                    // stretches its contentItem to the full button width, so
                    // anchors.centerIn on the layout is a no-op and the
                    // icon+label would sit hard against the left edge.
                    // Storm §3.9 primary: THE one bolt fill on this surface.
                    // boltInk is the ink on that fill (Storm: deep canvas
                    // navy; legacy: accentText) — stays readable once bolt
                    // routes to each legacy theme's own accent.
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
            }

            // --- Bio card (MSC4440 over MSC4133) ------------------------
            // Free text a REMOTE user wrote about themselves. PLAIN TEXT
            // ONLY, always: the value is bounded, control-stripped and
            // markup-stripped in Rust, and rendered with Text.PlainText here,
            // so no bio can become a link target, an image fetch or rich
            // content (§6). A user with no bio, a user we have not asked
            // about yet and a homeserver without extended profile fields all
            // render as NOTHING — there is no empty card and no error, and
            // an absent field (M_NOT_FOUND) is not a failure.
            //
            // A Loader rather than `visible:`, for the same reason as the
            // badge: a Label born holding "" never loses ItemObservesViewport.
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
                        // NOT `bioText` — that is the popover property this
                        // reads, and one identifier for two things in the
                        // same expression is a bug waiting to be written.
                        id: bioLabel
                        objectName: "profileBioText"
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: AppTheme.spacing12
                        anchors.rightMargin: AppTheme.spacing12
                        text: root.bioText
                        // NEVER StyledText or RichText. See above.
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: AppTheme.stormTextSecondary
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textBody
                        // A second bound, on top of Rust's: a bio the far end
                        // stored before anyone bounded it must not be able to
                        // make this card taller than the window.
                        maximumLineCount: 12
                        elide: Text.ElideRight
                    }
                    Accessible.role: Accessible.StaticText
                    Accessible.name: qsTr("Bio")
                    Accessible.description: root.bioText
                }
            }

            // --- Chip row -----------------------------------------------
            // Facts and light actions, wrapping rather than eliding: at 296px
            // a fixed row would clip a long homeserver name, and a clipped
            // domain is a misleading one.
            Flow {
                Layout.fillWidth: true
                spacing: AppTheme.spacing6

                // The homeserver half of their address. Informational only —
                // there is nothing to do with it that this card can do.
                StatusChip {
                    visible: root.homeserver.length > 0
                    storm: true
                    tone: "neutral"
                    iconName: "alternate_email"
                    label: root.homeserver
                }

                // The PUBLIC matrix.to profile link. It copies, and the
                // notice says so out loud — this card has no other link
                // control, so naming the outcome is what keeps "Share" from
                // being a second name for something the user cannot see.
                ProfileChipButton {
                    objectName: "profileShareButton"
                    iconName: "link"
                    label: qsTr("Share")
                    accessibleName: qsTr("Copy profile link")
                    onClicked: root.copyProfileLink()
                }

                // Membership, carrying the presence dot the way the reference
                // does. The dot is the SHARED component, so the state, the
                // colours, the watch lifecycle and the wording are the same
                // ones every other surface uses; unknown presence hides the
                // dot and leaves the chip reading plain membership.
                Rectangle {
                    visible: root.membershipLabel.length > 0
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
                    // The one disclosure the dot cannot make: when presence
                    // is KNOWN to be unavailable the dot renders nothing, and
                    // nothing is indistinguishable from "not answered yet".
                    // It rides the chip because the chip is always there.
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

                // Administrator / Moderator keep their own tones: they are
                // the room's REAL power levels, and a decorative badge must
                // never compete with them.
                StatusChip {
                    visible: root.role === "administrator" || root.role === "creator"
                    storm: true
                    tone: "accent"
                    label: qsTr("Administrator")
                }
                StatusChip {
                    visible: root.role === "moderator"
                    storm: true
                    tone: "info"
                    label: qsTr("Moderator")
                }
                StatusChip {
                    visible: root.isOwn
                    storm: true
                    tone: "neutral"
                    label: qsTr("You")
                }

                // Overflow. It carries only actions that REALLY exist — the
                // account-wide ignore and the id copy. There is no "Set
                // Nickname" row because Lightning has no per-user local
                // nickname to set, and this card has never rendered a
                // disabled placeholder for something with no backend.
                ProfileChipButton {
                    objectName: "profileOverflowButton"
                    iconName: "more_horiz"
                    label: ""
                    accessibleName: qsTr("More actions")
                    onClicked: profileOverflowMenu.popup(0, height + 2)
                    AppMenu {
                        id: profileOverflowMenu
                        menuWidth: 200
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
                            // A write is already in flight; offering a second
                            // one would race the first.
                            enabled: !app.moderation.busy
                            danger: !root.userIgnored
                            text: root.userIgnored ? qsTr("Unignore user")
                                                   : qsTr("Ignore user")
                            // The SCOPE, which the row's two words cannot
                            // carry: m.ignored_user_list is account-wide, not
                            // this room. The old full-width button said so in
                            // its accessible name and that must not be lost
                            // to the move into a menu.
                            Accessible.description: root.userIgnored
                                ? qsTr("Stop ignoring %1 in every room")
                                  .arg(root.visibleName)
                                : qsTr("Ignore %1 in every room")
                                  .arg(root.visibleName)
                            iconName: "block"
                            onTriggered: {
                                root.ignoreNotice = ""
                                if (root.userIgnored)
                                    app.moderation.unignoreUser(root.userId)
                                else
                                    app.moderation.ignoreUser(root.userId)
                            }
                        }
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
                        font.pixelSize: AppTheme.textMeta
                        font.weight: AppTheme.weightBold
                    }
                    Label {
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
                                horizontalAlignment: Text.AlignHCenter
                                // A bare Label contentItem fills the button,
                                // and Text defaults to top alignment — the
                                // label rides high without this.
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
                    font.pixelSize: AppTheme.textMeta
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
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
                                font.weight: AppTheme.weightBold
                            }
                            Item { Layout.fillWidth: true }
                        }
                        // Matches AppButton's storm danger treatment exactly
                        // (stormDangerSoft hover on a 30%-alpha outline).
                        // A full-strength danger outline around a resting
                        // button reads as an error box, and hovering to
                        // stormSelection made a destructive control give the
                        // same feedback as a neutral one.
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

            // v0.7.x: account-wide ignore. The ACTION lives in the chip
            // row's overflow menu now; what stays here is its report, which
            // has to be on the card itself — a menu closes the moment the row
            // is chosen, so a notice living inside it would never be read.
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
                // Set by _copy(): the notice NAMES what went to the
                // clipboard, which is what keeps a chip labelled "Share"
                // from being a control whose effect the user cannot see.
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
