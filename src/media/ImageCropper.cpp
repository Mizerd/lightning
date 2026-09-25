#include "media/ImageCropper.h"

#include "media/AnimatedImageSniff.h"

#include "media/StagedImageStore.h"

#include "storage/PortableMode.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QTemporaryDir>

#include <cstring>

Q_LOGGING_CATEGORY(lcCrop, "lightning.media.crop")

namespace imagecrop {

CropPlan planCrop(const QSize &source, const QRectF &requested, int maxEdge)
{
    CropPlan plan;
    if (source.width() <= 0 || source.height() <= 0) {
        plan.reason = QStringLiteral("empty_source");
        return plan;
    }

    // Round each edge independently: QRectF::toRect() rounds the origin and
    // then the size, which can move the right edge by a pixel.
    const int x = qRound(requested.x());
    const int y = qRound(requested.y());
    const int w = qRound(requested.width());
    const int h = qRound(requested.height());

    // Clamp rather than trust: a rounding error or bad input must yield a
    // smaller crop, never a read outside the decoded buffer.
    const QRect wanted(x, y, w, h);
    const QRect bounds(QPoint(0, 0), source);
    plan.sourceRect = wanted.intersected(bounds);

    if (plan.sourceRect.width() < 1 || plan.sourceRect.height() < 1) {
        plan.sourceRect = QRect();
        plan.reason = QStringLiteral("empty_rect");
        return plan;
    }

    plan.outputSize = plan.sourceRect.size();
    // Scale down only; enlarging adds bytes, not information.
    if (maxEdge > 0
        && qMax(plan.outputSize.width(), plan.outputSize.height()) > maxEdge) {
        plan.outputSize = plan.outputSize.scaled(maxEdge, maxEdge,
                                                 Qt::KeepAspectRatio);
        // QSize::scaled can floor an edge to 0 for an extreme aspect ratio.
        plan.outputSize.setWidth(qMax(1, plan.outputSize.width()));
        plan.outputSize.setHeight(qMax(1, plan.outputSize.height()));
    }

    plan.ok = true;
    return plan;
}

QString sniffRasterMime(const QByteArray &bytes)
{
    const auto starts = [&bytes](const char *magic, int len) {
        return bytes.size() >= len
               && std::memcmp(bytes.constData(), magic, size_t(len)) == 0;
    };
    if (starts("\x89PNG\r\n\x1a\n", 8))
        return QStringLiteral("image/png");
    if (starts("\xff\xd8\xff", 3))
        return QStringLiteral("image/jpeg");
    if (starts("GIF87a", 6) || starts("GIF89a", 6))
        return QStringLiteral("image/gif");
    if (bytes.size() >= 12 && starts("RIFF", 4)
        && std::memcmp(bytes.constData() + 8, "WEBP", 4) == 0)
        return QStringLiteral("image/webp");
    if (starts("BM", 2))
        return QStringLiteral("image/bmp");
    // JPEG XL: test the container before the bare codestream, since the
    // container's payload begins with the codestream signature. (cjxl output:
    // ff0a for bare streams, 0000000c4a584c200d0a870a with --container=1.)
    if (starts("\x00\x00\x00\x0CJXL \r\n\x87\n", 12)
        || starts("\xff\x0a", 2))
        return QStringLiteral("image/jxl");
    return QString();
}

OutputFormat chooseOutputFormat(const QString &sourceMime, bool hasAlpha)
{
    if (!hasAlpha && sourceMime == QLatin1String("image/jpeg")) {
        return { QStringLiteral("jpeg"), QStringLiteral("image/jpeg"),
                 QStringLiteral("jpg") };
    }
    return { QStringLiteral("png"), QStringLiteral("image/png"),
             QStringLiteral("png") };
}

int maxEdgeForRole(const QString &role)
{
    if (role == QLatin1String("banner"))
        return 1920;
    if (role == QLatin1String("avatar"))
        return 512;
    // An unknown role gets the tighter cap, never none.
    return 512;
}

} // namespace imagecrop

ImageCropper::ImageCropper(QObject *parent)
    : QObject(parent)
{
}

ImageCropper::~ImageCropper()
{
    // Release the live lock before the QTemporaryDir removes its directory.
    if (m_outputDir)
        lightning::portable::releaseScratchDir(m_outputDir->path());
}

void ImageCropper::setStagedImages(StagedImageStore *store)
{
    m_stagedImages = store;
}

void ImageCropper::setError(const QString &category)
{
    if (m_lastError == category)
        return;
    m_lastError = category;
    Q_EMIT lastErrorChanged();
}

int ImageCropper::maxEdgeForRole(const QString &role) const
{
    return imagecrop::maxEdgeForRole(role);
}

