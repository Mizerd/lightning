pragma Singleton
import QtQuick
import MatrixClient

QtObject {
    id: policy

    readonly property var _settings:
        (typeof app !== "undefined" && app && app.settings) ? app.settings : null

    readonly property int setting: {
        var v = policy._settings ? policy._settings.gifAutoplay : undefined
        return (v === 0 || v === 1 || v === 2) ? v : 2
    }

    readonly property int mode:
        setting === 2 ? 2
                      : ((setting === 1 || AppTheme.reducedMotion) ? 1 : 0)

    // Animated bytes may be fetched and decoded at all. False under Don't
    // play: no AnimatedImage gets a source (the GIF picker draws a provider
    // preview's first frame through a plain Image instead), so nothing is
    // decoded beyond the still picture every site draws. Speculative
    // video/audio downloads are NOT governed here: that is the Privacy ·
    // Media "Preload videos and audio" switch.
    readonly property bool allowed: mode !== 2

    readonly property bool autoplay: mode === 0

    readonly property bool suspended:
        typeof app !== "undefined" && app !== null
        && app.animationsSuspended === true

    function plays(hovered) {
        return policy.mode === 0 || (policy.mode === 1 && hovered === true)
    }
}
