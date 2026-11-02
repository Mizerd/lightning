#pragma once

#include <QQuickImageProvider>

class StagedImageStore;

class StagedImageProvider : public QQuickImageProvider
{
public:
    explicit StagedImageProvider(StagedImageStore *store);

    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;

private:
    StagedImageStore *m_store;
};
