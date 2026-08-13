import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    // TableView owns the row rectangle. Nested Loader content must never paint
    // into an adjacent message while a recycled row is being remeasured.
    // Clip everywhere except the thread ListView, which keeps its previous
    // unclipped behaviour. The room timeline's rows are packed edge to edge in
    // a Column, so a row must not paint over its neighbours.
    clip: ListView.view === null
    // The room timeline is a one-column TableView; thread replies still use
    // ListView. Keep the delegate's interaction contract independent of the
    // virtualizer while preserving each view's attached reuse lifecycle.
    // Settable, not readonly: the room timeline instantiates rows in a
    // Repeater/Column where no attached view exists, so it injects itself.
    // The thread panel is still a real ListView and keeps the attached
    // default (including its delegate reuse, which the room timeline no
    // longer does at all).
    property var timelineView:
        TableView.view ? TableView.view : ListView.view
    // v0.5.7: virtual SDK timeline rows (date divider / read marker /
    // timeline start) render as thin separators instead of messages.
    // eventType: 7 = DateDivider, 8 = ReadMarker, 9 = TimelineStart.
    readonly property bool isVirtualRow: model.isVirtual === true
    readonly property bool isStateActivity: model.isStateActivity === true
    readonly property bool isRoutineActivity: model.isRoutineActivity === true
    // State events remain in the authoritative timeline model. This is only
    // a zero-height presentation filter, so toggling the setting restores the
    // same delegates without a resync or a second timeline.
    readonly property bool roomActivityVisible:
        !isRoutineActivity || app.settings.showRoomActivity
    // v0.6.0: the timeline model this delegate's stable-id actions resolve
    // against. The room timeline supplies app.timeline; the thread panel
    // supplies app.thread.model — identical role/invokable surface.
    readonly property var timelineModel:
        root.timelineView && root.timelineView.timelineModel
        ? root.timelineView.timelineModel : app.timeline
    function sourceModelRow(viewRow) {
        return root.timelineView && root.timelineView.sourceRowForViewRow
                ? root.timelineView.sourceRowForViewRow(viewRow) : viewRow
    }
    // v0.6.0: the thread panel pins the root above the reply list; the same
    // row inside the ListView collapses so the root is never duplicated.
    readonly property bool suppressedAsThreadRoot:
        root.timelineView && (root.timelineView.suppressRootEventId || "") !== ""
        && (model.eventId || "") === root.timelineView.suppressRootEventId
    readonly property var stateActivityEntries: model.stateGroupEntries || []
    readonly property bool showsIdentity: model.showSenderIdentity === true

    // Settings → Appearance → Message layout (0 Modern, 1 Bubbles, 2
    // Compact). The thread panel always keeps the Modern rows; Bubbles
    // applies only to direct-message timelines (never ordinary rooms).
    readonly property int timelineLayout:
        inThreadPanel ? 0 : app.settings.messageLayout
    property bool isDirectRoom: root.timelineView
                                && root.timelineView.isDirectRoom === true
    readonly property bool bubbleMode: timelineLayout === 1 && isDirectRoom
    readonly property bool compactMode: timelineLayout === 2
    readonly property real bubblePad: bubbleMode ? 10 : 0

    // Group leaders (a new sender block or a lone message) get a slightly
    // more generous gap than the tight 8px so distinct groups read as
    // separated; continuation lines within a group stay at 1px. Compact/IRC
    // stays dense.
    readonly property real messageTopSpacing: showsIdentity
                                               ? (compactMode ? 2
                                                              : AppTheme.spacingM)
                                               : (compactMode ? 0 : 1)
    readonly property real avatarGutterWidth: compactMode ? 8
                                              : (bubbleMode ? 44 : 40)

    // v0.7: shared on-screen check for skeleton shimmer and GIF playback —
    // rows pooled in the cache buffer are `visible` but not on screen, and
    // must not burn animation work.
    // One geometry test for both hosts. The room timeline instantiates every
    // loaded row, so "instantiated" no longer implies "on screen" and this
    // must be a real intersection against the viewport. It is also safe to
    // read our own y/height now: the previous version had to avoid them
    // because they fed back into the rowHeightProvider measuring this same
    // object, and that provider is gone.
    // Settable for the same reason timelineView is: in the room timeline this
    // item sits inside a per-row Loader, so its own y is 0 relative to that
    // Loader and cannot be compared against the viewport. The Loader knows its
    // content-space position and assigns this. The default binding below still
    // serves the thread ListView, where the delegate IS the positioned item.
    property bool rowOnScreen:
        !!root.timelineView
        && (y + height) > root.timelineView.contentY
        && y < (root.timelineView.contentY + root.timelineView.height)

    // Once the verified-session bootstrap has given up (the automatic key
    // request timed out, or there is no backup to restore from), stop
    // shimmering every undecryptable row forever — hold a static reserved
    // state instead. Keys arriving later (e.g. after manual recovery) still
    // replace the row in place.
    readonly property bool decryptStalled:
        app.cryptoBootstrap
        && (app.cryptoBootstrap.phase
                === CryptoBootstrapModel.ManualRecoveryRequired
            || app.cryptoBootstrap.phase
                === CryptoBootstrapModel.NoBackupAvailable)
    visible: roomActivityVisible && !suppressedAsThreadRoot
    readonly property real naturalImplicitHeight:
        (!roomActivityVisible || suppressedAsThreadRoot) ? 0
        : isVirtualRow ? virtualRow.implicitHeight
        : isStateActivity ? stateActivity.implicitHeight
        : layout.implicitHeight + messageTopSpacing
    property bool heightSeedActive: false
    property real heightSeed: 0
    property string heightSeedIdentity: ""
    property bool heightMeasurementReady: false
    implicitHeight: heightSeedActive ? heightSeed : naturalImplicitHeight

    // Stable key for the pin-one-toolbar-at-a-time state on the ListView.
    // Prefer the SDK item id; fall back to the event id for backends that
    // don't set it. Empty for virtual rows (they have no actions).
    readonly property string actionKey: (model.itemId && model.itemId.length > 0)
                                        ? model.itemId
                                        : (model.eventId || "")
    // ── Find-in-timeline highlighting ────────────────────────────────────
    // The active query, or "" when the reader is not searching. Reads the
    // delegate's OWN model, so a thread panel search never lights up the room
    // timeline behind it.
    readonly property string searchHighlight:
        root.timelineModel && root.timelineModel.searchActive === true
        ? (root.timelineModel.searchQuery || "") : ""
    // Whether this row is the match the find bar is currently sitting on, so
    // it can be distinguished from the other matches on screen.
    readonly property bool isCurrentSearchHit:
        root.searchHighlight !== ""
        && root.timelineModel
        && (model.eventId || "") !== ""
        && (model.eventId || "") === root.timelineModel.searchCurrentEventId

    // Wrap every occurrence of `needle` in a highlight span.
    //
    // The body is RichText — either sanitized HTML from a formatted message or
    // linkified plain text — so a naive string replace would happily rewrite
    // the inside of a tag or an entity and corrupt the markup. This walks the
    // string instead, copying tags through untouched and only ever
    // substituting within text runs. Entities are treated as atomic for the
    // same reason: searching "amp" must not split "&amp;" down the middle.
    function highlightSearchMatches(html, needle, current) {
        if (!needle || needle.length === 0 || !html || html.length === 0)
            return html
        // Cheap reject first. Every loaded row re-evaluates this on each
        // query change, and in this timeline every loaded row is live.
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
                // Malformed tail: copy it through rather than guess.
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
        // Keep a recycled delegate at its own last known height for one event
        // turn while the nested Loaders rebind. After that, expose natural
        // geometry and let TableView measure it. Holding an estimate longer
        // lets child content paint outside stale row bounds and visibly stack.
        heightSeedReleaseTimer.restart()
    }
    onNaturalImplicitHeightChanged: {
        // Nested reply/media/preview Loaders can finish after the initial
        // reuse turn. A custom TableView rowHeightProvider is not guaranteed
        // to be queried again for that implicit-size change, so explicitly
        // coalesce a fresh exact measurement. Do not reactivate the seed:
        // that was what allowed stale row geometry to persist and stack.
        //
        // Readiness IS dropped here, deliberately: a nested Loader mid-swap
        // can expose its previous item's implicit size, and accepting that
        // live would cache a wrong height and shrink-then-grow the row. Keeping
        // readiness across in-place changes was tried in the belief that this
        // line was starving the cache; the real cause was the broken attached
        // -property guard in heightResolutionTimer below, so the debounce
        // stands as originally written.
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
        // A recycled media/preview Loader can expose its previous item's
        // implicit size for one or more queued turns. Commit only after the
        // natural geometry has been quiet for one frame. Every intervening
        // change restarts this timer through the handler above.
        interval: 16
        onTriggered: {
            if (root.heightSeedIdentity !== root.actionKey) {
                root.refreshHeightSeed()
                return
            }
            // root.timelineView ONLY. `TableView.view` here would attach to
            // this Timer, not to the delegate root — the attached `view` is
            // populated only on the item the TableView instantiated, so it was
            // permanently null and this guard returned every single time.
            // That one line disabled the whole exact-height cache: it made
            // rememberDelegateHeight() unreachable, and because it also
            // skipped setting heightMeasurementReady, the rowHeightProvider's
            // own commit path (which waits on that flag) never ran either.
            // Confirmed live: heightCommits=0 heightCached=0 in every gesture
            // of a full session, so contentHeight was a sum of pure metadata
            // estimates while TableView laid the loaded rows out at their real
            // heights. root.timelineView already resolves the attached view in
            // the delegate's own scope and covers the ListView case too.
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
    property string menuEventId: ""

    // v0.7: pooled-delegate reuse. The ListView recycles this delegate for a
    // different row; model-bound state re-derives through its change handlers
    // (onActionKeyChanged -> refreshPreview, onMediaIdentityChanged -> media
    // reset), but transient non-model state — the write-before-use popup
    // targets and the details payload — must be scrubbed so a stale event id
    // or dialog body can never carry across rows. Any popup opened on the old
    // row lives in the Overlay and is closed here defensively. objectName is
    // read by the reuse-safety test.
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

    function openContextMenu(x, y) {
        var eventId = root.eventIdForActions()
        if (eventId === "" || root.isVirtualRow || root.isStateActivity)
            return
        menuEventId = eventId
        var p = root.mapToItem(Overlay.overlay,
                               x === undefined ? root.width : x,
                               y === undefined ? 0 : y)
        root.ensureContextMenu().popup(Overlay.overlay, p.x, p.y)
    }
    function copyToClipboard(value) {
        if (!value || value.length === 0) return
        clipboardHelper.text = value
        clipboardHelper.selectAll()
        clipboardHelper.copy()
        clipboardHelper.text = ""
    }
    // v0.6.0: inside the thread panel a reply targets the thread composer
    // (a rich reply WITHIN the thread via the SDK path); in the room
    // timeline it targets the main composer as before.
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
                                (previewText || "").substring(0, 80))
    }
    activeFocusOnTab: !isVirtualRow && !isStateActivity
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

    // v0.7: recoverable unable-to-decrypt rows show a shimmering text
    // skeleton (keys can still arrive and replace the row in place);
    // deterministic failures (sent before join, sender requires
    // verification, withheld) keep their honest static explanation.
    readonly property bool showsDecryptingSkeleton: {
        if (model.undecryptable !== true || model.redacted)
            return false
        var kind = model.errorKind || ""
        return kind !== "membership" && kind !== "device_trust"
               && kind !== "withheld"
    }

    // v0.5.11: link-preview state for this row, resolved by
    // LinkPreviewController. Calling previewFor() may dispatch an automatic
    // request (unencrypted rooms with auto-load on); encrypted rooms stay in
    // "requires_action" until the explicit Load action.
    property var preview: ({ state: "none" })
    readonly property bool roomEncrypted:
        root.timelineView ? root.timelineView.roomEncrypted === true : false
    function refreshPreview() {
        if (isVirtualRow || isStateActivity || model.redacted || model.isImage || model.isFile
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
    }
    onActionKeyChanged: {
        refreshHeightSeed()
        refreshPreview()
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
        implicitHeight: unreadDivider.visible ? 28
                        : virtualLabel.visible
                        ? virtualLabel.implicitHeight + AppTheme.spacingS
                        : 0
        Label {
            id: virtualLabel
            anchors.centerIn: parent
            visible: root.isVirtualRow && model.eventType !== 8
            text: model.eventType === 7
                  ? Qt.locale().toString(model.timestamp, "dddd, d MMMM yyyy")
                  : (model.eventType === 9 ? qsTr("Beginning of conversation") : "")
            color: AppTheme.textMuted
            font.pixelSize: 11
        }
        RowLayout {
            id: unreadDivider
            objectName: "unreadDivider"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: AppTheme.spacingS
            // SDK receipt tracking (which the receipt chips require) also
            // revives the SDK's ReadMarker virtual row. While the reader
            // is pinned to the bottom, the own-receipt ack cycle
            // (~800ms ReadReceiptCoordinator debounce) would insert this
            // divider above every incoming message and remove it a moment
            // later — a 28px layout bounce under a live conversation. So
            // the divider renders ONLY when the reader is NOT following
            // the bottom (scrolled up = genuinely catching up). Standalone
            // hosts without a timeline ListView (fixtures, previews) keep
            // it visible.
            readonly property bool suppressedWhilePinned:
                root.timelineView
                && root.timelineView.stickToBottom === true
            visible: root.isVirtualRow && model.eventType === 8
                     && !suppressedWhilePinned
            // v0.6.5: reads unreadBadge, not accent — this divider is the
            // same "unread" semantic as every numeric unread badge in the
            // app, and it renders once per unread boundary in the visible
            // timeline (a passive, recurring status marker, not a
            // selection/focus/primary-action moment). unreadBadge is
            // periwinkle under Storm for exactly this reason; falls back to
            // accent for every legacy theme (pixel-identical).
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                color: AppTheme.unreadBadge
            }
            Label {
                objectName: "unreadDividerLabel"
                text: qsTr("New messages")
                color: AppTheme.unreadBadge
                font.pixelSize: 11
                font.weight: Font.DemiBold
                Accessible.name: text
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                color: AppTheme.unreadBadge
            }
        }
    }

    // Compact, discreet room-activity summary (Element-style) — never a
    // message-bubble-like card. Collapsed by default; the whole row (not
    // just the chevron) is the Expand/Collapse control.
    RoomActivityDelegate {
        id: stateActivity
        objectName: "stateActivityGroup"
        visible: root.isStateActivity && model.stateGroupLeader === true
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

    Rectangle {
        id: rowHighlight
        visible: !root.isVirtualRow && !root.isStateActivity
                 && (rowHover.hovered || root.actionsPinned
                     || app.pagination.highlightedEventId === (model.eventId || ""))
        x: -AppTheme.spacingXS
        y: layout.y
        width: root.width + AppTheme.spacingXS * 2
        height: layout.height
        color: app.pagination.highlightedEventId === (model.eventId || "")
               ? AppTheme.selected : AppTheme.hover
        // Design shell: message-row hover highlight is the soft theme tint
        // at an 8px radius — no border, no elevation.
        radius: AppTheme.radiusMd
        z: 0
    }

    ColumnLayout {
        id: layout
        visible: !root.isVirtualRow && !root.isStateActivity
        y: root.messageTopSpacing
        width: parent.width
        spacing: 2
        z: 1

        // One left-aligned sender timeline for every participant. Identity is
        // shown once at the start of a model-defined sender group; continuation
        // rows retain the same content indent without repeating the avatar.
        Item {
            id: bubbleRow
            objectName: "messagePresentationRow"
            Layout.fillWidth: true
            implicitHeight: Math.max(avatarSlot.implicitHeight,
                                     bubble.implicitHeight)

            HoverHandler { id: rowHover }
            TapHandler {
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
                    // Stable fallback colour per user id — resolving the
                    // display name later must not recolour the person.
                    colorKey: model.sender || ""
                    Accessible.name: qsTr("Avatar for %1").arg(
                                         model.senderDisplayName || model.sender)
                }

                // Continuations keep Discord's stable gutter without paying
                // for another avatar-height row. The timestamp is available
                // on hover in that gutter instead of consuming a metadata
                // line beneath every short message.
                Label {
                    objectName: "continuationTimestamp"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.rightMargin: 5
                    visible: !root.showsIdentity && rowHover.hovered
                             && !root.compactMode
                    text: Qt.formatDateTime(model.timestamp, "hh:mm")
                    horizontalAlignment: Text.AlignRight
                    color: AppTheme.textMuted
                    font.pixelSize: 9
                    Accessible.name: qsTr("Sent at %1").arg(text)
                }
            }

            // Transparent content column: ordinary messages are rows, not
            // incoming/outgoing speech bubbles.
            Rectangle {
                id: bubble
                objectName: "messageContentColumn"
                // Bubbles (DM only): content-sized, own messages right-
                // aligned in the accent-dark bubble, incoming in the chip
                // bubble. Modern/Compact keep the full-width row.
                x: root.bubbleMode && model.isOwn === true
                   ? Math.max(root.avatarGutterWidth, parent.width - width)
                   : root.avatarGutterWidth
                width: root.bubbleMode
                       ? Math.min(bubbleContent.implicitWidth + root.bubblePad * 2,
                                  Math.max(60, parent.width
                                               - root.avatarGutterWidth - 40))
                         // Modern/Compact: full row width up to a readable max,
                         // so long messages and media stay balanced on wide
                         // desktop windows (narrow windows shrink below it).
                       : Math.min(AppTheme.timelineContentMaxWidth,
                                  Math.max(1, parent.width - root.avatarGutterWidth))
                // v0.7 live-bug fix: `width` above is the OUTER capped
                // column — on a wide Modern/Compact pane it stays pinned at
                // AppTheme.timelineContentMaxWidth (760) no matter how short
                // the message actually is. Anything anchored to `width`
                // (the read-receipt chip stack) then floats however much of
                // that 760px cap the message didn't use — 732 logical px in
                // the 1600px-wide engine-test fixture for a one-line body,
                // the same class of gap the maintainer's live screenshot
                // showed.
                //
                // This is the ACTUAL rendered message CONTENT's right edge
                // (in this Rectangle's own local space) — deliberately NOT
                // the sender identity header (name + timestamp): a header-
                // inclusive anchor (bubbleContent.implicitWidth, an earlier
                // shape of this fix) measured 86.7 logical px of float on
                // the reporter's exact row ("SpongeMan" + "Fr fr") in the
                // same engine-test fixture, because the header renders
                // wider than the two-word body — receipts annotate the
                // MESSAGE, not who sent it.
                // nonHeaderContentWidth() below walks the bubbleContent
                // children that actually carry message content — reply
                // preview, media, poll, body text, the decrypting
                // skeleton, the undecryptable action row, the link-preview
                // card, and the meta/thread-summary row — and takes the
                // widest currently VISIBLE one, each clamped by that
                // child's own Layout.maximumWidth where it sets one.
                // bodyLabel is the clamp that matters most: its raw
                // unwrapped implicitWidth can be enormous (an unbroken URL
                // with no wrap point), bounded to min(720, width - 8) by
                // its own Layout.maximumWidth; the other candidates
                // self-clamp their own implicitWidth by construction, so
                // the generic clamp is a no-op for them — except the poll
                // card (fixed implicitWidth 420), where it does real work
                // on a column narrower than 420.
                // The reactions Flow is a SIBLING of readReceiptStrip (not
                // a bubbleContent child) and is deliberately excluded from
                // this anchor — chips trail the message content only;
                // folding reactions into the same anchor is a possible
                // future choice, not this one. Clamped to `width` itself
                // (the outer Math.min below) so wide content — a media
                // card, a long unbroken URL — never pushes the anchor past
                // the column's own cap.
                //
                // Hydration note: a media row or link-preview card starts
                // at implicitWidth 0 (or a small placeholder) before its
                // Loader resolves the real item, so the chip stack can
                // shift horizontally ONCE when the real card measures in.
                // Height is unaffected (readReceiptStrip's own
                // implicitHeight depends only on receiptRow, never on this
                // property) — accepted, the same one-time reflow every
                // hydrating card already causes for the row's own width.
                function nonHeaderContentWidth() {
                    // Hand-maintained: one entry per direct bubbleContent
                    // child EXCEPT identityHeader. Adding a new content
                    // type to bubbleContent? Add it here too, or the
                    // receipt chips will ignore its width.
                    var candidates = [replyBox, mediaBox, pollLoader,
                                       bodyLabel, decryptingSkeleton, utdRow,
                                       previewLoader, metaRow]
                    var widest = 0
                    for (var i = 0; i < candidates.length; ++i) {
                        var c = candidates[i]
                        if (!c || !c.visible) continue
                        var w = Math.min(c.implicitWidth, c.Layout.maximumWidth)
                        if (w > widest) widest = w
                    }
                    return widest
                }
                readonly property real renderedContentRight:
                    Math.min(width, bubbleContent.x + nonHeaderContentWidth())
                height: implicitHeight
                implicitHeight: bubbleContent.implicitHeight + root.bubblePad * 2
                // v0.6.0 checkpoint 11: mentions get a subtle tint — direct
                // mentions stronger than room-wide @room. v0.6.5: reads
                // mentionHighlight, not accent — under Storm, accent is
                // bolt, reserved for selection/focus/one primary action;
                // washing every mentioned row in it would both over-yellow
                // the timeline and (composited at low alpha over a dark
                // surface) read as a hueless brown rather than an
                // attention tint. mentionHighlight falls back to accent for
                // every legacy theme (pixel-identical) and resolves to the
                // Storm mention-rose under Storm, consistent with
                // mentionBadge's own tone.
                // v0.6.5 live-feedback: the wash at full 0.14/0.07 alpha
                // read as "too heavy/red" and made the reaction chips that
                // sit on this same background lose all contrast ("black
                // boxes over washed rows" — a chip's own translucent fill
                // compositing on top of an already-tinted row muddies
                // both). Cut the wash to a much quieter background tint and
                // moved the real signal to a left edge bar instead (below)
                // — a direct mention gets deliberate bolt yellow (the
                // personal "you were called out" case the design's yellow
                // discipline exists for); a room-wide @room stays neutral.
                color: root.bubbleMode
                       ? (model.isOwn === true ? AppTheme.ownBubble
                                               : AppTheme.otherBubble)
                       : model.mentionsMe === true
                       ? Qt.alpha(AppTheme.mentionHighlight, 0.05)
                       : model.mentionsRoom === true
                         ? Qt.alpha(AppTheme.mentionHighlight, 0.03)
                         : "transparent"
                radius: root.bubbleMode ? 16
                        : model.mentionsMe === true || model.mentionsRoom === true
                        ? AppTheme.radiusSm : 0
                topLeftRadius: root.bubbleMode
                               ? (model.isOwn === true ? 16 : 4) : radius
                topRightRadius: root.bubbleMode
                                ? (model.isOwn === true ? 4 : 16) : radius
                opacity: model.redacted ? 0.65 : 1.0

                // v0.6.5 live-feedback: the mention edge bar. Sits flush at
                // the bubble's own left edge, inside its rounded corner —
                // bubbleContent below gets a matching extra left inset so
                // the bar never overlaps the sender/body text.
                readonly property bool mentionBarVisible:
                    !root.bubbleMode
                    && (model.mentionsMe === true || model.mentionsRoom === true)
                Rectangle {
                    visible: bubble.mentionBarVisible
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 3
                    color: model.mentionsMe === true
                           ? AppTheme.bolt : AppTheme.borderStrong
                }

                // Click the message content to pin the action toolbar (click again
                // or press Escape to close). Does not consume media/link taps,
                // which have their own handlers on top.
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    onTapped: root.toggleActionsPin()
                }

                ColumnLayout {
                    id: bubbleContent
                    anchors.fill: parent
                    anchors.margins: root.bubblePad
                    anchors.leftMargin: root.bubblePad
                                        + (bubble.mentionBarVisible ? 8 : 0)
                    spacing: 2

                    RowLayout {
                        id: identityHeader
                        objectName: "senderIdentityHeader"
                        // Own DM bubbles need no self-identity line.
                        visible: root.showsIdentity
                                 && !(root.bubbleMode && model.isOwn === true)
                        spacing: 6
                        // Nested layouts default to fillWidth; the header
                        // line hugs its content so the timestamp sits 8px
                        // beside the sender name (design §3), not at the
                        // row's far edge.
                        Layout.fillWidth: false
                        Layout.maximumWidth: Math.max(1, bubble.width - 112)
                        Label {
                            id: nameLabel
                            objectName: "senderName"
                            text: model.senderDisplayName || model.sender
                            color: AppTheme.text
                            font.pixelSize: AppTheme.scaled(
                                root.compactMode || root.inThreadPanel
                                ? 13 : AppTheme.fontSizeM)
                            font.weight: Font.DemiBold
                            elide: Label.ElideRight
                            Layout.maximumWidth: 320
                            Accessible.name: qsTr("Sender: %1").arg(text)
                            // Full MXID on hover; always available even when
                            // the display name is shown.
                            ToolTip.text: model.sender
                            ToolTip.visible: nameHover.hovered
                            ToolTip.delay: 400
                            HoverHandler { id: nameHover }
                        }
                        // v0.5.9: compact disambiguator when the SDK reports
                        // two active members share this display name.
                        Label {
                            visible: model.senderNameAmbiguous === true
                                     && (model.senderDisplayName || "").length > 0
                            text: model.sender
                            color: AppTheme.textMuted
                            font.pixelSize: 10
                            elide: Label.ElideMiddle
                            Layout.maximumWidth: 180
                        }
                        Label {
                            objectName: "senderTimestamp"
                            text: Qt.formatDateTime(model.timestamp, "hh:mm")
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.scaled(10)
                            Accessible.name: qsTr("Sent at %1").arg(text)
                        }
                    }

                    // Reply preview — v0.6.5 live-feedback restyle: the old
                    // full-width flat-fill band read as an "ugly full-width
                    // band". Element-classic quote treatment instead:
                    // content-width (sized to what it actually holds, never
                    // stretched edge-to-edge), an inset fill that reads as
                    // a nested card, and a left accent bar rather than the
                    // dead `border.width: 0` this used to carry.
                    Rectangle {
                        id: replyBox
                        objectName: "replyNavigationTarget"
                        visible: model.replyToEventId && model.replyToEventId.length > 0
                                 && !model.redacted
                        readonly property int barWidth: 3
                        Layout.alignment: Qt.AlignLeft
                        Layout.maximumWidth: 320
                        implicitWidth: replyLayout.implicitWidth + barWidth + 16
                        implicitHeight: replyLayout.implicitHeight + 8
                        color: AppTheme.hover
                        radius: 6
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Jump to replied message")
                        TapHandler {
                            cursorShape: Qt.PointingHandCursor
                            onTapped: app.pagination.jumpToEvent(model.replyToEventId || "")
                        }
                        Rectangle {
                            width: replyBox.barWidth
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            color: AppTheme.borderStrong
                        }
                        ColumnLayout {
                            id: replyLayout
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            anchors.leftMargin: replyBox.barWidth + 8
                            anchors.rightMargin: 8
                            anchors.topMargin: 4
                            anchors.bottomMargin: 4
                            spacing: 0
                            Label {
                                text: model.replyToSender
                                      ? qsTr("↰ %1").arg(model.replyToSender)
                                      : qsTr("↰ Reply")
                                color: AppTheme.textSecondary
                                font.pixelSize: 11
                                elide: Label.ElideRight
                                Layout.fillWidth: true
                                maximumLineCount: 1
                            }
                            Label {
                                text: model.replyToPreview || qsTr("(original message not loaded)")
                                color: AppTheme.textMuted
                                font.pixelSize: 11
                                elide: Label.ElideRight
                                Layout.fillWidth: true
                                maximumLineCount: 1
                            }
                        }
                    }

                    // Media block (image, sticker, video, audio, or file).
                    // Every class reserves its own type-correct geometry
                    // before bytes arrive, so hydration never reflows rows.
                    Item {
                        id: mediaBox
                        visible: model.isImage || model.isFile
                                 || model.isVideo === true
                                 || model.isAudio === true
                                 || model.isSticker === true
                        Layout.alignment: Qt.AlignLeft
                        Layout.preferredWidth: Math.min(bubble.width,
                                                        implicitWidth)
                        Layout.maximumWidth: bubble.width
                        // v0.5.11: contribute a real implicit width so an
                        // image-only row grows to the media size instead of
                        // collapsing to the timestamp width (which made images
                        // render avatar-sized). Files stay compact.
                        implicitWidth: mediaLoader.item
                                       ? mediaLoader.item.implicitWidth : 0
                        implicitHeight: mediaLoader.item
                                        ? mediaLoader.item.implicitHeight : 0
                        Loader {
                            id: mediaLoader
                            anchors.left: parent.left
                            width: Math.min(bubble.width,
                                            item ? item.implicitWidth : 0)
                            sourceComponent: model.isImage ? imageComponent
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

                    // Poll block (MSC3381, v0.7). Renders the SDK-aggregated
                    // outcome only; votes route through app.composer and the
                    // updated poll returns as an in-place Set diff.
                    Loader {
                        id: pollLoader
                        active: model.isPoll === true
                        visible: active
                        Layout.alignment: Qt.AlignLeft
                        Layout.preferredWidth: item ? item.implicitWidth : 0
                        Layout.maximumWidth: bubble.width
                        sourceComponent: pollComponent
                    }

                    // Body text (hidden for media messages whose body is just
                    // the filename already shown in the media block, for poll
                    // rows, whose card renders the question, and for
                    // recoverable undecryptable rows, which show the
                    // decrypting skeleton instead).
                    TextEdit {
                        id: bodyLabel
                        objectName: "messageBody"
                        visible: text.length > 0 && !root.showsDecryptingSkeleton
                        // Media rows suppress a body that is just the
                        // filename echo; a genuinely different body renders
                        // once, styled as a caption below the card.
                        readonly property bool isMediaRow:
                            model.isImage
                            || model.isSticker === true
                            || model.isVideo === true
                            || model.isAudio === true
                            || model.isFile === true
                        readonly property bool isMediaCaption: {
                            if (!isMediaRow) return false
                            var body = (model.body || "").trim()
                            var name = (model.mediaFilename || "").trim()
                            return body.length > 0 && name.length > 0
                                   && body.toLowerCase() !== name.toLowerCase()
                        }
                        // Big-emoji: a body of exactly 1-3 user-perceived
                        // emoji sequences (and nothing but whitespace) renders
                        // large, Element-style. The count comes from the C++
                        // Unicode Emoji 17 catalogue — one ZWJ family, flag,
                        // keycap or tone variant counts once — so QML never
                        // scans the catalogue or guesses with a regex. Media
                        // captions, polls, redacted and undecryptable rows
                        // keep ordinary sizing.
                        readonly property int emojiOnlyCount:
                            (model.redacted || model.isPoll === true
                             || model.undecryptable === true || isMediaRow)
                            ? 0
                            : app.emojiCatalog.emojiOnlySequenceCount(
                                  model.body || "")
                        readonly property bool bigEmoji:
                            emojiOnlyCount >= 1 && emojiOnlyCount <= 3
                        text: {
                            if (model.redacted) return qsTr("[message deleted]")
                            // The poll card presents the question; the body
                            // is only the MSC1767 fallback for old clients.
                            if (model.isPoll === true) return ""
                            // Filename echoes never print twice. MSC2530
                            // senders (Element) put the caption in body and
                            // the real name in filename — compare loosely so
                            // case/whitespace variants of the same name are
                            // still recognized as echoes, not captions.
                            if (isMediaRow && !isMediaCaption) return ""
                            // Formatted messages (mentions, rich text) render
                            // their sanitized HTML directly — it is already a
                            // safe RichText subset from MessageHtml::sanitize,
                            // so it must NOT be re-escaped through linkifiedBody.
                            // Read the role ONCE — each read used to run the
                            // full sanitize in C++ (now memoized, but one
                            // read is still half the work of two).
                            var fb = model.formattedBody
                            if (fb && fb.length > 0)
                                return root.highlightSearchMatches(
                                            fb,
                                            root.searchHighlight,
                                            root.isCurrentSearchHit)
                            return root.highlightSearchMatches(
                                        app.linkPreviews.linkifiedBody(
                                            model.body || ""),
                                        root.searchHighlight,
                                        root.isCurrentSearchHit)
                        }
                        color: model.undecryptable === true
                               ? AppTheme.muted
                               : bodyLabel.isMediaCaption ? AppTheme.textSecondary
                               : root.bubbleMode && model.isOwn === true
                                 ? AppTheme.ownBubbleText : AppTheme.text
                        // TextEdit does not inherit the Controls font; the
                        // message body must follow the selected UI family.
                        font.family: AppTheme.uiFont
                        // Big-emoji: one uniform size for every 1-3-emoji
                        // message, 1.25x Element web's 48px bigEmoji per
                        // the maintainer's preference, slightly reduced in
                        // compact/thread presentation. All sizes go through
                        // AppTheme.scaled so the text-size setting applies
                        // to large emoji too.
                        font.pixelSize: {
                            if (bodyLabel.bigEmoji) {
                                return AppTheme.scaled(
                                    root.compactMode || root.inThreadPanel
                                    ? 48 : 60)
                            }
                            return AppTheme.scaled(
                                bodyLabel.isMediaCaption ? 12
                                : root.compactMode || root.inThreadPanel
                                ? 13 : AppTheme.fontSizeM)
                        }
                        font.italic: model.redacted || model.undecryptable === true
                        wrapMode: Text.Wrap
                        readOnly: true
                        // ColumnLayout incubates children before bubbleRow has
                        // received its final layout width. Measuring wrapped
                        // text against the old 1px clamp can turn a large body
                        // into a transient tens-of-thousands-pixel delegate,
                        // causing ListView to discard and recreate it forever.
                        // Use a normal column width during that brief startup
                        // phase, then follow the actual responsive width.
                        Layout.maximumWidth: bubble.width > 8
                                             ? Math.min(720, bubble.width - 8)
                                             : 560
                        textFormat: Text.RichText
                        selectByMouse: true
                        Accessible.name: model.body || ""
                        // Mentions carry an internal "mention:<user-id>" link
                        // (rewritten by the sanitizer); open the member
                        // profile. Everything else is a validated http(s) URL.
                        onLinkActivated: function(link) {
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
                            app.media.openWebUrl(link)
                        }

                        // v0.5.0-prep+12: hover tooltip for undecryptable rows
                        // so the user knows why the body is a placeholder.
                        // Text is deliberately reassuring, not alarming.
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

                    // v0.7: decrypting-text skeleton for recoverable rows.
                    // Two bounded line bars reserve stable text geometry; the
                    // in-place decryption update replaces them with the real
                    // body without moving the scroll anchor.
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
                                420, Math.max(120, bubble.width * 0.55))
                            Layout.preferredHeight: AppTheme.scaled(13)
                        }
                        Skeleton {
                            active: root.rowOnScreen
                                    && root.showsDecryptingSkeleton
                                    && !root.decryptStalled
                            Layout.preferredWidth: Math.min(
                                300, Math.max(80, bubble.width * 0.35))
                            Layout.preferredHeight: AppTheme.scaled(13)
                        }
                    }

                    // v0.6.0 checkpoint 8: unable-to-decrypt action row —
                    // safe reason category, automatic-recovery hint, a manual
                    // Retry (bounded/deduplicated in the backend), and a jump
                    // to Security settings. Never shows session ids,
                    // ciphertext, or raw event JSON.
                    RowLayout {
                        id: utdRow
                        visible: model.undecryptable === true
                        spacing: AppTheme.spacingS
                        Label {
                            text: {
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
                            font.pixelSize: 10
                            font.italic: true
                        }
                        // v0.6.5: these are inline text links (underlined,
                        // click-to-act), the exact role AppTheme.link exists
                        // for — not accent, which under Storm is bolt,
                        // reserved for selection/focus/the one primary
                        // action. link is periwinkle under Storm and falls
                        // back to accent for every legacy theme (pixel-
                        // identical), matching the real hyperlinks elsewhere
                        // in this same delegate (message-body links, mention
                        // links) that already read AppTheme.link.
                        Label {
                            text: qsTr("Retry decryption")
                            color: AppTheme.link
                            font.pixelSize: 10
                            font.underline: true
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.timelineModel.retryDecryption()
                            }
                        }
                        Label {
                            text: qsTr("Security settings")
                            color: AppTheme.link
                            font.pixelSize: 10
                            font.underline: true
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: app.showSettingsSection("security")
                            }
                        }
                    }

                    // v0.5.11: rich link-preview card. Backed by
                    // LinkPreviewController — Rust performs the protected
                    // outbound fetch; QML only renders whitelisted fields.
                    // Encrypted rooms default to click-to-load (privacy).
                    Loader {
                        id: previewLoader
                        Layout.alignment: Qt.AlignLeft
                        Layout.preferredWidth: Math.min(
                            bubble.width - 8,
                            item ? item.implicitWidth : 400)
                        Layout.maximumWidth: bubble.width
                        active: root.preview.state !== undefined
                                && root.preview.state !== "none"
                                && !model.redacted
                                && !model.isImage && !model.isFile
                        visible: active
                        sourceComponent: root.preview.state === "loaded"
                                         && root.preview.isDirectMedia === true
                                         && root.preview.gifOversized !== true
                                         ? directMediaPreviewComponent
                                         : linkPreviewComponent
                    }

                    RowLayout {
                        id: metaRow
                        Layout.fillWidth: true
                        spacing: AppTheme.spacingXS
                        Label {
                            id: metaLabel
                            visible: text.length > 0
                            text: {
                                var ts = Qt.formatDateTime(model.timestamp, "hh:mm")
                                // Status: 0=Sent, 1=Sending, 2=Failed
                                if (model.isOwn && model.status === 1) return ts + " • " + qsTr("sending…")
                                if (model.isOwn && model.status === 2) return ts + " • " + qsTr("failed")
                                if (model.edited) return qsTr("edited")
                                return ""
                            }
                            color: root.bubbleMode && model.isOwn === true
                                   ? AppTheme.onAccentMuted : AppTheme.textMuted
                            font.pixelSize: AppTheme.scaled(10)
                            Accessible.name: text
                        }
                        // v0.5.7: retry action for failed local echoes. The
                        // SDK send queue re-attempts the same queued item,
                        // so retrying never duplicates the message. v0.6.5:
                        // an inline text link — see the "Retry
                        // decryption"/"Security settings" comment above for
                        // why this reads AppTheme.link, not accent.
                        Label {
                            visible: model.isOwn && model.status === 2
                            text: qsTr("Retry")
                            color: AppTheme.link
                            font.pixelSize: 10
                            font.underline: true
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.timelineModel.retrySend(
                                               root.sourceModelRow(index))
                            }
                        }
                        // v0.6.1: Element-style thread summary card on the root
                        // event. Thread replies are hidden from the main
                        // timeline (SDK hide_threaded_events), so the root
                        // carries the entire thread's presence here. The card
                        // shows the message-bubble icon, latest sender, a safe
                        // preview, the authoritative reply count and an unread
                        // indicator; activating it opens the correct thread.
                        ThreadSummaryCard {
                            id: threadSummaryCard
                            visible: model.isThreadRoot === true
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

                            // v0.7 facepile. The root event id is read once
                            // here rather than in each binding below, so a
                            // late model change cannot leave the card
                            // showing one thread's faces under another's.
                            readonly property string rootId:
                                model.isThreadRoot === true
                                    ? root.eventIdForActions() : ""
                            // Guarded like Avatar.qml's canary: an
                            // unqualified `app` lookup performed during
                            // delegate creation can resolve undefined, and
                            // the binding would then throw and latch at ""
                            // with no self-heal path (30ee39b).
                            readonly property string roomId:
                                typeof app !== "undefined" ? app.currentRoomId : ""

                            // Pure read — issues no request, so this is safe
                            // as a binding. The fetch is explicit below.
                            function refreshParticipants() {
                                participants = (rootId !== "" && roomId !== "")
                                    ? app.threads.participants(roomId, rootId)
                                    : []
                            }
                            // Fetched once per (room, root) —
                            // requestParticipants is idempotent, so a card
                            // may call this on every appearance without
                            // generating traffic.
                            //
                            // NOTE this is NOT a viewport gate. The room
                            // timeline is a non-virtualized Repeater +
                            // Column (TimelinePane), so every LOADED row is
                            // instantiated and visible whether or not it is
                            // on screen. Opening a thread-heavy room, and
                            // each pagination batch that lands more roots,
                            // therefore issues one relations fetch per root.
                            // Bounding that fan-out is an accepted
                            // follow-up; the per-root dedupe already caps it
                            // at once per root per session.
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

            // Action toolbar. Visible while the shared row hover is active,
            // while it is pinned open by a click, or while one of its menus
            // is open — so it never vanishes as the pointer travels from the
            // message to the buttons. Subtle AppTheme surface/border framing.
            // Perf: the four-button hover action bar is created on first
            // hover (or pin / open menu) instead of eagerly for every
            // loaded row, then kept for the delegate's lifetime with
            // visibility gating — zero creation cost at room open.
            Loader {
                id: actionBarLoader
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: -3
                anchors.rightMargin: 2
                z: 3
                // Created on first need, then latched alive for the
                // delegate's lifetime; visibility gates afterwards. The
                // latch write is DEFERRED: onLoaded fires synchronously
                // inside the active binding's own evaluation, and a direct
                // write to one of its dependencies from there is a
                // detected binding loop.
                property bool latched: false
                active: latched || rowHover.hovered || root.actionsPinned
                        || root.moreMenuOpen
                onLoaded: Qt.callLater(function() { latched = true })
                visible: rowHover.hovered || root.actionsPinned
                         || root.moreMenuOpen
                sourceComponent: Rectangle {
                id: actionBar
                // v0.6.5 (SPEC 1a): container surface bg, 1px borderStrong,
                // radius radiusTile, 2px padding.
                radius: AppTheme.radiusTile
                color: AppTheme.surface
                border.color: AppTheme.borderStrong
                border.width: 1
                implicitWidth: actionRow.implicitWidth + AppTheme.spacing2 * 2
                implicitHeight: actionRow.implicitHeight + AppTheme.spacing2 * 2

                Row {
                    id: actionRow
                    anchors.centerIn: parent
                    spacing: 2
                    IconButton {
                        id: reactButton
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "add_reaction"
                        iconSize: 18
                        // Cheap local predicate — semantically identical
                        // to "messagePermalink() is non-empty" but without
                        // the per-row O(n) timeline scan the C++ call cost
                        // (three of these per row made room open O(n²)).
                        enabled: !model.redacted
                                 && (model.eventId || "").length > 0
                                 && model.eventId.indexOf("local:") !== 0
                        Accessible.name: qsTr("React to message")
                        ToolTip.text: qsTr("React")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: {
                            if (root.timelineView)
                                root.timelineView.pinnedActionsKey = root.actionKey
                            root.openReactionPickerFor(root.eventIdForActions(),
                                                       reactButton)
                        }
                    }
                    IconButton {
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "reply"
                        iconSize: 18
                        // Cheap local predicate — semantically identical
                        // to "messagePermalink() is non-empty" but without
                        // the per-row O(n) timeline scan the C++ call cost
                        // (three of these per row made room open O(n²)).
                        enabled: !model.redacted
                                 && (model.eventId || "").length > 0
                                 && model.eventId.indexOf("local:") !== 0
                        Accessible.name: qsTr("Reply to message")
                        ToolTip.text: qsTr("Reply")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: {
                            root.beginReply(root.eventIdForActions())
                        }
                    }
                    IconButton {
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "forum"
                        iconSize: 18
                        visible: !root.inThreadPanel
                        // Cheap local predicate — semantically identical
                        // to "messagePermalink() is non-empty" but without
                        // the per-row O(n) timeline scan the C++ call cost
                        // (three of these per row made room open O(n²)).
                        enabled: !model.redacted
                                 && (model.eventId || "").length > 0
                                 && model.eventId.indexOf("local:") !== 0
                        Accessible.name: qsTr("Reply in thread")
                        ToolTip.text: qsTr("Reply in thread")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: {
                            var eventId = root.eventIdForActions()
                            var details = root.timelineModel.messageDetails(eventId)
                            var rootId = (details.threadRootId || "").length > 0
                                         ? details.threadRootId : eventId
                            app.thread.openThread(app.currentRoomId, rootId)
                        }
                    }
                    IconButton {
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "more_vert"
                        iconSize: 18
                        // v0.6.5 (SPEC 1a): active button gets the accentSoft
                        // chip while its menu is open.
                        active: root.moreMenuOpen
                        Accessible.name: qsTr("More message actions")
                        ToolTip.text: qsTr("More")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: {
                            var menu = root.ensureContextMenu()
                            root.openContextMenu(
                                root.width - menu.implicitWidth,
                                actionBarLoader.y + actionBarLoader.height)
                        }
                    }
                }
            }
            }
        }

        // Reactions row
        Flow {
            id: reactionsFlow
            // Identity-guarded projection (same pattern as the read-receipt
            // strip below): ReactionsRole builds a fresh QVariantList on
            // every read, and a Repeater bound to it tore down and rebuilt
            // every chip on every unrelated Set diff (receipt moves alone
            // made that per-message-burst). Only a REAL change replaces the
            // model the Repeater sees.
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
            Layout.alignment: Qt.AlignLeft
            // Align with the message body across every layout mode (Modern 40,
            // compact 8, bubble 44) instead of a fixed 36; add a deliberate
            // gap so the chips sit clearly below a media card rather than
            // crowding it.
            Layout.leftMargin: root.avatarGutterWidth
            Layout.topMargin: AppTheme.spacingXS
            spacing: AppTheme.spacingXS
            Repeater {
                model: reactionsFlow.shownReactions
                Rectangle {
                    id: reactionChip
                    objectName: "reactionChip"
                    // Design §3: own reaction = accent-soft fill + accent
                    // border + accent-text; others = neutral chip. Pill radius,
                    // 9px side / 3px vertical padding, min height 22. Fill
                    // darkens slightly on hover — paint only, geometry never
                    // moves on hover/press/selected.
                    // v0.6.5 live-feedback: "add deliberate yellow" for byMe
                    // chips — the softer accentBorder fallback read as too
                    // faint to register as "you reacted here" at a glance
                    // (worse once it was sitting on the mention wash this
                    // round also toned down). Full-strength accent for the
                    // border only, a touch heavier — the fill stays the
                    // soft tint deliberately (a solid bolt fill on every
                    // own-reaction pill across a busy thread would be the
                    // over-yellow case the design's own discipline warns
                    // against; the crisp ring is enough to read as "mine").
                    readonly property color baseFill: modelData.byMe
                        ? AppTheme.accentSoft : AppTheme.reactionBackground
                    color: reactionHover.hovered ? Qt.darker(baseFill, 1.08) : baseFill
                    radius: AppTheme.radiusPill
                    border.color: modelData.byMe ? AppTheme.accent : AppTheme.border
                    border.width: modelData.byMe ? 1.5 : 1
                    implicitWidth: reactionRow.implicitWidth + 18
                    // reactionRow.implicitHeight is deterministic now: both
                    // labels below are pinned to a fixed 16px content height,
                    // so every chip in a row lands on the SAME height no
                    // matter which emoji it holds. Before this, a taller
                    // color-emoji glyph (many report bigger font metrics than
                    // the 12px count label at the identical pixel size) grew
                    // reactionRow's own implicitHeight, so per-chip height —
                    // and the count label's vertical position within it —
                    // varied chip to chip. Chip chrome stays unscaled by
                    // design (it's interface chrome, not message-body text).
                    implicitHeight: Math.max(22, reactionRow.implicitHeight + 6)
                    HoverHandler { id: reactionHover }
                    RowLayout {
                        id: reactionRow
                        anchors.centerIn: parent
                        spacing: 5
                        Label {
                            Layout.alignment: Qt.AlignVCenter
                            Layout.preferredHeight: 16
                            text: modelData.key
                            font.pixelSize: 12
                            verticalAlignment: Text.AlignVCenter
                        }
                        Label {
                            Layout.alignment: Qt.AlignVCenter
                            Layout.preferredHeight: 16
                            text: modelData.count
                            color: modelData.byMe ? AppTheme.selectedText : AppTheme.textSecondary
                            font.pixelSize: 12
                            font.weight: Font.Bold
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: app.composer.reactTo(root.eventIdForActions(), modelData.key)
                    }
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.byMe
                        ? qsTr("Reaction %1, %2, selected").arg(modelData.key).arg(modelData.count)
                        : qsTr("Reaction %1, %2").arg(modelData.key).arg(modelData.count)
                    // Accessible.role/name alone describe the control to
                    // assistive tech but do not make it ACTIVATABLE — an AT
                    // user invoking it (not clicking with a mouse) needs
                    // this mirror of the MouseArea's onClicked.
                    Accessible.onPressAction:
                        app.composer.reactTo(root.eventIdForActions(), modelData.key)
                }
            }
        }

        // Element-style read-receipt chips: a small stack of the OTHER
        // users whose read receipt points at this message (the model
        // excludes ONLY the local user and sorts newest first — a user's
        // marker renders even on their own message, which is how a DM
        // says "read up to here"). Bounded to 4 avatars + a "+N"
        // overflow chip fed by the uncapped readReceiptsTotal. Invisible
        // when the list is empty — a ColumnLayout skips invisible children
        // entirely, so an unread message keeps exactly its previous
        // geometry. Thread rows never carry receipt metadata (their SDK
        // timelines deliberately keep receipt tracking Disabled: the SDK's
        // receipt handling is not thread-aware, so enabling it would
        // attach the room's unthreaded receipts to thread rows), so the
        // strip stays collapsed in the thread panel.
        Item {
            id: readReceiptStrip
            objectName: "readReceiptStrip"
            readonly property var receipts: model.readReceipts || []
            readonly property int maxAvatars: 4
            // Uncapped other-reader count from the model; degrades to the
            // delivered list length when the role is absent (fixtures,
            // older payloads) — undefined fails the >= 0 test.
            readonly property int totalOthers:
                model.readReceiptsTotal >= 0 ? model.readReceiptsTotal
                                             : receipts.length
            readonly property int overflowCount:
                Math.max(0, totalOthers - shown.length)

            // Chip payload with an identity guard: `receipts` delivers a
            // fresh array object on every role read, and binding the
            // Repeater to a per-evaluation slice() would tear down and
            // recreate every chip delegate on each unrelated Set diff.
            // `shown` is reassigned ONLY when the projected content
            // actually changes (≤4 plain objects — stringify comparison
            // is cheap).
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

            // One line for the tooltip, the accessible name, and the QML
            // test. Reads at most the first two names; the rest is the
            // total count — never a walk over all N receipts.
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
            visible: !model.redacted && receipts.length > 0
            // Sender status must never drive horizontal flow in Modern
            // rows — that is the semantic rule of the one-left-aligned-
            // sender presentation contract (whose scan enforces it by
            // banning the layout right-align literal in this file). This
            // strip respects it: the placement is identical for every
            // message, own or not. The chip stack trails
            // bubble.renderedContentRight — the RENDERED message content
            // (never the sender identity header, never the outer capped
            // column) — see that property's own comment for the full
            // rationale and the 732-logical-px live-bug measurement it
            // fixes.
            Layout.fillWidth: true
            Layout.topMargin: 2
            implicitHeight: receiptRow.implicitHeight

            Row {
                id: receiptRow
                objectName: "readReceiptRow"
                x: Math.max(root.avatarGutterWidth,
                            bubble.x + bubble.renderedContentRight - width)
                // Facepile overlap; each avatar sits on an 18px surface
                // ring so overlapped edges stay legible on any theme.
                spacing: -4
                // The ring must paint what the row currently shows — a
                // bare AppTheme.background ring punches visible holes
                // into the hover/selection tint.
                readonly property color hoverTint:
                    rowHighlight.visible ? rowHighlight.color : "transparent"
                Repeater {
                    // Array model + modelData, the same shape the reaction
                    // chips use: each delegate carries its receipt
                    // directly, with no document-id dereference from
                    // delegate scope (which resolved undefined under the
                    // engine-test fixture). `shown` is the identity-
                    // guarded projection above, so delegates are only
                    // recreated when the visible chips actually change.
                    model: readReceiptStrip.shown
                    Rectangle {
                        id: chip
                        objectName: "readReceiptChip"
                        width: 18
                        height: 18
                        radius: 9
                        color: AppTheme.background
                        z: index
                        // Hover-tint overlay: composites the row's own
                        // highlight tint over the ring exactly like the
                        // row rectangle composites it over the pane
                        // background. Reads only the guarded visual
                        // parent chain — no document ids from delegate
                        // scope.
                        Rectangle {
                            anchors.fill: parent
                            radius: chip.radius
                            color: chip.parent ? chip.parent.hoverTint
                                               : "transparent"
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
                        font.pixelSize: 9
                        font.weight: Font.Bold
                    }
                }
            }

            HoverHandler { id: receiptHover }
            ToolTip.text: summary
            ToolTip.visible: receiptHover.hovered && summary.length > 0
            ToolTip.delay: 500
            // One accessible summary for the whole strip — individual
            // chips are deliberately not focus stops.
            Accessible.role: Accessible.StaticText
            Accessible.name: summary
        }
    }

    // v0.7: the reaction picker and sender-profile popover are SHARED
    // view-level surfaces (one instance per timeline, not one per row —
    // dozens of eager per-delegate popups measurably slowed room opening
    // and scrolling). The target event id is snapshotted at open, so a
    // recycled delegate can never redirect a reaction.
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

    // Perf: the ~25-item context menu (with its Shortcuts and quick-react
    // strip) and the modal details dialog used to be instantiated eagerly
    // by EVERY loaded row — the dominant per-row creation cost this
    // timeline pays for its no-virtualization design. Both now load on
    // first use and stay loaded for the delegate's lifetime. The Loaders
    // inherit the delegate context, so model.* and every root.* helper
    // resolve exactly as before.
    readonly property bool moreMenuOpen:
        moreMenuItem ? moreMenuItem.opened : false
    function ensureContextMenu() {
        if (!moreMenuItem)
            moreMenuItem = moreMenuComponent.createObject(root)
        return moreMenuItem
    }
    function openMessageDetails(details) {
        if (!details || !details.eventId)
            return
        if (!detailsDialogItem)
            detailsDialogItem = detailsDialogComponent.createObject(root)
        detailsDialogItem.details = details
        detailsDialogItem.open()
    }
    // Popups are not Items, so a Loader cannot host them — lazy-create
    // through a Component instead (created parented to the delegate, so
    // lifetime and context are identical to the old inline declaration).
    property var moreMenuItem: null
    Component {
        id: moreMenuComponent
        AppMenu {
            id: moreMenu
            objectName: "messageContextMenu"
            menuWidth: AppTheme.menuWidthMessage
            // Storm §3.1 mono context header — this row's own
            // sender and time (the menu instance lives in the
            // delegate, so the row data is authoritative).
            contextLabel: qsTr("Message · %1 · %2")
                .arg(model.senderDisplayName || model.sender || "")
                .arg(Qt.formatDateTime(model.timestamp, "hh:mm"))
            onClosed: root.menuEventId = ""
            // v0.6.5 (SPEC 1a): single-key accelerators while
            // the menu is open. Keys cannot attach to a Menu
            // (a Popup, not an Item), so these are Shortcuts
            // scoped by moreMenu.opened — inert whenever the
            // menu is closed. Each one calls exactly the same
            // action expression as the matching row's
            // onTriggered below, gated by the same enabled
            // condition, then closes the menu. The mock hints
            // ↑ on Edit (a composer-history convention this
            // app does not have, and a Key_Up shortcut would
            // steal menu arrow navigation) — the real binding
            // is E and the row's keycap says so.
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
                                root.menuEventId))
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
            // v0.6.5 (SPEC 1a): quick-react row — the 5 most
            // recently used emoji plus a trailing "more" cell
            // that opens the full shared picker. Replaces the
            // standalone "React" row (removed below); the
            // hover action bar's own React button is a
            // separate affordance and is unaffected.
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
                    app.composer.reactTo(root.menuEventId, emoji)
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
                    // v0.6.0: opens the thread panel; its
                    // composer sends real SDK m.thread
                    // replies.
                    app.thread.openThread(app.currentRoomId,
                                          rootId)
                }
            }
            // v0.6.0 checkpoint 5: from a thread reply,
            // locate the same event in the room timeline
            // (highlighted); the existing navigation shows a
            // safe message when the target is unavailable.
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
            // v0.7: unified media action — every media row
            // offers Save from the same menu (cards keep
            // their inline affordances too).
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
                enabled: visible && root.menuEventId !== ""
                onTriggered: {
                    if (root.timelineView
                        && root.timelineView.saveMedia)
                        root.timelineView.saveMedia(
                            model.mediaKey || "",
                            model.mediaFilename || "download")
                }
            }
            // v0.6.6 UX rework: GIF starring moved OFF this
            // menu entirely — it is now a Discord-style hover
            // star overlaid on the GIF media itself (see
            // imageComponent's starEligible/refreshStarredState
            // below), never a dropdown row.
            // SPEC 1a: the copy group and the people/editing
            // group are separate — third divider.
            AppMenuSeparator { }
            AppMenuItem {
                iconName: "person"
                text: qsTr("View profile")
                enabled: root.menuEventId !== ""
                         && root.timelineView
                         && !!root.timelineView.openSenderProfile
                onTriggered: {
                    var details = root.timelineModel.messageDetails(
                                      root.menuEventId)
                    if (!details.senderId)
                        return
                    root.timelineView.openSenderProfile({
                        userId: details.senderId,
                        displayName: details.senderName || "",
                        avatarUrl: model.senderAvatarMxc || ""
                    })
                }
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
                iconName: "edit_square"
                text: qsTr("Edit")
                accel: "E"
                enabled: root.timelineModel.canEditEvent(root.menuEventId)
                visible: enabled
                onTriggered: app.composer.beginEdit(
                    root.menuEventId,
                    root.timelineModel.visibleTextForEvent(root.menuEventId),
                    root.timelineModel.sanitizedHtmlForEvent(root.menuEventId))
            }
            // v0.7 polls: conservative rule — own running
            // polls only. The server and receiving clients
            // enforce the actual MSC3381 permission rules.
            AppMenuItem {
                objectName: "endPollMenuItem"
                iconName: "check_circle"
                text: qsTr("End poll")
                visible: model.isPoll === true
                         && model.canEndPoll === true
                enabled: visible && root.menuEventId !== ""
                onTriggered: app.composer.endPoll(
                    root.menuEventId,
                    root.inThreadPanel
                    ? (app.thread.rootEventId || "") : "")
            }
            AppMenuSeparator {
                visible: root.timelineModel.canRedactEvent(
                             root.menuEventId)
            }
            AppMenuItem {
                iconName: "delete"
                text: qsTr("Delete")
                danger: true
                enabled: root.timelineModel.canRedactEvent(root.menuEventId)
                visible: enabled
                onTriggered: app.composer.redact(root.menuEventId)
            }
        }
    }
    property var detailsDialogItem: null
    Component {
        id: detailsDialogComponent
        Dialog {
                id: messageDetailsDialog
                objectName: "messageDetailsDialog"
                parent: Overlay.overlay
                anchors.centerIn: parent
                modal: true
                title: qsTr("Message details")
                standardButtons: Dialog.Ok
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

    // Local echoes carry "local:*" ids. QML actions should still work because
    // the backend keys pendingSends by txnId, but redact of a local echo will
    // fail server-side. We still allow it — the failure surfaces via the
    // status bar error signal.
    function eventIdForActions() { return model.eventId }

    // ---- link preview card ----

    // A validated direct raster response is media, not article metadata.
    // It receives its own compact renderer so GIFs and images never inherit
    // the generic embed card's accent edge, host footer, or fixed card width.
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
            readonly property real maxWidth: Math.min(360, bubble.width - 8)
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
            readonly property string staticSource:
                (p.imageSource || "").length > 0
                ? app.mediaBridge.previewImageSource(p.imageSource || "",
                                                     p.imageMime || "") : ""

            implicitWidth: displayWidth
            implicitHeight: Math.max(1, displayWidth * aspectRatio)
            color: AppTheme.cardElevated
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
                    font.pixelSize: 9
                    font.weight: Font.Bold
                }
            }
            MouseArea {
                anchors.fill: parent
                enabled: (directMedia.p.url || "").length > 0
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: app.media.openWebUrl(directMedia.p.url)
            }
        }
    }

    Component {
        id: linkPreviewComponent
        Rectangle {
            id: card
            readonly property var p: root.preview
            readonly property string st: p.state || "none"
            readonly property string previewAnimation:
                p.isGif === true && app.settings.gifAutoplay !== 2
                ? app.mediaBridge.previewAnimatedSource(p.imageSource || "",
                                                        p.imageMime || "") : ""
            readonly property string previewStatic:
                (p.imageSource || "").length > 0
                ? app.mediaBridge.previewImageSource(p.imageSource || "",
                                                     p.imageMime || "") : ""
            implicitWidth: Math.min(400, bubble.width - 8)
            // Gate/loading/failed keep a monotonic reserved height so the
            // consent click and a failure never reflow the row under the
            // reader; only the loaded preview re-measures. The latch is
            // per-event: pooled delegate reuse for another row must not
            // inherit the previous event's minimum.
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
                   ? AppTheme.hover : AppTheme.surfaceElevated
            radius: AppTheme.radiusMd
            border.color: AppTheme.border
            border.width: 1

            HoverHandler { id: cardHover }

            // Whole card opens the URL (loaded state only).
            MouseArea {
                anchors.fill: parent
                enabled: card.st === "loaded" && (card.p.url || "").length > 0
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: app.media.openWebUrl(card.p.url)
            }

            ColumnLayout {
                id: cardCol
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: AppTheme.spacingS
                anchors.bottomMargin: AppTheme.spacingS
                anchors.leftMargin: AppTheme.spacingM
                anchors.rightMargin: AppTheme.spacingS
                spacing: 4

                // Consent / privacy gate (encrypted rooms, or auto-load off).
                ColumnLayout {
                    visible: card.st === "requires_action"
                    Layout.fillWidth: true
                    spacing: 4
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing6
                        Icon {
                            name: "link"
                            size: 15
                            color: AppTheme.textMuted
                        }
                        Label {
                            text: card.p.host || ""
                            color: AppTheme.link
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            elide: Label.ElideRight
                            Layout.fillWidth: true
                        }
                    }
                    Label {
                        text: root.roomEncrypted
                              ? qsTr("Loading this preview contacts the linked "
                                     + "website directly and may reveal your IP address.")
                              : qsTr("Previews load from the linked website.")
                        color: root.roomEncrypted ? AppTheme.warning
                                                  : AppTheme.textMuted
                        font.pixelSize: 10
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    AppButton {
                        objectName: "linkPreviewLoadButton"
                        text: qsTr("Show preview")
                        Layout.topMargin: 2
                        onClicked: app.linkPreviews.requestPreviewForEvent(
                                       root.previewRoomId, root.actionKey)
                    }
                }

                // Loading: separate title/description/domain region
                // skeletons at a stable card height, replaced in place as
                // the validated preview fields arrive — the card never
                // collapses to a spinner row and re-expands.
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
                            font.pixelSize: 11
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
                        color: AppTheme.cardElevated
                        radius: AppTheme.radiusSm
                        clip: true
                        Image {
                            id: thumb
                            anchors.fill: parent
                            visible: card.previewAnimation.length === 0
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            cache: true
                            source: card.previewStatic.length > 0
                                    ? card.previewStatic
                                    : (card.p.imageMxc || "").length > 0
                                    && app.mediaBridge.supported
                                    ? app.mediaBridge.mxcImageSource(card.p.imageMxc, 480)
                                    : ""
                            Connections {
                                target: app.mediaBridge
                                enabled: (card.p.imageMxc || "").length > 0
                                function onMediaCached(cacheKey) {
                                    if (cacheKey.endsWith(":" + card.p.imageMxc))
                                        thumb.source = app.mediaBridge.mxcImageSource(
                                            card.p.imageMxc, 480)
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
                                font.pixelSize: 9
                                font.weight: Font.Bold
                            }
                        }
                    }

                    Label {
                        visible: card.p.isDirectMedia !== true
                                 && (card.p.siteName || "").length > 0
                        text: card.p.siteName || ""
                        color: AppTheme.textMuted
                        font.pixelSize: 10
                        elide: Label.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        visible: card.p.isDirectMedia !== true
                                 && (card.p.title || "").length > 0
                        text: card.p.title || ""
                        color: AppTheme.text
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
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
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                        maximumLineCount: 3
                        elide: Label.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        text: card.p.host || ""
                        color: AppTheme.link
                        font.pixelSize: 10
                        elide: Label.ElideRight
                        Layout.fillWidth: true
                    }
                }
            }
        }
    }

    // ---- media sub-components ----

    Component {
        id: imageComponent
        Item {
            id: imageBox

            // v0.5.11: responsive timeline-image sizing. The display box is
            // derived from the intrinsic media dimensions and a responsive
            // bound (never avatar-sized, never overflowing the column, never
            // upscaling a tiny image beyond its natural size). This box's
            // implicitWidth/Height flow up into the row so the row grows
            // to the picture instead of collapsing to the timestamp width.
            readonly property real maxW: Math.min(360, bubble.width)
            readonly property real maxH: 320
            readonly property real natW: model.mediaWidth > 0 ? model.mediaWidth : 0
            readonly property real natH: model.mediaHeight > 0 ? model.mediaHeight : 0
            readonly property real ratio: (natW > 0 && natH > 0)
                                          ? (natH / natW) : 0.66
            // Never upscale media with known intrinsic dimensions. Unknown
            // dimensions use the responsive bound until the image metadata is
            // available from a later timeline update.
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
            // 2026-08 media round: the hover star's eligibility gate generalized from
            // "GIF only" to any of the four raster formats Lightning can
            // validate/store byte-for-byte (see gif::validateRasterBytes) —
            // isGif itself is UNCHANGED and still drives GIF-only animation
            // playback above/below.
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
            // v0.6.1: autoplay policy — 0 Always, 1 OnHover, 2 Never.
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

            // v0.5.9: prefer the media bridge (works for encrypted rooms —
            // the SDK decrypts inside Rust); HTTP-backend URLs remain the
            // fallback. An empty bridgeSource means "fetch in flight".
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
                // Fetch the animated bytes whenever autoplay is not Never, so
                // OnHover playback starts instantly on hover.
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
            // v0.6.6 UX rework: Discord-style hover star — replaced the old
            // "Star GIF"/"Unstar GIF" context-menu rows entirely. Eligible
            // under the exact same mimetype/source/bridge gate those rows
            // used, and toggles through the exact same app.starChatGif /
            // GifStarredStore path.
            //
            // v0.6.6 review (L1), DELIBERATE NARROWING: the removed menu
            // row's gate was mimetype-only, so it also matched an animated
            // GIF STICKER (m.sticker, image/gif) — this hover star only
            // exists here, inside imageComponent (model.isImage rows), never
            // in stickerComponent, so a GIF sticker is no longer starrable.
            // Extending it there is not a small lift: stickerComponent has
            // none of isGif/starEligible/starred/
            // refreshStarredState/the starredStore Connections, and (unlike
            // imageComponent) not even an onMediaIdentityChanged reuse hook
            // for its own media identity — duplicating that whole block
            // would be ~80-100 lines of new, independently-maintained state
            // for a rare case (chat GIFs are sent as m.image via
            // GifSendController, never as a sticker; a sticker-starrable row
            // could previously only come from an m.sticker some other
            // client/bot sent). Accepted as a real, honest gap rather than
            // silently claimed as covered — see
            // GifHoverStarContractTest::hoverStarIsScopedToImageRowsNotStickers.
            // `starred` is a plain tracked property (not a live QML binding
            // on the Q_INVOKABLE isChatGifStarred() call, which carries no
            // NOTIFY QML could bind to) — refreshed on reuse/identity
            // change, on the store's own starFinished/unstarFinished
            // signals (exactly like bridgeSource/animatedSource above
            // already refresh from the same signals), and — v0.6.6 fix —
            // whenever MediaBridge caches this row's full bytes
            // (onAnimatedMediaReady/onMediaCached below), since the durable
            // content-hash answer app.isChatGifStarred() gives can only
            // become true once those bytes exist. See GifStarredStore's
            // class comment ("DURABLE STARRED-STATE DESIGN") for what
            // app.isChatGifStarred() actually checks — it is NOT
            // session-scoped, despite MessageDelegate only ever seeing one
            // session.
            //
            // v0.6.6 fix: excludes a row still pending (local echo — see
            // `pendingMedia`). Starring while pending would fetch/hash bytes
            // keyed off the echo's temporary id, then never be found again
            // once the echo is replaced by the real event (its mediaKey
            // changes) — the hover star simply does not exist yet for a row
            // that has not finished sending.
            // 2026-08 media round: generalized from GIF-only to any saveable raster
            // format — see isRasterImage above. Still excludes a sticker
            // (this gate only exists inside imageComponent, never
            // stickerComponent — see the DELIBERATE NARROWING note above)
            // and a pending local echo.
            readonly property bool starEligible:
                imageBox.isRasterImage && model.mediaSourceAvailable === true
                && app.mediaBridge.supported && !imageBox.pendingMedia
            property bool starred: false
            function refreshStarredState() {
                imageBox.starred = imageBox.starEligible
                    && app.isChatGifStarred(model.mediaKey || "")
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
            // A row created before the SDK confirmed mediaSourceAvailable/
            // bridge support keeps starEligible (and thus starred)
            // false until that flips — re-check the instant it does, rather
            // than staying stuck showing an outline star for a row that just
            // became eligible.
            onStarEligibleChanged: refreshStarredState()
            Connections {
                target: app.mediaBridge
                enabled: imageBox.usesBridge
                function onMediaCached(cacheKey) {
                    if (cacheKey === imageBox.bridgeCacheKey)
                        imageBox.bridgeSource = app.mediaBridge.cachedSource(cacheKey)
                    // v0.6.6 fix: the durable starred check only becomes
                    // answerable once the FULL payload (never the "thumb:"
                    // class) is cached — re-check exactly then, whether or
                    // not it was also this row's bridgeCacheKey.
                    //
                    // review H1c: mediaCached and animatedMediaReady BOTH
                    // fire for the same "full:" key whenever a GIF's bytes
                    // land while the animated preview path is also active
                    // (MediaBridge emits mediaCached unconditionally, and
                    // animatedMediaReady whenever the key was in
                    // m_animatedWanted) — Qt.callLater coalesces the two
                    // calls into at most one deferred refreshStarredState()
                    // per event-loop turn, rather than running the check
                    // twice back to back.
                    // 2026-08 media round: was `imageBox.isGif` — generalized to
                    // starEligible so a saved-eligible PNG/JPEG/WebP row
                    // also re-checks once its full bytes land (a static
                    // image never fetches an "animatedSource", so this is
                    // its only trigger to learn the durable answer besides
                    // the store's own signals below).
                    if (imageBox.starEligible && cacheKey === "full:" + (model.mediaKey || ""))
                        Qt.callLater(imageBox.refreshStarredState)
                }
                function onAnimatedMediaReady(cacheKey) {
                    if (cacheKey === "full:" + (model.mediaKey || "")) {
                        imageBox.animatedSource = app.mediaBridge.animatedSource(model.mediaKey)
                        // v0.6.6 fix: full bytes just landed in the cache —
                        // the durable starred answer may now be knowable.
                        // See the H1c comment above onMediaCached for why
                        // this is deferred/coalesced rather than direct.
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
                // v0.6.6 review (H1): starFinished/unstarFinished are NOT the
                // only ways the session-starred answer can change under a
                // row that never gets torn down. Settings -> Clear All wipes
                // the whole store and emits ONLY countChanged (never
                // unstarFinished per hash); an account switch repoints the
                // same long-lived store at a different directory via
                // openFor()/close() -> GifStoredModel::reopen(), which also
                // emits only countChanged. Without this handler a starred
                // tile that Clear All just deleted from disk kept rendering
                // filled/"Remove from saved GIFs", and activating it would
                // have called app.starChatGif() and RE-WRITTEN the bytes the
                // user just explicitly deleted — a real data-at-rest leak,
                // not just a stale label. countChanged also fires on every
                // ordinary star/unstar (GifStoredModel::insertFront/
                // removeEntry), so this is a safe, idempotent superset of
                // the two handlers above, not a replacement for them.
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
            // Round the corners of a media-bridge image via the provider's baked
            // mask (no per-frame effect). Only the in-process provider path can
            // carry the shape suffix; a plain http fallback URL is left as-is.
            readonly property string roundedSource:
                resolvedSource.indexOf("image://lightning-media/") === 0
                ? resolvedSource + "|shape:round:35"
                : resolvedSource

            // v0.7: image skeleton keeps the exact reserved rectangle while
            // bytes download/decrypt, and is replaced in place — no zero-size
            // flash, no reflow when the bitmap arrives. Shimmer runs only
            // while the row is on screen; a fetch failure keeps the static
            // surface (geometry never collapses).
            Skeleton {
                objectName: "imageSkeleton"
                anchors.fill: parent
                radius: AppTheme.radiusSm
                visible: img.status !== Image.Ready
                         && animatedImg.status !== AnimatedImage.Ready
                active: root.rowOnScreen && !imageBox.bridgeFailed
                        && img.status !== Image.Error
            }
            // GIFs announce themselves on the placeholder too, so the
            // reserved box reads as "an animation is coming".
            Rectangle {
                visible: imageBox.isGif
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
                    font.pixelSize: 9
                    font.weight: Font.Bold
                }
            }

            // Static path (default; also the frame for non-animated GIFs).
            Image {
                id: img
                anchors.fill: parent
                visible: !imageBox.animateGif
                fillMode: Image.PreserveAspectFit
                source: imageBox.animateGif ? "" : imageBox.roundedSource
                sourceSize.width: 640
                asynchronous: true
                cache: true
            }

            // Animated path — only when the message is a confirmed GIF and the
            // "Animate GIF previews" setting is on. Paused while off-screen to
            // avoid burning CPU on scrolled-away rows.
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

            MouseArea {
                anchors.fill: parent
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

            // v0.6.6 review (L5): only a GIF ever needs the hover star — a
            // Loader keeps every plain (non-GIF) image row from paying for a
            // HoverHandler + Item + Icon + two Rectangles it will never use.
            // `active` re-evaluates starEligible live, so a row that only
            // later confirms itself as a GIF (see onStarEligibleChanged
            // above) still gets one created.
            Loader {
                id: gifStarLoader
                anchors.fill: parent
                active: imageBox.starEligible
                sourceComponent: Component {
                    Item {
                        id: starLayer
                        anchors.fill: parent

                        // Hover detection uses a HoverHandler — a passive
                        // pointer handler — never a MouseArea, so it can
                        // never grab/steal the wheel or drag gestures needed
                        // to scroll the timeline over a GIF (CLAUDE.md GIF
                        // integration rules; the maintainer's UX request for
                        // a Discord-style hover star).
                        HoverHandler {
                            id: gifStarHover
                        }

                        // The star itself: revealed while the pointer is
                        // over the media OR the star (so moving from the
                        // media onto the button never hides it), or while it
                        // holds keyboard focus. It stays present (visible:
                        // true, only its opacity toggles) so Tab can reach
                        // it even before the pointer hovers anything —
                        // mirrors QuickReactionStrip's cell (HoverHandler +
                        // TapHandler + explicit Keys handlers +
                        // Accessible.onPressAction), never an AbstractButton
                        // — Qt Quick Controls' built-in Space/Return
                        // handling on AbstractButton would risk firing a
                        // second time on top of an explicit Keys handler for
                        // the exact same key.
                        //
                        // v0.6.6 review (M2): bottom-right, not top-right —
                        // the message action bar (React/Reply/Thread/More)
                        // is anchored top-right of the WHOLE row at z:3, and
                        // a continuation row with no reply preview puts
                        // mediaBox's own top flush with the row's top, so a
                        // wide GIF filling a narrow column (the 340px thread
                        // panel, or any narrow window) put a top-right star
                        // directly under the action bar's higher-z buttons —
                        // unreachable by mouse and invisible under them.
                        // The action bar is anchored to the row's TOP only
                        // (never bottom) regardless of width, so bottom-right
                        // is clear in every layout; bottom-left already
                        // belongs to the "GIF" badge, so bottom-right stays
                        // free.
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
                            // v0.6.7 (maintainer request): the star appears on
                            // hover/focus ONLY — never parked on the media at
                            // rest. v0.6.6 had kept a saved GIF's star
                            // permanently visible so its state could be read
                            // without hovering; in practice that left a
                            // yellow badge sitting on every saved GIF in the
                            // timeline. The state is still legible the moment
                            // the pointer arrives, and the picker's Saved tab
                            // is the authoritative list. GifPicker.qml's tile
                            // star follows the same rule, so the one star
                            // behaves identically in both places.
                            opacity: revealed ? 1 : 0
                            Behavior on opacity { NumberAnimation { duration: 100 } }

                            Accessible.role: Accessible.Button
                            // v0.6.7: one verb everywhere. This button and the
                            // picker's tile star now do the same thing, say
                            // the same thing, and land in the same place — the
                            // picker's Saved tab. See GifPicker.qml's header.
                            // 2026-08 media round: format-neutral wording — this button now
                            // saves GIF/PNG/JPEG/WebP alike, so it no longer
                            // names "GIF" specifically.
                            Accessible.name: imageBox.starred
                                ? qsTr("Remove from saved") : qsTr("Save image")
                            Accessible.onPressAction: gifStarButton.activate()

                            function activate() {
                                var key = model.mediaKey || ""
                                if (!key)
                                    return
                                // v0.6.6 fix: app.isChatGifStarred/
                                // unstarChatGif give the durable,
                                // content-addressed answer (not just this
                                // session's) — see GifStarredStore's class
                                // comment.
                                if (app.isChatGifStarred(key))
                                    app.unstarChatGif(key)
                                else
                                    app.starChatGif(key)
                            }

                            // v0.6.7: saved state is a FILL, matching the
                            // picker tile exactly — the bundled Material
                            // Symbols subset is a static FILL=0 instance, so
                            // there is no filled star glyph and colour alone
                            // had to carry the whole state.
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
                            // v0.6.6 review (L3): gated on `revealed`, not
                            // just `enabled: true` — the Item stays
                            // visible/hit-testable at opacity 0 so Tab can
                            // reach it, which on a TOUCH input (no synthetic
                            // hover-before-tap the way a mouse gets one)
                            // would otherwise let a tap on the invisible
                            // corner silently star/unstar without the user
                            // ever seeing the button.
                            //
                            // v0.6.7: the `|| imageBox.starred` relaxation
                            // added in v0.6.6 is REMOVED along with the
                            // at-rest visibility that justified it. A saved
                            // GIF's star is now invisible at rest again, so
                            // allowing a tap on it would restore exactly the
                            // hazard above — an unseen corner that
                            // saves/unsaves on touch. The gate must track the
                            // opacity, not the saved state.
                            TapHandler {
                                enabled: gifStarButton.revealed
                                onTapped: gifStarButton.activate()
                            }
                            // v0.6.6 review (L2): ignore key-repeat — held
                            // Space/Return would otherwise call activate() at
                            // OS repeat rate, each one a real file write/
                            // QFile::remove plus a banner re-trigger.
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
                font.pixelSize: 11
                visible: img.status === Image.Error || imageBox.bridgeFailed
            }
        }
    }

    // ---- sticker ----
    // Stickers keep their transparency intent: the reserved aspect box has
    // no opaque backing card once the bitmap is ready — transparent pixels
    // reveal the timeline surface. The skeleton only exists while loading.
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
            property string bridgeSource: ""
            property bool bridgeFailed: false
            function refreshBridgeSource() {
                if (!usesBridge || !model.mediaKey) return
                if (bridgeFailed)
                    app.mediaBridge.retry(bridgeCacheKey)
                bridgeFailed = false
                bridgeSource = app.mediaBridge.mediaSource(
                    model.mediaKey,
                    model.mediaThumbAvailable ? "thumb" : "full")
            }
            Component.onCompleted: refreshBridgeSource()
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
            }
            readonly property string resolvedSource:
                usesBridge ? bridgeSource
                           : (model.mediaThumbUrl
                              && model.mediaThumbUrl.toString().length > 0
                              ? model.mediaThumbUrl
                              : (model.mediaUrl || ""))

            Skeleton {
                anchors.fill: parent
                visible: stickerImg.status !== Image.Ready
                active: root.rowOnScreen && !stickerBox.bridgeFailed
                        && stickerImg.status !== Image.Error
            }
            Image {
                id: stickerImg
                anchors.fill: parent
                fillMode: Image.PreserveAspectFit
                source: stickerBox.resolvedSource
                sourceSize.width: 360
                asynchronous: true
                cache: true
            }
            HoverHandler { id: stickerHover }
            ToolTip.text: model.body || ""
            ToolTip.visible: stickerHover.hovered && (model.body || "").length > 0
            ToolTip.delay: 400
            TapHandler {
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
                font.pixelSize: 11
                visible: stickerImg.status === Image.Error
                         || stickerBox.bridgeFailed
            }
        }
    }

    // ---- video ----
    // Reserves the thumbnail geometry from Matrix info metadata (bounded
    // 16:9 fallback), shows the media skeleton plus a play badge and the
    // duration, and swaps the real thumbnail in place. Playback stays an
    // explicit Save/External action — Lightning has no embedded player yet.
    Component {
        id: videoComponent
        Item {
            id: videoBox
            objectName: "videoMedia"
            // 60-75% of the content column, bounded: landscape videos get a
            // useful width, portrait videos a useful height, and the card
            // never drops below the width the control bar needs (the old
            // flat 360/320 caps rendered portrait video ~180px wide and
            // clipped seek/speed/expand clean off).
            readonly property real maxW: {
                var cap = Math.min(560, Math.max(280, bubble.width * 0.72))
                return Math.max(1, Math.min(cap, bubble.width))
            }
            // Dimensions learned from a previous poster extraction (persisted
            // per account) size the card correctly from the FIRST render —
            // without them a metadata-less video started 16:9 and visibly
            // resized when its poster landed (maintainer screenshots).
            readonly property size learnedDims:
                model.mediaWidth > 0 ? Qt.size(0, 0)
                : app.settings.knownVideoDimensions(model.mediaKey || "")
            readonly property real natW: model.mediaWidth > 0
                ? model.mediaWidth
                : (learnedDims.width > 0 ? learnedDims.width : 0)
            readonly property real natH: model.mediaHeight > 0
                ? model.mediaHeight
                : (learnedDims.height > 0 ? learnedDims.height : 0)
            // Metadata-less events (every Lightning-sent video before the
            // send-metadata fix) used to default the card to 16:9, which
            // letterboxed square/portrait videos massively during playback
            // (maintainer screenshots, 2026-08-12). The extracted poster
            // carries the video's REAL shape — use it whenever the event
            // itself declares none; 16:9 remains only the last-resort
            // guess before any poster exists.
            readonly property real posterRatio:
                thumbImg.status === Image.Ready && thumbImg.implicitWidth > 0
                ? thumbImg.implicitHeight / thumbImg.implicitWidth : 0
            readonly property real ratio: (natW > 0 && natH > 0)
                                          ? (natH / natW)
                                          : (posterRatio > 0 ? posterRatio
                                                             : 0.5625)
            readonly property real maxH: ratio > 1 ? 440 : 400
            readonly property real minControlW: Math.min(260, bubble.width)
            readonly property real dispW: {
                var w = natW > 0 ? Math.min(natW, maxW) : maxW
                if (w * ratio > maxH) w = maxH / ratio
                return Math.max(minControlW, Math.min(w, maxW))
            }
            implicitWidth: dispW
            // Height caps even after the minimum-width floor (a portrait
            // video letterboxes inside rather than towering).
            implicitHeight: Math.max(1, Math.min(maxH, dispW * ratio))

            // v0.7: inline playback. Explicit user intent swaps the cover
            // for the player card in place — identical geometry, so
            // starting playback never reflows the timeline. Delegate reuse
            // for another event always drops back to the cover.
            property bool playerActive: false
            readonly property string mediaIdentity:
                root.actionKey + "\u001f" + (model.mediaKey || "")
            readonly property bool playbackAvailable:
                model.mediaSourceAvailable === true && app.mediaBridge.supported

            // The poster path serves BOTH cases now: a Matrix thumbnail is
            // fetched as before, and a video without one gets a locally
            // extracted first-frame poster (MediaBridge.videoPosterSource),
            // bounded by the speculative prefetch cap.
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
                // Speculative payload prefetch is governed by the SAME
                // user preference as GIF autoplay (never = no passive
                // downloads); a declared size of 0 makes MediaBridge
                // decline while the poster path still serves an
                // already-materialized file.
                // Declared size first; else the size learned from a
                // previous fetch (persisted per account), so the
                // pre-metadata-fix backlog prefetches — and posters — on
                // every session after a single play.
                var prefetchSize = app.settings.gifAutoplay !== 2
                                   ? (model.mediaSize
                                      || app.settings.knownMediaSizeBytes(
                                             model.mediaKey || "")
                                      || 0)
                                   : 0
                if (model.mediaThumbAvailable === true) {
                    bridgeSource = app.mediaBridge.mediaSource(model.mediaKey,
                                                               "thumb")
                } else if (root.rowOnScreen) {
                    bridgeSource = app.mediaBridge.videoPosterSource(
                        model.mediaKey, prefetchSize)
                } else {
                    // Off-screen rows must not trigger poster/prefetch work;
                    // the onScreen observer below re-runs this on reveal.
                    bridgeSource = ""
                }
                // Bounded speculative payload prefetch so pressing Play is
                // (usually) instant instead of a multi-second download.
                // MediaBridge enforces the size cap and deduplication.
                if (root.rowOnScreen && playbackAvailable && prefetchSize > 0)
                    app.mediaBridge.prefetchPlayable(model.mediaKey,
                                                     prefetchSize)
            }
            readonly property bool coverOnScreen: root.rowOnScreen
            onCoverOnScreenChanged: {
                if (coverOnScreen && bridgeSource.length === 0
                    && !bridgeFailed)
                    videoSourceRefresh.restart()
            }
            function resetForMedia() {
                // A pooled Loader keeps this videoBox instance alive while
                // model roles rebind. Clear the old thumbnail synchronously
                // so another video's duration can never appear over stale
                // pixels, then fetch after the role-update turn settles.
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
                // A video with NO declared size (every Lightning-sent
                // video before the send-metadata fix) is never prefetched,
                // so its poster can only come from the file the user's own
                // Play just materialized. Re-run the poster path exactly
                // when that file lands — videoPosterSource then extracts
                // from the materialized file with no network at all.
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

            // No Matrix thumbnail (or its fetch failed): a stable styled
            // placeholder instead of an empty transparent box — surface
            // tone, type icon, filename. The play affordance and duration
            // chip overlay it exactly as they would a real poster, so the
            // card never looks broken while (or because) no poster exists.
            // Placeholder shows when no bridge is available, the fetch
            // failed, or a no-Matrix-thumbnail video has no poster (yet, or
            // ever — an over-cap video is not prefetched for one).
            readonly property bool showPlaceholder:
                !usesBridge || bridgeFailed
                || (model.mediaThumbAvailable !== true
                    && bridgeSource.length === 0)
            Rectangle {
                objectName: "videoNoThumbPlaceholder"
                anchors.fill: parent
                radius: AppTheme.radiusSm
                color: AppTheme.surfaceElevated
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
                        color: AppTheme.textMuted
                        font.pixelSize: 11
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
                // Rounded via the provider's baked mask when served from the
                // in-process media bridge (see the image path above).
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
                        font.pixelSize: 9
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
                enabled: !videoBox.playerActive
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
    // v0.7: real inline playback. The compact card keeps its stable
    // geometry; pressing Play fetches through MediaBridge's validated
    // playable materialization and plays in-process. Voice messages render
    // their real MSC3245 waveform when present. Non-bridge backends keep
    // the external-open path.
    Component {
        id: audioComponent
        AudioPlayerCard {
            objectName: "audioMedia"
            bubble: bubble
            mediaKey: model.mediaKey || ""
            ownerKey: root.actionKey + "\u001f" + (model.mediaKey || "")
            filename: model.mediaFilename || model.body || ""
            mimetype: model.mediaMimetype || ""
            fileSize: model.mediaSize || 0
            durationMs: model.mediaDurationMs || 0
            isVoice: model.mediaIsVoice === true
            waveform: model.mediaWaveform || []
            rowOnScreen: root.rowOnScreen
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
            implicitWidth: Math.min(340, bubble.width)
            implicitHeight: fileRow.implicitHeight + 16
            color: AppTheme.surfaceElevated
            radius: AppTheme.radiusMd
            border.color: AppTheme.border
            border.width: 1

            // Save state, keyed by this card's media so pooled delegate
            // reuse and unrelated downloads can never cross-talk. The view
            // exposes the keys only in the main timeline; thread-panel
            // delegates fall back to stateless presentation.
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
                // "application/x-zip-compressed" → "ZIP", "application/pdf"
                // → "PDF": the subtype tail reads better than a raw MIME.
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
                        color: AppTheme.text
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        elide: Label.ElideMiddle
                        Layout.fillWidth: true
                    }
                    Label {
                        text: {
                            var kb = model.mediaSize / 1024
                            var size = kb < 1024 ? kb.toFixed(1) + " KB"
                                                 : (kb / 1024).toFixed(1) + " MB"
                            var kind = fileCard.fileTypeLabel(model.mediaMimetype)
                            var status = fileCard.saving ? qsTr("Saving…")
                                       : fileCard.savedFlash
                                         ? (fileCard.savedOk ? qsTr("Saved")
                                                             : qsTr("Save failed"))
                                         : ""
                            var base = kind ? size + " • " + kind : size
                            return status ? base + " • " + status : base
                        }
                        color: fileCard.savedFlash && !fileCard.savedOk
                               ? AppTheme.danger
                               : fileCard.savedFlash ? AppTheme.success
                               : AppTheme.textMuted
                        font.pixelSize: 10
                        elide: Label.ElideRight
                        Layout.fillWidth: true
                    }
                }

                // Download / save state action. MediaBridge saves are
                // atomic (no progress or cancel API) — the in-flight state
                // is honest-indeterminate, never a fake percentage.
                BusyIndicator {
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
                // HTTP backend keeps its external-open path (plain media).
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

    // MSC3381 poll card (v0.7). Entirely stateless: selection, counts and
    // the ended flag all derive from model roles, so pooled-delegate reuse
    // can never show another row's votes. Undisclosed running polls arrive
    // with zeroed counts from the bridge — hidden tallies never reach QML.
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
                    // Re-clicking the own choice retracts the vote (the
                    // empty response list is the MSC3381 retraction).
                    selection = ownSelection().indexOf(answerId) >= 0
                                ? [] : [answerId]
                }
                app.composer.votePoll(eventId, selection, pollThreadRoot)
            }

            // Fixed intrinsic width; the Loader's Layout.maximumWidth clamps
            // it to the bubble. Referencing bubble.width here is circular in
            // the Bubbles layout (bubble sizes itself from content width).
            implicitWidth: 420
            implicitHeight: pollColumn.implicitHeight + 20
            color: AppTheme.surfaceElevated
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
                                    // A rounded pill: give any non-zero share at
                                    // least its own height so it never renders as
                                    // a thin sliver; zero votes show no fill.
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
                            return voters === 1
                                ? qsTr("Final result • 1 vote")
                                : qsTr("Final result • %1 votes").arg(voters)
                        if (voters === 0) return qsTr("No votes yet")
                        return voters === 1 ? qsTr("1 vote")
                                            : qsTr("%1 votes").arg(voters)
                    }
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.scaled(11)
                }
            }
        }
    }
}
