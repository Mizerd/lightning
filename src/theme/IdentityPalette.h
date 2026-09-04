#pragma once

#include <QColor>
#include <QObject>
#include <QVariant>
#include <QQmlEngine>
#include <QString>

#include "theme/IdentityColors.h"

// QML-facing wrapper over lightning::theme, so AppTheme.qml derives its
// identity discs from the very same code the notification painter uses
// instead of a second implementation that drifts from it. Deliberately
// separate from IdentityPalette.h: the notification path links no QML at
// all, and it must keep being able to include the arithmetic.
class IdentityPalette : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit IdentityPalette(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE int index(const QString &key) const
    {
        return lightning::theme::identityIndex(key);
    }
    Q_INVOKABLE QColor disc(int slot, const QColor &accent) const
    {
        return lightning::theme::discColor(slot, accent);
    }
    Q_INVOKABLE QColor ink(int slot, const QColor &accent) const
    {
        return lightning::theme::discInk(slot, accent);
    }
    // The sender-name ink for a slot. `surfaces` is every ground a name is
    // drawn on; the ink clears the worst of them. QVariantList because that
    // is what a QML array arrives as.
    Q_INVOKABLE QColor nameInk(int slot, const QColor &accent,
                               const QVariantList &surfaces) const
    {
        QList<QColor> grounds;
        grounds.reserve(surfaces.size());
        for (const QVariant &value : surfaces) {
            const QColor ground = value.value<QColor>();
            if (ground.isValid())
                grounds.append(ground);
        }
        return lightning::theme::nameInk(slot, accent, grounds);
    }
    // A colour the user chose, made legible on the viewer's surfaces. Their
    // hue, this window's readability.
    Q_INVOKABLE QColor legibleChoice(const QColor &chosen,
                                     const QVariantList &surfaces) const
    {
        QList<QColor> grounds;
        grounds.reserve(surfaces.size());
        for (const QVariant &value : surfaces) {
            const QColor ground = value.value<QColor>();
            if (ground.isValid())
                grounds.append(ground);
        }
        return lightning::theme::legibleChoice(chosen, grounds);
    }
};
