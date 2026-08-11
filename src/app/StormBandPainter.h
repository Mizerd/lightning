#pragma once

#include <QColor>
#include <QImage>
#include <QQuickImageProvider>
#include <QSize>
#include <QString>

// The "Storm Band" — a procedurally generated pixel-art storm landscape
// painted once for the bottom of the About page.
//
// Every tile is rendered exactly once in software (QImage + QPainter, no
// GPU, no windowing system) from a small set of theme-derived inputs and
// served to QML through StormBandImageProvider under:
//
//   image://storm-band/<layer>/<W>x<H>?dark=0|1&base=RRGGBB&accent=RRGGBB&seed=N
//
// `<W>x<H>` is the LOGICAL pixel size QML wants; the returned QImage matches
// it exactly. `base`/`accent`/`dark` come from AppTheme (background/accent/
// dark) — this file never hardcodes a theme literal of its own. Every
// derived hue traces back to those three inputs through derivePalette().
//
// No Canvas (paints nothing under the offscreen QPA platform — see
// qml/TrustCard.qml and qml/StormNode.qml's comments), no ShaderEffect
// (qtshadertools is not linked), no QtQuick.Particles/Shapes (not linked).
// QML only tiles/animates plain Image/Rectangle geometry built from these
// bitmaps; nothing here runs per frame — a tile regenerates only when its
// id (i.e. the theme) changes.
//
// Determinism is load-bearing: identical inputs must produce byte-identical
// output, so every random choice routes through a std::mt19937 seeded
// purely from the parsed request (see makeRng in the .cpp) — never
// QRandomGenerator::global() and never a time source.
namespace stormband {

// Recognised layer names. Keep in sync with layerFromName()/renderLayer().
enum class Layer {
    Sky,
    FarHills,
    Mist,
    MidHills,
    Spires,
    Ridge,
    Rain,
    Bolt,
    Bloom,
    Streak,
    Unknown,
};

Layer layerFromName(const QString &name);

// A fully parsed, validated request. `size` is the LOGICAL pixel size QML
// asked for.
struct SceneParams {
    Layer layer = Layer::Unknown;
    QSize size;
    bool dark = false;
    QColor base;
    QColor accent;
    quint32 seed = 1;
};

// Parses the part of the image id AFTER the provider name, e.g.
// "sky/1536x190?dark=1&base=0A0F24&accent=FFD447&seed=7". Returns false
// (and leaves *out unmodified) for anything malformed, out of range, or
// referring to an unknown layer — never throws, never crashes. `layer`,
// `<W>x<H>`, `dark`, `base`, and `accent` are all required; `seed` is
// optional and defaults to 1.
bool parseId(const QString &id, SceneParams *out);

// The full derived scene palette. Every colour a layer painter uses comes
// from here, so no individual layer hardcodes a theme hex value.
struct Palette {
    QColor skyTop;
    QColor skyBottom;
    QColor horizonGlow;
    QColor farHill;
    QColor midHill;
    QColor spireBody;
    QColor spireGlow;
    QColor ridgeBody;
    QColor ridgeAccent;
    QColor mistTint;
    QColor rainInk;
    QColor starOrDust;
    QColor moonBody;
    QColor nebula;
    QColor boltCore;
    QColor boltGlow;
};

Palette derivePalette(const QColor &base, const QColor &accent, bool dark);

// Renders one tile at art (half) resolution and upscales it with
// nearest-neighbour scaling so the result stays crisp/blocky rather than
// blurred. Returns a null QImage for Layer::Unknown or a non-positive/
// oversized `params.size` — the caller must serve that as "nothing",
// never a placeholder.
QImage renderLayer(const SceneParams &params);

// Hard bound a caller must never exceed — defends against a hostile or
// buggy id driving an unbounded allocation.
constexpr int kMaxDimension = 4096;

} // namespace stormband

// QML-facing provider registered as image://storm-band/<id>.
class StormBandImageProvider : public QQuickImageProvider
{
public:
    StormBandImageProvider();

    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;
};
