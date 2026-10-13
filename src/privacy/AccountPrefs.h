#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariantMap>

// Pure parsing and composing of two standard account-data preferences:
//   * MSC4278 `m.media_preview_config` { invite_avatars, media_previews }
//   * Element's `m.recent_emoji` { recent_emoji: [[emoji, count], ...] }
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

using EmojiCount = QPair<QString, int>;
inline constexpr int kRecentEmojiServerLimit = 100;

// Malformed entries are skipped, never fatal; order is the stored order.
QList<EmojiCount> parseRecentEmoji(const QVariantMap &content);

// One use: +1 for the emoji (inserted at 1), most-used first (stable), capped.
QList<EmojiCount> bumpRecentEmoji(QList<EmojiCount> list, const QString &emoji);

// Other fields of `existing` are kept.
QVariantMap composeRecentEmoji(const QVariantMap &existing,
                               const QList<EmojiCount> &list);

// Lightning's own most-recent-first list stays first (what you just used);
// the account's entries that it lacks follow in the account's order.
QStringList mergeRecentEmoji(const QStringList &localMru,
                             const QList<EmojiCount> &server, int limit);

} // namespace AccountPrefs
