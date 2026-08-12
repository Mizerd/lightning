import QtQuick
import QtQuick.Controls
import MatrixClient

// v0.5.11: shared avatar element used across the whole UI (room list, Space
// rail, room header, Room Information and invite results). A Matrix Space is
// a room, so it uses the same account-scoped room-avatar mechanism.
//
// It resolves the mxc URI through the shared MediaBridge (dedup, bounded
// cache, account-separated, cleared on sign-out) and shows a stable initial
// placeholder until — and unless — a real bitmap is available. The bitmap is
// only shown once fully loaded, so a broken-image icon never appears and a
// stale avatar never flashes when the mxc changes (delegate reuse, account
// switch, logout, avatar-URL change). The avatar shape (circle for people,
// rounded square for rooms/Spaces) is baked into the decoded bitmap by
// MediaImageProvider via the "|shape:" source suffix — once per cached
// image — instead of a per-item MultiEffect mask, which cost two extra
// render passes per avatar on every scroll frame.
Rectangle {
    id: root

    // mxc:// avatar URI (empty → initials only).
    property string mxc: ""
    // Name used for the initial placeholder.
    property string name: ""
    property int size: 40
    property bool circle: true
    // Rounded-square corner radius for room/Space avatars (design: 8 in the
    // room list, 9 in the room header, 12 on the rail, 14 on the room card).
    property int squareRadius: AppTheme.radiusMd
    // Rooms render a "#" glyph instead of initials (design handoff).
    property bool roomGlyph: false
    // Stable identity key for the fallback colour (roomId/userId); the
    // display name is used when no key is given so renames keep a colour
    // only as long as the name is stable.
    property string colorKey: ""
    // Explicit initials font size; 0 derives from the avatar size.
    property int labelSize: 0
    // Perf: rows outside the viewport set this false so their loading
    // skeletons stop animating — every other Skeleton in the app gates on
    // rowOnScreen, and hundreds of off-screen infinite animations kept the
    // scene graph permanently dirty during room open. Default true keeps
    // non-timeline surfaces (room list, popovers, rail) unchanged.
    property bool onScreen: true

    implicitWidth: size
    implicitHeight: size
    width: size
    height: size
    radius: circle ? size / 2 : squareRadius

    // v0.7: explicit avatar result states.
    //   missing — no avatar to load: deterministic initials fallback.
    //   loading — bytes/decode in flight: circular/rounded skeleton (no
    //             initials flash, no random colour flash).
    //   ready   — ONLY the decoded bitmap. The fallback fill must never
    //             remain beneath a successfully loaded avatar: transparent
    //             pixels reveal the surrounding surface, not the palette
    //             colour (the shape mask is baked into the bitmap with its
    //             alpha preserved).
    //   failed  — fetch/decode failed: initials fallback (a later cache
    //             completion still promotes to ready).
    property bool fetchFailed: false
    readonly property string presentationState:
        !hasImage ? "missing"
        : img.status === Image.Ready ? "ready"
        : (fetchFailed || img.status === Image.Error) ? "failed"
        : "loading"

    // Deterministic per-identity colour from the shared handoff palette;
    // neutral surface only when there is nothing to derive a colour from.
    readonly property string _paletteKey: colorKey.length > 0 ? colorKey : name
    function _paletteColor(key) {
        var h = 0
        for (var i = 0; i < key.length; ++i)
            h = ((h << 5) - h + key.charCodeAt(i)) | 0
        var palette = AppTheme.avatarPalette
        return palette[Math.abs(h) % palette.length]
    }
    color: presentationState === "missing" || presentationState === "failed"
           ? (_paletteKey.length > 0 ? _paletteColor(_paletteKey)
                                     : AppTheme.cardElevated)
           : "transparent"

    // Up to two initials: first letters of the first two words, with
    // Matrix sigils stripped ("@user:hs" → "U").
    function _initials(value) {
        var cleaned = value.replace(/^[@#!+]/, "").trim()
        if (cleaned.length === 0)
            return "?"
        var words = cleaned.split(/\s+/).filter(function (w) { return w.length > 0 })
        if (words.length >= 2)
            return (words[0].charAt(0) + words[1].charAt(0)).toUpperCase()
        return cleaned.charAt(0).toUpperCase()
    }

    // The media bridge this avatar loads through. Normally the shared
    // app.mediaBridge — but resolved DEFENSIVELY: when a Repeater
    // instantiates a delegate synchronously from inside a property-change
    // handler (the read-receipt chips: `shown` is reassigned while the row
    // is still being built, or from a receipts dataChanged), the FIRST
    // unqualified `app` context lookup performed during that nested
    // creation resolves to undefined; every LATER lookup in the same
    // object resolves correctly (observed by direct probing — the
    // Qt-internal cause is not established, so this is a behavioral
    // characterization, not a mechanism claim). A binding that THROWS on
    // that first lookup (the 2026-08 live bug: "TypeError: Cannot read
    // property 'mediaBridge' of undefined") sticks at its last value
    // because none of its captured dependencies (only the per-chip
    // constant `mxc`) ever changes for a chip — the avatar never fetched,
    // initials only. So: `typeof`-guard the binding (never throws), and
    // re-resolve via resolveBridge() at completion and on every refresh().
    // KEEP `bridge` THE FIRST `app` REFERENCE IN THIS FILE: it is the
    // canary that absorbs the one poisoned lookup; move another `app`
    // binding above it and THAT binding takes the hit instead. A consumer
    // can also inject its own bridge explicitly; resolveBridge() never
    // overwrites a non-null value.
    property var bridge: (typeof app !== "undefined" && app)
                         ? app.mediaBridge : null
    // Idempotent recovery: safe to call anywhere, no-op once resolved
    // (and app.mediaBridge is a CONSTANT property, so replacing the
    // original binding loses nothing).
    function resolveBridge() {
        if (!bridge && typeof app !== "undefined" && app && app.mediaBridge)
            bridge = app.mediaBridge
    }
    readonly property bool hasImage:
        mxc.length > 0 && bridge !== null && bridge !== undefined
        && bridge.supported
    property string src: ""

    function refresh() {
        // Recovery point: if BOTH the creation-time lookup and completion
        // missed (never observed, but unbounded by inspection), any later
        // poke — an mxc change, hasImage flip, size change — self-heals
        // instead of leaving a permanently initials-only avatar.
        resolveBridge()
        if (!hasImage) {
            src = ""
            return
        }
        // The bridge fetches every avatar at one canonical server-side
        // edge regardless of the render size (a single request, cache
        // entry, and failure mark per identity); the Image scales down.
        var s = bridge.avatarSource(mxc, size)
        src = s
        // "" is either "fetch in flight" (loading) or "suppressed by a
        // failure mark". The mark case has NO later signal of its own, so
        // ask synchronously and show honest initials instead of an
        // eternal skeleton; mediaRetryable promotes it back once the
        // transient window expires.
        if (s === "" && bridge.avatarFailureCategory(mxc) !== "")
            fetchFailed = true
    }
    onMxcChanged: {
        // A new identity is a new load attempt; the old failure (and any
        // stale bitmap via the source change below) must not leak across
        // delegate reuse.
        fetchFailed = false
        refresh()
    }
    onSizeChanged: refresh()
    // Covers bridge.supported flipping after a late setClient (session
    // restore/account switch): the avatar re-resolves the moment the
    // bridge becomes usable instead of staying empty until an unrelated
    // poke.
    onHasImageChanged: refresh()
    Component.onCompleted: {
        // The completion half of the defensive bridge resolution (see the
        // `bridge` property): by completion the scope was observed wired
        // in every exercised creation path. refresh() calls
        // resolveBridge() itself, so this is one call, not two.
        refresh()
    }

    Connections {
        target: root.bridge
        enabled: root.hasImage
        function onMediaCached(cacheKey) {
            // Cache keys end with the mxc URI ("mxc:<edge>:<uri>") — only
            // this avatar's own completion needs a refresh, not every
            // media byte fetched anywhere in the app. Skip it once already
            // resolved: every continuation row of a sender shares one mxc, so
            // an un-guarded fan-out re-resolves every already-shown same-sender
            // avatar on each completion — the repeated avatar cache=hit churn
            // seen while paginating a dense conversation. onMxcChanged /
            // Component.onCompleted still resolve unconditionally (new identity
            // / new instance); a failed avatar (fetchFailed) still retries.
            if (cacheKey.endsWith(":" + root.mxc)
                && (root.src === "" || root.fetchFailed))
                root.refresh()
        }
        function onMediaFetchFailed(cacheKey, category) {
            if (cacheKey.endsWith(":" + root.mxc))
                root.fetchFailed = true
        }
        function onMediaRetryable(cacheKey) {
            // An expired transient failure mark: re-resolve exactly like a
            // cache completion (bounded — the bridge re-arms the mark on a
            // failed attempt, so this cannot hammer the backend).
            if (cacheKey.endsWith(":" + root.mxc))
                root.refresh()
        }
    }

    // Loading: quiet shape-matched skeleton — never a random palette flash
    // that a decoded bitmap then replaces.
    Skeleton {
        objectName: "avatarSkeleton"
        anchors.fill: parent
        visible: root.presentationState === "loading"
        active: root.onScreen
        circle: root.circle
        radius: root.circle ? Math.min(width, height) / 2 : root.squareRadius
    }

    Label {
        objectName: "avatarInitials"
        anchors.centerIn: parent
        visible: root.presentationState === "missing"
                 || root.presentationState === "failed"
        text: root.roomGlyph ? "#" : root._initials(root.name)
        // White 800-weight glyph on the palette colour per the handoff;
        // neutral ink only on the colourless fallback surface.
        color: root._paletteKey.length > 0 ? "#FFFFFF" : AppTheme.textSecondary
        font.pixelSize: root.labelSize > 0
                        ? root.labelSize
                        : Math.max(10, Math.round(root.size
                              * (text.length > 1 ? 0.36 : 0.43)))
        font.weight: Font.ExtraBold
    }

    Image {
        id: img
        objectName: "avatarImage"
        anchors.fill: parent
        // The provider bakes the mask into the bitmap; the suffix selects
        // circle or rounded-square (radius as a permille of the edge).
        source: root.src === "" ? ""
                : root.src + (root.circle
                    ? "|shape:circle"
                    : "|shape:rsq:" + Math.max(1, Math.min(500,
                          Math.round(root.radius * 1000 / Math.max(1, root.size)))))
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        cache: true
        // Only shown once fully decoded — no broken-image glyph, no flash.
        visible: img.status === Image.Ready
        // Self-heal the cache-hit-then-evicted race: avatarSource() saw the
        // bytes, but the provider read after LRU eviction and returned
        // empty → Error. refresh() re-dispatches; the re-cached bytes get a
        // NEW revision URL, so this Image actually reloads. Bounded: a
        // cache hit returns the identical string (no reload, no loop).
        onStatusChanged: {
            if (status === Image.Error)
                root.refresh()
        }
    }
}
