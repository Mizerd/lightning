#include "app/StormBandPainter.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPen>
#include <QPolygon>
#include <QRadialGradient>
#include <QRegularExpression>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <random>

namespace stormband {

Layer layerFromName(const QString &name)
{
    if (name == QLatin1String("sky"))
        return Layer::Sky;
    if (name == QLatin1String("farhills"))
        return Layer::FarHills;
    if (name == QLatin1String("mist"))
        return Layer::Mist;
    if (name == QLatin1String("midhills"))
        return Layer::MidHills;
    if (name == QLatin1String("spires"))
        return Layer::Spires;
    if (name == QLatin1String("ridge"))
        return Layer::Ridge;
    if (name == QLatin1String("rain"))
        return Layer::Rain;
    if (name == QLatin1String("bolt"))
        return Layer::Bolt;
    if (name == QLatin1String("bloom"))
        return Layer::Bloom;
    if (name == QLatin1String("streak"))
        return Layer::Streak;
    return Layer::Unknown;
}

namespace {

bool parseHex6(const QString &s, QColor *out)
{
    static const QRegularExpression re(QStringLiteral("^[0-9A-Fa-f]{6}$"));
    if (!re.match(s).hasMatch())
        return false;
    bool ok = false;
    const uint v = s.toUInt(&ok, 16);
    if (!ok)
        return false;
    *out = QColor::fromRgb(int((v >> 16) & 0xFF), int((v >> 8) & 0xFF),
                           int(v & 0xFF));
    return true;
}

} // namespace

bool parseId(const QString &id, SceneParams *out)
{
    if (!out)
        return false;

    const int qIdx = id.indexOf(QLatin1Char('?'));
    const QString path = qIdx >= 0 ? id.left(qIdx) : id;
    const QString query = qIdx >= 0 ? id.mid(qIdx + 1) : QString();

    const QStringList parts =
        path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() != 2)
        return false;

    const Layer layer = layerFromName(parts.at(0));
    if (layer == Layer::Unknown)
        return false;

    const QStringList dims = parts.at(1).split(QLatin1Char('x'));
    if (dims.size() != 2)
        return false;
    bool wOk = false;
    bool hOk = false;
    const int w = dims.at(0).toInt(&wOk);
    const int h = dims.at(1).toInt(&hOk);
    if (!wOk || !hOk || w <= 0 || h <= 0 || w > kMaxDimension
        || h > kMaxDimension) {
        return false;
    }

    if (query.isEmpty())
        return false;
    const QUrlQuery q(query);
    if (!q.hasQueryItem(QStringLiteral("dark"))
        || !q.hasQueryItem(QStringLiteral("base"))
        || !q.hasQueryItem(QStringLiteral("accent"))) {
        return false;
    }

    const QString darkStr = q.queryItemValue(QStringLiteral("dark"));
    if (darkStr != QLatin1String("0") && darkStr != QLatin1String("1"))
        return false;

    QColor base;
    QColor accent;
    if (!parseHex6(q.queryItemValue(QStringLiteral("base")), &base))
        return false;
    if (!parseHex6(q.queryItemValue(QStringLiteral("accent")), &accent))
        return false;

    quint32 seed = 1;
    if (q.hasQueryItem(QStringLiteral("seed"))) {
        bool ok = false;
        const quint32 s = q.queryItemValue(QStringLiteral("seed")).toUInt(&ok);
        if (!ok)
            return false;
        seed = s;
    }

    out->layer = layer;
    out->size = QSize(w, h);
    out->dark = (darkStr == QLatin1String("1"));
    out->base = base;
    out->accent = accent;
    out->seed = seed;
    return true;
}

