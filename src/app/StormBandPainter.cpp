#include "app/StormBandPainter.h"

#include <QPainter>
#include <QRadialGradient>
#include <QUrlQuery>
#include <QVector>

#include <cmath>

// Exact port of storm-band-export.html's <script id="storm-band"> painters.
// Comments quote the reference where the code would otherwise look odd —
// several loops deliberately re-evaluate the PRNG inside their condition
// because the JS source does, and the sequence must match bit-for-bit.

namespace stormband {

namespace {

// --- palette helpers (hexA/hexS/mix/rgba/lum ports) ------------------------

int clamp255(double v)
{
    return int(std::max(0.0, std::min(255.0, std::round(v))));
}

// mix(): components are ROUNDED at every call, exactly like the reference's
// hexS() round-trip — nested mixes re-quantize at each level.
QColor mixc(const QColor &a, const QColor &b, double t)
{
    return QColor(clamp255(a.red() + (b.red() - a.red()) * t),
                  clamp255(a.green() + (b.green() - a.green()) * t),
                  clamp255(a.blue() + (b.blue() - a.blue()) * t));
}

QColor rgba(const QColor &c, double a)
{
    QColor out = c;
    out.setAlphaF(a);
    return out;
}

double lum(const QColor &c)
{
    return (0.2126 * c.red() + 0.7152 * c.green() + 0.0722 * c.blue()) / 255.0;
}

QImage makeCanvas(int w, int h)
{
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    return img;
}

// canvas fillRect with globalAlpha: source-over with the alpha folded in.
void fillRect(QPainter &p, int x, int y, int w, int h, const QColor &c,
              double globalAlpha = 1.0)
{
    QColor col = c;
    col.setAlphaF(col.alphaF() * globalAlpha);
    p.fillRect(x, y, w, h, col);
}

// Two-pass box blur over an alpha plane (for the baked bolt glows).
void boxBlurAlpha(QVector<float> &a, int w, int h, int r)
{
    if (r <= 0)
        return;
    QVector<float> tmp(a.size());
    const float norm = 1.0f / (2 * r + 1);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float sum = 0;
            for (int k = -r; k <= r; ++k) {
                const int xx = qBound(0, x + k, w - 1);
                sum += a[y * w + xx];
            }
            tmp[y * w + x] = sum * norm;
        }
    }
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) {
            float sum = 0;
            for (int k = -r; k <= r; ++k) {
                const int yy = qBound(0, y + k, h - 1);
                sum += tmp[yy * w + x];
            }
            a[y * w + x] = sum * norm;
        }
    }
}

} // namespace

