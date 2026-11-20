#include "matrix/EventPreview.h"

#include "matrix/TimelineEvent.h"

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace matrix::preview {

static QString decodeHtmlEntities(const QString &text)
{
    static const QRegularExpression entity(
        QStringLiteral("&(#[xX][0-9a-fA-F]{1,6}|#[0-9]{1,7}|[a-zA-Z]{2,8});"));
    QString out;
    out.reserve(text.size());
    qsizetype last = 0;
    auto it = entity.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        out += QStringView(text).mid(last, match.capturedStart() - last);
        last = match.capturedEnd();
        const QString name = match.captured(1);
        if (name.startsWith(QLatin1Char('#'))) {
            bool ok = false;
            const uint code = name.startsWith(QLatin1String("#x"),
                                              Qt::CaseInsensitive)
                ? name.mid(2).toUInt(&ok, 16) : name.mid(1).toUInt(&ok, 10);
            if (ok && code > 0 && code <= 0x10FFFF
                && !(code >= 0xD800 && code <= 0xDFFF)) {
                const char32_t c = code;
                out += QString::fromUcs4(&c, 1);
            }
            continue;
        }
        const QString lower = name.toLower();
        if (lower == QLatin1String("amp"))
            out += QLatin1Char('&');
        else if (lower == QLatin1String("lt"))
            out += QLatin1Char('<');
        else if (lower == QLatin1String("gt"))
            out += QLatin1Char('>');
        else if (lower == QLatin1String("quot"))
            out += QLatin1Char('"');
        else if (lower == QLatin1String("apos"))
            out += QLatin1Char('\'');
        else if (lower == QLatin1String("nbsp"))
            out += QLatin1Char(' ');
        else
            out += match.captured(0);
    }
    out += QStringView(text).mid(last);
    return out;
}

QString htmlPlainText(const QString &html, const QString &placeholder)
{
    const QString body = stripReplyFallback(html);
    static const QRegularExpression tag(
        QStringLiteral("<(/?)([a-zA-Z][a-zA-Z0-9-]*)([^>]*)>"));
    static const QStringList breaking{
        QStringLiteral("br"), QStringLiteral("p"), QStringLiteral("div"),
        QStringLiteral("li"), QStringLiteral("blockquote"),
        QStringLiteral("tr"), QStringLiteral("h1"), QStringLiteral("h2"),
        QStringLiteral("h3"), QStringLiteral("h4"), QStringLiteral("h5"),
        QStringLiteral("h6"), QStringLiteral("pre")
    };
    QString out;
    qsizetype last = 0;
    int spoilerDepth = 0;
    auto it = tag.globalMatch(body);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        if (spoilerDepth == 0)
            out += QStringView(body).mid(last, match.capturedStart() - last);
        last = match.capturedEnd();
        const bool closing = !match.captured(1).isEmpty();
        const QString name = match.captured(2).toLower();
        const QString attributes = match.captured(3);
        if (spoilerDepth > 0) {
            if (name == QLatin1String("span")) {
                if (closing)
                    --spoilerDepth;
                else if (!attributes.trimmed().endsWith(QLatin1Char('/')))
                    ++spoilerDepth;
            }
            continue;
        }
        if (!closing && name == QLatin1String("span")
            && attributes.contains(QLatin1String("data-mx-spoiler"),
                                   Qt::CaseInsensitive)) {
            out += QLatin1Char(' ') + placeholder + QLatin1Char(' ');
            spoilerDepth = 1;
            continue;
        }
        if (breaking.contains(name))
            out += QLatin1Char('\n');
    }
    if (spoilerDepth == 0)
        out += QStringView(body).mid(last);
    return decodeHtmlEntities(out);
}

QString stripReplyFallback(const QString &body)
{
    QString b = body;
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
            return b;
        QStringView line = QStringView(b).mid(pos, nl - pos);
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        if (line.isEmpty())
            return b.mid(nl + 1);
        if (line == QLatin1String(">") || line.startsWith(QLatin1String("> ")))
            pos = nl + 1;
        else
            return b;
    }
}

