#pragma once

#include <QByteArray>
#include <QtGlobal>

#include <cstdio>

#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace lightning {

// Colours for a person reading the log in a terminal. stderr only: the log
// files format their own lines and never see these escapes.

// The de facto rules: NO_COLOR (https://no-color.org) and TERM=dumb opt out,
// and a QT_MESSAGE_PATTERN is the user's own format.
inline bool logColorWanted(bool stderrIsTerminal, const QByteArray &noColor,
                           const QByteArray &term, const QByteArray &pattern)
{
    return stderrIsTerminal && noColor.isEmpty() && term != "dumb"
           && pattern.isEmpty();
}

inline const char *logColorFor(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return "\033[90m";
    case QtInfoMsg:     return "\033[36m";
    case QtWarningMsg:  return "\033[33m";
    case QtCriticalMsg: return "\033[31m";
    case QtFatalMsg:    return "\033[1;31m";
    }
    return "";
}

inline bool stderrTakesLogColor()
{
#ifdef Q_OS_WIN
    if (!_isatty(_fileno(stderr)))
        return false;
    // Without this a Windows console prints the escapes as text.
    HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    return GetConsoleMode(handle, &mode)
           && SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
    return isatty(fileno(stderr)) != 0;
#endif
}

} // namespace lightning