Palette derivePalette(const QColor &bg, const QColor &ac)
{
    Palette P;
    const bool light = lum(bg) > 0.5;
    P.light = light;
    const QColor up = light ? QColor("#FFFFFF") : QColor("#C9D2F2");
    // "Scene tint: a cool violet base with only a hint of the theme accent."
    const QColor tint =
        mixc(light ? QColor("#4A4670") : QColor("#6B5AA8"), ac, 0.22);
    const QColor ink = mixc(
        mixc(light ? QColor("#241B33") : QColor("#07040F"), tint, 0.18), ac,
        0.05);
    P.sky1 = light ? mixc(bg, tint, 0.12) : mixc(mixc(bg, tint, 0.16), up, 0.08);
    P.sky2 = light ? mixc(bg, tint, 0.22) : mixc(mixc(bg, tint, 0.26), up, 0.14);
    P.sky3 = light ? mixc(bg, ink, 0.3) : mixc(mixc(bg, tint, 0.34), up, 0.18);
    const QColor far =
        light ? mixc(bg, ink, 0.46) : mixc(mixc(bg, tint, 0.42), up, 0.12);
    const QColor mid =
        light ? mixc(bg, ink, 0.66) : mixc(mixc(bg, ink, 0.34), tint, 0.18);
    const QColor front = light ? mixc(bg, ink, 0.9) : mixc(bg, ink, 0.62);
    const double rimK = light ? 0.16 : 0.34;

    if (light) {
        P.star[0] = mixc(P.sky2, ink, .55);
        P.star[1] = mixc(P.sky2, ink, .4);
        P.star[2] = mixc(P.sky2, ac, .5);
        P.star[3] = mixc(P.sky2, ink, .3);
        P.star[4] = mixc(P.sky2, ink, .2);
    } else {
        P.star[0] = QColor("#FFFDF0");
        P.star[1] = mixc(tint, QColor("#FFFFFF"), .62);
        P.star[2] = QColor("#F2F4FF");
        P.star[3] = mixc(P.sky3, up, .55);
        P.star[4] = mixc(P.sky3, up, .28);
    }
    P.starGlow = light ? rgba(ink, .16)
                       : rgba(mixc(tint, QColor("#FFFFFF"), .6), .4);
    P.neb1 = rgba(mixc(tint, up, .45), light ? .045 : .055);
    P.neb2 = rgba(mixc(tint, ink, .35), light ? .04 : .05);
    P.mist1 = rgba(light ? QColor("#FFFFFF") : mixc(up, tint, .3),
                   light ? .5 : .11);
    P.mist2 = rgba(light ? mixc(QColor("#FFFFFF"), tint, .25)
                         : mixc(tint, up, .5),
                   light ? .34 : .08);
    P.farFill = far;
    P.farRim = mixc(far, light ? QColor("#FFFFFF") : up, rimK);
    P.midFill = mid;
    P.midRim = mixc(mid, light ? QColor("#FFFFFF") : tint, rimK * .8);
    P.ink = front;
    P.inkLine = mixc(front, ink, .6);
    P.inkLit = mixc(front, light ? QColor("#FFFFFF") : tint, light ? .12 : .3);
    P.rune1 = mixc(ac, QColor("#FFFFFF"), .35);
    P.rune2 = mixc(ac, up, .2);
    P.frontFill = mixc(front, ink, .35);
    P.frontRim = mixc(front, light ? QColor("#FFFFFF") : up, light ? .1 : .2);
    P.rain1 = rgba(light ? mixc(ink, tint, .35)
                         : mixc(tint, QColor("#FFFFFF"), .7),
                   light ? .34 : .55);
    P.rain2 = rgba(light ? ink : mixc(P.sky3, up, .5), light ? .24 : .5);
    P.moon1 = light ? mixc(mixc(ac, tint, .4), QColor("#FFFFFF"), .66)
                    : mixc(up, QColor("#FFFFFF"), .5);
    P.moon2 = light ? mixc(mixc(ac, tint, .4), QColor("#FFFFFF"), .45)
                    : mixc(up, P.sky3, .4);
    P.moonHalo = rgba(light ? ac : up, light ? .16 : .28);
    P.boltCore = light ? QColor("#FFFFFF") : QColor("#FFFDF0");
    P.boltMid = mixc(QColor("#FFD447"), ac, .3);
    P.boltDark = mixc(mixc(QColor("#FFD447"), ac, .3), ink, light ? .55 : .42);
    P.boltHaze1 = rgba(mixc(QColor("#FFD447"), ac, .4), light ? .16 : .1);
    P.boltHaze2 = rgba(mixc(QColor("#F2712C"), ac, .3), light ? .3 : .22);
    P.boltOut = rgba(ink, light ? .55 : .3);
    P.horizon = rgba(light ? ink : mixc(tint, ink, .35), light ? .16 : .34);
    P.glowA = rgba(mixc(QColor("#FFD447"), ac, .5), light ? .2 : .28);
    P.glowB = rgba(mixc(ac, up, .4), light ? .22 : .34);
    P.glowC = rgba(mixc(ac, QColor("#FFFFFF"), .4), light ? .18 : .3);
    P.ember1 = mixc(QColor("#FFD447"), ac, .4);
    P.ember2 = mixc(ac, up, .35);
    P.ember3 = mixc(ac, QColor("#FFFFFF"), .35);
    return P;
}

