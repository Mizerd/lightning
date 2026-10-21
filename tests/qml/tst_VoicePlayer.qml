import QtQuick
import QtQuick.Controls
import QtTest
import MatrixClient

// The voice/audio player's QML against a recording stand-in for
// app.voicePlayback (VoicePlaybackController) and app.settings: the docked
// strip and the popped-out card (design-media U1, B1, B2, U2, U3, U5, U7, P1),
// and the timeline row (B6, P4, U5, U7). Every case names the finding it
// pins; each fails on the code before the fix (see the case comments).
TestCase {
    id: test
    name: "VoicePlayer"
    width: 900
    height: 700
    visible: true
    when: windowShown

    // ── Stand-ins ──
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
    // The card over a scrollable view, as over the timeline.
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
    // A row inside a scrolling view, as in the timeline.
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

    function init() {
        voice.calls = []
        voice.info = {}
        voice.clipInfoRevision = 0
        voice.active = false
        voice.rate = 1.0
        settings.voicePlayerDocked = true
        settings.voiceMiniPlayerGeometry = Qt.rect(0, 0, 0, 0)
        settings.voicePlaybackRate = 1.0
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

    // ── U1: docked by default; the popped-out card only on request ──
    function test_dockedByDefault() {
        var host = makeHost()
        var bar = createTemporaryObject(barComponent, test)
        voice.load({})
        var card = cardOf(host)
        verify(!find(host, "voiceMiniPlayerHost").wanted,
               "the floating card showed while the player is docked")
        compare(card.visible, false)
        verify(bar.visible && bar.height > 20, "the strip did not show")
        // The strip's pop-out writes the setting; the card's dock writes it
        // back.
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

    // ── B1: the progress line is a seek target, not "go to message" ──
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
        // The title is still the way back.
        var info = find(card, "voiceMiniPlayerInfo")
        mouseClick(info, 10, info.height / 2)
        compare(appStub.opened.length, 1)
        compare(appStub.opened[0], "!room:x")
    }

    // ── B2 / U2: where the card shows ──
    function test_theCardShowsWhereverTheRowIsNot() {
        var host = makeHost()
        var mini = find(host, "voiceMiniPlayerHost")
        popOut()
        // In the clip's own room with its row off screen: shown (U2; it used
        // to hide whenever the room was open).
        voice.load({ rowOnScreen: false })
        appStub.currentRoomId = "!room:x"
        verify(mini.wanted, "no player in the clip's room with its row away")
        voice.rowOnScreen = true
        verify(!mini.wanted, "the card showed beside its own visible row")
        // Settings hides the rows: shown again (B2).
        appStub.currentScreen = 2
        verify(mini.wanted, "no player while Settings is open")
        appStub.currentScreen = 1
        voice.stop()
        verify(!mini.wanted)
    }

    // ── Free move, gentle snap, persistence relative to the window ──
    function test_theCardMovesFreelyAndRemembersItsPlace() {
        var host = makeHost()
        popOut()
        voice.load({})
        var mini = find(host, "voiceMiniPlayerHost")
        var card = cardOf(host)
        tryCompare(card, "opacity", 1)
        // Grab it by its avatar slot (no control there) and drop it in the
        // middle of the window.
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
        // A window resize keeps the relative place and the card inside.
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
        // Dropped close to the left edge: settles against it.
        var p = avatar.mapToItem(test, avatar.width / 2, avatar.height / 2)
        var travel = card.x - mini.area.x - 10
        mousePress(test, p.x, p.y)
        for (var j = 1; j <= 10; ++j)
            mouseMove(test, p.x - travel * j / 10, p.y)
        mouseRelease(test, p.x - travel, p.y)
        tryCompare(card, "x", mini.area.x)
        compare(settings.voiceMiniPlayerGeometry.x, 0)
    }

    // ── Resize with min/max and reflow ──
    function test_theCardResizesAndReflows() {
        var host = makeHost()
        popOut()
        voice.load({ waveform: [10, 50, 90, 40] })
        var mini = find(host, "voiceMiniPlayerHost")
        var card = cardOf(host)
        tryCompare(card, "opacity", 1)
        compare(card.tier, "regular")
        // Bottom-right corner, dragged far out: bounded by the maximum.
        var corner = find(card, "voiceMiniPlayerResize_br")
        verify(corner, "no resize handle")
        compare(corner.cursorShape, Qt.SizeFDiagCursor)
        var c = corner.mapToItem(test, corner.width / 2, corner.height / 2)
        // Room to grow: move the card to the top-left first.
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
        // ...and down past the minimum.
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

    // ── Never off screen ──
    function test_aRememberedPlaceCanNeverBeOffScreen() {
        var host = makeHost()
        popOut()
        // Remembered from a big window: far corner, large.
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
        // A corrupt or hostile value reads as fractions inside the area.
        settings.voiceMiniPlayerGeometry = Qt.rect(7, -3, 300, 92)
        host.width = 900
        host.height = 700
        wait(0)
        verify(card.x + card.width <= host.width && card.y >= 0)
    }

    // ── Keyboard alternatives (U5) ──
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

    // ── U3: speed, next, call pause, thread, track name ──
    function test_theBarSaysWhatPlaysAndWhy() {
        var bar = createTemporaryObject(barComponent, test)
        var host = makeHost()
        popOut()
        voice.load({ threadRootId: "$root", eventId: "$reply",
                     upNext: [{ eventId: "$n", senderName: "Bob" }] })
        // The card says the same (it used to show the room name alone).
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
        // An audio file: its own title and artist.
        voice.load({ isVoice: false, filename: "song.mp3",
                     trackTitle: "Short Test Song", trackArtist: "Band" })
        compare(title.text, "Short Test Song")
        compare(sub.text, "Band · Lounge")
        // The speed chip steps and shows the clip's speed.
        var chip = find(bar, "voicePlayerBarSpeed")
        voice.calls = []
        mouseClick(chip)
        compare(voice.made("rate:"), ["rate:1.5"])
    }

    // ── U7: text size ──
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

    // ── P1: the card's surface and inks in every theme ──
    // Measured on the card as drawn: its colour against both bubbles and the
    // canvas, and every label's colour against it (a failure line too).
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
        // The loop really varied the palette (CLAUDE.md §16).
        verify(Object.keys(palettes).length >= 8,
               "only " + Object.keys(palettes).length + " distinct palettes")
    }

    // ── The timeline row ──
    function test_anIdleRowLightsNoBarAndFillsItsWidth() {
        var row = createTemporaryObject(rowComponent, test,
                                        { waveform: [50, 60, 70, 80, 90, 40,
                                                     30, 20, 10, 50] })
        var wave = find(row, "voiceSeekWave")
        verify(wave && wave.visible, "no waveform drawn")
        // The Row places its bars on the next polish.
        waitForRendering(row)
        var lit = 0
        var right = 0
        for (var i = 0; i < wave.children.length; ++i) {
            var bar = wave.children[i]
            if (bar.width <= 0 || bar.height <= 0)
                continue
            if (Qt.colorEqual(bar.color, AppTheme.accent))
                ++lit
            right = Math.max(right, bar.x + bar.width)
        }
        compare(lit, 0, "an idle row lights " + lit + " bar(s)")
        verify(right >= wave.width * 0.95,
               "ten buckets cover " + right + " of " + wave.width
               + " px: dead space after a short waveform")
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
        // Keyboard: the bar is focusable and steps 5 s.
        voice.load({ eventId: "$row", position: 10000, duration: 60000 })
        seek.forceActiveFocus()
        verify(seek.activeFocus, "the waveform takes no keyboard focus")
        voice.calls = []
        keyClick(Qt.Key_Right)
        compare(voice.made("seek:"), ["seek:15000"])
        keyClick(Qt.Key_Home)
        compare(voice.made("seek:")[1], "seek:0")
    }

    function test_anAudioFileKeepsItsHeightAndShowsItsTitle() {
        var row = createTemporaryObject(rowComponent, test,
                                        { isVoice: false,
                                          filename: "Short Test Song.mp3" })
        var title = find(row, "audioTitle")
        compare(title.text, "Short Test Song.mp3")
        var before = row.implicitHeight
        var seekBefore = find(row, "audioSeekSlider").width
        // An earlier play found the tags and a cover.
        voice.info = { "mk-row": { title: "Short Test Song", artist: "Band",
                                   artworkSource: "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==" } }
        voice.clipInfoRevision++
        compare(title.text, "Short Test Song")
        verify(find(row, "audioClock").text.indexOf("Band") >= 0)
        wait(50)
        compare(row.implicitHeight, before,
                "the cover changed the row's height")
        // Playing it changes neither the height nor the scrubber's width.
        voice.load({ eventId: "$row", isVoice: false, loaded: true })
        wait(0)
        compare(row.implicitHeight, before)
        compare(find(row, "audioSeekSlider").width, seekBefore,
                "the scrubber shrank when playback started")
    }

    function test_anIdleVoiceRowShowsTheVoiceSpeed() {
        settings.voicePlaybackRate = 1.5
        var row = createTemporaryObject(rowComponent, test)
        var speed = find(row, "audioSpeedButton")
        verify(speed.visible && speed.enabled,
               "an idle row hides the speed it will play at")
        compare(speed.rate, 1.5)
        // A file plays at 1x whatever the voice speed.
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
        // A click hides the tooltip it would have contradicted.
        mouseMove(button, button.width / 2, button.height / 2)
        mouseClick(button)
        compare(button.tipSuppressed, true)
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
        // Another clip's row never claims to show this one.
        var other = createTemporaryObject(rowComponent, test,
                                          { eventId: "$other" })
        reports = voice.made("row:$other:")
        compare(reports[reports.length - 1], "row:$other:false")
    }

    // ── The shared volume control is reachable and usable by keyboard (U5;
    // the media-video package relies on it). It used to be a hover popup
    // Tab never entered.
    function test_theVolumeSliderIsATabStop() {
        var row = createTemporaryObject(volumeComponent, test)
        var control = row.control
        control.forceActiveFocus(Qt.TabFocusReason)
        verify(control.activeFocus)
        // Up on the button steps the level, and remembers it.
        keyClick(Qt.Key_Up)
        compare(Math.round(row.output.volume * 100), 55)
        compare(Math.round(settings.mediaVolume * 100), 55)
        verify(control.Accessible.description.indexOf("55%") >= 0,
               control.Accessible.description)
        // Tab goes into the slider.
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
        // Tab leaves it for the next control.
        keyClick(Qt.Key_Tab)
        tryVerify(function() { return row.after.activeFocus },
                  2000, "Tab stayed in the slider")
    }

    // Live finding (xdotool, 2026-10-20): a downward drag of the card over
    // the timeline was stolen by the timeline beneath it, which scrolled
    // instead, and the card stopped after the first steps. NOT COVERAGE for
    // that fix: this case, and a second attempt over the real TimelinePane
    // in voice-player-dock-qml (review round 1, since removed), both pass
    // with the old DragHandler too; synthesized events do not reproduce the
    // steal. It only pins that a drag on the card moves the card and not a
    // plain Flickable beneath it. The live xdotool drag is the evidence.
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

    // U2, measured live: the timeline's rowOnScreen is an activation band a
    // viewport wide on each side, so a row scrolled well away still counted
    // as on screen and the popped-out card never appeared in its own room.
    // The current row now reports its real geometry in its scrolling view.
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

    // Review round 1 (M1): the shortcut's focus on a strip, then Space,
    // plays or pauses and never opens the message: the strip under a header
    // (TimelinePane) and the one along the bottom (Main.qml's fallback).
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
        // The link, if Tab put focus there, takes Space the same way.
        voice.calls = []
        find(bar, "voicePlayerBarSource").forceActiveFocus()
        keyClick(Qt.Key_Space)
        compare(appStub.opened.length, 0)
        compare(voice.made("toggle").length, 1)
    }

    // Review round 1 (L2): a press or drag on the card does not take the
    // keyboard focus (it left the composer and drew the ring).
    function test_aDragDoesNotTakeTheFocus() {
        var host = makeHost()
        popOut()
        voice.load({})
        var card = cardOf(host)
        tryCompare(card, "opacity", 1)
        // On the card itself (the avatar), not on a control.
        var avatar = find(card, "voiceMiniPlayerAvatar")
        var at = avatar.mapToItem(card, avatar.width / 2, avatar.height / 2)
        mouseDrag(card, at.x, at.y, -60, -40)
        verify(!card.activeFocus, "a drag took the keyboard focus")
        mouseClick(card, at.x, at.y)
        verify(!card.activeFocus, "a click took the keyboard focus")
        compare(card.border.width, 1)
    }

    // Review round 1 (L1, L9): an idle row's volume button is not shown as
    // muted; the compact card still names what plays.
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

    // Review round 1 (L7): accessible names do not change with the position.
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
}
