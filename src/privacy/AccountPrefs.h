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
    // "Private rooms" now means NON-PUBLIC join rule (MSC4278's own meaning,
    // as Element reads it); Lightning used to read it as "encrypted or a
    // direct message". True once the account recorded that the user chose
    // "Private rooms only" under the new meaning (kPrivateRoomsMarker). Until
    // then a compatibility guard keeps the old, narrower set loading, so an
    // existing choice is never silently broadened.
    bool privateMeansNonPublic = false;
};

// Lightning's own field inside m.media_preview_config recording the adoption
// above. Other clients ignore it (writes keep fields they do not own).
inline constexpr auto kPrivateRoomsMarker = "org.lightning_matrix.private_rooms";

MediaPreview parseMediaPreview(const QVariantMap &content);

// `existing` is the last content the account held; fields this build does not
// own are carried over unchanged.
QVariantMap composeMediaPreview(const QVariantMap &existing,
                                const QString &mediaPreviews,
                                bool inviteAvatars,
                                bool privateMeansNonPublic = false);

// Whether a join rule is a known non-public one. "" (unknown) is NOT.
bool joinRuleIsNonPublic(const QString &joinRule);

// Whether media in a room should start collapsed (collapsed media is one line
// with an explicit reveal, and fetches nothing):
//   "on"      -> never;
//   "private" -> unless the room's join rule is known and non-public; an
//                unknown join rule collapses (conservative). Before the
//                account adopted the non-public meaning, the room must ALSO be
//                encrypted or a direct message (the old meaning), so the set
//                that loads can only shrink, never grow, until the user
//                chooses "Private rooms only" again;
//   "off" and anything unknown -> always.
bool collapsesMedia(const QString &mediaPreviews, const QString &joinRule,
                    bool encrypted, bool direct, bool privateMeansNonPublic);

// ---- incoming-invite policy (spec v1.18 / MSC4380) --------------------------
//
// Server-side rejection of invitations, honoured only by a homeserver that
// implements it (AccountPrefsController asks /versions first). Not the same
// thing as the invite push rule (notification only) or ignoring a user.

inline constexpr auto kInvitePermissionType = "m.invite_permission_config";
inline constexpr auto kInvitePermissionUnstableType =
    "org.matrix.msc4380.invite_permission_config";

// Stable: `default_action` "block" blocks; missing, invalid or unknown values
// mean "receive invites as normal" (the spec's wording).
bool stableInvitePolicyBlocks(const QVariantMap &content);
// MSC4380: `block_all` true blocks; anything else does not.
bool unstableInvitePolicyBlocks(const QVariantMap &content);

struct AccountDataWrite {
    QString type;
    QVariantMap content;
};

// The writes that make the account's policy `block` (or allow-all), given
// what the server supports and what each type holds now. Fields this build
// does not own are kept. Allowing clears a block in EITHER form, so a stale
// rule cannot stay active behind a switch that says invites are allowed;
// blocking writes the stable form when the server supports it, the MSC4380
// form otherwise. Empty when nothing needs to change, or neither form is
// supported (the setting is then not offered at all).
QList<AccountDataWrite> composeInvitePolicy(bool stableSupported,
                                            bool unstableSupported,
                                            const QVariantMap &stableContent,
                                            const QVariantMap &unstableContent,
                                            bool block);

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
