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

// Chat backgrounds and surface depth: the pure half. No files, no network, no
// QML, so every rule here is testable headless (ChatBackdropTest).
//
//   * resolve()     which background a room shows (precedence + opt-outs);
//   * measure()     what an image actually looks like, from its pixels;
//   * planScrim()   how much of the theme's ground must cover the image so
//                   the inks drawn straight on it stay readable;
//   * depthStops()  the "Depth" gradient for a flat surface colour;
//   * spaceChain()  which Spaces a room inherits a background from.
namespace backdrop {

// ---- colour ----------------------------------------------------------------

/// WCAG relative luminance of an opaque colour (alpha ignored).
double relativeLuminance(const QColor &c);
/// WCAG 2.x contrast ratio, 1 .. 21.
double contrast(const QColor &a, const QColor &b);
/// `top` at `alpha` source-over an opaque `bottom`, per channel in sRGB and
/// rounded to 8 bits: what the scene graph actually puts on screen.
QColor composite(const QColor &top, double alpha, const QColor &bottom);
/// Linear interpolation a -> b in sRGB, t in 0..1.
QColor mix(const QColor &a, const QColor &b, double t);
/// CIE L* (D65), 0 .. 100.
double lstar(const QColor &c);
/// The same colour moved by `deltaL` in CIE L* with a and b kept, clamped to
/// sRGB. Used by depthStops(); never changes hue on purpose.
QColor shiftLightness(const QColor &c, double deltaL);

// ---- images ----------------------------------------------------------------

/// What a background image looks like, measured from its own pixels after a
/// small decode (<= kSampleEdge on the long side, which is the image blurred
/// at about 1/kSampleEdge of its width). Never taken from declared metadata.
struct ImageStats
{
    /// False: nothing measured yet; planScrim() then assumes the worst case
    /// (pure white and pure black both under the text).
    bool measured = false;
    QVector<QRgb> samples;
    /// The most saturated common hue's average colour, or the mean colour of
    /// a near-grey image. Invalid when not measured.
    QColor dominant;
};

constexpr int kSampleEdge = 48;

/// Measures `image` (any size; it is scaled down here). A null image gives an
/// unmeasured result.
ImageStats measure(const QImage &image);

// ---- the scrim -------------------------------------------------------------

/// Highest scrim opacity a background is allowed: above it the picture is
/// gone, and a floor that needs more than this means "this palette cannot
/// carry a picture here", reported as !feasible.
constexpr double kMaxScrimAlpha = 0.97;
/// Upper bound on how much of the image's dominant colour may tint the scrim.
constexpr double kMaxTint = 0.35;
/// The share of measured samples allowed to fall below the bar: isolated
/// specks, smaller than 1/kSampleEdge of the image, that no blur hides.
constexpr double kOutlierShare = 0.01;
/// The readability bar for ink drawn straight on the conversation background
/// (CustomThemeStore's table: textPrimary, textSecondary and textMuted on
/// `background` are all 4.5:1).
constexpr double kInkBar = 4.5;

struct ScrimPlan
{
    /// The scrim colour: the theme ground, possibly tinted.
    QColor color;
    /// Smallest opacity at which every ink keeps the bar over the image.
    double floor = 1.0;
    /// The tint actually used (<= the request; reduced until feasible).
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

/// The opacity actually drawn: the floor plus `dim` of the remaining range.
/// Never below the floor, whatever `dim` says.
double scrimAlpha(double floor, double dim);

/// Worst ink contrast over the samples at `alpha`, after discarding the
/// kOutlierShare worst samples of a measured image. Exposed for the tests.
double worstContrastAt(const QColor &scrim, double alpha,
                       const QList<QColor> &inks, const ImageStats &stats);

// ---- depth -----------------------------------------------------------------

/// How far "Depth" moves the far stop, in CIE L*.
constexpr double kDepthDeltaL = 4.0;

/// Two stops, top -> bottom, for a surface whose flat colour is `base`. The
/// shift goes AWAY from the ink: darker on a dark theme, lighter on a light
/// one, so the contrast of text on the surface can only rise. Returned as
/// "#RRGGBB".
QStringList depthStops(const QColor &base, bool darkTheme);

// ---- presentation ----------------------------------------------------------

/// Presentation hints, clamped exactly as rust/src/backdrop.rs clamps them:
/// dim/blur/tint 0..1 (defaults 0.2/0/0.25), fit cover|contain|tile, align
/// center|top|bottom. Unknown keys are dropped.
QVariantMap normalisePresentation(const QVariantMap &raw);

/// The shared event's form of the same hints: dim, blur and tint as INTEGER
/// percentages 0..100. Event content is canonical JSON, which has no floats
/// (Synapse answers 400 M_BAD_JSON "Bad JSON value: float"), so the unit
/// values used everywhere in the app are converted only at this boundary.
QVariantMap presentationToWire(const QVariantMap &unit);
/// The reverse, for content read from a room: integer percentages (clamped;
/// anything else takes the default) back to normalised unit values.
QVariantMap presentationFromWire(const QVariantMap &wire);

// ---- precedence ------------------------------------------------------------

/// One room's inputs. Each map is a background record (shared: canonical
/// content with "url"; personal: a record with "file") or empty for none.
struct ResolveInput
{
    QVariantMap personalRoom;
    QVariantMap sharedRoom;
    /// Nearest Space first: (spaceId, content).
    QList<QPair<QString, QVariantMap>> sharedSpaces;
    QVariantMap personalDefault;
    /// Settings: "Show backgrounds set by others".
    bool showShared = true;
    /// Per-room "Hide this room's background".
    bool roomHidden = false;
};

struct Resolved
{
    /// "personal-room" | "room" | "space" | "personal-default" | "none".
    QString source = QStringLiteral("none");
    /// The room or Space whose state supplied it ("" for personal/none).
    QString scopeId;
    QVariantMap record;
};

/// personal per-room override > shared room > shared Space (nearest first) >
/// personal default > none. The opt-outs remove the two shared levels only.
Resolved resolve(const QString &roomId, const ResolveInput &in);

// ---- Spaces ----------------------------------------------------------------

/// The Spaces a room inherits from, nearest first: one direct parent (the
/// one under `activeSpaceId` when there is one, else the first in
/// `spaceOrder`), then its ancestors. `childrenOf` maps each joined Space to
/// its direct children. Cycle-safe and bounded by kMaxSpaceDepth.
QStringList spaceChain(const QString &roomId,
                       const QHash<QString, QStringList> &childrenOf,
                       const QStringList &spaceOrder,
                       const QString &activeSpaceId);

constexpr int kMaxSpaceDepth = 8;

} // namespace backdrop
