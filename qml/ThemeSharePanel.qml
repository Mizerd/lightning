import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Share and Import in one popover under the editor's top bar.
//
// Share: what the theme contains and one button that copies its code.
// Import: a field for a pasted code (a dropped file lands in it too), and, as
// soon as the text is a theme, a PREVIEW of what it would do before anything is
// applied: a small rendered swatch, its name, its base, how much it sets and
// how readable it is; then "Import as new theme" or "Replace current".
//
// It holds no theme state and validates nothing: the editor passes `preview`
// (built from CustomThemeStore's own validation) and answers the signals.
//
// Painted in the invariant editor tokens (see `editorCanvas` in AppTheme.qml).
ColumnLayout {
    id: root

    // "share" or "import".
    property string mode: "share"

    // Share.
    property string themeName: ""
    property int colourCount: 0
    property int gradientCount: 0
    property int problems: 0
    // "Copied." after the code was copied; empty before.
    property string copyNote: ""

    // Import: the text in the field, and what it would do:
    // { ok, error, name, baseName, colourCount, gradientCount, problems,
    //   atLimit, swatch: { rail, sidebar, background, surface, accent,
    //   textPrimary, ownBubble, otherBubble } }, or null for an empty field.
    property string importText: ""
    property var preview: null
    // The current theme's name, for the Replace button.
    property string currentName: ""
    // The field focus lands on when the import tab opens.
    readonly property alias importField: importField

    signal modeRequested(string mode)
    signal copyRequested()
    signal importTextEdited(string text)
    signal importAsNewRequested()
    signal replaceRequested()
    signal clearRequested()

    objectName: "themeSharePanel"
    spacing: AppTheme.spacing8

    // Self-contained controls, so this file depends on no other file's parts.
    component PanelButton: Rectangle {
        id: btn
        property string text: ""
        property bool primary: false
        property bool danger: false
        signal clicked()
        implicitWidth: btnLabel.implicitWidth + AppTheme.spacing16 * 2
        implicitHeight: 32
        radius: AppTheme.radiusMd
        color: !btn.enabled ? "transparent"
             : btn.primary
               ? (btnHover.containsMouse ? Qt.lighter(AppTheme.editorAccent, 1.08)
                                         : AppTheme.editorAccent)
               : btnHover.containsMouse ? AppTheme.editorInset : "transparent"
        border.width: btn.primary ? 0 : 1
        border.color: btn.danger ? AppTheme.editorDanger
                                 : AppTheme.editorBorderStrong
        opacity: btn.enabled ? 1.0 : 0.45
        activeFocusOnTab: btn.enabled
        Accessible.role: Accessible.Button
        Accessible.name: btn.text
        Keys.onPressed: (e) => {
            if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter
                    || e.key === Qt.Key_Space) {
                btn.clicked()
                e.accepted = true
            }
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: 2
            visible: btn.activeFocus
            radius: AppTheme.radiusSm
            color: "transparent"
            border.width: 2
            border.color: btn.primary ? AppTheme.editorAccentInk
                                      : AppTheme.editorAccent
        }
        Label {
            id: btnLabel
            anchors.centerIn: parent
            text: btn.text
            color: btn.primary ? AppTheme.editorAccentInk
                 : btn.danger ? AppTheme.editorDanger : AppTheme.editorText
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
            font.weight: AppTheme.weightStrong
        }
        MouseArea {
            id: btnHover
            anchors.fill: parent
            hoverEnabled: true
            enabled: btn.enabled
            cursorShape: Qt.PointingHandCursor
            onClicked: btn.clicked()
        }
    }

    // Share | Import.
    Rectangle {
        Layout.fillWidth: true
        implicitHeight: 32
        radius: AppTheme.radiusMd
        color: AppTheme.editorInset
        border.width: 1
        border.color: AppTheme.editorBorderStrong
        Accessible.role: Accessible.Grouping
        Accessible.name: qsTr("Share or import")

        Row {
            anchors.fill: parent
            anchors.margins: 2
            spacing: 2
            Repeater {
                model: [ { key: "share", text: qsTr("Share") },
                         { key: "import", text: qsTr("Import") } ]
                delegate: Rectangle {
                    id: seg
                    required property var modelData
                    readonly property bool current: root.mode === modelData.key
                    objectName: "themeShareMode_" + modelData.key
                    width: (parent.width - 2) / 2
                    height: parent.height
                    radius: AppTheme.radiusSm
                    color: seg.current ? AppTheme.editorAccent
                         : segHover.containsMouse ? AppTheme.editorSelection
                                                  : "transparent"
                    activeFocusOnTab: true
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: seg.modelData.text
                    Accessible.checked: seg.current
                    Keys.onPressed: (e) => {
                        if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter
                                || e.key === Qt.Key_Space) {
                            root.modeRequested(seg.modelData.key)
                            e.accepted = true
                        }
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 2
                        visible: seg.activeFocus
                        radius: AppTheme.radiusSm
                        color: "transparent"
                        border.width: 2
                        border.color: seg.current ? AppTheme.editorAccentInk
                                                  : AppTheme.editorAccent
                    }
                    Label {
                        anchors.centerIn: parent
                        text: seg.modelData.text
                        color: seg.current ? AppTheme.editorAccentInk
                                           : AppTheme.editorText
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                        font.weight: AppTheme.weightStrong
                    }
                    MouseArea {
                        id: segHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.modeRequested(seg.modelData.key)
                    }
                }
            }
        }
    }

    // ---- Share ----
    ColumnLayout {
        objectName: "themeShareSection"
        Layout.fillWidth: true
        visible: root.mode === "share"
        spacing: AppTheme.spacing8

        Label {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            text: qsTr("“%1” as a code you can paste to anyone. It holds colours and gradients only, nothing about you or your rooms.")
                  .arg(root.themeName.length > 0 ? root.themeName
                                                 : qsTr("Untitled"))
            color: AppTheme.editorTextSecondary
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }
        Label {
            objectName: "themeShareSummary"
            Layout.fillWidth: true
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            text: root.problems > 0
                  ? qsTr("%1 · %2 · %n thing(s) in it are hard to read",
                         "custom theme share summary", root.problems)
                    .arg(qsTr("%n colour(s)", "custom theme, edited colours",
                              root.colourCount))
                    .arg(qsTr("%n gradient(s)", "custom theme, gradients",
                              root.gradientCount))
                  : qsTr("%1 · %2 · readable")
                    .arg(qsTr("%n colour(s)", "custom theme, edited colours",
                              root.colourCount))
                    .arg(qsTr("%n gradient(s)", "custom theme, gradients",
                              root.gradientCount))
            color: root.problems > 0 ? AppTheme.editorDanger
                                     : AppTheme.editorTextMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            PanelButton {
                objectName: "themeShareCopyButton"
                primary: true
                text: qsTr("Copy code")
                onClicked: root.copyRequested()
            }
            Label {
                objectName: "themeShareCopyNote"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                visible: root.copyNote.length > 0
                text: root.copyNote
                color: AppTheme.editorTextSecondary
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textMeta
                elide: Label.ElideRight
            }
        }
    }

    // ---- Import ----
    ColumnLayout {
        objectName: "themeImportSection"
        Layout.fillWidth: true
        visible: root.mode === "import"
        spacing: AppTheme.spacing8

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 32
            radius: AppTheme.radiusMd
            color: AppTheme.editorInset
            border.width: importField.activeFocus ? 2 : 1
            border.color: importField.activeFocus ? AppTheme.editorAccent
                                                  : AppTheme.editorBorderStrong

            TextInput {
                id: importField
                objectName: "customThemeImportField"
                anchors.fill: parent
                anchors.leftMargin: AppTheme.spacing8
                anchors.rightMargin: AppTheme.spacing8
                verticalAlignment: TextInput.AlignVCenter
                clip: true
                color: AppTheme.editorText
                selectionColor: AppTheme.editorAccent
                selectedTextColor: AppTheme.editorAccentInk
                font.family: AppTheme.monoFont
                font.pixelSize: AppTheme.textMeta
                // Far above any real shared theme; stops holding an unrelated
                // paste.
                maximumLength: 24576
                activeFocusOnTab: true
                Accessible.role: Accessible.EditableText
                Accessible.name: qsTr("Paste a shared theme")
                text: root.importText
                onTextEdited: root.importTextEdited(text)
                Label {
                    anchors.fill: parent
                    verticalAlignment: Text.AlignVCenter
                    visible: importField.text.length === 0
                    text: qsTr("Paste a shared theme, or drop its file on the editor")
                    color: AppTheme.editorTextMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                    elide: Text.ElideRight
                }
            }
        }

        // Why it is not a theme, in the store's own words.
        Label {
            objectName: "themeImportError"
            Layout.fillWidth: true
            visible: root.preview !== null && root.preview.ok === false
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: root.preview && root.preview.error ? root.preview.error : ""
            color: AppTheme.editorDanger
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }

        // What it would do, before it does anything.
        Rectangle {
            id: card
            objectName: "themeImportPreview"
            Layout.fillWidth: true
            visible: root.preview !== null && root.preview.ok === true
            implicitHeight: cardRow.implicitHeight + AppTheme.spacing12 * 2
            radius: AppTheme.radiusMd
            color: AppTheme.editorInset
            border.width: 1
            border.color: AppTheme.editorBorder
            readonly property var sw: root.preview && root.preview.swatch
                                      ? root.preview.swatch : ({})

            RowLayout {
                id: cardRow
                anchors.fill: parent
                anchors.margins: AppTheme.spacing12
                spacing: AppTheme.spacing12

                // A small rendered window in the incoming colours: rail,
                // room list, conversation with two bubbles, and the composer.
                Rectangle {
                    objectName: "themeImportSwatch"
                    Layout.alignment: Qt.AlignTop
                    implicitWidth: 132
                    implicitHeight: 88
                    radius: 4
                    clip: true
                    color: card.sw.background !== undefined ? card.sw.background
                                                            : "transparent"
                    border.width: 1
                    border.color: AppTheme.editorBorderStrong
                    Accessible.role: Accessible.Graphic
                    Accessible.name: qsTr("A small preview of the theme")

                    Rectangle {
                        x: 0; y: 0; width: 14; height: parent.height
                        color: card.sw.rail !== undefined ? card.sw.rail : "transparent"
                        Rectangle {
                            x: 3; y: 6; width: 8; height: 8; radius: 4
                            color: card.sw.accent !== undefined ? card.sw.accent
                                                                : "transparent"
                        }
                    }
                    Rectangle {
                        x: 14; y: 0; width: 38; height: parent.height
                        color: card.sw.sidebar !== undefined ? card.sw.sidebar
                                                             : "transparent"
                        Repeater {
                            model: 4
                            delegate: Rectangle {
                                required property int index
                                x: 5; y: 8 + index * 12
                                width: index === 1 ? 26 : 20
                                height: 4; radius: 2
                                color: card.sw.textPrimary !== undefined
                                       ? card.sw.textPrimary : "transparent"
                                opacity: index === 1 ? 0.9 : 0.5
                            }
                        }
                    }
                    Rectangle {
                        x: 58; y: 8; width: 44; height: 14; radius: 5
                        color: card.sw.otherBubble !== undefined ? card.sw.otherBubble
                                                                 : "transparent"
                        Rectangle {
                            x: 5; y: 5; width: 28; height: 4; radius: 2
                            color: card.sw.textPrimary !== undefined
                                   ? card.sw.textPrimary : "transparent"
                        }
                    }
                    Rectangle {
                        x: 80; y: 28; width: 44; height: 14; radius: 5
                        color: card.sw.ownBubble !== undefined ? card.sw.ownBubble
                                                               : "transparent"
                        Rectangle {
                            x: 5; y: 5; width: 24; height: 4; radius: 2
                            color: card.sw.textPrimary !== undefined
                                   ? card.sw.textPrimary : "transparent"
                            opacity: 0.8
                        }
                    }
                    Rectangle {
                        x: 58; y: parent.height - 22; width: 66; height: 14
                        radius: 5
                        color: card.sw.surface !== undefined ? card.sw.surface
                                                             : "transparent"
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.alignment: Qt.AlignTop
                    spacing: 2
                    Label {
                        objectName: "themeImportName"
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                        text: root.preview && root.preview.name
                              ? root.preview.name : ""
                        color: AppTheme.editorText
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textBody
                        font.weight: AppTheme.weightStrong
                        elide: Label.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                        text: qsTr("Based on %1").arg(
                                  root.preview && root.preview.baseName
                                  ? root.preview.baseName : "")
                        color: AppTheme.editorTextSecondary
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                        elide: Label.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        text: root.preview && root.preview.ok
                              ? qsTr("%1 · %2")
                                .arg(qsTr("%n colour(s)", "custom theme, edited colours",
                                          root.preview.colourCount))
                                .arg(qsTr("%n gradient(s)", "custom theme, gradients",
                                          root.preview.gradientCount))
                              : ""
                        color: AppTheme.editorTextSecondary
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                    }
                    Label {
                        objectName: "themeImportReadability"
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        readonly property int n: root.preview && root.preview.ok
                                                 ? root.preview.problems : 0
                        text: n > 0
                              ? qsTr("%n thing(s) in it would be hard to read (estimated)",
                                     "custom theme readability", n)
                              : qsTr("Readable (estimated)")
                        color: n > 0 ? AppTheme.editorDanger
                                     : AppTheme.editorTextMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: card.visible && root.preview.atLimit === true
            wrapMode: Text.WordWrap
            text: qsTr("You already have the most themes there is room for. Replace the current one, or delete another first.")
            color: AppTheme.editorTextMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }

        Flow {
            Layout.fillWidth: true
            visible: card.visible
            spacing: AppTheme.spacing8
            PanelButton {
                objectName: "themeImportAsNewButton"
                primary: true
                text: qsTr("Import as new theme")
                enabled: root.preview !== null && root.preview.ok === true
                         && root.preview.atLimit !== true
                onClicked: root.importAsNewRequested()
            }
            PanelButton {
                objectName: "themeImportReplaceButton"
                danger: true
                text: qsTr("Replace current")
                enabled: root.preview !== null && root.preview.ok === true
                Accessible.description:
                    qsTr("Replace the colours of “%1” with these. Its name stays, and Undo brings the old colours back.")
                        .arg(root.currentName)
                onClicked: root.replaceRequested()
            }
            PanelButton {
                objectName: "themeImportClearButton"
                text: qsTr("Clear")
                onClicked: root.clearRequested()
            }
        }
        Label {
            Layout.fillWidth: true
            visible: card.visible
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: qsTr("Replace keeps the name “%1” and changes only its colours and gradients.")
                  .arg(root.currentName.length > 0 ? root.currentName
                                                   : qsTr("Untitled"))
            color: AppTheme.editorTextMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }
    }
}
