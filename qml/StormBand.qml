import QtQuick
import QtQuick.Effects
import MatrixClient

// The Storm Band — a 1:1 port of storm-band-export.html's mountStormBand().
//
// Every tile and sprite is generated in C++ (StormBandPainter, an exact port
// of the reference's painters — same PRNG, same seeds, same pixels) and
// served through image://storm-band/<layer>?bg=..&accent=.. — so the scene a
// given (background, accent) pair produces matches the HTML reference.
// This file reproduces the reference's LAYER STACK, geometry and CSS
// keyframe timings:
//
//   gradient(static) sky(1536/620s) moon shoot(23s) far(662/265s)
//   mist(1090/128s) mid(794/158s) spire(1282/104s) bolts(6)+blooms
//   front(1438/58s) rain(320/2.1s diagonal) horizon embers(11)
//
// and the container's 5-stop alpha mask (0 -> .18@14% -> .55@30% ->
// .85@44% -> 1@58%), implemented as a MultiEffect mask over the whole
// composite, so the band dissolves into WHATEVER is behind it.
//
// CSS keyframes are reproduced exactly: each animated element carries a
// looping 0..1 `phase` and maps it through kf() — linear interpolation
// between the reference's keyframe stops, which is precisely what CSS does.
// Reduced motion mirrors the reference's prefers-reduced-motion rule
// (animation: none): every phase and scroll offset rests at 0, leaving the
// static scene visible. The band also pauses while off-screen, hidden, or
// minimized.
//
// Purely decorative: no pointer or hover handlers of any kind and
// `enabled: false`, so it never intercepts input or steals focus.
Item {
    id: root

    implicitHeight: 190
    enabled: false

    // Palette inputs, exactly the reference's mountStormBand(host,
    // { bg, accent }): bg is the color of the page BEHIND the band.
    property color backdropColor: AppTheme.background
    readonly property color accentColor: AppTheme.accent

    readonly property bool onScreen: root.visible
                                     && Window.window !== null
                                     && Window.window.visible
                                     && Window.window.visibility
                                        !== Window.Minimized
    readonly property bool animating: root.onScreen && !AppTheme.reducedMotion

    // ---- provider URL plumbing -----------------------------------------
    function hex2(v) {
        var h = Math.max(0, Math.min(255, Math.round(v * 255))).toString(16)
        return h.length < 2 ? "0" + h : h
    }
    readonly property string themeQuery:
        "?bg=" + hex2(backdropColor.r) + hex2(backdropColor.g)
        + hex2(backdropColor.b)
        + "&accent=" + hex2(accentColor.r) + hex2(accentColor.g)
        + hex2(accentColor.b)
    function tileUrl(layer) {
        // review M2: a hidden band generates NOTHING — the component is
        // instantiated for every Settings section, and a theme switch on
        // the Appearance page must not regenerate ~20 tiles for a band the
        // user cannot see. root.visible is read here so every source
        // binding re-evaluates when the About page shows.
        if (!root.visible)
            return ""
        return "image://storm-band/" + layer + root.themeQuery
    }

    // CSS-style keyframe evaluation: frames is [[t, value], ...] sorted by
    // t; values interpolate linearly between stops, exactly like the
    // reference's @keyframes.
    function kf(phase, frames) {
        if (phase <= frames[0][0])
            return frames[0][1]
        for (var i = 1; i < frames.length; ++i) {
            if (phase <= frames[i][0]) {
                var t0 = frames[i - 1][0], v0 = frames[i - 1][1]
                var t1 = frames[i][0], v1 = frames[i][1]
                return t1 > t0 ? v0 + (v1 - v0) * (phase - t0) / (t1 - t0)
                               : v1
            }
        }
        return frames[frames.length - 1][1]
    }

    // sb-strikeA / sb-strikeB / sb-ember opacity curves, verbatim.
    readonly property var strikeA:
        [[0, 0], [.84, 0], [.844, 1], [.85, .12], [.856, .95], [.866, .08],
         [.874, 0], [1, 0]]
    readonly property var strikeB:
        [[0, 0], [.61, 0], [.614, .9], [.622, .1], [.628, 1], [.64, 0],
         [1, 0]]
    readonly property var emberCurve: [[0, 0], [.12, .9], [.7, .6], [1, 0]]

    // BOLTS: [left fraction, width css px, cycle ms, delay ms, curve,
    // bloom]. Bolt tips land at y=158 (the ridge line) inside the 190px
    // band; sprite height = w * 74 / 46.
    readonly property var boltDefs: [
        { xf: .17, w: 78, cycle: 9400,  delay: 0,     curve: "A", bloom: "bloomA", sprite: "bolt1" },
        { xf: .38, w: 62, cycle: 13700, delay: 2600,  curve: "B", bloom: "bloomB", sprite: "bolt2" },
        { xf: .57, w: 86, cycle: 21300, delay: 5200,  curve: "A", bloom: "bloomC", sprite: "bolt3" },
        { xf: .74, w: 58, cycle: 17100, delay: 8400,  curve: "B", bloom: "bloomA", sprite: "bolt4" },
        { xf: .89, w: 72, cycle: 27900, delay: 3700,  curve: "A", bloom: "bloomB", sprite: "bolt5" },
        { xf: .05, w: 52, cycle: 31400, delay: 11500, curve: "B", bloom: "bloomC", sprite: "bolt6" }
    ]
    // EMBERS: [left %, bottom px, cycle s, delay s] — 11 rising motes.
    readonly property var emberDefs:
        [[9, 24, 9, 0], [17, 30, 12.5, 2.4], [26, 20, 10.8, 5.1],
         [34, 28, 14, 1.2], [43, 22, 11.4, 6.8], [51, 32, 13.2, 3.6],
         [60, 18, 10.2, 8.1], [68, 26, 15.3, 4.4], [77, 21, 12.1, 9.6],
         [85, 29, 13.9, 2.1], [94, 23, 11.7, 7.3]]

    // Reusable phase driver: pause(delay) once, then loop phase 0..1 over
    // one cycle — CSS animation-delay + infinite linear iteration. When
    // stopped (reduced motion / hidden) the phase rests at 0, the CSS
    // animation-none state.
    component PhaseDriver: SequentialAnimation {
        id: driver
        property Item item
        property int delayMs: 0
        property int cycleMs: 1000
        running: root.animating && item !== null
        onRunningChanged: if (!running && item) item.phase = 0
        PauseAnimation { duration: driver.delayMs }
        SequentialAnimation {
            loops: Animation.Infinite
            NumberAnimation {
                target: driver.item
                property: "phase"
                from: 0; to: 1
                duration: driver.cycleMs
            }
        }
    }

    // One scrolling tile strip: background-repeat:repeat-x at 132px tall,
    // bottom-aligned, background-position-x animating 0 -> -tileW.
    component Scroller: Item {
        id: sc
        property string layerKey
        property int tileW: 100
        property int cycleMs: 1000
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 132
        clip: true
        readonly property int tileCount:
            Math.max(2, Math.ceil(width / Math.max(1, tileW)) + 2)
        Item {
            id: strip
            height: sc.height
            width: sc.tileCount * sc.tileW
            x: 0
            NumberAnimation on x {
                running: root.animating
                from: 0; to: -sc.tileW
                duration: sc.cycleMs
                loops: Animation.Infinite
                // Stopped = the CSS animation-none state: offset 0.
                onRunningChanged: if (!running) strip.x = 0
            }
            Repeater {
                model: sc.tileCount
                Image {
                    required property int index
                    x: index * sc.tileW
                    width: sc.tileW
                    height: strip.height
                    smooth: false
                    cache: true
                    source: root.tileUrl(sc.layerKey)
                }
            }
        }
    }

    // The whole scene renders through the reference's alpha mask, so
    // content scrolled behind the band shows through its dissolved top.
    Item {
        id: scene
        anchors.fill: parent
        clip: true
        layer.enabled: true
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: maskGradient
            // Threshold 0 + full spread = use the mask's alpha channel
            // directly (the documented linear-mask configuration).
            maskThresholdMin: 0.0
            maskSpreadAtMin: 1.0
        }

        // gradient: transparent -> sky1 30% -> sky2 64% -> sky3 100%.
        Image {
            anchors.fill: parent
            source: root.tileUrl("gradient")
            smooth: true
        }

        Scroller { layerKey: "sky"; tileW: 1536; cycleMs: 620000 }

        // moon: left 11%, top 62, 36x36, opacity .9, static.
        Image {
            x: scene.width * 0.11
            y: 62
            width: 36; height: 36
            smooth: false
            opacity: .9
            source: root.tileUrl("moon")
        }

        // shooting star: left 8%, top 48, 60x24; sb-shoot 23s — hidden 82%
        // of the cycle, then a 6% sweep of translate(300px, 96px) with a
        // short fade-out tail to (330, 106).
        Image {
            id: shoot
            property real phase: 0
            x: scene.width * 0.08 + root.kf(phase,
                   [[0, 0], [.82, 0], [.88, 300], [.89, 330], [1, 330]])
            y: 48 + root.kf(phase,
                   [[0, 0], [.82, 0], [.88, 96], [.89, 106], [1, 106]])
            width: 60; height: 24
            smooth: false
            opacity: root.kf(phase,
                   [[0, 0], [.82, 0], [.83, 1], [.88, 1], [.89, 0], [1, 0]])
            source: root.tileUrl("shoot")
        }
        PhaseDriver { item: shoot; cycleMs: 23000 }

        Scroller { layerKey: "far";   tileW: 662;  cycleMs: 265000 }
        Scroller { layerKey: "mist";  tileW: 1090; cycleMs: 128000 }
        Scroller { layerKey: "mid";   tileW: 794;  cycleMs: 158000 }
        Scroller { layerKey: "spire"; tileW: 1282; cycleMs: 104000 }

        // Six bolts, each with its ground bloom sharing the same strike
        // curve — radial-gradient(170px 120px at (left+1)% 78%).
        Repeater {
            model: root.boltDefs
            Item {
                id: bolt
                required property var modelData
                anchors.fill: parent
                property real phase: 0
                readonly property real flash:
                    root.kf(phase, modelData.curve === "A" ? root.strikeA
                                                           : root.strikeB)
                readonly property real spriteScale: modelData.w / 46
                readonly property real boltH:
                    Math.round(modelData.w * 74 / 46)
                Image {
                    x: scene.width * (bolt.modelData.xf + 0.01) - 170
                    y: 190 * 0.78 - 120
                    width: 340; height: 240
                    smooth: true
                    opacity: bolt.flash
                    source: root.tileUrl(bolt.modelData.bloom)
                }
                Image {
                    // The sprite carries an 8px baked-glow pad ring around
                    // the 46x74 art (see StormBandPainter::kBoltPad).
                    x: scene.width * bolt.modelData.xf
                       - 8 * bolt.spriteScale
                    y: (158 - bolt.boltH) - 8 * bolt.spriteScale
                    width: bolt.modelData.w + 16 * bolt.spriteScale
                    height: bolt.boltH + 16 * bolt.spriteScale
                    smooth: false
                    opacity: bolt.flash
                    source: root.tileUrl(bolt.modelData.sprite)
                }
                PhaseDriver {
                    item: bolt
                    cycleMs: bolt.modelData.cycle
                    delayMs: bolt.modelData.delay
                }
            }
        }

        Scroller { layerKey: "front"; tileW: 1438; cycleMs: 58000 }

        // rain: 320x132 tile repeating in BOTH axes over the whole band,
        // background-position animating (-320, +396) per 2.1s — one
        // horizontal wrap and three vertical wraps per cycle.
        Item {
            id: rainLayer
            anchors.fill: parent
            clip: true
            opacity: .3
            property real phase: 0
            readonly property int cols:
                Math.max(2, Math.ceil((width + 320) / 320) + 1)
            readonly property int rows: 4
            Item {
                x: -rainLayer.phase * 320
                y: ((rainLayer.phase * 396) % 132) - 132
                Repeater {
                    model: rainLayer.cols * rainLayer.rows
                    Image {
                        required property int index
                        x: (index % rainLayer.cols) * 320
                        y: Math.floor(index / rainLayer.cols) * 132
                        width: 320; height: 132
                        smooth: false
                        source: root.tileUrl("rain")
                    }
                }
            }
            NumberAnimation on phase {
                running: root.animating
                from: 0; to: 1
                duration: 2100
                loops: Animation.Infinite
                onRunningChanged: if (!running) rainLayer.phase = 0
            }
        }

        // horizon: accent bloom along the bottom 70px.
        Image {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 70
            smooth: true
            source: root.tileUrl("horizon")
        }

        // 11 rising embers: 2x2 motes translating (-9, -54) over one cycle
        // with the sb-ember opacity curve.
        Repeater {
            model: root.emberDefs
            Image {
                id: ember
                required property var modelData
                required property int index
                property real phase: 0
                x: scene.width * (modelData[0] / 100) - 9 * phase
                y: (190 - modelData[1] - 2) - 54 * phase
                width: 2; height: 2
                smooth: false
                opacity: root.kf(phase, root.emberCurve)
                source: root.tileUrl("ember" + (index % 3 + 1))
                PhaseDriver {
                    item: ember
                    cycleMs: Math.round(ember.modelData[2] * 1000)
                    delayMs: Math.round(ember.modelData[3] * 1000)
                }
            }
        }
    }

    // The reference's container mask, as a texture for the MultiEffect
    // above: linear-gradient(to bottom, 0, .18 14%, .55 30%, .85 44%,
    // 1 58%).
    Rectangle {
        id: maskGradient
        anchors.fill: parent
        visible: false
        layer.enabled: true
        gradient: Gradient {
            GradientStop { position: 0.0;  color: Qt.rgba(0, 0, 0, 0) }
            GradientStop { position: 0.14; color: Qt.rgba(0, 0, 0, 0.18) }
            GradientStop { position: 0.30; color: Qt.rgba(0, 0, 0, 0.55) }
            GradientStop { position: 0.44; color: Qt.rgba(0, 0, 0, 0.85) }
            GradientStop { position: 0.58; color: Qt.rgba(0, 0, 0, 1) }
            GradientStop { position: 1.0;  color: Qt.rgba(0, 0, 0, 1) }
        }
    }
}
