#pragma once

#include "matrix/PresentableName.h"

#include <QString>

struct TimelineEvent;

// One-line event summaries for side surfaces (room list, DM list, desktop
// notifications). Bodies are free-form (newlines, markdown, MSC3381 poll
// fallbacks) and must never define side-surface geometry, so every consumer
// goes through this choke point instead of forwarding `body`.
namespace matrix::preview {

// Semantic one-line summary by event type: "Poll: <question>",
// "Voice message", "File: <name>", "Sticker", "Unable to decrypt",
// "Message removed", media labels, else the normalized body. A room-state
// row names its actor by `actorLabel` (else the event's senderDisplayName,
// else the localpart), never by the raw user id; see actorSentence.
QString oneLineSummary(const TimelineEvent &event,
                       const QString &actorLabel = QString());

// A room-state sentence from the Rust bridge opens with the sender's user id
// ("@alice:example.org changed the room name."), because the bridge phrases it
// before any profile is known. Replaces that leading id with how the person is
// presented (matrix/PresentableName.h, the incoming-call card's rule): the
// display name with bidi and invisible characters removed, the localpart when
// there is none, and "Name (localpart)" when the name looks like an address
// or another member uses it (`ambiguous`). A display name is room state any
// member writes; "@admin:example.org" as a name must not make a power-level
// row read exactly like the id-phrased original.
// Anything else is returned unchanged: a sentence that does not begin with
// `actorUserId` followed by a space or an apostrophe, or an empty id.
// Inline: TimelineModel uses it, and test targets compile TimelineModel.cpp
// without EventPreview.cpp.
inline QString actorSentence(const QString &sentence,
                             const QString &actorUserId, const QString &label,
                             bool ambiguous = false)
{
    if (actorUserId.isEmpty() || !sentence.startsWith(actorUserId))
        return sentence;
    // A whole id only: "@al" must not match "@alice:...".
    if (sentence.size() > actorUserId.size()) {
        const QChar next = sentence.at(actorUserId.size());
        if (next != QLatin1Char(' ') && next != QLatin1Char('\''))
            return sentence;
    }
    const QString name =
        presentable_name::presentable(actorUserId, label, ambiguous);
    if (name.isEmpty())
        return sentence;
    return name + sentence.mid(actorUserId.size());
}

// Presentation normalization, for summaries and plain-text previews (the
// Rust latest-event path):
//   - matrix.to markdown links reduce to their label ("[@x](…)" -> "@x"),
//   - newlines and line/paragraph separators become spaces,
//   - whitespace runs collapse,
//   - the result is bounded with a trailing ellipsis.
// A reply quote elides to its available width in the timeline, so its cap
// only keeps a pathological body out of the model.
inline constexpr int kReplyPreviewMaxChars = 320;

QString normalizePreviewText(const QString &text, int maxChars = 120);

} // namespace matrix::preview