QImage skyTile(const Palette &P)
{
    const int W = 768;
    QImage img = makeCanvas(W, kTileH);
    QPainter p(&img);
    Rnd r(11);
    for (int i = 0; i < 7; ++i) {
        const double nx = r() * W;
        const double ny = 4 + r() * 30;
        const double rad = 26 + r() * 46;
        const QColor col = r() > .5 ? P.neb1 : P.neb2;
        for (int py = 0; py < 46; ++py) {
            for (int px = int(std::floor(nx - rad)); px < nx + rad; ++px) {
                const double dx = (px - nx) / 1.8;
                const double d = std::sqrt(dx * dx + (py - ny) * (py - ny));
                if (d > rad * .5 || (px * 3 + py * 5) % 7 != 0)
                    continue;
                fillRect(p, (px + W) % W, py, 1, 1, col);
            }
        }
    }
    const int n = P.light ? 190 : 340;
    for (int i = 0; i < n; ++i) {
        const int px = int(std::floor(r() * W));
        const int py = int(std::floor(r() * 50));
        const double v = r();
        const QColor c = v > .95 ? P.star[0]
                       : v > .88 ? P.star[1]
                       : v > .74 ? P.star[2]
                       : v > .42 ? P.star[3]
                                 : P.star[4];
        fillRect(p, px, py, 1, 1, c);
        if (v > .975 && !P.light) {
            fillRect(p, px - 1, py, 1, 1, P.starGlow);
            fillRect(p, px + 1, py, 1, 1, P.starGlow);
            fillRect(p, px, py - 1, 1, 1, P.starGlow);
            fillRect(p, px, py + 1, 1, 1, P.starGlow);
        }
    }
    return img;
}

QImage mistTile(const Palette &P)
{
    const int W = 545;
    QImage img = makeCanvas(W, kTileH);
    QPainter p(&img);
    for (int px = 0; px < W; ++px) {
        const double t = 2 * M_PI * px / W;
        const int top = 36
            + int(std::round(3.5 * std::sin(t) + 2 * std::sin(3 * t + 1.1)
                             + 1.2 * std::sin(7 * t + .4)));
        for (int py = top; py < top + 16; ++py) {
            const double f = 1 - double(py - top) / 16;
            if ((px + py) % 2 == 0 && f > .3)
                fillRect(p, px, py, 1, 1, P.mist1, f);
            else if ((px + py * 3) % 5 == 0)
                fillRect(p, px, py, 1, 1, P.mist2, f * .8);
        }
    }
    return img;
}

QImage hillTile(const Palette &P, int W, double base, double amp,
                const QColor &fill, const QColor &rimCol, quint32 seed,
                int peaks)
{
    Q_UNUSED(P);
    QImage img = makeCanvas(W, kTileH);
    QPainter p(&img);
    Rnd r(seed);
    const double p1 = r() * 6.28, p2 = r() * 6.28, p3 = r() * 6.28,
                 p4 = r() * 6.28;
    const auto yAt = [&](int px) {
        const double t = 2 * M_PI * px / W;
        return base + amp * std::sin(t + p1) + amp * .6 * std::sin(2 * t + p2)
            + amp * .34 * std::sin(5 * t + p3)
            + amp * .18 * std::sin(11 * t + p4);
    };
    QVector<double> top(W);
    for (int px = 0; px < W; ++px)
        top[px] = yAt(px);
    for (int i = 0; i < peaks; ++i) {
        const int cx = int(std::floor(r() * W));
        const double hgt = 4 + r() * 9;
        const int wid = 4 + int(std::floor(r() * 7));
        for (int d = -wid; d <= wid; ++d) {
            const int px = (cx + d + W) % W;
            top[px] = std::min(top[px],
                               yAt(px) - hgt * (1 - double(std::abs(d)) / wid));
        }
    }
    for (int px = 0; px < W; ++px) {
        const int y0 = int(std::round(top[px]));
        fillRect(p, px, y0, 1, kTileH - y0, fill);
        fillRect(p, px, y0, 1, 1, rimCol);
        if ((px + y0) % 3 == 0)
            fillRect(p, px, y0 + 1, 1, 1, rimCol);
    }
    return img;
}

