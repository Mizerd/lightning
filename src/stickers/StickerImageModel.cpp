#include "stickers/StickerImageModel.h"

StickerImageModel::StickerImageModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int StickerImageModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_images.size();
}

QVariant StickerImageModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_images.size())
        return {};
    const stickers::PackImage &image = m_images.at(index.row());
    switch (role) {
    case ShortcodeRole:  return image.shortcode;
    case UrlRole:        return image.url;
    case BodyRole:       return image.body;
    case MimetypeRole:   return image.mimetype;
    case WidthRole:      return image.width;
    case HeightRole:     return image.height;
    case SizeRole:       return image.size;
    case IsEmoticonRole: return image.isEmoticon;
    case IsStickerRole:  return image.isSticker;
    case AspectRole:
        // A pack that declares no dimensions is the ordinary case, so the
        // grid must have a defensible answer rather than a division by zero.
        return (image.width > 0 && image.height > 0)
            ? static_cast<double>(image.width) / image.height
            : 1.0;
    default:             return {};
    }
}

QHash<int, QByteArray> StickerImageModel::roleNames() const
{
    return {
        { ShortcodeRole,  "shortcode" },
        { UrlRole,        "url" },
        { BodyRole,       "body" },
        { MimetypeRole,   "mimetype" },
        { WidthRole,      "width" },
        { HeightRole,     "height" },
        { SizeRole,       "size" },
        { IsEmoticonRole, "isEmoticon" },
        { IsStickerRole,  "isSticker" },
        { AspectRole,     "aspect" },
    };
}

void StickerImageModel::reset(const QList<stickers::PackImage> &images)
{
    beginResetModel();
    m_images = images;
    endResetModel();
    Q_EMIT countChanged();
}

void StickerImageModel::clear()
{
    if (m_images.isEmpty())
        return;
    beginResetModel();
    m_images.clear();
    endResetModel();
    Q_EMIT countChanged();
}

QVariantMap StickerImageModel::get(int row) const
{
    if (row < 0 || row >= m_images.size())
        return {};
    return m_images.at(row).toVariantMap();
}
