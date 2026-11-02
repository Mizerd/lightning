#pragma once

#include <QByteArray>
#include <QDir>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace lightning {

// Drop search-path entries that belong to a different Qt major from the
// variables Qt reads at startup.
//
// A desktop session that still exports Qt 5 locations (a KDE or qt5ct
// wrapper, a distro profile script) puts them in QT_PLUGIN_PATH,
// QML2_IMPORT_PATH, QT_QPA_PLATFORM_PLUGIN_PATH and QT_QPA_FONTDIR, and the
// wrapper around a Nix build only PREFIXES its own entries, so they stay.
// Qt searches those variables BEFORE its built-in locations: a Qt 5
// QtQuick/Shapes or QtQuick/Layouts directory in QML2_IMPORT_PATH shadows
// the Qt 6 module of the same name (its plugin then refuses to load), and a
// Qt 5 imageformats directory is probed for every format. Unsetting the
// variables is what fixes it, and a variable set to the empty string is not
// the same as one that is unset, so a variable left with nothing is unset.
//
// An entry is foreign when a path component names Qt 3, 4 or 5: `qt5`,
// `qt-5.15.18`, `qtbase-5.15.18-bin`, `qt5ct-1.5`, with or without a Nix store
// hash in front (`qt5compat-6.8` is a Qt 6 module and stays). Entries that name no Qt version are kept; so are Qt 6 entries
// of any minor, which Qt itself already version-checks plugin by plugin.
namespace foreignqt {

inline bool componentNamesOldQt(QString c)
{
    static const QRegularExpression hash(
        QStringLiteral("^[0-9a-z]{32}-"));
    c.remove(hash);
    c = c.toLower();
    static const QRegularExpression forms(QStringLiteral(
        "^(qt-?[345](\\.\\d+)*|qt[345]ct-\\d+(\\.\\d+)*"
        "|qt[a-z0-9]*-[345]\\.\\d+(\\.\\d+)*)(-.*)?$"));
    return forms.match(c).hasMatch();
}

inline bool isForeignQtPath(const QString &entry)
{
    QString normalised = entry;
    normalised.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QStringList parts =
        normalised.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &c : parts) {
        if (componentNamesOldQt(c))
            return true;
    }
    return false;
}

inline QByteArray filterPathList(const QByteArray &value,
                                 QStringList *dropped = nullptr)
{
    const QString text = QString::fromLocal8Bit(value);
    QStringList kept;
    for (const QString &entry : text.split(QDir::listSeparator(),
                                           Qt::SkipEmptyParts)) {
        if (isForeignQtPath(entry)) {
            if (dropped)
                dropped->append(entry);
        } else {
            kept.append(entry);
        }
    }
    return kept.join(QDir::listSeparator()).toLocal8Bit();
}

}

inline QStringList sanitizeForeignQtEnvironment()
{
    static const char *const names[] = {
        "QT_PLUGIN_PATH", "QML2_IMPORT_PATH", "QML_IMPORT_PATH",
        "QT_QPA_PLATFORM_PLUGIN_PATH", "QT_QPA_FONTDIR",
    };
    QStringList report;
    for (const char *name : names) {
        if (!qEnvironmentVariableIsSet(name))
            continue;
        const QByteArray before = qgetenv(name);
        QStringList dropped;
        const QByteArray after = foreignqt::filterPathList(before, &dropped);
        if (dropped.isEmpty())
            continue;
        if (after.isEmpty())
            qunsetenv(name);
        else
            qputenv(name, after);
        report.append(QStringLiteral("%1: ignored %2 Qt 5 or older entr%3 (%4)")
                          .arg(QLatin1String(name))
                          .arg(dropped.size())
                          .arg(dropped.size() == 1 ? QStringLiteral("y")
                                                   : QStringLiteral("ies"))
                          .arg(dropped.join(QLatin1Char(' '))));
    }
    return report;
}

}
