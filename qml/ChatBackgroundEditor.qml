import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Chat background editor, shared by Room information, Space settings and
// Settings -> Appearance. Edits one of:
//
//   scopeKind "room":    the room's shared background (everyone here) or this
//                        account's own picture for the room (only me), plus
//                        "hide what others set here";
//   scopeKind "space":   the Space's shared background, which its rooms use
//                        unless they have their own;
//   scopeKind "default": this account's own picture for every room.
//
// Nothing is written until Apply. The shared choice is offered only when the
// room's power levels allow it (app.backdrops.canSetShared, false until the
// room answered), and says before upload that room state is not end-to-end
// encrypted.
ColumnLayout {
    id: editor
    objectName: "chatBackgroundEditor"

    property string scopeKind: "room"
    property string scopeId: ""
    // "shared" | "personal" once the user picks one; "" follows the room's
    // answer (shared when this account may set it, else only me). A Space
    // edits shared only; the default edits personal only.
    property string audience: ""
    // Show the group title (hosts with their own heading turn it off).
    property bool showTitle: true

    spacing: AppTheme.spacing8

    readonly property bool available: typeof app !== "undefined" && app
                                      && !!app.backdrops
    readonly property bool sharedPossible: available && scopeKind !== "default"
                                           && app.backdrops.sharedAvailable
                                           && scopeId !== ""
    readonly property bool canShare: {
        if (!editor.sharedPossible)
            return false
        var _dep = app.backdrops.revision
        return app.backdrops.canSetShared(editor.scopeId)
    }
    // A backend without shared backgrounds leaves a room with "only me".
    readonly property string effectiveAudience:
        scopeKind === "space" ? "shared"
        : (scopeKind === "default" || !sharedPossible ? "personal"
           : (audience !== "" ? audience : (canShare ? "shared" : "personal")))
    readonly property string personalKey: scopeKind === "default" ? "" : scopeId

    // The record being edited, as it is now.
    readonly property var current: {
        if (!editor.available)
            return ({})
        var _dep = app.backdrops.revision
        if (editor.effectiveAudience === "shared")
            return editor.scopeId !== "" ? app.backdrops.sharedFor(editor.scopeId)
                                         : ({})
        return app.backdrops.personalFor(editor.personalKey)
    }
    readonly property bool hasCurrent: {
        var c = editor.current
        if (!c)
            return false
        return editor.effectiveAudience === "shared" ? !!c.url : !!c.imageUrl
    }
    readonly property var prepared: editor.available ? app.backdrops.prepared : ({})
    readonly property bool hasPrepared: !!(editor.prepared && editor.prepared.imageUrl)

    // Pending presentation. Loaded from the current record whenever the
    // target changes and nothing has been touched.
    property real pDim: 0.2
    property real pBlur: 0
    property real pTint: 0.25
    property string pFit: "cover"
    property string pAlign: "center"
    property bool dirty: false
    /// A picked SVG is being converted to a PNG in the background.
    property bool convertingSvg: false

    function _presentationOf(record) {
        if (!record)
            return null
        if (record.presentation)
            return record.presentation
        if (record.dim !== undefined)
            return record   // personalFor() flattens presentation
        return null
    }
    function loadFromCurrent() {
        var p = _presentationOf(editor.current)
        editor.pDim = p && p.dim !== undefined ? p.dim : 0.2
        editor.pBlur = p && p.blur !== undefined ? p.blur : 0
        editor.pTint = p && p.tint !== undefined ? p.tint : 0.25
        editor.pFit = p && p.fit ? p.fit : "cover"
        editor.pAlign = p && p.align ? p.align : "center"
        editor.dirty = false
    }
    onCurrentChanged: if (!editor.dirty) editor.loadFromCurrent()
    onEffectiveAudienceChanged: editor.loadFromCurrent()
    onScopeIdChanged: {
        // Another room: its own answer decides again, and a picture picked
        // for the previous room is not carried over.
        editor.audience = ""
        editor.discardPicked()
        editor.loadFromCurrent()
        if (editor.sharedPossible)
            app.backdrops.refreshScope(editor.scopeId)
    }
    Component.onCompleted: {
        editor.loadFromCurrent()
        if (editor.sharedPossible)
            app.backdrops.refreshScope(editor.scopeId)
    }
    // A host created before its scope or the backend was ready asks once it is.
    onSharedPossibleChanged: {
        if (editor.sharedPossible)
            app.backdrops.refreshScope(editor.scopeId)
    }

    function presentation() {
        return { dim: editor.pDim, blur: editor.pBlur, tint: editor.pTint,
                 fit: editor.pFit, align: editor.pAlign }
    }

    // What the preview draws: the picked picture, or the current one, with
    // the pending presentation.
    readonly property var previewSpec: {
        var base = null
        if (editor.hasPrepared) {
            base = { kind: "staged", imageUrl: editor.prepared.imageUrl,
                     statsKey: editor.prepared.statsKey,
                     color: editor.prepared.color }
        } else if (editor.hasCurrent) {
            if (editor.effectiveAudience === "shared")
                base = { kind: "mxc", mxc: editor.current.url,
                         statsKey: "mxc:" + editor.current.url,
                         color: editor.current.color || "" }
            else
                base = { kind: "staged", imageUrl: editor.current.imageUrl,
                         statsKey: editor.current.statsKey,
                         color: editor.current.color || "" }
        }
        if (!base)
            return ({ kind: "none" })
        base.dim = editor.pDim
        base.blur = editor.pBlur
        base.tint = editor.pTint
        base.fit = editor.pFit
        base.align = editor.pAlign
        return base
    }

    function errorText(category) {
        // A refused SVG: the controller words the reason.
        if (category.indexOf("svg_") === 0)
            return app.backdrops.svgMessage(category.substring(4))
        // One honest sentence per category; every category the controller,
        // the FFI or rust/src/backdrop.rs can report is listed, and an unknown
        // one still shows its code rather than a sentence that says nothing.
        switch (category) {
        case "": return ""
        // Picking the picture.
        case "unsupported_image":
            return qsTr("That file is not a picture Lightning can use (PNG, "
                        + "JPEG, WebP, GIF or BMP).")
        case "too_large": return qsTr("That file is too large to use (over 64 MB).")
        case "undecodable":
            return qsTr("That picture is damaged or in a format this computer "
                        + "cannot open.")
        case "unreadable": return qsTr("That file could not be opened.")
        case "encode_failed":
            return qsTr("Lightning could not convert that picture. Try another one.")
        // Before anything is sent.
        case "no_picture": return qsTr("Choose a picture first.")
        case "write_failed":
        case "read_failed":
        case "not_a_file":
            return qsTr("Lightning could not stage the picture for upload: "
                        + "the temporary folder is not writable.")
        case "file_too_large":
            return qsTr("The converted picture is still too large to upload "
                        + "(over 16 MB). Try a smaller one.")
        case "invalid_content":
            return qsTr("Lightning could not build a valid background from "
                        + "these settings. Please report this.")
        case "not_joined": return qsTr("You are no longer in this room.")
        case "signed_out":
            return qsTr("Your session has ended. Sign in again, then retry.")
        case "rejected":
            return qsTr("Lightning could not start the upload. Please report "
                        + "this if it keeps happening.")
        // What the server said.
        case "forbidden":
            return qsTr("You are not allowed to change this background: your "
                        + "power level in this room is too low.")
        case "rate_limited":
            return qsTr("The server is limiting requests. Try again in a moment.")
        case "upload_too_large":
            return qsTr("The server refused the picture because it is larger "
                        + "than it accepts.")
        case "bad_json":
            return qsTr("The server refused the background's data. Please "
                        + "report this: it is a Lightning bug.")
        case "server_error":
            return qsTr("The server had a problem saving the background. Try "
                        + "again later.")
        case "server_refused":
            return qsTr("The server refused the background.")
        case "timeout":
            return qsTr("The server took too long to answer. Try again.")
        case "network":
            return qsTr("Could not reach the server. Check your connection "
                        + "and try again.")
        // Personal pictures.
        case "no_account": return qsTr("Sign in to keep your own backgrounds.")
        case "encrypted_room":
            return qsTr("Pictures from encrypted rooms cannot be used as a "
                        + "background.")
        case "too_many":
            return qsTr("You have your own background in too many rooms. "
                        + "Remove one first.")
        default:
            return qsTr("The background could not be saved (%1).").arg(category)
        }
    }

    component ToneSlider: Slider {
        id: tone
        from: 0
        to: 1
        stepSize: 0.05
        snapMode: Slider.SnapAlways
        Layout.fillWidth: true
        background: Rectangle {
            x: tone.leftPadding
            y: tone.topPadding + tone.availableHeight / 2 - 2
            width: tone.availableWidth
            height: 4
            radius: AppTheme.radiusPill
            color: AppTheme.stormInset
            Rectangle {
                width: tone.visualPosition * parent.width
                height: parent.height
                radius: AppTheme.radiusPill
                color: tone.enabled ? AppTheme.accent : AppTheme.textDisabled
            }
        }
        handle: Rectangle {
            x: tone.leftPadding + tone.visualPosition * (tone.availableWidth - width)
            y: tone.topPadding + tone.availableHeight / 2 - height / 2
            width: 16
            height: 16
            radius: 8
            color: AppTheme.surface
            border.width: tone.visualFocus ? 2 : 1
            border.color: tone.visualFocus ? AppTheme.focusRing : AppTheme.borderStrong
        }
    }

    component ToneRow: RowLayout {
        property alias label: toneLabel.text
        default property alias content: slot.data
        Layout.fillWidth: true
        spacing: AppTheme.spacing8
        Label {
            id: toneLabel
            Layout.preferredWidth: 72
            color: AppTheme.textSecondary
            font.pixelSize: AppTheme.textMeta
            elide: Label.ElideRight
        }
        Item {
            id: slot
            Layout.fillWidth: true
            implicitHeight: 28
        }
    }

    Label {
        visible: editor.showTitle
        text: editor.scopeKind === "default" ? qsTr("Chat background")
                                             : qsTr("Background")
        color: AppTheme.textSecondary
        font.pixelSize: AppTheme.textBody
        font.weight: AppTheme.weightStrong
    }

    // Who sees it. Rooms only; a Space is shared, the default is personal.
    SegmentedControl {
        objectName: "chatBackgroundAudience"
        visible: editor.scopeKind === "room" && editor.sharedPossible
        model: [
            { label: qsTr("Everyone here"), value: "shared" },
            { label: qsTr("Only me"), value: "personal" },
        ]
        current: editor.effectiveAudience
        onActivated: (value) => {
            editor.discardPicked()
            editor.audience = value
        }
    }

    function discardPicked() {
        if (editor.available && editor.hasPrepared)
            app.backdrops.discardPrepared()
    }

    // The preview, with the two inks that sit straight on a background.
    Rectangle {
        Layout.fillWidth: true
        implicitHeight: 128
        radius: AppTheme.radiusMd
        clip: true
        color: AppTheme.background
        border.color: AppTheme.border
        border.width: 1

        ChatBackdrop {
            anchors.fill: parent
            anchors.margins: 1
            spec: editor.previewSpec
            edgeFades: false
        }
        Column {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.margins: AppTheme.spacing12
            spacing: 2
            Label {
                width: parent.width
                text: qsTr("Messages stay readable on top of it.")
                color: AppTheme.textPrimary
                font.pixelSize: AppTheme.textBody
                elide: Label.ElideRight
            }
            Label {
                width: parent.width
                text: editor.previewSpec.kind === "none"
                      ? qsTr("No background")
                      : qsTr("Lightning dims the picture as much as this theme "
                             + "needs.")
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
                elide: Label.ElideRight
            }
        }
    }

    // Permission and privacy, before anything is uploaded.
    Label {
        Layout.fillWidth: true
        visible: editor.effectiveAudience === "shared" && editor.sharedPossible
                 && !editor.canShare
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        color: AppTheme.textMuted
        font.pixelSize: AppTheme.textMeta
        text: editor.scopeKind === "space"
              ? qsTr("Only people allowed to change this space's settings can "
                     + "set its background.")
              : qsTr("Only people allowed to change this room's settings can set "
                     + "a background everyone sees. You can still choose one "
                     + "just for yourself.")
    }
    Label {
        Layout.fillWidth: true
        visible: {
            if (editor.effectiveAudience !== "shared" || !editor.sharedPossible)
                return false
            var _dep = app.backdrops.revision
            return app.backdrops.sharedUnsupported(editor.scopeId)
        }
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        color: AppTheme.textMuted
        font.pixelSize: AppTheme.textMeta
        text: qsTr("A newer version of Lightning set this background, so it "
                   + "is not shown here.")
    }
    // This account's own picture for the room wins over the shared one, on
    // this account only. Said while editing the shared one, with the way
    // back: dropping the own picture shows the room's.
    ColumnLayout {
        objectName: "chatBackgroundOwnOverride"
        Layout.fillWidth: true
        spacing: AppTheme.spacing4
        visible: {
            if (editor.scopeKind !== "room" || editor.effectiveAudience !== "shared"
                    || !editor.available)
                return false
            var _dep = app.backdrops.revision
            var own = app.backdrops.personalFor(editor.scopeId)
            return !!(own && own.imageUrl)
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textMeta
            text: qsTr("Everyone here sees the room's picture. You chose your "
                       + "own picture for this room under \"Only me\", so on "
                       + "this account you see yours instead.")
        }
        AppButton {
            objectName: "chatBackgroundUseRoomPicture"
            text: qsTr("Use the room's picture instead")
            kind: "ghost"
            size: "sm"
            onClicked: app.backdrops.clearPersonal(editor.scopeId)
        }
    }
    Label {
        objectName: "chatBackgroundPrivacyNote"
        Layout.fillWidth: true
        visible: editor.effectiveAudience === "shared" && editor.canShare
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        color: AppTheme.textMuted
        font.pixelSize: AppTheme.textMeta
        text: {
            if (editor.scopeKind === "space")
                return qsTr("Everyone in this space sees this picture behind "
                            + "its rooms, unless a room has its own. It is "
                            + "stored on the homeserver unencrypted.")
            var encryptedOrUnknown = !app.backdrops.roomEncryptionKnown(editor.scopeId)
                                     || app.backdrops.roomEncrypted(editor.scopeId)
            return encryptedOrUnknown
                   ? qsTr("Room backgrounds are not end-to-end encrypted: "
                          + "everyone here sees this picture, and so can the "
                          + "homeserver, even though this room's messages are "
                          + "encrypted.")
                   : qsTr("Everyone here sees this picture in Lightning. "
                          + "Other apps ignore it.")
        }
    }

    readonly property bool editable: editor.available
                                     && (editor.effectiveAudience === "personal"
                                         || editor.canShare)
                                     && !app.backdrops.busy

    Flow {
        Layout.fillWidth: true
        spacing: AppTheme.spacing8
        AppButton {
            objectName: "chatBackgroundChoose"
            text: editor.hasCurrent || editor.hasPrepared
                  ? qsTr("Change picture…") : qsTr("Choose picture…")
            size: "sm"
            enabled: editor.editable
            onClicked: pictureDialog.open()
        }
        AppButton {
            objectName: "chatBackgroundApply"
            text: qsTr("Apply")
            kind: "primary"
            size: "sm"
            visible: editor.hasPrepared || (editor.dirty && editor.hasCurrent)
            enabled: editor.editable
            onClicked: {
                if (editor.effectiveAudience === "shared")
                    app.backdrops.setShared(editor.scopeId, editor.presentation())
                else
                    app.backdrops.setPersonal(editor.personalKey,
                                              editor.presentation())
                editor.dirty = false
            }
        }
        AppButton {
            text: qsTr("Cancel")
            kind: "ghost"
            size: "sm"
            visible: editor.hasPrepared || editor.dirty
            onClicked: {
                editor.discardPicked()
                editor.loadFromCurrent()
            }
        }
        AppButton {
            objectName: "chatBackgroundRemove"
            text: qsTr("Remove")
            kind: "danger"
            size: "sm"
            visible: editor.hasCurrent && !editor.hasPrepared
            enabled: editor.editable
            onClicked: {
                if (editor.effectiveAudience === "shared")
                    app.backdrops.clearShared(editor.scopeId)
                else
                    app.backdrops.clearPersonal(editor.personalKey)
            }
        }
    }

    // Presentation, once there is something to present.
    ColumnLayout {
        Layout.fillWidth: true
        visible: editor.hasPrepared || editor.hasCurrent
        enabled: editor.editable
        spacing: 2

        ToneRow {
            label: qsTr("Dim")
            ToneSlider {
                anchors.fill: parent
                value: editor.pDim
                Accessible.name: qsTr("Dim the picture")
                onMoved: { editor.pDim = value; editor.dirty = true }
            }
        }
        ToneRow {
            label: qsTr("Blur")
            ToneSlider {
                anchors.fill: parent
                value: editor.pBlur
                Accessible.name: qsTr("Blur the picture")
                onMoved: { editor.pBlur = value; editor.dirty = true }
            }
        }
        Label {
            Layout.fillWidth: true
            visible: editor.pBlur > 0.01 && app.softwareRenderer
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textMeta
            text: qsTr("Blur is off on this computer because it is drawing "
                       + "without graphics acceleration.")
        }
        ToneRow {
            label: qsTr("Tint")
            ToneSlider {
                anchors.fill: parent
                value: editor.pTint
                Accessible.name: qsTr("Tint the theme with the picture's colour")
                onMoved: { editor.pTint = value; editor.dirty = true }
            }
        }
        SegmentedControl {
            model: [
                { label: qsTr("Fill"), value: "cover" },
                { label: qsTr("Fit"), value: "contain" },
                { label: qsTr("Tile"), value: "tile" },
            ]
            current: editor.pFit
            onActivated: (value) => { editor.pFit = value; editor.dirty = true }
        }
        SegmentedControl {
            visible: editor.pFit !== "tile"
            model: [
                { label: qsTr("Centre"), value: "center" },
                { label: qsTr("Top"), value: "top" },
                { label: qsTr("Bottom"), value: "bottom" },
            ]
            current: editor.pAlign
            onActivated: (value) => { editor.pAlign = value; editor.dirty = true }
        }
    }

    // Per-room opt-out of what others set (rooms only; Settings has the
    // global one).
    RowLayout {
        Layout.fillWidth: true
        visible: editor.scopeKind === "room" && editor.available
        spacing: AppTheme.spacing8
        readonly property bool hidden: {
            if (!editor.available)
                return false
            var _dep = app.backdrops.revision
            return app.backdrops.roomHidden(editor.scopeId)
        }
        AppSwitch {
            objectName: "chatBackgroundHideSwitch"
            checked: parent.hidden
            onToggled: app.backdrops.setRoomHidden(editor.scopeId, !checked)
            Accessible.name: hideLabel.text
        }
        Label {
            id: hideLabel
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            color: AppTheme.textSecondary
            font.pixelSize: AppTheme.textMeta
            text: qsTr("Hide backgrounds others set for this room")
        }
    }

    Label {
        Layout.fillWidth: true
        visible: text.length > 0
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        color: editor.convertingSvg ? AppTheme.textSecondary : AppTheme.danger
        font.pixelSize: AppTheme.textMeta
        text: editor.convertingSvg
              ? qsTr("Converting the SVG to a picture…")
              : (editor.available ? editor.errorText(app.backdrops.lastError) : "")
    }

    // The converted SVG arrives here (see prepareImage).
    Connections {
        target: editor.available ? app.backdrops : null
        function onImagePrepared(result) {
            if (!editor.convertingSvg)
                return
            editor.convertingSvg = false
            if (result.ok)
                editor.dirty = true
        }
    }

    NativeFileDialog {
        id: pictureDialog
        purpose: "image"
        title: qsTr("Choose a background picture")
        nameFilters: [ qsTr("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp *.svg)") ]
        // The file is read, sniffed, re-encoded and previewed from memory by
        // the controller; QML never loads the chosen path itself.
        onAccepted: {
            var result = app.backdrops.prepareImage(selectedFile)
            // An SVG answers later, on imagePrepared.
            editor.convertingSvg = result.pending === true
            if (result.ok)
                editor.dirty = true
        }
    }
}