QImage spireTile(const Palette &P)
{
    const int W = 641;
    QImage img = makeCanvas(W, kTileH);
    QPainter p(&img);
    Rnd r(83);
    const QColor DK = P.ink, OL = P.inkLine, LT = P.inkLit;
    const auto col = [&](int cx, int top, int bot, double hwTop, double hwBot,
                         double lean) {
        for (int y = top; y <= bot; ++y) {
            const double t = double(y - top) / std::max(1, bot - top);
            const int hw = std::max(
                1, int(std::round(hwTop + (hwBot - hwTop) * t)));
            const int off = int(std::round(lean * (1 - t) * 4));
            fillRect(p, cx - hw - 1 + off, y, hw * 2 + 3, 1, OL);
            fillRect(p, cx - hw + off, y, hw * 2 + 1, 1, DK);
            fillRect(p, cx + off, y, hw, 1, LT);
        }
    };
    double px = 4 + r() * 30;
    while (px < W - 10) {
        const int kind = int(std::floor(r() * 7));
        const int bot = 53 + int(std::floor(r() * 3));
        const int hgt = 8 + int(std::floor(r() * 28));
        const int top = bot - hgt;
        const double lean = (r() - .5) * 2;
        const int cx = int(std::round(px));
        if (kind == 0) { // needle
            col(cx, top, bot, 1, 3 + std::floor(r() * 2), lean);
        } else if (kind == 1) { // block
            col(cx, top + 2, bot, 3 + std::floor(r() * 2),
                4 + std::floor(r() * 3), 0);
            fillRect(p, cx - 4, top + 1, 4, 2, OL);
            fillRect(p, cx + 1, top, 4, 2, OL);
        } else if (kind == 2) { // broken
            col(cx, top + 4, bot, 3, 4, 0);
            fillRect(p, cx - 3, top + 3, 3, 2, DK);
            fillRect(p, cx + 1, top + 5, 3, 2, DK);
        } else if (kind == 3) { // crystal
            for (int k = -1; k <= 1; ++k)
                col(cx + k * 4, top + std::abs(k) * 6, bot, 1, 2, k * 1.4);
        } else if (kind == 4) { // arch
            col(cx - 5, top + 4, bot, 2, 2, 0);
            col(cx + 5, top + 4, bot, 2, 2, 0);
            fillRect(p, cx - 8, top + 2, 17, 4, OL);
            fillRect(p, cx - 7, top + 3, 15, 2, DK);
            fillRect(p, cx, top + 3, 7, 1, LT);
        } else if (kind == 5) { // twin
            col(cx - 3, top, bot, 1, 2, -.6);
            col(cx + 4, top + 5, bot, 1, 2, .6);
        } else { // stack — the JS re-rolls the loop bound EVERY iteration
            int yy = bot;
            int k = 0;
            while (true) {
                const int bound = 3 + int(std::floor(r() * 3));
                if (k >= bound)
                    break;
                const int bh = 3 + int(std::floor(r() * 3));
                const int bw = 4 - k;
                fillRect(p, cx - bw - 1, yy - bh, bw * 2 + 3, bh, OL);
                fillRect(p, cx - bw, yy - bh, bw * 2 + 1, bh, DK);
                fillRect(p, cx, yy - bh, bw, bh - 1, LT);
                yy -= bh;
                ++k;
            }
        }
        if (r() > .55) {
            const int gy = top + 3
                + int(std::floor(r() * std::max(1, hgt - 6)));
            const QColor rune = r() > .5 ? P.rune1 : P.rune2;
            fillRect(p, cx - 1, gy, 2, 1, rune);
            fillRect(p, cx - 2, gy - 1, 4, 3, rune, .28);
        }
        px += 14 + r() * 34;
    }
    return img;
}

QImage frontTile(const Palette &P)
{
    const int W = 719;
    QImage img = makeCanvas(W, kTileH);
    QPainter p(&img);
    Rnd r(29);
    const double p1 = r() * 6.28, p2 = r() * 6.28;
    QVector<double> top(W);
    for (int px = 0; px < W; ++px) {
        const double t = 2 * M_PI * px / W;
        top[px] = 52 + 3 * std::sin(t + p1) + 1.7 * std::sin(3 * t + p2)
            + std::sin(7 * t);
    }
    for (int px = 0; px < W; ++px) {
        const int y0 = int(std::round(top[px]));
        fillRect(p, px, y0, 1, kTileH - y0, P.frontFill);
        fillRect(p, px, y0, 1, 1, P.frontRim);
    }
    double px = r() * 26;
    while (px < W - 6) {
        const int cx = int(std::round(px));
        const int gy = int(std::round(top[cx]));
        const double pick = r();
        if (pick < .3) { // boulder
            const int rad = 2 + int(std::floor(r() * 4));
            for (int dy = 0; dy <= rad; ++dy) {
                const int w = int(std::round(
                    std::sqrt(std::max(0, rad * rad - dy * dy)) * 2.1));
                fillRect(p, cx - (w >> 1), gy - rad + dy, w, 1, P.frontFill);
                if (!dy)
                    fillRect(p, cx - (w >> 1), gy - rad, w, 1, P.frontRim);
            }
        } else if (pick < .52) { // dead tree
            const int hgt = 6 + int(std::floor(r() * 8));
            fillRect(p, cx, gy - hgt, 2, hgt, P.frontFill);
            for (int b = 0; b < 3; ++b) {
                const int by = gy - hgt + 1 + b * 3;
                const int dir = (b % 2) ? 1 : -1;
                const int len = 3 + int(std::floor(r() * 3));
                for (int k = 0; k < len; ++k)
                    fillRect(p, cx + (dir > 0 ? 2 + k : -1 - k), by + k / 2,
                             1, 1, P.frontFill);
            }
        } else if (pick < .7) { // shard
            const int hgt = 3 + int(std::floor(r() * 5));
            fillRect(p, cx - 2, gy - hgt, 5, hgt, P.inkLine);
            fillRect(p, cx, gy - hgt + 1, 2, hgt - 1, P.inkLit);
            if (r() > .6)
                fillRect(p, cx, gy - hgt + 2, 1, 1, P.rune1, .55);
        } else if (pick < .84) { // grass tuft — bound re-rolled per iteration
            int k = 0;
            while (true) {
                const int bound = 3 + int(std::floor(r() * 3));
                if (k >= bound)
                    break;
                fillRect(p, cx + k * 2, gy - 1 - int(std::floor(r() * 3)), 1,
                         3, P.frontRim);
                ++k;
            }
        }
        px += 8 + r() * 22;
    }
    return img;
}

