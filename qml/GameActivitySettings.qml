import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings → Labs: "Playing X" game activity. Experimental and OFF by
// default. While a game runs, only its NAME is published, as the presence
// status ("Playing Hades II"), unless the user has set a status of their own,
// which always comes first (Discord's order).
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
        // A removal reflows the list under a stationary pointer, so the
        // second click of a double click lands on the NEXT row's Remove.
        // Only a POINTER removal arms the guard, and only for the platform's
        // own double-click interval: two keyboard removals in a row, or two
        // deliberate clicks a beat apart, are never swallowed.
        property double lastPointerRemoveMs: 0
        // An unsent draft must not outlive the page it was typed on.
        onVisibleChanged: if (!visible) entryField.text = ""

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
                    objectName: "listRemoveButton"
                    onClicked: {
                        // visualFocus is true only for focus that arrived
                        // from the keyboard; a mouse click gives activeFocus
                        // alone.
                        const keyboard = visualFocus
                        const now = Date.now()
                        if (!keyboard) {
                            if (now - editor.lastPointerRemoveMs
                                    < Qt.styleHints.mouseDoubleClickInterval)
                                return
                            editor.lastPointerRemoveMs = now
                        }
                        // The focused delegate is destroyed by the change, and
                        // so is the context this handler runs in: hand focus
                        // to the field FIRST, so Tab continues from here.
                        if (keyboard)
                            entryField.forceActiveFocus()
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
                objectName: "listEntryField"
                Layout.fillWidth: true
                storm: true
                placeholderText: editor.placeholder
                Accessible.name: editor.placeholder
                onAccepted: addButton.clicked()
                // Escape cancels a draft instead of closing Settings over it.
                // SettingsScreen owns an unconditional Escape Shortcut, and a
                // Shortcut is dispatched BEFORE the focused item sees the
                // key: without claiming the override, the handler below is
                // never reached and Settings closes over the draft.
                Keys.onShortcutOverride: (event) => {
                    if (event.key === Qt.Key_Escape && text.length > 0)
                        event.accepted = true
                }
                Keys.onEscapePressed: (event) => {
                    if (text.length > 0) {
                        text = ""
                        event.accepted = true
                    } else {
                        event.accepted = false
                    }
                }
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
        // Keyboard focus ring: the stock CheckBox draws none that is visible.
        Rectangle {
            objectName: "activityCheckFocusRing"
            anchors.fill: parent
            anchors.margins: -2
            radius: AppTheme.radiusSm
            color: "transparent"
            border.width: 2
            border.color: AppTheme.storm ? AppTheme.bolt : AppTheme.focusRing
            visible: parent.visualFocus
        }
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
                   + "see your online status. A status you set yourself is "
                   + "always shown instead while you have one. Only the game's name is "
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
