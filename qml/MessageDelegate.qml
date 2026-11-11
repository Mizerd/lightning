import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    // Not clipped: rows size to their content exactly, and a clip would cut off
    // the hover action bar on a row shorter than it.
    clip: false
    // A lifted action bar reaches over the row above, so the hovered row paints
    // (and takes hover) above its neighbours.
    z: actionsVisible ? 2 : 1
    property var timelineView:
        TableView.view ? TableView.view : ListView.view

    Loader {
        objectName: "keyboardMessageSelectionLoader"
        anchors.fill: parent
        z: 2
        active: root.keyboardSelected
        sourceComponent: Rectangle {
            objectName: "keyboardMessageSelection"
            color: "transparent"
            radius: AppTheme.scaled(6)
            border.width: AppTheme.scaled(2)
            border.color: AppTheme.accent
        }
    }

    readonly property bool keyboardSelected: !!root.timelineView
        && root.timelineView.keyboardSelectedEventId !== undefined
        && root.timelineView.keyboardSelectedEventId !== ""
        && root.timelineView.keyboardSelectedEventId === model.eventId

    // SettingsManager::kTimelineShow / kTimelineOnHover / kTimelineHide.
    readonly property int avatarsMode: app.settings.timelineAvatars
    readonly property int reactionsMode: app.settings.timelineReactions
    readonly property int receiptsMode: app.settings.timelineReadReceipts
    readonly property bool messageActionsHidden:
        app.settings.timelineMessageActions === 2
    onMessageActionsHiddenChanged: {
        if (messageActionsHidden)
            messageActionBarLoader.latched = false
    }
    readonly property bool anyHoverReveal:
        avatarsMode === 1 || reactionsMode === 1 || receiptsMode === 1
    // Opacity only: building on hover would cost frames under a moving pointer.
    readonly property bool hoverRevealed: revealHover.hovered
        || root.keyboardSelected || root.actionsPinned || root.moreMenuOpen
    function revealOpacity(mode) {
        return mode === 1 && !root.hoverRevealed ? 0 : 1
    }
    HoverHandler {
        id: revealHover
        objectName: "timelineRevealHover"
        enabled: root.anyHoverReveal && !root.rowHoverSuspended
    }

    property string sourceRoomName: ""
    readonly property bool bulkSelectionMode:
        app.bulkRedaction.selecting === true
        && root.timelineModel.roomId === app.bulkRedaction.timelineId
    readonly property bool selectionMode:
        app.forward.selecting === true || root.bulkSelectionMode
    readonly property bool rowActionsEnabled: !root.selectionMode
    readonly property bool readOnlyView:
        root.timelineView && root.timelineView.readOnlyContext === true
    readonly property bool copyableText:
        model.undecryptable !== true
        && root.menuEventId !== ""
        && root.timelineModel.visibleTextForEvent(root.menuEventId).length > 0
    readonly property bool canRedactOthers:
        !!app.openRoomPermissions
        && app.openRoomPermissions.roomId === app.currentRoomId
        && app.openRoomPermissions.redactKnown === true
            ? app.openRoomPermissions.canRedactOthers === true
            : (!!app.roomInfo && app.roomInfo.roomId === app.currentRoomId
               && app.roomInfo.canRedactOthers === true)
    readonly property bool rowSelectable:
        root.bulkSelectionMode
        ? root.timelineModel.canRedactEvent(root.eventIdForActions(),
                                            root.canRedactOthers)
        : model.isVirtual !== true && model.redacted !== true
        && model.isLocalEcho !== true
        && !root.isCallEvent && !root.isStateActivity
        && !root.isThreadKeyWait
        && root.eventIdForActions() !== ""
        && !root.isGallery
    readonly property real selectionGutterWidth: 26
    // isSelected() is a plain call Qt cannot observe; selectedCount changes on
    // every toggle and carries the dependency.
    readonly property bool rowSelected:
        root.selectionMode && root.rowSelectable
        && (root.bulkSelectionMode
            ? app.bulkRedaction.selectedCount > 0
              && app.bulkRedaction.isSelected(root.eventIdForActions())
            : app.forward.selectedCount > 0
              && app.forward.isSelected(root.eventIdForActions()))

    function toggleSelectionForThisRow() {
        if (!root.rowSelectable)
            return
        if (root.bulkSelectionMode) {
            app.bulkRedaction.toggleFromPointer(root.eventIdForActions(),
                                                root.canRedactOthers)
            return
        }
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
            isLocation: model.isLocation === true,
            senderDisplayName: model.senderDisplayName || "",
            senderName: model.senderDisplayName || "",
            body: model.body || "",
            mediaKey: model.mediaKey || "",
            mediaFilename: model.mediaFilename || "",
            timestampMs: model.timestampMs || 0,
            sourceRoomName: root.sourceRoomName || "",
        })
    }
    readonly property bool speculativeMediaAllowed:
        rowOnScreen
        && (!root.timelineView
            || root.timelineView.speculativeMediaAllowed !== false)
    property bool mediaInBand: true
    property bool mediaPixmapRetained: true
    readonly property bool mediaPixmapCacheAllowed:
        !timelineView || timelineView.mediaRetainFirstRow === undefined
    // A part built only in some states keeps what it had when a scroll began
    // until the scroll settles, so nothing is built mid-gesture.
    readonly property bool scrollHoldsParts: !!root.timelineView
        && root.timelineView.userScrollActive === true

    readonly property bool isVirtualRow: model.isVirtual === true
    readonly property bool isStateActivity: model.isStateActivity === true
    readonly property bool isRoutineActivity: model.isRoutineActivity === true
    readonly property bool isCallEvent: model.isCallEvent === true
    readonly property bool isThreadKeyWait: model.threadReplyAwaitingKeys === true
    readonly property bool roomActivityVisible: {
        if (isStateActivity)
            return model.stateGroupLeader !== true
                   || root.stateActivityEntries.length > 0
        if (!isRoutineActivity) return true
        if (!app.settings.showRoomActivity) return false
        if (model.stateKind === "membership")
            return app.settings.showMembershipEvents
        if (model.stateKind === "member_profile")
            return app.settings.showProfileChangeEvents
        return true
    }
    readonly property var timelineModel:
        root.timelineView && root.timelineView.timelineModel
        ? root.timelineView.timelineModel : app.timeline
    function canCancelSendAt(viewRow) {
        return root.timelineModel !== null
            && typeof root.timelineModel.canCancelSend === "function"
            && root.timelineModel.canCancelSend(root.sourceModelRow(viewRow))
    }
    function sourceModelRow(viewRow) {
        return root.timelineView && root.timelineView.sourceRowForViewRow
                ? root.timelineView.sourceRowForViewRow(viewRow) : viewRow
    }
    readonly property bool suppressedAsThreadRoot:
        root.timelineView && (root.timelineView.suppressRootEventId || "") !== ""
        && (model.eventId || "") === root.timelineView.suppressRootEventId
    readonly property bool threadInline:
        !root.inThreadPanel && !!root.timelineModel
        && root.timelineModel.threadRepliesInline === true
    readonly property string rowThreadId:
        (model.threadRootId || "").length > 0 ? model.threadRootId
        : (model.isThreadRoot === true ? (model.eventId || "") : "")
    readonly property bool threadFilterVisible: {
        if (!root.threadInline)
            return true
        var focus = root.timelineModel.threadFilterRootId || ""
        if (focus.length > 0)
            return (model.eventId || "") === focus
                || (model.threadRootId || "") === focus
        if (root.timelineModel.hideThreadReplies === true)
            return (model.threadRootId || "").length === 0
        return true
    }
    function showThread(rootId) {
        if (rootId && rootId.length > 0)
            app.showThread(app.currentRoomId, rootId)
    }
    readonly property var stateActivityEntries: model.stateGroupEntries || []
    readonly property bool showsIdentity: model.showSenderIdentity === true

    readonly property string rowKind:
        isVirtualRow ? (model.eventType === 7 ? "divider"
                        : model.eventType === 8 ? "read-marker" : "start")
        : isStateActivity ? "state" : "event"

    readonly property bool dividerSuppressed:
        isVirtualRow && (model.eventType === 7 || model.eventType === 8)
        && model.dividerIntroducesVisibleContent === false

    readonly property bool deletedFollower:
        model.redacted === true && model.deletedGroupLeader === false

    readonly property int timelineLayout:
        inThreadPanel ? (app.settings.messageLayout === 3 ? 3 : 0)
                      : app.settings.messageLayout
    property bool isDirectRoom: root.timelineView
                                && root.timelineView.isDirectRoom === true
    readonly property bool bubbleMode: timelineLayout === 1 && isDirectRoom
    readonly property bool compactMode: timelineLayout === 2
    readonly property bool smallAvatarMode: timelineLayout === 3
    readonly property int headerAvatarSize: 20
    readonly property real bubblePad: bubbleMode ? 10 : 0

    readonly property real messageTopSpacing: showsIdentity
        ? Math.max(0, (compactMode ? 2 : smallAvatarMode ? 6
                                       : AppTheme.spacingM)
                      + AppTheme.messageGroupGapExtra)
        : Math.max(0, (compactMode ? 0 : smallAvatarMode ? 2 : 1)
                      + AppTheme.messageContinuationGapExtra)
    readonly property bool gutterAvatarsHidden:
        avatarsMode === 2 && !compactMode && !smallAvatarMode
    readonly property real avatarGutterWidth:
        (compactMode || smallAvatarMode || gutterAvatarsHidden)
        ? 8 : (bubbleMode ? 44 : 40)

    property bool rowOnScreen:
        !!root.timelineView
        && (y + height) > root.timelineView.contentY
        && y < (root.timelineView.contentY + root.timelineView.height)

    readonly property bool decryptStalled:
        app.cryptoBootstrap
        && (app.cryptoBootstrap.phase
                === CryptoBootstrapModel.ManualRecoveryRequired
            || app.cryptoBootstrap.phase
                === CryptoBootstrapModel.NoBackupAvailable)
    readonly property bool threadSearchVisible: {
        if (!root.timelineView || !root.timelineView.threadSearchQuery
                || root.timelineView.threadSearchQuery.trim().length === 0)
            return true
        var id = model.eventId || ""
        return id !== "" && id !== root.timelineView.suppressRootEventId
            && root.timelineModel.visibleTextForEvent(id).toLowerCase()
                .indexOf(root.timelineView.threadSearchQuery.trim().toLowerCase()) >= 0
    }
    visible: roomActivityVisible && !suppressedAsThreadRoot && !dividerSuppressed
             && !deletedFollower && threadFilterVisible && threadSearchVisible
    readonly property real naturalImplicitHeight:
        (!roomActivityVisible || suppressedAsThreadRoot || dividerSuppressed
         || deletedFollower || !threadSearchVisible) ? 0
        : !threadFilterVisible ? 0
        : isVirtualRow ? virtualRowLoader.implicitHeight
        : isCallEvent ? callEventRow.implicitHeight + AppTheme.spacingS * 2
        : isThreadKeyWait ? threadKeyWaitRow.implicitHeight + AppTheme.spacingS * 2
        : isStateActivity ? (stateActivity.item !== null && stateActivity.item.entryCount > 0
                             ? stateActivity.implicitHeight : 0)
        : layout.implicitHeight + messageTopSpacing
    property bool heightSeedActive: false
    property real heightSeed: 0
    property string heightSeedIdentity: ""
    property bool heightMeasurementReady: false
    implicitHeight: heightSeedActive ? heightSeed : naturalImplicitHeight

    readonly property string actionKey: (model.itemId && model.itemId.length > 0)
                                        ? model.itemId
                                        : (model.eventId || "")
    readonly property bool mediaHideable:
        (model.isImage === true || model.isSticker === true)
        && model.redacted !== true
        && root.mediaVisibilityKey.length > 0
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
    readonly property string spoilerKey: model.eventId || root.actionKey
    readonly property bool spoilerHidden: {
        var revision = SpoilerState.revision
        return model.mediaSpoiler === true && !SpoilerState.isRevealed(root.spoilerKey)
    }
    function revealSpoiler() { SpoilerState.reveal(root.spoilerKey) }
    Connections {
        target: (typeof app !== "undefined" && app) ? app.mediaVisibility : null
        function onHiddenChanged(key, hidden) {
            if (key === root.mediaVisibilityKey)
                root.mediaHidden = hidden
        }
    }
    readonly property bool collapseEmbedsSetting:
        typeof app !== "undefined" && app && app.settings
        && (app.settings.collapseEmbeds === true
            || root.accountCollapsesMedia)
    readonly property string roomJoinRule:
        root.timelineView && root.timelineView.roomJoinRule !== undefined
        ? String(root.timelineView.roomJoinRule) : ""
    readonly property bool accountCollapsesMedia: {
        if (typeof app === "undefined" || !app || !app.accountPrefs)
            return false
        const prefs = app.accountPrefs
        const _deps = [prefs.mediaPreviews, prefs.mediaPreviewKnown,
                       prefs.privateMeansNonPublic]
        return prefs.collapsesMedia(root.roomJoinRule, root.roomEncrypted,
                                    root.isDirectRoom)
    }
    property bool embedExpanded: false
    function toggleEmbedExpanded() { root.embedExpanded = !root.embedExpanded }

    readonly property bool mediaEmbedCollapsible:
        root.collapseEmbedsSetting && root.mediaRowBody
        && model.redacted !== true
    readonly property bool mediaEmbedCollapsed:
        root.mediaEmbedCollapsible && !root.embedExpanded
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
        return qsTr("%1 MB").arg(Number(bytes / (1024 * 1024)).toLocaleString(Qt.locale(AppLocale.name), "f", 1))
    }
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
    function mediaEmbedDetail() {
        if (root.isGallery) return ""
        var parts = []
        var name = (model.mediaFilename || "").trim()
        if (name.length > 0 && model.mediaIsVoice !== true)
            parts.push(name)
        if (model.isVideo === true || model.isAudio === true) {
            var d = root.embedDurationText(model.mediaDurationMs || 0)
            if (d.length > 0) parts.push(d)
            else if (model.isAudio === true) {
                var as = root.embedSizeText(model.mediaSize || 0)
                if (as.length > 0) parts.push(as)
            }
        } else if (model.isImage === true || model.isSticker === true) {
            if (model.mediaWidth > 0 && model.mediaHeight > 0) {
                parts.push(model.mediaWidth + "×" + model.mediaHeight)
            } else {
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
    function previewEmbedDetail() {
        var p = root.preview || {}
        var parts = []
        var host = (p.host || "").trim()
        if (host.length > 0) parts.push(host)
        var title = (p.title || "").trim()
        if (title.length === 0) title = (p.siteName || "").trim()
        if (title.length === 0) title = (p.fileName || "").trim()
        if (title.length > 0) parts.push(title)
        return parts.join(" · ")
    }
    function previewMediaKind() {
        var p = root.preview || {}
        return p.isDirectMedia === true ? (p.mediaKind || "") : ""
    }
    function previewEmbedIcon() {
        var kind = root.previewMediaKind()
        return kind === "video" ? "videocam" : kind === "image" ? "image" : "link"
    }
    function previewEmbedKind() {
        var kind = root.previewMediaKind()
        return kind === "video" ? qsTr("Video")
             : kind === "image" ? qsTr("Image") : qsTr("Link")
    }
    function linkMediaSummary() {
        var p = root.preview || {}
        var parts = [root.previewEmbedKind()]
        var name = (p.fileName || "").trim()
        if (name.length > 0) parts.push(name)
        var size = root.embedSizeText(p.mediaSize || 0)
        if (size.length > 0) parts.push(size)
        return parts.join(" · ")
    }
    readonly property bool linkMediaInline:
        typeof app !== "undefined" && app && app.linkPreviews
        && app.linkPreviews.inlineMedia === true
    function openLinkMedia() {
        var p = root.preview || {}
        var key = p.mediaKey || ""
        if (key.length === 0)
            return
        if (!app.linkPreviews.linkMediaAvailable(key)
                || !root.timelineView || !root.timelineView.openImage) {
            if ((p.url || "").length > 0)
                app.media.openWebUrl(p.url)
            return
        }
        root.timelineView.openImage(key, "")
    }
    readonly property string searchHighlight:
        root.timelineModel && root.timelineModel.searchActive === true
        ? (root.timelineModel.searchQuery || "") : ""
    readonly property bool isCurrentSearchHit:
        root.searchHighlight !== ""
        && root.timelineModel
        && (model.eventId || "") !== ""
        && (model.eventId || "") === root.timelineModel.searchCurrentEventId

    function inkLinksForOwnBubble(html) {
        if (!drawsOnOwnBubble || !html || html.length === 0)
            return html
        var link = ("" + AppTheme.link).toLowerCase()
        var ink = ("" + AppTheme.ownBubbleText).toLowerCase()
        var out = html
        out = out.split("style=\"color:" + link + "\"").join(
                    "style=\"color:" + ink + "; text-decoration:underline\"")
        out = out.split("style=\"color:" + link + ";").join(
                    "style=\"color:" + ink + ";")
        return out
    }

    function highlightSearchMatches(html, needle, current) {
        if (!needle || needle.length === 0 || !html || html.length === 0)
            return html
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
        heightSeedReleaseTimer.restart()
    }
    onNaturalImplicitHeightChanged: {
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
    readonly property bool transientOwnerBlocks: {
        if (!root.timelineView)
            return false
        var owner = root.timelineView.transientInteractionOwner
        if (owner === undefined || owner === null || owner === "")
            return false
        return !(owner === "menu" && root.moreMenuOpen)
    }
    readonly property bool actionsVisible:
        !root.readOnlyView
        && !root.messageActionsHidden
        && !root.transientOwnerBlocks
        && root.hasServerEventId
        && (root.moreMenuOpen
            || (root.timelineView
                && (root.timelineView.hoveredActionsKey === actionKey
                    || (actionsPinned
                        && root.timelineView.hoveredActionsKey === ""))))
    property string menuEventId: ""
    readonly property bool hasServerEventId: {
        var id = model.eventId || ""
        return id.length > 0 && id.indexOf("local:") !== 0
    }

    // Clears the view's hovered key if this row owns it. A row destroyed under
    // the pointer (room change) gets no leave event, and no bar would show
    // until the pointer entered another row. Only destruction can do this: in
    // onActionKeyChanged the old key is already gone.
    readonly property bool rowHoverSuspended: !!root.timelineView
        && root.timelineView.rowHoverSuspended === true
    function releaseHoveredActions() {
        if (root.timelineView
                && root.timelineView.hoveredActionsKey === root.actionKey)
            root.timelineView.hoveredActionsKey = ""
    }
    Component.onDestruction: releaseHoveredActions()
    onRowOnScreenChanged: {
        if (!rowOnScreen) {
            messageActionBarLoader.latched = false
            releaseClosedPopups()
        }
    }
    function releaseClosedPopups() {
        if (moreMenuItem && !moreMenuItem.visible) {
            var menu = moreMenuItem
            moreMenuItem = null
            menu.destroy()
        }
        if (detailsDialogItem && !detailsDialogItem.visible) {
            var details = detailsDialogItem
            detailsDialogItem = null
            details.destroy()
        }
        if (confirmDialogItem && !confirmDialogItem.visible) {
            var confirm = confirmDialogItem
            confirmDialogItem = null
            confirm.destroy()
        }
        if (editHistoryDialogItem && !editHistoryDialogItem.visible) {
            var history = editHistoryDialogItem
            editHistoryDialogItem = null
            history.destroy()
        }
        if (eventSourceDialogItem && !eventSourceDialogItem.visible) {
            var source = eventSourceDialogItem
            eventSourceDialogItem = null
            source.destroy()
        }
    }

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

    function openContextMenu(x, y, alreadyInOverlaySpace) {
        var eventId = root.eventIdForActions()
        if (eventId === "" || root.isVirtualRow || root.isStateActivity
            || root.isCallEvent || root.isThreadKeyWait || root.readOnlyView)
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
    function openSenderProfileForRow(anchor) {
        if (!root.timelineView || !root.timelineView.openSenderProfile)
            return
        var userId = model.sender || ""
        if (userId.length === 0)
            return
        root.timelineView.openSenderProfile({
            userId: userId,
            displayName: model.senderDisplayName || "",
            avatarUrl: model.senderAvatarMxc || ""
        }, anchor || root.pointerAnchor())
    }
    function pointerAnchor() {
        var pos = rowHover.point.position
        var p = bubbleRow.mapToItem(Overlay.overlay, pos.x, pos.y)
        return { x: p.x, y: p.y, width: 1, height: 1 }
    }
    function openMessageLink(link) {
        if (link === "spoiler:toggle") {
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
                }, root.pointerAnchor())
            }
            return
        }
        var isMatrixLink =
            link.indexOf("matrix:") === 0
            || link.indexOf("matrix.to/#/") !== -1
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
        var helper = Qt.createQmlObject(
                    "import QtQuick; TextEdit { visible: false }", root)
        helper.text = value
        helper.selectAll()
        helper.copy()
        helper.destroy()
    }
    readonly property bool inThreadPanel:
        root.timelineView && root.timelineView.threadContext === true
    function beginReply(eventId) {
        var details = root.timelineModel.messageDetails(eventId)
        if (!details.eventId) return
        if (root.inThreadPanel) {
            app.thread.beginReply(eventId)
            return
        }
        var previewText = root.timelineModel.replyPreviewForEvent(eventId)
        app.composer.beginReply(eventId, details.senderName || details.senderId,
                                (previewText || "").substring(0, 80),
                                root.timelineModel.mediaKeyForEvent(eventId),
                                root.threadInline ? (details.threadRootId || "")
                                                  : "")
    }
    readonly property color rowHoverTint:
        Qt.alpha(AppTheme.textPrimary, AppTheme.dark ? 0.06 : 0.055)
    function replyInThread(eventId) {
        if (!eventId)
            return
        var details = root.timelineModel.messageDetails(eventId)
        var rootId = (details.threadRootId || "").length > 0
                     ? details.threadRootId : eventId
        root.showThread(rootId)
    }
    function itemTakesInput(c) {
        if (c.selectByMouse !== undefined) return true
        if (c.hoveredLink !== undefined && c.hoveredLink !== "") return true
        if (typeof c.clicked === "function") return true
        var kids = c.data
        for (var i = 0; kids && i < kids.length; ++i) {
            if (kids[i].tapCount !== undefined) return true
        }
        return false
    }
    function pointTakesInput(host, pos) {
        var item = host
        var x = pos.x
        var y = pos.y
        for (var depth = 0; depth < 32; ++depth) {
            var child = item.childAt(x, y)
            if (!child) return false
            var m = item.mapToItem(child, x, y)
            x = m.x
            y = m.y
            item = child
            if (root.itemTakesInput(child)) return true
        }
        return false
    }
    readonly property bool replyEligible:
        root.rowActionsEnabled && !root.readOnlyView
        && model.redacted !== true && model.isLocalEcho !== true
        && !root.isVirtualRow && !root.isStateActivity && !root.isCallEvent
        && !root.isThreadKeyWait && !!root.timelineModel
        && (model.eventId || "").length > 0
        && (model.eventId || "").indexOf("local:") !== 0

    function replyOnDoubleTap(host, pos) {
        var id = model.eventId || ""
        if (!root.replyEligible
                || root.timelineModel.messagePermalink(id).length === 0)
            return false
        if (root.pointTakesInput(host, pos))
            return false
        root.beginReply(id)
        return true
    }
    activeFocusOnTab: !isVirtualRow && !isStateActivity && !isCallEvent
                      && !isThreadKeyWait
    function focusNeighbourRow(forward) {
        var it = root
        for (var hops = 0; hops < 64; ++hops) {
            it = it.nextItemInFocusChain(forward)
            if (!it || it === root)
                return false
            if (it.objectName === "messageDelegateRoot") {
                if (it.parent !== root.parent)
                    return false
                if (!it.visible || !it.enabled)
                    continue
                it.forceActiveFocus(Qt.TabFocusReason)
                if (root.timelineView && root.timelineView.revealRow)
                    root.timelineView.revealRow(it)
                return true
            }
        }
        return false
    }
    Rectangle {
        objectName: "messageRowFocusRing"
        anchors.fill: parent
        visible: root.activeFocus
        radius: AppTheme.radiusSm
        color: "transparent"
        border.width: 2
        border.color: AppTheme.focusRing
        z: 1000
    }
    Keys.onPressed: (event) => {
        if (root.activeFocus
                && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                && (model.isImage === true || model.isSticker === true)
                && !root.isSvgImage && model.mediaSourceAvailable === true
                && root.timelineView && root.timelineView.openImage) {
            root.timelineView.openImage(model.mediaKey || "", model.mediaUrl)
            event.accepted = true
            return
        }
        if (root.activeFocus
                && (event.key === Qt.Key_Up || event.key === Qt.Key_Down)) {
            if (!(event.modifiers & (Qt.ControlModifier | Qt.AltModifier
                                     | Qt.ShiftModifier | Qt.MetaModifier))
                    && root.focusNeighbourRow(event.key === Qt.Key_Up)) {
                event.accepted = true
                return
            }
        }
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

    readonly property bool showsDecryptingSkeleton: {
        if (model.undecryptable !== true || model.redacted)
            return false
        var kind = model.errorKind || ""
        return kind !== "membership" && kind !== "device_trust"
               && kind !== "withheld"
    }

    readonly property var messageSegments: model.messageSegments || []
    readonly property var galleryItems: model.galleryItems || []
    readonly property bool isGallery: galleryItems.length > 1
    readonly property bool isSvgImage: model.isImage === true
        && (model.mediaMimetype || "").toLowerCase().indexOf("image/svg") === 0
    readonly property bool svgWithoutThumbnail:
        isSvgImage && model.mediaThumbAvailable !== true
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
    readonly property real uploadProgress:
        model.uploadProgress === undefined ? -1 : model.uploadProgress
    readonly property bool mediaCaptionBody: {
        if (!mediaRowBody) return false
        var body = (model.body || "").trim()
        var name = (model.mediaFilename || "").trim()
        return body.length > 0 && name.length > 0
               && body.toLowerCase() !== name.toLowerCase()
    }
    readonly property bool isEmoteRow:
        model.eventType === 1 && model.redacted !== true
        && model.undecryptable !== true && !mediaRowBody
    function emoteLead() {
        var who = model.senderDisplayName || model.sender || ""
        return "* " + who.replace(/&/g, "&amp;").replace(/</g, "&lt;")
                          .replace(/>/g, "&gt;") + " "
    }
    function withEmoteLead(html) {
        if (html.indexOf("<p>") === 0)
            return "<p>" + root.emoteLead() + html.substring(3)
        return root.emoteLead() + html
    }
    readonly property bool hasMessageSegments:
        messageSegments.length > 0
        && !isVirtualRow && !isStateActivity
        && !model.redacted && model.isPoll !== true
        && !showsDecryptingSkeleton
        && (!mediaRowBody || mediaCaptionBody)

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

    readonly property string replySenderKey: model.replyToSenderId || ""
    readonly property bool replyOnOwnBubble:
        bubbleMode && model.isOwn === true
    readonly property bool drawsOnOwnBubble: bubbleMode && model.isOwn === true
    readonly property color ownBubbleSecondaryInk:
        Qt.alpha(AppTheme.ownBubbleText, 0.85)
    readonly property color captionInk:
        drawsOnOwnBubble ? ownBubbleSecondaryInk : AppTheme.textSecondary
    readonly property color inlineLinkInk:
        drawsOnOwnBubble ? AppTheme.ownBubbleText : AppTheme.link
    readonly property color replySenderInk:
        replyOnOwnBubble ? AppTheme.ownBubbleText
                         : AppTheme.userColor(replySenderKey)
    readonly property color replyBodyInk:
        replyOnOwnBubble ? ownBubbleSecondaryInk : AppTheme.textSecondary

    readonly property bool fileCardRow:
        !root.isGallery
        && (root.svgWithoutThumbnail
            || (!model.isImage && model.isSticker !== true
                && model.isVideo !== true && model.isAudio !== true
                && model.isFile === true))
    readonly property var attachmentDownloads:
        root.fileCardRow
        && typeof app !== "undefined" && app && app.downloads
        && (model.mediaKey || "") !== ""
        ? app.downloads.items.filter(function(item) {
            return item.mediaKey === model.mediaKey
        }) : []
    Loader {
        id: attachmentNoticeLoader
        objectName: "attachmentDownloadNoticeLoader"
        z: 2
        active: root.fileCardRow && (model.mediaKey || "") !== ""
                && root.attachmentDownloads.length > 0
                && root.attachmentDownloads[root.attachmentDownloads.length - 1]
                       .risky !== true
        sourceComponent: AttachmentDownloadNotice {
            items: root.attachmentDownloads
            mediaKey: model.mediaKey || ""
            surface: root.timelineView && root.timelineView.downloadSurface
                     ? root.timelineView.downloadSurface : "main"
            eligible: root.rowOnScreen && mediaBox.visible && root.fileCardRow
                      && mediaLoader.item !== null
                      && (!root.timelineView
                          || root.timelineView.viewportOldestRow === undefined
                          || (index >= root.timelineView.viewportNewestRow
                              && index <= root.timelineView.viewportOldestRow))
            row: root
            attachment: mediaBox
            ownBubble: root.bubbleMode && model.isOwn === true
            contentItems: root.bubbleMode ? [bubble] : bubbleContent.children
            collisionItems: {
                var items = [metaRowLoader, previewLoader]
                var chips = reactionsLoader.item
                            ? reactionsLoader.item.children : []
                for (var i = 0; i < chips.length; ++i)
                    items.push(chips[i])
                return items
            }
            toolbar: root.messageActionsHidden ? null : messageActionBarLoader
            receipts: receiptStripLoader.row
        }
    }

    readonly property real receiptRailReserve:
        (!root.bubbleMode && receiptStripLoader.visible)
        ? Math.max(0, (bubble.x + bubble.width)
                      - Math.max(root.avatarGutterWidth,
                                 receiptStripLoader.width
                                 - receiptStripLoader.rowWidth)
                      + AppTheme.spacingXS)
        : 0

    readonly property bool ownBubbleReceiptFooter:
        root.bubbleMode && model.isOwn === true && !model.redacted
        && !root.isVirtualRow && !root.isStateActivity && !root.isCallEvent
        && !root.isThreadKeyWait && root.receiptsMode !== 2
        && (root.shownReactions.length === 0 || root.reactionsMode === 2)
    readonly property real ownBubbleReceiptBandHeight: 18

    readonly property var liveReactions: model.reactions || []
    property var shownReactions: []
    function refreshReactions() {
        var next = root.liveReactions
        if (JSON.stringify(next) !== JSON.stringify(shownReactions))
            shownReactions = next
    }
    onLiveReactionsChanged: refreshReactions()

    readonly property real bubbleReceiptInset:
        (root.bubbleMode && !root.ownBubbleReceiptFooter
         && model.isOwn !== true
         && receiptStripLoader.visible && receiptStripLoader.rowWidth > 0)
        ? receiptStripLoader.rowWidth + AppTheme.spacingXS
        : 0

    readonly property real contentInnerCap: {
        var avail = Math.max(1, bubbleRow.width - root.avatarGutterWidth)
        if (root.bubbleMode)
            avail = Math.max(60, avail - 40 - root.bubbleReceiptInset)
        return Math.max(1, avail - root.bubblePad * 2)
    }

    readonly property real bodyTextCap: root.bubbleMode
        ? Math.min(720, root.contentInnerCap)
        : root.contentInnerCap

    // Media cards keep their own controls in their top-right corner, so their
    // action bar sits above the row instead of over the card.
    readonly property bool liftsActionBar:
        root.mediaRowBody && !messageActionBarLoader.leftOfOwnBubble
    readonly property bool receiptBandMeetsBar:
        (layout.height - bubbleRow.y - receiptStripLoader.rowHeight)
        < (root.liftsActionBar ? AppTheme.spacingXS
                                 + messageActionBarLoader.headerPush : 33)
    readonly property real actionBarReceiptReserve:
        (!root.ownBubbleReceiptFooter && receiptStripLoader.visible
         && receiptStripLoader.rowWidth > 0
         && root.receiptBandMeetsBar)
        ? receiptStripLoader.rowWidth + AppTheme.spacingXS
        : 0

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
            return Qt.locale(AppLocale.name).toString(ts, "dddd")
        return Qt.locale(AppLocale.name).toString(
            ts, ts.getFullYear() === now.getFullYear() ? "d MMMM"
                                                       : "d MMMM yyyy")
    }

    property var preview: ({ state: "none" })
    readonly property bool previewDismissed:
        root.preview ? root.preview.dismissed === true : false
    readonly property bool roomEncrypted:
        root.timelineView ? root.timelineView.roomEncrypted === true : false
    function refreshPreview() {
        if (isVirtualRow || isStateActivity || isCallEvent || isThreadKeyWait
            || model.redacted || model.isImage || model.isFile
            || model.isPoll === true
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
        refreshReactions()
    }
    onActionKeyChanged: {
        refreshHeightSeed()
        refreshPreview()
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

    Loader {
        id: virtualRowLoader
        active: root.isVirtualRow
        visible: active
        width: parent.width
        sourceComponent: Item {
            id: virtualRow
            implicitHeight: unreadDivider.visible ? 30
                            : virtualLabel.active
                            ? Math.max(30, virtualLabel.implicitHeight
                                           + AppTheme.spacingS * 2)
                            : 0
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
                anchors.leftMargin: root.avatarGutterWidth
                anchors.rightMargin: AppTheme.spacingS
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
                anchors.leftMargin: root.avatarGutterWidth
                anchors.rightMargin: AppTheme.spacingS
                spacing: AppTheme.spacingM
                visible: root.isVirtualRow && model.eventType === 8
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: AppTheme.unreadBadge
                }
                Label {
                    objectName: "unreadDividerLabel"
                    text: qsTr("New messages")
                    color: AppTheme.unreadDividerInk
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
    }

    Loader {
        id: callEventRow
        objectName: "callEventRow"
        active: root.isCallEvent
        visible: active
        y: AppTheme.spacingS
        width: parent.width
        sourceComponent: CallEventDelegate {
            roomId: root.previewRoomId
            actorUserId: model.sender || ""
            actorName: model.senderDisplayName || ""
            actorAvatarMxc: model.senderAvatarMxc || ""
            sentence: model.callEventText || ""
            isLatestCallRow: !root.timelineModel
                             || !root.timelineModel.latestCallEventId
                             || root.timelineModel.latestCallEventId === (model.eventId || "")
            video: model.callIsVideo === true
            declinedCount: model.callDeclinedCount || 0
            timestamp: model.timestamp
            onScreen: root.rowOnScreen
        }
    }

    Loader {
        id: threadKeyWaitRow
        objectName: "threadKeyWaitRow"
        active: root.isThreadKeyWait
        visible: active
        y: AppTheme.spacingS
        x: root.avatarGutterWidth
        width: parent.width - x
        sourceComponent: RowLayout {
            id: keyWaitLine
            spacing: AppTheme.spacingS
            readonly property string rootId: model.threadRootId || ""
            Icon {
                name: "forum"
                size: 14
                color: AppTheme.textMuted
            }
            Label {
                objectName: "threadKeyWaitText"
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: qsTr("A reply in a thread is waiting for keys")
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                font.italic: true
            }
            Label {
                objectName: "threadKeyWaitOpen"
                text: qsTr("Open thread")
                color: root.inlineLinkInk
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                font.underline: true
                visible: keyWaitLine.rootId !== ""
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    enabled: root.rowActionsEnabled
                    onClicked: root.showThread(keyWaitLine.rootId)
                }
            }
            Label {
                objectName: "threadKeyWaitRetry"
                text: qsTr("Retry decryption")
                color: root.inlineLinkInk
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                font.underline: true
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    enabled: root.rowActionsEnabled
                    onClicked: root.timelineModel.retryDecryption()
                }
            }
        }
    }

    Loader {
        id: stateActivity
        active: root.isStateActivity && model.stateGroupLeader === true
        visible: item !== null && item.entryCount > 0
        width: parent.width
        sourceComponent: RoomActivityDelegate {
            objectName: "stateActivityGroup"
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
    }

    Rectangle {
        anchors.fill: parent
        visible: root.rowSelected
        color: AppTheme.messageHighlight
        opacity: 0.55
        z: -1
    }
    Loader {
        active: root.selectionMode && root.rowSelectable
        anchors.left: parent.left
        anchors.leftMargin: 2
        anchors.verticalCenter: parent.verticalCenter
        z: 20
        sourceComponent: Rectangle {
            objectName: "messageSelectionCheck"
            width: 18; height: 18; radius: 9
            color: root.rowSelected ? AppTheme.accent : AppTheme.background
            border.width: 1
            border.color: root.rowSelected ? AppTheme.accent : AppTheme.borderStrong
            Icon {
                anchors.centerIn: parent
                visible: root.rowSelected
                name: "check"
                size: 12
                color: AppTheme.accentText
            }
        }
    }
    TapHandler {
        enabled: root.selectionMode && root.rowSelectable
        grabPermissions: PointerHandler.CanTakeOverFromAnything
        onTapped: root.toggleSelectionForThisRow()
    }

    Rectangle {
        id: rowHighlight
        readonly property bool navigationLanded:
            root.navigationHighlightId === (model.eventId || "")
        readonly property bool wanted:
            !root.isVirtualRow && !root.isStateActivity && !root.isCallEvent
            && !root.isThreadKeyWait
            && (rowHover.hovered || root.actionsPinned || navigationLanded)
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
        color: navigationLanded ? AppTheme.messageHighlight : root.rowHoverTint
        radius: AppTheme.radiusMd
        z: 0
    }

    ColumnLayout {
        id: layout
        visible: !root.isVirtualRow && !root.isStateActivity && !root.isCallEvent
                 && !root.isThreadKeyWait
        y: root.messageTopSpacing
        x: root.selectionMode && root.rowSelectable ? root.selectionGutterWidth : 0
        width: parent.width - x
        spacing: 2
        z: 1

        Item {
            id: bubbleRow
            objectName: "messagePresentationRow"
            Layout.fillWidth: true
            implicitHeight: Math.max(avatarSlot.implicitHeight,
                                     bubble.implicitHeight)
                            + (root.ownBubbleReceiptFooter
                               ? AppTheme.spacingXS
                                 + root.ownBubbleReceiptBandHeight : 0)

            HoverHandler {
                id: rowHover
                objectName: "messageRowHover"
                enabled: !root.rowHoverSuspended
                onHoveredChanged: {
                    if (!root.timelineView || root.actionKey === "")
                        return
                    if (hovered) {
                        if (root.transientOwnerBlocks)
                            return
                        root.timelineView.hoveredActionsKey = root.actionKey
                    } else if (!barHover.hovered) {
                        root.releaseHoveredActions()
                    }
                }
            }
            TapHandler {
                objectName: "rowEmptySideTap"
                enabled: root.rowActionsEnabled
                acceptedButtons: Qt.LeftButton
                onTapped: (eventPoint) => {
                    var pos = eventPoint.position
                    var skip = [bubble, avatarSlot.visible ? avatarSlot : null,
                                receiptStripLoader.row,
                                messageActionBarLoader.visible
                                    ? messageActionBarLoader : null]
                    for (var i = 0; i < skip.length; ++i) {
                        var b = skip[i]
                        if (!b) continue
                        var m = bubbleRow.mapToItem(b, pos.x, pos.y)
                        if (m.x >= 0 && m.x <= b.width
                            && m.y >= 0 && m.y <= b.height)
                            return
                    }
                    root.toggleActionsPin()
                }
            }
            TapHandler {
                acceptedButtons: Qt.RightButton
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
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
                implicitHeight: root.showsIdentity && !root.smallAvatarMode
                                && !root.gutterAvatarsHidden
                                ? 34 : bodyLoader.item ? bodyLoader.item.implicitHeight
                                                       : emptyBodyLine.height
                // An empty body line's height, kept by a row that builds no body.
                FontMetrics {
                    id: emptyBodyLine
                    font: bodyLoader.bodyFont
                }

                Loader {
                    anchors.top: parent.top
                    active: root.showsIdentity && !root.compactMode
                            && !root.smallAvatarMode
                            && !root.gutterAvatarsHidden
                            && !(root.bubbleMode && model.isOwn === true)
                    opacity: root.revealOpacity(root.avatarsMode)
                    enabled: opacity > 0
                    sourceComponent: Avatar {
                        objectName: "senderAvatar"
                        onScreen: root.rowOnScreen
                        size: 32
                        mxc: model.senderAvatarMxc || ""
                        name: model.senderDisplayName || model.senderInitials
                        colorKey: model.sender || ""
                        Accessible.name: qsTr("Avatar for %1").arg(
                                             model.senderDisplayName || model.sender)

                        TapHandler {
                            enabled: root.rowActionsEnabled
                            acceptedButtons: Qt.LeftButton
                            onTapped: root.openSenderProfileForRow(parent)
                        }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                    }
                }

                Loader {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.rightMargin: 5
                    active: !root.showsIdentity && rowHover.hovered
                            && !root.selectionMode
                            && !root.compactMode && !root.smallAvatarMode
                            && !root.gutterAvatarsHidden
                    sourceComponent: Label {
                        objectName: "continuationTimestamp"
                        text: Qt.formatDateTime(model.timestamp,
                                                app.settings.clockTimeFormat)
                        horizontalAlignment: Text.AlignRight
                        color: AppTheme.timestampInk
                        font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                        wrapMode: Text.NoWrap
                        fontSizeMode: Text.HorizontalFit
                        minimumPixelSize: 6
                        Accessible.name: qsTr("Sent at %1").arg(text)
                    }
                }
            }

            Rectangle {
                id: bubble
                objectName: "messageContentColumn"
                x: root.bubbleMode && model.isOwn === true
                   ? Math.max(root.avatarGutterWidth,
                              parent.width - width - root.bubbleReceiptInset)
                   : root.avatarGutterWidth
                width: root.bubbleMode
                       ? Math.min(bubbleContent.implicitWidth + root.bubblePad * 2,
                                  Math.max(60, parent.width
                                               - root.avatarGutterWidth - 40
                                               - root.bubbleReceiptInset))
                       : Math.max(1, parent.width - root.avatarGutterWidth)
                height: implicitHeight
                implicitHeight: bubbleContent.implicitHeight + root.bubblePad * 2
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

                readonly property bool mentionBarVisible:
                    !root.bubbleMode
                    && (model.roomMentionHighlighted === true
                        || (model.mentionsMe === true
                            && (model.pushHighlighted === true
                                || model.pushVerdictKnown !== true)))
                Rectangle {
                    visible: bubble.mentionBarVisible
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.topMargin: 1
                    anchors.bottomMargin: 1
                    width: 3
                    radius: 1
                    color: AppTheme.bolt
                }

                TapHandler {
                    enabled: root.rowActionsEnabled
                    acceptedButtons: Qt.LeftButton
                    function overlayAt(pos) {
                        var bands = [
                            (model.edited === true && metaRowLoader.item)
                                ? metaRowLoader.item.statusItem : null,
                            receiptStripLoader.row,
                            replyLoader.visible ? replyLoader : null,
                            identityLoader.visible ? identityLoader : null
                        ]
                        for (var i = 0; i < bands.length; ++i) {
                            var b = bands[i]
                            if (!b) continue
                            var m = bubble.mapToItem(b, pos.x, pos.y)
                            if (m.x >= 0 && m.x <= b.width
                                && m.y >= 0 && m.y <= b.height)
                                return true
                        }
                        var bar = messageActionBarLoader
                        if (bar.visible && bar.item) {
                            var mb = bubble.mapToItem(bar, pos.x, pos.y)
                            if (mb.x >= 0 && mb.x <= bar.width
                                && mb.y >= 0 && mb.y <= bar.height
                                && root.pointTakesInput(bar, mb))
                                return true
                        }
                        return false
                    }
                    onTapped: (eventPoint) => {
                        if (overlayAt(eventPoint.position))
                            return
                        root.toggleActionsPin()
                    }
                    onDoubleTapped: (eventPoint, button) => {
                        if (!overlayAt(eventPoint.position))
                            root.replyOnDoubleTap(bubble, eventPoint.position)
                    }
                }

                ColumnLayout {
                    id: bubbleContent
                    anchors.fill: parent
                    anchors.margins: root.bubblePad
                    anchors.leftMargin: root.bubblePad
                                        + (bubble.mentionBarVisible ? 8 : 0)
                    spacing: 2

                    Loader {
                        id: identityLoader
                        active: root.showsIdentity
                                && !(root.bubbleMode && model.isOwn === true)
                        visible: active
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
                        Loader {
                            active: root.smallAvatarMode && root.avatarsMode !== 2
                            visible: active
                            opacity: root.revealOpacity(root.avatarsMode)
                            enabled: opacity > 0
                            Layout.alignment: Qt.AlignVCenter
                            sourceComponent: Avatar {
                                objectName: "senderHeaderAvatar"
                                onScreen: root.rowOnScreen
                                size: root.headerAvatarSize
                                mxc: model.senderAvatarMxc || ""
                                name: model.senderDisplayName
                                      || model.senderInitials
                                colorKey: model.sender || ""
                                Accessible.name: qsTr("Avatar for %1").arg(
                                    model.senderDisplayName || model.sender)
                                TapHandler {
                                    enabled: root.rowActionsEnabled
                                    acceptedButtons: Qt.LeftButton
                                    onTapped: root.openSenderProfileForRow(parent)
                                }
                                HoverHandler {
                                    cursorShape: Qt.PointingHandCursor
                                }
                            }
                        }
                        Label {
                            id: nameLabel
                            objectName: "senderName"
                            text: model.senderDisplayName || model.sender
                            textFormat: Text.PlainText
                            color: AppTheme.userColor(model.sender || "")
                            font.pixelSize: AppTheme.scaled(
                                root.compactMode || root.inThreadPanel
                                ? 13 : AppTheme.fontSizeM)
                            font.weight: Font.DemiBold
                            elide: Label.ElideRight
                            // fillWidth makes the name the one item the header
                            // shrinks when the Loader's cap bites. Without it
                            // a Layout item is Fixed at its implicit width, so
                            // a long name never elided and pushed the
                            // timestamp off the row (break-it B114). The
                            // header hugs its content, so the name never grows
                            // past its own text.
                            Layout.fillWidth: true
                            Layout.maximumWidth: 320
                            Accessible.name: qsTr("Sender: %1").arg(text)
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
                                onTapped: root.openSenderProfileForRow(parent)
                            }
                        }
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
                                font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                                elide: Label.ElideMiddle
                            }
                        }
                        Label {
                            objectName: "senderTimestamp"
                            text: Qt.formatDateTime(model.timestamp,
                                                    app.settings.clockTimeFormat)
                            color: AppTheme.timestampInk
                            font.pixelSize: AppTheme.scaled(10)
                            Accessible.name: qsTr("Sent at %1").arg(text)
                        }
                        }
                    }

                    Loader {
                        id: threadTagLoader
                        objectName: "threadTagLoader"
                        active: root.threadInline && root.rowThreadId !== ""
                                && model.redacted !== true
                        visible: active
                        Layout.fillWidth: false
                        Layout.maximumWidth: root.contentInnerCap
                        sourceComponent: AbstractButton {
                            id: threadTag
                            objectName: "threadTag"
                            readonly property bool narrowed:
                                (root.timelineModel.threadFilterRootId || "")
                                === root.rowThreadId
                            readonly property color ink:
                                AppTheme.userColor(root.rowThreadId)
                            enabled: root.rowActionsEnabled
                            focusPolicy: Qt.NoFocus
                            hoverEnabled: true
                            padding: 0
                            leftPadding: AppTheme.spacingXS
                            rightPadding: AppTheme.spacingS
                            implicitHeight: AppTheme.scaled(18)
                            Accessible.name: text
                            text: narrowed ? qsTr("Back to the room")
                                           : qsTr("Open this thread")
                            ToolTip.text: text
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            background: Rectangle {
                                radius: height / 2
                                color: threadTag.hovered
                                       ? Qt.alpha(threadTag.ink, 0.22)
                                       : Qt.alpha(threadTag.ink, 0.12)
                            }
                            contentItem: Row {
                                spacing: AppTheme.spacingXS
                                Icon {
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: threadTag.narrowed ? "close" : "forum"
                                    size: 12
                                    color: threadTag.ink
                                }
                                Label {
                                    objectName: "threadTagLabel"
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: (model.threadRootId || "").length > 0
                                          ? qsTr("in thread") : qsTr("thread")
                                    color: threadTag.ink
                                    font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                                    font.strikeout: threadTag.narrowed
                                }
                            }
                            onClicked: {
                                if (narrowed)
                                    root.timelineModel.threadFilterRootId = ""
                                else
                                    root.showThread(root.rowThreadId)
                            }
                        }
                    }

                    Loader {
                        id: replyLoader
                        active: !!model.replyToEventId && model.replyToEventId.length > 0
                                && !model.redacted
                        visible: active
                        Layout.fillWidth: true
                        Layout.maximumWidth: root.contentInnerCap
                        Layout.bottomMargin: 2
                        sourceComponent: Rectangle {
                            id: replyBox
                            objectName: "replyNavigationTarget"
                            readonly property int barWidth: 2
                            implicitWidth: replyRowWrap.implicitWidth
                                           + replyBox.barWidth + 16
                            implicitHeight: replyRowWrap.implicitHeight + 8
                            color: replyHover.hovered
                                   ? Qt.alpha(root.replySenderInk, 0.12)
                                   : "transparent"
                            Behavior on color { ColorAnimation { duration: 90 } }
                            radius: AppTheme.radiusSm
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
                                onTapped: root.navigateToReplyTarget()
                            }
                            Rectangle {
                                width: replyBox.barWidth
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                anchors.topMargin: 1
                                anchors.bottomMargin: 1
                                radius: 1
                                color: root.replySenderInk
                                opacity: replyHover.hovered ? 1.0 : 0.8
                                Behavior on opacity { NumberAnimation { duration: 90 } }
                            }
                            RowLayout {
                                id: replyRowWrap
                                anchors.left: parent.left
                                anchors.right: parent.right
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
                                Image {
                                    id: replyThumb
                                    objectName: "replyQuoteThumbnail"
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
                                    // Visibility reads status, which changes synchronously
                                    // inside source loading. Never feed it back into source.
                                    source: root.mediaPixmapRetained
                                            && app.mediaBridge.supported
                                            && bridgeSource.length > 0
                                            ? bridgeSource + "|shape:round:160" : ""
                                    Connections {
                                        target: app.mediaBridge
                                        enabled: (model.replyToMediaKey || "")
                                                     .length > 0
                                        function onMediaCached(key) {
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
                                    textFormat: Text.PlainText
                                    color: root.replySenderInk
                                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                                    font.weight: AppTheme.weightStrong
                                    elide: Label.ElideRight
                                    maximumLineCount: 1
                                    Layout.maximumWidth: AppTheme.scaled(180)
                                }
                                Label {
                                    objectName: "replyQuoteBody"
                                    text: model.replyToPreview
                                          || (model.replyToRedacted === true
                                              ? qsTr("Original message removed") : "")
                                          || root.replyKindLabel()
                                          || qsTr("(original message not loaded)")
                                    textFormat: Text.PlainText
                                    color: root.replyBodyInk
                                    opacity: replyHover.hovered
                                             || root.replyOnOwnBubble ? 1.0 : 0.85
                                    Behavior on opacity { NumberAnimation { duration: 90 } }
                                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                                    elide: Label.ElideRight
                                    Layout.fillWidth: true
                                    maximumLineCount: 1
                                }
                            }
                        }
                    }

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
                        implicitWidth: mediaLoader.item
                                       ? mediaLoader.item.implicitWidth : 0
                        implicitHeight: mediaLoader.item
                                        ? mediaLoader.item.implicitHeight : 0
                        Loader {
                            id: mediaLoader
                            active: !root.mediaEmbedCollapsed
                            anchors.left: parent.left
                            width: Math.min(root.contentInnerCap,
                                            item ? item.implicitWidth : 0)
                            sourceComponent: root.isGallery ? galleryComponent
                                            : root.svgWithoutThumbnail
                                              ? fileComponent
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

                    Loader {
                        id: pollLoader
                        active: model.isPoll === true
                        visible: active
                        Layout.alignment: Qt.AlignLeft
                        Layout.preferredWidth: item ? item.implicitWidth : 0
                        Layout.maximumWidth: root.contentInnerCap
                        sourceComponent: pollComponent
                    }

                    Loader {
                        id: locationLoader
                        active: model.isLocation === true
                        visible: active
                        Layout.alignment: Qt.AlignLeft
                        Layout.preferredWidth: item ? item.implicitWidth : 0
                        Layout.maximumWidth: root.contentInnerCap
                        sourceComponent: locationComponent
                    }

                    Loader {
                        id: bodyLoader
                        objectName: "messageBodyLoader"
                        readonly property bool isMediaRow: root.mediaRowBody
                        readonly property bool isMediaCaption:
                            root.mediaCaptionBody
                        readonly property int emojiOnlyCount:
                            (!app.settings.largeEmoji
                             || model.redacted || model.isPoll === true
                             || model.undecryptable === true || isMediaRow
                             || root.isEmoteRow)
                            ? 0
                            : app.emojiCatalog.emojiOnlySequenceCount(
                                  model.body || "")
                        readonly property bool bigEmoji:
                            emojiOnlyCount >= 1 && emojiOnlyCount <= 3
                        readonly property string bodyText: {
                            if (root.hasMessageSegments) return ""
                            if (model.redacted) {
                                return model.deletedGroupCount > 1
                                    ? qsTr("%n message(s) deleted",
                                           "collapsed run of redactions",
                                           model.deletedGroupCount)
                                    : qsTr("Message deleted")
                            }
                            if (model.isPoll === true) return ""
                            if (model.isLocation === true) return ""
                            if (isMediaRow && !isMediaCaption) return ""
                            var fb = model.formattedBody
                            var html = (fb && fb.length > 0)
                                ? fb
                                : app.linkPreviews.linkifiedBody(
                                      model.body || "")
                            if (model.mentionsRoom === true && root.timelineModel)
                                html = root.timelineModel.markRoomMention(
                                           html, "" + AppTheme.roomMentionBadge,
                                           "" + AppTheme.mentionBadgeInk)
                            if (root.isEmoteRow)
                                html = root.withEmoteLead(html)
                            return root.highlightSearchMatches(
                                        root.inkLinksForOwnBubble(html),
                                        root.searchHighlight,
                                        root.isCurrentSearchHit)
                        }
                        active: bodyText.length > 0
                        visible: active && !root.showsDecryptingSkeleton
                        Layout.maximumWidth: bubbleRow.width > 8
                                             ? Math.max(1, root.bodyTextCap
                                                   - root.receiptRailReserve)
                                             : 560
                        Layout.rightMargin: root.receiptRailReserve
                        Layout.fillWidth: item ? item.rightToLeft : false
                        // TextEdit does not inherit the Controls font. A whole
                        // font so the colour emoji face is a real per-character
                        // fallback (QML has no families list, and Qt 6.8's
                        // automatic fallback picks a monochrome face). Size and
                        // italic pass through because assigning `font` replaces
                        // them.
                        readonly property font bodyFont: app.textFontWithEmoji(
                                  AppTheme.uiFont,
                                  bodyLoader.bigEmoji
                                      ? AppTheme.scaled(root.compactMode
                                                        || root.inThreadPanel ? 48 : 60)
                                      : AppTheme.scaled(
                                            bodyLoader.isMediaCaption ? 12
                                            : root.compactMode || root.inThreadPanel
                                              ? 13 : AppTheme.fontSizeM),
                                  model.redacted || model.undecryptable === true
                                  || root.isEmoteRow)
                        sourceComponent: TextEdit {
                            id: bodyLabel
                            objectName: "messageBody"
                            text: bodyLoader.bodyText
                            font: bodyLoader.bodyFont
                            readonly property bool rightToLeft: bodyDirection.rightToLeft
                            color: model.undecryptable === true
                                   ? (root.drawsOnOwnBubble
                                      ? root.ownBubbleSecondaryInk : AppTheme.muted)
                                   : bodyLoader.isMediaCaption ? root.captionInk
                                   : root.drawsOnOwnBubble
                                     ? AppTheme.ownBubbleText : AppTheme.otherBubbleText
                            // No lineHeight: lineHeight/lineHeightMode are
                            // QQuickText properties, and assigning them on a
                            // TextEdit is a load-time error. Body leading would
                            // need a `line-height` from MessageHtml::sanitize or a
                            // move off TextEdit.
                            wrapMode: Text.Wrap
                            readOnly: true
                            textFormat: Text.RichText
                            clip: (model.formattedBody || "").indexOf("<table") >= 0
                            selectByMouse: !/spoiler:toggle[^>]*><span[^>]*;color:/.test(
                                               model.formattedBody || "")
                            selectionColor: AppTheme.textSelection
                            selectedTextColor: AppTheme.text
                            Accessible.name: model.body || ""

                            ParagraphDirection {
                                id: bodyDirection
                                document: bodyLabel.textDocument
                            }

                            onLinkActivated: function(link) {
                                root.openMessageLink(link)
                            }

                            Loader {
                                anchors.fill: parent
                                active: model.undecryptable === true
                                sourceComponent: Item {
                                    HoverHandler { id: undecryptHover }
                                    ToolTip {
                                        visible: undecryptHover.hovered
                                        delay: 400
                                        text: qsTr(
                                            "Missing room key. Restore your recovery key " +
                                            "in Settings, or wait for another verified " +
                                            "device to share the key.")
                                    }
                                }
                            }
                        }
                    }

                    Loader {
                        id: segmentsLoader
                        objectName: "messageSegments"
                        readonly property bool ownMessage: model.isOwn === true
                        readonly property string plainBody: model.body || ""
                        active: root.hasMessageSegments
                        visible: active
                        Layout.fillWidth: true
                        Layout.maximumWidth: bubbleRow.width > 8
                                             ? root.bodyTextCap
                                             : 560
                        Layout.rightMargin: root.receiptRailReserve
                        readonly property real segmentCap:
                            Math.max(80, Math.min(root.bodyTextCap,
                                                  root.contentInnerCap - 8))
                        sourceComponent: ColumnLayout {
                            spacing: 4
                            Accessible.role: Accessible.StaticText
                            Accessible.name: segmentsLoader.plainBody
                            Repeater {
                                model: root.messageSegments
                                delegate: Item {
                                    id: segmentRow
                                    objectName: "messageSegmentRow"
                                    required property var modelData
                                    readonly property bool isCode:
                                        modelData && modelData.kind === 1
                                    Layout.fillWidth: true
                                    implicitWidth: segmentLoader.implicitWidth
                                    implicitHeight: segmentLoader.implicitHeight
                                    Loader {
                                        id: segmentLoader
                                        anchors.left: parent.left
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
                                            id: segmentText
                                            objectName: "messageSegmentText"
                                            text: root.highlightSearchMatches(
                                                      root.inkLinksForOwnBubble(
                                                          segmentRow.modelData.text
                                                          || ""),
                                                      root.searchHighlight,
                                                      root.isCurrentSearchHit)
                                            color: root.mediaCaptionBody
                                                   ? root.captionInk
                                                   : root.bubbleMode
                                                     && segmentsLoader.ownMessage
                                                   ? AppTheme.ownBubbleText
                                                   : AppTheme.otherBubbleText
                                            font.family: AppTheme.uiFont
                                            font.pixelSize: AppTheme.scaled(
                                                root.mediaCaptionBody ? 12
                                                : root.compactMode
                                                  || root.inThreadPanel
                                                ? 13 : AppTheme.fontSizeM)
                                            wrapMode: Text.Wrap
                                            readOnly: true
                                            textFormat: Text.RichText
                                            clip: (segmentRow.modelData.text
                                                   || "").indexOf("<table") >= 0
                                            selectByMouse: !/spoiler:toggle[^>]*><span[^>]*;color:/.test(
                                                               segmentRow.modelData.text || "")
                                            selectionColor: AppTheme.textSelection
                                            selectedTextColor: AppTheme.text
                                            onLinkActivated: function(link) {
                                                root.openMessageLink(link)
                                            }
                                            ParagraphDirection {
                                                document: segmentText.textDocument
                                            }
                                        }
                                    }
                                    Component {
                                        id: codeSegment
                                        CodeBlock {
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

                    Loader {
                        active: root.showsDecryptingSkeleton
                        visible: active
                        Layout.fillWidth: true
                        Layout.topMargin: 2
                        sourceComponent: ColumnLayout {
                            objectName: "decryptingSkeleton"
                            spacing: 5
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
                    }

                    Loader {
                        active: model.undecryptable === true
                        visible: active
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredWidth: item ? item.naturalWidth : 0
                        sourceComponent: Flow {
                            id: utdRow
                            objectName: "undecryptableActions"
                            readonly property real naturalWidth: {
                                var w = 0
                                var n = 0
                                for (var i = 0; i < children.length; ++i) {
                                    if (!children[i].visible)
                                        continue
                                    w += children[i].implicitWidth
                                    ++n
                                }
                                return w + Math.max(0, n - 1) * spacing
                            }
                            spacing: AppTheme.spacingS
                            Label {
                                text: {
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
                                color: root.drawsOnOwnBubble
                                       ? root.ownBubbleSecondaryInk
                                       : AppTheme.textMuted
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                                font.italic: true
                            }
                            Label {
                                text: qsTr("Retry decryption")
                                color: root.inlineLinkInk
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
                                color: root.inlineLinkInk
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
                    }

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
                            iconName: root.previewEmbedIcon()
                            kindLabel: root.previewEmbedKind()
                            detailText: root.previewEmbedDetail()
                            onToggleRequested: root.toggleEmbedExpanded()
                        }
                    }
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
                                && !(root.collapseEmbedsSetting
                                     && root.preview.state === "loaded"
                                     && !root.embedExpanded)
                        visible: active
                        sourceComponent: root.preview.state === "loaded"
                                         && root.preview.isDirectMedia === true
                                         && root.linkMediaInline
                                         ? (root.preview.mediaKind === "image"
                                            && root.preview.mediaHeld === true
                                            ? directMediaPreviewComponent
                                            : linkMediaCardComponent)
                                         : linkPreviewComponent
                    }

                    Loader {
                        objectName: "previewRestoreLoader"
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
                                enabled: root.rowActionsEnabled
                    onTapped: app.linkPreviews.restorePreviewForEvent(
                                    root.previewRoomId, root.actionKey)
                            }
                        }
                    }

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

                    Loader {
                        id: metaRowLoader
                        active: (model.isOwn === true
                                 && (model.status === 1 || model.status === 2))
                                || model.edited === true || model.isThreadRoot === true
                        Layout.fillWidth: true
                        sourceComponent: RowLayout {
                            id: metaRow
                            readonly property Item statusItem: metaLabel
                            spacing: AppTheme.spacingXS
                            Loader {
                                id: metaLabel
                                active: (model.isOwn === true
                                         && (model.status === 1
                                             || model.status === 2))
                                        || model.edited === true
                                visible: active
                                sourceComponent: Label {
                                    objectName: "messageStatusText"
                                    text: {
                                        var ts = Qt.formatDateTime(
                                            model.timestamp,
                                            app.settings.clockTimeFormat)
                                        if (model.isOwn && model.status === 1) {
                                            if (root.uploadProgress >= 0)
                                                return qsTr("%1 • sending… %2%")
                                                    .arg(ts)
                                                    .arg(Math.round(
                                                        root.uploadProgress * 100))
                                            return qsTr("%1 • sending…").arg(ts)
                                        }
                                        if (model.isOwn && model.status === 2) {
                                            var why = model.sendErrorCategory || ""
                                            if (why === "network")
                                                return qsTr("%1 • not sent: connection problem")
                                                    .arg(ts)
                                            if (why === "rejected")
                                                return qsTr("%1 • not sent: the server refused it")
                                                    .arg(ts)
                                            return qsTr("%1 • failed").arg(ts)
                                        }
                                        if (model.edited) return qsTr("(edited)")
                                        return ""
                                    }
                                    color: root.drawsOnOwnBubble
                                           ? root.ownBubbleSecondaryInk
                                           : AppTheme.textMuted
                                    font.pixelSize: AppTheme.scaled(10)
                                    font.underline: editedHover.hovered
                                                    && model.edited === true
                                                    && !(model.isOwn && model.status !== 1
                                                         && model.status !== undefined
                                                         && model.status === 2)
                                    HoverHandler {
                                        id: editedHover
                                        enabled: model.edited === true
                                                 && root.rowActionsEnabled
                                        cursorShape: Qt.PointingHandCursor
                                    }
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
                            Label {
                                visible: model.isOwn && model.status === 2
                                text: qsTr("Retry")
                                color: root.inlineLinkInk
                                font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                                font.underline: true
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                            onClicked: root.timelineModel.retrySend(
                                                   root.sourceModelRow(index))
                                }
                            }
                            Label {
                                objectName: "cancelSendLink"
                                visible: model.isOwn === true
                                         && (model.status === 1
                                             || model.status === 2)
                                         && root.canCancelSendAt(index)
                                text: qsTr("Cancel")
                                color: root.inlineLinkInk
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
                            Loader {
                                active: model.isThreadRoot === true
                                visible: active
                                Layout.fillWidth: true
                                Layout.maximumWidth: item ? item.implicitWidth : 0
                                sourceComponent: ThreadSummaryCard {
                                id: threadSummaryCard
                                objectName: "threadSummaryCard"
                                replyCount: model.threadReplyCount !== undefined
                                            ? model.threadReplyCount : -1
                                latestSender: model.threadLatestSenderDisplayName || ""
                                latestSenderId: model.threadLatestSender || ""
                                latestPreview: model.threadLatestPreview || ""
                                latestKind: model.threadLatestKind || "text"
                                latestAvatarMxc: model.threadLatestSenderAvatarMxc || ""
                                latestTimestamp: model.threadLatestTimestamp
                                unread: model.threadUnread === true
                                onActivated: root.showThread(
                                    root.eventIdForActions())

                                readonly property string rootId:
                                    model.isThreadRoot === true
                                        ? root.eventIdForActions() : ""
                                readonly property string roomId:
                                    typeof app !== "undefined" ? app.currentRoomId : ""

                                function refreshParticipants() {
                                    participants = (rootId !== "" && roomId !== "")
                                        ? app.threads.participants(roomId, rootId)
                                        : []
                                }
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
            }

            Loader {
                id: messageActionBarLoader
                objectName: "messageActionBarLoader"
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: restY + headerPush
                readonly property real restY: root.liftsActionBar
                    ? AppTheme.spacingXS - height : -3
                property real headerPush: 0
                function refreshHeaderPush() {
                    var view = root.timelineView
                    var overlay = Overlay.overlay
                    if (!view || !view.mapToItem || !overlay || !visible) {
                        headerPush = 0
                        return
                    }
                    var viewTop = Math.min(view.mapToItem(overlay, 0, 0).y,
                                           view.mapToItem(overlay, 0, view.height).y)
                    var rowTop = bubbleRow.mapToItem(overlay, 0, 0).y
                    headerPush = Math.max(0, Math.min(
                        viewTop - rowTop - restY + AppTheme.spacing2,
                        bubbleRow.height - height))
                }
                onVisibleChanged: refreshHeaderPush()
                onHeightChanged: refreshHeaderPush()
                // Over a lifted bar the pointer is outside its row. On the Loader
                // so it exists before the bar does: Qt caches whether an item's
                // hover targets all lie inside it.
                HoverHandler {
                    id: barHover
                    blocking: true
                    onHoveredChanged: {
                        if (!hovered)
                            Qt.callLater(function() {
                                if (!rowHover.hovered && !barHover.hovered)
                                    root.releaseHoveredActions()
                            })
                    }
                }
                readonly property real barWidth:
                    item ? item.implicitWidth : 200
                readonly property bool leftOfOwnBubble:
                    root.bubbleMode && model.isOwn === true
                    && bubble.x - barWidth - AppTheme.spacingXS
                       >= root.avatarGutterWidth
                anchors.rightMargin: leftOfOwnBubble
                    ? bubbleRow.width - bubble.x + AppTheme.spacingXS
                    : AppTheme.scrollbarWidth + AppTheme.spacing2
                      + root.actionBarReceiptReserve
                z: 3
                // Created on first need, then kept until the row leaves the
                // screen.
                // The latch write is deferred: onLoaded runs inside the
                // `active` binding's evaluation, and writing a dependency there
                // is a binding loop.
                property bool latched: false
                active: latched || root.actionsVisible
                onLoaded: Qt.callLater(function() { latched = root.rowOnScreen })
                visible: root.actionsVisible
                sourceComponent: Rectangle {
                id: messageActionBar
                Connections {
                    target: root.timelineView || null
                    ignoreUnknownSignals: true
                    enabled: messageActionBarLoader.visible
                    function onContentYChanged() {
                        messageActionBarLoader.refreshHeaderPush()
                    }
                }
                radius: AppTheme.radiusTile
                color: AppTheme.surface
                border.color: AppTheme.borderStrong
                border.width: 1
                implicitWidth: threadActionRow.implicitWidth
                               + AppTheme.spacing2 * 2
                implicitHeight: threadActionRow.implicitHeight
                                + AppTheme.spacing2 * 2

                readonly property bool tooltipsBelow:
                    Overlay.overlay
                    ? bubbleRow.mapToItem(Overlay.overlay, 0, 0).y
                      + messageActionBarLoader.restY < 41
                    : false

                Row {
                    id: threadActionRow
                    anchors.centerIn: parent
                    spacing: 2
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
                        id: threadStartButton
                        objectName: "messageThreadReplyButton"
                        visible: !root.inThreadPanel && !!app.thread
                                 && app.thread.supported
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "forum"
                        iconSize: 18
                        enabled: !model.redacted
                                 && (model.eventId || "").length > 0
                                 && model.eventId.indexOf("local:") !== 0
                        Accessible.name: qsTr("Reply in thread")
                        ToolTip {
                            visible: threadStartButton.hovered
                            delay: 500
                            text: qsTr("Reply in thread")
                            y: messageActionBar.tooltipsBelow
                               ? threadStartButton.height + AppTheme.spacingXS
                               : -implicitHeight - AppTheme.spacingXS
                        }
                        onClicked: root.replyInThread(root.eventIdForActions())
                    }
                    IconButton {
                        id: threadEditButton
                        objectName: "messageEditButton"
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "edit_square"
                        iconSize: 18
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
                                root.timelineModel.roomId)
                        }
                    }
                    IconButton {
                        id: threadMoreButton
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "more_vert"
                        iconSize: 18
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

        Loader {
            id: reactionsLoader
            active: !model.redacted && root.shownReactions.length > 0
                    && root.reactionsMode !== 2
            visible: active
            opacity: root.revealOpacity(root.reactionsMode)
            enabled: opacity > 0
            Layout.fillWidth: true
            readonly property bool followsOwnBubble:
                root.bubbleMode && model.isOwn === true
            readonly property real pileReserve: receiptStripLoader.visible
                && !root.ownBubbleReceiptFooter
                ? receiptStripLoader.rowWidth + AppTheme.spacingXS : 0
            Layout.rightMargin: reactionsLoader.followsOwnBubble
                ? Math.max(reactionsLoader.pileReserve,
                           bubbleRow.width - (bubble.x + bubble.width))
                : reactionsLoader.pileReserve
            Layout.alignment: Qt.AlignLeft
            readonly property real ownBubbleChipRun: 220
            Layout.leftMargin: reactionsLoader.followsOwnBubble
                ? Math.max(root.avatarGutterWidth,
                           Math.min(bubble.x,
                                    bubbleRow.width
                                    - reactionsLoader.ownBubbleChipRun))
                : root.avatarGutterWidth
            readonly property real designTopGap: AppTheme.spacingXS
            Layout.topMargin: reactionsLoader.designTopGap
            sourceComponent: Flow {
                id: reactionsFlow
                objectName: "reactionsFlow"
                readonly property real designTopGap: reactionsLoader.designTopGap
                spacing: AppTheme.spacingXS
                Repeater {
                    model: root.shownReactions
                    Rectangle {
                        id: reactionChip
                        objectName: "reactionChip"
                        readonly property color baseFill: modelData.byMe
                            ? AppTheme.reactionSelectedBackground
                            : AppTheme.reactionBackground
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
                                      ? AppTheme.reactionSelectedBorder
                                      : reactionHover.hovered
                                        ? (AppTheme.dark ? AppTheme.borderStrong
                                                         : AppTheme.textMuted)
                                        : AppTheme.reactionBorder
                        border.width: modelData.byMe ? 2 : 1
                        activeFocusOnTab: !root.readOnlyView
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
                        Rectangle {
                            anchors.fill: parent
                            visible: reactionChip.activeFocus
                            color: "transparent"
                            radius: reactionChip.radius
                            border.width: 2
                            border.color: AppTheme.focusRing
                        }
                        implicitWidth: reactionRow.implicitWidth + 14
                        implicitHeight:
                            Math.max(AppTheme.scaled(AppTheme.reactionChipHeight),
                                     reactionRow.implicitHeight + 4)
                        HoverHandler { id: reactionHover }

                        readonly property var reactorNames:
                            modelData.reactorNames || []
                        readonly property int reactorTotal:
                            modelData.reactorTotal >= 0
                            ? modelData.reactorTotal : (modelData.count || 0)
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
                        ToolTip.text: reactionChip.reactorSummary
                        ToolTip.visible: reactionHover.hovered
                                         && reactionChip.reactorSummary.length > 0
                        ToolTip.delay: 300
                        readonly property bool customEmojiReaction:
                            typeof modelData.key === "string"
                            && modelData.key.startsWith("mxc://")
                            && modelData.key.length > 6
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
                                Layout.preferredHeight:
                                    AppTheme.scaled(AppTheme.reactionEmojiBox)
                                lineHeightMode: Text.FixedHeight
                                lineHeight: AppTheme.scaled(AppTheme.reactionEmojiBox)
                                text: reactionChip.customEmojiReaction
                                      ? "" : modelData.key
                                // A reaction key is any string the reactor chose.
                                textFormat: Text.PlainText
                                font.family: app.emojiFontFamily || ""
                                font.pixelSize:
                                    AppTheme.scaled(AppTheme.reactionEmojiSize)
                                verticalAlignment: Text.AlignVCenter
                            }
                            CustomEmojiImage {
                                id: customEmojiImage
                                objectName: "reactionCustomEmoji"
                                visible: reactionChip.customEmojiReaction
                                Layout.alignment: Qt.AlignVCenter
                                Layout.preferredHeight:
                                    AppTheme.scaled(AppTheme.reactionEmojiBox)
                                Layout.preferredWidth:
                                    AppTheme.scaled(AppTheme.reactionEmojiBox)
                                mxc: reactionChip.customEmojiReaction ? modelData.key : ""
                                retained: root.mediaPixmapRetained
                                onScreen: root.rowOnScreen
                                stillCache: root.mediaPixmapCacheAllowed
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
                            enabled: root.rowActionsEnabled && !root.readOnlyView
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
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
                        Accessible.description: reactionChip.reactorSummary
                        Accessible.onPressAction: {
                            if (!root.readOnlyView)
                                root.timelineModel.toggleReaction(root.eventIdForActions(), modelData.key)
                        }
                    }
                }

                Rectangle {
                    id: reactionAddChip
                    objectName: "reactionAddChip"
                    visible: !model.redacted && !root.readOnlyView
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
                    border.width: 1
                    border.color: addChipHover.hovered
                                  ? (AppTheme.dark ? AppTheme.borderStrong
                                                   : AppTheme.textMuted)
                                  : (AppTheme.dark ? AppTheme.border
                                                   : AppTheme.borderStrong)
                    activeFocusOnTab: !root.readOnlyView
                    function activate() {
                        if (root.readOnlyView)
                            return
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
                        enabled: root.rowActionsEnabled && !root.readOnlyView
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
        }

    }

    Loader {
        id: receiptStripLoader
        readonly property Item row: item ? item.row : null
        readonly property real rowWidth: row ? row.width : 0
        readonly property real rowHeight: row ? row.height : 0
        active: !model.redacted
                && ((model.readReceipts || []).length > 0
                    || model.sentReceipt === true)
                && root.naturalImplicitHeight > 0
                && root.receiptsMode !== 2
        visible: active
        opacity: root.revealOpacity(root.receiptsMode)
        enabled: opacity > 0
        x: layout.x
        width: layout.width
        y: root.ownBubbleReceiptFooter
           ? layout.y + bubbleRow.y
             + Math.max(avatarSlot.implicitHeight, bubble.implicitHeight)
             + AppTheme.spacingXS
           : root.isCallEvent || root.isStateActivity || root.isThreadKeyWait
           ? root.height : layout.y + layout.height
        height: 0
        z: 3
        sourceComponent: Item {
            id: readReceiptStrip
            objectName: "readReceiptStrip"
            readonly property var receipts: model.readReceipts || []
            readonly property int maxAvatars: 3
            readonly property int totalOthers:
                model.readReceiptsTotal >= 0 ? model.readReceiptsTotal
                                             : receipts.length
            readonly property int overflowCount:
                Math.max(0, totalOthers - Math.min(shown.length, maxAvatars))
            readonly property bool sentOnly:
                model.sentReceipt === true && receipts.length === 0

            property var shown: []
            function refreshShown() {
                var next = []
                var n = Math.min(receipts.length, maxAvatars)
                for (var i = 0; i < n; ++i) {
                    var r = receipts[i]
                    next.push({ userId: r.userId || "",
                                displayName: r.displayName || "",
                                avatarMxc: r.avatarMxc || "",
                                tsMs: r.tsMs || 0 })
                }
                if (JSON.stringify(next) !== JSON.stringify(shown))
                    shown = next
            }
            onReceiptsChanged: refreshShown()
            Component.onCompleted: refreshShown()

            readonly property string summary: {
                if (sentOnly)
                    return qsTr("Sent")
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
            readonly property Item row: receiptRow

            Rectangle {
                z: 100
                x: receiptRow.x - 2
                y: receiptRow.y - 2
                width: receiptRow.width + 4
                height: receiptRow.height + 4
                radius: height / 2
                color: "transparent"
                border.width: 2
                border.color: AppTheme.focusRing
                visible: receiptRow.activeFocus
            }

            Row {
                id: receiptRow
                objectName: "readReceiptRow"
                x: root.ownBubbleReceiptFooter
                   ? bubble.x + bubble.width - width
                   : Math.max(root.avatarGutterWidth,
                              readReceiptStrip.width - width)
                y: root.ownBubbleReceiptFooter ? 0 : -height
                spacing: -6

                property int hoverIndex: -1
                readonly property int liftSlots:
                    restChips + (restOverflow > 0 ? 1 : 0)
                function liftFor(i) {
                    if (hoverIndex < 0 || AppTheme.reducedMotion)
                        return 0
                    return 4 * Math.pow(0.45, Math.abs(i - hoverIndex))
                }
                function zFor(i) {
                    return hoverIndex < 0
                           ? i : liftSlots - Math.abs(i - hoverIndex)
                }
                function scaleFor(i) {
                    return hoverIndex === i && !AppTheme.reducedMotion ? 1.05 : 1
                }
                function slotAt(px) {
                    var best = -1
                    var bestD = 1e9
                    for (var i = 0; i < restChips; ++i) {
                        var c = chipRepeater.itemAt(i)
                        if (!c)
                            continue
                        var d = Math.abs(px - (c.x + c.width / 2))
                        if (d < bestD) {
                            bestD = d
                            best = i
                        }
                    }
                    if (restOverflow > 0
                            && Math.abs(px - (overflowPill.x
                                              + overflowPill.width / 2)) < bestD)
                        best = restChips
                    return best
                }

                readonly property int restChips:
                    Math.min(readReceiptStrip.shown.length,
                             readReceiptStrip.maxAvatars)
                readonly property int restOverflow:
                    Math.max(0, readReceiptStrip.totalOthers - restChips)
                function readTimeText(tsMs) {
                    if (!tsMs || tsMs <= 0)
                        return ""
                    var d = new Date(tsMs)
                    var now = new Date()
                    var startOfToday = new Date(now.getFullYear(), now.getMonth(),
                                                now.getDate())
                    if (d >= startOfToday)
                        return Qt.formatTime(d, app.settings.clockTimeFormat)
                    if (startOfToday - d < 6 * 86400000)
                        return Qt.locale(AppLocale.name).toString(d, "ddd hh:mm")
                    return Qt.locale(AppLocale.name).toString(d, "MMM d, hh:mm")
                }
                function overflowNames() {
                    var rs = readReceiptStrip.receipts
                    var out = []
                    for (var i = restChips; i < rs.length && out.length < 8; ++i)
                        out.push(rs[i].displayName || rs[i].userId || "")
                    var rest = restOverflow - out.length
                    if (rest > 0)
                        out.push("+" + rest)
                    return out.join("\n")
                }
                function openList(from) {
                    if (!root.timelineView || !root.timelineView.openReceiptList)
                        return
                    var src = from || receiptRow
                    var p = src.mapToItem(Overlay.overlay,
                                          src.width / 2, src.height / 2)
                    root.timelineView.openReceiptList(
                        model.readReceipts || [],
                        readReceiptStrip.totalOthers, Qt.point(p.x, p.y),
                        model.eventId || "")
                }
                function openProfileFor(item, reader) {
                    if (!root.timelineView || !root.timelineView.openSenderProfile)
                        return
                    var uid = reader.userId || ""
                    if (uid.length === 0)
                        return
                    var tl = item.mapToItem(Overlay.overlay, 0, 0)
                    root.timelineView.openSenderProfile({
                        userId: uid,
                        displayName: reader.displayName || "",
                        avatarUrl: reader.avatarMxc || ""
                    }, { x: tl.x, y: tl.y, width: item.width, height: item.height })
                }
                activeFocusOnTab: root.rowActionsEnabled
                                  && !readReceiptStrip.sentOnly && visible
                Keys.onReturnPressed: receiptRow.openList()
                Keys.onEnterPressed: receiptRow.openList()
                Keys.onSpacePressed: receiptRow.openList()
                readonly property color hoverTint:
                    rowHighlight.visible ? rowHighlight.color : "transparent"
                readonly property real hoverTintOpacity:
                    rowHighlight.visible ? rowHighlight.opacity : 0
                Repeater {
                    id: chipRepeater
                    model: readReceiptStrip.shown
                    Rectangle {
                        id: chip
                        objectName: "readReceiptChip"
                        readonly property int chipIndex: index
                        readonly property real lift: receiptRow.liftFor(index)
                        property real liftNow: lift
                        Behavior on liftNow {
                            NumberAnimation {
                                duration: AppTheme.reducedMotion ? 0 : 350
                                easing.type: Easing.BezierSpline
                                easing.bezierCurve: [0.22, 1, 0.36, 1, 1, 1]
                            }
                        }
                        width: 18
                        height: 18
                        radius: 9
                        color: AppTheme.background
                        z: receiptRow.zFor(index)
                        scale: receiptRow.scaleFor(index)
                        Behavior on scale {
                            NumberAnimation {
                                duration: AppTheme.reducedMotion ? 0 : 350
                                easing.type: Easing.BezierSpline
                                easing.bezierCurve: [0.22, 1, 0.36, 1, 1, 1]
                            }
                        }
                        transform: Translate { y: -chip.liftNow }
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
                        Loader {
                            anchors.fill: parent
                            active: receiptHover.hovered
                            sourceComponent: Item {
                                ToolTip {
                                    visible: receiptRow.hoverIndex === chip.chipIndex
                                    delay: 400
                                    timeout: 0
                                    contentItem: Label {
                                        textFormat: Text.PlainText
                                        text: {
                                            var t = receiptRow.readTimeText(modelData.tsMs)
                                            var n = modelData.displayName
                                                    || modelData.userId
                                            return t.length > 0 ? n + "\n" + t : n
                                        }
                                        color: AppTheme.textPrimary
                                        font.pixelSize: AppTheme.fontCaption
                                    }
                                }
                            }
                        }
                    }
                }
                Item {
                    objectName: "sentReceiptCheck"
                    visible: readReceiptStrip.sentOnly
                    width: 18
                    height: 18
                    Rectangle {
                        anchors.centerIn: parent
                        width: 16
                        height: 16
                        radius: 8
                        color: "transparent"
                        border.width: 1
                        border.color: AppTheme.textMuted
                        Icon {
                            anchors.centerIn: parent
                            name: "check"
                            size: 11
                            color: AppTheme.textMuted
                        }
                    }
                }
                Rectangle {
                    id: overflowPill
                    objectName: "readReceiptOverflow"
                    readonly property int pillIndex: receiptRow.restChips
                    readonly property real lift: receiptRow.liftFor(pillIndex)
                    property real liftNow: lift
                    Behavior on liftNow {
                        NumberAnimation {
                            duration: AppTheme.reducedMotion ? 0 : 350
                            easing.type: Easing.BezierSpline
                            easing.bezierCurve: [0.22, 1, 0.36, 1, 1, 1]
                        }
                    }
                    visible: receiptRow.restOverflow > 0
                    width: Math.max(18, overflowLabel.implicitWidth + 8)
                    height: 18
                    radius: AppTheme.radiusPill
                    color: AppTheme.reactionBackground
                    border.color: AppTheme.border
                    border.width: 1
                    z: receiptRow.zFor(pillIndex)
                    scale: receiptRow.scaleFor(pillIndex)
                    Behavior on scale {
                        NumberAnimation {
                            duration: AppTheme.reducedMotion ? 0 : 350
                            easing.type: Easing.BezierSpline
                            easing.bezierCurve: [0.22, 1, 0.36, 1, 1, 1]
                        }
                    }
                    transform: Translate { y: -overflowPill.liftNow }
                    Label {
                        id: overflowLabel
                        anchors.centerIn: parent
                        text: "+" + receiptRow.restOverflow
                        color: AppTheme.textSecondary
                        font.pixelSize: AppTheme.fontMicro
                        font.weight: AppTheme.weightBold
                    }
                    Loader {
                        anchors.fill: parent
                        active: receiptHover.hovered
                        sourceComponent: Item {
                            ToolTip {
                                visible: receiptRow.hoverIndex === overflowPill.pillIndex
                                delay: 400
                                timeout: 0
                                contentItem: Label {
                                    textFormat: Text.PlainText
                                    text: receiptRow.overflowNames()
                                    color: AppTheme.textPrimary
                                    font.pixelSize: AppTheme.fontCaption
                                }
                            }
                        }
                    }
                }

                HoverHandler {
                    id: receiptHover
                    cursorShape: readReceiptStrip.sentOnly ? Qt.ArrowCursor
                                                           : Qt.PointingHandCursor
                    onPointChanged: receiptRow.updateHover()
                    onHoveredChanged: receiptRow.updateHover()
                }
                function updateHover() {
                    hoverIndex = (receiptHover.hovered
                                  && !readReceiptStrip.sentOnly)
                                 ? slotAt(receiptHover.point.position.x) : -1
                }
                TapHandler {
                    enabled: root.rowActionsEnabled && !readReceiptStrip.sentOnly
                    gesturePolicy: TapHandler.WithinBounds
                    onTapped: function(eventPoint) {
                        var slot = receiptRow.slotAt(eventPoint.position.x)
                        if (slot < 0)
                            return
                        if (slot === receiptRow.restChips) {
                            receiptRow.forceActiveFocus(Qt.MouseFocusReason)
                            receiptRow.openList(overflowPill)
                            return
                        }
                        var chip = chipRepeater.itemAt(slot)
                        var reader = readReceiptStrip.shown[slot]
                        if (chip && reader)
                            receiptRow.openProfileFor(chip, reader)
                    }
                }
                ToolTip.text: readReceiptStrip.summary
                ToolTip.visible: receiptHover.hovered && readReceiptStrip.sentOnly
                                 && readReceiptStrip.summary.length > 0
                ToolTip.delay: 500
                Accessible.role: Accessible.Button
                Accessible.name: readReceiptStrip.summary
                Accessible.onPressAction: receiptRow.openList()
            }
        }
    }

    function openReactionPickerFor(eventId, anchorItem, expanded) {
        if (!root.timelineView || !root.timelineView.openReactionPicker
            || eventId === "")
            return
        var p = anchorItem.mapToItem(Overlay.overlay,
                                     anchorItem.width / 2, anchorItem.height)
        root.timelineView.openReactionPicker(eventId, p, expanded === true)
    }

    readonly property bool moreMenuOpen:
        moreMenuItem ? moreMenuItem.opened : false
    function ensureContextMenu() {
        if (!moreMenuItem)
            moreMenuItem = moreMenuComponent.createObject(root)
        return moreMenuItem
    }

    function forwardEligible() {
        var isMediaRow = model.isImage === true || model.isVideo === true
            || model.isAudio === true || model.isSticker === true
            || model.isFile === true
        return model.redacted !== true && model.isLocalEcho !== true
            && model.undecryptable !== true
            && model.isVirtual !== true && model.isPoll !== true
            && (isMediaRow ? model.mediaSourceAvailable === true
                           : (model.body || "").length > 0)
            && !root.isGallery
    }
    function beginForward() {
        if (!forwardEligible() || root.menuEventId === "")
            return
        app.forward.begin(
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
                isLocation: model.isLocation === true,
                mediaIsVoice: model.mediaIsVoice === true,
                senderDisplayName: model.senderDisplayName || "",
                body: model.body || "",
                mediaKey: model.mediaKey || "",
                mediaFilename: model.mediaFilename || "",
                mediaMimetype: model.mediaMimetype || "",
                mediaWidth: model.mediaWidth || 0,
                mediaHeight: model.mediaHeight || 0,
                mediaSpoiler: model.mediaSpoiler === true
            })
    }
    function deleteThisMessage() {
        var id = root.menuEventId
        if (id === "" || !root.timelineModel.canRedactEvent(
                id, root.canRedactOthers))
            return
        if (model.isOwn === true) {
            root.confirmDestructive(
                qsTr("Delete message?"),
                qsTr("This removes the message for everyone in the "
                     + "room. It cannot be undone."),
                qsTr("Delete"),
                function() { root.timelineModel.redactEvent(id) })
            return
        }
        root.confirmDestructive(
            qsTr("Remove message?"),
            qsTr("This removes %1's message for everyone in the "
                 + "room. It cannot be undone.")
                .arg(model.senderDisplayName || model.sender || ""),
            qsTr("Remove"),
            function() { root.timelineModel.redactEvent(id) })
    }
    function copyMessageText() {
        if (root.copyableText)
            root.copyToClipboard(
                root.timelineModel.visibleTextForEvent(root.menuEventId))
    }
    function copyMessageLink() {
        if (root.menuEventId !== "")
            root.copyToClipboard(
                root.timelineModel.messagePermalink(root.menuEventId))
    }
    function toggleSavedMessage() {
        if (!app.savedMessages || !app.savedMessages.loaded
                || app.savedMessages.busy || root.menuEventId === ""
                || model.redacted === true || model.undecryptable === true
                || model.isLocalEcho === true || model.isVirtual === true
                || model.isPoll === true)
            return
        const roomId = root.timelineModel.realRoomIdForEvent(root.menuEventId)
        if (!roomId || roomId.length === 0)
            return
        if (app.savedMessages.isSaved(roomId, root.menuEventId)) {
            app.savedMessages.remove(roomId, root.menuEventId)
        } else {
            const details = root.timelineModel.messageDetails(root.menuEventId)
            const threadRootId = root.inThreadPanel
                  ? app.thread.rootEventId : (details.threadRootId || "")
            app.savedMessages.save(roomId, root.menuEventId, threadRootId)
        }
    }
    function runSelectionAction(action) {
        if (model.isVirtual === true || !model.eventId)
            return false
        root.menuEventId = model.eventId
        switch (action) {
        case "timeline.replyInThread":
            if (root.timelineModel.messagePermalink(root.menuEventId).length > 0
                    && !root.timelineModel.messageDetails(root.menuEventId).redacted)
                replyInThread(root.menuEventId)
            return true
        case "timeline.editMessage":
            if (root.timelineModel.canEditEvent(root.menuEventId))
                app.composer.beginEdit(root.menuEventId,
                    root.timelineModel.visibleTextForEvent(root.menuEventId),
                    root.timelineModel.sanitizedHtmlForEvent(root.menuEventId),
                    root.timelineModel.roomId)
            return true
        case "timeline.reactToMessage":
            if (root.timelineModel.messagePermalink(root.menuEventId).length > 0
                    && !root.timelineModel.messageDetails(root.menuEventId).redacted)
                openReactionPickerFor(root.menuEventId, bubbleRow, true)
            return true
        case "timeline.pinMessage":
            if (!root.inThreadPanel && app.pinned) {
                if (app.pinned.canTogglePin(root.menuEventId, false))
                    app.pinned.unpin(root.menuEventId)
                else if (model.redacted !== true
                         && app.pinned.canTogglePin(root.menuEventId, true))
                    app.pinned.pin(root.menuEventId)
            }
            return true
        case "timeline.forwardMessage": beginForward(); return true
        case "timeline.saveMessage": toggleSavedMessage(); return true
        case "timeline.deleteMessage":
            if (root.bulkSelectionMode)
                app.bulkRedaction.toggle(root.menuEventId, root.canRedactOthers)
            else
                deleteThisMessage()
            return true
        case "timeline.copyMessageText": copyMessageText(); return true
        case "timeline.copyMessageLink": copyMessageLink(); return true
        case "timeline.viewMessageSource":
            if (root.timelineModel.requestEventSource)
                openEventSource(root.menuEventId)
            return true
        }
        return false
    }
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
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
            width: Math.min(420, parent ? parent.width - 32 : 420)
            property string heading: ""
            property string body: ""
            property string acceptText: ""
            property var action: null
            onOpened: confirmCancelButton.forceActiveFocus()
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
                    AppButton {
                        id: confirmCancelButton
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
    property var moreMenuItem: null
    Component {
        id: moreMenuComponent
        AppMenu {
            id: moreMenu
            objectName: "messageContextMenu"
            menuWidth: AppTheme.menuWidthMessage
            contextLabel: qsTr("Message · %1 · %2")
                .arg(model.senderDisplayName || model.sender || "")
                .arg(Qt.formatDateTime(model.timestamp,
                                       app.settings.clockTimeFormat))
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
                if (root.timelineView && ("openRowMenu" in root.timelineView)
                        && root.timelineView.openRowMenu === moreMenu)
                    root.timelineView.openRowMenu = null
            }
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
                        root.showThread(rootId)
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
                    if (root.copyableText) {
                        root.copyToClipboard(
                            root.timelineModel.visibleTextForEvent(
                                root.menuEventId))
                        moreMenu.close()
                    }
                }
            }
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
                    root.openReactionPickerFor(root.menuEventId, bubbleRow,
                                               true)
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
                onTriggered: root.replyInThread(root.menuEventId)
            }
            AppMenuItem {
                iconName: "arrow_forward"
                text: qsTr("Open in room")
                visible: root.inThreadPanel
                enabled: root.menuEventId !== ""
                onTriggered: app.pagination.jumpToEvent(
                    root.menuEventId)
            }
            AppMenuSeparator {}
            AppMenuItem {
                iconName: "push_pin"
                text: qsTr("Pin message")
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
                objectName: "saveMessageMenuItem"
                iconName: "bookmark"
                readonly property string targetRoom:
                    root.timelineModel.realRoomIdForEvent(root.menuEventId)
                readonly property bool alreadySaved:
                    app.savedMessages && app.savedMessages.revision >= 0
                    && app.savedMessages.isSaved(targetRoom, root.menuEventId)
                text: alreadySaved ? qsTr("Remove from saved messages")
                                   : qsTr("Save message")
                accel: "S"
                visible: !!app.savedMessages && app.savedMessages.loaded
                         && model.redacted !== true && model.undecryptable !== true
                         && model.isLocalEcho !== true && model.isVirtual !== true
                         && model.isPoll !== true
                         && targetRoom !== "" && root.menuEventId !== ""
                enabled: visible && !app.savedMessages.busy
                onTriggered: root.toggleSavedMessage()
            }
            AppMenuItem {
                objectName: "copyTextMenuItem"
                iconName: "content_copy"
                text: qsTr("Copy text")
                accel: "Ctrl+C"
                enabled: root.copyableText
                onTriggered: root.copyMessageText()
            }
            AppMenuItem {
                iconName: "link"
                text: qsTr("Copy message link")
                enabled: root.timelineModel.messagePermalink(
                             root.menuEventId).length > 0
                onTriggered: root.copyMessageLink()
            }
            AppMenuItem {
                objectName: "openMediaMenuItem"
                iconName: "open_in_full"
                text: qsTr("Open image")
                // Images and stickers only; video has its own player and
                // fullscreen overlay. Not an SVG: the viewer would ask for the
                // payload, which is never decoded; Save covers it.
                visible: (model.isImage === true || model.isSticker === true)
                         && !root.isSvgImage
                         && model.mediaSourceAvailable === true
                enabled: visible && root.timelineView
                         && !!root.timelineView.openImage
                onTriggered: root.timelineView.openImage(
                    model.mediaKey || "", model.mediaUrl)
            }
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
            AppMenuItem {
                objectName: "republishPrivateImageMenuItem"
                iconName: "star"
                text: qsTr("Add to my stickers…")
                visible: model.isEncrypted === true
                         && (model.isSticker === true || model.isImage === true)
                         && (model.mediaKey || "").length > 0
                         && app.stickers.available
                enabled: visible && !app.stickers.saving
                onTriggered: {
                    republishLoader.active = true
                    // Only the media key: the private caption or file name
                    // must never name the public copy.
                    republishLoader.item.ask(model.mediaKey || "")
                }
            }
            AppMenuItem {
                objectName: "saveStickerToRoomMenuItem"
                iconName: "workspaces"
                text: qsTr("Add to this room's stickers")
                visible: model.isSticker === true
                         && (model.mediaMxc || "").length > 0
                         && app.stickers.available
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
                id: saveEmoticonsItem
                objectName: "saveEmoticonsMenuItem"
                iconName: "add_reaction"
                readonly property var pending: {
                    var _r = app.stickers.revision
                    if (!app.stickers.available)
                        return []
                    return app.stickers
                        .emoticonsInHtml(model.formattedBody || "")
                        .filter(function (e) { return !e.saved })
                }
                text: pending.length === 1
                      ? qsTr("Add :%1: to my emoji").arg(pending[0].shortcode)
                      : qsTr("Add %n custom emoji to my pack", "", pending.length)
                visible: pending.length > 0
                enabled: visible && !app.stickers.transferring
                         && app.stickers.canSave(pending[0].url)
                onTriggered: app.stickers.saveEmoticons(pending)
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
                         && !root.isGallery
                enabled: visible && root.menuEventId !== ""
                onTriggered: {
                    if (root.timelineView
                        && root.timelineView.saveMedia)
                        root.timelineView.saveMedia(
                            model.mediaKey || "",
                            model.mediaFilename || "",
                            model.mediaMimetype || "", true)
                }
            }
            AppMenuItem {
                objectName: "hideMediaMenuItem"
                iconName: root.mediaHidden ? "visibility" : "visibility_off"
                text: root.mediaHidden ? qsTr("Show image")
                                       : qsTr("Hide image")
                visible: root.mediaHideable
                enabled: visible
                onTriggered: root.setMediaHidden(!root.mediaHidden)
            }

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
                visible: model.isImage === true
                         && model.mediaSourceAvailable === true
                         && app.mediaBridge.supported
                         && !root.isGallery
                enabled: visible && root.menuEventId !== ""
                onTriggered: app.copyImageToClipboard(model.mediaKey || "")
            }
            AppMenuItem {
                objectName: "useAsBackgroundMenuItem"
                iconName: "image"
                text: qsTr("Use as my background here")
                // Unencrypted rooms only (the controller re-checks): keeping
                // an encrypted room's picture as a file would be decrypted
                // media at rest.
                visible: model.isImage === true
                         && model.mediaSourceAvailable === true
                         && app.mediaBridge.supported
                         && !root.isGallery
                         && !root.roomEncrypted
                         && !!app.backdrops
                enabled: visible && root.menuEventId !== ""
                onTriggered: app.backdrops.useMessageImage(app.currentRoomId,
                                                           model.mediaKey || "")
            }
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
                             root.menuEventId, root.canRedactOthers)
                         || (app.moderation.reportSupported
                             && model.isOwn !== true)
            }
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
                objectName: "selectMessagesToDeleteMenuItem"
                iconName: "delete"
                text: qsTr("Select messages to delete")
                // The bulk-delete bar belongs to the live timeline.
                visible: !root.readOnlyView
                         && (root.timelineModel.roomId || "").indexOf("\u001fctx\u001f") < 0
                         && root.timelineModel.canRedactEvent(
                             root.menuEventId, root.canRedactOthers)
                enabled: visible && !app.bulkRedaction.running
                onTriggered: {
                    app.forward.cancelSelecting()
                    app.bulkRedaction.begin(root.timelineModel)
                    app.bulkRedaction.toggle(root.menuEventId,
                                             root.canRedactOthers)
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
                    && !root.isGallery
                enabled: eligible && root.menuEventId !== ""
                visible: eligible
                onTriggered: root.beginForward()
            }
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
                objectName: "deleteMessageMenuItem"
                iconName: "delete"
                text: qsTr("Delete")
                danger: true
                enabled: root.timelineModel.canRedactEvent(
                             root.menuEventId, root.canRedactOthers)
                visible: enabled
                onTriggered: root.deleteThisMessage()
            }
        }
    }
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

    function eventIdForActions() { return model.eventId }

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
            readonly property string sourceText: p.imageSource || ""
            readonly property string imageMime: p.imageMime || ""
            readonly property bool animationCandidate:
                (imageMime === "image/gif" && p.gifOversized !== true)
                || imageMime === "image/webp"
            readonly property bool animationWanted:
                animationCandidate && MotionPolicy.allowed
                && MotionPolicy.plays(directMediaHover.hovered)
            // Materialized whenever animation is allowed at all, so On hover
            // starts at once. Inline preview bytes are local; a server-route
            // mxc fetches the original from the homeserver, never the site.
            property int animationTick: 0
            readonly property string animatedSource: {
                var _ = animationTick
                if (!animationCandidate || !MotionPolicy.allowed)
                    return ""
                if (sourceText.indexOf("mxc://") === 0)
                    return app.mediaBridge.mxcAnimatedSource(sourceText)
                return app.mediaBridge.previewAnimatedSource(sourceText,
                                                             imageMime)
            }
            property int previewTick: 0
            Connections {
                target: app.mediaBridge
                function onMediaCached(cacheKey) {
                    if (directMedia.sourceText.indexOf("mxc://") === 0
                        && cacheKey === "mxcimg:512:" + directMedia.sourceText) {
                        directMedia.previewTick += 1
                        if (root.timelineView && root.timelineView.notePreviewRefresh)
                            root.timelineView.notePreviewRefresh()
                    }
                }
                function onAnimatedMediaReady(cacheKey) {
                    if (cacheKey === "mxcanim:" + directMedia.sourceText)
                        directMedia.animationTick += 1
                }
            }
            readonly property string staticSource: {
                var _ = previewTick
                var src = sourceText
                if (src.length === 0)
                    return ""
                if (src.indexOf("mxc://") === 0)
                    return app.mediaBridge.mxcImageSource(src, 512)
                return app.mediaBridge.previewImageSource(src, imageMime)
            }

            implicitWidth: displayWidth
            implicitHeight: Math.max(1, displayWidth * aspectRatio)
            color: AppTheme.embedSurface
            radius: AppTheme.radiusSm
            clip: true

            Image {
                anchors.fill: parent
                visible: !directAnimated.visible
                source: root.mediaPixmapRetained ? directMedia.staticSource : ""
                fillMode: Image.PreserveAspectFit
                sourceSize.width: 720
                asynchronous: true
                cache: root.mediaPixmapCacheAllowed
            }
            AnimatedImage {
                id: directAnimated
                anchors.fill: parent
                visible: directMedia.animationWanted
                         && status === AnimatedImage.Ready
                source: root.mediaPixmapRetained && directMedia.animationWanted
                        ? directMedia.animatedSource : ""
                fillMode: Image.PreserveAspectFit
                // Suspension stops it through `playing`: a `paused` set
                // while a movie is not running is lost when it starts.
                playing: visible && root.rowOnScreen && !MotionPolicy.suspended
                asynchronous: true
                cache: false
                // Keeps the bridge from evicting the file mid-play.
                AnimationFileHold {}
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
                    text: qsTr("GIF")
                    color: AppTheme.scrimInk
                    font.pixelSize: AppTheme.fontMicro
                    font.weight: Font.Bold
                }
            }
            MouseArea {
                objectName: "directMediaOpenViewer"
                anchors.fill: parent
                enabled: (directMedia.p.mediaKey || "").length > 0
                             && root.rowActionsEnabled
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: root.openLinkMedia()
            }
            Rectangle {
                z: 3
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: 4
                radius: AppTheme.radiusSm
                color: AppTheme.overlayScrim
                width: directActions.implicitWidth
                height: directActions.implicitHeight
                opacity: directMediaHover.hovered || directOpenButton.hovered
                         || directDismissButton.hovered
                         || directOpenButton.activeFocus
                         || directDismissButton.activeFocus ? 1.0 : 0.6
                Row {
                    id: directActions
                    IconButton {
                        id: directOpenButton
                        objectName: "directMediaOpenInBrowser"
                        size: "sm"
                        iconName: "link"
                        iconColorOverride: AppTheme.scrimInk
                        enabled: (directMedia.p.url || "").length > 0
                                 && root.rowActionsEnabled
                        Accessible.name: qsTr("Open link in browser")
                        ToolTip.text: qsTr("Open in browser")
                        ToolTip.visible: directOpenButton.hovered
                        ToolTip.delay: 400
                        onClicked: app.media.openWebUrl(directMedia.p.url)
                    }
                    IconButton {
                        id: directDismissButton
                        objectName: "linkPreviewDismissButton"
                        size: "sm"
                        iconName: "close"
                        iconColorOverride: AppTheme.scrimInk
                        Accessible.name: qsTr("Dismiss link preview")
                        ToolTip.text: qsTr("Dismiss preview")
                        ToolTip.visible: directDismissButton.hovered
                        ToolTip.delay: 400
                        onClicked: app.linkPreviews.dismissPreviewForEvent(
                                       root.previewRoomId, root.actionKey)
                    }
                }
            }
            HoverHandler { id: directMediaHover }
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("View image %1")
                                 .arg(directMedia.p.fileName
                                      || directMedia.p.host || "")
        }
    }

    Component {
        id: linkMediaCardComponent
        LinkMediaCard {
            preview: root.preview
            maximumWidth: root.contentInnerCap
            ownerKey: root.actionKey + "\u001f" + (root.preview.mediaKey || "")
            rowOnScreen: root.rowOnScreen
            interactive: root.rowActionsEnabled
            onViewRequested: root.openLinkMedia()
            onConsentGiven: app.linkPreviews.requestPreviewForEvent(
                                root.previewRoomId, root.actionKey)
            onOpenInBrowserRequested: app.media.openWebUrl(root.preview.url)
            onDismissRequested: app.linkPreviews.dismissPreviewForEvent(
                                    root.previewRoomId, root.actionKey)
        }
    }

    Component {
        id: linkPreviewComponent
        Rectangle {
            id: card
            objectName: "linkPreviewCard"
            readonly property var p: root.preview
            readonly property string liveSt: p.state || "none"
            // Each state is built when first shown, so one reached while the
            // timeline scrolls waits for the settle instead of building mid-gesture.
            readonly property bool holdState: !!root.timelineView
                && root.timelineView.userScrollActive === true
            property string heldSt: ""
            onHoldStateChanged: heldSt = holdState ? liveSt : ""
            Component.onCompleted: if (holdState) heldSt = liveSt
            readonly property string st: heldSt !== "" ? heldSt : liveSt
            readonly property bool dismissible: st === "loaded" || st === "failed"
            readonly property string previewAnimation:
                p.isGif === true && MotionPolicy.allowed
                ? app.mediaBridge.previewAnimatedSource(p.imageSource || "",
                                                        p.imageMime || "") : ""
            property int previewTick: 0
            Connections {
                target: app.mediaBridge
                function onMediaCached(cacheKey) {
                    var imageMxc = card.p.imageSource || ""
                    if (imageMxc.indexOf("mxc://") === 0
                        && cacheKey === "mxcimg:512:" + imageMxc) {
                        card.previewTick += 1
                        if (root.timelineView && root.timelineView.notePreviewRefresh)
                            root.timelineView.notePreviewRefresh()
                    }
                }
            }
            readonly property string previewStatic: {
                var _ = previewTick
                var src = p.imageSource || ""
                // A link's own image is never drawn on this card.
                if (src.length === 0 || p.isDirectMedia === true)
                    return ""
                if (src.indexOf("mxc://") === 0)
                    return app.mediaBridge.mxcImageSource(src, 512)
                return app.mediaBridge.previewImageSource(src, p.imageMime || "")
            }
            readonly property real fullW:
                Math.min(400, root.contentInnerCap)
            implicitWidth: st === "requires_action"
                           ? Math.min(fullW,
                                      cardCol.implicitWidth
                                      + AppTheme.spacingM + AppTheme.spacingS)
                           : fullW
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
            border.color: AppTheme.embedBorder
            border.width: 1

            HoverHandler { id: cardHover }

            MouseArea {
                anchors.fill: parent
                enabled: card.st === "loaded"
                         && (card.p.url || "").length > 0
                         && root.rowActionsEnabled
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: app.media.openWebUrl(card.p.url)
            }

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
                anchors.rightMargin: card.dismissible
                                     ? AppTheme.spacingS + 24
                                     : AppTheme.spacingS
                spacing: 4

                Loader {
                    active: card.st === "requires_action"
                    visible: active
                    Layout.fillWidth: true
                    sourceComponent: RowLayout {
                        id: consentRow
                        objectName: "linkPreviewConsentRow"
                        spacing: AppTheme.spacing6

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
                                readonly property bool routed:
                                    typeof app !== "undefined" && !!app && !!app.networkRoute
                                    && !!app.networkRoute.route
                                    && (app.networkRoute.route.kind || "none") !== "none"
                                text: routed
                                      ? (root.roomEncrypted
                                         ? qsTr("Your server sees this URL — never direct")
                                         : qsTr("Your server loads it — never direct"))
                                      : root.roomEncrypted
                                      ? qsTr("Your server sees this URL — or directly, your IP")
                                      : qsTr("Your server loads it — or directly, your IP")
                                color: root.roomEncrypted ? AppTheme.warning
                                                          : AppTheme.textMuted
                                font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                                wrapMode: Text.WordWrap
                                elide: Label.ElideNone
                                Layout.fillWidth: true
                            }
                        }

                        AppButton {
                            objectName: "linkPreviewLoadButton"
                            text: qsTr("Show")
                            size: "sm"
                            minWidth: 0
                            Accessible.name: qsTr("Show link preview")
                            Layout.alignment: Qt.AlignVCenter
                            onClicked: app.linkPreviews.requestPreviewForEvent(
                                           root.previewRoomId, root.actionKey)
                        }
                    }
                }

                Loader {
                    active: card.st === "loading"
                    visible: active
                    Layout.fillWidth: true
                    sourceComponent: ColumnLayout {
                        objectName: "linkPreviewSkeleton"
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
                }

                Loader {
                    active: card.st === "failed"
                    visible: active
                    Layout.fillWidth: true
                    sourceComponent: ColumnLayout {
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
                }

                Loader {
                    active: card.st === "loaded"
                    visible: active
                    Layout.fillWidth: true
                    sourceComponent: ColumnLayout {
                        spacing: 3

                        Rectangle {
                            id: cardMedia
                            visible: ((card.p.imageMxc || "").length > 0
                                      || (card.p.imageSource || "").length > 0)
                                     && !(card.p.gifOversized === true)
                                     && card.p.isDirectMedia !== true
                            Layout.fillWidth: true
                            Layout.preferredHeight: visible ? Math.min(180,
                                (card.p.imageHeight > 0 && card.p.imageWidth > 0)
                                ? width * (card.p.imageHeight / card.p.imageWidth)
                                : 140) : 0
                            color: AppTheme.embedSurface
                            radius: AppTheme.radiusSm
                            clip: true
                            HoverHandler { id: cardMediaHover }
                            Image {
                                id: thumb
                                anchors.fill: parent
                                visible: !cardAnimation.visible
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                                cache: root.mediaPixmapCacheAllowed
                                property int resolveTick: 0
                                source: {
                                    var _tick = resolveTick
                                    if (!root.mediaPixmapRetained)
                                        return ""
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
                                id: cardAnimation
                                objectName: "linkPreviewCardAnimation"
                                anchors.fill: parent
                                visible: card.previewAnimation.length > 0
                                         && status === AnimatedImage.Ready
                                source: cardMedia.visible && root.mediaPixmapRetained
                                        ? card.previewAnimation : ""
                                fillMode: Image.PreserveAspectFit
                                playing: MotionPolicy.plays(cardMediaHover.hovered)
                                         && root.rowOnScreen
                                         && !MotionPolicy.suspended
                                asynchronous: true
                                cache: false
                                AnimationFileHold {}
                            }
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
                                    text: qsTr("GIF")
                                    color: AppTheme.scrimInk
                                    font.pixelSize: AppTheme.fontMicro
                                    font.weight: Font.Bold
                                }
                            }
                        }

                        Loader {
                            active: card.p.isDirectMedia === true
                            visible: active
                            Layout.fillWidth: true
                            sourceComponent: Label {
                                objectName: "linkPreviewMediaSummary"
                                // Sender-chosen file name: never markup.
                                textFormat: Text.PlainText
                                text: root.linkMediaSummary()
                                color: AppTheme.text
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                                font.weight: AppTheme.weightStrong
                                elide: Label.ElideMiddle
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
                            color: AppTheme.embedText
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
    }

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
                            readonly property bool drawable:
                                key.length > 0
                                && (!isVideo || modelData.thumbAvailable === true)
                            readonly property string fetchKind:
                                modelData.thumbAvailable === true ? "thumb" : "full"
                            readonly property bool hiddenNow:
                                root.mediaHidden
                                || (!!app.mediaVisibility
                                    && root.mediaVisibilityKey.length > 0
                                    && app.mediaVisibility.isHidden(
                                           root.mediaVisibilityKey))
                            readonly property bool wanted:
                                drawable && app.mediaBridge.supported
                                && root.mediaInBand && !hiddenNow
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
                                cache: root.mediaPixmapCacheAllowed
                                sourceSize.width: Math.round(galleryBox.tile * 2)
                                source: root.mediaPixmapRetained
                                        ? galleryTile.bridgeSource : ""
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
                                    if (!galleryTile.isVideo) {
                                        if (root.timelineView && root.timelineView.openImage)
                                            root.timelineView.openImage(galleryTile.key, "")
                                    } else if (root.timelineView
                                               && root.timelineView.saveMedia) {
                                        root.timelineView.saveMedia(
                                            galleryTile.key,
                                            galleryTile.modelData.filename || "",
                                            galleryTile.modelData.mimetype || "",
                                            false)
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
                                Accessible.name: qsTr("Download %1")
                                    .arg(galleryFile.modelData.filename || qsTr("file"))
                                ToolTip.text: qsTr("Download")
                                ToolTip.visible: hovered
                                ToolTip.delay: 600
                                onClicked: root.timelineView.saveMedia(
                                    galleryFile.modelData.mediaKey,
                                    galleryFile.modelData.filename || "",
                                    galleryFile.modelData.mimetype || "",
                                    false)
                            }
                        }
                    }
                }
            }

            Loader {
                anchors.fill: parent
                active: root.mediaHidden
                sourceComponent: MediaHiddenPlaceholder {
                    hidden: root.mediaHidden
                    onRevealRequested: root.setMediaHidden(false)
                }
            }
        }
    }

    Component {
        id: imageComponent
        Item {
            id: imageBox
            objectName: "imageMedia"

            readonly property real maxW:
                Math.min(360, root.contentInnerCap)
            readonly property real maxH: 320
            readonly property real natW: model.mediaWidth > 0 ? model.mediaWidth : 0
            readonly property real natH: model.mediaHeight > 0 ? model.mediaHeight : 0
            readonly property real ratio: (natW > 0 && natH > 0)
                                          ? (natH / natW) : 0.66
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
            readonly property int gifMode: MotionPolicy.mode
            property bool gifHovered: false
            HoverHandler {
                enabled: imageBox.isGif && imageBox.gifMode === 1
                onHoveredChanged: imageBox.gifHovered = hovered
            }
            readonly property bool animateGif:
                isGif && MotionPolicy.allowed
                && MotionPolicy.plays(gifHovered)
                && !pendingMedia && animatedSource.length > 0
                && !root.mediaHidden && !root.spoilerHidden

            readonly property bool usesBridge:
                model.mediaSourceAvailable === true && app.mediaBridge.supported
            readonly property string mediaIdentity: root.actionKey + "\u001f"
                                                    + (model.mediaKey || "")
            readonly property string bridgeCacheKey:
                (model.mediaThumbAvailable ? "thumb:" : "full:") + (model.mediaKey || "")
            property string bridgeSource: ""
            property bool bridgeFailed: false
            property int retrySeconds: 0

            function refreshBridgeSource() {
                if (!usesBridge || !model.mediaKey) return
                if (root.mediaHidden) return
                if (!root.mediaInBand) return
                if (isGif && MotionPolicy.allowed && !pendingMedia) {
                    animatedSource = app.mediaBridge.animatedSource(model.mediaKey)
                    if (MotionPolicy.autoplay)
                        return
                }
                if (bridgeFailed)
                    app.mediaBridge.retry(bridgeCacheKey)
                bridgeFailed = false
                retrySeconds = 0
                bridgeSource = app.mediaBridge.mediaSource(
                    model.mediaKey,
                    model.mediaThumbAvailable ? "thumb" : "full")
            }
            // A released picture re-reads the bridge's in-memory bytes, which
            // may have been evicted meanwhile; fetch them again.
            function reloadEvictedBytes() {
                if (!usesBridge || bridgeSource.length === 0
                    || img.status !== Image.Error
                    || app.mediaBridge.cachedSource(bridgeCacheKey).length > 0)
                    return
                bridgeSource = ""
                refreshBridgeSource()
            }
            readonly property bool starEligible:
                imageBox.isRasterImage && model.mediaSourceAvailable === true
                && app.mediaBridge.supported && !imageBox.pendingMedia
            property bool starred: false
            function refreshStarredState() {
                var a = (typeof app !== "undefined" && app) ? app : null
                imageBox.starred = imageBox.starEligible && a !== null
                    && a.isChatGifStarred(model.mediaKey || "")
            }
            readonly property bool skeletonWanted: root.mediaPixmapRetained
                && !root.spoilerHidden
                && img.status !== Image.Ready
                && animatedImg.status !== AnimatedImage.Ready
            readonly property bool skeletonHeld: root.scrollHoldsParts
            property int heldSkeleton: -1
            onSkeletonHeldChanged: heldSkeleton = skeletonHeld ? (skeletonWanted ? 1 : 0) : -1
            readonly property bool skeletonBuilt: heldSkeleton >= 0 ? heldSkeleton === 1
                                                                    : skeletonWanted
            Component.onCompleted: {
                if (skeletonHeld)
                    heldSkeleton = skeletonWanted ? 1 : 0
                refreshBridgeSource()
                refreshStarredState()
            }
            onGifModeChanged: if (isGif) refreshBridgeSource()
            onMediaIdentityChanged: {
                animatedSource = ""
                bridgeSource = ""
                bridgeFailed = false
                refreshBridgeSource()
                refreshStarredState()
            }
            Connections {
                target: root
                function onMediaInBandChanged() {
                    if (root.mediaInBand && imageBox.bridgeSource.length === 0
                        && imageBox.animatedSource.length === 0)
                        imageBox.refreshBridgeSource()
                }
            }
            onStarEligibleChanged: refreshStarredState()
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
                    if (cacheKey === imageBox.bridgeCacheKey)
                        imageBox.refreshBridgeSource()
                }
                function onMediaCached(cacheKey) {
                    // A re-cached key has the same bytes; a new revision URL
                    // would blank the drawn picture while it decodes again.
                    if (cacheKey === imageBox.bridgeCacheKey
                        && !(img.status === Image.Ready
                             && imageBox.bridgeSource.length > 0))
                        imageBox.bridgeSource = app.mediaBridge.cachedSource(cacheKey)
                    if (imageBox.starEligible && cacheKey === "full:" + (model.mediaKey || ""))
                        Qt.callLater(imageBox.refreshStarredState)
                }
                function onAnimatedMediaReady(cacheKey) {
                    if (cacheKey === "full:" + (model.mediaKey || "")) {
                        imageBox.animatedSource = app.mediaBridge.animatedSource(model.mediaKey)
                        Qt.callLater(imageBox.refreshStarredState)
                    }
                }
                function onMediaFetchFailed(cacheKey, category) {
                    if (cacheKey === imageBox.bridgeCacheKey) {
                        imageBox.retrySeconds = category === "server_unreachable"
                            ? app.mediaBridge.failureRetrySeconds(cacheKey) : 0
                        imageBox.bridgeFailed = true
                    }
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
                function onCountChanged() {
                    imageBox.refreshStarredState()
                }
            }

            readonly property string resolvedSource:
                usesBridge ? bridgeSource
                           : root.isSvgImage ? ""
                           : (model.mediaThumbUrl
                              && model.mediaThumbUrl.toString().length > 0
                              ? model.mediaThumbUrl
                              : (model.mediaUrl || ""))
            readonly property string roundedSource:
                resolvedSource.indexOf("image://lightning-media/") === 0
                ? resolvedSource + "|shape:round:35"
                : resolvedSource

            Loader {
                anchors.fill: parent
                active: imageBox.skeletonBuilt
                sourceComponent: Skeleton {
                    objectName: "imageSkeleton"
                    radius: AppTheme.radiusSm
                    visible: !root.mediaHidden
                             && img.status !== Image.Ready
                             && animatedImg.status !== AnimatedImage.Ready
                    active: root.rowOnScreen && !imageBox.bridgeFailed
                            && img.status !== Image.Error
                }
            }
            Rectangle {
                objectName: "imageGifBadge"
                z: 1
                visible: imageBox.isGif && !root.mediaHidden
                         && !(imageBox.animateGif
                              && animatedImg.status === AnimatedImage.Ready)
                         && !imageBox.bridgeFailed
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 5
                radius: 3
                color: AppTheme.overlayScrim
                width: placeholderGifLabel.implicitWidth + 8
                height: placeholderGifLabel.implicitHeight + 4
                Loader {
                    id: placeholderGifLabel
                    anchors.centerIn: parent
                    active: imageBox.isGif
                    sourceComponent: Label {
                        text: qsTr("GIF")
                        color: AppTheme.scrimInk
                        font.pixelSize: AppTheme.fontMicro
                        font.weight: Font.Bold
                    }
                }
            }

            Image {
                id: img
                objectName: "messageStillImage"
                anchors.fill: parent
                visible: !imageBox.animateGif && !root.mediaHidden
                         && !root.spoilerHidden
                fillMode: Image.PreserveAspectFit
                source: (imageBox.animateGif || root.mediaHidden
                         || root.spoilerHidden || !root.mediaPixmapRetained)
                        ? "" : imageBox.roundedSource
                sourceSize.width: 640
                asynchronous: true
                cache: root.mediaPixmapCacheAllowed
                onStatusChanged: if (status === Image.Error)
                                     Qt.callLater(imageBox.reloadEvictedBytes)
            }

            AnimatedImage {
                id: animatedImg
                anchors.fill: parent
                visible: imageBox.animateGif
                fillMode: Image.PreserveAspectFit
                source: imageBox.animateGif && root.mediaPixmapRetained
                        ? imageBox.animatedSource : ""
                asynchronous: true
                cache: root.mediaPixmapCacheAllowed
                playing: imageBox.animateGif && root.rowOnScreen
                         && !MotionPolicy.suspended
                AnimationFileHold {}
            }

            Loader {
                anchors.fill: parent
                active: root.mediaHidden || root.spoilerHidden
                sourceComponent: root.mediaHidden ? imageHiddenCover : imageSpoilerCover
                Component {
                    id: imageHiddenCover
                    MediaHiddenPlaceholder {
                        hidden: root.mediaHidden
                        onRevealRequested: root.setMediaHidden(false)
                    }
                }
                Component {
                    id: imageSpoilerCover
                    SpoilerCover {
                        covered: true
                        source: root.mediaPixmapRetained ? imageBox.resolvedSource : ""
                        reason: model.mediaSpoilerReason || ""
                        interactive: root.rowActionsEnabled
                        onRevealRequested: root.revealSpoiler()
                    }
                }
            }

            Loader {
                active: root.isSvgImage
                visible: !root.mediaHidden && img.status === Image.Ready
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 5
                sourceComponent: Rectangle {
                    objectName: "svgPreviewBadge"
                    radius: 3
                    color: AppTheme.overlayScrim
                    width: svgBadgeLabel.implicitWidth + 8
                    height: svgBadgeLabel.implicitHeight + 4
                    Label {
                        id: svgBadgeLabel
                        anchors.centerIn: parent
                        text: "SVG"
                        color: AppTheme.scrimInk
                        font.pixelSize: AppTheme.fontMicro
                        font.weight: Font.Bold
                    }
                }
            }

            MouseArea {
                anchors.fill: parent
                enabled: !root.mediaHidden && root.rowActionsEnabled
                visible: enabled
                cursorShape: Qt.PointingHandCursor
                onClicked: imageBox.activate()
            }

            function activate() {
                if (root.spoilerHidden) {
                    root.revealSpoiler()
                    return
                }
                if (imageBox.bridgeFailed) {
                    imageBox.refreshBridgeSource()
                    return
                }
                if (root.isSvgImage) {
                    if (root.timelineView && root.timelineView.saveMedia
                        && model.mediaSourceAvailable === true)
                        root.timelineView.saveMedia(model.mediaKey || "",
                                                    model.mediaFilename || "",
                                                    model.mediaMimetype
                                                    || "image/svg+xml", true)
                    return
                }
                if (root.timelineView && root.timelineView.openImage)
                    root.timelineView.openImage(model.mediaKey || "",
                                                 model.mediaUrl)
                else if (model.mediaUrl && model.mediaUrl.toString().length > 0)
                    app.media.openExternal(model.mediaUrl)
            }

            Loader {
                id: gifStarLoader
                anchors.fill: parent
                active: imageBox.starEligible && !root.mediaHidden
                        && !root.spoilerHidden
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
                            opacity: revealed ? 1 : 0
                            Behavior on opacity { NumberAnimation { duration: 100 } }

                            Accessible.role: Accessible.Button
                            Accessible.name: imageBox.starred
                                ? qsTr("Remove from saved") : qsTr("Save image")
                            Accessible.onPressAction: gifStarButton.activate()

                            function activate() {
                                var key = model.mediaKey || ""
                                if (!key)
                                    return
                                if (app.isChatGifStarred(key))
                                    app.unstarChatGif(key)
                                else
                                    app.starChatGif(key)
                            }

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
                            TapHandler {
                                enabled: gifStarButton.revealed
                                         && root.rowActionsEnabled
                                onTapped: gifStarButton.activate()
                            }
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
                      ? (imageBox.retrySeconds > 0
                         ? qsTr("Media server unreachable — retrying in %1 min")
                               .arg(Math.max(1, Math.ceil(imageBox.retrySeconds / 60)))
                         : qsTr("Image failed to load — click to retry"))
                      : qsTr("Image unavailable")
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                visible: !root.mediaHidden
                         && (img.status === Image.Error
                             || imageBox.bridgeFailed)
            }
        }
    }

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
            readonly property string animatedCacheKey:
                "full:" + (model.mediaKey || "")
            property string bridgeSource: ""
            property bool bridgeFailed: false

            readonly property string declaredMimetype:
                (model.mediaMimetype || "").toLowerCase()
            readonly property bool maybeAnimated:
                declaredMimetype === "" || declaredMimetype === "image/gif"
                || declaredMimetype === "image/webp"
            readonly property int gifMode: MotionPolicy.mode
            property bool gifHovered: false
            readonly property bool playAnimation:
                MotionPolicy.plays(gifHovered)
            onGifModeChanged: refreshAnimatedSource()
            property string animatedSource: ""

            function refreshBridgeSource() {
                if (!usesBridge || !model.mediaKey) return
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
                    if (cacheKey === stickerBox.bridgeCacheKey)
                        stickerBox.refreshBridgeSource()
                }
                function onAnimatedMediaReady(cacheKey) {
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
                visible: !root.mediaHidden && !stickerAnim.animating
                fillMode: Image.PreserveAspectFit
                source: root.mediaHidden || !root.mediaPixmapRetained
                        ? "" : stickerBox.resolvedSource
                sourceSize.width: 360
                asynchronous: true
                cache: root.mediaPixmapCacheAllowed
            }
            AnimatedImage {
                id: stickerAnim
                objectName: "stickerAnimatedMedia"
                anchors.fill: parent
                fillMode: Image.PreserveAspectFit
                source: (!root.mediaHidden && root.mediaPixmapRetained
                         && stickerBox.gifMode !== 2)
                        ? stickerBox.animatedSource : ""
                readonly property bool animating:
                    status === AnimatedImage.Ready
                    && stickerBox.animatedSource.length > 0
                    && stickerBox.playAnimation && !root.mediaHidden
                visible: animating
                playing: animating && root.rowOnScreen
                         && !MotionPolicy.suspended
                asynchronous: true
                cache: root.mediaPixmapCacheAllowed
                AnimationFileHold {}
            }
            Loader {
                anchors.fill: parent
                active: root.mediaHidden
                sourceComponent: MediaHiddenPlaceholder {
                    hidden: root.mediaHidden
                    onRevealRequested: root.setMediaHidden(false)
                }
            }
            HoverHandler {
                id: stickerHover
                enabled: !root.mediaHidden
                onHoveredChanged: stickerBox.gifHovered = hovered
            }
            readonly property bool loadFailed:
                !root.mediaHidden
                && (stickerImg.status === Image.Error || bridgeFailed)
            PlainToolTip {
                objectName: "stickerTip"
                visible: stickerHover.hovered && text.length > 0
                delay: 400
                text: stickerBox.loadFailed
                      ? qsTr("Sticker failed to load — click to retry")
                      : (model.body || "")
                x: Math.max(0, (parent.width - width) / 2)
                y: Math.max(0, parent.height - height - AppTheme.spacing4)
            }
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
            Item {
                id: stickerError
                objectName: "stickerError"
                anchors.fill: parent
                clip: true
                visible: stickerBox.loadFailed
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Sticker failed to load — click to retry")
                Accessible.onPressAction: stickerBox.refreshBridgeSource()
                Column {
                    anchors.centerIn: parent
                    width: parent.width - 8
                    spacing: 2
                    Icon {
                        anchors.horizontalCenter: parent.horizontalCenter
                        name: "refresh"
                        size: Math.max(12, Math.min(24,
                                                    stickerError.height - 12))
                        color: AppTheme.textMuted
                    }
                    Label {
                        objectName: "stickerErrorLabel"
                        width: parent.width
                        visible: stickerError.height >= 48
                        horizontalAlignment: Text.AlignHCenter
                        text: qsTr("Retry")
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        elide: Text.ElideRight
                        maximumLineCount: 1
                    }
                }
            }
        }
    }

    Component {
        id: videoComponent
        Item {
            id: videoBox
            objectName: "videoMedia"
            readonly property real maxW: {
                var cap = Math.min(
                    560, Math.max(280, root.contentInnerCap * 0.72))
                return Math.max(1, Math.min(cap, root.contentInnerCap))
            }
            readonly property size learnedDims:
                model.mediaWidth > 0 ? Qt.size(0, 0)
                : app.settings.knownVideoDimensions(model.mediaKey || "")
            readonly property real natW: model.mediaWidth > 0
                ? model.mediaWidth
                : (learnedDims.width > 0 ? learnedDims.width : 0)
            readonly property real natH: model.mediaHeight > 0
                ? model.mediaHeight
                : (learnedDims.height > 0 ? learnedDims.height : 0)
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
            implicitHeight: Math.max(1, Math.min(maxH, dispW * ratio))

            property bool playerActive: false
            readonly property string mediaIdentity:
                root.actionKey + "\u001f" + (model.mediaKey || "")
            readonly property bool playbackAvailable:
                model.mediaSourceAvailable === true && app.mediaBridge.supported

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
                var prefetchSize = app.settings.videoPrefetchEnabled
                                   ? (model.mediaSize
                                      || app.settings.knownMediaSizeBytes(
                                             model.mediaKey || "")
                                      || 0)
                                   : 0
                var prefetchCap = app.settings.videoPrefetchMaxMb * 1024 * 1024
                if (model.mediaThumbAvailable === true) {
                    bridgeSource = app.mediaBridge.mediaSource(model.mediaKey,
                                                               "thumb")
                } else if (root.speculativeMediaAllowed) {
                    bridgeSource = app.mediaBridge.videoPosterSource(
                        model.mediaKey, prefetchSize, prefetchCap)
                } else {
                    bridgeSource = ""
                }
                if (root.speculativeMediaAllowed && playbackAvailable
                    && prefetchSize > 0)
                    app.mediaBridge.prefetchPlayable(model.mediaKey,
                                                     prefetchSize, prefetchCap)
            }
            readonly property bool coverOnScreen: root.speculativeMediaAllowed
            onCoverOnScreenChanged: {
                if (coverOnScreen && bridgeSource.length === 0
                    && !bridgeFailed)
                    videoSourceRefresh.restart()
            }
            function reloadEvictedBytes() {
                if (!usesBridge || bridgeSource.length === 0
                    || thumbImg.status !== Image.Error
                    || app.mediaBridge.cachedSource(bridgeCacheKey).length > 0)
                    return
                bridgeSource = ""
                refreshBridgeSource()
            }
            function resetForMedia() {
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
            readonly property int liveParts:
                (!showPlaceholder && root.mediaPixmapRetained
                 && thumbImg.status !== Image.Ready ? 1 : 0)
                | (bridgeFailed ? 2 : 0)
            readonly property bool partsHeld: root.scrollHoldsParts
            property int heldParts: -1
            onPartsHeldChanged: heldParts = partsHeld ? liveParts : -1
            readonly property int builtParts: heldParts >= 0 ? heldParts : liveParts
            Component.onCompleted: {
                if (partsHeld)
                    heldParts = liveParts
                resetForMedia()
            }
            onMediaIdentityChanged: resetForMedia()
            Connections {
                target: app.mediaBridge
                enabled: videoBox.usesBridge
                function onMediaCached(cacheKey) {
                    if (cacheKey === videoBox.bridgeCacheKey
                        && !(thumbImg.status === Image.Ready
                             && videoBox.bridgeSource.length > 0))
                        videoBox.bridgeSource =
                            app.mediaBridge.cachedSource(cacheKey)
                }
                function onMediaFetchFailed(cacheKey, category) {
                    if (cacheKey === videoBox.bridgeCacheKey)
                        videoBox.bridgeFailed = true
                }
                function onMediaRetryable(cacheKey) {
                    if (cacheKey === videoBox.bridgeCacheKey)
                        videoBox.refreshBridgeSource()
                }
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
                return AppTheme.clockText(Math.round(ms / 1000))
            }
            readonly property real resumeMs: {
                var revision = app.playback.resumeRevision
                return app.playback.videoResumePosition
                       ? app.playback.videoResumePosition(model.mediaKey || "")
                       : 0
            }
            readonly property bool watched: {
                var revision = app.playback.resumeRevision
                return app.playback.videoFinished
                       ? app.playback.videoFinished(model.mediaKey || "")
                       : false
            }
            function activate() {
                if (root.spoilerHidden) {
                    root.revealSpoiler()
                    return
                }
                if (videoBox.playbackAvailable)
                    videoBox.playerActive = true
                else if (model.mediaUrl
                         && model.mediaUrl.toString().length > 0)
                    app.media.openExternal(model.mediaUrl)
            }
            property bool openedByKey: false
            onPlayerActiveChanged: if (!playerActive) openedByKey = false
            function activateFromKey(event) {
                if (!root.rowActionsEnabled) {
                    event.accepted = false
                    return
                }
                const toggleKey = event.key === Qt.Key_Space
                                  || event.key === Qt.Key_K
                if (videoBox.playerActive) {
                    if (!toggleKey || !videoPlayerLoader.item) {
                        event.accepted = false
                        return
                    }
                    if (videoPlayerLoader.item.ready)
                        videoPlayerLoader.item.togglePlayPause()
                    event.accepted = true
                    return
                }
                if (event.key === Qt.Key_K) {
                    event.accepted = false
                    return
                }
                videoBox.openedByKey = true
                videoBox.activate()
                if (!videoBox.playerActive)
                    videoBox.openedByKey = false
                event.accepted = true
            }
            readonly property string coverName:
                model.mediaFilename || model.body || qsTr("video")
            activeFocusOnTab: !videoBox.playerActive && root.rowActionsEnabled
            Keys.onPressed: (event) => {
                if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier
                                       | Qt.MetaModifier))
                    return
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                        || event.key === Qt.Key_Space || event.key === Qt.Key_K)
                    videoBox.activateFromKey(event)
            }
            Accessible.role: Accessible.Button
            Accessible.onPressAction: {
                if (!videoBox.playerActive && root.rowActionsEnabled)
                    videoBox.activate()
            }
            Accessible.name: videoBox.watched
                ? qsTr("Replay %1").arg(videoBox.coverName)
                : videoBox.resumeMs > 0
                  ? qsTr("Resume %1 from %2").arg(videoBox.coverName)
                        .arg(AppTheme.clockPosition(videoBox.resumeMs))
                  : qsTr("Play %1").arg(videoBox.coverName)
            function saveVideo() {
                if (root.timelineView && root.timelineView.saveMedia)
                    root.timelineView.saveMedia(model.mediaKey || "",
                                                 model.mediaFilename || "",
                                                 model.mediaMimetype || "",
                                                 false)
            }

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
                Label {
                    objectName: "videoPlaceholderName"
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 30
                    width: Math.min(implicitWidth, videoBox.dispW - 48)
                    horizontalAlignment: Text.AlignHCenter
                    text: model.mediaFilename || model.body || qsTr("Video")
                    textFormat: Text.PlainText
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    elide: Label.ElideMiddle
                }
            }
            Loader {
                anchors.fill: parent
                active: (videoBox.builtParts & 1) !== 0
                sourceComponent: Skeleton {
                    objectName: "videoPosterSkeleton"
                    radius: AppTheme.radiusSm
                    visible: thumbImg.status !== Image.Ready
                            && !videoBox.showPlaceholder
                    active: root.rowOnScreen && !videoBox.showPlaceholder
                }
            }
            Image {
                id: thumbImg
                objectName: "videoPosterImage"
                anchors.fill: parent
                fillMode: Image.PreserveAspectCrop
                source: root.mediaPixmapRetained && videoBox.usesBridge
                        && !root.spoilerHidden
                        ? (videoBox.bridgeSource.indexOf("image://lightning-media/") === 0
                           ? videoBox.bridgeSource + "|shape:round:35"
                           : videoBox.bridgeSource)
                        : ""
                sourceSize.width: 640
                asynchronous: true
                cache: root.mediaPixmapCacheAllowed
                visible: status === Image.Ready
                onStatusChanged: if (status === Image.Error)
                                     Qt.callLater(videoBox.reloadEvictedBytes)
            }
            Rectangle {
                id: videoPlayCircle
                anchors.centerIn: parent
                width: 44; height: 44; radius: 22
                color: AppTheme.overlayScrim
                Icon {
                    objectName: "videoCoverGlyph"
                    anchors.centerIn: parent
                    name: videoBox.watched ? "refresh" : "play_arrow"
                    size: 26
                    color: AppTheme.scrimInk
                }
            }
            Rectangle {
                objectName: "videoResumeChip"
                visible: videoBox.resumeMs > 0 && !videoBox.playerActive
                anchors.top: videoPlayCircle.bottom
                anchors.topMargin: AppTheme.spacing6
                anchors.horizontalCenter: parent.horizontalCenter
                radius: 3
                color: AppTheme.overlayScrim
                width: Math.min(resumeLabel.implicitWidth + 12,
                                videoBox.width - 12)
                height: resumeLabel.implicitHeight + 4
                Label {
                    id: resumeLabel
                    anchors.centerIn: parent
                    width: Math.min(implicitWidth, parent.width - 12)
                    text: qsTr("Resume from %1")
                          .arg(AppTheme.clockPosition(videoBox.resumeMs))
                    color: AppTheme.scrimInk
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    font.weight: AppTheme.weightStrong
                    elide: Label.ElideRight
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
            IconButton {
                objectName: "videoSaveButton"
                visible: !videoBox.playerActive && videoBox.playbackAvailable
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: 5
                iconName: "download"
                iconSize: 15
                implicitWidth: 26; implicitHeight: 26
                Accessible.name: qsTr("Download %1")
                    .arg(model.mediaFilename || qsTr("video"))
                ToolTip.text: qsTr("Download")
                ToolTip.visible: hovered
                ToolTip.delay: 600
                onClicked: videoBox.saveVideo()
            }
            Loader {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.margins: 5
                active: (videoBox.builtParts & 2) !== 0
                sourceComponent: IconButton {
                    objectName: "videoThumbRetryButton"
                    visible: !videoBox.playerActive && videoBox.bridgeFailed
                             && root.rowActionsEnabled
                    iconName: "refresh"
                    iconSize: 15
                    implicitWidth: 26; implicitHeight: 26
                    Accessible.name: qsTr("Retry the video preview")
                    ToolTip.text: qsTr("Retry preview")
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    onClicked: videoBox.refreshBridgeSource()
                }
            }
            TapHandler {
                enabled: !videoBox.playerActive && root.rowActionsEnabled
                         && !root.spoilerHidden
                onTapped: videoBox.activate()
            }
            Rectangle {
                objectName: "videoCoverFocusRing"
                anchors.fill: parent
                radius: AppTheme.radiusSm
                color: "transparent"
                border.width: 2
                border.color: AppTheme.focusRing
                visible: videoBox.activeFocus && !videoBox.playerActive
            }
            Loader {
                id: videoPlayerLoader
                anchors.fill: parent
                active: videoBox.playerActive || root.spoilerHidden
                onLoaded: {
                    if (!videoBox.openedByKey || sourceComponent !== videoPlayerCard)
                        return
                    videoBox.openedByKey = false
                    item.focusControls()
                }
                visible: active
                sourceComponent: root.spoilerHidden && !videoBox.playerActive
                                 ? videoSpoilerCover : videoPlayerCard
                Component {
                    id: videoSpoilerCover
                    SpoilerCover {
                        covered: true
                        focusable: false
                        source: root.mediaPixmapRetained && videoBox.usesBridge
                                ? videoBox.bridgeSource : ""
                        reason: model.mediaSpoilerReason || ""
                        interactive: root.rowActionsEnabled
                        onRevealRequested: root.revealSpoiler()
                    }
                }
                Component {
                    id: videoPlayerCard
                    VideoPlayerCard {
                        mediaKey: model.mediaKey || ""
                        ownerKey: root.actionKey + "\u001f"
                                  + (model.mediaKey || "")
                        filename: model.mediaFilename || ""
                        declaredDurationMs: model.mediaDurationMs || 0
                        rowOnScreen: root.rowOnScreen
                        senderName: model.senderDisplayName || model.sender || ""
                        timestamp: model.timestamp || null
                        posterSource: videoBox.usesBridge && !videoBox.bridgeFailed
                                      ? videoBox.bridgeSource : ""
                        canSave: videoBox.playbackAvailable
                        onSaveRequested: videoBox.saveVideo()
                        onCloseRequested: {
                            const hadFocus = activeFocus
                            videoBox.playerActive = false
                            if (hadFocus)
                                videoBox.forceActiveFocus(Qt.TabFocusReason)
                        }
                    }
                }
            }
        }
    }

    Component {
        id: audioComponent
        AudioPlayerCard {
            objectName: "audioMedia"
            hostContentWidth: root.contentInnerCap
            inBubble: root.bubbleMode
            mediaKey: model.mediaKey || ""
            eventId: model.eventId || root.actionKey
            roomId: typeof app !== "undefined" ? app.currentRoomId : ""
            senderName: model.senderDisplayName || model.sender || ""
            senderId: model.sender || ""
            senderAvatarMxc: model.senderAvatarMxc || ""
            threadRootId: model.threadRootId || ""
            filename: model.mediaFilename || model.body || ""
            mimetype: model.mediaMimetype || ""
            fileSize: model.mediaSize || 0
            durationMs: model.mediaDurationMs || 0
            thumbAvailable: model.mediaThumbAvailable === true
            isVoice: model.mediaIsVoice === true
            waveform: model.mediaWaveform || []
            rowOnScreen: root.rowOnScreen
            prefetchAllowed: root.speculativeMediaAllowed
            canSave: model.mediaSourceAvailable === true
                     && app.mediaBridge.supported
            onSaveRequested: {
                if (root.timelineView && root.timelineView.saveMedia)
                    root.timelineView.saveMedia(model.mediaKey || "",
                                                 model.mediaFilename || "",
                                                 model.mediaMimetype || "",
                                                 false)
            }
            onOpenExternalRequested: {
                if (model.mediaUrl && model.mediaUrl.toString().length > 0)
                    app.media.openExternal(model.mediaUrl)
            }
            onArtworkViewRequested: (source, title) => {
                if (root.timelineView && root.timelineView.openArtwork)
                    root.timelineView.openArtwork(source, title)
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

            readonly property var tlView: root.timelineView
            readonly property bool saving:
                (model.mediaKey || "") !== ""
                && typeof app !== "undefined" && app && app.mediaBridge
                && app.mediaBridge.savingKeys.indexOf(model.mediaKey) >= 0
            readonly property bool savedFlash:
                tlView && tlView.lastSavedKey !== undefined
                && (model.mediaKey || "") !== ""
                && tlView.lastSavedKey === model.mediaKey
            readonly property bool savedOk:
                savedFlash && tlView.lastSaveOk === true
            readonly property bool savedCancelled:
                savedFlash && !savedOk && tlView.lastSaveCancelled === true

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
                var m = (mime || "").toLowerCase()
                var slash = m.indexOf("/")
                if (slash < 0) return ""
                var top = m.substring(0, slash)
                var sub = m.substring(slash + 1)
                var semi = sub.indexOf(";")
                if (semi >= 0) sub = sub.substring(0, semi).trim()
                if (sub === "epub+zip") return "EPUB"
                if (/(^|[-.+])(zip|tar|gzip|bzip2?|xz|7z|rar|zstd|lzma)($|[-.+])/
                        .test(sub.replace(/^x-/, ""))
                        || sub.indexOf("compressed") >= 0
                        || sub === "java-archive"
                        || sub === "vnd.android.package-archive")
                    return qsTr("Archive")
                if (top === "text") {
                    if (sub === "csv") return "CSV"
                    if (sub === "html") return "HTML"
                    if (sub === "markdown" || sub === "x-markdown")
                        return "Markdown"
                    if (sub === "calendar") return qsTr("Calendar")
                    if (sub === "vcard" || sub === "x-vcard")
                        return qsTr("Contact")
                    return qsTr("Text")
                }
                if (top === "font") return qsTr("Font")
                if (sub.toLowerCase().indexOf("svg") === 0) return "SVG"
                var plus = sub.lastIndexOf("+")
                if (plus >= 0) sub = sub.substring(plus + 1)
                if (sub.indexOf("x-") === 0) sub = sub.substring(2)
                if (sub === "octet-stream") return qsTr("File")
                return sub.length > 0 && sub.length <= 12 ? sub.toUpperCase()
                                                          : qsTr("File")
            }

            RowLayout {
                id: fileRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 10

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
                        objectName: "fileCardSubtitle"
                        text: {
                            var size = root.embedSizeText(model.mediaSize || 0)
                            var kind = fileCard.fileTypeLabel(model.mediaMimetype)
                            var status = fileCard.saving ? qsTr("Saving…")
                                       : fileCard.savedFlash
                                         ? (fileCard.savedOk ? qsTr("Saved")
                                            : fileCard.savedCancelled
                                              ? qsTr("Cancelled")
                                              : qsTr("Save failed"))
                                         : ""
                            var base = size.length === 0 ? (kind || "")
                                     : kind ? size + " • " + kind : size
                            return status ? base + " • " + status : base
                        }
                        textFormat: Text.PlainText
                        color: fileCard.savedFlash && !fileCard.savedOk
                               && !fileCard.savedCancelled
                               ? AppTheme.danger
                               : fileCard.savedFlash ? AppTheme.success
                               : AppTheme.textMuted
                        font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                        elide: Label.ElideRight
                        Layout.fillWidth: true
                    }
                }

                Loader {
                    active: fileCard.saving
                    visible: active
                    sourceComponent: RowLayout {
                        spacing: 10
                        AppBusyIndicator {
                            size: 26
                            Layout.preferredWidth: 26
                            Layout.preferredHeight: 26
                        }
                        IconButton {
                            objectName: "fileSaveCancelButton"
                            iconName: "close"
                            iconSize: 16
                            implicitWidth: 30; implicitHeight: 30
                            Accessible.name: qsTr("Cancel download")
                            ToolTip.text: qsTr("Cancel download")
                            ToolTip.visible: hovered
                            ToolTip.delay: 600
                            onClicked: app.mediaBridge.cancelSave(model.mediaKey || "")
                        }
                    }
                }
                Icon {
                    visible: fileCard.savedFlash && !fileCard.saving
                             && !fileCard.savedCancelled
                    name: fileCard.savedOk ? "check_circle" : "error"
                    size: 20
                    color: fileCard.savedOk ? AppTheme.success : AppTheme.danger
                }
                IconButton {
                    objectName: "fileSaveButton"
                    visible: model.mediaSourceAvailable === true
                             && app.mediaBridge.supported && !fileCard.saving
                    iconName: fileCard.savedFlash && !fileCard.savedOk
                              && !fileCard.savedCancelled
                              ? "refresh" : "download"
                    iconSize: 18
                    implicitWidth: 30; implicitHeight: 30
                    Accessible.name: qsTr("Download %1")
                        .arg(model.mediaFilename || qsTr("file"))
                    ToolTip.text: fileCard.savedCancelled
                                  ? qsTr("Download again")
                                  : fileCard.savedFlash && !fileCard.savedOk
                                    ? qsTr("Try again") : qsTr("Download")
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    onClicked: {
                        if (root.timelineView && root.timelineView.saveMedia)
                            root.timelineView.saveMedia(model.mediaKey || "",
                                                         model.mediaFilename || "",
                                                         model.mediaMimetype || "",
                                                         false)
                    }
                }
                Loader {
                    active: !(model.mediaSourceAvailable === true)
                            && (model.mediaUrl
                                ? model.mediaUrl.toString().length > 0
                                : false)
                    visible: active
                    sourceComponent: IconButton {
                        iconName: "open_in_full"
                        iconSize: 18
                        implicitWidth: 30; implicitHeight: 30
                        Accessible.name: qsTr("Open file")
                        onClicked: app.media.openExternal(model.mediaUrl)
                    }
                }
            }
        }
    }

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
                        name: "explore"
                        size: 18
                        color: AppTheme.textMuted
                    }
                    Label {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
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
                                  .arg(Math.round(model.locationUncertaintyM)
                                       .toLocaleString(Qt.locale(AppLocale.name), "f", 0))
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
                        var lat = model.locationLat
                        var lon = model.locationLon
                        app.media.openWebUrl(
                            "https://www.openstreetmap.org/?mlat=" + lat
                            + "&mlon=" + lon + "#map=16/" + lat + "/" + lon)
                    }
                }

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
                !pollEnded && !root.readOnlyView && app.composer.pollsSupported()
            readonly property string pollThreadRoot:
                root.inThreadPanel ? (app.thread.rootEventId || "") : ""
            readonly property int totalVotes: {
                var sum = 0
                for (var i = 0; i < pollAnswers.length; ++i)
                    sum += (pollAnswers[i].count || 0)
                return sum
            }
            property var pendingSelection: null
            property bool capHintShown: false
            function echoedSelection() {
                var ids = []
                for (var i = 0; i < pollAnswers.length; ++i)
                    if (pollAnswers[i].byMe) ids.push(pollAnswers[i].id)
                return ids
            }
            function ownSelection() {
                return pendingSelection !== null
                       ? pendingSelection.slice() : echoedSelection()
            }
            function sameSet(a, b) {
                if (a.length !== b.length) return false
                for (var i = 0; i < a.length; ++i)
                    if (b.indexOf(a[i]) < 0) return false
                return true
            }
            onPollAnswersChanged: {
                if (pendingSelection !== null
                        && sameSet(echoedSelection(), pendingSelection))
                    pendingSelection = null
            }
            Timer {
                id: pendingExpiry
                interval: 8000
                onTriggered: pollCard.pendingSelection = null
            }
            Timer {
                id: capHintTimer
                interval: 3500
                onTriggered: pollCard.capHintShown = false
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
                    else {
                        capHintShown = true
                        capHintTimer.restart()
                        return
                    }
                } else {
                    selection = ownSelection().indexOf(answerId) >= 0
                                ? [] : [answerId]
                }
                pendingSelection = selection
                pendingExpiry.restart()
                app.composer.votePoll(eventId, selection, pollThreadRoot)
            }

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
                    // MSC3381 question and answers are plain text from the
                    // sender: never AutoText, which would render their markup
                    // (fetching remote <img>s, live <a>s with a spoofed label).
                    Label {
                        objectName: "pollQuestion"
                        text: model.pollQuestion || ""
                        textFormat: Text.PlainText
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
                                    objectName: "pollAnswerText"
                                    text: modelData.text || ""
                                    textFormat: Text.PlainText
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
                    objectName: "pollCapHint"
                    visible: pollCard.capHintShown
                    text: qsTr("You can pick up to %n answer(s)",
                               "poll selection cap", pollCard.maxSelections)
                    color: AppTheme.warning
                    font.pixelSize: AppTheme.scaled(11)
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
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

    Loader {
        id: republishLoader
        active: false
        sourceComponent: PrivateImageRepublishDialog {
            onClosed: Qt.callLater(function () { republishLoader.active = false })
        }
    }
}