QImage rainTile(const Palette &P)
{
    const int W = 160;
    QImage img = makeCanvas(W, kTileH);
    QPainter p(&img);
    Rnd r(53);
    for (int i = 0; i < 34; ++i) {
        const int sx = int(std::floor(r() * W));
        const int sy = int(std::floor(r() * kTileH));
        const int len = 3 + int(std::floor(r() * 4));
        for (int k = 0; k < len; ++k) {
            fillRect(p, (sx - int(std::round(k * .8)) + W) % W,
                     (sy + k) % kTileH, 1, 1, k == 0 ? P.rain1 : P.rain2);
        }
    }
    return img;
}

QImage moonSprite(const Palette &P)
{
    const int S = 18;
    QImage img = makeCanvas(S, S);
    QPainter p(&img);
    for (int y = 0; y < S; ++y) {
        for (int px = 0; px < S; ++px) {
            const double d = std::sqrt((px - 8.5) * (px - 8.5)
                                       + (y - 8.5) * (y - 8.5));
            if (d <= 7)
                fillRect(p, px, y, 1, 1, P.moon1);
            else if (d <= 8)
                fillRect(p, px, y, 1, 1, P.moon2);
            else if (d <= 9.4 && (px + y) % 2 == 0)
                fillRect(p, px, y, 1, 1, P.moonHalo);
        }
    }
    const int craters[3][2] = { { 6, 6 }, { 11, 10 }, { 5, 11 } };
    for (const auto &c : craters)
        fillRect(p, c[0], c[1], 2, 1, P.moon2);
    return img;
}

QImage shootingStarSprite(const Palette &P)
{
    QImage img = makeCanvas(30, 12);
    QPainter p(&img);
    for (int i = 0; i < 22; ++i) {
        fillRect(p, 26 - i, 2 + int(std::round(i * .32)), 1, 1,
                 P.light ? P.star[0] : P.star[1], (1 - i / 22.0) * .9);
    }
    fillRect(p, 26, 2, 2, 2, P.light ? P.star[0] : P.boltCore);
    return img;
}

