import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Confirms a kick, ban or unban from a Space and shows what happened.
//
// The flow lives in app.spaceModeration (SpaceModerationController); this
// view only renders it. The consequence text, the cascade label and every
// per-room outcome come from there, so what the user reads is what the tests
// check. Rooms the backend's plan does not offer are listed with the reason
// and cannot be selected: a partial power set is stated, not hidden.
AppDialog {
    id: root
    objectName: "spaceMemberActionDialog"

    readonly property var ctl: app.spaceModeration
    readonly property string phase: ctl ? ctl.phase : "idle"
    readonly property bool running: phase === "running"
    readonly property bool finished: phase === "done" || phase === "failed"

    title: ctl ? ctl.title : ""
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(520, parent ? parent.width - 64 : 520)
    standardButtons: Dialog.NoButton
    // Steps already sent keep running on the server; the dialog stays until
    // every one has answered so no outcome goes unreported.
    closePolicy: running ? Popup.NoAutoClose : Popup.CloseOnEscape

    // `op` is "kick", "ban" or "unban". The names are shown, never trusted.
    function openFor(spaceId, spaceName, userId, displayName, op) {
        if (!ctl || running)
            return
        reasonField.text = ""
        ctl.begin(spaceId, spaceName, userId, displayName, op)
        open()
    }

    onClosed: {
        if (ctl && !running)
            ctl.reset()
    }

    // The Space's roster changes with the action; re-read it when it is the
    // one on screen.
    Connections {
        target: root.ctl
        function onFinished(spaceId, userId, op, succeeded, failed) {
            if (succeeded > 0 && app.roomInfo
                    && app.roomInfo.roomId === spaceId)
                app.roomInfo.refreshMembers()
        }
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        Label {
            objectName: "spaceMemberActionConsequence"
            Layout.fillWidth: true
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            text: root.ctl ? root.ctl.consequenceText : ""
            color: AppTheme.stormTextSecondary
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textBody
        }

        AppTextField {
            id: reasonField
            objectName: "spaceMemberActionReason"
            Layout.fillWidth: true
            storm: true
            visible: root.phase === "ready"
            placeholderText: qsTr("Reason (optional)")
        }
        Label {
            Layout.fillWidth: true
            visible: reasonField.visible
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            text: qsTr("The reason is recorded in each room it applies to, "
                       + "where its members can read it.")
            color: AppTheme.stormTextMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }

        // The Space's own step, first.
        RowLayout {
            objectName: "spaceMemberActionSpaceRow"
            Layout.fillWidth: true
            visible: root.phase !== "planning" && root.phase !== "idle"
                     && root.phase !== "failed"
            spacing: AppTheme.spacing8
            readonly property var row: root.ctl ? root.ctl.spaceRow : ({})
            Icon {
                name: "workspaces"
                size: 16
                color: AppTheme.stormTextMuted
            }
            Label {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                elide: Label.ElideRight
                text: qsTr("The space itself")
                color: AppTheme.stormText
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textBody
            }
            StepOutcome { row: parent.row }
        }

        // Cascade choice.
        RowLayout {
            objectName: "spaceMemberActionCascade"
            Layout.fillWidth: true
            visible: root.ctl && root.ctl.eligibleRoomCount > 0
                     && root.phase === "ready"
            spacing: AppTheme.spacing8
            AppSwitch {
                objectName: "spaceMemberActionCascadeSwitch"
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

        // Every room beneath the Space: offered ones selectable, the rest
        // with the reason they are not offered.
        ListView {
            id: roomList
            objectName: "spaceMemberActionRooms"
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
            objectName: "spaceMemberActionTruncated"
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

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            AppBusyIndicator {
                visible: root.phase === "planning" || root.running
                running: visible
                size: 16
            }
            Label {
                objectName: "spaceMemberActionStatus"
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
                objectName: "spaceMemberActionCancel"
                storm: true
                text: root.finished ? qsTr("Close") : qsTr("Cancel")
                enabled: !root.running
                onClicked: root.close()
            }
            AppButton {
                objectName: "spaceMemberActionConfirm"
                storm: true
                visible: !root.finished
                kind: root.ctl && root.ctl.op === "unban" ? "primary"
                                                          : "dangerPrimary"
                text: root.ctl ? root.ctl.confirmLabel : ""
                enabled: root.ctl && root.ctl.canConfirm
                onClicked: root.ctl.confirm(reasonField.text)
            }
        }
    }

    // One row's outcome: before confirming, why it is not offered; after,
    // what happened to it.
    component StepOutcome: RowLayout {
        id: outcome
        property var row: ({})
        readonly property string status: row && row.status ? row.status : ""
        spacing: AppTheme.spacing4
        Layout.maximumWidth: 260
        Icon {
            visible: outcome.status === "ok" || outcome.status === "failed"
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
                if (outcome.status === "failed")
                    return r.message || ""
                if (outcome.status === "ok")
                    return qsTr("Done")
                if (outcome.status === "running")
                    return qsTr("Working…")
                if (outcome.status === "pending")
                    return qsTr("Waiting")
                if (r.eligible === false)
                    return r.reasonText || ""
                if (outcome.status === "skipped")
                    return qsTr("Skipped")
                return ""
            }
            color: outcome.status === "failed" ? AppTheme.stormDanger
                                               : AppTheme.stormTextMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }
    }
}
