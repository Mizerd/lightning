import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.7.4: the renderer for one fenced code block.
//
// Why this exists at all. Qt's rich-text engine treats <pre> as PREFORMATTED
// and refuses to wrap it, so a single long terminal line laid a message
// TextEdit out far past its own width. MessageDelegate's root is deliberately
// `clip: false` (the hover action bar overhangs it), so that overflow escaped
// the bubble and the timeline entirely. Styling the one rich-text item better
// cannot fix that: the fix is to stop asking a rich-text item to hold content
// it is documented not to wrap, and give the block its own bounded,
// horizontally scrollable surface. MessageHtml::segments() is the split that
// makes that possible; this is the other half.
//
// Public surface is exactly:
//     CodeBlock { code: "…"; language: "…" }
// plus whatever width / Layout.* the caller sets. Everything else here is
// readonly or internal.
Rectangle {
    id: root

    // The program text. PLAIN text, already entity-decoded by
    // MessageHtml::segments() — it is rendered with Text.PlainText, so
    // "&lt;b&gt;" arrives as those six literal characters and can never
    // become markup again.
    property string code: ""
    // Optional language tag. MessageHtml already validated it against
    // ^[A-Za-z0-9+#._-]{1,24}$, and it is re-validated here rather than
    // trusted: a class attribute is sender-chosen text, this label is
    // user-visible AND part of an accessible name, and the component must
    // be safe for any caller, not only the one that exists today.
    property string language: ""

    readonly property string safeLanguage:
        /^[A-Za-z0-9+#._-]{1,24}$/.test(root.language) ? root.language : ""

    // Line count of the program, which is also the number of gutter rows.
    // wrapMode is NoWrap, so one source line is exactly one visual line and
    // the gutter cannot drift out of step with the code.
    readonly property int lineCount: root.code.length === 0
                                     ? 1 : root.code.split("\n").length

    // Bounded height: a 4000-line paste must not own the whole viewport.
    // Past this the block scrolls internally (see the wheel note below for
    // why that scrolling is deliberately NOT on the mouse wheel).
    readonly property real maxBodyHeight: AppTheme.scaled(360)
    readonly property real framePadding: AppTheme.spacing8
    readonly property real gutterGap: AppTheme.spacing8

    readonly property bool horizontalOverflow:
        codeFlick.contentWidth > codeFlick.width + 1
    // The Flickable's own bottomMargin is part of its scrollable extent, so
    // the horizontal bar's reserved band counts as content the reader can
    // reach — otherwise the last line would sit permanently under the bar.
    readonly property bool verticalOverflow:
        codeFlick.contentHeight + codeFlick.bottomMargin > codeFlick.height + 1
    // Room for the horizontal bar so it never paints over the last line. The
    // bar is attached to the Flickable and anchors to ITS bottom edge, so the
    // band has to be inside the Flickable (as bottomMargin), not below it.
    readonly property real horizontalBarSpace: root.horizontalOverflow ? 8 : 0

    // The natural width of the widest line — used ONLY so a two-word snippet
    // does not stretch the bubble. It is CLAMPED, because the widest line is
    // precisely the unbounded number that escaped the timeline in the first
    // place: past the clamp the overflow becomes contentX, never geometry.
    readonly property real naturalContentWidth:
        gutterText.implicitWidth + root.gutterGap + codeArea.implicitWidth
        + 2 * root.framePadding
    implicitWidth: Math.min(
        Math.max(root.naturalContentWidth,
                 headerRow.implicitWidth + 2 * root.framePadding),
        AppTheme.timelineContentMaxWidth)
    implicitHeight: bodyColumn.implicitHeight + 2 * root.framePadding

    color: AppTheme.codeBlock
    radius: AppTheme.radiusMd
    border.width: 1
    // The focus ring is the border itself rather than a second rectangle:
    // one 1px outline, in the shared focus ink, exactly like the app's other
    // focusable surfaces.
    border.color: root.focusWithin ? AppTheme.focusRing : AppTheme.border

    readonly property bool focusWithin:
        root.activeFocus || codeArea.activeFocus || copyButton.activeFocus

    activeFocusOnTab: true
    Accessible.role: Accessible.Grouping
    // Counts and the validated language token only — never the program text.
    Accessible.name: root.safeLanguage.length > 0
        ? qsTr("Code block, %1, %2 lines").arg(root.safeLanguage)
                                          .arg(root.lineCount)
        : qsTr("Code block, %1 lines").arg(root.lineCount)

    function copyCode() {
        // Copies root.code and nothing else — never the gutter, and never a
        // selection made in the visible TextEdit. Copying THROUGH the visible
        // editor (selectAll/copy/deselect) is the tempting shortcut and is
        // wrong twice: it flashes a selection the user did not make, and it
        // destroys the selection they DID make. The hidden-TextEdit idiom is
        // the one already used for "copy room link" and "copy user id".
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

    // Keyboard: the block owns horizontal motion (which nothing else in the
    // timeline offers) and, only while it actually overflows, vertical motion
    // inside itself. Anything it does not use is explicitly NOT accepted, so
    // PageUp/PageDown/Home/End keep reaching the timeline's own handler.
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

            // A Loader, NOT a `visible:` gate, and the distinction is the
            // single most expensive QML mistake known in this codebase.
            // Every QQuickText is BORN carrying ItemObservesViewport, and the
            // only code that clears it is QQuickText::setText — which opens
            // with `if (d->text == n) return;`, BEFORE the line that clears
            // the flag. So a Label created holding "" never clears it, and
            // QQuickItemPrivate::transformChanged can no longer prune this
            // subtree on every contentY change. Visibility is irrelevant to
            // the mechanism; being created empty is what matters, and a fence
            // with no language tag is the common case. The 2026-08-19 scroll
            // round removed exactly this hazard with seven Loaders and there
            // is a test requiring ZERO observers across a timeline row tree.
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

        // The scrolling body. Height is the content's, capped, plus room for
        // the horizontal bar when there is one.
        Item {
            id: bodyArea
            objectName: "codeBlockBody"
            width: parent.width
            height: Math.min(codeArea.implicitHeight, root.maxBodyHeight)
                    + root.horizontalBarSpace
            clip: true

            // The gutter lives OUTSIDE the Flickable and is translated by
            // -contentY, so it scrolls vertically WITH the code but stays
            // pinned horizontally — line numbers you cannot scroll away from.
            // The tempting alternative (put the gutter inside the Flickable's
            // content) makes the numbers slide off to the left the moment the
            // reader scrolls right, which is exactly when they are needed.
            Text {
                id: gutterText
                objectName: "codeBlockGutter"
                x: 0
                y: -codeFlick.contentY
                width: implicitWidth
                horizontalAlignment: Text.AlignRight
                textFormat: Text.PlainText
                // Deliberately a Text, not a TextEdit: the gutter must never
                // be selectable, or "select the whole block and copy" would
                // carry line numbers into the paste.
                text: {
                    var lines = []
                    for (var i = 1; i <= root.lineCount; ++i)
                        lines.push(String(i))
                    return lines.join("\n")
                }
                color: AppTheme.textDisabled
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
                contentWidth: codeArea.implicitWidth
                contentHeight: codeArea.implicitHeight
                bottomMargin: root.horizontalBarSpace

                // ── The nested-scroll decision, and why it is this one ──────
                // The room timeline is a Flickable rotated 180 degrees whose
                // WheelHandler (TimelinePane.qml's timelineWheelHandler) is
                // "the single pointer-wheel owner": it drives pagination,
                // anchoring, follow-latest and the settle timer. Wheel events
                // are delivered innermost-first, so a Flickable in a message
                // row sees every notch BEFORE that owner does.
                //
                // QQuickFlickable::wheelEvent early-returns when
                // `interactive` is false, leaving the event ignored so it
                // propagates outward. That is the ONLY reliable way to stay
                // out of the timeline's way, so this Flickable is a viewport,
                // never an input surface. A vertical notch over a code block
                // therefore scrolls the CONVERSATION, exactly as it does over
                // any other row — a code block that swallowed the wheel would
                // trap the reader mid-timeline.
                //
                // It costs nothing else: `selectByMouse` on the editor below
                // already owns press-and-drag, so an interactive Flickable
                // here would have fought text selection anyway. Scrolling
                // inside the block is by the scrollbars, the keyboard
                // (Keys.onPressed above) and Shift+wheel / horizontal wheel
                // (wheelRouter below) — none of which the timeline wants.
                interactive: false

                TextEdit {
                    id: codeArea
                    objectName: "codeBlockText"
                    text: root.code
                    // PlainText is load-bearing, not a default: the segment
                    // text is already entity-decoded, so RichText here would
                    // re-interpret a program's own angle brackets as markup.
                    textFormat: Text.PlainText
                    readOnly: true
                    selectByMouse: true
                    selectByKeyboard: true
                    // No wrapping: a code block's line breaks are the
                    // program's. Wrapping is what the horizontal scroll
                    // replaces.
                    wrapMode: Text.NoWrap
                    activeFocusOnTab: false
                    padding: 0
                    textMargin: 0
                    color: AppTheme.textPrimary
                    selectionColor: AppTheme.accent
                    selectedTextColor: AppTheme.accentText
                    font.family: AppTheme.monoFont
                    font.pixelSize: AppTheme.scaled(AppTheme.fontMono)
                    // The screen reader reads the code from here; the frame
                    // above carries the counts-only summary.
                    Accessible.role: Accessible.EditableText
                    Accessible.name: qsTr("Code")
                }

                // Before AppScrollBar existed this was the ONE
                // hand-styled scrollbar in the repository (a flat
                // borderStrong pill, no hover or press step). It now rides
                // the shared control, so the code block and every other
                // scrolling surface finally agree — and `thin` keeps it from
                // widening on hover inside a message row, where the extra
                // 4px would reflow the block's reserved bar band.
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

            // Horizontal-intent wheel only. `acceptedButtons: Qt.NoButton`
            // means presses fall straight through to the editor's selection
            // handling; MouseArea's documented contract is that an UNaccepted
            // wheel is passed on with QQuickItem::wheelEvent, which is what
            // hands a plain vertical notch back to the timeline.
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
