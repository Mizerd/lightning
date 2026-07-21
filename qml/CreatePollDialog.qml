import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// MSC3381 poll creation (v0.7). Pure presentation: the dialog collects the
// question, 2..20 answers, the disclosure kind, and whether voters may pick
// several answers, then hands everything to app.composer.createPoll — the
// Rust bridge builds the actual Matrix content through ruma constructors.
// The composer routes to the open thread when it is in thread-reply mode.
Dialog {
    id: root
    objectName: "createPollDialog"

    readonly property int maxAnswers: 20
    // Answer texts, index-addressed; the ListModel keeps TextField focus
    // stable while rows are added/removed.
    property var answerModel: ListModel {}

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
    }
    function answerTexts() {
        var texts = []
        for (var i = 0; i < answerModel.count; ++i) {
            var t = (answerModel.get(i).answerText || "").trim()
            if (t.length > 0) texts.push(t)
        }
        return texts
    }
    readonly property bool formValid:
        questionField.text.trim().length > 0 && answerTexts().length >= 2

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
    width: Math.min(440, (parent ? parent.width : 440) - AppTheme.spacing24 * 2)
    padding: AppTheme.spacing20

    Popup {
        id: discardConfirm
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        focus: true
        padding: AppTheme.spacing16
        background: Rectangle {
            color: AppTheme.surface
            radius: AppTheme.radiusMd
            border.color: AppTheme.borderStrong
            border.width: 1
        }
        contentItem: ColumnLayout {
            spacing: AppTheme.spacing12
            Label {
                text: qsTr("Discard this poll draft?")
                color: AppTheme.text
                font.pixelSize: 14
                font.weight: Font.DemiBold
            }
            RowLayout {
                spacing: AppTheme.spacing8
                Item { Layout.fillWidth: true }
                AppButton {
                    objectName: "pollDiscardKeepButton"
                    text: qsTr("Keep editing")
                    onClicked: discardConfirm.close()
                }
                AppButton {
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
        color: AppTheme.surface
        radius: AppTheme.radiusLg
        border.color: AppTheme.border
        border.width: 1
    }

    onClosed: resetAll()

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Label {
                text: qsTr("Create a poll")
                color: AppTheme.text
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.fontSizeL
                font.weight: Font.DemiBold
                Layout.fillWidth: true
            }
            IconButton {
                objectName: "createPollCloseButton"
                iconName: "close"
                iconSize: 18
                implicitWidth: 28; implicitHeight: 28
                Accessible.name: qsTr("Close poll creation")
                onClicked: root.maybeClose()
            }
        }

        Label {
            text: qsTr("Question")
            color: AppTheme.textMuted
            font.pixelSize: 12
        }
        AppTextField {
            id: questionField
            objectName: "pollQuestionField"
            Layout.fillWidth: true
            placeholderText: qsTr("Ask a question…")
            Accessible.name: qsTr("Poll question")
            onAccepted: if (root.formValid) root.submit()
        }

        Label {
            text: qsTr("Answers")
            color: AppTheme.textMuted
            font.pixelSize: 12
        }
        Repeater {
            id: answerRepeater
            model: root.answerModel
            delegate: RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                AppTextField {
                    objectName: "pollAnswerField"
                    Layout.fillWidth: true
                    placeholderText: qsTr("Answer %1").arg(index + 1)
                    text: model.answerText
                    Accessible.name: qsTr("Poll answer %1").arg(index + 1)
                    onTextEdited: root.answerModel.setProperty(
                                      index, "answerText", text)
                    onAccepted: {
                        if (index === root.answerModel.count - 1
                            && root.answerModel.count < root.maxAnswers)
                            root.answerModel.append({ answerText: "" })
                        else if (root.formValid)
                            root.submit()
                    }
                }
                IconButton {
                    objectName: "pollAnswerRemoveButton"
                    iconName: "close"
                    iconSize: 14
                    implicitWidth: 24; implicitHeight: 24
                    // A poll needs two answers; the last two rows only clear.
                    enabled: root.answerModel.count > 2
                    Accessible.name: qsTr("Remove answer %1").arg(index + 1)
                    onClicked: root.answerModel.remove(index)
                }
            }
        }
        AppButton {
            objectName: "pollAddAnswerButton"
            text: qsTr("Add answer")
            enabled: root.answerModel.count < root.maxAnswers
            Accessible.name: qsTr("Add another poll answer")
            onClicked: root.answerModel.append({ answerText: "" })
        }

        // Options — the switch rows follow the Settings pattern: the whole
        // row is clickable, AppSwitch itself stays a bare bound control.
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
                        color: AppTheme.text
                        font.pixelSize: 13
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                    }
                    Label {
                        text: qsTr("Voters see the tallies only after you end the poll.")
                        color: AppTheme.textMuted
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
                    color: AppTheme.text
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

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: AppTheme.spacing8
            spacing: AppTheme.spacing8
            Item { Layout.fillWidth: true }
            AppButton {
                objectName: "createPollCancelButton"
                text: qsTr("Cancel")
                onClicked: root.maybeClose()
            }
            AppButton {
                objectName: "createPollSubmitButton"
                kind: "primary"
                text: qsTr("Create poll")
                enabled: root.formValid
                Accessible.name: qsTr("Create poll")
                onClicked: root.submit()
            }
        }
    }
}
