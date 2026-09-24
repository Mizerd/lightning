#pragma once

#include <QColor>
#include <QImage>
#include <QString>

// The initials avatar the interface draws for an identity with no picture,
// rendered as an image so a desktop notification can carry it instead of the
// daemon's generic glyph. Colours come from lightning::theme
// (src/theme/IdentityPalette.*), shared with AppTheme.qml; the initials rule
// mirrors qml/Avatar.qml (_initials). The theme id is required because disc
// colours follow the active theme's accent.
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
// initialsFor(name) in an ink readable on that disc. `colorKey` falls back to
// `name` when empty, as Avatar.qml's _paletteKey does. Returns a null image
// for an empty edge or when both name and key are empty.
QImage fallbackAvatar(const QString &name, const QString &colorKey, int edge,
                      int themeId);

} // namespace lightning::notifications