namespace {

// ---- Small colour helpers. Every one operates only on the two inputs
// (`base`/`accent`) plus generic algorithmic constants (a violet hue
// reference, a warm ember reference) that are NOT theme literals — they are
// blended WITH the theme colour, never used in place of it. ----

QColor mix(const QColor &a, const QColor &b, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgb(
        int(std::lround(a.red() * (1.0 - t) + b.red() * t)),
        int(std::lround(a.green() * (1.0 - t) + b.green() * t)),
        int(std::lround(a.blue() * (1.0 - t) + b.blue() * t)));
}

QColor withAlpha(QColor c, int alpha)
{
    c.setAlpha(std::clamp(alpha, 0, 255));
    return c;
}

// ---- Deterministic PRNG. Seeded purely from the parsed request plus a
// caller-chosen salt (used to decorrelate layers sharing one `seed`) — never
// QRandomGenerator::global(), never a time source. ----

std::mt19937 makeRng(const SceneParams &p, quint32 salt)
{
    quint32 s = p.seed * 2654435761u;
    s ^= salt * 40503u;
    s ^= quint32(p.size.width()) * 2246822519u;
    s ^= quint32(p.size.height()) * 3266489917u;
    s ^= p.dark ? 0x9E3779B9u : 0x85EBCA6Bu;
    s ^= quint32(p.base.rgb());
    s ^= quint32(p.accent.rgb()) << 1;
    return std::mt19937(s);
}

int randInt(std::mt19937 &rng, int lo, int hi)
{
    if (hi <= lo)
        return lo;
    return std::uniform_int_distribution<int>(lo, hi)(rng);
}

constexpr int kArtScale = 2; // 1 art pixel == 2 logical pixels.

QSize artSize(const QSize &logical)
{
    return QSize(std::max(1, logical.width() / kArtScale),
                std::max(1, logical.height() / kArtScale));
}

QImage blankArtCanvas(const QSize &art)
{
    QImage img(art, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    return img;
}

// ---- Layer painters. All work at ART resolution with antialiasing off so
// the nearest-neighbour upscale in renderLayer() stays crisp. ----

void paintSky(QPainter &p, const QSize &art, const Palette &pal,
             const SceneParams &params, std::mt19937 &rng)
{
    QLinearGradient grad(0, 0, 0, art.height());
    grad.setColorAt(0.0, pal.skyTop);
    grad.setColorAt(0.7, pal.skyBottom);
    grad.setColorAt(1.0, mix(pal.skyBottom, pal.horizonGlow, 0.5));
    p.fillRect(QRect(QPoint(0, 0), art), grad);

    // Nebula dithering — sparse translucent flecks in the upper reaches.
    const int nebulaCount = std::max(6, art.width() / 6);
    for (int i = 0; i < nebulaCount; ++i) {
        const int x = randInt(rng, 0, art.width() - 1);
        const int y = randInt(rng, 0, int(art.height() * 0.65));
        p.fillRect(QRect(x, y, 1, 1),
                  withAlpha(pal.nebula, randInt(rng, 10, 45)));
    }

    if (params.dark) {
        // Stars.
        const int starCount = std::max(10, art.width() / 3);
        for (int i = 0; i < starCount; ++i) {
            const int x = randInt(rng, 0, art.width() - 1);
            const int y = randInt(rng, 0, int(art.height() * 0.78));
            p.fillRect(QRect(x, y, 1, 1),
                      withAlpha(pal.starOrDust, randInt(rng, 60, 235)));
        }
    } else {
        // Daylight dust motes instead of stars.
        const int dustCount = std::max(6, art.width() / 8);
        for (int i = 0; i < dustCount; ++i) {
            const int x = randInt(rng, 0, art.width() - 1);
            const int y = randInt(rng, 0, int(art.height() * 0.85));
            p.fillRect(QRect(x, y, 1, 1),
                      withAlpha(pal.starOrDust, randInt(rng, 20, 90)));
        }
    }

    // Moon.
    const int moonR = std::max(3, art.height() / 11);
    const int moonX =
        randInt(rng, art.width() / 6, std::max(art.width() / 6 + 1,
                                              art.width() - art.width() / 6));
    const int moonY = randInt(rng, moonR + 1, std::max(moonR + 2, art.height() / 3));
    p.setPen(Qt::NoPen);
    p.setBrush(pal.moonBody);
    p.drawEllipse(QPoint(moonX, moonY), moonR, moonR);
    p.setBrush(withAlpha(pal.skyTop, 90));
    p.drawEllipse(QPoint(moonX - moonR / 3, moonY - moonR / 4),
                 std::max(1, moonR / 3), std::max(1, moonR / 3));
}

void paintHillSilhouette(QPainter &p, const QSize &art, const QColor &fill,
                         std::mt19937 &rng, int baseline, int amplitude,
                         int segments)
{
    segments = std::max(2, segments);
    QPolygon poly;
    poly << QPoint(0, art.height());
    int prevY = baseline;
    for (int i = 0; i <= segments; ++i) {
        const int x = art.width() * i / segments;
        const int jitter = randInt(rng, -amplitude, amplitude);
        int y = std::clamp(baseline + jitter, 1, art.height() - 1);
        y = (y + prevY) / 2; // smooth so adjacent peaks don't look chaotic
        prevY = y;
        poly << QPoint(x, y);
    }
    poly << QPoint(art.width(), art.height());
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawPolygon(poly);
}

void paintMist(QPainter &p, const QSize &art, const Palette &pal,
              std::mt19937 &rng)
{
    QLinearGradient grad(0, 0, 0, art.height());
    grad.setColorAt(0.0, withAlpha(pal.mistTint, 0));
    grad.setColorAt(0.5, withAlpha(pal.mistTint, 90));
    grad.setColorAt(1.0, withAlpha(pal.mistTint, 30));
    p.fillRect(QRect(QPoint(0, 0), art), grad);

    const int puffs = std::max(3, art.width() / 60);
    p.setPen(Qt::NoPen);
    for (int i = 0; i < puffs; ++i) {
        const int w = randInt(rng, std::max(2, art.width() / 10),
                              std::max(3, art.width() / 5));
        const int h = randInt(rng, std::max(2, art.height() / 3),
                              std::max(3, art.height()));
        const int x = randInt(rng, 0, art.width() - 1);
        const int y = randInt(rng, 0, std::max(1, art.height() - h / 2));
        p.setBrush(withAlpha(pal.mistTint, randInt(rng, 20, 55)));
        p.drawEllipse(QPoint(x, y), w / 2, h / 2);
    }
}

void paintSpires(QPainter &p, const QSize &art, const Palette &pal,
                 std::mt19937 &rng, bool dark)
{
    int x = 0;
    while (x < art.width()) {
        const int w = randInt(rng, art.width() / 22 + 1, art.width() / 12 + 2);
        const int h = randInt(rng, std::max(2, art.height() / 3), art.height());
        const int y = art.height() - h;
        p.setPen(Qt::NoPen);
        p.setBrush(pal.spireBody);
        p.drawRect(QRect(x, y, w, h));
        if (randInt(rng, 0, 2) == 0) {
            QPolygon cap;
            cap << QPoint(x, y)
                << QPoint(x + w / 2, std::max(0, y - randInt(rng, 3, 8)))
                << QPoint(x + w, y);
            p.drawPolygon(cap);
        }
        const int lit = dark ? randInt(rng, 1, 4) : randInt(rng, 0, 2);
        for (int i = 0; i < lit; ++i) {
            const int wx = x + randInt(rng, 1, std::max(1, w - 2));
            const int wy = y + randInt(rng, 2, std::max(2, h - 2));
            p.fillRect(QRect(wx, wy, 1, 1),
                      withAlpha(pal.spireGlow, randInt(rng, 120, 230)));
        }
        x += w + randInt(rng, std::max(2, art.width() / 30),
                         std::max(3, art.width() / 14));
    }
}

void paintRidge(QPainter &p, const QSize &art, const Palette &pal,
               std::mt19937 &rng)
{
    paintHillSilhouette(p, art, pal.ridgeBody, rng, art.height() * 2 / 5,
                        std::max(1, art.height() / 3),
                        std::max(6, art.width() / 40));

    const int boulders = std::max(2, art.width() / 90);
    p.setPen(Qt::NoPen);
    for (int i = 0; i < boulders; ++i) {
        const int r = randInt(rng, 2, 5);
        const int x = randInt(rng, 0, art.width() - 1);
        const int y = art.height() - randInt(rng, 0, std::max(1, art.height() / 4)) - r;
        p.setBrush(pal.ridgeBody.darker(115));
        p.drawEllipse(QPoint(x, y), r, r);
    }

    const int trees = std::max(1, art.width() / 220);
    for (int i = 0; i < trees; ++i) {
        const int x = randInt(rng, 0, art.width() - 1);
        const int baseY = art.height() - randInt(rng, 0, std::max(1, art.height() / 5));
        const int trunkH = randInt(rng, 6, 14);
        p.setPen(QPen(pal.ridgeBody.darker(140), 1));
        p.drawLine(x, baseY, x, baseY - trunkH);
        p.drawLine(x, baseY - trunkH, x - 3, baseY - trunkH - 3);
        p.drawLine(x, baseY - trunkH, x + 3, baseY - trunkH - 4);
    }

    const int shards = std::max(1, art.width() / 160);
    p.setPen(Qt::NoPen);
    for (int i = 0; i < shards; ++i) {
        const int x = randInt(rng, 0, art.width() - 1);
        const int y = art.height() - randInt(rng, 0, std::max(1, art.height() / 5));
        const int h = randInt(rng, 3, 7);
        QPolygon tri;
        tri << QPoint(x, y) << QPoint(x - 2, y - h) << QPoint(x + 2, y - h);
        p.setBrush(withAlpha(pal.ridgeAccent, randInt(rng, 150, 230)));
        p.drawPolygon(tri);
    }

    const int tufts = std::max(3, art.width() / 40);
    for (int i = 0; i < tufts; ++i) {
        const int x = randInt(rng, 0, art.width() - 1);
        const int y = art.height() - randInt(rng, 0, std::max(1, art.height() / 6));
        p.setPen(QPen(pal.ridgeBody.darker(120), 1));
        p.drawLine(x, y, x - 1, y - 3);
        p.drawLine(x, y, x, y - 4);
        p.drawLine(x, y, x + 1, y - 3);
    }
}

void paintRain(QPainter &p, const QSize &art, const Palette &pal,
              std::mt19937 &rng)
{
    const int streaks = std::max(8, (art.width() * art.height()) / 90);
    for (int i = 0; i < streaks; ++i) {
        const int x = randInt(rng, 0, art.width() - 1);
        const int y = randInt(rng, 0, art.height() - 1);
        const int len = randInt(rng, 2, 5);
        p.setPen(QPen(withAlpha(pal.rainInk, randInt(rng, 70, 110)), 1));
        p.drawLine(x, y, x - 1, y + len);
    }
}

void paintBolt(QPainter &p, const QSize &art, const Palette &pal,
              std::mt19937 &rng)
{
    QPolygon spine;
    int x = art.width() / 2;
    spine << QPoint(x, 0);
    const int steps = std::max(3, art.height() / 6);
    for (int i = 1; i <= steps; ++i) {
        const int y = art.height() * i / steps;
        x = std::clamp(x + randInt(rng, -std::max(1, art.width() / 3),
                                  std::max(1, art.width() / 3)),
                      1, std::max(1, art.width() - 2));
        spine << QPoint(x, y);
    }
    p.setBrush(Qt::NoBrush);

    QPen glowPen(withAlpha(pal.boltGlow, 130), std::max(2, art.width() / 6));
    glowPen.setCapStyle(Qt::RoundCap);
    glowPen.setJoinStyle(Qt::RoundJoin);
    p.setPen(glowPen);
    p.drawPolyline(spine);

    QPen corePen(pal.boltCore, std::max(1, art.width() / 14));
    corePen.setCapStyle(Qt::RoundCap);
    corePen.setJoinStyle(Qt::RoundJoin);
    p.setPen(corePen);
    p.drawPolyline(spine);
}

void paintBloom(QPainter &p, const QSize &art, const Palette &pal)
{
    QRadialGradient grad(art.width() / 2.0, art.height() / 2.0,
                         std::max(art.width(), art.height()) / 2.0);
    grad.setColorAt(0.0, withAlpha(pal.boltGlow, 150));
    grad.setColorAt(0.6, withAlpha(pal.boltGlow, 45));
    grad.setColorAt(1.0, withAlpha(pal.boltGlow, 0));
    p.fillRect(QRect(QPoint(0, 0), art), grad);
}

void paintStreak(QPainter &p, const QSize &art, const Palette &pal)
{
    QLinearGradient grad(0, art.height(), art.width(), 0);
    grad.setColorAt(0.0, withAlpha(pal.starOrDust, 0));
    grad.setColorAt(0.75, withAlpha(pal.starOrDust, 120));
    grad.setColorAt(1.0, withAlpha(mix(pal.starOrDust, Qt::white, 0.6), 235));
    QPen pen(QBrush(grad), std::max(1, art.height() / 3));
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.drawLine(0, art.height() - 1, art.width() - 1, 0);
}

} // namespace

Palette derivePalette(const QColor &base, const QColor &accent, bool dark)
{
    Palette p;
    if (dark) {
        // Cool violet-tinted dark scenes with only a hint of accent. The
        // violet/ember reference tones below are fixed ALGORITHM constants
        // (see the file header) — every field still traces back to `base`
        // and `accent`, it is never returned unmixed.
        const QColor violet(88, 74, 160);
        p.skyTop = mix(base.darker(220), violet, 0.22);
        p.skyBottom = mix(base.darker(140), violet, 0.12);
        p.horizonGlow = mix(base, accent, 0.30);
        p.farHill = mix(base.darker(260), violet, 0.18);
        p.midHill = mix(base.darker(190), violet, 0.14);
        p.spireBody = base.darker(230);
        p.spireGlow = mix(accent, base, 0.15);
        p.ridgeBody = base.darker(320);
        p.ridgeAccent = mix(accent, base, 0.35);
        p.mistTint = mix(base, violet, 0.25);
        p.rainInk = mix(base.lighter(160), Qt::white, 0.2);
        p.starOrDust = mix(base.lighter(320), Qt::white, 0.6);
        p.moonBody = mix(base.lighter(260), Qt::white, 0.5);
        p.nebula = mix(accent, violet, 0.5);
        p.boltCore = accent.lighter(115);
        p.boltGlow = mix(accent, Qt::white, 0.4);
    } else {
        // Daylight treatment: everything lightens toward the base, with a
        // soft accent-tinted horizon and dust instead of stars.
        p.skyTop = mix(base.lighter(115), QColor(220, 235, 250), 0.35);
        p.skyBottom = base.lighter(108);
        p.horizonGlow = mix(base, accent, 0.22);
        p.farHill = mix(base.darker(115), accent, 0.05);
        p.midHill = mix(base.darker(130), accent, 0.08);
        p.spireBody = base.darker(150);
        p.spireGlow = mix(accent, base, 0.30);
        p.ridgeBody = base.darker(180);
        p.ridgeAccent = mix(accent, base, 0.40);
        p.mistTint = mix(base, Qt::white, 0.55);
        p.rainInk = mix(base.darker(115), Qt::white, 0.3);
        p.starOrDust = mix(base.darker(120), accent, 0.25);
        p.moonBody = mix(base.lighter(180), Qt::white, 0.6);
        p.nebula = mix(accent, Qt::white, 0.7);
        p.boltCore = accent.lighter(105);
        p.boltGlow = mix(accent, Qt::white, 0.5);
    }
    return p;
}

QImage renderLayer(const SceneParams &params)
{
    if (params.layer == Layer::Unknown)
        return {};
    if (params.size.width() <= 0 || params.size.height() <= 0
        || params.size.width() > kMaxDimension
        || params.size.height() > kMaxDimension) {
        return {};
    }

    const QSize art = artSize(params.size);
    QImage canvas = blankArtCanvas(art);
    QPainter p(&canvas);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);

