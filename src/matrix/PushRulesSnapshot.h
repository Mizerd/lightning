#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

// The notification settings page's view of the account's push rules, as the
// Rust bridge reports it (`push_rules_snapshot`, rust/src/pushrules.rs) turned
// into the flat map QML and PushRulesController read.
//
// Keys:
//   masterMute            bool   the .m.rule.master override is on
//   dmAll, roomsAll       bool   the default level is "all messages"
//   dmDrift, roomsDrift   bool   the plain and the encrypted rule disagree
//   mentionUserId, mentionDisplayName, mentionUsername, mentionRoom
//   eventReactions, eventInvites, eventNotices
//                         int    -1 the server has no such rule, 0 Off,
//                                1 Notify, 2 Loud
//   keywords              list of { keyword: string, level: 0|1|2 }
namespace PushRulesSnapshot {

enum Level { Absent = -1, Off = 0, Notify = 1, Loud = 2 };


namespace detail {

inline int levelOf(const QJsonObject &row)
{
    if (!row.value(QStringLiteral("present")).toBool())
        return Absent;
    const QString level = row.value(QStringLiteral("level")).toString();
    if (level == QLatin1String("loud"))
        return Loud;
    if (level == QLatin1String("notify"))
        return Notify;
    return Off;
}

} // namespace detail

inline QVariantMap empty()
{
    QVariantMap out;
    out.insert(QStringLiteral("masterMute"), false);
    out.insert(QStringLiteral("dmAll"), true);
    out.insert(QStringLiteral("dmDrift"), false);
    out.insert(QStringLiteral("roomsAll"), true);
    out.insert(QStringLiteral("roomsDrift"), false);
    for (const char *key : {"mentionUserId", "mentionDisplayName",
                            "mentionUsername", "mentionRoom", "eventReactions",
                            "eventInvites", "eventNotices"}) {
        out.insert(QLatin1String(key), static_cast<int>(Absent));
    }
    out.insert(QStringLiteral("keywords"), QVariantList());
    return out;
}

inline QVariantMap fromRustEvent(const QJsonObject &event)
{
    QVariantMap out = empty();
    out.insert(QStringLiteral("masterMute"),
               event.value(QStringLiteral("master_mute")).toBool());

    const QJsonObject dm = event.value(QStringLiteral("dm")).toObject();
    out.insert(QStringLiteral("dmAll"),
               dm.value(QStringLiteral("level")).toString()
                   != QLatin1String("mentions"));
    out.insert(QStringLiteral("dmDrift"),
               dm.value(QStringLiteral("drift")).toBool());
    const QJsonObject rooms = event.value(QStringLiteral("rooms")).toObject();
    out.insert(QStringLiteral("roomsAll"),
               rooms.value(QStringLiteral("level")).toString()
                   != QLatin1String("mentions"));
    out.insert(QStringLiteral("roomsDrift"),
               rooms.value(QStringLiteral("drift")).toBool());

    const QJsonObject mentions =
        event.value(QStringLiteral("mentions")).toObject();
    out.insert(QStringLiteral("mentionUserId"),
               detail::levelOf(mentions.value(QStringLiteral("user_id")).toObject()));
    out.insert(QStringLiteral("mentionDisplayName"),
               detail::levelOf(mentions.value(QStringLiteral("display_name")).toObject()));
    out.insert(QStringLiteral("mentionUsername"),
               detail::levelOf(mentions.value(QStringLiteral("username")).toObject()));
    out.insert(QStringLiteral("mentionRoom"),
               detail::levelOf(mentions.value(QStringLiteral("room")).toObject()));

    const QJsonObject events = event.value(QStringLiteral("events")).toObject();
    out.insert(QStringLiteral("eventReactions"),
               detail::levelOf(events.value(QStringLiteral("reactions")).toObject()));
    out.insert(QStringLiteral("eventInvites"),
               detail::levelOf(events.value(QStringLiteral("invites")).toObject()));
    out.insert(QStringLiteral("eventNotices"),
               detail::levelOf(events.value(QStringLiteral("notices")).toObject()));

    QVariantList keywords;
    const QJsonArray raw = event.value(QStringLiteral("keywords")).toArray();
    keywords.reserve(raw.size());
    for (const QJsonValue &value : raw) {
        const QJsonObject item = value.toObject();
        const QString keyword = item.value(QStringLiteral("keyword")).toString();
        if (keyword.isEmpty())
            continue;
        QJsonObject present = item;
        present.insert(QStringLiteral("present"), true);
        QVariantMap entry;
        entry.insert(QStringLiteral("keyword"), keyword);
        entry.insert(QStringLiteral("level"), detail::levelOf(present));
        keywords.append(entry);
    }
    out.insert(QStringLiteral("keywords"), keywords);
    return out;
}



} // namespace PushRulesSnapshot
