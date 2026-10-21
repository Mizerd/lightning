import QtQuick
import QtQuick.Controls
import QtTest
import MatrixClient

// The media browser against a stub model: keyboard reach (one Tab stop per
// view, arrows, Return, Menu), middle-click autoscroll, a fresh scan when the
// panel is shown again after a while (not on a quick section switch).
// Not covered: the narrow-panel date row (C34-F07, seen once at 1100 px, not
// reproduced, so not changed).
TestCase {
    id: test
    name: "MediaBrowser"
    width: 300
    height: 700
    visible: true
    when: windowShown

    QtObject {
        id: bridge
        property bool supported: true
        signal mediaCached(string cacheKey)
        signal mediaFetchFailed(string cacheKey, string category)
        signal mediaRetryable(string cacheKey)
        function mediaSource(key, kind) { return "" }
        function mxcImageSource(uri, edge) { return "" }
    }
    property var app: ({ mediaBridge: bridge })

    // A ListModel that also answers the browser's model API.
    Component {
        id: modelComponent
        ListModel {
            id: m
            property bool available: true
            property string category: "media"
            property string roomId: ""
            property int loadedCount: count
            property int shownCount: count
            property bool complete: false
            property bool loading: false
            property bool walking: false
            property int scannedTotal: count
            property int undecryptableCount: 0
            property string lastError: ""
            property var knownSenders: []
            property string query: ""
            property string senderFilter: ""
            property var fromDate: ""
            property var toDate: ""
            property int reloads: 0
            property int loadMores: 0
            function loadMore() { loadMores++ }
            function reload() { reloads++ }
            function loadAll() {}
            function cancelWalk() {}
            function fillViewport() {}
            function setDateRange(a, b) { return true }
            function entryAt(row) {
                return row < 0 || row >= count ? ({})
                    : { eventId: get(row).eventId, kind: get(row).kind }
            }
            function imageIndexForRow(row) { return row }
            function imageEntries() { return [] }
        }
    }
    Component {
        id: browserComponent
        MediaBrowser {
            width: 300
            height: 700
        }
    }

    function fillModel(m, n, kind) {
        for (var i = 0; i < n; ++i)
            m.append({
                kind: kind, body: "file" + i + ".dat", filename: "file" + i + ".dat",
                mimetype: "application/octet-stream", size: 1000 + i,
                durationMs: 0, mediaWidth: 100, mediaHeight: 100,
                mxc: "mxc://x/" + i, thumbnailMxc: "", mediaKey: "",
                encrypted: false, url: "", host: "", sender: "@a:x",
                timestampMs: 1700000000000 + i * 1000, eventId: "$e" + i,
                dateGroup: "Today"
            })
    }
    function make(kind, n, category) {
        var m = createTemporaryObject(modelComponent, test)
        fillModel(m, n, kind)
        m.category = category || "media"
        var b = createTemporaryObject(browserComponent, test, { model: m })
        verify(b)
        return b
    }
    function find(item, name) {
        for (var i = 0; i < item.children.length; ++i) {
            var c = item.children[i]
            if (c.objectName === name) return c
            var r = find(c, name)
            if (r) return r
        }
        return null
    }
    function findAll(item, pred, out) {
        for (var i = 0; i < item.children.length; ++i) {
            if (pred(item.children[i])) out.push(item.children[i])
            findAll(item.children[i], pred, out)
        }
        return out
    }

    function test_gridIsOneTabStopAndKeysOpenTheCurrentTile() {
        var b = make("image", 6)
        var grid = findAll(b, function(i) { return i.objectName === "" && i.cellWidth !== undefined }, [])[0]
        verify(grid, "grid")
        verify(grid.activeFocusOnTab, "the grid must take Tab focus")
        verify(grid.keyNavigationEnabled)
        var opened = []
        b.openImagesRequested.connect(function(entries, index) { opened.push(index) })
        grid.forceActiveFocus()
        verify(grid.activeFocus)
        keyClick(Qt.Key_Right)
        compare(grid.currentIndex, 1)
        keyClick(Qt.Key_End)
        compare(grid.currentIndex, 5)
        keyClick(Qt.Key_Home)
        compare(grid.currentIndex, 0)
        keyClick(Qt.Key_Right)
        keyClick(Qt.Key_Return)
        compare(opened.length, 1)
        compare(opened[0], 1)
        var ring = find(grid.currentItem, "focusRing")
        verify(ring && ring.visible, "the current tile shows a focus ring")
    }

    function test_listRowsTakeKeyboardAndShowARing() {
        var b = make("file", 30, "file")
        b.category = "file"
        var list = findAll(b, function(i) { return i.section !== undefined && i.spacing !== undefined }, [])[0]
        verify(list, "list")
        verify(list.visible)
        verify(list.activeFocusOnTab)
        var jumps = []
        b.jumpToEventRequested.connect(function(id) { jumps.push(id) })
        list.forceActiveFocus()
        keyClick(Qt.Key_Down)
        compare(list.currentIndex, 1)
        keyClick(Qt.Key_End)
        compare(list.currentIndex, 29)
        keyClick(Qt.Key_Home)
        compare(list.currentIndex, 0)
        keyClick(Qt.Key_PageDown)
        verify(list.currentIndex > 1, "Page Down must move by a page")
        keyClick(Qt.Key_Home)
        keyClick(Qt.Key_Down)
        keyClick(Qt.Key_Return)
        compare(jumps.length, 1)
        compare(jumps[0], "$e1")
        var ring = find(list.currentItem, "focusRing")
        verify(ring && ring.visible)
    }

    function test_middleClickScrollerIsPresentOverTheResults() {
        var b = make("image", 3)
        var s = find(b, "mediaBrowserMiddleClickScroller")
        verify(s, "MiddleClickScroller must be instantiated")
        verify(s.view !== null)
    }

    function test_showingAgainAfterAWhileStartsANewScan() {
        var b = make("image", 3)
        var m = b.model
        b.staleAfterMs = 0
        b.visible = false
        b.visible = true
        compare(m.reloads, 1, "a browser re-shown after the stale bound must reload from the live edge")
        // An empty, unfinished model asks for its first page instead.
        var m2 = createTemporaryObject(modelComponent, test)
        var b2 = createTemporaryObject(browserComponent, test, { model: m2 })
        b2.staleAfterMs = 0
        b2.visible = false
        b2.visible = true
        compare(m2.reloads, 0)
        verify(m2.loadMores >= 1)
    }

    function test_aQuickSectionSwitchKeepsThePlace() {
        var b = make("image", 3)
        var m = b.model
        b.staleAfterMs = 600000
        b.visible = false
        b.visible = true
        compare(m.reloads, 0, "a short hide must keep the loaded pages and scroll position")
        b.visible = false
        b.roomId = "!other:example.org"
        b.visible = true
        compare(m.reloads, 1, "a different room at re-show must rescan")
    }
}
