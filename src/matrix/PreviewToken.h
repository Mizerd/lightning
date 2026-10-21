#pragma once

#include <QCoreApplication>
#include <QString>
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
};

inline constexpr int kKindCount = int(Kind::Attachments) + 1;

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
