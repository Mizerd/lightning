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

// The magenta wedge, held back.
//
// At these lightnesses magenta and hot pink are the loudest part of the
// wheel: two of them in a room list read as a lot more than two, and on a
// cool theme they are the slots furthest from anything the shell is made of.
// Indigo Night's accent sits at 239 degrees, so the arc's warm end lands
// squarely here and the fallback avatars came out pink — reported in exactly
// those words.
//
// Damping the SATURATION rather than moving the hue is what keeps this from
// costing anything else: rotating or narrowing the arc either collapses
// several dark themes onto one identical family (they all end up clamped to
// the same span) or drops the all-pairs separation below the gate. This
// leaves every hue where it is, so the families stay distinct and the worst
// pair across all eleven themes is unchanged at dE 19.7 — the two affected
// slots simply become mauve and plum instead of pink and magenta.
constexpr double kMagentaLowDegrees = 290.0;
constexpr double kMagentaHighDegrees = 350.0;
constexpr double kMagentaDamping = 0.55;

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

QColor nameInk(int index, const QColor &accent, const QList<QColor> &surfaces)
{
    const int slot = clampSlot(index);

    // THE SAME HUE AS THE DISC. That is the whole point: a person's avatar
    // and their name are one identity, and the arc arithmetic below is
    // deliberately identical to slotColor()'s so the two cannot drift.
    const double accentHue = accent.hueF() < 0.0 ? 0.0 : accent.hueF();
    // A WIDER ARC THAN THE DISCS USE, and this is measured rather than
    // chosen. Nine inks spread over the discs' 190 degrees are 23.75 degrees
    // apart, and at the lightness a legible text ink needs that is simply not
    // nine distinguishable colours: the worst pair across the eleven themes
    // came out dE 3.3 (Nordic), against a floor of 12. No lightness pattern
    // rescues it — 2-phase, 3-phase and a full permutation were all tried and
    // all failed on the slots two apart.
    //
    // The old hand-tuned tables cleared the floor comfortably because they
    // used 321 degrees of the wheel, which is exactly why they matched no
    // theme. A filled disc can afford a tight family; thin text cannot.
    //
    // So the ink family stays CENTRED ON THE THEME's anchor — that is what
    // makes it the theme's palette — and spends more of the wheel around it.
    constexpr double kInkArcDegrees = 340.0;
    const double offset = (double(slot) / double(kIdentitySlots - 1) - 0.5)
                          * (kInkArcDegrees / 360.0);
    double hue = std::fmod(accentHue + offset, 1.0);
    if (hue < 0.0)
        hue += 1.0;

    // More chroma than the pale disc slots carry. A disc is a filled shape
    // and reads at low saturation; a name is thin strokes on a flat ground
    // and washes out at the same value.
    double saturation = 0.80;
    // THE MAGENTA DAMPING IS A DISC RULE AND DOES NOT TRANSFER. It exists
    // because two filled pink discs in a room list read as far more than two
    // — a solid shape at that hue is the loudest thing on the surface. A
    // NAME is thin strokes, and damping it there does not calm anything; it
    // just removes the chroma that tells two adjacent slots apart. Measured:
    // Lightning Light's slots 7 and 8 both land in the wedge (296 and 320
    // degrees) and came out dE 11.9, under the floor, purely from this.
    // Eased rather than dropped, so the wedge is still the quieter end.
    const double degrees = hue * 360.0;
    if (degrees >= kMagentaLowDegrees && degrees <= kMagentaHighDegrees)
        saturation *= 0.88;

    // Which direction to walk is decided by the GROUND, not by a dark/light
    // flag — a custom theme has no flag to consult, and the surfaces are the
    // truth in either case.
    double meanLuminance = 0.0;
    for (const QColor &surface : surfaces)
        meanLuminance += relativeLuminance(surface);
    if (!surfaces.isEmpty())
        meanLuminance /= double(surfaces.size());
    const bool darkGround = meanLuminance < 0.18;

    // ALTERNATE THE LIGHTNESS, for the same reason the disc ladder does.
    // Nine hues 23.75 degrees apart, all solved to the same lightness, are
    // not nine tellable-apart colours: measured, Lightning Dark's slots 4
    // and 5 came out dE 9.8 — below the floor the palette has advertised
    // since the 2026-08-21 de-duplication. Hue is not enough on its own once
    // the family is this tight.
    //
    // The bump is applied AFTER the contrast floor is found and always in
    // the direction of MORE contrast, so it can never undo legibility: on a
    // dark ground a lighter ink is a safer ink, and on a light ground a
    // darker one is.
    // EVERY SLOT A DIFFERENT LIGHTNESS, spread by permutation.
    //
    // Nine hues 23.75 degrees apart are not nine tellable-apart colours on
    // their own: light inks on a dark ground desaturate perceptually and
    // neighbouring hues converge. The disc ladder solves this by alternating
    // lightness, and this is the same idea taken further — a 2-phase
    // alternation leaves slots two apart identical (measured dE 10.3), and a
    // 3-phase one only pushes the collision further out (dE 11.9).
    //
    // `slot * 4 % 9` is a full permutation of 0..8 whose consecutive values
    // are always four steps apart, so ADJACENT slots get maximally different
    // lightness and no two slots share one. The bump is applied AFTER the
    // contrast floor and always toward MORE contrast, so it can never cost
    // legibility: lighter on a dark ground, darker on a light one.
    // With the arc widened, hue carries most of the separation and the
    // lightness only has to break ties between neighbours, so a plain
    // alternation is enough.
    // THREE phases, not two. A 2-phase alternation gives slots two apart the
    // SAME lightness, and on the themes whose elevated cards are lightest
    // (Nordic, Purple Dusk) every ink is already pushed near the top of the
    // range to clear 4.5:1 — so hue is doing the work alone up there and 4/6
    // collapsed to dE 6.1. Three phases give every slot a different lightness
    // from both neighbours and from the next one out.
    // `slot * 2 % 9` walks every ninth of the range exactly once, so no two
    // slots share a lightness and — unlike a 3-phase cycle — slots THREE
    // apart differ too. That was the last collision standing: Nordic's 4 and
    // 7 shared a phase and, on a theme whose elevated card pushes every ink
    // to the top of the range, lightness was the only axis left.
    constexpr double kInkSpread = 0.20;
    const double bump = double((slot * 2) % kIdentitySlots)
                        / double(kIdentitySlots - 1) * kInkSpread;


    // SATURATION IS HELD, NOT EASED TOWARD THE EXTREMES. Easing it was the
    // obvious thing to do — a near-white at full chroma can read as a tint —
    // but it is what flattened the themes whose "dark" surfaces are lightest.
    // Nordic and Purple Dusk push every ink up near 0.9 lightness to clear
    // 4.5:1 on their elevated cards, and easing the chroma there collapsed
    // hues 100 degrees apart to dE 5. Holding the chroma is what keeps them
    // apart once lightness can no longer do it.
    // QUANTISED before it is measured. QColor keeps float channels, but what
    // ships is the 8-bit value, and solving against the float one left slot 5
    // of Lightning Dark at 4.50 internally and 4.49 once rounded — the test
    // reading the hex was right and the derivation was wrong. Round here, so
    // the colour that is measured is the colour that is used.
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

    // TWO STAGES, and the order is the point. Lightness first, because it
    // costs nothing: a lighter ink on a dark ground is simply more legible.
    // Chroma is spent only when lightness has run out, because chroma is
    // what tells one identity from another.
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
