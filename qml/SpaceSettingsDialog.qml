import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

// Space settings: one modal covering everything Lightning can actually change
// about a Space, reached from the rail's right-click menu and from Space Home.
//
// A SPACE IS A MATRIX ROOM. Every control here writes ordinary room state
// through the same permission-gated backend a room's own settings use
// (RoomInfoController), and every one of them is gated on the room's REAL
// required power level for that state event — never on a role label, and never
// optimistically: the write completes, the roster is re-read, so a rejection
// cannot leave a value the Space does not have.
//
// WHAT IS DELIBERATELY NOT HERE. The reference client this was modelled on
// also offers Cosmetics, Abbreviations and a per-Space Appearance. A name
// colour, a font and a per-Space "show room icons" flag are not Matrix state:
// they would be private storage only Lightning could interpret, presented as
// though it were part of the Space, and every other client — and every other
// device — would see nothing. The local rail folders are the one place this
// app keeps device-local organisation, and they are defensible precisely
// because they touch NO Matrix state and say so. Four dead tabs are worse than
// four missing ones.
//
// One correction to the record (2026-08-26): the earlier version of this
// comment lumped "Emojis & Stickers" in with those. That was wrong about
// Matrix — `im.ponies.room_emotes` (MSC2545) IS a state event that Element,
// Cinny, FluffyChat, Nheko and the reference client all read. It is absent
// here because nobody has built it yet, NOT because it would be private
// storage. Do not repeat the old reasoning.
//
// The BANNER below is the same category and the opposite outcome: it is a real
// state event (`page.codeberg.everypizza.room.banner`, rust/src/banner.rs)
// chosen specifically so the reference client reads what Lightning writes.
AppDialog {
    id: root
    objectName: "spaceSettingsDialog"

    /// The Space this is editing. Never a plain room: the rail only offers it
    /// for a real Space, and Space Home only for the Space it is showing.
    property string spaceId: ""
    /// Where app.roomInfo was pointing when this opened, so closing puts it
    /// back. The controller is shared with the Room Information panel and with
    /// Space Home; leaving it aimed at the Space after the dialog closed would
    /// silently repoint whatever surface is underneath.
    property string _restoreRoomId: ""
    property int section: 0

    /// Bumped on every roster answer.
    ///
    /// Read it inside any binding that calls a Q_INVOKABLE on app.roomInfo —
    /// `filterMembers`, `memberRoleGroups`, `powerLevelForKey`,
    /// `roleLabelForLevel`. A method CALL creates no property dependency, so
    /// those bindings never re-evaluate on their own: the member list here
    /// used to be bound to the search field's text alone and would therefore
    /// not refresh when the roster itself changed. Same shape as the
    /// `resolveTick` counter the media-cache handlers use (2026-08-23) —
    /// an unused local (`var _t = root.rosterTick`) is enough to create the
    /// dependency, confirmed on Qt 6.11.
    property int rosterTick: 0

    // No `title`: this dialog is its own header (avatar + name + section), the
    // AppDialog escape hatch for exactly that.
    title: ""
    modal: true
    parent: Overlay.overlay
    anchors.centerIn: parent
    standardButtons: Dialog.NoButton
    width: Math.min(940, Overlay.overlay ? Overlay.overlay.width - 80 : 940)
    height: Math.min(660, Overlay.overlay ? Overlay.overlay.height - 80 : 660)

    readonly property var info: app.spaces && spaceId.length > 0
                                ? app.spaces.spaceInfo(spaceId) : ({})
    // Only ever true when the roster on screen is THIS Space's. app.roomInfo
    // is shared, and reading canEditName off another room's snapshot is how a
    // permission gate ends up lying.
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
        memberFilter.text = ""
        membershipCombo.currentIndex = 0
        sortCombo.currentIndex = 0
        if (app.banners)
            app.banners.requestRoom(targetSpaceId)
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

    /// The Permissions matrix, grouped as the reference client groups it.
    ///
    /// Every `key` here must also be in RoomInfoController::powerLevelKeys()
    /// and in the allowlist in rooms::set_room_power_level_key — the Rust edge
    /// refuses anything else, so a typo produces an inert control rather than
    /// an unexpected state event.
    ///
    /// `m.call.member` ("Start & Join Calls" in the reference client) is
    /// DELIBERATELY ABSENT. The identifier Lightning actually sends today is
    /// the MSC3401 unstable one, ruma aliases the stable name onto it, and a
    /// Space has no timeline to hold a call — a row that honestly governs
    /// neither string is worse than a missing row.
    ///
    /// The reference client shows both "Change All Permission" and "Edit
    /// Power Levels"; both are the level for `m.room.power_levels`. Two rows
    /// for one key would be a lie, so there is one.
    readonly property var permissionGroups: [
        {
            title: qsTr("Users"),
            // Group-level, never per row: a Label whose text can be "" keeps
            // ItemObservesViewport forever (QQuickText::setText early-returns
            // before clearing the flag), which is the single most expensive
            // QML mistake known in this tree. A note that is always present
            // when its group renders cannot be that Label.
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
            // Matrix has no separate unban level — it is max(ban, kick) — so
            // there is deliberately no unban row to offer.
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

        // ── Header: which Space, which section, and the way out ──────────
        RowLayout {
            Layout.fillWidth: true
            Layout.bottomMargin: AppTheme.spacing12
            spacing: AppTheme.spacing12

            Avatar {
                size: 32
                circle: false
                squareRadius: AppTheme.radiusMd
                name: root.info.name || ""
                mxc: root.info.avatarUrl || ""
                colorKey: root.spaceId
            }
            Label {
                text: root.info.name || qsTr("Space")
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

            // ── Section nav ──────────────────────────────────────────────
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

            // ── Section body ─────────────────────────────────────────────
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth

                ColumnLayout {
                    width: parent ? parent.width : 0
                    spacing: AppTheme.spacing16

                    // A single honest line whenever the roster this page reads
                    // is not loaded yet: every gate below is derived from it,
                    // and rendering them all disabled with no explanation
                    // looks like a permission refusal.
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

                    // ══ GENERAL ═════════════════════════════════════════
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
                            // The picker CHOOSES; the crop dialog decides what
                            // is uploaded, and is the gate that refuses a
                            // non-raster file before anything renders it.
                            onAccepted: spaceAvatarCrop.openFor(selectedFile)
                        }
                        ImageCropDialog {
                            id: spaceAvatarCrop
                            role: "avatar"
                            // m.room.avatar, exactly as a room's own avatar —
                            // Lightning invents no Space-specific storage.
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
                            // The URL crosses as-is; the manager converts it.
                            // Stripping "file://" here produced "/C:/…" on
                            // Windows.
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
                                        // Explicit mirror, not a binding: the
                                        // first keystroke breaks a binding
                                        // permanently, and this dialog can be
                                        // reopened on another Space.
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
                                        enabled: root.infoIsOurs
                                                 && app.roomInfo.canEditTopic
                                                 && !app.roomInfo.editPending
                                        onClicked: app.roomInfo.setRoomTopic(
                                                       topicField.text.trim())
                                    }
                                }
                            }
                        }

                        // ── Banner ──────────────────────────────────────
                        //
                        // A REAL state event, and the reason this page has one
                        // at all: `page.codeberg.everypizza.room.banner` was
                        // chosen in rust/src/banner.rs to match the reference
                        // client's own type, so a banner set here is a banner
                        // it renders. A client that does not know the type
                        // simply shows none — nothing about the Space breaks.
                        //
                        // Permission comes from the BANNER manager, never from
                        // app.roomInfo: this is a custom state type with its
                        // own required level, and reusing canEditAvatar here
                        // would be a guess dressed as a permission.
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

                            // `revision` is the manager's own change counter;
                            // both answers are METHOD calls and would create no
                            // dependency without it.
                            readonly property string bannerMxc: {
                                if (!app.banners || root.spaceId === "")
                                    return ""
                                var _dep = app.banners.revision
                                return app.banners.roomBannerFor(root.spaceId)
                            }
                            readonly property bool canEdit: {
                                if (!app.banners || root.spaceId === "")
                                    return false
                                // Until the room replies this is false, so the
                                // control is never offered on a guess.
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
                                    // 3:1 — the ratio ImageCropDialog crops a
                                    // banner to. A flat height here re-cropped
                                    // the image inside the region the user had
                                    // already chosen, so the preview did not
                                    // show what was saved.
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
                                        readonly property string mxc:
                                            bannerCard.bannerMxc
                                        // A counter the binding READS, never
                                        // an assignment to `source`: assigning
                                        // a bound property imperatively
                                        // destroys the binding, which is what
                                        // made Space banners sticky in
                                        // 0.7.6 — the first image that
                                        // finished loading became the only one
                                        // this Image ever showed.
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
                                        }
                                    }
                                    Label {
                                        anchors.centerIn: parent
                                        visible: !bannerPreview.visible
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

                                // A refusal is reported where it happened, and
                                // nothing was applied optimistically to undo.
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
                                    // Explicit mirror rather than a two-way
                                    // binding: a rejected write must snap back
                                    // to what the Space actually holds, and a
                                    // binding the user's own selection already
                                    // broke cannot do that.
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
                                // Only `invite`/`public`/`knock` are settable.
                                // A restricted rule carries an allow-rule list
                                // this surface cannot build, and sending one
                                // with an empty list would lock the space to
                                // invite-only while claiming otherwise.
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

                        // ── Advanced ────────────────────────────────────
                        //
                        // The version is READ from the SDK (`Room::version()`),
                        // never parsed out of m.room.create by hand.
                        //
                        // There is NO Upgrade button, on purpose. An upgrade is
                        // irreversible, it tombstones the Space, and every
                        // m.space.child edge the old room held is orphaned by
                        // it — none of which can be undone by a client. It
                        // needs a typed confirmation and a migration of the
                        // children, and until both exist an honest disclosure
                        // beats a one-click door.
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
                                          ? qsTr("You have the permission to "
                                                 + "upgrade this space, but "
                                                 + "Lightning can't perform an "
                                                 + "upgrade yet: it is "
                                                 + "irreversible and would "
                                                 + "orphan every room the "
                                                 + "space lists.")
                                          : qsTr("Only someone allowed to send "
                                                 + "m.room.tombstone can "
                                                 + "upgrade this space. "
                                                 + "Lightning can't perform an "
                                                 + "upgrade yet.")
                                }
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
                    }

                    // ══ MEMBERS ═════════════════════════════════════════
                    ColumnLayout {
                        id: membersSection
                        Layout.fillWidth: true
                        Layout.topMargin: AppTheme.spacing12
                        visible: root.section === 1
                        spacing: AppTheme.spacing12

                        // Index order must match membershipValues. "" is ALL;
                        // the C++ filter matches nothing for an unrecognised
                        // facet rather than quietly meaning "all", so these
                        // strings must stay exactly the snapshot's own.
                        readonly property var membershipValues:
                            ["", "joined", "invited", "banned"]
                        readonly property string membership:
                            membershipValues[
                                Math.max(0, membershipCombo.currentIndex)]
                        readonly property bool alphabetical:
                            sortCombo.currentIndex === 1

                        // The count Sable puts in its title. joinedCount is a
                        // WHOLE-roster fact computed in Rust, so it stays
                        // honest above the snapshot cap — which is exactly why
                        // the truncation line below has to exist.
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
                        // Without this line a 34,000-member space showed an
                        // honest 34156 above 500 rows and said nothing about
                        // the difference. A count that describes a population
                        // the list does not contain is the failure this whole
                        // file guards against everywhere else.
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
                                // Pure VIEW state, so a plain currentIndex is
                                // right here: nothing is written, so there is
                                // nothing for a rejection to snap back to.
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

                        // Grouped by role, exactly as Sable groups it. The
                        // buckets come from C++ (memberRoleGroups), not from
                        // QML: which roles a room HAS is a model fact, and a
                        // room using 42 gets its own "Custom (42)" group
                        // rather than being folded into Moderator.
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
                                        // Split on the FIRST colon only: a
                                        // server name may carry a port
                                        // (":8448"), and splitting on the last
                                        // one would tear that off.
                                        readonly property int _colon:
                                            uid.indexOf(":")
                                        readonly property string localpart:
                                            _colon > 0 ? uid.substring(0, _colon)
                                                       : uid
                                        readonly property string server:
                                            _colon > 0 ? uid.substring(_colon + 1)
                                                       : ""

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
                                                color: AppTheme.stormText
                                                font.family: AppTheme.uiFont
                                                font.pixelSize: AppTheme.textBody
                                            }
                                            // Invited and banned rows are in
                                            // the list on purpose (a banned
                                            // member you cannot see is a ban
                                            // you cannot lift) — so they have
                                            // to be legible AS invited and
                                            // banned.
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
                                            // Localpart over server, stacked
                                            // right, as Sable renders it.
                                            ColumnLayout {
                                                spacing: 0
                                                Label {
                                                    Layout.alignment: Qt.AlignRight
                                                    text: memberRow.localpart
                                                    color: AppTheme.stormTextSecondary
                                                    font.family: AppTheme.monoFont
                                                    font.pixelSize: AppTheme.textMicro
                                                }
                                                Label {
                                                    Layout.alignment: Qt.AlignRight
                                                    visible: memberRow.server.length > 0
                                                    text: memberRow.server
                                                    color: AppTheme.stormTextMuted
                                                    font.family: AppTheme.monoFont
                                                    font.pixelSize: AppTheme.textMicro
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // ══ PERMISSIONS ═════════════════════════════════════
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
                            // The offer policy, stated. The server applies the
                            // same rules; saying so is what stops a disabled
                            // control reading as a bug.
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
                        // A backend that sends no thresholds leaves the matrix
                        // EMPTY, and an empty matrix means UNKNOWN — never
                        // "everything is 0", which is a real and very
                        // permissive configuration.
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

                        // ── The power-level matrix ──────────────────────
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

                                // Rendered only for the two groups that carry
                                // a real warning; a Loader, so no Label is
                                // ever created holding "".
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
                                        // powerLevelKnown / powerLevelForKey
                                        // are METHOD calls and create no
                                        // dependency of their own — hence the
                                        // tick.
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
                                        // The presets, plus the room's OWN
                                        // current value when it is none of
                                        // them — so the control can show the
                                        // truth and re-select it. It cannot
                                        // INVENT a new arbitrary number; the
                                        // field beside it can.
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
                                            // Explicit mirror. syncToValue,
                                            // never `currentIndex: indexOfValue(…)`:
                                            // indexOfValue() is -1 at creation
                                            // time and clamping that to 0 makes
                                            // the control lie about the room.
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

                                        // An arbitrary number, because a room
                                        // may legitimately use one and the
                                        // presets can only ever re-select the
                                        // value it already has.
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
                                            // The controller re-checks this
                                            // before dispatching; asking it
                                            // here is what keeps the control
                                            // from offering a write the server
                                            // must refuse.
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

                                        // Nothing is applied optimistically:
                                        // a rejection re-reads the roster, the
                                        // tick fires, and this snaps the combo
                                        // back to what the space actually
                                        // holds.
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

                        // ── Member roles ────────────────────────────────
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
                                    // canSetPowerLevel FAILS CLOSED on an
                                    // unknown target: levels may legitimately
                                    // be negative, so absence of the roster
                                    // row is the unknown state, never a
                                    // sentinel.
                                    enabled: app.roomInfo.canSetPowerLevel(
                                                 roleRow.uid, 100)
                                             && !app.roomInfo.powerLevelPending
                                    onClicked: app.roomInfo.setMemberPowerLevel(
                                                   roleRow.uid, 100)
                                }
                                AppButton {
                                    storm: true
                                    size: "sm"
                                    text: qsTr("Moderator")
                                    enabled: app.roomInfo.canSetPowerLevel(
                                                 roleRow.uid, 50)
                                             && !app.roomInfo.powerLevelPending
                                    onClicked: app.roomInfo.setMemberPowerLevel(
                                                   roleRow.uid, 50)
                                }
                                AppButton {
                                    storm: true
                                    size: "sm"
                                    text: qsTr("Member")
                                    enabled: app.roomInfo.canSetPowerLevel(
                                                 roleRow.uid,
                                                 app.roomInfo.usersDefaultPowerLevel)
                                             && !app.roomInfo.powerLevelPending
                                    onClicked: app.roomInfo.setMemberPowerLevel(
                                                   roleRow.uid,
                                                   app.roomInfo.usersDefaultPowerLevel)
                                }
                            }
                        }
                    }

                    // ══ DEVELOPER TOOLS ═════════════════════════════════
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

                        // Hidden helper for clipboard copy without C++
                        // additions — the same relay RoomInfoPanel, CodeBlock
                        // and MessageDelegate use.
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
                                    { k: qsTr("Your power level"),
                                      v: root.infoIsOurs
                                         ? String(app.roomInfo.ownPowerLevel) : "" },
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

                        // The reference client also lists every state event
                        // TYPE with its count and offers "Fetch Full State".
                        // That needs a bounded Rust reader that emits
                        // [{type, count}] and NOTHING else — 35,000 raw
                        // m.room.member events must never cross the bridge —
                        // and no such entry point exists yet. Saying so beats
                        // an expandable row that expands to nothing.
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

    /// The host owns the invite dialog, exactly as it owns the clipboard proxy
    /// and the leave confirmation: a dialog that reached up into its host by id
    /// is how the reader popover's click ended up silently dead.
    signal inviteRequested(string spaceId)

    // A Space change ALWAYS wins over a half-typed value: it belongs to the
    // Space that is no longer on screen. A roster refresh only re-snaps a field
    // the user has not edited, so a remote change (or a rejected write) lands
    // without destroying an edit in progress.
    onSpaceIdChanged: {
        nameField.resetForSpace()
        topicField.resetForSpace()
        aliasField.resetForSpace()
        joinRuleCombo.refreshRule()
        if (app.banners && spaceId.length > 0)
            app.banners.requestRoom(spaceId)
    }
    Connections {
        target: app.roomInfo
        function onMembersChanged() {
            nameField.refreshName()
            topicField.refreshTopic()
            aliasField.refreshAlias()
            joinRuleCombo.refreshRule()
            // Every binding that CALLS a controller method depends on this and
            // on nothing else.
            root.rosterTick++
        }
    }
    Connections {
        target: app.spaces
        function onSpacesChanged() {
            nameField.refreshName()
            topicField.refreshTopic()
        }
    }
}
