#include "theme/IdentityColors.h"

#include <QGuiApplication>
#include <QStyleHints>
#include <QtMath>

#include <array>
#include <cstdlib>

namespace lightning::theme {
namespace {

// Alternates deep and pale: within one 190-degree family hue alone does not
// separate adjacent slots. Hence the initials ink is chosen per disc.
constexpr std::array<double, kIdentitySlots> kLightness = {
    0.30, 0.56, 0.38, 0.62, 0.33, 0.58, 0.42, 0.60, 0.35,
};

// Degrees of hue the nine slots span, centred on the theme's accent.
constexpr double kArcDegrees = 190.0;

// Magenta and hot pink are the loudest discs at these lightnesses. Saturation
// is damped rather than the hue moved: rotating or narrowing the arc collapses
// several themes onto one family or breaks the all-pairs separation gate.
constexpr double kMagentaLowDegrees = 290.0;
constexpr double kMagentaHighDegrees = 350.0;
constexpr double kMagentaDamping = 0.55;

// Yellow-green, the brightest part of the wheel, damped the same way.
constexpr double kLimeLowDegrees = 62.0;
constexpr double kLimeHighDegrees = 108.0;
constexpr double kLimeDamping = 0.55;

constexpr double kMinInkContrast = 4.5;

double channelLinear(double c)
{
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double relativeLuminance(const QColor &color)
{
    return 0.2126 * channelLinear(color.redF())
         + 0.7152 * channelLinear(color.greenF())
         + 0.0722 * channelLinear(color.blueF());
}

double contrastRatio(const QColor &a, const QColor &b)
{
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
}

const QColor &darkInk()
{
    static const QColor ink(0x12, 0x15, 0x1A);
    return ink;
}

// Pale slots carry less chroma, so they read as a tint of the theme rather
// than as a second accent competing with it.
double saturationFor(double lightness, double accentSaturation)
{
    const double base = (0.34 + accentSaturation * 0.45)
                        * (1.0 - (lightness - 0.28) * 0.55);
    return qBound(0.30, base, 0.72);
}

QColor slotColor(int index, const QColor &accent, double lightness)
{
    const double accentHue = accent.hueF() < 0.0 ? 0.0 : accent.hueF();
    const double offset = (double(index) / double(kIdentitySlots - 1) - 0.5)
                          * (kArcDegrees / 360.0);
    double hue = std::fmod(accentHue + offset, 1.0);
    if (hue < 0.0)
        hue += 1.0;
    double saturation = saturationFor(lightness, accent.hslSaturationF());
    const double degrees = hue * 360.0;
    if (degrees >= kMagentaLowDegrees && degrees <= kMagentaHighDegrees)
        saturation *= kMagentaDamping;
    if (degrees >= kLimeLowDegrees && degrees <= kLimeHighDegrees)
        saturation *= kLimeDamping;
    QColor out;
    out.setHslF(float(hue), float(saturation), float(lightness));
    return out.toRgb();
}

int clampSlot(int index)
{
    if (index < 0 || index >= kIdentitySlots)
        return 0;
    return index;
}

} // namespace

int identityIndex(const QString &key)
{
    // JavaScript's `h = ((h << 5) - h + c) | 0` wraps at 32 bits. Computed
    // unsigned and reinterpreted, since signed overflow is UB in C++.
    quint32 hash = 0;
    for (const QChar character : key)
        hash = (hash << 5) - hash + character.unicode();
    const qint32 signedHash = static_cast<qint32>(hash);
    // Widened before abs(): |INT32_MIN| does not fit in an int32.
    const qint64 magnitude = std::llabs(static_cast<qint64>(signedHash));
    return int(magnitude % kIdentitySlots);
}

QColor discColor(int index, const QColor &accent)
{
    const int slot = clampSlot(index);
    double lightness = kLightness[size_t(slot)];
    QColor disc = slotColor(slot, accent, lightness);
    // Push the disc away from mid-tone until an ink clears 4.5:1 on it.
    const bool preferWhite = lightness <= 0.5;
    for (int guard = 0; guard < 24; ++guard) {
        const QColor ink = preferWhite ? QColor(Qt::white) : darkInk();
        if (contrastRatio(disc, ink) >= kMinInkContrast)
            break;
        lightness = preferWhite ? lightness * 0.95
                                : qMin(0.92, lightness * 1.05);
        disc = slotColor(slot, accent, lightness);
    }
    return disc;
}

QColor discInk(int index, const QColor &accent)
{
    const QColor disc = discColor(index, accent);
    return contrastRatio(disc, QColor(Qt::white))
                   >= contrastRatio(disc, darkInk())
            ? QColor(Qt::white)
            : darkInk();
}

QColor nameInk(int index, const QColor &accent, const QList<QColor> &surfaces)
{
    const int slot = clampSlot(index);

    // Same anchor as the discs, but a wider arc: at text lightness nine inks
    // 23.75 degrees apart are not distinguishable, whatever the lightness
    // pattern. A filled disc can afford a tighter family than thin text.
    const double accentHue = accent.hueF() < 0.0 ? 0.0 : accent.hueF();
    constexpr double kInkArcDegrees = 340.0;
    const double offset = (double(slot) / double(kIdentitySlots - 1) - 0.5)
                          * (kInkArcDegrees / 360.0);
    double hue = std::fmod(accentHue + offset, 1.0);
    if (hue < 0.0)
        hue += 1.0;

    // More chroma than the discs: thin strokes wash out at disc saturation.
    double saturation = 0.80;
    // Only a light magenta easing: the disc-strength damping would erase the
    // chroma that separates adjacent name inks.
    const double degrees = hue * 360.0;
    if (degrees >= kMagentaLowDegrees && degrees <= kMagentaHighDegrees)
        saturation *= 0.88;

    // Direction is decided by the surfaces, not a dark/light flag, so custom
    // themes work too.
    double meanLuminance = 0.0;
    for (const QColor &surface : surfaces)
        meanLuminance += relativeLuminance(surface);
    if (!surfaces.isEmpty())
        meanLuminance /= double(surfaces.size());
    const bool darkGround = meanLuminance < 0.18;

    // A per-slot lightness bump breaks ties where inks are pushed to the top
    // of the range and hue alone separates them. `slot * 2 % 9` is a
    // permutation of 0..8, so no two slots share a lightness. Applied after
    // the contrast floor and always toward more contrast, so it never costs
    // legibility.
    constexpr double kInkSpread = 0.20;
    const double bump = double((slot * 2) % kIdentitySlots)
                        / double(kIdentitySlots - 1) * kInkSpread;


    // Saturation is not eased toward the extremes: near the top of the
    // lightness range it is what keeps hues apart.
    // Quantised to 8 bits before measuring, so the measured colour is the
    // one that ships.
    const auto build = [&](double l, double sat) {
        const QColor exact =
            QColor::fromHslF(float(hue), float(sat), float(l)).toRgb();
        return QColor(exact.red(), exact.green(), exact.blue());
    };
    const auto worstContrast = [&](const QColor &c) {
        double worst = 21.0;
        for (const QColor &surface : surfaces)
            worst = qMin(worst, contrastRatio(c, surface));
        return worst;
    };

    // Adjust lightness first; spend chroma, which separates identities, only
    // when lightness has run out.
    double lightness = darkGround ? 0.70 : 0.40;
    for (int guard = 0; guard < 48; ++guard) {
        if (worstContrast(build(lightness, saturation)) >= kMinInkContrast)
            break;
        lightness = darkGround ? qMin(0.96, lightness + 0.02)
                               : qMax(0.06, lightness - 0.02);
    }
    for (int guard = 0; guard < 24; ++guard) {
        if (worstContrast(build(lightness, saturation)) >= kMinInkContrast)
            break;
        saturation = qMax(0.18, saturation - 0.04);
    }
    lightness = darkGround ? qMin(0.96, lightness + bump)
                           : qMax(0.06, lightness - bump);
    return build(lightness, saturation);
}

QColor legibleChoice(const QColor &chosen, const QList<QColor> &surfaces)
{
    if (!chosen.isValid())
        return chosen;

    const auto quantise = [](const QColor &c) {
        return QColor(c.red(), c.green(), c.blue());
    };
    const auto worstContrast = [&](const QColor &c) {
        double worst = 21.0;
        for (const QColor &surface : surfaces)
            worst = qMin(worst, contrastRatio(c, surface));
        return worst;
    };

    QColor ink = quantise(chosen);
    if (surfaces.isEmpty() || worstContrast(ink) >= kMinInkContrast)
        return ink;   // already legible here; leave it exactly as chosen

    double meanLuminance = 0.0;
    for (const QColor &surface : surfaces)
        meanLuminance += relativeLuminance(surface);
    meanLuminance /= double(surfaces.size());
    const bool darkGround = meanLuminance < 0.18;

    // Hue and saturation are held; only lightness moves.
    const double hue = chosen.hueF() < 0.0 ? 0.0 : chosen.hueF();
    const double saturation = chosen.hslSaturationF();
    double lightness = chosen.lightnessF();
    for (int guard = 0; guard < 48; ++guard) {
        ink = quantise(QColor::fromHslF(float(hue), float(saturation),
                                        float(lightness)).toRgb());
        if (worstContrast(ink) >= kMinInkContrast)
            break;
        lightness = darkGround ? qMin(0.97, lightness + 0.02)
                               : qMax(0.03, lightness - 0.02);
    }
    return ink;
}

QColor anchorForTheme(int themeId)
{
    // The header's rule, pre-applied to qml/AppTheme.qml's literals.
    switch (themeId) {
    case 1:  return QColor(0x1D, 0x57, 0xFF);   // Lightning Light  bg 212 / accent 225
    case 2:  return QColor(0x1D, 0x57, 0xFF);   // Lightning Dark   214 / 225
    case 3:  return QColor(0x2E, 0x6E, 0xEB);   // Graphite         neutral bg
    case 4:  return QColor(0x1D, 0x57, 0xFF);   // Midnight         216 / 225
    case 5:  return QColor(0x5E, 0x81, 0xAC);   // Nordic           neutral bg
    case 6:  return QColor(0x8F, 0x73, 0xE9);   // Purple Dusk      248 / 254
    case 7:  return QColor(0xA3, 0x4C, 0x00);   // Warm              37 /  28
    case 8:  return QColor(0x00, 0x77, 0x57);   // Moss Light       135 / 164
    case 9:  return QColor(0x4A, 0x4E, 0xED);   // Indigo Night     neutral bg
    case 10: return QColor(0x27, 0xC2, 0xAD);   // Deep Teal        180 / 172
    // Storm: the navy shell, not the yellow accent (233 vs 46 degrees).
    case 11: return QColor(0x02, 0x05, 0x1D);
    default: break;
    }
    // System (0) resolves as AppTheme does. A custom theme (12) falls back to
    // the brand shell, since its override lives in QML.
    if (themeId == 0 && qGuiApp
        && QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Light) {
        return QColor(0x00, 0x77, 0x57);
    }
    return QColor(0x02, 0x05, 0x1D);
}

} // namespace lightning::theme
