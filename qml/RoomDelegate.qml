import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    implicitHeight: content.implicitHeight + AppTheme.spacing6 * 2
    Accessible.role: Accessible.ListItem
    Accessible.name: model.membership === "invited"
                     ? qsTr("Invitation to %1").arg(model.name)
                     : (model.highlightCount > 0
                        ? qsTr("%1, %2 mentions").arg(model.name).arg(model.highlightCount)
                        : model.name)
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
    signal clicked()
    signal acceptInvite()
    signal rejectInvite()
    signal markRead()
    signal markUnread()
    // v0.6.5 (SPEC 1d): the delegate stays signal-only for every mutation —
    // the host (RoomsPanel) performs the actual app.settings/app.roomInfo
    // calls, exactly like markRead/markUnread today.
    signal setNotificationMode(int mode)
    signal copyRoomLink()
    signal leaveRoomRequested()

    // Read rows are dimmed and lighter-weight; unread/selected rows carry
    // full ink (design handoff §2 room-row states).
    readonly property bool isUnread: model.hasUnread || model.markedUnread

    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: AppTheme.spacing4
        anchors.rightMargin: AppTheme.spacing4
        // v0.5.9: softer selected state from the semantic tokens — the
        // selected row keeps readable primary/secondary ink in both themes.
        // Design shell: row highlight is an 8px rounded chip, not a full-
        // bleed square.
        radius: AppTheme.radiusMd
        color: selected ? (hover.hovered ? AppTheme.selectedHover : AppTheme.selected)
             : hover.hovered ? AppTheme.hover
             : "transparent"
        HoverHandler { id: hover }
        TapHandler { onTapped: root.clicked() }
    }

    // v0.6.5 live-feedback: "deliberate yellow ... selected-room edge" —
    // a left edge bar marking the active room. Sits in the 4px gutter the
    // rounded highlight chip above already leaves before its own
    // anchors.leftMargin inset, so it needs no change to the row's own
    // content margins.
    Rectangle {
        visible: selected
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 3
        color: AppTheme.bolt
    }

    RowLayout {
        id: content
        anchors.fill: parent
        anchors.leftMargin: AppTheme.spacing8
        anchors.rightMargin: AppTheme.spacing8
        anchors.topMargin: AppTheme.spacing6
        anchors.bottomMargin: AppTheme.spacing6
        spacing: AppTheme.spacing8

        Avatar {
            size: 30
            name: model.name || ""
            mxc: model.avatarUrl || ""
            colorKey: model.roomId || ""
            // Design shell: people are circles, rooms and Spaces are
            // rounded squares that show a "#" glyph until the avatar loads.
            circle: model.isDirect === true
            roomGlyph: model.isDirect !== true
            // Invite rows grow a third line (Accept/Reject): keep the
            // avatar with the room name at the top instead of letting it
            // float between the text lines.
            Layout.alignment: model.membership === "invited"
                              ? Qt.AlignTop : Qt.AlignVCenter
            Layout.topMargin: model.membership === "invited" ? 2 : 0
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: model.name
                    // Read rows dim to secondary ink; unread/selected keep
                    // full primary ink (handoff §2).
                    color: selected ? AppTheme.selectedText
                         : root.isUnread ? AppTheme.textPrimary
                                         : AppTheme.textSecondary
                    font.pixelSize: AppTheme.scaled(AppTheme.fontBody)
                    font.weight: (root.isUnread || selected)
                                 ? Font.Bold : Font.Medium
                    elide: Label.ElideRight
                    Layout.fillWidth: true
                }
                Icon {
                    visible: model.encrypted === true
                    name: "lock"
                    size: 12
                    color: selected ? AppTheme.selectedText : AppTheme.textMuted
                }
                Label {
                    visible: model.lastActivity && model.lastActivity.toString() !== ""
                    text: visible ? Qt.formatDateTime(model.lastActivity, "hh:mm") : ""
                    font.pixelSize: AppTheme.fontCaption
                    color: selected ? AppTheme.selectedText : AppTheme.textMuted
                }
                Label {
                    visible: model.unreadCount > 0 || model.highlightCount > 0
                    text: model.highlightCount > 0 ? model.highlightCount : model.unreadCount
                    // Mention pills use white ink on the red badge; plain
                    // unread pills invert to the row background colour on the
                    // accent fill (handoff §2 count pill).
                    color: model.highlightCount > 0 ? AppTheme.dangerText
                                                    : AppTheme.accentText
                    background: Rectangle {
                        color: model.highlightCount > 0 ? AppTheme.mentionBadge
                                                        : AppTheme.unreadBadge
                        radius: AppTheme.radiusPill
                    }
                    leftPadding: 7; rightPadding: 7; topPadding: 1; bottomPadding: 1
                    font.pixelSize: AppTheme.fontCaption
                    font.weight: Font.ExtraBold
                }
            }
            Label {
                objectName: "roomPreviewLabel"
                text: model.lastMessagePreview
                color: selected ? AppTheme.selectedText : AppTheme.textMuted
                opacity: selected ? 0.9 : 1.0
                font.pixelSize: AppTheme.scaled(AppTheme.fontMessageSender)
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

            RowLayout {
                visible: model.membership === "invited"
                spacing: AppTheme.spacingS
                Label {
                    Layout.fillWidth: true
                    text: (model.isSpace ? qsTr("Space invitation") : qsTr("Room invitation"))
                          + (model.inviter ? qsTr(" from %1").arg(model.inviter) : "")
                    color: AppTheme.textMuted
                    elide: Label.ElideRight
                }
                AppButton {
                    kind: "primary"
                    implicitHeight: 26
                    leftPadding: 10
                    rightPadding: 10
                    text: qsTr("Accept")
                    enabled: !model.invitePending
                    Accessible.name: qsTr("Accept room invitation")
                    onClicked: root.acceptInvite()
                }
                AppButton {
                    kind: "danger"
                    implicitHeight: 26
                    leftPadding: 10
                    rightPadding: 10
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
                wrapMode: Text.WordWrap
            }
        }
    }

    // Marked-unread dot. Shown only when there is no numeric badge, so it
    // can never overlap the unread/highlight badge. v0.6.5: reads
    // unreadBadge (the same token the numeric badge it substitutes for
    // already uses), not accent — this is an unread indicator, not a
    // selection/focus/primary-action moment.
    Rectangle {
        visible: model.markedUnread && model.unreadCount === 0
                 && model.highlightCount === 0
        width: 8; height: 8; radius: 4
        color: AppTheme.unreadBadge
        anchors.right: parent.right
        anchors.rightMargin: AppTheme.spacingS
        anchors.verticalCenter: parent.verticalCenter
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
    AppMenu {
        id: roomMenu
        objectName: "roomContextMenu"
        menuWidth: AppTheme.menuWidthRoom
        // Storm §4 2b: mono room-address header. Mono is for ADDRESSES —
        // use the canonical alias when the room has one; otherwise the
        // display name WITHOUT a fabricated # prefix.
        contextLabel: model.isDirect === true
                      ? (model.name || "")
                      : ((model.canonicalAlias || "").length > 0
                         ? model.canonicalAlias : (model.name || ""))
        AppMenuItem {
            iconName: "check"
            text: qsTr("Mark as read")
            onTriggered: root.markRead()
        }
        AppMenuItem {
            iconName: "visibility_off"
            text: qsTr("Mark as unread")
            onTriggered: root.markUnread()
        }
        AppMenuSeparator {}
        // v0.6.5 (SPEC 1d): per-room notification mode, three radio rows
        // bound to the REAL setting (SettingsManager::roomNotification-
        // Mode is Q_INVOKABLE, not a property, so it is re-queried explicitly
        // rather than bound directly — see refreshMode() below). radioSelected
        // stays a pure binding on the local currentMode property; it is never
        // imperatively assigned (AppMenuItem itself never self-toggles it).
        // On backends with server push-rule support the setting doubles as
        // the cache of the account's server mode (see AppController).
        AppMenu {
            id: notificationsFlyout
            objectName: "roomNotificationsFlyout"
            title: qsTr("Notifications")
            submenuIconName: "notifications"
            menuWidth: AppTheme.menuWidthFlyout
            // Storm §4 2b: flyout header is a bare mono caption, no bolt.
            contextLabel: qsTr("Notify mode")
            contextBolt: false
            property int currentMode: 0
            // True while the room's last server push-rule write failed —
            // the disclaimer then says the mode was kept on this device
            // instead of claiming it was saved to the account.
            property bool syncFailed: false
            function refreshMode() {
                currentMode = app.settings.roomNotificationMode(model.roomId)
                syncFailed = app.roomNotificationModeSyncFailed(model.roomId)
            }
            onAboutToShow: {
                refreshMode()
                // Poll-on-open: re-query the server rule so a change made
                // in another client lands in the cache (and, via the
                // Connections below, in this flyout). A guarded no-op on
                // backends without server push-rule support.
                app.requestRoomNotificationMode(model.roomId)
            }
            Connections {
                target: app.settings
                function onRoomNotificationModeChanged(roomId) {
                    if (roomId === model.roomId)
                        notificationsFlyout.refreshMode()
                }
            }
            Connections {
                target: app
                function onRoomNotificationModeSyncStateChanged(roomId) {
                    if (roomId === model.roomId)
                        notificationsFlyout.refreshMode()
                }
            }
            AppMenuItem {
                text: qsTr("All messages")
                radio: true
                radioSelected: notificationsFlyout.currentMode === 0
                onTriggered: root.setNotificationMode(0)
            }
            AppMenuItem {
                // "& keywords" is what the rule actually does: the SDK's
                // MentionsAndKeywordsOnly mode keeps keyword rules firing.
                text: qsTr("Mentions & keywords")
                radio: true
                radioSelected: notificationsFlyout.currentMode === 1
                onTriggered: root.setNotificationMode(1)
            }
            AppMenuItem {
                text: qsTr("Muted")
                radio: true
                radioSelected: notificationsFlyout.currentMode === 2
                onTriggered: root.setNotificationMode(2)
            }
            // v0.7: the same "follow account default" choice Room
            // Information offers. Without it a room set to mode 3 shows NO
            // selected radio here — two entry points to one setting
            // disagreeing, with this one rendering a state it cannot
            // express. Server-capable backends only: with a device-local
            // backend there is no account rule to defer to.
            AppMenuItem {
                visible: app.serverRoomNotificationModes
                text: qsTr("Follow account default")
                radio: true
                radioSelected: notificationsFlyout.currentMode === 3
                onTriggered: root.setNotificationMode(3)
            }
            Label {
                objectName: "roomNotificationDisclaimer"
                leftPadding: AppTheme.menuItemPadding
                rightPadding: AppTheme.menuItemPadding
                topPadding: AppTheme.spacing4
                bottomPadding: AppTheme.spacing6
                // Backend-honest: the Rust backend writes the account's
                // server push rules through the SDK ("saved", not
                // continuously synced — there is no live push-rule watcher
                // yet); a failed write is admitted instead of claimed
                // saved; other backends keep the mode strictly
                // device-local.
                // v0.7: a failed write is now retried on the next
                // reconnection, so the failure line says so. It still
                // admits the failure first — the retry is a promise to try
                // again, never a claim that the rule was saved.
                text: app.serverRoomNotificationModes
                      ? (notificationsFlyout.syncFailed
                         ? qsTr("Couldn't save to the server — "
                                + "kept on this device. "
                                + "Retried when you reconnect.")
                         : qsTr("Saved to your account's notification "
                                + "settings (server push rules)."))
                      : qsTr("Local setting: it does not change this "
                             + "room's server push rules.")
                color: AppTheme.stormTextFaint
                font.pixelSize: AppTheme.fontMicro
                wrapMode: Text.WordWrap
            }
        }
        AppMenuSeparator {}
        AppMenuItem {
            iconName: "link"
            text: qsTr("Copy room link")
            onTriggered: root.copyRoomLink()
        }
        AppMenuItem {
            iconName: "logout"
            text: qsTr("Leave room")
            danger: true
            onTriggered: root.leaveRoomRequested()
        }
    }

    // Design shell: no per-row hairline — rows separate through spacing
    // and hover/selection tints only.
}
