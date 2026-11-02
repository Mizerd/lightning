.pragma library

function editorHeight(expanded, compactHeight, tallHeight) {
    return expanded ? tallHeight : compactHeight
}

function returnShouldSend(modifiers, enterInsertsNewline, control, shift) {
    return enterInsertsNewline ? (modifiers & control) !== 0
                              : (modifiers & shift) === 0
}
