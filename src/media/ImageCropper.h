#pragma once

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <memory>

class QTemporaryDir;
class StagedImageStore;

// Crop maths for the display-image upload path. Pure (no files, codecs or
// state) so it is testable headless; `ImageCropper` below is the stateful
// shell.
namespace imagecrop {

/// A resolved crop request. When `ok` is false only `reason` is meaningful.
struct CropPlan
{
    bool ok = false;
    /// Integer rect inside the source image, guaranteed non-empty and
    /// entirely within `source` when `ok`.
    QRect sourceRect;
    /// Never larger than `sourceRect.size()`: crops are never upscaled.
    QSize outputSize;
    /// "" when ok. Otherwise "empty_source" | "empty_rect".
    QString reason;
};

/// Resolves a crop in source pixel coordinates (the space ImageCropper::load
/// reports). The request is rounded and intersected with the source bounds,
/// never trusted. `maxEdge <= 0` means uncapped; otherwise the longer edge is
/// reduced to `maxEdge`, keeping the aspect ratio.
CropPlan planCrop(const QSize &source, const QRectF &requested, int maxEdge);

/// MIME type from magic bytes only, or empty for anything but the five raster
/// formats Lightning accepts; this refuses SVG (CLAUDE.md §6), HTML error
/// pages and video. Same signatures as `rooms::sniff_image_mime` and
/// ForwardController's gate. Not gif::validateRasterBytes, whose saved-GIF
/// caps would refuse an ordinary 5K photo.
QString sniffRasterMime(const QByteArray &bytes);

/// Encoder, MIME and suffix for a cropped result, returned together so they
/// always agree.
struct OutputFormat
{
    /// Qt image-plugin format name, for QImage::save / QImageWriter.
    QString encoder;
    QString mime;
    QString suffix;
};

/// Picks the output format:
///   * transparency -> PNG (JPEG would flatten alpha onto black);
///   * otherwise a JPEG source -> JPEG;
///   * everything else -> PNG.
/// GIF/WebP/BMP sources come out as PNG: one frame is not the animation, and
/// WebP writing needs qtimageformats, which packaged builds may lack.
OutputFormat chooseOutputFormat(const QString &sourceMime, bool hasAlpha);

/// Output caps by role, defined here so QML cannot invent its own.
///   avatar - square, drawn at 40-200 px; 512 is generous.
///   banner - a wide strip across a profile or Space card.
int maxEdgeForRole(const QString &role);

} // namespace imagecrop

/// Reads a user-chosen file, holds one decoded source for the open dialog, and
/// writes the cropped result as a temp file. Every display-image sink takes a
/// local path and uploads it, so call sites keep their existing sinks.
///
/// The preview is served through `image://lightning-staged/<token>` from bytes
/// already sniffed here. Pointing a QML Image at the chosen file:// URL would
/// let Qt's loader render an `.svg` as active content.
class ImageCropper : public QObject
{
    Q_OBJECT

    /// Machine-readable category of the last failure, or "": "unreadable" |
    /// "too_large" | "unsupported_image" | "undecodable" | "no_source" |
    /// "empty_rect" | "encode_failed" | "write_failed". Never contains the
    /// path.
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit ImageCropper(QObject *parent = nullptr);
    ~ImageCropper() override;

    /// Not owned; outlives this object (AppController holds both).
    void setStagedImages(StagedImageStore *store);

    QString lastError() const { return m_lastError; }

    /// Reads, sniffs, decodes and stages `fileUrl` for preview. Returns { ok,
    /// width, height, previewUrl, mime, error }. `width`/`height` are the
    /// decoded size, the coordinate space for later calls; sources over
    /// kMaxSourceEdge are decoded downscaled.
    Q_INVOKABLE QVariantMap load(const QUrl &fileUrl);

    /// Crops the loaded source to `rect` (source pixels) and writes the result.
    /// Returns a file:// URL, or empty with `lastError` set. `maxEdge <= 0` is
    /// uncapped; prefer maxEdgeForRole().
    Q_INVOKABLE QUrl crop(double x, double y, double w, double h, int maxEdge);

    /// Cap for a role name ("avatar" | "banner"), for QML to pass to crop().
    Q_INVOKABLE int maxEdgeForRole(const QString &role) const;

    /// Animated sources. Qt cannot encode a GIF or an animated WebP, so a crop
    /// always flattens to one frame. To keep the motion the original FRAMES
    /// are uploaded instead, uncropped (clients centre-crop avatars and banners
    /// when drawing them), with metadata stripped (EXIF, XMP, GIF comments;
    /// see animsniff::stripAnimationMetadata). load() reports `animated` and,
    /// when the stripped file is clean and small enough, `animatedUrl`: that
    /// 0600 copy, for the dialog's preview and the upload.
    ///
    /// True when the loaded source is an animation that Lightning would play
    /// in that role (kMaxAnimatedUploadBytes, and the canvas bounds in
    /// AnimatedImageSniff.h).
    Q_INVOKABLE bool canKeepAnimation(const QString &role) const;
    /// The stripped animation as a file:// URL for the upload sinks, or empty
    /// with `lastError` "animation_too_large" | "no_source".
    Q_INVOKABLE QUrl useAnimation(const QString &role);

    /// Releases the staged preview and decoded source when the dialog closes.
    Q_INVOKABLE void discard();

    /// Also removes written temp files. Called on sign-out and account switch.
    void clearSession();

    /// Bound on what a single file can cost; far above any avatar limit.
    static constexpr qint64 kMaxSourceBytes = 64LL * 1024 * 1024;
    /// Decode ceiling, matching MediaImageProvider / StagedImageProvider.
    static constexpr int kMaxSourceEdge = 4096;
    /// Matches the Rust avatar and banner upload caps (MAX_AVATAR_BYTES,
    /// MAX_BANNER_BYTES).
    static constexpr qint64 kMaxAnimatedUploadBytes = 8LL * 1024 * 1024;
    /// Written crops kept on disk. Sinks read the path asynchronously, so the
    /// newest must outlive the dialog.
    static constexpr int kRetainedOutputs = 4;

Q_SIGNALS:
    void lastErrorChanged();

private:
    void setError(const QString &category);
    QString outputDirectory();

    StagedImageStore *m_stagedImages = nullptr;
    std::unique_ptr<QTemporaryDir> m_outputDir;
    QStringList m_written;          // oldest first
    QImage m_source;
    QString m_sourceMime;
    QString m_previewToken;
    // The loaded source's animation copy ("" when still or too large), its
    // canvas area, and whether useAnimation() handed it to a sink.
    QString m_animatedPath;
    qint64 m_animatedPixels = 0;
    bool m_animatedHandedOut = false;
    QString m_lastError;
    quint64 m_nextOutput = 1;
};
