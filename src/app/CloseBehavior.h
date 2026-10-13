#pragma once

#include <QtGlobal>

// What closing the main window does, and what brings it back. Pure decisions
// with the platform passed in, so they are testable on Linux (GitHub #34).
//
// Windows/Linux: closing quits unless the user chose close-to-tray AND a tray
// exists to restore from. macOS: a window close is not a quit (Cmd+W closes,
// Cmd+Q quits, the Dock icon brings the window back), so it hides regardless
// of the tray setting, which then only decides whether a menu-bar item exists.
namespace closebehavior {

#ifdef Q_OS_MACOS
inline constexpr bool kIsMac = true;
#else
inline constexpr bool kIsMac = false;
#endif

/// Whether a close request hides the window instead of letting it quit.
/// `quitIntended`: a real quit (Cmd/Ctrl+Q, tray Quit, update, Dock Quit).
constexpr bool closeHidesWindow(bool isMac, bool closeToTray,
                                bool trayAvailable, bool quitIntended)
{
    if (quitIntended)
        return false;
    if (isMac)
        return true;
    return closeToTray && trayAvailable;
}

/// Whether the app becoming active should bring a hidden window back: the Dock
/// icon click (Qt reports it as a forced ApplicationActive) or Cmd+Tab. Only
/// when WE hid it by a close; a window hidden at startup (start in tray) or
/// minimised by the user is left alone.
constexpr bool activationRestoresWindow(bool isMac, bool hiddenByClose,
                                        bool windowHidden)
{
    return isMac && hiddenByClose && windowHidden;
}

/// Whether the tray icon may carry a native (NSMenu) context menu. On macOS
/// Qt's status-item menu path reads NSEvent.clickCount of NSApp.currentEvent
/// from the menu-tracking notification, which is a SysDefined event on newer
/// macOS and throws NSInternalInconsistencyException (GitHub #34). There the
/// menu is shown by us from the activation instead.
constexpr bool trayUsesNativeMenu(bool isMac)
{
    return !isMac;
}

} // namespace closebehavior
