pragma Singleton
import QtQuick

// Who shows a download's notice. A download is shown in exactly one place:
// the notice of the on-screen file-card row that claims its media key, else
// the pane-level fallback card (PaneDownloadsCard). Never both, never neither.
//
// A row's notice CLAIMS a key while it is eligible (on screen, expanded, a
// file card with enough empty space beside it, the item is its own primary
// media, not a file type that can run programs); it shows that key's newest
// download. The same event can be on
// screen in the main timeline AND the thread panel or context view, so the
// claims are arbitrated: the surface the click came from (`begin`) wins, else
// main, thread, context in that order. `revision` moves on every change so
// bindings re-evaluate; nothing here reads it from a handler it writes.
QtObject {
    id: registry

    // Moved on every change to claims or origins; bindings read it.
    property int revision: 0
    property var origin: ({})   // mediaKey -> surface the click came from
    // Download ids that have been shown beside their file card. Such a
    // download keeps the card's rule wherever it is shown later: it stays
    // until opened or dismissed, so the pane card never times it out.
    property var shownBeside: ({})
    property var claims: ({})   // mediaKey -> [{token, surface}]
    property var _tick: ({ n: 0 })

    function _bump() { registry._tick.n += 1; registry.revision = registry._tick.n }

    // The surface a download was started from; "" when it has none (viewer,
    // media history), which lets any claiming surface show it.
    function begin(key, surface) {
        if (!key) return
        var o = registry.origin
        o[key] = surface || ""
        _bump()
    }

    function setClaim(key, token, surface, on) {
        if (!key) return
        var list = registry.claims[key] || []
        var kept = list.filter(function(c) { return c.token !== token })
        if (on) kept.push({ token: token, surface: surface })
        if (kept.length > 0) registry.claims[key] = kept
        else delete registry.claims[key]
        _bump()
    }

    // No revision bump: it is called from a notice's own change handler, and
    // a bump there re-enters the `owned` binding that fired it (a binding
    // loop). None is needed either: the record matters only once the item is
    // on the pane card again, and that takes a claim release, which bumps.
    function markShownBeside(id) {
        if (id === undefined || id === null)
            return
        registry.shownBeside[id] = true
    }
    function wasShownBeside(id) {
        var rev = registry.revision
        return registry.shownBeside[id] === true
    }

    function _rank(surface) {
        return surface === "main" ? 0 : surface === "thread" ? 1
             : surface === "context" ? 2 : 3
    }

    // The claiming token that shows `key`, or "" when no row does.
    function owner(key) {
        var rev = registry.revision
        var list = registry.claims[key]
        if (!list || list.length === 0) return ""
        var want = registry.origin[key] || ""
        var best = null
        for (var i = 0; i < list.length; ++i) {
            var c = list[i]
            if (want !== "" && c.surface === want) return c.token
            if (best === null || _rank(c.surface) < _rank(best.surface))
                best = c
        }
        return best.token
    }

    // The items no row shows: the fallback card's content. A row's notice
    // shows only the NEWEST download of its key (AttachmentDownloadNotice
    // .item: the last of that key in app.downloads order), so an older save
    // of the same file stays here.
    function unclaimed(items) {
        var rev = registry.revision
        var list = items || []
        var newest = {}
        for (var i = 0; i < list.length; ++i)
            newest[list[i].mediaKey] = list[i].id
        var out = []
        for (var j = 0; j < list.length; ++j) {
            var it = list[j]
            if (owner(it.mediaKey) === "" || newest[it.mediaKey] !== it.id)
                out.push(it)
        }
        return out
    }

    // Account switch / sign-out: nothing from one account is carried to the
    // next. Claims are rebuilt by the rows themselves.
    function reset() {
        registry.origin = ({})
        registry.shownBeside = ({})
        _bump()
    }
}
