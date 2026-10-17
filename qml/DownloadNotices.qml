pragma Singleton
import QtQuick

// Who shows a download's notice. A download is shown in exactly one place:
// the notice of the on-screen file-card row that claims its media key, else
// the pane-level fallback card (PaneDownloadsCard). Never both, never neither.
//
// A row's notice CLAIMS a key while it is eligible (on screen, expanded, a
// file card with enough empty space beside it, the item is its own primary
// media). The same event can be on
// screen in the main timeline AND the thread panel or context view, so the
// claims are arbitrated: the surface the click came from (`begin`) wins, else
// main, thread, context in that order. `revision` moves on every change so
// bindings re-evaluate; nothing here reads it from a handler it writes.
QtObject {
    id: registry

    // Moved on every change to claims or origins; bindings read it.
    property int revision: 0
    property var origin: ({})   // mediaKey -> surface the click came from
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

    // The items no row shows: the fallback card's content.
    function unclaimed(items) {
        var rev = registry.revision
        var out = []
        for (var i = 0; i < (items || []).length; ++i) {
            if (owner(items[i].mediaKey) === "") out.push(items[i])
        }
        return out
    }

    // Account switch / sign-out: nothing from one account is carried to the
    // next. Claims are rebuilt by the rows themselves.
    function reset() {
        registry.origin = ({})
        _bump()
    }
}
