import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

AppDialog {
    id: root
    objectName: "spaceSettingsDialog"

    property string spaceId: ""
    property bool roomMode: false
    property string roomName: ""
    property string _restoreRoomId: ""
    property int section: 0

    /// Bumped on every roster answer. Read it in any binding that calls a
    /// Q_INVOKABLE on app.roomInfo (filterMembers, memberRoleGroups,
    /// powerLevelForKey, roleLabelForLevel): a method call creates no
    /// dependency, and an unused local read is enough to create one.
    property int rosterTick: 0
    property var orderDraft: []
    property bool orderDirty: false
    property bool orderPending: false
    property string orderStatus: ""
    function resetOrderDraft() {
        orderDraft = app.spaces.defaultOrderChildren(root.spaceId)
        orderDirty = false
    }
    function moveDefaultChild(index, delta) {
        const target = index + delta
        if (orderPending || target < 0 || target >= orderDraft.length) return
        var next = orderDraft.slice()
        const row = next.splice(index, 1)[0]
        next.splice(target, 0, row)
        orderDraft = next
        orderDirty = true
    }
    Connections {
        target: app.spaces
        function onChildOrderFinished(spaceId, ok) {
            if (spaceId !== root.spaceId) return
            root.orderPending = false
            root.orderStatus = ok
                ? qsTr("Default order saved. Your personal room order still takes precedence.")
                : qsTr("Could not save the complete order. Some rooms may have moved; save again to retry, or reload the order.")
            if (ok) root.orderDirty = false
        }
    }

    title: ""
    modal: true
    parent: Overlay.overlay
    anchors.centerIn: parent
    standardButtons: Dialog.NoButton
    width: Math.min(940, Overlay.overlay ? Overlay.overlay.width - 80 : 940)
    height: Math.min(660, Overlay.overlay ? Overlay.overlay.height - 80 : 660)

    property int spacesTick: 0
    readonly property var info: {
        var _dep = root.spacesTick
        if (root.roomMode)
            return ({ name: root.roomName, avatarUrl: "" })
        return app.spaces && spaceId.length > 0
               ? app.spaces.spaceInfo(spaceId) : ({})
    }
    readonly property bool infoIsOurs:
        app.roomInfo && app.roomInfo.roomId === root.spaceId

    function openFor(targetSpaceId) {
        if (!targetSpaceId || targetSpaceId.length === 0)
            return
        roomMode = false
        spaceId = targetSpaceId
        _restoreRoomId = app.roomInfo ? app.roomInfo.roomId : ""
        if (app.roomInfo)
            app.roomInfo.roomId = targetSpaceId
        section = 0
        if (app.spaces)
            app.spaces.noteSettingsDialogOpened(targetSpaceId)
        bodyFlick.contentY = 0
        memberError = ""
        memberFilter.text = ""
        membershipCombo.currentIndex = 0
        sortCombo.currentIndex = 0
        nameField.reset()
        topicField.reset()
        aliasField.resetForSpace()
        joinRuleCombo.refreshRule()
        if (app.banners)
            app.banners.refreshRoom(targetSpaceId)
        if (app.backdrops)
            app.backdrops.refreshScope(targetSpaceId)
        if (app.roomClosure)
            app.roomClosure.checkServerAdmin()
        orderPending = app.spaces.childOrderPending(targetSpaceId)
        orderStatus = ""
        resetOrderDraft()
        open()
    }

    function openPermissionsForRoom(roomId, displayName) {
        if (!roomId)
            return
        roomMode = true
        roomName = displayName || roomId
        spaceId = roomId
        _restoreRoomId = app.roomInfo ? app.roomInfo.roomId : ""
        if (app.roomInfo)
            app.roomInfo.roomId = roomId
        section = 2
        bodyFlick.contentY = 0
        memberError = ""
        open()
    }

    readonly property Flickable bodyFlick: bodyScroll.contentItem

    function ensureVisibleInBody(item) {
        var f = root.bodyFlick
        if (!f || !item || !f.contentItem)
            return
        var p = item
        while (p && p !== f.contentItem)
            p = p.parent
        if (!p)
            return
        var top = item.mapToItem(f.contentItem, 0, 0).y
        var bottom = top + item.height
        var maxY = Math.max(0, f.contentHeight - f.height)
        if (top < f.contentY)
            f.contentY = Math.max(0, top - 8)
        else if (bottom > f.contentY + f.height)
            f.contentY = Math.min(maxY, bottom - f.height + 8)
        var left = item.mapToItem(f.contentItem, 0, 0).x
        var right = left + item.width
        var maxX = Math.max(0, f.contentWidth - f.width)
        if (left < f.contentX)
            f.contentX = Math.max(0, left - 8)
        else if (right > f.contentX + f.width)
            f.contentX = Math.min(maxX, right - f.width + 8)
    }

    onSectionChanged: bodyFlick.contentY = 0

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
    readonly property var permissionGroups: roomMode ? [
        { title: qsTr("Members"), note: qsTr("The level every member starts at."),
          rows: [{ key: "users_default", label: qsTr("Default power") }] },
        { title: qsTr("Messages and notifications"), note: "",
          rows: [
              { key: "events_default", label: qsTr("Send messages") },
              { key: "notifications.room", label: qsTr("Notify everyone (@room)") }
          ] },
        { title: qsTr("Moderation"), note: "",
          rows: [
              { key: "invite", label: qsTr("Invite") },
              { key: "kick", label: qsTr("Kick") },
              { key: "ban", label: qsTr("Ban") },
              { key: "redact", label: qsTr("Remove messages") }
          ] },
        { title: qsTr("Room profile"), note: "",
          rows: [
              { key: "m.room.name", label: qsTr("Room name") },
              { key: "m.room.avatar", label: qsTr("Room avatar") },
              { key: "m.room.topic", label: qsTr("Room topic") }
          ] },
        { title: qsTr("Settings"),
          note: qsTr("Set \"Edit power levels\" above your own level and you can never lower it again."),
          rows: [
              { key: "m.room.join_rules", label: qsTr("Change room access") },
              { key: "m.room.canonical_alias", label: qsTr("Publish address") },
              { key: "m.room.power_levels", label: qsTr("Edit power levels") },
              { key: "m.room.history_visibility", label: qsTr("History visibility") },
              { key: "m.room.guest_access", label: qsTr("Guest access") },
              { key: "m.room.tombstone", label: qsTr("Upgrade room") },
              { key: "state_default", label: qsTr("Other settings") }
          ] }
    ] : [
        {
            title: qsTr("Users"),
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
                AvatarViewAction {}
            }
            Label {
                objectName: "spaceSettingsHeaderName"
                text: root.info.name || (root.roomMode ? qsTr("Room")
                                                      : qsTr("Space"))
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

            ColumnLayout {
                objectName: "spaceSettingsNav"
                Layout.fillWidth: false
                Layout.preferredWidth: 200
                Layout.maximumWidth: 200
                Layout.fillHeight: true
                Layout.topMargin: AppTheme.spacing12
                spacing: 2

                Repeater {
                    id: navRepeater
                    model: root.roomMode ? [root.sections[2]] : root.sections
                    delegate: Rectangle {
                        id: navRow
                        required property var modelData
                        required property int index
                        objectName: "spaceSettingsNav_" + modelData.key
                        Layout.fillWidth: true
                        implicitHeight: 34
                        radius: AppTheme.radiusMd
                        readonly property int sectionIndex:
                            root.roomMode ? 2 : index
                        readonly property bool current:
                            root.section === sectionIndex
                        color: current ? AppTheme.stormSelection
                               : navHover.hovered ? AppTheme.hover
                                                  : "transparent"
                        HoverHandler {
                            id: navHover
                            cursorShape: Qt.PointingHandCursor
                        }
                        TapHandler { onTapped: root.section = navRow.sectionIndex }
                        activeFocusOnTab: true
                        function _goto(i) {
                            var n = root.roomMode ? 1 : root.sections.length
                            root.section = Math.max(0, Math.min(n - 1, i))
                            if (root.roomMode)
                                root.section = 2
                            var target = navRepeater.itemAt(
                                root.roomMode ? 0 : root.section)
                            if (target)
                                target.forceActiveFocus(Qt.TabFocusReason)
                        }
                        Keys.onUpPressed: navRow._goto(navRow.index - 1)
                        Keys.onDownPressed: navRow._goto(navRow.index + 1)
                        Keys.onReturnPressed: root.section = navRow.sectionIndex
                        Keys.onEnterPressed: root.section = navRow.sectionIndex
                        Keys.onSpacePressed: root.section = navRow.sectionIndex
                        border.width: activeFocus ? 2 : 0
                        border.color: AppTheme.bolt
                        Accessible.role: Accessible.Button
                        Accessible.name: navRow.modelData.label
                        Accessible.focusable: true
                        Accessible.onPressAction: root.section = navRow.sectionIndex
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

            Item {
                id: bodyHost
                Layout.fillWidth: true
                Layout.fillHeight: true

            ScrollView {
                id: bodyScroll
                objectName: "spaceSettingsBodyScroll"
                anchors.fill: parent
                clip: true
                rightPadding: AppTheme.spacing8
                readonly property real minContentWidth:
                    (root.section === 1 || root.section === 2) ? 500 : 0
                contentWidth: Math.max(availableWidth, minContentWidth)
                activeFocusOnTab: true
                Keys.onPressed: (event) => {
                    var f = bodyFlick
                    var step = 40
                    var page = Math.max(40, f.height - 40)
                    var top = 0
                    var bottom = Math.max(0, f.contentHeight - f.height)
                    var y = f.contentY
                    if (event.key === Qt.Key_Down) y += step
                    else if (event.key === Qt.Key_Up) y -= step
                    else if (event.key === Qt.Key_PageDown) y += page
                    else if (event.key === Qt.Key_PageUp) y -= page
                    else if (event.key === Qt.Key_Home) y = top
                    else if (event.key === Qt.Key_End) y = bottom
                    else return
                    f.contentY = Math.max(top, Math.min(bottom, y))
                    event.accepted = true
                }

                ColumnLayout {
                    width: parent ? parent.width : 0
                    spacing: AppTheme.spacing16

                    Label {
                        Layout.fillWidth: true
                        Layout.topMargin: AppTheme.spacing12
                        visible: !root.infoIsOurs
                        wrapMode: Text.WordWrap
                        color: AppTheme.stormTextMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                        text: root.roomMode
                              ? qsTr("Loading this room's members. Controls remain disabled until the roster arrives.")
                              : qsTr("Loading this space's members. What you "
                                     + "can change depends on them, so the "
                                     + "controls stay disabled until they "
                                     + "arrive.")
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: AppTheme.spacing12
                        visible: root.section === 0
                        spacing: AppTheme.spacing16

                        NativeFileDialog {
                            id: spaceAvatarFile
                            purpose: "image"
                            title: qsTr("Choose space avatar")
                            currentFolder: app.defaultFileDialogFolder()
                            fileMode: "open"
                            nameFilters: [qsTr("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp *.svg)")]
                            onAccepted: spaceAvatarCrop.openFor(selectedFile)
                        }
                        ImageCropDialog {
                            id: spaceAvatarCrop
                            role: "avatar"
                            onCropped: function (file) {
                                app.roomInfo.setRoomAvatar(file)
                            }
                        }
                        NativeFileDialog {
                            id: spaceBannerFile
                            purpose: "image"
                            title: qsTr("Choose a banner image")
                            currentFolder: app.defaultFileDialogFolder()
                            fileMode: "open"
                            nameFilters: [qsTr("Images (*.png *.jpg *.jpeg *.gif *.webp *.svg)")]
                            onAccepted: spaceBannerCrop.openFor(selectedFile)
                        }
                        ImageCropDialog {
                            id: spaceBannerCrop
                            role: "banner"
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
                                        AvatarViewAction {}
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
                                SpaceTextEditRow {
                                    id: nameField
                                    Layout.fillWidth: true
                                    storm: true
                                    targetId: root.spaceId
                                    authoritative: root.info.name || ""
                                    targetCurrent: root.infoIsOurs
                                    canEdit: root.infoIsOurs
                                             && app.roomInfo.canEditName
                                    pending: root.infoIsOurs
                                             && app.roomInfo.editPending
                                    requireNonEmpty: true
                                    buttonText: qsTr("Rename")
                                    fieldObjectName: "spaceSettingsNameField"
                                    buttonObjectName: "spaceSettingsRenameButton"
                                    accessibleName: qsTr("Space name")
                                    onSubmitted: (value) => {
                                        if (root.infoIsOurs && !app.roomInfo.editPending)
                                            app.roomInfo.setRoomName(value)
                                    }
                                }

                                Label {
                                    text: qsTr("Topic")
                                    color: AppTheme.stormTextMuted
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                }
                                SpaceTextEditRow {
                                    id: topicField
                                    Layout.fillWidth: true
                                    storm: true
                                    targetId: root.spaceId
                                    authoritative: root.info.topic || ""
                                    targetCurrent: root.infoIsOurs
                                    canEdit: root.infoIsOurs
                                             && app.roomInfo.canEditTopic
                                    pending: root.infoIsOurs
                                             && app.roomInfo.editPending
                                    buttonText: qsTr("Save")
                                    fieldObjectName: "spaceSettingsTopicField"
                                    buttonObjectName: "spaceSettingsTopicSaveButton"
                                    accessibleName: qsTr("Space topic")
                                    onSubmitted: (value) => {
                                        if (root.infoIsOurs && !app.roomInfo.editPending)
                                            app.roomInfo.setRoomTopic(value)
                                    }
                                }
                            }
                        }

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
                                        property int resolveTick: 0
                                        source: {
                                            var _tick = resolveTick
                                            return mxc.length > 0
                                                   && app.mediaBridge.supported
                                                   ? app.mediaBridge.wideImageSource(mxc)
                                                   : ""
                                        }
                                        Component.onCompleted: {
                                            if (mxc.length > 0
                                                    && source.toString().length === 0)
                                                resolveTick++
                                        }
                                        Connections {
                                            target: app.mediaBridge
                                            enabled: bannerPreview.mxc.length > 0
                                            function onMediaCached(key) {
                                                if (key.endsWith(":" + bannerPreview.mxc)
                                                    && bannerPreview.source.toString().length === 0)
                                                    bannerPreview.resolveTick++
                                            }
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

                        MenuSectionLabel {
                            text: qsTr("Chat background")
                            visible: chatBackgroundCard.visible
                        }
                        Rectangle {
                            id: chatBackgroundCard
                            objectName: "spaceSettingsChatBackgroundCard"
                            Layout.fillWidth: true
                            visible: !!app.backdrops && app.backdrops.sharedAvailable
                                     && root.spaceId !== ""
                            radius: AppTheme.radiusMd
                            color: AppTheme.stormInset
                            border.color: AppTheme.stormBorder
                            border.width: 1
                            implicitHeight: spaceBackdropEditor.implicitHeight
                                            + AppTheme.spacing16 * 2
                            ChatBackgroundEditor {
                                id: spaceBackdropEditor
                                anchors.fill: parent
                                anchors.margins: AppTheme.spacing16
                                scopeKind: "space"
                                scopeId: root.spaceId
                                showTitle: false
                            }
                        }

                        ColumnLayout {
                            objectName: "spaceDefaultOrderEditor"
                            Layout.fillWidth: true
                            visible: !root.roomMode
                            spacing: AppTheme.spacing8
                            MenuSectionLabel { text: qsTr("Default room order") }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text: qsTr("The starting order for everyone in this space. Personal room arrangements and sort choices take precedence.")
                                color: AppTheme.stormTextMuted
                                font.pixelSize: AppTheme.textMeta
                            }
                            Repeater {
                                model: root.orderDraft
                                delegate: RowLayout {
                                    required property int index
                                    required property var modelData
                                    Layout.fillWidth: true
                                    Label {
                                        Layout.fillWidth: true
                                        text: modelData.name || modelData.roomId
                                        textFormat: Text.PlainText
                                        elide: Text.ElideRight
                                        color: AppTheme.stormText
                                    }
                                    IconButton {
                                        objectName: "defaultRoomMoveUp"
                                        storm: true
                                        iconName: "expand_less"
                                        Accessible.name: qsTr("Move room up")
                                        enabled: root.infoIsOurs && app.roomInfo.canManageSpaceChildren
                                                 && !root.orderPending && index > 0
                                        onClicked: root.moveDefaultChild(index, -1)
                                    }
                                    IconButton {
                                        objectName: "defaultRoomMoveDown"
                                        storm: true
                                        iconName: "expand_more"
                                        Accessible.name: qsTr("Move room down")
                                        enabled: root.infoIsOurs && app.roomInfo.canManageSpaceChildren
                                                 && !root.orderPending && index + 1 < root.orderDraft.length
                                        onClicked: root.moveDefaultChild(index, 1)
                                    }
                                }
                            }
                            RowLayout {
                                AppButton {
                                    objectName: "saveDefaultRoomOrder"
                                    text: root.orderPending ? qsTr("Saving…") : qsTr("Save default order")
                                    storm: true
                                    enabled: root.infoIsOurs && app.roomInfo.canManageSpaceChildren
                                             && root.orderDirty && !root.orderPending
                                    onClicked: {
                                        root.orderPending = true
                                        root.orderStatus = ""
                                        app.spaces.setDefaultChildOrder(root.spaceId,
                                            root.orderDraft.map(function(row) { return row.roomId }))
                                    }
                                }
                                AppButton {
                                    text: qsTr("Reload order")
                                    storm: true
                                    kind: "ghost"
                                    enabled: !root.orderPending
                                    onClicked: { root.resetOrderDraft(); root.orderStatus = "" }
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: text.length > 0
                                text: root.orderStatus
                                wrapMode: Text.WordWrap
                                color: AppTheme.stormTextMuted
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
                                    model: [
                                        qsTr("Invited people only"),
                                        qsTr("Anyone with the link"),
                                        qsTr("Ask to join (knock)")
                                    ]
                                    readonly property var ruleValues:
                                        ["invite", "public", "knock"]
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
                                        onAccepted: if (aliasSaveButton.enabled)
                                                        aliasSaveButton.clicked()
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
                                        id: aliasSaveButton
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
                                AppButton {
                                    objectName: "spaceUpgradeButton"
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

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: AppTheme.spacing12
                            visible: !root.roomMode
                            spacing: AppTheme.spacing8
                            AppButton {
                                objectName: "spaceSettingsLeaveButton"
                                kind: "danger"
                                size: "sm"
                                text: qsTr("Leave Space")
                                enabled: !!app.roomInfo && !app.roomInfo.leavePending
                                onClicked: spaceLeaveConfirm.openFor(
                                               root.spaceId,
                                               root.info.name || "",
                                               undefined, undefined, undefined,
                                               spaceLeaveConfirm.soleAdminOf(
                                                   app.roomInfo, root.spaceId))
                            }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                color: AppTheme.stormTextMuted
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMeta
                                text: qsTr("Leaving does not remove the rooms inside it.")
                            }
                        }
                        LeaveRoomConfirmDialog {
                            id: spaceLeaveConfirm
                            objectName: "spaceSettingsLeaveConfirm"
                            onConfirmed: (roomId) => {
                                root.close()
                                if (app.spaces && app.spaces.activeSpaceId === roomId)
                                    app.spaces.activeSpaceId = ""
                                app.roomInfo.leaveRoom(roomId)
                            }
                        }

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

                        Label {
                            Layout.fillWidth: true
                            text: root.infoIsOurs
                                  ? qsTr("%n member(s)", "",
                                         app.roomInfo.joinedCount)
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

                        Label {
                            objectName: "spaceSettingsMembersEmpty"
                            Layout.fillWidth: true
                            visible: root.infoIsOurs
                                     && memberGroupsRepeater.count === 0
                                     && (memberFilter.text.length > 0
                                         || membersSection.membership !== "")
                            wrapMode: Text.WordWrap
                            color: AppTheme.stormTextMuted
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("No members match this name.")
                        }

                        Repeater {
                            id: memberGroupsRepeater
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

                                        TapHandler {
                                            acceptedButtons: Qt.RightButton
                                            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
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
                                            ColumnLayout {
                                                spacing: 0
                                                Label {
                                                    textFormat: Text.PlainText
                                                    Layout.alignment: Qt.AlignRight
                                                    text: memberRow.localpart
                                                    color: AppTheme.stormTextSecondary
                                                    font.family: AppTheme.monoFont
                                                    font.pixelSize: AppTheme.textMicro
                                                }
                                                Label {
                                                    textFormat: Text.PlainText
                                                    Layout.alignment: Qt.AlignRight
                                                    visible: memberRow.server.length > 0
                                                    text: memberRow.server
                                                    color: AppTheme.stormTextMuted
                                                    font.family: AppTheme.monoFont
                                                    font.pixelSize: AppTheme.textMicro
                                                }
                                            }
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
                                                Keys.onMenuPressed: clicked()
                                                Keys.onPressed: (event) => {
                                                    if (event.key === Qt.Key_F10
                                                        && (event.modifiers
                                                            & Qt.ShiftModifier)) {
                                                        clicked()
                                                        event.accepted = true
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

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
                            text: root.roomMode
                                  ? qsTr("You can only require a level at or below your own, and only set a member's role below your own — never for someone at or above it. A room using a custom level shows that number.")
                                  : qsTr("You can only require a level at or below your own, and only set a member's role below your own — never for someone at or above it. A space using a custom level shows that number rather than being relabelled.")
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: root.infoIsOurs
                                     && !app.roomInfo.canChangePowerLevels
                            wrapMode: Text.WordWrap
                            color: AppTheme.stormTextMuted
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            text: root.roomMode
                                  ? qsTr("You don't have permission to change roles or permissions in this room.")
                                  : qsTr("You don't have permission to change roles or permissions in this space.")
                        }
                        Label {
                            objectName: "spaceSettingsMatrixUnavailable"
                            Layout.fillWidth: true
                            visible: root.infoIsOurs
                                     && Object.keys(app.roomInfo.powerLevels).length === 0
                            wrapMode: Text.WordWrap
                            color: AppTheme.stormTextMuted
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            text: root.roomMode
                                  ? qsTr("This room's permission levels haven't loaded on this backend, so they are not shown.")
                                  : qsTr("This space's permission levels haven't "
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
                                            onAccepted: if (applyLevel.enabled)
                                                            applyLevel.clicked()
                                        }
                                        IconButton {
                                            id: applyLevel
                                            objectName: "spacePermApply_"
                                                        + permRow.permKey
                                            visible: permRow.known
                                            iconName: "check"
                                            implicitWidth: 28
                                            implicitHeight: 28
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

                        MenuSectionLabel {
                            Layout.fillWidth: true
                            Layout.topMargin: AppTheme.spacing12
                            text: qsTr("Member roles")
                        }

                        Repeater {
                            model: {
                                var _t = root.rosterTick
                                if (!(root.infoIsOurs
                                      && app.roomInfo.canChangePowerLevels))
                                    return []
                                return app.roomInfo.filterMembers("").filter(
                                    function (m) {
                                        return m.membership !== "banned"
                                               && m.membership !== "invited"
                                    })
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
                                AppTextField {
                                    id: customMemberLevel
                                    objectName: "memberCustomPowerLevel"
                                    storm: true
                                    Layout.preferredWidth: 68
                                    enabled: root.infoIsOurs
                                             && app.roomInfo.canChangePowerLevels
                                             && !app.roomInfo.powerLevelPending
                                    placeholderText: qsTr("Level")
                                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                                    validator: IntValidator { bottom: -100; top: 100 }
                                    readonly property int parsedLevel:
                                        text.trim().length === 0 ? -1000
                                                                 : parseInt(text.trim(), 10)
                                    Accessible.name: qsTr("Custom level for %1")
                                                         .arg(roleRow.name)
                                    onAccepted: if (applyMemberLevel.enabled)
                                                    applyMemberLevel.clicked()
                                }
                                IconButton {
                                    id: applyMemberLevel
                                    objectName: "applyMemberCustomPowerLevel"
                                    iconName: "check"
                                    implicitWidth: 28
                                    implicitHeight: 28
                                    enabled: !isNaN(customMemberLevel.parsedLevel)
                                             && app.roomInfo.canSetPowerLevel(
                                                    roleRow.uid,
                                                    customMemberLevel.parsedLevel)
                                             && !app.roomInfo.powerLevelPending
                                    Accessible.name: qsTr("Set custom level for %1")
                                                         .arg(roleRow.name)
                                    onClicked: {
                                        roleConfirm.openFor(
                                            roleRow.uid, roleRow.name,
                                            customMemberLevel.parsedLevel,
                                            qsTr("Custom (%1)").arg(
                                                customMemberLevel.parsedLevel),
                                            roleRow.isOwn)
                                        customMemberLevel.text = ""
                                    }
                                }
                            }
                        }
                    }

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
                                    id: devCopyButton
                                    objectName: "spaceSettingsDevCopy"
                                    iconName: "content_copy"
                                    implicitWidth: 26
                                    implicitHeight: 26
                                    iconSize: 15
                                    enabled: devRow.modelData.v.length > 0
                                    Accessible.name: qsTr("Copy %1")
                                                         .arg(devRow.modelData.k)
                                    property bool justCopied: false
                                    Timer {
                                        id: copiedTimer
                                        interval: 1500
                                        onTriggered: devCopyButton.justCopied = false
                                    }
                                    ToolTip.text: justCopied
                                                  ? qsTr("Copied")
                                                  : Accessible.name
                                    ToolTip.visible: hovered || justCopied
                                    ToolTip.delay: justCopied ? 0 : 500
                                    onClicked: {
                                        devCopyHelper.text = devRow.modelData.v
                                        devCopyHelper.selectAll()
                                        devCopyHelper.copy()
                                        justCopied = true
                                        copiedTimer.restart()
                                    }
                                }
                            }
                        }

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

            MiddleClickScroller {
                objectName: "spaceSettingsMiddleClickScroller"
                anchors.fill: parent
                view: bodyFlick
            }

            Connections {
                target: bodyHost.Window.window
                function onActiveFocusItemChanged() {
                    var win = bodyHost.Window.window
                    var item = win ? win.activeFocusItem : null
                    if (item)
                        root.ensureVisibleInBody(item)
                }
            }
            }
        }
    }

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
            var settable = app.roomInfo.canSetPowerLevel(uid, level)
            if (!settable && level !== current)
                continue
            options.push({ level: level,
                           label: app.roomInfo.roleLabelForLevel(level),
                           current: level === current,
                           settable: settable })
        }
        var notJoined = member.membership === "banned"
                        || member.membership === "invited"
        memberMenu.roleOptions = options.length > 1 && !notJoined ? options : []
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
                text: root.roomMode ? qsTr("Role in this room")
                                    : qsTr("Role in this space")
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
            enabled: memberMenu.canKick
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
            enabled: memberMenu.canBan
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
            enabled: memberMenu.canUnban
            iconName: "undo"
            text: qsTr("Unban…")
            onTriggered: memberAction.openFor(root.spaceId, root.info.name || "",
                                              memberMenu.targetUserId,
                                              memberMenu.targetName, "unban")
        }
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
        readonly property bool selfDemotion:
            root.infoIsOurs && isOwn && level < app.roomInfo.ownPowerLevel
            && app.roomInfo.ownPowerLevel < 9007199254740992
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
                text: (root.roomMode
                       ? qsTr("Set %1 to %2 in this room?")
                       : qsTr("Set %1 to %2 in this space? Roles in the space's rooms are set in each room."))
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
                text: root.roomMode
                      ? qsTr("You will also no longer be able to change roles or permissions in this room.")
                      : qsTr("You will also no longer be able to change roles "
                             + "or permissions in this space.")
                color: AppTheme.stormDanger
                font.pixelSize: AppTheme.textMeta
            }
        }
    }

    SpaceMemberActionDialog {
        id: memberAction
    }

    signal inviteRequested(string spaceId)

    onSpaceIdChanged: {
        nameField.reset()
        topicField.reset()
        aliasField.resetForSpace()
        joinRuleCombo.refreshRule()
    }
    Connections {
        target: app.roomInfo
        function onMembersChanged() {
            aliasField.refreshAlias()
            joinRuleCombo.refreshRule()
            root.rosterTick++
        }
    }
    Connections {
        target: app.spaces
        function onSpacesChanged() {
            root.spacesTick++
        }
    }
}
