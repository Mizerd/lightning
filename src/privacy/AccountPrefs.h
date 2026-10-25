#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariantMap>

// Pure parsing and composing of two standard account-data preferences:
//   * MSC4278 `m.media_preview_config` { invite_avatars, media_previews }
//   * Matrix v1.18+ `m.recent_emoji` { recent_emoji: [{emoji, total}, ...] }
//     (older Element builds wrote [[emoji, count], ...]; still read, never
//     written)
// No I/O, no Qt object state: tested directly, and shared by the controller.
namespace AccountPrefs {

inline constexpr auto kMediaPreviewType = "m.media_preview_config";
inline constexpr auto kRecentEmojiType = "m.recent_emoji";

// ---- MSC4278 ---------------------------------------------------------------

struct MediaPreview {
    // "on" | "private" | "off". An ABSENT field is "on" (the MSC's default);
    // a value this build does not know is "off": a newer client's third mode
    // must never read as permission to load.
    QString mediaPreviews = QStringLiteral("on");
    bool inviteAvatars = true;
};

MediaPreview parseMediaPreview(const QVariantMap &content);

// `existing` is the last content the account held; fields this build does not
// own are carried over unchanged.
QVariantMap composeMediaPreview(const QVariantMap &existing,
                                const QString &mediaPreviews,
                                bool inviteAvatars);

// Whether media in a room should start collapsed. "private" loads in a
// private room (here: encrypted or a direct message) only.
bool collapsesMedia(const QString &mediaPreviews, bool roomIsPrivate);

// ---- m.recent_emoji --------------------------------------------------------

// One stable `m.recent_emoji` entry. `extra` holds any other fields the entry
// carried, written back unchanged. `total` is a non-negative integer below
// 2^53 (the schema's range); Lightning never decrements or clamps a received
// value.
struct EmojiCount {
    QString emoji;
    qint64 total = 0;
    QVariantMap extra;
    EmojiCount() = default;
    EmojiCount(const QString &e, qint64 t) : emoji(e), total(t) {}
    bool operator==(const EmojiCount &o) const
    {
        return emoji == o.emoji && total == o.total;
    }
};
inline constexpr int kRecentEmojiServerLimit = 100;
// The schema's exclusive upper bound for `total`.
inline constexpr qint64 kRecentEmojiTotalBound = qint64(1) << 53;

// Wire order is preserved (most recently used first, by the standard's
// sending rule). Schema-invalid entries (no string `emoji`, a `total` that is
// not an integer in [0, 2^53)) are dropped; schema-valid ones are kept even
// when this build cannot display the emoji. Legacy [emoji, count] tuples are
// imported once; the next write emits objects.
QList<EmojiCount> parseRecentEmoji(const QVariantMap &content);

// One successful use: the emoji moves to the FRONT and its total grows by one
// (saturating below 2^53, never decreasing); the list is capped at 100.
QList<EmojiCount> bumpRecentEmoji(QList<EmojiCount> list, const QString &emoji);

// Writes the stable object shape. Other fields of `existing` are kept.
QVariantMap composeRecentEmoji(const QVariantMap &existing,
                               const QList<EmojiCount> &list);

// Lightning's own most-recent-first list stays first (what you just used);
// the account's entries that it lacks follow in the account's order.
QStringList mergeRecentEmoji(const QStringList &localMru,
                             const QList<EmojiCount> &server, int limit);

} // namespace AccountPrefs
