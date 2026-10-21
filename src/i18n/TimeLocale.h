#pragma once

#include <QLocale>
#include <QString>
#include <QtGlobal>

// How the desktop asks for times and dates to be written, separately from the
// UI language. Header-only: SettingsManager (the clock setting) and
// LocalizationManager (the default QLocale) both use it, and SettingsManager
// is compiled into many test targets without LocalizationManager.
namespace i18n {

// The locale of the desktop's TIME category. On Unix that is the first set of
// LC_ALL, LC_TIME, LANG, as the C library resolves it: LANG=en_US with
// LC_TIME=en_GB asks for British dates in an American desktop. Elsewhere the
// platform has no such split and QLocale::system() answers. A "C"/"POSIX" or
// unset category yields QLocale::c(), whose language matches no UI language.
inline QLocale timeCategoryLocale()
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN) && !defined(Q_OS_ANDROID)
    for (const char *name : { "LC_ALL", "LC_TIME", "LANG" }) {
        QString value = qEnvironmentVariable(name);
        if (value.isEmpty())
            continue;
        // "en_GB.UTF-8", "de_DE@euro": only the locale name.
        for (const QChar stop : { QLatin1Char('.'), QLatin1Char('@') }) {
            const qsizetype cut = value.indexOf(stop);
            if (cut >= 0)
                value.truncate(cut);
        }
        if (value == QLatin1String("C") || value == QLatin1String("POSIX"))
            return QLocale::c();
        return QLocale(value);
    }
    return QLocale::c();
#else
    return QLocale::system();
#endif
}

// True when a Qt time format uses a 12-hour cycle: an AM/PM designator
// ("a"/"A") outside quoted text.
inline bool usesTwelveHourClock(const QString &format)
{
    bool quoted = false;
    for (const QChar c : format) {
        if (c == QLatin1Char('\''))
            quoted = !quoted;
        else if (!quoted && (c == QLatin1Char('a') || c == QLatin1Char('A')))
            return true;
    }
    return false;
}

// The clock setting's "Follow the system", as a Qt time format.
//   * The desktop's time category speaks the UI language: its own pattern
//     (en_GB in an English UI: "HH:mm").
//   * It speaks another language: the desktop's 12/24-hour CYCLE, written
//     with the UI language's own pattern, so no foreign literal leaks into
//     the UI (fr_CA's "HH 'h' mm" in an English one). When the UI language's
//     pattern has the other cycle, a plain pattern of the desktop's cycle.
// `systemTimeFormat` is what the desktop itself reports
// (QLocale::system().timeFormat(QLocale::ShortFormat)).
inline QString followSystemClockFormat(const QLocale &ui,
                                       const QLocale &timeCategory,
                                       const QString &systemTimeFormat)
{
    if (timeCategory.language() == ui.language())
        return timeCategory.timeFormat(QLocale::ShortFormat);
    const QString uiFormat = ui.timeFormat(QLocale::ShortFormat);
    // No desktop preference at all ("C", unset): the UI language's own.
    if (timeCategory.language() == QLocale::C)
        return uiFormat;
    const bool systemTwelve = usesTwelveHourClock(systemTimeFormat);
    if (usesTwelveHourClock(uiFormat) == systemTwelve)
        return uiFormat;
    return systemTwelve ? QStringLiteral("h:mm AP") : QStringLiteral("HH:mm");
}

} // namespace i18n
