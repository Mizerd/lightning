import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings -> Sound & video: how much picture and processing a call uses.
//
// Camera resolution, screen-share resolution / frame rate / bitrate limit, and
// the two webrtcdsp switches (echo cancellation, automatic gain). Every choice
// has "Auto", which is what Lightning did before these were settings, and Auto
// is the default. A camera or share change applies to the next camera or
// share you start; the voice switches apply to the next call.
ColumnLayout {
    id: root

    spacing: AppTheme.spacing8

    /// One labelled choice. `setting` is the SettingsManager property name,
    /// `changed` the signal that announces it.
    component QualityRow: RowLayout {
        id: row
        property string title: ""
        property string setting: ""
        property var choices: []
        property alias combo: combo

        Layout.fillWidth: true
        spacing: AppTheme.spacing8

        Label {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            text: row.title
            color: AppTheme.stormText
            font.pixelSize: AppTheme.textBody
            font.weight: AppTheme.weightMedium
            elide: Text.ElideRight
        }
        AppComboBox {
            id: combo
            storm: true
            implicitWidth: 150
            textRole: "label"
            valueRole: "value"
            model: row.choices
            Accessible.name: row.title
            // Never bind currentIndex: indexOfValue() is -1 at creation.
            function resync() { syncToValue(app.settings[row.setting]) }
            Component.onCompleted: resync()
            onActivated: {
                app.settings[row.setting] = currentValue
                resync()
            }
            Connections {
                target: app.settings
                function onShareQualityChanged() { combo.resync() }
                function onCameraQualityChanged() { combo.resync() }
            }
        }
    }

    component Hint: Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        lineHeight: AppTheme.lineHeightBody
        lineHeightMode: Text.ProportionalHeight
        color: AppTheme.stormTextMuted
        font.pixelSize: AppTheme.textMeta
    }

    Label {
        text: qsTr("Quality")
        color: AppTheme.stormText
        font.pixelSize: AppTheme.textBody
        font.weight: AppTheme.weightStrong
    }

    QualityRow {
        objectName: "cameraResolutionRow"
        title: qsTr("Camera resolution")
        setting: "cameraResolution"
        choices: [
            { label: qsTr("Auto"), value: 0 },
            { label: qsTr("360p"), value: 360 },
            { label: qsTr("480p"), value: 480 },
            { label: qsTr("720p"), value: 720 },
            { label: qsTr("1080p"), value: 1080 }
        ]
    }
    QualityRow {
        objectName: "shareResolutionRow"
        title: qsTr("Screen share resolution")
        setting: "shareMaxHeight"
        choices: [
            { label: qsTr("Auto"), value: 0 },
            { label: qsTr("720p"), value: 720 },
            { label: qsTr("1080p"), value: 1080 },
            { label: qsTr("1440p"), value: 1440 },
            { label: qsTr("4K"), value: 2160 }
        ]
    }
    QualityRow {
        objectName: "shareFrameRateRow"
        title: qsTr("Screen share frame rate")
        setting: "shareFps"
        choices: [
            { label: qsTr("Auto"), value: 0 },
            { label: qsTr("15 fps"), value: 15 },
            { label: qsTr("30 fps"), value: 30 },
            { label: qsTr("60 fps"), value: 60 }
        ]
    }
    QualityRow {
        objectName: "shareBitrateRow"
        title: qsTr("Screen share bitrate limit")
        setting: "shareBitrateCapKbps"
        choices: [
            { label: qsTr("Auto"), value: 0 },
            { label: qsTr("0.5 Mbps"), value: 500 },
            { label: qsTr("1 Mbps"), value: 1000 },
            { label: qsTr("2 Mbps"), value: 2000 },
            { label: qsTr("3 Mbps"), value: 3000 },
            { label: qsTr("4.5 Mbps"), value: 4500 }
        ]
    }
    Label {
        objectName: "settingsShareQualityWarning"
        Layout.fillWidth: true
        visible: app.settings.shareQualityDemanding
        wrapMode: Text.WordWrap
        color: AppTheme.warning
        font.pixelSize: AppTheme.textMeta
        text: qsTr("This resolution and frame rate ask a lot of your "
                   + "computer and may stutter.")
    }
    Hint {
        text: qsTr("Auto is 720p for the camera and 1080p at 30 fps for a "
                   + "screen share. A bitrate limit only ever lowers the "
                   + "share's quality, which helps on a slow connection. "
                   + "Changes apply to the next camera or share you start.")
    }

    Label {
        Layout.topMargin: AppTheme.spacing8
        text: qsTr("Microphone processing")
        color: AppTheme.stormText
        font.pixelSize: AppTheme.textBody
        font.weight: AppTheme.weightStrong
    }
    SettingsCheckBox {
        objectName: "autoGainControlCheck"
        palette.windowText: AppTheme.stormText
        text: qsTr("Automatic gain control")
        checked: app.settings.autoGainControl
        onToggled: app.settings.autoGainControl = checked
    }
    Hint {
        text: qsTr("Lifts a quiet microphone and evens out loud and soft "
                   + "speech. Turn it off if you set your level by hand.")
    }
    SettingsCheckBox {
        objectName: "echoCancellationCheck"
        palette.windowText: AppTheme.stormText
        text: qsTr("Echo cancellation")
        checked: app.settings.echoCancellation
        onToggled: app.settings.echoCancellation = checked
    }
    Hint {
        text: qsTr("Removes your callers' voices from your microphone when "
                   + "you use speakers. Headphones do not need it. It needs "
                   + "the WebRTC audio component and is new: with several people "
                   + "in a call it may cancel only some of them. Both apply to "
                   + "your next call.")
    }
}
