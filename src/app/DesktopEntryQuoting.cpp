#include "app/DesktopEntryQuoting.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringList>

namespace lightning::desktop_entry {

QString quoteExecArgument(const QString &path)
{
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

QString withDefaultHandler(const QString &mimeapps, const QString &mime,
                           const QString &desktopId)
{
    static const QString kGroup = QStringLiteral("[Default Applications]");
    const QString entry = mime + QLatin1Char('=') + desktopId + QLatin1Char(';');
    QStringList lines = mimeapps.split(QLatin1Char('\n'));
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    int insertAt = -1;
    bool inGroup = false;
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            inGroup = line == kGroup;
            if (inGroup && insertAt < 0)
                insertAt = i + 1;
            continue;
        }
        if (inGroup && line.section(QLatin1Char('='), 0, 0).trimmed() == mime)
            return {};
    }
    if (insertAt < 0) {
        if (!lines.isEmpty())
            lines.append(QString());
        lines.append(kGroup);
        lines.append(entry);
    } else {
        lines.insert(insertAt, entry);
    }
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

bool queryNamesAnotherHandler(bool exitedNormally, int exitCode,
                              const QString &output, const QString &ours)
{
    if (!exitedNormally || exitCode != 0)
        return true;
    const QString handler = output.trimmed();
    return !handler.isEmpty() && handler != ours;
}

QString claimDefaultHandler(const QString &path, const QString &mime,
                            const QString &desktopId,
                            const std::function<bool()> &handledElsewhere)
{
    const QFileInfo info(path);
    // A managed (e.g. home-manager) symlink must not be replaced by a file.
    if (info.isSymLink())
        return QStringLiteral("skipped: mimeapps.list is a symlink");
    QByteArray existing;
    if (info.exists()) {
        QFile current(path);
        if (!current.open(QIODevice::ReadOnly))
            return QStringLiteral("skipped: mimeapps.list unreadable");
        existing = current.readAll();
        if (current.error() != QFileDevice::NoError)
            return QStringLiteral("skipped: mimeapps.list unreadable");
    }
    const QByteArray updated =
        withDefaultHandler(QString::fromUtf8(existing), mime, desktopId).toUtf8();
    if (updated.isEmpty())
        return QStringLiteral("kept the existing default");
    if (handledElsewhere && handledElsewhere())
        return QStringLiteral("kept another handler");
    QDir().mkpath(info.absolutePath());
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || out.write(updated) != updated.size() || !out.commit())
        return QStringLiteral("failed: could not write mimeapps.list");
    return QStringLiteral("registered");
}

}
