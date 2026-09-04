#pragma once

#include <QColor>
#include <QList>
#include <QString>

namespace lightning::theme {

// The nine identity slots. One per user or room, chosen by a stable hash of
// the id, so the same person keeps the same colour everywhere.
inline constexpr int kIdentitySlots = 9;

// The ONE identity hash. Mirrors the JavaScript in AppTheme.identityIndex,
// including its 32-bit wrapping arithmetic.
int identityIndex(const QString &key);

// The disc fill for one slot, DERIVED FROM THE ACTIVE THEME'S ACCENT.
//
// Until 2026-08-22 this was a fixed nine-colour ladder whose centre of
// gravity had deliberately been moved warm. That reads as a mistake on a
// cool theme: a deep indigo window with amber and rust discs in its room
// list. The slots are now nine evenly spaced hues in a 190-degree arc
// CENTRED ON THE THEME'S OWN ACCENT, so the family belongs to the theme,
// with the arc wide enough that two rooms are still told apart at a glance.
//
// Two properties this must keep, both of which the theme-token test pins
// across every theme accent:
//   * adjacent slots stay far apart perceptually — the 2026-08-21 audit
//     found sender inks that were "the same colour" at dE 5.6, and a disc
//     ladder can fail the same way;
//   * the initials on every disc clear 4.5:1. That is why the lightness
//     ladder alternates deep and pale instead of sitting at one level: with
//     every disc pinned under the white-text luminance cap there is no axis
//     left to separate them on once the hues are pulled into one family.
QColor discColor(int index, const QColor &accent);

// The initials ink for that disc — white or near-black, whichever the disc
// carries at 4.5:1. Never assume white: half of these discs are pale.
QColor discInk(int index, const QColor &accent);

// The colour each SettingsManager::Theme id derives its identity discs from.
//
// USUALLY the accent, because in ten of the eleven themes the accent IS the
// shell's own hue — Lightning Dark's background is 214 degrees and its accent
// 225, Purple Dusk's 248 and 254, Warm's 37 and 28.
//
// Storm is the exception and it is why this is not simply called "the
// accent": its shell is deep navy at 233 degrees and its accent is the brand
// bolt at 46, almost exactly opposite. Anchoring on the accent there built a
// magenta-red-orange-lime family and dropped it onto a navy window, which is
// exactly as out of place as it sounds. So: the accent anchors the discs
// unless it is nowhere near the surface they sit on, in which case the
// surface wins. Measured on the literals in qml/AppTheme.qml, the rule is
//
//     background, when it has a usable hue (HSL saturation >= 0.20)
//                 AND its hue is more than 60 degrees from the accent;
//     the accent otherwise.
//
// This is a hand-kept mirror of that rule applied to that file, which is the
// sole source of truth for colour. It exists because desktop notifications
// are painted with no QML engine anywhere near them, and a notification whose
// fallback avatar disagreed with the one in the window would be the same
// defect the old palette copy caused in the 2026-08-21 round. ThemeTokensTest
// parses both literals per theme, applies the rule, and requires the answers
// equal.
//
// Theme 12 (a user's custom theme) resolves to the brand anchor here: C++ has
// no access to the custom override layer, and a custom accent is a live QML
// value. The in-window discs do follow the override, because AppTheme passes
// its own resolved anchor.
QColor anchorForTheme(int themeId);

// The sender-name INK for a slot: the same hue family as that slot's disc,
// darkened or lightened until it clears 4.5:1 against every surface a name is
// drawn on.
//
// This used to be two hand-tuned nine-colour tables picked by dark/light and
// nothing else, so a person's avatar disc followed the theme and their NAME
// did not — a green disc with a red name on Moss Light. The tables even
// carried a comment claiming they were "hue-matched index-for-index to
// avatarPalette", which stopped being true the moment the discs were made
// theme-derived and the inks were left behind.
//
// Deriving it here rather than in QML is the same argument the discs make:
// one implementation, and the notification painter can reach it with no QML
// engine anywhere near. It also means a CUSTOM theme gets real name colours
// from its own two colours instead of inheriting a stranger's table.
//
// `surfaces` is every ground a name is painted on — background, card,
// elevated card, and the other party's bubble. The ink clears the WORST of
// them, so it is legible everywhere rather than on average.
QColor nameInk(int index, const QColor &accent, const QList<QColor> &surfaces);

} // namespace lightning::theme
