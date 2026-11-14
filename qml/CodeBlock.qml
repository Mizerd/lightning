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

    LayoutMirroring.enabled: false
    LayoutMirroring.childrenInherit: true

    // Plain text, already entity-decoded by MessageHtml::segments(); rendered
    // with Text.PlainText so it can never become markup again.
    property string code: ""
    property string language: ""

    readonly property string safeLanguage:
        /^[A-Za-z0-9+#._-]{1,24}$/.test(root.language) ? root.language : ""

    readonly property int lineCount: root.code.length === 0
                                     ? 1 : root.code.split("\n").length

    readonly property real maxBodyHeight: AppTheme.scaled(360)
    readonly property int wheelRestMs: 300
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
    readonly property real horizontalBarSpace: root.horizontalOverflow ? 8 : 0
    readonly property real verticalBarSpace: root.verticalOverflow ? 8 : 0

    function maxContentY() {
        return Math.max(0, codeFlick.contentHeight + codeFlick.bottomMargin
                           - codeFlick.height)
    }

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
    border.color: root.focusWithin ? AppTheme.focusRing : AppTheme.codeBlockBorder

    readonly property bool focusWithin:
        root.activeFocus || codeArea.activeFocus || copyButton.activeFocus

    activeFocusOnTab: true
    Accessible.role: Accessible.Grouping
    Accessible.name: root.safeLanguage.length > 0
        ? qsTr("Code block, %1, %2 lines").arg(root.safeLanguage)
                                          .arg(root.lineCount)
        : qsTr("Code block, %1 lines").arg(root.lineCount)

    function copyCode() {
        clipboardRelay.text = root.code
        clipboardRelay.selectAll()
        clipboardRelay.copy()
        clipboardRelay.text = ""
        root.copied = true
        copiedTimer.restart()
    }

    property bool copied: false

    property real toolbarClearance: 0

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

                ToolTip {
                    objectName: "codeBlockCopyTip"
                    visible: copyButton.hovered
                    delay: 500
                    text: qsTr("Copy code")
                }
            }
        }

        Item {
            id: bodyArea
            objectName: "codeBlockBody"
            width: parent.width
            height: Math.min(codeArea.implicitHeight, root.maxBodyHeight)
                    + root.horizontalBarSpace
            clip: true

            Text {
                id: gutterText
                objectName: "codeBlockGutter"
                x: 0
                y: -codeFlick.contentY
                width: implicitWidth
                horizontalAlignment: Text.AlignRight
                textFormat: Text.PlainText
                text: {
                    var lines = []
                    for (var i = 1; i <= root.lineCount; ++i)
                        lines.push(String(i))
                    return lines.join("\n")
                }
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
                contentWidth: codeArea.implicitWidth + root.verticalBarSpace
                contentHeight: codeArea.implicitHeight
                bottomMargin: root.horizontalBarSpace

                interactive: false

                TextEdit {
                    id: codeArea
                    objectName: "codeBlockText"
                    text: root.code
                    textFormat: Text.PlainText
                    readOnly: true
                    selectByMouse: true
                    selectByKeyboard: true
                    wrapMode: Text.NoWrap
                    activeFocusOnTab: false
                    padding: 0
                    textMargin: 0
                    color: AppTheme.codeText
                    selectionColor: AppTheme.accent
                    selectedTextColor: AppTheme.accentText
                    font.family: AppTheme.monoFont
                    font.pixelSize: AppTheme.scaled(AppTheme.fontMono)
                    Accessible.role: Accessible.EditableText
                    Accessible.name: qsTr("Code")

                    onCursorPositionChanged: Qt.callLater(root.ensureCaretVisible)

                    PointHandler {
                        id: dragPoint
                        objectName: "codeBlockDragPoint"
                        acceptedButtons: Qt.LeftButton
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    }

                    ParagraphDirection {
                        document: codeArea.textDocument
                        leftToRight: true
                    }
                }

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

            MouseArea {
                id: wheelRouter
                objectName: "codeBlockWheelRouter"
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                // Without this a MouseArea shows the arrow over the code's I-beam.
                cursorShape: undefined

                property real restSinceMs: -1
                property real trainLastMs: 0
                property string gesture: ""
                property bool gestureRested: false
                property bool ownX: false
                property bool ownY: false
                property bool lastOwnX: false
                property bool lastOwnY: false
                property real lastGestureEndMs: 0

                function rested(now) {
                    if (restSinceMs < 0) {
                        restSinceMs = now
                        return false
                    }
                    return now - restSinceMs >= root.wheelRestMs
                }

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
                                gesture = "active"
                                ownX = lastOwnX
                                ownY = lastOwnY
                            } else {
                                gesture = "pending"
                                gestureRested = rested(now)
                            }
                        } else if (gesture === "") {
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
                            passOn(wheel, true)
                            return
                        }
                        scrollByX(horizontalDelta(wheel, shifted))
                        take(wheel, true)
                        return
                    }
                    if (!vertical || !root.verticalOverflow) {
                        wheel.accepted = false
                        return
                    }
                    var dy = verticalDelta(wheel)
                    var ownsTrain = trainLastMs > 0
                                    && now - trainLastMs < root.wheelTrainMs
                    if (!ownsTrain && (!rested(now) || !canScrollY(dy))) {
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
