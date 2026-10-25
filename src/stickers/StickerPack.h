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
    // The EXACT `images` map key: the image's identity. Every edit names this
    // (never `shortcode`), so `hello__world`, `Cat` vs `cat` and 65-100 byte
    // keys are addressed as themselves.
    QString key;
    // Whether `key` satisfies Matrix v1.19's grammar `[A-Za-z0-9_-]{1,100}`.
    // An invalid received key is still listed and can be removed or renamed,
    // but is never offered as a typed `:shortcode:`.
    bool keyValid = true;
    // Display text: `key` when valid, otherwise a one-line rendering of it.
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
    // `user`, or an opaque id for a room pack's (dialect, room, state key);
    // stable across refreshes. Never split: use roomId/stateKey/ns.
    QString id;
    // "stable" (m.room.image_pack) or "legacy" (im.ponies.*; the account's own
    // pack is always legacy). Writes go back to this dialect.
    QString ns = QStringLiteral("legacy");
    // A legacy record under the same room/key is hidden by this stable one.
    bool legacyTwin = false;
    // Only the legacy subscription list holds it, while the stable list rules.
    bool legacySubscribed = false;
    // The pack names only usage values this client does not know.
    bool usageUnsupported = false;
    // False for a subscribed reference that could not be read; `unavailable`
    // is why ("absent", "not_joined", "forbidden", "network", ...).
    bool available = true;
    QString unavailable;
    // Reached through the active room's canonical space ancestry.
    bool fromSpace = false;
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
