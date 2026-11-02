import QtQuick
import QtQuick.Effects
import MatrixClient

Item {
    id: root

    implicitHeight: 190
    enabled: false

    property color backdropColor: AppTheme.background
    readonly property color accentColor: AppTheme.accent

    readonly property bool onScreen: root.visible
                                     && Window.window !== null
                                     && Window.window.visible
                                     && Window.window.visibility
                                        !== Window.Minimized
    readonly property bool animating: root.onScreen && !AppTheme.reducedMotion

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
        // A hidden band generates nothing (it exists for every Settings
        // section). Reading root.visible here re-evaluates the sources when
        // About shows.
        if (!root.visible)
            return ""
        return "image://storm-band/" + layer + root.themeQuery
    }

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

    readonly property var strikeA:
        [[0, 0], [.84, 0], [.844, 1], [.85, .12], [.856, .95], [.866, .08],
         [.874, 0], [1, 0]]
    readonly property var strikeB:
        [[0, 0], [.61, 0], [.614, .9], [.622, .1], [.628, 1], [.64, 0],
         [1, 0]]
    readonly property var emberCurve: [[0, 0], [.12, .9], [.7, .6], [1, 0]]

    readonly property var boltDefs: [
        { xf: .17, w: 78, cycle: 9400,  delay: 0,     curve: "A", bloom: "bloomA", sprite: "bolt1" },
        { xf: .38, w: 62, cycle: 13700, delay: 2600,  curve: "B", bloom: "bloomB", sprite: "bolt2" },
        { xf: .57, w: 86, cycle: 21300, delay: 5200,  curve: "A", bloom: "bloomC", sprite: "bolt3" },
        { xf: .74, w: 58, cycle: 17100, delay: 8400,  curve: "B", bloom: "bloomA", sprite: "bolt4" },
        { xf: .89, w: 72, cycle: 27900, delay: 3700,  curve: "A", bloom: "bloomB", sprite: "bolt5" },
        { xf: .05, w: 52, cycle: 31400, delay: 11500, curve: "B", bloom: "bloomC", sprite: "bolt6" }
    ]
    readonly property var emberDefs:
        [[9, 24, 9, 0], [17, 30, 12.5, 2.4], [26, 20, 10.8, 5.1],
         [34, 28, 14, 1.2], [43, 22, 11.4, 6.8], [51, 32, 13.2, 3.6],
         [60, 18, 10.2, 8.1], [68, 26, 15.3, 4.4], [77, 21, 12.1, 9.6],
         [85, 29, 13.9, 2.1], [94, 23, 11.7, 7.3]]

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

    // The mask is a MultiEffect, which the software scene graph cannot draw:
    // the whole scene would render as nothing. There the same ramp is laid
    // over the scene in the page's colour instead (`softwareFade` below).
    readonly property bool maskable:
        root.GraphicsInfo.api !== GraphicsInfo.Software

    Item {
        id: scene
        objectName: "stormBandScene"
        anchors.fill: parent
        clip: true
        layer.enabled: root.maskable
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: maskGradient
            maskThresholdMin: 0.0
            maskSpreadAtMin: 1.0
        }

        Image {
            anchors.fill: parent
            source: root.tileUrl("gradient")
            smooth: true
        }

        Scroller { layerKey: "sky"; tileW: 1536; cycleMs: 620000 }

        Image {
            x: scene.width * 0.11
            y: 62
            width: 36; height: 36
            smooth: false
            opacity: .9
            source: root.tileUrl("moon")
        }

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

        Image {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 70
            smooth: true
            source: root.tileUrl("horizon")
        }

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

    Rectangle {
        id: maskGradient
        anchors.fill: parent
        visible: false
        layer.enabled: root.maskable
        gradient: Gradient {
            GradientStop { position: 0.0;  color: Qt.rgba(0, 0, 0, 0) }
            GradientStop { position: 0.14; color: Qt.rgba(0, 0, 0, 0.18) }
            GradientStop { position: 0.30; color: Qt.rgba(0, 0, 0, 0.55) }
            GradientStop { position: 0.44; color: Qt.rgba(0, 0, 0, 0.85) }
            GradientStop { position: 0.58; color: Qt.rgba(0, 0, 0, 1) }
            GradientStop { position: 1.0;  color: Qt.rgba(0, 0, 0, 1) }
        }
    }

    Rectangle {
        objectName: "stormBandSoftwareFade"
        anchors.fill: parent
        visible: !root.maskable
        gradient: Gradient {
            GradientStop { position: 0.0;  color: Qt.rgba(root.backdropColor.r, root.backdropColor.g, root.backdropColor.b, 1) }
            GradientStop { position: 0.14; color: Qt.rgba(root.backdropColor.r, root.backdropColor.g, root.backdropColor.b, 0.82) }
            GradientStop { position: 0.30; color: Qt.rgba(root.backdropColor.r, root.backdropColor.g, root.backdropColor.b, 0.45) }
            GradientStop { position: 0.44; color: Qt.rgba(root.backdropColor.r, root.backdropColor.g, root.backdropColor.b, 0.15) }
            GradientStop { position: 0.58; color: Qt.rgba(root.backdropColor.r, root.backdropColor.g, root.backdropColor.b, 0) }
            GradientStop { position: 1.0;  color: Qt.rgba(root.backdropColor.r, root.backdropColor.g, root.backdropColor.b, 0) }
        }
    }
}
