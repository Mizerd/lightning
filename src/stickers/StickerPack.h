#pragma once

#include <QList>
#include <QString>
#include <QVariantMap>

// Plain value types for MSC2545 image packs.
//
// Every field here arrived from another account's global account data or from
// room state anyone with the power level can write. It was validated and
// bounded in `rust/src/stickers.rs` before it crossed the FFI — a url that is
// not `mxc://` was DROPPED there, a DECLARED mimetype outside the five raster
// types was refused there, and shortcodes/bodies/names were stripped of
// control characters and capped there. Nothing on this side re-derives any of
// that: there is exactly ONE place that decides what a pack may contain, and
// duplicating the rule in C++ is how the two copies drift apart.
//
// What this side must still honour: these strings are LABELS. They are set on
// `text`, never on rich text, and never on a URL, a file path or a command.
namespace stickers {

struct PackImage
{
    // The `images` map key, repaired to MSC2545's own `[a-zA-Z0-9-_]+`.
    QString shortcode;
    // Always a syntactically valid mxc:// URI, or the image would not exist.
    QString url;
    // Alt text. MSC2545 defaults it to the shortcode; Rust already applied
    // that default, so this is never empty for a live row.
    QString body;
    // May be EMPTY, which means the pack DECLARED nothing — genuinely
    // unknown, and different from a declared type we refused. The bytes are
    // sniffed when the media is fetched either way.
    QString mimetype;
    int width = 0;
    int height = 0;
    qint64 size = 0;
    // MSC2545's usage, already resolved through the image-then-pack
    // inheritance rule (an empty set at both levels means BOTH).
    bool isEmoticon = false;
    bool isSticker = false;

    // The map QML hands back to send/save. Deliberately the SAME shape the
    // image model exposes as roles, so a caller can round-trip a row without
    // knowing which of the two it came from.
    QVariantMap toVariantMap() const;
    static PackImage fromVariantMap(const QVariantMap &map);
};

struct Pack
{
    // `user`, or `room:<room id>:<state key>`. Stable across refreshes, and
    // what the picker's selected-tab property holds.
    QString id;
    QString displayName;
    // "" unless the pack declared an mxc avatar (a non-mxc one was dropped
    // in Rust, so a pack cannot put an http beacon on a tab).
    QString avatarUrl;
    QString attribution;
    // `user` | `room`.
    QString source;
    QString roomId;
    QString stateKey;
    // ROOM packs only. `enabledGlobally` is whether `im.ponies.emote_rooms`
    // lists this pack — i.e. whether it is available OUTSIDE its own room; a
    // room's packs are always usable INSIDE that room whatever it says.
    // `canManage` is the room's own required power level for
    // `im.ponies.room_emotes`, asked of the SDK — never a role label, and
    // false until a snapshot has actually said otherwise.
    bool enabledGlobally = false;
    bool canManage = false;
    QList<PackImage> images;

    int stickerCount() const;
    int emoticonCount() const;
};

} // namespace stickers
