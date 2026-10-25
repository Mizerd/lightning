#include "privacy/AccountPrefs.h"

#include <QVariantList>
#include <algorithm>
#include <cmath>

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

namespace {

// An integer in [0, 2^53), from whichever numeric type the JSON decoder chose.
bool readTotal(const QVariant &v, qint64 *out)
{
    switch (v.metaType().id()) {
    case QMetaType::Int:
    case QMetaType::LongLong:
    case QMetaType::UInt:
    case QMetaType::ULongLong: {
        bool ok = false;
        const qint64 n = v.toLongLong(&ok);
        if (!ok || n < 0 || n >= kRecentEmojiTotalBound)
            return false;
        *out = n;
        return true;
    }
    case QMetaType::Double: {
        const double d = v.toDouble();
        if (!(d >= 0) || d >= double(kRecentEmojiTotalBound) || d != std::floor(d))
            return false;
        *out = qint64(d);
        return true;
    }
    default:
        return false;
    }
}

// A sanity bound only: a stored "emoji" is short; this keeps one hostile entry
// from carrying megabytes, without refusing long future ZWJ sequences.
constexpr int kMaxEmojiChars = 256;

} // namespace

QList<EmojiCount> parseRecentEmoji(const QVariantMap &content)
{
    QList<EmojiCount> out;
    const QVariantList rows = content.value(QStringLiteral("recent_emoji")).toList();
    for (const QVariant &row : rows) {
        EmojiCount entry;
        if (row.metaType().id() == QMetaType::QVariantMap) {
            QVariantMap object = row.toMap();
            const QVariant emoji = object.take(QStringLiteral("emoji"));
            const QVariant total = object.take(QStringLiteral("total"));
            if (emoji.metaType().id() != QMetaType::QString
                || !readTotal(total, &entry.total))
                continue;
            entry.emoji = emoji.toString();
            entry.extra = object;
        } else {
            // Legacy tuple [emoji, count] from older Element builds.
            const QVariantList pair = row.toList();
            if (pair.size() != 2 || pair.at(0).metaType().id() != QMetaType::QString
                || !readTotal(pair.at(1), &entry.total))
                continue;
            entry.emoji = pair.at(0).toString();
        }
        if (entry.emoji.isEmpty() || entry.emoji.size() > kMaxEmojiChars)
            continue;
        const bool dup = std::any_of(out.cbegin(), out.cend(),
            [&](const EmojiCount &e) { return e.emoji == entry.emoji; });
        if (!dup)
            out.append(entry);
        if (out.size() >= kRecentEmojiServerLimit)
            break;
    }
    return out;
}

QList<EmojiCount> bumpRecentEmoji(QList<EmojiCount> list, const QString &emoji)
{
    if (emoji.isEmpty())
        return list;
    EmojiCount used(emoji, 0);
    for (qsizetype i = 0; i < list.size(); ++i) {
        if (list.at(i).emoji == emoji) {
            used = list.takeAt(i);
            break;
        }
    }
    // Saturate below the schema bound; never wrap, never decrease.
    if (used.total < kRecentEmojiTotalBound - 1)
        ++used.total;
    list.prepend(used);
    while (list.size() > kRecentEmojiServerLimit)
        list.removeLast();
    return list;
}

QVariantMap composeRecentEmoji(const QVariantMap &existing,
                               const QList<EmojiCount> &list)
{
    QVariantList rows;
    for (const EmojiCount &e : list) {
        QVariantMap object = e.extra;
        object.insert(QStringLiteral("emoji"), e.emoji);
        object.insert(QStringLiteral("total"), e.total);
        rows.append(object);
    }
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
        if (!out.contains(e.emoji))
            out.append(e.emoji);
    while (out.size() > limit)
        out.removeLast();
    return out;
}

} // namespace AccountPrefs
