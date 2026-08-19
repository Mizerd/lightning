import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.7.3: the "an update is available" corner card.
//
// Deliberately the same shape as VerifySessionPrompt rather than a dialog: an
// update is never urgent enough to stand between the user and their messages,
// so it asks once, offers two honest answers, and gets out of the way. The
// two prompts share one bottom-right column in Main.qml, so they stack
// instead of drawing on top of each other when a session is also unverified.
//
// "Update" does NOT start installing. It opens the ONE update dialog, which
// carries the release notes, the download size and the honest per-package
// rules — a package-managed installation cannot be updated in place at all.
// That dialog used to open itself; it no longer does, because two
// self-opening surfaces for one event is the duplication its own header
// warns about, and a modal is the wrong weight for something never urgent.
// "Not now" is a real, persisted dismissal scoped to THIS version: a later
// release asks again, and the rail badge stays regardless.
Rectangle {
    id: root

    // One-shot suppression for this run of the shell, so the card cannot
    // flash back behind the dialog it just opened.
    property bool suppressed: false

    // Emitted when the user presses Update; Main.qml opens the shared
    // UpdateAvailableDialog, so this component knows nothing about it.
    signal detailsRequested()

    readonly property var um: app.updateManager

    readonly property bool shouldShow:
        um && um.updateAvailableWarning
        && !suppressed
        // The chat shell only. In Settings the Updates page is already on
        // screen (or one click away in the nav), and a floating card
        // offering to open the page the user is looking at is noise — the
        // same reasoning VerifySessionPrompt applies to the Sessions page.
        && app.currentScreen === 1

    // Re-arm when a genuinely different version turns up, so dismissing
    // 0.7.3 never silences 0.7.4.
    readonly property string _version: um ? um.latestVersion : ""
    on_VersionChanged: suppressed = false

    visible: opacity > 0
    opacity: shouldShow ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 140 } }

    implicitWidth: 320
    implicitHeight: promptColumn.implicitHeight + AppTheme.spacing16 * 2
    height: implicitHeight
    width: implicitWidth

    color: AppTheme.stormPanel
    // radiusLg like its corner-card siblings (VerifySessionPrompt,
    // IncomingCallPrompt) — they stack in the same corner column.
    radius: AppTheme.radiusLg
    border.width: 1
    border.color: AppTheme.stormBorder

    objectName: "updateAvailablePrompt"

    ColumnLayout {
        id: promptColumn
        anchors.fill: parent
        anchors.margins: AppTheme.spacing16
        spacing: AppTheme.spacing8

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            // Warning-toned, not danger: a pending update is not a security
            // failure, and colouring it like one would devalue the red badge
            // an unverified session actually needs.
            // The corner-card icon vocabulary its siblings use
            // (VerifySessionPrompt's Icon + Bold title) — the bespoke
            // "!"-in-a-circle sat directly above them looking foreign.
            Icon {
                Layout.alignment: Qt.AlignVCenter
                name: "download"
                size: 18
                color: AppTheme.warning
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: AppTheme.stormText
                font.pixelSize: AppTheme.fontSecondary
                font.weight: Font.Bold
                text: qsTr("Lightning %1 is available")
                      .arg(root.um ? root.um.latestVersion : "")
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextSecondary
            font.pixelSize: AppTheme.fontCaption
            text: qsTr("You have %1.").arg(root.um ? root.um.currentVersion : "")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Item { Layout.fillWidth: true }
            AppButton {
                storm: true
                objectName: "updatePromptDismissButton"
                text: qsTr("Not now")
                onClicked: {
                    root.suppressed = true;
                    if (root.um)
                        root.um.dismissVersion();
                }
            }
            AppButton {
                storm: true
                kind: "primary"
                objectName: "updatePromptOpenButton"
                text: qsTr("Update")
                onClicked: {
                    root.suppressed = true;
                    root.detailsRequested();
                }
            }
        }
    }
}
