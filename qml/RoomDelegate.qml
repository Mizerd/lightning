import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    implicitHeight: content.implicitHeight + AppTheme.spacing6 * 2
    Accessible.role: Accessible.ListItem
    Accessible.name: {
        if (model.membership === "invited")
            return qsTr("Invitation to %1").arg(model.name)
        var base = model.highlightCount > 0
                   ? qsTr("%1, %2 mentions").arg(model.name)
                                            .arg(model.highlightCount)
                   : model.name
        // The mute glyph is decorative, so the row itself has to say it —
        // otherwise a screen reader cannot tell a muted room from a loud
        // one, which is the whole point of the state.
        return root.muted ? qsTr("%1, muted").arg(base) : base
    }
    // v0.6.5 (SPEC 1d): keyboard-operable context menu, mirroring
    // MessageDelegate's Menu-key/Shift+F10 open path (MessageDelegate.qml
    // :146-154). The menu itself stays joined-room-only (see Keys.onPressed
    // near roomMenu below), matching the existing right-click gate.
    // TRADE-OFF, deliberate: every row is a tab stop, so a long room list
    // costs many Tab presses — but the room list has no list-level arrow
    // navigation today, and without per-row stops the keyboard menu path
    // would only reach the selected room. Revisit if list-level focus
    // navigation lands.
    activeFocusOnTab: true

    property bool selected: false
    // Group rule under the LAST row of the favourites block. The room list
    // pins its section labels (RoomsPanel: ViewSection.CurrentLabelAtStart)
    // and they carry no fill of their own, so with a short favourites block
    // the two groups read as one continuous list. RoomsPanel owns the
    // decision of which row this is — the delegate cannot see the section
    // boundary without the view's attached properties, and only the room
    // list has sections at all (RoomListPane leaves this false).
    property bool showGroupDivider: false
    signal clicked()
    signal acceptInvite()
    signal rejectInvite()
    signal markRead()
    signal markUnread()
    // Element-parity favourites. Carries the value to WRITE, not a toggle
    // request: the menu reads the room's current flag and the row never
    // flips it locally, so a refused server write cannot leave the row and
    // the account disagreeing.
    signal setFavourite(bool on)
    // v0.6.5 (SPEC 1d): the delegate stays signal-only for every mutation —
    // the host (RoomsPanel) performs the actual app.settings/app.roomInfo
    // calls, exactly like markRead/markUnread today.
    signal setNotificationMode(int mode)
    signal copyRoomLink()
    signal leaveRoomRequested()

    // Meta ink for this row. `textMuted` is tuned against the LIST surface,
    // not against `selected`, and on a selected row it measured 3.12:1 on
    // Lightning Dark after the 2026-08-21 ladder rebuild — a raised selection
    // makes every unconditional muted ink worse. Six sites already branched on
    // `selected` by hand; four did not, and those four were the sub-AA ones.
    // The rule is named once here so the next one cannot be missed.
    readonly property color metaInk: selected ? AppTheme.selectedText
                                              : AppTheme.textMuted

    // Read rows are dimmed and lighter-weight; unread/selected rows carry
    // full ink (design handoff §2 room-row states).
    readonly property bool isUnread: model.hasUnread || model.markedUnread

    // ── Mute state ────────────────────────────────────────────────────────
    // 2026-08-21: a muted room used to be visually identical to a loud one
    // and still shouted a full-strength count pill, so the only way to check
    // whether the mute took was to reopen the context menu.
    //
    // SettingsManager::roomNotificationMode is Q_INVOKABLE, not a property,
    // so it cannot be bound — it is re-queried on the two events that can
    // change the answer for THIS row: the id changing under delegate reuse
    // (ListView.reuseItems is on, so a recycled row would otherwise keep the
    // previous room's mute state), and the settings manager announcing a
    // write. Same discipline the notifications flyout below already uses.
    // The lookup itself is cheap: SettingsManager caches the account slug
    // precisely because this runs on a hot path.
    readonly property string roomId: model.roomId || ""
    property int notificationMode: 0
    readonly property bool muted: notificationMode === 2
    function refreshNotificationMode() {
        // Guarded like Avatar/PresenceDot's `bridge` lookups (the 30ee39b
        // precedent): a delegate created synchronously from inside a
        // property-change handler can see `app` undefined on its first
        // context lookup, and an unmuted-by-accident row is a lie about a
        // setting the user changed on purpose. Absent service => leave the
        // mode at its last known value rather than asserting "not muted".
        if (typeof app === "undefined" || !app || !app.settings)
            return
        notificationMode = roomId.length > 0
                           ? app.settings.roomNotificationMode(roomId) : 0
    }
    onRoomIdChanged: refreshNotificationMode()
    Component.onCompleted: refreshNotificationMode()
    Connections {
        target: app.settings
        function onRoomNotificationModeChanged(changedRoomId) {
            if (changedRoomId === root.roomId)
                root.refreshNotificationMode()
        }
    }

    // An invitation and a pending knock are the two rows that want the eye
    // before anything else in the column. A tinted chip says so without
    // spending the accent — which the rail, the wordmark and the selected
    // room already carry.
    readonly property bool needsAttention:
        model.membership === "invited" || model.membership === "knocked"

    // The count pill and the muted-room dot both need to know whether there
    // is a number to show, and the pill is what the marked-unread dot stands
    // in for — one predicate so they can never both render.
    readonly property bool hasCountBadge:
        model.unreadCount > 0 || model.highlightCount > 0

    // Element's room-list recency: a clock time only while "today" is still
    // true. Before this the format string was a bare "hh:mm", so a room last
    // active three weeks ago read as though it had just spoken — the exact
    // job a sidebar timestamp exists to do, not done. Kept byte-identical to
    // HomePane.activityLabel(): the two surfaces show the same rooms and had
    // drifted into two different formats.
    function activityLabel(when) {
        if (!when || isNaN(when.getTime()) || when.getTime() <= 0)
            return ""
        var now = new Date()
        var days = Math.floor((now - when) / 86400000)
        if (when.toDateString() === now.toDateString())
            // ONE clock format for the whole application (Settings ->
            // Appearance): 24-hour, 12-hour, or the system's. The setting
            // resolves to a Qt format string on the C++ side, so nothing
            // here has to know what "12-hour" spells. Read as a PROPERTY —
            // a settings HELPER call would create no dependency anywhere.
            //
            // Honest limitation: this label is produced by a function, so
            // it re-renders when its caller's binding next does rather than
            // the instant the format changes. That is exactly what the
            // locale read it replaces already did.
            return Qt.formatTime(when, app.settings.clockTimeFormat)
        if (days < 2) return qsTr("Yesterday")
        if (days < 7) return Qt.formatDate(when, "ddd")
        if (when.getFullYear() === now.getFullYear())
            return Qt.formatDate(when, "d MMM")
        return Qt.formatDate(when, "MMM yyyy")
    }

    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: AppTheme.spacing4
        anchors.rightMargin: AppTheme.spacing4
        // The group rule below occupies the row's last pixel line. Yielding
        // it keeps the rule off the selection chip entirely: drawn ON it, a
        // hairline in `border` against a `selected` fill is very nearly
        // invisible, which is what "the divider disappears when the room is
        // selected" looked like.
        anchors.bottomMargin: root.showGroupDivider ? 1 : 0
        // v0.5.9: softer selected state from the semantic tokens — the
        // selected row keeps readable primary/secondary ink in both themes.
        // Design shell: row highlight is an 8px rounded chip, not a full-
        // bleed square.
        radius: AppTheme.radiusMd
        color: selected ? (hover.hovered ? AppTheme.selectedHover : AppTheme.selected)
             : hover.hovered ? AppTheme.hover
             : root.needsAttention ? AppTheme.chipAccentFill
             : "transparent"
        HoverHandler { id: hover }
        TapHandler { onTapped: root.clicked() }
    }

    // v0.6.5 live-feedback: "deliberate yellow ... selected-room edge" —
    // a left edge bar marking the active room. Sits in the 4px gutter the
    // rounded highlight chip above already leaves before its own
    // anchors.leftMargin inset, so it needs no change to the row's own
    // content margins.
    // 2026-08-21: rounded and inset instead of a full-height square rule —
    // a hard 3px bar butted against the row's top and bottom edges read as
    // a table border between rows rather than as a marker on one of them.
    Rectangle {
        visible: selected
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: 3
        height: Math.max(16, parent.height - AppTheme.spacing12)
        radius: width / 2
        color: AppTheme.bolt
    }

    // Drawn INSIDE the row's own bounds rather than by growing it, so
    // turning the rule on cannot shift the rows below it by a pixel.
    // Inset to the row's content box so it lines up with the avatar column
    // instead of butting into the sidebar's own right-hand border.
    Rectangle {
        objectName: "roomGroupDivider"
        // Above every other layer in the row, so nothing added later can
        // paint over it.
        z: 1
        visible: root.showGroupDivider
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: AppTheme.spacing8
        anchors.rightMargin: AppTheme.spacing8
        height: 1
        color: AppTheme.borderStrong
    }

    RowLayout {
        id: content
        anchors.fill: parent
        anchors.leftMargin: AppTheme.spacing8
        anchors.rightMargin: AppTheme.spacing8
        anchors.topMargin: AppTheme.spacing6
        anchors.bottomMargin: AppTheme.spacing6
        spacing: AppTheme.spacing10

        Avatar {
            size: 32
            name: model.name || ""
            mxc: model.avatarUrl || ""
            // One fallback-colour policy: an unambiguous DM is coloured as
            // the PERSON (their MXID), matching their message rows and
            // receipt chips everywhere else.
            colorKey: model.identityColorKey || model.roomId || ""
            // Design shell: people are circles, rooms and Spaces are
            // rounded squares that show a "#" glyph until the avatar loads.
            circle: model.isDirect === true
            // A muted room recedes as a whole, avatar included — the glyph
            // and the grey pill alone left a full-colour disc shouting from
            // a row that is supposed to be quiet.
            opacity: root.muted && !selected ? 0.55 : 1.0
            // Invite rows grow a third line (Accept/Reject): keep the
            // avatar with the room name at the top instead of letting it
            // float between the text lines.
            Layout.alignment: model.membership === "invited"
                              ? Qt.AlignTop : Qt.AlignVCenter
            Layout.topMargin: model.membership === "invited" ? 2 : 0

            // v0.7.x Matrix presence on DM rows. Only an unambiguous 1:1
            // DM shows a dot — identityColorKey is the partner's MXID
            // exactly in that case (see RoomInfo::identityColorKey), so
            // reusing it here keeps "whose presence" and "whose colour"
            // the same decision. Unknown presence renders nothing.
            PresenceDot {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: -1
                dotSize: 10
                // The dot's ring has to be the colour actually BEHIND it or
                // it reads as a halo: this column is the sidebar, not the
                // `surface` the default assumed, and a selected row paints
                // its own chip under the avatar.
                ring: selected ? AppTheme.selected : AppTheme.sidebar
                userId: model.isDirect === true
                        && (model.identityColorKey || "").charAt(0) === "@"
                        ? model.identityColorKey : ""
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing6
                Label {
                    text: model.name
                    // Read rows dim to secondary ink; unread/selected keep
                    // full primary ink (handoff §2). A muted room never
                    // claims the unread emphasis — that is what muting it
                    // asked for.
                    color: selected ? AppTheme.selectedText
                         : (root.isUnread && !root.muted) ? AppTheme.textPrimary
                                                          : AppTheme.textSecondary
                    font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                    font.weight: (selected || (root.isUnread && !root.muted))
                                 ? AppTheme.weightBold : AppTheme.weightMedium
                    elide: Label.ElideRight
                    Layout.fillWidth: true
                }
                // Unified inbox: which bridged network this conversation
                // arrives over. The model returns an empty label for a
                // native Matrix room, so nothing renders and the row is
                // exactly as it was — a badge appears only where it says
                // something the room name does not.
                Label {
                    objectName: "roomNetworkTag"
                    visible: (model.networkLabel || "") !== ""
                    text: model.networkLabel || ""
                    color: selected ? AppTheme.selectedText : AppTheme.textMuted
                    font.pixelSize: AppTheme.fontCaption
                    font.weight: Font.DemiBold
                    leftPadding: 5; rightPadding: 5
                    background: Rectangle {
                        color: AppTheme.hover
                        border.color: AppTheme.border
                        border.width: 1
                        radius: AppTheme.radiusSm
                    }
                }
                Icon {
                    visible: model.encrypted === true
                    name: "lock"
                    size: 12
                    color: selected ? AppTheme.selectedText : AppTheme.textMuted
                }
                // "There is a call in this room", on the title line with the
                // lock and the mute glyph — the row's other state marks.
                // Collapses to zero width when there is no call, so a row
                // without one is pixel-identical to what it was.
                RoomCallGlyph {
                    objectName: "roomCallGlyph"
                    roomId: model.roomId
                    glyphSize: 13
                    color: selected ? AppTheme.selectedText : AppTheme.textMuted
                }
                // The visible half of the mute state. Decorative here — the
                // row's Accessible.name carries the word.
                Icon {
                    objectName: "roomMutedGlyph"
                    visible: root.muted
                    name: "notifications_off"
                    size: 13
                    color: selected ? AppTheme.selectedText : AppTheme.textMuted
                    Accessible.ignored: true
                }
                Label {
                    objectName: "roomActivityLabel"
                    visible: model.lastActivity
                             && model.lastActivity.toString() !== ""
                             && text.length > 0
                    text: root.activityLabel(model.lastActivity)
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    font.weight: (root.isUnread && !root.muted)
                                 ? AppTheme.weightStrong : AppTheme.weightBody
                    color: selected ? AppTheme.selectedText
                         : (root.isUnread && !root.muted)
                           ? AppTheme.textSecondary : AppTheme.textMuted
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing6

                Label {
                    objectName: "roomPreviewLabel"
                    text: model.lastMessagePreview
                    color: selected ? AppTheme.selectedText : AppTheme.textMuted
                    opacity: selected ? 0.9 : 1.0
                    // The same emoji-capable font the composer and the message body
                    // take: a preview IS message text, so it hits the identical Qt 6.8
                    // fallback that renders emoji monochrome. Fourth call site of this
                    // — each fix hid the next.
                    font: app.textFontWithEmoji(
                              AppTheme.uiFont,
                              AppTheme.scaled(AppTheme.textMeta))
                    elide: Label.ElideRight
                    // Hard one-line guarantee: the summary layer normalizes
                    // newlines away, but a persisted pre-normalization preview
                    // (or any future producer bug) must still never expand the
                    // row — explicit '\n's would otherwise break lines even
                    // with elide set. Plain text: a message body must never
                    // rich-format the room list.
                    maximumLineCount: 1
                    wrapMode: Text.NoWrap
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                }

                // The count pill lives on the PREVIEW line (Element's
                // layout), not beside the timestamp. Sharing a row with the
                // clock meant every arriving message — and every 9→10→100
                // digit change — slid the timestamp sideways, so a quiet
                // sidebar was in constant low-level motion.
                Label {
                    objectName: "roomUnreadBadge"
                    visible: root.hasCountBadge
                    text: model.highlightCount > 0 ? model.highlightCount
                                                   : model.unreadCount
                    // Mention pills keep their colour even when the room is
                    // muted — muting a room silences the noise, it does not
                    // hide that somebody named you. A muted plain count
                    // drops to an outline pill in muted ink, which is what
                    // Element does and what makes the mute legible at a
                    // glance.
                    color: model.highlightCount > 0 ? AppTheme.dangerText
                         : root.muted ? root.metaInk
                                      : AppTheme.accentText
                    background: Rectangle {
                        color: model.highlightCount > 0 ? AppTheme.mentionBadge
                             : root.muted ? "transparent"
                                          : AppTheme.unreadBadge
                        border.width: root.muted && model.highlightCount === 0
                                      ? 1 : 0
                        border.color: AppTheme.chipNeutralBorder
                        radius: AppTheme.radiusPill
                    }
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: AppTheme.spacing6
                    rightPadding: AppTheme.spacing6
                    font.pixelSize: AppTheme.textMicro
                    font.weight: AppTheme.weightBold
                    // A single digit renders as a circle rather than the
                    // squat lozenge 14px of side padding used to produce.
                    Layout.preferredHeight: 18
                    Layout.minimumWidth: 18
                }

                // Marked-unread dot. Shown only when there is no numeric
                // badge, so it can never overlap the count pill it stands in
                // for. v0.6.5: reads unreadBadge (the same token the numeric
                // badge already uses), not accent — this is an unread
                // indicator, not a selection/focus/primary-action moment.
                // 2026-08-21: moved inline next to the pill it substitutes
                // for, instead of floating on the row's right edge where it
                // sat at a different x than the badge.
                Rectangle {
                    visible: model.markedUnread && !root.hasCountBadge
                    Layout.preferredWidth: 8
                    Layout.preferredHeight: 8
                    Layout.alignment: Qt.AlignVCenter
                    radius: 4
                    color: root.muted ? root.metaInk : AppTheme.unreadBadge
                }
            }

            RowLayout {
                visible: model.membership === "invited"
                spacing: AppTheme.spacingS
                Label {
                    Layout.fillWidth: true
                    text: model.inviter
                          ? (model.isSpace
                             ? qsTr("Space invitation from %1").arg(model.inviter)
                             : qsTr("Room invitation from %1").arg(model.inviter))
                          : (model.isSpace ? qsTr("Space invitation")
                                           : qsTr("Room invitation"))
                    color: root.metaInk
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    elide: Label.ElideRight
                }
                AppButton {
                    kind: "primary"
                    implicitHeight: AppTheme.buttonHeightSm
                    leftPadding: AppTheme.buttonPaddingHSm
                    rightPadding: AppTheme.buttonPaddingHSm
                    text: qsTr("Accept")
                    enabled: !model.invitePending
                    Accessible.name: qsTr("Accept room invitation")
                    onClicked: root.acceptInvite()
                }
                AppButton {
                    kind: "danger"
                    implicitHeight: AppTheme.buttonHeightSm
                    leftPadding: AppTheme.buttonPaddingHSm
                    rightPadding: AppTheme.buttonPaddingHSm
                    text: qsTr("Reject")
                    enabled: !model.invitePending
                    Accessible.name: qsTr("Reject room invitation")
                    onClicked: root.rejectInvite()
                }
            }
            Label {
                visible: model.inviteError && model.inviteError.length > 0
                text: model.inviteError || ""
                color: AppTheme.error
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightTight
                lineHeightMode: Text.ProportionalHeight
                Layout.fillWidth: true
            }

            // v0.7.x room upgrades: this room was replaced and the user can
            // actually reach the replacement — the model's role already
            // requires the successor to be joined or invited AND to point
            // back at this room. The row stays fully interactive: it is
            // demoted in the ordering and labelled, never hidden, because
            // the old room remains readable and its permalinks keep working.
            StatusChip {
                objectName: "roomUpgradedChip"
                visible: model.supersededByAccessibleSuccessor === true
                label: qsTr("Upgraded")
                tone: "neutral"
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("This room has been upgraded")
            }

            // v0.7.x: pending knock. The knocked room flows into the list
            // via sliding sync; the only honest actions are waiting and
            // withdrawing the request.
            RowLayout {
                visible: model.membership === "knocked"
                spacing: AppTheme.spacingS
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Join request pending")
                    color: root.metaInk
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    elide: Label.ElideRight
                }
                AppButton {
                    implicitHeight: AppTheme.buttonHeightSm
                    leftPadding: AppTheme.buttonPaddingHSm
                    rightPadding: AppTheme.buttonPaddingHSm
                    text: qsTr("Withdraw")
                    Accessible.name: qsTr("Withdraw the join request")
                    onClicked: app.discovery.cancelKnock(model.roomId)
                }
            }
        }
    }

    // Read/unread context menu is a joined-room-only affordance; it must not
    // appear for invitations (which offer Accept/Reject instead).
    TapHandler {
        acceptedButtons: Qt.RightButton
        enabled: model.membership === "joined"
        onTapped: roomMenu.popup()
    }
    // v0.6.5 (SPEC 1d): keyboard-operable open path, joined-only like the
    // right-click gate above.
    Keys.onPressed: (event) => {
        if (model.membership === "joined"
            && (event.key === Qt.Key_Menu
                || (event.key === Qt.Key_F10
                    && (event.modifiers & Qt.ShiftModifier)))) {
            roomMenu.popup()
            event.accepted = true
        }
    }
    // The menu itself lives in RoomActionsMenu.qml — ONE menu, used by this
    // row and by the Channels layout's row, which had none at all.
    RoomActionsMenu {
        id: roomMenu
        objectName: "roomContextMenu"
        roomId: model.roomId
        roomName: model.name || ""
        canonicalAlias: model.canonicalAlias || ""
        isDirect: model.isDirect === true
        isFavourite: model.isFavourite === true
        onMarkRead: root.markRead()
        onMarkUnread: root.markUnread()
        onSetFavourite: on => root.setFavourite(on)
        onSetNotificationMode: mode => root.setNotificationMode(mode)
        onCopyRoomLink: root.copyRoomLink()
        onLeaveRoomRequested: root.leaveRoomRequested()
    }

    // Design shell: no per-row hairline — rows separate through spacing
    // and hover/selection tints only.
}
