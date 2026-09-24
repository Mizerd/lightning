import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import MatrixClient

// The Lightning dialog shell; use it instead of a bare `Dialog`.
//
// The Basic style's Dialog has square corners, a canvas-coloured panel
// (indistinguishable from the screen under Storm) and a footer of square
// stock Buttons whose focus border is invisible under Storm. This gives a
// rounded storm panel and AppButton footer buttons with a visible focus ring
// on every theme.
//
// Usage:
//   AppDialog {
//       title: qsTr("Clear saved GIFs?")
//       standardButtons: Dialog.Yes | Dialog.Cancel
//       destructive: true
//       ColumnLayout { ... }
//   }
Dialog {
    id: root

    // Renders the accept/yes button as a solid danger button instead of the
    // accent primary.
    property bool destructive: false
    // Dialogs are storm surfaces by default; one hosted in a themed pane can
    // opt out.
    property bool storm: true
    // Dialogs that are their own header (hero card, avatar row) leave `title`
    // empty and get no header strip.
    readonly property bool _hasHeader: title.length > 0

    modal: true
    anchors.centerIn: parent
    padding: AppTheme.spacing20
    topPadding: _hasHeader ? AppTheme.spacing4 : AppTheme.spacing20
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    // The theme-aware modal dim, matching every other modal.
    Overlay.modal: Rectangle {
        color: AppTheme.modalScrim
    }

    header: Item {
        visible: root._hasHeader
        implicitHeight: root._hasHeader ? headerLabel.implicitHeight
                                          + AppTheme.spacing20
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
            anchors.leftMargin: AppTheme.spacing20
            anchors.rightMargin: AppTheme.spacing20
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
        padding: AppTheme.spacing20
        topPadding: AppTheme.spacing16
        background: Item {}

        // Every footer button is an AppButton, for the app's geometry, states
        // and focus ring.
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

        // Sibling shadow sized to the panel, so the dialog's measured geometry
        // is untouched (as in AppMenu).
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
            color: root.storm ? AppTheme.stormPanel : AppTheme.surface
            border.color: root.storm ? AppTheme.stormBorderStrong
                                     : AppTheme.borderStrong
            border.width: 1
            radius: AppTheme.radiusCard
        }
    }
}