QVariantMap ImageCropper::load(const QUrl &fileUrl)
{
    QVariantMap result;
    result.insert(QStringLiteral("ok"), false);

    // Release the previous source first, so a refused file cannot leave the
    // last accepted one staged.
    discard();

    const QString path = fileUrl.isLocalFile() ? fileUrl.toLocalFile()
                                               : fileUrl.toString();
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isFile() || !info.isReadable()) {
        setError(QStringLiteral("unreadable"));
        result.insert(QStringLiteral("error"), m_lastError);
        return result;
    }
    if (info.size() <= 0 || info.size() > kMaxSourceBytes) {
        setError(info.size() <= 0 ? QStringLiteral("unreadable")
                                  : QStringLiteral("too_large"));
        result.insert(QStringLiteral("error"), m_lastError);
        return result;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(QStringLiteral("unreadable"));
        result.insert(QStringLiteral("error"), m_lastError);
        return result;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    // The gate: magic bytes decide before any decode and before QML gets a URL.
    // SVG, HTML error pages, video and fake .png files stop here.
    const QString mime = imagecrop::sniffRasterMime(bytes);
    if (mime.isEmpty()) {
        setError(QStringLiteral("unsupported_image"));
        result.insert(QStringLiteral("error"), m_lastError);
        return result;
    }

    QByteArray probe = bytes;
    QBuffer buffer(&probe);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);   // honour EXIF orientation
    const QSize natural = reader.size();
    if (natural.isValid()
        && qMax(natural.width(), natural.height()) > kMaxSourceEdge) {
        reader.setScaledSize(natural.scaled(kMaxSourceEdge, kMaxSourceEdge,
                                            Qt::KeepAspectRatio));
    }
    QImage decoded = reader.read();
    if (decoded.isNull() || decoded.width() < 1 || decoded.height() < 1) {
        // Recognised by magic but the codec refused it (truncated, or e.g. WebP
        // without qtimageformats). Distinct from "unsupported_image".
        setError(QStringLiteral("undecodable"));
        result.insert(QStringLiteral("error"), m_lastError);
        return result;
    }

    if (m_stagedImages)
        m_previewToken = m_stagedImages->add(bytes);
    if (m_previewToken.isEmpty()) {
        // No preview, no dialog; every fallback would hand QML the raw path.
        setError(QStringLiteral("undecodable"));
        result.insert(QStringLiteral("error"), m_lastError);
        return result;
    }

    m_source = decoded;
    m_sourceMime = mime;
    setError(QString());

    // An animation is kept as its own frames (see canKeepAnimation), with its
    // metadata stripped the way the still path's re-encode drops EXIF. The copy
    // exists only for a file that strips cleanly, is still an animation and is
    // small enough to upload.
    namespace sniff = lightning::animsniff;
    const bool animated = sniff::isAnimation(bytes);
    const QByteArray clean =
        animated ? sniff::stripAnimationMetadata(bytes) : QByteArray();
    if (!clean.isEmpty() && sniff::isAnimation(clean)
        && clean.size() <= kMaxAnimatedUploadBytes) {
        const QSize canvas = sniff::canvasSize(clean);
        const QString dir = outputDirectory();
        if (canvas.isValid() && canvas.width() > 0 && canvas.height() > 0
            && !dir.isEmpty()) {
            const QString path = QDir(dir).filePath(
                QStringLiteral("anim-%1.%2")
                    .arg(m_nextOutput++)
                    .arg(sniff::animationSuffix(clean)));
            QSaveFile out(path);
            if (out.open(QIODevice::WriteOnly)) {
                out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
                if (out.write(clean) == clean.size() && out.commit()) {
                    m_animatedPath = path;
                    m_animatedPixels =
                        qint64(canvas.width()) * canvas.height();
                }
            }
        }
    }

    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("width"), m_source.width());
    result.insert(QStringLiteral("height"), m_source.height());
    result.insert(QStringLiteral("mime"), mime);
    result.insert(QStringLiteral("previewUrl"),
                  QStringLiteral("image://lightning-staged/") + m_previewToken);
    result.insert(QStringLiteral("error"), QString());
    result.insert(QStringLiteral("animated"), animated);
    result.insert(QStringLiteral("animatedUrl"),
                  m_animatedPath.isEmpty()
                      ? QString()
                      : QUrl::fromLocalFile(m_animatedPath).toString());
    // Dimensions and type only, never the path.
    qCInfo(lcCrop) << "crop source loaded" << m_source.width() << "x"
                   << m_source.height() << mime;
    return result;
}

QString ImageCropper::outputDirectory()
{
    if (m_outputDir && m_outputDir->isValid())
        return m_outputDir->path();
    // Under mediaScratchRoot(), never QDir::temp(): keeps these files inside a
    // portable folder and within cleanStaleTempDirs()'s reach after a crash.
    m_outputDir = std::make_unique<QTemporaryDir>(
        lightning::portable::mediaScratchRoot()
        + QStringLiteral("/lightning-crop-XXXXXX"));
    if (!m_outputDir->isValid()) {
        m_outputDir.reset();
        return QString();
    }
    // Explicitly 0700, as the playable-media path does.
    QFile::setPermissions(m_outputDir->path(),
                          QFile::ReadOwner | QFile::WriteOwner
                              | QFile::ExeOwner);
    // Marked live so another instance's startup sweep cannot delete output an
    // upload is still reading.
    lightning::portable::holdScratchDirLive(m_outputDir->path());
    return m_outputDir->path();
}

