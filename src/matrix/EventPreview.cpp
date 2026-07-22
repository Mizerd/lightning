#include "matrix/EventPreview.h"

#include "matrix/TimelineEvent.h"

#include <QRegularExpression>

namespace matrix::preview {

QString normalizePreviewText(const QString &text, int maxChars)
{
    QString s = text;
    // A mention's plain-body fallback legitimately carries the matrix.to
    // permalink in markdown form; a one-line preview shows only the label.
    // Bounded quantifiers keep the pattern linear on adversarial input.
    static const QRegularExpression mentionLink(QStringLiteral(
        "\\[([^\\]\\n]{1,120})\\]\\(https://matrix\\.to/#/[^)\\s]{1,512}\\)"));
    s.replace(mentionLink, QStringLiteral("\\1"));
    // Unicode line/paragraph separators break lines in QML Text like '\n'
    // does; simplified() below only folds ASCII whitespace.
    s.replace(QChar(0x2028), QLatin1Char(' '));
    s.replace(QChar(0x2029), QLatin1Char(' '));
    s = s.simplified();
    if (maxChars > 1 && s.size() > maxChars) {
        int cut = maxChars - 1;
        // Never split a surrogate pair (astral-plane emoji) at the cut.
        if (cut > 0 && s.at(cut - 1).isHighSurrogate())
            --cut;
        s = s.left(cut) + QChar(0x2026);
    }
    return s;
}

QString oneLineSummary(const TimelineEvent &event)
{
    if (event.redacted)
        return QStringLiteral("Message removed");
    if (event.undecryptable)
        return QStringLiteral("Unable to decrypt");

    switch (event.type) {
    case TimelineEvent::Image:
        return event.mediaMimetype.compare(QLatin1String("image/gif"),
                                           Qt::CaseInsensitive) == 0
            ? QStringLiteral("GIF")
            : (event.mediaFilename.isEmpty()
                   ? QStringLiteral("Image")
                   : normalizePreviewText(event.mediaFilename));
    case TimelineEvent::File:
        return event.mediaFilename.isEmpty()
            ? QStringLiteral("File")
            : QStringLiteral("File: ")
                  + normalizePreviewText(event.mediaFilename);
    case TimelineEvent::Video:
        return event.mediaFilename.isEmpty()
            ? QStringLiteral("Video")
            : normalizePreviewText(event.mediaFilename);
    case TimelineEvent::Audio:
        return event.mediaIsVoice
            ? QStringLiteral("Voice message")
            : (event.mediaFilename.isEmpty()
                   ? QStringLiteral("Audio")
                   : normalizePreviewText(event.mediaFilename));
    case TimelineEvent::Sticker:
        return QStringLiteral("Sticker");
    case TimelineEvent::Poll: {
        // Never the MSC3381 multi-line fallback (question + one line per
        // answer). Prefer the typed question; degrade to the fallback's
        // first line.
        QString question = event.pollQuestion;
        if (question.isEmpty())
            question = event.body.section(QLatin1Char('\n'), 0, 0);
        const QString normalized = normalizePreviewText(question);
        return normalized.isEmpty()
            ? QStringLiteral("Poll")
            : QStringLiteral("Poll: ") + normalized;
    }
    default:
        return normalizePreviewText(event.body);
    }
}

} // namespace matrix::preview
