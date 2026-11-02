#pragma once

#include <QColor>
#include <QImage>
#include <QString>

namespace lightning::notifications {

// AppTheme.identityIndex: a 32-bit-wrapping string hash, then modulo the
// palette size. Hashes UTF-16 code units, because charCodeAt() does.
int identityIndex(const QString &key);

QColor identityColor(const QString &key, int themeId);

QString initialsFor(const QString &name);

QImage fallbackAvatar(const QString &name, const QString &colorKey, int edge,
                      int themeId);

}
