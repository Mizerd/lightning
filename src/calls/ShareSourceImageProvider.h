#pragma once

#include <QQuickImageProvider>

// Screen grabs are content: produced on demand for one QML Image and never
// cached, stored or logged.
class ShareSourceImageProvider : public QQuickImageProvider
{
public:
    ShareSourceImageProvider();

    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;
};
