#include "backdrop/BackdropMath.h"

#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>

namespace backdrop {

namespace {

// sRGB 8-bit channel -> linear light, computed once. Every contrast in this
// file goes through 8-bit channels (what is actually on screen), so a table
// replaces pow() in the scrim search's inner loop.
const std::array<double, 256> &linearTable()
{
    static const std::array<double, 256> table = [] {
        std::array<double, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const double c = i / 255.0;
            t[size_t(i)] = c <= 0.04045 ? c / 12.92
                                        : std::pow((c + 0.055) / 1.055, 2.4);
        }
        return t;
    }();
    return table;
}

double luminanceOf(int r, int g, int b)
{
    const auto &lin = linearTable();
    return 0.2126 * lin[size_t(std::clamp(r, 0, 255))]
        + 0.7152 * lin[size_t(std::clamp(g, 0, 255))]
        + 0.0722 * lin[size_t(std::clamp(b, 0, 255))];
}

double ratio(double la, double lb)
{
    const double hi = std::max(la, lb);
    const double lo = std::min(la, lb);
    return (hi + 0.05) / (lo + 0.05);
}

double linearOf(double c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double gammaOf(double v)
{
    v = std::clamp(v, 0.0, 1.0);
    return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

// D65 reference white.
constexpr double kXn = 0.95047;
constexpr double kYn = 1.0;
constexpr double kZn = 1.08883;

double labF(double t)
{
    return t > 0.008856 ? std::cbrt(t) : 7.787 * t + 16.0 / 116.0;
}

double labFInverse(double f)
{
    const double cube = f * f * f;
    return cube > 0.008856 ? cube : (f - 16.0 / 116.0) / 7.787;
}

struct Lab { double l, a, b; };

Lab toLab(const QColor &c)
{
    const double r = linearOf(c.redF());
    const double g = linearOf(c.greenF());
    const double b = linearOf(c.blueF());
    const double x = 0.4124 * r + 0.3576 * g + 0.1805 * b;
    const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    const double z = 0.0193 * r + 0.1192 * g + 0.9505 * b;
    const double fx = labF(x / kXn);
    const double fy = labF(y / kYn);
    const double fz = labF(z / kZn);
    return { 116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz) };
}

QColor fromLab(const Lab &lab)
{
    const double fy = (lab.l + 16.0) / 116.0;
    const double fx = fy + lab.a / 500.0;
    const double fz = fy - lab.b / 200.0;
    const double x = labFInverse(fx) * kXn;
    const double y = labFInverse(fy) * kYn;
    const double z = labFInverse(fz) * kZn;
    const double r = 3.2406 * x - 1.5372 * y - 0.4986 * z;
    const double g = -0.9689 * x + 1.8758 * y + 0.0415 * z;
    const double b = 0.0557 * x - 0.2040 * y + 1.0570 * z;
    return QColor(int(std::lround(gammaOf(r) * 255.0)),
                  int(std::lround(gammaOf(g) * 255.0)),
                  int(std::lround(gammaOf(b) * 255.0)));
}

bool isDarkGround(const QColor &ground)
{
    // AppTheme's own split for a custom palette: luminance below 0.18.
    return relativeLuminance(ground) < 0.18;
}

// How far the veil (the scrim's base colour) moves away from the ink, in L*.
// Measured on all eleven presets (2026-10-06 note): a veil equal to the ground
// leaves Warm and Moss Light no headroom at all for textMuted, so any picture
// would be hidden completely; lifting the veil off the ink gives every preset
// room without moving the ink.
constexpr double kVeilLiftLight = 8.0;
constexpr double kVeilLiftDark = -6.0;

QList<QRgb> worstCaseSamples()
{
    return { qRgb(255, 255, 255), qRgb(0, 0, 0) };
}

} // namespace

// ---- colour ----------------------------------------------------------------

double relativeLuminance(const QColor &c)
{
    const QColor rgb = c.toRgb();
    return luminanceOf(rgb.red(), rgb.green(), rgb.blue());
}

double contrast(const QColor &a, const QColor &b)
{
    return ratio(relativeLuminance(a), relativeLuminance(b));
}

QColor composite(const QColor &top, double alpha, const QColor &bottom)
{
    alpha = std::clamp(alpha, 0.0, 1.0);
    const QColor t = top.toRgb();
    const QColor u = bottom.toRgb();
    const auto ch = [alpha](int a, int b) {
        return int(std::lround(alpha * a + (1.0 - alpha) * b));
    };
    return QColor(ch(t.red(), u.red()), ch(t.green(), u.green()),
                  ch(t.blue(), u.blue()));
}

QColor mix(const QColor &a, const QColor &b, double t)
{
    return composite(b, t, a);
}

double lstar(const QColor &c)
{
    const double y = relativeLuminance(c);
    return y <= 0.008856 ? y * 903.3 : 116.0 * std::cbrt(y) - 16.0;
}

QColor shiftLightness(const QColor &c, double deltaL)
{
    Lab lab = toLab(c.toRgb());
    lab.l = std::clamp(lab.l + deltaL, 0.0, 100.0);
    return fromLab(lab);
}

// ---- images ----------------------------------------------------------------

ImageStats measure(const QImage &image)
{
    ImageStats stats;
    if (image.isNull() || image.width() <= 0 || image.height() <= 0)
        return stats;
    QImage thumb = image;
    if (thumb.width() > kSampleEdge || thumb.height() > kSampleEdge) {
        thumb = thumb.scaled(kSampleEdge, kSampleEdge, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }
    thumb = thumb.convertToFormat(QImage::Format_ARGB32);
    if (thumb.isNull())
        return stats;

    // Dominant colour: a 12-bin hue histogram weighted by saturation, value
    // and alpha; a near-grey picture falls back to its mean colour.
    constexpr int kBins = 12;
    std::array<double, kBins> weight{};
    std::array<std::array<double, 3>, kBins> sum{};
    double meanR = 0, meanG = 0, meanB = 0, meanW = 0;
    double saturatedWeight = 0;
    const int pixels = thumb.width() * thumb.height();
    for (int y = 0; y < thumb.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(thumb.constScanLine(y));
        for (int x = 0; x < thumb.width(); ++x) {
            const QRgb px = row[x];
            const double a = qAlpha(px) / 255.0;
            if (a <= 0.0)
                continue;
            meanR += qRed(px) * a;
            meanG += qGreen(px) * a;
            meanB += qBlue(px) * a;
            meanW += a;
            const QColor c = QColor::fromRgb(px);
            const double s = c.hsvSaturationF();
            const double v = c.valueF();
            const int hue = c.hsvHue();
            if (hue < 0 || s < 0.15 || v < 0.12)
                continue;
            const double w = s * v * a;
            const int bin = std::clamp(hue * kBins / 360, 0, kBins - 1);
            weight[size_t(bin)] += w;
            sum[size_t(bin)][0] += qRed(px) * w;
            sum[size_t(bin)][1] += qGreen(px) * w;
            sum[size_t(bin)][2] += qBlue(px) * w;
            saturatedWeight += w;
        }
    }
    if (meanW <= 0.0)
        return stats;   // fully transparent: nothing to measure
    QColor dominant(int(std::lround(meanR / meanW)), int(std::lround(meanG / meanW)),
                    int(std::lround(meanB / meanW)));
    if (saturatedWeight >= 0.10 * pixels) {
        const auto best = std::max_element(weight.begin(), weight.end());
        const size_t bin = size_t(std::distance(weight.begin(), best));
        if (*best > 0.0) {
            dominant = QColor(int(std::lround(sum[bin][0] / *best)),
                              int(std::lround(sum[bin][1] / *best)),
                              int(std::lround(sum[bin][2] / *best)));
        }
    }
    stats.dominant = dominant;

    // Samples as drawn: a translucent pixel shows the dominant-colour
    // placeholder ChatBackdrop paints beneath the picture.
    stats.samples.reserve(pixels);
    for (int y = 0; y < thumb.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(thumb.constScanLine(y));
        for (int x = 0; x < thumb.width(); ++x) {
            const QRgb px = row[x];
            const QColor drawn = composite(QColor::fromRgb(px), qAlpha(px) / 255.0,
                                           dominant);
            stats.samples.append(drawn.rgb());
        }
    }
    stats.measured = !stats.samples.isEmpty();
    return stats;
}

// ---- the scrim -------------------------------------------------------------

double worstContrastAt(const QColor &scrim, double alpha,
                       const QList<QColor> &inks, const ImageStats &stats)
{
    if (inks.isEmpty())
        return 21.0;
    const QColor s = scrim.toRgb();
    QVector<double> inkLum;
    inkLum.reserve(inks.size());
    for (const QColor &ink : inks)
        inkLum.append(relativeLuminance(ink));

    const QList<QRgb> fallback = worstCaseSamples();
    const bool measured = stats.measured && !stats.samples.isEmpty();
    const int n = measured ? int(stats.samples.size()) : int(fallback.size());
    QVector<double> worst;
    worst.reserve(n);
    const double a = std::clamp(alpha, 0.0, 1.0);
    for (int i = 0; i < n; ++i) {
        const QRgb px = measured ? stats.samples.at(i) : fallback.at(i);
        const int r = int(std::lround(a * s.red() + (1.0 - a) * qRed(px)));
        const int g = int(std::lround(a * s.green() + (1.0 - a) * qGreen(px)));
        const int b = int(std::lround(a * s.blue() + (1.0 - a) * qBlue(px)));
        const double l = luminanceOf(r, g, b);
        double w = 21.0;
        for (double il : inkLum)
            w = std::min(w, ratio(il, l));
        worst.append(w);
    }
    if (worst.isEmpty())
        return 21.0;
    // Specks smaller than one sample are allowed to fail, and only for a
    // measured picture with enough samples to have specks at all. The worst
    // case is never trimmed.
    int skip = 0;
    if (measured && n >= 100)
        skip = int(std::floor(n * kOutlierShare));
    std::nth_element(worst.begin(), worst.begin() + skip, worst.end());
    return worst.at(skip);
}

ScrimPlan planScrim(const QColor &ground, const QList<QColor> &inks,
                    const ImageStats &stats, double requestedTint, double bar)
{
    ScrimPlan plan;
    const QColor base = ground.toRgb();
    const QColor veil = shiftLightness(
        base, isDarkGround(base) ? kVeilLiftDark : kVeilLiftLight);
    double tint = std::clamp(std::isfinite(requestedTint) ? requestedTint : 0.0,
                             0.0, kMaxTint);
    const bool canTint = stats.measured && stats.dominant.isValid();
    if (!canTint)
        tint = 0.0;

    // Largest usable tint first; step it down until the picture can show.
    for (;;) {
        const QColor scrim = tint > 0.0 ? mix(veil, stats.dominant, tint) : veil;
        // The veil alone must carry the inks at full opacity, or no opacity
        // can.
        if (worstContrastAt(scrim, 1.0, inks, stats) >= bar) {
            double floor = 0.0;
            for (int k = 99; k >= 0; --k) {
                const double a = k / 100.0;
                if (worstContrastAt(scrim, a, inks, stats) < bar) {
                    floor = (k + 1) / 100.0;
                    break;
                }
            }
            if (floor <= kMaxScrimAlpha) {
                plan.color = scrim;
                plan.floor = floor;
                plan.tint = tint;
                plan.feasible = true;
                return plan;
            }
        }
        if (tint <= 0.0)
            break;
        tint = std::max(0.0, tint - 0.05);
    }
    // Nothing fits: the ground itself, fully opaque. The picture does not
    // show and the room reads exactly as it would without one.
    plan.color = base;
    plan.floor = 1.0;
    plan.tint = 0.0;
    plan.feasible = false;
    return plan;
}

double scrimAlpha(double floor, double dim)
{
    floor = std::clamp(std::isfinite(floor) ? floor : 1.0, 0.0, 1.0);
    dim = std::clamp(std::isfinite(dim) ? dim : 0.0, 0.0, 1.0);
    if (floor >= kMaxScrimAlpha)
        return floor;
    return floor + dim * (kMaxScrimAlpha - floor);
}

// ---- depth -----------------------------------------------------------------

QStringList depthStops(const QColor &base, bool darkTheme)
{
    const QColor b = base.toRgb();
    if (!b.isValid())
        return {};
    const QColor shifted = shiftLightness(b, darkTheme ? -kDepthDeltaL : kDepthDeltaL);
    const QString flat = b.name(QColor::HexRgb).toUpper();
    const QString moved = shifted.name(QColor::HexRgb).toUpper();
    // Top -> bottom; light from above in both cases.
    return darkTheme ? QStringList{ flat, moved } : QStringList{ moved, flat };
}

// ---- presentation ----------------------------------------------------------

QVariantMap normalisePresentation(const QVariantMap &raw)
{
    const auto unit = [&raw](const char *key, double fallback) {
        const QVariant v = raw.value(QLatin1String(key));
        bool ok = false;
        const double d = v.toDouble(&ok);
        if (!ok || !std::isfinite(d) || v.typeId() == QMetaType::QString)
            return fallback;
        return std::clamp(d, 0.0, 1.0);
    };
    const auto choice = [&raw](const char *key, const QStringList &allowed) {
        const QString v = raw.value(QLatin1String(key)).toString();
        return allowed.contains(v) ? v : allowed.first();
    };
    QVariantMap out;
    out.insert(QStringLiteral("dim"), unit("dim", 0.2));
    out.insert(QStringLiteral("blur"), unit("blur", 0.0));
    out.insert(QStringLiteral("tint"), unit("tint", 0.25));
    out.insert(QStringLiteral("fit"),
               choice("fit", { QStringLiteral("cover"), QStringLiteral("contain"),
                               QStringLiteral("tile") }));
    out.insert(QStringLiteral("align"),
               choice("align", { QStringLiteral("center"), QStringLiteral("top"),
                                 QStringLiteral("bottom") }));
    return out;
}

QVariantMap presentationToWire(const QVariantMap &unit)
{
    QVariantMap clean = normalisePresentation(unit);
    for (const char *key : { "dim", "blur", "tint" }) {
        const QString k = QLatin1String(key);
        clean.insert(k, int(std::lround(clean.value(k).toDouble() * 100.0)));
    }
    return clean;
}

QVariantMap presentationFromWire(const QVariantMap &wire)
{
    QVariantMap unit;
    const auto pct = [&wire](const char *key, int fallback) {
        const QVariant v = wire.value(QLatin1String(key));
        bool ok = false;
        const double d = v.toDouble(&ok);
        if (!ok || !std::isfinite(d) || v.typeId() == QMetaType::QString)
            return fallback / 100.0;
        return std::clamp(std::round(d), 0.0, 100.0) / 100.0;
    };
    unit.insert(QStringLiteral("dim"), pct("dim", 20));
    unit.insert(QStringLiteral("blur"), pct("blur", 0));
    unit.insert(QStringLiteral("tint"), pct("tint", 25));
    unit.insert(QStringLiteral("fit"), wire.value(QStringLiteral("fit")));
    unit.insert(QStringLiteral("align"), wire.value(QStringLiteral("align")));
    return normalisePresentation(unit);
}

// ---- precedence ------------------------------------------------------------

Resolved resolve(const QString &roomId, const ResolveInput &in)
{
    Resolved out;
    if (roomId.isEmpty())
        return out;
    if (!in.personalRoom.isEmpty()) {
        out.source = QStringLiteral("personal-room");
        out.record = in.personalRoom;
        return out;
    }
    const bool shared = in.showShared && !in.roomHidden;
    if (shared && !in.sharedRoom.isEmpty()) {
        out.source = QStringLiteral("room");
        out.scopeId = roomId;
        out.record = in.sharedRoom;
        return out;
    }
    if (shared) {
        for (const auto &space : in.sharedSpaces) {
            if (space.second.isEmpty())
                continue;
            out.source = QStringLiteral("space");
            out.scopeId = space.first;
            out.record = space.second;
            return out;
        }
    }
    if (!in.personalDefault.isEmpty()) {
        out.source = QStringLiteral("personal-default");
        out.record = in.personalDefault;
        return out;
    }
    return out;
}

// ---- Spaces ----------------------------------------------------------------

QStringList spaceChain(const QString &roomId,
                       const QHash<QString, QStringList> &childrenOf,
                       const QStringList &spaceOrder,
                       const QString &activeSpaceId)
{
    if (roomId.isEmpty())
        return {};
    const auto parentsOf = [&](const QString &id) {
        QStringList parents;
        for (const QString &space : spaceOrder) {
            if (space != id && childrenOf.value(space).contains(id))
                parents.append(space);
        }
        return parents;
    };
    // Ancestors of `id`, nearest first, one parent per level (the first in
    // spaceOrder), cycle-safe and bounded.
    const auto ancestors = [&](const QString &id) {
        QStringList chain;
        QSet<QString> seen{ id };
        QString cursor = id;
        for (int depth = 0; depth < kMaxSpaceDepth; ++depth) {
            const QStringList parents = parentsOf(cursor);
            QString next;
            for (const QString &p : parents) {
                if (!seen.contains(p)) {
                    next = p;
                    break;
                }
            }
            if (next.isEmpty())
                break;
            seen.insert(next);
            chain.append(next);
            cursor = next;
        }
        return chain;
    };

    const QStringList direct = parentsOf(roomId);
    if (direct.isEmpty())
        return {};
    QString chosen = direct.first();
    if (!activeSpaceId.isEmpty()) {
        for (const QString &parent : direct) {
            if (parent == activeSpaceId
                || ancestors(parent).contains(activeSpaceId)) {
                chosen = parent;
                break;
            }
        }
    }
    QStringList chain{ chosen };
    for (const QString &a : ancestors(chosen)) {
        if (a == roomId || chain.contains(a))
            break;
        chain.append(a);
        if (chain.size() > kMaxSpaceDepth)
            break;
    }
    return chain;
}

} // namespace backdrop
