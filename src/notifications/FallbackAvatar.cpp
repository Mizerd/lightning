#include "notifications/FallbackAvatar.h"

#include "theme/IdentityColors.h"

#include <QBrush>
#include <QFont>
#include <QPainter>
#include <QRegularExpression>
#include <QStringList>

namespace lightning::notifications {

int identityIndex(const QString &key)
{
    return lightning::theme::identityIndex(key);
}

QColor identityColor(const QString &key, int themeId)
{
    return lightning::theme::discColor(lightning::theme::identityIndex(key),
                                       lightning::theme::anchorForTheme(themeId));
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

QImage fallbackAvatar(const QString &name, const QString &colorKey, int edge,
                      int themeId)
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
    painter.setBrush(identityColor(paletteKey, themeId));
    painter.drawEllipse(0, 0, edge, edge);

    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(qMax(1, int(edge * 0.42)));
    painter.setFont(font);
    // The ink THIS disc can carry, exactly as Avatar.qml picks it. Half the
    // slots are pale — that alternation is what keeps two rooms apart once
    // the hues share one family — and white on a pale disc is unreadable.
    painter.setPen(lightning::theme::discInk(
        lightning::theme::identityIndex(paletteKey),
        lightning::theme::anchorForTheme(themeId)));
    painter.drawText(image.rect(), Qt::AlignCenter, initialsFor(name));
    painter.end();

    return image;
}

} // namespace lightning::notifications
