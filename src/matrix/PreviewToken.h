#pragma once

#include <QCoreApplication>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QStringView>

// A room-list preview as WHAT it says, not the words saying it, so it can be
// worded again in whatever language is current. A preview worded once and
// kept as text ("You joined", "Image", "Unable to decrypt") stayed in the
// language it was first written in after the user switched (2026-10-18 live
// runs, every platform). A free-text preview (a message body, a file name) is
// Kind::Text and is shown as it is.
//
// Header-only on purpose: every test target that compiles RoomInfo users
// gets it without another source file. The words keep their "EventPreview"
// catalogue context, so moving them here changed no translation key.
namespace matrix::preview {

// APPEND-ONLY. The numbers are persisted (CacheStore rooms.preview_kind), so
// an existing value never changes meaning: add new kinds at the end, never
// reorder or remove. A build that meets a number it does not know keeps the
// stored words (isKnownKind).
enum class Kind : quint8 {
    Text = 0,      // arg is the text itself
    Removed,       // "Message removed"
    Undecryptable, // "Unable to decrypt"
    OwnJoin,       // "You joined"
    Image,         // "Image"
    Gif,           // "GIF"
    File,          // "File"
    FileNamed,     // "File: <arg>"
    Video,         // "Video"
    Audio,         // "Audio"
    Voice,         // "Voice message"
    Sticker,       // "Sticker"
    Poll,          // "Poll"
    PollQuestion,  // "Poll: <arg>"
    LiveLocation,  // "Live location"
    Images,        // "<arg> images", arg is the count
    Attachments,   // "<arg> attachments", arg is the count
    StateSentence, // a room-state or membership sentence; arg is
                   // packStateSentence(sentence, actor, target)
};

inline constexpr int kKindCount = int(Kind::StateSentence) + 1;

// A stored or received kind number this build understands. An unknown one
// (written by a newer build) is not guessed at: the caller keeps the text.
inline bool isKnownKind(int value)
{
    return value >= 0 && value < kKindCount;
}

namespace detail {

inline QString t(const char *source)
{
    return QCoreApplication::translate("EventPreview", source);
}

// A count with plural forms. A catalogue without the entry yields the source
// "%n image(s)"; English then gets a real plural instead of "(s)".
inline QString countText(const char *source, const QString &englishOne,
                         const QString &englishMany, int n)
{
    const QString raw =
        QCoreApplication::translate("EventPreview", source, nullptr, n);
    if (raw == QString::fromLatin1(source).replace(QLatin1String("%n"),
                                                   QString::number(n)))
        return (n == 1 ? englishOne : englishMany).arg(n);
    return raw;
}

} // namespace detail

// A room-state or membership row's sentence, from the closed name the Rust
// bridge sends as `state_sentence` (rust/src/timeline.rs, `state_sentence`
// and `membership_sentence`), the actor (the sender, as presented) and the
// target member. Empty for a name this build does not know: the caller then
// shows the bridge's English body. Names are untrusted plain text; with
// `isolate` each is wrapped in U+2068/U+2069 (first-strong isolate), so a
// right-to-left name cannot reorder the sentence around it. Safe because
// presentable_name::sanitized() strips isolates from the names themselves.
inline QString stateSentence(QStringView sentence, QString actor, QString target,
                             bool isolate = true)
{
    using detail::t;
    if (isolate) {
        const auto wrap = [](QString &name) {
            if (!name.isEmpty())
                name = QChar(0x2068) + name + QChar(0x2069);
        };
        wrap(actor);
        wrap(target);
    }
    if (sentence == QLatin1String("room_created"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 created the room.")).arg(actor);
    if (sentence == QLatin1String("room_name_changed"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 changed the room name.")).arg(actor);
    if (sentence == QLatin1String("room_name_removed"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 removed the room name.")).arg(actor);
    if (sentence == QLatin1String("room_topic_removed"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 removed the room topic.")).arg(actor);
    if (sentence == QLatin1String("room_topic_changed"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 changed the room topic.")).arg(actor);
    if (sentence == QLatin1String("room_avatar_changed"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 changed the room avatar.")).arg(actor);
    if (sentence == QLatin1String("encryption_enabled"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "Encryption was enabled."));
    if (sentence == QLatin1String("room_upgraded"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 upgraded this room.")).arg(actor);
    if (sentence == QLatin1String("call_started"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 started a call.")).arg(actor);
    if (sentence == QLatin1String("video_call_started"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 started a video call.")).arg(actor);
    if (sentence == QLatin1String("policy_rule_updated"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 updated a moderation rule.")).arg(actor);
    if (sentence == QLatin1String("settings_updated"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 updated room settings.")).arg(actor);
    if (sentence == QLatin1String("space_parent_removed"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 removed this room from a space.")).arg(actor);
    if (sentence == QLatin1String("policy_rule_removed"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 removed a moderation rule.")).arg(actor);
    if (sentence == QLatin1String("member_joined"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 joined the room.")).arg(target);
    if (sentence == QLatin1String("member_left"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 left the room.")).arg(target);
    if (sentence == QLatin1String("member_invited"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 invited %2.")).arg(actor, target);
    if (sentence == QLatin1String("invitation_accepted"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 accepted the invitation.")).arg(target);
    if (sentence == QLatin1String("member_kicked"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 removed %2 from the room.")).arg(actor, target);
    if (sentence == QLatin1String("member_banned"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 banned %2.")).arg(actor, target);
    if (sentence == QLatin1String("member_unbanned"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 unbanned %2.")).arg(actor, target);
    if (sentence == QLatin1String("invitation_rejected"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 rejected the invitation.")).arg(target);
    if (sentence == QLatin1String("invitation_revoked"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "%1 revoked %2's invitation.")).arg(actor, target);
    if (sentence == QLatin1String("membership_changed"))
        return t(QT_TRANSLATE_NOOP("EventPreview", "Membership for %1 changed.")).arg(target);
    return {};
}

// Every sentence name this build words. stateSentence() must word each one
// (StateSentenceTest checks both directions against the Rust table).
inline const QSet<QString> &stateSentenceNames()
{
    static const QSet<QString> names = {
        QStringLiteral("room_created"), QStringLiteral("room_name_changed"),
        QStringLiteral("room_name_removed"), QStringLiteral("room_topic_removed"),
        QStringLiteral("room_topic_changed"), QStringLiteral("room_avatar_changed"),
        QStringLiteral("encryption_enabled"), QStringLiteral("room_upgraded"),
        QStringLiteral("call_started"), QStringLiteral("video_call_started"),
        QStringLiteral("policy_rule_updated"), QStringLiteral("settings_updated"),
        QStringLiteral("space_parent_removed"), QStringLiteral("policy_rule_removed"),
        QStringLiteral("member_joined"), QStringLiteral("member_left"),
        QStringLiteral("member_invited"), QStringLiteral("invitation_accepted"),
        QStringLiteral("member_kicked"), QStringLiteral("member_banned"),
        QStringLiteral("member_unbanned"), QStringLiteral("invitation_rejected"),
        QStringLiteral("invitation_revoked"), QStringLiteral("membership_changed"),
    };
    return names;
}

// Whether `sentence` is one this build words, so a stored or received name
// is trusted as a closed value. A set lookup: the ingest asks it per row.
inline bool isStateSentence(const QString &sentence)
{
    return stateSentenceNames().contains(sentence);
}

// One argument for Kind::StateSentence: the sentence name, actor, target
// and the bridge's English words as a fallback, joined by U+001F. A
// separator inside a field is dropped, so a display name cannot shift the
// fields.
inline QString packStateSentence(const QString &sentence, QString actor,
                                 QString target, QString fallback = QString())
{
    const QChar sep(0x1f);
    actor.remove(sep);
    target.remove(sep);
    fallback.remove(sep);
    return sentence + sep + actor + sep + target + sep + fallback;
}

// The words for `kind` in the language whose translators are installed now.
inline QString render(Kind kind, const QString &arg)
{
    using detail::t;
    switch (kind) {
    case Kind::Text:
        return arg;
    case Kind::Removed:
        return t(QT_TRANSLATE_NOOP("EventPreview", "Message removed"));
    case Kind::Undecryptable:
        return t(QT_TRANSLATE_NOOP("EventPreview", "Unable to decrypt"));
    case Kind::OwnJoin:
        return t(QT_TRANSLATE_NOOP("EventPreview", "You joined"));
    case Kind::Image:
        return t(QT_TRANSLATE_NOOP("EventPreview", "Image"));
    case Kind::Gif:
        return t(QT_TRANSLATE_NOOP("EventPreview", "GIF"));
    case Kind::File:
        return t(QT_TRANSLATE_NOOP("EventPreview", "File"));
    case Kind::FileNamed:
        return t(QT_TRANSLATE_NOOP("EventPreview", "File: %1")).arg(arg);
    case Kind::Video:
        return t(QT_TRANSLATE_NOOP("EventPreview", "Video"));
    case Kind::Audio:
        return t(QT_TRANSLATE_NOOP("EventPreview", "Audio"));
    case Kind::Voice:
        return t(QT_TRANSLATE_NOOP("EventPreview", "Voice message"));
    case Kind::Sticker:
        return t(QT_TRANSLATE_NOOP("EventPreview", "Sticker"));
    case Kind::Poll:
        return t(QT_TRANSLATE_NOOP("EventPreview", "Poll"));
    case Kind::PollQuestion:
        return t(QT_TRANSLATE_NOOP("EventPreview", "Poll: %1")).arg(arg);
    case Kind::LiveLocation:
        return t(QT_TRANSLATE_NOOP("EventPreview", "Live location"));
    case Kind::Images:
        return detail::countText(QT_TRANSLATE_N_NOOP("EventPreview", "%n image(s)"),
                                 QStringLiteral("%1 image"),
                                 QStringLiteral("%1 images"), arg.toInt());
    case Kind::Attachments:
        return detail::countText(
            QT_TRANSLATE_N_NOOP("EventPreview", "%n attachment(s)"),
            QStringLiteral("%1 attachment"), QStringLiteral("%1 attachments"),
            arg.toInt());
    case Kind::StateSentence: {
        // Never an empty row: a name this build does not know (a newer
        // build's, read back from the cache) shows the English the bridge
        // sent, else the generic sentence.
        const QStringList parts = arg.split(QChar(0x1f));
        const QString actor = parts.value(1);
        const QString worded =
            stateSentence(parts.value(0), actor, parts.value(2));
        if (!worded.isEmpty())
            return worded;
        if (!parts.value(3).isEmpty())
            return parts.value(3);
        return stateSentence(u"settings_updated", actor, QString());
    }
    }
    return arg;
}

// The names the Rust bridge sends in `last_message_preview_kind` and
// `preview_kind`: the `kind_preview(...)` calls in rust/src/lib.rs
// `raw_event_preview_line`. Anything else,
// including "", is Text: the English words it also sends are shown instead.
inline Kind kindFromWire(QStringView name)
{
    if (name == QLatin1String("image")) return Kind::Image;
    if (name == QLatin1String("file")) return Kind::File;
    if (name == QLatin1String("video")) return Kind::Video;
    if (name == QLatin1String("audio")) return Kind::Audio;
    if (name == QLatin1String("voice")) return Kind::Voice;
    if (name == QLatin1String("sticker")) return Kind::Sticker;
    if (name == QLatin1String("poll")) return Kind::Poll;
    if (name == QLatin1String("poll_question")) return Kind::PollQuestion;
    if (name == QLatin1String("images")) return Kind::Images;
    if (name == QLatin1String("attachments")) return Kind::Attachments;
    return Kind::Text;
}

} // namespace matrix::preview
