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

namespace imagecrop {

struct CropPlan
{
    bool ok = false;
    QRect sourceRect;
    QSize outputSize;
    QString reason;
};

CropPlan planCrop(const QSize &source, const QRectF &requested, int maxEdge);

/// MIME type from magic bytes only, or empty for anything but the five raster
/// formats Lightning accepts; this refuses SVG (CLAUDE.md §6), HTML error
/// pages and video. Same signatures as `rooms::sniff_image_mime` and
/// ForwardController's gate. Not gif::validateRasterBytes, whose saved-GIF
/// caps would refuse an ordinary 5K photo.
QString sniffRasterMime(const QByteArray &bytes);

struct OutputFormat
{
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

int maxEdgeForRole(const QString &role);

}

class ImageCropper : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit ImageCropper(QObject *parent = nullptr);
    ~ImageCropper() override;

    void setStagedImages(StagedImageStore *store);

    QString lastError() const { return m_lastError; }

    Q_INVOKABLE QVariantMap load(const QUrl &fileUrl);

    Q_INVOKABLE QUrl crop(double x, double y, double w, double h, int maxEdge);

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
    Q_INVOKABLE QUrl useAnimation(const QString &role);

    Q_INVOKABLE void discard();

    void clearSession();

    static constexpr qint64 kMaxSourceBytes = 64LL * 1024 * 1024;
    static constexpr int kMaxSourceEdge = 4096;
    static constexpr qint64 kMaxAnimatedUploadBytes = 8LL * 1024 * 1024;
    static constexpr int kRetainedOutputs = 4;

Q_SIGNALS:
    void lastErrorChanged();
    void svgLoaded(const QVariantMap &info);

private:
    void setError(const QString &category);
    QString outputDirectory();
    QVariantMap loadBytes(const QByteArray &bytes);

    StagedImageStore *m_stagedImages = nullptr;
    std::unique_ptr<QTemporaryDir> m_outputDir;
    QStringList m_written;
    QImage m_source;
    QString m_sourceMime;
    QString m_previewToken;
    QString m_animatedPath;
    qint64 m_animatedPixels = 0;
    bool m_animatedHandedOut = false;
    QString m_lastError;
    quint64 m_nextOutput = 1;
    quint64 m_loadGeneration = 0;
};
