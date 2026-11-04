import QtQuick
import QtQuick.Controls
import QtTest
import MatrixClient

TestCase {
    id: test
    name: "VoicePlayer"
    width: 900
    height: 700
    visible: true
    when: windowShown

    QtObject {
        id: voice
        property bool active: false
        property string eventId: ""
        property string roomId: ""
        property string roomName: ""
        property string threadRootId: ""
        property string senderName: ""
        property string senderId: ""
        property string senderAvatarMxc: ""
        property string mediaKey: ""
        property string filename: ""
        property bool isVoice: true
        property var waveform: []
        property bool loaded: false
        property bool playing: false
        property string fetchState: "idle"
        property real position: 0
        property real duration: 0
        property bool seekable: true
        property real rate: 1.0
        property var upNext: []
        readonly property bool hasNext: upNext.length > 0
        property bool pausedForCall: false
        property string trackTitle: ""
        property string trackArtist: ""
        property string artworkSource: ""
        property int clipInfoRevision: 0
        property var info: ({})
        property bool rowOnScreen: false
        property var player: null
        property var audioOutput: null
        property var calls: []
        signal currentChanged()
        signal metadataChanged()
        function record(entry) { calls.push(entry) }
        function made(prefix) {
            return calls.filter(function(c) { return c.indexOf(prefix) === 0 })
        }
        function seek(ms) { record("seek:" + Math.round(ms)); position = ms }
        function togglePlayPause() { record("toggle"); playing = !playing }
        function toggle(e, r, k, m) { record("toggle:" + e) }
        function play(e, r, k, m) {
            record("play:" + e + ":" + (m && m.startAtMs !== undefined
                                        ? m.startAtMs : "-"))
        }
        function stop() { record("stop"); active = false; currentChanged() }
        function playNext() { record("next"); return true }
        function restart() { record("restart") }
        function setRate(r) { record("rate:" + r); rate = r }
        function clipInfo(key) { return info[key] || ({}) }
        function reportRowVisible(row, id, visible) {
            record("row:" + id + ":" + visible)
        }
        function load(over) {
            var d = { active: true, eventId: "$clip", roomId: "!room:x",
                      roomName: "Lounge", threadRootId: "",
                      senderName: "Alice", senderId: "@alice:x",
                      senderAvatarMxc: "", mediaKey: "mk", filename: "clip.ogg",
                      isVoice: true, loaded: true, playing: true,
                      fetchState: "idle", position: 4000, duration: 60000,
                      upNext: [], pausedForCall: false, rowOnScreen: false,
                      trackTitle: "", trackArtist: "", artworkSource: "" }
            for (var k in over)
                d[k] = over[k]
            for (var key in d)
                voice[key] = d[key]
            currentChanged()
        }
    }
    QtObject {
        id: settings
        property bool voicePlayerDocked: true
        property rect voiceMiniPlayerGeometry: Qt.rect(0, 0, 0, 0)
        property real voicePlaybackRate: 1.0
        property real mediaVolume: 1.0
        property int gifAutoplay: 0
        property string albumArtStyle: "card"
        function knownMediaSizeBytes(k) { return 0 }
    }
    QtObject {
        id: bridge
        property bool supported: true
        signal mediaCached(string cacheKey)
        signal mediaFetchFailed(string cacheKey, string category)
        signal mediaRetryable(string cacheKey)
        signal animatedMediaReady(string cacheKey)
        signal motionSlotFreed()
        function prefetchPlayable(k, s) {}
        function mxcImageSource(uri, edge) { return "" }
        function cachedSource(k) { return "" }
        function mediaSource(k, v) { return "" }
    }
    QtObject {
        id: appStub
        property var voicePlayback: voice
        property var settings: settings
        property var mediaBridge: bridge
        property int currentScreen: 1
        property string currentRoomId: ""
        property var opened: []
        function openRoom(id) { opened.push(id) }
        function showMain() { currentScreen = 1 }
        property var pagination: QtObject {
            function jumpToEvent(id) {}
        }
        property var thread: QtObject {
            function openThread(r, t) {}
        }
    }
    property var app: appStub

    property int historyHits: 0
    Shortcut {
        sequences: ["Alt+Left", "Alt+Right"]
        onActivated: test.historyHits++
    }

    Component {
        id: hostComponent
        Item {
            width: test.width
            height: test.height
            property alias card: mini
            VoiceMiniPlayer {
                id: mini
                anchors.fill: parent
            }
        }
    }
    Component {
        id: overFlickableComponent
        Item {
            width: test.width
            height: test.height
            property alias flick: flick
            Flickable {
                id: flick
                anchors.fill: parent
                contentHeight: 4000
                contentY: 1500
                Rectangle { width: 10; height: 4000; color: "grey" }
            }
            VoiceMiniPlayer { anchors.fill: parent }
        }
    }
    Component {
        id: rowInViewComponent
        Flickable {
            width: 400
            height: 200
            contentHeight: 2000
            property alias row: inView
            AudioPlayerCard {
                id: inView
                y: 1000
                width: 360
                eventId: "$row"
                roomId: "!room:x"
                mediaKey: "mk-row"
                durationMs: 60000
                isVoice: true
            }
        }
    }
    Component {
        id: barComponent
        VoicePlayerBar {
            width: 600
            shown: true
        }
    }
    Component {
        id: rowComponent
        AudioPlayerCard {
            width: 360
            eventId: "$row"
            roomId: "!room:x"
            mediaKey: "mk-row"
            durationMs: 60000
            isVoice: true
        }
    }

    Component {
        id: volumeComponent
        Row {
            property alias control: volume
            property alias after: afterButton
            property alias output: output
            spacing: 8
            QtObject {
                id: output
                property real volume: 0.5
                property bool muted: false
                property bool userUnmuted: false
            }
            MediaVolumeControl {
                id: volume
                objectName: "volumeUnderTest"
                audio: output
                sliderObjectName: "volumeUnderTestSlider"
            }
            IconButton {
                id: afterButton
                iconName: "close"
            }
        }
    }

    Component {
        id: placedVolumeComponent
        Item {
            id: place
            property real controlY: 0
            property alias control: placed
            property alias output: placedOutput
            width: test.width
            height: test.height
            QtObject {
                id: placedOutput
                property real volume: 1.0
                property bool muted: false
                property bool userUnmuted: false
            }
            MediaVolumeControl {
                id: placed
                y: place.controlY
                audio: placedOutput
                sliderObjectName: "placedSlider"
            }
        }
    }

    function init() {
        voice.calls = []
        voice.info = {}
        voice.clipInfoRevision = 0
        voice.active = false
        voice.rate = 1.0
        settings.voicePlayerDocked = true
        settings.voiceMiniPlayerGeometry = Qt.rect(0, 0, 0, 0)
        settings.voicePlaybackRate = 1.0
        settings.albumArtStyle = "card"
        appStub.currentScreen = 1
        appStub.opened = []
        AppTheme.mode = 2
        AppTheme.textScale = 1
        failOnWarning(/Binding loop/)
    }

    function find(item, name) {
        if (!item)
            return null
        if (item.objectName === name)
            return item
        var kids = item.children
        for (var i = 0; kids && i < kids.length; ++i) {
            var found = find(kids[i], name)
            if (found)
                return found
        }
        var dat = item.data
        for (var d = 0; dat && d < dat.length; ++d) {
            if (dat[d] && dat[d].objectName === name)
                return dat[d]
        }
        if (item.contentItem && item.contentItem !== item
                && item.contentItem.parent !== item) {
            var inContent = find(item.contentItem, name)
            if (inContent)
                return inContent
        }
        return null
    }
    function cardOf(host) {
        var card = find(host, "voiceMiniPlayer")
        verify(card, "the card was not built")
        return card
    }
    function makeHost() {
        var host = createTemporaryObject(hostComponent, test)
        verify(host, "the host did not build")
        return host
    }
    function popOut() {
        settings.voicePlayerDocked = false
    }

    function test_dockedByDefault() {
        var host = makeHost()
        var bar = createTemporaryObject(barComponent, test)
        voice.load({})
        var card = cardOf(host)
        verify(!find(host, "voiceMiniPlayerHost").wanted,
               "the floating card showed while the player is docked")
        compare(card.visible, false)
        verify(bar.visible && bar.height > 20, "the strip did not show")
        var popOutButton = find(bar, "voicePlayerBarPopOut")
        verify(popOutButton)
        mouseClick(popOutButton)
        compare(settings.voicePlayerDocked, false)
        tryCompare(card, "opacity", 1)
        var dock = find(card, "voiceMiniPlayerDock")
        verify(dock)
        mouseClick(dock)
        compare(settings.voicePlayerDocked, true)
        tryCompare(card, "visible", false)
    }

    function test_theScrubberSeeksAndDoesNotLeaveTheRoom() {
        var host = makeHost()
        popOut()
        voice.load({ position: 0, duration: 60000 })
        var card = cardOf(host)
        tryCompare(card, "opacity", 1)
        var seek = find(card, "voiceMiniPlayerSeek")
        verify(seek && seek.visible, "the card has no scrubber")
        verify(seek.height >= 12,
               "the scrubber's hit band is " + seek.height + " px")
        mouseClick(seek, seek.width * 0.5, seek.height / 2)
        verify(voice.made("seek:").length > 0, "a click on the bar did not seek")
        var target = Number(voice.made("seek:")[0].split(":")[1])
        verify(Math.abs(target - 30000) < 3000,
               "seeked to " + target + " for the middle of a 60 s clip")
        compare(appStub.opened.length, 0, "the click left for the clip's room")
        var info = find(card, "voiceMiniPlayerInfo")
        mouseClick(info, 10, info.height / 2)
        compare(appStub.opened.length, 1)
        compare(appStub.opened[0], "!room:x")
    }

    function test_theCardShowsWhereverTheRowIsNot() {
        var host = makeHost()
        var mini = find(host, "voiceMiniPlayerHost")
        popOut()
        voice.load({ rowOnScreen: false })
        appStub.currentRoomId = "!room:x"
        verify(mini.wanted, "no player in the clip's room with its row away")
        voice.rowOnScreen = true
        verify(!mini.wanted, "the card showed beside its own visible row")
        appStub.currentScreen = 2
        verify(mini.wanted, "no player while Settings is open")
        appStub.currentScreen = 1
        voice.stop()
        verify(!mini.wanted)
    }

    function test_theCardMovesFreelyAndRemembersItsPlace() {
        var host = makeHost()
        popOut()
        voice.load({})
        var mini = find(host, "voiceMiniPlayerHost")
        var card = cardOf(host)
        tryCompare(card, "opacity", 1)
        var avatar = find(card, "voiceMiniPlayerAvatar")
        var from = avatar.mapToItem(test, avatar.width / 2, avatar.height / 2)
        var to = Qt.point(450, 300)
        mousePress(test, from.x, from.y)
        for (var i = 1; i <= 10; ++i)
            mouseMove(test, from.x + (to.x - from.x) * i / 10,
                      from.y + (to.y - from.y) * i / 10)
        mouseRelease(test, to.x, to.y)
        var g = settings.voiceMiniPlayerGeometry
        verify(g.width > 0, "the drop was not remembered")
        verify(g.x > 0.2 && g.x < 0.8 && g.y > 0.2 && g.y < 0.8,
               "remembered at " + g.x + "," + g.y + ": not free placement")
        var cx = card.x
        var cy = card.y
        verify(cx > mini.area.x + mini.snapDistance
               && cy > mini.area.y + mini.snapDistance,
               "a drop in the middle snapped to an edge")
        host.width = 600
        host.height = 400
        tryVerify(function() {
            return card.x >= mini.area.x && card.y >= mini.area.y
                && card.x + card.width <= mini.area.x + mini.area.width + 0.5
                && card.y + card.height <= mini.area.y + mini.area.height + 0.5
        }, 2000, "the card left the window after a resize")
        var freeX = mini.area.width - card.width
        tryVerify(function() {
            return Math.abs((card.x - mini.area.x) / freeX - g.x) < 0.02
        }, 2000, "the relative place was not kept")
        var p = avatar.mapToItem(test, avatar.width / 2, avatar.height / 2)
        var travel = card.x - mini.area.x - 10
        mousePress(test, p.x, p.y)
        for (var j = 1; j <= 10; ++j)
            mouseMove(test, p.x - travel * j / 10, p.y)
        mouseRelease(test, p.x - travel, p.y)
        tryCompare(card, "x", mini.area.x)
        compare(settings.voiceMiniPlayerGeometry.x, 0)
    }

    function test_theCardResizesAndReflows() {
        var host = makeHost()
        popOut()
        voice.load({ waveform: [10, 50, 90, 40] })
        var mini = find(host, "voiceMiniPlayerHost")
        var card = cardOf(host)
        tryCompare(card, "opacity", 1)
        compare(card.tier, "regular")
        var corner = find(card, "voiceMiniPlayerResize_br")
        verify(corner, "no resize handle")
        compare(corner.cursorShape, Qt.SizeFDiagCursor)
        var c = corner.mapToItem(test, corner.width / 2, corner.height / 2)
        settings.voiceMiniPlayerGeometry = Qt.rect(0, 0, 300, 92)
        tryCompare(card, "x", mini.area.x)
        c = corner.mapToItem(test, corner.width / 2, corner.height / 2)
        mousePress(test, c.x, c.y)
        for (var i = 1; i <= 10; ++i)
            mouseMove(test, c.x + 80 * i, c.y + 60 * i)
        mouseRelease(test, c.x + 800, c.y + 600)
        tryCompare(card, "width", mini.maxWidth)
        tryCompare(card, "height", mini.maxHeight)
        compare(card.tier, "large")
        verify(find(card, "voiceMiniPlayerWave").visible,
               "the large card does not show the waveform")
        verify(find(card, "voiceMiniPlayerSpeed").visible)
        c = corner.mapToItem(test, corner.width / 2, corner.height / 2)
        mousePress(test, c.x, c.y)
        for (var j = 1; j <= 10; ++j)
            mouseMove(test, c.x - 80 * j, c.y - 60 * j)
        mouseRelease(test, c.x - 800, c.y - 600)
        tryCompare(card, "width", mini.minWidth)
        tryCompare(card, "height", mini.minHeight)
        compare(card.tier, "compact")
        verify(find(card, "voiceMiniPlayerCompactSeek").visible,
               "the compact card lost its scrubber")
        compare(settings.voiceMiniPlayerGeometry.width, mini.minWidth)
    }

    function test_aRememberedPlaceCanNeverBeOffScreen() {
        var host = makeHost()
        popOut()
        settings.voiceMiniPlayerGeometry = Qt.rect(1, 1, 540, 300)
        voice.load({})
        var mini = find(host, "voiceMiniPlayerHost")
        var card = cardOf(host)
        tryCompare(card, "opacity", 1)
        var cases = [[900, 700], [420, 300], [240, 120], [700, 160]]
        for (var i = 0; i < cases.length; ++i) {
            host.width = cases[i][0]
            host.height = cases[i][1]
            wait(0)
            verify(card.x >= 0 && card.y >= 0
                   && card.x + card.width <= host.width
                   && card.y + card.height <= host.height,
                   "at " + cases[i] + " the card is at " + card.x + ","
                   + card.y + " " + card.width + "x" + card.height)
        }
        settings.voiceMiniPlayerGeometry = Qt.rect(7, -3, 300, 92)
        host.width = 900
        host.height = 700
        wait(0)
        verify(card.x + card.width <= host.width && card.y >= 0)
    }

    function test_theKeyboardMovesAndResizesTheCard() {
        var host = makeHost()
        popOut()
        settings.voiceMiniPlayerGeometry = Qt.rect(0.5, 0.5, 300, 92)
        voice.load({})
        var card = cardOf(host)
        tryCompare(card, "opacity", 1)
        card.forceActiveFocus()
        verify(card.activeFocus)
        var x0 = card.x
        var w0 = card.width
        keyClick(Qt.Key_Right, Qt.AltModifier)
        tryVerify(function() { return Math.abs(card.x - (x0 + 24)) < 0.5 })
        keyClick(Qt.Key_Right, Qt.AltModifier | Qt.ShiftModifier)
        tryVerify(function() { return Math.abs(card.width - (w0 + 24)) < 0.5 })
        voice.calls = []
        keyClick(Qt.Key_Space)
        compare(voice.made("toggle").length, 1)
        verify(card.activeFocusOnTab, "Tab cannot reach the card")
    }

    function test_theBarSaysWhatPlaysAndWhy() {
        var bar = createTemporaryObject(barComponent, test)
        var host = makeHost()
        popOut()
        voice.load({ threadRootId: "$root", eventId: "$reply",
                     upNext: [{ eventId: "$n", senderName: "Bob" }] })
        var card = cardOf(host)
        tryCompare(card, "opacity", 1)
        compare(find(card, "voiceMiniPlayerRoom").text,
                "Voice message · Thread · Lounge")
        verify(find(card, "voiceMiniPlayerNext").visible,
               "the card has no Next with a run")
        var title = find(bar, "voicePlayerBarTitle")
        var sub = find(bar, "voicePlayerBarSubtitle")
        compare(title.text, "Alice")
        compare(sub.text, "Voice message · Thread · Lounge")
        verify(find(bar, "voicePlayerBarNext").visible, "no Next with a run")
        voice.pausedForCall = true
        voice.playing = false
        verify(sub.text.indexOf("call") >= 0,
               "a clip the call paused says nothing: " + sub.text)
        voice.load({ isVoice: false, filename: "song.mp3",
                     trackTitle: "Short Test Song", trackArtist: "Band" })
        compare(title.text, "Short Test Song")
        compare(sub.text, "Band · Lounge")
        var chip = find(bar, "voicePlayerBarSpeed")
        voice.calls = []
        mouseClick(chip)
        compare(voice.made("rate:"), ["rate:1.5"])
    }

    function test_theBarAndTheRowFollowTheTextSize() {
        var bar = createTemporaryObject(barComponent, test)
        var row = createTemporaryObject(rowComponent, test)
        voice.load({})
        var title = find(bar, "voicePlayerBarTitle")
        var rowTitle = find(row, "audioTitle")
        var small = title.font.pixelSize
        var rowSmall = rowTitle.font.pixelSize
        AppTheme.textScale = 1.5
        verify(title.font.pixelSize >= small * 1.4,
               "the bar ignored the text size: " + small + " -> "
               + title.font.pixelSize)
        verify(rowTitle.font.pixelSize >= rowSmall * 1.4,
               "the row ignored the text size")
    }

    function test_theCardReadsInEveryTheme() {
        var host = makeHost()
        popOut()
        voice.load({})
        var card = cardOf(host)
        tryCompare(card, "opacity", 1)
        function lum(c) { return AppTheme.relativeLuminance(c) }
        function ratio(a, b) {
            var x = lum(a), y = lum(b)
            return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05)
        }
        function lab(c) {
            function lin(v) {
                return v <= 0.04045 ? v / 12.92
                                    : Math.pow((v + 0.055) / 1.055, 2.4)
            }
            var r = lin(c.r), g = lin(c.g), b = lin(c.b)
            var X = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047
            var Y = 0.2126 * r + 0.7152 * g + 0.0722 * b
            var Z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883
            function f(t) { return t > 0.008856 ? Math.pow(t, 1 / 3)
                                                : 7.787 * t + 16 / 116 }
            return [116 * f(Y) - 16, 500 * (f(X) - f(Y)), 200 * (f(Y) - f(Z))]
        }
        function dE(a, b) {
            var p = lab(a), q = lab(b)
            return Math.sqrt(Math.pow(p[0] - q[0], 2) + Math.pow(p[1] - q[1], 2)
                             + Math.pow(p[2] - q[2], 2))
        }
        var title = find(card, "voiceMiniPlayerSender")
        var sub = find(card, "voiceMiniPlayerRoom")
        var clock = find(card, "voiceMiniPlayerClock")
        var bar = createTemporaryObject(barComponent, test)
        var barSub = find(bar, "voicePlayerBarSubtitle")
        var palettes = {}
        var checked = 0
        for (var mode = 1; mode <= 11; ++mode) {
            AppTheme.mode = mode
            voice.fetchState = "idle"
            var surface = card.color
            palettes[AppTheme.otherBubble.toString()] = true
            verify(dE(surface, AppTheme.otherBubble) >= 6,
                   "theme " + mode + ": card vs other bubble dE "
                   + dE(surface, AppTheme.otherBubble).toFixed(2))
            verify(dE(surface, AppTheme.ownBubble) >= 6,
                   "theme " + mode + ": card vs own bubble dE "
                   + dE(surface, AppTheme.ownBubble).toFixed(2))
            verify(dE(surface, AppTheme.timelineBackground) >= 6,
                   "theme " + mode + ": card vs canvas")
            var inks = [title.color, sub.color, clock.color]
            voice.fetchState = "failed"
            inks.push(sub.color)
            for (var i = 0; i < inks.length; ++i)
                verify(ratio(inks[i], surface) >= 4.5,
                       "theme " + mode + " card ink " + i + ": "
                       + ratio(inks[i], surface).toFixed(2))
            verify(ratio(barSub.color, bar.color) >= 4.5,
                   "theme " + mode + " strip subtitle: "
                   + ratio(barSub.color, bar.color).toFixed(2))
            ++checked
        }
        voice.fetchState = "idle"
        compare(checked, 11)
        verify(Object.keys(palettes).length >= 8,
               "only " + Object.keys(palettes).length + " distinct palettes")
    }

    function test_anIdleRowLightsNoBarAndFillsItsWidth() {
        var row = createTemporaryObject(rowComponent, test,
                                        { waveform: [50, 60, 70, 80, 90, 40,
                                                     30, 20, 10, 50] })
        var wave = find(row, "voiceSeekWave")
        verify(wave && wave.visible, "no waveform drawn")
        waitForRendering(row)
        var lit = 0
        var right = 0
        var bars = wave.bars()
        verify(bars.length > 10, bars.length + " bars")
        for (var i = 0; i < bars.length; ++i) {
            var bar = bars[i]
            if (bar.width <= 0 || bar.height <= 0)
                continue
            if (bar.lit)
                ++lit
            right = Math.max(right, bar.x + bar.width)
        }
        compare(lit, 0, "an idle row lights " + lit + " bar(s)")
        verify(right >= wave.width * 0.95,
               "ten buckets cover " + right + " of " + wave.width
               + " px: dead space after a short waveform")
    }

    Component {
        id: barPerRectangleComponent
        Rectangle {
            id: plate
            property var waveform: []
            property color ink
            property real waveWidth: 0
            property real waveHeight: 0
            width: waveWidth + 8
            height: waveHeight + 8
            Row {
                id: plateBars
                x: 4
                y: 4
                width: plate.waveWidth
                height: plate.waveHeight
                spacing: 1
                readonly property int count: Math.max(1, Math.floor((width + 1) / 3))
                readonly property real barWidth:
                    Math.max(1, (width - (count - 1)) / count)
                Repeater {
                    model: plateBars.count
                    delegate: Rectangle {
                        required property int index
                        readonly property real amp: {
                            var wf = plate.waveform
                            var at = Math.floor(index * wf.length / plateBars.count)
                            return Math.max(0.12, Math.min(1, wf[at] / 100))
                        }
                        width: plateBars.barWidth
                        height: plateBars.height * amp
                        anchors.verticalCenter: parent.verticalCenter
                        radius: Math.min(1, width / 2)
                        color: plate.ink
                    }
                }
            }
        }
    }

    // The painted waveform against the Rectangle-per-bar one it replaced.
    function test_thePaintedWaveformDrawsTheSameBarsAsRectangles() {
        var wf = [0, 5, 12, 25, 40, 55, 70, 85, 100, 64, 33, 90, 18, 47]
        var row = createTemporaryObject(rowComponent, test, { waveform: wf })
        var wave = find(row, "voiceSeekWave")
        verify(wave && wave.visible && wave.width > 100)
        var plate = createTemporaryObject(barPerRectangleComponent, test, {
            y: 300, waveform: wf, color: row.color,
            ink: AppTheme.borderStrong,
            waveWidth: wave.width, waveHeight: wave.height })
        wait(50)
        waitForRendering(test)
        var shot = grabImage(test)
        var at = wave.mapToItem(test, 0, 0)
        var bg = [shot.red(plate.x + 1, plate.y + 1),
                  shot.green(plate.x + 1, plate.y + 1),
                  shot.blue(plate.x + 1, plate.y + 1)]
        function inked(x, y) {
            return Math.abs(shot.red(x, y) - bg[0])
                 + Math.abs(shot.green(x, y) - bg[1])
                 + Math.abs(shot.blue(x, y) - bg[2])
        }
        var bars = wave.bars()
        var mid = Math.floor(at.y + wave.height / 2)
        var full = 0
        for (var b = 0; b < bars.length; ++b)
            full = Math.max(full, inked(Math.floor(at.x + bars[b].x + bars[b].width / 2), mid))
        verify(full > 15, "no visible ink: " + full)
        var compared = 0
        for (var i = 0; i < bars.length; ++i) {
            var cx = bars[i].x + bars[i].width / 2
            var painted = 0
            var reference = 0
            for (var y = 0; y < Math.ceil(wave.height); ++y) {
                if (inked(Math.floor(at.x + cx), Math.floor(at.y) + y) > full / 2)
                    ++painted
                if (inked(Math.floor(plate.x + 4 + cx), plate.y + 4 + y) > full / 2)
                    ++reference
            }
            // The software renderer snaps a Rectangle out to whole pixels.
            verify(Math.abs(painted - reference) <= 2,
                   "bar " + i + ": " + painted + " px painted, "
                   + reference + " px as a Rectangle")
            ++compared
        }
        compare(compared, Math.floor((wave.width + 1) / 3))
    }

    function test_anIdleRowLaysOutLikeThePlayingOne() {
        var names = ["audioPlayPauseButton", "audioTitle", "audioWaveRow",
                     "audioSeekSlider", "audioClock", "audioSpeedButton",
                     "audioMuteButton", "audioSaveButton", "audioCoverArtBox",
                     "voiceSeekWave"]
        function rects(row) {
            var out = {}
            for (var i = 0; i < names.length; ++i) {
                var it = find(row, names[i])
                if (!it || !it.visible)
                    continue
                var p = it.mapToItem(row, 0, 0)
                out[names[i]] = [p.x, p.y, it.width, it.height].join(",")
            }
            return out
        }
        function muteLook(row) {
            var m = find(row, "audioMuteButton")
            return [m.iconName, m.enabled, m.opacity, m.iconSize,
                    m.Accessible.name].join("|")
        }
        var cases = [{ voice: true, style: "card" },
                     { voice: false, style: "compact" },
                     { voice: false, style: "card" },
                     { voice: false, style: "large" }]
        var checked = 0
        for (var c = 0; c < cases.length; ++c) {
            settings.albumArtStyle = cases[c].style
            voice.active = false
            voice.info = cases[c].voice ? ({})
                : ({ "mk-row": { title: "Song", artist: "Band",
                                 artworkSource: tinyCover } })
            voice.clipInfoRevision++
            var row = createTemporaryObject(rowComponent, test, {
                isVoice: cases[c].voice, canSave: true, filename: "song.mp3",
                waveform: cases[c].voice ? [30, 60, 90, 45] : [] })
            wait(50)
            var idle = rects(row)
            var idleMute = muteLook(row)
            verify(Object.keys(idle).length >= 7,
                   cases[c].style + ": " + JSON.stringify(idle))
            var save = find(row, "audioSaveButton")
            fuzzyCompare(save.mapToItem(row, save.width, 0).x, row.width - 6, 0.5)
            voice.load({ eventId: "$row", isVoice: cases[c].voice,
                         loaded: false, playing: false, position: 0,
                         duration: 0, mediaKey: "mk-row" })
            wait(0)
            compare(JSON.stringify(rects(row)), JSON.stringify(idle),
                    cases[c].style + (cases[c].voice ? " voice" : " file"))
            compare(muteLook(row), idleMute)
            compare(find(row, "audioMuteButton").sliderObjectName,
                    "audioVolumeSlider")
            voice.stop()
            wait(0)
            compare(JSON.stringify(rects(row)), JSON.stringify(idle))
            ++checked
        }
        compare(checked, 4)
    }

    function test_anIdleRowSeeksByStartingThere() {
        var row = createTemporaryObject(rowComponent, test,
                                        { waveform: [50, 60, 70, 80] })
        var seek = find(row, "audioWaveRow")
        verify(seek, "no seek surface")
        verify(seek.enabled, "an idle row cannot be seeked")
        voice.calls = []
        mouseClick(seek, seek.width * 0.5, seek.height / 2)
        var plays = voice.made("play:$row:")
        compare(plays.length, 1, "no start: " + voice.calls)
        var at = Number(plays[0].split(":")[2])
        verify(Math.abs(at - 30000) < 3000, "started at " + at)
        voice.load({ eventId: "$row", position: 10000, duration: 60000 })
        seek.forceActiveFocus()
        verify(seek.activeFocus, "the waveform takes no keyboard focus")
        voice.calls = []
        keyClick(Qt.Key_Right)
        compare(voice.made("seek:"), ["seek:15000"])
        keyClick(Qt.Key_Home)
        compare(voice.made("seek:")[1], "seek:0")
    }

    readonly property string tinyCover:
        "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=="

    function test_anAudioFileKeepsItsHeightAndShowsItsTitle() {
        settings.albumArtStyle = "compact"
        var row = createTemporaryObject(rowComponent, test,
                                        { isVoice: false,
                                          filename: "Short Test Song.mp3" })
        var title = find(row, "audioTitle")
        compare(title.text, "Short Test Song.mp3")
        var before = row.implicitHeight
        voice.info = { "mk-row": { title: "Short Test Song", artist: "Band",
                                   artworkSource: tinyCover } }
        voice.clipInfoRevision++
        compare(title.text, "Short Test Song")
        verify(find(row, "audioClock").text.indexOf("Band") >= 0)
        wait(50)
        compare(row.implicitHeight, before,
                "the cover changed the row's height")
        var seekBefore = find(row, "audioSeekSlider").width
        voice.load({ eventId: "$row", isVoice: false, loaded: true })
        wait(0)
        compare(row.implicitHeight, before)
        compare(find(row, "audioSeekSlider").width, seekBefore,
                "the scrubber shrank when playback started")
    }

    function test_aLargeCoverGrowsTheRowOnceAndPlaybackNeverMovesIt() {
        compare(settings.albumArtStyle, "card")
        var row = createTemporaryObject(rowComponent, test,
                                        { isVoice: false,
                                          filename: "Short Test Song.mp3" })
        var compact = row.implicitHeight
        voice.info = { "mk-row": { title: "Short Test Song", artist: "Band",
                                   artworkSource: tinyCover } }
        voice.clipInfoRevision++
        wait(50)
        var box = find(row, "audioCoverArtBox")
        verify(box && box.visible && box.height > 80,
               "the large cover is not shown")
        var withCover = row.implicitHeight
        var seekWithCover = find(row, "audioSeekSlider").width
        verify(withCover >= box.height + 12 && withCover > compact,
               "the row did not make room for the large cover: " + compact
               + " -> " + withCover)
        voice.load({ eventId: "$row", isVoice: false, loaded: true })
        wait(0)
        compare(row.implicitHeight, withCover, "starting playback moved the row")
        compare(find(row, "audioSeekSlider").width, seekWithCover,
                "the scrubber changed width when playback began")
        voice.load({ eventId: "$row", isVoice: false, loaded: false })
        wait(0)
        compare(row.implicitHeight, withCover, "stopping playback moved the row")
        settings.albumArtStyle = "compact"
        wait(50)
        compare(row.implicitHeight, compact)
    }

    function test_theLargeStyleIsASquareAboveTheControls() {
        settings.albumArtStyle = "large"
        var row = createTemporaryObject(rowComponent, test,
                                        { isVoice: false, width: 360,
                                          filename: "Song.mp3" })
        voice.info = { "mk-row": { artworkSource: tinyCover } }
        voice.clipInfoRevision++
        wait(50)
        var box = find(row, "audioCoverArtBox")
        var play = find(row, "audioPlayPauseButton")
        compare(box.width, row.width - 12, "the art does not fill the card")
        compare(box.height, box.width)
        var bp = box.mapToItem(row, 0, 0)
        compare(bp.x, 6)
        compare(bp.x + box.width, row.width - 6)
        var pp = play.mapToItem(row, 0, 0)
        verify(pp.y >= bp.y + box.height, "the controls are not below the art")
        var h = row.implicitHeight
        voice.load({ eventId: "$row", isVoice: false, loaded: true })
        wait(0)
        compare(row.implicitHeight, h, "starting playback moved the row")
        row.width = 240
        wait(50)
        compare(box.width, 40, "a narrow row keeps the compact tile")
    }

    function test_theLargeStyleFillsABubbleAndReservesItsSquareBeforeTheArt() {
        settings.albumArtStyle = "large"
        var row = createTemporaryObject(rowComponent, test,
                                        { isVoice: false, width: 360,
                                          inBubble: true, thumbAvailable: true,
                                          filename: "Song.mp3" })
        wait(0)
        var box = find(row, "audioCoverArtBox")
        compare(box.width, 360, "the art does not fill the bubble")
        compare(box.height, 360)
        var h = row.implicitHeight
        verify(h >= 360 + 12, "the square was not reserved: " + h)
        voice.info = { "mk-row": { artworkSource: tinyCover } }
        voice.clipInfoRevision++
        wait(50)
        compare(box.width, 360)
        compare(row.implicitHeight, h, "the art arriving moved the row")
    }

    function test_clickingTheCoverAsksForTheEnlargedArtAndOnlyThat() {
        for (var large = 0; large < 3; ++large) {
            settings.albumArtStyle = ["compact", "card", "large"][large]
            voice.calls = []
            var row = createTemporaryObject(rowComponent, test,
                                            { isVoice: false,
                                              filename: "Song.mp3" })
            voice.info = { "mk-row": { artworkSource: tinyCover } }
            voice.clipInfoRevision++
            wait(50)
            var asked = []
            row.artworkViewRequested.connect(function(src, title) {
                asked.push([src, title]) })
            var button = find(row, "audioCoverButton")
            verify(button && button.visible && button.enabled,
                   "no cover button, large=" + large)
            compare(button.Accessible.name, "View cover art")
            mouseClick(button, button.width / 2, button.height / 2)
            compare(asked.length, 1, "click, large=" + large)
            compare(asked[0][0], tinyCover)
            compare(asked[0][1], "Song.mp3")
            button.forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Return)
            compare(asked.length, 2, "Enter, large=" + large)
            compare(voice.calls.filter(function(c) {
                return c.indexOf("row:") !== 0 }), [],
                "clicking the cover touched playback: " + voice.calls)
        }
    }

    function test_noKnownCoverDrawsNoTileInAnyStyle() {
        var styles = ["compact", "card", "large"]
        var compactHeight = -1
        for (var i = 0; i < styles.length; ++i) {
            settings.albumArtStyle = styles[i]
            var row = createTemporaryObject(rowComponent, test,
                                            { isVoice: false,
                                              filename: "Long.flac" })
            wait(0)
            compare(find(row, "audioCoverArtBox"), null,
                    "an empty cover tile in " + styles[i])
            compare(find(row, "audioCoverButton"), null, styles[i])
            verify(!find(row, "audioArtSlot").visible, styles[i])
            var play = find(row, "audioPlayPauseButton")
            verify(play.mapToItem(row, 0, 0).x < 12,
                   "the controls still leave room for a tile in " + styles[i])
            if (i === 0)
                compactHeight = row.implicitHeight
            compare(row.implicitHeight, compactHeight,
                    "the row reserved art height in " + styles[i])
        }
    }

    function test_anEventThumbnailReservesTheTileForEachStyle() {
        var styles = ["compact", "card", "large"]
        var sizes = [40, Math.round(AppTheme.scaled(96)), 360 - 12]
        for (var i = 0; i < styles.length; ++i) {
            settings.albumArtStyle = styles[i]
            var row = createTemporaryObject(rowComponent, test,
                                            { isVoice: false, width: 360,
                                              thumbAvailable: true,
                                              filename: "Long.flac" })
            wait(0)
            var box = find(row, "audioCoverArtBox")
            verify(box && box.visible, "no tile in " + styles[i])
            compare(box.width, sizes[i], styles[i])
            compare(box.height, sizes[i], styles[i])
            var bp = box.mapToItem(row, 0, 0)
            verify(bp.x >= 0 && bp.x + box.width <= row.width
                   && bp.y + box.height <= row.implicitHeight,
                   "the tile leaves the card in " + styles[i])
            var h = row.implicitHeight
            voice.info = { "mk-row": { artworkSource: tinyCover } }
            voice.clipInfoRevision++
            wait(50)
            compare(row.implicitHeight, h, "the art moved the row in " + styles[i])
            voice.info = {}
            voice.clipInfoRevision++
        }
    }

    function test_aPendingEventCoverIsInert() {
        settings.albumArtStyle = "card"
        var row = createTemporaryObject(rowComponent, test,
                                        { isVoice: false, thumbAvailable: true,
                                          filename: "Song.mp3" })
        var asked = 0
        row.artworkViewRequested.connect(function() { asked++ })
        var button = find(row, "audioCoverButton")
        verify(button && !button.enabled, "the placeholder tile is live")
        mouseClick(button, button.width / 2, button.height / 2)
        keyClick(Qt.Key_Return)
        compare(asked, 0)
    }

    function test_anIdleVoiceRowShowsTheVoiceSpeed() {
        settings.voicePlaybackRate = 1.5
        var row = createTemporaryObject(rowComponent, test)
        var speed = find(row, "audioSpeedButton")
        verify(speed.visible && speed.enabled,
               "an idle row hides the speed it will play at")
        compare(speed.rate, 1.5)
        var file = createTemporaryObject(rowComponent, test,
                                         { isVoice: false })
        compare(find(file, "audioSpeedButton").rate, 1.0)
    }

    function test_thePlayTooltipFollowsTheState() {
        var row = createTemporaryObject(rowComponent, test)
        var button = find(row, "audioPlayPauseButton")
        voice.load({ eventId: "$row", playing: true })
        compare(button.actionLabel, "Pause voice message")
        verify(button.Accessible.description.indexOf("Playing") === 0)
        voice.playing = false
        compare(button.actionLabel, "Play voice message")
        mouseMove(button, button.width / 2, button.height / 2)
        mouseClick(button)
        compare(button.tipSuppressed, true)
    }

    function test_thePlayTooltipIsCentredAboveTheButton() {
        var row = createTemporaryObject(rowComponent, test, { x: 200, y: 200 })
        var button = find(row, "audioPlayPauseButton")
        var tip = null
        for (var i = 0; i < button.data.length; ++i)
            if (button.data[i].objectName === "audioPlayPauseTip")
                tip = button.data[i]
        verify(tip !== null)
        mouseMove(button, button.width / 2, button.height / 2)
        tryVerify(function() { return tip.visible }, 2000, "tooltip never opened")
        verify(tip.y < 0, "above the button, y=" + tip.y)
        fuzzyCompare(tip.x + tip.width / 2, button.width / 2, 1.5)
    }

    function test_inABubbleTheRowDrawsNoBox() {
        var row = createTemporaryObject(rowComponent, test, { inBubble: true })
        compare(row.border.width, 0)
        compare(row.color.a, 0)
    }

    function test_theRowReportsWhetherItIsOnScreen() {
        voice.load({ eventId: "$row" })
        voice.calls = []
        var row = createTemporaryObject(rowComponent, test)
        var reports = voice.made("row:")
        compare(reports[reports.length - 1], "row:$row:true")
        row.rowOnScreen = false
        reports = voice.made("row:")
        compare(reports[reports.length - 1], "row:$row:false")
        var other = createTemporaryObject(rowComponent, test,
                                          { eventId: "$other" })
        reports = voice.made("row:$other:")
        compare(reports[reports.length - 1], "row:$other:false")
    }

    function test_theVolumeSliderIsATabStop() {
        var row = createTemporaryObject(volumeComponent, test)
        var control = row.control
        control.forceActiveFocus(Qt.TabFocusReason)
        verify(control.activeFocus)
        keyClick(Qt.Key_Up)
        compare(Math.round(row.output.volume * 100), 55)
        compare(Math.round(settings.mediaVolume * 100), 55)
        verify(control.Accessible.description.indexOf("55%") >= 0,
               control.Accessible.description)
        keyClick(Qt.Key_Tab)
        var slider = find(Overlay.overlay, "volumeUnderTestSlider")
        verify(slider, "no slider in the popup")
        tryVerify(function() { return slider.activeFocus },
                  2000, "Tab did not reach the volume slider")
        verify(slider.Accessible.name.indexOf("%") > 0, slider.Accessible.name)
        keyClick(Qt.Key_Down)
        compare(Math.round(row.output.volume * 100), 50)
        keyClick(Qt.Key_End)
        compare(row.output.volume, 1)
        keyClick(Qt.Key_Home)
        compare(row.output.volume, 0)
        compare(row.output.muted, true)
        keyClick(Qt.Key_Tab)
        tryVerify(function() { return row.after.activeFocus },
                  2000, "Tab stayed in the slider")
    }

    function test_aDragOverTheTimelineMovesTheCardNotTheTimeline() {
        var scene = createTemporaryObject(overFlickableComponent, test)
        popOut()
        settings.voiceMiniPlayerGeometry = Qt.rect(0.5, 0.1, 300, 92)
        voice.load({})
        var card = cardOf(scene)
        tryCompare(card, "opacity", 1)
        var avatar = find(card, "voiceMiniPlayerAvatar")
        var from = avatar.mapToItem(test, avatar.width / 2, avatar.height / 2)
        var y0 = card.y
        mousePress(test, from.x, from.y)
        for (var i = 1; i <= 12; ++i) {
            mouseMove(test, from.x + 2 * i, from.y + 25 * i)
            wait(5)
        }
        mouseRelease(test, from.x + 24, from.y + 300)
        compare(scene.flick.contentY, 1500, "the timeline scrolled under the card")
        verify(Math.abs(card.y - (y0 + 300)) < 2,
               "the card moved " + (card.y - y0) + " of 300 px")
    }

    function test_aRowScrolledAwayIsNotOnScreen() {
        voice.load({ eventId: "$row" })
        var view = createTemporaryObject(rowInViewComponent, test)
        verify(view.row.rowOnScreen, "the fixture's band says on screen")
        tryVerify(function() {
            var r = voice.made("row:$row:")
            return r.length > 0 && r[r.length - 1] === "row:$row:false"
        }, 2000, "a row 800 px below the view reported itself on screen")
        view.contentY = 950
        tryVerify(function() {
            var r = voice.made("row:$row:")
            return r[r.length - 1] === "row:$row:true"
        }, 2000, "the row scrolled into view did not report it")
    }

    function test_spaceAfterTheShortcutPlaysOrPauses_data() {
        return [ { tag: "under the header", atBottom: false },
                 { tag: "along the bottom", atBottom: true } ]
    }
    function test_spaceAfterTheShortcutPlaysOrPauses(data) {
        var bar = createTemporaryObject(barComponent, test,
                                        { atBottom: data.atBottom })
        voice.load({})
        voice.calls = []
        appStub.opened = []
        bar.focusPlayer()
        verify(find(bar, "voicePlayerBarPlayPause").activeFocus,
               "the shortcut's focus is not on play/pause")
        keyClick(Qt.Key_Space)
        compare(appStub.opened.length, 0, "Space opened the message")
        compare(voice.made("toggle").length, 1, "Space did not play/pause")
        voice.calls = []
        find(bar, "voicePlayerBarSource").forceActiveFocus()
        keyClick(Qt.Key_Space)
        compare(appStub.opened.length, 0)
        compare(voice.made("toggle").length, 1)
    }

    function test_aDragDoesNotTakeTheFocus() {
        var host = makeHost()
        popOut()
        voice.load({})
        var card = cardOf(host)
        tryCompare(card, "opacity", 1)
        var avatar = find(card, "voiceMiniPlayerAvatar")
        var at = avatar.mapToItem(card, avatar.width / 2, avatar.height / 2)
        mouseDrag(card, at.x, at.y, -60, -40)
        verify(!card.activeFocus, "a drag took the keyboard focus")
        mouseClick(card, at.x, at.y)
        verify(!card.activeFocus, "a click took the keyboard focus")
        compare(card.border.width, 1)
    }

    function test_idleVolumeIsNotMutedAndCompactKeepsTheTitle() {
        var row = createTemporaryObject(rowComponent, test)
        compare(find(row, "audioMuteButton").iconName, "volume_up")
        var host = makeHost()
        popOut()
        settings.voiceMiniPlayerGeometry = Qt.rect(0.5, 0.5, 200, 52)
        voice.load({})
        var card = cardOf(host)
        tryCompare(card, "opacity", 1)
        compare(card.tier, "compact")
        var title = find(card, "voiceMiniPlayerSender")
        verify(title.visible && title.width > 20,
               "the compact card shows no title")
    }

    function test_accessibleNamesDoNotTick() {
        var bar = createTemporaryObject(barComponent, test)
        voice.load({ position: 1000 })
        var seek = find(bar, "voicePlayerBarSeek")
        var before = [bar.Accessible.name, bar.Accessible.description,
                      seek.Accessible.name, seek.Accessible.description]
        voice.position = 31000
        var after = [bar.Accessible.name, bar.Accessible.description,
                     seek.Accessible.name, seek.Accessible.description]
        compare(after, before)
    }

    function test_returnAndEnterActivateTheButtons() {
        var bar = createTemporaryObject(barComponent, test)
        voice.load({})
        voice.calls = []
        var play = find(bar, "voicePlayerBarPlayPause")
        play.forceActiveFocus(Qt.TabFocusReason)
        keyClick(Qt.Key_Return)
        compare(voice.made("toggle").length, 1, "Return on the strip's play")
        keyClick(Qt.Key_Enter)
        compare(voice.made("toggle").length, 2, "Enter on the strip's play")
        var chip = find(bar, "voicePlayerBarSpeed")
        chip.forceActiveFocus(Qt.TabFocusReason)
        keyClick(Qt.Key_Return)
        compare(voice.made("rate:").length, 1, "Return on the speed chip")
        keyClick(Qt.Key_Enter)
        compare(voice.made("rate:").length, 2, "Enter on the speed chip")

        var host = makeHost()
        popOut()
        var card = cardOf(host)
        tryCompare(card, "opacity", 1)
        voice.calls = []
        var next = find(card, "voiceMiniPlayerPlayPause")
        next.forceActiveFocus(Qt.TabFocusReason)
        keyClick(Qt.Key_Return)
        compare(voice.made("toggle").length, 1, "Return on the card's play")
        var stop = find(card, "voiceMiniPlayerStop")
        stop.forceActiveFocus(Qt.TabFocusReason)
        keyClick(Qt.Key_Return)
        compare(voice.made("stop").length, 1, "Return on the card's stop")

        var row = createTemporaryObject(rowComponent, test)
        voice.calls = []
        var rowPlay = find(row, "audioPlayPauseButton")
        rowPlay.forceActiveFocus(Qt.TabFocusReason)
        keyClick(Qt.Key_Return)
        compare(voice.made("toggle:").length, 1, "Return on the row's play")
        var speed = find(row, "audioSpeedButton")
        speed.forceActiveFocus(Qt.TabFocusReason)
        keyClick(Qt.Key_Return)
        var menu = speed.speedMenu
        verify(menu && menu.objectName === "audioSpeedMenu", "no speed menu")
        tryVerify(function() { return menu.visible }, 2000,
                  "Return on the row's speed chip opened no menu")
    }

    function test_aDragFromTheTitleMovesTheCard() {
        var host = makeHost()
        popOut()
        settings.voiceMiniPlayerGeometry = Qt.rect(0.5, 0.3, 300, 92)
        voice.load({})
        var card = cardOf(host)
        tryCompare(card, "opacity", 1)
        var title = find(card, "voiceMiniPlayerSender")
        var from = title.mapToItem(test, 8, title.height / 2)
        var x0 = card.x
        var y0 = card.y
        mousePress(test, from.x, from.y)
        for (var i = 1; i <= 8; ++i) {
            mouseMove(test, from.x - 5 * i, from.y + 20 * i)
            wait(5)
        }
        mouseRelease(test, from.x - 40, from.y + 160)
        verify(Math.abs(card.y - (y0 + 160)) < 2,
               "a drag from the title moved the card " + (card.y - y0)
               + " of 160 px")
        verify(card.x < x0 - 30, "and " + (x0 - card.x) + " of 40 px sideways")
        compare(appStub.opened.length, 0, "the drag opened the message")
        var at = title.mapToItem(card, 8, title.height / 2)
        mouseClick(card, at.x, at.y)
        tryVerify(function() { return appStub.opened.length > 0 }, 2000,
                  "a click on the title did not go to the message")
    }

    function test_aPressOffTheTitleReleasedOverItDoesNotOpenTheMessage() {
        var host = makeHost()
        popOut()
        settings.voiceMiniPlayerGeometry = Qt.rect(0.5, 0.3, 300, 92)
        voice.load({})
        var card = cardOf(host)
        tryCompare(card, "opacity", 1)
        var title = find(card, "voiceMiniPlayerSender")
        var t = title.mapToItem(card, 0, 0)
        var startX = t.x - 3
        var y = t.y + title.height / 2
        verify(startX > 0)
        mousePress(card, startX, y)
        mouseRelease(card, t.x + 1, y)
        wait(50)
        compare(appStub.opened.length, 0,
                "a press that began off the title opened the message")
    }

    function test_altArrowsMoveTheCardAndNotTheHistory() {
        var host = makeHost()
        popOut()
        settings.voiceMiniPlayerGeometry = Qt.rect(0.5, 0.5, 300, 92)
        voice.load({})
        var card = cardOf(host)
        tryCompare(card, "opacity", 1)
        card.forceActiveFocus()
        test.historyHits = 0
        var x0 = card.x
        keyClick(Qt.Key_Left, Qt.AltModifier)
        tryVerify(function() { return Math.abs(card.x - (x0 - 24)) < 0.5 })
        keyClick(Qt.Key_Right, Qt.AltModifier)
        tryVerify(function() { return Math.abs(card.x - x0) < 0.5 })
        compare(test.historyHits, 0, "the history shortcut ran")
        card.focus = false
        test.forceActiveFocus()
        keyClick(Qt.Key_Left, Qt.AltModifier)
        compare(test.historyHits, 1, "the shortcut stopped working elsewhere")
    }

    function test_escapeClosesTheVolumePopup() {
        var row = createTemporaryObject(volumeComponent, test)
        var control = row.control
        control.forceActiveFocus(Qt.TabFocusReason)
        var slider = find(Overlay.overlay, "volumeUnderTestSlider")
        verify(slider, "no popup")
        tryVerify(function() { return slider.visible }, 2000)
        keyClick(Qt.Key_Escape)
        tryVerify(function() { return !slider.visible }, 2000,
                  "Escape left the popup open")
        control.focus = false
        control.forceActiveFocus(Qt.TabFocusReason)
        tryVerify(function() { return slider.visible }, 2000)
        keyClick(Qt.Key_Tab)
        tryVerify(function() { return slider.activeFocus }, 2000)
        keyClick(Qt.Key_Escape)
        tryVerify(function() { return !slider.visible }, 2000,
                  "Escape in the slider left the popup open")
    }

    function test_escapeClosesAHoverOpenedVolumePopup() {
        var row = createTemporaryObject(volumeComponent, test)
        var control = row.control
        row.after.forceActiveFocus(Qt.TabFocusReason)
        verify(row.after.activeFocus)
        mouseMove(control, control.width / 2, control.height / 2)
        var slider = find(Overlay.overlay, "volumeUnderTestSlider")
        verify(slider, "no popup")
        tryVerify(function() { return slider.visible }, 2000,
                  "hover did not open the popup")
        verify(!control.activeFocus && !slider.activeFocus)
        keyClick(Qt.Key_Escape)
        tryVerify(function() { return !slider.visible }, 2000,
                  "Escape left the hover-opened popup open")
    }

    function popupRectOf(controlY) {
        var place = createTemporaryObject(placedVolumeComponent, test,
                                          { controlY: controlY })
        verify(place, "the placed control did not build")
        place.control.forceActiveFocus(Qt.TabFocusReason)
        var slider = find(Overlay.overlay, "placedSlider")
        verify(slider, "no popup")
        tryVerify(function() { return slider.visible }, 2000)
        var tl = slider.mapToItem(Overlay.overlay, 0, 0)
        var ctl = place.control.mapToItem(Overlay.overlay, 0, 0)
        return { top: tl.y - 8, bottom: tl.y + slider.height + 8,
                 ctlTop: ctl.y, ctlBottom: ctl.y + place.control.height,
                 window: Overlay.overlay.height }
    }
    function test_theVolumePopupOpensDownWhenThereIsNoRoomAbove() {
        var r = popupRectOf(4)
        verify(r.top >= 0, "popup top " + r.top + " is above the window")
        verify(r.bottom <= r.window, "popup bottom " + r.bottom + " is below the window")
        verify(Math.abs(r.top - r.ctlBottom) <= 1,
               "popup is not against the button's bottom edge: " + r.top + " vs " + r.ctlBottom)
    }
    function test_theVolumePopupOpensUpWhereItFits() {
        var r = popupRectOf(300)
        verify(r.top >= 0 && r.bottom <= r.window)
        verify(Math.abs(r.bottom - r.ctlTop) <= 1,
               "popup is not against the button's top edge: " + r.bottom + " vs " + r.ctlTop)
    }
    function test_theVolumePopupStaysInsideAtTheWindowBottom() {
        var r = popupRectOf(test.height - 40)
        verify(r.top >= 0 && r.bottom <= r.window,
               "popup spans " + r.top + ".." + r.bottom + " in " + r.window)
        verify(Math.abs(r.bottom - r.ctlTop) <= 1, "did not open upward")
    }

    function test_theWheelStepsTheVolume() {
        var row = createTemporaryObject(volumeComponent, test)
        var control = row.control
        control.forceActiveFocus(Qt.TabFocusReason)
        var slider = find(Overlay.overlay, "volumeUnderTestSlider")
        verify(slider)
        tryVerify(function() { return slider.visible }, 2000)
        var before = row.output.volume
        mouseWheel(slider, slider.width / 2, slider.height / 2, 0, 120)
        verify(row.output.volume > before + 0.01,
               "the wheel did not raise the level: " + row.output.volume)
        mouseWheel(slider, slider.width / 2, slider.height / 2, 0, -240)
        verify(row.output.volume < before - 0.01,
               "the wheel did not lower the level: " + row.output.volume)
    }

    function test_theStripAndTheCardCarryTheVolume() {
        var bar = createTemporaryObject(barComponent, test)
        voice.load({})
        verify(find(bar, "voicePlayerBarVolume"), "no volume on the strip")
        var host = makeHost()
        popOut()
        settings.voiceMiniPlayerGeometry = Qt.rect(0.5, 0.5, 300, 92)
        var card = cardOf(host)
        tryCompare(card, "opacity", 1)
        var v = find(card, "voiceMiniPlayerVolume")
        verify(v && v.visible, "no volume on the card")
        compare(v.enabled, false)
    }

    function test_theSpeedChipSaysWhatAClickDoes() {
        var bar = createTemporaryObject(barComponent, test)
        voice.load({ rate: 1.0 })
        var chip = find(bar, "voicePlayerBarSpeed")
        verify(chip.ToolTip.text.indexOf("1.5") >= 0, chip.ToolTip.text)
        voice.rate = 2.0
        verify(chip.ToolTip.text.indexOf("2") >= 0
               && chip.ToolTip.text.indexOf("1×") >= 0, chip.ToolTip.text)
    }

    function test_theScrubberTooltipNamesTheTime() {
        var bar = createTemporaryObject(barComponent, test)
        voice.load({ duration: 60000, position: 1000 })
        var seek = find(bar, "voicePlayerBarSeek")
        mouseMove(seek, seek.width / 2, seek.height / 2)
        tryVerify(function() {
            return seek.ToolTip.text.indexOf("0:30") >= 0
        }, 2000, "tooltip: " + seek.ToolTip.text)
    }

    function test_aNarrowStripKeepsThePosition() {
        var bar = createTemporaryObject(barComponent, test, { width: 340 })
        voice.load({ position: 4000, duration: 60000 })
        var clock = find(bar, "voicePlayerBarClock")
        verify(clock.visible, "no clock at 340 px")
        compare(clock.text, "0:04")
        bar.width = 600
        compare(clock.text, "0:04 / 1:00")
    }

    function test_theRowInksReadInEveryTheme() {
        var row = createTemporaryObject(rowComponent, test)
        voice.load({ eventId: "$row", rate: 1.0 })
        settings.voicePlaybackRate = 2.0
        var clock = find(row, "audioClock")
        var speed = find(row, "audioSpeedButton")
        function ratio(a, b) {
            var x = AppTheme.relativeLuminance(a)
            var y = AppTheme.relativeLuminance(b)
            return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05)
        }
        var palettes = {}
        var checked = 0
        for (var mode = 1; mode <= 11; ++mode) {
            AppTheme.mode = mode
            palettes[AppTheme.accent.toString()] = true
            verify(ratio(clock.color, row.color) >= 4.5,
                   "theme " + mode + " row clock "
                   + ratio(clock.color, row.color).toFixed(2))
            verify(ratio(speed.contentItem.color, row.color) >= 4.5,
                   "theme " + mode + " row speed "
                   + ratio(speed.contentItem.color, row.color).toFixed(2))
            ++checked
        }
        compare(checked, 11)
        verify(Object.keys(palettes).length >= 8,
               "only " + Object.keys(palettes).length + " distinct palettes")
    }

    function test_theNarrowCompactCardKeepsMoreOfTheTitle() {
        var host = makeHost()
        popOut()
        settings.voiceMiniPlayerGeometry = Qt.rect(0.5, 0.5, 200, 52)
        voice.load({})
        var card = cardOf(host)
        tryCompare(card, "opacity", 1)
        compare(card.tier, "compact")
        var title = find(card, "voiceMiniPlayerSender")
        verify(title.width >= 60, "the title has " + title.width + " px")
    }
}
