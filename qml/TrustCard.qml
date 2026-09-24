import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Verification/trust surface. Uses the routed storm* roles like every other
// surface and owns no colour tokens (AppTheme.qml records the old mapping);
// Space Grotesk on the display name keeps the brand identity. Purely
// presentational: every value (steps, status, whether Verify applies) comes
// from the caller. It never reads the app context, never invents trust and
// never promotes local UI state to SDK trust. The embedding surface derives
// `steps` from CryptoHealthModel/sessionDevices and wires verifyRequested to
// own-verification.
Item {
    id: root
    objectName: "trustCard"

    property string displayName: ""
    property string userId: ""
    property string avatarMxc: ""
    // [{label: string, iconName: string, complete: bool}, ...]
    property var steps: []
    property string statusText: ""
    // Verify is the only action (SAS is the only real flow).
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
        // The SettingsCard pair (stormPanel/stormBorder), so this reads as one
        // of the page's cards.
        color: AppTheme.stormPanel
        border.width: 1
        border.color: AppTheme.stormBorder
    }

    // Decorative bolt watermark, cropped to the card (stormWatermark, as in
    // IdentityCard and MemberProfilePopover); exempt from the non-text contrast
    // bar.
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

        // Header: avatar with double ring + identity
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
                // Double ring: a 2px gap of card ground plus a 2px stroke, like
                // the rail's active ring. wordmarkBolt: a brand mark around a
                // face, not a state light.
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
                    // Untrusted text: never markup.
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

        // Trust chain module
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
                    // The adjacent caption carries the meaning.
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
                            // Only non-last steps stretch, so connectors fill
                            // the gaps evenly.
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
                                        // Bolt means complete here: a state, not
                                        // decoration.
                                        color: AppTheme.bolt
                                    }
                                    // Pending nodes get a dashed ring of eight
                                    // Rectangles: QtQuick.Shapes is not linked,
                                    // and Canvas paints nothing under the
                                    // offscreen platform.
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
                                        // Complete: the glyph sits on the bolt
                                        // disc, so boltInk. Pending:
                                        // stormTextMuted, which meets AA on the
                                        // input fill everywhere.
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
                                // Through the node centres (24px nodes, 2px
                                // bar: top offset 11), so the chain reads as
                                // one path.
                                Layout.alignment: Qt.AlignTop
                                Layout.topMargin: 11
                                implicitHeight: 2
                                // A segment is complete only when both ends
                                // are.
                                color: (stepItem.stepComplete && stepItem.nextComplete)
                                       ? AppTheme.bolt : AppTheme.stormBorderStrong
                            }
                        }
                    }
                }

                // Status copy inside the chain module, so it reads as one
                // object.
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

        // Actions: Verify only
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
                    // A quiet outlined control; an accent hover would compete
                    // with the bolt discs.
                    color: (verifyButton.hovered || verifyButton.down)
                           ? Qt.alpha(AppTheme.stormBorderStrong, 0.25)
                           : "transparent"
                    border.width: 1
                    border.color: AppTheme.stormBorderStrong
                }
                // Keyboard focus indicator: the app-wide focusRing.
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
