import QtQuick
import QtQuick.Controls
import QtTest
import MatrixClient

TestCase {
    id: test
    name: "AttachmentDownloadNotice"
    width: 1100
    height: 650
    visible: true
    when: windowShown

    // Pin theme, font scale, viewport, attachment size and initial scroll.
    Component.onCompleted: {
        AppTheme.mode = 2
        AppTheme.textScale = 1
    }
    property real paneWidth: 1100
    property bool ownBubble: false
    property real captionWidth: 340
    property int reactionCount: 0
    property var dismissed: []
    property var app: ({ downloads: {
        dismiss: function(id) { test.dismissed = test.dismissed.concat([id]) },
        cancel: function(id) {},
        showInFolder: function(id) {}, retry: function(id) {}
    } })
    function item(id, key, state) {
        return { id: id, mediaKey: key, fileName: "original-name.exe",
                 folderName: "Downloads", state: state || "done", risky: true,
                 canOpen: false }
    }

    Flickable {
        id: timeline
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: 2200
        Item {
            id: row
            y: 180
            width: test.paneWidth
            // The row's height as the host computes it: the media box's
            // implicit height, which is the card's alone, plus fixed padding.
            // The notice takes no part in it.
            implicitHeight: box.implicitHeight + 226
            height: implicitHeight
            Item {
                id: box
                implicitHeight: fileCard.implicitHeight
                Rectangle {
                    id: fileCard
                    x: test.ownBubble ? row.width - width - 16 : 48
                    width: 340
                    height: 64
                    implicitHeight: height
                    color: AppTheme.embedSurface
                }
                Rectangle {
                    id: caption
                    x: test.ownBubble ? fileCard.x + fileCard.width - width : fileCard.x
                    y: fileCard.height + 8
                    width: test.captionWidth
                    height: 100
                    color: AppTheme.embedSurface
                }
            }
            Item {
                id: toolbar
                y: -3
                width: row.width
                height: 32
            }
            Item {
                id: receipts
                x: row.width - width
                y: row.height - height
                width: 70
                height: 22
            }
            Flow {
                id: reactions
                x: fileCard.x
                y: fileCard.height + 8
                width: row.width - x - receipts.width - AppTheme.spacing8
                visible: test.reactionCount > 0
                spacing: AppTheme.spacing4
                Repeater {
                    model: test.reactionCount
                    Rectangle {
                        width: 90
                        height: 24
                        color: AppTheme.reactionBackground
                    }
                }
            }
            Item {
                id: overlayParent
                x: 48
                y: 72
                Rectangle {
                    id: overlay
                    visible: false
                    width: 140
                    height: 30
                    color: AppTheme.embedSurface
                }
            }
            AttachmentDownloadNotice {
                id: notice
                attachment: fileCard
                row: row
                toolbar: toolbar
                receipts: receipts
                contentItems: [fileCard, caption]
                collisionItems: {
                    var items = [overlay]
                    for (var i = 0; i < reactions.children.length; ++i)
                        items.push(reactions.children[i])
                    return items
                }
                ownBubble: test.ownBubble
                mediaKey: "k1"
                surface: "main"
                eligible: row.y + fileCard.height > timeline.contentY
                          && row.y < timeline.contentY + timeline.height
                dismissAfterMs: 150
                items: test.current
            }
        }
    }
    // The fallback card in the same scene, fed the same list.
    PaneDownloadsCard {
        id: pane
        parent: test
        safeArea: Qt.rect(0, 0, test.width, test.height)
        allItems: test.all
        dismissAfterMs: 150
    }
    property var all: []
    // What the row's attachmentDownloads would be: items for its own key.
    readonly property var current: all.filter(function(i) { return i.mediaKey === "k1" })

    function init() {
        timeline.contentY = 0
        paneWidth = 1100
        ownBubble = false
        captionWidth = 340
        reactionCount = 0
        overlay.visible = false
        overlayParent.x = 48
        overlayParent.y = 72
        box.y = 0
        toolbar.y = -3
        receipts.x = Qt.binding(function() { return row.width - receipts.width })
        receipts.y = Qt.binding(function() { return row.height - receipts.height })
        notice.surface = "main"
        notice.eligible = Qt.binding(function() {
            return row.y + fileCard.height > timeline.contentY
                   && row.y < timeline.contentY + timeline.height })
        dismissed = []
        all = []
        DownloadNotices.reset()
        wait(0)
    }
    function cleanup() { all = [] }

    function test_besideAndFollowsScroll() {
        all = [item(1, "k1")]
        tryVerify(function() { return notice.visible })
        var card = fileCard.mapToItem(test, 0, 0)
        var bubble = notice.mapToItem(test, 0, 0)
        compare(bubble.x, card.x + fileCard.width + AppTheme.spacing12)
        verify(bubble.y >= card.y && bubble.y < card.y + fileCard.height)
        // The toolbar occupies the row's first 29px, 4px of margin below it.
        // Assert geometry, not visibility or z order.
        verify(bubble.y >= row.mapToItem(test, 0, 33).y)
        timeline.contentY = 85
        wait(0)
        var movedCard = fileCard.mapToItem(test, 0, 0)
        var movedBubble = notice.mapToItem(test, 0, 0)
        compare(movedBubble.y, bubble.y - 85)
        compare(movedBubble.y - movedCard.y, bubble.y - card.y)
        compare(movedBubble.x - movedCard.x, bubble.x - card.x)
    }

    // L1: the toolbar band is the ROW's, so a box that sits well below the row
    // top is not offset a further 33px.
    function test_theToolbarBandIsMeasuredFromTheRowNotTheBox() {
        all = [item(1, "k1")]
        tryVerify(function() { return notice.visible })
        // The box is at the row's top here: the band forces 33.
        compare(notice.y, 33)
        box.y = 120
        wait(0)
        // Already below the band: the notice rides at the card's top.
        compare(notice.y, fileCard.mapToItem(row, 0, 0).y)
        box.y = 0
    }

    function assertInsideAndClear() {
        verify(notice.x >= AppTheme.spacing4)
        verify(notice.y >= AppTheme.spacing4)
        verify(notice.x + notice.width + AppTheme.spacing4 <= row.width)
        verify(notice.y + notice.height + AppTheme.spacing4 <= row.height)
        verify(notice.y >= toolbar.y + toolbar.height + AppTheme.spacing4)
        verify(notice.x + notice.width <= receipts.x
               || notice.x >= receipts.x + receipts.width
               || notice.y + notice.height <= receipts.y
               || notice.y >= receipts.y + receipts.height)
        for (var item of notice.collisionItems) {
            if (!item.visible || item.width <= 0 || item.height <= 0)
                continue
            var r = item.mapToItem(row, 0, 0, item.width, item.height)
            verify(notice.x + notice.width <= r.x || notice.x >= r.x + r.width
                   || notice.y + notice.height <= r.y || notice.y >= r.y + r.height)
        }
    }
    function test_correctSideForIncomingAndOwnBubbles() {
        for (var own of [false, true]) {
            ownBubble = own
            all = [item(1, "k1", "saving")]
            tryVerify(function() { return notice.visible })
            var card = fileCard.mapToItem(row, 0, 0)
            if (own)
                compare(notice.x + notice.width + AppTheme.spacing12, card.x)
            else
                compare(notice.x, card.x + fileCard.width + AppTheme.spacing12)
            assertInsideAndClear()
            compare(placeCount(1), 1)
        }
    }
    function test_widthThresholdIncludesMargins_data() {
        return [{tag: "incoming", own: false, threshold: 704},
                {tag: "own", own: true, threshold: 672}]
    }
    function test_widthThresholdIncludesMargins(data) {
        ownBubble = data.own
        paneWidth = data.threshold
        all = [item(1, "k1", "saving")]
        tryVerify(function() { return notice.visible })
        assertInsideAndClear()
        paneWidth -= 1
        tryVerify(function() { return !notice.visible })
        compare(pane.items.length, 1)
        compare(placeCount(1), 1)
    }
    function test_narrowPaneFallsBackWithoutOverlayingMessages() {
        for (var own of [false, true]) {
            ownBubble = own
            paneWidth = 520
            all = [item(1, "k1", "failed")]
            tryVerify(function() { return pane.items.length === 1 })
            verify(!notice.visible)
            compare(placeCount(1), 1)
        }
    }
    function test_wideCaptionLeavesNoFreeSide() {
        for (var own of [false, true]) {
            ownBubble = own
            all = [item(1, "k1", "saving")]
            tryVerify(function() { return notice.visible })
            captionWidth = 900
            tryVerify(function() { return !notice.visible })
            compare(pane.items.length, 1)
            compare(placeCount(1), 1)
            captionWidth = 340
            tryVerify(function() { return notice.visible })
            compare(placeCount(1), 1)
        }
    }
    function test_toolbarChangesCannotCauseAnOverlap() {
        all = [item(1, "k1", "saving")]
        tryVerify(function() { return notice.visible })
        toolbar.y = 100
        wait(0)
        assertInsideAndClear()
        toolbar.y = row.height - 32
        tryVerify(function() { return !notice.visible })
        compare(pane.items.length, 1)
        compare(placeCount(1), 1)
    }
    function test_noticeCannotExtendBelowTheRow() {
        all = [item(1, "k1", "saving")]
        tryVerify(function() { return notice.visible })
        box.y = row.height - 20
        tryVerify(function() { return !notice.visible })
        compare(pane.items.length, 1)
        compare(placeCount(1), 1)
    }
    function test_receiptCollisionHandsTheNoticeToThePane() {
        all = [item(1, "k1", "saving")]
        tryVerify(function() { return notice.visible })
        receipts.x = notice.x + 20
        receipts.y = notice.y + 20
        tryVerify(function() { return !notice.visible })
        compare(pane.items.length, 1)
        compare(placeCount(1), 1)
        receipts.x = row.width - receipts.width
        receipts.y = row.height - receipts.height
        tryVerify(function() { return notice.visible })
        assertInsideAndClear()
    }
    function test_wrappingReactionsReleaseTheClaimWithoutChangingGeometry() {
        var before = [row.width, row.height, row.implicitHeight,
                      fileCard.x, fileCard.y, fileCard.width, fileCard.height,
                      caption.x, caption.y, caption.width, caption.height]
        all = [item(1, "k1", "saving")]
        tryVerify(function() { return notice.visible })
        // A short run leaves the free side usable despite the wide Flow.
        reactionCount = 2
        tryCompare(reactions, "height", 24)
        verify(notice.visible)
        assertInsideAndClear()
        compare(placeCount(1), 1)
        reactionCount = 60
        tryVerify(function() { return reactions.height > 3 * 24 })
        tryVerify(function() { return !notice.visible })
        compare(pane.items.length, 1)
        compare(placeCount(1), 1)
        compare([row.width, row.height, row.implicitHeight,
                 fileCard.x, fileCard.y, fileCard.width, fileCard.height,
                 caption.x, caption.y, caption.width, caption.height], before)
        reactionCount = 2
        tryVerify(function() { return notice.visible })
        assertInsideAndClear()
        compare(placeCount(1), 1)
        reactionCount = 0
        tryVerify(function() { return notice.visible })
        compare(placeCount(1), 1)
    }
    function test_collisionTracksIntermediateAncestorsAndVisibility() {
        all = [item(1, "k1", "saving")]
        tryVerify(function() { return notice.visible })
        overlay.visible = true
        verify(notice.visible)
        // The leaf's own x/y never change. Mapping must track its parent.
        overlayParent.x = notice.x + 20
        tryVerify(function() { return !notice.visible })
        compare(placeCount(1), 1)
        overlay.visible = false
        tryVerify(function() { return notice.visible })
        compare(placeCount(1), 1)
        overlay.visible = true
        tryVerify(function() { return !notice.visible })
        overlayParent.y = row.height - 30
        tryVerify(function() { return notice.visible })
        compare(placeCount(1), 1)
    }
    function test_aTallerOutcomeOrMultipleDownloadsCanStopFitting() {
        all = [item(1, "k1", "saving")]
        tryVerify(function() { return notice.visible })
        all = [item(1, "k1", "failed"), item(2, "k1", "failed"),
               item(3, "k1", "failed"), item(4, "k1", "failed"),
               item(5, "k1", "failed")]
        tryVerify(function() { return !notice.visible })
        compare(pane.items.length, 5)
        for (var id of [1, 2, 3, 4, 5]) compare(placeCount(id), 1)
    }

    // M3: the row's height is the same before, during and after a notice.
    function test_theRowDoesNotChangeHeightWhenTheNoticeComesAndGoes() {
        for (var paneSize of [1100, 520]) {
            paneWidth = paneSize
            all = []
            wait(0)
            var captionBefore = caption.width
            var widthBefore = row.width
            var before = row.height
            var implicitBefore = row.implicitHeight
            var boxBefore = box.implicitHeight
            verify(!notice.visible)
            all = [item(1, "k1", "saving")]
            tryVerify(function() { return paneSize === 1100 ? notice.visible : pane.items.length === 1 })
            compare(caption.width, captionBefore)
            compare(row.width, widthBefore)
            compare(row.height, before)
            compare(row.implicitHeight, implicitBefore)
            compare(box.implicitHeight, boxBefore)
            all = [item(1, "k1", "done")]
            wait(10)
            compare(row.height, before)
            all = []
            tryVerify(function() { return !notice.visible })
            compare(row.height, before)
            compare(row.implicitHeight, implicitBefore)
            compare(caption.width, captionBefore)
            compare(row.width, widthBefore)
        }
    }

    // M1: an item no row claims is on the pane card, an item a row claims is
    // on the row's notice, never on both and never on neither.
    function placeCount(id) {
        var n = 0
        for (var i = 0; i < pane.items.length; ++i)
            if (pane.items[i].id === id) ++n
        if (notice.visible)
            for (var j = 0; j < notice.items.length; ++j)
                if (notice.items[j].id === id) ++n
        return n
    }
    function test_anItemWithNoRowFallsBackToThePaneCard() {
        // The image viewer, media history and a non-primary gallery item all
        // save under a key no row's primary media has.
        all = [item(1, "viewer-only-key"), item(2, "gallery-sub-item")]
        tryVerify(function() { return pane.visible })
        compare(pane.items.length, 2)
        compare(placeCount(1), 1)
        compare(placeCount(2), 1)
        verify(!notice.visible)
        // The risky-file warning is on that card.
        verify(findChild(pane, "downloadRiskNotice") !== null)
    }
    function test_aRowsItemIsShownByTheRowAndNotByThePane() {
        all = [item(1, "k1"), item(2, "viewer-only-key")]
        tryVerify(function() { return notice.visible })
        compare(placeCount(1), 1)
        compare(placeCount(2), 1)
        compare(pane.items.length, 1)
        compare(pane.items[0].id, 2)
    }
    function test_aScrolledAwayRowHandsItsItemToThePaneCard() {
        all = [item(1, "k1", "saving")]
        tryVerify(function() { return notice.visible })
        compare(pane.items.length, 0)
        timeline.contentY = 400
        tryVerify(function() { return !notice.visible })
        tryCompare(pane.items, "length", 1)
        compare(placeCount(1), 1)
        timeline.contentY = 0
        tryVerify(function() { return notice.visible })
        tryCompare(pane.items, "length", 0)
        compare(placeCount(1), 1)
    }
    function test_aCollapsedRowDoesNotClaim() {
        all = [item(1, "k1")]
        tryVerify(function() { return notice.visible })
        notice.eligible = false // collapsed embed: mediaBox hidden
        tryVerify(function() { return !notice.visible })
        tryCompare(pane.items, "length", 1)
        compare(placeCount(1), 1)
    }

    // L3: the same event in two surfaces shows its notice once, where the
    // click happened.
    Component {
        id: twinComp
        AttachmentDownloadNotice {
            mediaKey: "k1"
            eligible: true
        }
    }
    function test_theSameEventInTwoSurfacesIsShownOnceWhereClickedAndNotTwice() {
        var twin = twinComp.createObject(row, { attachment: fileCard,
                            row: row, toolbar: toolbar, receipts: receipts,
                            contentItems: [fileCard, caption], surface: "thread" })
        verify(twin)
        twin.items = Qt.binding(function() { return test.current })
        all = [item(1, "k1")]
        // No recorded origin (viewer, history): main wins, thread yields.
        tryVerify(function() { return notice.visible })
        compare(twin.visible, false)
        // Clicked in the thread panel: the thread's notice, only.
        DownloadNotices.begin("k1", "thread")
        tryVerify(function() { return twin.visible })
        compare(notice.visible, false)
        compare(pane.items.length, 0)
        // Clicked in the main timeline.
        DownloadNotices.begin("k1", "main")
        tryVerify(function() { return notice.visible })
        compare(twin.visible, false)
        // The thread panel closing hands nothing back and leaves main's.
        twin.destroy()
        wait(0)
        compare(notice.visible, true)
        compare(pane.items.length, 0)
    }
    function test_anOriginSurfaceWithoutSpaceYieldsToAnotherRowOrThePane() {
        var twin = twinComp.createObject(row, { attachment: fileCard,
                            row: row, toolbar: toolbar, receipts: receipts,
                            contentItems: [fileCard, caption], surface: "thread" })
        verify(twin)
        twin.items = Qt.binding(function() { return test.current })
        all = [item(1, "k1", "saving")]
        DownloadNotices.begin("k1", "thread")
        tryVerify(function() { return twin.visible })
        compare(notice.visible, false)
        twin.width = 1000 // the clicked surface cannot accommodate its notice
        tryVerify(function() { return notice.visible })
        compare(twin.visible, false)
        compare(placeCount(1), 1)
        paneWidth = 520
        tryVerify(function() { return pane.items.length === 1 })
        compare(notice.visible, false)
        compare(twin.visible, false)
        compare(placeCount(1), 1)
        paneWidth = 1100
        twin.width = 300
        tryVerify(function() { return twin.visible })
        compare(notice.visible, false)
        compare(pane.items.length, 0)
        twin.destroy()
        wait(0)
    }
    function test_aDestroyedNoticeReleasesItsClaim() {
        var twin = twinComp.createObject(row, { attachment: fileCard,
                            row: row, toolbar: toolbar, receipts: receipts,
                            contentItems: [fileCard, caption], surface: "thread" })
        twin.items = Qt.binding(function() { return test.current })
        all = [item(1, "k1")]
        DownloadNotices.begin("k1", "thread")
        tryVerify(function() { return twin.visible })
        twin.destroy()
        wait(0)
        // Main's notice takes it over; the pane never showed it.
        tryVerify(function() { return notice.visible })
        compare(pane.items.length, 0)
    }

    // M2: the dismiss timer counts only while the notice is visible.
    function test_theDismissTimerOnlyRunsWhileTheNoticeIsVisible() {
        notice.dismissAfterMs = 400
        pane.dismissAfterMs = 400
        all = [item(1, "k1")]
        tryVerify(function() { return notice.visible })
        // Scrolled away at once: the row cannot show it, the pane card does,
        // and ONLY the visible card's timer counts. Nothing is dismissed while
        // nobody could see the item; there is no card at all to count here, so
        // hide the pane card too (the pane is not on screen).
        pane.parent = null
        timeline.contentY = 400
        tryVerify(function() { return !notice.visible })
        wait(1300) // three intervals
        compare(dismissed.length, 0)
        // Seen again: shown once for a full interval, then dismissed.
        timeline.contentY = 0
        tryVerify(function() { return notice.visible })
        compare(dismissed.length, 0)
        tryVerify(function() { return dismissed.length === 1 }, 3000)
        compare(dismissed[0], 1)
        pane.parent = test
        notice.dismissAfterMs = 150
        pane.dismissAfterMs = 150
    }
}
