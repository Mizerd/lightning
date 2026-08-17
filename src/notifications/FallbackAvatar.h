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
// The palette, the hash and the initials rule are reproduced from
// qml/AppTheme.qml (avatarPalette / identityIndex) and qml/Avatar.qml
// (_initials) EXACTLY, so the disc in a notification is the same colour and
// the same letters as the disc in the room list. notification-avatar pins
// that agreement against values computed independently of this code; if the
// QML palette or hash ever changes, that suite fails rather than the two
// quietly drifting apart.
namespace lightning::notifications {

// AppTheme.identityIndex: a 32-bit-wrapping string hash, then modulo the
// palette size. Hashes UTF-16 code units, because charCodeAt() does.
int identityIndex(const QString &key);

// AppTheme.avatarColor for the same key.
QColor identityColor(const QString &key);

// Avatar.qml _initials: Matrix sigils stripped, up to two initials from the
// first two words, uppercased; "?" when there is nothing to derive.
QString initialsFor(const QString &name);

// A square ARGB32 disc of identityColor(colorKey) carrying initialsFor(name)
// in white. `colorKey` falls back to `name` when empty, exactly as
// Avatar.qml's _paletteKey does. Returns a null image for an empty edge or
// when both name and key are empty — a blank disc would be no more
// informative than the daemon's own placeholder.
QImage fallbackAvatar(const QString &name, const QString &colorKey, int edge);

} // namespace lightning::notifications
