#include "stickers/StickerPack.h"

namespace stickers {

QVariantMap PackImage::toVariantMap() const
{
    QVariantMap map;
    map.insert(QStringLiteral("shortcode"), shortcode);
    map.insert(QStringLiteral("url"), url);
    map.insert(QStringLiteral("body"), body);
    map.insert(QStringLiteral("mimetype"), mimetype);
    map.insert(QStringLiteral("width"), width);
    map.insert(QStringLiteral("height"), height);
    map.insert(QStringLiteral("size"), size);
    map.insert(QStringLiteral("isEmoticon"), isEmoticon);
    map.insert(QStringLiteral("isSticker"), isSticker);
    return map;
}

PackImage PackImage::fromVariantMap(const QVariantMap &map)
{
    PackImage image;
    image.shortcode = map.value(QStringLiteral("shortcode")).toString();
    image.url = map.value(QStringLiteral("url")).toString();
    image.body = map.value(QStringLiteral("body")).toString();
    image.mimetype = map.value(QStringLiteral("mimetype")).toString();
    image.width = map.value(QStringLiteral("width")).toInt();
    image.height = map.value(QStringLiteral("height")).toInt();
    image.size = map.value(QStringLiteral("size")).toLongLong();
    image.isEmoticon = map.value(QStringLiteral("isEmoticon")).toBool();
    image.isSticker = map.value(QStringLiteral("isSticker")).toBool();
    return image;
}

int Pack::stickerCount() const
{
    int n = 0;
    for (const PackImage &image : images) {
        if (image.isSticker)
            ++n;
    }
    return n;
}

int Pack::emoticonCount() const
{
    int n = 0;
    for (const PackImage &image : images) {
        if (image.isEmoticon)
            ++n;
    }
    return n;
}

} // namespace stickers
