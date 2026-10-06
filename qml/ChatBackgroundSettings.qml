import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings -> Appearance: chat backgrounds and surface depth.
//
//   * Your own background for every room (ChatBackgroundEditor, "default").
//   * "Show backgrounds set by others" (app.backdrops.showShared, default on).
//     Off hides every room's and Space's shared picture; your own still show.
//   * Depth: flat surfaces, or a gentle light-from-above gradient on the
//     window's large grounds that only ever raises text contrast.
ColumnLayout {
    id: section
    objectName: "chatBackgroundSettings"
    spacing: AppTheme.spacing8

    readonly property bool available: typeof app !== "undefined" && app
                                      && !!app.backdrops

    component GroupLabel: Label {
        Layout.topMargin: AppTheme.spacing8
        color: AppTheme.stormTextMuted
        font.family: AppTheme.menuSectionFont
        font.pixelSize: AppTheme.menuSectionSize
        font.weight: AppTheme.menuSectionWeight
        font.letterSpacing: AppTheme.menuSectionTracking
    }
    component Hint: Label {
        Layout.fillWidth: true
        Layout.leftMargin: AppTheme.spacing4
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        lineHeight: AppTheme.lineHeightBody
        lineHeightMode: Text.ProportionalHeight
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
    }

    GroupLabel { text: qsTr("Chat background") }
    ChatBackgroundEditor {
        objectName: "chatBackgroundDefaultEditor"
        Layout.fillWidth: true
        Layout.maximumWidth: 520
        scopeKind: "default"
        showTitle: false
        visible: section.available
    }
    Hint {
        text: qsTr("Your own picture behind every conversation. Only you see "
                   + "it. A room or space can set a background everyone in it "
                   + "sees, and you can choose your own for a single room from "
                   + "Room information.")
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: AppTheme.spacing12
        visible: section.available
        AppSwitch {
            id: showSharedSwitch
            objectName: "showSharedBackgroundsSwitch"
            checked: section.available && app.backdrops.showShared
            onToggled: app.backdrops.showShared = !checked
            Accessible.name: showSharedLabel.text
        }
        Label {
            id: showSharedLabel
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            color: AppTheme.stormText
            font.pixelSize: AppTheme.textBody
            text: qsTr("Show backgrounds set by others")
            TapHandler { onTapped: app.backdrops.showShared = !showSharedSwitch.checked }
        }
    }
    Hint {
        text: qsTr("Rooms and spaces can share a picture with everyone in them. "
                   + "Turn this off to see only your own.")
    }

    GroupLabel { text: qsTr("Depth") }
    SegmentedControl {
        objectName: "surfaceDepthControl"
        storm: true
        visible: section.available
        model: [
            { label: qsTr("Flat"), value: 0 },
            { label: qsTr("Depth"), value: 1 },
        ]
        current: section.available ? app.backdrops.surfaceDepth : 0
        onActivated: (value) => app.backdrops.surfaceDepth = value
    }
    Hint {
        text: qsTr("Depth gives the conversation, the room list and the spaces "
                   + "rail a soft light-from-above shading. Text never gets "
                   + "harder to read: the shading always moves away from it.")
    }
}
