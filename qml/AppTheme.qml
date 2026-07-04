pragma Singleton
import QtQuick

QtObject {
    id: root

    // 0 = System, 1 = Light, 2 = Dark. Kept in sync with SettingsManager::Theme
    // by an external Binding in Main.qml — singletons don't reliably see
    // root context properties, so we push the value in rather than pull.
    property int mode: 0

    // v0.1: System theme falls back to light. Real system-scheme follow
    // (via QStyleHints::colorScheme) arrives in v0.3.
    readonly property bool dark: mode === 2

    readonly property color background:      dark ? "#1b1d22" : "#f5f6f8"
    readonly property color surface:         dark ? "#24272d" : "#ffffff"
    readonly property color surfaceAlt:      dark ? "#2c3038" : "#eef0f4"
    readonly property color border:          dark ? "#3a3f48" : "#d6dae1"
    readonly property color text:            dark ? "#e6e8ee" : "#1a1d22"
    readonly property color textMuted:       dark ? "#a1a7b3" : "#5a6070"
    readonly property color accent:          "#4e7cff"
    readonly property color accentText:      "#ffffff"
    readonly property color ownBubble:       "#4e7cff"
    readonly property color ownBubbleText:   "#ffffff"
    readonly property color otherBubble:     dark ? "#333842" : "#e5e8ee"
    readonly property color otherBubbleText: dark ? "#e6e8ee" : "#1a1d22"
    readonly property color error:           "#e5484d"
    readonly property color success:         "#30a46c"

    readonly property int   spacingXS: 4
    readonly property int   spacingS:  8
    readonly property int   spacingM:  12
    readonly property int   spacingL:  16
    readonly property int   spacingXL: 24
    readonly property int   radius:    8
}
