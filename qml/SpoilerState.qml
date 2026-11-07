pragma Singleton
import QtQuick

// Spoilers the reader revealed this session, by event id. Forgotten when the
// active account changes, sign-out included.
QtObject {
    property var revealed: ({})
    property int revision: 0
    readonly property string owner:
        (typeof app !== "undefined" && app && app.accounts)
        ? app.accounts.activeUserId : ""
    onOwnerChanged: {
        revealed = ({})
        revision++
    }

    function isRevealed(key) {
        return !!key && revealed[key] === true
    }
    function reveal(key) {
        if (!key || revealed[key] === true)
            return
        revealed[key] = true
        revision++
    }
}
