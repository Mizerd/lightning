#pragma once

#include <QQuickImageProvider>

class StagedImageStore;

// Serves staged (not yet sent) attachment images to QML under
// image://lightning-staged/<token>.
//
// The whole reason it exists is the clipboard paste path: those bytes never
// become a file, so there is no file:// URL for the composer chip to show.
// See StagedImageStore.
//
// Decoding is bounded the same way MediaImageProvider's is: an image is never
// decoded above kMaxDecodeEdge, and a `sourceSize` request is honoured through
// QImageReader::setScaledSize so a 4000px screenshot does not become tens of
// megabytes of pixels for a 64px chip. Never touches the network or disk.
class StagedImageProvider : public QQuickImageProvider
{
public:
    explicit StagedImageProvider(StagedImageStore *store);

    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;

private:
    StagedImageStore *m_store; // not owned; outlives the QML engine
};
