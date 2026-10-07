import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings -> Appearance: chat backgrounds and surface depth.
//
//   * Your own background for every room (ChatBackgroundEditor, "default").
//   * Where the other kinds live: one room's own picture, and a picture shared
//     with everyone in a room or Space, are set from Room information.
//   * "Show backgrounds set by others" (app.backdrops.showShared, default on).
//     Off hides every room's and Space's shared picture; your own still show.
//   * Depth: flat surfaces, or a gentle light-from-above gradient on the
//     window's large grounds that only ever raises text contrast; and the way
//     to a gradient of your own, which is a custom theme's (Fill in the theme
//     editor), so the two kinds of gradient point at each other.
ColumnLayout {
    id: section
    objectName: "chatBackgroundSettings"
    spacing: AppTheme.spacing8

    // "Make your own gradient": the host opens the theme editor on a surface.
    signal makeGradientRequested()

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

    GroupLabel {
        objectName: "chatBackgroundHeading"
        text: qsTr("Chat background")
    }
    Hint {
        text: qsTr("A picture behind every conversation. Only you see it.")
    }
    ChatBackgroundEditor {
        objectName: "chatBackgroundDefaultEditor"
        Layout.fillWidth: true
        Layout.maximumWidth: 520
        scopeKind: "default"
        showTitle: false
        storm: true
        visible: section.available
    }
    Hint {
        objectName: "chatBackgroundPerRoomHint"
        text: qsTr("For one room only, or a picture everyone in a room or space "
                   + "sees: choose Chat background… from the room's ⋮ menu or "
                   + "right-click the room in the list.")
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: AppTheme.spacing4
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

    GroupLabel {
        objectName: "surfaceDepthHeading"
        text: qsTr("Depth")
    }
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
    // Depth is the automatic gradient; this is the way to one you choose.
    ColumnLayout {
        Layout.fillWidth: true
        spacing: AppTheme.spacing6
        Hint {
            objectName: "surfaceDepthGradientHint"
            Layout.fillWidth: true
            text: qsTr("Want your own colours? A custom theme can give each of "
                       + "these areas a gradient.")
        }
        AppButton {
            objectName: "surfaceDepthMakeGradientButton"
            Layout.leftMargin: AppTheme.spacing4
            storm: true
            size: "sm"
            text: qsTr("Make a gradient…")
            onClicked: section.makeGradientRequested()
        }
    }
}
