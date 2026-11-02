#pragma once

#include <QLocale>
#include <QString>
#include <QtGlobal>

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

inline QString followSystemClockFormat(const QLocale &ui,
                                       const QLocale &timeCategory,
                                       const QString &systemTimeFormat)
{
    if (timeCategory.language() == ui.language())
        return timeCategory.timeFormat(QLocale::ShortFormat);
    const QString uiFormat = ui.timeFormat(QLocale::ShortFormat);
    if (timeCategory.language() == QLocale::C)
        return uiFormat;
    const bool systemTwelve = usesTwelveHourClock(systemTimeFormat);
    if (usesTwelveHourClock(uiFormat) == systemTwelve)
        return uiFormat;
    return systemTwelve ? QStringLiteral("h:mm AP") : QStringLiteral("HH:mm");
}

}
