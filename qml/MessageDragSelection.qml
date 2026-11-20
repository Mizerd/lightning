import QtQuick

// A mouse drag that starts in a message's text and leaves its row selects
// across rows. The pressed TextEdit keeps its own grab and selection; this
// overlay only watches the point passively and drives the other rows.
Item {
    id: root
    property var model: null
    // function(scenePoint) -> { item, row } for the row under the point, where
    // row is a model row (older rows are lower), or null.
    property var rowAt: null
    // function(row) -> the delegate for a model row, or null.
    property var itemForRow: null
    // function(dy, starting): move the content by dy physical pixels.
    property var scrollBy: null
    // function(): moves keyboard focus to the view.
    property var takeFocus: null

    property var ids: []
    readonly property bool active: ids.length > 0
    readonly property bool dragging: pointer.active && originStable !== ""

    property string originStable: ""
    property string currentStable: ""
    property Item originText: null
    // In originText's coordinates, so it follows the start row when the content scrolls.
    property point originPoint
    property point lastPoint
    property var touched: []
    readonly property real edge: 40

    function clear() {
        for (var i = 0; i < touched.length; ++i)
            if (touched[i])
                touched[i].applyDragSelection("none", null, null)
        touched = []
        ids = []
        currentStable = ""
    }

    function begin(sp) {
        clear()
        originStable = ""
        var hit = rowAt ? rowAt(sp) : null
        var text = hit && hit.item.dragTextItemAt ? hit.item.dragTextItemAt(sp) : null
        if (!text)
            return
        originText = text
        originPoint = text.mapFromItem(null, sp.x, sp.y)
        lastPoint = sp
        originStable = model.stableIdAt(hit.row)
    }

    function update(sp) {
        if (originStable === "")
            return
        var local = mapFromItem(null, sp.x, sp.y)
        var y = Math.max(1, Math.min(height - 1, local.y))
        lastPoint = mapToItem(null, local.x, y)
        autoScroll.depth = local.y < edge ? local.y - edge
                         : local.y > height - edge ? local.y - (height - edge) : 0
        refresh()
    }

    function refresh() {
        var origin = model.rowForStableId(originStable)
        var hit = rowAt(lastPoint)
        if (origin < 0) {
            clear()
            originStable = ""
            return
        }
        if (!hit)
            return
        var originItem = itemForRow(origin)
        var originScene = originText ? originText.mapToItem(null, originPoint.x, originPoint.y) : null
        if (hit.row === origin) {
            for (var k = 0; k < touched.length; ++k)
                if (touched[k])
                    touched[k].applyDragSelection("none", null, originText)
            touched = originItem ? [originItem] : []
            ids = []
            currentStable = ""
            return
        }
        var lo = Math.min(origin, hit.row)
        var hi = Math.max(origin, hit.row)
        var next = []
        for (var r = lo; r <= hi; ++r) {
            var item = r === hit.row ? hit.item : itemForRow(r)
            if (!item || !item.applyDragSelection)
                continue
            item.applyDragSelection(r === lo ? "tail" : r === hi ? "head" : "all",
                                    r === origin ? originScene
                                    : r === hit.row ? lastPoint : null,
                                    null)
            next.push(item)
        }
        for (var i = 0; i < touched.length; ++i)
            if (touched[i] && next.indexOf(touched[i]) < 0)
                touched[i].applyDragSelection("none", null, null)
        touched = next
        currentStable = model.stableIdAt(hit.row)
        ids = model.messageIdsInRows(lo, hi)
    }

    function finish() {
        autoScroll.depth = 0
        if (!active) {
            originStable = ""
            return
        }
        // Takes focus from the pressed TextEdit, which drops its selection on
        // focus loss; the refresh puts it back as a programmatic selection.
        if (takeFocus)
            takeFocus()
        refresh()
    }

    // The ends copy only their selected part; an end row scrolled out of the
    // view copies whole.
    function text(timeFormat) {
        if (!active)
            return ""
        var a = model.rowForStableId(originStable)
        var b = model.rowForStableId(currentStable)
        var list = ids.slice()
        var tail = ""
        var head = ""
        if (a >= 0 && b >= 0) {
            var oldest = itemForRow(Math.min(a, b))
            var newest = itemForRow(Math.max(a, b))
            if (model.eventIdAt(Math.min(a, b)) === list[0] && oldest) {
                tail = oldest.dragSelectedText()
                if (tail === "")
                    list.shift()
            }
            if (list.length > 0 && newest
                    && model.eventIdAt(Math.max(a, b)) === list[list.length - 1]) {
                head = newest.dragSelectedText()
                if (head === "")
                    list.pop()
            }
        }
        return model.plainTextForMessages(list, timeFormat, tail, head)
    }

    function copy(timeFormat) {
        clipboardHelper.text = text(timeFormat)
        clipboardHelper.selectAll()
        clipboardHelper.copy()
        clipboardHelper.text = ""
    }
    TextEdit {
        id: clipboardHelper
        visible: false
    }

    Connections {
        target: root.model
        function onModelReset() {
            root.clear()
            root.originStable = ""
        }
    }

    Timer {
        id: autoScroll
        property real depth: 0
        property bool started: false
        interval: 16
        repeat: true
        running: root.dragging && depth !== 0
        onRunningChanged: started = false
        onTriggered: {
            if (!root.scrollBy)
                return
            root.scrollBy(Math.max(-1, Math.min(1, depth / root.edge)) * 24, !started)
            started = true
            root.refresh()
        }
    }

    // Every left-button release over the view, a drag or a plain click.
    signal released(int modifiers)
    property int pressModifiers: 0

    PointHandler {
        id: pointer
        acceptedButtons: Qt.LeftButton
        onActiveChanged: {
            if (pointer.active) {
                root.pressModifiers = pointer.point.modifiers
                root.begin(pointer.point.scenePressPosition)
            } else {
                root.finish()
                root.released(root.pressModifiers)
            }
        }
        onPointChanged: if (pointer.active) root.update(pointer.point.scenePosition)
    }
}
