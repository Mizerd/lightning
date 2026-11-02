#include "models/LinkPreview.h"

#include <QRegularExpression>
#include <QUrl>

namespace matrix::link_preview {

namespace {

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

}

QString firstPreviewableUrl(const QString &body)
{
    if (body.isEmpty())
        return {};
    const QString text = withoutCodeSpans(body);

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
        if (!url.userInfo().isEmpty())
            continue;
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

static QString linkifiedHtml(const QString &body, bool bareWww)
{
    static const QRegularExpression webUrl(
        QStringLiteral("\\bhttps?://[^\\s<>]+"),
        QRegularExpression::CaseInsensitiveOption);
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

}
