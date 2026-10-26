import QtQuick
import QtQuick.Controls
import QtMultimedia
import QtTest
import MatrixClient

// The inline video card, its control bar and the expanded view, against the
// production sources with a recording playback controller and a bridge that
// hands out a sound from the tree (what matters is a source that loads, seeks
// and ends; this tree cannot assume a video codec). Every case here failed on
// the code the 2026-10-20 design review measured:
//   B4  the expanded bar read 1× over a video playing at 1.5×
//   B5  the speed menu dropped over its own button and the composer, and the
//       bar faded out from under it
//   U4  a forced stop forgot the position; the expanded view snapped shut at
//       the end
//   U5  no keyboard seek or volume on the inline card; no spoken position
//   U6  a 260 px portrait card left the seek track ~45 px
//   P3  the expanded view had no header and a 10 px time label
// Audio goes nowhere: scripts/test-video-player.sh runs this with no sound
// server, and every card starts muted.
TestCase {
    id: test
    name: "VideoPlayer"
    width: 800
    height: 760
    visible: true
    when: windowShown

    readonly property url fixture: Qt.resolvedUrl("../../data/sounds/ring.wav")

    QtObject {
        id: playback
        property string audibleOwner: ""
        property int stopGeneration: 0
        property real videoPlaybackRate: 1.0
        property int resumeRevision: 0
        property var remembered: []
        property var finished: []
        property var resume: ({})
        signal togglePlayPauseRequested(string ownerKey)
        function acquire(k) { if (k.length > 0) audibleOwner = k }
        function release(k) { if (audibleOwner === k) audibleOwner = "" }
        function owns(k) { return k.length > 0 && audibleOwner === k }
        function rememberVideoPosition(k, p, d) {
            remembered.push({ key: k, pos: p, dur: d })
        }
        function markVideoFinished(k) { finished.push(k) }
        function videoResumePosition(k) { return resume[k] || 0 }
        function videoFinished(k) { return finished.indexOf(k) >= 0 }
    }
    QtObject {
        id: settings
        property real mediaVolume: 0.8
    }
    QtObject {
        id: bridge
        property bool supported: true
        signal playableMediaReady(string cacheKey)
        signal mediaFetchFailed(string cacheKey, string category)
        signal saveFinished(bool ok, string message, string mediaKey)
        // True while a fetch is "in flight": playableSource has nothing yet
        // and the card waits for playableMediaReady.
        property bool holdFetch: false
        // A source that is not media at all: the player fails to decode it.
        property bool brokenSource: false
        function playableSource(key) {
            if (holdFetch) return ""
            return brokenSource ? Qt.resolvedUrl("tst_VideoPlayer.qml").toString()
                                : test.fixture.toString()
        }
        function failureCategory(key) { return "" }
        function cancelPlayable(key) {}
        function pinPlayable(key) {}
        function unpinPlayable(key) {}
        function retry(key) {}
    }
    property var app: ({ playback: playback, mediaBridge: bridge,
                         settings: settings, softwareRenderer: false })
    // pwcrash's play gate: absent (null) except in the case that installs it.
    property var qtAudio: null
    QtObject {
        id: gate
        property int plays: 0
        property int cancels: 0
        signal playGivenUp(var p)
        function play(p) { plays++; p.play() }
        function cancelPlay(p) { cancels++ }
    }

    Component {
        id: cardComponent
        VideoPlayerCard {
            mediaKey: "$video"
            ownerKey: "owner-video"
            filename: "clip.mp4"
            startMuted: true
        }
    }

    // Two bars on one player, as the card and the expanded view are.
    Component {
        id: twoBarsComponent
        Item {
            width: 800
            height: 600
            property alias player: mp
            property alias first: firstBar
            property alias second: secondBar
            MediaPlayer {
                id: mp
                audioOutput: AudioOutput {
                    id: ao
                    muted: true
                    property bool userUnmuted: false
                }
            }
            VideoControlBar {
                id: firstBar
                y: 260
                width: 480
                player: mp
                audio: ao
            }
            VideoControlBar {
                id: secondBar
                y: 540
                width: 760
                player: mp
                audio: ao
                showClose: false
            }
        }
    }

    function cleanup() {
        test.qtAudio = null
        AppTheme.textScale = 1.0
    }

    function init() {
        playback.audibleOwner = ""
        playback.videoPlaybackRate = 1.0
        playback.remembered = []
        playback.finished = []
        playback.resume = ({})
        settings.mediaVolume = 0.8
        bridge.holdFetch = false
        bridge.brokenSource = false
    }

    // ---- helpers ----
    function barOf(card) { return findChild(card, "videoControlBar") }
    function playerOf(card) { return barOf(card).player }
    function overlayOf(card) { return findChild(card, "videoViewerOverlay") }
    function inOverlay(card, name) {
        var overlay = overlayOf(card)
        return overlay && overlay.contentItem
               ? findChild(overlay.contentItem, name) : null
    }
    // The bar's speed menu (a Popup, so not a visual child).
    function menuOf(bar) {
        for (var i = 0; i < bar.data.length; ++i) {
            var o = bar.data[i]
            if (o && typeof o.popup === "function")
                return o
        }
        return null
    }
    function speedText(bar) {
        return textWith(findChild(bar, "videoSpeedButton"), "×")
    }
    function textWith(item, needle) {
        if (!item)
            return ""
        if (typeof item.text === "string" && item.text.indexOf(needle) >= 0)
            return item.text
        var kids = item.children || []
        for (var i = 0; i < kids.length; ++i) {
            var t = textWith(kids[i], needle)
            if (t.length > 0)
                return t
        }
        return ""
    }
    function windowRect(item) {
        var p = item.mapToItem(null, 0, 0)
        return { x: p.x, y: p.y, w: item.width, h: item.height }
    }
    function inside(item, container) {
        var a = item.mapToItem(container, 0, 0)
        return a.x >= -0.5 && a.y >= -0.5
               && a.x + item.width <= container.width + 0.5
               && a.y + item.height <= container.height + 0.5
    }
    function loadedCard(props) {
        var card = createTemporaryObject(cardComponent, test, props)
        verify(card)
        var player = playerOf(card)
        tryVerify(function() { return player.seekable && player.duration > 0 },
                  10000, "the fixture never loaded")
        return card
    }

    // ---- B4: one rate for every bar ----
    function test_everyBarShowsThePlayersRate() {
        var host = createTemporaryObject(twoBarsComponent, test)
        verify(host)
        compare(speedText(host.first), "1×")
        compare(speedText(host.second), "1×")

        // Choose 1.5× from the first bar's menu, as the user did inline.
        mouseClick(findChild(host.first, "videoSpeedButton"))
        var menu = menuOf(host.first)
        verify(menu)
        tryCompare(menu, "opened", true)
        // Its rows are laid out (a click before that lands on nothing).
        tryVerify(function() { return menu.height > 100 })
        var item = null
        for (var i = 0; i < menu.count; ++i) {
            var row = menu.itemAt(i)
            if (row && row.text === "1.5×")
                item = row
        }
        verify(item, "no 1.5× row in the speed menu")
        mouseClick(item)
        tryCompare(menu, "visible", false)

        compare(host.player.playbackRate, 1.5)
        compare(speedText(host.first), "1.5×")
        // The expanded bar used to keep its own index and read 1×.
        compare(speedText(host.second), "1.5×",
                "the second bar does not show the player's rate")
        // And the choice is the session's video speed.
        compare(playback.videoPlaybackRate, 1.5)

        // A rate set anywhere else is what both bars show.
        host.player.playbackRate = 2.0
        compare(speedText(host.first), "2×")
        compare(speedText(host.second), "2×")
    }

    // The session speed reaches a new player, so a resumed video does not
    // fall back to 1×.
    function test_aNewCardStartsAtTheSessionSpeed() {
        playback.videoPlaybackRate = 1.5
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        compare(playerOf(card).playbackRate, 1.5)
        compare(speedText(barOf(card)), "1.5×")
    }

    // ---- B5: the speed menu opens upward and the bar stays ----
    function test_theSpeedMenuOpensUpward() {
        var card = loadedCard({ width: 480, height: 270, y: 330 })
        var bar = barOf(card)
        var player = playerOf(card)
        tryCompare(player, "playbackState", MediaPlayer.PlayingState)
        mouseMove(card, card.width / 2, card.height / 2)
        tryCompare(bar, "opacity", 1.0)

        var button = findChild(bar, "videoSpeedButton")
        verify(button.visible)
        mouseClick(button)
        var menu = menuOf(bar)
        verify(menu)
        tryCompare(menu, "opened", true)
        wait(50)
        // In window coordinates: the menu ends above the button that opened
        // it, and starts inside the card rather than over the composer.
        var b = windowRect(button)
        var top = menu.parent.mapToItem(null, menu.x, menu.y).y
        var bottom = top + menu.height
        verify(bottom <= b.y + 0.5,
               "the speed menu runs over its own button: bottom " + bottom
               + ", button top " + b.y)
        // Nothing of it below the card, where the composer is.
        var c = windowRect(card)
        verify(bottom <= c.y + c.h + 0.5,
               "the speed menu runs below the card: bottom " + bottom
               + ", card bottom " + (c.y + c.h))
        menu.close()
    }

    function test_theBarStaysWhileItsSpeedMenuIsOpen() {
        var card = loadedCard({ width: 480, height: 270, y: 330 })
        var bar = barOf(card)
        var player = playerOf(card)
        tryCompare(player, "playbackState", MediaPlayer.PlayingState)
        mouseMove(card, card.width / 2, card.height / 2)
        tryCompare(bar, "opacity", 1.0)
        mouseClick(findChild(bar, "videoSpeedButton"))
        var menu = menuOf(bar)
        tryCompare(menu, "opened", true)

        // The pointer leaves the card while the menu is open: the bar it
        // belongs to must not fade out from under it.
        mouseMove(test, 4, 4)
        wait(400)
        verify(player.playbackState === MediaPlayer.PlayingState)
        verify(bar.shown, "the control bar hid while its speed menu was open")
        compare(bar.opacity, 1.0)
        menu.close()
    }

    // ---- U6: a portrait card keeps a scrubbable track ----
    function test_aNarrowCardKeepsAScrubbableTrack() {
        var cases = [
            { w: 260, h: 440, speed: false },
            { w: 280, h: 158, speed: true },
            { w: 340, h: 192, speed: true },
        ]
        for (var i = 0; i < cases.length; ++i) {
            var c = cases[i]
            var card = createTemporaryObject(cardComponent, test,
                                             { width: c.w, height: c.h })
            verify(card)
            var bar = barOf(card)
            tryVerify(function() { return bar.width === c.w })
            var slider = findChild(bar, "videoSeekSlider")
            verify(slider.width >= 120,
                   c.w + " px card: the seek track is " + slider.width + " px")
            var required = ["videoPlayPauseButton", "videoSeekSlider",
                            "videoTimeLabel", "videoMuteButton",
                            "videoExpandButton"]
            for (var j = 0; j < required.length; ++j) {
                var control = findChild(bar, required[j])
                verify(control, required[j])
                verify(control.visible, c.w + " px: " + required[j])
                verify(inside(control, card), c.w + " px: " + required[j]
                       + " is outside the card")
            }
            compare(findChild(bar, "videoSpeedButton").visible, c.speed,
                    c.w + " px: speed visibility")
            // Close is still one click away: in the card's corner.
            var close = findChild(card, "videoCardCloseButton")
            verify(close, c.w + " px: no close button left")
            verify(close.visible && inside(close, card),
                   c.w + " px: the corner close is not on the card")
            compare(findChild(bar, "videoCloseButton").visible, false)
            card.destroy()
        }
        // A wide card keeps the single row, close included.
        var wide = createTemporaryObject(cardComponent, test,
                                         { width: 480, height: 270 })
        var wideBar = barOf(wide)
        tryVerify(function() { return wideBar.width === 480 })
        verify(findChild(wideBar, "videoCloseButton").visible)
        verify(!findChild(wide, "videoCardCloseButton").visible)
        verify(findChild(wideBar, "videoSeekSlider").width >= 120)
    }

    // ---- U5: the inline card's keyboard map ----
    function test_theInlineCardSeeksFromTheKeyboard() {
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        var bar = barOf(card)
        var player = playerOf(card)
        player.pause()
        player.position = 500
        tryVerify(function() { return Math.abs(player.position - 500) < 100 })

        var play = findChild(bar, "videoPlayPauseButton")
        play.forceActiveFocus(Qt.TabFocusReason)
        verify(play.activeFocus)
        keyClick(Qt.Key_Right)
        tryVerify(function() { return player.position >= 5000 }, 3000,
                  "Right on the focused card did not seek 5 s")

    }

    function test_theSeekTrackStepsFiveSecondsAndSpeaksItsPosition() {
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        var bar = barOf(card)
        var player = playerOf(card)
        player.pause()
        player.position = 5500
        tryVerify(function() { return Math.abs(player.position - 5500) < 100 })
        // On the track itself, Left is 5 s too, not a tenth of a ms.
        var slider = findChild(bar, "videoSeekSlider")
        slider.forceActiveFocus(Qt.TabFocusReason)
        keyClick(Qt.Key_Left)
        tryVerify(function() { return player.position < 1000 }, 3000,
                  "Left on the seek track moved " + player.position)
        // It speaks its position.
        verify(slider.Accessible.description.indexOf("0:00") === 0
               && slider.Accessible.description.indexOf("0:06") > 0,
               "the seek track has no spoken position: '"
               + slider.Accessible.description + "'")

    }

    // Master prompt Part 5 §5.4: hovering the seek track names the time a
    // click would land on, without decoding a frame.
    function test_hoveringTheSeekTrackShowsTheTimeUnderThePointer() {
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        var bar = barOf(card)
        var player = playerOf(card)
        player.pause()
        var slider = findChild(bar, "videoSeekSlider")
        var tip = null
        for (var i = 0; i < slider.data.length; ++i)
            if (slider.data[i] && slider.data[i].objectName === "videoSeekTimeTip")
                tip = slider.data[i]
        verify(tip, "the seek track has no time tip")
        verify(!tip.visible)
        var x = slider.leftPadding + slider.availableWidth * 0.5
        mouseMove(slider, x, slider.height / 2)
        tryVerify(function() { return tip.visible }, 2000,
                  "hovering the track showed no time")
        compare(tip.text, AppTheme.clockPosition(player.duration * 0.5))
        mouseMove(card, 5, 5)
        tryVerify(function() { return !tip.visible })
    }

    // Long media reads h:mm:ss, not a growing minute count.
    function test_theMediaClockIsHourAware() {
        compare(AppTheme.clockText(0), "0:00")
        compare(AppTheme.clockText(59), "0:59")
        compare(AppTheme.clockText(3599), "59:59")
        compare(AppTheme.clockText(3600), "1:00:00")
        compare(AppTheme.clockText(3723), "1:02:03")
        compare(AppTheme.clockPosition(3723999), "1:02:03")
    }

    function test_tabReachesTheVolumeAndTheKeysSetIt() {
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        var bar = barOf(card)
        playerOf(card).pause()
        // Tab reaches the volume control, and Up/Down set the level the way
        // the slider does: remembered, and an explicit unmute.
        var play = findChild(bar, "videoPlayPauseButton")
        play.forceActiveFocus(Qt.TabFocusReason)
        var volume = findChild(bar, "videoMuteButton")
        for (var i = 0; i < 6 && !volume.activeFocus; ++i)
            keyClick(Qt.Key_Tab)
        verify(volume.activeFocus, "Tab never reached the volume control")
        var audio = bar.audio
        var before = audio.volume
        keyClick(Qt.Key_Up)
        tryVerify(function() { return audio.volume > before + 0.04 }, 1000,
                  "Up on the volume control did not raise the level")
        verify(audio.userUnmuted)
        verify(!audio.muted)
        compare(settings.mediaVolume, audio.volume)
        keyClick(Qt.Key_Down)
        tryVerify(function() { return Math.abs(audio.volume - before) < 0.01 })
        keyClick(Qt.Key_M)
        verify(audio.muted, "M did not mute the inline card")
    }

    // ---- U4: a forced stop remembers the position; a new card resumes ----
    function test_aForcedStopRemembersWhereTheVideoWas() {
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        var player = playerOf(card)
        player.pause()
        player.position = 4000
        tryVerify(function() { return Math.abs(player.position - 4000) < 100 })
        // A room switch: AppController's stopAll() bumps the generation.
        playback.stopGeneration += 1
        compare(playback.remembered.length, 1,
                "a forced stop did not remember the position")
        compare(playback.remembered[0].key, "$video")
        verify(Math.abs(playback.remembered[0].pos - 4000) < 100)
        verify(playback.remembered[0].dur > 5000)
        compare(player.source.toString(), "")

        // The next Play resumes there.
        playback.resume = ({ "$video": 4200 })
        var again = loadedCard({ width: 480, height: 270, y: 300 })
        var p2 = playerOf(again)
        tryVerify(function() { return p2.position >= 4100 }, 5000,
                  "the new card started at " + p2.position
                  + " instead of resuming at 4.2 s")
    }

    // Delegate reuse changes mediaKey before the reset: the position is the
    // OLD video's.
    function test_aReusedCardRemembersUnderTheVideoItPlayed() {
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        var player = playerOf(card)
        player.pause()
        player.position = 3500
        tryVerify(function() { return Math.abs(player.position - 3500) < 100 })
        card.mediaKey = "$other"
        compare(playback.remembered.length, 1)
        compare(playback.remembered[0].key, "$video")
    }

    // ---- P3: the expanded view's header and type ----
    function test_theExpandedViewHasAHeaderAndReadableTime() {
        var card = loadedCard({ width: 480, height: 270, y: 300,
                                senderName: "Fixture Sender",
                                timestamp: new Date(2026, 9, 20, 14, 5),
                                canSave: true })
        var spySave = createTemporaryObject(spyComponent, test,
                                            { target: card,
                                              signalName: "saveRequested" })
        mouseClick(findChild(barOf(card), "videoExpandButton"))
        var overlay = overlayOf(card)
        tryCompare(overlay, "opened", true)

        // P3: a header with the name, sender and date, Download and Close,
        // and a time label that can be read across the room.
        var title = inOverlay(card, "videoViewerTitle")
        verify(title, "the expanded view has no title")
        compare(title.text, "clip.mp4")
        var subtitle = inOverlay(card, "videoViewerSubtitle")
        verify(subtitle && subtitle.text.indexOf("Fixture Sender") === 0)
        verify(subtitle.text.indexOf("2026") > 0)
        var download = inOverlay(card, "videoViewerDownloadButton")
        verify(download && download.visible)
        mouseClick(download)
        compare(spySave.count, 1)
        verify(inOverlay(card, "videoViewerCloseButton").visible)
        var overlayBar = inOverlay(card, "videoOverlayControlBar")
        verify(findChild(overlayBar, "videoTimeLabel").font.pixelSize >= 13,
               "the expanded view's time label is "
               + findChild(overlayBar, "videoTimeLabel").font.pixelSize
               + " px")
        overlay.close()
    }

    function test_theExpandedViewStaysOpenAtTheEndWithReplay() {
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        var spyClose = createTemporaryObject(spyComponent, test,
                                             { target: card,
                                               signalName: "closeRequested" })
        var player = playerOf(card)
        mouseClick(findChild(barOf(card), "videoExpandButton"))
        var overlay = overlayOf(card)
        tryCompare(overlay, "opened", true)

        // Play to the real end (6 s at 4×).
        player.playbackRate = 4.0
        player.play()
        tryVerify(function() {
            return player.mediaStatus === MediaPlayer.EndOfMedia
                   || !overlay.opened
        }, 10000)
        wait(50)
        verify(overlay.opened, "the expanded view closed itself at the end")
        compare(playback.finished.length, 1)
        var replay = inOverlay(card, "videoReplayButton")
        verify(replay && replay.visible, "no Replay at the end")
        compare(spyClose.count, 0)

        mouseClick(replay)
        tryCompare(player, "playbackState", MediaPlayer.PlayingState)
        verify(!replay.visible)
        verify(overlay.opened)

        // Ending again and then leaving the view finishes inline: the card
        // closes to the cover, which offers Replay.
        player.pause()
        card.handleEndOfMedia()
        verify(overlay.opened)
        overlay.close()
        tryCompare(spyClose, "count", 1)
        compare(player.source.toString(), "")
    }

    // Inline, the end still returns to the cover, now marked watched.
    function test_anInlineEndReturnsToTheCoverMarkedWatched() {
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        var spyClose = createTemporaryObject(spyComponent, test,
                                             { target: card,
                                               signalName: "closeRequested" })
        card.handleEndOfMedia()
        compare(spyClose.count, 1)
        compare(playback.finished, ["$video"])
        // A watched video is not also "resume from the end".
        compare(playback.remembered.length, 0)
    }

    // The overlay's Up/Down go through the same path as the slider now:
    // remembered, and an explicit unmute of a muted-start card.
    function test_theExpandedViewsVolumeKeysAreRemembered() {
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        mouseClick(findChild(barOf(card), "videoExpandButton"))
        var overlay = overlayOf(card)
        tryCompare(overlay, "opened", true)
        overlay.contentItem.forceActiveFocus()
        var audio = barOf(card).audio
        verify(audio.muted)
        keyClick(Qt.Key_Up)
        verify(audio.userUnmuted, "Up in the expanded view left the card muted")
        verify(!audio.muted)
        compare(settings.mediaVolume, audio.volume)
        overlay.close()
    }

    // ---- pwcrash: every play goes through the audio gate when there is one
    function test_playsGoThroughTheAudioGateWhenThereIsOne() {
        gate.plays = 0
        gate.cancels = 0
        test.qtAudio = gate
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        verify(gate.plays >= 1, "the card's first play bypassed the gate")
        var c0 = gate.cancels
        mouseClick(findChild(barOf(card), "videoPlayPauseButton"))
        verify(gate.cancels > c0, "a pause left a gated play pending")
        var p0 = gate.plays
        mouseClick(findChild(barOf(card), "videoPlayPauseButton"))
        verify(gate.plays > p0, "the bar's play bypassed the gate")
        gate.playGivenUp(playerOf(card))
        compare(card.fetchState, "failed",
                "a play the gate gave up on left the card spinning")
        // It says why: not "this video cannot be played" (review R2-4).
        var why = findChild(card, "videoFailedText")
        verify(why && why.visible)
        verify(why.text.indexOf("sound system") >= 0,
               "the give-up reads as a broken video: " + why.text)
    }

    // A play waiting on the gate is cancelled when the row leaves the
    // screen, playing or not (review R2-3).
    QtObject {
        id: holdingGate
        property int plays: 0
        property int cancels: 0
        signal playGivenUp(var p)
        // Holds every play: the player never starts.
        function play(p) { plays++ }
        function cancelPlay(p) { cancels++ }
    }
    function test_aWaitingPlayIsCancelledWhenTheRowLeavesTheScreen() {
        holdingGate.plays = 0
        holdingGate.cancels = 0
        test.qtAudio = holdingGate
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        verify(holdingGate.plays >= 1)
        verify(playerOf(card).playbackState !== MediaPlayer.PlayingState)
        var c0 = holdingGate.cancels
        card.rowOnScreen = false
        verify(holdingGate.cancels > c0,
               "a held play survived the row leaving the screen")
    }

    // ---- M-F02: a slow Play that completes off screen stays paused ----
    // The fetch that a Play started can finish after the reader scrolled the
    // row away. start() used to acquire audibility and play regardless, so
    // a video nobody could see started making sound.
    function test_aFetchThatLandsOffScreenGivesAPausedReadyCard() {
        holdingGate.plays = 0
        holdingGate.cancels = 0
        test.qtAudio = holdingGate
        bridge.holdFetch = true
        var card = createTemporaryObject(cardComponent, test,
                                         { width: 480, height: 270, y: 300 })
        verify(card)
        compare(card.fetchState, "fetching")
        card.rowOnScreen = false
        bridge.holdFetch = false
        bridge.playableMediaReady("full:$video")
        verify(card.ready, "the landed file was not handed to the player")
        compare(holdingGate.plays, 0,
                "a fetch landing off screen started playback")
        verify(!playback.owns("owner-video"),
               "a fetch landing off screen claimed the audio")
        compare(card.fetchState, "idle")
    }
    // The same landing on screen still plays: the gate is visibility, not a
    // blanket refusal to start after a fetch.
    function test_aFetchThatLandsOnScreenStillPlays() {
        holdingGate.plays = 0
        test.qtAudio = holdingGate
        bridge.holdFetch = true
        var card = createTemporaryObject(cardComponent, test,
                                         { width: 480, height: 270, y: 300 })
        bridge.holdFetch = false
        bridge.playableMediaReady("full:$video")
        compare(holdingGate.plays, 1)
        verify(playback.owns("owner-video"))
    }

    // ---- M-F07: the spinner says which wait it is ----
    // A file still downloading and a player still opening its local copy
    // were one anonymous spinner; the download is not the decoder's buffer.
    function test_theSpinnerSaysDownloadingUntilTheFileIsHere() {
        bridge.holdFetch = true
        var card = createTemporaryObject(cardComponent, test,
                                         { width: 480, height: 270, y: 300 })
        compare(card.loadPhase, "downloading")
        var label = findChild(card, "videoLoadPhaseLabel")
        verify(label && label.visible, "the download wait is unnamed")
        compare(label.text, "Downloading…")
        bridge.holdFetch = false
        bridge.playableMediaReady("full:$video")
        verify(card.loadPhase !== "downloading",
               "the card still says it is downloading a file it has")
        tryVerify(function() { return card.loadPhase === "" }, 10000)
        verify(!label.visible)
    }

    // ---- M-F08: track menus only when there is a choice ----
    // The fixture has one audio track and no subtitles, so no menu; the
    // label falls back to the track's position when the container names
    // nothing.
    function test_noTrackMenuWithoutAChoice() {
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        var bar = barOf(card)
        verify(!bar.hasTrackChoice)
        verify(!findChild(bar, "videoTracksButton").visible,
               "a track menu with nothing to choose")
        compare(bar.trackLabel(null, 1), "Track 2")
    }

    // ---- M-F06: a failure says which kind it is ----
    // Every failure used to read "This video cannot be played" with a Retry,
    // including a permanent server refusal and a file this device cannot
    // decode, where retrying the same bytes changes nothing.
    function failureText(card) {
        var t = findChild(card, "videoFailedText")
        return t && t.visible ? t.text : ""
    }
    function test_aGoneVideoSaysSoAndOffersNoRetry() {
        bridge.holdFetch = true
        var card = createTemporaryObject(cardComponent, test,
                                         { width: 480, height: 270, y: 300 })
        bridge.mediaFetchFailed("full:$video", "not_found")
        compare(card.fetchState, "failed")
        verify(failureText(card).indexOf("no longer available") >= 0,
               failureText(card))
        verify(!findChild(card, "videoRetryButton").visible,
               "a permanent failure offers a Retry that cannot help")
    }
    function test_aNetworkFailureKeepsItsRetry() {
        bridge.holdFetch = true
        var card = createTemporaryObject(cardComponent, test,
                                         { width: 480, height: 270, y: 300 })
        bridge.mediaFetchFailed("full:$video", "timeout")
        verify(failureText(card).indexOf("could not be downloaded") >= 0,
               failureText(card))
        verify(findChild(card, "videoRetryButton").visible)
        verify(!findChild(card, "videoFailedDownloadButton").visible)
    }
    function test_anUndecodableFileSaysItIsTheFormatAndOffersDownload() {
        bridge.brokenSource = true
        var card = createTemporaryObject(cardComponent, test,
                                         { width: 480, height: 270, y: 300,
                                           canSave: true })
        tryCompare(card, "fetchState", "failed", 10000)
        verify(failureText(card).indexOf("format") >= 0, failureText(card))
        verify(!findChild(card, "videoRetryButton").visible)
        verify(findChild(card, "videoFailedDownloadButton").visible)
    }

    // ---- M-F03: the expanded view owns playback while it is open ----
    // The row under an expanded video can leave the viewport (resize, media
    // reflow, a timeline update). The offscreen pause used to fire anyway and
    // paused the video the user was watching full-size.
    function test_theExpandedViewIsNotPausedWhenItsRowLeavesTheScreen() {
        holdingGate.plays = 0
        holdingGate.cancels = 0
        test.qtAudio = holdingGate
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        mouseClick(findChild(barOf(card), "videoExpandButton"))
        tryVerify(function() { return overlayOf(card).opened })
        var c0 = holdingGate.cancels
        card.rowOnScreen = false
        compare(holdingGate.cancels, c0,
                "the hidden row paused the expanded view")
        // Closing the view hands playback back to a row nobody can see:
        // that pauses.
        overlayOf(card).close()
        tryVerify(function() { return !overlayOf(card).opened })
        verify(holdingGate.cancels > c0,
               "closing the view left an off-screen row playing")
    }

    // ---- review R2-1: a player opened by a key takes the keys ----
    Component {
        id: timelineComponent
        Flickable {
            id: flick
            width: 800
            height: 400
            contentHeight: 2000
            contentY: 600
            focus: true
            property alias card: hosted
            // The timeline pages on Space (TimelinePane.qml); a key the card
            // leaves unused reaches this.
            Keys.onSpacePressed: (event) => {
                contentY += 300
                event.accepted = true
            }
            VideoPlayerCard {
                id: hosted
                y: 700
                width: 480
                height: 270
                mediaKey: "$video"
                ownerKey: "owner-video"
                filename: "clip.mp4"
                startMuted: true
            }
        }
    }
    function test_aKeyOpenedPlayerTakesSpaceFromTheTimeline() {
        var flick = createTemporaryObject(timelineComponent, test)
        var card = flick.card
        var player = playerOf(card)
        tryVerify(function() { return player.seekable && player.duration > 0 },
                  10000)
        tryCompare(player, "playbackState", MediaPlayer.PlayingState)
        // What MessageDelegate does once the Loader has the card.
        card.focusControls()
        var play = findChild(barOf(card), "videoPlayPauseButton")
        verify(play.activeFocus, "focus did not move into the player")
        var y0 = flick.contentY
        keyClick(Qt.Key_Space)
        tryCompare(player, "playbackState", MediaPlayer.PausedState)
        keyClick(Qt.Key_Space)
        tryCompare(player, "playbackState", MediaPlayer.PlayingState)
        compare(flick.contentY, y0, "Space paged the timeline")
    }

    // ---- review F2: the speed label fits at a large text scale ----
    function test_theSpeedButtonFitsItsLabelAtLargeText() {
        AppTheme.textScale = 1.5
        var host = createTemporaryObject(twoBarsComponent, test)
        host.player.playbackRate = 1.75
        var bars = [host.first, host.second]
        for (var i = 0; i < bars.length; ++i) {
            var button = findChild(bars[i], "videoSpeedButton")
            var label = findChild(bars[i], "videoSpeedLabel")
            compare(label.text, "1.75\u00d7")
            verify(label.implicitWidth <= button.width,
                   "the speed label (" + label.implicitWidth
                   + " px) overflows its " + button.width + " px button")
        }
    }

    // ---- review F5: a second click on the speed button closes the menu
    function test_aSecondClickOnTheSpeedButtonClosesTheMenu() {
        var host = createTemporaryObject(twoBarsComponent, test)
        var button = findChild(host.first, "videoSpeedButton")
        var menu = menuOf(host.first)
        mouseClick(button)
        tryCompare(menu, "opened", true)
        mouseClick(button)
        tryCompare(menu, "visible", false)
        wait(100)
        verify(!menu.visible, "the second click reopened the menu")
    }

    // ---- review F6: right-to-left keeps the time axis and opens inward
    Component {
        id: rtlComponent
        Item {
            width: 800
            height: 600
            LayoutMirroring.enabled: true
            LayoutMirroring.childrenInherit: true
            property alias bar: rtlBar
            MediaPlayer {
                id: rtlPlayer
                audioOutput: AudioOutput {
                    id: rtlAudio
                    muted: true
                    property bool userUnmuted: false
                }
            }
            VideoControlBar {
                id: rtlBar
                y: 500
                width: 480
                player: rtlPlayer
                audio: rtlAudio
            }
        }
    }
    function test_rightToLeftKeepsTheSeekAxisAndOpensTheMenuInward() {
        var host = createTemporaryObject(rtlComponent, test)
        var slider = findChild(host.bar, "videoSeekSlider")
        verify(!slider.mirrored, "the seek track runs right to left")
        var button = findChild(host.bar, "videoSpeedButton")
        verify(button.mirrored)
        mouseClick(button)
        var menu = menuOf(host.bar)
        tryCompare(menu, "opened", true)
        var left = menu.parent.mapToItem(null, menu.x, 0).x
        var b = windowRect(button)
        verify(left >= b.x - 0.5,
               "the mirrored menu opens leftwards off the bar: " + left)
        menu.close()
    }

    // ---- review F8: Down keeps a mute; Up unmutes ----
    function test_downKeepsTheMuteAndUpLiftsIt() {
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        var bar = barOf(card)
        var audio = bar.audio
        verify(audio.muted)
        findChild(bar, "videoPlayPauseButton").forceActiveFocus(Qt.TabFocusReason)
        var before = audio.volume
        keyClick(Qt.Key_Down)
        verify(audio.muted, "Down unmuted a muted card")
        verify(audio.volume < before)
        keyClick(Qt.Key_Up)
        verify(!audio.muted, "Up did not unmute")
    }

    // ---- review F10: no scrim over the expanded stage ----
    function test_theExpandedBarDrawsNoScrimOverTheStage() {
        var host = createTemporaryObject(twoBarsComponent, test)
        verify(findChild(host.first, "videoBarScrim").visible)
        host.second.large = true
        verify(!findChild(host.second, "videoBarScrim").visible)
    }

    // ---- review F4: Download answers inside the expanded view ----
    function test_theExpandedViewConfirmsADownload() {
        var card = loadedCard({ width: 480, height: 270, y: 300,
                                senderName: "Fixture Sender",
                                timestamp: new Date(2026, 9, 20, 14, 5),
                                canSave: true })
        mouseClick(findChild(barOf(card), "videoExpandButton"))
        var overlay = overlayOf(card)
        tryCompare(overlay, "opened", true)
        var notice = inOverlay(card, "videoViewerSaveNotice")
        verify(notice, "the expanded view has no save confirmation")
        verify(!notice.visible)
        // Another video's save is not this view's answer.
        bridge.saveFinished(true, "Saved.", "$other")
        verify(!notice.visible)
        bridge.saveFinished(true, "Saved.", "$video")
        verify(notice.visible)
        compare(notice.text, "Saved.")
        // It keeps the header up while it is there, even mid-playback.
        verify(overlay.chromeShown)
        overlay.close()
    }

    // ---- review R3-1: the card reports focus held by any of its controls,
    // which is what its host reads on close to hand focus back to the cover.
    function test_theCardKnowsWhenFocusIsInsideItOnClose() {
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        var focusAtClose = null
        card.closeRequested.connect(function() {
            focusAtClose = card.activeFocus
        })
        card.focusControls()
        var close = findChild(barOf(card), "videoCloseButton")
        verify(close.visible)
        close.forceActiveFocus(Qt.TabFocusReason)
        verify(close.activeFocus)
        verify(card.activeFocus,
               "focus on the card's close button is invisible to the card")
        keyClick(Qt.Key_Space)
        tryVerify(function() { return focusAtClose !== null })
        verify(focusAtClose,
               "on close the card said nothing inside it had focus")
    }

    // ---- swarm C20 round 1 ----
    Component {
        id: lateBarComponent
        Item {
            width: 600
            height: 120
            property alias player: lmp
            property alias bar: lbar
            property var target: null
            MediaPlayer {
                id: lmp
                audioOutput: AudioOutput { id: lao; muted: true
                                           property bool userUnmuted: false }
            }
            VideoControlBar {
                id: lbar
                width: 600
                // The expanded view's order: the bar exists first, the
                // player arrives when the view opens.
                player: parent.target
                audio: lao
            }
        }
    }

    // C20-F02: a view opened while PAUSED drew the handle at 0.
    function test_aBarGivenALoadedPausedPlayerDrawsTheHandleAtTheClock() {
        var host = createTemporaryObject(lateBarComponent, test)
        var mp = host.player
        mp.source = test.fixture
        tryVerify(function() { return mp.seekable && mp.duration > 0 }, 10000)
        mp.position = Math.floor(mp.duration / 2)
        tryVerify(function() { return mp.position > 0 })
        verify(mp.playbackState !== MediaPlayer.PlayingState)
        host.target = mp
        var slider = findChild(host.bar, "videoSeekSlider")
        tryVerify(function() { return slider.value > 0 }, 2000,
                  "the handle sits at 0 under a clock reading "
                  + host.bar.timeText)
        verify(slider.visualPosition > 0.2)
    }

    // C20-F05/D16: Return and Enter click the focused bar button.
    function test_returnActivatesTheFocusedBarButton() {
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        var player = playerOf(card)
        card.focusControls()
        var play = findChild(barOf(card), "videoPlayPauseButton")
        verify(play.activeFocus)
        tryCompare(player, "playbackState", MediaPlayer.PlayingState)
        keyClick(Qt.Key_Return)
        tryCompare(player, "playbackState", MediaPlayer.PausedState)
        keyClick(Qt.Key_Enter)
        tryCompare(player, "playbackState", MediaPlayer.PlayingState)
    }

    // C20-F09/D08: every bar button explains itself on hover.
    function test_everyBarButtonHasATooltip() {
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        var bar = barOf(card)
        var names = ["videoPlayPauseButton", "videoSpeedButton",
                     "videoExpandButton", "videoCloseButton"]
        for (var i = 0; i < names.length; ++i) {
            var b = findChild(bar, names[i])
            verify(b, names[i])
            verify(b.ToolTip.text.length > 0, names[i] + " has no tooltip")
        }
    }

    // C20-D05: the corner close is clear of the row's top-right toolbar.
    function test_theCornerCloseIsNotInTheRowToolbarsCorner() {
        var card = createTemporaryObject(cardComponent, test,
                                         { width: 260, height: 440 })
        var close = findChild(card, "videoCardCloseButton")
        tryVerify(function() { return close.visible })
        verify(close.x < card.width / 2,
               "the close button is at x=" + close.x + " of " + card.width)
    }

    // C20-F04: the second click of a double-click on the cover must not
    // pause the card the first one created.
    function test_aClickRightAfterTheCardAppearsDoesNotPauseIt() {
        var card = createTemporaryObject(cardComponent, test,
                                         { width: 480, height: 270, y: 300 })
        var player = playerOf(card)
        tryCompare(player, "playbackState", MediaPlayer.PlayingState, 10000)
        verify(card.ready, "the card was not ready, the click would be vacuous")
        mouseClick(card, 240, 40)
        wait(150)
        verify(player.playbackState !== MediaPlayer.PausedState,
               "the early click paused the player")
    }

    // C20-D03: a mouse-started card holds keyboard focus.
    function test_aNewCardHoldsKeyboardFocusForSpace() {
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        verify(card.activeFocus, "the card does not hold focus")
        var player = playerOf(card)
        tryCompare(player, "playbackState", MediaPlayer.PlayingState)
        keyClick(Qt.Key_Space)
        tryCompare(player, "playbackState", MediaPlayer.PausedState)
    }

    // Review r1: the card forwards only player keys, so Up/Down/Home keep
    // scrolling the timeline and do not change the volume.
    function test_theCardLeavesUpDownAndHomeToTheTimeline() {
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        verify(card.activeFocus)
        var player = playerOf(card)
        player.position = 3000
        tryCompare(player, "position", 3000, 5000)
        keyClick(Qt.Key_Home)
        keyClick(Qt.Key_Up)
        wait(100)
        verify(player.position >= 3000, "Home was taken by the card")
    }

    // Review r1: the corner close has a tooltip and the scrim button type.
    function test_theCornerCloseHasATooltip() {
        var card = createTemporaryObject(cardComponent, test,
                                         { width: 260, height: 440 })
        var close = findChild(card, "videoCardCloseButton")
        verify(close.ToolTip.text.length > 0)
        verify(findChild(close, "scrimFocusRing"), "not a ScrimIconButton")
    }

    Component {
        id: spyComponent
        SignalSpy {}
    }
}
