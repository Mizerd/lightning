#include "media/StagedImageProvider.h"

#include "media/StagedImageStore.h"

#include <QBuffer>
#include <QImageReader>

namespace {
// Same hard bound as MediaImageProvider: nothing decodes above this edge.
constexpr int kMaxDecodeEdge = 4096;
} // namespace

StagedImageProvider::StagedImageProvider(StagedImageStore *store)
    : QQuickImageProvider(QQuickImageProvider::Image)
    , m_store(store)
{
}

QImage StagedImageProvider::requestImage(const QString &id, QSize *size,
                                         const QSize &requestedSize)
{
    if (!m_store)
        return {};
    const QByteArray bytes = m_store->bytes(id);
    if (bytes.isEmpty())
        return {};

    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly))
        return {};
    QImageReader reader(&buffer);
    // The format comes from the CONTENT, never from a claimed name: this is
    // the same rule the rest of the media path follows.
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);

    const QSize natural = reader.size();
    // A width-only `sourceSize` is QML's documented keep-the-aspect idiom, and
    // QSize::isEmpty() is true whenever EITHER axis is below 1 — the trap that
    // once made every timeline image decode at full resolution. Resolve each
    // axis explicitly instead.
    const int rw = qMax(0, requestedSize.width());
    const int rh = qMax(0, requestedSize.height());
    QSize target;
    if (natural.isValid() && !natural.isEmpty() && (rw > 0 || rh > 0)) {
        if (rw > 0 && rh > 0)
            target = natural.scaled(rw, rh, Qt::KeepAspectRatio);
        else if (rw > 0)
            target = QSize(rw, qMax(1, qRound(double(natural.height()) * rw
                                              / natural.width())));
        else
            target = QSize(qMax(1, qRound(double(natural.width()) * rh
                                          / natural.height())), rh);
        // Never upscale: the scene graph interpolates from the source pixels
        // just as well, and inflating in memory only costs.
        if (target.width() >= natural.width()
            || target.height() >= natural.height())
            target = QSize();
    }
    if (target.isValid() && !target.isEmpty())
        reader.setScaledSize(target);
    else if (natural.isValid()
             && qMax(natural.width(), natural.height()) > kMaxDecodeEdge) {
        reader.setScaledSize(natural.scaled(kMaxDecodeEdge, kMaxDecodeEdge,
                                            Qt::KeepAspectRatio));
    }

    const QImage image = reader.read();
    if (image.isNull())
        return {};
    if (size)
        *size = image.size();
    return image;
}
