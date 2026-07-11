#pragma once

#include <QQuickImageProvider>

class MediaBridge;

// v0.5.9: serves decoded media images to QML from MediaBridge's in-memory
// cache under image://lightning-media/<percent-encoded cache key>.
//
// Decoding is bounded: images larger than the safety edge are downscaled at
// decode time, so a hostile attachment cannot force an unbounded
// allocation. The provider never touches the network or disk.
class MediaImageProvider : public QQuickImageProvider
{
public:
    explicit MediaImageProvider(MediaBridge *bridge);

    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;

private:
    MediaBridge *m_bridge; // not owned; outlives the QML engine
};
