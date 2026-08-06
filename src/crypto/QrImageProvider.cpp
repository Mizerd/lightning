#include "crypto/QrImageProvider.h"

#include <QImage>
#include <QMutexLocker>

namespace {
// Default and maximum rendered edge, in pixels. The maximum bounds the
// allocation a requested size can drive; the default keeps modules crisp
// when QML asks for no particular size.
constexpr int kDefaultModulePixels = 8;
constexpr int kMaxEdgePixels = 1024;

int strideFor(int modules)
{
    return modules > 0 ? (modules + 7) / 8 : 0;
}
} // namespace

bool QrCodeStore::setCode(const QString &token, int modules,
                          const QByteArray &bits)
{
    if (token.isEmpty() || modules <= 0 || modules > kMaxModules
        || bits.size() != strideFor(modules) * modules) {
        return false;
    }
    QMutexLocker locker(&m_mutex);
    m_token = token;
    m_modules = modules;
    m_bits = bits;
    return true;
}

void QrCodeStore::clear()
{
    QMutexLocker locker(&m_mutex);
    m_token.clear();
    m_modules = 0;
    // Release the store's reference to the grid as well as its key, so the
    // store stops serving the code the moment its flow ends. This is NOT a
    // secure erase: QByteArray is implicitly shared, so any copy already
    // handed out by gridFor() keeps its own reference, and the freed buffer
    // is not zeroed. What is guaranteed is that no later request can obtain
    // the grid through this store.
    m_bits.clear();
    m_bits.squeeze();
}

QByteArray QrCodeStore::gridFor(const QString &token, int *modules) const
{
    if (modules)
        *modules = 0;
    QMutexLocker locker(&m_mutex);
    if (token.isEmpty() || token != m_token || m_modules <= 0)
        return {};
    if (modules)
        *modules = m_modules;
    return m_bits;
}

QrImageProvider::QrImageProvider(QrCodeStore *store)
    : QQuickImageProvider(QQuickImageProvider::Image)
    , m_store(store)
{
}

QImage QrImageProvider::requestImage(const QString &id, QSize *size,
                                     const QSize &requestedSize)
{
    if (size)
        *size = QSize();
    if (!m_store)
        return {};

    int modules = 0;
    // An unknown or stale token renders NOTHING. Falling back to whatever
    // code happens to be stored would show one flow's code under another
    // flow's URL, which is exactly the confusion a scannable secret must
    // never create. (MediaImageProvider takes the same line: a cache miss
    // returns a null QImage rather than a substitute.)
    const QByteArray bits = m_store->gridFor(id, &modules);
    if (modules <= 0 || bits.isEmpty())
        return {};

    const int total = modules + 2 * kQuietZoneModules;
    const int stride = strideFor(modules);

    // One pixel per module first, then a nearest-neighbour upscale. Drawing
    // straight to the target size would let rounding shave or double module
    // rows; scaling a clean grid cannot.
    QImage base(total, total, QImage::Format_RGB32);
    base.fill(Qt::white);
    for (int y = 0; y < modules; ++y) {
        const uchar *row =
            reinterpret_cast<const uchar *>(bits.constData()) + y * stride;
        for (int x = 0; x < modules; ++x) {
            if (row[x / 8] & (0x80u >> (x % 8)))
                base.setPixel(kQuietZoneModules + x, kQuietZoneModules + y,
                              qRgb(0, 0, 0));
        }
    }

    int edge = total * kDefaultModulePixels;
    if (requestedSize.isValid()) {
        const int wanted = qMax(requestedSize.width(), requestedSize.height());
        if (wanted > 0) {
            // Whole-module scaling only: a fractional module size is what
            // makes a rendered code fail to scan.
            const int scale = qMax(1, wanted / total);
            edge = total * scale;
        }
    }
    if (edge > kMaxEdgePixels)
        edge = total * qMax(1, kMaxEdgePixels / total);

    QImage out = base.scaled(edge, edge, Qt::IgnoreAspectRatio,
                             Qt::FastTransformation);
    if (size)
        *size = out.size();
    return out;
}
