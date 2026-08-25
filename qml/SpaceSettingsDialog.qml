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
// also offers Cosmetics, Abbreviations, Emojis & Stickers and a per-Space
// Appearance. None of those is Matrix state that Lightning can read or write:
// they would be private storage only Lightning could interpret, presented as
// though it were part of the Space, and every other client — and every other
// device — would see nothing. The local rail folders are the one place this
// app keeps device-local organisation, and they are defensible precisely
// because they touch NO Matrix state and say so. A settings page that silently
// stored a Space's emoji pack in a Lightning-only format would not be. Four
// dead tabs are worse than four missing ones.
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
                            // m.room.avatar, exactly as a room's own avatar —
                            // Lightning invents no Space-specific storage.
                            onAccepted: app.roomInfo.setRoomAvatar(selectedFile)
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
                        Layout.fillWidth: true
                        Layout.topMargin: AppTheme.spacing12
                        visible: root.section === 1
                        spacing: AppTheme.spacing12

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

                        Repeater {
                            model: root.infoIsOurs
                                   ? app.roomInfo.filterMembers(memberFilter.text)
                                   : []
                            delegate: Rectangle {
                                id: memberRow
                                required property var modelData
                                Layout.fillWidth: true
                                implicitHeight: 44
                                radius: AppTheme.radiusMd
                                color: memberHover.hovered ? AppTheme.hover
                                                           : "transparent"
                                HoverHandler { id: memberHover }
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: AppTheme.spacing8
                                    anchors.rightMargin: AppTheme.spacing8
                                    spacing: AppTheme.spacing8
                                    Avatar {
                                        size: 28
                                        name: memberRow.modelData.displayName
                                              || memberRow.modelData.userId
                                        mxc: memberRow.modelData.avatarUrl || ""
                                        colorKey: memberRow.modelData.userId || ""
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 0
                                        Label {
                                            Layout.fillWidth: true
                                            elide: Label.ElideRight
                                            text: memberRow.modelData.displayName
                                                  || memberRow.modelData.userId
                                            color: AppTheme.stormText
                                            font.family: AppTheme.uiFont
                                            font.pixelSize: AppTheme.textBody
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            elide: Label.ElideRight
                                            text: memberRow.modelData.userId || ""
                                            color: AppTheme.stormTextMuted
                                            font.family: AppTheme.uiFont
                                            font.pixelSize: AppTheme.textMicro
                                        }
                                    }
                                    Label {
                                        text: root.infoIsOurs
                                              ? app.roomInfo.roleLabelForLevel(
                                                    app.roomInfo.powerLevelFor(
                                                        memberRow.modelData.userId))
                                              : ""
                                        color: AppTheme.stormTextSecondary
                                        font.family: AppTheme.uiFont
                                        font.pixelSize: AppTheme.textMeta
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
                            text: qsTr("You can only set a role below your "
                                       + "own, and never for someone at or "
                                       + "above it. A space using a custom "
                                       + "level shows that number rather than "
                                       + "being relabelled.")
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
                                       + "roles in this space.")
                        }

                        Repeater {
                            model: root.infoIsOurs
                                   && app.roomInfo.canChangePowerLevels
                                   ? app.roomInfo.filterMembers("") : []
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
                                    text: app.roomInfo.roleLabelForLevel(
                                              app.roomInfo.powerLevelFor(
                                                  roleRow.uid))
                                    color: AppTheme.stormTextSecondary
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.textMeta
                                }
                                AppButton {
                                    storm: true
                                    size: "sm"
                                    text: qsTr("Moderator")
                                    // canSetPowerLevel FAILS CLOSED on an
                                    // unknown target: levels may legitimately
                                    // be negative, so absence of the roster
                                    // row is the unknown state, never a
                                    // sentinel.
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

                        Repeater {
                            model: [
                                { k: qsTr("Space ID"), v: root.spaceId },
                                { k: qsTr("Published address"),
                                  v: root.infoIsOurs
                                     ? (app.roomInfo.canonicalAlias
                                        || qsTr("None")) : "" },
                                { k: qsTr("Join rule"),
                                  v: root.infoIsOurs ? app.roomInfo.joinRule : "" },
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
                            }
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
    }
    Connections {
        target: app.roomInfo
        function onMembersChanged() {
            nameField.refreshName()
            topicField.refreshTopic()
            aliasField.refreshAlias()
            joinRuleCombo.refreshRule()
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
