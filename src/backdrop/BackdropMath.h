#pragma once

#include <QColor>
#include <QHash>
#include <QImage>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

namespace backdrop {

double relativeLuminance(const QColor &c);
double contrast(const QColor &a, const QColor &b);
QColor composite(const QColor &top, double alpha, const QColor &bottom);
QColor mix(const QColor &a, const QColor &b, double t);
double lstar(const QColor &c);
QColor shiftLightness(const QColor &c, double deltaL);

struct ImageStats
{
    bool measured = false;
    QVector<QRgb> samples;
    QColor dominant;
};

constexpr int kSampleEdge = 48;

ImageStats measure(const QImage &image);

ImageStats measureFrames(const QList<QImage> &frames);

constexpr double kMaxScrimAlpha = 0.97;
constexpr double kMaxTint = 0.35;
constexpr double kOutlierShare = 0.01;
constexpr double kInkBar = 4.5;

struct ScrimPlan
{
    QColor color;
    double floor = 1.0;
    double tint = 0.0;
    /// False when even the untinted ground at kMaxScrimAlpha fails; the
    /// floor is then 1.0 and the picture does not show.
    bool feasible = false;
};

/// Plans the scrim for one palette and one image.
///
/// `inks` are the colours drawn directly on the ground (no bubble of their
/// own). The floor is found on a 0.01 grid scanning DOWN from 1.0 and stops at
/// the first opacity that fails, so every opacity at or above the floor
/// passes, whether or not contrast is monotonic in opacity for a given image.
ScrimPlan planScrim(const QColor &ground, const QList<QColor> &inks,
                    const ImageStats &stats, double requestedTint,
                    double bar = kInkBar);

double scrimAlpha(double floor, double dim);

double worstContrastAt(const QColor &scrim, double alpha,
                       const QList<QColor> &inks, const ImageStats &stats);

constexpr double kDepthDeltaL = 4.0;

/// Two stops, top -> bottom, for a surface whose flat colour is `base`. The
/// shift goes AWAY from the ink: darker on a dark theme, lighter on a light
/// one, so the contrast of text on the surface can only rise. Returned as
/// "#RRGGBB".
QStringList depthStops(const QColor &base, bool darkTheme);

QVariantMap normalisePresentation(const QVariantMap &raw);

QVariantMap presentationToWire(const QVariantMap &unit);
QVariantMap presentationFromWire(const QVariantMap &wire);

struct ResolveInput
{
    QVariantMap personalRoom;
    QVariantMap sharedRoom;
    QList<QPair<QString, QVariantMap>> sharedSpaces;
    QVariantMap personalDefault;
    bool showShared = true;
    bool roomHidden = false;
};

struct Resolved
{
    QString source = QStringLiteral("none");
    QString scopeId;
    QVariantMap record;
};

/// personal per-room override > shared room > shared Space (nearest first) >
/// personal default > none. The opt-outs remove the two shared levels only.
Resolved resolve(const QString &roomId, const ResolveInput &in);

QStringList spaceChain(const QString &roomId,
                       const QHash<QString, QStringList> &childrenOf,
                       const QStringList &spaceOrder,
                       const QString &activeSpaceId);

constexpr int kMaxSpaceDepth = 8;

}
