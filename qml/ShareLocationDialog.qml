import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Sharing a PLACE — `m.location`, MSC3488.
//
// # Why there is no map, and no "use my location"
//
// Lightning has no position source. There is no GPS on a desktop, and
// deriving one from an IP address would be both inaccurate and a privacy
// decision nobody asked for — so this asks for the point instead of
// pretending to know it.
//
// There is no embedded map either: a map widget means tiles, and tiles mean
// every reader's IP address reaching a tile server the moment a message is
// rendered. Coordinates in, an OpenStreetMap link out.
//
// # And no LIVE sharing
//
// MSC3672 exists to be updated with new positions as you move. With no
// position source, a "live" share would publish one fixed point under a
// banner telling everyone it is current. RECEIVING one works fully.
Dialog {
    id: root
    objectName: "shareLocationDialog"

    /// The composer to send through — handed in, so this owns no globals.
    property var composer: null

    readonly property real lat: parseFloat(latField.text)
    readonly property real lon: parseFloat(lonField.text)
    readonly property bool valid:
        composer !== null && latField.text.trim().length > 0
        && lonField.text.trim().length > 0 && !isNaN(lat) && !isNaN(lon)
        && composer.locationIsValid(lat, lon)

    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(420, parent ? parent.width - AppTheme.spacing24 * 2 : 420)
    padding: AppTheme.spacing16

    function openDialog() {
        latField.text = ""
        lonField.text = ""
        placeField.text = ""
        pasteField.text = ""
        open()
    }

    /// Pull a latitude/longitude out of a pasted map link or coordinate pair.
    ///
    /// Deliberately permissive about the SHAPE and strict about the RESULT:
    /// people paste OpenStreetMap URLs, Google Maps URLs and bare "51.5,-0.1"
    /// pairs, and the useful thing is to find two numbers in any of them and
    /// then check they are a real point. Anything that does not yield two is
    /// left for the user to type.
    function parsePasted(text) {
        if (!text)
            return null
        // Query parameters first (`?mlat=..&mlon=..`, `?q=..`), then an
        // `@lat,lon` fragment, then any bare pair. Ordered most specific to
        // least, so a URL that carries both a centre and a marker gives the
        // marker.
        var m = /[?&]mlat=(-?\d+(?:\.\d+)?)[^]*?[?&]mlon=(-?\d+(?:\.\d+)?)/.exec(text)
        if (!m)
            m = /@(-?\d+(?:\.\d+)?),(-?\d+(?:\.\d+)?)/.exec(text)
        if (!m)
            m = /(?:^|[\/=,\s#])(-?\d{1,3}(?:\.\d+)?)\s*,\s*(-?\d{1,3}(?:\.\d+)?)/.exec(text)
        if (!m)
            return null
        var a = parseFloat(m[1])
        var b = parseFloat(m[2])
        if (isNaN(a) || isNaN(b))
            return null
        return { lat: a, lon: b }
    }

    background: Rectangle {
        color: AppTheme.surface
        border.color: AppTheme.border
        radius: AppTheme.radiusLg
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        Label {
            Layout.fillWidth: true
            text: qsTr("Share a place")
            color: AppTheme.textPrimary
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightStrong
        }

        // ── Paste a link ─────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            AppTextField {
                id: pasteField
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                placeholderText: qsTr("Paste a map link or coordinates")
                onTextChanged: {
                    var p = root.parsePasted(text)
                    if (p) {
                        latField.text = "" + p.lat
                        lonField.text = "" + p.lon
                    }
                }
            }
        }

        // ── Or type them ─────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    text: qsTr("Latitude")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.textMeta
                }
                AppTextField {
                    id: latField
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    placeholderText: "51.5008"
                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    text: qsTr("Longitude")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.textMeta
                }
                AppTextField {
                    id: lonField
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    placeholderText: "-0.1247"
                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                }
            }
        }

        AppTextField {
            id: placeField
            Layout.fillWidth: true
            placeholderText: qsTr("What is here? (optional)")
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textMeta
            // Says what LEAVES, which is the part that matters: this is a
            // real place and everyone in the room will see it.
            text: qsTr("The coordinates and this description are sent to the "
                       + "room as an ordinary message. Lightning does not "
                       + "know where you are — it sends exactly what is "
                       + "typed here.")
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: (latField.text.trim().length > 0
                      || lonField.text.trim().length > 0) && !root.valid
            color: AppTheme.danger
            font.pixelSize: AppTheme.textMeta
            text: qsTr("Latitude must be between -90 and 90, and longitude "
                       + "between -180 and 180.")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Item { Layout.fillWidth: true }
            AppButton {
                text: qsTr("Cancel")
                kind: "ghost"
                onClicked: root.close()
            }
            AppButton {
                objectName: "shareLocationSendButton"
                text: qsTr("Send")
                kind: "primary"
                enabled: root.valid
                onClicked: {
                    root.composer.sendLocation(root.lat, root.lon,
                                               placeField.text)
                    root.close()
                }
            }
        }
    }
}
