import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

ColumnLayout {
    id: root

    property var report: []
    property var gradedPalette: ({})
    property int problems: 0
    property bool edited: false
    property int translucent: 0
    property int missing: 0

    signal findingRequested(string role)
    signal fixRequested(string role)
    signal closeRequested()

    objectName: "themeReadabilityPanel"
    spacing: AppTheme.spacing8

    function firstStop() {
        var n = problemRepeater.count
        var row = n > 0 ? problemRepeater.itemAt(0) : null
        return row && row.fixItem ? row.fixItem : closeButton
    }
    function lastStop() {
        var n = problemRepeater.count
        var row = n > 0 ? problemRepeater.itemAt(n - 1) : null
        return row && row.fixItem ? row.fixItem : closeButton
    }
    function takeFocus() {
        root.firstStop().forceActiveFocus()
    }
    property bool refocusAfterRebuild: false
    Timer {
        id: refocusExpiry
        interval: 600
        onTriggered: root.refocusAfterRebuild = false
    }
    onRefocusAfterRebuildChanged: {
        if (root.refocusAfterRebuild)
            refocusExpiry.restart()
        else
            refocusExpiry.stop()
    }
    onReportChanged: {
        if (root.refocusAfterRebuild)
            Qt.callLater(function () {
                root.refocusAfterRebuild = false
                var held = root.Window.activeFocusItem
                var inside = false
                for (var it = held; it; it = it.parent)
                    if (it === root) { inside = true; break }
                if (!inside && root.visible)
                    root.takeFocus()
            })
    }
    onVisibleChanged: if (visible) Qt.callLater(root.takeFocus)
    Keys.onTabPressed: (e) => {
        if (root.Window.activeFocusItem === root.lastStop()) {
            closeButton.forceActiveFocus()
            e.accepted = true
        }
    }
    Keys.onBacktabPressed: (e) => {
        if (root.Window.activeFocusItem === closeButton) {
            root.lastStop().forceActiveFocus()
            e.accepted = true
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: AppTheme.spacing8
        Label {
            objectName: "themeReadabilityTitle"
            Layout.fillWidth: true
            text: root.problems > 0
                  ? qsTr("Hard to read")
                  : root.edited ? qsTr("Nothing hard to read")
                                : qsTr("Readable")
            color: root.problems > 0 ? AppTheme.editorDanger
                                     : AppTheme.editorText
            font.family: AppTheme.uiFont
            font.pixelSize: AppTheme.textBody
            font.weight: AppTheme.weightStrong
            elide: Label.ElideRight
        }
        Rectangle {
            id: closeButton
            objectName: "themeReportCloseButton"
            implicitWidth: 26
            implicitHeight: 26
            radius: AppTheme.radiusSm
            color: closeHover.containsMouse ? AppTheme.editorSelection
                                            : "transparent"
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Close the readability report")
            Keys.onPressed: (e) => {
                if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter
                        || e.key === Qt.Key_Space) {
                    root.closeRequested()
                    e.accepted = true
                }
            }
            Rectangle {
                anchors.fill: parent
                visible: parent.activeFocus
                radius: AppTheme.radiusSm
                color: "transparent"
                border.width: 2
                border.color: AppTheme.editorAccent
            }
            Icon {
                anchors.centerIn: parent
                name: "close"
                size: 14
                color: AppTheme.editorTextSecondary
            }
            MouseArea {
                id: closeHover
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.closeRequested()
            }
        }
    }

    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: root.problems > 0
              ? qsTr("These are measured against the same rules the built-in themes meet. Click one to open it, or fix it here.")
              : root.edited
                ? qsTr("Every text and edge this theme paints clears the bar the built-in themes clear.")
                : qsTr("Nothing has been changed yet, so this is the starting preset as it ships.")
        color: AppTheme.editorTextMuted
        font.family: AppTheme.uiFont
        font.pixelSize: AppTheme.textMeta
    }

    Label {
        objectName: "themeReadabilityUnchecked"
        visible: root.translucent > 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        // Worded to read correctly at n == 1: the English catalog has no
        // numerus forms for this family, so the source string is shown at
        // every n.
        text: qsTr("%n colour(s) could not be checked: a see-through colour reads differently depending on what is behind it.",
                   "custom theme readability", root.translucent)
        color: AppTheme.editorTextMuted
        font.family: AppTheme.uiFont
        font.pixelSize: AppTheme.textMeta
    }

    Label {
        objectName: "themeReadabilityMissing"
        visible: root.missing > 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        text: qsTr("%n check(s) could not be made: this build has no colour by that name.",
                   "custom theme readability", root.missing)
        color: AppTheme.editorDanger
        font.family: AppTheme.uiFont
        font.pixelSize: AppTheme.textMeta
    }

    Flickable {
        id: problemScroll
        objectName: "themeReadabilityScroll"
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.preferredHeight: problemColumn.implicitHeight
        Layout.maximumHeight: 360
        visible: root.problems > 0
        clip: true
        contentWidth: width
        contentHeight: problemColumn.implicitHeight
        boundsBehavior: Flickable.StopAtBounds

        ScrollBar.vertical: ScrollBar {
            policy: problemScroll.contentHeight > problemScroll.height + 1
                    ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
            contentItem: Rectangle {
                implicitWidth: 5
                radius: 2.5
                color: AppTheme.editorBorderStrong
            }
        }

        ColumnLayout {
            id: problemColumn
            width: parent.width
            spacing: 2

            FontMetrics {
                id: reportMetrics
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textMeta
            }
            readonly property int rowTextHeight:
                2 * Math.ceil(reportMetrics.lineSpacing)
            readonly property int rowHeight:
                44 + Math.ceil(reportMetrics.lineSpacing)

            Repeater {
                id: problemRepeater
                model: root.report
                delegate: Rectangle {
                    id: problemRow
                    required property var modelData
                    readonly property Item fixItem: fixButton
                    objectName: "themeReadabilityRow"
                    Layout.fillWidth: true
                    implicitHeight: problemColumn.rowHeight
                    radius: AppTheme.radiusSm
                    color: problemHover.containsMouse ? AppTheme.editorInset
                                                      : "transparent"

                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: problemText.text
                    // Assigning contentY does not clamp.
                    onActiveFocusChanged: {
                        if (!activeFocus)
                            return
                        var top = mapToItem(problemColumn, 0, 0).y
                        var want = problemScroll.contentY
                        if (top < want)
                            want = top
                        else if (top + height > want + problemScroll.height)
                            want = top + height - problemScroll.height
                        problemScroll.contentY = Math.max(
                            0, Math.min(want, problemScroll.contentHeight
                                              - problemScroll.height))
                    }
                    Keys.onPressed: (e) => {
                        if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter
                                || e.key === Qt.Key_Space) {
                            root.findingRequested(problemRow.modelData.role)
                            e.accepted = true
                        }
                    }
                    Rectangle {
                        anchors.fill: parent
                        visible: problemRow.activeFocus
                        radius: AppTheme.radiusSm
                        color: "transparent"
                        border.width: 2
                        border.color: AppTheme.editorAccent
                        z: 1
                    }

                    MouseArea {
                        id: problemHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.findingRequested(problemRow.modelData.role)
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: AppTheme.spacing8
                        anchors.rightMargin: AppTheme.spacing8
                        spacing: AppTheme.spacing8

                        Rectangle {
                            implicitWidth: 30
                            implicitHeight: 30
                            radius: AppTheme.radiusSm
                            color: root.gradedPalette[problemRow.modelData.bg]
                            border.width: 1
                            border.color: AppTheme.editorBorderStrong
                            Label {
                                anchors.centerIn: parent
                                //: A two-letter type specimen shown on a colour
                                //: pair to demonstrate its readability.
                                //: Translate to whichever letters best show
                                //: this locale's script — a Latin "Aa" says
                                //: nothing about legibility in Cyrillic, Greek
                                //: or CJK.
                                text: qsTr("Aa")
                                color: root.gradedPalette[problemRow.modelData.fg]
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMeta
                                font.weight: AppTheme.weightStrong
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: 0
                            Label {
                                id: problemText
                                objectName: "themeReadabilityRowLabel"
                                Layout.fillWidth: true
                                Layout.preferredHeight: problemColumn.rowTextHeight
                                verticalAlignment: Text.AlignVCenter
                                wrapMode: Text.WordWrap
                                maximumLineCount: 2
                                lineHeightMode: Text.FixedHeight
                                lineHeight: problemColumn.rowTextHeight / 2
                                textFormat: Text.PlainText
                                text: problemRow.modelData.label
                                color: AppTheme.editorText
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMeta
                                elide: Label.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                textFormat: Text.PlainText
                                text: problemRow.modelData.kind === "ink"
                                      ? qsTr("%1:1 — needs %2:1")
                                        .arg(problemRow.modelData.value.toLocaleString(Qt.locale(AppLocale.name), "f", 1))
                                        .arg(problemRow.modelData.minimum.toLocaleString(Qt.locale(AppLocale.name), "f", 1))
                                      : qsTr("ΔL* %1 — needs %2")
                                        .arg(problemRow.modelData.value.toLocaleString(Qt.locale(AppLocale.name), "f", 1))
                                        .arg(problemRow.modelData.minimum.toLocaleString(Qt.locale(AppLocale.name), "f", 1))
                                color: AppTheme.editorDanger
                                font.family: AppTheme.monoFont
                                font.pixelSize: AppTheme.textMeta
                            }
                        }

                        Rectangle {
                            id: fixButton
                            objectName: "themeReadabilityFix"
                            implicitWidth: fixLabel.implicitWidth
                                           + AppTheme.spacing12 * 2
                            implicitHeight: 26
                            radius: AppTheme.radiusSm
                            color: fixHover.containsMouse
                                   ? AppTheme.editorSelection : "transparent"
                            border.width: 1
                            border.color: AppTheme.editorBorderStrong
                            activeFocusOnTab: true
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Make this readable")
                            Accessible.description:
                                qsTr("Move the colour to the nearest one that is easy to read against its neighbours")
                            Keys.onPressed: (e) => {
                                if (e.key === Qt.Key_Return
                                        || e.key === Qt.Key_Enter
                                        || e.key === Qt.Key_Space) {
                                    root.refocusAfterRebuild = true
                                    root.fixRequested(problemRow.modelData.role)
                                    e.accepted = true
                                }
                            }
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: 2
                                visible: fixButton.activeFocus
                                radius: AppTheme.radiusSm
                                color: "transparent"
                                border.width: 2
                                border.color: AppTheme.editorAccent
                            }
                            Label {
                                id: fixLabel
                                anchors.centerIn: parent
                                text: qsTr("Fix")
                                color: AppTheme.editorText
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMeta
                                font.weight: AppTheme.weightStrong
                            }
                            MouseArea {
                                id: fixHover
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    root.refocusAfterRebuild = true
                                    root.fixRequested(problemRow.modelData.role)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
