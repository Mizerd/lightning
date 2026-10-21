import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The theme editor at its Simple level: start from a preset, change four base
// colours, and see one readability verdict. Everything else in the theme is
// worked out from those four through the groups the other levels show, so a
// newcomer can make a theme without meeting the full list of colours.
//
// It holds no theme state. The editor passes what to show and answers the
// signals; a base colour opens in the same picker as any other colour.
//
// Painted in the invariant editor tokens (see `editorCanvas` in AppTheme.qml).
Flickable {
    id: root

    // [{ key, label, color }]: the base colours, in the order shown.
    property var swatches: []
    // [{ key, label, color }]: the surfaces that can be a gradient.
    property var gradientRoles: []
    // The base preset in use, and the choice of presets: [{ id, name }].
    property int baseTheme: 0
    property var bases: []
    // The audit's verdict, in the editor's own words.
    property int problems: 0
    property int unchecked: 0
    // True when the conversation background is dark, for the one-line mood.
    property bool dark: true
    // The role currently open in the picker, marked on its swatch.
    property string openRole: ""

    signal roleRequested(string key)
    signal baseRequested(int id)
    signal reportRequested()

    objectName: "themeSimplePanel"
    clip: true
    contentWidth: width
    contentHeight: column.implicitHeight
    boundsBehavior: Flickable.StopAtBounds

    // Keyboard focus below the fold used to be invisible: Tab moved on to a
    // control the panel had not scrolled to. Follow the focused control.
    function ensureVisible(item) {
        if (!item || item === root)
            return
        let inside = false
        for (let i = item; i && !inside; i = i.parent)
            inside = (i === root.contentItem)
        if (!inside)
            return
        const top = item.mapToItem(root.contentItem, 0, 0).y - AppTheme.spacing6
        const bottom = top + item.height + AppTheme.spacing6 * 2
        const maxY = Math.max(0, root.contentHeight - root.height)
        if (top < root.contentY)
            root.contentY = Math.max(0, top)
        else if (bottom > root.contentY + root.height)
            root.contentY = Math.min(maxY, bottom - root.height)
    }
    Connections {
        target: root.Window.window
        function onActiveFocusItemChanged() {
            root.ensureVisible(root.Window.window.activeFocusItem)
        }
    }
    // A click on the panel's own text gives it the keys below.
    TapHandler { onTapped: root.forceActiveFocus(Qt.MouseFocusReason) }
    // Keys no focused control handles scroll the panel.
    Keys.onPressed: (e) => {
        const maxY = Math.max(0, root.contentHeight - root.height)
        const page = Math.max(40, root.height - 40)
        let y = root.contentY
        if (e.key === Qt.Key_PageDown) y += page
        else if (e.key === Qt.Key_PageUp) y -= page
        else if (e.key === Qt.Key_Down) y += 40
        else if (e.key === Qt.Key_Up) y -= 40
        else if (e.key === Qt.Key_Home) y = 0
        else if (e.key === Qt.Key_End) y = maxY
        else return
        root.contentY = Math.max(0, Math.min(maxY, y))
        e.accepted = true
    }

    ScrollBar.vertical: ScrollBar {
        policy: root.contentHeight > root.height + 1 ? ScrollBar.AlwaysOn
                                                     : ScrollBar.AlwaysOff
        contentItem: Rectangle {
            implicitWidth: 5
            radius: 2.5
            color: AppTheme.editorBorderStrong
        }
    }

    component Heading: Label {
        Layout.fillWidth: true
        Layout.topMargin: AppTheme.spacing6
        textFormat: Text.PlainText
        color: AppTheme.editorTextSecondary
        font.family: AppTheme.menuSectionFont
        font.pixelSize: AppTheme.menuSectionSize
        font.weight: AppTheme.menuSectionWeight
    }

    ColumnLayout {
        id: column
        width: root.width - (root.contentHeight > root.height + 1
                             ? AppTheme.spacing12 : 0)
        spacing: AppTheme.spacing8

        Heading { text: qsTr("Start from") }

        Flow {
            id: baseFlow
            objectName: "themeSimpleBases"
            Layout.fillWidth: true
            spacing: AppTheme.spacing6
            readonly property real cardWidth:
                Math.floor((width - spacing) / 2)

            Repeater {
                model: root.bases
                delegate: Rectangle {
                    id: card
                    required property var modelData
                    readonly property bool current: root.baseTheme === modelData.id
                    readonly property var cardPal:
                        AppTheme.paletteForTheme(modelData.id)
                    objectName: "themeSimpleBase_" + modelData.id
                    width: baseFlow.cardWidth
                    height: 34
                    radius: AppTheme.radiusMd
                    color: current ? AppTheme.editorSelection
                         : cardHover.containsMouse ? AppTheme.editorInset
                                                   : "transparent"
                    border.width: current ? 2 : 1
                    border.color: current ? AppTheme.editorAccent
                                          : AppTheme.editorBorder
                    activeFocusOnTab: true
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: modelData.name
                    Accessible.checked: current
                    Keys.onPressed: (e) => {
                        if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter
                                || e.key === Qt.Key_Space) {
                            root.baseRequested(card.modelData.id)
                            e.accepted = true
                        }
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 2
                        visible: card.activeFocus
                        radius: AppTheme.radiusSm
                        color: "transparent"
                        border.width: 2
                        border.color: AppTheme.editorAccent
                    }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: AppTheme.spacing6
                        anchors.rightMargin: AppTheme.spacing6
                        spacing: AppTheme.spacing6
                        Row {
                            spacing: 1
                            Repeater {
                                model: ["sidebar", "background", "accent"]
                                delegate: Rectangle {
                                    required property string modelData
                                    width: 8
                                    height: 20
                                    radius: 2
                                    color: card.cardPal[modelData]
                                }
                            }
                        }
                        Label {
                            id: cardLabel
                            Layout.fillWidth: true
                            text: card.modelData.name
                            textFormat: Text.PlainText
                            color: AppTheme.editorText
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            elide: Label.ElideRight
                        }
                    }
                    MouseArea {
                        id: cardHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.baseRequested(card.modelData.id)
                        // A narrow panel elides names ("Lightning ..." twice).
                        ToolTip.visible: containsMouse && cardLabel.truncated
                        ToolTip.delay: 400
                        ToolTip.text: card.modelData.name
                    }
                }
            }
        }

        Heading { text: qsTr("Base colours") }

        Repeater {
            model: root.swatches
            delegate: Rectangle {
                id: swatchRow
                required property var modelData
                readonly property bool open: root.openRole === modelData.key
                objectName: "themeSimpleSwatch_" + modelData.key
                Layout.fillWidth: true
                implicitHeight: 40
                radius: AppTheme.radiusSm
                color: open ? AppTheme.editorSelection
                     : swatchHover.containsMouse ? AppTheme.editorInset
                                                 : "transparent"
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: modelData.label
                Keys.onPressed: (e) => {
                    if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter
                            || e.key === Qt.Key_Space) {
                        root.roleRequested(swatchRow.modelData.key)
                        e.accepted = true
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    visible: swatchRow.activeFocus
                    radius: AppTheme.radiusSm
                    color: "transparent"
                    border.width: 2
                    border.color: AppTheme.editorAccent
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: AppTheme.spacing8
                    anchors.rightMargin: AppTheme.spacing8
                    spacing: AppTheme.spacing8
                    Rectangle {
                        implicitWidth: 28
                        implicitHeight: 28
                        radius: AppTheme.radiusSm
                        color: swatchRow.modelData.color
                        border.width: 1
                        border.color: AppTheme.editorBorderStrong
                    }
                    Label {
                        Layout.fillWidth: true
                        text: swatchRow.modelData.label
                        textFormat: Text.PlainText
                        color: AppTheme.editorText
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                        elide: Label.ElideRight
                    }
                }
                MouseArea {
                    id: swatchHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.roleRequested(swatchRow.modelData.key)
                }
            }
        }

        Label {
            objectName: "themeSimpleMood"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: root.dark
                  ? qsTr("A dark theme. Everything else is worked out from these colours; switch to Groups to refine it.")
                  : qsTr("A light theme. Everything else is worked out from these colours; switch to Groups to refine it.")
            color: AppTheme.editorTextMuted
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }

        // The one verdict. The pairs themselves are in the report it opens.
        Rectangle {
            id: pill
            objectName: "themeSimpleReadability"
            Layout.fillWidth: true
            implicitHeight: 32
            radius: AppTheme.radiusPill
            color: pillHover.containsMouse ? AppTheme.editorSelection
                                           : AppTheme.editorInset
            border.width: 1
            border.color: root.problems > 0 ? AppTheme.editorDanger
                                            : AppTheme.editorBorderStrong
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: pillLabel.text
            Keys.onPressed: (e) => {
                if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter
                        || e.key === Qt.Key_Space) {
                    root.reportRequested()
                    e.accepted = true
                }
            }
            Rectangle {
                anchors.fill: parent
                anchors.margins: 2
                visible: pill.activeFocus
                radius: AppTheme.radiusPill
                color: "transparent"
                border.width: 2
                border.color: AppTheme.editorAccent
            }
            Label {
                id: pillLabel
                anchors.fill: parent
                anchors.leftMargin: AppTheme.spacing12
                anchors.rightMargin: AppTheme.spacing12
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                elide: Label.ElideRight
                textFormat: Text.PlainText
                text: root.problems > 0
                      ? qsTr("%n spot(s) hard to read. Review",
                             "custom theme readability", root.problems)
                      : root.unchecked > 0
                        ? qsTr("Readable, %n not checked",
                               "custom theme readability", root.unchecked)
                        : qsTr("Readable everywhere")
                color: root.problems > 0 ? AppTheme.editorDanger
                                         : AppTheme.editorTextSecondary
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textMeta
                font.weight: AppTheme.weightStrong
            }
            MouseArea {
                id: pillHover
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.reportRequested()
            }
        }

        // Gradients are found here, and again as the Fill choice on every
        // surface that can take one.
        Heading {
            visible: root.gradientRoles.length > 0
            text: qsTr("Gradients")
        }
        Label {
            visible: root.gradientRoles.length > 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: qsTr("The large areas can be a gradient instead of one colour. Open one, then choose Linear or Radial under Fill.")
            color: AppTheme.editorTextSecondary
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }
        Flow {
            objectName: "themeSimpleGradients"
            Layout.fillWidth: true
            spacing: AppTheme.spacing4
            visible: root.gradientRoles.length > 0
            Repeater {
                model: root.gradientRoles
                delegate: Rectangle {
                    id: gchip
                    required property var modelData
                    objectName: "themeSimpleGradient_" + modelData.key
                    implicitWidth: gchipRow.implicitWidth + AppTheme.spacing8 * 2
                    implicitHeight: 24
                    radius: AppTheme.radiusPill
                    color: gchipHover.containsMouse ? AppTheme.editorInset
                                                    : "transparent"
                    border.width: 1
                    border.color: AppTheme.editorBorderStrong
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.label
                    Keys.onPressed: (e) => {
                        if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter
                                || e.key === Qt.Key_Space) {
                            root.roleRequested(gchip.modelData.key)
                            e.accepted = true
                        }
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 2
                        visible: gchip.activeFocus
                        radius: AppTheme.radiusPill
                        color: "transparent"
                        border.width: 2
                        border.color: AppTheme.editorAccent
                    }
                    Row {
                        id: gchipRow
                        anchors.centerIn: parent
                        spacing: AppTheme.spacing6
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 10
                            height: 10
                            radius: 5
                            color: gchip.modelData.color
                            border.width: 1
                            border.color: AppTheme.editorBorderStrong
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            textFormat: Text.PlainText
                            text: gchip.modelData.label
                            color: AppTheme.editorText
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                        }
                    }
                    MouseArea {
                        id: gchipHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.roleRequested(gchip.modelData.key)
                    }
                }
            }
        }
    }
}
