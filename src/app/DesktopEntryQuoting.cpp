#include "app/DesktopEntryQuoting.h"

#include <QCryptographicHash>
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

namespace {

const QLatin1String kMarker("X-Lightning-Generated=");

QString sha256Hex(const QByteArray &bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString unquoteExecArgument(const QString &quoted)
{
    if (quoted.size() < 2 || !quoted.startsWith(QLatin1Char('"'))
        || !quoted.endsWith(QLatin1Char('"')))
        return {};
    const QString inner = quoted.mid(1, quoted.size() - 2);
    QString layer1;
    for (qsizetype i = 0; i < inner.size(); ++i) {
        if ((inner.at(i) == QLatin1Char('\\') || inner.at(i) == QLatin1Char('%'))
            && i + 1 < inner.size())
            ++i;
        layer1.append(inner.at(i));
    }
    QString path;
    for (qsizetype i = 0; i < layer1.size(); ++i) {
        if (layer1.at(i) == QLatin1Char('\\') && i + 1 < layer1.size())
            ++i;
        path.append(layer1.at(i));
    }
    return path;
}

// Frozen copy of what 0.9.3 to 0.10.5 wrote; the two releases differ only in
// the payload's Comment=.
bool isReleasedEntry(const QString &entry)
{
    QString exec;
    QString version;
    for (const QString &line : entry.split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("Exec=")))
            exec = line.mid(5);
        else if (line.startsWith(QLatin1String("X-AppImage-Version=")))
            version = line.mid(19);
    }
    const QLatin1String args(" --backend=rust");
    if (!exec.endsWith(args))
        return false;
    const QString path = unquoteExecArgument(exec.chopped(args.size()));
    if (path.isEmpty())
        return false;
    QString tail = QStringLiteral("Exec=") + quoteExecArgument(path) + args
        + QLatin1Char('\n');
    if (!path.contains(QLatin1Char(' ')) && !path.contains(QLatin1Char('\t')))
        tail += QStringLiteral("TryExec=")
            + QString(path).replace(QLatin1Char('\\'), QLatin1String("\\\\"))
            + QLatin1Char('\n');
    tail += QStringLiteral("Icon=lightning\nStartupWMClass=lightning-matrix\n"
                           "X-AppImage-Version=")
        + version + QStringLiteral("\nX-Lightning-Generated=true\n");
    for (const QLatin1String comment : { QLatin1String("Native Qt Matrix chat client"),
                                         QLatin1String("Chat and calls on Matrix") }) {
        const QString head =
            QStringLiteral("[Desktop Entry]\nType=Application\nName=Lightning\n"
                           "GenericName=Matrix Client\nComment=")
            + comment
            + QStringLiteral("\nTerminal=false\n"
                             "Categories=Network;Chat;InstantMessaging;\n"
                             "Keywords=Matrix;Chat;Messaging;\nStartupNotify=true\n");
        if (entry == head + tail)
            return true;
    }
    return false;
}

}

QString withGeneratedMarker(const QString &body)
{
    return body + kMarker + sha256Hex(body.toUtf8()) + QLatin1Char('\n');
}

bool isUntouchedGeneratedEntry(const QString &entry)
{
    if (!entry.endsWith(QLatin1Char('\n')))
        return false;
    const qsizetype at = entry.lastIndexOf(QLatin1Char('\n'), -2) + 1;
    const QString last = entry.mid(at).chopped(1);
    if (!last.startsWith(kMarker))
        return false;
    if (last == kMarker + QLatin1String("true"))
        return isReleasedEntry(entry);
    return last.mid(kMarker.size()) == sha256Hex(entry.left(at).toUtf8());
}

QString fileSha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return {};
    return QString::fromLatin1(hash.result().toHex());
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
