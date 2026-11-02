import QtQuick

Item {
    id: root
    anchors.fill: parent

    property var accounts: []
    property string activeUserId: ""
    property bool busy: false
    property int lockMs: 250
    property int notch: 120

    signal switchRequested(string userId)

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
    Timer { id: idle; interval: 400; onTriggered: root.accum = 0 }
    Timer { id: lock; interval: root.lockMs }

    WheelHandler {
        objectName: "accountWheelHandler"
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: (event) => root._accept(event.angleDelta.y)
    }
}
