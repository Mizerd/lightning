#pragma once

#include <QList>
#include <QString>
#include <QVariantMap>

namespace stickers {

struct PackImage
{
    // The EXACT `images` map key: the image's identity. Every edit names this
    // (never `shortcode`), so `hello__world`, `Cat` vs `cat` and 65-100 byte
    // keys are addressed as themselves.
    QString key;
    // Whether `key` satisfies Matrix v1.19's grammar `[A-Za-z0-9_-]{1,100}`.
    // An invalid received key is still listed and can be removed or renamed,
    // but is never offered as a typed `:shortcode:`.
    bool keyValid = true;
    QString shortcode;
    QString url;
    QString body;
    QString mimetype;
    int width = 0;
    int height = 0;
    qint64 size = 0;
    bool isEmoticon = false;
    bool isSticker = false;

    QVariantMap toVariantMap() const;
    static PackImage fromVariantMap(const QVariantMap &map);
};

struct Pack
{
    QString id;
    QString ns = QStringLiteral("legacy");
    bool legacyTwin = false;
    bool legacySubscribed = false;
    bool usageUnsupported = false;
    bool available = true;
    QString unavailable;
    bool fromSpace = false;
    bool packEmoticon = true;
    bool packSticker = true;
    bool hidden = false;
    QString displayName;
    QString avatarUrl;
    QString attribution;
    QString source;
    QString roomId;
    QString stateKey;
    bool enabledGlobally = false;
    bool canManage = false;
    QList<PackImage> images;

    int stickerCount() const;
    int emoticonCount() const;
};

}
