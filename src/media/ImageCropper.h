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

// Crop / adjust maths for the display-image upload path.
//
// Everything in this namespace is PURE — no files, no Qt image plugins, no
// state — precisely so it can be unit tested without a display, an image
// codec or a homeserver. `ImageCropper` below is the thin stateful shell
// that reads a file, drives these, and writes the result.
namespace imagecrop {

/// What a crop request resolves to once the source bounds and the output cap
/// have been applied. `ok == false` carries a stable machine-readable
/// `reason` and NOTHING else is meaningful.
struct CropPlan
{
    bool ok = false;
    /// Integer rect inside the source image, guaranteed non-empty and
    /// entirely within `source` when `ok`.
    QRect sourceRect;
    /// What the encoder will actually produce. Never larger than
    /// `sourceRect.size()` — a crop is never UPSCALED, because enlarging
    /// costs bytes and adds no information.
    QSize outputSize;
    /// "" when ok. Otherwise "empty_source" | "empty_rect".
    QString reason;
};

/// Resolve a crop request expressed in SOURCE PIXEL coordinates.
///
/// `requested` comes from QML, which works in the decoded image's own pixel
/// space (see `ImageCropper::load`, which reports that space). It is rounded
/// to integers and then INTERSECTED with the source bounds rather than
/// trusted: a QML rounding error, a resize mid-gesture or a hand-written
/// call must never be able to ask for pixels outside the image.
///
/// `maxEdge <= 0` means uncapped. Otherwise the longer output edge is
/// brought down to `maxEdge`, keeping the aspect ratio; a crop already
/// smaller than the cap is left alone.
CropPlan planCrop(const QSize &source, const QRectF &requested, int maxEdge);

/// The MIME type these bytes ACTUALLY are, from magic bytes alone — never a
/// file name, never a claimed type. Empty for anything that is not one of
/// the five raster formats Lightning accepts, which is what refuses SVG
/// (CLAUDE.md §6: untrusted SVG must never enter a media path) as well as
/// an HTML error page, a video container, or a PNG with a corrupt header.
///
/// Deliberately the SAME five signatures as `rooms::sniff_image_mime` and
/// `ForwardController`'s gate, so the three cannot disagree about what is
/// acceptable. Deliberately NOT `gif::validateRasterBytes`, which carries
/// the saved-GIF store's 4096px / 25 MiB caps and would refuse an ordinary
/// 5K photograph that this path crops perfectly well.
QString sniffRasterMime(const QByteArray &bytes);

/// The encoder, MIME and suffix for a cropped result.
///
/// The three ALWAYS agree — the point of returning them together is that
/// nothing downstream can declare PNG over JPEG bytes.
struct OutputFormat
{
    /// Qt image-plugin format name, for QImage::save / QImageWriter.
    QString encoder;
    QString mime;
    QString suffix;
};

/// Pick an honest output format.
///
///   * transparency present -> PNG. JPEG has no alpha and would flatten it
///     onto black, which on an avatar with a cut-out background is a
///     visibly wrong picture rather than a smaller one.
///   * otherwise a JPEG source -> JPEG. Re-encoding a photograph as PNG
///     multiplies its size several-fold for no gain.
///   * everything else -> PNG.
///
/// A GIF, WebP or BMP source therefore comes out as PNG, and is DECLARED as
/// PNG. Two reasons it is not passed through: one frame of an animation is
/// not the animation, so calling the result "image/gif" would be a lie; and
/// WebP *writing* lives in qtimageformats, which the packaged DEB/RPM/
/// AppImage builds need not carry — the same class of defect ForwardController
/// was corrected for once already.
OutputFormat chooseOutputFormat(const QString &sourceMime, bool hasAlpha);

/// The output caps offered to QML, by role. Kept here rather than in QML so
/// a site cannot invent its own ceiling.
///   avatar — square, rendered at 40-96 px almost everywhere in this app and
///            at 200 px at its very largest (Space Home). 512 is generous.
///   banner — a wide strip drawn across a profile or Space card.
int maxEdgeForRole(const QString &role);

} // namespace imagecrop

