#pragma once

#include <QDir>
#include <QFileInfo>

// Qt reads zones from TZDIR or /usr/share/zoneinfo; a bare Nix container has
// neither, only a tzdata store path. Call before the first QTimeZone use.
inline void ensureZoneDataForTests()
{
    if (!qEnvironmentVariableIsEmpty("TZDIR")
            || QFileInfo::exists(QStringLiteral("/usr/share/zoneinfo/UTC")))
        return;
    const QDir store(QStringLiteral("/nix/store"));
    for (const QString &entry : store.entryList({ QStringLiteral("*-tzdata-*") },
                                                QDir::Dirs)) {
        const QString dir = store.filePath(entry) + QStringLiteral("/share/zoneinfo");
        if (QFileInfo::exists(dir + QStringLiteral("/UTC"))) {
            qputenv("TZDIR", dir.toUtf8());
            return;
        }
    }
}
