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
    // 0: a plain file-card row, the normal case (measured on the real
    // delegate: card 340x54, row 57 in Modern). >0: a caption under the card.
    property real captionHeight: 0
    property int reactionCount: 0
    property var dismissed: []
    property var app: ({ downloads: {
        dismiss: function(id) { test.dismissed = test.dismissed.concat([id]) },
        cancel: function(id) {},
        showInFolder: function(id) {}, retry: function(id) {}
    } })
    function item(id, key, state, risky, message) {
        state = state || "done"
        return { id: id, mediaKey: key,
                 fileName: risky ? "original-name.exe" : "original-name.pdf",
                 folderName: "Downloads", state: state, risky: risky === true,
                 canOpen: state === "done" && risky !== true,
                 message: message || "" }
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
            // implicit height (the card, plus a caption when there is one)
            // and the delegate's 3px of padding. The notice takes no part.
            implicitHeight: box.implicitHeight + 3
            height: implicitHeight
            Item {
                id: box
                implicitHeight: fileCard.implicitHeight
                                + (caption.visible ? caption.height + 8 : 0)
                Rectangle {
                    id: fileCard
                    x: test.ownBubble ? row.width - width - 16 : 48
                    width: 340
                    height: 54
                    implicitHeight: height
                    color: AppTheme.embedSurface
                }
                Rectangle {
                    id: caption
                    x: test.ownBubble ? fileCard.x + fileCard.width - width : fileCard.x
                    y: fileCard.height + 8
                    visible: test.captionHeight > 0
                    width: test.captionWidth
                    height: test.captionHeight
                    color: AppTheme.embedSurface
                }
            }
            // The hover toolbar: 122x32 at the row's top-right, as measured
            // on the real delegate (the scrollbar margin is 12).
            Item {
                id: toolbar
                x: row.width - width - 12
                y: -3
                width: 122
                height: 32
            }
            Item {
                id: receipts
                x: row.width - width
                y: row.height - height
                width: 56
                height: 18
            }
            Flow {
                id: reactions
                x: fileCard.x
                // Under the content, as in the delegate.
                y: fileCard.height + 8 + (caption.visible ? caption.height + 8 : 0)
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
            // A card with no size: a notice pointed at it cannot place itself.
            Item { id: noCard }
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
        captionHeight = 0
        reactionCount = 0
        overlay.visible = false
        overlayParent.x = 48
        overlayParent.y = 72
        box.y = 0
        toolbar.y = -3
        toolbar.x = Qt.binding(function() { return row.width - toolbar.width - 12 })
        fileCard.height = 54
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
        // Beside the card: the two bands meet.
        verify(bubble.y < card.y + fileCard.height
               && bubble.y + notice.height > card.y)
        assertInsideAndClear()
        timeline.contentY = 85
        wait(0)
        var movedCard = fileCard.mapToItem(test, 0, 0)
        var movedBubble = notice.mapToItem(test, 0, 0)
        compare(movedBubble.y, bubble.y - 85)
        compare(movedBubble.y - movedCard.y, bubble.y - card.y)
        compare(movedBubble.x - movedCard.x, bubble.x - card.x)
    }

    // C3b (live round 1, 2026-10-18): a plain file-card row is 57px tall and
    // the old 300x84 notice, placed under the toolbar's band, could never fit
    // in it, so every download went to the corner. In the 720px timeline of
    // the default 1100px window, with the toolbar's band beside the card and
    // a receipt pile on the row, the notice is beside the card.
    function test_aPlainFileRowShowsItsNoticeBesideTheCard_data() {
        return [{tag: "default-window", width: 720},
                {tag: "wide", width: 1100}]
    }
    function test_aPlainFileRowShowsItsNoticeBesideTheCard(data) {
        paneWidth = data.width
        wait(0)
        compare(row.height, 57)
        for (var state of ["saving", "done", "failed"]) {
            all = [item(1, "k1", state)]
            tryVerify(function() { return notice.visible })
            compare(notice.height, 34)
            assertInsideAndClear()
            compare(placeCount(1), 1)
            compare(pane.items.length, 0)
        }
    }

    // In a one-line row the toolbar's band is beside the card too, so it
    // is a horizontal limit; with the row narrower still it covers the space
    // and the pane card takes over.
    function test_theToolbarBandIsAHorizontalLimitInAShortRow() {
        paneWidth = 720
        all = [item(1, "k1", "done")]
        tryVerify(function() { return notice.visible })
        verify(notice.x + notice.width <= toolbar.x - AppTheme.spacing4)
        toolbar.x = notice.x + 10
        tryVerify(function() { return !notice.visible })
        compare(pane.items.length, 1)
        compare(placeCount(1), 1)
    }

    // A taller row (a sender header, a caption) has room beside the card
    // BELOW the toolbar's band; where the toolbar narrows the centred band,
    // the notice takes that one, at its full width with its status text.
    // Where nothing narrows it, it stays centred on the card.
    function test_aTallerRowPutsTheNoticeBelowTheToolbarBand() {
        captionHeight = 100
        all = [item(1, "k1", "done")]
        tryVerify(function() { return notice.visible })
        compare(notice.y, (fileCard.height - notice.height) / 2)
        compare(notice.width, notice.naturalWidth)
        paneWidth = 720
        tryVerify(function() { return notice.y >= toolbar.y + toolbar.height + AppTheme.spacing4 })
        verify(notice.y < fileCard.y + fileCard.height)
        compare(notice.width, notice.naturalWidth)
        verify(notice.showLabel)
        assertInsideAndClear()
        // A one-line row has no such room: centred, narrowed by the band.
        captionHeight = 0
        tryVerify(function() { return notice.y < toolbar.y + toolbar.height })
        verify(notice.visible)
        verify(notice.x + notice.width <= toolbar.x - AppTheme.spacing4)
        assertInsideAndClear()
    }

    function assertInsideAndClear() {
        verify(notice.x >= AppTheme.spacing4)
        verify(notice.y >= AppTheme.spacing4)
        verify(notice.x + notice.width + AppTheme.spacing4 <= row.width)
        verify(notice.y + notice.height + AppTheme.spacing4 <= row.height)
        // The toolbar's band, plus 4px under it.
        verify(notice.x + notice.width <= toolbar.x || notice.x >= toolbar.x + toolbar.width
               || notice.y >= toolbar.y + toolbar.height + AppTheme.spacing4
               || notice.y + notice.height <= toolbar.y)
        verify(notice.x + notice.width <= receipts.x
               || notice.x >= receipts.x + receipts.width
               || notice.y + notice.height <= receipts.y
               || notice.y >= receipts.y + receipts.height)
        var content = [fileCard, caption].concat(notice.collisionItems)
        for (var item of content) {
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
    // The narrowest pane that still fits, derived from the notice's own
    // minimum (fonts differ between machines): the card's right edge, the
    // 12px gap, the minimum, 4px to the toolbar's band, the band and its 12px
    // margin. An own bubble has only the 4px row margin on its left.
    function test_widthThresholdIncludesMargins_data() {
        return [{tag: "incoming", own: false}, {tag: "own", own: true}]
    }
    function test_widthThresholdIncludesMargins(data) {
        ownBubble = data.own
        all = [item(1, "k1", "saving")]
        tryVerify(function() { return notice.visible })
        var threshold = data.own
            ? AppTheme.spacing4 + notice.minWidth + AppTheme.spacing12 + 340 + 16
            : 48 + 340 + AppTheme.spacing12 + notice.minWidth + AppTheme.spacing4
              + toolbar.width + 12
        paneWidth = Math.ceil(threshold)
        tryVerify(function() { return notice.visible })
        assertInsideAndClear()
        paneWidth = Math.floor(threshold) - 1
        tryVerify(function() { return !notice.visible })
        compare(pane.items.length, 1)
        compare(placeCount(1), 1)
    }
    function test_narrowPaneFallsBackWithoutOverlayingMessages() {
        for (var own of [false, true]) {
            ownBubble = own
            paneWidth = 480
            all = [item(1, "k1", "failed")]
            tryVerify(function() { return pane.items.length === 1 })
            verify(!notice.visible)
            compare(placeCount(1), 1)
        }
    }
    function test_wideCaptionLeavesNoFreeSide() {
        captionHeight = 100
        for (var own of [false, true]) {
            ownBubble = own
            all = [item(1, "k1", "saving")]
            tryVerify(function() { return notice.visible })
            captionWidth = 1000
            tryVerify(function() { return !notice.visible })
            compare(pane.items.length, 1)
            compare(placeCount(1), 1)
            captionWidth = 340
            tryVerify(function() { return notice.visible })
            compare(placeCount(1), 1)
        }
    }
    function test_aRowShorterThanTheNoticeFallsBack() {
        all = [item(1, "k1", "saving")]
        tryVerify(function() { return notice.visible })
        fileCard.height = 30 // row 33: no room for a 34px strip and margins
        tryVerify(function() { return !notice.visible })
        compare(pane.items.length, 1)
        compare(placeCount(1), 1)
        fileCard.height = 54
        tryVerify(function() { return notice.visible })
    }
    function test_receiptCollisionHandsTheNoticeToThePane() {
        all = [item(1, "k1", "saving")]
        tryVerify(function() { return notice.visible })
        receipts.x = notice.x + 20
        receipts.y = notice.y + 10
        tryVerify(function() { return !notice.visible })
        compare(pane.items.length, 1)
        compare(placeCount(1), 1)
        receipts.x = row.width - receipts.width
        receipts.y = row.height - receipts.height
        tryVerify(function() { return notice.visible })
        assertInsideAndClear()
    }
    // Reactions sit under the card, so even a long wrapped run leaves the
    // band beside the card free; a chip in that band would take it.
    function test_reactionsUnderTheCardDoNotMoveTheNoticeOrTheRow() {
        captionHeight = 100
        var before = [row.width, row.height, row.implicitHeight,
                      fileCard.x, fileCard.y, fileCard.width, fileCard.height,
                      caption.x, caption.y, caption.width, caption.height]
        all = [item(1, "k1", "saving")]
        tryVerify(function() { return notice.visible })
        var place = [notice.x, notice.y]
        reactionCount = 60
        tryVerify(function() { return reactions.height > 3 * 24 })
        wait(0)
        verify(notice.visible)
        compare([notice.x, notice.y], place)
        assertInsideAndClear()
        compare(placeCount(1), 1)
        compare([row.width, row.height, row.implicitHeight,
                 fileCard.x, fileCard.y, fileCard.width, fileCard.height,
                 caption.x, caption.y, caption.width, caption.height], before)
        reactions.y = notice.y
        tryVerify(function() { return !notice.visible })
        compare(pane.items.length, 1)
        reactions.y = Qt.binding(function() {
            return fileCard.height + 8 + (caption.visible ? caption.height + 8 : 0) })
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
        overlayParent.y = notice.y
        overlayParent.x = notice.x + 20
        tryVerify(function() { return !notice.visible })
        compare(placeCount(1), 1)
        overlay.visible = false
        tryVerify(function() { return notice.visible })
        compare(placeCount(1), 1)
        overlay.visible = true
        tryVerify(function() { return !notice.visible })
        overlayParent.y = row.height + 30
        tryVerify(function() { return notice.visible })
        compare(placeCount(1), 1)
    }
    // A file saved twice: its newest download is beside the card and the
    // older one on the pane card, each in exactly one place. A finishing
    // download never changes the notice's height, so it never stops fitting.
    function test_onlyTheNewestDownloadOfAFileIsBesideTheCard() {
        all = [item(1, "k1", "saving")]
        tryVerify(function() { return notice.visible })
        var h = notice.height
        all = [item(1, "k1", "done"), item(2, "k1", "failed"),
               item(3, "k1", "done"), item(4, "k1", "saving")]
        tryVerify(function() { return notice.visible && notice.item.id === 4 })
        compare(notice.height, h)
        compare(pane.items.length, 3)
        for (var id of [1, 2, 3, 4]) compare(placeCount(id), 1)
        all = [item(1, "k1", "done"), item(2, "k1", "failed"),
               item(3, "k1", "done"), item(4, "k1", "done")]
        wait(0)
        verify(notice.visible)
        compare(notice.height, h)
        for (var id2 of [1, 2, 3, 4]) compare(placeCount(id2), 1)
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
        if (notice.visible && notice.item && notice.item.id === id)
            ++n
        return n
    }
    function test_anItemWithNoRowFallsBackToThePaneCard() {
        // The image viewer, media history and a non-primary gallery item all
        // save under a key no row's primary media has.
        all = [item(1, "viewer-only-key", "done", true),
               item(2, "gallery-sub-item")]
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
        twin.attachment = noCard // the clicked surface cannot accommodate its notice
        tryVerify(function() { return notice.visible })
        compare(twin.visible, false)
        compare(placeCount(1), 1)
        paneWidth = 520
        tryVerify(function() { return pane.items.length === 1 })
        compare(notice.visible, false)
        compare(twin.visible, false)
        compare(placeCount(1), 1)
        paneWidth = 1100
        twin.attachment = fileCard
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

    // Review round 1: a file whose type can run programs is never beside the
    // card, for its whole download: the pane card says in visible text why it
    // has no Open, which a one-line strip could only put in a tooltip. The
    // same row with a harmless file shows its notice (the control).
    function test_aRiskyFileStaysOnThePaneCardWithItsVisibleWarning() {
        all = [item(1, "k1", "saving")]
        tryVerify(function() { return notice.visible })
        for (var state of ["saving", "done", "failed"]) {
            all = [item(1, "k1", state, true)]
            tryVerify(function() { return !notice.visible })
            tryCompare(pane.items, "length", 1)
            compare(placeCount(1), 1)
        }
        all = [item(1, "k1", "done", true)]
        tryVerify(function() { return pane.items.length === 1 && !notice.visible })
        var warning = findChild(pane, "downloadRiskNotice")
        verify(warning !== null)
        tryVerify(function() { return warning.visible })
        verify(warning.text.length > 0)
        var open = findChild(pane, "downloadOpenButton")
        verify(open === null || !open.visible)
        // The notice is still in the row and could have fitted.
        verify(notice.fits)
    }

    // Review round 1: the status is readable wherever the notice fits. At its
    // narrowest span a failure still has its label (Open's width is free in
    // that state); a status shorter than 40px is shown whole; and the icon
    // carries the status as its accessible name, label or not.
    function test_theStatusIsReadableAtTheNarrowestFit() {
        paneWidth = Math.ceil(48 + 340 + AppTheme.spacing12 + notice.minWidth
                              + AppTheme.spacing4 + toolbar.width + 12)
        all = [item(1, "k1", "failed", false, "The server refused the file.")]
        tryVerify(function() { return notice.visible })
        verify(notice.span >= notice.minWidth && notice.span < notice.minWidth + 1)
        verify(notice.showLabel)
        var label = findChild(notice, "downloadNoticeStatus")
        verify(label.visible)
        compare(label.text, "The server refused the file.")
        var icon = findChild(notice, "downloadNoticeStatusIcon")
        compare(icon.Accessible.name, "The server refused the file.")
        all = [item(1, "k1", "saving")]
        tryVerify(function() { return notice.saving })
        compare(icon.Accessible.name, notice.statusText)
        verify(notice.statusText.length > 0)
        // Wide, and a message shorter than 40px: shown, not dropped.
        paneWidth = 1100
        all = [item(1, "k1", "failed", false, "Gone")]
        tryVerify(function() { return notice.visible && notice.failed })
        verify(findChild(notice, "downloadNoticeStatus").implicitWidth < 40)
        verify(notice.showLabel)
        verify(findChild(notice, "downloadNoticeStatus").visible)
    }

    // M2: a finished download stays beside its card until the user
    // dismisses or opens it, the way a browser keeps a finished download in
    // its list. (Off the row it is the pane card's, under that card's timer.)
    function test_aFinishedDownloadStaysBesideItsCardUntilDismissed() {
        pane.dismissAfterMs = 150
        all = [item(1, "k1", "done")]
        tryVerify(function() { return notice.visible })
        wait(1500) // ten pane-card intervals
        verify(notice.visible)
        compare(dismissed.length, 0)
        var dismiss = findChild(notice, "downloadDismissButton")
        verify(dismiss && dismiss.visible)
        mouseClick(dismiss)
        compare(dismissed, [1])
        // Scrolled away: the pane card shows it, and keeps the card's rule
        // (lead decision 2026-10-20): shown beside its card once, it stays
        // until opened or dismissed, so the pane card's timer never runs.
        dismissed = []
        timeline.contentY = 400
        tryVerify(function() { return pane.items.length === 1 })
        compare(pane.items[0].id, 1)
        verify(pane.items[0].keepUntilDismissed === true)
        wait(1500) // ten pane-card intervals
        compare(dismissed, [])
        timeline.contentY = 0
    }

    // A download that never had a card (a save from the image viewer) is the
    // pane card's alone and still expires there.
    function test_aDownloadThatNeverHadACardStillExpiresOnThePaneCard() {
        pane.dismissAfterMs = 150
        all = [item(5, "viewer-only", "done")]
        tryVerify(function() { return pane.items.length === 1 })
        verify(pane.items[0].keepUntilDismissed !== true)
        tryVerify(function() { return dismissed.length === 1 }, 3000)
        compare(dismissed, [5])
    }

    // A notice whose row changes media key in place, both downloads running.
    // The claim it made for the old key must be released, or that download
    // is held by a notice that no longer shows it and the pane card skips it:
    // shown nowhere (review 2026-10-20, L1).
    readonly property Item toolbarItem: toolbar
    readonly property Item receiptsItem: receipts
    Component {
        id: swapNotice
        AttachmentDownloadNotice {
            attachment: fileCard
            // Not `row: row`: inside this object that names its own property.
            row: box.parent
            // Through the test's own properties: inside this object the bare
            // names are its own properties (a binding loop).
            toolbar: test.toolbarItem
            receipts: test.receiptsItem
            contentItems: [fileCard, caption]
            mediaKey: "kA"
            surface: "main"
            eligible: true
        }
    }
    function test_aMediaKeyChangedInPlaceReleasesTheOldClaim() {
        all = [item(1, "kA", "saving"), item(2, "kB", "saving")]
        var n = createTemporaryObject(swapNotice, row, { items: [all[0]] })
        verify(n !== null)
        tryVerify(function() { return n.visible })
        compare(DownloadNotices.owner("kA"), n.token)
        // The pane card has only kB.
        tryVerify(function() { return pane.items.length === 1 })
        compare(pane.items[0].id, 2)
        n.mediaKey = "kB"
        n.items = [all[1]]
        tryVerify(function() { return n.visible })
        compare(DownloadNotices.owner("kB"), n.token)
        compare(DownloadNotices.owner("kA"), "")
        // kA went back to the pane card; kB is beside its card.
        tryVerify(function() { return pane.items.length === 1 })
        compare(pane.items[0].id, 1)
    }

    // A notice as MessageDelegate's Loader builds it: only for a row that has
    // a download, so with its items from the start. A Text BORN holding ""
    // observes the viewport for life and makes every scroll walk the whole
    // timeline (docs/timeline-scrolling.md, "Shipped 1"), so nothing the
    // notice builds may be one: not its status, not an icon, and not the
    // leading Icon AppButton builds even with no iconName (the Open button
    // replaces that content). Checked synchronously, at creation, in each
    // state; timelineRowsCarryNoPermanentViewportObservers and its download
    // sibling check the real flag in the real timeline.
    Component {
        id: loadedNotice
        AttachmentDownloadNotice {
            attachment: fileCard
            // Not `row: row`: inside this object that names its own property.
            row: box.parent
            mediaKey: "k-loaded"
            eligible: false
        }
    }
    function textsIn(item, out) {
        if (item.lineCount !== undefined && item.textFormat !== undefined
            && typeof item.text === "string")
            out.push(item)
        for (var i = 0; i < item.children.length; ++i)
            textsIn(item.children[i], out)
        return out
    }
    function test_aLoadedNoticeIsBornWithNoEmptyText_data() {
        return [ { tag: "saving", state: "saving" },
                 { tag: "done", state: "done" },
                 { tag: "failed", state: "failed" } ]
    }
    function test_aLoadedNoticeIsBornWithNoEmptyText(data) {
        var n = createTemporaryObject(loadedNotice, row,
                                      { items: [item(9, "k-loaded", data.state)] })
        verify(n !== null)
        var texts = textsIn(n, [])
        // The status label, the status icon, Open's label and three icon
        // buttons: proof the walk reached them.
        verify(texts.length >= 6, "only " + texts.length + " Texts found")
        var empty = texts.filter(function(t) { return t.text === "" })
            .map(function(t) {
                var up = t.parent
                while (up && up !== n && !up.objectName)
                    up = up.parent
                return (t.objectName || "?") + " in "
                       + (up ? up.objectName : "?")
            })
        compare(empty, [])
    }
}
