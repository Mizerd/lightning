import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

// Space settings: one modal for everything Lightning can change about a Space,
// from the rail's menu and Space Home.
//
// A Space is a Matrix room: every control writes ordinary room state through
// RoomInfoController, gated on the real required power level for that event,
// and never applied optimistically (the roster is re-read after each write).
//
// Not offered: name colours, fonts and per-Space appearance, which are not
// Matrix state and would be private storage no other client or device sees.
// Sticker packs (im.ponies.room_emotes, MSC2545) are real state and simply not
// built here yet. The banner is a real state event
// (page.codeberg.everypizza.room.banner, rust/src/banner.rs) chosen so other
// clients read it.
AppDialog {
    id: root
    objectName: "spaceSettingsDialog"

    /// The Space being edited; never a plain room.
    property string spaceId: ""
    /// Where app.roomInfo pointed when this opened, restored on close: the
    /// controller is shared with Room Information and Space Home.
    property string _restoreRoomId: ""
    property int section: 0

    /// Bumped on every roster answer. Read it in any binding that calls a
    /// Q_INVOKABLE on app.roomInfo (filterMembers, memberRoleGroups,
    /// powerLevelForKey, roleLabelForLevel): a method call creates no
    /// dependency, and an unused local read is enough to create one.
    property int rosterTick: 0

    // No title: this dialog draws its own header.
    title: ""
    modal: true
    parent: Overlay.overlay
    anchors.centerIn: parent
    standardButtons: Dialog.NoButton
    width: Math.min(940, Overlay.overlay ? Overlay.overlay.width - 80 : 940)
    height: Math.min(660, Overlay.overlay ? Overlay.overlay.height - 80 : 660)

    /// Bumped on every SpaceManager change; read in `info`, which calls
    /// app.spaces.spaceInfo() and would otherwise depend only on spaceId, so
    /// remote renames and avatar/topic changes would never reach the dialog.
    property int spacesTick: 0
    readonly property var info: {
        var _dep = root.spacesTick
        return app.spaces && spaceId.length > 0
               ? app.spaces.spaceInfo(spaceId) : ({})
    }
    // True only when the roster on screen is this Space's; app.roomInfo is
    // shared, and another room's permissions must not gate these controls.
    readonly property bool infoIsOurs:
        app.roomInfo && app.roomInfo.roomId === root.spaceId

    function openFor(targetSpaceId) {
        if (!targetSpaceId || targetSpaceId.length === 0)
            return
        spaceId = targetSpaceId
        _restoreRoomId = app.roomInfo ? app.roomInfo.roomId : ""
        if (app.roomInfo)
            app.roomInfo.roomId = targetSpaceId
        section = 0
        memberError = ""
        memberFilter.text = ""
        membershipCombo.currentIndex = 0
        sortCombo.currentIndex = 0
        // Re-snap every mirrored field on each open: onSpaceIdChanged does not
        // fire when reopening the same Space.
        nameField.resetForSpace()
        topicField.resetForSpace()
        aliasField.resetForSpace()
        joinRuleCombo.refreshRule()
        // Refresh, not request: the banner is a custom state event that sliding
        // sync does not deliver, so read it on every open (a deliberate action
        // on one Space).
        if (app.banners)
            app.banners.refreshRoom(targetSpaceId)
        // Asked once per session, so the delete is offered only to a server
        // administrator.
        if (app.roomClosure)
            app.roomClosure.checkServerAdmin()
        open()
    }

    onClosed: {
        if (app.roomInfo && app.roomInfo.roomId === root.spaceId)
            app.roomInfo.roomId = root._restoreRoomId
        _restoreRoomId = ""
    }

    readonly property var sections: [
        { key: "general", label: qsTr("General"), icon: "settings" },
        { key: "members", label: qsTr("Members"), icon: "person" },
        { key: "permissions", label: qsTr("Permissions"), icon: "lock" },
        { key: "developer", label: qsTr("Developer tools"), icon: "code" }
    ]

    /// The Permissions matrix. Every key must also be in
    /// RoomInfoController::powerLevelKeys() and the allowlist in
    /// rooms::set_room_power_level_key; Rust refuses anything else.
    /// m.call.member is deliberately absent: Lightning sends the MSC3401
    /// unstable identifier and a Space has no timeline for calls.
    /// m.room.power_levels has one row, not two.
    readonly property var permissionGroups: [
        {
            title: qsTr("Users"),
            // Notes are group-level: a per-row Label whose text can be "" would
            // keep ItemObservesViewport forever.
            note: qsTr("The level every member starts at. Raising it grants "
                       + "EVERYONE in the space everything at that level."),
            rows: [
                { key: "users_default", label: qsTr("Default power") }
            ]
        },
        {
            title: qsTr("Manage"),
            note: "",
            rows: [
                { key: "m.space.child", label: qsTr("Manage space rooms") },
                { key: "events_default", label: qsTr("Message events") }
            ]
        },
        {
            title: qsTr("Moderation"),
            // Matrix has no separate unban level (it is max(ban, kick)).
            note: "",
            rows: [
                { key: "invite", label: qsTr("Invite") },
                { key: "kick", label: qsTr("Kick") },
                { key: "ban", label: qsTr("Ban") },
                { key: "redact", label: qsTr("Remove messages") }
            ]
        },
        {
            title: qsTr("Space profile"),
            note: "",
            rows: [
                { key: "m.room.avatar", label: qsTr("Space avatar") },
                { key: "m.room.name", label: qsTr("Space name") },
                { key: "m.room.topic", label: qsTr("Space topic") }
            ]
        },
        {
            title: qsTr("Settings"),
            note: qsTr("Set \"Edit power levels\" above your own level and "
                       + "you can never lower it again."),
            rows: [
                { key: "m.room.join_rules",
                  label: qsTr("Change space access") },
                { key: "m.room.canonical_alias",
                  label: qsTr("Publish address") },
                { key: "m.room.power_levels",
                  label: qsTr("Edit power levels") },
                { key: "m.room.tombstone", label: qsTr("Upgrade space") },
                { key: "state_default", label: qsTr("Other settings") }
            ]
        }
    ]

    contentItem: ColumnLayout {
        spacing: 0

        // Header: which Space, which section, and close
        RowLayout {
            Layout.fillWidth: true
            Layout.bottomMargin: AppTheme.spacing12
            spacing: AppTheme.spacing12

            Avatar {
                objectName: "spaceSettingsHeaderAvatar"
                size: 32
                circle: false
                squareRadius: AppTheme.radiusMd
                name: root.info.name || ""
                mxc: root.info.avatarUrl || ""
                colorKey: root.spaceId
            }
            Label {
                objectName: "spaceSettingsHeaderName"
                text: root.info.name || qsTr("Space")
                textFormat: Text.PlainText
                elide: Label.ElideRight
                Layout.maximumWidth: 260
                color: AppTheme.stormText
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textTitle
                font.weight: AppTheme.weightBold
            }
            Rectangle {
                implicitWidth: 1
                Layout.preferredHeight: 22
                color: AppTheme.stormBorder
            }
            Label {
                // Untrusted text: never markup.
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: root.sections[root.section].label
                color: AppTheme.stormTextSecondary
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textTitle
                font.weight: AppTheme.weightMedium
            }
            IconButton {
                objectName: "spaceSettingsCloseButton"
                iconName: "close"
                onClicked: root.close()
                Accessible.name: qsTr("Close space settings")
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: AppTheme.stormBorder
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: AppTheme.spacing16

            // Section nav
            ColumnLayout {
                Layout.preferredWidth: 200
                Layout.fillHeight: true
                Layout.topMargin: AppTheme.spacing12
                spacing: 2

                Repeater {
                    model: root.sections
                    delegate: Rectangle {
                        id: navRow
                        required property var modelData
                        required property int index
                        objectName: "spaceSettingsNav_" + modelData.key
                        Layout.fillWidth: true
                        implicitHeight: 34
                        radius: AppTheme.radiusMd
                        readonly property bool current: root.section === index
                        color: current ? AppTheme.stormSelection
                               : navHover.hovered ? AppTheme.hover
                                                  : "transparent"
                        HoverHandler {
                            id: navHover
                            cursorShape: Qt.PointingHandCursor
                        }
                        TapHandler { onTapped: root.section = navRow.index }
                        Accessible.role: Accessible.Button
                        Accessible.name: navRow.modelData.label
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: AppTheme.spacing12
                            anchors.rightMargin: AppTheme.spacing8
                            spacing: AppTheme.spacing8
                            Icon {
                                name: navRow.modelData.icon
                                size: 16
                                color: navRow.current ? AppTheme.bolt
                                                      : AppTheme.stormTextMuted
                            }
                            Label {
                                // Untrusted text: never markup.
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                text: navRow.modelData.label
                                elide: Label.ElideRight
                                color: navRow.current
                                       ? AppTheme.stormText
                                       : AppTheme.stormTextSecondary
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textBody
                                font.weight: AppTheme.weightMedium
                            }
                        }
                    }
                }
                Item { Layout.fillHeight: true }
            }

            Rectangle {
                Layout.fillHeight: true
                implicitWidth: 1
                color: AppTheme.stormBorder
            }

            // Section body
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth

                ColumnLayout {
                    width: parent ? parent.width : 0
                    spacing: AppTheme.spacing16

                    // Shown while the roster is not loaded: every gate below
                    // depends on it, and unexplained disabled controls look
                    // like a permission refusal.
                    Label {
                        Layout.fillWidth: true
                        Layout.topMargin: AppTheme.spacing12
                        visible: !root.infoIsOurs
                        wrapMode: Text.WordWrap
                        color: AppTheme.stormTextMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                        text: qsTr("Loading this space's members. What you "
                                   + "can change depends on them, so the "
                                   + "controls stay disabled until they "
                                   + "arrive.")
                    }

                    // ══ GENERAL ══
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: AppTheme.spacing12
                        visible: root.section === 0
                        spacing: AppTheme.spacing16

                        FileDialog {
                            id: spaceAvatarFile
                            title: qsTr("Choose space avatar")
                            fileMode: FileDialog.OpenFile
                            nameFilters: [qsTr("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp)")]
                            // The crop dialog decides what is uploaded and
                            // refuses non-raster files before rendering.
                            onAccepted: spaceAvatarCrop.openFor(selectedFile)
                        }
                        ImageCropDialog {
                            id: spaceAvatarCrop
                            role: "avatar"
                            // m.room.avatar, exactly as for a room.
                            onCropped: function (file) {
                                app.roomInfo.setRoomAvatar(file)
                            }
                        }
                        FileDialog {
                            id: spaceBannerFile
                            title: qsTr("Choose a banner image")
                            fileMode: FileDialog.OpenFile
                            nameFilters: [qsTr("Images (*.png *.jpg *.jpeg *.gif *.webp)")]
                            onAccepted: spaceBannerCrop.openFor(selectedFile)
                        }
                        ImageCropDialog {
                            id: spaceBannerCrop
                            role: "banner"
                            // Pass the URL as-is; stripping "file://" breaks
                            // Windows paths.
                            onCropped: function (file) {
                                app.banners.setRoomBanner(root.spaceId,
                                                          file.toString())
                            }
                        }

                        MenuSectionLabel { text: qsTr("Profile") }

                        Rectangle {
                            Layout.fillWidth: true
                            radius: AppTheme.radiusMd
                            color: AppTheme.stormInset
                            border.color: AppTheme.stormBorder
                            border.width: 1
                            implicitHeight: profileCol.implicitHeight
                                            + AppTheme.spacing16 * 2
                            ColumnLayout {
                                id: profileCol
                                anchors.fill: parent
                                anchors.margins: AppTheme.spacing16
                                spacing: AppTheme.spacing12

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing12
                                    Avatar {
                                        size: 56
                                        circle: false
                                        squareRadius: AppTheme.radiusMd
                                        name: root.info.name || ""
                                        mxc: root.info.avatarUrl || ""
                                        colorKey: root.spaceId
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: AppTheme.spacing8
                                        AppButton {
                                            objectName: "spaceSettingsChangeAvatar"
                                            storm: true
                                            size: "sm"
                                            text: qsTr("Change avatar…")
                                            enabled: root.infoIsOurs
                                                     && app.roomInfo.canEditAvatar
                                                     && !app.roomInfo.editPending
                                            onClicked: spaceAvatarFile.open()
                                        }
                                        AppButton {
                                            objectName: "spaceSettingsRemoveAvatar"
                                            storm: true
                                            size: "sm"
                                            kind: "danger"
                                            text: qsTr("Remove avatar")
                                            enabled: root.infoIsOurs
                                                     && app.roomInfo.canEditAvatar
                                                     && !app.roomInfo.editPending
                                            onClicked: app.roomInfo.removeRoomAvatar()
                                        }
                                    }
                                }

                                Label {
                                    text: qsTr("Name")
                                    color: AppTheme.stormTextMuted
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    AppTextField {
                                        id: nameField
                                        objectName: "spaceSettingsNameField"
                                        storm: true
                                        Layout.fillWidth: true
                                        // An explicit mirror: typing breaks a
                                        // binding, and the dialog can be reopened
                                        // on another Space.
                                        property string authoritative: ""
                                        function resetForSpace() {
                                            authoritative = root.info.name || ""
                                            text = authoritative
                                        }
                                        function refreshName() {
                                            var next = root.info.name || ""
                                            if (text === authoritative)
                                                text = next
                                            authoritative = next
                                        }
                                        enabled: root.infoIsOurs
                                                 && app.roomInfo.canEditName
                                        Accessible.name: qsTr("Space name")
                                    }
                                    AppButton {
                                        storm: true
                                        kind: "primary"
                                        text: qsTr("Rename")
                                        enabled: root.infoIsOurs
                                                 && app.roomInfo.canEditName
                                                 && !app.roomInfo.editPending
                                                 && nameField.text.trim().length > 0
                                                 && nameField.text.trim()
                                                    !== (root.info.name || "")
                                        onClicked: app.roomInfo.setRoomName(
                                                       nameField.text.trim())
                                    }
                                }

                                Label {
                                    text: qsTr("Topic")
                                    color: AppTheme.stormTextMuted
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    AppTextField {
                                        id: topicField
                                        objectName: "spaceSettingsTopicField"
                                        storm: true
                                        Layout.fillWidth: true
                                        property string authoritative: ""
                                        function resetForSpace() {
                                            authoritative = root.info.topic || ""
                                            text = authoritative
                                        }
                                        function refreshTopic() {
                                            var next = root.info.topic || ""
                                            if (text === authoritative)
                                                text = next
                                            authoritative = next
                                        }
                                        enabled: root.infoIsOurs
                                                 && app.roomInfo.canEditTopic
                                        Accessible.name: qsTr("Space topic")
                                    }
                                    AppButton {
                                        storm: true
                                        kind: "primary"
                                        text: qsTr("Save")
                                        // Same test as Rename, so both agree on
                                        // whether there is anything to save;
                                        // otherwise it would send an identical
                                        // m.room.topic. No length test: a topic
                                        // may be cleared, a name may not.
                                        enabled: root.infoIsOurs
                                                 && app.roomInfo.canEditTopic
                                                 && !app.roomInfo.editPending
                                                 && topicField.text.trim()
                                                    !== (root.info.topic || "")
                                        onClicked: app.roomInfo.setRoomTopic(
                                                       topicField.text.trim())
                                    }
                                }
                            }
                        }

                        // Banner: a real state event matching the reference
                        // client's type (see rust/src/banner.rs). Permission
                        // comes from the banner manager, since this custom type
                        // has its own required level.
                        MenuSectionLabel {
                            text: qsTr("Banner")
                            visible: bannerCard.visible
                        }

                        Rectangle {
                            id: bannerCard
                            objectName: "spaceSettingsBannerCard"
                            Layout.fillWidth: true
                            visible: !!app.banners
                            radius: AppTheme.radiusMd
                            color: AppTheme.stormInset
                            border.color: AppTheme.stormBorder
                            border.width: 1
                            implicitHeight: bannerCol.implicitHeight
                                            + AppTheme.spacing16 * 2

                            // `revision` is the manager's change counter; both
                            // answers are method calls.
                            readonly property string bannerMxc: {
                                if (!app.banners || root.spaceId === "")
                                    return ""
                                var _dep = app.banners.revision
                                return app.banners.roomBannerFor(root.spaceId)
                            }
                            readonly property bool canEdit: {
                                if (!app.banners || root.spaceId === "")
                                    return false
                                // False until the room replies, so the control
                                // is never offered on a guess.
                                var _dep = app.banners.revision
                                return app.banners.canSetRoomBanner(root.spaceId)
                            }

                            ColumnLayout {
                                id: bannerCol
                                anchors.fill: parent
                                anchors.margins: AppTheme.spacing16
                                spacing: AppTheme.spacing12

                                Rectangle {
                                    Layout.fillWidth: true
                                    // 3:1, the ratio ImageCropDialog crops to,
                                    // so the preview matches what was saved.
                                    implicitHeight: Math.round(width / 3)
                                    radius: AppTheme.radiusMd
                                    clip: true
                                    color: AppTheme.stormPanel
                                    Image {
                                        id: bannerPreview
                                        anchors.fill: parent
                                        fillMode: Image.PreserveAspectCrop
                                        sourceSize.width: 1200
                                        asynchronous: true
                                        visible: status === Image.Ready
                                        opacity: bannerPreviewMotion.shown ? 0 : 1
                                        readonly property string mxc:
                                            bannerCard.bannerMxc
                                        // A counter the binding reads, never an
                                        // assignment to `source`, which would
                                        // destroy the binding.
                                        property int resolveTick: 0
                                        source: {
                                            var _tick = resolveTick
                                            return mxc.length > 0
                                                   && app.mediaBridge.supported
                                                   ? app.mediaBridge.wideImageSource(mxc)
                                                   : ""
                                        }
                                        Connections {
                                            target: app.mediaBridge
                                            enabled: bannerPreview.mxc.length > 0
                                            function onMediaCached(key) {
                                                if (key.endsWith(":" + bannerPreview.mxc)
                                                    && bannerPreview.source.toString().length === 0)
                                                    bannerPreview.resolveTick++
                                            }
                                            // An expired transient failure mark:
                                            // re-ask, as Avatar.qml does. The bridge
                                            // re-arms the mark on failure, so this
                                            // cannot hammer the backend.
                                            function onMediaRetryable(key) {
                                                if (key.endsWith(":" + bannerPreview.mxc)
                                                    && bannerPreview.source.toString().length === 0)
                                                    bannerPreview.resolveTick++
                                            }
                                        }
                                    }
                                    BannerMotion {
                                        id: bannerPreviewMotion
                                        objectName: "spaceSettingsBannerMotion"
                                        anchors.fill: parent
                                        mxc: bannerPreview.mxc
                                        stillReady: bannerPreview.status === Image.Ready
                                    }
                                    // "No banner" only when true: the preview
                                    // is also invisible while loading and after
                                    // a failed fetch, and those render as the
                                    // empty panel.
                                    Label {
                                        objectName: "spaceSettingsNoBanner"
                                        anchors.centerIn: parent
                                        visible: bannerCard.bannerMxc.length === 0
                                        text: qsTr("No banner")
                                        color: AppTheme.stormTextMuted
                                        font.family: AppTheme.uiFont
                                        font.pixelSize: AppTheme.textMeta
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    AppButton {
                                        objectName: "spaceSettingsChangeBanner"
                                        storm: true
                                        size: "sm"
                                        text: bannerCard.bannerMxc.length > 0
                                              ? qsTr("Change banner…")
                                              : qsTr("Upload banner…")
                                        enabled: bannerCard.canEdit
                                                 && !app.banners.busy
                                        onClicked: spaceBannerFile.open()
                                    }
                                    AppButton {
                                        objectName: "spaceSettingsRemoveBanner"
                                        storm: true
                                        size: "sm"
                                        kind: "danger"
                                        text: qsTr("Remove banner")
                                        enabled: bannerCard.canEdit
                                                 && !app.banners.busy
                                                 && bannerCard.bannerMxc.length > 0
                                        onClicked: app.banners.clearRoomBanner(
                                                       root.spaceId)
                                    }
                                    Item { Layout.fillWidth: true }
                                }

                                // A refusal is reported in place; nothing was
                                // applied optimistically.
                                Label {
                                    Layout.fillWidth: true
                                    visible: !!app.banners
                                             && app.banners.lastError.length > 0
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormDanger
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    text: app.banners
                                          ? qsTr("The banner could not be "
                                                 + "saved (%1).")
                                                .arg(app.banners.lastError)
                                          : ""
                                }
                            }
                        }

                        MenuSectionLabel { text: qsTr("Options") }

                        Rectangle {
                            Layout.fillWidth: true
                            radius: AppTheme.radiusMd
                            color: AppTheme.stormInset
                            border.color: AppTheme.stormBorder
                            border.width: 1
                            implicitHeight: accessCol.implicitHeight
                                            + AppTheme.spacing16 * 2
                            ColumnLayout {
                                id: accessCol
                                anchors.fill: parent
                                anchors.margins: AppTheme.spacing16
                                spacing: AppTheme.spacing12

                                readonly property bool restricted:
                                    root.infoIsOurs
                                    && (app.roomInfo.joinRule === "restricted"
                                        || app.roomInfo.joinRule
                                           === "knock_restricted")

                                Label {
                                    text: qsTr("Space access")
                                    color: AppTheme.stormText
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightMedium
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    text: qsTr("Change how people can join "
                                               + "the space.")
                                    color: AppTheme.stormTextMuted
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                }
                                AppComboBox {
                                    id: joinRuleCombo
                                    objectName: "spaceSettingsJoinRuleCombo"
                                    Layout.fillWidth: true
                                    visible: !accessCol.restricted
                                    enabled: root.infoIsOurs
                                             && app.roomInfo.canChangeJoinRule
                                             && !app.roomInfo.editPending
                                    // Index order must match ruleValues.
                                    model: [
                                        qsTr("Invited people only"),
                                        qsTr("Anyone with the link"),
                                        qsTr("Ask to join (knock)")
                                    ]
                                    readonly property var ruleValues:
                                        ["invite", "public", "knock"]
                                    // An explicit mirror so a rejected write
                                    // snaps back to what the Space holds.
                                    property int displayedIndex: 0
                                    function refreshRule() {
                                        var idx = ruleValues.indexOf(
                                            root.infoIsOurs
                                            ? app.roomInfo.joinRule : "")
                                        displayedIndex = idx >= 0 ? idx : 0
                                    }
                                    currentIndex: displayedIndex
                                    onActivated: (index) => {
                                        app.roomInfo.setJoinRule(
                                            joinRuleCombo.ruleValues[index])
                                    }
                                }
                                // Only invite/public/knock are settable: a
                                // restricted rule needs an allow-rule list this
                                // surface cannot build.
                                Label {
                                    Layout.fillWidth: true
                                    visible: accessCol.restricted
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    text: qsTr("Members of another space can "
                                               + "join. Lightning can't change "
                                               + "space-restricted access yet.")
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    implicitHeight: 1
                                    color: AppTheme.stormBorder
                                }

                                Label {
                                    text: qsTr("Published address")
                                    color: AppTheme.stormText
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightMedium
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    text: qsTr("A published address lets "
                                               + "people find and join this "
                                               + "space by name. Leave it "
                                               + "empty to remove it.")
                                    color: AppTheme.stormTextMuted
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    AppTextField {
                                        id: aliasField
                                        objectName: "spaceSettingsAliasField"
                                        storm: true
                                        Layout.fillWidth: true
                                        placeholderText: qsTr("#space-name")
                                        enabled: root.infoIsOurs
                                                 && app.roomInfo.canChangeAlias
                                        property string authoritative: ""
                                        function resetForSpace() {
                                            authoritative =
                                                root.infoIsOurs
                                                ? app.roomInfo.canonicalAlias : ""
                                            text = authoritative
                                        }
                                        function refreshAlias() {
                                            var next = root.infoIsOurs
                                                ? app.roomInfo.canonicalAlias : ""
                                            if (text === authoritative)
                                                text = next
                                            authoritative = next
                                        }
                                    }
                                    AppButton {
                                        storm: true
                                        kind: "primary"
                                        text: qsTr("Save")
                                        enabled: root.infoIsOurs
                                                 && app.roomInfo.canChangeAlias
                                                 && !app.roomInfo.editPending
                                                 && aliasField.text.trim()
                                                    !== app.roomInfo.canonicalAlias
                                        onClicked: app.roomInfo.setCanonicalAlias(
                                                       aliasField.text)
                                    }
                                }
                            }
                        }

                        // Advanced. The version is read from the SDK
                        // (Room::version()), never parsed from m.room.create.
                        MenuSectionLabel {
                            text: qsTr("Advanced")
                            visible: root.infoIsOurs
                                     && app.roomInfo.roomVersion.length > 0
                        }

                        Rectangle {
                            objectName: "spaceSettingsAdvancedCard"
                            Layout.fillWidth: true
                            visible: root.infoIsOurs
                                     && app.roomInfo.roomVersion.length > 0
                            radius: AppTheme.radiusMd
                            color: AppTheme.stormInset
                            border.color: AppTheme.stormBorder
                            border.width: 1
                            implicitHeight: advancedCol.implicitHeight
                                            + AppTheme.spacing16 * 2
                            ColumnLayout {
                                id: advancedCol
                                anchors.fill: parent
                                anchors.margins: AppTheme.spacing16
                                spacing: AppTheme.spacing8
                                Label {
                                    text: root.infoIsOurs
                                          ? qsTr("Current version: %1")
                                                .arg(app.roomInfo.roomVersion)
                                          : ""
                                    color: AppTheme.stormText
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightMedium
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: AppTheme.stormTextMuted
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                    text: root.infoIsOurs
                                          && app.roomInfo.canUpgradeRoom
                                          ? qsTr("Upgrading is irreversible: "
                                                 + "it creates a new space and "
                                                 + "marks this one as replaced. "
                                                 + "The rooms this space lists "
                                                 + "stay listed by the OLD "
                                                 + "space; re-add them to the "
                                                 + "new one afterwards.")
                                          : qsTr("Only someone allowed to send "
                                                 + "m.room.tombstone can "
                                                 + "upgrade this space.")
                                }
                                // Upgrade, gated on the tombstone power; opens
                                // a confirmation.
                                AppButton {
                                    objectName: "spaceUpgradeButton"
                                    // Not gated on app.roomUpgrade.upgraded,
                                    // which describes the active room, not this
                                    // Space.
                                    visible: root.infoIsOurs
                                             && app.roomInfo.canUpgradeRoom
                                    kind: "secondary"
                                    size: "sm"
                                    text: qsTr("Upgrade space…")
                                    onClicked: {
                                        spaceUpgradeDialog.kind = "space"
                                        spaceUpgradeDialog.openFor(
                                            app.roomInfo.roomId)
                                    }
                                }
                                RoomUpgradeDialog { id: spaceUpgradeDialog }
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: root.infoIsOurs
                                     && app.roomInfo.editError.length > 0
                            text: root.infoIsOurs ? app.roomInfo.editError : ""
                            color: AppTheme.stormDanger
                            wrapMode: Text.WordWrap
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                        }

                        // Close, and for a server administrator delete.
                        // Matrix has no delete for a room's own admins, so
                        // "close" is offered and never called one.
                        MenuSectionLabel {
                            Layout.fillWidth: true
                            Layout.topMargin: AppTheme.spacing12
                            visible: closeSpaceButton.visible
                                     || deleteSpaceButton.visible
                            text: qsTr("Close or delete")
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: closeSpaceButton.visible
                            wrapMode: Text.WordWrap
                            color: AppTheme.stormTextMuted
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("Closing makes the space and the rooms "
                                       + "you choose invite-only and removes "
                                       + "the members you are allowed to "
                                       + "remove. Nothing is deleted: history "
                                       + "stays on every server that took "
                                       + "part.")
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing8
                            AppButton {
                                id: closeSpaceButton
                                objectName: "spaceCloseButton"
                                visible: root.infoIsOurs
                                         && app.roomInfo.canChangeJoinRule
                                         && !!app.roomClosure
                                kind: "danger"
                                size: "sm"
                                text: qsTr("Close space…")
                                onClicked: spaceCloseDialog.openFor(
                                               root.spaceId, root.info.name || "",
                                               true, "close")
                            }
                            AppButton {
                                id: deleteSpaceButton
                                objectName: "spaceDeleteButton"
                                // Only after the server said this account is
                                // one of its administrators.
                                visible: !!app.roomClosure
                                         && app.roomClosure.serverAdmin === "yes"
                                kind: "danger"
                                size: "sm"
                                text: qsTr("Delete from server…")
                                onClicked: spaceCloseDialog.openFor(
                                               root.spaceId, root.info.name || "",
                                               true, "delete")
                            }
                            Item { Layout.fillWidth: true }
                        }
                        RoomCloseDialog { id: spaceCloseDialog }
                    }

                    // ══ MEMBERS ══
                    ColumnLayout {
                        id: membersSection
                        Layout.fillWidth: true
                        Layout.topMargin: AppTheme.spacing12
                        visible: root.section === 1
                        spacing: AppTheme.spacing12

                        // Index order must match membershipValues. "" is all;
                        // these strings must match the snapshot's own, since
                        // the C++ filter matches nothing for an unknown facet.
                        readonly property var membershipValues:
                            ["", "joined", "invited", "banned"]
                        readonly property string membership:
                            membershipValues[
                                Math.max(0, membershipCombo.currentIndex)]
                        readonly property bool alphabetical:
                            sortCombo.currentIndex === 1

                        // joinedCount is computed in Rust over the whole
                        // roster, so it stays honest above the snapshot cap
                        // (hence the truncation line below).
                        Label {
                            Layout.fillWidth: true
                            text: root.infoIsOurs
                                  ? qsTr("%1 members").arg(app.roomInfo.joinedCount)
                                  : ""
                            color: AppTheme.stormText
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textTitle
                            font.weight: AppTheme.weightBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: root.infoIsOurs
                                  ? qsTr("%1 joined · %2 invited")
                                        .arg(app.roomInfo.joinedCount)
                                        .arg(app.roomInfo.invitedCount)
                                  : ""
                            color: AppTheme.stormTextMuted
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                        }
                        // Says when the list holds fewer members than the
                        // count.
                        Label {
                            objectName: "spaceSettingsMemberTruncationNotice"
                            Layout.fillWidth: true
                            visible: root.infoIsOurs && app.roomInfo.truncated
                            wrapMode: Text.WordWrap
                            color: AppTheme.stormTextMuted
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("This space has more members than "
                                       + "Lightning loads at once, so the list "
                                       + "below is a part of it — sorted "
                                       + "alphabetically it is missing names "
                                       + "from the middle, not just the end.")
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing8
                            AppTextField {
                                id: memberFilter
                                objectName: "spaceSettingsMemberFilter"
                                storm: true
                                Layout.fillWidth: true
                                searchIcon: true
                                clearButton: true
                                placeholderText: qsTr("Search members")
                            }
                            AppComboBox {
                                id: membershipCombo
                                objectName: "spaceSettingsMembershipCombo"
                                storm: true
                                Layout.preferredWidth: 130
                                // Pure view state, so a plain currentIndex is
                                // fine.
                                model: [
                                    qsTr("Everyone"),
                                    qsTr("Joined"),
                                    qsTr("Invited"),
                                    qsTr("Banned")
                                ]
                                Accessible.name: qsTr("Filter by membership")
                            }
                            AppComboBox {
                                id: sortCombo
                                objectName: "spaceSettingsSortCombo"
                                storm: true
                                Layout.preferredWidth: 130
                                model: [ qsTr("By role"), qsTr("A to Z") ]
                                Accessible.name: qsTr("Sort members")
                            }
                            AppButton {
                                objectName: "spaceSettingsInviteButton"
                                storm: true
                                kind: "primary"
                                iconName: "person_add"
                                text: qsTr("Invite")
                                enabled: root.infoIsOurs
                                         && app.roomInfo.canInvite
                                onClicked: root.inviteRequested(root.spaceId)
                            }
                        }

                        // A refused role change, from the member menu.
                        Label {
                            objectName: "spaceSettingsMemberError"
                            Layout.fillWidth: true
                            visible: root.memberError.length > 0
                            wrapMode: Text.WordWrap
                            textFormat: Text.PlainText
                            text: root.memberError
                            color: AppTheme.stormDanger
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                        }

                        // Grouped by role; buckets come from C++
                        // (memberRoleGroups), so a custom level gets its own
                        // group.
                        Repeater {
                            model: {
                                var _t = root.rosterTick
                                return root.infoIsOurs
                                       ? app.roomInfo.memberRoleGroups(
                                             memberFilter.text,
                                             membersSection.membership,
                                             membersSection.alphabetical)
                                       : []
                            }
                            delegate: ColumnLayout {
                                id: roleGroup
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 2

                                MenuSectionLabel {
                                    Layout.fillWidth: true
                                    Layout.topMargin: AppTheme.spacing8
                                    text: qsTr("%1 · %2")
                                              .arg(roleGroup.modelData.label)
                                              .arg(roleGroup.modelData.members.length)
                                }

                                Repeater {
                                    model: roleGroup.modelData.members
                                    delegate: Rectangle {
                                        id: memberRow
                                        required property var modelData
                                        Layout.fillWidth: true
                                        implicitHeight: 44
                                        radius: AppTheme.radiusMd
                                        color: memberHover.hovered
                                               ? AppTheme.hover : "transparent"
                                        HoverHandler { id: memberHover }

                                        readonly property string uid:
                                            modelData.userId || ""
                                        // Split on the first colon: a server name
                                        // may carry a port.
                                        readonly property int _colon:
                                            uid.indexOf(":")
                                        readonly property string localpart:
                                            _colon > 0 ? uid.substring(0, _colon)
                                                       : uid
                                        readonly property string server:
                                            _colon > 0 ? uid.substring(_colon + 1)
                                                       : ""
                                        readonly property bool menuOpenHere:
                                            memberMenu.opened
                                            && memberMenu.targetUserId === uid

                                        // Right-click anywhere on the row opens the
                                        // same menu as the button.
                                        TapHandler {
                                            acceptedButtons: Qt.RightButton
                                            onTapped: (eventPoint) => root.openMemberMenu(
                                                memberRow.modelData, memberRow,
                                                eventPoint.position.x,
                                                eventPoint.position.y)
                                        }

                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: AppTheme.spacing8
                                            anchors.rightMargin: AppTheme.spacing8
                                            spacing: AppTheme.spacing8
                                            Avatar {
                                                size: 28
                                                name: memberRow.modelData.displayName
                                                      || memberRow.uid
                                                mxc: memberRow.modelData.avatarUrl || ""
                                                colorKey: memberRow.uid
                                            }
                                            Label {
                                                Layout.fillWidth: true
                                                elide: Label.ElideRight
                                                text: memberRow.modelData.displayName
                                                      || memberRow.uid
                                                textFormat: Text.PlainText
                                                color: AppTheme.stormText
                                                font.family: AppTheme.uiFont
                                                font.pixelSize: AppTheme.textBody
                                            }
                                            // Role chip for anyone above the default
                                            // level, from the member's real level.
                                            StatusChip {
                                                objectName: "spaceMemberRoleChip"
                                                readonly property var level:
                                                    memberRow.modelData.powerLevel
                                                visible: root.infoIsOurs
                                                         && level !== undefined
                                                         && level > app.roomInfo.usersDefaultPowerLevel
                                                storm: true
                                                tone: level >= 100 ? "accent" : "info"
                                                label: level !== undefined
                                                       ? memberRow.modelData.roleLabel
                                                         || app.roomInfo.roleLabelForLevel(level)
                                                       : ""
                                            }
                                            // Invited and banned rows are listed (a
                                            // hidden ban cannot be lifted) and marked.
                                            Label {
                                                visible: memberRow.modelData.membership
                                                         === "invited"
                                                         || memberRow.modelData.membership
                                                            === "banned"
                                                text: memberRow.modelData.membership
                                                      === "banned"
                                                      ? qsTr("Banned")
                                                      : qsTr("Invited")
                                                color: memberRow.modelData.membership
                                                       === "banned"
                                                       ? AppTheme.stormDanger
                                                       : AppTheme.stormTextMuted
                                                font.family: AppTheme.uiFont
                                                font.pixelSize: AppTheme.textMicro
                                            }
                                            // Localpart over server, stacked right.
                                            ColumnLayout {
                                                spacing: 0
                                                Label {
                                                    // Untrusted text: never markup.
                                                    textFormat: Text.PlainText
                                                    Layout.alignment: Qt.AlignRight
                                                    text: memberRow.localpart
                                                    color: AppTheme.stormTextSecondary
                                                    font.family: AppTheme.monoFont
                                                    font.pixelSize: AppTheme.textMicro
                                                }
                                                Label {
                                                    // Untrusted text: never markup.
                                                    textFormat: Text.PlainText
                                                    Layout.alignment: Qt.AlignRight
                                                    visible: memberRow.server.length > 0
                                                    text: memberRow.server
                                                    color: AppTheme.stormTextMuted
                                                    font.family: AppTheme.monoFont
                                                    font.pixelSize: AppTheme.textMicro
                                                }
                                            }
                                            // Kept in the layout while hidden so rows
                                            // do not shift under the pointer.
                                            IconButton {
                                                id: memberMenuButton
                                                objectName: "spaceMemberMenuButton"
                                                iconName: "more_horiz"
                                                size: "sm"
                                                storm: true
                                                opacity: memberHover.hovered
                                                         || memberRow.menuOpenHere
                                                         || activeFocus ? 1 : 0
                                                Accessible.name: qsTr("Actions for %1")
                                                    .arg(memberRow.modelData.displayName
                                                         || memberRow.uid)
                                                onClicked: root.openMemberMenu(
                                                    memberRow.modelData,
                                                    memberMenuButton, 0,
                                                    memberMenuButton.height + 2)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // ══ PERMISSIONS ══
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: AppTheme.spacing12
                        visible: root.section === 2
                        spacing: AppTheme.spacing12

                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: AppTheme.stormTextMuted
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            // The offer policy, stated, so a disabled control
                            // does not look like a bug.
                            text: qsTr("You can only require a level at or "
                                       + "below your own, and only set a "
                                       + "member's role below your own — never "
                                       + "for someone at or above it. A space "
                                       + "using a custom level shows that "
                                       + "number rather than being "
                                       + "relabelled.")
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: root.infoIsOurs
                                     && !app.roomInfo.canChangePowerLevels
                            wrapMode: Text.WordWrap
                            color: AppTheme.stormTextMuted
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("You don't have permission to change "
                                       + "roles or permissions in this space.")
                        }
                        // No thresholds from the backend means unknown, never
                        // "everything is 0".
                        Label {
                            objectName: "spaceSettingsMatrixUnavailable"
                            Layout.fillWidth: true
                            visible: root.infoIsOurs
                                     && Object.keys(app.roomInfo.powerLevels).length === 0
                            wrapMode: Text.WordWrap
                            color: AppTheme.stormTextMuted
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("This space's permission levels haven't "
                                       + "loaded on this backend, so they are "
                                       + "not shown.")
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: root.infoIsOurs
                                     && app.roomInfo.powerMatrixError.length > 0
                            wrapMode: Text.WordWrap
                            color: AppTheme.stormDanger
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            text: root.infoIsOurs
                                  ? app.roomInfo.powerMatrixError : ""
                        }

                        // The power-level matrix
                        Repeater {
                            model: root.permissionGroups
                            delegate: ColumnLayout {
                                id: permGroup
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: AppTheme.spacing8
                                visible: root.infoIsOurs
                                         && Object.keys(app.roomInfo.powerLevels).length > 0

                                MenuSectionLabel {
                                    Layout.fillWidth: true
                                    Layout.topMargin: AppTheme.spacing8
                                    text: permGroup.modelData.title
                                }

                                // Only for the two groups with a warning; a
                                // Loader so no empty Label exists.
                                Loader {
                                    Layout.fillWidth: true
                                    active: permGroup.modelData.note.length > 0
                                    visible: active
                                    sourceComponent: Label {
                                        wrapMode: Text.WordWrap
                                        text: permGroup.modelData.note
                                        color: AppTheme.stormTextMuted
                                        font.family: AppTheme.uiFont
                                        font.pixelSize: AppTheme.textMicro
                                    }
                                }

                                Repeater {
                                    model: permGroup.modelData.rows
                                    delegate: RowLayout {
                                        id: permRow
                                        required property var modelData
                                        Layout.fillWidth: true
                                        spacing: AppTheme.spacing8

                                        readonly property string permKey:
                                            modelData.key
                                        // Method calls create no dependency; hence
                                        // the tick.
                                        readonly property bool known: {
                                            var _t = root.rosterTick
                                            return root.infoIsOurs
                                                   && app.roomInfo.powerLevelKnown(
                                                          permRow.permKey)
                                        }
                                        readonly property int authoritative: {
                                            var _t = root.rosterTick
                                            return permRow.known
                                                   ? app.roomInfo.powerLevelForKey(
                                                         permRow.permKey)
                                                   : -1
                                        }
                                        // The presets plus the room's current
                                        // value when it is none of them, so the
                                        // control shows the truth. The field
                                        // beside it sets arbitrary values.
                                        readonly property var levelOptions: {
                                            var d = root.infoIsOurs
                                                    ? app.roomInfo.usersDefaultPowerLevel
                                                    : 0
                                            var opts = [
                                                { label: qsTr("Member (%1)").arg(d),
                                                  value: d }
                                            ]
                                            if (d !== 50)
                                                opts.push({ label: qsTr("Moderator (50)"),
                                                            value: 50 })
                                            if (d !== 100)
                                                opts.push({ label: qsTr("Administrator (100)"),
                                                            value: 100 })
                                            var cur = permRow.authoritative
                                            if (permRow.known
                                                && !opts.some(function (o) {
                                                    return o.value === cur
                                                })) {
                                                opts.push({
                                                    label: qsTr("Custom (%1)").arg(cur),
                                                    value: cur })
                                            }
                                            return opts
                                        }

                                        Label {
                                            // Untrusted text: never markup.
                                            textFormat: Text.PlainText
                                            Layout.fillWidth: true
                                            elide: Label.ElideRight
                                            text: permRow.modelData.label
                                            color: AppTheme.stormText
                                            font.family: AppTheme.uiFont
                                            font.pixelSize: AppTheme.textBody
                                        }

                                        Label {
                                            visible: !permRow.known
                                            text: qsTr("Not known")
                                            color: AppTheme.stormTextMuted
                                            font.family: AppTheme.uiFont
                                            font.pixelSize: AppTheme.textMeta
                                        }

                                        AppComboBox {
                                            id: levelCombo
                                            objectName: "spacePermCombo_"
                                                        + permRow.permKey
                                            storm: true
                                            visible: permRow.known
                                            Layout.preferredWidth: 190
                                            model: permRow.levelOptions
                                            textRole: "label"
                                            valueRole: "value"
                                            enabled: root.infoIsOurs
                                                     && app.roomInfo.canChangePowerLevels
                                                     && !app.roomInfo.powerMatrixPending
                                            // Explicit mirror via syncToValue, never
                                            // currentIndex: indexOfValue(), which is
                                            // -1 at creation and would clamp to 0.
                                            function snapBack() {
                                                if (permRow.known)
                                                    levelCombo.syncToValue(
                                                        permRow.authoritative)
                                            }
                                            Component.onCompleted: snapBack()
                                            onActivated: (index) => {
                                                app.roomInfo.setPowerLevelKey(
                                                    permRow.permKey,
                                                    levelCombo.valueAt(index))
                                            }
                                        }

                                        // An arbitrary level, which a room may
                                        // legitimately use.
                                        AppTextField {
                                            id: customLevel
                                            objectName: "spacePermCustom_"
                                                        + permRow.permKey
                                            storm: true
                                            visible: permRow.known
                                            Layout.preferredWidth: 62
                                            placeholderText: qsTr("Level")
                                            inputMethodHints: Qt.ImhFormattedNumbersOnly
                                            validator: IntValidator {
                                                bottom: -100
                                                top: 100
                                            }
                                            enabled: root.infoIsOurs
                                                     && app.roomInfo.canChangePowerLevels
                                                     && !app.roomInfo.powerMatrixPending
                                            Accessible.name:
                                                qsTr("Custom level for %1")
                                                    .arg(permRow.modelData.label)
                                            readonly property int parsedLevel:
                                                text.trim().length === 0
                                                ? -1000
                                                : parseInt(text.trim(), 10)
                                        }
                                        IconButton {
                                            objectName: "spacePermApply_"
                                                        + permRow.permKey
                                            visible: permRow.known
                                            iconName: "check"
                                            implicitWidth: 28
                                            implicitHeight: 28
                                            // The controller re-checks before
                                            // dispatch; asking here avoids offering a
                                            // write the server must refuse.
                                            enabled: root.infoIsOurs
                                                     && !isNaN(customLevel.parsedLevel)
                                                     && app.roomInfo.canSetPowerLevelKey(
                                                            permRow.permKey,
                                                            customLevel.parsedLevel)
                                            Accessible.name:
                                                qsTr("Apply custom level to %1")
                                                    .arg(permRow.modelData.label)
                                            onClicked: {
                                                app.roomInfo.setPowerLevelKey(
                                                    permRow.permKey,
                                                    customLevel.parsedLevel)
                                                customLevel.text = ""
                                            }
                                        }

                                        // A rejection re-reads the roster, the
                                        // tick fires, and this snaps back.
                                        Connections {
                                            target: root
                                            function onRosterTickChanged() {
                                                levelCombo.snapBack()
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // Member roles
                        MenuSectionLabel {
                            Layout.fillWidth: true
                            Layout.topMargin: AppTheme.spacing12
                            text: qsTr("Member roles")
                        }

                        Repeater {
                            model: {
                                var _t = root.rosterTick
                                return root.infoIsOurs
                                       && app.roomInfo.canChangePowerLevels
                                       ? app.roomInfo.filterMembers("") : []
                            }
                            delegate: RowLayout {
                                id: roleRow
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: AppTheme.spacing8
                                readonly property string uid:
                                    modelData.userId || ""
                                readonly property string name:
                                    modelData.displayName || uid
                                readonly property bool isOwn:
                                    modelData.isOwn === true
                                Avatar {
                                    size: 24
                                    name: roleRow.modelData.displayName
                                          || roleRow.uid
                                    mxc: roleRow.modelData.avatarUrl || ""
                                    colorKey: roleRow.uid
                                }
                                Label {
                                    Layout.fillWidth: true
                                    elide: Label.ElideRight
                                    text: roleRow.modelData.displayName
                                          || roleRow.uid
                                    textFormat: Text.PlainText
                                    color: AppTheme.stormText
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textBody
                                }
                                Label {
                                    text: {
                                        var _t = root.rosterTick
                                        return app.roomInfo.roleLabelForLevel(
                                            app.roomInfo.powerLevelFor(
                                                roleRow.uid))
                                    }
                                    color: AppTheme.stormTextSecondary
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                }
                                AppButton {
                                    storm: true
                                    size: "sm"
                                    text: qsTr("Admin")
                                    // canSetPowerLevel fails closed on an
                                    // unknown target (levels may be negative,
                                    // so absence is the unknown state).
                                    enabled: app.roomInfo.canSetPowerLevel(
                                                 roleRow.uid, 100)
                                             && !app.roomInfo.powerLevelPending
                                    onClicked: roleConfirm.openFor(
                                                   roleRow.uid, roleRow.name,
                                                   100, text, roleRow.isOwn)
                                }
                                AppButton {
                                    storm: true
                                    size: "sm"
                                    text: qsTr("Moderator")
                                    enabled: app.roomInfo.canSetPowerLevel(
                                                 roleRow.uid, 50)
                                             && !app.roomInfo.powerLevelPending
                                    onClicked: roleConfirm.openFor(
                                                   roleRow.uid, roleRow.name,
                                                   50, text, roleRow.isOwn)
                                }
                                AppButton {
                                    storm: true
                                    size: "sm"
                                    text: qsTr("Member")
                                    enabled: app.roomInfo.canSetPowerLevel(
                                                 roleRow.uid,
                                                 app.roomInfo.usersDefaultPowerLevel)
                                             && !app.roomInfo.powerLevelPending
                                    onClicked: roleConfirm.openFor(
                                                   roleRow.uid, roleRow.name,
                                                   app.roomInfo.usersDefaultPowerLevel,
                                                   text, roleRow.isOwn)
                                }
                            }
                        }
                    }

                    // ══ DEVELOPER TOOLS ══
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: AppTheme.spacing12
                        visible: root.section === 3
                        spacing: AppTheme.spacing8

                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: AppTheme.stormTextMuted
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("Read-only. Everything here is what "
                                       + "this device currently knows about "
                                       + "the space.")
                        }

                        // Hidden clipboard relay, as in RoomInfoPanel,
                        // CodeBlock and MessageDelegate.
                        TextEdit {
                            id: devCopyHelper
                            visible: false
                            width: 0
                            height: 0
                        }

                        Repeater {
                            model: {
                                var _t = root.rosterTick
                                return [
                                    { k: qsTr("Space ID"), v: root.spaceId },
                                    { k: qsTr("Space version"),
                                      v: root.infoIsOurs
                                         ? app.roomInfo.roomVersion : "" },
                                    { k: qsTr("Published address"),
                                      v: root.infoIsOurs
                                         ? (app.roomInfo.canonicalAlias
                                            || qsTr("None")) : "" },
                                    { k: qsTr("Join rule"),
                                      v: root.infoIsOurs
                                         ? app.roomInfo.joinRule : "" },
                                    // A creator's level is the bridge's 2^53
                                    // sentinel, not a number to show.
                                    { k: qsTr("Your power level"),
                                      v: !root.infoIsOurs ? ""
                                         : app.roomInfo.ownPowerLevel >= 9007199254740992
                                         ? app.roomInfo.roleLabelForLevel(
                                               app.roomInfo.ownPowerLevel)
                                         : String(app.roomInfo.ownPowerLevel) },
                                    { k: qsTr("Default power level"),
                                      v: root.infoIsOurs
                                         ? String(app.roomInfo.usersDefaultPowerLevel)
                                         : "" },
                                    { k: qsTr("Members loaded"),
                                      v: root.infoIsOurs
                                         ? String((app.roomInfo.members || []).length)
                                         : "" },
                                    { k: qsTr("Direct children"),
                                      v: app.spaces
                                         ? String(app.spaces.directChildRoomsDetailed(
                                                      root.spaceId).length) : "" }
                                ]
                            }
                            delegate: RowLayout {
                                id: devRow
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: AppTheme.spacing8
                                Label {
                                    Layout.preferredWidth: 170
                                    text: devRow.modelData.k
                                    color: AppTheme.stormTextMuted
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                }
                                Label {
                                    Layout.fillWidth: true
                                    elide: Label.ElideRight
                                    text: devRow.modelData.v
                                    color: AppTheme.stormText
                                    font.family: AppTheme.monoFont
                                    font.pixelSize: AppTheme.textMeta
                                }
                                IconButton {
                                    objectName: "spaceSettingsDevCopy"
                                    iconName: "content_copy"
                                    implicitWidth: 26
                                    implicitHeight: 26
                                    iconSize: 15
                                    enabled: devRow.modelData.v.length > 0
                                    Accessible.name: qsTr("Copy %1")
                                                         .arg(devRow.modelData.k)
                                    ToolTip.text: Accessible.name
                                    ToolTip.visible: hovered
                                    ToolTip.delay: 500
                                    onClicked: {
                                        devCopyHelper.text = devRow.modelData.v
                                        devCopyHelper.selectAll()
                                        devCopyHelper.copy()
                                    }
                                }
                            }
                        }

                        // Not yet: listing state event types and counts needs a
                        // bounded Rust reader that returns only [{type,
                        // count}]; raw member events must never cross the
                        // bridge.
                        Label {
                            Layout.fillWidth: true
                            Layout.topMargin: AppTheme.spacing8
                            wrapMode: Text.WordWrap
                            color: AppTheme.stormTextMuted
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMicro
                            text: qsTr("A full room-state and account-data "
                                       + "inspector isn't built yet.")
                        }
                    }

                    Item { Layout.fillHeight: true; Layout.preferredHeight: 8 }
                }
            }
        }
    }

    // ── Member actions ──
    //
    // One menu for every row, from the ⋯ button or a right-click. What it
    // offers is captured at open from app.roomInfo, which is this Space's
    // roster while infoIsOurs; the controllers re-check at dispatch.
    property string memberError: ""
    Connections {
        target: app.roomInfo
        function onPowerLevelActionFinished(roomId, userId, level, ok, message) {
            if (roomId === root.spaceId)
                root.memberError = ok ? "" : message
        }
    }

    function openMemberMenu(member, anchor, x, y) {
        if (!root.infoIsOurs || !member || !member.userId)
            return
        root.memberError = ""
        var uid = member.userId
        memberMenu.targetUserId = uid
        memberMenu.targetName = member.displayName || uid
        memberMenu.targetIsOwn = member.isOwn === true
        memberMenu.canKick = app.roomInfo.canModerate(uid, "kick")
        memberMenu.canBan = app.roomInfo.canModerate(uid, "ban")
        memberMenu.canUnban = app.roomInfo.canModerate(uid, "unban")
        var current = app.roomInfo.powerLevelFor(uid)
        var candidates = [100, 50, app.roomInfo.usersDefaultPowerLevel]
        var seen = {}
        var options = []
        for (var i = 0; i < candidates.length; ++i) {
            var level = candidates[i]
            if (seen[level] === true)
                continue
            seen[level] = true
            // canSetPowerLevel refuses the current level; list it anyway,
            // marked, so the menu shows where they stand.
            var settable = app.roomInfo.canSetPowerLevel(uid, level)
            if (!settable && level !== current)
                continue
            options.push({ level: level,
                           label: app.roomInfo.roleLabelForLevel(level),
                           current: level === current,
                           settable: settable })
        }
        // Only the current level and nothing to change it to is no choice.
        memberMenu.roleOptions = options.length > 1 ? options : []
        memberMenu.popup(anchor, x, y)
    }

    AppMenu {
        id: memberMenu
        objectName: "spaceMemberMenu"
        menuWidth: 220
        property string targetUserId: ""
        property string targetName: ""
        property bool targetIsOwn: false
        property bool canKick: false
        property bool canBan: false
        property bool canUnban: false
        property var roleOptions: []
        readonly property bool anyModeration: canKick || canBan || canUnban
        contextLabel: targetName

        Loader {
            active: memberMenu.roleOptions.length > 0
            visible: active
            sourceComponent: MenuSectionLabel {
                leftPadding: AppTheme.menuItemPadding + 6
                rightPadding: AppTheme.menuItemPadding
                text: qsTr("Role in this space")
            }
        }
        Repeater {
            model: memberMenu.roleOptions
            delegate: AppMenuItem {
                required property var modelData
                objectName: "spaceMemberRole_" + modelData.level
                text: modelData.label
                radio: true
                radioSelected: modelData.current
                enabled: modelData.settable && !app.roomInfo.powerLevelPending
                onTriggered: roleConfirm.openFor(memberMenu.targetUserId,
                                                 memberMenu.targetName,
                                                 modelData.level,
                                                 modelData.label,
                                                 memberMenu.targetIsOwn)
            }
        }
        AppMenuSeparator {
            visible: memberMenu.roleOptions.length > 0
                     && memberMenu.anyModeration
        }
        AppMenuItem {
            objectName: "spaceMemberKick"
            visible: memberMenu.canKick
            danger: true
            iconName: "person_remove"
            text: qsTr("Kick from space…")
            onTriggered: memberAction.openFor(root.spaceId, root.info.name || "",
                                              memberMenu.targetUserId,
                                              memberMenu.targetName, "kick")
        }
        AppMenuItem {
            objectName: "spaceMemberBan"
            visible: memberMenu.canBan
            danger: true
            iconName: "block"
            text: qsTr("Ban from space…")
            onTriggered: memberAction.openFor(root.spaceId, root.info.name || "",
                                              memberMenu.targetUserId,
                                              memberMenu.targetName, "ban")
        }
        AppMenuItem {
            objectName: "spaceMemberUnban"
            visible: memberMenu.canUnban
            iconName: "undo"
            text: qsTr("Unban…")
            onTriggered: memberAction.openFor(root.spaceId, root.info.name || "",
                                              memberMenu.targetUserId,
                                              memberMenu.targetName, "unban")
        }
        // Says why the menu is empty rather than showing nothing.
        AppMenuItem {
            objectName: "spaceMemberNoActions"
            visible: memberMenu.roleOptions.length === 0
                     && !memberMenu.anyModeration
            enabled: false
            text: qsTr("You can't change this member")
        }
    }

    AppDialog {
        id: roleConfirm
        objectName: "spaceMemberRoleConfirm"
        parent: Overlay.overlay
        width: Math.min(420, parent ? parent.width - 64 : 420)
        title: selfDemotion ? qsTr("Lower your own role?")
                            : qsTr("Change role?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        property string userId: ""
        property string name: ""
        property int level: 0
        property string label: ""
        property bool isOwn: false
        // A grant at or above your own level cannot be taken back.
        readonly property bool irreversible:
            root.infoIsOurs && !isOwn && level >= app.roomInfo.ownPowerLevel
        // Neither can lowering your own: nobody may raise themselves. A
        // creator (v12, 2^53 from the bridge) cannot be lowered at all.
        readonly property bool selfDemotion:
            root.infoIsOurs && isOwn && level < app.roomInfo.ownPowerLevel
            && app.roomInfo.ownPowerLevel < 9007199254740992
        // The new level is below what changing roles needs here.
        // rosterTick: the two calls are Q_INVOKABLEs, which a binding does
        // not track; the tick moves on every roster refresh.
        readonly property bool losesRoleControl: {
            var _t = root.rosterTick
            return selfDemotion
                   && app.roomInfo.powerLevelKnown("m.room.power_levels")
                   && level < app.roomInfo.powerLevelForKey("m.room.power_levels")
        }
        function openFor(uid, displayName, newLevel, newLabel, own) {
            userId = uid
            name = displayName
            level = newLevel
            label = newLabel
            isOwn = own === true
            open()
        }
        onAccepted: {
            if (root.infoIsOurs)
                app.roomInfo.setMemberPowerLevel(userId, level)
        }
        ColumnLayout {
            width: parent ? parent.width : 0
            spacing: AppTheme.spacing8
            Label {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                text: qsTr("Set %1 to %2 in this space? Roles in the space's "
                           + "rooms are set in each room.")
                          .arg(roleConfirm.name).arg(roleConfirm.label)
                color: AppTheme.stormText
            }
            Label {
                Layout.fillWidth: true
                visible: roleConfirm.irreversible
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                text: qsTr("This gives them your own level or higher. You "
                           + "will not be able to change it back.")
                color: AppTheme.stormDanger
                font.pixelSize: AppTheme.textMeta
            }
            Label {
                objectName: "spaceSelfDemotionWarning"
                Layout.fillWidth: true
                visible: roleConfirm.selfDemotion
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                text: qsTr("You are lowering your own role. You will not be "
                           + "able to raise it back yourself: only someone "
                           + "whose role is above your new one can, and if "
                           + "nobody else holds a role as high as yours, "
                           + "nobody can.")
                color: AppTheme.stormDanger
                font.pixelSize: AppTheme.textMeta
            }
            Label {
                objectName: "spaceSelfDemotionLosesControl"
                Layout.fillWidth: true
                visible: roleConfirm.losesRoleControl
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                text: qsTr("You will also no longer be able to change roles "
                           + "or permissions in this space.")
                color: AppTheme.stormDanger
                font.pixelSize: AppTheme.textMeta
            }
        }
    }

    SpaceMemberActionDialog {
        id: memberAction
    }

    /// The host owns the invite dialog, like the clipboard proxy and the leave
    /// confirmation.
    signal inviteRequested(string spaceId)

    // A Space change always wins over a half-typed value; a roster refresh only
    // re-snaps untouched fields.
    onSpaceIdChanged: {
        nameField.resetForSpace()
        topicField.resetForSpace()
        aliasField.resetForSpace()
        joinRuleCombo.refreshRule()
        // The banner read happens in openFor() only.
    }
    Connections {
        target: app.roomInfo
        function onMembersChanged() {
            nameField.refreshName()
            topicField.refreshTopic()
            aliasField.refreshAlias()
            joinRuleCombo.refreshRule()
            // Every binding that calls a controller method depends on this.
            root.rosterTick++
        }
    }
    Connections {
        target: app.spaces
        function onSpacesChanged() {
            // The tick first: `info` holds the old value until it re-evaluates.
            root.spacesTick++
            nameField.refreshName()
            topicField.refreshTopic()
        }
    }
}
