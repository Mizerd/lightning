// The badge colour rules of AppTheme.qml (its "Badge colours" section), for
// the C++ fixtures that resolve a palette the way paletteForTheme() does
// (CustomThemeTest, ChatBackdropTest). AppTheme.qml is the rule; this is a
// copy kept beside the tests that need it, so the readability table can grade
// the badge fills and inks AppTheme actually derives.
//
// Same arithmetic as the QML: WCAG relative luminance with the 0.03928 knee,
// HSL lightness steps of 0.005 tried both ways, the opposite ink when the own
// ink would move the fill by more than kMaxShift and the opposite needs less.
// High contrast is not modelled (the fixtures grade the default mode).
#pragma once

#include <QColor>
#include <QList>
#include <QString>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <limits>

namespace badgerules {

constexpr double kInkRatio = 4.6;
constexpr double kMaxShift = 0.14;
inline const QString kDarkInk = QStringLiteral("#0B0D12");
inline const QString kLightInk = QStringLiteral("#FFFFFF");

inline double luminance(const QColor &c)
{
    const auto lin = [](double v) {
        return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF()) + 0.0722 * lin(c.blueF());
}

inline double ratio(const QColor &a, const QColor &b)
{
    const double la = luminance(a) + 0.05;
    const double lb = luminance(b) + 0.05;
    return la > lb ? la / lb : lb / la;
}

struct Fit {
    QColor color;
    double shift = 0.0;
    bool ok = false;
    double worst = 0.0;
};

inline double worstRatio(const QColor &c, const QList<QColor> &against)
{
    double worst = std::numeric_limits<double>::infinity();
    for (const QColor &a : against)
        worst = std::min(worst, ratio(c, a));
    return worst;
}

// AppTheme._lightnessFit.
inline Fit lightnessFit(const QColor &fill, const QList<QColor> &against, double minRatio)
{
    Fit best{fill, 0.0, false, worstRatio(fill, against)};
    if (best.worst >= minRatio) {
        best.ok = true;
        return best;
    }
    const double h = fill.hslHueF() < 0 ? 0.0 : fill.hslHueF();
    const double s = fill.hslSaturationF();
    const double l = fill.lightnessF();
    for (int i = 1; i <= 200; ++i) {
        const double step = i * 0.005;
        for (const double nl : { l - step, l + step }) {
            if (nl < 0.0 || nl > 1.0)
                continue;
            const QColor c = QColor::fromHslF(float(h), float(s), float(nl), 1.0f);
            const double w = worstRatio(c, against);
            if (w >= minRatio)
                return Fit{c, step, true, w};
            if (w > best.worst)
                best = Fit{c, step, false, w};
        }
    }
    return best;
}

// AppTheme.badgeInk.
inline QColor badgeInk(const QColor &fill, const QColor &ink, double minRatio = kInkRatio)
{
    const Fit own = lightnessFit(fill, {ink}, minRatio);
    if (own.ok && own.shift <= kMaxShift)
        return ink;
    const QColor other(luminance(ink) > 0.18 ? kDarkInk : kLightInk);
    const Fit alt = lightnessFit(fill, {other}, minRatio);
    if (alt.ok && (!own.ok || alt.shift < own.shift))
        return other;
    return ink;
}

// The derived badge keys paletteForTheme() adds after its element loop, from
// the palette's own resolved roles. `own` names what the palette or the user
// set explicitly (an element they set keeps its colour). A source that is
// absent or translucent leaves its derived keys absent, as the audit then
// reports.
inline void addDerived(QVariantMap &p, const QVariantMap &own = QVariantMap())
{
    const auto color = [&p](const char *key) {
        const QVariant v = p.value(QLatin1String(key));
        QColor c = v.canConvert<QColor>() && v.typeId() == QMetaType::QColor
            ? v.value<QColor>() : QColor::fromString(v.toString());
        return (c.isValid() && c.alpha() == 255) ? c : QColor();
    };
    const auto hex = [](const QColor &c) { return c.name(QColor::HexRgb).toUpper(); };
    const QColor mention = color("mentionBadge");
    QColor mentionText = color("mentionBadgeText");
    if (!mentionText.isValid())
        mentionText = QColor(kLightInk);   // AppTheme's dangerText literal
    if (mention.isValid()) {
        const QColor ink = badgeInk(mention, mentionText);
        const QColor fill = lightnessFit(mention, {ink}, kInkRatio).color;
        p.insert(QStringLiteral("mentionBadgeInk"), hex(ink));
        p.insert(QStringLiteral("mentionBadgeFill"), hex(fill));
        if (!own.contains(QStringLiteral("roomMentionBadge")))
            p.insert(QStringLiteral("roomMentionBadge"), hex(fill));
    }
    const QColor unread = color("roomUnreadBadge");
    const QColor unreadText = color("unreadBadgeText");
    if (unread.isValid() && unreadText.isValid()) {
        const QColor ink = badgeInk(unread, unreadText);
        p.insert(QStringLiteral("unreadCountInk"), hex(ink));
        p.insert(QStringLiteral("unreadCountFill"),
                 hex(lightnessFit(unread, {ink}, kInkRatio).color));
    }
}

} // namespace badgerules
