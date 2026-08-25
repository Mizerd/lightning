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
    // NOT clipped. The clip dates from the TableView era ("while a recycled
    // row is being remeasured"); rows are no longer recycled and size to
    // their content exactly, so there is nothing to spill. What the clip DID
    // do was cut the hover action bar off on any row shorter than it, which
    // is the reported defect. The thread panel has always run unclipped with
    // the same delegate and the same bar.
    clip: false
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
    // Speculative media work (full-payload prefetch, poster extraction) is
    // only worth spending on a row the reader actually stopped on — see the
    // long note on `speculativeMediaAllowed` in TimelinePane.qml. A host
    // without a pane (fixtures, the thread panel) is permissive.
    readonly property bool speculativeMediaAllowed:
        rowOnScreen
        && (!root.timelineView
            || root.timelineView.speculativeMediaAllowed !== false)

    readonly property bool isVirtualRow: model.isVirtual === true
    readonly property bool isStateActivity: model.isStateActivity === true
    readonly property bool isRoutineActivity: model.isRoutineActivity === true
    // 2026-08-26: a call somebody started. Its OWN row kind, not a state
    // event — this is the third row family in this chooser (message /
    // virtual / state), and §16 records what happens when a chooser names
    // only SOME of the model's kinds: a group label rendered as a room row.
    // Every branch below that enumerates row kinds names this one too.
    // `=== true` deliberately: a host whose model lacks the role reads
    // `undefined` and keeps today's behaviour.
    readonly property bool isCallEvent: model.isCallEvent === true
    // State events remain in the authoritative timeline model. This is only
    // a zero-height presentation filter, so toggling the setting restores the
    // same delegates without a resync or a second timeline.
    // 2026-08-26: "room activity" is now a master switch with two halves,
    // and the bridge has distinguished them all along (rust/src/timeline.rs
    // emits state_kind "membership" and "member_profile"). Anything that is
    // neither — room settings, topic, name — follows the master alone.
    // Keep this matrix in step with TimelineModel::activityKindVisible,
    // which answers the same question for a DATE DIVIDER's own visibility.
    readonly property bool roomActivityVisible: {
        if (!isRoutineActivity) return true
        if (!app.settings.showRoomActivity) return false
        if (model.stateKind === "membership")
            return app.settings.showMembershipEvents
        if (model.stateKind === "member_profile")
            return app.settings.showProfileChangeEvents
        return true
    }
    // v0.6.0: the timeline model this delegate's stable-id actions resolve
    // against. The room timeline supplies app.timeline; the thread panel
    // supplies app.thread.model — identical role/invokable surface.
    readonly property var timelineModel:
        root.timelineView && root.timelineView.timelineModel
        ? root.timelineView.timelineModel : app.timeline
    // Whether this row's send can still be aborted. Asks the model, never
    // the status alone: a backend with no send queue has nothing to cancel,
    // and a row with no transaction id is not addressable in one. The
    // typeof guard is for the QML suites' plain ListModel fixtures, which
    // carry no such method.
    //
    // A FUNCTION, not a root property: it needs `index`, and a root-level
    // binding on `index` is a creation-time delegate-context lookup — the
    // family that produced the poisoned-context defect fixed in 30ee39b.
    // Its one caller is deep in the built tree, where the Retry row already
    // reads `index` safely.
    function canCancelSendAt(viewRow) {
        return root.timelineModel !== null
            && typeof root.timelineModel.canCancelSend === "function"
            && root.timelineModel.canCancelSend(root.sourceModelRow(viewRow))
    }
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

    // A date divider that introduces nothing the reader can see — a run of
    // routine state rows hidden by the room-activity setting, or the
    // non-leader rows of a collapsed group — is an orphan date and must not
    // render at all. The MODEL answers this (one cached scan of the run);
    // the delegate never walks its neighbours. `=== false` deliberately:
    // a host whose model lacks the role reads `undefined` and keeps today's
    // behaviour rather than silently hiding every divider.
    readonly property bool dividerSuppressed:
        isVirtualRow && model.eventType === 7
        && model.dividerIntroducesVisibleContent === false

    // A redacted row that is NOT the first of its run renders nothing: the
    // leader carries the whole run's "N messages deleted" line. `=== false`
    // deliberately, matching dividerSuppressed above — a host whose model
    // lacks the role reads `undefined` and keeps one row per deletion rather
    // than silently hiding every deleted message.
    readonly property bool deletedFollower:
        model.redacted === true && model.deletedGroupLeader === false

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

    // Stable key for the pin-one-toolbar-at-a-time state on the ListView.
    // Prefer the SDK item id; fall back to the event id for backends that
    // don't set it. Empty for virtual rows (they have no actions).
    readonly property string actionKey: (model.itemId && model.itemId.length > 0)
                                        ? model.itemId
                                        : (model.eventId || "")
    // ── Element-style hide image ─────────────────────────────────────────
    //
    // PURELY LOCAL. Hiding sends nothing, edits nothing and redacts nothing;
    // it stops this client painting a bitmap. See MediaVisibilityStore for
    // why the flag is session-scoped and why it lives there rather than in
    // this delegate (a timeline row is destroyed the moment it leaves the
    // cache buffer, so a flag inside one is gone by the time the reader
    // scrolls back).
    //
    // Images and stickers only. Both draw a bitmap the reader may not want on
    // screen; a video card has its own poster and controls, and extending
    // this to it without evidence that anyone wants it there would be adding
    // a control to a surface that did not ask for one.
    readonly property bool mediaHideable:
        (model.isImage === true || model.isSticker === true)
        && model.redacted !== true
        && root.mediaVisibilityKey.length > 0
    // The row's media identity. `mediaKey` IS the event id once the event is
    // remote (see the Rust bridge), so this is per-event in practice; the
    // eventId fallback covers a backend that reports no key.
    readonly property string mediaVisibilityKey:
        (model.mediaKey && model.mediaKey.length > 0)
            ? model.mediaKey : (model.eventId || "")
    // A plain tracked property, not a binding on the Q_INVOKABLE: isHidden()
    // carries no per-key NOTIFY for QML to bind to. Refreshed on the two
    // events that can change the answer for THIS row — the identity changing
    // under delegate reuse, and the store announcing a write.
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
            // Keyed, so one reveal somewhere else in the timeline cannot
            // re-resolve every image row in the app.
            if (key === root.mediaVisibilityKey)
                root.mediaHidden = hidden
        }
    }
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
    // One bar at a time. The hovered row always wins over a PINNED one (a
    // pinned row shows its bar only while nothing is hovered) — without
    // that, a pinned row and a hovered row both rendered and two messages
    // looked selected at once.
    //
    // The single deliberate exception is a row whose More menu is open: the
    // menu is positioned from that bar, so hiding it would strand the menu.
    // That state ends with the menu.
    //
    // C6: while a transient surface owns row interaction — the shared
    // reaction picker, its tone popup, the profile/reader popovers, the image
    // viewer — no row may show its bar. One owner on the view, not another
    // boolean per surface, and deliberately NOT solved with z: a bar that is
    // merely covered still takes hover and still reads as selected. The More
    // menu keeps the exception above, because it is positioned FROM the bar.
    // An undefined owner (a host that predates the contract) blocks nothing.
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

    // Clears the view's hovered key if — and only if — this row owns it.
    // A row can stop being hovered without ever getting a leave event: the
    // delegate is destroyed under the pointer on a room change. The key
    // would then keep naming a row that no longer exists and NO bar would
    // show until the pointer entered some other row.
    //
    // Destruction is the only hook that can do this. An actionKey change
    // cannot: by the time the handler runs the property already holds the
    // NEW key, so there is nothing left to compare the stale one against.
    function releaseHoveredActions() {
        if (root.timelineView
                && root.timelineView.hoveredActionsKey === root.actionKey)
            root.timelineView.hoveredActionsKey = ""
    }
    Component.onDestruction: releaseHoveredActions()

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

    // v0.7.1: `alreadyInOverlaySpace` lets a caller that already computed a
    // point in Overlay.overlay coordinates (the floating action bar below)
    // hand it over directly — mapping it through root.mapToItem again would
    // double-map it.
    function openContextMenu(x, y, alreadyInOverlaySpace) {
        var eventId = root.eventIdForActions()
        if (eventId === "" || root.isVirtualRow || root.isStateActivity
            || root.isCallEvent)
            return
        // Dismiss any transient row surface FIRST. The picker and this menu
        // are both Popup.Item in one overlay, so the last one opened paints
        // and hit-tests on top — and a menu opened over the emoji grid covers
        // the very thing the reader is trying to click. z cannot fix that
        // (see TimelinePane's note); mutual exclusion can.
        //
        // Reached through `timelineView`, NOT through the pane root: a
        // delegate only ever sees its view, so a pane-root function is
        // invisible here. That is the same unreachability that silently
        // killed the reader-popover click in the 2026-08-19 round, and this
        // contract was declared on the view in the 2026-08-21 round and then
        // never called from here — which is why the picker kept being covered.
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
    // Open THIS row's sender profile. One function, three callers: the
    // avatar, the sender name, and the context menu's "View profile" — a
    // tester asked for clicking a user to open their profile and only the
    // menu did it, which is the least discoverable of the three.
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
    // Mentions carry an internal "mention:<user-id>" link (rewritten by the
    // sanitizer); open the member profile. Everything else is a validated
    // http(s) or Matrix URL. Lives on root because every body renderer in
    // this delegate — the single TextEdit and each rich-text segment —
    // must route identically.
    function openMessageLink(link) {
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
        // v0.7.x: room-oriented Matrix links open IN-APP through the
        // Discover surface (which resolves them via the SDK). User links
        // keep the web behavior — matrix.to renders a profile page there.
        var isMatrixLink =
            link.indexOf("matrix:") === 0
            || link.indexOf("matrix.to/#/") !== -1
        // Percent-encoded user permalinks (Element emits
        // matrix.to/#/%40user…) are user links too (review L2).
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

    // ── Fenced code blocks ───────────────────────────────────────────────
    // The model hands over ORDERED segments only for a body that actually
    // carries a <pre> block; an ordinary message reads back an empty list and
    // keeps the single-TextEdit path below untouched, which is the whole
    // point — this timeline instantiates every loaded row, so the hot path
    // must not grow an item for a feature most messages never use.
    // `|| []` covers a host whose model has no such role at all.
    readonly property var messageSegments: model.messageSegments || []
    // Media/redacted/poll rows suppress the body for their own reasons; the
    // segmented renderer must obey exactly the same suppression, so both it
    // and bodyLabel read these two predicates instead of each keeping its
    // own copy. They live on root (not on bodyLabel) because a root property
    // must never dereference an id declared further down the document.
    readonly property bool mediaRowBody:
        model.isImage
        || model.isSticker === true
        || model.isVideo === true
        || model.isAudio === true
        || model.isFile === true
    // -1 means "uploading, extent unknown"; 0..1 is a REPORTED fraction.
    // Normalised once here so every reader agrees, and so a fixture model
    // without the role (the QML suites' ListModels) reads as unknown rather
    // than assigning undefined to a real property.
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

    // ── Navigation (C5) ──────────────────────────────────────────────────
    // The delegate is shared by the room timeline and the thread panel, so it
    // must not know HOW a jump is performed — only that its host offers one.
    // The room pane routes to PaginationController; the thread panel resolves
    // the target inside its own thread timeline. A true thread reply must
    // never be handed to the room history loader, and the SDK's thread focus
    // guarantees a reply preview in the panel points at another in-thread
    // event or at the root, so there is no room-handoff case to write here.
    // Guarded on the function existing: a standalone fixture host degrades to
    // doing nothing instead of throwing.
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

    // ---- Reply-quote identity (2026-08-21 reply restyle) ----------------
    // Element puts the QUOTED sender's own colour on the quote's rule and
    // name, which is what makes a one-line quote scannable — you know who
    // you are about to jump to before you read a word of it. This is the
    // same deterministic hash the message header below uses, so a person's
    // quote and their own messages agree on one hue.
    //
    // The key is the raw MXID. `replyToSenderId` is NOT a TimelineModel role
    // yet — ReplyToSenderRole resolves a DISPLAY NAME, and hashing that
    // would give the same person a DIFFERENT colour in the quote than on
    // their own message, which is worse than no colour at all. Reading the
    // absent role yields undefined (the established degradation pattern in
    // this file, cf. readReceiptsTotal), userColor("") falls back to the
    // primary ink, and the quote is neutral-but-legible until the role
    // lands — at which point it colours itself with no change here.
    readonly property string replySenderKey: model.replyToSenderId || ""
    // Inside an own outgoing bubble the quote sits on saturated accent, so
    // the identity inks — tuned for contrast against surface / card /
    // other-bubble — do not apply and the bubble's own ink family does.
    // Same two-branch shape as the body ink and the status line.
    readonly property bool replyOnOwnBubble:
        bubbleMode && model.isOwn === true
    readonly property color replySenderInk:
        replyOnOwnBubble ? AppTheme.ownBubbleText
                         : AppTheme.userColor(replySenderKey)
    readonly property color replyBodyInk:
        replyOnOwnBubble ? AppTheme.onAccentMuted : AppTheme.textSecondary

    // ---- Read-receipt rail clearance -----------------------------------
    // The facepile is a zero-height overlay painted UPWARD from the row's
    // bottom edge at the ROW's right margin (a fixed rail, maintainer
    // decision 2026-08-14), while the content column stops at
    // timelineContentMaxWidth. The reaction Flow already reserves the
    // rail's width; the body never did, so once the pane was narrower than
    // roughly 820px — the 320px pane minimum, the thread panel open, a
    // laptop screen — four avatars landed directly on the last line of the
    // message.
    //
    // Reserve EXACTLY the overlap, and only when there is one: an
    // unconditional reservation would shave the rail's width off every
    // message body on a wide window for a facepile that is nowhere near it.
    // Modern/Compact only — in Bubbles the bubble's width IS its content's
    // width, so feeding a content constraint back from it closes a loop,
    // and that layout already handles the collision with the tap-band
    // exclusion on the bubble.
    readonly property real receiptRailReserve:
        (!root.bubbleMode && readReceiptStrip.visible)
        ? Math.max(0, (bubble.x + bubble.width)
                      - Math.max(root.avatarGutterWidth,
                                 readReceiptStrip.width - receiptRow.width)
                      + AppTheme.spacingXS)
        : 0

    // Date-divider wording. A divider that always spells out
    // "pirmadienis, 17 rugpjūčio 2025" makes the reader do arithmetic to
    // answer the only question it is there for — is this today? Element
    // branches Today / Yesterday / weekday-within-a-week / date, and drops
    // the year while it is the current one.
    //
    // `now` is sampled at binding time and is NOT reactive: a session left
    // open across midnight keeps yesterday's "Today" until the row is
    // rebuilt. Accepted — the alternative is a per-row clock dependency on
    // a surface that instantiates one item per loaded day.
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

    // v0.5.11: link-preview state for this row, resolved by
    // LinkPreviewController. Calling previewFor() may dispatch an automatic
    // request (unencrypted rooms with auto-load on); encrypted rooms stay in
    // "requires_action" until the explicit Load action.
    property var preview: ({ state: "none" })
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
        // A day boundary is the strongest structural break a timeline has,
        // and it used to get LESS vertical air (label + 8px total) than the
        // 12px gap between two consecutive sender groups, while rendering
        // as an unadorned scrap of grey text with no rule. It now shares
        // the unread divider's rule-label-rule idiom — two dividers in one
        // file must not speak two visual languages — at the same 30px row
        // height, with the rules in `border` so the day break stays quieter
        // than the unread break it sits near.
        implicitHeight: unreadDivider.visible ? 30
                        : virtualLabel.active
                        ? Math.max(30, virtualLabel.implicitHeight
                                       + AppTheme.spacingS * 2)
                        : 0
        // Computed on the ROW, not inside the Loader's Label: it is also
        // the Loader's `active` guard, so a divider whose text resolves
        // empty (an invalid timestamp) creates no item at all rather than a
        // Label born holding "" — the permanent-viewport-observer hazard
        // documented immediately below.
        readonly property string dividerText:
            model.eventType === 7 ? root.dayLabel(model.timestamp)
            : model.eventType === 9 ? qsTr("Beginning of conversation")
            : ""
        // ── 2026-08-19 scroll round: THE expensive QML mistake ─────────
        // A Loader, never an always-created Label. Mechanism, verified in
        // qtdeclarative 6.11.1 sources:
        //   * every QQuickText is BORN with ItemObservesViewport
        //     (QQuickTextPrivate::init, "default until size is known");
        //   * the ONLY code that clears it is QQuickText::setText, which
        //     opens with `if (d->text == n) return;` — so a text binding
        //     that produces the SAME empty string the item already holds
        //     never reaches the clearing line. (Visibility is never
        //     consulted; an invisible Label with real text is fine.)
        //   * QQuickItemPrivate::transformChanged can only switch off its
        //     per-subtree walk once NO descendant observes the viewport.
        // So one such Label per row makes Qt walk the ENTIRE instantiated
        // timeline tree on EVERY contentY change: profiled at 19.2% of all
        // cycles, with a tree walk measuring exactly 3 observers per row
        // over 1000 rows. THE RULE: in a per-row delegate, a Label whose
        // text can be "" in the state it is created in belongs in a Loader
        // — and that includes labels reading message fields, which are all
        // empty on a VIRTUAL (date-divider / read-marker) row.
        Loader {
            id: virtualLabel
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            // Start the rule at the message content indent so the divider
            // lines up with the timeline instead of floating in the avatar
            // gutter.
            anchors.leftMargin: root.avatarGutterWidth
            anchors.rightMargin: AppTheme.spacingS
            // `!dividerSuppressed`: an orphan date divider creates no label
            // at all, which is also what zeroes virtualRow's implicitHeight
            // above — the row occupies no space rather than drawing an empty
            // one.
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
                    // Message-stream text, not container chrome: it must
                    // follow the text-size setting like the timestamps and
                    // the message body it sits between.
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
            // Same inset and gap as the day divider above: one divider
            // idiom, two semantics (border rules for the day break, the
            // unread tone for the unread break).
            anchors.leftMargin: root.avatarGutterWidth
            anchors.rightMargin: AppTheme.spacingS
            spacing: AppTheme.spacingM
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
                // Scaled for the same reason the day divider is: both are
                // markers inside the message stream, and at 140% a 11px
                // fixed label beside a 20px body reads as a rendering bug.
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

    // A call somebody started. Behind a Loader because the overwhelming
    // majority of rows are not calls and this row instantiates a card, an
    // avatar, a glyph and a live-session gate — the room timeline is not
    // virtualized, so an item every row pays for is an item every row pays
    // for. `active` on the row kind alone, so a call row is built exactly
    // once and never rebuilt.
    Loader {
        id: callEventRow
        objectName: "callEventRow"
        active: root.isCallEvent
        visible: active
        // Its own air, not the message ladder's: `messageTopSpacing` is 1px
        // for a row that begins no sender group, and a CARD sitting 1px
        // under a message reads as part of it.
        y: AppTheme.spacingS
        width: parent.width
        sourceComponent: CallEventDelegate {
            // The room the CALL is in. A call row only ever appears in the
            // room timeline (a thread has none), and the pane's own call
            // banner reads the same room, so both surfaces answer for one
            // call.
            roomId: root.previewRoomId
            actorUserId: model.sender || ""
            actorName: model.senderDisplayName || ""
            actorAvatarMxc: model.senderAvatarMxc || ""
            sentence: model.callEventText || ""
            video: model.callIsVideo === true
            declinedCount: model.callDeclinedCount || 0
            timestamp: model.timestamp
            onScreen: root.rowOnScreen
        }
    }

    // Compact, discreet room-activity summary (Element-style) — never a
    // message-bubble-like card. Collapsed by default; the whole row (not
    // just the chevron) is the Expand/Collapse control.
    RoomActivityDelegate {
        id: stateActivity
        objectName: "stateActivityGroup"
        // An EMPTY entry list draws nothing: a run made only of call
        // membership yields no entries, and "0 room updates" is worse than
        // no row at all. The count comes from the DELEGATE, not from the raw
        // list — it drops call entries of its own (see its note), so the raw
        // length would claim a row it will not draw.
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

    Rectangle {
        id: rowHighlight
        // C5: the VIEW says what is highlighted. Reading app.pagination here
        // lit up the wrong timeline — a room jump highlighted the matching id
        // inside an open thread panel, which is a different navigation.
        readonly property bool navigationLanded:
            root.navigationHighlightId === (model.eventId || "")
        readonly property bool wanted:
            !root.isVirtualRow && !root.isStateActivity && !root.isCallEvent
            && (rowHover.hovered || root.actionsPinned || navigationLanded)
        // Opacity, not `visible`. A reply jump used to SLAM a saturated
        // selected-blue block on and then off again with no easing when
        // PaginationController's 1800ms timer fired — it read as a
        // rendering glitch rather than as "this is the message you asked
        // for", and the thread panel's equivalent landing has always been
        // a soft 120ms animated accent border. One user action must not
        // have two visual languages.
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
        // Design shell: message-row hover highlight is the soft theme tint
        // at an 8px radius — no border, no elevation.
        radius: AppTheme.radiusMd
        z: 0
    }

    ColumnLayout {
        id: layout
        visible: !root.isVirtualRow && !root.isStateActivity && !root.isCallEvent
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

            HoverHandler {
                id: rowHover
                onHoveredChanged: {
                    if (!root.timelineView || root.actionKey === "")
                        return
                    if (hovered) {
                        // C6: a stray hover under an open picker must not
                        // re-claim the bar the owner just cleared. No
                        // re-hover is needed once the owner releases — the
                        // next real hover event sets the key normally.
                        if (root.transientOwnerBlocks)
                            return
                        root.timelineView.hoveredActionsKey = root.actionKey
                    } else {
                        root.releaseHoveredActions()
                    }
                }
            }
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

                    // Clicking a person opens that person, as in Element and
                    // Discord. LeftButton only, so the row's right-click
                    // context menu still comes through from the bubble
                    // handler above.
                    TapHandler {
                        acceptedButtons: Qt.LeftButton
                        onTapped: root.openSenderProfileForRow()
                    }
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                }

                // Continuations keep Discord's stable gutter without paying
                // for another avatar-height row. The timestamp is available
                // on hover in that gutter instead of consuming a metadata
                // line beneath every short message.
                // Loader, active only WHILE HOVERED (2026-08-19 scroll
                // round). Measured as a viewport observer before this
                // change: Qt.formatDateTime() yields "" for the absent
                // timestamp of a VIRTUAL row, so the flag was never
                // cleared there (see the virtualLabel note). Not creating
                // it until hover also drops one item per continuation row.
                // Accepted trade, NOT separately measured: mousing down a
                // column now creates and destroys one Label per row
                // crossed, instead of ~microseconds of nothing. The
                // alternative — a persistent laid-out Label on every
                // continuation row — costs a text layout per row at load,
                // which is the more expensive side.
                Loader {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.rightMargin: 5
                    active: !root.showsIdentity && rowHover.hovered
                            && !root.compactMode
                    sourceComponent: Label {
                        objectName: "continuationTimestamp"
                        // ONE clock format for the whole application
                        // (Settings -> Appearance): 24-hour, 12-hour, or the
                        // system's. The literal "hh:mm" every message row
                        // used was 24-hour regardless of locale while the
                        // room list and Home used the locale's short format,
                        // so a 12-hour locale already saw both. Read as a
                        // PROPERTY so the binding has a real dependency —
                        // through a helper function it would keep rendering
                        // the old format until the row was next created.
                        text: Qt.formatDateTime(model.timestamp,
                                                app.settings.clockTimeFormat)
                        horizontalAlignment: Text.AlignRight
                        color: AppTheme.textMuted
                        // Scaled like the identity-line timestamp it stands
                        // in for; a fixed 9px beside a scaled sender line
                        // was the widest gap in the row at 140%.
                        font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                        Accessible.name: qsTr("Sent at %1").arg(text)
                    }
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
                // History note: a `renderedContentRight` anchor (walking
                // the widest visible bubbleContent child) lived here from
                // the 2026-08-06 float fix until 2026-08-14, when the
                // maintainer asked for Element parity instead — the chip
                // stack now rides the FULL ROW's right edge (one fixed
                // rail identical for every row, like Element's receipt
                // gutter), so a per-row content anchor is no longer
                // needed.
                height: implicitHeight
                implicitHeight: bubbleContent.implicitHeight + root.bubblePad * 2
                // ── THE MENTIONED-ROW WASH IS GONE (2026-08-21) ──────────
                // History: v0.6.0 washed a mentioned row in
                // `mentionHighlight`; v0.6.5 live feedback called it "too
                // heavy/red" and the alpha was cut 0.14/0.07 -> 0.05/0.03
                // (345f4d1) rather than removed. It came back as the SAME
                // report this round — "tagging a person creates a red box
                // around it" — and the mechanism is now measured: Storm
                // routes `mentionHighlight` to `_stoMention` #E5677A, the
                // rose the room list uses for its MENTION BADGE, and the
                // three other design themes carry the same red family. The
                // wash is therefore a rounded rectangle in the app's
                // DANGER hue drawn around every message that mentions you,
                // so a routine ping reads as an error state. Cutting the
                // alpha only made a red box fainter; it never stopped
                // being red.
                //
                // The edge bar below already exists precisely because the
                // wash was judged too loud once — it is the deliberate
                // signal (bolt for "you", neutral for @room) and it is
                // enough. Removing the fill also fixes the second half of
                // that live-feedback report: a reaction chip's own
                // translucent fill compositing on top of a tinted row
                // muddied both ("black boxes over washed rows"), and
                // there is now no tint to composite onto.
                //
                // If a wash is ever wanted back, it belongs in AppTheme as
                // a `mentionRowWash` token pointed at something that is
                // NOT the danger family — not at `mentionHighlight`, whose
                // job is the badge.
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

                // v0.6.5 live-feedback: the mention edge bar. Sits flush at
                // the bubble's own left edge — bubbleContent below gets a
                // matching extra left inset so the bar never overlaps the
                // sender/body text. Since the wash above was removed this
                // is the WHOLE signal, so it is rounded at both ends
                // instead of reading as a cut-off slab against a fill that
                // no longer exists.
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

                // Click the message content to pin the action toolbar (click again
                // or press Escape to close). Does not consume media/link taps,
                // which have their own handlers on top.
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    onTapped: (eventPoint) => {
                        // The receipt facepile paints upward from a
                        // zero-height boundary and can overlap this
                        // bubble's bottom edge (flush in bubbleMode on
                        // own messages). TapHandlers are non-exclusive
                        // across subtrees — the EmojiPicker lesson from
                        // this same round — so a tap in the facepile's
                        // band must not ALSO pin the action toolbar.
                        if (receiptRow.visible) {
                            var rp = bubble.mapToItem(
                                        receiptRow,
                                        eventPoint.position.x,
                                        eventPoint.position.y)
                            if (rp.x >= 0 && rp.x <= receiptRow.width
                                && rp.y >= 0 && rp.y <= receiptRow.height)
                                return
                        }
                        // Same class, fifth occurrence (facepile, rail
                        // chevron, tone popup, receipt chips, and now this):
                        // the reply preview's own TapHandler lives in a
                        // SIBLING subtree, so without this band exclusion a
                        // click that navigates to the replied message ALSO
                        // pinned this row's action bar.
                        if (replyBox.visible) {
                            var qp = bubble.mapToItem(
                                        replyBox,
                                        eventPoint.position.x,
                                        eventPoint.position.y)
                            if (qp.x >= 0 && qp.x <= replyBox.width
                                && qp.y >= 0 && qp.y <= replyBox.height)
                                return
                        }
                        // Sixth occurrence. The sender name carries its own
                        // TapHandler (click a person, get that person), and
                        // it lives INSIDE this bubble — so without the band
                        // the one click both opened the profile and pinned
                        // the action bar behind it.
                        if (identityLoader.visible) {
                            var ip = bubble.mapToItem(
                                        identityLoader,
                                        eventPoint.position.x,
                                        eventPoint.position.y)
                            if (ip.x >= 0 && ip.x <= identityLoader.width
                                && ip.y >= 0 && ip.y <= identityLoader.height)
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

                    // Loader (2026-08-19 scroll round): the header's own
                    // Labels read message fields, which are ALL empty on a
                    // virtual row — and an empty text binding leaves the
                    // born-with ItemObservesViewport flag in place (see the
                    // virtualLabel note). Not creating the header on a
                    // continuation row, where it is invisible anyway, also
                    // drops several items per row.
                    Loader {
                        id: identityLoader
                        // Own DM bubbles need no self-identity line.
                        active: root.showsIdentity
                                && !(root.bubbleMode && model.isOwn === true)
                        visible: active
                        // Nested layouts default to fillWidth; the header
                        // line hugs its content so the timestamp sits 8px
                        // beside the sender name (design §3), not at the
                        // row's far edge. These live on the LOADER because
                        // Layout attached properties only bind on a direct
                        // child of the enclosing ColumnLayout.
                        Layout.fillWidth: false
                        Layout.maximumWidth: Math.max(1, bubble.width - 112)
                        sourceComponent: RowLayout {
                        id: identityHeader
                        objectName: "senderIdentityHeader"
                        spacing: 6
                        Label {
                            id: nameLabel
                            objectName: "senderName"
                            text: model.senderDisplayName || model.sender
                            // Element-style identity colour: deterministic
                            // per-user ink hashed from the MXID, hue-matched
                            // to the same user's avatar disc.
                            color: AppTheme.userColor(model.sender || "")
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
                            HoverHandler {
                                id: nameHover
                                cursorShape: Qt.PointingHandCursor
                            }
                            TapHandler {
                                acceptedButtons: Qt.LeftButton
                                onTapped: root.openSenderProfileForRow()
                            }
                        }
                        // v0.5.9: compact disambiguator when the SDK reports
                        // two active members share this display name. A
                        // Loader (2026-08-19 scroll round): its text is
                        // model.sender, which is "" on a virtual row, so it
                        // measured as a viewport observer — see the
                        // virtualLabel note for the mechanism.
                        Loader {
                            active: model.senderNameAmbiguous === true
                                    && (model.senderDisplayName
                                        || "").length > 0
                            visible: active
                            Layout.maximumWidth: 180
                            sourceComponent: Label {
                                text: model.sender
                                color: AppTheme.textMuted
                                // Message data, not chrome: it sits on the
                                // identity line beside a scaled name and a
                                // scaled timestamp and has to move with them.
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

                    // ── Reply quote (2026-08-21 rebuild) ─────────────────
                    // Element's quote tile: a rule in the QUOTED SENDER's
                    // identity ink, the name in that same ink, one
                    // ellipsised line of body, and NO resting fill.
                    //
                    // What was wrong before, in order of severity:
                    //
                    //  * The fill was `AppTheme.hover` — the exact token the
                    //    row highlight painted underneath it — so pointing
                    //    at a reply made its quote DISSOLVE into the row.
                    //    And in four palettes (Lightning Light, Nordic,
                    //    Warm, Moss Light) `hover` is byte-identical to
                    //    `otherBubble`, so an incoming DM bubble's quote had
                    //    zero contrast at rest as well. A quote needs no
                    //    fill: the rule is the signifier, and leaving the
                    //    resting state transparent is what lets hover mean
                    //    something.
                    //  * It was the only coloured thing in the bubble that
                    //    never branched on Bubbles-for-DMs, so inside an own
                    //    outgoing bubble a pale slab of grey-blue sat in
                    //    saturated accent.
                    //  * Both labels were raw `font.pixelSize: 11`, so the
                    //    90-140% text-size setting did not reach the quote
                    //    at all: at 140% an ~20px body carried an 11px
                    //    ribbon.
                    //  * implicitWidth/implicitHeight named only the text
                    //    column and ignored the 34px thumbnail beside it, so
                    //    an image reply elided ~40px early and the thumb was
                    //    cropped to 34x31 by PreserveAspectCrop.
                    //  * It capped at a hardcoded 320px while every sibling
                    //    binds to `bubble.width` — eliding at 320 of an
                    //    available 760 in the room, and exceeding the 340px
                    //    thread panel.
                    //  * Two stacked 11px labels made the quote ~48% of a
                    //    one-line reply row: the thing being quoted
                    //    outweighed the thing being said. It is ONE row now.
                    //
                    // The ↰ was also dropped: a Unicode arrow inside a
                    // translatable string, rendered in the UI face (so it
                    // falls back to a system font wherever the codepoint is
                    // missing) and kept by ElideRight while the display name
                    // it decorates is cut. The mapped Material Symbols
                    // "reply" glyph is the same signifier without any of
                    // that.
                    Rectangle {
                        id: replyBox
                        objectName: "replyNavigationTarget"
                        visible: model.replyToEventId && model.replyToEventId.length > 0
                                 && !model.redacted
                        readonly property int barWidth: 2
                        // Bind to the bubble like every sibling (media,
                        // preview card, metaRow) instead of a constant that
                        // is simultaneously too small for the room and too
                        // large for the thread panel. Spanning the message's
                        // own width is also what makes the left rule read as
                        // a rule rather than as the edge of a pill.
                        Layout.fillWidth: true
                        Layout.maximumWidth: bubble.width
                        Layout.bottomMargin: 2
                        implicitWidth: replyRowWrap.implicitWidth
                                       + replyBox.barWidth + 16
                        implicitHeight: replyRowWrap.implicitHeight + 8
                        // Transparent at rest; the hover tint is drawn from
                        // the quoted sender's own ink so the feedback
                        // belongs to the tile instead of repeating the row
                        // highlight it sits on.
                        color: replyHover.hovered
                               ? Qt.alpha(root.replySenderInk, 0.12)
                               : "transparent"
                        Behavior on color { ColorAnimation { duration: 90 } }
                        radius: AppTheme.radiusSm
                        // A quote block that navigates is a control, and it
                        // was reachable only with a mouse: no tab stop, no
                        // key activation, no focus ring, and an accessible
                        // name that never said whose message it goes to.
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
                        // The tile responded to nothing on hover although
                        // the cursor changed shape over it — so it never
                        // read as clickable.
                        HoverHandler { id: replyHover }
                        TapHandler {
                            cursorShape: Qt.PointingHandCursor
                            // Routes through the view contract, never
                            // app.pagination: inside the thread panel that
                            // would hand a true thread reply to the ROOM
                            // history loader, which cannot hold it.
                            // Deliberately does NOT take focus: a mouse jump
                            // must not pull focus out of the composer
                            // mid-sentence. Tab reaches the same control.
                            onTapped: root.navigateToReplyTarget()
                        }
                        Rectangle {
                            width: replyBox.barWidth
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            anchors.topMargin: 1
                            anchors.bottomMargin: 1
                            // Rounded so the rule's ends do not fill in the
                            // arc the box's own radius cuts out on hover —
                            // the square-corners-inside-a-rounded-box edge
                            // this block used to show.
                            radius: 1
                            color: root.replySenderInk
                            opacity: replyHover.hovered ? 1.0 : 0.8
                            Behavior on opacity { NumberAnimation { duration: 90 } }
                        }
                        RowLayout {
                            id: replyRowWrap
                            anchors.left: parent.left
                            anchors.right: parent.right
                            // verticalCenter, NOT top+bottom: anchoring both
                            // edges stretched the row to the box height the
                            // box had computed from the TEXT alone, which is
                            // what squashed a 34px thumbnail to 31px.
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
                            // 2026-08-18 tester report #2: an image reply
                            // target shows a small thumbnail — same media
                            // bridge, same registry, keyed by the reply
                            // target's own event id. 24px on ONE line now,
                            // with the corner baked by MediaImageProvider's
                            // "|shape:round:" suffix (the message image and
                            // the video poster already use it) rather than a
                            // per-row MultiEffect mask, which costs two
                            // extra render passes per item per frame.
                            Image {
                                id: replyThumb
                                // Bumped by the cache-fill handler below, so
                                // this re-resolves without anyone assigning
                                // `source` imperatively — that would destroy
                                // the binding and strand the row on whichever
                                // image happened to land first.
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
                                        // The first mediaSource() call may
                                        // return "" while bytes fetch;
                                        // re-ask once the cache fills.
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
                                // A user-chosen display name must not be
                                // allowed to eat the quoted line it
                                // introduces; the body keeps the rest.
                                // A FIXED cap, deliberately — deriving it
                                // from replyRowWrap.width closes a loop in
                                // Bubbles mode, where the bubble's width is
                                // its content's implicit width and this
                                // Label is part of that content. The message
                                // header above caps the same way (320).
                                Layout.maximumWidth: AppTheme.scaled(180)
                            }
                            Label {
                                objectName: "replyQuoteBody"
                                text: model.replyToPreview
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
                        // Defined on root so the segmented renderer below can
                        // apply the identical suppression without an id
                        // dereference into this object; the two names stay
                        // here because the whole file reads them.
                        readonly property bool isMediaRow: root.mediaRowBody
                        readonly property bool isMediaCaption:
                            root.mediaCaptionBody
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
                            // A body that carries fenced code renders through
                            // the segmented column below instead. Returning ""
                            // here (rather than only hiding this item) is what
                            // keeps a long code message from being laid out
                            // twice — the RichText document would otherwise
                            // still be built for an invisible item.
                            if (root.hasMessageSegments) return ""
                            if (model.redacted) {
                                // A run of deletions collapses to one line.
                                // TimelineModel groups them exactly the way it
                                // groups state changes, so a moderator
                                // clearing twenty messages costs one row here
                                // instead of twenty identical ones.
                                return model.deletedGroupCount > 1
                                    ? qsTr("%n message(s) deleted",
                                           "collapsed run of redactions",
                                           model.deletedGroupCount)
                                    : qsTr("[message deleted]")
                            }
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
                            var html = (fb && fb.length > 0)
                                ? fb
                                : app.linkPreviews.linkifiedBody(
                                      model.body || "")
                            // A whole-room mention is plain body text in BOTH
                            // paths — there is no matrix.to link for
                            // "everyone here" — so without this it renders as
                            // ordinary text while every other mention is
                            // inked. Gated on the event's own
                            // m.mentions.room: a body that merely contains
                            // the characters must never be painted as a
                            // broadcast ping.
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
                        // NO lineHeight here, and it is not an oversight.
                        // The design system asks for
                        // AppTheme.lineHeightBody on every wrapping text
                        // item, and a wrapped message paragraph genuinely
                        // does run tighter inside itself (~1.2, the font's
                        // own hhea metrics) than the 12px gap between two
                        // senders — which is what makes a busy room read as
                        // a wall. But `lineHeight`/`lineHeightMode` are
                        // QQuickText properties and this is a TextEdit
                        // (selectByMouse + RichText + link activation), so
                        // assigning them is a hard "cannot assign to
                        // non-existent property" component error, not a
                        // no-op — verified with qmllint 6.11.1 against the
                        // resolved QtQuick module. Setting the leading here
                        // needs either a `line-height` declaration emitted
                        // by MessageHtml::sanitize (C++) or a move off
                        // TextEdit; both are outside a presentation change.
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
                        // Keep the last line clear of the receipt rail —
                        // see receiptRailReserve.
                        Layout.rightMargin: root.receiptRailReserve
                        textFormat: Text.RichText
                        selectByMouse: true
                        Accessible.name: model.body || ""
                        // One routing implementation, shared with the
                        // segmented renderer below — a second copy would
                        // drift, and this one carries the mention and
                        // user-permalink rules.
                        onLinkActivated: function(link) {
                            root.openMessageLink(link)
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

                    // ── Fenced code blocks (C1) ──────────────────────────
                    // Ordered segments in place of the single body TextEdit,
                    // for the only rows that need them. A Loader, not an
                    // always-created column: `active` is false for every
                    // ordinary message, so those rows instantiate NOTHING
                    // here — which also keeps the zero-viewport-observer
                    // guarantee, since none of the text items below can
                    // exist holding an empty string.
                    Loader {
                        id: segmentsLoader
                        objectName: "messageSegments"
                        // Row values captured OUT HERE, outside the Repeater:
                        // inside a delegate the bare name `model` can resolve
                        // to the segment's own model object instead of this
                        // timeline row's, and a silently wrong resolution is
                        // exactly the class of bug this file keeps paying for.
                        readonly property bool ownMessage: model.isOwn === true
                        readonly property string plainBody: model.body || ""
                        active: root.hasMessageSegments
                        visible: active
                        Layout.fillWidth: true
                        Layout.maximumWidth: bubble.width > 8
                                             ? Math.min(720, bubble.width - 8)
                                             : 560
                        Layout.rightMargin: root.receiptRailReserve
                        // The width a segment may grow to, derived from the
                        // ROW and nothing below it. It cannot be read from
                        // `bubble.width` or from a segment's own arranged
                        // width: in Bubbles mode bubble.width IS
                        // bubbleContent.implicitWidth, which is this column's
                        // implicit width, which is the segments' — so a
                        // segment sized against either one feeds its own
                        // input and Qt reported a binding loop on
                        // implicitWidth for every message carrying a fenced
                        // code block. bubbleRow is fillWidth in `layout`
                        // (whose width is the delegate's) and reports no
                        // implicit width, so this end of the chain is inert.
                        readonly property real segmentCap: {
                            var avail = Math.max(
                                1, bubbleRow.width - root.avatarGutterWidth)
                            avail = root.bubbleMode
                                    ? Math.max(60, avail - 40)
                                    : Math.min(AppTheme.timelineContentMaxWidth,
                                               avail)
                            return Math.max(
                                80, Math.min(720,
                                             avail - root.bubblePad * 2 - 8))
                        }
                        sourceComponent: ColumnLayout {
                            spacing: 4
                            // ONE accessible reading for the whole message,
                            // on the container: naming every segment with
                            // the full body would read the message once per
                            // segment, and naming each with its own
                            // RichText would read markup. model.body is the
                            // original markdown source, code fences and all.
                            Accessible.role: Accessible.StaticText
                            Accessible.name: segmentsLoader.plainBody
                            Repeater {
                                model: root.messageSegments
                                delegate: Item {
                                    id: segmentRow
                                    required property var modelData
                                    // kind: 0 RichText, 1 CodeBlock. Anything
                                    // else is treated as rich text — an
                                    // unknown kind must degrade to readable
                                    // prose, never to a blank row.
                                    readonly property bool isCode:
                                        modelData && modelData.kind === 1
                                    Layout.fillWidth: true
                                    // Both implicit sizes are propagated, not
                                    // just the height: in Bubbles layout the
                                    // bubble's width IS bubbleContent's
                                    // implicit width, so a row that reports 0
                                    // would collapse a code-only DM message
                                    // to the 60px floor.
                                    implicitWidth: segmentLoader.item
                                        ? segmentLoader.item.implicitWidth : 0
                                    implicitHeight: segmentLoader.item
                                        ? segmentLoader.item.implicitHeight : 0
                                    Loader {
                                        id: segmentLoader
                                        anchors.left: parent.left
                                        // The mediaBox idiom: take the
                                        // segment's own natural width, capped
                                        // at the column. A code block sizes
                                        // to its widest line (it clamps and
                                        // scrolls internally past that) and a
                                        // two-word snippet must not stretch
                                        // edge to edge; a long paragraph hits
                                        // the cap and wraps, exactly as the
                                        // single-body TextEdit does.
                                        width: item
                                               ? Math.min(
                                                     segmentsLoader.segmentCap,
                                                     item.implicitWidth)
                                               : segmentsLoader.segmentCap
                                        sourceComponent: segmentRow.isCode
                                                         ? codeSegment
                                                         : richSegment
                                    }
                                    // Declared inside the delegate on
                                    // purpose: an inline Component's creation
                                    // context is the object it is declared
                                    // in, so `segmentRow` and `root` resolve
                                    // here. A Component hoisted to the file
                                    // root could not see the delegate scope.
                                    Component {
                                        id: richSegment
                                        TextEdit {
                                            objectName: "messageSegmentText"
                                            text: root.highlightSearchMatches(
                                                      segmentRow.modelData.text
                                                      || "",
                                                      root.searchHighlight,
                                                      root.isCurrentSearchHit)
                                            // Same ink, family, scaling and
                                            // interaction the single-body
                                            // path uses, so prose either
                                            // side of a code block is
                                            // visually unchanged. Big emoji
                                            // cannot occur here: a body with
                                            // a fenced block is not an
                                            // emoji-only body.
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
                                            // No lineHeight, same reason
                                            // as the single-body path
                                            // above: TextEdit has no such
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
                                            // PLAIN text from the model,
                                            // never html — CodeBlock renders
                                            // it with PlainText formatting.
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
                            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
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
                            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
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
                            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
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

                    // Upload progress for an outgoing attachment. The
                    // figures are the SDK send queue's own MediaUpload
                    // reports, carried on the local echo's send state, so
                    // this is real transferred bytes and not a timer.
                    //
                    // uploadProgress is -1 while the total is NOT known —
                    // the first diff of a media send routinely lands before
                    // the first progress report — and that renders as the
                    // INDETERMINATE sweep. Drawing a 0% bar there would
                    // claim a measurement that does not exist, and it would
                    // sit at 0% for the whole of a small upload.
                    //
                    // Loader, not a `visible:` binding: this exists on one
                    // row in a thousand, and an always-built bar is one more
                    // permanent item per delegate in an un-virtualized
                    // Column.
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
                        // Loader, not an empty-text Label: this text is ""
                        // on every row that is neither sending, failed nor
                        // edited, which is a permanent viewport observer —
                        // see the virtualLabel note for the mechanism.
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
                                        // Percentage only where there IS
                                        // one. -1 is "extent unknown", and
                                        // "sending… 0%" would be a claim.
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
                                Accessible.name: text
                            }
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
                            // Its sibling one line up is already
                            // scaled(10); the two are one status line.
                            font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                            font.underline: true
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.timelineModel.retrySend(
                                               root.sourceModelRow(index))
                            }
                        }
                        // Discard a send that has not reached the server —
                        // the answer to a message wedged in "sending…", and
                        // to a failed one the user simply does not want any
                        // more. Routed to the SDK send queue's own abort,
                        // which is the only thing that can cancel an
                        // in-flight media UPLOAD as well as a queued event.
                        //
                        // The row is not removed here. The abort can lose
                        // the race with the server, and the backend removes
                        // the item only when it really aborted — a local
                        // removal would hide a message the room already
                        // has. Gated on the model, not on the status alone,
                        // so a backend with no send queue never offers a
                        // cancel there is nothing behind.
                        Label {
                            objectName: "cancelSendLink"
                            // model.status is named FIRST so the binding
                            // takes a dependency on it: canCancelSendAt is
                            // a plain function call and re-evaluates only
                            // when something in this expression changes.
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
                        // v0.6.1: Element-style thread summary card on the root
                        // event. Thread replies are hidden from the main
                        // timeline (SDK hide_threaded_events), so the root
                        // carries the entire thread's presence here. The card
                        // shows the message-bubble icon, latest sender, a safe
                        // preview, the authoritative reply count and an unread
                        // indicator; activating it opens the correct thread.
                        // Loader (2026-08-19 scroll round): the card existed
                        // in EVERY row though thread roots are the rare
                        // case, and its own timeLabel() returns "" whenever
                        // latestTimestamp is unset — a permanent viewport
                        // observer (see the virtualLabel note). Note that
                        // hazard survives INSIDE an active card, so the
                        // regression test seeds a thread root with no
                        // timestamp. previewLoader above is the precedent.
                        Loader {
                            active: model.isThreadRoot === true
                            visible: active
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
            }

            // Action toolbar.
            //
            // v0.7.1 CORRECTION: an earlier version of this fix made the
            // per-row Loader's loaded Rectangle reparent into
            // Overlay.overlay to escape bubbleRow's clip (needed because a
            // short/continuation row is shorter than the ~32px bar). That
            // reparenting was BROKEN: the Loader still believed it owned
            // the (now elsewhere-parented) item for destruction purposes,
            // and destroying the delegate during pagination/room-switch
            // churn produced a dangling-pointer SIGSEGV — bisected against
            // timeline-pane-qml-test (52 passed/11 failed on HEAD, crash on
            // that version). The `detailsDialogComponent` Dialog precedent
            // this followed does not transfer: a Dialog is a Popup, which
            // manages its own overlay/window lifetime; a plain Rectangle
            // loaded by a Loader is not.
            //
            // Root's clip is `false` in the thread panel (a real ListView —
            // see line 13's `ListView.view === null`), so that host never
            // had the clipping defect and keeps the original, always-safe,
            // in-row anchored bar below (with the tooltip-flip fix folded
            // in, since that half of the original report — the "More"
            // tooltip clipping against the window's top edge — applies
            // there too).
            //
            // ONE bar per row, created on first hover and anchored in
            // place. Positioning is plain anchors against the row itself —
            // no mapToItem into an overlay, which could not survive the
            // rows' 180-degree rotation (it placed the bar at the visual
            // BOTTOM) and had no dependency to re-evaluate on when the view
            // scrolled. Hover is naturally exclusive, so two rows can never
            // both show one.
            Loader {
                id: messageActionBarLoader
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: -3
                // `parent` is bubbleRow — the FULL timeline width. The bar
                // sits at the row's top-RIGHT CORNER, which is where Element
                // puts it (`mx_MessageActionBar` is `right: 8px` on an event
                // tile that spans the whole timeline panel).
                //
                // The 2026-08-21 round briefly pulled the bar left by the
                // content column's empty gutter, to land it on the text's
                // own right edge. That was WRONG in practice and Rokas
                // reported it: the content column is clamped to
                // timelineContentMaxWidth, so the "content edge" is a
                // CONSTANT — the bar rendered at the same fixed x on every
                // row, floating mid-row with nothing under it, and short
                // rows looked no different from long ones. A corner is a
                // landmark; a fixed offset into open space is not.
                //
                // The margin clears the overlaid vertical scrollbar
                // (AppScrollBar sits on the Flickable's right edge and this
                // row is not inset from it) so the bar can never render
                // underneath the handle.
                //
                // Still a plain in-row anchor: no mapToItem, which is what
                // the reverted overlay bar was and could not survive the
                // rows' 180-degree rotation.
                anchors.rightMargin: AppTheme.scrollbarWidth + AppTheme.spacing2
                z: 3
                // Created on first need, then latched alive for the
                // delegate's lifetime; visibility gates afterwards. The
                // latch write is DEFERRED: onLoaded fires synchronously
                // inside the active binding's own evaluation, and a direct
                // write to one of its dependencies from there is a
                // detected binding loop.
                property bool latched: false
                active: latched || root.actionsVisible
                onLoaded: Qt.callLater(function() { latched = true })
                visible: root.actionsVisible
                sourceComponent: Rectangle {
                id: messageActionBar
                // v0.6.5 (SPEC 1a): container surface bg, 1px borderStrong,
                // radius radiusTile, 2px padding.
                radius: AppTheme.radiusTile
                color: AppTheme.surface
                border.color: AppTheme.borderStrong
                border.width: 1
                implicitWidth: threadActionRow.implicitWidth
                               + AppTheme.spacing2 * 2
                implicitHeight: threadActionRow.implicitHeight
                                + AppTheme.spacing2 * 2

                // Tooltip-flip only — a plain computed boolean from a live
                // mapToItem READ. This stores nothing and reparents
                // nothing, so it carries none of the risk the bar's own
                // positioning did; it is exactly as safe as the reaction
                // picker's own anchor-point computation elsewhere in this
                // file. 44 is a conservative one-line tooltip height
                // (~30px) plus a clear margin.
                readonly property bool tooltipsBelow:
                    Overlay.overlay
                    ? bubbleRow.mapToItem(Overlay.overlay, 0, 0).y < 44
                    : false

                Row {
                    id: threadActionRow
                    anchors.centerIn: parent
                    spacing: 2
                    // Element's own bar leads with this on an image row, and
                    // leading is right: it is the only action here that is
                    // about the picture rather than about the message.
                    //
                    // Gone once the image IS hidden — the placeholder's "Show
                    // image" is then the primary action, and a second control
                    // offering to hide what is already hidden is noise.
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
                        // Cheap local predicate — semantically identical
                        // to "messagePermalink() is non-empty" but without
                        // the per-row O(n) timeline scan the C++ call cost
                        // (three of these per row made room open O(n²)).
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
                        // Cheap local predicate — semantically identical
                        // to "messagePermalink() is non-empty" but without
                        // the per-row O(n) timeline scan the C++ call cost
                        // (three of these per row made room open O(n²)).
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
                        id: threadMoreButton
                        implicitWidth: 28; implicitHeight: 28
                        radius: AppTheme.radiusControl
                        iconName: "more_vert"
                        iconSize: 18
                        // v0.6.5 (SPEC 1a): active button gets the accentSoft
                        // chip while its menu is open.
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
                            // MAP to overlay space — the old code passed
                            // row-LOCAL coordinates while claiming they were
                            // already in overlay space, so the menu popped at
                            // a position that had nothing to do with the
                            // button, and the room timeline's 180-degree row
                            // rotation mirrored it to the opposite edge of
                            // the screen.
                            //
                            // Both corners are mapped and the max taken
                            // rather than assuming which local corner is
                            // visually bottom-right: that flips with the
                            // rotation, and the thread panel is NOT rotated.
                            // mapToItem handles the transform; this only has
                            // to stay agnostic about which way round it is.
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
            // 2026-08-18 tester report ("infinite reactions eina i sona"):
            // a Flow only wraps if it HAS a width, and without fillWidth its
            // width was its own implicit single-row width — so a busy message
            // ran its chips straight off the right edge of the window, out of
            // reach, and over the row's hover action bar. Filling the row
            // gives the Flow a real width to wrap inside; the right margin
            // keeps the last chip clear of the read-receipt rail, which is
            // painted upward from the row's bottom edge at the same corner.
            Layout.fillWidth: true
            Layout.rightMargin: readReceiptStrip.visible
                                ? receiptRow.width + AppTheme.spacingXS : 0
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
                    // 9px side / 3px vertical padding, min height 22. Every
                    // state is PAINT ONLY — geometry never moves on
                    // hover/press/focus/selected, because these chips wrap
                    // in a Flow and a 1px growth would reflow the row.
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
                        ? AppTheme.reactionSelectedBackground
                        : AppTheme.reactionBackground
                    // Qt.darker BOTH ways was backwards on the eight dark
                    // themes: the pill moved toward the near-black row
                    // behind it, so pointing at the most-clicked control in
                    // a chat client made it RECEDE. Lift on dark, deepen on
                    // light — one predicate, both directions.
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
                    // Whole pixels only: a 1.5px border cannot land on a
                    // pixel boundary at DPR 1 and rendered as two rows of
                    // half-covered antialiasing — soft exactly where the
                    // "you reacted" signal has to be crisp.
                    border.width: modelData.byMe ? 2 : 1
                    // The chip was a bare Rectangle: Accessible.role said
                    // Button but a Rectangle is not a focus stop, so Tab
                    // never reached it and nothing ever drew focus.
                    activeFocusOnTab: true
                    Keys.onReturnPressed: (event) => {
                        app.composer.reactTo(root.eventIdForActions(),
                                             modelData.key)
                        event.accepted = true
                    }
                    Keys.onEnterPressed: (event) => {
                        app.composer.reactTo(root.eventIdForActions(),
                                             modelData.key)
                        event.accepted = true
                    }
                    Keys.onSpacePressed: (event) => {
                        app.composer.reactTo(root.eventIdForActions(),
                                             modelData.key)
                        event.accepted = true
                    }
                    // Drawn INSIDE the pill, not at the shared -4px outset:
                    // chips sit in a Flow with 4px spacing and an outset
                    // ring would cross its neighbour and the read-receipt
                    // rail beside the last one.
                    Rectangle {
                        anchors.fill: parent
                        visible: reactionChip.activeFocus
                        color: "transparent"
                        radius: reactionChip.radius
                        border.width: 2
                        border.color: AppTheme.focusRing
                    }
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
                    // The 22px floor scales with the chip's own text:
                    // pinning it while the labels grow would let a 140%
                    // count overflow its pill.
                    implicitHeight: Math.max(AppTheme.scaled(22),
                                             reactionRow.implicitHeight + 6)
                    HoverHandler { id: reactionHover }

                    // ── Who reacted (C2) ─────────────────────────────────
                    // Names are RESOLVED in C++ (room display name, localpart
                    // fallback, never a bare MXID) and the delivered list is
                    // capped; `reactorTotal` is the UNCAPPED count, so the
                    // tail is always truthful. Nothing is fabricated here: an
                    // absent list means no tooltip at all, never a guess.
                    // Degrades on a host whose model predates the fields.
                    readonly property var reactorNames:
                        modelData.reactorNames || []
                    readonly property int reactorTotal:
                        modelData.reactorTotal >= 0
                        ? modelData.reactorTotal : (modelData.count || 0)
                    // A display name is user-chosen and can be 255 chars, and
                    // the shared ToolTip instance has no width cap of its
                    // own — so bound each name here and mark the cut with an
                    // ellipsis. Visibly truncated, never silently rewritten.
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
                        // A delivered list longer than the reported total
                        // would make the tail negative — trust the larger.
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
                    // The ATTACHED form, exactly like the read-receipt strip
                    // above: one shared ToolTip instance for the whole
                    // application. A declared ToolTip child would build a
                    // Popup, a background and a Label PER CHIP, and a busy
                    // message carries dozens of chips — the same eager
                    // per-row instantiation cost this file has already paid
                    // for twice (the context menu and the details dialog are
                    // both lazy now for exactly this reason).
                    //
                    // The HoverHandler covers the whole chip, so moving the
                    // pointer within it never leaves and the tip cannot
                    // flicker. Short delay — this is a read, not a warning.
                    ToolTip.text: reactionChip.reactorSummary
                    ToolTip.visible: reactionHover.hovered
                                     && reactionChip.reactorSummary.length > 0
                    ToolTip.delay: 300
                    RowLayout {
                        id: reactionRow
                        anchors.centerIn: parent
                        spacing: 5
                        Label {
                            Layout.alignment: Qt.AlignVCenter
                            Layout.preferredHeight: AppTheme.scaled(16)
                            text: modelData.key
                            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                            verticalAlignment: Text.AlignVCenter
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
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        // Deliberately does NOT take focus, for the same
                        // reason the reply quote does not: a click here must
                        // not pull the caret out of the composer mid-
                        // sentence. Tab reaches the same control.
                        onClicked: app.composer.reactTo(
                                       root.eventIdForActions(), modelData.key)
                    }
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.byMe
                        ? qsTr("Reaction %1, %2, selected").arg(modelData.key).arg(modelData.count)
                        : qsTr("Reaction %1, %2").arg(modelData.key).arg(modelData.count)
                    // The same information the hover tooltip carries, so a
                    // keyboard/AT user is not the only one who cannot find
                    // out who reacted. Empty when the senders are unknown —
                    // an absent list is never described as "nobody".
                    Accessible.description: reactionChip.reactorSummary
                    // Accessible.role/name alone describe the control to
                    // assistive tech but do not make it ACTIVATABLE — an AT
                    // user invoking it (not clicking with a mouse) needs
                    // this mirror of the MouseArea's onClicked.
                    Accessible.onPressAction:
                        app.composer.reactTo(root.eventIdForActions(), modelData.key)
                }
            }

            // Element's inline "+" pill, closing the reaction row. Joining
            // an EXISTING reaction was one click; adding a NEW one to a
            // message that already had reactions meant finding the hover
            // toolbar — which, before this round, floated hundreds of pixels
            // to the right of the row. This puts the affordance where the
            // hand already is. It reuses the SHARED picker through the same
            // openReactionPickerFor() the toolbar button calls, so it costs
            // no extra popup instance per row.
            Rectangle {
                id: reactionAddChip
                objectName: "reactionAddChip"
                // The same geometry contract as the chips beside it: paint
                // changes on hover/press/focus, geometry never moves.
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
                // Outlined at rest so it reads as "add", not as a reaction
                // somebody left; it fills in only once pointed at.
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
            // message, own or not. Since 2026-08-14 (maintainer request,
            // Element parity) the chip stack rides this strip's own right
            // edge — the FULL ROW width, not the 760px-capped content
            // column — one fixed receipt rail for every row, like
            // Element's receipt gutter at the timeline's right edge.
            Layout.fillWidth: true
            // This is a zero-height overlay boundary at the message bottom,
            // not another row below the message. Cancel ColumnLayout's
            // inter-child spacing and paint the measured chip row upward from
            // that boundary, so neither font scaling nor media/reaction height
            // can create a receipt-only tail.
            Layout.topMargin: -layout.spacing
            implicitHeight: 0

            Row {
                id: receiptRow
                objectName: "readReceiptRow"
                x: Math.max(root.avatarGutterWidth,
                            readReceiptStrip.width - width)
                y: -height
                // Facepile overlap; each avatar sits on an 18px surface
                // ring so overlapped edges stay legible on any theme.
                spacing: -4
                // The ring must paint what the row currently shows — a
                // bare AppTheme.background ring punches visible holes
                // into the hover/selection tint. Since the row highlight
                // now FADES, the ring has to fade with it or the discs
                // flash a fully-opaque tint over a half-faded row.
                readonly property color hoverTint:
                    rowHighlight.visible ? rowHighlight.color : "transparent"
                readonly property real hoverTintOpacity:
                    rowHighlight.visible ? rowHighlight.opacity : 0
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
                        // The rail's geometry is fixed (18px avatar discs),
                        // so this label is genuinely chrome and does NOT
                        // scale — but it goes through the token rather than
                        // a bare 9.
                        font.pixelSize: AppTheme.fontMicro
                        font.weight: AppTheme.weightBold
                    }
                }

                TapHandler {
                    // Click → the full reader list (2026-08-18 tester report #2):
                    // everything the bridge delivered (up to 16, newest first)
                    // plus a truthful "+N more" tail — never fabricated names.
                    onTapped: (eventPoint) => {
                        if (!root.timelineView
                            || !root.timelineView.openReceiptList)
                            return
                        // eventPoint.position is local to the handler's
                        // PARENT — receiptRow, not the strip. The row is
                        // offset from the strip by its right-alignment x
                        // and its own -height y, so mapping from the
                        // strip would misplace the popover by exactly
                        // that offset (review find, 2026-08-18).
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
                // One accessible summary for the whole strip — individual
                // chips are deliberately not focus stops.
                Accessible.role: Accessible.StaticText
                Accessible.name: readReceiptStrip.summary
            }
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
                .arg(Qt.formatDateTime(model.timestamp,
                                       app.settings.clockTimeFormat))
            // C6: the menu takes transient row-interaction ownership so no
            // OTHER row can show its toolbar underneath it, while this row
            // keeps its own — transientOwnerBlocks carries exactly that
            // exception, because the menu is positioned FROM this row's bar
            // and hiding it would strand the menu. Without these two lines
            // the "menu" owner was never claimed by anything, so the
            // documented exception was unreachable and the round's own
            // comment described behaviour the code did not have.
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
            }
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
            // This one deliberately KEEPS app.pagination while
            // the reply preview moved to the view contract: it
            // is offered only in the thread panel and its whole
            // purpose is the ROOM, so routing it through the
            // thread's own navigation would defeat it.
            AppMenuItem {
                iconName: "arrow_forward"
                text: qsTr("Open in room")
                visible: root.inThreadPanel
                enabled: root.menuEventId !== ""
                onTriggered: app.pagination.jumpToEvent(
                    root.menuEventId)
            }
            AppMenuSeparator {}
            // v0.7.x pinned messages. Exactly ONE of these is ever offered:
            // canTogglePin() answers false for the action that does not
            // apply, for a viewer without the room's real
            // m.room.pinned_events power level, and while a write is in
            // flight. It also fails closed before the first snapshot, so
            // the menu never offers a pin it cannot honour.
            //
            // Deliberately hidden in the thread panel: a thread reply is
            // pinnable Matrix-wise, but the pinned surface lives in Room
            // Information and pinning from a thread would put the message
            // somewhere the user is not looking.
            AppMenuItem {
                iconName: "push_pin"
                text: qsTr("Pin message")
                // `revision` is the re-evaluation dependency: canTogglePin
                // is a Q_INVOKABLE and a binding cannot observe its inputs.
                //
                // 2026-08-18 tester report ("you can pin 'message deleted'
                // useless"): a redacted event has no content left to pin, so
                // pinning one only adds a dead entry to
                // m.room.pinned_events. UNPIN below stays offered for a
                // redacted event on purpose — a message pinned before it was
                // deleted must still be removable.
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
            // Right-clicking a picture should offer what a person expects
            // to find there (the 2026-08-22 report asked for it in those
            // words). Open comes first because it is what the left click
            // does, and a context menu that omits the obvious action reads
            // as the wrong menu.
            //
            // There is deliberately no "Copy image address": under
            // authenticated media an mxc URL is not a link anyone else can
            // follow, and handing one out is exactly what CLAUDE.md §6
            // forbids.
            AppMenuItem {
                objectName: "openMediaMenuItem"
                iconName: "open_in_full"
                text: qsTr("Open image")
                // Images and stickers only. The image viewer is what
                // `openImage` opens; a video row has its own player card and
                // its own fullscreen overlay, and sending it here would open
                // the wrong surface.
                visible: (model.isImage === true || model.isSticker === true)
                         && model.mediaSourceAvailable === true
                enabled: visible && root.timelineView
                         && !!root.timelineView.openImage
                onTriggered: root.timelineView.openImage(
                    model.mediaKey || "", model.mediaUrl)
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
            // A SIBLING of "Save as…", not a child of it. Nested inside, this
            // became a child ITEM of that row and painted on top of it — two
            // labels overlapping in the same 32px strip, which is the
            // "Sopy asnage" in the 2026-08-21 screenshot. A Menu lays out its
            // own AppMenuItem children; one nested in another is not in that
            // list and gets no row of its own.
            // The same local hide, from the menu. Not duplication for its own
            // sake: the action bar appears on hover, and a keyboard user
            // reaches the menu instead. One state, two entry points, and the
            // label says which way it goes.
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
                objectName: "copyImageMenuItem"
                iconName: "content_copy"
                text: qsTr("Copy image")
                // Images only (the raster clipboard is meaningless for video/
                // files), same availability gates as Save as.
                visible: model.isImage === true
                         && model.mediaSourceAvailable === true
                         && app.mediaBridge.supported
                enabled: visible && root.menuEventId !== ""
                onTriggered: app.copyImageToClipboard(model.mediaKey || "")
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
            // 2026-08-18 tester request ("add function remove all edits").
            // Matrix has no unedit: the edits are separate m.replace events
            // and taking them back means redacting them, which is what this
            // does — the message returns to its original text and stops
            // being marked as edited. Own, edited, editable messages only,
            // and only on a backend that can reach the relations.
            AppMenuItem {
                objectName: "removeEditsMenuItem"
                iconName: "undo"
                text: qsTr("Remove edits")
                enabled: model.edited === true
                         && root.timelineModel.canEditEvent(root.menuEventId)
                         && app.composer.canRemoveEdits()
                visible: enabled
                onTriggered: app.composer.removeEdits(root.menuEventId)
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
                         || (app.moderation.reportSupported
                             && model.isOwn !== true)
            }
            // v0.7.x message forwarding :
            // withheld for redacted / local-echo / undecryptable / poll
            // content — none of it has anything safe to re-send — and for
            // a media row whose source is not (yet) fetchable. begin()
            // takes an IMMUTABLE snapshot of exactly this row's model data,
            // captured here at click time, never a live re-resolution (see
            // ForwardController's class comment) — this is why the whole
            // snapshot is built inline rather than deferred into C++.
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
            // v0.7.x: report to the homeserver administrator (stable /v3
            // event report). Own messages are excluded — deleting them is
            // the sensible action, and self-reports only add noise.
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
                onTriggered: app.composer.redact(root.menuEventId)
            }
        }
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
                // The body Labels ink from the GENERAL namespace
                // (AppTheme.text / textMuted), so the panel must be the
                // general surface too — a storm panel under general inks
                // pairs two different routing tables.
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
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: app.media.openWebUrl(directMedia.p.url)
            }
        }
    }

    Component {
        id: linkPreviewComponent
        Rectangle {
            id: card
            objectName: "linkPreviewCard"
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
            readonly property real fullW: Math.min(400, bubble.width - 8)
            // The consent gate is ONE band, so it sizes to its own content
            // instead of claiming the full preview width for a link nobody
            // has agreed to load yet; every other state still fills the
            // column. Reading a layout's implicitWidth from an ancestor is
            // only safe when nothing under it reads the width that layout
            // COMPUTES (CLAUDE.md 2026-08-26, the recursive-rearrange
            // round) — everything in the gate row contributes a natural
            // text or control implicit width, and QQuickText reports its
            // unwrapped natural width whatever wrapMode says, so there is
            // no path back from the assigned width into this value.
            implicitWidth: st === "requires_action"
                           ? Math.min(fullW,
                                      cardCol.implicitWidth
                                      + AppTheme.spacingM + AppTheme.spacingS)
                           : fullW
            // Gate/loading/failed keep a monotonic reserved height so a
            // failure never reflows the row under the reader; only the
            // loaded preview re-measures. The latch is per-event: pooled
            // delegate reuse for another row must not inherit the previous
            // event's minimum.
            //
            // The gate is now SHORTER than the loading skeletons, so the
            // consent click grows the row by one step. That is deliberate
            // and it is the user's own click: the alternative is reserving
            // a loading-sized box under every unloaded link in the room,
            // which is the cost readers complained about.
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
                //
                // 2026-08-26: this used to be a STACK — a host row, a
                // wrapped two-line amber sentence, then a full-width
                // button — roughly four message lines of timeline spent on
                // one link nobody had agreed to load, and that is what the
                // reader report was about. What the gate has to STATE is
                // unchanged, because it is the whole reason the control
                // exists (link previews default OFF, and an encrypted room
                // is stricter still): the linked site is contacted
                // DIRECTLY, and it therefore learns your IP. Both facts are
                // still in the row; the long sentence is still readable
                // verbatim, as the row's tooltip.
                //
                // The BUTTON is still the consent. The row is deliberately
                // not clickable and the card's whole-card MouseArea stays
                // gated on "loaded" — hovering to read the privacy sentence
                // must never be able to agree to the fetch.
                RowLayout {
                    id: consentRow
                    objectName: "linkPreviewConsentRow"
                    visible: card.st === "requires_action"
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing6

                    // The ATTACHED tooltip form: one shared instance for the
                    // whole application. A declared ToolTip child would
                    // build a Popup, a background and a Label per timeline
                    // row carrying a link (the reaction chips above carry
                    // the same note and the same reason).
                    readonly property string fullPrivacyText:
                        qsTr("Loading this preview contacts the linked website directly and may reveal your IP address.")
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
                            // Short, but it still names BOTH facts: the
                            // request goes to the site itself, and the site
                            // sees your address. It WRAPS rather than
                            // elides — an elided privacy notice is a notice
                            // the reader may never reach the end of, and on
                            // a narrow bubble the tail is the half that
                            // matters.
                            text: root.roomEncrypted
                                  ? qsTr("Contacts the site directly — it sees your IP")
                                  : qsTr("Loads directly from the site, which sees your IP")
                            color: root.roomEncrypted ? AppTheme.warning
                                                      : AppTheme.textMuted
                            font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                            wrapMode: Text.WordWrap
                            // EXPLICIT, not left to the Label default: the
                            // comment above claims this notice is never
                            // truncated, and a claim a style could quietly
                            // override is not a guarantee. Pinned by
                            // LinkPreviewQmlTest.
                            elide: Label.ElideNone
                            Layout.fillWidth: true
                        }
                    }

                    AppButton {
                        objectName: "linkPreviewLoadButton"
                        // "Show preview" as a full-height button was a
                        // third row of its own. The label shortens; what it
                        // DOES is unchanged, and the accessible name keeps
                        // the long form for anyone reading the row without
                        // seeing it.
                        text: qsTr("Show")
                        size: "sm"
                        minWidth: 0
                        Accessible.name: qsTr("Show link preview")
                        Layout.alignment: Qt.AlignVCenter
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
                            // Re-resolve through a counter, never by assigning
                            // `source`: an imperative write destroys the
                            // binding, so a card whose preview later changes
                            // would keep painting the first image it loaded.
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
                        // A wrapping paragraph inside a card needs the same
                        // leading rule as the message body it sits under.
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
                // A hidden animation must actually STOP. Leaving an
                // AnimatedImage playing behind an opaque placeholder burns a
                // decode per frame for something nobody can see.
                && !root.mediaHidden

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
                // Hiding must never START a fetch. It is a rendering
                // preference, so a hidden row asks the network for nothing;
                // bytes already cached stay cached, and revealing takes the
                // ordinary cache path.
                if (root.mediaHidden) return
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
            // Revealing takes the ordinary path: whatever is cached is used,
            // and a row hidden before its bytes ever arrived fetches now.
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
                visible: !root.mediaHidden
                         && img.status !== Image.Ready
                         && animatedImg.status !== AnimatedImage.Ready
                active: root.rowOnScreen && !imageBox.bridgeFailed
                        && img.status !== Image.Error
            }
            // GIFs announce themselves on the placeholder too, so the
            // reserved box reads as "an animation is coming".
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
                // Cleared while hidden rather than merely made invisible: an
                // Image with a source still holds the decoded pixmap, and the
                // point of hiding is that it is not painted or decoded.
                source: (imageBox.animateGif || root.mediaHidden)
                        ? "" : imageBox.roundedSource
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

            MediaHiddenPlaceholder {
                anchors.fill: parent
                hidden: root.mediaHidden
                onRevealRequested: root.setMediaHidden(false)
            }

            MouseArea {
                anchors.fill: parent
                enabled: !root.mediaHidden
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

            // v0.6.6 review (L5): only a GIF ever needs the hover star — a
            // Loader keeps every plain (non-GIF) image row from paying for a
            // HoverHandler + Item + Icon + two Rectangles it will never use.
            // `active` re-evaluates starEligible live, so a row that only
            // later confirms itself as a GIF (see onStarEligibleChanged
            // above) still gets one created.
            Loader {
                id: gifStarLoader
                anchors.fill: parent
                active: imageBox.starEligible && !root.mediaHidden
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
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                visible: !root.mediaHidden
                         && (img.status === Image.Error
                             || imageBox.bridgeFailed)
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
                // Hiding never starts a fetch; see the note in
                // imageComponent's own refreshBridgeSource.
                if (root.mediaHidden) return
                if (bridgeFailed)
                    app.mediaBridge.retry(bridgeCacheKey)
                bridgeFailed = false
                bridgeSource = app.mediaBridge.mediaSource(
                    model.mediaKey,
                    model.mediaThumbAvailable ? "thumb" : "full")
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
                active: root.rowOnScreen && !stickerBox.bridgeFailed
                        && stickerImg.status !== Image.Error
            }
            Image {
                id: stickerImg
                anchors.fill: parent
                visible: !root.mediaHidden
                fillMode: Image.PreserveAspectFit
                source: root.mediaHidden ? "" : stickerBox.resolvedSource
                sourceSize.width: 360
                asynchronous: true
                cache: true
            }
            MediaHiddenPlaceholder {
                anchors.fill: parent
                hidden: root.mediaHidden
                onRevealRequested: root.setMediaHidden(false)
            }
            HoverHandler { id: stickerHover; enabled: !root.mediaHidden }
            ToolTip.text: model.body || ""
            ToolTip.visible: stickerHover.hovered && (model.body || "").length > 0
            ToolTip.delay: 400
            TapHandler {
                enabled: !root.mediaHidden
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
                    // Thumbnails are never gated: small, and they are what
                    // the reader is looking at while scrolling.
                    bridgeSource = app.mediaBridge.mediaSource(model.mediaKey,
                                                               "thumb")
                } else if (root.speculativeMediaAllowed) {
                    // videoPosterSource MATERIALIZES the payload to extract a
                    // frame (MediaBridge::videoPosterSource -> prefetchPlayable),
                    // so it is speculative work too, not a cheap read.
                    bridgeSource = app.mediaBridge.videoPosterSource(
                        model.mediaKey, prefetchSize)
                } else {
                    // Off-screen rows, and rows sweeping past mid-gesture,
                    // must not trigger poster/prefetch work; the observers
                    // below re-run this once the view settles.
                    bridgeSource = ""
                }
                // Bounded speculative payload prefetch so pressing Play is
                // (usually) instant instead of a multi-second download.
                // MediaBridge enforces the size cap and deduplication.
                if (root.speculativeMediaAllowed && playbackAvailable
                    && prefetchSize > 0)
                    app.mediaBridge.prefetchPlayable(model.mediaKey,
                                                     prefetchSize)
            }
            // Re-run when the row appears OR when the view settles — a row
            // that swept past mid-gesture deliberately took the empty
            // branch above and needs the retry once spending is allowed.
            readonly property bool coverOnScreen: root.speculativeMediaAllowed
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
            // The card's speculative prefetch waits for a settle for the
            // same reason the video path does (see TimelinePane.qml's
            // `speculativeMediaAllowed`): a row swept past is not worth a
            // download. Its playback/reclamation logic keeps rowOnScreen.
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
            implicitWidth: Math.min(340, bubble.width)
            implicitHeight: fileRow.implicitHeight + 16
            color: AppTheme.embedSurface
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
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        font.weight: AppTheme.weightStrong
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
                        font.pixelSize: AppTheme.scaled(AppTheme.textMicro)
                        elide: Label.ElideRight
                        Layout.fillWidth: true
                    }
                }

                // Download / save state action. MediaBridge saves are
                // atomic (no progress or cancel API) — the in-flight state
                // is honest-indeterminate, never a fake percentage.
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
