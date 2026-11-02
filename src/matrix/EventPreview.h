#pragma once

#include "matrix/PresentableName.h"
#include "matrix/PreviewToken.h"

#include <QString>

struct TimelineEvent;

namespace matrix::preview {

QString undecryptablePreview();
QString ownJoinPreview();

QString oneLineSummary(const TimelineEvent &event,
                       const QString &actorLabel = QString());

struct Token {
    Kind kind = Kind::Text;
    QString arg;
};
Token summaryToken(const TimelineEvent &event,
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

inline constexpr int kReplyPreviewMaxChars = 320;

QString normalizePreviewText(const QString &text, int maxChars = 120);

QString stripReplyFallback(const QString &body);

}
