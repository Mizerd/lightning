import QtQuick
import QtQuick.Controls
import MatrixClient

// Security, application-wide: Qt Quick Controls shares one ToolTip instance
// for every attached ToolTip.text, and the Basic style's Text uses
// AutoText, which promotes markup-looking strings to StyledText. Several
// tooltips show remote display names, so an <img> in a name would make
// every hovering viewer fetch that URL. Forcing plain text on the shared
// instance fixes it once for the whole app. On an Item, not the window:
// ToolTip attaches to Item, and attaching to the ApplicationWindow warns.
//
// Main.qml instantiates it once; ToolTipPlainTextQmlTest proves a markup room
// name in an attached tooltip is shown as text and fetches nothing. An
// explicit ToolTip { } is NOT covered (it has its own contentItem): use
// PlainToolTip for one that shows text someone else chose.
Item {
    id: sharedToolTipGuard
    objectName: "sharedToolTipPlainTextGuard"
    visible: false

    function applyToolTipChrome() {
        const shared = ToolTip.toolTip
        if (!shared)
            return
        const bg = shared.background
        if (!bg)
            return
        if (bg.radius !== undefined)
            bg.radius = AppTheme.radiusMd
        if (bg.border !== undefined)
            bg.border.color = AppTheme.border
    }

    Component.onCompleted: {
        const shared = ToolTip.toolTip
        if (shared && shared.contentItem
                && shared.contentItem.textFormat !== undefined)
            shared.contentItem.textFormat = Text.PlainText
        if (shared && shared.contentItem
                && shared.contentItem.wrapMode !== undefined)
            shared.contentItem.wrapMode = Text.Wrap
        applyToolTipChrome()
    }

    function capToolTipWidth() {
        const shared = sharedToolTipGuard.ToolTip.toolTip
        if (!shared)
            return
        shared.width = Math.min(shared.implicitWidth, AppTheme.scaled(420))
    }
    Connections {
        target: sharedToolTipGuard.ToolTip.toolTip
        ignoreUnknownSignals: true
        function onAboutToShow() { sharedToolTipGuard.capToolTipWidth() }
        function onOpened() { sharedToolTipGuard.capToolTipWidth() }
    }

    Connections {
        target: AppTheme
        function onEffectiveThemeChanged() {
            sharedToolTipGuard.applyToolTipChrome()
        }
    }
}
