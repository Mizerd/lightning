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
    // v0.5.0-prep+12: semantic colors used by the polished UI.
    readonly property color warning:         "#e5a23c"
    // Destructive-action button (reset local session, logout, etc).
    readonly property color danger:          "#c53030"
    readonly property color dangerText:      "#ffffff"
    // Subtle "encrypted / undecryptable" tone: less alarming than
    // error red, distinguishable from normal text.
    readonly property color muted:           dark ? "#7c8291" : "#7a808b"
    // Selected room tinted background — lighter than accent so the
    // sidebar doesn't shout when many rooms are visible.
    readonly property color selectedBg:      dark ? "#2f394f" : "#dbe4ff"

    readonly property int   spacingXS: 4
    readonly property int   spacingS:  8
    readonly property int   spacingM:  12
    readonly property int   spacingL:  16
    readonly property int   spacingXL: 24
    readonly property int   radius:    8
    // v0.5.0-prep+12: additional radii + font-size scale so QML
    // stops sprinkling magic numbers.
    readonly property int   radiusSm:  4
    readonly property int   radiusPill: 999
    readonly property int   fontSizeXS: 11
    readonly property int   fontSizeS:  12
    readonly property int   fontSizeM:  13
    readonly property int   fontSizeL:  15
    readonly property int   fontSizeXL: 18
}