    const Palette pal = derivePalette(params.base, params.accent, params.dark);
    auto rng = makeRng(params, quint32(params.layer) + 100);

    switch (params.layer) {
    case Layer::Sky:
        paintSky(p, art, pal, params, rng);
        break;
    case Layer::FarHills:
        paintHillSilhouette(p, art, pal.farHill, rng, art.height() * 3 / 5,
                            std::max(1, art.height() / 4),
                            std::max(5, art.width() / 26));
        break;
    case Layer::Mist:
        paintMist(p, art, pal, rng);
        break;
    case Layer::MidHills:
        paintHillSilhouette(p, art, pal.midHill, rng, art.height() / 2,
                            std::max(1, art.height() / 3),
                            std::max(6, art.width() / 20));
        break;
    case Layer::Spires:
        paintSpires(p, art, pal, rng, params.dark);
        break;
    case Layer::Ridge:
        paintRidge(p, art, pal, rng);
        break;
    case Layer::Rain:
        paintRain(p, art, pal, rng);
        break;
    case Layer::Bolt:
        paintBolt(p, art, pal, rng);
        break;
    case Layer::Bloom:
        paintBloom(p, art, pal);
        break;
    case Layer::Streak:
        paintStreak(p, art, pal);
        break;
    case Layer::Unknown:
        break;
    }
    p.end();

    // Nearest-neighbour upscale back to the logical size QML asked for —
    // this is what keeps the art crisp/pixelated instead of blurred.
    QImage out = canvas.scaled(params.size, Qt::IgnoreAspectRatio,
                               Qt::FastTransformation);
    return out.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

} // namespace stormband

StormBandImageProvider::StormBandImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage StormBandImageProvider::requestImage(const QString &id, QSize *size,
                                            const QSize & /*requestedSize*/)
{
    if (size)
        *size = QSize();
    stormband::SceneParams params;
    if (!stormband::parseId(id, &params))
        return {};
    const QImage img = stormband::renderLayer(params);
    if (size)
        *size = img.size();
    return img;
}
