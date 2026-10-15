#include "matrix/EventPreview.h"

#include "matrix/TimelineEvent.h"

#include <QCoreApplication>
#include <QRegularExpression>

#include <algorithm>

namespace matrix::preview {

namespace {

// Context "EventPreview": translate() rather than tr() because these are free
// functions. With no translator installed translate() returns the source.
QString t(const char *source)
{
    return QCoreApplication::translate("EventPreview", source);
}

// A count with plural forms. A catalogue without the entry yields the source
// "%n image(s)"; English then gets a real plural instead of "(s)".
QString countText(const char *source, const QString &englishOne,
                  const QString &englishMany, int n)
{
    const QString raw = QCoreApplication::translate("EventPreview", source,
                                                    nullptr, n);
    if (raw == QString::fromLatin1(source).replace(QLatin1String("%n"),
                                                   QString::number(n)))
        return (n == 1 ? englishOne : englishMany).arg(n);
    return raw;
}

} // namespace

QString stripReplyFallback(const QString &body)
{
    QString b = body;
    // HTML body: the <mx-reply> block opens it and does not nest.
    if (b.startsWith(QLatin1String("<mx-reply>"))) {
        const int end = b.indexOf(QLatin1String("</mx-reply>"));
        if (end >= 0)
            return b.mid(end + int(QLatin1String("</mx-reply>").size()));
        return b;
    }
    if (!(b.startsWith(QLatin1String("> <@"))
          || b.startsWith(QLatin1String("> * <@"))))
        return b;
    int pos = 0;
    for (;;) {
        const int nl = b.indexOf(QLatin1Char('\n'), pos);
        if (nl < 0)
            return b; // quote runs to the end: not a fallback
        QStringView line = QStringView(b).mid(pos, nl - pos);
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        if (line.isEmpty())
            return b.mid(nl + 1);
        if (line == QLatin1String(">") || line.startsWith(QLatin1String("> ")))
            pos = nl + 1;
        else
            return b; // text before any blank line: not a fallback
    }
}

QString normalizePreviewText(const QString &text, int maxChars)
{
    QString s = stripReplyFallback(text);
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

QString undecryptablePreview()
{
    return t(QT_TRANSLATE_NOOP("EventPreview", "Unable to decrypt"));
}

QString oneLineSummary(const TimelineEvent &event, const QString &actorLabel)
{
    if (event.redacted)
        return t(QT_TRANSLATE_NOOP("EventPreview", "Message removed"));
    if (event.undecryptable)
        return undecryptablePreview();
    // An MSC4274 gallery: the sender's caption, else what it holds. The row's
    // media fields name only the primary item.
    if (event.galleryItems.size() > 1) {
        const QString caption = normalizePreviewText(event.body);
        if (!caption.isEmpty())
            return caption;
        const bool allImages = std::all_of(
            event.galleryItems.cbegin(), event.galleryItems.cend(),
            [](const GalleryItem &g) { return g.kind == QLatin1String("image"); });
        const int n = int(event.galleryItems.size());
        return allImages
            ? countText(QT_TRANSLATE_NOOP("EventPreview", "%n image(s)"),
                        QStringLiteral("%1 image"), QStringLiteral("%1 images"), n)
            : countText(QT_TRANSLATE_NOOP("EventPreview", "%n attachment(s)"),
                        QStringLiteral("%1 attachment"),
                        QStringLiteral("%1 attachments"), n);
    }

    switch (event.type) {
    case TimelineEvent::Location:
        // A live location's body is the sender's description, or empty: the
        // bridge no longer writes an English label into it.
        if (event.locationLive) {
            const QString description = normalizePreviewText(event.body);
            return description.isEmpty()
                ? t(QT_TRANSLATE_NOOP("EventPreview", "Live location"))
                : description;
        }
        return normalizePreviewText(event.body);
    case TimelineEvent::Image:
        return event.mediaMimetype.compare(QLatin1String("image/gif"),
                                           Qt::CaseInsensitive) == 0
            ? t(QT_TRANSLATE_NOOP("EventPreview", "GIF"))
            : (event.mediaFilename.isEmpty()
                   ? t(QT_TRANSLATE_NOOP("EventPreview", "Image"))
                   : normalizePreviewText(event.mediaFilename));
    case TimelineEvent::File:
        return event.mediaFilename.isEmpty()
            ? t(QT_TRANSLATE_NOOP("EventPreview", "File"))
            : t(QT_TRANSLATE_NOOP("EventPreview", "File: %1"))
                  .arg(normalizePreviewText(event.mediaFilename));
    case TimelineEvent::Video:
        return event.mediaFilename.isEmpty()
            ? t(QT_TRANSLATE_NOOP("EventPreview", "Video"))
            : normalizePreviewText(event.mediaFilename);
    case TimelineEvent::Audio:
        return event.mediaIsVoice
            ? t(QT_TRANSLATE_NOOP("EventPreview", "Voice message"))
            : (event.mediaFilename.isEmpty()
                   ? t(QT_TRANSLATE_NOOP("EventPreview", "Audio"))
                   : normalizePreviewText(event.mediaFilename));
    case TimelineEvent::Sticker:
        return t(QT_TRANSLATE_NOOP("EventPreview", "Sticker"));
    case TimelineEvent::Poll: {
        // Never the MSC3381 multi-line fallback; prefer the typed question,
        // else the fallback's first line.
        QString question = event.pollQuestion;
        if (question.isEmpty())
            question = event.body.section(QLatin1Char('\n'), 0, 0);
        const QString normalized = normalizePreviewText(question);
        return normalized.isEmpty()
            ? t(QT_TRANSLATE_NOOP("EventPreview", "Poll"))
            : t(QT_TRANSLATE_NOOP("EventPreview", "Poll: %1")).arg(normalized);
    }
    case TimelineEvent::StateChange:
        // The room list showed "@alice:example.org updated room settings."
        // where the person's name belongs (VM test, 2026-10-07).
        return normalizePreviewText(actorSentence(
            event.body, event.sender,
            actorLabel.isEmpty() ? event.senderDisplayName : actorLabel,
            event.senderNameAmbiguous));
    default:
        return normalizePreviewText(event.body);
    }
}

} // namespace matrix::preview
