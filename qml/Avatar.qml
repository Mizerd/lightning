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

    readonly property bool hasImage:
        mxc.length > 0 && app.mediaBridge && app.mediaBridge.supported
    property string src: ""

    function refresh() {
        src = hasImage ? app.mediaBridge.avatarSource(mxc, Math.max(32, size * 2))
                       : ""
    }
    onMxcChanged: {
        // A new identity is a new load attempt; the old failure (and any
        // stale bitmap via the source change below) must not leak across
        // delegate reuse.
        fetchFailed = false
        refresh()
    }
    onSizeChanged: refresh()
    Component.onCompleted: refresh()

    Connections {
        target: app.mediaBridge
        enabled: root.hasImage
        function onMediaCached(cacheKey) {
            // Cache keys end with the mxc URI ("mxc:<edge>:<uri>") — only
            // this avatar's own completion needs a refresh, not every
            // media byte fetched anywhere in the app.
            if (cacheKey.endsWith(":" + root.mxc))
                root.refresh()
        }
        function onMediaFetchFailed(cacheKey, category) {
            if (cacheKey.endsWith(":" + root.mxc))
                root.fetchFailed = true
        }
    }

    // Loading: quiet shape-matched skeleton — never a random palette flash
    // that a decoded bitmap then replaces.
    Skeleton {
        objectName: "avatarSkeleton"
        anchors.fill: parent
        visible: root.presentationState === "loading"
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
    }
}
