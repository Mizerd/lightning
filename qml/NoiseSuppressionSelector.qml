import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings → Labs: which ONE noise suppressor runs on the microphone in calls
// (GitHub #20). A radio list, not toggles: the modes are mutually exclusive by
// construction. A mode this build or system cannot run is shown disabled with
// the reason, never hidden, so its absence explains itself.
ColumnLayout {
    id: root
    objectName: "noiseSuppressionSelector"
    spacing: AppTheme.spacing8

    /// [{key, available, reason}] in display order; from
    /// SfuCallController::noiseSuppressionChoices(). Injectable for tests.
    property var choices: []
    /// The selected mode key.
    property string selected: "webrtc"
    /// The mode key that failed to start in the running call, or "".
    property string failedMode: ""
    /// With failedMode: WebRTC suppression runs in its place.
    property bool fellBackToWebrtc: false

    signal chosen(string key)

    function titleFor(key) {
        if (key === "off") return qsTr("Off")
        if (key === "webrtc") return qsTr("WebRTC")
        if (key === "rnnoise") return qsTr("RNNoise")
        if (key === "deepfilternet") return qsTr("DeepFilterNet")
        return key
    }
    function descriptionFor(key) {
        if (key === "off")
            return qsTr("No noise suppression. Your microphone is sent as "
                        + "captured; volume levelling still applies.")
        if (key === "webrtc")
            return qsTr("Standard WebRTC suppression, as browsers use. The "
                        + "lightest on CPU, and the recommended default.")
        if (key === "rnnoise")
            return qsTr("Neural suppression. Removes more noise, such as fans "
                        + "and keyboards, at moderate CPU use.")
        if (key === "deepfilternet")
            return qsTr("Advanced neural suppression. Potentially the "
                        + "strongest, and the most demanding on CPU.")
        return ""
    }
    function reasonText(reason) {
        if (reason === "no-call-engine")
            return qsTr("Calls are not available in this build.")
        if (reason === "no-webrtcdsp")
            return qsTr("Needs GStreamer's webrtcdsp element "
                        + "(gst-plugins-bad), which is not installed.")
        if (reason === "not-in-build")
            return qsTr("Not included in this build.")
        return ""
    }

    Repeater {
        model: root.choices
        delegate: AbstractButton {
            id: option
            required property var modelData
            readonly property bool current: root.selected === modelData.key
            objectName: "noiseSuppressionOption_" + modelData.key
            Layout.fillWidth: true
            enabled: modelData.available
            hoverEnabled: true
            focusPolicy: Qt.StrongFocus
            padding: AppTheme.spacing12

            Accessible.role: Accessible.RadioButton
            Accessible.name: root.titleFor(modelData.key)
            Accessible.description: modelData.available
                ? root.descriptionFor(modelData.key)
                : root.reasonText(modelData.reason)
            Accessible.checked: current

            onClicked: root.chosen(modelData.key)
            Keys.onReturnPressed: root.chosen(modelData.key)
            Keys.onEnterPressed: root.chosen(modelData.key)

            background: Rectangle {
                radius: AppTheme.radiusMd
                color: option.current ? AppTheme.stormSelection
                       : (option.enabled && (option.hovered || option.activeFocus)
                          ? AppTheme.hover : AppTheme.stormInset)
                border.width: option.current || option.activeFocus ? 2 : 0
                border.color: option.activeFocus ? AppTheme.focusRing
                                                 : AppTheme.bolt
            }

            contentItem: RowLayout {
                id: optionBody
                spacing: AppTheme.spacing12

                // Radio: a ring, filled when chosen.
                Rectangle {
                    Layout.alignment: Qt.AlignTop
                    Layout.topMargin: AppTheme.spacing4
                    implicitWidth: 16
                    implicitHeight: 16
                    radius: 8
                    color: "transparent"
                    border.width: 2
                    // stormTextMuted: the ink a radio ring needs to clear on
                    // every theme (see the theme cards).
                    border.color: option.current ? AppTheme.bolt
                                                 : AppTheme.stormTextMuted
                    opacity: option.enabled ? 1 : 0.55
                    Rectangle {
                        anchors.centerIn: parent
                        width: 8
                        height: 8
                        radius: 4
                        visible: option.current
                        color: AppTheme.bolt
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        Layout.fillWidth: true
                        text: root.titleFor(option.modelData.key)
                        textFormat: Text.PlainText
                        color: option.enabled ? AppTheme.stormText
                                              : AppTheme.stormTextSecondary
                        font.pixelSize: AppTheme.textBody
                        font.weight: AppTheme.weightStrong
                        elide: Text.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.descriptionFor(option.modelData.key)
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        color: AppTheme.stormTextMuted
                        font.pixelSize: AppTheme.textMeta
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                    }
                    Label {
                        objectName: "noiseSuppressionReason_" + option.modelData.key
                        Layout.fillWidth: true
                        visible: !option.modelData.available
                        text: root.reasonText(option.modelData.reason)
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        // Muted, not textDisabled: the reason is the one line
                        // the user needs to read here.
                        color: AppTheme.stormTextMuted
                        font.pixelSize: AppTheme.textMeta
                        font.italic: true
                    }
                }
            }
        }
    }

    Label {
        objectName: "noiseSuppressionFailure"
        Layout.fillWidth: true
        visible: root.failedMode !== ""
        text: (root.fellBackToWebrtc
               ? qsTr("%1 could not start in this call; WebRTC noise "
                      + "suppression is used instead.")
               : qsTr("%1 could not start in this call, so your microphone is "
                      + "being sent without noise suppression."))
              .arg(root.titleFor(root.failedMode))
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        color: AppTheme.stormDanger
        font.pixelSize: AppTheme.textMeta
    }
}
