import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import MatrixClient

Dialog {
    id: root

    property bool destructive: false
    property bool storm: true
    readonly property bool _hasHeader: title.length > 0

    modal: true
    anchors.centerIn: parent
    padding: AppTheme.dialogInset
    topPadding: _hasHeader ? AppTheme.spacing4 : AppTheme.dialogInset
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    Overlay.modal: Rectangle {
        color: AppTheme.modalScrim
    }

    header: Item {
        visible: root._hasHeader
        implicitHeight: root._hasHeader ? headerLabel.implicitHeight
                                          + AppTheme.dialogInset
                                          + AppTheme.spacing12
                                        : 0
        Label {
            // Remote or externally chosen text: never markup.
            textFormat: Text.PlainText
            id: headerLabel
            objectName: "dialogTitle"
            text: root.title
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: AppTheme.dialogInset
            anchors.rightMargin: AppTheme.dialogInset
            anchors.bottomMargin: AppTheme.spacing12
            elide: Label.ElideRight
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightBold
            color: root.storm ? AppTheme.stormText : AppTheme.textPrimary
        }
    }

    footer: DialogButtonBox {
        visible: root.standardButtons !== 0
        alignment: Qt.AlignRight
        spacing: AppTheme.spacing8
        padding: AppTheme.dialogInset
        topPadding: AppTheme.dialogFooterGap
        background: Item {}

        delegate: AppButton {
            storm: root.storm
            readonly property int _role: DialogButtonBox.buttonRole
            kind: {
                if (_role === DialogButtonBox.DestructiveRole)
                    return "dangerPrimary"
                if (_role === DialogButtonBox.AcceptRole
                        || _role === DialogButtonBox.YesRole
                        || _role === DialogButtonBox.ApplyRole)
                    return root.destructive ? "dangerPrimary" : "primary"
                return "secondary"
            }
        }
    }

    background: Item {
        implicitWidth: 320

        MultiEffect {
            source: dialogPanel
            anchors.fill: dialogPanel
            z: -1
            shadowEnabled: !AppTheme.reducedMotion
            shadowColor: AppTheme.shadowStrong
            shadowBlur: 1.0
            shadowVerticalOffset: AppTheme.elevationModalY
            shadowHorizontalOffset: 0
        }

        Rectangle {
            id: dialogPanel
            objectName: "dialogPanel"
            anchors.fill: parent
            color: AppTheme.popoverSurface
            border.color: root.storm ? AppTheme.stormBorderStrong
                                     : AppTheme.borderStrong
            border.width: 1
            radius: AppTheme.radiusCard
        }
    }
}
