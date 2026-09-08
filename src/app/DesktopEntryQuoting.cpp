#include "app/DesktopEntryQuoting.h"

namespace lightning::desktop_entry {

QString quoteExecArgument(const QString &path)
{
    // Layer 1: the Exec rule.
    QString exec;
    exec.reserve(path.size() + 8);
    for (const QChar c : path) {
        if (c.category() == QChar::Other_Control)
            return {};
        if (c == QLatin1Char('"') || c == QLatin1Char('\\')
            || c == QLatin1Char('$') || c == QLatin1Char('`'))
            exec.append(QLatin1Char('\\'));
        exec.append(c);
    }
    // Layer 2: the string escape the reader undoes first, plus the field code.
    QString out;
    out.reserve(exec.size() + 8);
    out.append(QLatin1Char('"'));
    for (const QChar c : exec) {
        if (c == QLatin1Char('\\'))
            out.append(QLatin1String("\\\\"));
        else if (c == QLatin1Char('%'))
            out.append(QLatin1String("%%"));
        else
            out.append(c);
    }
    out.append(QLatin1Char('"'));
    return out;
}

} // namespace lightning::desktop_entry
