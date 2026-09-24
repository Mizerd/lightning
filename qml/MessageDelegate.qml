import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    // Not clipped: rows size to their content exactly, and a clip would cut off
    // the hover action bar on a row shorter than it.
    clip: false
    // Settable: the room timeline instantiates rows in a Repeater/Column with
    // no attached view, so it injects itself. The thread panel is a ListView
    // and keeps the attached default.
    property var timelineView:
        TableView.view ? TableView.view : ListView.view

    // Multi-message selection: while selecting, a tap toggles the row instead
    // of its normal action (open an image, follow a link, react).
    /// The open room's display name, for "with context" forwarding.
    property string sourceRoomName: ""
    readonly property bool selectionMode: app.forward.selecting === true
    /// What a row's own surfaces are gated on while selecting. The selection
    /// TapHandler's grab permissions do not suspend child handlers (they receive
    /// the press first), so each surface checks this explicitly.
    readonly property bool rowActionsEnabled: !root.selectionMode
    // Only messages are selectable, not call cards or state rows.
    readonly property bool rowSelectable:
        model.isVirtual !== true && model.redacted !== true
        && model.isLocalEcho !== true
        && !root.isCallEvent && !root.isStateActivity
        && root.eventIdForActions() !== ""
        // A gallery forwards as its first attachment only.
        && !root.isGallery
    /// Width of the selection circle's column; the content shifts right by this
    /// so the circle never overlaps the avatar or text.
    readonly property real selectionGutterWidth: 26
    // isSelected() is a plain call Qt cannot observe; selectedCount changes on
    // every toggle and carries the dependency.
    readonly property bool rowSelected:
        root.selectionMode && root.rowSelectable
        && app.forward.selectedCount > 0
        && app.forward.isSelected(root.eventIdForActions())

    function toggleSelectionForThisRow() {
        if (!root.rowSelectable)
            return
        app.forward.toggleSelected(root.eventIdForActions(), {
            redacted: model.redacted === true,
            isLocalEcho: model.isLocalEcho === true,
            undecryptable: model.undecryptable === true,
            isVirtual: model.isVirtual === true,
            isImage: model.isImage === true,
            isVideo: model.isVideo === true,
            isAudio: model.isAudio === true,
            isSticker: model.isSticker === true,
            isFile: model.isFile === true,
            senderDisplayName: model.senderDisplayName || "",
            senderName: model.senderDisplayName || "",
            body: model.body || "",
            mediaKey: model.mediaKey || "",
            mediaFilename: model.mediaFilename || "",
            timestampMs: model.timestampMs || 0,
            // Read from the open room: a selection is always from one room.
            sourceRoomName: root.sourceRoomName || "",
        })
    }
    // Virtual rows (eventType 7 = DateDivider, 8 = ReadMarker, 9 =
    // TimelineStart) render as thin separators. Speculative media work is only
    // spent on rows the reader stopped on (see speculativeMediaAllowed in
    // TimelinePane.qml). A host without a pane is permissive.
    readonly property bool speculativeMediaAllowed:
        rowOnScreen
        && (!root.timelineView
            || root.timelineView.speculativeMediaAllowed !== false)
    // Whether this row is near enough to the viewport to fetch its picture now.
    // Every row is instantiated, so without this every image in the loaded
    // history would fetch on room open. The pane publishes the band (see
    // refreshMediaBand in TimelinePane.qml) and assigns this from its index
    // range. Hosts without a band (thread ListView, fixtures) stay permissive.
    property bool mediaInBand: true

    readonly property bool isVirtualRow: model.isVirtual === true
    readonly property bool isStateActivity: model.isStateActivity === true
    readonly property bool isRoutineActivity: model.isRoutineActivity === true
    // A call somebody started: its own row kind alongside message, virtual and
    // state. Every branch that enumerates row kinds must name it. `=== true` so
    // a model without the role keeps the old behaviour.
    readonly property bool isCallEvent: model.isCallEvent === true
    // A zero-height presentation filter: state events stay in the model, so
    // toggling the setting needs no resync. "Room activity" is a master switch
    // with membership and member_profile halves; other kinds follow the master.
    // Keep in step with TimelineModel::activityKindVisible.
    readonly property bool roomActivityVisible: {
        if (!isRoutineActivity) return true
        if (!app.settings.showRoomActivity) return false
        if (model.stateKind === "membership")
            return app.settings.showMembershipEvents
        if (model.stateKind === "member_profile")
            return app.settings.showProfileChangeEvents
        return true
    }
    // The model this delegate's stable-id actions resolve against: app.timeline
    // for the room, app.thread.model for the thread panel.
    readonly property var timelineModel:
        root.timelineView && root.timelineView.timelineModel
        ? root.timelineView.timelineModel : app.timeline
    // Whether this row's send can still be aborted. Asks the model: a backend
    // without a send queue has nothing to cancel. The typeof guard is for plain
    // ListModel fixtures. A function rather than a root property because it
    // needs `index`, and a root-level binding on `index` is a creation-time
    // context lookup.
    function canCancelSendAt(viewRow) {
        return root.timelineModel !== null
            && typeof root.timelineModel.canCancelSend === "function"
            && root.timelineModel.canCancelSend(root.sourceModelRow(viewRow))
    }
    function sourceModelRow(viewRow) {
        return root.timelineView && root.timelineView.sourceRowForViewRow
                ? root.timelineView.sourceRowForViewRow(viewRow) : viewRow
    }
    // The thread panel pins the root above the reply list, so the same row
    // inside the ListView collapses.
    readonly property bool suppressedAsThreadRoot:
        root.timelineView && (root.timelineView.suppressRootEventId || "") !== ""
        && (model.eventId || "") === root.timelineView.suppressRootEventId
    readonly property var stateActivityEntries: model.stateGroupEntries || []
    readonly property bool showsIdentity: model.showSenderIdentity === true

    // A date divider that introduces nothing visible is suppressed. The model
    // answers this. `=== false` so a model without the role keeps every
    // divider.
    readonly property bool dividerSuppressed:
        isVirtualRow && model.eventType === 7
        && model.dividerIntroducesVisibleContent === false

    // A redacted row that is not the first of its run renders nothing; the
    // leader shows "N messages deleted". `=== false` as above.
    readonly property bool deletedFollower:
        model.redacted === true && model.deletedGroupLeader === false

    // 0 Modern, 1 Bubbles, 2 Compact. The thread panel always uses Modern;
    // Bubbles applies only to direct-message timelines.
    readonly property int timelineLayout:
        inThreadPanel ? 0 : app.settings.messageLayout
    property bool isDirectRoom: root.timelineView
                                && root.timelineView.isDirectRoom === true
    readonly property bool bubbleMode: timelineLayout === 1 && isDirectRoom
    readonly property bool compactMode: timelineLayout === 2
    readonly property real bubblePad: bubbleMode ? 10 : 0

    // Group leaders get a larger gap than continuation lines. Compact stays
    // dense.
    readonly property real messageTopSpacing: showsIdentity
                                               ? (compactMode ? 2
                                                              : AppTheme.spacingM)
                                               : (compactMode ? 0 : 1)
    readonly property real avatarGutterWidth: compactMode ? 8
                                              : (bubbleMode ? 44 : 40)

    // On-screen check for skeleton shimmer and GIF playback. Settable: in the
    // room timeline this item sits inside a per-row Loader (its own y is 0), so
    // the Loader assigns it. The default binding serves the thread ListView,
    // where the delegate is the positioned item.
    property bool rowOnScreen:
        !!root.timelineView
        && (y + height) > root.timelineView.contentY
        && y < (root.timelineView.contentY + root.timelineView.height)

    // Once the bootstrap has given up on keys, stop shimmering undecryptable
    // rows and hold a static state. Keys arriving later still replace the row.
    readonly property bool decryptStalled:
        app.cryptoBootstrap
        && (app.cryptoBootstrap.phase
                === CryptoBootstrapModel.ManualRecoveryRequired
            || app.cryptoBootstrap.phase
                === CryptoBootstrapModel.NoBackupAvailable)
    visible: roomActivityVisible && !suppressedAsThreadRoot && !dividerSuppressed
             && !deletedFollower
    readonly property real naturalImplicitHeight:
        (!roomActivityVisible || suppressedAsThreadRoot || dividerSuppressed
         || deletedFollower) ? 0
        : isVirtualRow ? virtualRow.implicitHeight
        : isCallEvent ? callEventRow.implicitHeight + AppTheme.spacingS * 2
        : isStateActivity ? stateActivity.implicitHeight
        : layout.implicitHeight + messageTopSpacing
    property bool heightSeedActive: false
    property real heightSeed: 0
    property string heightSeedIdentity: ""
    property bool heightMeasurementReady: false
    implicitHeight: heightSeedActive ? heightSeed : naturalImplicitHeight

    // Key for the one-pinned-toolbar state: the SDK item id, else the event id.
    // Empty for virtual rows.
    readonly property string actionKey: (model.itemId && model.itemId.length > 0)
                                        ? model.itemId
                                        : (model.eventId || "")
    // Element-style hide image. Purely local: nothing is sent, edited or
    // redacted. The flag lives in MediaVisibilityStore because a row can be
    // destroyed and rebuilt. Images and stickers only.
    readonly property bool mediaHideable:
        (model.isImage === true || model.isSticker === true)
        && model.redacted !== true
        && root.mediaVisibilityKey.length > 0
    // `mediaKey` is the event id once the event is remote; eventId is the
    // fallback.
    readonly property string mediaVisibilityKey:
        (model.mediaKey && model.mediaKey.length > 0)
            ? model.mediaKey : (model.eventId || "")
    // A tracked property rather than a binding on the Q_INVOKABLE (isHidden()
    // has no per-key NOTIFY). Refreshed when the identity changes and when the
    // store announces a write.
    property bool mediaHidden: false
    function refreshMediaHidden() {
        if (typeof app === "undefined" || !app || !app.mediaVisibility) {
            mediaHidden = false
            return
        }
        mediaHidden = root.mediaVisibilityKey.length > 0
                      && app.mediaVisibility.isHidden(root.mediaVisibilityKey)
    }
    function setMediaHidden(hidden) {
        if (typeof app === "undefined" || !app || !app.mediaVisibility)
            return
        if (root.mediaVisibilityKey.length === 0)
            return
        app.mediaVisibility.setHidden(root.mediaVisibilityKey, hidden)
    }
    onMediaVisibilityKeyChanged: root.refreshMediaHidden()
    Connections {
        target: (typeof app !== "undefined" && app) ? app.mediaVisibility : null
        function onHiddenChanged(key, hidden) {
            // Keyed, so one reveal does not re-resolve every image row.
            if (key === root.mediaVisibilityKey)
                root.mediaHidden = hidden
        }
    }
    // Collapse embeds to one line. Off by default; with it off every Loader it
    // gates keeps its normal `active`/`sourceComponent`.
    //   * Covered: images, GIFs, stickers, video, audio/voice, file cards, and
    //     a loaded link preview.
    //   * Not covered: reply quotes (context, not media), thread summary cards,
    //     polls and places (the card is the message), link previews still
    //     asking/loading/failed (they carry the consent or retry control), and
    //     code blocks.
    // A collapsed row does not instantiate its media component, so it fetches
    // nothing.
    readonly property bool collapseEmbedsSetting:
        typeof app !== "undefined" && app && app.settings
        && app.settings.collapseEmbeds === true
    // Transient, not persisted: an expansion is "show me this one", not a
    // preference. Reset when the row identity changes (onActionKeyChanged).
    property bool embedExpanded: false
    function toggleEmbedExpanded() { root.embedExpanded = !root.embedExpanded }

    readonly property bool mediaEmbedCollapsible:
        root.collapseEmbedsSetting && root.mediaRowBody
        && model.redacted !== true
    readonly property bool mediaEmbedCollapsed:
        root.mediaEmbedCollapsible && !root.embedExpanded
    // `loaded` only; see the exclusions above.
    readonly property bool previewEmbedCollapsible:
        root.collapseEmbedsSetting
        && root.preview !== undefined && root.preview !== null
        && root.preview.state === "loaded"
        && !model.redacted
    readonly property bool previewEmbedCollapsed:
        root.previewEmbedCollapsible && !root.embedExpanded

    function embedDurationText(ms) {
        if (!ms || ms <= 0) return ""
        var total = Math.round(ms / 1000)
        var m = Math.floor(total / 60)
        var s = total % 60
        return m + ":" + (s < 10 ? "0" : "") + s
    }
    function embedSizeText(bytes) {
        if (!bytes || bytes <= 0) return ""
        if (bytes < 1024) return qsTr("%1 B").arg(bytes)
        if (bytes < 1024 * 1024)
            return qsTr("%1 KB").arg(Math.round(bytes / 1024))
        return qsTr("%1 MB").arg((bytes / (1024 * 1024)).toFixed(1))
    }
    // Functions rather than properties: they only run inside the summary
    // component (collapsed rows), so uncollapsed rows pay nothing. Property
    // reads inside a QML function are still captured by the calling binding.
    function mediaIsGif() {
        return (model.mediaMimetype || "").toLowerCase() === "image/gif"
    }
    function mediaEmbedIcon() {
        if (model.isSticker === true) return "mood"
        if (model.isVideo === true) return "videocam"
        if (model.isAudio === true)
            return model.mediaIsVoice === true ? "mic" : "graphic_eq"
        if (model.isImage === true)
            return root.mediaIsGif() ? "gif_box" : "image"
        if (model.isFile === true) return "attach_file"
        return "attach_file"
    }
    function mediaEmbedKind() {
        if (root.isGallery)
            return root.galleryCountLabel(root.galleryItems.length,
                                          root.galleryAllImages())
        if (model.isSticker === true) return qsTr("Sticker")
        if (model.isVideo === true) return qsTr("Video")
        if (model.isAudio === true)
            return model.mediaIsVoice === true ? qsTr("Voice message")
                                               : qsTr("Audio")
        if (model.isImage === true)
            return root.mediaIsGif() ? qsTr("GIF") : qsTr("Image")
        if (model.isFile === true) return qsTr("File")
        return qsTr("Attachment")
    }
    // Name, then size. Every part is optional; an empty detail yields "Image",
    // not "Image · ".
    function mediaEmbedDetail() {
        // A gallery's summary is its count (see mediaEmbedKind()).
        if (root.isGallery) return ""
        var parts = []
        var name = (model.mediaFilename || "").trim()
        // A voice message's filename is generated; only its length matters.
        if (name.length > 0 && model.mediaIsVoice !== true)
            parts.push(name)
        if (model.isVideo === true || model.isAudio === true) {
            var d = root.embedDurationText(model.mediaDurationMs || 0)
            if (d.length > 0) parts.push(d)
            // Audio whose duration is unknown still has a size.
            else if (model.isAudio === true) {
                var as = root.embedSizeText(model.mediaSize || 0)
                if (as.length > 0) parts.push(as)
            }
        } else if (model.isImage === true || model.isSticker === true) {
            if (model.mediaWidth > 0 && model.mediaHeight > 0) {
                parts.push(model.mediaWidth + "×" + model.mediaHeight)
            } else {
                // Stickers from packs Lightning uploaded have no dimensions
                // (upload_to_user_pack does not decode the image), and an image
                // may arrive before its info hydrates; fall back to size.
                var is = root.embedSizeText(model.mediaSize || 0)
                if (is.length > 0) parts.push(is)
            }
        }
        if (model.isFile === true) {
            var s = root.embedSizeText(model.mediaSize || 0)
            if (s.length > 0) parts.push(s)
        }
        return parts.join(" · ")
    }
    // `host` comes from LinkPreviewController::sanitizedHost; title and
    // siteName are resolved because this only exists in the `loaded` state.
    function previewEmbedDetail() {
        var p = root.preview || {}
        var parts = []
        var host = (p.host || "").trim()
        if (host.length > 0) parts.push(host)
        var title = (p.title || "").trim()
        if (title.length === 0) title = (p.siteName || "").trim()
        if (title.length > 0) parts.push(title)
        return parts.join(" · ")
    }
    // Find highlighting: the active query, or "". Reads the delegate's own
    // model, so a thread panel search does not light up the room timeline.
    readonly property string searchHighlight:
        root.timelineModel && root.timelineModel.searchActive === true
        ? (root.timelineModel.searchQuery || "") : ""
    // Whether this row is the current match.
    readonly property bool isCurrentSearchHit:
        root.searchHighlight !== ""
        && root.timelineModel
        && (model.eventId || "") !== ""
        && (model.eventId || "") === root.timelineModel.searchCurrentEventId

    // Wrap every occurrence of `needle` in a highlight span. The body is
    // RichText, so walk it: tags are copied untouched and only text runs are
    // substituted. Entities are atomic, so "amp" does not split "&amp;".
    function highlightSearchMatches(html, needle, current) {
        if (!needle || needle.length === 0 || !html || html.length === 0)
            return html
        // Cheap reject first: every row re-evaluates on each query change.
        if (html.toLowerCase().indexOf(needle.toLowerCase()) < 0)
            return html

        var open = current ? "<span style=\"background-color:"
                             + AppTheme.accent + "; color:"
                             + AppTheme.accentText + ";\">"
                           : "<span style=\"background-color:"
                             + AppTheme.accentSoft + ";\">"
        var close = "</span>"

        function highlightRun(run) {
            var lowerRun = run.toLowerCase()
            var lowerNeedle = needle.toLowerCase()
            // Entity spans are no-go zones for a match.
            var blocked = []
            var entity = /&(#[0-9]+|#x[0-9a-fA-F]+|[a-zA-Z]+);/g
            var found
            while ((found = entity.exec(run)) !== null)
                blocked.push([found.index, found.index + found[0].length])
            var out = ""
            var pos = 0
            while (pos <= lowerRun.length) {
                var hit = lowerRun.indexOf(lowerNeedle, pos)
                if (hit < 0)
                    break
                var end = hit + needle.length
                var clash = false
                for (var b = 0; b < blocked.length; ++b) {
                    if (hit < blocked[b][1] && end > blocked[b][0]) {
                        clash = true
                        break
                    }
                }
                if (clash) {
                    pos = hit + 1
                    continue
                }
                out += run.substring(pos, hit) + open
                       + run.substring(hit, end) + close
                pos = end
            }
            return out + run.substring(pos)
        }

        var result = ""
        var i = 0
        while (i < html.length) {
            var tagStart = html.indexOf("<", i)
            if (tagStart < 0) {
                result += highlightRun(html.substring(i))
                break
            }
            result += highlightRun(html.substring(i, tagStart))
            var tagEnd = html.indexOf(">", tagStart)
            if (tagEnd < 0) {
                // Malformed tail: copy it through.
                result += html.substring(tagStart)
                break
            }
            result += html.substring(tagStart, tagEnd + 1)
            i = tagEnd + 1
        }
        return result
    }

    function refreshHeightSeed() {
        heightSeedActive = false
        heightMeasurementReady = false
        heightSeedIdentity = actionKey
        if (!TableView.view || !root.timelineView)
            return
        if (actionKey !== "" && root.timelineView.cachedDelegateHeight) {
            var seed = root.timelineView.cachedDelegateHeight(actionKey)
            if (isFinite(seed) && seed >= 0) {
                heightSeed = seed
                heightSeedActive = true
            }
        }
        // Keep a recycled delegate at its last known height for one event turn
        // while nested Loaders rebind, then expose natural geometry.
        heightSeedReleaseTimer.restart()
    }
    onNaturalImplicitHeightChanged: {
        // Nested Loaders can finish after the reuse turn, and a
        // rowHeightProvider is not guaranteed to be queried again, so coalesce
        // a fresh measurement. Do not reactivate the seed. Readiness is dropped
        // because a Loader mid-swap can expose its previous item's size.
        if (TableView.view && root.timelineView) {
            heightMeasurementReady = false
            heightResolutionTimer.restart()
        }
    }
    Timer {
        id: heightSeedReleaseTimer
        interval: 0
        onTriggered: {
            if (root.heightSeedIdentity !== root.actionKey) {
                root.refreshHeightSeed()
                return
            }
            root.heightSeedActive = false
            heightResolutionTimer.restart()
        }
    }
    Timer {
        id: heightResolutionTimer
        // Commit only after natural geometry has been quiet for one frame.
        interval: 16
        onTriggered: {
            if (root.heightSeedIdentity !== root.actionKey) {
                root.refreshHeightSeed()
                return
            }
            // root.timelineView only: `TableView.view` here would attach to
            // this Timer, not the delegate, and would always be null.
            // root.timelineView covers the ListView case too.
            if (!root.timelineView)
                return
            root.heightMeasurementReady = true
            if (root.actionKey !== ""
                    && root.timelineView.rememberDelegateHeight) {
                root.timelineView.rememberDelegateHeight(
                            root.actionKey,
                            Math.max(0, Math.round(
                                root.naturalImplicitHeight)))
            }
            if (root.timelineView.scheduleHeightLayout)
                root.timelineView.scheduleHeightLayout()
        }
    }
    readonly property string previewRoomId: app.currentRoomId || ""
    readonly property string previewOwnerKey:
        previewRoomId.length > 0 && actionKey.length > 0
        ? previewRoomId + "\u001f" + actionKey : ""
    readonly property bool actionsPinned: root.timelineView
            && root.timelineView.pinnedActionsKey !== ""
            && root.timelineView.pinnedActionsKey === actionKey
    // One bar at a time: a hovered row beats a pinned one. A row whose More
    // menu is open keeps its bar, since the menu is positioned from it. While a
    // transient surface owns row interaction (picker, tone popup,
    // profile/reader popovers, image viewer), no row shows its bar; not solved
    // with z, since a covered bar still takes hover. An undefined owner blocks
    // nothing.
    readonly property bool transientOwnerBlocks: {
        if (!root.timelineView)
            return false
        var owner = root.timelineView.transientInteractionOwner
        if (owner === undefined || owner === null || owner === "")
            return false
        return !(owner === "menu" && root.moreMenuOpen)
    }
    readonly property bool actionsVisible:
        !root.transientOwnerBlocks
        && (root.moreMenuOpen
            || (root.timelineView
                && (root.timelineView.hoveredActionsKey === actionKey
                    || (actionsPinned
                        && root.timelineView.hoveredActionsKey === ""))))
    property string menuEventId: ""

    // Clears the view's hovered key if this row owns it. A row destroyed under
    // the pointer (room change) gets no leave event, and no bar would show
    // until the pointer entered another row. Only destruction can do this: in
    // onActionKeyChanged the old key is already gone.
    function releaseHoveredActions() {
        if (root.timelineView
                && root.timelineView.hoveredActionsKey === root.actionKey)
            root.timelineView.hoveredActionsKey = ""
    }
    Component.onDestruction: releaseHoveredActions()

    // Pooled-delegate reuse: model-bound state re-derives through change
    // handlers, but transient state (popup targets, the details payload) is
    // scrubbed so a stale event id cannot carry across rows. objectName is read
    // by the reuse-safety test.
    objectName: "messageDelegateRoot"
    function resetForReuse() {
        menuEventId = ""
        if (detailsDialogItem) {
            detailsDialogItem.details = ({})
            if (detailsDialogItem.visible)
                detailsDialogItem.close()
        }
        if (moreMenuItem && moreMenuItem.visible)
            moreMenuItem.close()
        refreshPreview()
    }
    ListView.onReused: resetForReuse()
    TableView.onReused: {
        resetForReuse()
        refreshHeightSeed()
    }

    // `alreadyInOverlaySpace`: the caller passes a point already in
    // Overlay.overlay coordinates, so it is not mapped twice.
    function openContextMenu(x, y, alreadyInOverlaySpace) {
        var eventId = root.eventIdForActions()
        if (eventId === "" || root.isVirtualRow || root.isStateActivity
            || root.isCallEvent)
            return
        // Dismiss transient row surfaces first: the picker and this menu share
        // one overlay, so the later one covers the other and z cannot fix it.
        // Reached through `timelineView`, since a delegate cannot see pane-root
        // functions.
        if (root.timelineView && root.timelineView.closeTransientRowSurfaces)
            root.timelineView.closeTransientRowSurfaces()
        menuEventId = eventId
        var p = alreadyInOverlaySpace
                ? Qt.point(x, y)
                : root.mapToItem(Overlay.overlay,
                                 x === undefined ? root.width : x,
                                 y === undefined ? 0 : y)
        root.ensureContextMenu().popup(Overlay.overlay, p.x, p.y)
    }
    // Opens this row's sender profile. Shared by the avatar, sender name and
    // the context menu's "View profile".
    function openSenderProfileForRow() {
        if (!root.timelineView || !root.timelineView.openSenderProfile)
            return
        var userId = model.sender || ""
        if (userId.length === 0)
            return
        root.timelineView.openSenderProfile({
            userId: userId,
            displayName: model.senderDisplayName || "",
            avatarUrl: model.senderAvatarMxc || ""
        })
    }
    // Mentions carry an internal "mention:<user-id>" link (from the sanitizer)
    // that opens the member profile; everything else is a validated http(s) or
    // Matrix URL. On root so every body renderer routes identically.
    function openMessageLink(link) {
        // Internal spoiler toggle; the scheme only exists in sanitize()'s
        // output and never reaches the browser.
        if (link === "spoiler:toggle") {
            // Through timelineModel: thread rows resolve against
            // app.thread.model.
            if (root.timelineModel && root.timelineModel.toggleSpoilers)
                root.timelineModel.toggleSpoilers(model.eventId)
            return
        }
        if (link.indexOf("mention:") === 0) {
            if (root.timelineView
                && root.timelineView.openSenderProfile) {
                root.timelineView.openSenderProfile({
                    userId: link.substring(8),
                    displayName: "",
                    avatarUrl: ""
                })
            }
            return
        }
        // Room-oriented Matrix links open in-app through Discover (resolved via
        // the SDK). User links keep the web behaviour.
        var isMatrixLink =
            link.indexOf("matrix:") === 0
            || link.indexOf("matrix.to/#/") !== -1
        // Percent-encoded user permalinks (matrix.to/#/%40user…) are user links
        // too.
        var lower = link.toLowerCase()
        var isUserLink =
            link.indexOf("#/@") !== -1
            || lower.indexOf("#/%40") !== -1
            || link.indexOf("matrix:u/") === 0
        if (isMatrixLink && !isUserLink) {
            app.openMatrixLink(link)
            return
        }
        app.media.openWebUrl(link)
    }
    function copyToClipboard(value) {
        if (!value || value.length === 0) return
        clipboardHelper.text = value
        clipboardHelper.selectAll()
        clipboardHelper.copy()
        clipboardHelper.text = ""
    }
    // In the thread panel a reply targets the thread composer (an in-thread
    // rich reply via the SDK path); in the room timeline, the main composer.
    readonly property bool inThreadPanel:
        root.timelineView && root.timelineView.threadContext === true
    function beginReply(eventId) {
        var details = root.timelineModel.messageDetails(eventId)
        if (!details.eventId) return
        if (root.inThreadPanel) {
            app.thread.beginReply(eventId)
            return
        }
        var previewText = root.timelineModel.visibleTextForEvent(eventId)
        app.composer.beginReply(eventId, details.senderName || details.senderId,
                                (previewText || "").substring(0, 80),
                                root.timelineModel.mediaKeyForEvent(eventId))
    }
    activeFocusOnTab: !isVirtualRow && !isStateActivity && !isCallEvent
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Menu
            || (event.key === Qt.Key_F10
                && (event.modifiers & Qt.ShiftModifier))) {
            root.openContextMenu(root.width / 2, root.height / 2)
            event.accepted = true
        }
    }
    function toggleActionsPin() {
        if (!root.timelineView || actionKey === "") return
        root.timelineView.pinnedActionsKey =
            actionsPinned ? "" : actionKey
    }

    // Recoverable decryption failures shimmer (keys can still arrive and
    // replace the row); deterministic failures (sent before join, verification
    // required, withheld) keep their static explanation.
    readonly property bool showsDecryptingSkeleton: {
        if (model.undecryptable !== true || model.redacted)
            return false
        var kind = model.errorKind || ""
        return kind !== "membership" && kind !== "device_trust"
               && kind !== "withheld"
    }

    // Fenced code blocks: the model returns ordered segments only for a body
    // with a <pre> block; ordinary messages get an empty list and keep the
    // single TextEdit path, so the hot path gains no items. `|| []` covers
    // models without the role.
    readonly property var messageSegments: model.messageSegments || []
    // An MSC4274 gallery's attachments (two or more), or empty.
    readonly property var galleryItems: model.galleryItems || []
    readonly property bool isGallery: galleryItems.length > 1
    // "3 images" / "3 attachments", for the gallery summary and a reply quoting
    // one.
    function galleryCountLabel(count, allImages) {
        return allImages ? qsTr("%n image(s)", "", count)
                         : qsTr("%n attachment(s)", "", count)
    }
    function galleryAllImages() {
        for (var i = 0; i < galleryItems.length; ++i) {
            if (galleryItems[i].kind !== "image") return false
        }
        return true
    }
    // What the reply quote says when the target has no text: an image with an
    // empty body, or a gallery with no caption. Empty when the kind is unknown,
    // so the "(original message not loaded)" fallback keeps its meaning.
    function replyKindLabel() {
        var count = model.replyToCount || 0
        var kind = model.replyToKind || ""
        if (count > 1) return root.galleryCountLabel(count, kind === "image")
        switch (kind) {
        case "image":   return qsTr("Image")
        case "gif":     return qsTr("GIF")
        case "video":   return qsTr("Video")
        case "audio":   return qsTr("Audio")
        case "file":    return qsTr("File")
        case "sticker": return qsTr("Sticker")
        default:        return ""
        }
    }
    readonly property bool mediaRowBody:
        model.isImage
        || model.isSticker === true
        || model.isVideo === true
        || model.isAudio === true
        || model.isFile === true
    // -1 means uploading with unknown extent; 0..1 is a reported fraction.
    // Normalised here so a model without the role reads as unknown.
    readonly property real uploadProgress:
        model.uploadProgress === undefined ? -1 : model.uploadProgress
    readonly property bool mediaCaptionBody: {
        if (!mediaRowBody) return false
        var body = (model.body || "").trim()
        var name = (model.mediaFilename || "").trim()
        return body.length > 0 && name.length > 0
               && body.toLowerCase() !== name.toLowerCase()
    }
    readonly property bool hasMessageSegments:
        messageSegments.length > 0
        && !isVirtualRow && !isStateActivity
        && !model.redacted && model.isPoll !== true
        && !showsDecryptingSkeleton
        && (!mediaRowBody || mediaCaptionBody)

    // Navigation: the delegate is shared by the room timeline and thread panel,
    // so it only knows that its host offers a jump. A thread reply's targets
    // are always in-thread, so there is no room hand-off case. Guarded so a
    // fixture host does nothing instead of throwing.
    readonly property string navigationHighlightId:
        root.timelineView
        && root.timelineView.navigationHighlightEventId !== undefined
        ? root.timelineView.navigationHighlightEventId : ""
    function navigateToReplyTarget() {
        var target = model.replyToEventId || ""
        if (target.length === 0)
            return
        if (root.timelineView && root.timelineView.navigateToEvent)
            root.timelineView.navigateToEvent(target)
    }

    // The quoted sender's colour on the quote's rule and name, using the same
    // hash as the message header. Keyed by the raw MXID, not the display name
    // (ReplyToSenderRole), so a person's quote and messages share one hue.
    // Empty falls back to the primary ink.
    readonly property string replySenderKey: model.replyToSenderId || ""
    // Inside an own bubble the quote sits on accent, so the bubble's ink family
    // applies instead of the identity inks.
    readonly property bool replyOnOwnBubble:
        bubbleMode && model.isOwn === true
    readonly property color replySenderInk:
        replyOnOwnBubble ? AppTheme.ownBubbleText
                         : AppTheme.userColor(replySenderKey)
    readonly property color replyBodyInk:
        replyOnOwnBubble ? AppTheme.onAccentMuted : AppTheme.textSecondary

    // Read-receipt rail clearance. The facepile paints upward from the row's
    // bottom at the row's right margin; on a narrow pane it would land on the
    // body's last line. Reserve exactly the overlap, only when there is one.
    // Modern/Compact only: in Bubbles the bubble's width is its content's
    // width, so this would close a loop.
    readonly property real receiptRailReserve:
        (!root.bubbleMode && readReceiptStrip.visible)
        ? Math.max(0, (bubble.x + bubble.width)
                      - Math.max(root.avatarGutterWidth,
                                 readReceiptStrip.width - receiptRow.width)
                      + AppTheme.spacingXS)
        : 0

    // An own bubble is right-aligned to the same edge the facepile rides, so
    // inset it (and narrow a wide incoming one) by the pile's width when there
    // is a pile. receiptRow.width depends only on the chip count, so no loop.
    readonly property real bubbleReceiptInset:
        (root.bubbleMode && readReceiptStrip.visible && receiptRow.width > 0)
        ? receiptRow.width + AppTheme.spacingXS
        : 0

    // The width a child inside the content column may use. bubbleContent insets
    // children by bubblePad (0 in Modern/Compact, 10 in Bubbles), so caps
    // written against bubble.width are too generous in Bubbles. Derived from
    // bubbleRow, not bubble: in Bubbles the bubble is sized from its content,
    // so a child clamped against bubble.width would feed its own input. In
    // Modern/Compact this equals bubble.width.
    readonly property real contentInnerCap: {
        var avail = Math.max(1, bubbleRow.width - root.avatarGutterWidth)
        avail = root.bubbleMode
                ? Math.max(60, avail - 40 - root.bubbleReceiptInset)
                : Math.min(AppTheme.timelineContentMaxWidth, avail)
        return Math.max(1, avail - root.bubblePad * 2)
    }

    // The hover action bar and the read-receipt facepile are anchored to the
    // same rail from opposite ends of the row. On a short row they overlap and
    // the facepile (same z, later in the document) makes the buttons
    // unclickable. Reserve the pile's width only while the two bands meet. The
    // bar's bottom is 29px below bubbleRow's top (28px buttons + 2*2 padding -
    // 3px overhang), +4 so they do not touch. Feeds only a Loader's
    // rightMargin, so it cannot loop through the bubble's width and applies in
    // Bubbles too.
    readonly property real actionBarReceiptReserve:
        (readReceiptStrip.visible && receiptRow.width > 0
         && (layout.height - bubbleRow.y - receiptRow.height) < 33)
        ? receiptRow.width + AppTheme.spacingXS
        : 0

    // Today / Yesterday / weekday within a week / date, without the year while
    // it is the current one. `now` is not reactive: a session open across
    // midnight keeps "Today" until the row is rebuilt.
    function dayLabel(ts) {
        if (!ts || typeof ts.getFullYear !== "function")
            return ""
        var stamp = new Date(ts.getFullYear(), ts.getMonth(), ts.getDate())
        if (isNaN(stamp.getTime()))
            return ""
        var now = new Date()
        var today = new Date(now.getFullYear(), now.getMonth(), now.getDate())
        var days = Math.round((today.getTime() - stamp.getTime()) / 86400000)
        if (days === 0) return qsTr("Today")
        if (days === 1) return qsTr("Yesterday")
        if (days > 1 && days < 7)
            return Qt.locale().toString(ts, "dddd")
        return Qt.locale().toString(
            ts, ts.getFullYear() === now.getFullYear() ? "d MMMM"
                                                       : "d MMMM yyyy")
    }

    // Link-preview state from LinkPreviewController. previewFor() may dispatch
    // an automatic request (unencrypted rooms with auto-load on); encrypted
    // rooms stay in "requires_action" until the explicit Load.
    property var preview: ({ state: "none" })
    // The reader dismissed this row's card. State is "none" (the Loader
    // collapses); only the context menu's undo needs this.
    readonly property bool previewDismissed:
        root.preview ? root.preview.dismissed === true : false
    readonly property bool roomEncrypted:
        root.timelineView ? root.timelineView.roomEncrypted === true : false
    function refreshPreview() {
        if (isVirtualRow || isStateActivity || isCallEvent || model.redacted
            || model.isImage || model.isFile
            || actionKey === "" || !app.linkPreviews.supported) {
            preview = ({ state: "none" })
            return
        }
        preview = app.linkPreviews.previewForEvent(previewRoomId, actionKey,
                                                   model.body || "",
                                                   roomEncrypted)
    }
    Component.onCompleted: {
        refreshHeightSeed()
        refreshPreview()
        refreshMediaHidden()
    }
    onActionKeyChanged: {
        refreshHeightSeed()
        refreshPreview()
        // A different event: reset its expansion. The thread ListView still
        // recycles delegates, and fixtures can rebind a row in place.
        embedExpanded = false
    }
    onPreviewRoomIdChanged: {
        preview = ({ state: "none" })
        refreshPreview()
    }
    Connections {
        target: app.linkPreviews
        function onPreviewChanged(itemKey) {
            if (itemKey === root.previewOwnerKey) root.refreshPreview()
        }
        function onPolicyChanged() { root.refreshPreview() }
    }

    Item {
        id: virtualRow
        visible: root.isVirtualRow
        width: parent.width
        // Same rule-label-rule idiom and 30px height as the unread divider,
        // with the rules in `border` so the day break stays quieter.
        implicitHeight: unreadDivider.visible ? 30
                        : virtualLabel.active
                        ? Math.max(30, virtualLabel.implicitHeight
                                       + AppTheme.spacingS * 2)
                        : 0
        // Computed on the row because it is also the Loader's `active` guard:
        // an empty text creates no Label at all (see below).
        readonly property string dividerText:
            model.eventType === 7 ? root.dayLabel(model.timestamp)
            : model.eventType === 9 ? qsTr("Beginning of conversation")
            : ""
        // A Loader, never an always-created Label. Every QQuickText is born
        // with ItemObservesViewport, and only setText() clears it, which
        // returns early when the text is unchanged. One Label per row created
        // with "" makes Qt walk the whole timeline tree on every contentY
        // change. Rule: in a per-row delegate, a Label whose text can be "" at
        // creation belongs in a Loader. This includes labels reading message
        // fields, which are empty on virtual rows.
        Loader {
            id: virtualLabel
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            // Start the rule at the content indent, not in the avatar gutter.
            anchors.leftMargin: root.avatarGutterWidth
            anchors.rightMargin: AppTheme.spacingS
            // An orphan date divider creates no label, which also zeroes the
            // row's implicitHeight.
            active: root.isVirtualRow && model.eventType !== 8
                    && !root.dividerSuppressed
                    && virtualRow.dividerText.length > 0
            sourceComponent: RowLayout {
                objectName: "timelineDayDivider"
                spacing: AppTheme.spacingM
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: AppTheme.border
                }
                Label {
                    objectName: "timelineDayDividerLabel"
                    text: virtualRow.dividerText
                    color: AppTheme.textMuted
                    // Message-stream text: follows the text-size setting.
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    font.weight: AppTheme.weightStrong
                    Accessible.name: text
                }
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: AppTheme.border
                }
            }
        }
        RowLayout {
            id: unreadDivider
            objectName: "unreadDivider"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            // Same inset and gap as the day divider.
            anchors.leftMargin: root.avatarGutterWidth
            anchors.rightMargin: AppTheme.spacingS
            spacing: AppTheme.spacingM
            // Receipt tracking revives the SDK's ReadMarker row, and while
            // pinned to the bottom the own-receipt ack cycle would insert and
            // remove it above every incoming message. Render only when the
            // reader is not following the bottom. Hosts without a timeline view
            // keep it visible.
            readonly property bool suppressedWhilePinned:
                root.timelineView
                && root.timelineView.stickToBottom === true
            visible: root.isVirtualRow && model.eventType === 8
                     && !suppressedWhilePinned
            // unreadBadge, the same "unread" semantic as the numeric badges;
            // falls back to accent on legacy themes.
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                color: AppTheme.unreadBadge
            }
            Label {
                objectName: "unreadDividerLabel"
                text: qsTr("New messages")
                color: AppTheme.unreadBadge
                // Scaled like the day divider.
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                font.weight: AppTheme.weightStrong
                Accessible.name: text
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                color: AppTheme.unreadBadge
            }
        }
    }

    // Call row, behind a Loader because most rows are not calls and the
    // timeline is not virtualized. Active on the row kind alone, so it is built
    // once.
    Loader {
        id: callEventRow
        objectName: "callEventRow"
        active: root.isCallEvent
        visible: active
        // Its own spacing: messageTopSpacing is 1px for a continuation, and a
        // card 1px under a message reads as part of it.
        y: AppTheme.spacingS
        width: parent.width
        sourceComponent: CallEventDelegate {
            // The call's room. Call rows only appear in the room timeline,
            // matching the pane's call banner.
            roomId: root.previewRoomId
            actorUserId: model.sender || ""
            actorName: model.senderDisplayName || ""
            actorAvatarMxc: model.senderAvatarMxc || ""
            sentence: model.callEventText || ""
            // Only the newest call row carries Join (see
            // CallEventDelegate.isLatestCallRow); the model tracks it.
            isLatestCallRow: !root.timelineModel
                             || !root.timelineModel.latestCallEventId
                             || root.timelineModel.latestCallEventId === (model.eventId || "")
            video: model.callIsVideo === true
            declinedCount: model.callDeclinedCount || 0
            timestamp: model.timestamp
            onScreen: root.rowOnScreen
        }
    }

    // Compact room-activity summary, collapsed by default; the whole row is the
    // expand/collapse control.
    RoomActivityDelegate {
        id: stateActivity
        objectName: "stateActivityGroup"
        // An empty entry list draws nothing ("0 room updates" is worse than no
        // row). The count comes from the delegate, which drops call entries
        // itself.
        visible: root.isStateActivity && model.stateGroupLeader === true
                 && stateActivity.entryCount > 0
        width: parent.width
        groupId: model.stateGroupId || ""
        entries: root.stateActivityEntries
        expanded: root.timelineView
                  ? root.timelineView.stateGroupExpanded(groupId)
                  : false
        onToggleRequested: {
            if (root.timelineView)
                root.timelineView.toggleStateGroup(groupId)
        }
    }

    // Selection affordance, painted under the row's content and above the hover
    // highlight. The tap handler takes the whole row while selecting.
    Rectangle {
        anchors.fill: parent
        visible: root.rowSelected
        color: AppTheme.selected
        opacity: 0.55
        z: -1
    }
    Rectangle {
        objectName: "messageSelectionCheck"
        visible: root.selectionMode && root.rowSelectable
        anchors.left: parent.left
        anchors.leftMargin: 2
        anchors.verticalCenter: parent.verticalCenter
        width: 18; height: 18; radius: 9
        // Solid when empty too, so the circle reads over an avatar.
        color: root.rowSelected ? AppTheme.accent : AppTheme.background
        border.width: 1
        border.color: root.rowSelected ? AppTheme.accent : AppTheme.borderStrong
        z: 20
        Icon {
            anchors.centerIn: parent
            visible: root.rowSelected
            name: "check"
            size: 12
            color: AppTheme.accentText
        }
    }
    TapHandler {
        enabled: root.selectionMode && root.rowSelectable
        grabPermissions: PointerHandler.CanTakeOverFromAnything
        onTapped: root.toggleSelectionForThisRow()
    }

    Rectangle {
        id: rowHighlight
        // The view says what is highlighted; reading app.pagination here would
        // light up a matching id inside an open thread panel.
        readonly property bool navigationLanded:
            root.navigationHighlightId === (model.eventId || "")
        readonly property bool wanted:
            !root.isVirtualRow && !root.isStateActivity && !root.isCallEvent
            && (rowHover.hovered || root.actionsPinned || navigationLanded)
        // Opacity rather than `visible`, so the jump highlight fades like the
        // thread panel's landing instead of snapping on and off.
        visible: opacity > 0
        opacity: wanted ? 1 : 0
        Behavior on opacity {
            NumberAnimation { duration: 130; easing.type: Easing.OutCubic }
        }
        Behavior on color { ColorAnimation { duration: 130 } }
        x: -AppTheme.spacingXS
        y: layout.y
        width: root.width + AppTheme.spacingXS * 2
        height: layout.height
        color: navigationLanded ? AppTheme.selected : AppTheme.hover
        // Soft theme tint at an 8px radius, no border or elevation.
        radius: AppTheme.radiusMd
        z: 0
    }

    ColumnLayout {
        id: layout
        visible: !root.isVirtualRow && !root.isStateActivity && !root.isCallEvent
        y: root.messageTopSpacing
        // Indented while selecting so the circle has its own column.
        x: root.selectionMode && root.rowSelectable ? root.selectionGutterWidth : 0
        width: parent.width - x
        spacing: 2
        z: 1

        // One left-aligned sender timeline. Identity is shown once per sender
        // group; continuation rows keep the same indent without the avatar.
        Item {
            id: bubbleRow
            objectName: "messagePresentationRow"
            Layout.fillWidth: true
            implicitHeight: Math.max(avatarSlot.implicitHeight,
                                     bubble.implicitHeight)

            HoverHandler {
                id: rowHover
                onHoveredChanged: {
                    if (!root.timelineView || root.actionKey === "")
                        return
                    if (hovered) {
                        // A stray hover under an open picker must not re-claim
                        // the bar.
                        if (root.transientOwnerBlocks)
                            return
                        root.timelineView.hoveredActionsKey = root.actionKey
                    } else {
                        root.releaseHoveredActions()
                    }
                }
            }
            TapHandler {
                // Not gated on rowActionsEnabled: the context menu is also how
                // selection mode is left.
                acceptedButtons: Qt.RightButton
                onTapped: (eventPoint, button) => {
                    var p = bubbleRow.mapToItem(root, eventPoint.position.x,
                                               eventPoint.position.y)
                    root.openContextMenu(p.x, p.y)
                }
            }

            Item {
                id: avatarSlot
                objectName: "senderAvatarSlot"
                x: 0
                width: root.avatarGutterWidth
                height: parent.height
                implicitHeight: root.showsIdentity ? 34 : bodyLabel.implicitHeight

                Avatar {
                    objectName: "senderAvatar"
                    anchors.top: parent.top
                    visible: root.showsIdentity && !root.compactMode
                             && !(root.bubbleMode && model.isOwn === true)
                    onScreen: root.rowOnScreen
                    size: 32
                    mxc: model.senderAvatarMxc || ""
                    name: model.senderDisplayName || model.senderInitials
                    // Keyed by user id so resolving the display name later does
                    // not recolour.
                    colorKey: model.sender || ""
                    Accessible.name: qsTr("Avatar for %1").arg(
                                         model.senderDisplayName || model.sender)

                    // Clicking a person opens their profile. LeftButton only,
                    // so right-click still reaches the row's context menu.
                    TapHandler {
                        enabled: root.rowActionsEnabled
                        acceptedButtons: Qt.LeftButton
                        onTapped: root.openSenderProfileForRow()
                    }
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                }

                // Continuations show the timestamp in the gutter on hover only.
                // A Loader active while hovered: an always-present Label would
                // be a viewport observer on virtual rows (empty text) and a
                // text layout on every continuation row.
                Loader {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.rightMargin: 5
                    // While selecting the gutter belongs to the selection
                    // circle.
                    active: !root.showsIdentity && rowHover.hovered
                            && !root.compactMode && !root.selectionMode
                    sourceComponent: Label {
                        objectName: "continuationTimestamp"
                        // The app-wide clock format (Settings -> Appearance),
                        // read as a property so the binding updates when it
                        // changes.
                        text: Qt.formatDateTime(model.timestamp,
                                                app.settings.clockTimeFormat)
                        horizontalAlignment: Text.AlignRight
                        color: AppTheme.textMuted
                        // Scaled like the identity-line timestamp.
                        font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                        Accessible.name: qsTr("Sent at %1").arg(text)
                    }
                }
            }

            // Transparent content column: ordinary messages are rows, not
            // bubbles.
            Rectangle {
                id: bubble
                objectName: "messageContentColumn"
                // Bubbles (DM only): content-sized, own messages right-aligned
                // in the accent bubble, incoming in the chip bubble.
                // Modern/Compact use the full row.
                x: root.bubbleMode && model.isOwn === true
                   ? Math.max(root.avatarGutterWidth,
                              parent.width - width - root.bubbleReceiptInset)
                   : root.avatarGutterWidth
                width: root.bubbleMode
                       ? Math.min(bubbleContent.implicitWidth + root.bubblePad * 2,
                                  Math.max(60, parent.width
                                               - root.avatarGutterWidth - 40
                                               - root.bubbleReceiptInset))
                         // Modern/Compact: full width up to a readable max.
                       : Math.min(AppTheme.timelineContentMaxWidth,
                                  Math.max(1, parent.width - root.avatarGutterWidth))
                height: implicitHeight
                implicitHeight: bubbleContent.implicitHeight + root.bubblePad * 2
                // No mention wash: mentionHighlight is the danger-family badge
                // colour, so a tinted row reads as an error. The edge bar below
                // is the signal. If a wash is ever wanted, add a mentionRowWash
                // token outside the danger family.
                color: root.bubbleMode
                       ? (model.isOwn === true ? AppTheme.ownBubble
                                               : AppTheme.otherBubble)
                       : "transparent"
                radius: root.bubbleMode ? 16 : 0
                topLeftRadius: root.bubbleMode
                               ? (model.isOwn === true ? 16 : 4) : radius
                topRightRadius: root.bubbleMode
                                ? (model.isOwn === true ? 4 : 16) : radius
                opacity: model.redacted ? 0.65 : 1.0

                // Mention edge bar, flush at the bubble's left edge;
                // bubbleContent gets a matching inset so it never overlaps
                // text. Rounded at both ends.
                readonly property bool mentionBarVisible:
                    !root.bubbleMode
                    && (model.mentionsMe === true || model.mentionsRoom === true)
                Rectangle {
                    visible: bubble.mentionBarVisible
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.topMargin: 1
                    anchors.bottomMargin: 1
                    width: 3
                    radius: 1
                    color: model.mentionsMe === true
                           ? AppTheme.bolt : AppTheme.borderStrong
                }

                // Clicking the message content pins the action toolbar (click
                // again or Escape to close). Media and link taps have their own
                // handlers on top.
                TapHandler {
                    enabled: root.rowActionsEnabled
                    acceptedButtons: Qt.LeftButton
                    onTapped: (eventPoint) => {
                        // TapHandlers are non-exclusive across subtrees, so
                        // each overlaid control needs a band exclusion here.
                        // The "edited" marker opens the edit history.
                        if (model.edited === true && metaLabel.visible) {
                            var mp = bubble.mapToItem(
                                        metaLabel,
                                        eventPoint.position.x,
                                        eventPoint.position.y)
                            if (mp.x >= 0 && mp.x <= metaLabel.width
                                && mp.y >= 0 && mp.y <= metaLabel.height)
                                return
                        }
                        // The receipt facepile can overlap this bubble's bottom
                        // edge.
                        if (receiptRow.visible) {
                            var rp = bubble.mapToItem(
                                        receiptRow,
                                        eventPoint.position.x,
                                        eventPoint.position.y)
                            if (rp.x >= 0 && rp.x <= receiptRow.width
                                && rp.y >= 0 && rp.y <= receiptRow.height)
                                return
                        }
                        // The reply preview's own TapHandler (navigates to the
                        // replied message).
                        if (replyBox.visible) {
                            var qp = bubble.mapToItem(
                                        replyBox,
                                        eventPoint.position.x,
                                        eventPoint.position.y)
                            if (qp.x >= 0 && qp.x <= replyBox.width
                                && qp.y >= 0 && qp.y <= replyBox.height)
                                return
                        }
                        // The sender name opens the profile.
                        if (identityLoader.visible) {
                            var ip = bubble.mapToItem(
                                        identityLoader,
                                        eventPoint.position.x,
                                        eventPoint.position.y)
                            if (ip.x >= 0 && ip.x <= identityLoader.width
                                && ip.y >= 0 && ip.y <= identityLoader.height)
                                return
                        }
                        // The action bar's padding and the gaps between its
                        // buttons do not accept the press, so a near miss would
                        // toggle the pin and close the bar.
                        if (messageActionBarLoader.visible) {
                            var ap = bubble.mapToItem(
                                        messageActionBarLoader,
                                        eventPoint.position.x,
                                        eventPoint.position.y)
                            if (ap.x >= 0 && ap.x <= messageActionBarLoader.width
                                && ap.y >= 0
                                && ap.y <= messageActionBarLoader.height)
                                return
                        }
                        root.toggleActionsPin()
                    }
                }

                ColumnLayout {
                    id: bubbleContent
                    anchors.fill: parent
                    anchors.margins: root.bubblePad
                    anchors.leftMargin: root.bubblePad
                                        + (bubble.mentionBarVisible ? 8 : 0)
                    spacing: 2

                    // A Loader: the header's Labels read message fields, which
                    // are empty on virtual rows (see the viewport-observer note
                    // above), and continuation rows need no header.
                    Loader {
                        id: identityLoader
                        active: root.showsIdentity
                                && !(root.bubbleMode && model.isOwn === true)
                        visible: active
                        // Hug the content so the timestamp sits 8px beside the
                        // name. On the Loader because Layout attached
                        // properties only bind on direct children of the
                        // ColumnLayout.
                        Layout.fillWidth: false
                        // Not bubble.width in Bubbles: the bubble is sized from
                        // this column's implicitWidth, so clamping against it
                        // feeds the child's own input and collapsed the header
                        // to one pixel. Derived from bubbleRow (inert:
                        // fillWidth, no implicit width), mirroring the bubble's
                        // whole cap including bubbleReceiptInset.
                        Layout.maximumWidth: root.bubbleMode
                            ? root.contentInnerCap
                            : Math.max(1, bubble.width - 112)
                        sourceComponent: RowLayout {
                        id: identityHeader
                        objectName: "senderIdentityHeader"
                        spacing: 6
                        Label {
                            id: nameLabel
                            objectName: "senderName"
                            text: model.senderDisplayName || model.sender
                            textFormat: Text.PlainText
                            // Deterministic per-user ink hashed from the MXID,
                            // matching the avatar.
                            color: AppTheme.userColor(model.sender || "")
                            font.pixelSize: AppTheme.scaled(
                                root.compactMode || root.inThreadPanel
                                ? 13 : AppTheme.fontSizeM)
                            font.weight: Font.DemiBold
                            elide: Label.ElideRight
                            Layout.maximumWidth: 320
                            Accessible.name: qsTr("Sender: %1").arg(text)
                            // Full MXID on hover.
                            ToolTip.text: model.sender
                            ToolTip.visible: nameHover.hovered
                            ToolTip.delay: 400
                            HoverHandler {
                                id: nameHover
                                cursorShape: Qt.PointingHandCursor
                            }
                            TapHandler {
                                enabled: root.rowActionsEnabled
                                acceptedButtons: Qt.LeftButton
                                onTapped: root.openSenderProfileForRow()
                            }
                        }
                        // Disambiguator when two members share this display
                        // name. A Loader because its text is "" on virtual rows
                        // (viewport observer).
                        Loader {
                            active: model.senderNameAmbiguous === true
                                    && (model.senderDisplayName
                                        || "").length > 0
                            visible: active
                            Layout.maximumWidth: 180
                            sourceComponent: Label {
                                text: model.sender
                                textFormat: Text.PlainText
                                color: AppTheme.textMuted
                                // Scaled with the name and timestamp beside it.
                                font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                                elide: Label.ElideMiddle
                            }
                        }
                        Label {
                            objectName: "senderTimestamp"
                            text: Qt.formatDateTime(model.timestamp,
                                                    app.settings.clockTimeFormat)
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.scaled(10)
                            Accessible.name: qsTr("Sent at %1").arg(text)
                        }
                        }
                    }

                    // Reply quote: a rule and name in the quoted sender's
                    // identity ink, one elided line of body, and no resting
                    // fill (the row hover tint would dissolve it, and several
                    // palettes share that tint with otherBubble). Scaled text,
                    // sized to the bubble, and a Material Symbols "reply" glyph
                    // rather than a Unicode arrow.
                    Rectangle {
                        id: replyBox
                        objectName: "replyNavigationTarget"
                        visible: model.replyToEventId && model.replyToEventId.length > 0
                                 && !model.redacted
                        readonly property int barWidth: 2
                        // Spans the bubble like its siblings, so the rule reads
                        // as a rule.
                        Layout.fillWidth: true
                        Layout.maximumWidth: root.contentInnerCap
                        Layout.bottomMargin: 2
                        implicitWidth: replyRowWrap.implicitWidth
                                       + replyBox.barWidth + 16
                        implicitHeight: replyRowWrap.implicitHeight + 8
                        // Transparent at rest; the hover tint uses the quoted
                        // sender's ink.
                        color: replyHover.hovered
                               ? Qt.alpha(root.replySenderInk, 0.12)
                               : "transparent"
                        Behavior on color { ColorAnimation { duration: 90 } }
                        radius: AppTheme.radiusSm
                        // A navigating quote is a control: tab stop, key
                        // activation, focus ring, accessible name.
                        activeFocusOnTab: true
                        border.width: activeFocus ? 2 : 0
                        border.color: AppTheme.focusRing
                        Accessible.role: Accessible.Button
                        Accessible.name: (model.replyToSender || "").length > 0
                            ? qsTr("Go to message from %1").arg(model.replyToSender)
                            : qsTr("Go to the original message")
                        Accessible.onPressAction: root.navigateToReplyTarget()
                        Keys.onReturnPressed: (event) => {
                            root.navigateToReplyTarget()
                            event.accepted = true
                        }
                        Keys.onEnterPressed: (event) => {
                            root.navigateToReplyTarget()
                            event.accepted = true
                        }
                        Keys.onSpacePressed: (event) => {
                            root.navigateToReplyTarget()
                            event.accepted = true
                        }
                        HoverHandler { id: replyHover }
                        TapHandler {
                            enabled: root.rowActionsEnabled
                            cursorShape: Qt.PointingHandCursor
                            // Routes through the view contract, never
                            // app.pagination: in the thread panel that would
                            // hand a thread reply to the room loader. Does not
                            // take focus, so a mouse jump does not pull focus
                            // out of the composer.
                            onTapped: root.navigateToReplyTarget()
                        }
                        Rectangle {
                            width: replyBox.barWidth
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            anchors.topMargin: 1
                            anchors.bottomMargin: 1
                            // Rounded so the rule's ends follow the box's
                            // corner radius on hover.
                            radius: 1
                            color: root.replySenderInk
                            opacity: replyHover.hovered ? 1.0 : 0.8
                            Behavior on opacity { NumberAnimation { duration: 90 } }
                        }
                        RowLayout {
                            id: replyRowWrap
                            anchors.left: parent.left
                            anchors.right: parent.right
                            // verticalCenter, not top+bottom, which would
                            // squash the thumbnail to the text's height.
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: replyBox.barWidth + 8
                            anchors.rightMargin: 8
                            spacing: 6
                            Icon {
                                name: "reply"
                                size: AppTheme.scaled(13)
                                color: root.replySenderInk
                                opacity: 0.75
                                Layout.alignment: Qt.AlignVCenter
                            }
                            // Thumbnail for an image reply target, keyed by the
                            // target's event id. The corner is baked by
                            // MediaImageProvider's "|shape:round:" suffix
                            // rather than a per-row MultiEffect mask.
                            Image {
                                id: replyThumb
                                // Bumped by the cache-fill handler so `source`
                                // re-resolves without an imperative assignment,
                                // which would destroy the binding.
                                property int resolveTick: 0
                                readonly property string bridgeSource: {
                                    var _tick = resolveTick
                                    return (model.replyToMediaKey || "").length > 0
                                        ? app.mediaBridge.mediaSource(
                                              model.replyToMediaKey, "thumb")
                                        : ""
                                }
                                visible: (model.replyToMediaKey || "").length > 0
                                         && status !== Image.Error
                                         && app.mediaBridge.supported
                                Layout.preferredWidth: 24
                                Layout.preferredHeight: 24
                                Layout.maximumHeight: 24
                                Layout.alignment: Qt.AlignVCenter
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                sourceSize.width: 48
                                source: visible && bridgeSource.length > 0
                                        ? bridgeSource + "|shape:round:160" : ""
                                Connections {
                                    target: app.mediaBridge
                                    enabled: (model.replyToMediaKey || "")
                                                 .length > 0
                                    function onMediaCached(key) {
                                        // mediaSource() may return "" while
                                        // fetching; re-ask once the cache fills.
                                        if (key === "thumb:" + (model.replyToMediaKey || "")
                                            && replyThumb.source.toString()
                                                   .length === 0)
                                            replyThumb.resolveTick++
                                    }
                                }
                            }
                            Label {
                                objectName: "replyQuoteSender"
                                text: model.replyToSender || qsTr("Reply")
                                color: root.replySenderInk
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                                font.weight: AppTheme.weightStrong
                                elide: Label.ElideRight
                                maximumLineCount: 1
                                // A display name must not eat the quoted line.
                                // A fixed cap: deriving it from
                                // replyRowWrap.width loops in Bubbles.
                                Layout.maximumWidth: AppTheme.scaled(180)
                            }
                            Label {
                                objectName: "replyQuoteBody"
                                text: model.replyToPreview
                                      || root.replyKindLabel()
                                      || qsTr("(original message not loaded)")
                                color: root.replyBodyInk
                                opacity: replyHover.hovered ? 1.0 : 0.85
                                Behavior on opacity { NumberAnimation { duration: 90 } }
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                                elide: Label.ElideRight
                                Layout.fillWidth: true
                                maximumLineCount: 1
                            }
                        }
                    }

                    // One-line summary that replaces the attachment while
                    // embeds are collapsed, and stays above it once expanded.
                    // Its own Layout child because both can be on screen.
                    Loader {
                        id: mediaEmbedSummaryLoader
                        objectName: "mediaEmbedSummaryLoader"
                        active: root.mediaEmbedCollapsible
                        visible: active
                        Layout.alignment: Qt.AlignLeft
                        Layout.preferredWidth: item ? item.implicitWidth : 0
                        Layout.maximumWidth: root.contentInnerCap
                        sourceComponent: CollapsedEmbedRow {
                            maximumWidth: root.contentInnerCap
                            interactive: root.rowActionsEnabled
                            expanded: root.embedExpanded
                            iconName: root.mediaEmbedIcon()
                            kindLabel: root.mediaEmbedKind()
                            detailText: root.mediaEmbedDetail()
                            onToggleRequested: root.toggleEmbedExpanded()
                        }
                    }
                    // Media block (image, sticker, video, audio, file). Each
                    // class reserves its geometry before bytes arrive, so
                    // hydration never reflows rows.
                    Item {
                        id: mediaBox
                        visible: (model.isImage || model.isFile
                                  || model.isVideo === true
                                  || model.isAudio === true
                                  || model.isSticker === true)
                                 && !root.mediaEmbedCollapsed
                        Layout.alignment: Qt.AlignLeft
                        Layout.preferredWidth: Math.min(root.contentInnerCap,
                                                        implicitWidth)
                        Layout.maximumWidth: root.contentInnerCap
                        // A real implicit width so an image-only row grows to
                        // the media size. Files stay compact.
                        implicitWidth: mediaLoader.item
                                       ? mediaLoader.item.implicitWidth : 0
                        implicitHeight: mediaLoader.item
                                        ? mediaLoader.item.implicitHeight : 0
                        Loader {
                            id: mediaLoader
                            // `active`, not `visible`: a hidden but
                            // instantiated attachment would still download,
                            // decode and animate.
                            active: !root.mediaEmbedCollapsed
                            anchors.left: parent.left
                            width: Math.min(root.contentInnerCap,
                                            item ? item.implicitWidth : 0)
                            sourceComponent: root.isGallery ? galleryComponent
                                            : model.isImage ? imageComponent
                                            : model.isSticker === true
                                              ? stickerComponent
                                            : model.isVideo === true
                                              ? videoComponent
                                            : model.isAudio === true
                                              ? audioComponent
                                            : model.isFile  ? fileComponent
                                            : null
                        }
                    }

                    // Poll block (MSC3381): renders the SDK-aggregated outcome;
                    // votes go through app.composer and return as an in-place
                    // diff.
                    Loader {
                        id: pollLoader
                        active: model.isPoll === true
                        visible: active
                        Layout.alignment: Qt.AlignLeft
                        Layout.preferredWidth: item ? item.implicitWidth : 0
                        Layout.maximumWidth: root.contentInnerCap
                        sourceComponent: pollComponent
                    }

                    // Shared place: a card, since the useful action is opening
                    // a map and a live share must say whether it is current.
                    Loader {
                        id: locationLoader
                        active: model.isLocation === true
                        visible: active
                        Layout.alignment: Qt.AlignLeft
                        Layout.preferredWidth: item ? item.implicitWidth : 0
                        Layout.maximumWidth: root.contentInnerCap
                        sourceComponent: locationComponent
                    }

                    // Body text. Hidden for media rows whose body is the
                    // filename, polls (the card shows the question) and
                    // recoverable undecryptable rows (skeleton).
                    TextEdit {
                        id: bodyLabel
                        objectName: "messageBody"
                        visible: text.length > 0 && !root.showsDecryptingSkeleton
                        // Media rows suppress a filename echo; a different body
                        // renders once as a caption. Defined on root so the
                        // segmented renderer applies the same rule.
                        readonly property bool isMediaRow: root.mediaRowBody
                        readonly property bool isMediaCaption:
                            root.mediaCaptionBody
                        // Big emoji: a body of 1-3 emoji sequences renders
                        // large. The count comes from the C++ Unicode Emoji
                        // catalogue. Captions, polls, redacted and
                        // undecryptable rows keep ordinary sizing.
                        readonly property int emojiOnlyCount:
                            (model.redacted || model.isPoll === true
                             || model.undecryptable === true || isMediaRow)
                            ? 0
                            : app.emojiCatalog.emojiOnlySequenceCount(
                                  model.body || "")
                        readonly property bool bigEmoji:
                            emojiOnlyCount >= 1 && emojiOnlyCount <= 3
                        text: {
                            // Fenced code renders through the segmented column;
                            // returning "" keeps the RichText document from
                            // being built for an invisible item.
                            if (root.hasMessageSegments) return ""
                            if (model.redacted) {
                                // A run of deletions collapses to one line,
                                // grouped by TimelineModel.
                                return model.deletedGroupCount > 1
                                    ? qsTr("%n message(s) deleted",
                                           "collapsed run of redactions",
                                           model.deletedGroupCount)
                                    : qsTr("[message deleted]")
                            }
                            // The poll card shows the question; the body is the
                            // MSC1767 fallback.
                            if (model.isPoll === true) return ""
                            // The location card already shows the body.
                            if (model.isLocation === true) return ""
                            // MSC2530 senders put the caption in body and the
                            // name in filename; compare loosely so
                            // case/whitespace variants are still echoes.
                            if (isMediaRow && !isMediaCaption) return ""
                            // Formatted bodies are already sanitized by
                            // MessageHtml::sanitize and must not be re-escaped.
                            // Read the role once.
                            var fb = model.formattedBody
                            var html = (fb && fb.length > 0)
                                ? fb
                                : app.linkPreviews.linkifiedBody(
                                      model.body || "")
                            // @room is plain text (no matrix.to link), so ink
                            // it explicitly. Gated on the event's
                            // m.mentions.room, never on the characters alone.
                            if (model.mentionsRoom === true && root.timelineModel)
                                html = root.timelineModel.markRoomMention(html)
                            return root.highlightSearchMatches(
                                        html,
                                        root.searchHighlight,
                                        root.isCurrentSearchHit)
                        }
                        color: model.undecryptable === true
                               ? AppTheme.muted
                               : bodyLabel.isMediaCaption ? AppTheme.textSecondary
                               : root.bubbleMode && model.isOwn === true
                                 ? AppTheme.ownBubbleText : AppTheme.text
                        // TextEdit does not inherit the Controls font. A whole
                        // font so the colour emoji face is a real per-character
                        // fallback (QML has no families list, and Qt 6.8's
                        // automatic fallback picks a monochrome face). Size and
                        // italic pass through because assigning `font` replaces
                        // them.
                        font: app.textFontWithEmoji(
                                  AppTheme.uiFont,
                                  bodyLabel.bigEmoji
                                      ? AppTheme.scaled(root.compactMode
                                                        || root.inThreadPanel ? 48 : 60)
                                      : AppTheme.scaled(
                                            bodyLabel.isMediaCaption ? 12
                                            : root.compactMode || root.inThreadPanel
                                              ? 13 : AppTheme.fontSizeM),
                                  model.redacted || model.undecryptable === true)
                        // No lineHeight: lineHeight/lineHeightMode are
                        // QQuickText properties, and assigning them on a
                        // TextEdit is a load-time error. Body leading would
                        // need a `line-height` from MessageHtml::sanitize or a
                        // move off TextEdit.
                        wrapMode: Text.Wrap
                        readOnly: true
                        // Before bubbleRow has its final width, measuring
                        // wrapped text against a tiny width yields an enormous
                        // transient height; use a normal column width until
                        // then.
                        Layout.maximumWidth: bubbleRow.width > 8
                                             ? Math.min(720,
                                                        root.contentInnerCap)
                                             : 560
                        // Keep the last line clear of the receipt rail.
                        Layout.rightMargin: root.receiptRailReserve
                        textFormat: Text.RichText
                        selectByMouse: true
                        Accessible.name: model.body || ""
                        // One routing implementation, shared with the segmented
                        // renderer.
                        onLinkActivated: function(link) {
                            root.openMessageLink(link)
                        }

                        // Tooltip explaining an undecryptable placeholder;
                        // reassuring, not alarming.
                        HoverHandler {
                            id: undecryptHover
                            enabled: model.undecryptable === true
                        }
                        ToolTip {
                            visible: undecryptHover.hovered
                            delay: 400
                            text: qsTr(
                                "Missing room key. Restore your recovery key " +
                                "in Settings, or wait for another verified " +
                                "device to share the key.")
                        }
                    }

                    // Fenced code blocks: ordered segments in place of the
                    // single body TextEdit. Inactive for ordinary messages, so
                    // they instantiate nothing (and no text item can exist
                    // holding "").
                    Loader {
                        id: segmentsLoader
                        objectName: "messageSegments"
                        // Captured outside the Repeater: inside it, `model` can
                        // resolve to the segment's own model.
                        readonly property bool ownMessage: model.isOwn === true
                        readonly property string plainBody: model.body || ""
                        active: root.hasMessageSegments
                        visible: active
                        Layout.fillWidth: true
                        Layout.maximumWidth: bubbleRow.width > 8
                                             ? Math.min(720,
                                                        root.contentInnerCap)
                                             : 560
                        Layout.rightMargin: root.receiptRailReserve
                        // Derived from the row, not bubble.width or the
                        // segment's own width: in Bubbles those are this
                        // column's implicit width, which loops. Same cap as
                        // everywhere else (root.contentInnerCap).
                        readonly property real segmentCap:
                            Math.max(80, Math.min(720,
                                                  root.contentInnerCap - 8))
                        sourceComponent: ColumnLayout {
                            spacing: 4
                            // One accessible reading on the container;
                            // model.body is the original markdown source.
                            Accessible.role: Accessible.StaticText
                            Accessible.name: segmentsLoader.plainBody
                            Repeater {
                                model: root.messageSegments
                                delegate: Item {
                                    id: segmentRow
                                    objectName: "messageSegmentRow"
                                    required property var modelData
                                    // kind: 0 RichText, 1 CodeBlock. Unknown
                                    // kinds degrade to rich text.
                                    readonly property bool isCode:
                                        modelData && modelData.kind === 1
                                    Layout.fillWidth: true
                                    // Propagate both implicit sizes: in Bubbles
                                    // the bubble's width is bubbleContent's
                                    // implicit width, so a 0 would collapse a
                                    // code-only message. Read from the Loader,
                                    // never segmentLoader.item: QQuickTextEdit
                                    // computes its implicit width lazily and
                                    // emits implicitWidthChanged synchronously
                                    // on the first read, which Qt reports as a
                                    // binding loop. The Loader mirrors the same
                                    // value without that side effect.
                                    implicitWidth: segmentLoader.implicitWidth
                                    implicitHeight: segmentLoader.implicitHeight
                                    Loader {
                                        id: segmentLoader
                                        anchors.left: parent.left
                                        // The two components have different sizing
                                        // contracts. A CodeBlock's implicit width
                                        // is width-independent (NoWrap, clamped),
                                        // so it can size itself: short code stays
                                        // narrow, long lines clamp and scroll. A
                                        // wrapping TextEdit's content depends on
                                        // its width, so it takes its width from
                                        // the row and reports its natural width
                                        // upward only; min(cap, implicitWidth)
                                        // here would loop. The `> 8` guard is
                                        // bodyLabel's: before the row has a width,
                                        // wrapped text measures enormously tall.
                                        width: !segmentLoader.item
                                               ? segmentsLoader.segmentCap
                                               : segmentRow.isCode
                                                 ? Math.min(
                                                       segmentsLoader.segmentCap,
                                                       segmentLoader.implicitWidth)
                                                 : (segmentRow.width > 8
                                                    ? Math.min(
                                                          segmentsLoader.segmentCap,
                                                          segmentRow.width)
                                                    : segmentsLoader.segmentCap)
                                        sourceComponent: segmentRow.isCode
                                                         ? codeSegment
                                                         : richSegment
                                    }
                                    // Declared inside the delegate so its
                                    // creation context resolves `segmentRow`
                                    // and `root`.
                                    Component {
                                        id: richSegment
                                        TextEdit {
                                            objectName: "messageSegmentText"
                                            text: root.highlightSearchMatches(
                                                      segmentRow.modelData.text
                                                      || "",
                                                      root.searchHighlight,
                                                      root.isCurrentSearchHit)
                                            // Same ink, font, scaling and interaction
                                            // as the single-body path. Big emoji
                                            // cannot occur with a fenced block.
                                            color: root.bubbleMode
                                                   && segmentsLoader.ownMessage
                                                   ? AppTheme.ownBubbleText
                                                   : AppTheme.text
                                            font.family: AppTheme.uiFont
                                            font.pixelSize: AppTheme.scaled(
                                                root.mediaCaptionBody ? 12
                                                : root.compactMode
                                                  || root.inThreadPanel
                                                ? 13 : AppTheme.fontSizeM)
                                            // No lineHeight: TextEdit has no such
                                            // property.
                                            wrapMode: Text.Wrap
                                            readOnly: true
                                            textFormat: Text.RichText
                                            selectByMouse: true
                                            onLinkActivated: function(link) {
                                                root.openMessageLink(link)
                                            }
                                        }
                                    }
                                    Component {
                                        id: codeSegment
                                        CodeBlock {
                                            // Plain text, rendered by CodeBlock as
                                            // PlainText.
                                            code: segmentRow.modelData.text
                                                  || ""
                                            language:
                                                segmentRow.modelData.language
                                                || ""
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Decrypting skeleton: two line bars reserve text geometry;
                    // decryption replaces them in place without moving the
                    // anchor.
                    ColumnLayout {
                        id: decryptingSkeleton
                        objectName: "decryptingSkeleton"
                        visible: root.showsDecryptingSkeleton
                        spacing: 5
                        Layout.fillWidth: true
                        Layout.topMargin: 2
                        Skeleton {
                            active: root.rowOnScreen
                                    && root.showsDecryptingSkeleton
                                    && !root.decryptStalled
                            Layout.preferredWidth: Math.min(
                                420, Math.max(
                                    120, root.contentInnerCap * 0.55))
                            Layout.preferredHeight: AppTheme.scaled(13)
                        }
                        Skeleton {
                            active: root.rowOnScreen
                                    && root.showsDecryptingSkeleton
                                    && !root.decryptStalled
                            Layout.preferredWidth: Math.min(
                                300, Math.max(
                                    80, root.contentInnerCap * 0.35))
                            Layout.preferredHeight: AppTheme.scaled(13)
                        }
                    }

                    // Unable-to-decrypt actions: reason category, recovery
                    // hint, a bounded manual Retry and a link to Security
                    // settings. Never shows session ids, ciphertext or raw
                    // event JSON.
                    RowLayout {
                        id: utdRow
                        visible: model.undecryptable === true
                        spacing: AppTheme.spacingS
                        Label {
                            text: {
                                // If this session's published identity key does
                                // not match its Olm account, keys can never
                                // arrive, so do not say "Waiting for keys…";
                                // the corner prompt explains and repairs. Uses
                                // a dedicated signal that fires at most once
                                // per session rather than securityStateChanged,
                                // which would re-evaluate every row.
                                if (app.encryptionIdentityBroken)
                                    return qsTr("This session can't unlock "
                                                + "encrypted messages")
                                var kind = model.errorKind || ""
                                if (kind === "membership")
                                    return qsTr("Sent before you joined")
                                if (kind === "device_trust")
                                    return qsTr("Sender requires a verified session")
                                if (kind === "withheld")
                                    return qsTr("Key withheld by sender")
                                return qsTr("Waiting for keys…")
                            }
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                            font.italic: true
                        }
                        // Inline text links use AppTheme.link, not accent.
                        Label {
                            text: qsTr("Retry decryption")
                            color: AppTheme.link
                            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                            font.underline: true
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                enabled: root.rowActionsEnabled
                        onClicked: root.timelineModel.retryDecryption()
                            }
                        }
                        Label {
                            text: qsTr("Security settings")
                            color: AppTheme.link
                            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                            font.underline: true
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                enabled: root.rowActionsEnabled
                        onClicked: app.showSettingsSection("security")
                            }
                        }
                    }

                    // The link preview's one-line summary, sharing the media
                    // summary's control and expansion flag. Only a loaded card
                    // collapses.
                    Loader {
                        id: previewEmbedSummaryLoader
                        objectName: "previewEmbedSummaryLoader"
                        active: root.previewEmbedCollapsible
                        visible: active
                        Layout.alignment: Qt.AlignLeft
                        Layout.preferredWidth: item ? item.implicitWidth : 0
                        Layout.maximumWidth: root.contentInnerCap
                        sourceComponent: CollapsedEmbedRow {
                            maximumWidth: root.contentInnerCap
                            interactive: root.rowActionsEnabled
                            expanded: root.embedExpanded
                            iconName: "link"
                            kindLabel: qsTr("Link")
                            detailText: root.previewEmbedDetail()
                            onToggleRequested: root.toggleEmbedExpanded()
                        }
                    }
                    // Rich link-preview card from LinkPreviewController: Rust
                    // performs the protected fetch; QML renders whitelisted
                    // fields only. Encrypted rooms default to click-to-load.
                    Loader {
                        id: previewLoader
                        Layout.alignment: Qt.AlignLeft
                        Layout.preferredWidth: Math.min(
                            root.contentInnerCap,
                            item ? item.implicitWidth : 400)
                        Layout.maximumWidth: root.contentInnerCap
                        active: root.preview.state !== undefined
                                && root.preview.state !== "none"
                                && !model.redacted
                                && !model.isImage && !model.isFile
                                && !root.previewEmbedCollapsed
                        visible: active
                        sourceComponent: root.preview.state === "loaded"
                                         && root.preview.isDirectMedia === true
                                         && root.preview.gifOversized !== true
                                         ? directMediaPreviewComponent
                                         : linkPreviewComponent
                    }

                    // Undo for a dismissed preview, where the card was. A
                    // Loader rather than a hidden Label (a Label born with ""
                    // stays a viewport observer).
                    Loader {
                        objectName: "previewRestoreLoader"
                        // Always shown while dismissed, so the reader can tell
                        // a preview existed and bring it back.
                        active: root.previewDismissed
                        visible: active
                        Layout.alignment: Qt.AlignLeft
                        Layout.preferredHeight: active ? implicitHeight : 0
                        sourceComponent: Label {
                            objectName: "previewRestoreInline"
                            text: qsTr("Show preview")
                            color: AppTheme.stormTextMuted
                            font.pixelSize: AppTheme.textMeta
                            font.underline: restoreHover.hovered
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Show the link preview again")
                            HoverHandler {
                                id: restoreHover
                                cursorShape: Qt.PointingHandCursor
                            }
                            TapHandler {
                                // previewRoomId: the same key the dismissal and
                                // the context menu's undo use.
                                enabled: root.rowActionsEnabled
                    onTapped: app.linkPreviews.restorePreviewForEvent(
                                    root.previewRoomId, root.actionKey)
                            }
                        }
                    }

                    // Upload progress for an outgoing attachment, from the SDK
                    // send queue's MediaUpload reports. -1 (total not known
                    // yet) shows the indeterminate sweep rather than a false
                    // 0%. A Loader: this exists on very few rows.
                    Loader {
                        id: uploadProgressLoader
                        objectName: "uploadProgressLoader"
                        Layout.fillWidth: true
                        Layout.topMargin: AppTheme.spacing2
                        active: model.isOwn === true && model.status === 1
                                && root.mediaRowBody
                        visible: active
                        sourceComponent: AppProgressBar {
                            objectName: "uploadProgressBar"
                            indeterminate: root.uploadProgress < 0
                            value: root.uploadProgress < 0
                                   ? 0 : root.uploadProgress
                            Accessible.role: Accessible.ProgressBar
                            Accessible.name: root.uploadProgress < 0
                                ? qsTr("Uploading")
                                : qsTr("Uploading, %1%").arg(
                                      Math.round(root.uploadProgress * 100))
                        }
                    }

                    RowLayout {
                        id: metaRow
                        Layout.fillWidth: true
                        spacing: AppTheme.spacingXS
                        // A Loader: this text is "" on most rows, which would
                        // be a permanent viewport observer.
                        Loader {
                            id: metaLabel
                            active: (model.isOwn === true
                                     && (model.status === 1
                                         || model.status === 2))
                                    || model.edited === true
                            visible: active
                            sourceComponent: Label {
                                text: {
                                    var ts = Qt.formatDateTime(
                                        model.timestamp,
                                        app.settings.clockTimeFormat)
                                    // Status: 0=Sent, 1=Sending, 2=Failed
                                    if (model.isOwn && model.status === 1) {
                                        // Percentage only when known.
                                        if (root.uploadProgress >= 0)
                                            return qsTr("%1 • sending… %2%")
                                                .arg(ts)
                                                .arg(Math.round(
                                                    root.uploadProgress * 100))
                                        return qsTr("%1 • sending…").arg(ts)
                                    }
                                    if (model.isOwn && model.status === 2)
                                        return qsTr("%1 • failed").arg(ts)
                                    if (model.edited) return qsTr("edited")
                                    return ""
                                }
                                color: root.bubbleMode
                                       && model.isOwn === true
                                       ? AppTheme.onAccentMuted
                                       : AppTheme.textMuted
                                font.pixelSize: AppTheme.scaled(10)
                                // The "edited" marker opens the edit history;
                                // underlined only then.
                                font.underline: model.edited === true
                                                && !(model.isOwn && model.status !== 1
                                                     && model.status !== undefined
                                                     && model.status === 2)
                                Accessible.name: model.edited === true
                                                 ? qsTr("edited — show edit history")
                                                 : text
                                Accessible.role: model.edited === true
                                                 ? Accessible.Button : Accessible.StaticText
                                TapHandler {
                                    enabled: model.edited === true
                                             && root.rowActionsEnabled
                                    onTapped:
                                        root.openEditHistory(model.eventId)
                                }
                            }
                        }
                        // Retry for failed local echoes: the SDK send queue
                        // re-attempts the same item, so retrying never
                        // duplicates. An inline link (AppTheme.link).
                        Label {
                            visible: model.isOwn && model.status === 2
                            text: qsTr("Retry")
                            color: AppTheme.link
                            // Matches its sibling on the same status line.
                            font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                            font.underline: true
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                // Not gated on rowActionsEnabled: failed echoes
                                // are not selectable, so retrying mid-selection
                                // is harmless.
                        onClicked: root.timelineModel.retrySend(
                                               root.sourceModelRow(index))
                            }
                        }
                        // Discard a send that has not reached the server, via
                        // the SDK send queue's abort (the only thing that can
                        // cancel an in-flight upload). The row is not removed
                        // here: the abort can lose the race, and the backend
                        // removes the item only when it really aborted. Gated
                        // on the model, so a backend without a send queue
                        // offers nothing.
                        Label {
                            objectName: "cancelSendLink"
                            // model.status first so the binding depends on it;
                            // canCancelSendAt is a plain function call.
                            visible: model.isOwn === true
                                     && (model.status === 1
                                         || model.status === 2)
                                     && root.canCancelSendAt(index)
                            text: qsTr("Cancel")
                            color: AppTheme.link
                            font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                            font.underline: true
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Cancel sending this message")
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.timelineModel.cancelSend(
                                               root.sourceModelRow(index))
                            }
                        }
                        // Thread summary card on the root event. Thread replies
                        // are hidden from the main timeline, so the root shows
                        // the thread: icon, latest sender, preview, reply count
                        // and unread indicator. A Loader because roots are rare
                        // and the card's time label is "" without a timestamp
                        // (viewport observer).
                        Loader {
                            active: model.isThreadRoot === true
                            visible: active
                            // Bounded by what is left of the row: an edited
                            // thread root shares metaRow with the "edited"
                            // marker, and a card sized only by its implicit
                            // width would run off the bubble. maximumWidth
                            // keeps its natural size when there is room; the
                            // card's preview elides. Loop-free only because
                            // that preview Label is single-line with elide, so
                            // its implicit width does not follow its width.
                            // Giving it a wrapMode or removing
                            // maximumLineCount: 1 would make this a binding
                            // loop.
                            Layout.fillWidth: true
                            Layout.maximumWidth: item ? item.implicitWidth : 0
                            sourceComponent: ThreadSummaryCard {
                            id: threadSummaryCard
                            replyCount: model.threadReplyCount !== undefined
                                        ? model.threadReplyCount : -1
                            latestSender: model.threadLatestSenderDisplayName || ""
                            latestSenderId: model.threadLatestSender || ""
                            latestPreview: model.threadLatestPreview || ""
                            latestKind: model.threadLatestKind || "text"
                            latestAvatarMxc: model.threadLatestSenderAvatarMxc || ""
                            latestTimestamp: model.threadLatestTimestamp
                            unread: model.threadUnread === true
                            onActivated: app.thread.openThread(
                                app.currentRoomId, root.eventIdForActions())

                            // Read once so a late model change cannot mix two
                            // threads' faces.
                            readonly property string rootId:
                                model.isThreadRoot === true
                                    ? root.eventIdForActions() : ""
                            // Guarded: an unqualified `app` lookup during
                            // delegate creation can resolve undefined and latch
                            // the binding at "".
                            readonly property string roomId:
                                typeof app !== "undefined" ? app.currentRoomId : ""

                            // Pure read, no request; the fetch is explicit
                            // below.
                            function refreshParticipants() {
                                participants = (rootId !== "" && roomId !== "")
                                    ? app.threads.participants(roomId, rootId)
                                    : []
                            }
                            // Fetched once per (room, root);
                            // requestParticipants is idempotent. Not a viewport
                            // gate: every loaded row is instantiated, so
                            // opening a thread-heavy room issues one relations
                            // fetch per root (deduplicated per session).
                            function ensureParticipants() {
                                if (!visible || rootId === "" || roomId === "")
                                    return
                                app.threads.requestParticipants(roomId, rootId)
                                refreshParticipants()
                            }
                            onRootIdChanged: ensureParticipants()
                            onVisibleChanged: ensureParticipants()
                            Component.onCompleted: ensureParticipants()
                            Connections {
                                target: app.threads
                                function onParticipantsChanged(roomId, rootEventId) {
                                    if (roomId === threadSummaryCard.roomId
                                        && rootEventId === threadSummaryCard.rootId)
                                        threadSummaryCard.refreshParticipants()
                                }
                            }
                            }
                        }
                    }
                }
            }

            // Action toolbar: one bar per row, created on first hover and
            // anchored in the row with plain anchors. Never reparent it into
            // Overlay.overlay: a Loader still owns the item for destruction,
            // which crashed during pagination and room switches, and mapToItem
            // cannot survive the rows' 180° rotation. Hover is exclusive, so
            // two rows never both show one.
            Loader {
                id: messageActionBarLoader
                objectName: "messageActionBarLoader"
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: -3
                // Top-right corner of the full-width row, as in Element. The
                // margin clears the overlaid scrollbar.
                anchors.rightMargin: AppTheme.scrollbarWidth + AppTheme.spacing2
                                     + root.actionBarReceiptReserve
                z: 3
                // Created on first need, then kept for the delegate's lifetime.
                // The latch write is deferred: onLoaded runs inside the
                // `active` binding's evaluation, and writing a dependency there
                // is a binding loop.
                property bool latched: false
                active: latched || root.actionsVisible
                onLoaded: Qt.callLater(function() { latched = true })
                visible: root.actionsVisible
                sourceComponent: Rectangle {
                id: messageActionBar
                // Surface background, 1px borderStrong, radiusTile, 2px
                // padding.
                radius: AppTheme.radiusTile
                color: AppTheme.surface
                border.color: AppTheme.borderStrong
                border.width: 1
                implicitWidth: threadActionRow.implicitWidth
                               + AppTheme.spacing2 * 2
                implicitHeight: threadActionRow.implicitHeight
                                + AppTheme.spacing2 * 2

                // Tooltip flip only: a computed boolean from a live mapToItem
                // read; stores and reparents nothing. 44 is a one-line tooltip
                // plus margin.
                readonly property bool tooltipsBelow:
                    Overlay.overlay
                    ? bubbleRow.mapToItem(Overlay.overlay, 0, 0).y < 44
                    : false

                Row {
                    id: threadActionRow
                    anchors.centerIn: parent
                    spacing: 2
                    // Leads on an image row, as in Element. Hidden once the
                    // image is hidden; the placeholder's "Show image" is then
                    // the action.
                    IconButton {
                        id: threadHideMediaButton
                        objectName: "messageHideMediaButton"
                        visible: root.mediaHideable && !root.mediaHidden
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "visibility_off"
                        iconSize: 18
                        Accessible.name: qsTr("Hide image")
                        ToolTip {
                            visible: threadHideMediaButton.hovered
                            delay: 500
                            text: qsTr("Hide")
                            y: messageActionBar.tooltipsBelow
                               ? threadHideMediaButton.height + AppTheme.spacingXS
                               : -implicitHeight - AppTheme.spacingXS
                        }
                        onClicked: root.setMediaHidden(true)
                    }
                    IconButton {
                        id: threadReactButton
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "add_reaction"
                        iconSize: 18
                        // Local predicate equivalent to "messagePermalink() is
                        // non-empty", without the per-row timeline scan the C++
                        // call costs.
                        enabled: !model.redacted
                                 && (model.eventId || "").length > 0
                                 && model.eventId.indexOf("local:") !== 0
                        Accessible.name: qsTr("React to message")
                        ToolTip {
                            visible: threadReactButton.hovered
                            delay: 500
                            text: qsTr("React")
                            y: messageActionBar.tooltipsBelow
                               ? threadReactButton.height + AppTheme.spacingXS
                               : -implicitHeight - AppTheme.spacingXS
                        }
                        onClicked: {
                            if (root.timelineView)
                                root.timelineView.pinnedActionsKey = root.actionKey
                            root.openReactionPickerFor(root.eventIdForActions(),
                                                       threadReactButton)
                        }
                    }
                    IconButton {
                        id: threadReplyButton
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "reply"
                        iconSize: 18
                        // Local predicate equivalent to "messagePermalink() is
                        // non-empty", without the per-row timeline scan the C++
                        // call costs.
                        enabled: !model.redacted
                                 && (model.eventId || "").length > 0
                                 && model.eventId.indexOf("local:") !== 0
                        Accessible.name: qsTr("Reply to message")
                        ToolTip {
                            visible: threadReplyButton.hovered
                            delay: 500
                            text: qsTr("Reply")
                            y: messageActionBar.tooltipsBelow
                               ? threadReplyButton.height + AppTheme.spacingXS
                               : -implicitHeight - AppTheme.spacingXS
                        }
                        onClicked: {
                            root.beginReply(root.eventIdForActions())
                        }
                    }
                    IconButton {
                        id: threadEditButton
                        objectName: "messageEditButton"
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "edit_square"
                        iconSize: 18
                        // Same gate as the context menu's Edit: own editable
                        // text, not a local echo.
                        visible: root.timelineModel
                                 && root.timelineModel.canEditEvent(
                                        root.eventIdForActions())
                        Accessible.name: qsTr("Edit message")
                        ToolTip {
                            visible: threadEditButton.hovered
                            delay: 500
                            text: qsTr("Edit")
                            y: messageActionBar.tooltipsBelow
                               ? threadEditButton.height + AppTheme.spacingXS
                               : -implicitHeight - AppTheme.spacingXS
                        }
                        onClicked: {
                            var id = root.eventIdForActions()
                            app.composer.beginEdit(
                                id,
                                root.timelineModel.visibleTextForEvent(id),
                                root.timelineModel.sanitizedHtmlForEvent(id),
                                // The timeline that holds it: the composite id
                                // in a thread panel, the room id otherwise.
                                root.timelineModel.roomId)
                        }
                    }
                    IconButton {
                        id: threadMoreButton
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "more_vert"
                        iconSize: 18
                        // accentSoft chip while the menu is open.
                        active: root.moreMenuOpen
                        Accessible.name: qsTr("More message actions")
                        ToolTip {
                            visible: threadMoreButton.hovered
                            delay: 500
                            text: qsTr("More")
                            y: messageActionBar.tooltipsBelow
                               ? threadMoreButton.height + AppTheme.spacingXS
                               : -implicitHeight - AppTheme.spacingXS
                        }
                        onClicked: {
                            var menu = root.ensureContextMenu()
                            // Map to overlay space. Both corners are mapped and
                            // the max taken, since which local corner is
                            // visually bottom-right depends on rotation (the
                            // room timeline is rotated, the thread panel is
                            // not).
                            var a = messageActionBar.mapToItem(
                                Overlay.overlay, 0, 0)
                            var b = messageActionBar.mapToItem(
                                Overlay.overlay, messageActionBar.width,
                                messageActionBar.height)
                            root.openContextMenu(
                                Math.max(a.x, b.x) - menu.implicitWidth,
                                Math.max(a.y, b.y),
                                true)
                        }
                    }
                }
            }
            }
        }

        // Reactions row
        Flow {
            id: reactionsFlow
            objectName: "reactionsFlow"
            // Identity-guarded projection: ReactionsRole returns a fresh list
            // on every read, so only a real change replaces the Repeater's
            // model (otherwise every chip is rebuilt on unrelated diffs).
            readonly property var liveReactions: model.reactions || []
            property var shownReactions: []
            function refreshReactions() {
                var next = liveReactions
                if (JSON.stringify(next) !== JSON.stringify(shownReactions))
                    shownReactions = next
            }
            onLiveReactionsChanged: refreshReactions()
            Component.onCompleted: refreshReactions()
            visible: !model.redacted && shownReactions.length > 0
            // A Flow only wraps if it has a width. The right margin keeps the
            // last chip clear of the read-receipt rail.
            Layout.fillWidth: true
            // A Flow packs from its left edge, so under an own bubble
            // (right-aligned in Bubbles) the chips landed far from the message.
            // Give the Flow the bubble's band so the chips hang under it.
            // Reading bubble.x/width is safe here: this Flow is a sibling of
            // bubbleRow and does not feed the bubble's measurement.
            readonly property bool followsOwnBubble:
                root.bubbleMode && model.isOwn === true
            // The facepile rides the row's right edge in every layout, so its
            // reservation is a floor on the right margin.
            readonly property real pileReserve: readReceiptStrip.visible
                ? receiptRow.width + AppTheme.spacingXS : 0
            Layout.rightMargin: reactionsFlow.followsOwnBubble
                ? Math.max(reactionsFlow.pileReserve,
                           bubbleRow.width - (bubble.x + bubble.width))
                : reactionsFlow.pileReserve
            Layout.alignment: Qt.AlignLeft
            // Align with the message body in every layout (Modern 40, Compact
            // 8, Bubbles 44) plus a gap below media cards. Under an own bubble
            // the band starts at the bubble's left edge, but is never narrower
            // than a short run of chips.
            readonly property real ownBubbleChipRun: 220
            Layout.leftMargin: reactionsFlow.followsOwnBubble
                ? Math.max(root.avatarGutterWidth,
                           Math.min(bubble.x,
                                    bubbleRow.width
                                    - reactionsFlow.ownBubbleChipRun))
                : root.avatarGutterWidth
            Layout.topMargin: AppTheme.spacingXS
            spacing: AppTheme.spacingXS
            Repeater {
                model: reactionsFlow.shownReactions
                Rectangle {
                    id: reactionChip
                    objectName: "reactionChip"
                    // Own reaction: accent-soft fill, full accent border,
                    // accent text; others: a neutral chip. Pill radius, 9px/3px
                    // padding, min height 22. States change paint only, never
                    // geometry, because a 1px growth would reflow the Flow.
                    readonly property color baseFill: modelData.byMe
                        ? AppTheme.reactionSelectedBackground
                        : AppTheme.reactionBackground
                    // Lift on dark themes, deepen on light, so the hovered chip
                    // never recedes.
                    readonly property color hoverFill:
                        AppTheme.dark ? Qt.lighter(baseFill, 1.35)
                                      : Qt.darker(baseFill, 1.07)
                    color: reactionMouse.pressed
                           ? (AppTheme.dark ? Qt.lighter(baseFill, 1.6)
                                            : Qt.darker(baseFill, 1.14))
                           : reactionHover.hovered ? hoverFill : baseFill
                    Behavior on color { ColorAnimation { duration: 80 } }
                    radius: AppTheme.radiusPill
                    border.color: modelData.byMe
                                  ? AppTheme.accent
                                  : reactionHover.hovered
                                    ? AppTheme.borderStrong : AppTheme.border
                    // Whole pixels only: a fractional border blurs at DPR 1.
                    border.width: modelData.byMe ? 2 : 1
                    // A focus stop, so Tab reaches the chip.
                    activeFocusOnTab: true
                    Keys.onReturnPressed: (event) => {
                        root.timelineModel.toggleReaction(root.eventIdForActions(),
                                             modelData.key)
                        event.accepted = true
                    }
                    Keys.onEnterPressed: (event) => {
                        root.timelineModel.toggleReaction(root.eventIdForActions(),
                                             modelData.key)
                        event.accepted = true
                    }
                    Keys.onSpacePressed: (event) => {
                        root.timelineModel.toggleReaction(root.eventIdForActions(),
                                             modelData.key)
                        event.accepted = true
                    }
                    // Focus ring drawn inside the pill: an outset ring would
                    // cross the neighbouring chip (4px Flow spacing) and the
                    // receipt rail.
                    Rectangle {
                        anchors.fill: parent
                        visible: reactionChip.activeFocus
                        color: "transparent"
                        radius: reactionChip.radius
                        border.width: 2
                        border.color: AppTheme.focusRing
                    }
                    implicitWidth: reactionRow.implicitWidth + 14
                    // Both labels are pinned to a 16px content height so every
                    // chip has the same height regardless of emoji font
                    // metrics. The 22px floor scales with the text so a large
                    // count does not overflow.
                    implicitHeight:
                        Math.max(AppTheme.scaled(AppTheme.reactionChipHeight),
                                 reactionRow.implicitHeight + 4)
                    HoverHandler { id: reactionHover }

                    // Who reacted: names are resolved in C++ (display name,
                    // localpart fallback, never a bare MXID) and capped;
                    // reactorTotal is the uncapped count. An absent list means
                    // no tooltip. Degrades on models without the fields.
                    readonly property var reactorNames:
                        modelData.reactorNames || []
                    readonly property int reactorTotal:
                        modelData.reactorTotal >= 0
                        ? modelData.reactorTotal : (modelData.count || 0)
                    // Bound each user-chosen name (the shared ToolTip has no
                    // width cap) and mark the cut with an ellipsis.
                    function boundedName(value) {
                        var name = value || ""
                        return name.length > 24
                               ? name.substring(0, 24) + "…" : name
                    }
                    readonly property string reactorSummary: {
                        var names = reactionChip.reactorNames
                        if (names.length === 0
                                || (names[0] || "").length === 0)
                            return ""
                        // A list longer than the reported total would make the
                        // tail negative.
                        var total = Math.max(reactionChip.reactorTotal,
                                             names.length)
                        var first = reactionChip.boundedName(names[0])
                        if (total <= 1)
                            return first
                        var second = names.length > 1
                                     ? reactionChip.boundedName(names[1]) : ""
                        if (second.length === 0) {
                            return total === 2
                                ? qsTr("%1 and 1 other").arg(first)
                                : qsTr("%1 and %2 others")
                                    .arg(first).arg(total - 1)
                        }
                        if (total === 2)
                            return qsTr("%1 and %2").arg(first).arg(second)
                        if (total === 3)
                            return qsTr("%1, %2 and 1 other")
                                .arg(first).arg(second)
                        return qsTr("%1, %2 and %3 others")
                            .arg(first).arg(second).arg(total - 2)
                    }
                    // The attached ToolTip: one shared instance app-wide,
                    // rather than a Popup per chip. The HoverHandler covers the
                    // whole chip, so the tip does not flicker.
                    ToolTip.text: reactionChip.reactorSummary
                    ToolTip.visible: reactionHover.hovered
                                     && reactionChip.reactorSummary.length > 0
                    ToolTip.delay: 300
                    // MSC2545 clients (Cinny, Sable) react with a custom emoji
                    // by putting the pack image's mxc URI in the key; render it
                    // rather than showing a raw "mxc://…". The key is remote
                    // text, so only a syntactically plain mxc takes the image
                    // branch, fetched through MediaBridge, never as a URL or
                    // rich text.
                    readonly property bool customEmojiReaction:
                        typeof modelData.key === "string"
                        && modelData.key.startsWith("mxc://")
                        && modelData.key.length > 6
                    // The shortcode when a pack this account holds carries that
                    // image, else "". Never the raw mxc.
                    readonly property string customEmojiName: {
                        if (!reactionChip.customEmojiReaction)
                            return ""
                        var rev = app.stickers.revision
                        var code = app.stickers.shortcodeForUrl(modelData.key)
                        return code.length > 0 ? code : qsTr("custom emoji")
                    }
                    RowLayout {
                        id: reactionRow
                        anchors.centerIn: parent
                        spacing: 4
                        Label {
                            objectName: "reactionEmoji"
                            visible: !reactionChip.customEmojiReaction
                            Layout.alignment: Qt.AlignVCenter
                            // A fixed box with the line height pinned to it:
                            // colour-emoji faces report different metrics per
                            // sequence, and without FixedHeight the ink sits
                            // high and looks raised beside the count.
                            Layout.preferredHeight:
                                AppTheme.scaled(AppTheme.reactionEmojiBox)
                            lineHeightMode: Text.FixedHeight
                            lineHeight: AppTheme.scaled(AppTheme.reactionEmojiBox)
                            text: reactionChip.customEmojiReaction
                                  ? "" : modelData.key
                            // Name the emoji face so Qt 6.8 does not fall back
                            // to a monochrome one.
                            font.family: app.emojiFontFamily || ""
                            // Sized like inline emoji in a body, not like the
                            // count; the chip's implicitHeight follows it.
                            font.pixelSize:
                                AppTheme.scaled(AppTheme.reactionEmojiSize)
                            verticalAlignment: Text.AlignVCenter
                        }
                        Image {
                            id: customEmojiImage
                            visible: reactionChip.customEmojiReaction
                            Layout.alignment: Qt.AlignVCenter
                            // Matched to the Unicode glyph size.
                            Layout.preferredHeight:
                                AppTheme.scaled(AppTheme.reactionEmojiBox)
                            Layout.preferredWidth:
                                AppTheme.scaled(AppTheme.reactionEmojiBox)
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            cache: true
                            // Re-resolve through a counter the binding reads;
                            // assigning `source` would destroy the binding.
                            property int resolveTick: 0
                            source: {
                                var _tick = resolveTick
                                return (reactionChip.customEmojiReaction
                                        && app.mediaBridge.supported)
                                    ? app.mediaBridge.mxcImageSource(
                                          modelData.key, 64)
                                    : ""
                            }
                            Connections {
                                target: app.mediaBridge
                                enabled: reactionChip.customEmojiReaction
                                function onMediaCached(cacheKey) {
                                    if (cacheKey.endsWith(":" + modelData.key))
                                        customEmojiImage.resolveTick++
                                }
                            }
                        }
                        Label {
                            Layout.alignment: Qt.AlignVCenter
                            Layout.preferredHeight: AppTheme.scaled(16)
                            text: modelData.count
                            color: modelData.byMe
                                   ? AppTheme.reactionSelectedInk
                                   : AppTheme.reactionInk
                            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                            font.weight: AppTheme.weightBold
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    MouseArea {
                        id: reactionMouse
                        enabled: root.rowActionsEnabled
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        // Does not take focus, so a click does not pull the
                        // caret out of the composer. Tab reaches the same
                        // control.
                        onClicked: root.timelineModel.toggleReaction(
                                       root.eventIdForActions(), modelData.key)
                    }
                    Accessible.role: Accessible.Button
                    readonly property string accessibleKey:
                        reactionChip.customEmojiReaction
                            ? reactionChip.customEmojiName : modelData.key
                    Accessible.name: modelData.byMe
                        ? qsTr("Reaction %1, %2, selected").arg(reactionChip.accessibleKey).arg(modelData.count)
                        : qsTr("Reaction %1, %2").arg(reactionChip.accessibleKey).arg(modelData.count)
                    // Same information as the tooltip. Empty when senders are
                    // unknown.
                    Accessible.description: reactionChip.reactorSummary
                    // Makes the control activatable by assistive technology,
                    // mirroring onClicked.
                    Accessible.onPressAction:
                        root.timelineModel.toggleReaction(root.eventIdForActions(), modelData.key)
                }
            }

            // Inline "+" pill closing the reaction row. Reuses the shared
            // picker via openReactionPickerFor(), so it adds no popup per row.
            Rectangle {
                id: reactionAddChip
                objectName: "reactionAddChip"
                // Same geometry contract as the chips: paint changes only.
                visible: !model.redacted
                         && (model.eventId || "").length > 0
                         && model.eventId.indexOf("local:") !== 0
                implicitWidth: AppTheme.scaled(34)
                implicitHeight: Math.max(AppTheme.scaled(22),
                                         addChipGlyph.implicitHeight + 6)
                radius: AppTheme.radiusPill
                color: addChipMouse.pressed
                       ? (AppTheme.dark
                          ? Qt.lighter(AppTheme.reactionBackground, 1.6)
                          : Qt.darker(AppTheme.reactionBackground, 1.14))
                       : addChipHover.hovered
                         ? (AppTheme.dark
                            ? Qt.lighter(AppTheme.reactionBackground, 1.35)
                            : Qt.darker(AppTheme.reactionBackground, 1.07))
                         : "transparent"
                Behavior on color { ColorAnimation { duration: 80 } }
                // Outlined at rest so it reads as "add", not as someone's
                // reaction.
                border.width: 1
                border.color: addChipHover.hovered ? AppTheme.borderStrong
                                                   : AppTheme.border
                activeFocusOnTab: true
                function activate() {
                    if (root.timelineView)
                        root.timelineView.pinnedActionsKey = root.actionKey
                    root.openReactionPickerFor(root.eventIdForActions(),
                                               reactionAddChip)
                }
                Keys.onReturnPressed: (event) => {
                    reactionAddChip.activate(); event.accepted = true
                }
                Keys.onEnterPressed: (event) => {
                    reactionAddChip.activate(); event.accepted = true
                }
                Keys.onSpacePressed: (event) => {
                    reactionAddChip.activate(); event.accepted = true
                }
                Rectangle {
                    anchors.fill: parent
                    visible: reactionAddChip.activeFocus
                    color: "transparent"
                    radius: reactionAddChip.radius
                    border.width: 2
                    border.color: AppTheme.focusRing
                }
                Icon {
                    id: addChipGlyph
                    anchors.centerIn: parent
                    name: "add_reaction"
                    size: AppTheme.scaled(14)
                    color: addChipHover.hovered ? AppTheme.textPrimary
                                                : AppTheme.textMuted
                }
                HoverHandler { id: addChipHover }
                MouseArea {
                    id: addChipMouse
                    enabled: root.rowActionsEnabled
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: reactionAddChip.activate()
                }
                ToolTip.text: qsTr("Add reaction")
                ToolTip.visible: addChipHover.hovered
                ToolTip.delay: 500
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Add reaction")
                Accessible.onPressAction: reactionAddChip.activate()
            }
        }

        // Read-receipt chips: other users whose receipt points at this message
        // (the model excludes only the local user, newest first). Up to 4
        // avatars plus a "+N" chip from the uncapped readReceiptsTotal.
        // Invisible when empty. Thread rows carry no receipts: SDK receipt
        // handling is not thread-aware, so thread timelines keep tracking
        // disabled.
    }

    Item {
        id: readReceiptStrip
        objectName: "readReceiptStrip"
        readonly property var receipts: model.readReceipts || []
        readonly property int maxAvatars: 4
        // Uncapped count; degrades to the list length when the role is absent.
        readonly property int totalOthers:
            model.readReceiptsTotal >= 0 ? model.readReceiptsTotal
                                         : receipts.length
        readonly property int overflowCount:
            Math.max(0, totalOthers - shown.length)

        // Identity-guarded chip payload: `receipts` is a fresh array on every
        // read, so `shown` is reassigned only when the projected content
        // changes.
        property var shown: []
        function refreshShown() {
            var next = []
            var n = Math.min(receipts.length, maxAvatars)
            for (var i = 0; i < n; ++i) {
                var r = receipts[i]
                next.push({ userId: r.userId || "",
                            displayName: r.displayName || "",
                            avatarMxc: r.avatarMxc || "" })
            }
            if (JSON.stringify(next) !== JSON.stringify(shown))
                shown = next
        }
        onReceiptsChanged: refreshShown()
        Component.onCompleted: refreshShown()

        // One line for the tooltip, accessible name and test: at most two
        // names, then the total.
        readonly property string summary: {
            if (totalOthers <= 0 || receipts.length === 0)
                return ""
            var first = receipts[0].displayName || ""
            if (totalOthers === 1)
                return qsTr("Read by %1").arg(first)
            var second = receipts.length > 1
                         ? (receipts[1].displayName || "") : ""
            if (second.length === 0)
                return qsTr("Read by %1 and %2 others")
                    .arg(first).arg(totalOthers - 1)
            if (totalOthers === 2)
                return qsTr("Read by %1 and %2")
                    .arg(first).arg(second)
            if (totalOthers === 3)
                return qsTr("Read by %1, %2 and 1 other")
                    .arg(first).arg(second)
            return qsTr("Read by %1, %2 and %3 others")
                .arg(first).arg(second).arg(totalOthers - 2)
        }
        // Only on a row with a body of its own.
        visible: !model.redacted && receipts.length > 0
                 && root.naturalImplicitHeight > 0
        // Placement is identical for every message, own or not (the
        // one-left-aligned-sender contract; its scan bans the right-align
        // literal in this file). The chip stack rides the full row's right
        // edge: one fixed receipt rail, as in Element.
        x: layout.x
        width: layout.width
        // A zero-height overlay at the bottom of whichever body this row shows;
        // the chips paint upward from it, so receipts never add a tail. Outside
        // `layout` because `layout` is hidden for call-event and room-activity
        // rows, which can carry receipts too.
        y: root.isCallEvent || root.isStateActivity
           ? root.height : layout.y + layout.height
        height: 0
        z: 3

        Row {
            id: receiptRow
            objectName: "readReceiptRow"
            x: Math.max(root.avatarGutterWidth,
                        readReceiptStrip.width - width)
            y: -height
            // Facepile overlap; each avatar sits on an 18px surface ring.
            spacing: -4
            // The ring must match what the row shows, including the fading
            // hover tint.
            readonly property color hoverTint:
                rowHighlight.visible ? rowHighlight.color : "transparent"
            readonly property real hoverTintOpacity:
                rowHighlight.visible ? rowHighlight.opacity : 0
            Repeater {
                // Array model + modelData, like the reaction chips: no
                // document-id lookups from delegate scope. Delegates are
                // rebuilt only when `shown` changes.
                model: readReceiptStrip.shown
                Rectangle {
                    id: chip
                    objectName: "readReceiptChip"
                    width: 18
                    height: 18
                    radius: 9
                    color: AppTheme.background
                    z: index
                    // Composites the row's highlight tint over the ring,
                    // reading only the visual parent chain.
                    Rectangle {
                        anchors.fill: parent
                        radius: chip.radius
                        color: chip.parent ? chip.parent.hoverTint
                                           : "transparent"
                        opacity: chip.parent
                                 ? chip.parent.hoverTintOpacity : 0
                    }
                    Avatar {
                        anchors.centerIn: parent
                        onScreen: root.rowOnScreen
                        size: 16
                        mxc: modelData.avatarMxc
                        name: modelData.displayName
                        colorKey: modelData.userId
                    }
                }
            }
            Rectangle {
                objectName: "readReceiptOverflow"
                visible: readReceiptStrip.overflowCount > 0
                width: Math.max(18, overflowLabel.implicitWidth + 8)
                height: 18
                radius: AppTheme.radiusPill
                color: AppTheme.reactionBackground
                border.color: AppTheme.border
                border.width: 1
                z: readReceiptStrip.maxAvatars
                Label {
                    id: overflowLabel
                    anchors.centerIn: parent
                    text: "+" + readReceiptStrip.overflowCount
                    color: AppTheme.textSecondary
                    // Chrome on a fixed rail: not scaled, but uses the token.
                    font.pixelSize: AppTheme.fontMicro
                    font.weight: AppTheme.weightBold
                }
            }

            TapHandler {
                enabled: root.rowActionsEnabled
                // Click opens the full reader list (up to 16, newest first,
                // plus a truthful "+N more").
                onTapped: (eventPoint) => {
                    if (!root.timelineView
                        || !root.timelineView.openReceiptList)
                        return
                    // eventPoint.position is local to the handler's parent,
                    // receiptRow, so map from there.
                    var p = receiptRow.mapToItem(
                                Overlay.overlay,
                                eventPoint.position.x,
                                eventPoint.position.y)
                    root.timelineView.openReceiptList(
                        model.readReceipts || [],
                        readReceiptStrip.totalOthers, Qt.point(p.x, p.y))
                }
            }
            HoverHandler { id: receiptHover }
            ToolTip.text: readReceiptStrip.summary
            ToolTip.visible: receiptHover.hovered
                             && readReceiptStrip.summary.length > 0
            ToolTip.delay: 500
            // One accessible summary; individual chips are not focus stops.
            Accessible.role: Accessible.StaticText
            Accessible.name: readReceiptStrip.summary
        }
    }

    // The reaction picker and profile popover are shared view-level surfaces.
    // The target event id is captured at open, so a recycled delegate cannot
    // redirect a reaction.
    function openReactionPickerFor(eventId, anchorItem) {
        if (!root.timelineView || !root.timelineView.openReactionPicker
            || eventId === "")
            return
        var p = anchorItem.mapToItem(Overlay.overlay,
                                     anchorItem.width / 2, anchorItem.height)
        root.timelineView.openReactionPicker(eventId, p)
    }

    TextEdit {
        id: clipboardHelper
        visible: false
        width: 0
        height: 0
    }

    // The context menu and details dialog load on first use and then stay,
    // since eager creation on every row was the dominant per-row cost. The
    // Loaders inherit the delegate context.
    readonly property bool moreMenuOpen:
        moreMenuItem ? moreMenuItem.opened : false
    function ensureContextMenu() {
        if (!moreMenuItem)
            moreMenuItem = moreMenuComponent.createObject(root)
        return moreMenuItem
    }

    // Destructive items (Delete, Remove edits, End poll) confirm first: each is
    // irreversible on Matrix. Created lazily, like the details dialog.
    property var confirmDialogItem: null
    function confirmDestructive(title, body, acceptText, action) {
        if (!confirmDialogItem)
            confirmDialogItem = confirmDialogComponent.createObject(root)
        confirmDialogItem.heading = title
        confirmDialogItem.body = body
        confirmDialogItem.acceptText = acceptText
        confirmDialogItem.action = action
        confirmDialogItem.open()
    }
    Component {
        id: confirmDialogComponent
        AppDialog {
            id: destructiveConfirm
            objectName: "messageDestructiveConfirmDialog"
            parent: Overlay.overlay
            anchors.centerIn: parent
            modal: true
            storm: false
            standardButtons: Dialog.NoButton
            // Clicking outside cancels; the only committing path is the
            // destructive button.
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
            width: Math.min(420, parent ? parent.width - 32 : 420)
            property string heading: ""
            property string body: ""
            property string acceptText: ""
            property var action: null
            title: destructiveConfirm.heading
            contentItem: ColumnLayout {
                spacing: AppTheme.spacing12
                Label {
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    text: destructiveConfirm.body
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing8
                    Item { Layout.fillWidth: true }
                    // AppButton so the destructive action uses `dangerPrimary`
                    // and geometry comes from the AppTheme ladder.
                    AppButton {
                        objectName: "messageDestructiveConfirmCancel"
                        kind: "secondary"
                        text: qsTr("Cancel")
                        onClicked: destructiveConfirm.close()
                    }
                    AppButton {
                        objectName: "messageDestructiveConfirmAccept"
                        kind: "dangerPrimary"
                        text: destructiveConfirm.acceptText
                        onClicked: {
                            destructiveConfirm.close()
                            if (destructiveConfirm.action)
                                destructiveConfirm.action()
                        }
                    }
                }
            }
        }
    }

    function openMessageDetails(details) {
        if (!details || !details.eventId)
            return
        if (!detailsDialogItem)
            detailsDialogItem = detailsDialogComponent.createObject(root)
        detailsDialogItem.details = details
        detailsDialogItem.open()
    }
    // Popups are not Items, so a Loader cannot host them; lazily created
    // through a Component parented to the delegate.
    property var moreMenuItem: null
    Component {
        id: moreMenuComponent
        AppMenu {
            id: moreMenu
            objectName: "messageContextMenu"
            menuWidth: AppTheme.menuWidthMessage
            // Mono context header with this row's sender and time.
            contextLabel: qsTr("Message · %1 · %2")
                .arg(model.senderDisplayName || model.sender || "")
                .arg(Qt.formatDateTime(model.timestamp,
                                       app.settings.clockTimeFormat))
            // The menu claims transient row-interaction ownership so no other
            // row shows its toolbar underneath; this row keeps its own (the
            // exception in transientOwnerBlocks), since the menu is positioned
            // from it.
            onOpened: {
                if (root.timelineView
                        && root.timelineView.claimTransientInteraction)
                    root.timelineView.claimTransientInteraction("menu")
            }
            onClosed: {
                root.menuEventId = ""
                if (root.timelineView
                        && root.timelineView.releaseTransientInteraction)
                    root.timelineView.releaseTransientInteraction("menu", "")
                // Hand back the view's open-menu slot.
                if (root.timelineView && ("openRowMenu" in root.timelineView)
                        && root.timelineView.openRowMenu === moreMenu)
                    root.timelineView.openRowMenu = null
            }
            // Single-key accelerators while the menu is open. Keys cannot
            // attach to a Menu, so these are Shortcuts scoped by
            // moreMenu.opened, each calling the same action as its row under
            // the same enabled condition. Edit is E, not ↑ (which would steal
            // menu arrow navigation).
            Shortcut {
                sequence: "R"
                enabled: moreMenu.opened
                context: Qt.ApplicationShortcut
                onActivated: {
                    if (root.timelineModel.messagePermalink(
                            root.menuEventId).length > 0
                        && !root.timelineModel.messageDetails(
                            root.menuEventId).redacted) {
                        root.beginReply(root.menuEventId)
                        moreMenu.close()
                    }
                }
            }
            Shortcut {
                sequence: "T"
                enabled: moreMenu.opened
                context: Qt.ApplicationShortcut
                onActivated: {
                    if (root.timelineModel.messagePermalink(
                            root.menuEventId).length > 0
                        && !root.timelineModel.messageDetails(
                            root.menuEventId).redacted) {
                        var details = root.timelineModel.messageDetails(
                                          root.menuEventId)
                        var rootId = (details.threadRootId || "").length > 0
                                     ? details.threadRootId
                                     : root.menuEventId
                        app.thread.openThread(app.currentRoomId, rootId)
                        moreMenu.close()
                    }
                }
            }
            Shortcut {
                sequence: "E"
                enabled: moreMenu.opened
                context: Qt.ApplicationShortcut
                onActivated: {
                    if (root.timelineModel.canEditEvent(root.menuEventId)) {
                        app.composer.beginEdit(
                            root.menuEventId,
                            root.timelineModel.visibleTextForEvent(
                                root.menuEventId),
                            root.timelineModel.sanitizedHtmlForEvent(
                                root.menuEventId),
                            root.timelineModel.roomId)
                        moreMenu.close()
                    }
                }
            }
            Shortcut {
                sequence: "Ctrl+C"
                enabled: moreMenu.opened
                context: Qt.ApplicationShortcut
                onActivated: {
                    if (root.timelineModel.visibleTextForEvent(
                            root.menuEventId).length > 0) {
                        root.copyToClipboard(
                            root.timelineModel.visibleTextForEvent(
                                root.menuEventId))
                        moreMenu.close()
                    }
                }
            }
            // Quick-react row: the 5 most recently used emoji plus a cell that
            // opens the shared picker.
            QuickReactionStrip {
                objectName: "quickReactionStrip"
                emojis: app.emojiCatalog.recentEmoji || []
                enabled: root.timelineModel.messagePermalink(
                             root.menuEventId).length > 0
                         && !root.timelineModel.messageDetails(
                             root.menuEventId).redacted
                opacity: enabled ? 1.0 : 0.5
                onPicked: (emoji) => {
                    if (root.timelineModel.messagePermalink(
                            root.menuEventId).length === 0
                        || root.timelineModel.messageDetails(
                            root.menuEventId).redacted)
                        return
                    root.timelineModel.toggleReaction(root.menuEventId, emoji)
                    moreMenu.close()
                }
                onMorePressed: {
                    root.openReactionPickerFor(root.menuEventId, bubbleRow)
                    moreMenu.close()
                }
            }
            AppMenuItem {
                iconName: "reply"
                text: qsTr("Reply")
                accel: "R"
                enabled: root.timelineModel.messagePermalink(
                             root.menuEventId).length > 0
                         && !root.timelineModel.messageDetails(
                             root.menuEventId).redacted
                onTriggered: root.beginReply(root.menuEventId)
            }
            AppMenuItem {
                iconName: "forum"
                text: qsTr("Reply in thread")
                accel: "T"
                enabled: root.timelineModel.messagePermalink(
                             root.menuEventId).length > 0
                         && !root.timelineModel.messageDetails(
                             root.menuEventId).redacted
                onTriggered: {
                    var details = root.timelineModel.messageDetails(
                                      root.menuEventId)
                    var rootId = (details.threadRootId || "").length > 0
                                 ? details.threadRootId
                                 : root.menuEventId
                    // Opens the thread panel; its composer sends SDK m.thread
                    // replies.
                    app.thread.openThread(app.currentRoomId,
                                          rootId)
                }
            }
            // From a thread reply, locate the event in the room timeline. Uses
            // app.pagination on purpose: this action is about the room, not the
            // thread.
            AppMenuItem {
                iconName: "arrow_forward"
                text: qsTr("Open in room")
                visible: root.inThreadPanel
                enabled: root.menuEventId !== ""
                onTriggered: app.pagination.jumpToEvent(
                    root.menuEventId)
            }
            AppMenuSeparator {}
            // Only one of pin/unpin is offered: canTogglePin() is false for the
            // inapplicable action, without the real m.room.pinned_events power
            // level, while a write is in flight, and before the first snapshot.
            // Hidden in the thread panel, since pins live in Room Information.
            AppMenuItem {
                iconName: "push_pin"
                text: qsTr("Pin message")
                // `revision` is the dependency for the Q_INVOKABLE. A redacted
                // event cannot be pinned; unpin stays offered so a message
                // deleted after pinning can still be removed.
                visible: !root.inThreadPanel && app.pinned
                         && model.redacted !== true
                         && app.pinned.revision >= 0
                         && app.pinned.canTogglePin(root.menuEventId, true)
                onTriggered: app.pinned.pin(root.menuEventId)
            }
            AppMenuItem {
                iconName: "close"
                text: qsTr("Unpin message")
                visible: !root.inThreadPanel && app.pinned
                         && app.pinned.revision >= 0
                         && app.pinned.canTogglePin(root.menuEventId, false)
                onTriggered: app.pinned.unpin(root.menuEventId)
            }
            AppMenuItem {
                iconName: "content_copy"
                text: qsTr("Copy text")
                accel: "Ctrl+C"
                enabled: root.timelineModel.visibleTextForEvent(
                             root.menuEventId).length > 0
                onTriggered: root.copyToClipboard(
                    root.timelineModel.visibleTextForEvent(root.menuEventId))
            }
            AppMenuItem {
                iconName: "link"
                text: qsTr("Copy message link")
                enabled: root.timelineModel.messagePermalink(
                             root.menuEventId).length > 0
                onTriggered: root.copyToClipboard(
                    root.timelineModel.messagePermalink(root.menuEventId))
            }
            // Open image first: it is what the left click does. No "Copy image
            // address": an authenticated-media mxc URL is not shareable and
            // must not be handed out.
            AppMenuItem {
                objectName: "openMediaMenuItem"
                iconName: "open_in_full"
                text: qsTr("Open image")
                // Images and stickers only; video has its own player and
                // fullscreen overlay.
                visible: (model.isImage === true || model.isSticker === true)
                         && model.mediaSourceAvailable === true
                enabled: visible && root.timelineView
                         && !!root.timelineView.openImage
                onTriggered: root.timelineView.openImage(
                    model.mediaKey || "", model.mediaUrl)
            }
            // Every media row offers Save from this menu. "Add to my stickers":
            // writes this sticker's mxc into the account's
            // im.ponies.user_emotes pack, with no re-upload (a pack holds a
            // plain mxc), deduplicated by mxc and usage ["sticker"]. The
            // shortcode is derived from the sticker's body and sanitized to
            // MSC2545's [a-zA-Z0-9-_] alphabet; collisions get a numeric
            // suffix, a duplicate mxc is refused in Rust, and the
            // read-modify-write reads the server copy so a concurrent edit is
            // not clobbered. Not gated on "already saved", which would need the
            // pack fetched first; a duplicate is refused and reported. An
            // encrypted sticker has no mxc, so canSave() is false and the row
            // is absent.
            AppMenuItem {
                objectName: "saveStickerMenuItem"
                iconName: "star"
                text: qsTr("Add to my stickers")
                visible: model.isSticker === true
                         && (model.mediaMxc || "").length > 0
                         && app.stickers.available
                enabled: visible && app.stickers.canSave(model.mediaMxc || "")
                onTriggered: app.stickers.saveSticker(
                    model.mediaMxc || "", model.body || "",
                    model.mediaMimetype || "",
                    model.mediaWidth || 0, model.mediaHeight || 0,
                    model.mediaSize || 0)
            }
            // The same sticker into the room's own pack. im.ponies.room_emotes
            // is room state, so this is power-level gated in Rust. Offered only
            // when a snapshot for this room reported the permission; unknown
            // means absent, and the personal pack row still works.
            AppMenuItem {
                objectName: "saveStickerToRoomMenuItem"
                iconName: "workspaces"
                text: qsTr("Add to this room's stickers")
                visible: model.isSticker === true
                         && (model.mediaMxc || "").length > 0
                         && app.stickers.available
                         // canSaveToRoom is a plain call; reading `revision`
                         // re-evaluates it when a snapshot lands.
                         && (app.stickers.revision >= 0)
                         && app.stickers.canSaveToRoom(
                                app.currentRoomId, model.mediaMxc || "")
                enabled: visible
                onTriggered: app.stickers.saveStickerToRoom(
                    app.currentRoomId, model.mediaMxc || "",
                    model.body || "", model.mediaMimetype || "",
                    model.mediaWidth || 0, model.mediaHeight || 0,
                    model.mediaSize || 0)
            }
            AppMenuItem {
                objectName: "saveMediaMenuItem"
                iconName: "download"
                text: qsTr("Save as…")
                visible: (model.isImage === true
                          || model.isVideo === true
                          || model.isAudio === true
                          || model.isSticker === true
                          || model.isFile === true)
                         && model.mediaSourceAvailable === true
                         && app.mediaBridge.supported
                         // A gallery's row media is one attachment; saving it
                         // here would skip the rest. Tiles, chips and the
                         // viewer save their own.
                         && !root.isGallery
                enabled: visible && root.menuEventId !== ""
                onTriggered: {
                    if (root.timelineView
                        && root.timelineView.saveMedia)
                        root.timelineView.saveMedia(
                            model.mediaKey || "",
                            model.mediaFilename || "download")
                }
            }
            // A sibling of "Save as…": nested inside it, it would paint over
            // that row. The same local hide as the action bar, reachable by
            // keyboard.
            AppMenuItem {
                objectName: "hideMediaMenuItem"
                iconName: root.mediaHidden ? "visibility" : "visibility_off"
                text: root.mediaHidden ? qsTr("Show image")
                                       : qsTr("Hide image")
                visible: root.mediaHideable
                enabled: visible
                onTriggered: root.setMediaHidden(!root.mediaHidden)
            }

            // Undo for the preview X, in the menu because the link itself is
            // still in the body. Restoring grants nothing: the ordinary policy
            // decides again, so an unconsented link returns as the consent
            // gate.
            AppMenuItem {
                objectName: "restoreLinkPreviewMenuItem"
                iconName: "link"
                text: qsTr("Show link preview")
                visible: root.previewDismissed
                enabled: visible
                onTriggered: app.linkPreviews.restorePreviewForEvent(
                                 root.previewRoomId, root.actionKey)
            }
            AppMenuItem {
                objectName: "copyImageMenuItem"
                iconName: "content_copy"
                text: qsTr("Copy image")
                // Images only, same gates as Save as.
                visible: model.isImage === true
                         && model.mediaSourceAvailable === true
                         && app.mediaBridge.supported
                         && !root.isGallery   // see "Save as…" above
                enabled: visible && root.menuEventId !== ""
                onTriggered: app.copyImageToClipboard(model.mediaKey || "")
            }
            // Separates the copy group from the people/editing group.
            AppMenuSeparator { }
            AppMenuItem {
                iconName: "person"
                text: qsTr("View profile")
                enabled: root.menuEventId !== ""
                         && root.timelineView
                         && !!root.timelineView.openSenderProfile
                onTriggered: root.openSenderProfileForRow()
            }
            AppMenuItem {
                iconName: "info"
                text: qsTr("View details")
                enabled: root.menuEventId !== ""
                onTriggered: root.openMessageDetails(
                    root.timelineModel.messageDetails(
                        root.menuEventId))
            }
            // Edit history for edited messages (the "edited" marker opens it
            // too); View source for every real event, at the bottom of the
            // group.
            AppMenuItem {
                objectName: "editHistoryMenuItem"
                iconName: "schedule"
                text: qsTr("Edit history")
                enabled: model.edited === true && root.menuEventId !== ""
                visible: model.edited === true
                onTriggered: root.openEditHistory(root.menuEventId)
            }
            AppMenuItem {
                objectName: "viewSourceMenuItem"
                iconName: "code"
                text: qsTr("View source")
                enabled: root.menuEventId !== ""
                         && !!root.timelineModel.requestEventSource
                onTriggered: root.openEventSource(root.menuEventId)
            }
            AppMenuItem {
                iconName: "edit_square"
                text: qsTr("Edit")
                accel: "E"
                enabled: root.timelineModel.canEditEvent(root.menuEventId)
                visible: enabled
                onTriggered: app.composer.beginEdit(
                    root.menuEventId,
                    root.timelineModel.visibleTextForEvent(root.menuEventId),
                    root.timelineModel.sanitizedHtmlForEvent(root.menuEventId),
                    root.timelineModel.roomId)
            }
            // Matrix has no unedit: edits are separate m.replace events, so
            // this redacts them and the message returns to its original text.
            // Own, edited, editable messages only, on a backend that can reach
            // the relations.
            AppMenuItem {
                objectName: "removeEditsMenuItem"
                iconName: "undo"
                text: qsTr("Remove edits")
                enabled: model.edited === true
                         && root.timelineModel.canEditEvent(root.menuEventId)
                         && app.composer.canRemoveEdits()
                visible: enabled
                onTriggered: {
                    var id = root.menuEventId
                    root.confirmDestructive(
                        qsTr("Remove edits?"),
                        qsTr("Matrix has no unedit: this redacts the edits, "
                             + "so the message returns to its original text. "
                             + "It cannot be undone."),
                        qsTr("Remove edits"),
                        function() { app.composer.removeEdits(id) })
                }
            }
            // Own running polls only; servers and clients enforce MSC3381.
            AppMenuItem {
                objectName: "endPollMenuItem"
                iconName: "check_circle"
                text: qsTr("End poll")
                visible: model.isPoll === true
                         && model.canEndPoll === true
                enabled: visible && root.menuEventId !== ""
                onTriggered: {
                    var id = root.menuEventId
                    var rootId = root.inThreadPanel
                                 ? (app.thread.rootEventId || "") : ""
                    root.confirmDestructive(
                        qsTr("End poll?"),
                        qsTr("This closes voting and publishes the result. "
                             + "A poll cannot be reopened."),
                        qsTr("End poll"),
                        function() { app.composer.endPoll(id, rootId) })
                }
            }
            AppMenuSeparator {
                visible: root.timelineModel.canRedactEvent(
                             root.menuEventId)
                         || (app.moderation.reportSupported
                             && model.isOwn !== true)
            }
            // Forwarding is withheld for redacted, local-echo, undecryptable
            // and poll content, and for media whose source is not fetchable.
            // begin() takes an immutable snapshot of this row's data at click
            // time (see ForwardController), so it is built inline.
            // Multi-message forwarding starts here.
            AppMenuItem {
                objectName: "selectMessagesMenuItem"
                iconName: "check"
                text: qsTr("Select messages")
                visible: model.isVirtual !== true && root.menuEventId !== ""
                enabled: visible
                onTriggered: {
                    app.forward.beginSelecting(
                        root.timelineModel.realRoomIdForEvent(root.menuEventId))
                    root.toggleSelectionForThisRow()
                }
            }
            AppMenuItem {
                objectName: "forwardMessageMenuItem"
                iconName: "arrow_forward"
                text: qsTr("Forward")
                readonly property bool isMediaRow:
                    model.isImage === true || model.isVideo === true
                    || model.isAudio === true || model.isSticker === true
                    || model.isFile === true
                readonly property bool eligible:
                    model.redacted !== true && model.isLocalEcho !== true
                    && model.undecryptable !== true
                    && model.isVirtual !== true && model.isPoll !== true
                    && (isMediaRow ? model.mediaSourceAvailable === true
                                   : (model.body || "").length > 0)
                    // Forwarding sends the row's one media key, which on a
                    // gallery is its first attachment; not offered until a
                    // whole gallery can be forwarded.
                    && !root.isGallery
                enabled: eligible && root.menuEventId !== ""
                visible: eligible
                onTriggered: app.forward.begin(
                    root.timelineModel.realRoomIdForEvent(root.menuEventId),
                    root.menuEventId,
                    {
                        redacted: model.redacted === true,
                        isLocalEcho: model.isLocalEcho === true,
                        undecryptable: model.undecryptable === true,
                        isVirtual: model.isVirtual === true,
                        isImage: model.isImage === true,
                        isVideo: model.isVideo === true,
                        isAudio: model.isAudio === true,
                        isSticker: model.isSticker === true,
                        isFile: model.isFile === true,
                        mediaIsVoice: model.mediaIsVoice === true,
                        senderDisplayName: model.senderDisplayName || "",
                        body: model.body || "",
                        mediaKey: model.mediaKey || "",
                        mediaFilename: model.mediaFilename || "",
                        mediaMimetype: model.mediaMimetype || "",
                        mediaWidth: model.mediaWidth || 0,
                        mediaHeight: model.mediaHeight || 0
                    })
            }
            // Report to the homeserver admin (/v3 event report). Not offered on
            // own messages.
            AppMenuItem {
                iconName: "flag"
                text: qsTr("Report message")
                danger: true
                enabled: app.moderation.reportSupported
                         && root.menuEventId !== ""
                         && model.isOwn !== true
                visible: app.moderation.reportSupported
                         && model.isOwn !== true
                onTriggered: app.moderation.beginReport(
                    root.timelineModel.realRoomIdForEvent(root.menuEventId),
                    root.menuEventId)
            }
            AppMenuItem {
                iconName: "delete"
                text: qsTr("Delete")
                danger: true
                enabled: root.timelineModel.canRedactEvent(root.menuEventId)
                visible: enabled
                onTriggered: {
                    var id = root.menuEventId
                    root.confirmDestructive(
                        qsTr("Delete message?"),
                        qsTr("This removes the message for everyone in the "
                             + "room. It cannot be undone."),
                        qsTr("Delete"),
                        function() { root.timelineModel.redactEvent(id) })
                }
            }
        }
    }
    // Created lazily like the details dialog; Dialogs own their overlay
    // lifetime.
    property var editHistoryDialogItem: null
    property var eventSourceDialogItem: null
    Component {
        id: editHistoryDialogComponent
        EditHistoryDialog {}
    }
    Component {
        id: eventSourceDialogComponent
        EventSourceDialog {}
    }
    function openEditHistory(eventId) {
        if (!eventId || !root.timelineModel)
            return
        if (!editHistoryDialogItem)
            editHistoryDialogItem = editHistoryDialogComponent.createObject(root)
        editHistoryDialogItem.openFor(root.timelineModel, eventId)
    }
    function openEventSource(eventId) {
        if (!eventId || !root.timelineModel)
            return
        if (!eventSourceDialogItem)
            eventSourceDialogItem = eventSourceDialogComponent.createObject(root)
        eventSourceDialogItem.openFor(root.timelineModel, eventId)
    }
    property var detailsDialogItem: null
    Component {
        id: detailsDialogComponent
        AppDialog {
                id: messageDetailsDialog
                objectName: "messageDetailsDialog"
                parent: Overlay.overlay
                anchors.centerIn: parent
                modal: true
                title: qsTr("Message details")
                standardButtons: Dialog.Ok
                // General namespace surface, matching the body Labels' general
                // inks.
                storm: false
                property var details: ({})
                width: Math.min(520, parent ? parent.width - 32 : 520)
                contentItem: ColumnLayout {
                    spacing: AppTheme.spacingS
                    Repeater {
                        model: [
                            [qsTr("Sender"), messageDetailsDialog.details.senderName || ""],
                            [qsTr("Sender ID"), messageDetailsDialog.details.senderId || ""],
                            [qsTr("Timestamp"), messageDetailsDialog.details.timestamp || ""],
                            [qsTr("Room ID"), messageDetailsDialog.details.roomId || ""],
                            [qsTr("Event ID"), messageDetailsDialog.details.eventId || ""],
                            [qsTr("Type"), messageDetailsDialog.details.eventType || ""],
                            [qsTr("Delivery"), messageDetailsDialog.details.delivery || ""],
                            [qsTr("Encryption"), messageDetailsDialog.details.encryption || ""],
                            [qsTr("Decryption"), messageDetailsDialog.details.decryption || ""],
                            [qsTr("Edited"), messageDetailsDialog.details.edited ? qsTr("Yes") : qsTr("No")],
                            [qsTr("Redacted"), messageDetailsDialog.details.redacted ? qsTr("Yes") : qsTr("No")],
                            [qsTr("Reply target"), messageDetailsDialog.details.replyTargetId || ""]
                        ]
                        RowLayout {
                            visible: modelData[1] !== ""
                            Layout.fillWidth: true
                            Label {
                                text: modelData[0]
                                color: AppTheme.textMuted
                                Layout.preferredWidth: 110
                            }
                            Label {
                                text: modelData[1]
                                color: AppTheme.text
                                wrapMode: Text.WrapAnywhere
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }
    }


    Connections {
        target: app
        function onCurrentRoomIdChanged() {
            if (root.moreMenuItem) root.moreMenuItem.close()
            if (root.detailsDialogItem) root.detailsDialogItem.close()
            root.menuEventId = ""
        }
    }

    // Local echoes carry "local:*" ids. Redacting one fails server-side and the
    // error surfaces through the status bar.
    function eventIdForActions() { return model.eventId }

    // ---- link preview card ----

    // A validated direct raster response is media, not article metadata, and
    // gets its own compact renderer.
    Component {
        id: directMediaPreviewComponent
        Rectangle {
            id: directMedia
            objectName: "directMediaPreview"
            readonly property var p: root.preview
            readonly property real naturalWidth: p.imageWidth > 0
                                                  ? p.imageWidth : 0
            readonly property real naturalHeight: p.imageHeight > 0
                                                   ? p.imageHeight : 0
            readonly property real aspectRatio:
                naturalWidth > 0 && naturalHeight > 0
                ? naturalHeight / naturalWidth : 0.75
            readonly property real maxWidth:
                Math.min(360, root.contentInnerCap)
            readonly property real maxHeight: 300
            readonly property real displayWidth: {
                var widthHint = naturalWidth > 0
                                ? Math.min(Math.max(160, naturalWidth), maxWidth)
                                : maxWidth
                if (widthHint * aspectRatio > maxHeight)
                    widthHint = maxHeight / aspectRatio
                return Math.max(1, Math.min(widthHint, maxWidth))
            }
            readonly property string animatedSource:
                p.isGif === true && app.settings.gifAutoplay !== 2
                ? app.mediaBridge.previewAnimatedSource(p.imageSource || "",
                                                        p.imageMime || "") : ""
            // A server-route preview (/preview_url) delivers og:image as an mxc
            // URI; previewImageSource only accepts inline data:, so an mxc goes
            // through the authenticated media route and the tick re-reads when
            // it lands.
            property int previewTick: 0
            Connections {
                target: app.mediaBridge
                function onMediaCached(cacheKey) { previewTick += 1 }
            }
            readonly property string staticSource: {
                var _ = previewTick
                var src = p.imageSource || ""
                if (src.length === 0)
                    return ""
                if (src.indexOf("mxc://") === 0)
                    return app.mediaBridge.mxcImageSource(src, 512)
                return app.mediaBridge.previewImageSource(src, p.imageMime || "")
            }

            implicitWidth: displayWidth
            implicitHeight: Math.max(1, displayWidth * aspectRatio)
            color: AppTheme.embedSurface
            radius: AppTheme.radiusSm
            clip: true

            Image {
                anchors.fill: parent
                visible: directMedia.animatedSource.length === 0
                source: directMedia.staticSource
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: true
            }
            AnimatedImage {
                anchors.fill: parent
                visible: directMedia.animatedSource.length > 0
                source: visible ? directMedia.animatedSource : ""
                fillMode: Image.PreserveAspectFit
                playing: visible
                asynchronous: true
                cache: false
            }
            Rectangle {
                visible: directMedia.p.isGif === true
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 5
                radius: 3
                color: AppTheme.overlayScrim
                width: directGifLabel.implicitWidth + 8
                height: directGifLabel.implicitHeight + 4
                Label {
                    id: directGifLabel
                    anchors.centerIn: parent
                    text: "GIF"
                    color: AppTheme.scrimInk
                    font.pixelSize: AppTheme.fontMicro
                    font.weight: Font.Bold
                }
            }
            MouseArea {
                anchors.fill: parent
                enabled: (directMedia.p.url || "").length > 0
                             && root.rowActionsEnabled
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: app.media.openWebUrl(directMedia.p.url)
            }
            // Same dismissal as the ordinary card; only built for a loaded
            // preview.
            IconButton {
                id: directDismissButton
                objectName: "linkPreviewDismissButton"
                z: 3
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: 2
                size: "sm"
                iconName: "close"
                opacity: directMediaHover.hovered || directDismissButton.hovered
                         || directDismissButton.activeFocus ? 1.0 : 0.5
                Accessible.name: qsTr("Dismiss link preview")
                ToolTip.text: qsTr("Dismiss preview")
                ToolTip.visible: directDismissButton.hovered
                ToolTip.delay: 400
                onClicked: app.linkPreviews.dismissPreviewForEvent(
                               root.previewRoomId, root.actionKey)
            }
            HoverHandler { id: directMediaHover }
        }
    }

    Component {
        id: linkPreviewComponent
        Rectangle {
            id: card
            objectName: "linkPreviewCard"
            readonly property var p: root.preview
            readonly property string st: p.state || "none"
            // Only a preview actually on screen can be dismissed; not the
            // consent gate.
            readonly property bool dismissible: st === "loaded" || st === "failed"
            readonly property string previewAnimation:
                p.isGif === true && app.settings.gifAutoplay !== 2
                ? app.mediaBridge.previewAnimatedSource(p.imageSource || "",
                                                        p.imageMime || "") : ""
            // Same mxc-or-data split as the full card.
            property int previewTick: 0
            Connections {
                target: app.mediaBridge
                function onMediaCached(cacheKey) { previewTick += 1 }
            }
            readonly property string previewStatic: {
                var _ = previewTick
                var src = p.imageSource || ""
                if (src.length === 0)
                    return ""
                if (src.indexOf("mxc://") === 0)
                    return app.mediaBridge.mxcImageSource(src, 512)
                return app.mediaBridge.previewImageSource(src, p.imageMime || "")
            }
            readonly property real fullW:
                Math.min(400, root.contentInnerCap)
            // The consent gate sizes to its content; other states fill the
            // column. Safe because nothing under it reads the width this layout
            // computes (QQuickText reports its unwrapped natural width
            // regardless of wrapMode).
            implicitWidth: st === "requires_action"
                           ? Math.min(fullW,
                                      cardCol.implicitWidth
                                      + AppTheme.spacingM + AppTheme.spacingS)
                           : fullW
            // Gate/loading/failed keep a monotonic reserved height so a failure
            // never reflows the row; only the loaded preview re-measures. Per
            // event, so a reused delegate does not inherit it. The gate is
            // shorter than the loading skeleton, so the consent click grows the
            // row once.
            property real reservedH: 0
            readonly property string _rowIdentity: root.actionKey
            on_RowIdentityChanged: reservedH = 0
            readonly property real naturalH:
                cardCol.implicitHeight + AppTheme.spacingS * 2
            onNaturalHChanged: {
                if (st !== "loaded")
                    reservedH = Math.max(reservedH, naturalH)
            }
            implicitHeight: st === "loaded" ? naturalH
                                            : Math.max(naturalH, reservedH)
            color: cardHover.hovered && card.st === "loaded"
                   ? AppTheme.hover : AppTheme.embedSurface
            radius: AppTheme.radiusMd
            border.color: AppTheme.border
            border.width: 1

            HoverHandler { id: cardHover }

            // Whole card opens the URL (loaded state only).
            MouseArea {
                anchors.fill: parent
                enabled: card.st === "loaded"
                         && (card.p.url || "").length > 0
                         && root.rowActionsEnabled
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: app.media.openWebUrl(card.p.url)
            }

            // Dismiss, offered only once a preview has been shown (loaded or
            // failed). Absent from the consent gate, where only the Show button
            // may consent (LinkPreviewQmlTest::onlyTheButtonConsents).
            // Overlaid, so it costs no height; the column's right margin widens
            // instead.
            IconButton {
                id: previewDismissButton
                objectName: "linkPreviewDismissButton"
                visible: card.dismissible
                enabled: visible
                z: 3
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: 2
                size: "sm"
                iconName: "close"
                opacity: cardHover.hovered || previewDismissButton.hovered
                         || previewDismissButton.activeFocus ? 1.0 : 0.5
                Accessible.name: qsTr("Dismiss link preview")
                ToolTip.text: qsTr("Dismiss preview")
                ToolTip.visible: previewDismissButton.hovered
                ToolTip.delay: 400
                onClicked: app.linkPreviews.dismissPreviewForEvent(
                               root.previewRoomId, root.actionKey)
            }

            ColumnLayout {
                id: cardCol
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: AppTheme.spacingS
                anchors.bottomMargin: AppTheme.spacingS
                anchors.leftMargin: AppTheme.spacingM
                // Room for the overlaid X.
                anchors.rightMargin: card.dismissible
                                     ? AppTheme.spacingS + 24
                                     : AppTheme.spacingS
                spacing: 4

                // Consent gate (encrypted rooms, or auto-load off), one compact
                // row. It must still state that the site is contacted directly
                // and learns your IP; the full sentence is the tooltip. The
                // button is the consent: the row is not clickable and the
                // whole-card MouseArea is gated on "loaded", so hovering to
                // read the notice can never agree to the fetch.
                RowLayout {
                    id: consentRow
                    objectName: "linkPreviewConsentRow"
                    visible: card.st === "requires_action"
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing6

                    // Attached tooltip: one shared instance. States the order
                    // because the order is the privacy property: the homeserver
                    // fetches the page and the thumbnail comes back as an mxc
                    // on the authenticated media path. The second sentence is
                    // the honest half: a server with previews disabled
                    // (Synapse's default) cannot do this, and Lightning then
                    // loads directly, revealing your address.
                    readonly property string fullPrivacyText:
                        qsTr("Your homeserver loads this preview, so the linked site does not see your IP address. If your server cannot, Lightning loads it directly and the site does see your IP.")
                    ToolTip.text: consentRow.fullPrivacyText
                    ToolTip.visible: consentHover.hovered
                    ToolTip.delay: 400
                    HoverHandler { id: consentHover }

                    Icon {
                        name: "link"
                        size: 14
                        color: AppTheme.textMuted
                        Layout.alignment: Qt.AlignVCenter
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0

                        Label {
                            objectName: "linkPreviewConsentHost"
                            text: card.p.host || ""
                            color: AppTheme.link
                            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                            font.weight: AppTheme.weightStrong
                            elide: Label.ElideRight
                            Layout.fillWidth: true
                        }
                        Label {
                            objectName: "linkPreviewConsentNotice"
                            // Short, but names both facts (who fetches, and
                            // that the direct fallback costs your IP). Wraps
                            // rather than elides so the tail is never lost. The
                            // encrypted variant stays stricter because asking
                            // the homeserver to preview reveals a URL the
                            // encryption was hiding. Must stay narrower than
                            // the loaded card
                            // (consentGateIsNarrowerThanTheStateItLeadsTo); the
                            // full text is the tooltip.
                            text: root.roomEncrypted
                                  ? qsTr("Your server sees this URL — or directly, your IP")
                                  : qsTr("Your server loads it — or directly, your IP")
                            color: root.roomEncrypted ? AppTheme.warning
                                                      : AppTheme.textMuted
                            font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                            wrapMode: Text.WordWrap
                            // Explicit so no style can truncate the notice.
                            // Pinned by LinkPreviewQmlTest.
                            elide: Label.ElideNone
                            Layout.fillWidth: true
                        }
                    }

                    AppButton {
                        objectName: "linkPreviewLoadButton"
                        // Short label; the accessible name keeps the long form.
                        text: qsTr("Show")
                        size: "sm"
                        minWidth: 0
                        Accessible.name: qsTr("Show link preview")
                        Layout.alignment: Qt.AlignVCenter
                        onClicked: app.linkPreviews.requestPreviewForEvent(
                                       root.previewRoomId, root.actionKey)
                    }
                }

                // Loading: region skeletons at a stable card height, replaced
                // in place as fields arrive.
                ColumnLayout {
                    objectName: "linkPreviewSkeleton"
                    visible: card.st === "loading"
                    Layout.fillWidth: true
                    spacing: 5
                    Skeleton {
                        active: root.rowOnScreen && card.st === "loading"
                        Layout.preferredWidth: Math.min(240, card.width * 0.6)
                        Layout.preferredHeight: 12
                    }
                    Skeleton {
                        active: root.rowOnScreen && card.st === "loading"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 10
                    }
                    Skeleton {
                        active: root.rowOnScreen && card.st === "loading"
                        Layout.preferredWidth: Math.min(300, card.width * 0.8)
                        Layout.preferredHeight: 10
                    }
                    Skeleton {
                        active: root.rowOnScreen && card.st === "loading"
                        Layout.preferredWidth: Math.min(140, card.width * 0.35)
                        Layout.preferredHeight: 9
                    }
                }

                // Failed.
                ColumnLayout {
                    visible: card.st === "failed"
                    Layout.fillWidth: true
                    spacing: 4
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing6
                        Icon {
                            name: "error"
                            size: 15
                            color: AppTheme.textMuted
                        }
                        Label {
                            text: qsTr("Preview unavailable")
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                            Layout.fillWidth: true
                        }
                    }
                    AppButton {
                        objectName: "linkPreviewRetryButton"
                        visible: card.p.retryable === true
                        text: qsTr("Retry")
                        onClicked: app.linkPreviews.retryForEvent(
                                       root.previewRoomId, root.actionKey)
                    }
                }

                // Loaded.
                ColumnLayout {
                    visible: card.st === "loaded"
                    Layout.fillWidth: true
                    spacing: 3

                    // Thumbnail bytes were fetched and validated by Rust.
                    Rectangle {
                        visible: ((card.p.imageMxc || "").length > 0
                                  || (card.p.imageSource || "").length > 0)
                                 && !(card.p.gifOversized === true)
                        Layout.fillWidth: true
                        Layout.preferredHeight: visible ? Math.min(180,
                            (card.p.imageHeight > 0 && card.p.imageWidth > 0)
                            ? width * (card.p.imageHeight / card.p.imageWidth)
                            : 140) : 0
                        color: AppTheme.embedSurface
                        radius: AppTheme.radiusSm
                        clip: true
                        Image {
                            id: thumb
                            anchors.fill: parent
                            visible: card.previewAnimation.length === 0
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            cache: true
                            // Re-resolve through a counter; assigning `source`
                            // would destroy the binding.
                            property int resolveTick: 0
                            source: {
                                var _tick = resolveTick
                                return card.previewStatic.length > 0
                                    ? card.previewStatic
                                    : (card.p.imageMxc || "").length > 0
                                    && app.mediaBridge.supported
                                    ? app.mediaBridge.mxcImageSource(card.p.imageMxc, 480)
                                    : ""
                            }
                            Connections {
                                target: app.mediaBridge
                                enabled: (card.p.imageMxc || "").length > 0
                                function onMediaCached(cacheKey) {
                                    if (cacheKey.endsWith(":" + card.p.imageMxc))
                                        thumb.resolveTick++
                                }
                            }
                        }
                        AnimatedImage {
                            anchors.fill: parent
                            visible: card.previewAnimation.length > 0
                            source: visible ? card.previewAnimation : ""
                            fillMode: Image.PreserveAspectFit
                            playing: visible
                            asynchronous: true
                            cache: false
                        }
                        // GIF badge.
                        Rectangle {
                            visible: card.p.isGif === true
                            anchors.left: parent.left
                            anchors.bottom: parent.bottom
                            anchors.margins: 4
                            radius: 3
                            color: AppTheme.overlayScrim
                            width: gifLabel.implicitWidth + 8
                            height: gifLabel.implicitHeight + 4
                            Label {
                                id: gifLabel
                                anchors.centerIn: parent
                                text: "GIF"
                                color: AppTheme.scrimInk
                                font.pixelSize: AppTheme.fontMicro
                                font.weight: Font.Bold
                            }
                        }
                    }

                    Label {
                        visible: card.p.isDirectMedia !== true
                                 && (card.p.siteName || "").length > 0
                        text: card.p.siteName || ""
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                        elide: Label.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        visible: card.p.isDirectMedia !== true
                                 && (card.p.title || "").length > 0
                        text: card.p.title || ""
                        color: AppTheme.text
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        font.weight: AppTheme.weightStrong
                        wrapMode: Text.WordWrap
                        maximumLineCount: 2
                        elide: Label.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        visible: card.p.isDirectMedia !== true
                                 && (card.p.description || "").length > 0
                        text: card.p.description || ""
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        // Same leading as the message body.
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                        wrapMode: Text.WordWrap
                        maximumLineCount: 3
                        elide: Label.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        text: card.p.host || ""
                        color: AppTheme.link
                        font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                        elide: Label.ElideRight
                        Layout.fillWidth: true
                    }
                }
            }
        }
    }

    // ---- media sub-components ----

    // MSC4274 gallery: several attachments in one event (what Sable sends for
    // multiple pictures). Pictures and videos are square tiles; audio and other
    // files are chips below. Each tile fetches by its own key under the same
    // gates as a single picture: media band, hidden media, collapsed embeds.
    // Tile geometry is fixed before bytes arrive.
    Component {
        id: galleryComponent
        Item {
            id: galleryBox
            objectName: "messageGallery"
            readonly property var items: root.galleryItems
            readonly property var visualItems: items.filter(function(it) {
                return it.kind === "image" || it.kind === "video"
            })
            readonly property var fileItems: items.filter(function(it) {
                return it.kind !== "image" && it.kind !== "video"
            })
            readonly property real gap: 4
            readonly property int columns: visualItems.length <= 1 ? 1
                : (visualItems.length === 2 || visualItems.length === 4) ? 2 : 3
            readonly property real gridMax: Math.min(420, root.contentInnerCap)
            readonly property real tile: Math.max(48, Math.floor(
                (gridMax - gap * (columns - 1)) / columns))
            implicitWidth: visualItems.length > 0
                           ? Math.max(columns * tile + (columns - 1) * gap,
                                      fileItems.length > 0
                                      ? Math.min(340, root.contentInnerCap) : 0)
                           : Math.min(340, root.contentInnerCap)
            implicitHeight: galleryColumn.implicitHeight

            Column {
                id: galleryColumn
                width: parent.width
                spacing: galleryBox.gap

                Grid {
                    objectName: "messageGalleryGrid"
                    visible: galleryBox.visualItems.length > 0
                    columns: galleryBox.columns
                    spacing: galleryBox.gap
                    Repeater {
                        model: galleryBox.visualItems
                        delegate: Rectangle {
                            id: galleryTile
                            objectName: "messageGalleryTile"
                            required property var modelData
                            width: galleryBox.tile
                            height: galleryBox.tile
                            color: AppTheme.embedSurface
                            border.color: AppTheme.border
                            border.width: 1
                            readonly property string key: modelData.mediaKey || ""
                            readonly property bool isVideo: modelData.kind === "video"
                            // A video tile draws only a real poster; without a
                            // server thumbnail the bridge would fall back to
                            // the video payload itself.
                            readonly property bool drawable:
                                key.length > 0
                                && (!isVideo || modelData.thumbAvailable === true)
                            readonly property string fetchKind:
                                modelData.thumbAvailable === true ? "thumb" : "full"
                            // Also asks the store: this first runs before
                            // root's onCompleted has read it, and a hidden row
                            // must not fetch in that window.
                            readonly property bool hiddenNow:
                                root.mediaHidden
                                || (!!app.mediaVisibility
                                    && root.mediaVisibilityKey.length > 0
                                    && app.mediaVisibility.isHidden(
                                           root.mediaVisibilityKey))
                            readonly property bool wanted:
                                drawable && app.mediaBridge.supported
                                && root.mediaInBand && !hiddenNow
                            // Bumped on cache fill so the binding re-asks
                            // without assigning `source`.
                            property int resolveTick: 0
                            property bool failed: false
                            readonly property string bridgeSource: {
                                var _tick = resolveTick
                                return wanted
                                    ? app.mediaBridge.mediaSource(key, fetchKind)
                                    : ""
                            }
                            Accessible.role: Accessible.Button
                            Accessible.name: (modelData.filename || "").length > 0
                                             ? modelData.filename
                                             : (isVideo ? qsTr("Video") : qsTr("Image"))

                            Image {
                                anchors.fill: parent
                                anchors.margins: 1
                                visible: !root.mediaHidden
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                cache: true
                                sourceSize.width: Math.round(galleryBox.tile * 2)
                                source: galleryTile.bridgeSource
                            }
                            Icon {
                                anchors.centerIn: parent
                                visible: !root.mediaHidden
                                         && (galleryTile.isVideo
                                             || galleryTile.failed
                                             || !galleryTile.drawable)
                                name: galleryTile.failed ? "refresh"
                                      : galleryTile.isVideo ? "play_arrow"
                                      : "image"
                                size: 28
                                color: AppTheme.textMuted
                            }
                            Connections {
                                target: app.mediaBridge
                                enabled: galleryTile.wanted
                                function onMediaCached(cacheKey) {
                                    if (cacheKey === galleryTile.fetchKind + ":"
                                            + galleryTile.key) {
                                        galleryTile.failed = false
                                        galleryTile.resolveTick++
                                    }
                                }
                                function onMediaFetchFailed(cacheKey, category) {
                                    if (cacheKey === galleryTile.fetchKind + ":"
                                            + galleryTile.key)
                                        galleryTile.failed = true
                                }
                                function onMediaRetryable(cacheKey) {
                                    // Same recovery channel as the other media
                                    // boxes: a swept transient mark re-asks.
                                    // Bounded by the bridge.
                                    if (cacheKey === galleryTile.fetchKind + ":"
                                            + galleryTile.key) {
                                        galleryTile.failed = false
                                        galleryTile.resolveTick++
                                    }
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                enabled: !root.mediaHidden && root.rowActionsEnabled
                                visible: enabled
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if (galleryTile.failed) {
                                        app.mediaBridge.retry(galleryTile.fetchKind
                                                              + ":" + galleryTile.key)
                                        galleryTile.failed = false
                                        galleryTile.resolveTick++
                                        return
                                    }
                                    // A picture opens the viewer, which pages
                                    // through the room's pictures
                                    // (TimelineModel::mediaEntries); in the
                                    // thread panel it shows this picture alone.
                                    // Videos are saved like other attachments.
                                    if (!galleryTile.isVideo) {
                                        if (root.timelineView && root.timelineView.openImage)
                                            root.timelineView.openImage(galleryTile.key, "")
                                    } else if (root.timelineView
                                               && root.timelineView.saveMedia) {
                                        root.timelineView.saveMedia(
                                            galleryTile.key,
                                            galleryTile.modelData.filename || "")
                                    }
                                }
                            }
                        }
                    }
                }

                Repeater {
                    model: galleryBox.fileItems
                    delegate: Rectangle {
                        id: galleryFile
                        objectName: "messageGalleryFile"
                        required property var modelData
                        width: galleryBox.width
                        height: galleryFileRow.implicitHeight + 12
                        radius: AppTheme.radiusMd
                        color: AppTheme.embedSurface
                        border.color: AppTheme.border
                        border.width: 1
                        RowLayout {
                            id: galleryFileRow
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 8
                            anchors.rightMargin: 4
                            spacing: 8
                            Icon {
                                name: galleryFile.modelData.kind === "audio"
                                      ? "graphic_eq" : "attach_file"
                                size: 18
                                color: AppTheme.textMuted
                            }
                            Label {
                                text: galleryFile.modelData.filename || qsTr("File")
                                textFormat: Text.PlainText
                                color: AppTheme.text
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                                elide: Label.ElideMiddle
                                Layout.fillWidth: true
                            }
                            Label {
                                text: root.embedSizeText(galleryFile.modelData.size || 0)
                                visible: text.length > 0
                                color: AppTheme.textMuted
                                font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                            }
                            IconButton {
                                visible: app.mediaBridge.supported
                                         && (galleryFile.modelData.mediaKey || "").length > 0
                                         && !!root.timelineView
                                         && !!root.timelineView.saveMedia
                                iconName: "download"
                                iconSize: 18
                                implicitWidth: 30; implicitHeight: 30
                                Accessible.name: qsTr("Save %1 as…")
                                    .arg(galleryFile.modelData.filename || qsTr("file"))
                                onClicked: root.timelineView.saveMedia(
                                    galleryFile.modelData.mediaKey,
                                    galleryFile.modelData.filename || "")
                            }
                        }
                    }
                }
            }

            MediaHiddenPlaceholder {
                anchors.fill: parent
                hidden: root.mediaHidden
                onRevealRequested: root.setMediaHidden(false)
            }
        }
    }

    Component {
        id: imageComponent
        Item {
            id: imageBox
            // Named like its siblings (stickerMedia, videoMedia, audioMedia,
            // fileCard) so a suite can check the picture was built, not merely
            // visible.
            objectName: "imageMedia"

            // Responsive sizing from the intrinsic dimensions: never
            // avatar-sized, never overflowing the column, never upscaling. The
            // implicit size flows up so the row grows to the picture.
            readonly property real maxW:
                Math.min(360, root.contentInnerCap)
            readonly property real maxH: 320
            readonly property real natW: model.mediaWidth > 0 ? model.mediaWidth : 0
            readonly property real natH: model.mediaHeight > 0 ? model.mediaHeight : 0
            readonly property real ratio: (natW > 0 && natH > 0)
                                          ? (natH / natW) : 0.66
            // Never upscale known dimensions; unknown ones use the responsive
            // bound until metadata arrives.
            readonly property real dispW: {
                var w = natW > 0 ? Math.min(natW, maxW) : maxW
                if (w * ratio > maxH) w = maxH / ratio
                return Math.max(1, Math.min(w, maxW))
            }
            readonly property real dispH: Math.max(1, dispW * ratio)

            implicitWidth: dispW
            implicitHeight: dispH

            readonly property bool isGif:
                (model.mediaMimetype || "").toLowerCase() === "image/gif"
            // The hover star applies to any of the four raster formats
            // Lightning can validate and store (see gif::validateRasterBytes);
            // isGif still drives animation.
            readonly property bool isRasterImage: {
                var m = (model.mediaMimetype || "").toLowerCase()
                return m === "image/gif" || m === "image/png"
                    || m === "image/jpeg" || m === "image/webp"
            }
            readonly property bool pendingMedia:
                (model.eventId || "").startsWith("local:")
                || (model.mediaUrl ? model.mediaUrl.toString()
                        .indexOf("send-queue.localhost") >= 0 : false)
            property string animatedSource: ""
            // Autoplay: 0 Always, 1 OnHover, 2 Never.
            readonly property int gifMode: app.settings.gifAutoplay
            property bool gifHovered: false
            HoverHandler {
                enabled: imageBox.isGif && imageBox.gifMode === 1
                onHoveredChanged: imageBox.gifHovered = hovered
            }
            readonly property bool animateGif:
                isGif && gifMode !== 2
                && (gifMode === 0 || gifHovered)
                && !pendingMedia && animatedSource.length > 0
                // A hidden animation must stop decoding.
                && !root.mediaHidden

            // Prefer the media bridge (works in encrypted rooms; Rust
            // decrypts); HTTP URLs are the fallback. An empty bridgeSource
            // means a fetch is in flight.
            readonly property bool usesBridge:
                model.mediaSourceAvailable === true && app.mediaBridge.supported
            readonly property string mediaIdentity: root.actionKey + "\u001f"
                                                    + (model.mediaKey || "")
            readonly property string bridgeCacheKey:
                (model.mediaThumbAvailable ? "thumb:" : "full:") + (model.mediaKey || "")
            property string bridgeSource: ""
            property bool bridgeFailed: false

            function refreshBridgeSource() {
                if (!usesBridge || !model.mediaKey) return
                // Hiding never starts a fetch; cached bytes stay cached.
                if (root.mediaHidden) return
                // Outside the media band: the band-entry Connections below asks
                // when the reader settles nearby.
                if (!root.mediaInBand) return
                // Fetch the animated bytes unless autoplay is Never, so OnHover
                // starts instantly.
                if (isGif && app.settings.gifAutoplay !== 2 && !pendingMedia) {
                    animatedSource = app.mediaBridge.animatedSource(model.mediaKey)
                    return
                }
                if (bridgeFailed)
                    app.mediaBridge.retry(bridgeCacheKey)
                bridgeFailed = false
                bridgeSource = app.mediaBridge.mediaSource(
                    model.mediaKey,
                    model.mediaThumbAvailable ? "thumb" : "full")
            }
            // Hover star for saving to the chat GIF collection, through
            // app.starChatGif / GifStarredStore. DELIBERATE NARROWING: only in
            // imageComponent, so GIF stickers are not starrable; chat GIFs are
            // sent as m.image. See
            // GifHoverStarContractTest::hoverStarIsScopedToImageRowsNotStickers.
            // `starred` is a tracked property, since isChatGifStarred() has no
            // NOTIFY; it is refreshed on reuse, on the store's finish signals,
            // and when the bridge caches this row's bytes (the durable
            // content-hash answer needs them; see GifStarredStore). Excludes a
            // pending local echo, whose temporary id would be lost when the
            // echo is replaced.
            readonly property bool starEligible:
                imageBox.isRasterImage && model.mediaSourceAvailable === true
                && app.mediaBridge.supported && !imageBox.pendingMedia
            property bool starred: false
            // `app` resolved defensively (as for the receipt chips): this runs
            // from Component.onCompleted, and a delegate built synchronously
            // inside a property-change handler can see a failed lookup for the
            // first unqualified `app` it evaluates.
            function refreshStarredState() {
                var a = (typeof app !== "undefined" && app) ? app : null
                imageBox.starred = imageBox.starEligible && a !== null
                    && a.isChatGifStarred(model.mediaKey || "")
            }
            Component.onCompleted: {
                refreshBridgeSource()
                refreshStarredState()
            }
            onMediaIdentityChanged: {
                animatedSource = ""
                bridgeSource = ""
                bridgeFailed = false
                refreshBridgeSource()
                refreshStarredState()
            }
            // The reader settled near this row: ask if nothing has been asked
            // for yet.
            Connections {
                target: root
                function onMediaInBandChanged() {
                    if (root.mediaInBand && imageBox.bridgeSource.length === 0
                        && imageBox.animatedSource.length === 0)
                        imageBox.refreshBridgeSource()
                }
            }
            // Re-check as soon as the row becomes eligible.
            onStarEligibleChanged: refreshStarredState()
            // Revealing uses whatever is cached; a row hidden before its bytes
            // arrived fetches now.
            Connections {
                target: root
                function onMediaHiddenChanged() {
                    if (!root.mediaHidden)
                        imageBox.refreshBridgeSource()
                }
            }
            Connections {
                target: app.mediaBridge
                enabled: imageBox.usesBridge
                function onMediaRetryable(cacheKey) {
                    // A transient failure marks the key; 60 s later the bridge
                    // sweeps the mark and emits this so the image asks again.
                    // Without it a failed image stayed on its fallback until a
                    // restart. Bounded: the bridge re-arms the mark on a failed
                    // attempt.
                    if (cacheKey === imageBox.bridgeCacheKey)
                        imageBox.refreshBridgeSource()
                }
                function onMediaCached(cacheKey) {
                    if (cacheKey === imageBox.bridgeCacheKey)
                        imageBox.bridgeSource = app.mediaBridge.cachedSource(cacheKey)
                    // The durable starred check is answerable once the full
                    // payload (never "thumb:") is cached. mediaCached and
                    // animatedMediaReady can both fire for the same key, so
                    // Qt.callLater coalesces them into one refresh. Applies to
                    // every star-eligible raster, since a static image never
                    // fetches an animatedSource.
                    if (imageBox.starEligible && cacheKey === "full:" + (model.mediaKey || ""))
                        Qt.callLater(imageBox.refreshStarredState)
                }
                function onAnimatedMediaReady(cacheKey) {
                    if (cacheKey === "full:" + (model.mediaKey || "")) {
                        imageBox.animatedSource = app.mediaBridge.animatedSource(model.mediaKey)
                        // Full bytes landed; coalesced as above.
                        Qt.callLater(imageBox.refreshStarredState)
                    }
                }
                function onMediaFetchFailed(cacheKey, category) {
                    if (cacheKey === imageBox.bridgeCacheKey)
                        imageBox.bridgeFailed = true
                }
            }
            Connections {
                target: app.gif.starredStore
                enabled: imageBox.starEligible
                function onStarFinished(mediaKey, ok, category, message) {
                    if (mediaKey === (model.mediaKey || ""))
                        imageBox.refreshStarredState()
                }
                function onUnstarFinished(hash) {
                    imageBox.refreshStarredState()
                }
                // Clear All and an account switch change the store and emit
                // only countChanged. Without this a deleted GIF would still
                // show as saved, and activating it would re-write the bytes the
                // user just deleted. Also fires on every star/unstar, so it is
                // an idempotent superset of the handlers above.
                function onCountChanged() {
                    imageBox.refreshStarredState()
                }
            }

            readonly property string resolvedSource:
                usesBridge ? bridgeSource
                           : (model.mediaThumbUrl
                              && model.mediaThumbUrl.toString().length > 0
                              ? model.mediaThumbUrl
                              : (model.mediaUrl || ""))
            // Rounded via the provider's baked mask (no per-frame effect). Only
            // the in-process provider path accepts the suffix.
            readonly property string roundedSource:
                resolvedSource.indexOf("image://lightning-media/") === 0
                ? resolvedSource + "|shape:round:35"
                : resolvedSource

            // Keeps the reserved rectangle while bytes download/decrypt and is
            // replaced in place. Shimmer only while on screen; a failure keeps
            // the static surface.
            Skeleton {
                objectName: "imageSkeleton"
                anchors.fill: parent
                radius: AppTheme.radiusSm
                visible: !root.mediaHidden
                         && img.status !== Image.Ready
                         && animatedImg.status !== AnimatedImage.Ready
                active: root.rowOnScreen && !imageBox.bridgeFailed
                        && img.status !== Image.Error
            }
            // GIFs announce themselves on the placeholder too.
            Rectangle {
                visible: imageBox.isGif && !root.mediaHidden
                         && img.status !== Image.Ready
                         && animatedImg.status !== AnimatedImage.Ready
                         && !imageBox.bridgeFailed
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 5
                radius: 3
                color: AppTheme.overlayScrim
                width: placeholderGifLabel.implicitWidth + 8
                height: placeholderGifLabel.implicitHeight + 4
                Label {
                    id: placeholderGifLabel
                    anchors.centerIn: parent
                    text: "GIF"
                    color: AppTheme.scrimInk
                    font.pixelSize: AppTheme.fontMicro
                    font.weight: Font.Bold
                }
            }

            // Static path (default; also the frame for non-animated GIFs).
            Image {
                id: img
                anchors.fill: parent
                visible: !imageBox.animateGif && !root.mediaHidden
                fillMode: Image.PreserveAspectFit
                // Cleared while hidden: an Image with a source still holds the
                // decoded pixmap.
                source: (imageBox.animateGif || root.mediaHidden)
                        ? "" : imageBox.roundedSource
                sourceSize.width: 640
                asynchronous: true
                cache: true
            }

            // Animated path: confirmed GIFs with animation enabled. Paused
            // off-screen.
            AnimatedImage {
                id: animatedImg
                anchors.fill: parent
                visible: imageBox.animateGif
                fillMode: Image.PreserveAspectFit
                source: imageBox.animateGif ? imageBox.animatedSource : ""
                asynchronous: true
                cache: true
                playing: imageBox.animateGif && root.rowOnScreen
            }

            MediaHiddenPlaceholder {
                anchors.fill: parent
                hidden: root.mediaHidden
                onRevealRequested: root.setMediaHidden(false)
            }

            MouseArea {
                anchors.fill: parent
                enabled: !root.mediaHidden && root.rowActionsEnabled
                visible: enabled
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (imageBox.bridgeFailed) {
                        imageBox.refreshBridgeSource()
                        return
                    }
                    if (root.timelineView && root.timelineView.openImage)
                        root.timelineView.openImage(model.mediaKey || "",
                                                     model.mediaUrl)
                    else if (model.mediaUrl && model.mediaUrl.toString().length > 0)
                        app.media.openExternal(model.mediaUrl)
                }
            }

            // A Loader, so rows that are not star-eligible do not pay for the
            // star's items. `active` tracks starEligible live.
            Loader {
                id: gifStarLoader
                anchors.fill: parent
                active: imageBox.starEligible && !root.mediaHidden
                sourceComponent: Component {
                    Item {
                        id: starLayer
                        anchors.fill: parent

                        // A passive HoverHandler, never a MouseArea, so it
                        // cannot steal wheel or drag gestures from the
                        // timeline.
                        HoverHandler {
                            id: gifStarHover
                        }

                        // The star: revealed while the pointer is over the
                        // media or the star, or while it has focus. Always
                        // present (opacity only) so Tab can reach it. Not an
                        // AbstractButton, whose built-in Space/Return handling
                        // would fire twice with the explicit Keys handlers.
                        // Bottom-right: the action bar is anchored to the row's
                        // top-right at a higher z and would cover a top-right
                        // star; bottom-left holds the GIF badge.
                        Item {
                            id: gifStarButton
                            objectName: "gifHoverStarButton"
                            anchors.bottom: parent.bottom
                            anchors.right: parent.right
                            anchors.margins: 6
                            width: 26
                            height: 26
                            z: 4
                            activeFocusOnTab: true
                            readonly property bool revealed:
                                gifStarHover.hovered || starHover.hovered
                                || gifStarButton.activeFocus
                            // Shown on hover or focus only, never at rest (same
                            // rule as GifPicker's tile star).
                            opacity: revealed ? 1 : 0
                            Behavior on opacity { NumberAnimation { duration: 100 } }

                            Accessible.role: Accessible.Button
                            // Same wording as the picker's tile star;
                            // format-neutral since it saves GIF/PNG/JPEG/WebP.
                            Accessible.name: imageBox.starred
                                ? qsTr("Remove from saved") : qsTr("Save image")
                            Accessible.onPressAction: gifStarButton.activate()

                            function activate() {
                                var key = model.mediaKey || ""
                                if (!key)
                                    return
                                // Durable, content-addressed answer; see
                                // GifStarredStore.
                                if (app.isChatGifStarred(key))
                                    app.unstarChatGif(key)
                                else
                                    app.starChatGif(key)
                            }

                            // Saved state is a fill: the bundled Material
                            // Symbols subset has no filled star glyph.
                            Rectangle {
                                anchors.fill: parent
                                radius: 13
                                color: imageBox.starred ? AppTheme.bolt
                                                        : AppTheme.overlayScrim
                            }
                            Icon {
                                anchors.centerIn: parent
                                name: "star"
                                size: 15
                                color: imageBox.starred
                                       ? AppTheme.boltInk : AppTheme.scrimInk
                            }
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -3
                                radius: 16
                                color: "transparent"
                                border.color: AppTheme.focusRing
                                border.width: 2
                                visible: gifStarButton.activeFocus
                            }

                            HoverHandler { id: starHover }
                            // Gated on `revealed`: the star is hit-testable at
                            // opacity 0 for Tab, and a touch tap has no hover
                            // first, so an invisible corner would otherwise
                            // save/unsave. The gate tracks opacity, not saved
                            // state.
                            TapHandler {
                                enabled: gifStarButton.revealed
                                         && root.rowActionsEnabled
                                onTapped: gifStarButton.activate()
                            }
                            // Ignore key repeat: each activation is a real file
                            // write or removal.
                            Keys.onReturnPressed: (event) => {
                                if (!event.isAutoRepeat) gifStarButton.activate()
                            }
                            Keys.onEnterPressed: (event) => {
                                if (!event.isAutoRepeat) gifStarButton.activate()
                            }
                            Keys.onSpacePressed: (event) => {
                                if (!event.isAutoRepeat) gifStarButton.activate()
                            }
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - 12
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: imageBox.bridgeFailed
                      ? qsTr("Image failed to load — click to retry")
                      : qsTr("(image unavailable)")
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                visible: !root.mediaHidden
                         && (img.status === Image.Error
                             || imageBox.bridgeFailed)
            }
        }
    }

    // ---- sticker ----
    // Stickers keep their transparency: no opaque backing card once loaded. The
    // skeleton exists only while loading.
    Component {
        id: stickerComponent
        Item {
            id: stickerBox
            objectName: "stickerMedia"
            readonly property real maxEdge: 180
            readonly property real natW: model.mediaWidth > 0 ? model.mediaWidth : 0
            readonly property real natH: model.mediaHeight > 0 ? model.mediaHeight : 0
            readonly property real ratio: (natW > 0 && natH > 0)
                                          ? (natH / natW) : 1.0
            readonly property real dispW: {
                var w = natW > 0 ? Math.min(natW, maxEdge) : maxEdge * 0.85
                if (w * ratio > maxEdge) w = maxEdge / ratio
                return Math.max(1, w)
            }
            implicitWidth: dispW
            implicitHeight: Math.max(1, dispW * ratio)

            readonly property bool usesBridge:
                model.mediaSourceAvailable === true && app.mediaBridge.supported
            readonly property string bridgeCacheKey:
                (model.mediaThumbAvailable ? "thumb:" : "full:")
                + (model.mediaKey || "")
            // The bridge keys animated materialization by kind 0, so for a
            // sticker with no server thumbnail (the usual MSC2545 shape) this
            // equals bridgeCacheKey and one fetch serves both.
            readonly property string animatedCacheKey:
                "full:" + (model.mediaKey || "")
            property string bridgeSource: ""
            property bool bridgeFailed: false

            // Animated stickers. info.mimetype is optional for m.sticker, so
            // the declared type cannot be the gate; MediaBridge decides from
            // the container magic and answers animatedMediaReady only for a
            // real animation. The request is speculative, so a still image
            // answers with silence rather than an error. The declared mimetype
            // is only used to skip the fetch when it names a PNG or JPEG; an
            // empty one always asks.
            readonly property string declaredMimetype:
                (model.mediaMimetype || "").toLowerCase()
            readonly property bool maybeAnimated:
                declaredMimetype === "" || declaredMimetype === "image/gif"
                || declaredMimetype === "image/webp"
            readonly property int gifMode: app.settings.gifAutoplay
            property bool gifHovered: false
            readonly property bool playAnimation:
                gifMode === 0 || (gifMode === 1 && gifHovered)
            property string animatedSource: ""

            function refreshBridgeSource() {
                if (!usesBridge || !model.mediaKey) return
                // Hiding never starts a fetch.
                if (root.mediaHidden) return
                if (bridgeFailed)
                    app.mediaBridge.retry(bridgeCacheKey)
                bridgeFailed = false
                bridgeSource = app.mediaBridge.mediaSource(
                    model.mediaKey,
                    model.mediaThumbAvailable ? "thumb" : "full")
                refreshAnimatedSource()
            }
            function refreshAnimatedSource() {
                if (!usesBridge || !model.mediaKey) return
                if (root.mediaHidden || !maybeAnimated || gifMode === 2) return
                animatedSource =
                    app.mediaBridge.animatedSource(model.mediaKey, true)
            }
            Component.onCompleted: refreshBridgeSource()
            Connections {
                target: root
                function onMediaHiddenChanged() {
                    if (!root.mediaHidden)
                        stickerBox.refreshBridgeSource()
                }
            }
            Connections {
                target: app.mediaBridge
                enabled: stickerBox.usesBridge
                function onMediaCached(cacheKey) {
                    if (cacheKey === stickerBox.bridgeCacheKey)
                        stickerBox.bridgeSource =
                            app.mediaBridge.cachedSource(cacheKey)
                }
                function onMediaFetchFailed(cacheKey, category) {
                    if (cacheKey === stickerBox.bridgeCacheKey)
                        stickerBox.bridgeFailed = true
                }
                function onMediaRetryable(cacheKey) {
                    // Same recovery channel as the image box.
                    if (cacheKey === stickerBox.bridgeCacheKey)
                        stickerBox.refreshBridgeSource()
                }
                function onAnimatedMediaReady(cacheKey) {
                    // The bridge validated the bytes as an animation; only now
                    // does the AnimatedImage get a source.
                    if (cacheKey === stickerBox.animatedCacheKey)
                        stickerBox.animatedSource =
                            app.mediaBridge.animatedSource(model.mediaKey, true)
                }
            }
            readonly property string resolvedSource:
                usesBridge ? bridgeSource
                           : (model.mediaThumbUrl
                              && model.mediaThumbUrl.toString().length > 0
                              ? model.mediaThumbUrl
                              : (model.mediaUrl || ""))

            Skeleton {
                anchors.fill: parent
                visible: !root.mediaHidden
                         && stickerImg.status !== Image.Ready
                         && !stickerAnim.animating
                active: root.rowOnScreen && !stickerBox.bridgeFailed
                        && stickerImg.status !== Image.Error
            }
            Image {
                id: stickerImg
                anchors.fill: parent
                // The still frame is the default and fallback: it draws until
                // the AnimatedImage reports Ready, so an undecodable animation
                // degrades to the still picture.
                visible: !root.mediaHidden && !stickerAnim.animating
                fillMode: Image.PreserveAspectFit
                source: root.mediaHidden ? "" : stickerBox.resolvedSource
                sourceSize.width: 360
                asynchronous: true
                cache: true
            }
            AnimatedImage {
                id: stickerAnim
                objectName: "stickerAnimatedMedia"
                anchors.fill: parent
                fillMode: Image.PreserveAspectFit
                // Loaded whenever animated bytes exist and animation is not
                // globally off, so on-hover playback starts immediately. The
                // still frame yields to `animating`, not `source`.
                source: (!root.mediaHidden && stickerBox.gifMode !== 2)
                        ? stickerBox.animatedSource : ""
                readonly property bool animating:
                    status === AnimatedImage.Ready
                    && stickerBox.animatedSource.length > 0
                    && stickerBox.playAnimation && !root.mediaHidden
                visible: animating
                playing: animating && root.rowOnScreen
                asynchronous: true
                cache: true
            }
            MediaHiddenPlaceholder {
                anchors.fill: parent
                hidden: root.mediaHidden
                onRevealRequested: root.setMediaHidden(false)
            }
            HoverHandler {
                id: stickerHover
                enabled: !root.mediaHidden
                onHoveredChanged: stickerBox.gifHovered = hovered
            }
            ToolTip.text: model.body || ""
            ToolTip.visible: stickerHover.hovered && (model.body || "").length > 0
            ToolTip.delay: 400
            TapHandler {
                enabled: !root.mediaHidden && root.rowActionsEnabled
                onTapped: {
                    if (stickerBox.bridgeFailed) {
                        stickerBox.refreshBridgeSource()
                        return
                    }
                    if (root.timelineView && root.timelineView.openImage)
                        root.timelineView.openImage(model.mediaKey || "",
                                                     model.mediaUrl)
                }
            }
            Label {
                anchors.centerIn: parent
                width: parent.width - 12
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: qsTr("Sticker failed to load — click to retry")
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                visible: !root.mediaHidden
                         && (stickerImg.status === Image.Error
                             || stickerBox.bridgeFailed)
            }
        }
    }

    // ---- video ----
    // Reserves thumbnail geometry from the event info (bounded 16:9 fallback),
    // shows the skeleton with a play badge and duration, and swaps in the real
    // thumbnail in place.
    Component {
        id: videoComponent
        Item {
            id: videoBox
            objectName: "videoMedia"
            // 60-75% of the content column, bounded, and never narrower than
            // the control bar needs.
            readonly property real maxW: {
                var cap = Math.min(
                    560, Math.max(280, root.contentInnerCap * 0.72))
                return Math.max(1, Math.min(cap, root.contentInnerCap))
            }
            // Dimensions learned from a previous poster extraction (persisted
            // per account) size the card correctly from the first render.
            readonly property size learnedDims:
                model.mediaWidth > 0 ? Qt.size(0, 0)
                : app.settings.knownVideoDimensions(model.mediaKey || "")
            readonly property real natW: model.mediaWidth > 0
                ? model.mediaWidth
                : (learnedDims.width > 0 ? learnedDims.width : 0)
            readonly property real natH: model.mediaHeight > 0
                ? model.mediaHeight
                : (learnedDims.height > 0 ? learnedDims.height : 0)
            // Without declared dimensions, use the extracted poster's real
            // shape; 16:9 is the last resort before any poster exists.
            readonly property real posterRatio:
                thumbImg.status === Image.Ready && thumbImg.implicitWidth > 0
                ? thumbImg.implicitHeight / thumbImg.implicitWidth : 0
            readonly property real ratio: (natW > 0 && natH > 0)
                                          ? (natH / natW)
                                          : (posterRatio > 0 ? posterRatio
                                                             : 0.5625)
            readonly property real maxH: ratio > 1 ? 440 : 400
            readonly property real minControlW:
                Math.min(260, root.contentInnerCap)
            readonly property real dispW: {
                var w = natW > 0 ? Math.min(natW, maxW) : maxW
                if (w * ratio > maxH) w = maxH / ratio
                return Math.max(minControlW, Math.min(w, maxW))
            }
            implicitWidth: dispW
            // Height is capped even after the width floor; portrait video
            // letterboxes.
            implicitHeight: Math.max(1, Math.min(maxH, dispW * ratio))

            // Inline playback: the player replaces the cover in place with
            // identical geometry. Delegate reuse drops back to the cover.
            property bool playerActive: false
            readonly property string mediaIdentity:
                root.actionKey + "\u001f" + (model.mediaKey || "")
            readonly property bool playbackAvailable:
                model.mediaSourceAvailable === true && app.mediaBridge.supported

            // Serves both a Matrix thumbnail and, without one, a locally
            // extracted first-frame poster (MediaBridge.videoPosterSource),
            // bounded by the prefetch cap.
            readonly property bool usesBridge:
                model.mediaSourceAvailable === true && app.mediaBridge.supported
            readonly property string bridgeCacheKey:
                "thumb:" + (model.mediaKey || "")
            property string bridgeSource: ""
            property bool bridgeFailed: false
            function refreshBridgeSource() {
                if (!usesBridge || !model.mediaKey) return
                if (bridgeFailed)
                    app.mediaBridge.retry(bridgeCacheKey)
                bridgeFailed = false
                // Speculative prefetch follows the GIF autoplay preference
                // (Never means no passive downloads); a size of 0 makes
                // MediaBridge decline. Uses the declared size, else the size
                // learned from a previous fetch.
                var prefetchSize = app.settings.gifAutoplay !== 2
                                   ? (model.mediaSize
                                      || app.settings.knownMediaSizeBytes(
                                             model.mediaKey || "")
                                      || 0)
                                   : 0
                if (model.mediaThumbAvailable === true) {
                    // Thumbnails are never gated.
                    bridgeSource = app.mediaBridge.mediaSource(model.mediaKey,
                                                               "thumb")
                } else if (root.speculativeMediaAllowed) {
                    // videoPosterSource materializes the payload to extract a
                    // frame, so it is speculative work too.
                    bridgeSource = app.mediaBridge.videoPosterSource(
                        model.mediaKey, prefetchSize)
                } else {
                    // Rows off screen or swept past mid-gesture do no
                    // poster/prefetch work; the observers below re-run this
                    // once the view settles.
                    bridgeSource = ""
                }
                // Bounded prefetch so Play is usually instant. MediaBridge
                // enforces the cap and deduplication.
                if (root.speculativeMediaAllowed && playbackAvailable
                    && prefetchSize > 0)
                    app.mediaBridge.prefetchPlayable(model.mediaKey,
                                                     prefetchSize)
            }
            // Re-run when the row appears or the view settles.
            readonly property bool coverOnScreen: root.speculativeMediaAllowed
            onCoverOnScreenChanged: {
                if (coverOnScreen && bridgeSource.length === 0
                    && !bridgeFailed)
                    videoSourceRefresh.restart()
            }
            function resetForMedia() {
                // A pooled Loader keeps this instance while roles rebind: clear
                // the old thumbnail synchronously, then fetch after the update
                // settles.
                playerActive = false
                bridgeSource = ""
                bridgeFailed = false
                videoSourceRefresh.restart()
            }
            Timer {
                id: videoSourceRefresh
                interval: 0
                onTriggered: videoBox.refreshBridgeSource()
            }
            Component.onCompleted: resetForMedia()
            onMediaIdentityChanged: resetForMedia()
            Connections {
                target: app.mediaBridge
                enabled: videoBox.usesBridge
                function onMediaCached(cacheKey) {
                    if (cacheKey === videoBox.bridgeCacheKey)
                        videoBox.bridgeSource =
                            app.mediaBridge.cachedSource(cacheKey)
                }
                function onMediaFetchFailed(cacheKey, category) {
                    if (cacheKey === videoBox.bridgeCacheKey)
                        videoBox.bridgeFailed = true
                }
                function onMediaRetryable(cacheKey) {
                    // Same swept-mark recovery as the image and sticker boxes.
                    if (cacheKey === videoBox.bridgeCacheKey)
                        videoBox.refreshBridgeSource()
                }
                // A video with no declared size is never prefetched, so its
                // poster can only come from the file the user's Play
                // materialized; re-run the poster path when it lands.
                function onPlayableMediaReady(cacheKey) {
                    if (cacheKey === "full:" + (model.mediaKey || "")
                        && model.mediaThumbAvailable !== true
                        && videoBox.bridgeSource.length === 0
                        && !videoBox.bridgeFailed)
                        videoBox.bridgeSource =
                            app.mediaBridge.videoPosterSource(
                                model.mediaKey, 0)
                }
            }

            function formatDuration(ms) {
                if (!ms || ms <= 0) return ""
                var total = Math.round(ms / 1000)
                var m = Math.floor(total / 60)
                var s = total % 60
                return m + ":" + (s < 10 ? "0" : "") + s
            }

            // Stable placeholder (surface tone, type icon, filename) when there
            // is no bridge, the fetch failed, or no poster exists; the play
            // affordance and duration overlay it as they would a real poster.
            readonly property bool showPlaceholder:
                !usesBridge || bridgeFailed
                || (model.mediaThumbAvailable !== true
                    && bridgeSource.length === 0)
            Rectangle {
                objectName: "videoNoThumbPlaceholder"
                anchors.fill: parent
                radius: AppTheme.radiusSm
                color: AppTheme.embedSurface
                border.color: AppTheme.border
                border.width: 1
                visible: videoBox.showPlaceholder
                ColumnLayout {
                    anchors.centerIn: parent
                    spacing: AppTheme.spacing8
                    Icon {
                        name: "videocam"
                        size: 28
                        color: AppTheme.textMuted
                        Layout.alignment: Qt.AlignHCenter
                    }
                    Label {
                        text: model.mediaFilename || model.body || qsTr("Video")
                        textFormat: Text.PlainText
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        elide: Label.ElideMiddle
                        Layout.maximumWidth: videoBox.dispW - 48
                        Layout.alignment: Qt.AlignHCenter
                    }
                }
            }
            Skeleton {
                anchors.fill: parent
                radius: AppTheme.radiusSm
                visible: thumbImg.status !== Image.Ready
                        && !videoBox.showPlaceholder
                active: root.rowOnScreen && !videoBox.showPlaceholder
            }
            Image {
                id: thumbImg
                anchors.fill: parent
                fillMode: Image.PreserveAspectCrop
                // Rounded via the provider's baked mask when served by the
                // media bridge.
                source: videoBox.usesBridge
                        ? (videoBox.bridgeSource.indexOf("image://lightning-media/") === 0
                           ? videoBox.bridgeSource + "|shape:round:35"
                           : videoBox.bridgeSource)
                        : ""
                sourceSize.width: 640
                asynchronous: true
                cache: true
                visible: status === Image.Ready
            }
            // Play affordance + type identity, over thumbnail or skeleton.
            Rectangle {
                anchors.centerIn: parent
                width: 44; height: 44; radius: 22
                color: AppTheme.overlayScrim
                Icon {
                    anchors.centerIn: parent
                    name: "play_arrow"
                    size: 26
                    color: AppTheme.scrimInk
                }
            }
            Rectangle {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 5
                radius: 3
                color: AppTheme.overlayScrim
                width: videoChipRow.implicitWidth + 10
                height: videoChipRow.implicitHeight + 4
                Row {
                    id: videoChipRow
                    anchors.centerIn: parent
                    spacing: 4
                    Icon {
                        anchors.verticalCenter: parent.verticalCenter
                        name: "videocam"
                        size: 11
                        color: AppTheme.scrimInk
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: videoBox.formatDuration(model.mediaDurationMs)
                              || qsTr("Video")
                        color: AppTheme.scrimInk
                        font.pixelSize: AppTheme.fontMicro
                        font.weight: Font.Bold
                    }
                }
            }
            // Explicit Save As stays available from the cover.
            IconButton {
                objectName: "videoSaveButton"
                visible: !videoBox.playerActive && videoBox.playbackAvailable
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: 5
                iconName: "download"
                iconSize: 15
                implicitWidth: 26; implicitHeight: 26
                Accessible.name: qsTr("Save %1 as…")
                    .arg(model.mediaFilename || qsTr("video"))
                onClicked: {
                    if (root.timelineView && root.timelineView.saveMedia)
                        root.timelineView.saveMedia(model.mediaKey || "",
                                                     model.mediaFilename
                                                     || "video")
                }
            }
            TapHandler {
                enabled: !videoBox.playerActive && root.rowActionsEnabled
                onTapped: {
                    if (videoBox.bridgeFailed) {
                        videoBox.refreshBridgeSource()
                        return
                    }
                    if (videoBox.playbackAvailable)
                        videoBox.playerActive = true
                    else if (model.mediaUrl
                             && model.mediaUrl.toString().length > 0)
                        app.media.openExternal(model.mediaUrl)
                }
            }
            // The inline player replaces the cover in place (same box).
            Loader {
                anchors.fill: parent
                active: videoBox.playerActive
                visible: active
                sourceComponent: VideoPlayerCard {
                    mediaKey: model.mediaKey || ""
                    ownerKey: root.actionKey + "\u001f"
                              + (model.mediaKey || "")
                    filename: model.mediaFilename || ""
                    rowOnScreen: root.rowOnScreen
                    onCloseRequested: videoBox.playerActive = false
                }
            }
        }
    }

    // ---- audio / voice ----
    // Inline playback through MediaBridge's validated playable materialization.
    // Voice messages render their MSC3245 waveform. Non-bridge backends keep
    // the external-open path.
    Component {
        id: audioComponent
        AudioPlayerCard {
            objectName: "audioMedia"
            hostContentWidth: root.contentInnerCap
            mediaKey: model.mediaKey || ""
            ownerKey: root.actionKey + "\u001f" + (model.mediaKey || "")
            filename: model.mediaFilename || model.body || ""
            mimetype: model.mediaMimetype || ""
            fileSize: model.mediaSize || 0
            durationMs: model.mediaDurationMs || 0
            isVoice: model.mediaIsVoice === true
            waveform: model.mediaWaveform || []
            rowOnScreen: root.rowOnScreen
            // Speculative prefetch waits for a settle, like the video path.
            // Playback logic keeps rowOnScreen.
            prefetchAllowed: root.speculativeMediaAllowed
            canSave: model.mediaSourceAvailable === true
                     && app.mediaBridge.supported
            onSaveRequested: {
                if (root.timelineView && root.timelineView.saveMedia)
                    root.timelineView.saveMedia(model.mediaKey || "",
                                                 model.mediaFilename || "audio")
            }
            onOpenExternalRequested: {
                if (model.mediaUrl && model.mediaUrl.toString().length > 0)
                    app.media.openExternal(model.mediaUrl)
            }
        }
    }

    Component {
        id: fileComponent
        Rectangle {
            id: fileCard
            objectName: "fileCard"
            implicitWidth: Math.min(340, root.contentInnerCap)
            implicitHeight: fileRow.implicitHeight + 16
            color: AppTheme.embedSurface
            radius: AppTheme.radiusMd
            border.color: AppTheme.border
            border.width: 1

            // Save state keyed by this card's media. Only the main timeline
            // exposes the keys; thread-panel delegates stay stateless.
            readonly property var tlView: root.timelineView
            readonly property bool saving:
                tlView && tlView.saveInFlightKey !== undefined
                && (model.mediaKey || "") !== ""
                && tlView.saveInFlightKey === model.mediaKey
            readonly property bool savedFlash:
                tlView && tlView.lastSavedKey !== undefined
                && (model.mediaKey || "") !== ""
                && tlView.lastSavedKey === model.mediaKey
            readonly property bool savedOk:
                savedFlash && tlView.lastSaveOk === true

            function fileTypeIcon(mime) {
                var m = (mime || "").toLowerCase()
                if (m.indexOf("image/") === 0) return "image"
                if (m.indexOf("video/") === 0) return "videocam"
                if (m.indexOf("audio/") === 0) return "graphic_eq"
                if (m.indexOf("text/") === 0
                        || m === "application/pdf") return "description"
                return "attach_file"
            }
            function fileTypeLabel(mime) {
                // Show the subtype tail ("ZIP", "PDF") rather than a raw MIME
                // type.
                var m = (mime || "")
                var slash = m.indexOf("/")
                if (slash < 0) return m
                var sub = m.substring(slash + 1)
                var plus = sub.lastIndexOf("+")
                if (plus >= 0) sub = sub.substring(plus + 1)
                if (sub.indexOf("x-") === 0) sub = sub.substring(2)
                if (sub === "octet-stream") return qsTr("File")
                return sub.length <= 12 ? sub.toUpperCase() : sub
            }

            RowLayout {
                id: fileRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 10

                // File-type identity chip.
                Rectangle {
                    Layout.preferredWidth: 38
                    Layout.preferredHeight: 38
                    radius: AppTheme.radiusMd
                    color: AppTheme.accentSoft
                    Icon {
                        anchors.centerIn: parent
                        name: fileCard.fileTypeIcon(model.mediaMimetype)
                        size: 20
                        color: AppTheme.accent
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Label {
                        text: model.mediaFilename || model.body || qsTr("File")
                        textFormat: Text.PlainText
                        color: AppTheme.text
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        font.weight: AppTheme.weightStrong
                        elide: Label.ElideMiddle
                        Layout.fillWidth: true
                    }
                    Label {
                        text: {
                            // Shared formatter, so this matches the collapsed
                            // summary and is translatable.
                            var size = root.embedSizeText(model.mediaSize || 0)
                            var kind = fileCard.fileTypeLabel(model.mediaMimetype)
                            var status = fileCard.saving ? qsTr("Saving…")
                                       : fileCard.savedFlash
                                         ? (fileCard.savedOk ? qsTr("Saved")
                                                             : qsTr("Save failed"))
                                         : ""
                            var base = size.length === 0 ? (kind || "")
                                     : kind ? size + " • " + kind : size
                            return status ? base + " • " + status : base
                        }
                        color: fileCard.savedFlash && !fileCard.savedOk
                               ? AppTheme.danger
                               : fileCard.savedFlash ? AppTheme.success
                               : AppTheme.textMuted
                        font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                        elide: Label.ElideRight
                        Layout.fillWidth: true
                    }
                }

                // Saves are atomic with no progress API, so the in-flight state
                // is indeterminate.
                AppBusyIndicator {
                    size: 26
                    visible: fileCard.saving
                    running: visible
                    Layout.preferredWidth: 26
                    Layout.preferredHeight: 26
                }
                Icon {
                    visible: fileCard.savedFlash && !fileCard.saving
                    name: fileCard.savedOk ? "check_circle" : "error"
                    size: 20
                    color: fileCard.savedOk ? AppTheme.success : AppTheme.danger
                }
                IconButton {
                    objectName: "fileSaveButton"
                    visible: model.mediaSourceAvailable === true
                             && app.mediaBridge.supported && !fileCard.saving
                    iconName: fileCard.savedFlash && !fileCard.savedOk
                              ? "refresh" : "download"
                    iconSize: 18
                    implicitWidth: 30; implicitHeight: 30
                    Accessible.name: qsTr("Save %1 as…")
                        .arg(model.mediaFilename || qsTr("file"))
                    ToolTip.text: fileCard.savedFlash && !fileCard.savedOk
                                  ? qsTr("Retry save") : qsTr("Save as…")
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    onClicked: {
                        if (root.timelineView && root.timelineView.saveMedia)
                            root.timelineView.saveMedia(model.mediaKey || "",
                                                         model.mediaFilename
                                                         || "download")
                    }
                }
                // HTTP backend keeps its external-open path.
                IconButton {
                    visible: !(model.mediaSourceAvailable === true)
                             && (model.mediaUrl
                                 ? model.mediaUrl.toString().length > 0
                                 : false)
                    iconName: "open_in_full"
                    iconSize: 18
                    implicitWidth: 30; implicitHeight: 30
                    Accessible.name: qsTr("Open file")
                    onClicked: app.media.openExternal(model.mediaUrl)
                }
            }
        }
    }

    // Poll card (MSC3381): stateless, derived from model roles, so reuse never
    // shows another row's votes. Undisclosed running polls arrive with zeroed
    // counts. Shared place: no embedded map (a tile server would see every
    // reader's IP); the card describes the place and opens it in the browser.
    // The link is built from the parsed numbers, never the sender's geo:
    // string, which UrlLauncher's allowlist would have to widen to accept.
    Component {
        id: locationComponent
        Rectangle {
            objectName: "locationCard"
            implicitWidth: Math.min(320, locationCol.implicitWidth
                                         + AppTheme.spacing12 * 2)
            implicitHeight: locationCol.implicitHeight + AppTheme.spacing12 * 2
            radius: AppTheme.radiusMd
            color: AppTheme.surfaceElevated
            border.color: AppTheme.border
            border.width: 1

            ColumnLayout {
                id: locationCol
                anchors.fill: parent
                anchors.margins: AppTheme.spacing12
                spacing: AppTheme.spacing4

                RowLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing8
                    Icon {
                        // The icon font is a subset and Icon.qml returns "" for
                        // an unknown name, so a wrong name is a blank glyph.
                        name: "explore"
                        size: 18
                        color: AppTheme.textMuted
                    }
                    Label {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        // The sender's own words: never markup.
                        textFormat: Text.PlainText
                        text: model.locationLive === true
                              ? (model.locationLiveActive === true
                                 ? qsTr("Sharing live location")
                                 : qsTr("Live location (ended)"))
                              : (model.locationAsset === "m.pin"
                                 ? qsTr("A place") : qsTr("Location"))
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.textMeta
                        elide: Label.ElideRight
                    }
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    text: model.locationDescription
                          && model.locationDescription.length > 0
                          ? model.locationDescription : (model.body || "")
                    color: AppTheme.textPrimary
                    font.pixelSize: AppTheme.textBody
                }

                Label {
                    Layout.fillWidth: true
                    visible: model.locationHasPoint === true
                    textFormat: Text.PlainText
                    text: {
                        var pos = model.locationLat.toFixed(5) + ", "
                                + model.locationLon.toFixed(5)
                        return model.locationUncertaintyM > 0
                            ? qsTr("%1 · within %2 m").arg(pos)
                                  .arg(Math.round(model.locationUncertaintyM))
                            : pos
                    }
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.textMeta
                }

                AppButton {
                    objectName: "locationOpenMapButton"
                    Layout.fillWidth: true
                    visible: model.locationHasPoint === true
                    size: "sm"
                    text: qsTr("Open in a map")
                    onClicked: {
                        // Zoom 16 is street level. Built from validated numbers
                        // only.
                        var lat = model.locationLat
                        var lon = model.locationLon
                        app.media.openWebUrl(
                            "https://www.openstreetmap.org/?mlat=" + lat
                            + "&mlon=" + lon + "#map=16/" + lat + "/" + lon)
                    }
                }

                // A point we could not read; say so.
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: model.locationHasPoint !== true
                    text: qsTr("This location could not be read.")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.textMeta
                }
            }
        }
    }

    Component {
        id: pollComponent
        Rectangle {
            id: pollCard
            objectName: "pollCard"
            readonly property var pollAnswers: model.pollAnswers || []
            readonly property bool pollEnded: model.pollEnded === true
            readonly property bool showCounts:
                pollEnded || model.pollKind === "disclosed"
            readonly property int maxSelections:
                Math.max(1, model.pollMaxSelections || 1)
            readonly property bool multiSelect: maxSelections > 1
            readonly property bool canVote:
                !pollEnded && app.composer.pollsSupported()
            readonly property string pollThreadRoot:
                root.inThreadPanel ? (app.thread.rootEventId || "") : ""
            readonly property int totalVotes: {
                var sum = 0
                for (var i = 0; i < pollAnswers.length; ++i)
                    sum += (pollAnswers[i].count || 0)
                return sum
            }
            function ownSelection() {
                var ids = []
                for (var i = 0; i < pollAnswers.length; ++i)
                    if (pollAnswers[i].byMe) ids.push(pollAnswers[i].id)
                return ids
            }
            function toggleAnswer(answerId) {
                if (!canVote) return
                var eventId = root.eventIdForActions()
                if (!eventId || eventId === "") return
                var selection
                if (multiSelect) {
                    selection = ownSelection()
                    var at = selection.indexOf(answerId)
                    if (at >= 0) selection.splice(at, 1)
                    else if (selection.length < maxSelections)
                        selection.push(answerId)
                    else return // selection cap reached
                } else {
                    // Re-clicking your own choice retracts the vote (an empty
                    // response list).
                    selection = ownSelection().indexOf(answerId) >= 0
                                ? [] : [answerId]
                }
                app.composer.votePoll(eventId, selection, pollThreadRoot)
            }

            // Fixed intrinsic width; the Loader's maximumWidth clamps it.
            // bubble.width here would loop in Bubbles.
            implicitWidth: 420
            implicitHeight: pollColumn.implicitHeight + 20
            color: AppTheme.embedSurface
            radius: AppTheme.radiusMd
            border.color: AppTheme.border
            border.width: 1

            ColumnLayout {
                id: pollColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 10
                spacing: 8

                RowLayout {
                    spacing: 8
                    Layout.fillWidth: true
                    Icon {
                        name: "check_circle"
                        size: 16
                        color: AppTheme.accent
                    }
                    Label {
                        objectName: "pollQuestion"
                        text: model.pollQuestion || ""
                        color: AppTheme.text
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.scaled(AppTheme.fontSizeM)
                        font.weight: Font.DemiBold
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                }

                Label {
                    visible: !pollCard.showCounts && !pollCard.pollEnded
                    text: qsTr("Results are revealed when the poll ends")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.scaled(11)
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                }

                Repeater {
                    model: pollCard.pollAnswers
                    delegate: AbstractButton {
                        id: answerRow
                        required property var modelData
                        readonly property int voteCount: modelData.count || 0
                        readonly property real voteShare:
                            pollCard.totalVotes > 0
                            ? voteCount / pollCard.totalVotes : 0
                        objectName: "pollAnswer"
                        Layout.fillWidth: true
                        padding: 6
                        enabled: pollCard.canVote
                        hoverEnabled: pollCard.canVote
                        focusPolicy: Qt.TabFocus
                        Accessible.role: pollCard.multiSelect
                                         ? Accessible.CheckBox
                                         : Accessible.RadioButton
                        Accessible.name: pollCard.showCounts
                            ? qsTr("%1, %2 votes").arg(modelData.text || "")
                                                  .arg(voteCount)
                            : (modelData.text || "")
                        Accessible.checkable: pollCard.canVote
                        Accessible.checked: modelData.byMe === true
                        onClicked: pollCard.toggleAnswer(modelData.id)
                        Keys.onReturnPressed: pollCard.toggleAnswer(modelData.id)
                        Keys.onSpacePressed: pollCard.toggleAnswer(modelData.id)

                        background: Rectangle {
                            radius: AppTheme.radiusSm
                            color: answerRow.hovered && pollCard.canVote
                                   ? AppTheme.hover : "transparent"
                            border.width: answerRow.visualFocus ? 2 : 0
                            border.color: AppTheme.focusRing
                        }
                        contentItem: ColumnLayout {
                            id: answerColumn
                            spacing: 4
                            RowLayout {
                                spacing: 8
                                Layout.fillWidth: true
                                // Radio / checkbox indicator by selection mode.
                                Rectangle {
                                    width: 16; height: 16
                                    radius: pollCard.multiSelect
                                            ? AppTheme.radiusSm / 2 : 8
                                    color: modelData.byMe === true
                                           ? AppTheme.accent : "transparent"
                                    border.width: 1
                                    border.color: modelData.byMe === true
                                                  ? AppTheme.accent
                                                  : AppTheme.borderStrong
                                    Icon {
                                        anchors.centerIn: parent
                                        visible: modelData.byMe === true
                                        name: "check"
                                        size: 11
                                        color: AppTheme.accentText
                                    }
                                }
                                Label {
                                    text: modelData.text || ""
                                    color: AppTheme.text
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.scaled(13)
                                    wrapMode: Text.Wrap
                                    Layout.fillWidth: true
                                }
                                Label {
                                    visible: pollCard.showCounts
                                    text: answerRow.voteCount
                                    color: modelData.byMe === true
                                           ? AppTheme.accent : AppTheme.textMuted
                                    font.pixelSize: AppTheme.scaled(12)
                                    font.weight: Font.Medium
                                }
                            }
                            // Result bar — only when tallies are visible.
                            Rectangle {
                                visible: pollCard.showCounts
                                Layout.fillWidth: true
                                Layout.leftMargin: 24
                                implicitHeight: 7
                                radius: height / 2
                                color: AppTheme.hover
                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    // Any non-zero share gets at least its own
                                    // height; zero shows no fill.
                                    width: answerRow.voteShare > 0
                                           ? Math.max(height,
                                                      parent.width * answerRow.voteShare)
                                           : 0
                                    radius: height / 2
                                    color: modelData.byMe === true
                                           ? AppTheme.accent : AppTheme.accentSoft
                                    Behavior on width {
                                        enabled: !AppTheme.reducedMotion
                                        NumberAnimation {
                                            duration: 200
                                            easing.type: Easing.OutCubic
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Label {
                    objectName: "pollFooter"
                    text: {
                        var voters = model.pollTotalVoters || 0
                        if (pollCard.pollEnded)
                            return qsTr("Final result • %n vote(s)",
                                        "closed poll tally", voters)
                        if (voters === 0) return qsTr("No votes yet")
                        return qsTr("%n vote(s)", "open poll tally", voters)
                    }
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.scaled(11)
                }
            }
        }
    }
}
