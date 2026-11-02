import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

Popup {
    id: root
    modal: true
    width: Math.min(440, (parent ? parent.width : 440) - 2 * AppTheme.spacing16)
    padding: 0
    focus: true
    height: Math.min(implicitHeight,
                     (parent ? parent.height : implicitHeight)
                     - 2 * AppTheme.spacing16)
    closePolicy: root.flowIsLive ? Popup.NoAutoClose
                                 : (Popup.CloseOnEscape
                                    | Popup.CloseOnPressOutside)

    // A non-terminal flow. Not `app.verificationActive || …`: the flow id stays
    // set at done/cancelled/failed, which would keep this true forever. The
    // state string is the authority on liveness.
    readonly property bool flowIsLive:
        app.verificationState !== ""
        && app.verificationState !== "done"
        && app.verificationState !== "cancelled"
        && !app.verificationState.startsWith("failed")
    readonly property bool flowPresent:
        app.verificationActive || app.verificationState !== ""

    function _syncOpenState() {
        if (flowPresent) {
            if (!opened)
                open()
        } else if (opened) {
            close()
        }
    }
    onFlowPresentChanged: _syncOpenState()
    onOpened: verificationPanel.focusPrimary()
    Component.onCompleted: _syncOpenState()

    onClosed: {
        if (!flowIsLive && app.verificationState !== "")
            app.cancelVerification()
    }

    Overlay.modal: Rectangle { color: AppTheme.modalScrim }

    background: Item {
        Rectangle {
            id: dialogBackground
            anchors.fill: parent
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            border.width: 1
            radius: AppTheme.radiusLg
        }
        MultiEffect {
            source: dialogBackground
            anchors.fill: dialogBackground
            z: -1
            shadowEnabled: true
            shadowColor: AppTheme.shadow
            shadowBlur: 0.6
            shadowVerticalOffset: 2
            shadowHorizontalOffset: 0
        }
    }

    contentItem: Flickable {
        id: scroller
        objectName: "verificationDialogScroller"
        clip: true
        contentWidth: width
        contentHeight: column.implicitHeight
        implicitHeight: column.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Session verification")

        ColumnLayout {
            id: column
            width: scroller.width
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: AppTheme.spacing16
                Layout.bottomMargin: AppTheme.spacing8
                spacing: AppTheme.spacing8
                Label {
                    Layout.fillWidth: true
                    objectName: "verificationDialogTitle"
                    text: qsTr("Verify this session")
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textTitle
                    font.weight: AppTheme.weightBold
                    elide: Label.ElideRight
                }
                IconButton {
                    objectName: "verificationDialogClose"
                    iconName: "close"
                    storm: true
                    visible: !root.flowIsLive
                    Accessible.name: qsTr("Close")
                    onClicked: root.close()
                }
            }

            Label {
                Layout.fillWidth: true
                Layout.leftMargin: AppTheme.spacing16
                Layout.rightMargin: AppTheme.spacing16
                Layout.bottomMargin: AppTheme.spacing8
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                wrapMode: Text.WordWrap
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textBody
                visible: app.verificationState !== "cancelled"
                         && !app.verificationState.startsWith("failed")
                text: qsTr("Verifying proves this device is really yours, so "
                           + "your other sessions will share encryption keys "
                           + "with it and encrypted history becomes readable "
                           + "here.")
            }

            VerificationPanel {
                id: verificationPanel
                objectName: "verificationPanel"
                Layout.fillWidth: true
                Layout.leftMargin: AppTheme.spacing16
                Layout.rightMargin: AppTheme.spacing16
                Layout.bottomMargin: AppTheme.spacing16
            }
        }
    }
}
