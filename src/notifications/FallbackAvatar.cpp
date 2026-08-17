#include "notifications/FallbackAvatar.h"

#include <QBrush>
#include <QFont>
#include <QPainter>
#include <QRegularExpression>
#include <QStringList>

#include <cstdlib>

namespace lightning::notifications {
namespace {

// qml/AppTheme.qml avatarPalette, in order — the index from identityIndex()
// selects into this, so the order is part of the contract.
const char *const kAvatarPalette[] = {
    "#2F8F5B", "#A3542F", "#6D5BD0", "#3A6EA5", "#B04A7E",
    "#C9662A", "#4A8F6D", "#B3823A", "#A05A92",
};
constexpr int kPaletteSize = int(sizeof(kAvatarPalette) / sizeof(kAvatarPalette[0]));

} // namespace

int identityIndex(const QString &key)
{
    // JavaScript's `h = ((h << 5) - h + c) | 0` is 32-bit wrapping signed
    // arithmetic. Done in UNSIGNED here and reinterpreted, because signed
    // overflow is undefined in C++ and would be free to compute anything.
    quint32 hash = 0;
    for (const QChar character : key)
        hash = (hash << 5) - hash + character.unicode();
    const qint32 signedHash = static_cast<qint32>(hash);
    // Widened before abs(): |INT32_MIN| does not fit in an int32, and
    // std::abs on it is undefined. JavaScript's Math.abs has no such edge
    // because it produces a double.
    const qint64 magnitude = std::llabs(static_cast<qint64>(signedHash));
    return int(magnitude % kPaletteSize);
}

QColor identityColor(const QString &key)
{
    return QColor(QLatin1String(kAvatarPalette[identityIndex(key)]));
}

QString initialsFor(const QString &name)
{
    QString cleaned = name;
    if (!cleaned.isEmpty()) {
        const QChar first = cleaned.at(0);
        if (first == QLatin1Char('@') || first == QLatin1Char('#')
            || first == QLatin1Char('!') || first == QLatin1Char('+')) {
            cleaned.remove(0, 1);
        }
    }
    cleaned = cleaned.trimmed();
    if (cleaned.isEmpty())
        return QStringLiteral("?");

    const QStringList words =
        cleaned.split(QRegularExpression(QStringLiteral("\\s+")),
                      Qt::SkipEmptyParts);
    if (words.size() >= 2) {
        return (words.at(0).left(1) + words.at(1).left(1)).toUpper();
    }
    return cleaned.left(1).toUpper();
}

QImage fallbackAvatar(const QString &name, const QString &colorKey, int edge)
{
    // Avatar.qml's _paletteKey: the explicit identity key when there is one,
    // otherwise the display name, so the disc matches whatever the interface
    // drew for the same identity.
    const QString paletteKey = colorKey.isEmpty() ? name : colorKey;
    if (edge <= 0 || paletteKey.isEmpty())
        return {};

    QImage image(edge, edge, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(identityColor(paletteKey));
    painter.drawEllipse(0, 0, edge, edge);

    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(qMax(1, int(edge * 0.42)));
    painter.setFont(font);
    // White, as Avatar.qml uses over a palette disc — the palette entries
    // are mid-tone fills chosen for exactly that.
    painter.setPen(QColor(0xFF, 0xFF, 0xFF));
    painter.drawText(image.rect(), Qt::AlignCenter, initialsFor(name));
    painter.end();

    return image;
}

} // namespace lightning::notifications
