#include "media/ImageCropper.h"

#include "media/StagedImageStore.h"

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

    // Round each edge independently rather than QRectF::toRect(), which
    // rounds the ORIGIN and then the SIZE relative to it — for a rect whose
    // x is 0.5 that silently moves the right edge by a pixel.
    const int x = qRound(requested.x());
    const int y = qRound(requested.y());
    const int w = qRound(requested.width());
    const int h = qRound(requested.height());

    // Clamp rather than trust. QML computes this from a live transform; a
    // rounding error at the edge of the image, or a caller that passes
    // nonsense, must produce a smaller crop and never a read outside the
    // decoded buffer.
    const QRect wanted(x, y, w, h);
    const QRect bounds(QPoint(0, 0), source);
    plan.sourceRect = wanted.intersected(bounds);

    if (plan.sourceRect.width() < 1 || plan.sourceRect.height() < 1) {
        plan.sourceRect = QRect();
        plan.reason = QStringLiteral("empty_rect");
        return plan;
    }

    plan.outputSize = plan.sourceRect.size();
    // Scale DOWN only. Enlarging a small crop to fill the cap produces a
    // bigger file carrying exactly the same information, and on an avatar it
    // makes a blurry picture look like a deliberate one.
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
    // JPEG XL, both shapes. The ISOBMFF CONTAINER is tested first and the bare
    // codestream second: the container's own payload begins with the codestream
    // signature, so checking the short form first would mis-report a container
    // as a bare stream. Verified against real cjxl 0.12.0 output -- lossy and
    // lossless both start ff0a, `--container=1` starts 0000000c4a584c200d0a870a.
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
    // An unknown role gets the tighter of the two rather than "no cap": a
    // typo in a call site must not silently uncap an upload.
    return 512;
}

} // namespace imagecrop

ImageCropper::ImageCropper(QObject *parent)
    : QObject(parent)
{
}

ImageCropper::~ImageCropper() = default;

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

    // Release whatever the previous open left behind FIRST, so a refused
    // file cannot leave the last accepted one staged and croppable.
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

    // THE GATE. Magic bytes decide, before anything is decoded and before
    // QML is given a URL to point an Image at. An SVG, an HTML error page,
    // a video, or a .png that is not one fails here and goes no further.
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
        // Identified by magic but the codec refused it — truncated, or a
        // format whose plugin this build does not carry (WebP lives in
        // qtimageformats). Distinct from "unsupported_image": the bytes ARE
        // a format Lightning accepts, this build just cannot open them.
        setError(QStringLiteral("undecodable"));
        result.insert(QStringLiteral("error"), m_lastError);
        return result;
    }

    if (m_stagedImages)
        m_previewToken = m_stagedImages->add(bytes);
    if (m_previewToken.isEmpty()) {
        // No preview means no dialog: showing a crop frame over nothing is
        // worse than refusing, and there is no fallback that does not hand
        // QML the user's raw path.
        setError(QStringLiteral("undecodable"));
        result.insert(QStringLiteral("error"), m_lastError);
        return result;
    }

    m_source = decoded;
    m_sourceMime = mime;
    setError(QString());

    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("width"), m_source.width());
    result.insert(QStringLiteral("height"), m_source.height());
    result.insert(QStringLiteral("mime"), mime);
    result.insert(QStringLiteral("previewUrl"),
                  QStringLiteral("image://lightning-staged/") + m_previewToken);
    result.insert(QStringLiteral("error"), QString());
    // Dimensions and the sniffed type only — never the path, which contains
    // whatever the user's home directory is called.
    qCInfo(lcCrop) << "crop source loaded" << m_source.width() << "x"
                   << m_source.height() << mime;
    return result;
}

QString ImageCropper::outputDirectory()
{
    if (m_outputDir && m_outputDir->isValid())
        return m_outputDir->path();
    m_outputDir = std::make_unique<QTemporaryDir>(
        QDir::temp().filePath(QStringLiteral("lightning-crop-XXXXXX")));
    if (!m_outputDir->isValid()) {
        m_outputDir.reset();
        return QString();
    }
    // QTemporaryDir is 0700 already; say so explicitly rather than rely on
    // it, the same way the playable-media path does.
    QFile::setPermissions(m_outputDir->path(),
                          QFile::ReadOwner | QFile::WriteOwner
                              | QFile::ExeOwner);
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
        // JPEG cannot carry alpha; the format choice above only reaches here
        // when there is none, but an ARGB buffer would still be written by
        // flattening onto an unspecified colour. Be explicit.
        out = out.convertToFormat(QImage::Format_RGB32);
    }

    const QString dir = outputDirectory();
    if (dir.isEmpty()) {
        setError(QStringLiteral("write_failed"));
        return {};
    }
    // The name is a counter, not the source's file name: the chosen file's
    // name is the user's and has no business being re-originated into a
    // temp path that other code may log.
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

    // The sink reads this path asynchronously (the backend uploads it on its
    // own schedule), so it must outlive the dialog. A small ring bounds the
    // disk cost without ever removing the one just handed out.
    m_written.append(path);
    while (m_written.size() > kRetainedOutputs)
        QFile::remove(m_written.takeFirst());

    setError(QString());
    qCInfo(lcCrop) << "crop written" << plan.outputSize.width() << "x"
                   << plan.outputSize.height() << format.mime
                   << encoded.size() << "bytes";
    return QUrl::fromLocalFile(path);
}

void ImageCropper::discard()
{
    if (m_stagedImages && !m_previewToken.isEmpty())
        m_stagedImages->remove(m_previewToken);
    m_previewToken.clear();
    m_source = QImage();
    m_sourceMime.clear();
}

void ImageCropper::clearSession()
{
    discard();
    for (const QString &path : std::as_const(m_written))
        QFile::remove(path);
    m_written.clear();
    // The QTemporaryDir destructor removes the directory recursively.
    m_outputDir.reset();
    setError(QString());
}
