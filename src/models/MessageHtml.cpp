#include "models/MessageHtml.h"

#include <QRegularExpression>
#include <QSet>
#include <QUrl>

namespace {

// Inline/block formatting tags allowed through (attributes stripped). This is
// the intersection of the Matrix suggested subset and the Qt RichText subset,
// so everything here actually renders.
const QSet<QString> &allowedTags()
{
    static const QSet<QString> s = {
        QStringLiteral("b"),      QStringLiteral("strong"),
        QStringLiteral("i"),      QStringLiteral("em"),
        QStringLiteral("u"),      QStringLiteral("s"),
        QStringLiteral("del"),    QStringLiteral("strike"),
        QStringLiteral("code"),   QStringLiteral("pre"),
        QStringLiteral("blockquote"), QStringLiteral("br"),
        QStringLiteral("p"),      QStringLiteral("span"),
        QStringLiteral("ul"),     QStringLiteral("ol"),
        QStringLiteral("li"),     QStringLiteral("sub"),
        QStringLiteral("sup"),    QStringLiteral("h1"),
        QStringLiteral("h2"),     QStringLiteral("h3"),
        QStringLiteral("h4"),     QStringLiteral("h5"),
        QStringLiteral("h6"),     QStringLiteral("hr"),
    };
    return s;
}

// Tags dropped WITH their content — nothing between the open and close tag is
// rendered.
const QSet<QString> &dropContentTags()
{
    static const QSet<QString> s = {
        QStringLiteral("script"),   QStringLiteral("style"),
        QStringLiteral("mx-reply"), QStringLiteral("head"),
        QStringLiteral("iframe"),   QStringLiteral("object"),
        QStringLiteral("embed"),    QStringLiteral("svg"),
        QStringLiteral("math"),     QStringLiteral("noscript"),
        QStringLiteral("template"),
    };
    return s;
}

struct ParsedTag {
    bool valid = false;
    bool closing = false;
    QString name;
    QString raw; // inside of <...>, sans the surrounding brackets
};

// Parse a tag beginning at html[pos] == '<'. endOut receives the index just
// past the closing '>'. valid is false for a stray '<' that is not a tag.
ParsedTag parseTag(const QString &html, qsizetype pos, qsizetype &endOut)
{
    ParsedTag t;
    endOut = pos + 1;
    // A '<' only starts a tag when immediately followed by a letter or '/'
    // (HTML rule). "a < b" is literal text, not a broken tag.
    if (pos + 1 >= html.size())
        return t;
    const QChar after = html[pos + 1];
    if (!after.isLetter() && after != QLatin1Char('/'))
        return t;
    const qsizetype gt = html.indexOf(QLatin1Char('>'), pos + 1);
    if (gt < 0) {
        endOut = html.size();
        return t;
    }
    endOut = gt + 1;
    QString inside = html.mid(pos + 1, gt - pos - 1).trimmed();
    if (inside.isEmpty())
        return t;
    t.raw = inside;
    if (inside.startsWith(QLatin1Char('/'))) {
        t.closing = true;
        inside = inside.mid(1).trimmed();
    }
    qsizetype i = 0;
    while (i < inside.size()
           && (inside[i].isLetterOrNumber() || inside[i] == QLatin1Char('-')))
        ++i;
    if (i == 0)
        return t; // "< " or "</ " — not a real tag
    t.name = inside.left(i).toLower();
    t.valid = true;
    return t;
}

QString extractHref(const QString &rawInside)
{
    static const QRegularExpression re(
        QStringLiteral("href\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)')"),
        QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(rawInside);
    if (!m.hasMatch())
        return {};
    QString v = m.captured(1);
    if (v.isEmpty())
        v = m.captured(2);
    v = v.trimmed();
    v.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return v;
}

bool isSafeHttp(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    return url.isValid() && !url.host().isEmpty()
        && (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
        && url.userInfo().isEmpty();
}

// A matrix.to user permalink -> the "@user:server" id, else empty.
QString matrixToUserId(const QString &href)
{
    const QUrl u(href);
    if (u.host().compare(QLatin1String("matrix.to"), Qt::CaseInsensitive) != 0)
        return {};
    QString frag = u.fragment(QUrl::FullyDecoded); // "/@user:server"
    if (frag.startsWith(QLatin1Char('/')))
        frag = frag.mid(1);
    const int slash = frag.indexOf(QLatin1Char('/'));
    if (slash >= 0)
        frag = frag.left(slash);
    if (frag.startsWith(QLatin1Char('@')) && frag.contains(QLatin1Char(':')))
        return frag;
    return {};
}

QString localpart(const QString &userId)
{
    QString lp = userId.startsWith(QLatin1Char('@')) ? userId.mid(1) : userId;
    const int colon = lp.indexOf(QLatin1Char(':'));
    return colon > 0 ? lp.left(colon) : lp;
}

} // namespace

QString MessageHtml::sanitize(
    const QString &html,
    const std::function<QString(const QString &)> &resolveDisplayName,
    const QString &ownUserId,
    const MentionStyle &mentionStyle)
{
    // Defensive bound: never process an unreasonably large formatted body.
    static constexpr qsizetype kMaxInput = 64 * 1024;
    const QString in = html.size() > kMaxInput ? html.left(kMaxInput) : html;

    QString out;
    out.reserve(in.size());
    int dropDepth = 0;      // inside a dropped-content element
    int mentionSwallow = 0; // inside a mention whose text we already replaced
    QList<bool> anchorEmitted; // did each open <a> emit an <a> we must close?

    const qsizetype n = in.size();
    qsizetype i = 0;
    while (i < n) {
        if (in[i] != QLatin1Char('<')) {
            qsizetype lt = in.indexOf(QLatin1Char('<'), i);
            if (lt < 0)
                lt = n;
            if (dropDepth == 0 && mentionSwallow == 0)
                out += in.mid(i, lt - i);
            i = lt;
            continue;
        }

        const qsizetype ltPos = i;
        qsizetype end = 0;
        const ParsedTag t = parseTag(in, ltPos, end);
        if (!t.valid) {
            // Literal '<' (stray, or unterminated tag): escape only this
            // character and keep scanning the rest of the text.
            if (dropDepth == 0 && mentionSwallow == 0)
                out += QLatin1String("&lt;");
            i = ltPos + 1;
            continue;
        }
        i = end;

        const QString &name = t.name;

        if (dropContentTags().contains(name)) {
            if (t.closing) {
                if (dropDepth > 0)
                    --dropDepth;
            } else {
                ++dropDepth;
            }
            continue;
        }
        if (dropDepth > 0)
            continue;

        if (name == QLatin1String("a")) {
            if (t.closing) {
                if (mentionSwallow > 0) {
                    --mentionSwallow;
                } else if (!anchorEmitted.isEmpty()) {
                    if (anchorEmitted.takeLast())
                        out += QLatin1String("</a>");
                }
                continue;
            }
            if (mentionSwallow > 0) {
                ++mentionSwallow; // nested anchor inside a replaced mention
                continue;
            }
            const QString href = extractHref(t.raw);
            const QString mentionUser = matrixToUserId(href);
            if (!mentionUser.isEmpty()) {
                QString disp =
                    resolveDisplayName ? resolveDisplayName(mentionUser) : QString();
                if (disp == mentionUser)
                    disp.clear();
                if (disp.startsWith(QLatin1Char('@')))
                    disp = disp.mid(1);
                if (disp.isEmpty())
                    disp = localpart(mentionUser);
                const bool self =
                    !ownUserId.isEmpty() && mentionUser == ownUserId;
                const bool chip = !mentionStyle.accentColor.isEmpty()
                    && !mentionStyle.softColor.isEmpty();
                out += QStringLiteral("<a href=\"mention:")
                    + mentionUser.toHtmlEscaped() + QStringLiteral("\"");
                if (chip) {
                    // Inline chip: accent ink on a soft surface, no anchor
                    // underline. The colors are model-validated QColor names,
                    // escaped again here so a style break-out is impossible.
                    out += QStringLiteral(" style=\"color:")
                        + mentionStyle.accentColor.toHtmlEscaped()
                        + QStringLiteral(";background-color:")
                        + mentionStyle.softColor.toHtmlEscaped()
                        + QStringLiteral(";text-decoration:none\"");
                }
                out += QStringLiteral(">");
                if (self)
                    out += QLatin1String("<b>");
                if (chip)
                    out += QStringLiteral("&nbsp;");
                out += (QStringLiteral("@") + disp).toHtmlEscaped();
                if (chip)
                    out += QStringLiteral("&nbsp;");
                if (self)
                    out += QLatin1String("</b>");
                out += QLatin1String("</a>");
                ++mentionSwallow; // drop the sender's original inner text + </a>
                continue;
            }
            const QUrl u(href);
            if (isSafeHttp(u)) {
                out += QStringLiteral("<a href=\"") + href.toHtmlEscaped()
                    + QStringLiteral("\">");
                anchorEmitted.append(true);
            } else {
                anchorEmitted.append(false); // drop the link, keep its text
            }
            continue;
        }

        if (mentionSwallow > 0)
            continue; // drop any other markup inside a replaced mention

        if (allowedTags().contains(name)) {
            if (t.closing)
                out += QStringLiteral("</") + name + QStringLiteral(">");
            else
                out += QStringLiteral("<") + name + QStringLiteral(">");
        }
        // Unknown tag: dropped; its text content still flows through.
    }

    // Close any anchors left open by malformed input.
    while (!anchorEmitted.isEmpty()) {
        if (anchorEmitted.takeLast())
            out += QLatin1String("</a>");
    }
    return out;
}
