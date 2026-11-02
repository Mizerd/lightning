#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace AccountPrefs {

inline constexpr auto kMediaPreviewType = "m.media_preview_config";
inline constexpr auto kRecentEmojiType = "m.recent_emoji";

struct MediaPreview {
    // "on" | "private" | "off". An ABSENT field is "on" (the MSC's default);
    // a value this build does not know is "off": a newer client's third mode
    // must never read as permission to load.
    QString mediaPreviews = QStringLiteral("on");
    bool inviteAvatars = true;
    bool privateMeansNonPublic = false;
};

inline constexpr auto kPrivateRoomsMarker = "org.lightning_matrix.private_rooms";

MediaPreview parseMediaPreview(const QVariantMap &content);

QVariantMap composeMediaPreview(const QVariantMap &existing,
                                const QString &mediaPreviews,
                                bool inviteAvatars,
                                bool privateMeansNonPublic = false);

bool joinRuleIsNonPublic(const QString &joinRule);

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

bool stableInvitePolicyBlocks(const QVariantMap &content);
bool unstableInvitePolicyBlocks(const QVariantMap &content);

struct AccountDataWrite {
    QString type;
    QVariantMap content;
};

QList<AccountDataWrite> composeInvitePolicy(bool stableSupported,
                                            bool unstableSupported,
                                            const QVariantMap &stableContent,
                                            const QVariantMap &unstableContent,
                                            bool block);

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
inline constexpr qint64 kRecentEmojiTotalBound = qint64(1) << 53;

QList<EmojiCount> parseRecentEmoji(const QVariantMap &content);

QList<EmojiCount> bumpRecentEmoji(QList<EmojiCount> list, const QString &emoji);

QVariantMap composeRecentEmoji(const QVariantMap &existing,
                               const QList<EmojiCount> &list);

QStringList mergeRecentEmoji(const QStringList &localMru,
                             const QList<EmojiCount> &server, int limit);

}
