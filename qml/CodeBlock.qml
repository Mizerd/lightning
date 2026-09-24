import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Renderer for one fenced code block.
//
// Qt's rich-text engine never wraps <pre>, so a long line would lay a message
// TextEdit out far past its width and escape the bubble (MessageDelegate's
// root is `clip: false`). Instead the block gets its own bounded,
// horizontally scrollable surface; MessageHtml::segments() does the split.
//
// Public surface: `CodeBlock { code: "…"; language: "…" }` plus whatever
// width / Layout.* the caller sets.
Rectangle {
    id: root

    // Code reads left to right in every language, so this opts out of the
    // app-wide RTL mirroring: the gutter stays on the code's leading edge.
    LayoutMirroring.enabled: false
    LayoutMirroring.childrenInherit: true

    // Plain text, already entity-decoded by MessageHtml::segments(); rendered
    // with Text.PlainText so it can never become markup again.
    property string code: ""
    // Optional language tag. Re-validated here (MessageHtml already did) since
    // it is sender-chosen and appears in the UI and the accessible name.
    property string language: ""

    readonly property string safeLanguage:
        /^[A-Za-z0-9+#._-]{1,24}$/.test(root.language) ? root.language : ""

    // Line count, which is also the number of gutter rows (NoWrap keeps them in
    // step).
    readonly property int lineCount: root.code.length === 0
                                     ? 1 : root.code.split("\n").length

    // Bounded height; taller code scrolls internally (not on the mouse wheel;
    // see below).
    readonly property real maxBodyHeight: AppTheme.scaled(360)
    readonly property real framePadding: AppTheme.spacing8
    readonly property real gutterGap: AppTheme.spacing8

    readonly property bool horizontalOverflow:
        codeFlick.contentWidth > codeFlick.width + 1
    // Width-independent on purpose: comparing against codeFlick.height would
    // depend on horizontalOverflow, which depends on the width that
    // verticalBarSpace consumes, forming a binding loop. Equivalent to the
    // height comparison.
    readonly property bool verticalOverflow:
        codeArea.implicitHeight > root.maxBodyHeight + 1
    // Room for the horizontal bar inside the Flickable (as bottomMargin), so it
    // never covers the last line.
    readonly property real horizontalBarSpace: root.horizontalOverflow ? 8 : 0
    // The same band for the vertical bar. An attached ScrollBar takes no layout
    // width, and MessageDelegate sizes a code segment at
    // `min(segmentCap, implicitWidth)`, so without it the bar covers the end of
    // the widest line with no scroll range to clear it. Added to contentWidth
    // rather than as a margin because the overflow check, the Right key and the
    // wheel router all clamp on contentWidth.
    readonly property real verticalBarSpace: root.verticalOverflow ? 8 : 0

    // Natural width of the widest line, so a short snippet doesn't stretch the
    // bubble. Clamped: past it, overflow becomes contentX, never geometry.
    readonly property real naturalContentWidth:
        gutterText.implicitWidth + root.gutterGap + codeArea.implicitWidth
        + root.verticalBarSpace + 2 * root.framePadding
    implicitWidth: Math.min(
        Math.max(root.naturalContentWidth,
                 headerRow.implicitWidth + 2 * root.framePadding),
        AppTheme.timelineContentMaxWidth)
    implicitHeight: bodyColumn.implicitHeight + 2 * root.framePadding

    color: AppTheme.codeBlock
    radius: AppTheme.radiusMd
    border.width: 1
    // The border doubles as the focus ring.
    border.color: root.focusWithin ? AppTheme.focusRing : AppTheme.border

    readonly property bool focusWithin:
        root.activeFocus || codeArea.activeFocus || copyButton.activeFocus

    activeFocusOnTab: true
    Accessible.role: Accessible.Grouping
    // Counts and the validated language only, never the program text.
    Accessible.name: root.safeLanguage.length > 0
        ? qsTr("Code block, %1, %2 lines").arg(root.safeLanguage)
                                          .arg(root.lineCount)
        : qsTr("Code block, %1 lines").arg(root.lineCount)

    function copyCode() {
        // Copies root.code only (never the gutter) via a hidden TextEdit, so
        // the visible editor's selection is neither flashed nor destroyed.
        clipboardRelay.text = root.code
        clipboardRelay.selectAll()
        clipboardRelay.copy()
        clipboardRelay.text = ""
        root.copied = true
        copiedTimer.restart()
    }

    property bool copied: false

    TextEdit {
        id: clipboardRelay
        visible: false
        width: 0
        height: 0
    }
    Timer {
        id: copiedTimer
        interval: 1500
        onTriggered: root.copied = false
    }

    // The block owns horizontal motion and, only while it overflows, vertical
    // motion. Unused keys aren't accepted, so PageUp/PageDown/Home/End reach
    // the timeline.
    Keys.onPressed: (event) => {
        var step = AppTheme.scaled(48)
        if (event.matches(StandardKey.Copy)) {
            root.copyCode(); event.accepted = true; return
        }
        switch (event.key) {
        case Qt.Key_Left:
            codeFlick.contentX = Math.max(0, codeFlick.contentX - step)
            event.accepted = true
            break
        case Qt.Key_Right:
            codeFlick.contentX = Math.min(
                Math.max(0, codeFlick.contentWidth - codeFlick.width),
                codeFlick.contentX + step)
            event.accepted = true
            break
        case Qt.Key_Up:
            if (!root.verticalOverflow) { event.accepted = false; break }
            codeFlick.contentY = Math.max(0, codeFlick.contentY - step)
            event.accepted = true
            break
        case Qt.Key_Down:
            if (!root.verticalOverflow) { event.accepted = false; break }
            codeFlick.contentY = Math.min(
                Math.max(0, codeFlick.contentHeight - codeFlick.height),
                codeFlick.contentY + step)
            event.accepted = true
            break
        default:
            event.accepted = false
        }
    }

    Column {
        id: bodyColumn
        anchors.fill: parent
        anchors.margins: root.framePadding
        spacing: AppTheme.spacing4

        RowLayout {
            id: headerRow
            width: parent.width
            height: 24
            spacing: AppTheme.spacing8

            // A Loader, not a `visible:` gate: a Text created empty never
            // clears ItemObservesViewport (QQuickText::setText returns early
            // for equal text), which defeats viewport pruning on every scroll.
            // A fence without a language is the common case, and a test
            // requires zero observers across a timeline row.
            Loader {
                active: root.safeLanguage.length > 0
                Layout.alignment: Qt.AlignVCenter
                sourceComponent: Label {
                    objectName: "codeBlockLanguageLabel"
                    text: root.safeLanguage
                    textFormat: Text.PlainText
                    color: AppTheme.textMuted
                    font.family: AppTheme.monoFont
                    font.pixelSize: AppTheme.fontMonoXS
                }
            }
            Item { Layout.fillWidth: true }
            Label {
                objectName: "codeBlockCopiedNotice"
                visible: root.copied
                text: qsTr("Copied")
                textFormat: Text.PlainText
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.fontCaption
                Layout.alignment: Qt.AlignVCenter
            }
            IconButton {
                id: copyButton
                objectName: "codeBlockCopyButton"
                iconName: root.copied ? "check" : "content_copy"
                iconSize: 15
                implicitWidth: 24
                implicitHeight: 24
                Layout.alignment: Qt.AlignVCenter
                Accessible.name: qsTr("Copy code")
                Accessible.onPressAction: root.copyCode()
                onClicked: root.copyCode()
            }
        }

        // The scrolling body: content height, capped, plus the horizontal bar's
        // band.
        Item {
            id: bodyArea
            objectName: "codeBlockBody"
            width: parent.width
            height: Math.min(codeArea.implicitHeight, root.maxBodyHeight)
                    + root.horizontalBarSpace
            clip: true

            // The gutter sits outside the Flickable, translated by -contentY,
            // so it scrolls vertically with the code but stays put
            // horizontally.
            Text {
                id: gutterText
                objectName: "codeBlockGutter"
                x: 0
                y: -codeFlick.contentY
                width: implicitWidth
                horizontalAlignment: Text.AlignRight
                textFormat: Text.PlainText
                // A Text, not a TextEdit: line numbers must never be selectable
                // or copied.
                text: {
                    var lines = []
                    for (var i = 1; i <= root.lineCount; ++i)
                        lines.push(String(i))
                    return lines.join("\n")
                }
                // textMuted, not textDisabled: the code-block fill is lighter
                // than the surface on light themes, where textDisabled falls to
                // 1.56-2.14:1. textMuted clears 4.5:1 on every palette.
                color: AppTheme.textMuted
                font.family: codeArea.font.family
                font.pixelSize: codeArea.font.pixelSize
                Accessible.ignored: true
            }

            Flickable {
                id: codeFlick
                objectName: "codeBlockScroll"
                x: gutterText.width + root.gutterGap
                width: Math.max(0, parent.width - x)
                height: parent.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                // The band is part of the scrollable extent (see
                // root.verticalBarSpace), so max contentX keeps the widest line
                // clear of the bar.
                contentWidth: codeArea.implicitWidth + root.verticalBarSpace
                contentHeight: codeArea.implicitHeight
                bottomMargin: root.horizontalBarSpace

                // ── Nested scrolling ── Wheel events reach this before
                // TimelinePane's timelineWheelHandler, which owns pagination,
                // anchoring and follow-latest. A non-interactive Flickable
                // ignores wheel events so they propagate, so vertical notches
                // scroll the conversation. Inside the block, scrolling is by
                // scrollbars, the keyboard and Shift/horizontal wheel
                // (wheelRouter). selectByMouse owns press-and-drag anyway.
                interactive: false

                TextEdit {
                    id: codeArea
                    objectName: "codeBlockText"
                    text: root.code
                    // PlainText: the text is already entity-decoded, so
                    // RichText would turn the program's angle brackets into
                    // markup.
                    textFormat: Text.PlainText
                    readOnly: true
                    selectByMouse: true
                    selectByKeyboard: true
                    // No wrapping: line breaks are the program's.
                    wrapMode: Text.NoWrap
                    activeFocusOnTab: false
                    padding: 0
                    textMargin: 0
                    color: AppTheme.textPrimary
                    selectionColor: AppTheme.accent
                    selectedTextColor: AppTheme.accentText
                    font.family: AppTheme.monoFont
                    font.pixelSize: AppTheme.scaled(AppTheme.fontMono)
                    // The screen reader reads the code here; the frame carries
                    // the counts-only summary.
                    Accessible.role: Accessible.EditableText
                    Accessible.name: qsTr("Code")
                }

                // `thin` so hover doesn't widen the bar and reflow the reserved
                // band.
                ScrollBar.vertical: AppScrollBar {
                    objectName: "codeBlockVerticalScrollBar"
                    thin: true
                    policy: root.verticalOverflow ? ScrollBar.AlwaysOn
                                                  : ScrollBar.AlwaysOff
                    interactive: true
                }
                ScrollBar.horizontal: AppScrollBar {
                    objectName: "codeBlockHorizontalScrollBar"
                    thin: true
                    policy: root.horizontalOverflow ? ScrollBar.AlwaysOn
                                                    : ScrollBar.AlwaysOff
                    interactive: true
                }
            }

            // Horizontal-intent wheel only. NoButton lets presses reach the
            // editor; an unaccepted wheel is passed on, so vertical notches go
            // to the timeline.
            MouseArea {
                id: wheelRouter
                objectName: "codeBlockWheelRouter"
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                onWheel: (wheel) => {
                    var dx = wheel.angleDelta.x !== 0 ? wheel.angleDelta.x
                                                      : wheel.pixelDelta.x
                    var shifted = (wheel.modifiers & Qt.ShiftModifier) !== 0
                    if (dx === 0 && shifted)
                        dx = wheel.angleDelta.y !== 0 ? wheel.angleDelta.y
                                                      : wheel.pixelDelta.y
                    if (dx === 0 || !root.horizontalOverflow) {
                        // Vertical (or nothing to scroll): the timeline's.
                        wheel.accepted = false
                        return
                    }
                    var maxX = Math.max(0, codeFlick.contentWidth
                                           - codeFlick.width)
                    codeFlick.contentX = Math.max(
                        0, Math.min(maxX, codeFlick.contentX - dx))
                    wheel.accepted = true
                }
            }
        }
    }
}
