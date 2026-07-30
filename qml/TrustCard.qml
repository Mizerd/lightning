import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.6.5 (SPEC 1r): the verification/trust surface. Deliberately
// brand-fixed — navy + yellow in every theme, using ONLY AppTheme.trust*
// tokens and the brand face (Space Grotesk), never the active theme's
// palette. Purely presentational: every real value (steps, status text,
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
        anchors.fill: parent
        radius: AppTheme.radiusLg
        color: AppTheme.trustNavy
        border.width: 1
        border.color: AppTheme.trustChainBorder
    }

    // Oversized outline bolt watermark, top-right, cropped to the card.
    Icon {
        name: "bolt"
        size: 120
        opacity: 0.10
        color: AppTheme.trustYellow
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
                // Double ring: 2px navy gap (the card background showing
                // through) + 2px yellow stroke — same outline idiom as
                // SpacesRail's active-space ring.
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -4
                    radius: 18 + 4
                    color: "transparent"
                    border.width: 2
                    border.color: AppTheme.trustYellow
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    Layout.fillWidth: true
                    text: root.displayName.length > 0 ? root.displayName
                                                       : root.userId
                    color: AppTheme.trustInk
                    font.family: AppTheme.brandFont
                    font.pixelSize: AppTheme.fontTrustName
                    font.weight: Font.Bold
                    elide: Label.ElideRight
                }
                Label {
                    Layout.fillWidth: true
                    text: root.userId
                    color: AppTheme.trustMuted
                    font.family: AppTheme.monoFont
                    font.pixelSize: AppTheme.fontMonoXS
                    elide: Label.ElideMiddle
                }
            }
        }

        // ── Trust chain module ──────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: chainColumn.implicitHeight + 2 * AppTheme.spacing12
            radius: AppTheme.radiusLg
            color: AppTheme.trustChainBg
            border.width: 1
            border.color: AppTheme.trustChainBorder

            ColumnLayout {
                id: chainColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: AppTheme.spacing12
                spacing: AppTheme.spacing12

                RowLayout {
                    spacing: AppTheme.spacing6
                    Icon { name: "bolt"; size: 13; color: AppTheme.trustYellow }
                    Label {
                        text: qsTr("TRUST CHAIN")
                        color: AppTheme.trustCaption
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
                                        color: AppTheme.trustYellow
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
                                                color: AppTheme.trustPending
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
                                        anchors.centerIn: parent
                                        name: (modelData.iconName
                                               && modelData.iconName.length > 0)
                                              ? modelData.iconName : "check"
                                        size: stepItem.stepComplete ? 13 : 12
                                        color: stepItem.stepComplete
                                               ? AppTheme.trustNavy
                                               : AppTheme.trustPending
                                    }
                                }
                                Label {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: modelData.label || ""
                                    color: stepItem.stepComplete
                                           ? AppTheme.trustCaption
                                           : AppTheme.trustCaptionDim
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
                                       ? AppTheme.trustYellow : AppTheme.trustPending
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
                    color: AppTheme.trustMuted
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
                        color: AppTheme.trustVerifyInk
                    }
                    Label {
                        text: qsTr("Verify")
                        color: AppTheme.trustVerifyInk
                        font.pixelSize: 13
                        font.weight: Font.Bold
                    }
                }
                background: Rectangle {
                    radius: AppTheme.radiusTile
                    color: (verifyButton.hovered || verifyButton.down)
                           ? Qt.alpha(AppTheme.trustPending, 0.25) : "transparent"
                    border.width: 1
                    border.color: AppTheme.trustPending
                }
                // Keyboard focus indicator, same convention as every other
                // custom button this round; brand yellow so it reads on
                // navy.
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -3
                    radius: AppTheme.radiusTile + 3
                    color: "transparent"
                    border.color: AppTheme.trustYellow
                    border.width: 2
                    visible: verifyButton.visualFocus
                }
                onClicked: root.verifyRequested()
            }
            Item { Layout.fillWidth: true }
        }
    }
}
