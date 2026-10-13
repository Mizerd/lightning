#include "privacy/AccountPrefs.h"

#include <QVariantList>
#include <algorithm>

namespace AccountPrefs {

MediaPreview parseMediaPreview(const QVariantMap &content)
{
    MediaPreview out;
    if (content.contains(QStringLiteral("media_previews"))) {
        const QVariant v = content.value(QStringLiteral("media_previews"));
        const QString s = v.metaType().id() == QMetaType::QString
                              ? v.toString() : QString();
        if (s == QLatin1String("on") || s == QLatin1String("private")
            || s == QLatin1String("off"))
            out.mediaPreviews = s;
        else
            out.mediaPreviews = QStringLiteral("off");
    }
    if (content.contains(QStringLiteral("invite_avatars"))) {
        const QVariant v = content.value(QStringLiteral("invite_avatars"));
        out.inviteAvatars = v.metaType().id() == QMetaType::QString
                            && v.toString() == QLatin1String("on");
    }
    return out;
}

QVariantMap composeMediaPreview(const QVariantMap &existing,
                                const QString &mediaPreviews, bool inviteAvatars)
{
    QVariantMap out = existing;
    const bool known = mediaPreviews == QLatin1String("on")
                       || mediaPreviews == QLatin1String("private");
    out.insert(QStringLiteral("media_previews"),
               known ? mediaPreviews : QStringLiteral("off"));
    out.insert(QStringLiteral("invite_avatars"),
               inviteAvatars ? QStringLiteral("on") : QStringLiteral("off"));
    return out;
}

bool collapsesMedia(const QString &mediaPreviews, bool roomIsPrivate)
{
    if (mediaPreviews == QLatin1String("on"))
        return false;
    if (mediaPreviews == QLatin1String("private"))
        return !roomIsPrivate;
    return true; // "off", and anything unknown
}

QList<EmojiCount> parseRecentEmoji(const QVariantMap &content)
{
    QList<EmojiCount> out;
    const QVariantList rows = content.value(QStringLiteral("recent_emoji")).toList();
    for (const QVariant &row : rows) {
        const QVariantList pair = row.toList();
        if (pair.size() != 2 || pair.at(0).metaType().id() != QMetaType::QString)
            continue;
        const QString emoji = pair.at(0).toString();
        bool ok = false;
        const int count = pair.at(1).toInt(&ok);
        if (emoji.isEmpty() || emoji.size() > 32 || !ok || count < 1)
            continue;
        const bool dup = std::any_of(out.cbegin(), out.cend(),
            [&](const EmojiCount &e) { return e.first == emoji; });
        if (!dup)
            out.append({ emoji, count });
        if (out.size() >= kRecentEmojiServerLimit)
            break;
    }
    return out;
}

QList<EmojiCount> bumpRecentEmoji(QList<EmojiCount> list, const QString &emoji)
{
    if (emoji.isEmpty())
        return list;
    bool found = false;
    for (EmojiCount &e : list) {
        if (e.first == emoji) {
            e.second = std::min(e.second + 1, 1000000);
            found = true;
            break;
        }
    }
    if (!found)
        list.append({ emoji, 1 });
    std::stable_sort(list.begin(), list.end(),
                     [](const EmojiCount &a, const EmojiCount &b) {
                         return a.second > b.second;
                     });
    while (list.size() > kRecentEmojiServerLimit)
        list.removeLast();
    return list;
}

QVariantMap composeRecentEmoji(const QVariantMap &existing,
                               const QList<EmojiCount> &list)
{
    QVariantList rows;
    for (const EmojiCount &e : list)
        rows.append(QVariant(QVariantList{ e.first, e.second }));
    QVariantMap out = existing;
    out.insert(QStringLiteral("recent_emoji"), rows);
    return out;
}

QStringList mergeRecentEmoji(const QStringList &localMru,
                             const QList<EmojiCount> &server, int limit)
{
    QStringList out;
    for (const QString &e : localMru)
        if (!e.isEmpty() && !out.contains(e))
            out.append(e);
    for (const EmojiCount &e : server)
        if (!out.contains(e.first))
            out.append(e.first);
    while (out.size() > limit)
        out.removeLast();
    return out;
}

} // namespace AccountPrefs
