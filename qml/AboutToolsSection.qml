import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Settings -> About, below the logo card: what to paste into a bug report, who
// the homeserver says it is, and "Reset local cache". Bound to app.about.
//
// Reset local cache clears only what can be rebuilt from the server (event
// cache, media cache, search index). It never touches the encryption keys or
// the sign-in, and the confirmation says so; CLAUDE.md §6.
ColumnLayout {
    id: root
    objectName: "aboutToolsSection"

    Layout.fillWidth: true
    spacing: AppTheme.spacing12

    readonly property var about: (typeof app !== "undefined" && app) ? app.about : null
    // How many enabled homeserver features the card lists before pointing at
    // the Developer tools for the rest.
    readonly property int featurePreviewCount: 6
    property bool versionCopied: false
    property Item resetOpener: null

    // The version summary is "Label: value" per line, built in C++. In an RTL
    // language the value (commit hash, version, platform) is a left-to-right
    // run embedded in an RTL sentence and the bidi algorithm drags its leading
    // digit or punctuation to the wrong side of the label. Isolate each value
    // (U+2066 LRI ... U+2069 PDI) so it stays one unit. Display only: the
    // clipboard text comes from AboutController::copyVersion, untouched.
    function isolateValues(summary) {
        return String(summary || "").split("\n").map(function (line) {
            const i = line.indexOf(": ")
            if (i < 0 || i + 2 >= line.length)
                return line
            return line.slice(0, i + 2) + "\u2066" + line.slice(i + 2) + "\u2069"
        }).join("\n")
    }

    // Same plane as every other settings card (see SettingsScreen's
    // SettingsCard).
    component AboutCard: Pane {
        Layout.fillWidth: true
        background: Rectangle {
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorder
            radius: AppTheme.radiusMd
        }
    }

    // The homeserver is asked once when the page is first shown, and again on
    // request; never in the background.
    function maybeLoadHomeserver() {
        if (root.visible && root.about && root.about.homeserverState === "idle")
            root.about.refreshHomeserver()
    }
    onVisibleChanged: maybeLoadHomeserver()
    Component.onCompleted: maybeLoadHomeserver()
    Connections {
        target: root.about
        function onHomeserverChanged() { root.maybeLoadHomeserver() }
    }

    Timer {
        id: copiedTimer
        interval: 1800
        onTriggered: root.versionCopied = false
    }

    // This build
    AboutCard {
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8
            Label {
                text: qsTr("This build")
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightStrong
            }
            Label {
                objectName: "aboutVersionSummary"
                Layout.fillWidth: true
                wrapMode: Text.WrapAnywhere
                textFormat: Text.PlainText
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: root.about ? root.isolateValues(root.about.versionSummary) : ""
            }
            RowLayout {
                spacing: AppTheme.spacing8
                AppButton {
                    storm: true
                    objectName: "aboutCopyVersionButton"
                    text: root.versionCopied ? qsTr("Copied") : qsTr("Copy version")
                    Accessible.description: qsTr("Copies the version, commit, platform and package kind")
                    onClicked: {
                        root.about.copyVersion()
                        root.versionCopied = true
                        copiedTimer.restart()
                    }
                }
                AppButton {
                    storm: true
                    objectName: "aboutReportIssueButton"
                    text: qsTr("Report an issue")
                    Accessible.description: qsTr("Opens the issue tracker in your browser")
                    onClicked: Qt.openUrlExternally(root.about.reportIssueUrl)
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: qsTr("Paste the version into your report. Please leave out "
                           + "anything private.")
            }
        }
    }

    // Homeserver
    AboutCard {
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8
            Label {
                text: qsTr("Homeserver")
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightStrong
            }
            Label {
                objectName: "aboutHomeserverStatus"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                visible: text !== ""
                text: {
                    const s = root.about ? root.about.homeserverState : "idle"
                    if (s === "loading") return qsTr("Asking the homeserver...")
                    if (s === "unsupported")
                        return qsTr("This backend cannot ask a homeserver.")
                    if (s === "failed")
                        return root.about.homeserver.error || ""
                    return ""
                }
            }
            GridLayout {
                columns: 2
                columnSpacing: AppTheme.spacing12
                rowSpacing: AppTheme.spacing4
                visible: root.about && root.about.homeserverState === "ready"
                Label {
                    text: qsTr("Address")
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textMeta
                }
                Label {
                    objectName: "aboutHomeserverAddress"
                    Layout.fillWidth: true
                    wrapMode: Text.WrapAnywhere
                    textFormat: Text.PlainText
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textBody
                    text: root.about ? (root.about.homeserver.address || "") : ""
                }
                Label {
                    text: qsTr("Software")
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textMeta
                }
                Label {
                    objectName: "aboutHomeserverSoftware"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textBody
                    text: {
                        const h = root.about ? root.about.homeserver : ({})
                        if (!h.softwareName) return qsTr("Not published by this server")
                        return h.softwareVersion
                            ? h.softwareName + " " + h.softwareVersion
                            : h.softwareName
                    }
                }
                Label {
                    text: qsTr("Features")
                    color: AppTheme.stormTextMuted
                    font.pixelSize: AppTheme.textMeta
                }
                Label {
                    objectName: "aboutHomeserverFeatures"
                    Layout.fillWidth: true
                    wrapMode: Text.WrapAnywhere
                    textFormat: Text.PlainText
                    color: AppTheme.stormText
                    font.pixelSize: AppTheme.textMeta
                    text: {
                        const h = root.about ? root.about.homeserver : ({})
                        const on = (h.features || []).filter(f => f.enabled)
                                                    .map(f => f.name)
                        const specs = (h.versions || [])
                        const head = specs.length > 0
                            ? qsTr("Matrix %1").arg(specs[specs.length - 1]) : ""
                        if (on.length === 0)
                            return head || qsTr("None listed")
                        const shown = on.slice(0, root.featurePreviewCount).join(", ")
                        const more = on.length > root.featurePreviewCount
                            ? qsTr(" and %n more (see Developer tools)", "",
                                   on.length - root.featurePreviewCount)
                            : ""
                        return (head ? head + "; " : "") + shown + more
                    }
                }
            }
            AppButton {
                storm: true
                objectName: "aboutHomeserverRefresh"
                text: qsTr("Refresh")
                enabled: root.about && root.about.homeserverState !== "loading"
                onClicked: root.about.refreshHomeserver()
            }
        }
    }

    // Local data
    AboutCard {
        ColumnLayout {
            width: parent.width
            spacing: AppTheme.spacing8
            Label {
                text: qsTr("Local data")
                color: AppTheme.stormTextSecondary
                font.pixelSize: AppTheme.textBody
                font.weight: AppTheme.weightStrong
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                color: AppTheme.stormTextMuted
                font.pixelSize: AppTheme.textMeta
                text: qsTr("If rooms look stale or stuck, clearing the local cache "
                           + "makes Lightning fetch them from the server again.")
            }
            RowLayout {
                spacing: AppTheme.spacing12
                AppButton {
                    id: resetCacheButton
                    storm: true
                    objectName: "aboutResetCacheButton"
                    text: qsTr("Reset local cache")
                    kind: "danger"
                    enabled: root.about && root.about.resetState !== "running"
                    onClicked: {
                        root.resetOpener = resetCacheButton
                        resetCacheConfirm.open()
                    }
                }
                BusyIndicator {
                    running: root.about && root.about.resetState === "running"
                    visible: running
                    Layout.preferredWidth: 24
                    Layout.preferredHeight: 24
                }
            }
            Label {
                objectName: "aboutResetSummary"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                font.pixelSize: AppTheme.textMeta
                visible: text !== ""
                color: root.about && root.about.resetState === "failed"
                       ? AppTheme.danger : AppTheme.stormTextMuted
                text: root.about ? root.about.resetSummary : ""
            }
        }
    }

    // Themed like SettingsScreen's ConfirmDialog (an inline component there,
    // so not reachable from this file). Parented to the overlay so it centres
    // on the window, not on this column.
    Dialog {
        id: resetCacheConfirm
        objectName: "aboutResetCacheConfirm"
        title: qsTr("Reset the local cache?")
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        width: 360
        padding: AppTheme.spacing20
        closePolicy: Popup.CloseOnEscape
        onAccepted: root.about.resetLocalCache()
        // A Popup closes with Qt.PopupFocusReason, which QQC2 does not count as
        // keyboard focus, so the opener got focus back with no ring drawn. Land
        // on the safe button when it opens, and hand focus back to the opener
        // with a keyboard reason when it closes.
        onOpened: resetCancelButton.forceActiveFocus(Qt.TabFocusReason)
        onClosed: {
            if (root.resetOpener)
                root.resetOpener.forceActiveFocus(Qt.TabFocusReason)
        }
        background: Rectangle {
            color: AppTheme.stormPanel
            border.color: AppTheme.stormBorderStrong
            border.width: 1
            radius: AppTheme.radiusLg
        }
        header: Label {
            text: resetCacheConfirm.title
            color: AppTheme.stormText
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightBold
            elide: Label.ElideRight
            leftPadding: AppTheme.spacing20
            rightPadding: AppTheme.spacing20
            topPadding: AppTheme.spacing16
            bottomPadding: AppTheme.spacing8
        }
        footer: Item {
            implicitHeight: resetFooter.implicitHeight + AppTheme.spacing20 * 2
            RowLayout {
                id: resetFooter
                anchors.right: parent.right
                anchors.rightMargin: AppTheme.spacing20
                anchors.verticalCenter: parent.verticalCenter
                spacing: AppTheme.spacing8
                AppButton {
                    id: resetCancelButton
                    objectName: "aboutResetCacheCancelButton"
                    storm: true
                    text: qsTr("Cancel")
                    onClicked: resetCacheConfirm.reject()
                }
                AppButton {
                    objectName: "aboutResetCacheConfirmButton"
                    storm: true
                    kind: "dangerPrimary"
                    text: qsTr("Reset cache")
                    onClicked: resetCacheConfirm.accept()
                }
            }
        }
        Label {
            width: 300
            wrapMode: Text.WordWrap
            color: AppTheme.stormTextSecondary
            font.pixelSize: AppTheme.textBody
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            text: qsTr("This clears three things on this device: the event cache "
                       + "(messages stored for quick opening), the media cache, "
                       + "and the search index. Rooms reload from the server and "
                       + "media downloads again, and searching your history stops "
                       + "working until Lightning has indexed it again.\n\n"
                       + "It does not touch your encryption keys, your sign-in "
                       + "or any message on the server, and it does not sign "
                       + "you out.")
        }
    }
}
