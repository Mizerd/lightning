#pragma once

#include <QColor>
#include <QList>
#include <QString>

namespace lightning::theme {

// The nine identity slots. One per user or room, chosen by a stable hash of
// the id, so the same person keeps the same colour everywhere.
inline constexpr int kIdentitySlots = 9;

// Mirrors AppTheme.identityIndex in JavaScript, including its 32-bit
// wrapping arithmetic.
int identityIndex(const QString &key);

// The disc fill for one slot: nine evenly spaced hues in a 190-degree arc
// centred on the theme's anchor, so the family belongs to the theme.
//
// ThemeTokensTest pins, across every theme, that adjacent slots stay
// perceptually distinct and that the initials on every disc clear 4.5:1.
// The lightness ladder alternates deep and pale for both reasons.
QColor discColor(int index, const QColor &accent);

// White or near-black, whichever the disc carries at 4.5:1; half the discs
// are pale.
QColor discInk(int index, const QColor &accent);

// The colour a SettingsManager::Theme id anchors its identity discs on:
//
//     background, when it has a usable hue (HSL saturation >= 0.20)
//                 AND its hue is more than 60 degrees from the accent;
//     the accent otherwise.
//
// In practice that is the accent everywhere except Storm, whose navy shell is
// almost opposite its yellow accent.
//
// A hand-kept mirror of that rule applied to qml/AppTheme.qml, which remains
// the source of truth; it exists for notifications, which are painted without
// a QML engine. ThemeTokensTest checks the two agree for every theme. A custom
// theme (12) resolves to the brand anchor here, since its override lives in
// QML; the in-window discs follow the override.
QColor anchorForTheme(int themeId);

// The sender-name ink for a slot, in the same hue family as its disc, adjusted
// until it clears 4.5:1 against the worst of `surfaces` (every ground a name
// is painted on).
QColor nameInk(int index, const QColor &accent, const QList<QColor> &surfaces);

// A colour another user chose, made legible on the viewer's surfaces. Only
// lightness moves, just far enough to clear 4.5:1 on the worst ground. The
// value is remote profile data, so painting it verbatim would let anyone make
// their name unreadable for everyone else.
QColor legibleChoice(const QColor &chosen, const QList<QColor> &surfaces);

} // namespace lightning::theme
