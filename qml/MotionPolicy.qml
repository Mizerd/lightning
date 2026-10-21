pragma Singleton
import QtQuick
import MatrixClient

// The one rule every animated image follows: avatars (people, rooms, Spaces),
// banners, the chat background, GIFs and animated WebP in messages and link
// previews, stickers, the GIF and sticker pickers, the composer's GIF chip, the
// image viewer and the crop dialog. A site reads this, not the setting.
// (ChatBackdrop also names Reduce motion in its own gate; `mode` already folds
// it in, so the two agree.)
//
// The inputs belong to C++: the "Animated images" setting
// (SettingsManager::gifAutoplay; the name is historical, see its header),
// Reduce motion (AppTheme.reducedMotion, bound from the same settings), and
// whether Lightning has focus (AppController::animationsSuspended). What
// follows from them is presentation, decided once, here.
QtObject {
    id: policy

    readonly property var _settings:
        (typeof app !== "undefined" && app && app.settings) ? app.settings : null

    // The stored choice: 0 Play automatically, 1 Play on hover, 2 Don't play.
    // Anything unreadable (no app yet, a test without the setting) is Don't
    // play: nothing animates that nobody allowed.
    readonly property int setting: {
        var v = policy._settings ? policy._settings.gifAutoplay : undefined
        return (v === 0 || v === 1 || v === 2) ? v : 2
    }

    // What actually happens. Reduce motion turns Play automatically into Play
    // on hover: motion the user asks for with the pointer still plays, nothing
    // moves on its own.
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

    // Plays without being asked.
    readonly property bool autoplay: mode === 0

    // True while no window of Lightning has focus (another application is in
    // front, or the window is minimised or hidden). Every animation stops on
    // its current frame, so no frame is decoded. Deliberately not a setting:
    // Discord exposes "play GIFs only when focused" as a toggle (on by
    // default); Lightning keeps only that default, because a niche switch
    // would buy nothing a GIF nobody is looking at is worth.
    //
    // HOW a site applies it matters. A site whose `playing` changes (hover,
    // row on screen, picker shown) must fold it into `playing`
    // (`... && !MotionPolicy.suspended`): QMovie ignores setPaused() while it
    // is not running and start() clears it, so a `paused` set before
    // `playing` turns true is lost. Only a site whose `playing` is the
    // constant `true` (Avatar, BannerMotion, ChatBackdrop, the crop dialog,
    // all with their own stall handling on `playingChanged`) uses
    // `paused: MotionPolicy.suspended`, which holds the frame without
    // touching that handling; the load path applies `paused` before the
    // first frame. Never write `paused: suspended && playing` (a binding
    // loop through the movie's own state).
    readonly property bool suspended:
        typeof app !== "undefined" && app !== null
        && app.animationsSuspended === true

    // Whether an animation that may load should be running, for a site whose
    // pointer state is `hovered`. Suspension is applied separately (above).
    function plays(hovered) {
        return policy.mode === 0 || (policy.mode === 1 && hovered === true)
    }
}
