#pragma once

#include <QColor>
#include <QImage>
#include <QQuickImageProvider>
#include <QString>

namespace stormband {

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

Palette derivePalette(const QColor &bg, const QColor &accent);

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

inline constexpr int kTileH = 66;

QImage skyTile(const Palette &P);
QImage mistTile(const Palette &P);
QImage hillTile(const Palette &P, int W, double base, double amp,
                const QColor &fill, const QColor &rim, quint32 seed,
                int peaks);
QImage spireTile(const Palette &P);
QImage frontTile(const Palette &P);
QImage rainTile(const Palette &P);
QImage moonSprite(const Palette &P);
QImage shootingStarSprite(const Palette &P);
QImage boltSprite(const Palette &P, quint32 seed);
inline constexpr int kBoltPad = 8;

QImage skyGradient(const Palette &P);
QImage horizonGradient(const Palette &P);
QImage bloomSprite(const QColor &glow);
QImage emberDot(const QColor &color);

QImage imageForId(const QString &id);

}

class StormBandImageProvider : public QQuickImageProvider
{
public:
    // Forced asynchronous: tile generation is real pixel work and would
    // otherwise run on the GUI thread for an Image without asynchronous:true.
    StormBandImageProvider()
        : QQuickImageProvider(QQuickImageProvider::Image,
                              QQmlImageProviderBase::ForceAsynchronousImageLoading)
    {}
    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;
};
