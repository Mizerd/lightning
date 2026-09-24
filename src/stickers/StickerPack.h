#pragma once

#include <QList>
#include <QString>
#include <QVariantMap>

// Value types for MSC2545 image packs.
//
// Every field came from another account's account data or member-writable room
// state, and was validated in rust/src/stickers.rs before crossing the FFI
// (non-mxc urls dropped, declared mimetypes outside the raster set refused,
// text stripped of control characters and capped). The rule lives in one
// place and is not duplicated here. These strings are labels only: shown as
// plain text, never used as a URL, path or command.
namespace stickers {

struct PackImage
{
    // The `images` map key, repaired to MSC2545's own `[a-zA-Z0-9-_]+`.
    QString shortcode;
    // Always a syntactically valid mxc:// URI, or the image would not exist.
    QString url;
    // Alt text; Rust already defaulted it to the shortcode.
    QString body;
    // May be empty: the pack declared nothing (unknown, unlike a refused type).
    // The bytes are sniffed on fetch either way.
    QString mimetype;
    int width = 0;
    int height = 0;
    qint64 size = 0;
    // MSC2545 usage after image-then-pack inheritance (empty at both levels
    // means both).
    bool isEmoticon = false;
    bool isSticker = false;

    // The map QML hands back to send/save; the same shape as the image model's
    // roles.
    QVariantMap toVariantMap() const;
    static PackImage fromVariantMap(const QVariantMap &map);
};

struct Pack
{
    // `user` or `room:<room id>:<state key>`; stable across refreshes.
    QString id;
    QString displayName;
    // "" unless the pack declared an mxc avatar (non-mxc ones were dropped).
    QString avatarUrl;
    QString attribution;
    // `user` | `room`.
    QString source;
    QString roomId;
    QString stateKey;
    // Room packs only. `enabledGlobally`: listed in `im.ponies.emote_rooms`, so
    // usable outside its room (always usable inside). `canManage`: the SDK's
    // answer for the room's required power level; false until a snapshot says
    // otherwise.
    bool enabledGlobally = false;
    bool canManage = false;
    QList<PackImage> images;

    int stickerCount() const;
    int emoticonCount() const;
};

} // namespace stickers
