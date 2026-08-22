#pragma once

#include <QColor>
#include <QImage>
#include <QString>

// The initials avatar the interface already draws for an identity with no
// picture, rendered as an image so a desktop notification can carry it too.
//
// A user without an avatar previously produced a notification with NO image
// hint at all, and the notification daemon filled that hole with its own
// generic document glyph — so the one notification that most needed a name
// attached to it looked like an unknown file. Every other surface in
// Lightning shows a coloured disc with the identity's initials instead.
//
// The disc colour comes from lightning::theme (src/theme/IdentityPalette.*),
// the SAME code AppTheme.qml calls, so there is no palette copy here to
// drift — there used to be one, and it did. The initials rule is still
// reproduced from qml/Avatar.qml (_initials).
//
// Because the discs now follow the active theme's accent, the theme id has
// to travel with the request: a notification painted in the old fixed
// palette would no longer match the window it came from.
namespace lightning::notifications {

// AppTheme.identityIndex: a 32-bit-wrapping string hash, then modulo the
// palette size. Hashes UTF-16 code units, because charCodeAt() does.
int identityIndex(const QString &key);

// AppTheme.avatarColor for the same key, under `themeId`
// (SettingsManager::Theme).
QColor identityColor(const QString &key, int themeId);

// Avatar.qml _initials: Matrix sigils stripped, up to two initials from the
// first two words, uppercased; "?" when there is nothing to derive.
QString initialsFor(const QString &name);

// A square ARGB32 disc of identityColor(colorKey, themeId) carrying
// initialsFor(name) in whichever ink that disc can actually carry — NOT
// always white, since half the slots are pale. `colorKey` falls back to
// `name` when empty, exactly as Avatar.qml's _paletteKey does. Returns a
// null image for an empty edge or when both name and key are empty — a blank
// disc would be no more informative than the daemon's own placeholder.
QImage fallbackAvatar(const QString &name, const QString &colorKey, int edge,
                      int themeId);

} // namespace lightning::notifications