QImage boltSprite(const Palette &P, quint32 seed)
{
    const int W = 46, H = 74;
    QImage art = makeCanvas(W, H);
    {
        QPainter p(&art);
        Rnd r(seed);
        // line(): Bresenham stroked as w x 1 fillRects. Endpoints can be
        // FRACTIONAL (a fork's clamped last point) — the reference then
        // never satisfies the equality exit and walks until the 400-step
        // guard, painting off-canvas (clipped). Reproduced as-is.
        const auto line = [&](double x0, double y0, double x1, double y1,
                              int w, const QColor &col) {
            const double dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
            const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
            double err = dx - dy;
            double cx0 = x0, cy0 = y0;
            int guard = 0;
            while (guard++ < 400) {
                fillRect(p, int(cx0) - (w >> 1), int(cy0), w, 1, col);
                if (cx0 == x1 && cy0 == y1)
                    break;
                const double e2 = 2 * err;
                if (e2 > -dy) { err -= dy; cx0 += sx; }
                if (e2 < dx) { err += dx; cy0 += sy; }
            }
        };
        struct Pt { double x, y; };
        const auto path = [&](double sx0, double sy0, double sy1,
                              double spread, bool thin) {
            QVector<Pt> pts;
            pts.append({ std::round(sx0), sy0 });
            double px = sx0, py = sy0;
            while (py < sy1) {
                py += 5 + std::floor(r() * 6);
                px = std::max(5.0, std::min(double(W - 6),
                                            px + (r() - .5) * spread));
                pts.append({ std::round(px),
                             std::min(std::round(py), sy1) });
            }
            for (int i = 0; i < pts.size() - 1; ++i) {
                const double t = double(i) / (pts.size() - 1);
                const int base = thin ? 1 : 2;
                const Pt &a = pts[i], &b = pts[i + 1];
                line(a.x, a.y, b.x, b.y, base + 5, P.boltHaze1);
                line(a.x, a.y, b.x, b.y, base + 4, P.boltOut);
                line(a.x, a.y, b.x, b.y, base + 3, P.boltHaze2);
                line(a.x, a.y, b.x, b.y, base + 2, P.boltDark);
                line(a.x, a.y, b.x, b.y,
                     base + 1 - int(std::round(t)), P.boltMid);
                line(a.x, a.y, b.x, b.y,
                     std::max(1, base - int(std::round(t))), P.boltCore);
            }
            return pts;
        };
        const QVector<Pt> main =
            path(W / 2.0 + (r() - .5) * 8, 0, H - 6, 15, false);
        // Fork count: the JS re-rolls "2 + floor(r()*2)" per loop check.
        int b = 0;
        while (true) {
            const int bound = 2 + int(std::floor(r() * 2));
            if (b >= bound)
                break;
            const int at = 1 + int(std::floor(r() * (main.size() - 2)));
            if (at >= 1 && at < main.size()) {
                path(main[at].x, main[at].y,
                     std::min(double(H - 8), main[at].y + 12 + r() * 20), 22,
                     true);
            }
            ++b;
        }
        const Pt tip = main.last();
        fillRect(p, int(tip.x) - 4, int(tip.y) - 1, 9, 3, P.boltMid, .5);
        fillRect(p, int(tip.x) - 1, int(tip.y) - 1, 3, 2, P.boltCore);
    }

    // Bake the reference's CSS drop-shadow pair —
    // drop-shadow(0 0 5px rgba(255,212,71,.55)) and
    // drop-shadow(0 0 14px rgba(169,139,245,.4)) — into a padded canvas so
    // QML needs no runtime filter. Radii halved to source scale (art is 2x).
    const int PW = W + 2 * kBoltPad, PH = H + 2 * kBoltPad;
    QVector<float> alpha(PW * PH, 0.0f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            alpha[(y + kBoltPad) * PW + (x + kBoltPad)] =
                qAlpha(art.pixel(x, y)) / 255.0f;

    QImage out = makeCanvas(PW, PH);
    QPainter po(&out);
    const auto paintGlow = [&](int radius, const QColor &tint) {
        QVector<float> g = alpha;
        boxBlurAlpha(g, PW, PH, radius);
        boxBlurAlpha(g, PW, PH, radius);
        QImage glow = makeCanvas(PW, PH);
        for (int y = 0; y < PH; ++y) {
            for (int x = 0; x < PW; ++x) {
                const double a = std::min(1.0f, g[y * PW + x] * 2.0f)
                    * tint.alphaF();
                if (a <= 0.003)
                    continue;
                QColor c = tint;
                c.setAlphaF(a);
                glow.setPixelColor(x, y, c);
            }
        }
        po.drawImage(0, 0, glow);
    };
    paintGlow(7, QColor(169, 139, 245, int(255 * .4)));
    paintGlow(3, QColor(255, 212, 71, int(255 * .55)));
    po.drawImage(kBoltPad, kBoltPad, art);
    return out;
}

QImage skyGradient(const Palette &P)
{
    // "linear-gradient(to bottom, transparent 0%, sky1 30%, sky2 64%,
    //  sky3 100%)" across the 190px band.
    QImage img(8, 190, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    QLinearGradient g(0, 0, 0, 190);
    g.setColorAt(0.0, rgba(P.sky1, 0.0));
    g.setColorAt(0.30, P.sky1);
    g.setColorAt(0.64, P.sky2);
    g.setColorAt(1.0, P.sky3);
    p.fillRect(img.rect(), g);
    return img;
}

QImage horizonGradient(const Palette &P)
{
    // "linear-gradient(to top, horizon, transparent)" over 70px.
    QImage img(8, 70, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    QLinearGradient g(0, 70, 0, 0);
    g.setColorAt(0.0, P.horizon);
    g.setColorAt(1.0, rgba(P.horizon, 0.0));
    p.fillRect(img.rect(), g);
    return img;
}

QImage bloomSprite(const QColor &glow)
{
    // "radial-gradient(170px 120px at ..., glow, transparent 70%)".
    QImage img(340, 240, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    QRadialGradient g(QPointF(0, 0), 1.0);
    g.setCoordinateMode(QGradient::ObjectMode);
    g.setCenter(0.5, 0.5);
    g.setFocalPoint(0.5, 0.5);
    g.setRadius(0.5);
    g.setColorAt(0.0, glow);
    g.setColorAt(0.7, rgba(glow, 0.0));
    g.setColorAt(1.0, rgba(glow, 0.0));
    p.fillRect(img.rect(), g);
    return img;
}

QImage emberDot(const QColor &color)
{
    QImage img(2, 2, QImage::Format_ARGB32_Premultiplied);
    img.fill(color);
    return img;
}

QImage imageForId(const QString &id)
{
    // "<layer>?bg=RRGGBB&accent=RRGGBB"
    const int q = id.indexOf(QLatin1Char('?'));
    const QString layer = q >= 0 ? id.left(q) : id;
    const QUrlQuery query(q >= 0 ? id.mid(q + 1) : QString());
    const auto hex = [&query](const char *key) -> QColor {
        const QString v = query.queryItemValue(QLatin1String(key));
        if (v.size() != 6)
            return {};
        const QColor c(QLatin1Char('#') + v);
        return c.isValid() ? c : QColor();
    };
    const QColor bg = hex("bg");
    const QColor accent = hex("accent");
    if (!bg.isValid() || !accent.isValid() || layer.isEmpty()
        || layer.size() > 16)
        return {};
    const Palette P = derivePalette(bg, accent);

    if (layer == QLatin1String("gradient")) return skyGradient(P);
    if (layer == QLatin1String("sky")) return skyTile(P);
    if (layer == QLatin1String("mist")) return mistTile(P);
    if (layer == QLatin1String("far"))
        return hillTile(P, 331, 30, 4, P.farFill, P.farRim, 3, 9);
    if (layer == QLatin1String("mid"))
        return hillTile(P, 397, 40, 5, P.midFill, P.midRim, 17, 7);
    if (layer == QLatin1String("spire")) return spireTile(P);
    if (layer == QLatin1String("front")) return frontTile(P);
    if (layer == QLatin1String("rain")) return rainTile(P);
    if (layer == QLatin1String("moon")) return moonSprite(P);
    if (layer == QLatin1String("shoot")) return shootingStarSprite(P);
    if (layer == QLatin1String("horizon")) return horizonGradient(P);
    if (layer == QLatin1String("bloomA")) return bloomSprite(P.glowA);
    if (layer == QLatin1String("bloomB")) return bloomSprite(P.glowB);
    if (layer == QLatin1String("bloomC")) return bloomSprite(P.glowC);
    if (layer == QLatin1String("ember1")) return emberDot(P.ember1);
    if (layer == QLatin1String("ember2")) return emberDot(P.ember2);
    if (layer == QLatin1String("ember3")) return emberDot(P.ember3);
    // Bolt seeds from the reference: 5, 41, 97, 163, 211, 307.
    static const quint32 kBoltSeeds[6] = { 5, 41, 97, 163, 211, 307 };
    if (layer.startsWith(QLatin1String("bolt")) && layer.size() == 5) {
        const int n = layer.at(4).digitValue();
        if (n >= 1 && n <= 6)
            return boltSprite(P, kBoltSeeds[n - 1]);
    }
    return {};
}

} // namespace stormband

QImage StormBandImageProvider::requestImage(const QString &id, QSize *size,
                                            const QSize &requestedSize)
{
    Q_UNUSED(requestedSize); // every layer has its reference-fixed size
    const QImage img = stormband::imageForId(id);
    if (size)
        *size = img.size();
    return img;
}
