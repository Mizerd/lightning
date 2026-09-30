#include "models/LinkPreview.h"

#include <QRegularExpression>
#include <QUrl>

namespace matrix::link_preview {

namespace {

// Strip fenced and inline code spans so their URLs are never previewed.
// Unterminated fences discard the rest of the message (the safe choice).
QString withoutCodeSpans(const QString &body)
{
    QString out = body;
    static const QRegularExpression fenced(
        QStringLiteral("```.*?(```|$)"),
        QRegularExpression::DotMatchesEverythingOption);
    out.replace(fenced, QStringLiteral(" "));
    static const QRegularExpression inlineCode(QStringLiteral("`[^`\\n]*`"));
    out.replace(inlineCode, QStringLiteral(" "));
    return out;
}

// Trim trailing characters that are almost always sentence punctuation
// rather than part of the URL. Closing parentheses/brackets are removed
// only while unbalanced against the URL's own opening ones ("(see
// https://en.wikipedia.org/wiki/Foo_(bar))" keeps the inner pair).
QString trimTrailingPunctuation(QString url)
{
    const QString plain = QStringLiteral(".,;:!?'\"");
    for (;;) {
        if (url.isEmpty())
            break;
        const QChar last = url.back();
        if (plain.contains(last)) {
            url.chop(1);
            continue;
        }
        if (last == QLatin1Char(')') || last == QLatin1Char(']')
            || last == QLatin1Char('}')) {
            const QChar open = last == QLatin1Char(')')   ? QLatin1Char('(')
                : last == QLatin1Char(']')                ? QLatin1Char('[')
                                                          : QLatin1Char('{');
            if (url.count(open) < url.count(last)) {
                url.chop(1);
                continue;
            }
        }
        break;
    }
    return url;
}

} // namespace

QString firstPreviewableUrl(const QString &body)
{
    if (body.isEmpty())
        return {};
    const QString text = withoutCodeSpans(body);

    // Positive scheme allow-list. javascript:, data:, file:, blob: and
    // friends can never match — they are not in the pattern at all.
    static const QRegularExpression urlRe(
        QStringLiteral("\\bhttps://[^\\s<>]+"),
        QRegularExpression::CaseInsensitiveOption);

    auto it = urlRe.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        // "blob:https://…" (and any other wrapped-scheme form) must not
        // expose its inner URL: reject matches directly preceded by ':'.
        const int start = match.capturedStart(0);
        if (start > 0 && text.at(start - 1) == QLatin1Char(':'))
            continue;
        const QString candidate = trimTrailingPunctuation(match.captured(0));
        const QUrl url(candidate, QUrl::StrictMode);
        if (!url.isValid() || url.host().isEmpty())
            continue;
        // Embedded credentials must never reach a request or a log.
        if (!url.userInfo().isEmpty())
            continue;
        // Matrix permalinks (users, rooms, events) are navigation targets,
        // never preview candidates; suppressed before any network contact or
        // consent card. Continue so a real external URL in the same message
        // still previews.
        if (url.host().compare(QLatin1String("matrix.to"),
                               Qt::CaseInsensitive) == 0)
            continue;
        return candidate;
    }
    return {};
}

QString sanitizedHost(const QString &url)
{
    return QUrl(url).host();
}

bool isSafeExternalUrl(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    return url.isValid() && !url.host().isEmpty()
        && (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
        && url.userInfo().isEmpty();
}

// One linkifier for message bodies and topics, so both keep the same escaping
// and the same isSafeExternalUrl gate. `bareWww` adds "www." hosts (topics).
static QString linkifiedHtml(const QString &body, bool bareWww)
{
    static const QRegularExpression webUrl(
        QStringLiteral("\\bhttps?://[^\\s<>]+"),
        QRegularExpression::CaseInsensitiveOption);
    // "www." only at the start of a word: not inside a host ("foo.www.x"), a
    // path ("/www.x") or an address ("a@www.x").
    static const QRegularExpression webOrWww(
        QStringLiteral("\\bhttps?://[^\\s<>]+|(?<![\\w.@/:-])www\\.[^\\s<>]+"),
        QRegularExpression::CaseInsensitiveOption);
    QString out;
    qsizetype cursor = 0;
    auto matches = (bareWww ? webOrWww : webUrl).globalMatch(body);
    while (matches.hasNext()) {
        const auto match = matches.next();
        out += body.mid(cursor, match.capturedStart() - cursor).toHtmlEscaped();
        const QString raw = match.captured();
        const QString candidate = trimTrailingPunctuation(raw);
        const bool bare = !candidate.startsWith(QLatin1String("http"),
                                                Qt::CaseInsensitive);
        const QString target =
            bare ? QStringLiteral("https://") + candidate : candidate;
        const QUrl url(target, QUrl::StrictMode);
        // A bare host needs a dot after "www." ("www.x" alone is a word).
        const bool linkable = isSafeExternalUrl(url)
            && (!bare || url.host().count(QLatin1Char('.')) >= 2);
        if (linkable) {
            out += QStringLiteral("<a href=\"") + target.toHtmlEscaped()
                + QStringLiteral("\">") + candidate.toHtmlEscaped()
                + QStringLiteral("</a>");
            out += raw.mid(candidate.size()).toHtmlEscaped();
        } else {
            out += raw.toHtmlEscaped();
        }
        cursor = match.capturedEnd();
    }
    out += body.mid(cursor).toHtmlEscaped();
    return out.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
}

QString linkifiedMessageHtml(const QString &body)
{
    return linkifiedHtml(body, false);
}

QString linkifiedTopicHtml(const QString &topic)
{
    return linkifiedHtml(topic, true);
}

GifClass classifyGif(const QString &validatedMime, qint64 sizeBytes,
                     int width, int height, const GifLimits &limits)
{
    // MIME parameters ("image/gif; charset=…") are irrelevant to the type.
    const QString mime =
        validatedMime.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
    if (mime != QLatin1String("image/gif"))
        return GifClass::NotGif;
    if (sizeBytes > limits.maxBytes)
        return GifClass::Oversized;
    if (width > limits.maxWidth || height > limits.maxHeight)
        return GifClass::Oversized;
    return GifClass::Gif;
}

} // namespace matrix::link_preview
