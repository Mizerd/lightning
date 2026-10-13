#pragma once

#include <QColor>
#include <QObject>
#include <QVariant>
#include <QQmlEngine>
#include <QString>

#include "theme/IdentityColors.h"

// QML wrapper over lightning::theme, so AppTheme.qml and the notification
// painter share one implementation. Kept separate from IdentityColors.h,
// which the QML-free notification path includes.
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
    // `surfaces` arrives from QML as a QVariantList of colours. `minContrast`
    // is the readability floor (4.5 Strong, 3.0 Weak, 1.0 Off).
    Q_INVOKABLE QColor nameInk(int slot, const QColor &accent,
                               const QVariantList &surfaces,
                               double minContrast
                                   = lightning::theme::kDefaultMinInkContrast) const
    {
        return lightning::theme::nameInk(slot, accent, grounds(surfaces),
                                         minContrast);
    }
    Q_INVOKABLE QColor legibleChoice(const QColor &chosen,
                                     const QVariantList &surfaces,
                                     double minContrast
                                         = lightning::theme::kDefaultMinInkContrast) const
    {
        return lightning::theme::legibleChoice(chosen, grounds(surfaces),
                                               minContrast);
    }

private:
    static QList<QColor> grounds(const QVariantList &surfaces)
    {
        QList<QColor> out;
        out.reserve(surfaces.size());
        for (const QVariant &value : surfaces) {
            const QColor ground = value.value<QColor>();
            if (ground.isValid())
                out.append(ground);
        }
        return out;
    }
};
