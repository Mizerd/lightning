#include "gif/GifStarredModel.h"

GifStarredModel::GifStarredModel(QSettings *settings, QObject *parent)
    : GifStoredModel(settings, QStringLiteral("gif/starred"), kMaxStarred,
                     parent)
{
}

qint64 GifStarredModel::totalBytes() const
{
    qint64 total = 0;
    for (int i = 0; i < count(); ++i)
        total += resultAt(i).gifBytes;
    return total;
}
