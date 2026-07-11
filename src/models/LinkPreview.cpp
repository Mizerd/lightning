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
        QStringLiteral("\\bhttps?://[^\\s<>]+"),
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
        return candidate;
    }
    return {};
}

QString sanitizedHost(const QString &url)
{
    return QUrl(url).host();
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
