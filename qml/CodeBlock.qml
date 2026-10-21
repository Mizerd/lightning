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

    // Bounded height; taller code scrolls internally, on the wheel too once
    // the pointer has come to rest on it (see wheelRouter).
    readonly property real maxBodyHeight: AppTheme.scaled(360)
    // How long the pointer must have been over the block, with no wheel event
    // handed on to the timeline, before a vertical notch scrolls the block
    // instead of the conversation (the latch, see wheelRouter).
    readonly property int wheelRestMs: 300
    // A vertical wheel stream the block has taken stays the block's, at its
    // edges too, until it pauses this long (the latch's other half).
    readonly property int wheelTrainMs: 500
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

    // The furthest the code can scroll down. The horizontal bar's band is the
    // Flickable's bottomMargin, so it is part of the range: without it the
    // last line stops under the bar.
    function maxContentY() {
        return Math.max(0, codeFlick.contentHeight + codeFlick.bottomMargin
                           - codeFlick.height)
    }

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
    border.color: root.focusWithin ? AppTheme.focusRing : AppTheme.codeBlockBorder

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

    // Width at the header's trailing end that the host keeps for something it
    // draws over the block (the message row's hover action bar sits on the
    // row's top-right and used to take the Copy button's clicks). 0 = none.
    property real toolbarClearance: 0

    // Scrolls the code the least that brings the caret inside the viewport.
    function ensureCaretVisible() {
        var r = codeArea.cursorRectangle
        var viewH = codeFlick.height - codeFlick.bottomMargin
        if (r.y < codeFlick.contentY)
            codeFlick.contentY = Math.max(0, r.y)
        else if (r.y + r.height > codeFlick.contentY + viewH)
            codeFlick.contentY = Math.min(root.maxContentY(),
                                          r.y + r.height - viewH)
        var viewW = codeFlick.width - root.verticalBarSpace
        var maxX = Math.max(0, codeFlick.contentWidth - codeFlick.width)
        if (r.x < codeFlick.contentX)
            codeFlick.contentX = Math.max(0, r.x)
        else if (r.x + r.width > codeFlick.contentX + viewW)
            codeFlick.contentX = Math.min(maxX, r.x + r.width - viewW)
    }

    // Drag-selection past the block's edge: while a left press that began in
    // the code is held outside the body, scroll toward the pointer and keep
    // extending the selection to it (a stationary pointer sends no moves).
    function autoscrollTick() {
        var p = bodyArea.mapFromItem(null, dragPoint.point.scenePosition.x,
                                     dragPoint.point.scenePosition.y)
        var step = AppTheme.scaled(16)
        var dy = p.y < 0 ? p.y : (p.y > bodyArea.height ? p.y - bodyArea.height : 0)
        var dx = p.x < codeFlick.x ? p.x - codeFlick.x
               : (p.x > bodyArea.width ? p.x - bodyArea.width : 0)
        if (dx === 0 && dy === 0)
            return
        var f = function (d) {
            return Math.max(-3, Math.min(3, d / AppTheme.scaled(24))) * step
        }
        if (dy !== 0)
            codeFlick.contentY = Math.max(0, Math.min(root.maxContentY(),
                                                      codeFlick.contentY + f(dy)))
        if (dx !== 0)
            codeFlick.contentX = Math.max(0, Math.min(
                Math.max(0, codeFlick.contentWidth - codeFlick.width),
                codeFlick.contentX + f(dx)))
        var inCode = codeArea.mapFromItem(bodyArea,
            Math.max(codeFlick.x, Math.min(bodyArea.width, p.x)),
            Math.max(0, Math.min(bodyArea.height, p.y)))
        codeArea.moveCursorSelection(codeArea.positionAt(inCode.x, inCode.y),
                                     TextEdit.SelectCharacters)
    }

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
            codeFlick.contentY = Math.min(root.maxContentY(),
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
            width: Math.max(0, parent.width - root.toolbarClearance)
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
                // Return and the keypad Enter copy too: IconButton answers
                // them itself (a held key does not re-copy).

                // The icon alone says nothing to a pointer user.
                ToolTip {
                    objectName: "codeBlockCopyTip"
                    visible: copyButton.hovered
                    delay: 500
                    text: qsTr("Copy code")
                }
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

                // ── Nested scrolling ── Wheel events reach the block before
                // TimelinePane's timelineWheelHandler, which owns pagination,
                // anchoring and follow-latest. A non-interactive Flickable
                // ignores wheel events, so every wheel decision is made in one
                // place, wheelRouter below, and whatever it does not take
                // propagates to the timeline. selectByMouse owns press-and-drag
                // anyway.
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
                    color: AppTheme.codeText
                    selectionColor: AppTheme.accent
                    selectedTextColor: AppTheme.accentText
                    font.family: AppTheme.monoFont
                    font.pixelSize: AppTheme.scaled(AppTheme.fontMono)
                    // The screen reader reads the code here; the frame carries
                    // the counts-only summary.
                    Accessible.role: Accessible.EditableText
                    Accessible.name: qsTr("Code")

                    // The Flickable is non-interactive, so nothing scrolls the
                    // caret into view: a selection grown by shift+Down or by a
                    // drag would otherwise extend off-screen unseen. Driven by
                    // the caret MOVING (cursorPosition), not by its rectangle,
                    // so that nothing but a caret move can undo a wheel scroll.
                    // Deferred so the rectangle has settled.
                    // (selectAll() does not emit cursorPositionChanged, measured,
                    // so Ctrl+A does not jump to the end of a tall block.)
                    onCursorPositionChanged: Qt.callLater(root.ensureCaretVisible)

                    // Passive: watches a held left press, wherever the pointer
                    // goes, to autoscroll a drag-selection (autoscrollTick).
                    // It lives on the TextEdit because the press is accepted
                    // there and never reaches a handler on an ancestor.
                    PointHandler {
                        id: dragPoint
                        objectName: "codeBlockDragPoint"
                        acceptedButtons: Qt.LeftButton
                        // selectByMouse does not select on touch, so a held
                        // finger has no selection to extend.
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    }

                    // Every line left to right: TextEdit would take the whole
                    // block's direction from an Arabic comment on line one.
                    ParagraphDirection {
                        document: codeArea.textDocument
                        leftToRight: true
                    }
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

            Timer {
                id: autoscrollTimer
                objectName: "codeBlockAutoscrollTimer"
                interval: 40
                repeat: true
                running: dragPoint.active
                onTriggered: root.autoscrollTick()
            }

            // The latch's rest clock (see wheelRouter). It starts when the
            // pointer arrives, whether the pointer moved or the timeline
            // carried the block under a still pointer (Qt re-delivers hover
            // when the scene moves), and it starts again whenever the block
            // moves under a pointer that has not moved (a timeline glide still
            // running): only a pointer resting on a resting block has rested.
            // Passive: the row's own hover handling is untouched.
            HoverHandler {
                id: bodyHover
                objectName: "codeBlockBodyHover"
                property point lastScenePos: Qt.point(-1, -1)
                property point lastLocalPos: Qt.point(-1, -1)
                onHoveredChanged: {
                    wheelRouter.restSinceMs = bodyHover.hovered ? Date.now() : -1
                    if (!bodyHover.hovered) {
                        wheelRouter.gesture = ""
                        wheelRouter.trainLastMs = 0
                    }
                }
                onPointChanged: {
                    if (!bodyHover.hovered)
                        return
                    var sp = bodyHover.point.scenePosition
                    var lp = bodyHover.point.position
                    var pointerStill = Math.abs(sp.x - lastScenePos.x) < 0.5
                                       && Math.abs(sp.y - lastScenePos.y) < 0.5
                    var blockMoved = Math.abs(lp.x - lastLocalPos.x) >= 0.5
                                     || Math.abs(lp.y - lastLocalPos.y) >= 0.5
                    if (pointerStill && blockMoved)
                        wheelRouter.restSinceMs = Date.now()
                    lastScenePos = sp
                    lastLocalPos = lp
                }
            }

            // Every wheel decision for the block. NoButton lets presses reach
            // the editor; an unaccepted wheel propagates to the timeline.
            //
            // Horizontal intent (a sideways wheel, Shift+wheel, or motion more
            // sideways than vertical) scrolls the code sideways whenever it
            // overflows, as before.
            //
            // Vertical intent: a block with vertical overflow takes a vertical
            // stream only when it starts on a pointer that has REST on it (the
            // latch): wheelRestMs over the block, the block not moving under
            // it, and nothing handed on to the timeline in that time. Every
            // event handed on restarts that clock, so a stream scrolling the
            // conversation stays on the conversation when a block scrolls
            // under a still pointer, however long it runs.
            //
            // Once the block takes a stream it keeps it, edges included, until
            // the stream pauses for wheelTrainMs (browser-style latching, as
            // Chromium does): the conversation never lurches because the code
            // ran out. A stream that STARTS where the block cannot move that
            // way (at its edge) is the conversation's.
            //
            // Phased touchpad gestures (Wayland, macOS) are decided on their
            // first moving frame and held to ScrollEnd: Qt delivers each frame
            // afresh with no grab (measured), so ownership lives here, per
            // axis. A sideways start claims the sideways axis whenever the code
            // overflows that way, but the vertical axis only under the rest
            // rule. A gesture that began elsewhere stays the timeline's; a
            // ScrollBegin with motion right after a ScrollEnd, and momentum
            // frames, continue the gesture that just ended (macOS momentum).
            //
            // Each event the block takes is reported to the nearest ancestor
            // that defines nestedWheelTaken() (the timeline), so it can do what
            // its own wheel handler does when the reader scrolls: stop an
            // autoscroll and cancel a pending jump.
            MouseArea {
                id: wheelRouter
                objectName: "codeBlockWheelRouter"
                anchors.fill: parent
                acceptedButtons: Qt.NoButton

                // Since when the pointer has rested on the block (see above);
                // -1 while it is not known to be over the block.
                property real restSinceMs: -1
                // When the block last took a vertical notch of a non-phased
                // stream it owns; 0 when it owns none.
                property real trainLastMs: 0
                // Phased gesture in progress: "", "pending" (no motion yet) or
                // "active", with what the block owns of it.
                property string gesture: ""
                property bool gestureRested: false
                property bool ownX: false
                property bool ownY: false
                // The gesture that ended last, for momentum continuation.
                property bool lastOwnX: false
                property bool lastOwnY: false
                property real lastGestureEndMs: 0

                function rested(now) {
                    if (restSinceMs < 0) {
                        // First contact without a hover report: the clock
                        // starts now, so this event is not the block's.
                        restSinceMs = now
                        return false
                    }
                    return now - restSinceMs >= root.wheelRestMs
                }

                // Hands the event to the timeline. Motion handed on restarts
                // the rest clock.
                function passOn(wheel, moving) {
                    if (moving)
                        restSinceMs = Date.now()
                    wheel.accepted = false
                }

                function take(wheel, moving) {
                    wheel.accepted = true
                    if (!moving)
                        return
                    for (var p = root.parent; p; p = p.parent) {
                        if (typeof p.nestedWheelTaken === "function") {
                            p.nestedWheelTaken()
                            return
                        }
                    }
                }

                // Sideways motion: a continuous source's own pixels (macOS
                // reports angleDelta at twice pixelDelta), otherwise the raw
                // angle (or pixel) delta, as before. Shift turns vertical into
                // horizontal.
                function horizontalDelta(wheel, shifted) {
                    if (wheel.phase !== Qt.NoScrollPhase)
                        return shifted && wheel.pixelDelta.x === 0
                               ? wheel.pixelDelta.y : wheel.pixelDelta.x
                    var dx = wheel.angleDelta.x !== 0 ? wheel.angleDelta.x
                                                      : wheel.pixelDelta.x
                    if (dx === 0 && shifted)
                        dx = wheel.angleDelta.y !== 0 ? wheel.angleDelta.y
                                                      : wheel.pixelDelta.y
                    return dx
                }

                // Vertical motion in pixels: the touchpad's own pixels, or the
                // system's lines per notch (fractions included: a 1/120 notch
                // from a fine-grained touchpad moves a fraction of a pixel). A
                // continuous frame with no pixel delta moves nothing, as on
                // the timeline.
                function verticalDelta(wheel) {
                    if (wheel.pixelDelta.y !== 0)
                        return wheel.pixelDelta.y
                    if (wheel.phase !== Qt.NoScrollPhase)
                        return 0
                    var lineHeight = codeArea.implicitHeight
                                     / Math.max(1, root.lineCount)
                    return wheel.angleDelta.y / 120.0
                           * Qt.styleHints.wheelScrollLines * lineHeight
                }

                function scrollByX(dx) {
                    var maxX = Math.max(0, codeFlick.contentWidth
                                           - codeFlick.width)
                    codeFlick.contentX = Math.max(
                        0, Math.min(maxX, codeFlick.contentX - dx))
                }
                // Whether the code can move at all in dy's direction; the edge
                // is decided here, never by the size of the step.
                function canScrollY(dy) {
                    if (dy === 0)
                        return false
                    return dy < 0 ? codeFlick.contentY < root.maxContentY() - 0.01
                                  : codeFlick.contentY > 0.01
                }
                function scrollByY(dy) {
                    codeFlick.contentY = Math.max(
                        0, Math.min(root.maxContentY(), codeFlick.contentY - dy))
                }

                onWheel: (wheel) => {
                    var now = Date.now()
                    var shifted = (wheel.modifiers & Qt.ShiftModifier) !== 0
                    var ax = wheel.angleDelta.x !== 0 ? wheel.angleDelta.x
                                                      : wheel.pixelDelta.x
                    var ay = wheel.angleDelta.y !== 0 ? wheel.angleDelta.y
                                                      : wheel.pixelDelta.y
                    var horizontal = shifted ? (ax !== 0 || ay !== 0)
                                             : Math.abs(ax) > Math.abs(ay)
                    var vertical = !horizontal && ay !== 0
                    var moving = horizontal || vertical

                    if (wheel.phase !== Qt.NoScrollPhase) {
                        var sinceEnd = lastGestureEndMs > 0
                                       ? now - lastGestureEndMs : Infinity
                        if (wheel.phase === Qt.ScrollBegin) {
                            if (moving && sinceEnd < 100) {
                                // Momentum restarting right after the finger
                                // lifted: the same gesture.
                                gesture = "active"
                                ownX = lastOwnX
                                ownY = lastOwnY
                            } else {
                                gesture = "pending"
                                gestureRested = rested(now)
                            }
                        } else if (gesture === "") {
                            // Momentum after ScrollEnd follows the gesture that
                            // just ended; anything else began off the block
                            // and stays where it began.
                            var momentum = wheel.phase === Qt.ScrollMomentum
                                           && sinceEnd < 1000
                            gesture = "active"
                            ownX = momentum && lastOwnX
                            ownY = momentum && lastOwnY
                        }
                        if (gesture === "pending" && moving) {
                            gesture = "active"
                            if (horizontal) {
                                ownX = root.horizontalOverflow
                                ownY = gestureRested && root.verticalOverflow
                            } else {
                                ownY = gestureRested && root.verticalOverflow
                                       && canScrollY(verticalDelta(wheel))
                                ownX = ownY && root.horizontalOverflow
                            }
                        }
                        var owned = gesture === "active"
                                    && (horizontal ? ownX
                                                   : vertical ? ownY
                                                              : ownX || ownY)
                        if (owned) {
                            if (ownX)
                                scrollByX(horizontalDelta(wheel, shifted))
                            if (ownY && !shifted)
                                scrollByY(verticalDelta(wheel))
                            take(wheel, moving)
                        } else {
                            passOn(wheel, gesture === "active" && moving)
                        }
                        if (wheel.phase === Qt.ScrollEnd) {
                            lastOwnX = gesture === "active" && ownX
                            lastOwnY = gesture === "active" && ownY
                            lastGestureEndMs = now
                            gesture = ""
                        }
                        return
                    }

                    if (horizontal) {
                        if (!root.horizontalOverflow) {
                            // Nothing to scroll sideways: the timeline's.
                            passOn(wheel, true)
                            return
                        }
                        scrollByX(horizontalDelta(wheel, shifted))
                        take(wheel, true)
                        return
                    }
                    if (!vertical || !root.verticalOverflow) {
                        // A short block never takes a vertical notch.
                        wheel.accepted = false
                        return
                    }
                    var dy = verticalDelta(wheel)
                    var ownsTrain = trainLastMs > 0
                                    && now - trainLastMs < root.wheelTrainMs
                    if (!ownsTrain && (!rested(now) || !canScrollY(dy))) {
                        // Not rested, or starting at the edge: the
                        // conversation's.
                        trainLastMs = 0
                        passOn(wheel, true)
                        return
                    }
                    scrollByY(dy)
                    trainLastMs = now
                    take(wheel, true)
                }
            }
        }
    }
}
