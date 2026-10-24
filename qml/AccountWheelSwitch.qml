import QtQuick

// Mouse wheel over the account tile steps through the signed-in accounts the
// way a classic client's account strip does: wheel up = the previous account,
// wheel down = the next, wrapping at both ends (GitHub #54). Sits inside a tile
// and fills it; it takes no taps and draws nothing.
//
// One switch per notch: a mouse notch is 120 units and acts at once, a touchpad
// streams small deltas that accumulate to one notch before acting, and after
// each switch further input is ignored for `lockMs` so a fling cannot walk
// the whole list. Nothing happens while a switch is already in progress
// (`busy`), and accounts that still need a sign-in are skipped because
// landing on one would open a login prompt from a scroll.
//
// Pure: it never touches `app`. The owner passes the account list and active id
// and performs the switch from switchRequested().
Item {
    id: root
    anchors.fill: parent

    // [{ userId, isActive, needsSignIn, sessionRevoked }, ...] in storage order.
    property var accounts: []
    property string activeUserId: ""
    // A switch is already running (AppController.accountSwitching).
    property bool busy: false
    property int lockMs: 250
    property int notch: 120

    signal switchRequested(string userId)

    // The account one step from the active one, or "" when there is nowhere to
    // go. `direction` is -1 (previous) or +1 (next).
    function targetFor(direction) {
        const list = (root.accounts || []).filter(a =>
            a && a.userId
            && a.needsSignIn !== true && a.sessionRevoked !== true)
        if (list.length < 2)
            return ""
        let at = -1
        for (let i = 0; i < list.length; ++i) {
            if (list[i].userId === root.activeUserId) { at = i; break }
        }
        if (at < 0)
            return list[0].userId
        return list[(at + direction + list.length) % list.length].userId
    }

    function _accept(deltaY) {
        if (root.busy || lock.running || deltaY === 0)
            return
        accum += deltaY
        idle.restart()
        if (Math.abs(accum) < root.notch)
            return
        // Wheel up (positive delta) is the previous account.
        const target = root.targetFor(accum > 0 ? -1 : 1)
        accum = 0
        if (target === "")
            return
        lock.restart()
        root.switchRequested(target)
    }

    property real accum: 0
    // A touchpad that stops mid-notch does not leave a half notch behind.
    Timer { id: idle; interval: 400; onTriggered: root.accum = 0 }
    Timer { id: lock; interval: root.lockMs }

    WheelHandler {
        objectName: "accountWheelHandler"
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: (event) => root._accept(event.angleDelta.y)
    }
}
