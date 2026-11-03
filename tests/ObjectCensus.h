#pragma once

#include <QList>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QQuickItem>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

#include <algorithm>

namespace census {

// Repeater-created items have a visual parent but no QObject parent, so both
// trees are walked.
inline void collect(QObject *object, QSet<QObject *> &seen)
{
    if (!object || seen.contains(object))
        return;
    seen.insert(object);
    for (QObject *child : object->children())
        collect(child, seen);
    if (auto *item = qobject_cast<QQuickItem *>(object)) {
        for (QQuickItem *child : item->childItems())
            collect(child, seen);
    }
}

inline QSet<QObject *> objectsUnder(QObject *root)
{
    QSet<QObject *> seen;
    collect(root, seen);
    return seen;
}

inline QString className(const QObject *object)
{
    static const QRegularExpression suffix(QStringLiteral("_QML(TYPE)?_\\d+$"));
    QString name = QString::fromLatin1(object->metaObject()->className());
    name.remove(suffix);
    return name;
}

inline QList<QPair<QString, int>> histogram(const QSet<QObject *> &objects)
{
    QMap<QString, int> counts;
    for (QObject *object : objects)
        ++counts[className(object)];
    QList<QPair<QString, int>> out;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it)
        out.append({ it.key(), it.value() });
    std::stable_sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
        return a.second > b.second;
    });
    return out;
}

inline QString describe(const QSet<QObject *> &objects, int top)
{
    const auto rows = histogram(objects);
    QStringList parts;
    for (int i = 0; i < rows.size() && i < top; ++i)
        parts << QStringLiteral("%1=%2").arg(rows.at(i).first).arg(rows.at(i).second);
    return QStringLiteral("%1 objects: %2").arg(objects.size()).arg(parts.join(QStringLiteral(", ")));
}

} // namespace census
