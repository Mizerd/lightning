.pragma library

// Shared presentation policy for the room, inline-thread, side-thread and edit
// editors. Text and destination remain owned by their existing controllers.
function editorHeight(expanded, compactHeight, tallHeight) {
    return expanded ? tallHeight : compactHeight
}

function returnShouldSend(modifiers, enterInsertsNewline, control, shift) {
    return enterInsertsNewline ? (modifiers & control) !== 0
                              : (modifiers & shift) === 0
}