/// The stateful half: read a user-chosen file, hold ONE decoded source for
/// the open dialog, and write the cropped result somewhere the existing
/// upload paths can read it.
///
/// WHY IT PRODUCES A FILE. Every display-image sink in this application —
/// `RoomInfoController::setRoomAvatar`, `ProfileBannerManager::setOwnBanner`
/// / `setRoomBanner`, `ConversationController`'s post-create avatar — takes a
/// LOCAL PATH and hands it to the backend, which reads and uploads it. So the
/// cropper is a pure pre-step: it writes a temp file and returns its file://
/// URL, and every call site keeps the sink it already had. No FFI, no Rust
/// and no MatrixClient change is involved in this feature at all.
///
/// WHY QML NEVER SEES THE USER'S FILE. The preview is served through the
/// existing `image://lightning-staged/<token>` provider, from bytes this
/// class has already sniffed. Pointing a QML `Image` straight at the chosen
/// file:// URL would hand an arbitrary user-selected path to Qt's image
/// loader — which would render an `.svg` as active content. Sniffing first
/// and serving the bytes ourselves is what makes that unreachable.
class ImageCropper : public QObject
{
    Q_OBJECT

    /// Stable machine-readable category for the last failure, or "".
    /// "unreadable" | "too_large" | "unsupported_image" | "undecodable" |
    /// "no_source" | "empty_rect" | "encode_failed" | "write_failed".
    /// QML maps these to sentences; nothing here is user-facing text and
    /// nothing here carries the path.
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit ImageCropper(QObject *parent = nullptr);
    ~ImageCropper() override;

    /// Not owned; outlives this object (AppController holds both).
    void setStagedImages(StagedImageStore *store);

    QString lastError() const { return m_lastError; }

    /// Read, sniff and decode `fileUrl`, and stage it for preview.
    ///
    /// Returns { ok: bool, width: int, height: int, previewUrl: string,
    ///           mime: string, error: string }.
    ///
    /// `width`/`height` are the DECODED size, which is the coordinate space
    /// every later call must use. It is the file's true size unless the file
    /// exceeds `kMaxSourceEdge`, in which case the decode is bounded down —
    /// harmless here because the output is capped far below that anyway, and
    /// necessary because a 12000px source would otherwise cost half a
    /// gigabyte of pixels to open a dialog.
    Q_INVOKABLE QVariantMap load(const QUrl &fileUrl);

    /// Crop the loaded source to `rect` (source pixels) and write the result.
    ///
    /// Returns a file:// URL for the caller to hand to its existing sink, or
    /// an empty URL on failure with `lastError` set. `maxEdge <= 0` is
    /// uncapped; prefer `maxEdgeForRole`.
    Q_INVOKABLE QUrl crop(double x, double y, double w, double h, int maxEdge);

    /// Cap for a role name ("avatar" | "banner"), for QML to pass to crop().
    Q_INVOKABLE int maxEdgeForRole(const QString &role) const;

    /// Release the staged preview and the decoded source. Called when the
    /// dialog closes, whichever way it closed.
    Q_INVOKABLE void discard();

    /// Drop everything, including written temp files. Sign-out / account
    /// switch: a cropped picture of the previous account's choosing must not
    /// outlive its session on disk.
    void clearSession();

    /// A display image nobody needs above this, and a bound on what a single
    /// bad file can cost. A 64 MiB photograph is already far outside what
    /// any homeserver accepts for an avatar.
    static constexpr qint64 kMaxSourceBytes = 64LL * 1024 * 1024;
    /// Decode ceiling, matching MediaImageProvider / StagedImageProvider.
    static constexpr int kMaxSourceEdge = 4096;
    /// How many written crops stay on disk. The sink reads the path
    /// ASYNCHRONOUSLY, so the newest must survive the dialog closing; a
    /// small ring is what keeps that true without unbounded disk.
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
    QString m_lastError;
    quint64 m_nextOutput = 1;
};
