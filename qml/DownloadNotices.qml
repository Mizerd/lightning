pragma Singleton
import QtQuick

QtObject {
    id: registry

    property int revision: 0
    property var origin: ({})
    property var shownBeside: ({})
    property var claims: ({})
    property var _tick: ({ n: 0 })

    function _bump() { registry._tick.n += 1; registry.revision = registry._tick.n }

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

    function reset() {
        registry.origin = ({})
        registry.shownBeside = ({})
        _bump()
    }
}
