import QtQuick
import MatrixClient

// Per-account activity for an account surface (Labs, concurrent accounts).
//
// AccountActivityModel answers per account through `activityFor(uid)`, a
// Q_INVOKABLE, and an invokable read inside a binding is not bound to anything
// (CLAUDE.md §16). So this copies the answers into `activity`, an UNBOUND map
// written only by the handlers below — on completion, when the surface's
// account list changes, and on the model's own per-account signals — and the
// surface's bindings read the map. A badge update is one account's entry and
// never rebuilds the surface's rows.
//
// `source` is `app.accountActivity`, which is null while Labs is off: then
// nothing is read, `activity` stays null, and every badge reads its defaults.
QtObject {
    id: feed

    // An AccountActivityModel, or null.
    property var source: null
    // The accounts the surface shows. Bound by the surface; with Labs off it
    // binds an empty list so no account list is read for this.
    property var userIds: []

    // uid -> {mentionCount, hasUnread, mentionsLowerBound, invites, liveState}.
    // Null while there is no source. Never bound; see above.
    property var activity: null

    // The answer for one account, defaults filled in. Reads only `activity`,
    // so a binding calling it follows the map.
    function entryFor(uid) {
        const a = feed.activity && uid ? feed.activity[uid] : undefined
        return {
            mentionCount: a && a.mentionCount > 0 ? a.mentionCount : 0,
            hasUnread: !!(a && a.hasUnread === true),
            mentionsLowerBound: !!(a && a.mentionsLowerBound === true),
            liveState: a && a.liveState !== undefined ? a.liveState : 0
        }
    }

    // True when two AccountListModel snapshots show the same accounts the
    // same way, ignoring what a host starting, running or pausing changes:
    // `liveState`, the `section` it moves the row into, `lastLiveAt`, and so
    // the row ORDER. An open surface keeps its rows across such a change (its
    // badges come from `activity`, not the rows), so delegates, scroll,
    // hover and the keyboard position survive; the surface applies the new
    // order when it closes. Pure: reads only its arguments. A value that is
    // not a primitive compares unequal, which only means "rebuild".
    function sameAccountsIgnoringLiveState(a, b) {
        if (!a || !b)
            return a === b
        if (a.length !== b.length)
            return false
        const ignored = { liveState: true, section: true, lastLiveAt: true }
        const byId = {}
        for (let i = 0; i < a.length; ++i)
            byId[a[i].userId || ""] = a[i]
        for (let j = 0; j < b.length; ++j) {
            const y = b[j]
            const x = byId[y.userId || ""]
            if (!x)
                return false
            const keys = {}
            for (const k in x) keys[k] = true
            for (const k in y) keys[k] = true
            for (const k in keys) {
                if (!ignored[k] && x[k] !== y[k])
                    return false
            }
        }
        return true
    }

    function _read(uid) {
        return feed.source && uid ? feed.source.activityFor(uid) : undefined
    }

    // One account's entry, copied into a fresh map so every binding reading
    // `activity` re-evaluates.
    function refresh(uid) {
        if (!feed.source || !uid)
            return
        const next = {}
        const current = feed.activity || {}
        for (const k in current)
            next[k] = current[k]
        next[uid] = feed._read(uid)
        feed.activity = next
    }

    function refreshAll() {
        if (!feed.source) {
            if (feed.activity !== null)
                feed.activity = null
            return
        }
        const next = {}
        const ids = feed.userIds || []
        for (let i = 0; i < ids.length; ++i) {
            if (ids[i])
                next[ids[i]] = feed._read(ids[i])
        }
        feed.activity = next
    }

    // Deferred: a change handler on a bound property can run inside another
    // binding's evaluation (the HomePane binding loop, §16), so the write
    // happens on the next turn of the event loop instead.
    onSourceChanged: Qt.callLater(feed.refreshAll)
    onUserIdsChanged: Qt.callLater(feed.refreshAll)
    Component.onCompleted: feed.refreshAll()

    readonly property Connections _watch: Connections {
        target: feed.source
        function onSummaryChanged(userId) { feed.refresh(userId) }
        function onLiveStateChanged(userId) { feed.refresh(userId) }
    }
}
