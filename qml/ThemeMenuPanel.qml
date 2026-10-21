import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The theme menu under the editor's top bar: the list of saved themes (the
// current one marked), its name, and New / Duplicate / Share / Import / Delete.
// It replaces the chips, name field and Manage drawer that used to take the
// editor's left column.
//
// It holds no theme state. The editor passes what to show and answers the
// signals; Delete asks before it acts, in the panel, and the editor owns the
// answer.
//
// Painted in the invariant editor tokens (see `editorCanvas` in AppTheme.qml).
ColumnLayout {
    id: root

    // [{ id, name, baseTheme, overrideCount }], as CustomThemeStore.themes.
    property var themes: []
    property string activeId: ""
    // The current theme's name, edited in place.
    property string themeName: ""
    property bool canDuplicate: true
    // False once the collection is full.
    property bool canCreate: true
    property bool canDelete: true
    property bool confirmingDelete: false
    // The field focus lands on when the menu opens.
    readonly property alias nameField: nameField

    signal themeRequested(string id)
    signal nameEdited(string text)
    signal newRequested()
    signal duplicateRequested()
    signal shareRequested()
    signal importRequested()
    signal deleteRequested()
    signal deleteConfirmed()
    signal deleteCancelled()

    objectName: "themeMenuPanel"
    spacing: AppTheme.spacing8

    // Self-contained button, so this file depends on no other file's parts.
    component MenuButton: Rectangle {
        id: btn
        property string text: ""
        property bool danger: false
        signal clicked()
        implicitWidth: btnLabel.implicitWidth + AppTheme.spacing16 * 2
        implicitHeight: 30
        radius: AppTheme.radiusMd
        color: !btn.enabled ? "transparent"
             : btnHover.containsMouse ? AppTheme.editorInset : "transparent"
        border.width: 1
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
            border.color: AppTheme.editorAccent
        }
        Label {
            id: btnLabel
            anchors.centerIn: parent
            text: btn.text
            color: btn.danger ? AppTheme.editorDanger : AppTheme.editorText
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

    Label {
        Layout.fillWidth: true
        text: qsTr("Your themes")
        color: AppTheme.editorTextSecondary
        font.family: AppTheme.uiFont
        font.pixelSize: AppTheme.textMeta
        font.weight: AppTheme.weightStrong
        elide: Label.ElideRight
    }

    // One row per saved theme; the list scrolls past seven.
    // The current theme's row, scrolled into view: with more than seven
    // themes it was off-screen on open and the check mark could not be seen.
    // Reveal only on open and on an active-id change, plus once more while the
    // list is still being filled; a later themes change (Duplicate, an edit)
    // must not yank a list the user has scrolled elsewhere.
    property bool revealPending: true
    function revealCurrent() {
        if (!root.revealPending)
            return
        let at = -1
        for (let i = 0; i < root.themes.length; ++i) {
            if (root.themes[i].id === root.activeId) { at = i; break }
        }
        const item = at >= 0 ? themeRows.itemAt(at) : null
        // A delegate that was just (re)built sits at y 0 until the layout
        // pass runs; a row that is not the first one cannot really be there,
        // so wait for the layout (listColumn.onHeightChanged) instead.
        if (!item || (at > 0 && item.y <= 0))
            return
        root.revealPending = false
        const maxY = Math.max(0, listScroll.contentHeight - listScroll.height)
        if (item.y < listScroll.contentY)
            listScroll.contentY = Math.min(item.y, maxY)
        else if (item.y + item.height > listScroll.contentY + listScroll.height)
            listScroll.contentY = Math.min(item.y + item.height - listScroll.height,
                                           maxY)
    }
    onActiveIdChanged: { root.revealPending = true; Qt.callLater(root.revealCurrent) }
    onThemesChanged: Qt.callLater(root.revealCurrent)
    onVisibleChanged: if (root.visible) { root.revealPending = true; Qt.callLater(root.revealCurrent) }
    Component.onCompleted: Qt.callLater(root.revealCurrent)

    Item {
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(listColumn.implicitHeight, 7 * 36)
    Flickable {
        id: listScroll
        objectName: "themeMenuList"
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: listColumn.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height + 1

        ScrollBar.vertical: ScrollBar {
            policy: listScroll.contentHeight > listScroll.height + 1
                    ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
            contentItem: Rectangle {
                implicitWidth: 5
                radius: 2.5
                color: AppTheme.editorBorderStrong
            }
        }

        ColumnLayout {
            id: listColumn
            onHeightChanged: Qt.callLater(root.revealCurrent)
            width: listScroll.width
                   - (listScroll.contentHeight > listScroll.height + 1
                      ? AppTheme.spacing12 : 0)
            spacing: 2

            Repeater {
                id: themeRows
                model: root.themes
                delegate: Rectangle {
                    id: row
                    required property var modelData
                    readonly property bool current: root.activeId === modelData.id
                    readonly property var basePal:
                        AppTheme.paletteForTheme(modelData.baseTheme)
                    objectName: "customThemeChip_" + modelData.id
                    Layout.fillWidth: true
                    implicitHeight: 34
                    radius: AppTheme.radiusSm
                    color: row.current ? AppTheme.editorSelection
                         : rowHover.containsMouse ? AppTheme.editorInset
                                                  : "transparent"
                    activeFocusOnTab: true
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: row.modelData.name.length > 0
                                     ? row.modelData.name : qsTr("Untitled")
                    Accessible.checked: row.current
                    Keys.onPressed: (e) => {
                        if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter
                                || e.key === Qt.Key_Space) {
                            root.themeRequested(row.modelData.id)
                            e.accepted = true
                        }
                    }
                    Rectangle {
                        anchors.fill: parent
                        visible: row.activeFocus
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
                        // The preset it sits on, in its own colours.
                        Row {
                            spacing: 1
                            Repeater {
                                model: ["sidebar", "background", "accent"]
                                delegate: Rectangle {
                                    required property string modelData
                                    width: 8
                                    height: 18
                                    radius: 2
                                    color: row.basePal[modelData]
                                }
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            textFormat: Text.PlainText
                            text: row.modelData.name.length > 0
                                  ? row.modelData.name : qsTr("Untitled")
                            color: AppTheme.editorText
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            font.weight: row.current ? AppTheme.weightStrong
                                                     : Font.Normal
                            elide: Label.ElideRight
                        }
                        Label {
                            textFormat: Text.PlainText
                            visible: row.modelData.overrideCount > 0
                            text: qsTr("%n changed", "custom theme, edited colours",
                                       row.modelData.overrideCount)
                            color: AppTheme.editorTextMuted
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.menuSectionSize
                        }
                        Icon {
                            visible: row.current
                            name: "check"
                            size: 16
                            color: AppTheme.editorAccent
                        }
                    }
                    MouseArea {
                        id: rowHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.themeRequested(row.modelData.id)
                    }
                }
            }
        }
    }
    // Middle-click autoscroll, as in the timeline and the room list.
    MiddleClickScroller {
        anchors.fill: parent
        objectName: "themeMenuMiddleClickScroller"
        view: listScroll
    }
    }

    Rectangle {
        Layout.fillWidth: true
        implicitHeight: 1
        color: AppTheme.editorBorder
    }

    // The theme's name, edited in place; a shared theme needs a name.
    Label {
        Layout.fillWidth: true
        text: qsTr("Name")
        color: AppTheme.editorTextSecondary
        font.family: AppTheme.uiFont
        font.pixelSize: AppTheme.textMeta
        font.weight: AppTheme.weightStrong
    }
    Rectangle {
        Layout.fillWidth: true
        implicitHeight: 32
        radius: AppTheme.radiusMd
        color: AppTheme.editorInset
        border.width: nameField.activeFocus ? 2 : 1
        border.color: nameField.activeFocus ? AppTheme.editorAccent
                                            : AppTheme.editorBorderStrong

        TextInput {
            id: nameField
            objectName: "customThemeNameField"
            anchors.fill: parent
            anchors.leftMargin: AppTheme.spacing8
            anchors.rightMargin: AppTheme.spacing8
            verticalAlignment: TextInput.AlignVCenter
            clip: true
            color: AppTheme.editorText
            selectionColor: AppTheme.editorAccent
            selectedTextColor: AppTheme.editorAccentInk
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
            maximumLength: 48
            activeFocusOnTab: true
            Accessible.role: Accessible.EditableText
            Accessible.name: qsTr("Theme name")
            text: root.themeName
            // Tab or the menu opening lands on a name ready to be replaced.
            onActiveFocusChanged: if (nameField.activeFocus) nameField.selectAll()
            // Committed as typed: editingFinished is not reliably reached (a
            // click outside takes no focus, and closing the menu destroys
            // nothing but hides it). onTextEdited fires for user edits only,
            // so the text binding cannot trigger a write.
            onTextEdited: root.nameEdited(text)
            onEditingFinished: root.nameEdited(text)
            Label {
                anchors.fill: parent
                verticalAlignment: Text.AlignVCenter
                visible: nameField.text.length === 0
                text: qsTr("Name this theme")
                color: AppTheme.editorTextMuted
                font: nameField.font
            }
        }
    }

    Flow {
        Layout.fillWidth: true
        visible: !root.confirmingDelete
        spacing: AppTheme.spacing6

        MenuButton {
            objectName: "customThemeNewButton"
            text: qsTr("New")
            enabled: root.canCreate
            onClicked: root.newRequested()
        }
        MenuButton {
            objectName: "customThemeDuplicateButton"
            text: qsTr("Duplicate")
            enabled: root.canDuplicate && root.canCreate
            onClicked: root.duplicateRequested()
        }
        MenuButton {
            objectName: "customThemeShareButton"
            text: qsTr("Share…")
            enabled: root.canDuplicate
            onClicked: root.shareRequested()
        }
        MenuButton {
            objectName: "customThemeImportButton"
            text: qsTr("Import…")
            onClicked: root.importRequested()
        }
        MenuButton {
            objectName: "customThemeDeleteButton"
            text: qsTr("Delete")
            danger: true
            enabled: root.canDelete
            onClicked: root.deleteRequested()
        }
    }

    // Delete, confirmed inline: a deleted theme cannot be undone, and its
    // history goes with it.
    ColumnLayout {
        Layout.fillWidth: true
        visible: root.confirmingDelete
        spacing: AppTheme.spacing6
        Label {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            text: qsTr("Delete “%1”? This cannot be undone.")
                  .arg(root.themeName.length > 0 ? root.themeName
                                                 : qsTr("Untitled"))
            color: AppTheme.editorText
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textMeta
        }
        Row {
            spacing: AppTheme.spacing6
            MenuButton {
                objectName: "customThemeDeleteConfirmButton"
                danger: true
                text: qsTr("Delete")
                onClicked: root.deleteConfirmed()
            }
            MenuButton {
                objectName: "customThemeDeleteKeepButton"
                text: qsTr("Keep")
                onClicked: root.deleteCancelled()
            }
        }
    }
}
