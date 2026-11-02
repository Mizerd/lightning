#pragma once

#include <QtGlobal>

namespace closebehavior {

#ifdef Q_OS_MACOS
inline constexpr bool kIsMac = true;
#else
inline constexpr bool kIsMac = false;
#endif

constexpr bool closeHidesWindow(bool isMac, bool closeToTray,
                                bool trayAvailable, bool quitIntended)
{
    if (quitIntended)
        return false;
    if (isMac)
        return true;
    return closeToTray && trayAvailable;
}

constexpr bool activationRestoresWindow(bool isMac, bool hiddenByClose,
                                        bool windowHidden)
{
    return isMac && hiddenByClose && windowHidden;
}

constexpr bool trayUsesNativeMenu(bool isMac)
{
    return !isMac;
}

}