QString normalizePreviewText(const QString &text, int maxChars)
{
    QString s = stripReplyFallback(text);
    static const QRegularExpression mentionLink(QStringLiteral(
        "\\[([^\\]\\n]{1,120})\\]\\(https://matrix\\.to/#/[^)\\s]{1,512}\\)"));
    s.replace(mentionLink, QStringLiteral("\\1"));
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
    return render(Kind::Undecryptable, {});
}

QString ownJoinPreview()
{
    return render(Kind::OwnJoin, {});
}

QString oneLineSummary(const TimelineEvent &event, const QString &actorLabel)
{
    const Token token = summaryToken(event, actorLabel);
    return render(token.kind, token.arg);
}

Token summaryToken(const TimelineEvent &event, const QString &actorLabel)
{
    const auto text = [](const QString &s) { return Token{ Kind::Text, s }; };
    if (event.redacted)
        return { Kind::Removed, {} };
    if (event.undecryptable)
        return { Kind::Undecryptable, {} };
    if (event.galleryItems.size() > 1) {
        const QString caption = normalizePreviewText(event.body);
        if (!caption.isEmpty())
            return text(caption);
        const bool allImages = std::all_of(
            event.galleryItems.cbegin(), event.galleryItems.cend(),
            [](const GalleryItem &g) { return g.kind == QLatin1String("image"); });
        return { allImages ? Kind::Images : Kind::Attachments,
                 QString::number(event.galleryItems.size()) };
    }

    switch (event.type) {
    case TimelineEvent::Location:
        if (event.locationLive) {
            const QString description = normalizePreviewText(event.body);
            return description.isEmpty() ? Token{ Kind::LiveLocation, {} }
                                         : text(description);
        }
        return text(normalizePreviewText(event.body));
    case TimelineEvent::Image:
        if (event.mediaMimetype.compare(QLatin1String("image/gif"),
                                        Qt::CaseInsensitive) == 0)
            return { Kind::Gif, {} };
        return event.mediaFilename.isEmpty()
            ? Token{ Kind::Image, {} }
            : text(normalizePreviewText(event.mediaFilename));
    case TimelineEvent::File:
        return event.mediaFilename.isEmpty()
            ? Token{ Kind::File, {} }
            : Token{ Kind::FileNamed, normalizePreviewText(event.mediaFilename) };
    case TimelineEvent::Video:
        return event.mediaFilename.isEmpty()
            ? Token{ Kind::Video, {} }
            : text(normalizePreviewText(event.mediaFilename));
    case TimelineEvent::Audio:
        if (event.mediaIsVoice)
            return { Kind::Voice, {} };
        return event.mediaFilename.isEmpty()
            ? Token{ Kind::Audio, {} }
            : text(normalizePreviewText(event.mediaFilename));
    case TimelineEvent::Sticker:
        return { Kind::Sticker, {} };
    case TimelineEvent::Poll: {
        QString question = event.pollQuestion;
        if (question.isEmpty())
            question = event.body.section(QLatin1Char('\n'), 0, 0);
        const QString normalized = normalizePreviewText(question);
        return normalized.isEmpty() ? Token{ Kind::Poll, {} }
                                    : Token{ Kind::PollQuestion, normalized };
    }
    case TimelineEvent::StateChange:
        if (!event.stateSentence.isEmpty()) {
            const QString actor = presentable_name::presentable(
                event.sender,
                actorLabel.isEmpty() ? event.senderDisplayName : actorLabel,
                event.senderNameAmbiguous);
            const QString english = normalizePreviewText(actorSentence(
                event.body, event.sender,
                actorLabel.isEmpty() ? event.senderDisplayName : actorLabel,
                event.senderNameAmbiguous));
            return { Kind::StateSentence,
                     packStateSentence(event.stateSentence,
                                       normalizePreviewText(
                                           actor.isEmpty() ? event.sender : actor),
                                       normalizePreviewText(
                                           presentable_name::sanitized(event.stateTarget)),
                                       english) };
        }
        return text(normalizePreviewText(actorSentence(
            event.body, event.sender,
            actorLabel.isEmpty() ? event.senderDisplayName : actorLabel,
            event.senderNameAmbiguous)));
    default:
        return text(normalizePreviewText(event.body));
    }
}

}
