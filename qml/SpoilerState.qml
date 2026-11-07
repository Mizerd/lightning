pragma Singleton
import QtQuick

// Spoilers the reader revealed this session, by event id.
QtObject {
    property var revealed: ({})
    property int revision: 0

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
