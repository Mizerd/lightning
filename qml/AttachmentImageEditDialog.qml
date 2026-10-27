import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shared room/thread attachment editor. Every preview URL is a re-encoded
// raster working copy from AttachmentQueueModel; the chosen file stays intact.
AppDialog {
    id: root
    property var queue
    property int row: -1
    property string previewUrl: ""
    property int previewWidth: 1
    property int previewHeight: 1
    property string errorText: ""
    property bool animatedSource: false
    property real cropLeft: 0
    property real cropTop: 0
    property real cropRight: 1
    property real cropBottom: 1
    property int turns: 0
    property bool flipped: false
    property var strokes: []
    property var activeStroke: []
    property var history: []

    title: qsTr("Edit image before sending")
    modal: true
    focus: true
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(620, parent ? parent.width - 32 : 620)
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape

    function openFor(model, index) {
        queue = model
        row = index
        cropLeft = 0; cropTop = 0; cropRight = 1; cropBottom = 1
        turns = 0; flipped = false; strokes = []; activeStroke = []
        history = []
        errorText = ""
        refresh()
        open()
    }
    function refresh() {
        if (!queue || row < 0) return
        var result = queue.renderImageEdit(row, cropLeft, cropTop,
                                           cropRight, cropBottom,
                                           turns, flipped, strokes)
        if (!result || !result.url) {
            errorText = result && result.error ? result.error
                                               : qsTr("The image edit failed.")
            return
        }
        previewUrl = result.url.toString()
        previewWidth = result.width
        previewHeight = result.height
        animatedSource = result.animated === true
        errorText = ""
    }
    function changeTransform(rotation, flip) {
        remember()
        // Strokes are drawn in the displayed frame. Clearing them when the
        // frame changes keeps their coordinates unambiguous.
        strokes = []; activeStroke = []
        turns = (turns + rotation) % 4
        if (flip) flipped = !flipped
        refresh()
    }
    function remember() {
        history = history.concat([{
            left: cropLeft, top: cropTop, right: cropRight,
            bottom: cropBottom, turns: turns, mirrored: flipped,
            strokes: strokes.slice()
        }])
    }
    function undo() {
        if (history.length === 0) return
        var last = history[history.length - 1]
        history = history.slice(0, -1)
        cropLeft = last.left; cropTop = last.top
        cropRight = last.right; cropBottom = last.bottom
        turns = last.turns; flipped = last.mirrored
        strokes = last.strokes; activeStroke = []
        refresh()
    }
    onClosed: {
        if (queue && row >= 0) queue.cancelImageEdit(row)
        previewUrl = ""
        queue = null
        row = -1
    }

    contentItem: ColumnLayout {
        spacing: 10
        Label {
            Layout.fillWidth: true
            text: root.animatedSource
                  ? qsTr("Editing this animation sends one still PNG frame.")
                  : qsTr("The original file stays unchanged. The preview is the image that will be sent.")
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
        }
        Label {
            visible: root.errorText.length > 0
            Layout.fillWidth: true
            text: root.errorText
            color: AppTheme.danger
            wrapMode: Text.WordWrap
        }
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 320
            Image {
                id: preview
                anchors.centerIn: parent
                width: Math.min(parent.width, parent.height
                                * root.previewWidth / Math.max(1, root.previewHeight))
                height: width * root.previewHeight / Math.max(1, root.previewWidth)
                source: root.previewUrl
                cache: false
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                MouseArea {
                    anchors.fill: parent
                    onPressed: function(mouse) {
                        root.activeStroke = [{x: mouse.x / width,
                                              y: mouse.y / height}]
                    }
                    onPositionChanged: function(mouse) {
                        if (!(pressedButtons & Qt.LeftButton)) return
                        var x = Math.max(0, Math.min(1, mouse.x / width))
                        var y = Math.max(0, Math.min(1, mouse.y / height))
                        root.activeStroke.push({x: x, y: y})
                        ink.requestPaint()
                    }
                    onReleased: {
                        if (root.activeStroke.length > 0) {
                            root.remember()
                            root.strokes = root.strokes.concat([root.activeStroke])
                            root.activeStroke = []
                            root.refresh()
                        }
                        ink.requestPaint()
                    }
                }
                Canvas {
                    id: ink
                    anchors.fill: parent
                    // Only the live stroke is drawn here. Committed strokes
                    // are already flattened into the preview PNG.
                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.clearRect(0, 0, width, height)
                        if (root.activeStroke.length < 2) return
                        ctx.strokeStyle = "#ff2d2d"
                        ctx.lineWidth = Math.max(2, width / 180)
                        ctx.lineCap = "round"
                        ctx.beginPath()
                        for (var i = 0; i < root.activeStroke.length; ++i) {
                            var p = root.activeStroke[i]
                            if (i === 0) ctx.moveTo(p.x * width, p.y * height)
                            else ctx.lineTo(p.x * width, p.y * height)
                        }
                        ctx.stroke()
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Button { text: qsTr("Rotate left"); onClicked: root.changeTransform(3, false) }
            Button { text: qsTr("Rotate right"); onClicked: root.changeTransform(1, false) }
            Button { text: qsTr("Mirror"); onClicked: root.changeTransform(0, true) }
            Button {
                text: qsTr("Undo")
                enabled: root.history.length > 0
                onClicked: root.undo()
            }
        }
        Label { text: qsTr("Crop: left, top, right and bottom") }
        GridLayout {
            columns: 2
            Layout.fillWidth: true
            Label { text: qsTr("Left") }
            Slider {
                Layout.fillWidth: true; from: 0; to: 0.9; value: root.cropLeft
                onPressedChanged: { if (pressed) root.remember(); else root.refresh() }
                onMoved: root.cropLeft = Math.min(value, root.cropRight - 0.01)
            }
            Label { text: qsTr("Top") }
            Slider {
                Layout.fillWidth: true; from: 0; to: 0.9; value: root.cropTop
                onPressedChanged: { if (pressed) root.remember(); else root.refresh() }
                onMoved: root.cropTop = Math.min(value, root.cropBottom - 0.01)
            }
            Label { text: qsTr("Right") }
            Slider {
                Layout.fillWidth: true; from: 0.1; to: 1; value: root.cropRight
                onPressedChanged: { if (pressed) root.remember(); else root.refresh() }
                onMoved: root.cropRight = Math.max(value, root.cropLeft + 0.01)
            }
            Label { text: qsTr("Bottom") }
            Slider {
                Layout.fillWidth: true; from: 0.1; to: 1; value: root.cropBottom
                onPressedChanged: { if (pressed) root.remember(); else root.refresh() }
                onMoved: root.cropBottom = Math.max(value, root.cropTop + 0.01)
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Button {
                text: qsTr("Reset")
                onClicked: {
                    root.remember()
                    root.cropLeft = 0; root.cropTop = 0
                    root.cropRight = 1; root.cropBottom = 1
                    root.turns = 0; root.flipped = false
                    root.strokes = []; root.activeStroke = []
                    root.refresh()
                }
            }
            Item { Layout.fillWidth: true }
            Button { text: qsTr("Cancel"); onClicked: root.close() }
            Button {
                text: qsTr("Use edited image")
                enabled: root.errorText.length === 0 && root.previewUrl.length > 0
                onClicked: {
                    if (root.queue.commitImageEdit(root.row)) root.close()
                    else root.errorText = qsTr("The edited image could not be queued.")
                }
            }
        }
    }
}
