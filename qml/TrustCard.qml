import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.6.5 (SPEC 1r): the verification/trust surface.
//
// 2026-08-26: this card USED to be brand-fixed — navy + yellow in every
// theme, through ten AppTheme.trust* tokens pinned to the raw Storm
// literals. That was deliberate ("the trust moment is the brand moment")
// and it became wrong when its neighbourhood moved: the card sits between
// SettingsCards painted stormCanvas/stormBorder, above a sessions list
// painted stormTextFaint/stormLink, so on any theme but Storm it was the
// one surface on the page that ignored the user's choice. Reported as "the
// blue lightning session status should match the rest of the theme".
//
// It now reaches for the routed storm* namespace BY ROLE, like every other
// surface, and owns no colour tokens of its own; AppTheme.qml records the
// old-token -> role mapping where the pin used to live. The brand face
// (Space Grotesk) on the display name STAYS — the complaint was colour, and
// the face carries the remaining brand identity at no cost to legibility.
//
// Purely presentational: every real value (steps, status text,
// whether Verify applies) is supplied by the caller through properties, so
// this file never reaches into the application context property directly,
// never invents trust for anyone, and never promotes local UI state to SDK
// trust. The embedding surface (see the implementer's embed spec) is
// responsible for deriving `steps` from real CryptoHealthModel /
// sessionDevices state and for wiring `verifyRequested` to the existing
// own-verification start path.
Item {
    id: root
    objectName: "trustCard"

    property string displayName: ""
    property string userId: ""
    property string avatarMxc: ""
    // [{label: string, iconName: string, complete: bool}, ...]
    property var steps: []
    property string statusText: ""
    // Verify is the ONLY action this card ever offers (no self-Message
    // action, no QR — SAS verification is the only real flow).
    property bool showVerify: false

    signal verifyRequested()

    implicitWidth: 320
    implicitHeight: body.implicitHeight + 2 * AppTheme.spacing16
    clip: true

    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Trust status for %1").arg(
        root.displayName.length > 0 ? root.displayName : root.userId)

    Rectangle {
        objectName: "trustCardSurface"
        anchors.fill: parent
        radius: AppTheme.radiusLg
        // Deliberately the SettingsCard pair (SettingsScreen.qml's
        // SettingsCard paints exactly these two), so the card reads as one
        // of the page's cards rather than as something pasted onto it.
        color: AppTheme.stormCanvas
        border.width: 1
        border.color: AppTheme.stormBorder
    }

    // Oversized outline bolt watermark, top-right, cropped to the card.
    // stormWatermark is the app's hero-card watermark treatment (the same
    // token IdentityCard and MemberProfilePopover use) — a 12%-alpha bolt.
    // Purely decorative: it carries no information, so it is exempt from
    // the 3:1 non-text bar that the raw accent misses on Indigo Night.
    Icon {
        name: "bolt"
        size: 120
        color: AppTheme.stormWatermark
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: -30
        anchors.rightMargin: -30
    }

    ColumnLayout {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: AppTheme.spacing16
        spacing: AppTheme.spacing16

        // ── Header: avatar with double ring + identity ──────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing12

            Item {
                implicitWidth: 58
                implicitHeight: 58

                Avatar {
                    anchors.fill: parent
                    size: 58
                    circle: false
                    squareRadius: 18
                    mxc: root.avatarMxc
                    name: root.displayName.length > 0 ? root.displayName
                                                       : root.userId
                    colorKey: root.userId
                }
                // Double ring: a 2px gap of the card ground showing through
                // + a 2px stroke — same outline idiom as SpacesRail's
                // active-space ring. wordmarkBolt, not bolt: this ring is a
                // brand mark around a face, not a state, and the raw accent
                // beside plain header text reads as a status light (the same
                // reasoning AppTheme.qml records for the wordmark's bolt).
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -4
                    radius: 18 + 4
                    color: "transparent"
                    border.width: 2
                    border.color: AppTheme.wordmarkBolt
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    Layout.fillWidth: true
                    text: root.displayName.length > 0 ? root.displayName
                                                       : root.userId
                    textFormat: Text.PlainText
                    color: AppTheme.stormText
                    font.family: AppTheme.brandFont
                    font.pixelSize: AppTheme.fontTrustName
                    font.weight: Font.Bold
                    elide: Label.ElideRight
                }
                Label {
                    // Remote or externally chosen text: never markup.
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: root.userId
                    color: AppTheme.stormTextMuted
                    font.family: AppTheme.monoFont
                    font.pixelSize: AppTheme.fontMonoXS
                    elide: Label.ElideMiddle
                }
            }
        }

        // ── Trust chain module ──────────────────────────────────────────
        Rectangle {
            objectName: "trustChainPanel"
            Layout.fillWidth: true
            implicitHeight: chainColumn.implicitHeight + 2 * AppTheme.spacing12
            radius: AppTheme.radiusLg
            color: AppTheme.stormInset
            border.width: 1
            border.color: AppTheme.stormBorder

            ColumnLayout {
                id: chainColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: AppTheme.spacing12
                spacing: AppTheme.spacing12

                RowLayout {
                    spacing: AppTheme.spacing6
                    // The glyph labels the module the caption names, so its
                    // meaning is carried by the adjacent text either way.
                    Icon { name: "bolt"; size: 13; color: AppTheme.bolt }
                    Label {
                        text: qsTr("TRUST CHAIN")
                        color: AppTheme.stormTextSecondary
                        font.family: AppTheme.monoFont
                        font.pixelSize: AppTheme.fontChip
                        font.weight: Font.Bold
                        font.letterSpacing: AppTheme.trackingMono
                        font.capitalization: Font.AllUppercase
                    }
                }

                RowLayout {
                    id: nodeRow
                    objectName: "trustChainNodeRow"
                    Layout.fillWidth: true
                    spacing: 0

                    Repeater {
                        id: stepRepeater
                        objectName: "trustChainStepRepeater"
                        model: root.steps

                        delegate: RowLayout {
                            id: stepItem
                            spacing: 0
                            // Only non-last steps stretch, so the connector
                            // bar fills the real gap between two nodes
                            // evenly regardless of the module's width.
                            Layout.fillWidth: index < root.steps.length - 1

                            readonly property bool stepComplete:
                                modelData.complete === true
                            readonly property bool nextComplete:
                                index < root.steps.length - 1
                                && root.steps[index + 1]
                                && root.steps[index + 1].complete === true

                            Column {
                                spacing: AppTheme.spacing4

                                Item {
                                    width: 24
                                    height: 24
                                    anchors.horizontalCenter: parent.horizontalCenter

                                    Rectangle {
                                        objectName: "trustNodeFill"
                                        visible: stepItem.stepComplete
                                        anchors.fill: parent
                                        radius: 12
                                        // State, not decoration: bolt is
                                        // "active/selected/complete/primary
                                        // ONLY" and complete is exactly this.
                                        color: AppTheme.bolt
                                    }
                                    // Pending nodes get a dashed ring. Drawn
                                    // as eight tangential dash Rectangles —
                                    // QtQuick.Shapes is not linked here, and
                                    // Canvas proved to paint NOTHING under
                                    // the offscreen platform regardless of
                                    // render strategy (verified by pixel
                                    // assertion), so the ring is plain
                                    // declarative geometry that always
                                    // renders.
                                    Item {
                                        objectName: "trustNodeDashRing"
                                        visible: !stepItem.stepComplete
                                        anchors.fill: parent
                                        Repeater {
                                            model: 8
                                            delegate: Rectangle {
                                                required property int index
                                                readonly property real angle:
                                                    index * Math.PI / 4
                                                width: 6
                                                height: 2.5
                                                radius: 1.25
                                                color: AppTheme.stormBorderStrong
                                                x: parent.width / 2
                                                   + Math.cos(angle)
                                                     * (parent.width / 2 - 2.25)
                                                   - width / 2
                                                y: parent.height / 2
                                                   + Math.sin(angle)
                                                     * (parent.height / 2 - 2.25)
                                                   - height / 2
                                                rotation: angle * 180 / Math.PI
                                                          + 90
                                            }
                                        }
                                    }
                                    Icon {
                                        objectName: "trustNodeIcon"
                                        anchors.centerIn: parent
                                        name: (modelData.iconName
                                               && modelData.iconName.length > 0)
                                              ? modelData.iconName : "check"
                                        size: stepItem.stepComplete ? 13 : 12
                                        // COMPLETE: this glyph sits ON the
                                        // bolt disc, so it is boltInk — never
                                        // the card surface. It only ever
                                        // looked right as trustNavy because
                                        // the pinned card fill happened to be
                                        // navy; routed, that would have put
                                        // the page ground on a yellow disc.
                                        // PENDING: was trustPending
                                        // (borderStrong), which measures
                                        // 1.76-3.50:1 on inputBackground
                                        // across the themes — an illegible
                                        // 12px glyph. stormTextMuted is
                                        // AA-covered on that fill everywhere.
                                        color: stepItem.stepComplete
                                               ? AppTheme.boltInk
                                               : AppTheme.stormTextMuted
                                    }
                                }
                                Label {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: modelData.label || ""
                                    textFormat: Text.PlainText
                                    color: stepItem.stepComplete
                                           ? AppTheme.stormTextSecondary
                                           : AppTheme.stormTextMuted
                                    font.family: AppTheme.monoFont
                                    font.pixelSize: AppTheme.fontMicro
                                    font.capitalization: Font.AllUppercase
                                    horizontalAlignment: Text.AlignHCenter
                                }
                            }

                            Rectangle {
                                objectName: "trustChainConnector"
                                visible: index < root.steps.length - 1
                                Layout.fillWidth: true
                                // Run through the NODE centres (24px nodes →
                                // centre 12, bar 2px → top offset 11), not
                                // the centre of the node+caption column —
                                // the chain must read as one path.
                                Layout.alignment: Qt.AlignTop
                                Layout.topMargin: 11
                                implicitHeight: 2
                                // A segment is only fully trusted when BOTH
                                // ends of it are complete.
                                color: (stepItem.stepComplete && stepItem.nextComplete)
                                       ? AppTheme.bolt : AppTheme.stormBorderStrong
                            }
                        }
                    }
                }

                // Status copy lives INSIDE the chain module (SPEC 1r) so
                // the module reads as one object.
                Label {
                    Layout.fillWidth: true
                    visible: root.statusText.length > 0
                    text: root.statusText
                    textFormat: Text.PlainText
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.fontChip
                    wrapMode: Text.WordWrap
                }
            }
        }

        // ── Actions: Verify only — no Message, no QR. ───────────────────
        RowLayout {
            Layout.fillWidth: true
            visible: root.showVerify
            spacing: AppTheme.spacing8

            AbstractButton {
                id: verifyButton
                objectName: "trustCardVerifyButton"
                text: qsTr("Verify")
                implicitHeight: 32
                leftPadding: 14
                rightPadding: 14
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Verify this session")
                contentItem: RowLayout {
                    spacing: AppTheme.spacing6
                    Icon {
                        name: "verified_user"
                        size: 16
                        color: AppTheme.stormTextSecondary
                    }
                    Label {
                        text: qsTr("Verify")
                        color: AppTheme.stormTextSecondary
                        font.pixelSize: 13
                        font.weight: Font.Bold
                    }
                }
                background: Rectangle {
                    radius: AppTheme.radiusTile
                    // A straight re-point of the old treatment, deliberately
                    // NOT accentSoft: the button is a quiet outlined control
                    // here, and an accent-tinted hover would make it compete
                    // with the bolt discs it sits under.
                    color: (verifyButton.hovered || verifyButton.down)
                           ? Qt.alpha(AppTheme.stormBorderStrong, 0.25)
                           : "transparent"
                    border.width: 1
                    border.color: AppTheme.stormBorderStrong
                }
                // Keyboard focus indicator. It was hand-rolled brand yellow
                // ("so it reads on navy") back when the card was always navy;
                // with the card themed, the app-wide focusRing is both
                // correct and the reason this control stops being the one
                // button in Lightning with its own focus colour.
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -3
                    radius: AppTheme.radiusTile + 3
                    color: "transparent"
                    border.color: AppTheme.focusRing
                    border.width: 2
                    visible: verifyButton.visualFocus
                }
                onClicked: root.verifyRequested()
            }
            Item { Layout.fillWidth: true }
        }
    }
}
