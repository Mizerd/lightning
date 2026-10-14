import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The readability report as the contents of a popover under the editor's
// readability pill: every colour pair the theme fails, each a row that jumps to
// the element and a "Make readable" that nudges it, plus what could not be
// checked (said, so "all passed" is not confused with "all we could check
// passed").
//
// It holds no theme state. The editor passes the findings (CustomThemeStore's
// audit rows: { role, fg, bg, label, kind, value, minimum }) and the palette
// they were graded on, and answers the signals.
//
// Painted in the invariant editor tokens (see `editorCanvas` in AppTheme.qml).
ColumnLayout {
    id: root

    // The failing rows, as audit() returns them.
    property var report: []
    // The throttled palette the rows were graded on, for the colour pair swatch.
    property var gradedPalette: ({})
    property int problems: 0
    // True once the theme has colours of its own (wording only).
    property bool edited: false
    // Colours that could not be graded because they are see-through, and
    // checks this build could not make at all.
    property int translucent: 0
    property int missing: 0

    signal findingRequested(string role)
    signal fixRequested(string role)
    signal closeRequested()

    objectName: "themeReadabilityPanel"
    spacing: AppTheme.spacing8

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

    // What was not checked, stated.
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

    // The readability table names a palette key this build does not return,
    // which is our bug; a suite keeps it unreachable.
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
        // Named so a focused row can scroll itself into view; this list often
        // overflows.
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

        // A bar only when the list scrolls.
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

            // Rows are a font-derived constant height: a wrapping Label feeding
            // its own row height is a layout loop waiting to happen.
            FontMetrics {
                id: reportMetrics
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.textMeta
            }
            // Ceiling per line, as Qt lays each out at an integral height.
            readonly property int rowTextHeight:
                2 * Math.ceil(reportMetrics.lineSpacing)
            readonly property int rowHeight:
                44 + Math.ceil(reportMetrics.lineSpacing)

            Repeater {
                model: root.report
                delegate: Rectangle {
                    id: problemRow
                    required property var modelData
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

                    // Behind the content, so the buttons on top still win.
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

                        // The two failing colours, one on the other, at the
                        // size they fail at.
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
                                // Pinned to two lines so the ratio sits at the
                                // same height in every row.
                                Layout.preferredHeight: problemColumn.rowTextHeight
                                verticalAlignment: Text.AlignVCenter
                                wrapMode: Text.WordWrap
                                maximumLineCount: 2
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
                                        .arg(problemRow.modelData.value.toFixed(1))
                                        .arg(problemRow.modelData.minimum.toFixed(1))
                                      : qsTr("ΔL* %1 — needs %2")
                                        .arg(problemRow.modelData.value.toFixed(1))
                                        .arg(problemRow.modelData.minimum.toFixed(1))
                                color: AppTheme.editorDanger
                                font.family: AppTheme.monoFont
                                font.pixelSize: AppTheme.textMeta
                            }
                        }

                        // Nudges the colour of this row's element to the
                        // nearest one that clears every bar it fails.
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
                                onClicked: root.fixRequested(problemRow.modelData.role)
                            }
                        }
                    }
                }
            }
        }
    }
}
