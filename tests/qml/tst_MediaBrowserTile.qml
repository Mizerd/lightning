import QtQuick
import QtTest
import MatrixClient

TestCase {
    id: test
    name: "MediaBrowserTile"
    width: 360
    height: 360
    visible: true
    when: windowShown

    readonly property string widePng: "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAASwAAABkCAIAAACzY5qXAAABkUlEQVR4nO3OQQ0AMBADoEo5qZM+GX2UBAHk3QFFqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMalPoBxqQ9gXOoDGJf6AMZ9cgzpGuKgPukAAAAASUVORK5CYII="
    readonly property int settle: 600
    readonly property string squarePng: "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAHgAAABkCAIAAADCEmNlAAAAyUlEQVR4nO3QAQkAIBDAQKN8VKPbQmEeLMC4tWd0ofX84JNAg24FGnQr0KBbgQbdCjToVqBBtwINuhVo0K1Ag24FGnQr0KBbgQbdCjToVqBBtwINuhVo0K1Ag24FGnQr0KBbgQbdCjToVqBBtwINuhVo0K1Ag24FGnQr0KBbgQbdCjToVqBBtwINuhVo0K1Ag24FGnQr0KBbgQbdCjToVqBBtwINuhVo0K1Ag24FGnQr0KBbgQbdCjToVqBBtwINuhVo0K1Ag251ALhnKg4MJ0UcAAAAAElFTkSuQmCC"

    QtObject {
        id: bridge
        property bool supported: true
        // Recorded with push(): reading and reassigning the array from inside
        // a binding's call would itself be a binding loop.
        property var calls: []
        property var answers: ({})
        signal mediaCached(string cacheKey)
        signal mediaFetchFailed(string cacheKey, string category)
        signal mediaRetryable(string cacheKey)
        function mediaSource(key, kind) {
            calls.push(kind + ":" + key)
            return answers[kind + ":" + key] || ""
        }
        function mxcImageSource(uri, edge) {
            calls.push("mxc:" + uri)
            return ""
        }
        function asked(entry) { return calls.indexOf(entry) >= 0 }
        function askedFor(prefix) {
            return calls.filter(function(c) { return c.indexOf(prefix) === 0 })
        }
    }
    property var app: ({ mediaBridge: bridge })

    Component {
        id: tileComponent
        MediaBrowserTile {
            width: 116
            height: 116
        }
    }

    ListModel { id: gridModel }
    Component {
        id: gridComponent
        GridView {
            width: 348
            height: 348
            cellWidth: 116
            cellHeight: 116
            cacheBuffer: 0
            model: gridModel
            delegate: MediaBrowserTile {
                width: GridView.view.cellWidth
                height: GridView.view.cellHeight
            }
        }
    }

    function row(overrides) {
        var r = {
            kind: "image", body: "pasted-image.png", filename: "pasted-image.png",
            mimetype: "image/png", size: 200 * 1024, mediaWidth: 0,
            mediaHeight: 0, mxc: "mxc://example.org/file", thumbnailMxc: "",
            mediaKey: "$enc", encrypted: true, sender: "@a:example.org",
            eventId: "$enc"
        }
        for (var k in overrides)
            r[k] = overrides[k]
        return r
    }
    function tile(overrides) {
        var props = row(overrides)
        props.index = 0
        var t = createTemporaryObject(tileComponent, test, props)
        verify(t, "the tile did not build")
        return t
    }
    function findImage(item) {
        if (item.fillMode !== undefined && item.sourceSize !== undefined)
            return item
        for (var i = 0; i < item.children.length; ++i) {
            var found = findImage(item.children[i])
            if (found)
                return found
        }
        return null
    }
    function preview(t) {
        var img = findImage(t)
        verify(img, "no preview Image")
        return img
    }

    function init() {
        bridge.calls = []
        bridge.answers = {}
        gridModel.clear()
        failOnWarning(/Binding loop/)
    }

    function test_anEncryptedImageWithoutAThumbnailShowsItsFile() {
        bridge.answers = { "full:$enc": test.squarePng }
        var t = tile({})
        tryVerify(function() { return bridge.asked("full:$enc") }, 2000,
                  "the tile never asked for the file: "
                  + JSON.stringify(bridge.calls))
        compare(bridge.askedFor("list_thumb:").length, 0,
                "asked for a list thumbnail that cannot exist")
        tryCompare(preview(t), "status", Image.Ready)
    }

    function test_theFileShowsWhenItArrives() {
        var t = tile({})
        tryVerify(function() { return bridge.asked("full:$enc") }, 2000)
        compare(preview(t).status, Image.Null)
        bridge.answers = { "full:$enc": test.squarePng }
        bridge.mediaCached("full:$enc")
        tryCompare(preview(t), "status", Image.Ready)
    }

    function test_anEncryptedSenderThumbnailIsStillPreferred() {
        tile({ thumbnailMxc: "mxc://example.org/encthumb" })
        wait(test.settle)
        verify(bridge.asked("list_thumb:$enc"))
        compare(bridge.askedFor("full:").length, 0)
    }

    function test_anUnencryptedImageAsksTheServerThumbnail() {
        tile({ encrypted: false })
        wait(test.settle)
        verify(bridge.asked("list_thumb:$enc"))
        compare(bridge.askedFor("full:").length, 0)
    }

    function test_anEncryptedVideoIsNeverDownloadedForATile() {
        tile({ kind: "video", mimetype: "video/mp4", size: 1024 * 1024 })
        wait(test.settle)
        compare(bridge.askedFor("full:").length, 0)
    }

    function test_aLargeOrUnsizedPictureIsNotDownloadedForATile() {
        tile({ size: 40 * 1024 * 1024 })
        tile({ size: 0 })
        wait(test.settle)
        compare(bridge.askedFor("full:").length, 0,
                JSON.stringify(bridge.calls))
    }

    function test_svgIsNeverFetched() {
        tile({ mimetype: "image/svg+xml" })
        wait(test.settle)
        compare(bridge.askedFor("full:").length, 0)
    }

    function test_aDrawnTileKeepsItsPicture() {
        bridge.answers = { "full:$enc": test.squarePng }
        var t = tile({})
        tryCompare(preview(t), "status", Image.Ready)
        bridge.answers = {}
        bridge.calls = []
        bridge.mediaCached("full:$other")
        compare(preview(t).status, Image.Ready)
        compare(bridge.calls.length, 0, JSON.stringify(bridge.calls))
    }

    function test_aWidePictureFitsWithoutABindingLoop() {
        bridge.answers = { "list_thumb:$enc": test.widePng }
        var t = tile({ encrypted: false, mediaWidth: 3000, mediaHeight: 1000 })
        var img = preview(t)
        var loads = 0
        img.statusChanged.connect(function() {
            if (img.status === Image.Loading)
                ++loads
        })
        tryCompare(img, "status", Image.Ready)
        wait(50)
        compare(img.fillMode, Image.PreserveAspectFit)
        compare(img.status, Image.Ready)
        verify(loads <= 1, "the picture loaded " + loads + " times")
    }

    function test_aNearlySquareOrUndeclaredPictureCrops() {
        bridge.answers = { "list_thumb:$enc": test.widePng }
        var t = tile({ encrypted: false, mediaWidth: 1200, mediaHeight: 1000 })
        tryCompare(preview(t), "status", Image.Ready)
        compare(preview(t).fillMode, Image.PreserveAspectCrop)
        var u = tile({ encrypted: false })
        tryCompare(preview(u), "status", Image.Ready)
        compare(preview(u).fillMode, Image.PreserveAspectCrop)
    }

    function fillGrid() {
        for (var i = 0; i < 120; ++i)
            gridModel.append(row({ mediaKey: "$e" + i, eventId: "$e" + i }))
        var grid = createTemporaryObject(gridComponent, test)
        verify(grid)
        tryVerify(function() { return bridge.askedFor("full:").length >= 9 },
                  2000, JSON.stringify(bridge.askedFor("full:")))
        wait(test.settle)
        var fetched = bridge.askedFor("full:")
        compare(fetched.length, 9, JSON.stringify(fetched))
        for (var k = 0; k < 9; ++k)
            verify(bridge.asked("full:$e" + k))
        bridge.calls = []
        return grid
    }
    function verifyOnlyOnScreen(grid) {
        var fetched = bridge.askedFor("full:")
        verify(fetched.length > 0 && fetched.length <= 12,
               "fetched at rest: " + JSON.stringify(fetched))
        var firstRow = Math.floor(grid.contentY / grid.cellHeight)
        var lastRow = Math.floor((grid.contentY + grid.height - 1) / grid.cellHeight)
        for (var f = 0; f < fetched.length; ++f) {
            var tileRow = Math.floor(Number(fetched[f].substring("full:$e".length)) / 3)
            verify(tileRow >= firstRow && tileRow <= lastRow,
                   fetched[f] + " is not on screen (rows " + firstRow + "-"
                   + lastRow + ")")
        }
    }

    function test_aFlickFetchesOnlyWhereItStops() {
        var grid = fillGrid()
        var duringFlick = []
        grid.contentYChanged.connect(function() {
            if (grid.moving)
                duringFlick = duringFlick.concat(bridge.askedFor("full:"))
        })
        grid.flick(0, -6000)
        tryVerify(function() { return grid.moving }, 1000)
        tryVerify(function() { return !grid.moving }, 10000)
        compare(duringFlick.length, 0,
                "fetched while flicking: " + JSON.stringify(duringFlick))
        tryVerify(function() { return bridge.askedFor("full:").length > 0 }, 2000)
        wait(test.settle)
        verifyOnlyOnScreen(grid)
    }

    function test_aWheelGlideFetchesOnlyWhereItStops() {
        var grid = fillGrid()
        for (var step = 1; step <= 30; ++step) {
            grid.contentY = step * 58
            wait(30)
        }
        tryVerify(function() { return bridge.askedFor("full:").length > 0 }, 2000)
        wait(test.settle)
        verifyOnlyOnScreen(grid)
    }

    function findNamed(item, name) {
        if (item.objectName === name)
            return item
        for (var i = 0; i < item.children.length; ++i) {
            var f = findNamed(item.children[i], name)
            if (f)
                return f
        }
        return null
    }

    function test_aVideoWithoutAPictureHasNoBadgeOverItsName() {
        var t = tile({ kind: "video", mimetype: "video/mp4",
                       filename: "silent-clip.mp4", mediaKey: "", encrypted: false })
        var badge = findNamed(t, "videoBadge")
        verify(badge, "no badge item")
        verify(!badge.visible, "play badge over the label fallback")
    }

    function test_aVideoWithAPictureKeepsItsBadge() {
        bridge.answers = { "list_thumb:$v": test.squarePng }
        var t = tile({ kind: "video", mimetype: "video/mp4", mediaKey: "$v",
                       encrypted: false, thumbnailMxc: "mxc://x/t" })
        tryCompare(preview(t), "status", Image.Ready)
        verify(findNamed(t, "videoBadge").visible)
    }

    function test_theFullNameIsAvailableOnHover() {
        var t = tile({ filename: "a-very-long-name-that-elides.png" })
        compare(t.displayName, "a-very-long-name-that-elides.png")
    }

    function test_openMenuIsExposedForTheMenuKey() {
        var t = tile({})
        verify(typeof t.openMenu === "function")
        verify(!t.keyboardCurrent, "a standalone tile never draws a focus ring")
    }
}
