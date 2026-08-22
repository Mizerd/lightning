#include "theme/IdentityColors.h"

#include <QGuiApplication>
#include <QStyleHints>
#include <QtMath>

#include <array>
#include <cstdlib>

namespace lightning::theme {
namespace {

// The lightness ladder. It alternates deep and pale ON PURPOSE: once nine
// hues are pulled into one 190-degree family, hue alone no longer separates
// adjacent slots, and pinning every disc under the white-text luminance cap
// removes the only other axis. Alternating lightness is what buys the
// separation back — and it is why the initials ink has to be chosen per disc.
constexpr std::array<double, kIdentitySlots> kLightness = {
    0.30, 0.56, 0.38, 0.62, 0.33, 0.58, 0.42, 0.60, 0.35,
};

// Degrees of hue the nine slots span, centred on the theme's accent.
constexpr double kArcDegrees = 190.0;

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
    QColor out;
    out.setHslF(float(hue),
                float(saturationFor(lightness, accent.hslSaturationF())),
                float(lightness));
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
    // JavaScript's `h = ((h << 5) - h + c) | 0` is 32-bit wrapping signed
    // arithmetic. Done in UNSIGNED here and reinterpreted, because signed
    // overflow is undefined in C++ and would be free to compute anything.
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
    // Push the disc away from mid-tone until SOME ink clears 4.5:1 on it.
    // A disc nobody can read initials on is not an identity colour.
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

QColor anchorForTheme(int themeId)
{
    // The rule in the header, already applied to qml/AppTheme.qml's literals.
    // In ten of eleven themes the accent IS the shell's hue (never more than
    // 29 degrees apart), so the accent anchors them. Storm is the one theme
    // where the accent is a brand highlight rather than the shell's colour.
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
    // Storm: the NAVY SHELL, not the yellow bolt. Its background is 233
    // degrees and its accent 46 — almost exactly opposite — and anchoring the
    // discs on the bolt built a magenta-red-orange-lime family and dropped it
    // onto a navy window. This is the branch the rule exists for.
    case 11: return QColor(0x02, 0x05, 0x1D);
    default: break;
    }
    // System (0) resolves the way AppTheme resolves it — Moss Light or Storm.
    // A custom theme (12) falls back to the brand shell: its override layer
    // is a live QML value C++ cannot reach, and a wrong-but-stable colour is
    // better than a blank disc.
    if (themeId == 0 && qGuiApp
        && QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Light) {
        return QColor(0x00, 0x77, 0x57);
    }
    return QColor(0x02, 0x05, 0x1D);
}

} // namespace lightning::theme
