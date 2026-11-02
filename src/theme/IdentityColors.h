#pragma once

#include <QColor>
#include <QList>
#include <QString>

namespace lightning::theme {

inline constexpr int kIdentitySlots = 9;

// Mirrors AppTheme.identityIndex in JavaScript, including its 32-bit
// wrapping arithmetic.
int identityIndex(const QString &key);

QColor discColor(int index, const QColor &accent);

QColor discInk(int index, const QColor &accent);

QColor anchorForTheme(int themeId);

inline constexpr double kDefaultMinInkContrast = 4.5;
QColor nameInk(int index, const QColor &accent, const QList<QColor> &surfaces,
               double minContrast = kDefaultMinInkContrast);

QColor legibleChoice(const QColor &chosen, const QList<QColor> &surfaces,
                     double minContrast = kDefaultMinInkContrast);

}
