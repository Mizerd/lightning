import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings → Labs: "Playing X" game activity. Experimental and OFF by
// default. While a game runs, only its NAME is published, as the presence
// status ("Playing Hades II"); the previous status returns when it exits.
// Nothing is sent while presence sharing is off. The process list never
// leaves this machine. Everything here is local policy over app.settings;
// detection and publishing live in GameActivity / PresenceManager.
ColumnLayout {
    id: root
    objectName: "gameActivitySettings"
    spacing: AppTheme.spacing8

    readonly property bool sharing: !!app.settings && app.settings.sharePresence
    readonly property bool featureOn: !!app.settings && app.settings.gameActivityEnabled
    readonly property string current: app.gameActivity ? app.gameActivity.currentGame : ""

    // One editable list of short strings kept in a settings property.
    component ListEditor: ColumnLayout {
        id: editor
        property var items: []
        property string placeholder: ""
        property string addLabel: qsTr("Add")
        property string removeLabel: qsTr("Remove")
        signal changed(var next)
        spacing: AppTheme.spacing4

        Repeater {
            model: editor.items
            delegate: RowLayout {
                required property string modelData
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                Label {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    text: modelData
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textBody
                }
                AppButton {
                    storm: true
                    kind: "ghost"
                    size: "sm"
                    text: editor.removeLabel
                    Accessible.name: qsTr("%1: %2").arg(editor.removeLabel).arg(modelData)
                    onClicked: {
                        var next = editor.items.slice()
                        next.splice(next.indexOf(modelData), 1)
                        editor.changed(next)
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            AppTextField {
                id: entryField
                Layout.fillWidth: true
                storm: true
                placeholderText: editor.placeholder
                Accessible.name: editor.placeholder
                onAccepted: addButton.clicked()
            }
            AppButton {
                id: addButton
                storm: true
                text: editor.addLabel
                enabled: entryField.text.trim().length > 0
                onClicked: {
                    var v = entryField.text.trim()
                    if (v.length === 0)
                        return
                    var next = editor.items.slice()
                    next.push(v)
                    editor.changed(next)
                    entryField.text = ""
                }
            }
        }
    }

    CheckBox {
        objectName: "gameActivityCheck"
        palette.windowText: AppTheme.stormText
        text: qsTr("Show the game I'm playing")
        checked: root.featureOn
        onToggled: app.settings.gameActivityEnabled = checked
    }
    Label {
        Layout.fillWidth: true
        Layout.leftMargin: AppTheme.spacing4
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        lineHeight: AppTheme.lineHeightBody
        lineHeightMode: Text.ProportionalHeight
        color: AppTheme.stormTextSecondary
        font.pixelSize: AppTheme.textMeta
        text: qsTr("An experiment. While a game is running, your status "
                   + "reads “Playing <game>” for the people who can "
                   + "see your online status, and your previous status "
                   + "returns when the game closes. Only the game's name is "
                   + "sent, never your list of programs. Lightning looks at "
                   + "the programs running on this computer every 15 seconds "
                   + "while this is on, and does nothing when it is off.")
    }
    Label {
        objectName: "gameActivitySharingNote"
        Layout.fillWidth: true
        Layout.leftMargin: AppTheme.spacing4
        visible: root.featureOn && !root.sharing
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        text: qsTr("Nothing is sent while “Share my online status” is off.")
    }
    Label {
        objectName: "gameActivitySandboxNote"
        Layout.fillWidth: true
        Layout.leftMargin: AppTheme.spacing4
        visible: !!app.gameActivity && app.gameActivity.limitedDetection
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        text: qsTr("This build runs in a sandbox that cannot see the other "
                   + "programs on your computer, so games are detected only "
                   + "partly or not at all.")
    }
    Label {
        objectName: "gameActivityUnsupportedNote"
        Layout.fillWidth: true
        Layout.leftMargin: AppTheme.spacing4
        visible: !!app.gameActivity && !app.gameActivity.detectionSupported
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
        text: qsTr("Game detection is not available on this system yet, so "
                   + "no game will be shown.")
    }
    Label {
        objectName: "gameActivityCurrentLine"
        Layout.fillWidth: true
        Layout.leftMargin: AppTheme.spacing4
        visible: root.featureOn && root.sharing
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        color: AppTheme.stormTextSecondary
        font.pixelSize: AppTheme.textMeta
        text: root.current.length > 0
              ? qsTr("Showing: %1").arg(root.current)
              : qsTr("No game detected right now.")
    }

    ColumnLayout {
        Layout.fillWidth: true
        visible: root.featureOn
        spacing: AppTheme.spacing8

        Label {
            textFormat: Text.PlainText
            text: qsTr("Never show these games")
            color: AppTheme.stormText
            font.pixelSize: AppTheme.textBody
            font.weight: AppTheme.weightStrong
            Layout.topMargin: AppTheme.spacing8
        }
        ListEditor {
            objectName: "gameActivityHideList"
            Layout.fillWidth: true
            items: app.settings ? app.settings.gameActivityHidden : []
            placeholder: qsTr("Game name")
            onChanged: (next) => app.settings.gameActivityHidden = next
        }

        Label {
            textFormat: Text.PlainText
            text: qsTr("Programs to recognise")
            color: AppTheme.stormText
            font.pixelSize: AppTheme.textBody
            font.weight: AppTheme.weightStrong
            Layout.topMargin: AppTheme.spacing8
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            color: AppTheme.stormTextMuted
            font.pixelSize: AppTheme.textMeta
            text: qsTr("Steam games are found automatically. For anything "
                       + "else, add the program's file name and the name to "
                       + "show, like: mygame.exe=My Game")
        }
        ListEditor {
            objectName: "gameActivityCustomList"
            Layout.fillWidth: true
            items: app.settings ? app.settings.gameActivityCustom : []
            placeholder: qsTr("file name=Game name")
            onChanged: (next) => app.settings.gameActivityCustom = next
        }
    }
}
