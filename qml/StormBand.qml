import QtQuick
import MatrixClient

// The "Storm Band" — a procedurally generated pixel-art storm landscape for
// the bottom of the About page.
//
// Every tile is rendered ONCE in C++ (StormBandPainter — QImage + QPainter,
// pure software) and served through
// image://storm-band/<layer>/<W>x<H>?dark=..&base=..&accent=..&seed=.. .
// This file only tiles/scrolls/animates plain Image and Rectangle geometry
// built from those bitmaps — none of the imperative-painting or effect
// primitives that either render blank under the offscreen QPA platform
// (see qml/TrustCard.qml and qml/StormNode.qml) or need modules this
// application does not link (shader pipelines, particles, shapes).
//
// Every colour traces back to AppTheme (background/accent/dark) through the
// provider URL; the C++ painter never hardcodes a theme literal. When the
// theme changes the URL changes and the affected tiles regenerate once —
// never per frame. AppTheme.reducedMotion stops every animation (scroll,
// rain, bolts, embers, shooting star) and leaves a static-but-intentional
// scene; the band also pauses while off-screen or its window is hidden.
//
// Purely decorative: no pointer or hover handlers of any kind and
// `enabled: false`, so it never intercepts input or steals focus.
Item {
    id: root

    implicitHeight: 190
    clip: true
    enabled: false

    // The integrator binds this to the About page's own background token
    // so the top of the band dissolves into the surrounding page instead of
    // showing a hard seam.
    property color backdropColor: AppTheme.background

    readonly property bool dark: AppTheme.dark
    readonly property color baseColor: AppTheme.background
    readonly property color accentColor: AppTheme.accent

    readonly property bool onScreen: root.visible
                                     && Window.window !== null
                                     && Window.window.visible
                                     && Window.window.visibility
                                        !== Window.Minimized
    // Exposed for tests and for any integrator hook that wants to key off
    // the same on/off signal the animations use.
    readonly property bool animating: root.onScreen && !AppTheme.reducedMotion

    // ---- Theme -> provider URL plumbing --------------------------------
    function toHex2(component) {
        var v = Math.max(0, Math.min(255, Math.round(component * 255)))
        var h = v.toString(16)
        return h.length < 2 ? "0" + h : h
    }
    function colorHex(c) {
        return toHex2(c.r) + toHex2(c.g) + toHex2(c.b)
    }
    readonly property string _baseHex: root.colorHex(root.baseColor)
    readonly property string _accentHex: root.colorHex(root.accentColor)
    function tileUrl(layer, w, h) {
        return "image://storm-band/" + layer + "/" + Math.max(1, Math.round(w))
             + "x" + Math.max(1, Math.round(h))
             + "?dark=" + (root.dark ? "1" : "0")
             + "&base=" + root._baseHex
             + "&accent=" + root._accentHex
             + "&seed=4242"
    }

    // ---- Parallax background layers, back to front ---------------------
    // width/duration pairs per spec: sky 1536/620s, far hills 662/265s,
    // mist 1090/128s, mid hills 794/158s, spires 1282/104s, front ridge
    // 1438/58s. The coprime widths/speeds keep wide monitors from ever
    // showing the same repeat twice at once.
    readonly property var layerDefs: [
        { layer: "sky",      w: 1536, h: 190, dur: 620000, y: 0 },
        { layer: "farhills", w: 662,  h: 64,  dur: 265000, y: 190 - 64 },
        { layer: "mist",     w: 1090, h: 70,  dur: 128000, y: 80,  op: 0.5 },
        { layer: "midhills", w: 794,  h: 88,  dur: 158000, y: 190 - 88 },
        { layer: "spires",   w: 1282, h: 150, dur: 104000, y: 190 - 150 },
        { layer: "ridge",    w: 1438, h: 86,  dur: 58000,  y: 190 - 86 },
    ]

    Repeater {
        model: root.layerDefs
        delegate: Item {
            id: layerRoot
            required property var modelData
            x: 0
            y: modelData.y
            width: root.width
            height: modelData.h
            opacity: modelData.op !== undefined ? modelData.op : 1.0
            clip: true

            readonly property int tileW: modelData.w
            // Enough copies to cover the pane at any window width, plus one
            // spare on each side so the -tileW scroll never exposes a gap.
            readonly property int tileCount: Math.max(2,
                Math.ceil(root.width / Math.max(1, tileW)) + 2)

            Row {
                id: scrollRow
                NumberAnimation on x {
                    running: root.animating
                    from: 0
                    to: -layerRoot.tileW
                    duration: layerRoot.modelData.dur
                    loops: Animation.Infinite
                }
                Repeater {
                    model: layerRoot.tileCount
                    delegate: Image {
                        smooth: false
                        cache: true
                        asynchronous: true
                        width: layerRoot.tileW
                        height: layerRoot.height
                        source: root.tileUrl(layerRoot.modelData.layer,
                                             layerRoot.tileW, layerRoot.height)
                    }
                }
            }
        }
    }

    // ---- Rain: ~320px tile, ~2.1s diagonal scroll, low opacity ---------
    Item {
        id: rainLayer
        anchors.fill: parent
        clip: true
        // visible too, not only transparent: reduced motion must not keep
        // a grid of rain tiles in the scene graph at opacity 0.
        visible: root.animating
        opacity: root.animating ? 0.35 : 0.0

        readonly property int tileSize: 320
        readonly property int cols: Math.max(2,
            Math.ceil(root.width / tileSize) + 2)
        readonly property int rows: Math.max(2,
            Math.ceil(root.height / tileSize) + 2)

        Item {
            id: rainScroll
            NumberAnimation on x {
                running: root.animating
                from: 0
                to: -rainLayer.tileSize
                duration: 2100
                loops: Animation.Infinite
            }
            NumberAnimation on y {
                running: root.animating
                from: 0
                to: rainLayer.tileSize
                duration: 2100
                loops: Animation.Infinite
            }
            Repeater {
                model: rainLayer.cols * rainLayer.rows
                delegate: Image {
                    required property int index
                    readonly property int col: index % rainLayer.cols
                    readonly property int row: Math.floor(index / rainLayer.cols)
                    x: col * rainLayer.tileSize - rainLayer.tileSize
                    y: row * rainLayer.tileSize - rainLayer.tileSize
                    width: rainLayer.tileSize
                    height: rainLayer.tileSize
                    smooth: false
                    cache: true
                    asynchronous: true
                    source: root.tileUrl("rain", rainLayer.tileSize,
                                         rainLayer.tileSize)
                }
            }
        }
    }

    // ---- Lightning: six bolts at fixed x fractions, independent cycles -
    readonly property var boltDefs: [
        { xf: 0.17, dur: 9400 },
        { xf: 0.38, dur: 13700 },
        { xf: 0.57, dur: 21300 },
        { xf: 0.74, dur: 17100 },
        { xf: 0.89, dur: 27900 },
        { xf: 0.05, dur: 31400 },
    ]

    Repeater {
        model: root.boltDefs
        delegate: Item {
            id: boltRoot
            required property var modelData
            width: 34
            height: 130
            x: root.width * modelData.xf - width / 2
            y: 4
            opacity: 0

            Image {
                anchors.fill: parent
                smooth: false
                cache: true
                asynchronous: true
                source: root.tileUrl("bolt", boltRoot.width, boltRoot.height)
            }
            Image {
                width: 120
                height: 54
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.bottom
                anchors.topMargin: -22
                smooth: false
                cache: true
                asynchronous: true
                source: root.tileUrl("bloom", width, height)
            }

            SequentialAnimation {
                running: root.animating
                loops: Animation.Infinite
                PauseAnimation { duration: Math.max(300, boltRoot.modelData.dur - 480) }
                NumberAnimation { target: boltRoot; property: "opacity"; to: 0.95; duration: 40 }
                NumberAnimation { target: boltRoot; property: "opacity"; to: 0.10; duration: 60 }
                NumberAnimation { target: boltRoot; property: "opacity"; to: 1.0;  duration: 50 }
                NumberAnimation { target: boltRoot; property: "opacity"; to: 0.15; duration: 90 }
                NumberAnimation { target: boltRoot; property: "opacity"; to: 0.0;  duration: 240 }
            }
            Binding {
                target: boltRoot
                property: "opacity"
                value: 0
                when: !root.animating
            }
        }
    }

    // ---- Embers: 11 rising particles, staggered -------------------------
    readonly property var emberFractions: [
        0.06, 0.15, 0.22, 0.31, 0.40, 0.50, 0.58, 0.67, 0.74, 0.83, 0.92
    ]

    Repeater {
        model: 11
        delegate: Rectangle {
            id: ember
            required property int index
            width: 3
            height: 3
            radius: 1.5
            color: Qt.lighter(AppTheme.accent, 1.2)
            x: root.width * root.emberFractions[index % root.emberFractions.length]
            y: 178
            opacity: 0

            SequentialAnimation {
                running: root.animating
                loops: Animation.Infinite
                PauseAnimation { duration: 260 * ember.index }
                ParallelAnimation {
                    NumberAnimation {
                        target: ember; property: "y"; from: 178; to: 12
                        duration: 4200 + (ember.index % 4) * 480
                    }
                    SequentialAnimation {
                        NumberAnimation { target: ember; property: "opacity"; to: 0.85; duration: 420 }
                        PauseAnimation { duration: 2600 }
                        NumberAnimation { target: ember; property: "opacity"; to: 0.0; duration: 1400 }
                    }
                }
            }
            Binding {
                target: ember
                property: "opacity"
                value: 0
                when: !root.animating
            }
        }
    }

    // ---- Shooting star: ~23s cycle, visible only briefly -----------------
    Image {
        id: shootingStar
        width: 46
        height: 14
        smooth: false
        cache: true
        asynchronous: true
        source: root.tileUrl("streak", width, height)
        opacity: 0
        x: root.width * 0.62
        y: 22

        SequentialAnimation {
            running: root.animating
            loops: Animation.Infinite
            PauseAnimation { duration: 21500 }
            ParallelAnimation {
                NumberAnimation {
                    target: shootingStar; property: "x"
                    from: root.width * 0.62; to: root.width * 0.62 - 130
                    duration: 900
                }
                NumberAnimation {
                    target: shootingStar; property: "y"
                    from: 22; to: 68
                    duration: 900
                }
                SequentialAnimation {
                    NumberAnimation { target: shootingStar; property: "opacity"; to: 0.9; duration: 120 }
                    PauseAnimation { duration: 500 }
                    NumberAnimation { target: shootingStar; property: "opacity"; to: 0.0; duration: 280 }
                }
            }
        }
        Binding {
            target: shootingStar
            property: "opacity"
            value: 0
            when: !root.animating
        }
    }

    // ---- Top dissolve: blends the band into the page behind it ----------
    // No shader mask — a plain gradient overlay of `backdropColor` at
    // decreasing alpha, painted on top of every layer above.
    Rectangle {
        id: dissolveOverlay
        anchors.fill: parent
        gradient: Gradient {
            orientation: Gradient.Vertical
            GradientStop { position: 0.00; color: root.backdropColor }
            GradientStop { position: 0.14; color: Qt.alpha(root.backdropColor, 0.82) }
            GradientStop { position: 0.30; color: Qt.alpha(root.backdropColor, 0.45) }
            GradientStop { position: 0.44; color: Qt.alpha(root.backdropColor, 0.15) }
            GradientStop { position: 0.58; color: Qt.alpha(root.backdropColor, 0.0) }
            GradientStop { position: 1.00; color: Qt.alpha(root.backdropColor, 0.0) }
        }
    }
}
