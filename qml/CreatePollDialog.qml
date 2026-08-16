import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// MSC3381 poll creation (v0.7). Pure presentation: the dialog collects the
// question, 2..20 answers, the disclosure kind, and whether voters may pick
// several answers, then hands everything to app.composer.createPoll — the
// Rust bridge builds the actual Matrix content through ruma constructors.
// The composer routes to the open thread when it is in thread-reply mode.
//
// v0.6.5 (SPEC 1s): two-column presentation — the form (unchanged validation,
// dirty-guard, dynamic closePolicy, room-change auto-close) on the left, a
// live "PREVIEW · AS SENT" panel mirroring the form's local state on the
// right. Deviation: the add-answer row uses a solid subtle border rather
// than a dashed one — a Canvas-drawn dash was judged disproportionate effort
// for this affordance (reported to the lead).
Dialog {
    id: root
    objectName: "createPollDialog"

    readonly property int maxAnswers: 20
    // Answer texts, index-addressed; the ListModel keeps TextField focus
    // stable while rows are added/removed.
    property var answerModel: ListModel {}
    // Bumped on every per-row text edit so the live preview (which reads
    // answerTexts() through previewAnswers below) stays reactive — ListModel
    // row values change via setProperty() without a tracked QML dependency
    // of their own; row insert/remove already retint through the model's
    // real countChanged.
    property int answerRevision: 0
    function bumpAnswerRevision() { answerRevision = answerRevision + 1 }

    function openDialog() {
        resetAll()
        open()
        questionField.forceActiveFocus()
    }
    function resetAll() {
        questionField.text = ""
        answerModel.clear()
        answerModel.append({ answerText: "" })
        answerModel.append({ answerText: "" })
        undisclosedSwitch.checked = false
        multipleSwitch.checked = false
        answerRevision = 0
    }
    function answerTexts() {
        var texts = []
        for (var i = 0; i < answerModel.count; ++i) {
            var t = (answerModel.get(i).answerText || "").trim()
            if (t.length > 0) texts.push(t)
        }
        return texts
    }
    // Live "as sent" preview data — depends on answerRevision explicitly so
    // per-row text edits (not just row count changes) refresh it.
    readonly property var previewAnswers: { answerRevision; return answerTexts() }
    // Same explicit answerRevision dependency as previewAnswers: ListModel
    // setProperty() edits carry no QML-tracked dependency of their own, so
    // without it the Create button would not react to per-row text edits.
    readonly property bool formValid: {
        answerRevision
        return questionField.text.trim().length > 0
               && answerTexts().length >= 2
    }

    function submit() {
        if (!formValid) return
        var answers = answerTexts()
        app.composer.createPoll(
            questionField.text.trim(), answers,
            undisclosedSwitch.checked,
            multipleSwitch.checked ? answers.length : 1)
        close()
    }

    // Anything typed makes the poll "dirty": click-outside no longer
    // discards silently, and Cancel/X ask before dropping the draft.
    // Escape stays a deliberate close for keyboard users.
    readonly property bool dirty:
        questionField.text.trim().length > 0 || answerTexts().length > 0
    function maybeClose() {
        if (dirty)
            discardConfirm.open()
        else
            close()
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    focus: true
    closePolicy: dirty ? Popup.CloseOnEscape
                       : (Popup.CloseOnEscape | Popup.CloseOnPressOutside)
    width: Math.min(680, (parent ? parent.width : 680) - AppTheme.spacing24 * 2)
    // Clamp to the window so the body Flickable actually engages — without
    // this the dialog grows with its implicit height and never scrolls.
    height: Math.min(implicitHeight,
                     parent ? parent.height - AppTheme.spacing24 * 2 : implicitHeight)
    padding: AppTheme.spacing20

    Popup {
        id: discardConfirm
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        focus: true
        padding: AppTheme.spacing16
        background: Rectangle {
            color: AppTheme.stormPanel
            radius: AppTheme.radiusMd
            border.color: AppTheme.stormBorderStrong
            border.width: 1
        }
        contentItem: ColumnLayout {
            spacing: AppTheme.spacing12
            Label {
                text: qsTr("Discard this poll draft?")
                color: AppTheme.stormText
                font.pixelSize: 14
                font.weight: Font.DemiBold
            }
            RowLayout {
                spacing: AppTheme.spacing8
                Item { Layout.fillWidth: true }
                AppButton {
                    storm: true
                    objectName: "pollDiscardKeepButton"
                    text: qsTr("Keep editing")
                    onClicked: discardConfirm.close()
                }
                AppButton {
                    storm: true
                    objectName: "pollDiscardConfirmButton"
                    kind: "danger"
                    text: qsTr("Discard")
                    onClicked: {
                        discardConfirm.close()
                        root.close()
                    }
                }
            }
        }
    }

    background: Rectangle {
        color: AppTheme.stormPanel
        radius: AppTheme.radiusLg
        border.color: AppTheme.stormBorder
        border.width: 1
    }

    onClosed: resetAll()

    // Notification routing or the quick switcher can change the room while
    // the dialog is open; a draft must never post into the wrong room.
    Connections {
        target: app
        function onCurrentRoomIdChanged() {
            if (root.visible) root.close()
        }
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Label {
                text: qsTr("Create a poll")
                color: AppTheme.stormText
                font.family: AppTheme.menuFont
                font.pixelSize: AppTheme.fontSizeL
                font.weight: Font.DemiBold
                Layout.fillWidth: true
            }
            IconButton {
                storm: true
                objectName: "createPollCloseButton"
                iconName: "close"
                iconSize: 18
                implicitWidth: 28; implicitHeight: 28
                Accessible.name: qsTr("Close poll creation")
                onClicked: root.maybeClose()
            }
        }

        // ── Form + preview body (scrolls when height-constrained; the
        // answer list grows to 20 rows, which must never push the title or
        // the Send/Cancel row outside the window) ────────────────────────
        Flickable {
            id: pollBodyFlick
            Layout.fillWidth: true
            Layout.fillHeight: true
            implicitHeight: pollBody.implicitHeight
            contentWidth: width
            contentHeight: pollBody.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            // Same wheel/touchpad feel as the room timeline; see
            // qml/SmoothWheelArea.qml.
            SmoothWheelArea {}

            RowLayout {
                id: pollBody
                width: pollBodyFlick.width
                spacing: AppTheme.spacing16

                // ── Left: form ──────────────────────────────────────────────
                ColumnLayout {
                    Layout.preferredWidth: 320
                    Layout.alignment: Qt.AlignTop
                    spacing: AppTheme.spacing8

                    MenuSectionLabel { text: qsTr("QUESTION") }
                    AppTextField {
                        storm: true
                        id: questionField
                        objectName: "pollQuestionField"
                        Layout.fillWidth: true
                        placeholderText: qsTr("Ask a question…")
                        Accessible.name: qsTr("Poll question")
                        onAccepted: if (root.formValid) root.submit()
                    }

                    MenuSectionLabel {
                        text: qsTr("OPTIONS")
                        Layout.topMargin: AppTheme.spacing8
                    }
                    Repeater {
                        id: answerRepeater
                        model: root.answerModel
                        delegate: Rectangle {
                            Layout.fillWidth: true
                            radius: AppTheme.radiusTile
                            color: "transparent"
                            border.width: 1
                            border.color: AppTheme.stormBorder
                            implicitHeight: answerRow.implicitHeight + AppTheme.spacing8

                            RowLayout {
                                id: answerRow
                                anchors.fill: parent
                                anchors.margins: AppTheme.spacing4
                                spacing: AppTheme.spacing8
                                AppTextField {
                                    storm: true
                                    objectName: "pollAnswerField"
                                    Layout.fillWidth: true
                                    placeholderText: qsTr("Answer %1").arg(index + 1)
                                    text: model.answerText
                                    Accessible.name: qsTr("Poll answer %1").arg(index + 1)
                                    onTextEdited: {
                                        root.answerModel.setProperty(
                                            index, "answerText", text)
                                        root.bumpAnswerRevision()
                                    }
                                    onAccepted: {
                                        if (index === root.answerModel.count - 1
                                            && root.answerModel.count < root.maxAnswers)
                                            root.answerModel.append({ answerText: "" })
                                        else if (root.formValid)
                                            root.submit()
                                    }
                                }
                                IconButton {
                                    storm: true
                                    objectName: "pollAnswerRemoveButton"
                                    iconName: "close"
                                    iconSize: 14
                                    implicitWidth: 24; implicitHeight: 24
                                    // A poll needs two answers; the last two
                                    // rows only clear.
                                    enabled: root.answerModel.count > 2
                                    Accessible.name: qsTr("Remove answer %1").arg(index + 1)
                                    onClicked: {
                                        root.answerModel.remove(index)
                                        root.bumpAnswerRevision()
                                    }
                                }
                            }
                        }
                    }

                    // Add-option row. Deviation (reported): solid subtle border
                    // rather than a dashed one — see the file header comment.
                    Rectangle {
                        id: addAnswerRow
                        objectName: "pollAddAnswerButton"
                        Layout.fillWidth: true
                        radius: AppTheme.radiusTile
                        color: addAnswerHover.hovered && enabled
                               ? AppTheme.stormSelection : "transparent"
                        border.width: 1
                        border.color: AppTheme.stormBorder
                        implicitHeight: addAnswerContent.implicitHeight
                                        + AppTheme.spacing8 * 2
                        enabled: root.answerModel.count < root.maxAnswers
                        opacity: enabled ? 1.0 : 0.5
                        activeFocusOnTab: enabled
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Add another poll answer")
                        function activateAdd() {
                            if (enabled) root.answerModel.append({ answerText: "" })
                        }
                        Keys.onReturnPressed: activateAdd()
                        Keys.onSpacePressed: activateAdd()
                        // stormLink, not bolt: Send poll is this surface's
                        // one yellow primary (§1 yellow discipline).
                        RowLayout {
                            id: addAnswerContent
                            anchors.centerIn: parent
                            spacing: AppTheme.spacing6
                            Icon { name: "add"; size: 16; color: AppTheme.stormLink }
                            Label {
                                text: qsTr("Add answer")
                                color: AppTheme.stormLink
                                font.pixelSize: AppTheme.fontSizeS
                                font.weight: Font.DemiBold
                            }
                        }
                        TapHandler {
                            enabled: addAnswerRow.enabled
                            onTapped: addAnswerRow.activateAdd()
                        }
                        HoverHandler {
                            id: addAnswerHover
                            cursorShape: addAnswerRow.enabled
                                ? Qt.PointingHandCursor : Qt.ArrowCursor
                        }
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: -3
                            radius: parent.radius + 3
                            color: "transparent"
                            border.width: 2
                            border.color: AppTheme.bolt
                            visible: addAnswerRow.activeFocus
                        }
                    }

                    // Options — the switch rows follow the Settings pattern: the
                    // whole row is clickable, AppSwitch itself stays a bare
                    // bound control.
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: AppTheme.spacing8
                        spacing: AppTheme.spacing8

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing12
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Label {
                                    text: qsTr("Hide results until the poll ends")
                                    color: AppTheme.stormText
                                    font.pixelSize: 13
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                }
                                Label {
                                    text: qsTr("Voters see the tallies only after you end the poll.")
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: 11
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                }
                            }
                            AppSwitch {
                                id: undisclosedSwitch
                                objectName: "pollUndisclosedSwitch"
                                Accessible.name: qsTr("Hide results until the poll ends")
                                onToggled: checked = !checked
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing12
                            Label {
                                text: qsTr("Allow choosing multiple answers")
                                color: AppTheme.stormText
                                font.pixelSize: 13
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                            }
                            AppSwitch {
                                id: multipleSwitch
                                objectName: "pollMultipleSwitch"
                                Accessible.name: qsTr("Allow choosing multiple answers")
                                onToggled: checked = !checked
                            }
                        }
                    }
                }

                // ── Right: live "as sent" preview ────────────────────────────
                ColumnLayout {
                    Layout.preferredWidth: 260
                    Layout.fillHeight: true
                    Layout.alignment: Qt.AlignTop
                    spacing: AppTheme.spacing8

                    MenuSectionLabel { text: qsTr("PREVIEW · AS SENT") }

                    // Storm: the preview module is a stormInset panel like
                    // every other nested module. "AS SENT" promises CONTENT
                    // fidelity (question/options/votes), not the viewer's
                    // timeline palette — a themed fill inside the storm
                    // dialog read as a foreign white card on light themes.
                    Rectangle {
                        objectName: "pollPreviewPanel"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.minimumHeight: 220
                        radius: AppTheme.radiusLg
                        color: AppTheme.stormInset
                        border.width: 1
                        border.color: AppTheme.stormBorder

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: AppTheme.spacing12
                            spacing: AppTheme.spacing8

                            Label {
                                objectName: "pollPreviewQuestion"
                                Layout.fillWidth: true
                                text: questionField.text.trim().length > 0
                                      ? questionField.text
                                      : qsTr("Ask a question…")
                                color: questionField.text.trim().length > 0
                                       ? AppTheme.stormText : AppTheme.stormTextMuted
                                font.pixelSize: AppTheme.fontResult
                                font.weight: Font.Bold
                                wrapMode: Text.WordWrap
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: AppTheme.spacing6
                                Repeater {
                                    model: root.previewAnswers
                                    delegate: Rectangle {
                                        id: previewOption
                                        required property string modelData
                                        required property int index
                                        readonly property bool first: index === 0
                                        Layout.fillWidth: true
                                        radius: AppTheme.radiusTile
                                        color: first ? AppTheme.stormSelection : AppTheme.stormPanel
                                        border.width: 1
                                        border.color: first ? AppTheme.stormBorderStrong
                                                            : AppTheme.stormBorder
                                        // The label consumes spacing8 margins on
                                        // BOTH edges — a single-spacing budget
                                        // left the row 8px short, running the
                                        // glyph descenders into the pill border.
                                        implicitHeight: previewOptionLabel.implicitHeight
                                                        + AppTheme.spacing8 * 2
                                        Label {
                                            id: previewOptionLabel
                                            anchors.fill: parent
                                            anchors.margins: AppTheme.spacing8
                                            text: previewOption.modelData
                                            color: previewOption.first
                                                   ? AppTheme.stormText : AppTheme.stormTextSecondary
                                            font.pixelSize: AppTheme.fontSizeS
                                            elide: Label.ElideRight
                                        }
                                    }
                                }
                                Label {
                                    objectName: "pollPreviewPlaceholder"
                                    visible: root.previewAnswers.length === 0
                                    text: qsTr("Options appear here as you type them.")
                                    color: AppTheme.stormTextMuted
                                    font.pixelSize: AppTheme.fontSizeXS
                                    wrapMode: Text.WordWrap
                                    Layout.fillWidth: true
                                }
                            }

                            Item { Layout.fillHeight: true }

                            Label {
                                objectName: "pollPreviewVoteLine"
                                text: undisclosedSwitch.checked
                                      ? qsTr("0 votes · anonymous")
                                      : qsTr("0 votes")
                                color: AppTheme.stormTextMuted
                                font.pixelSize: AppTheme.fontSizeXS
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: AppTheme.spacing8
            spacing: AppTheme.spacing8
            Item { Layout.fillWidth: true }
            AppButton {
                storm: true
                objectName: "createPollCancelButton"
                text: qsTr("Cancel")
                onClicked: root.maybeClose()
            }
            AppButton {
                storm: true
                objectName: "createPollSubmitButton"
                kind: "primary"
                text: qsTr("Send poll")
                enabled: root.formValid
                Accessible.name: qsTr("Send poll")
                onClicked: root.submit()
            }
        }
    }
}
