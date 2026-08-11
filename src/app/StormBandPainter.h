#pragma once

#include <QColor>
#include <QImage>
#include <QQuickImageProvider>
#include <QString>

// The Storm Band tile/sprite painter — an EXACT port of the reference
// implementation in storm-band-export.html (the maintainer-supplied,
// dependency-free HTML export). Every painter reproduces the reference's
// canvas code 1:1: the same mulberry32-style PRNG with the same seeds, the
// same tile source dimensions (66px tall, rendered at 2x by QML), the same
// per-pixel dithering conditions, and the same derivePalette() color math,
// so a given (background, accent) pair produces the same scene as the HTML.
//
// QImage + QPainter only (pure software) — works under the offscreen test
// platform and never touches the GPU at generation time. Tiles regenerate
// only when the theme inputs change (the provider URL changes).
namespace stormband {

// Field names match the reference's palette object 1:1.
struct Palette {
    bool light = false;
    QColor sky1, sky2, sky3;
    QColor star[5];
    QColor starGlow, neb1, neb2, mist1, mist2;
    QColor farFill, farRim, midFill, midRim;
    QColor ink, inkLine, inkLit, rune1, rune2;
    QColor frontFill, frontRim, rain1, rain2;
    QColor moon1, moon2, moonHalo;
    QColor boltCore, boltMid, boltDark, boltHaze1, boltHaze2, boltOut;
    QColor horizon, glowA, glowB, glowC;
    QColor ember1, ember2, ember3;
};

// Exact port of the reference's derivePalette(bg, accent).
Palette derivePalette(const QColor &bg, const QColor &accent);

// The reference's PRNG (rnd(seed) — a mulberry32 variant). Public so the
// determinism tests can pin the exact sequence.
class Rnd
{
public:
    explicit Rnd(quint32 seed) : m_s(seed) {}
    double operator()()
    {
        m_s += 0x6D2B79F5u;
        quint32 t = (m_s ^ (m_s >> 15)) * (m_s | 1u);
        t = (t + ((t ^ (t >> 7)) * (t | 61u))) ^ t;
        return double(t ^ (t >> 14)) / 4294967296.0;
    }

private:
    quint32 m_s;
};

// Source pixel height of every scrolling tile (rendered at 2x — the art is
// authored at 1 art px = 2 css px).
inline constexpr int kTileH = 66;

// Pixel-art painters — exact ports (name-for-name) of the HTML's painters.
QImage skyTile(const Palette &P);                       // 768 x 66
QImage mistTile(const Palette &P);                      // 545 x 66
QImage hillTile(const Palette &P, int W, double base, double amp,
                const QColor &fill, const QColor &rim, quint32 seed,
                int peaks);                             // far 331 / mid 397
QImage spireTile(const Palette &P);                     // 641 x 66
QImage frontTile(const Palette &P);                     // 719 x 66
QImage rainTile(const Palette &P);                      // 160 x 66
QImage moonSprite(const Palette &P);                    // 18 x 18
QImage shootingStarSprite(const Palette &P);            // 30 x 12
// 46x74 art in a 62x90 canvas: an 8px padding ring carries the baked
// equivalent of the reference's CSS drop-shadow glows.
QImage boltSprite(const Palette &P, quint32 seed);
inline constexpr int kBoltPad = 8;

// The reference's CSS-gradient (non-pixel-art) surfaces, rendered smooth.
QImage skyGradient(const Palette &P);      // 8 x 190: transparent->sky1->sky2->sky3
QImage horizonGradient(const Palette &P);  // 8 x 70: horizon color -> transparent (upward)
QImage bloomSprite(const QColor &glow);    // 340 x 240 radial: glow -> transparent at 70%
QImage emberDot(const QColor &color);      // 2 x 2

// image://storm-band/<layer>?bg=RRGGBB&accent=RRGGBB
// Layers: gradient, sky, mist, far, mid, spire, front, rain, moon, shoot,
// bolt1..bolt6, bloomA, bloomB, bloomC, horizon, ember1..ember3.
// Malformed ids yield a null image (never a crash, never a placeholder).
QImage imageForId(const QString &id);

} // namespace stormband

// Serves the painters above. Stateless: theme colors and the layer name
// travel in the image id, so a theme change regenerates via new URLs.
class StormBandImageProvider : public QQuickImageProvider
{
public:
    StormBandImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;
};