QUrl ImageCropper::crop(double x, double y, double w, double h, int maxEdge)
{
    if (m_source.isNull()) {
        setError(QStringLiteral("no_source"));
        return {};
    }

    const imagecrop::CropPlan plan =
        imagecrop::planCrop(m_source.size(), QRectF(x, y, w, h), maxEdge);
    if (!plan.ok) {
        setError(plan.reason);
        return {};
    }

    QImage out = m_source.copy(plan.sourceRect);
    if (out.size() != plan.outputSize) {
        out = out.scaled(plan.outputSize, Qt::IgnoreAspectRatio,
                         Qt::SmoothTransformation);
    }
    if (out.isNull()) {
        setError(QStringLiteral("encode_failed"));
        return {};
    }

    const imagecrop::OutputFormat format =
        imagecrop::chooseOutputFormat(m_sourceMime, out.hasAlphaChannel());
    if (format.encoder == QLatin1String("jpeg")) {
        // No alpha reaches here, but an ARGB buffer would be flattened onto an
        // unspecified colour; convert explicitly.
        out = out.convertToFormat(QImage::Format_RGB32);
    }

    const QString dir = outputDirectory();
    if (dir.isEmpty()) {
        setError(QStringLiteral("write_failed"));
        return {};
    }
    // A counter, not the source file name, which is the user's and may be
    // logged.
    const QString path = QDir(dir).filePath(
        QStringLiteral("crop-%1.%2").arg(m_nextOutput++).arg(format.suffix));

    QByteArray encoded;
    {
        QBuffer sink(&encoded);
        sink.open(QIODevice::WriteOnly);
        QImageWriter writer(&sink, format.encoder.toLatin1());
        if (format.encoder == QLatin1String("jpeg"))
            writer.setQuality(90);
        if (!writer.write(out)) {
            setError(QStringLiteral("encode_failed"));
            return {};
        }
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(QStringLiteral("write_failed"));
        return {};
    }
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    if (file.write(encoded) != encoded.size() || !file.commit()) {
        setError(QStringLiteral("write_failed"));
        return {};
    }

    // The sink reads this asynchronously, so it must outlive the dialog. A
    // small ring bounds disk use without removing the path just handed out.
    m_written.append(path);
    while (m_written.size() > kRetainedOutputs)
        QFile::remove(m_written.takeFirst());

    setError(QString());
    qCInfo(lcCrop) << "crop written" << plan.outputSize.width() << "x"
                   << plan.outputSize.height() << format.mime
                   << encoded.size() << "bytes";
    return QUrl::fromLocalFile(path);
}

bool ImageCropper::canKeepAnimation(const QString &role) const
{
    if (m_animatedPath.isEmpty() || m_animatedPixels <= 0)
        return false;
    const qint64 maxPixels = role == QLatin1String("banner")
        ? lightning::animsniff::kBannerMaxPixels
        : lightning::animsniff::kAvatarMaxPixels;
    return m_animatedPixels <= maxPixels;
}

QUrl ImageCropper::useAnimation(const QString &role)
{
    if (m_source.isNull()) {
        setError(QStringLiteral("no_source"));
        return {};
    }
    if (!canKeepAnimation(role)) {
        setError(QStringLiteral("animation_too_large"));
        return {};
    }
    // Retained like a crop: the sink reads it after the dialog closes.
    if (!m_animatedHandedOut) {
        m_animatedHandedOut = true;
        m_written.append(m_animatedPath);
        while (m_written.size() > kRetainedOutputs)
            QFile::remove(m_written.takeFirst());
    }
    setError(QString());
    qCInfo(lcCrop) << "animation kept" << role << m_animatedPixels << "px";
    return QUrl::fromLocalFile(m_animatedPath);
}

void ImageCropper::discard()
{
    if (m_stagedImages && !m_previewToken.isEmpty())
        m_stagedImages->remove(m_previewToken);
    m_previewToken.clear();
    m_source = QImage();
    m_sourceMime.clear();
    // A preview copy nobody uploaded is removed with the dialog.
    if (!m_animatedPath.isEmpty() && !m_animatedHandedOut)
        QFile::remove(m_animatedPath);
    m_animatedPath.clear();
    m_animatedPixels = 0;
    m_animatedHandedOut = false;
}

void ImageCropper::clearSession()
{
    discard();
    for (const QString &path : std::as_const(m_written))
        QFile::remove(path);
    m_written.clear();
    // Release the live lock before the directory is removed.
    if (m_outputDir)
        lightning::portable::releaseScratchDir(m_outputDir->path());
    // Removes the directory recursively.
    m_outputDir.reset();
    setError(QString());
}
