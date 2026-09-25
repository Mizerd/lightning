import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Closes a room or a Space, or deletes one from the homeserver for a server
// administrator, and shows what happened to each room.
//
// The flow lives in app.roomClosure (RoomClosureController); this view only
// renders it, so the text the user confirms is the text the tests check. A
// close is never called a delete: Matrix has none, and the consequence text
// says what stays. Rooms the plan does not offer are listed with the reason
// and cannot be selected.
AppDialog {
    id: root
    objectName: "roomCloseDialog"

    readonly property var ctl: app.roomClosure
    readonly property string phase: ctl ? ctl.phase : "idle"
    readonly property string mode: ctl ? ctl.mode : ""
    readonly property bool running: phase === "running"
    readonly property bool finished: phase === "done" || phase === "failed"

    title: ctl ? ctl.title : ""
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(560, parent ? parent.width - 64 : 560)
    standardButtons: Dialog.NoButton
    // A close or delete can run for many minutes. Hiding the dialog leaves it
    // running in the controller; the next open shows where it is, or how it
    // ended.
    closePolicy: Popup.CloseOnEscape

    // `mode` is "close" or "delete". While an earlier flow runs the
    // controller refuses the new one and sets its notice; the dialog opens on
    // the running flow so that is seen.
    function openFor(roomId, name, isSpace, mode) {
        if (!ctl)
            return
        if (ctl.begin(roomId, name, isSpace, mode)) {
            reasonField.text = ""
            typedField.text = ""
        } else if (ctl.phase === "idle") {
            // Refused with nothing to show (no client, or no administrator
            // answer for this account): open nothing.
            return
        }
        open()
    }

    onClosed: {
        if (ctl)
            ctl.viewClosed()
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        Label {
            objectName: "roomCloseNotice"
            Layout.fillWidth: true
            visible: text.length > 0
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            text: root.ctl ? root.ctl.notice : ""
            color: AppTheme.stormText
            font.weight: AppTheme.weightBold
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }

        Label {
            objectName: "roomCloseConsequence"
            Layout.fillWidth: true
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            text: root.ctl ? root.ctl.consequenceText : ""
            color: root.mode === "delete" ? AppTheme.stormDanger
                                          : AppTheme.stormTextSecondary
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textBody
        }

        // ── Close options ──
        OptionRow {
            objectName: "roomCloseLeave"
            visible: root.mode === "close" && root.phase === "ready"
            checked: root.ctl ? root.ctl.leaveAfter : false
            text: qsTr("Leave once it is closed. If anything is left undone "
                       + "you stay, so you can run it again.")
            onToggled: root.ctl.leaveAfter = !root.ctl.leaveAfter
        }
        OptionRow {
            objectName: "roomCloseUnlist"
            visible: root.mode === "close" && root.phase === "ready"
                     && root.ctl && root.ctl.parentSpaceNames.length > 0
            checked: root.ctl ? root.ctl.unlistFromParents : false
            text: root.ctl
                  ? qsTr("Also remove it from %1")
                        .arg(root.ctl.parentSpaceNames.join(", "))
                  : ""
            onToggled: root.ctl.unlistFromParents = !root.ctl.unlistFromParents
        }
        Label {
            objectName: "roomCloseLockedParents"
            Layout.fillWidth: true
            visible: root.mode === "close" && root.phase === "ready"
                     && root.ctl && root.ctl.lockedParentNames.length > 0
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            text: root.ctl
                  ? qsTr("It stays listed in %1: you can't change that "
                         + "space's rooms.")
                        .arg(root.ctl.lockedParentNames.join(", "))
                  : ""
            color: AppTheme.stormTextMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }
        AppTextField {
            id: reasonField
            objectName: "roomCloseReason"
            Layout.fillWidth: true
            storm: true
            visible: root.mode === "close" && root.phase === "ready"
            placeholderText: qsTr("Reason shown to the people removed (optional)")
        }

        // ── Delete options ──
        OptionRow {
            objectName: "roomCloseBlock"
            visible: root.mode === "delete" && root.phase === "ready"
            checked: root.ctl ? root.ctl.block : false
            text: qsTr("Stop anyone on this server joining it again")
            onToggled: root.ctl.block = !root.ctl.block
        }

        // The target's own step, first.
        RowLayout {
            objectName: "roomCloseTargetRow"
            Layout.fillWidth: true
            visible: root.phase !== "planning" && root.phase !== "idle"
                     && root.phase !== "failed"
            spacing: AppTheme.spacing8
            readonly property var row: root.ctl ? root.ctl.targetRow : ({})
            Icon {
                name: root.ctl && root.ctl.targetIsSpace ? "workspaces" : "tag"
                size: 16
                color: AppTheme.stormTextMuted
            }
            Label {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                elide: Label.ElideRight
                text: root.ctl && root.ctl.targetIsSpace
                      ? qsTr("The space itself") : qsTr("The room")
                color: AppTheme.stormText
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textBody
            }
            StepOutcome { row: parent.row }
        }

        // Cascade choice.
        RowLayout {
            objectName: "roomCloseCascade"
            Layout.fillWidth: true
            visible: root.ctl && root.ctl.targetIsSpace
                     && root.ctl.eligibleRoomCount > 0 && root.phase === "ready"
            spacing: AppTheme.spacing8
            AppSwitch {
                checked: root.ctl ? root.ctl.cascade : false
                onToggled: root.ctl.cascade = !root.ctl.cascade
                Accessible.name: cascadeText.text
            }
            Label {
                id: cascadeText
                Layout.fillWidth: true
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                text: root.ctl
                      ? qsTr("%1 (%2 of %3 selected)")
                            .arg(root.ctl.cascadeLabel)
                            .arg(root.ctl.selectedRoomCount)
                            .arg(root.ctl.eligibleRoomCount)
                      : ""
                color: AppTheme.stormText
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textBody
                TapHandler { onTapped: root.ctl.cascade = !root.ctl.cascade }
            }
        }

        ListView {
            id: roomList
            objectName: "roomCloseRooms"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 240)
            visible: count > 0 && root.phase !== "planning"
            clip: true
            model: root.ctl ? root.ctl.rooms : []
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: AppScrollBar {}
            delegate: ItemDelegate {
                id: roomRow
                required property var modelData
                width: ListView.view.width
                height: 34
                readonly property bool eligible: modelData.eligible === true
                readonly property bool choosable: eligible
                                                  && root.phase === "ready"
                                                  && root.ctl.cascade
                enabled: choosable
                onClicked: root.ctl.setRoomSelected(modelData.roomId,
                                                    !modelData.selected)
                Accessible.name: modelData.name || modelData.roomId
                background: Rectangle {
                    radius: AppTheme.radiusMd
                    color: roomRow.hovered && roomRow.choosable
                           ? AppTheme.hover : "transparent"
                }
                contentItem: RowLayout {
                    spacing: AppTheme.spacing8
                    CheckBox {
                        // The row owns the click; this is an indicator.
                        visible: roomRow.eligible && root.phase === "ready"
                        checked: roomRow.modelData.selected === true
                                 && root.ctl.cascade
                        enabled: false
                        opacity: root.ctl.cascade ? 1 : 0.5
                    }
                    Icon {
                        name: roomRow.modelData.isSpace ? "workspaces" : "tag"
                        size: 15
                        color: AppTheme.stormTextMuted
                    }
                    Label {
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                        elide: Label.ElideRight
                        text: roomRow.modelData.name || roomRow.modelData.roomId
                        color: roomRow.eligible ? AppTheme.stormText
                                                : AppTheme.stormTextMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textBody
                    }
                    StepOutcome { row: roomRow.modelData }
                }
            }
        }

        Label {
            objectName: "roomCloseTruncated"
            Layout.fillWidth: true
            visible: root.ctl && root.ctl.planTruncated
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            text: qsTr("This space has more rooms than Lightning checks at "
                       + "once. Rooms past the limit are not listed and are "
                       + "left unchanged.")
            color: AppTheme.stormTextMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }

        // Typing the name is the confirmation of a delete.
        Label {
            Layout.fillWidth: true
            visible: typedField.visible
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            text: !root.ctl ? ""
                  : root.ctl.confirmationNameUsable
                  ? qsTr("Type %1 to confirm. Its id, below, works too.")
                        .arg(root.ctl.confirmationPhrase)
                  : qsTr("Its name has characters that cannot be typed. Type "
                         + "its id, below, to confirm.")
            color: AppTheme.stormText
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }
        // The id, selectable so it can be copied.
        TextEdit {
            objectName: "roomCloseTypedId"
            Layout.fillWidth: true
            visible: typedField.visible
            readOnly: true
            selectByMouse: true
            textFormat: TextEdit.PlainText
            wrapMode: TextEdit.WrapAnywhere
            text: root.ctl ? root.ctl.targetId : ""
            color: AppTheme.stormTextSecondary
            // The text fields' own selection colour, visible on every theme.
            selectionColor: AppTheme.selectedHover
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }
        AppTextField {
            id: typedField
            objectName: "roomCloseTyped"
            Layout.fillWidth: true
            storm: true
            visible: root.mode === "delete" && root.phase === "ready"
            onTextChanged: if (root.ctl) root.ctl.setTypedConfirmation(text)
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            AppBusyIndicator {
                visible: root.phase === "planning" || root.running
                running: visible
                size: 16
            }
            Label {
                objectName: "roomCloseStatus"
                Layout.fillWidth: true
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                visible: text.length > 0
                text: root.ctl ? root.ctl.statusText : ""
                color: root.ctl && (root.ctl.failedCount > 0
                                    || root.phase === "failed")
                       ? AppTheme.stormDanger : AppTheme.stormTextMuted
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textMeta
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Item { Layout.fillWidth: true }
            AppButton {
                objectName: "roomCloseCancel"
                storm: true
                // While running this only hides the dialog; the work goes on.
                text: root.running ? qsTr("Hide")
                                   : root.finished ? qsTr("Close")
                                                   : qsTr("Cancel")
                onClicked: root.close()
            }
            AppButton {
                objectName: "roomCloseConfirm"
                storm: true
                visible: !root.finished
                kind: "dangerPrimary"
                text: root.ctl ? root.ctl.confirmLabel : ""
                enabled: root.ctl && root.ctl.canConfirm
                onClicked: root.ctl.confirm(reasonField.text)
            }
        }
    }

    // A switch with its sentence; the whole row toggles it.
    component OptionRow: RowLayout {
        id: option
        property bool checked: false
        property string text: ""
        signal toggled()
        Layout.fillWidth: true
        spacing: AppTheme.spacing8
        AppSwitch {
            checked: option.checked
            onToggled: option.toggled()
            Accessible.name: option.text
        }
        Label {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            text: option.text
            color: AppTheme.stormText
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textBody
            TapHandler { onTapped: option.toggled() }
        }
    }

    // One row's outcome: before confirming, what closing does there or why
    // it is not offered; while running, its progress; after, what happened.
    component StepOutcome: RowLayout {
        id: outcome
        property var row: ({})
        readonly property string status: row && row.status ? row.status : ""
        spacing: AppTheme.spacing4
        Layout.maximumWidth: 300
        Icon {
            visible: outcome.status === "ok" || outcome.status === "failed"
                     || outcome.status === "partial"
            name: outcome.status === "ok" ? "check_circle" : "error"
            size: 15
            color: outcome.status === "ok" ? AppTheme.stormSuccess
                                           : AppTheme.stormDanger
        }
        Label {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            elide: Label.ElideRight
            horizontalAlignment: Text.AlignRight
            text: {
                var r = outcome.row || {}
                if (outcome.status === "failed" || outcome.status === "ok"
                        || outcome.status === "partial"
                        || outcome.status === "following"
                        || outcome.status === "unknown")
                    return r.message || ""
                if (outcome.status === "running")
                    return r.progress || qsTr("Working…")
                if (outcome.status === "pending")
                    return qsTr("Waiting")
                if (r.eligible === false)
                    return r.reasonText || ""
                if (outcome.status === "skipped")
                    return qsTr("Skipped")
                if (r.alsoElsewhere > 0)
                    return qsTr("Also in another space")
                return r.summary || ""
            }
            color: outcome.status === "failed" || outcome.status === "partial"
                   || outcome.status === "unknown"
                   ? AppTheme.stormDanger : AppTheme.stormTextMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
            ToolTip.visible: hover.hovered && text.length > 0 && truncated
            ToolTip.text: text
            HoverHandler { id: hover }
        }
    }
}
