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
    // Markup tags carry no initial ("Alice <b>Bold</b>" -> "AB").
    static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
    cleaned.replace(tags, QStringLiteral(" "));

    // The first letter or digit CODE POINT of each of the first two words
    // that have one: a surrogate pair stays whole, punctuation and symbols
    // (emoji included) are skipped. Mirrors Avatar.qml's _initials().
    const QStringList words =
        cleaned.split(QRegularExpression(QStringLiteral("\\s+")),
                      Qt::SkipEmptyParts);
    QString out;
    int found = 0;
    for (const QString &word : words) {
        if (found >= 2)
            break;
        const QList<uint> cps = word.toUcs4();
        for (const uint cp : cps) {
            if (QChar::isLetterOrNumber(cp)) {
                out += QString::fromUcs4(&cp, 1).toUpper();
                ++found;
                break;
            }
        }
    }
    return out.isEmpty() ? QStringLiteral("?") : out;
}

QImage fallbackAvatar(const QString &name, const QString &colorKey, int edge,
                      int themeId)
{
    // Avatar.qml's _paletteKey: the identity key, else the display name.
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
    // The ink this disc can carry, as Avatar.qml picks it: half the slots are
    // pale, and white on a pale disc is unreadable.
    painter.setPen(lightning::theme::discInk(
        lightning::theme::identityIndex(paletteKey),
        lightning::theme::anchorForTheme(themeId)));
    painter.drawText(image.rect(), Qt::AlignCenter, initialsFor(name));
    painter.end();

    return image;
}

} // namespace lightning::notifications
