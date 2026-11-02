#pragma once

#include <QByteArray>
#include <QString>

namespace imagemeta {

enum class Mode {
    Keep,
    RemoveLocation,
    RemoveAll,
};

// "location" / "all" / "keep". An unknown string is the DEFAULT
// (RemoveLocation), never the nearest value: this is a privacy setting and an
// unreadable value must not switch it off.
// Header-only so SettingsManager (linked into dozens of targets) can use them
// without pulling the byte-level code in.
inline Mode defaultMode() { return Mode::RemoveLocation; }
inline Mode modeFromString(const QString &value)
{
    const QString v = value.trimmed().toLower();
    if (v == QLatin1String("all"))
        return Mode::RemoveAll;
    if (v == QLatin1String("keep"))
        return Mode::Keep;
    return defaultMode();
}
inline QString modeToString(Mode mode)
{
    switch (mode) {
    case Mode::Keep: return QStringLiteral("keep");
    case Mode::RemoveAll: return QStringLiteral("all");
    case Mode::RemoveLocation: break;
    }
    return QStringLiteral("location");
}

enum class Status {
    Unchanged,
    Stripped,
    Unsupported,
    Failed,
};

struct Result {
    Status status = Status::Unchanged;
    QByteArray bytes;
};

Result strip(const QByteArray &input, Mode mode);

bool containsLocation(const QByteArray &input);
int exifOrientation(const QByteArray &input);

}
