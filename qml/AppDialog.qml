import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import MatrixClient

// The Lightning dialog shell. Every modal in the app should be this rather
// than a bare `Dialog`.
//
// Why it exists. main.cpp sets QQuickStyle "Basic", whose Dialog background
// is `Rectangle { color: palette.window; border.color: palette.dark }` — no
// radius — and whose footer is a DialogButtonBox of plain 100x40 square
// Buttons. Six dialogs shipped that way: square corners against an app whose
// smallest rounded surface is 4px, a panel painted in the CANVAS token so
// under Storm the dialog body was the same colour as the screen behind it,
// separated only by a 1px hairline drawn in a BODY-TEXT ink.
//
// Worse, Basic's Button draws keyboard focus as
// `border.color: visualFocus ? palette.highlight : palette.windowText`, and
// Main.qml maps both `button` -> cardElevated and `highlight` -> selected,
// which under Storm are the SAME colour (#3D4190 on #3D4190). Tabbing
// through a stock dialog under the app's own brand theme produced no visible
// focus indicator at all. AppButton's inset ring fixes that here by
// construction, on all eleven themes.
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

    // Marks the accept/yes button as destructive: it renders as a solid
    // danger button instead of the accent primary. Say it once here rather
    // than restyling a footer button per dialog.
    property bool destructive: false
    // Storm surfaces are the default for dialogs (every popover in the app
    // is), but a dialog hosted inside a themed pane can opt out.
    property bool storm: true
    // Some dialogs are their own header (a hero card, an avatar row); those
    // set `title` empty and get no header strip.
    readonly property bool _hasHeader: title.length > 0

    modal: true
    anchors.centerIn: parent
    padding: AppTheme.spacing20
    topPadding: _hasHeader ? AppTheme.spacing4 : AppTheme.spacing20
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    // The dim behind a modal. Basic paints `#80000000`; the token is theme
    // aware and matches every other modal surface in the app.
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

        // Every footer button is an AppButton, so the geometry, the corner,
        // the hover/press ladder and above all the focus ring are the app's
        // and not the style's.
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

        // Sibling shadow, sourced from the panel and sized to it, so the
        // dialog's measured geometry is untouched — the same constraint
        // AppMenu documents.
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
