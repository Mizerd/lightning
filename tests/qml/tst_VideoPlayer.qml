import QtQuick
import QtQuick.Controls
import QtMultimedia
import QtTest
import MatrixClient

TestCase {
    id: test
    name: "VideoPlayer"
    width: 800
    height: 760
    visible: true
    when: windowShown

    readonly property url fixture: Qt.resolvedUrl("../../data/sounds/ring.wav")
    // Two audio tracks (440/880 Hz) and two subtitle tracks.
    readonly property url tracksFixture: Qt.resolvedUrl("tracks.mka")

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
        property var tracks: ({})
        function rememberVideoTracks(k, a, s) { tracks[k] = [a, s] }
        function videoTracks(k) { return tracks[k] || [-1, -2] }
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
        property bool holdFetch: false
        property bool brokenSource: false
        function playableSource(key) {
            if (holdFetch) return ""
            if (key === "$tracks") return test.tracksFixture.toString()
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
        playback.tracks = ({})
        settings.mediaVolume = 0.8
        bridge.holdFetch = false
        bridge.brokenSource = false
    }

    function barOf(card) { return findChild(card, "videoControlBar") }
    function playerOf(card) { return barOf(card).player }
    function overlayOf(card) { return findChild(card, "videoViewerOverlay") }
    function inOverlay(card, name) {
        var overlay = overlayOf(card)
        return overlay && overlay.contentItem
               ? findChild(overlay.contentItem, name) : null
    }
    function menuOf(bar) {
        for (var i = 0; i < bar.data.length; ++i) {
            var o = bar.data[i]
            if (o && typeof o.popup === "function"
                    && o.objectName === "videoSpeedMenu")
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

    function test_everyBarShowsThePlayersRate() {
        var host = createTemporaryObject(twoBarsComponent, test)
        verify(host)
        compare(speedText(host.first), "1×")
        compare(speedText(host.second), "1×")

        mouseClick(findChild(host.first, "videoSpeedButton"))
        var menu = menuOf(host.first)
        verify(menu)
        tryCompare(menu, "opened", true)
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
        compare(speedText(host.second), "1.5×",
                "the second bar does not show the player's rate")
        compare(playback.videoPlaybackRate, 1.5)

        host.player.playbackRate = 2.0
        compare(speedText(host.first), "2×")
        compare(speedText(host.second), "2×")
    }

    function test_aNewCardStartsAtTheSessionSpeed() {
        playback.videoPlaybackRate = 1.5
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        compare(playerOf(card).playbackRate, 1.5)
        compare(speedText(barOf(card)), "1.5×")
    }

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
        var b = windowRect(button)
        var top = menu.parent.mapToItem(null, menu.x, menu.y).y
        var bottom = top + menu.height
        verify(bottom <= b.y + 0.5,
               "the speed menu runs over its own button: bottom " + bottom
               + ", button top " + b.y)
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

        mouseMove(test, 4, 4)
        wait(400)
        verify(player.playbackState === MediaPlayer.PlayingState)
        verify(bar.shown, "the control bar hid while its speed menu was open")
        compare(bar.opacity, 1.0)
        menu.close()
    }

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
            var close = findChild(card, "videoCardCloseButton")
            verify(close, c.w + " px: no close button left")
            verify(close.visible && inside(close, card),
                   c.w + " px: the corner close is not on the card")
            compare(findChild(bar, "videoCloseButton").visible, false)
            card.destroy()
        }
        var wide = createTemporaryObject(cardComponent, test,
                                         { width: 480, height: 270 })
        var wideBar = barOf(wide)
        tryVerify(function() { return wideBar.width === 480 })
        verify(findChild(wideBar, "videoCloseButton").visible)
        verify(!findChild(wide, "videoCardCloseButton").visible)
        verify(findChild(wideBar, "videoSeekSlider").width >= 120)
    }

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
        var slider = findChild(bar, "videoSeekSlider")
        slider.forceActiveFocus(Qt.TabFocusReason)
        keyClick(Qt.Key_Left)
        tryVerify(function() { return player.position < 1000 }, 3000,
                  "Left on the seek track moved " + player.position)
        verify(slider.Accessible.description.indexOf("0:00") === 0
               && slider.Accessible.description.indexOf("0:06") > 0,
               "the seek track has no spoken position: '"
               + slider.Accessible.description + "'")

    }

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

    function test_aForcedStopRemembersWhereTheVideoWas() {
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        var player = playerOf(card)
        player.pause()
        player.position = 4000
        tryVerify(function() { return Math.abs(player.position - 4000) < 100 })
        playback.stopGeneration += 1
        compare(playback.remembered.length, 1,
                "a forced stop did not remember the position")
        compare(playback.remembered[0].key, "$video")
        verify(Math.abs(playback.remembered[0].pos - 4000) < 100)
        verify(playback.remembered[0].dur > 5000)
        compare(player.source.toString(), "")

        playback.resume = ({ "$video": 4200 })
        var again = loadedCard({ width: 480, height: 270, y: 300 })
        var p2 = playerOf(again)
        tryVerify(function() { return p2.position >= 4100 }, 5000,
                  "the new card started at " + p2.position
                  + " instead of resuming at 4.2 s")
    }

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

        player.pause()
        card.handleEndOfMedia()
        verify(overlay.opened)
        overlay.close()
        tryCompare(spyClose, "count", 1)
        compare(player.source.toString(), "")
    }

    function test_anInlineEndReturnsToTheCoverMarkedWatched() {
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        var spyClose = createTemporaryObject(spyComponent, test,
                                             { target: card,
                                               signalName: "closeRequested" })
        card.handleEndOfMedia()
        compare(spyClose.count, 1)
        compare(playback.finished, ["$video"])
        compare(playback.remembered.length, 0)
    }

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
        var why = findChild(card, "videoFailedText")
        verify(why && why.visible)
        verify(why.text.indexOf("sound system") >= 0,
               "the give-up reads as a broken video: " + why.text)
    }

    QtObject {
        id: holdingGate
        property int plays: 0
        property int cancels: 0
        signal playGivenUp(var p)
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

    function test_noTrackMenuWithoutAChoice() {
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        var bar = barOf(card)
        verify(!bar.hasTrackChoice)
        verify(!findChild(bar, "videoTracksButton").visible,
               "a track menu with nothing to choose")
        compare(bar.trackLabel(null, 1), "Track 2")
    }

    function trackMenuOf(bar) {
        for (var i = 0; i < bar.data.length; ++i) {
            var o = bar.data[i]
            if (o && o.objectName === "videoTracksMenu")
                return o
        }
        return null
    }
    function pickTrack(bar, name) {
        var menu = trackMenuOf(bar)
        menu.open()
        tryCompare(menu, "opened", true)
        var row = null
        for (var i = 0; i < menu.count; ++i)
            if (menu.itemAt(i) && menu.itemAt(i).objectName === name)
                row = menu.itemAt(i)
        verify(row, "no " + name + " row in the track menu")
        row.triggered()
        menu.close()
    }
    function tickedRows(bar) {
        var menu = trackMenuOf(bar)
        var out = []
        for (var i = 0; i < menu.count; ++i)
            if (menu.itemAt(i) && menu.itemAt(i).iconName === "check")
                out.push(menu.itemAt(i).objectName)
        return out.join(",")
    }
    function tracksCard(props) {
        var card = createTemporaryObject(cardComponent, test, props)
        verify(card)
        card.mediaKey = "$tracks"
        card.start()
        var bar = barOf(card)
        tryVerify(function() {
            return bar.audioTracks.length === 2 && bar.subtitleTracks.length === 2
        }, 10000, "the two-track fixture never loaded")
        return card
    }

    function test_aPickedTrackIsTickedAndSurvivesAReplay() {
        var card = tracksCard({ width: 480, height: 270, y: 300 })
        var bar = barOf(card)
        var player = playerOf(card)
        compare(tickedRows(bar), "videoAudioTrack0,videoSubtitleOff")

        pickTrack(bar, "videoAudioTrack1")
        pickTrack(bar, "videoSubtitleTrack1")
        tryCompare(player, "activeAudioTrack", 1)
        tryCompare(player, "activeSubtitleTrack", 1)
        compare(tickedRows(bar), "videoAudioTrack1,videoSubtitleTrack1")

        card.resetPlayback()
        card.start()
        tryVerify(function() { return bar.audioTracks.length === 2 }, 10000)
        tryCompare(player, "activeAudioTrack", 1, 5000,
                   "a replay went back to the first audio track")
        tryCompare(player, "activeSubtitleTrack", 1, 5000,
                   "a replay turned the subtitles off")
        compare(tickedRows(bar), "videoAudioTrack1,videoSubtitleTrack1")

        var again = tracksCard({ width: 480, height: 270, y: 300 })
        tryCompare(playerOf(again), "activeAudioTrack", 1, 5000,
                   "a new card for the same video lost the pick")
        compare(tickedRows(barOf(again)), "videoAudioTrack1,videoSubtitleTrack1")
    }

    function test_aShownSubtitleIsLiftedAboveTheBar() {
        var card = tracksCard({ width: 480, height: 270, y: 300 })
        var bar = barOf(card)
        var output = findChild(card, "videoCardOutput")
        verify(output)
        bar.player.pause()
        tryCompare(bar, "opacity", 1.0)
        compare(output.height, card.height)
        pickTrack(bar, "videoSubtitleTrack0")
        var o = output.mapToItem(null, 0, 0)
        var b = bar.mapToItem(null, 0, 0)
        verify(o.y + output.height <= b.y + 0.5,
               "the picture (and its subtitles) ends at " + (o.y + output.height)
               + " under the bar at " + b.y)
    }

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
    function test_bytesTheBridgeRefusesReadAsAFormatProblem() {
        bridge.holdFetch = true
        var card = createTemporaryObject(cardComponent, test,
                                         { width: 480, height: 270, y: 300,
                                           canSave: true })
        bridge.mediaFetchFailed("full:$video", "rejected")
        verify(failureText(card).indexOf("format") >= 0, failureText(card))
        verify(!findChild(card, "videoRetryButton").visible)
        verify(findChild(card, "videoFailedDownloadButton").visible)
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
        overlayOf(card).close()
        tryVerify(function() { return !overlayOf(card).opened })
        verify(holdingGate.cancels > c0,
               "closing the view left an off-screen row playing")
    }

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

    function test_theExpandedBarDrawsNoScrimOverTheStage() {
        var host = createTemporaryObject(twoBarsComponent, test)
        verify(findChild(host.first, "videoBarScrim").visible)
        host.second.large = true
        verify(!findChild(host.second, "videoBarScrim").visible)
    }

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
        bridge.saveFinished(true, "Saved.", "$other")
        verify(!notice.visible)
        bridge.saveFinished(true, "Saved.", "$video")
        verify(notice.visible)
        compare(notice.text, "Saved.")
        verify(overlay.chromeShown)
        overlay.close()
    }

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
                player: parent.target
                audio: lao
            }
        }
    }

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

    function test_theCornerCloseIsNotInTheRowToolbarsCorner() {
        var card = createTemporaryObject(cardComponent, test,
                                         { width: 260, height: 440 })
        var close = findChild(card, "videoCardCloseButton")
        tryVerify(function() { return close.visible })
        verify(close.x < card.width / 2,
               "the close button is at x=" + close.x + " of " + card.width)
    }

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

    function test_aNewCardHoldsKeyboardFocusForSpace() {
        var card = loadedCard({ width: 480, height: 270, y: 300 })
        verify(card.activeFocus, "the card does not hold focus")
        var player = playerOf(card)
        tryCompare(player, "playbackState", MediaPlayer.PlayingState)
        keyClick(Qt.Key_Space)
        tryCompare(player, "playbackState", MediaPlayer.PausedState)
    }

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
