#include "models/MessageHtml.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringView>
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

// ---- Code-block segmentation helpers (v0.7.4) ----------------------------

// Decode the entities a formatted body can carry into their literal
// characters. Code-block text is PLAIN text (the UI renders it with
// Text.PlainText), so "&lt;script&gt;" must arrive as those six literal
// characters and can never become markup again.
//
// ONE left-to-right pass, deliberately: a second pass over the result would
// turn "&amp;lt;" — which the sender wrote to display the literal string
// "&lt;" — into "<". Decoding once is both correct and unable to resurrect
// markup from escaped text.
QString decodeEntities(const QString &in)
{
    QString out;
    out.reserve(in.size());
    qsizetype i = 0;
    while (i < in.size()) {
        const QChar c = in[i];
        if (c != QLatin1Char('&')) {
            out += c;
            ++i;
            continue;
        }
        const qsizetype semi = in.indexOf(QLatin1Char(';'), i + 1);
        // An unterminated or absurdly long "&…" is not an entity. Keep the
        // ampersand literally instead of swallowing the rest of the line.
        if (semi < 0 || semi - i > 12) {
            out += c;
            ++i;
            continue;
        }
        const QString name = in.mid(i + 1, semi - i - 1);
        QString rep;
        if (name == QLatin1String("lt"))
            rep = QStringLiteral("<");
        else if (name == QLatin1String("gt"))
            rep = QStringLiteral(">");
        else if (name == QLatin1String("amp"))
            rep = QStringLiteral("&");
        else if (name == QLatin1String("quot"))
            rep = QStringLiteral("\"");
        else if (name == QLatin1String("apos"))
            rep = QStringLiteral("'");
        else if (name == QLatin1String("nbsp"))
            rep = QStringLiteral(" "); // a code block wants a real space
        else if (name.startsWith(QLatin1Char('#')) && name.size() > 1) {
            bool ok = false;
            uint value = 0;
            if (name.size() > 2
                && (name[1] == QLatin1Char('x') || name[1] == QLatin1Char('X')))
                value = QStringView(name).mid(2).toUInt(&ok, 16);
            else
                value = QStringView(name).mid(1).toUInt(&ok, 10);
            // Reject surrogates and out-of-range code points rather than
            // planting a replacement character of our own invention.
            if (ok && value > 0 && value <= 0x10FFFFu
                && !(value >= 0xD800u && value <= 0xDFFFu)) {
                const char32_t ucs = static_cast<char32_t>(value);
                rep = QString::fromUcs4(&ucs, 1);
            }
        }
        if (rep.isEmpty()) {
            out += c;
            ++i;
            continue;
        }
        out += rep;
        i = semi + 1;
    }
    return out;
}

// The validated language token from a `class="language-rust"` / `lang-rust`.
// Fail closed: anything that is not a short plain identifier yields an empty
// language. The RAW class string is never returned — it is sender-chosen text
// that would end up in a label and an accessible name.
QString languageFromClass(const QString &rawInside)
{
    static const QRegularExpression attr(
        // The tag name is always first in `raw`, so a real class attribute is
        // preceded by whitespace. Anchoring on that stops `data-class=` (and
        // any other suffix attribute) being read as the class.
        QStringLiteral("(?:\\A|\\s)class\\s*=\\s*"
                       "(?:\"([^\"]*)\"|'([^']*)'|([^\\s\"'=<>`]+))"),
        QRegularExpression::CaseInsensitiveOption);
    const auto m = attr.match(rawInside);
    if (!m.hasMatch())
        return {};
    QString value = m.captured(1);
    if (value.isEmpty())
        value = m.captured(2);
    if (value.isEmpty())
        value = m.captured(3);

    static const QRegularExpression gap(QStringLiteral("\\s+"));
    static const QRegularExpression token(
        QStringLiteral("\\A[A-Za-z0-9+#._-]{1,24}\\z"));
    const QStringList parts = value.split(gap, Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        QString candidate;
        if (part.startsWith(QLatin1String("language-"), Qt::CaseInsensitive))
            candidate = part.mid(9);
        else if (part.startsWith(QLatin1String("lang-"), Qt::CaseInsensitive))
            candidate = part.mid(5);
        else
            continue;
        if (token.match(candidate).hasMatch())
            return candidate;
    }
    return {};
}

// Tags that carry no content of their own — a run made only of these is
// spacing, not a message. Dropping such a run is what stops an empty line
// rendering between two adjacent code blocks; an <hr>, a list or a heading
// is real content and deliberately does NOT appear here.
bool isLayoutOnlyTag(const QString &name)
{
    return name == QLatin1String("p") || name == QLatin1String("br")
        || name == QLatin1String("span") || name == QLatin1String("div");
}

// Does a sanitized RichText run carry anything a reader would see?
bool richTextCarriesContent(const QString &rich)
{
    const qsizetype n = rich.size();
    qsizetype i = 0;
    while (i < n) {
        if (rich[i] != QLatin1Char('<')) {
            qsizetype lt = rich.indexOf(QLatin1Char('<'), i);
            if (lt < 0)
                lt = n;
            QString run = rich.mid(i, lt - i);
            run.replace(QLatin1String("&nbsp;"), QLatin1String(" "));
            if (!run.trimmed().isEmpty())
                return true;
            i = lt;
            continue;
        }
        qsizetype end = 0;
        const ParsedTag t = parseTag(rich, i, end);
        if (!t.valid) {
            // A stray '<' the sanitizer escaped is text, so this can only be
            // malformed output; treat it as content rather than silently
            // dropping the run.
            return true;
        }
        if (!isLayoutOnlyTag(t.name))
            return true;
        i = end;
    }
    return false;
}

// Trim the markup shape off a decoded code block: normalize line endings so
// the renderer's line count is the program's, drop the one leading newline a
// `<pre><code>` almost always carries, and drop trailing newlines.
QString normalizeCodeText(QString text)
{
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    if (text.startsWith(QLatin1Char('\n')))
        text.remove(0, 1);
    while (text.endsWith(QLatin1Char('\n')))
        text.chop(1);
    return text;
}

// Is there a real code block in here? Cheap reject first — every formatted
// body in the timeline runs through this, and the ordinary message must not
// pay for the parser. A <pre> inside dropped content (a <script>, an
// <mx-reply> fallback) is NOT a code block: nothing in there is rendered.
bool containsCodeBlock(const QString &html)
{
    if (!html.contains(QLatin1String("<pre"), Qt::CaseInsensitive))
        return false;
    int dropDepth = 0;
    const qsizetype n = html.size();
    qsizetype i = 0;
    while (i < n) {
        if (html[i] != QLatin1Char('<')) {
            qsizetype lt = html.indexOf(QLatin1Char('<'), i);
            if (lt < 0)
                lt = n;
            i = lt;
            continue;
        }
        qsizetype end = 0;
        const ParsedTag t = parseTag(html, i, end);
        if (!t.valid) {
            i += 1;
            continue;
        }
        i = end;
        if (dropContentTags().contains(t.name)) {
            if (t.closing) {
                if (dropDepth > 0)
                    --dropDepth;
            } else {
                ++dropDepth;
            }
            continue;
        }
        if (dropDepth == 0 && !t.closing && t.name == QLatin1String("pre"))
            return true;
    }
    return false;
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
            if (t.closing) {
                out += QStringLiteral("</") + name + QStringLiteral(">");
            } else if (!mentionStyle.codeBackground.isEmpty()
                       && (name == QLatin1String("code")
                           || name == QLatin1String("pre"))) {
                // Give inline `code` and ```code blocks``` a subtle boxed
                // background so they read as code rather than blending into the
                // chat text. The colour is a validated theme QColor, escaped
                // again here so a style break-out is impossible; the plain
                // </tag> emitted above closes it.
                out += QStringLiteral("<") + name
                    + QStringLiteral(" style=\"background-color:")
                    + mentionStyle.codeBackground.toHtmlEscaped()
                    + QStringLiteral("\">");
            } else {
                out += QStringLiteral("<") + name + QStringLiteral(">");
            }
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

QList<MessageHtml::Segment> MessageHtml::segments(
    const QString &html,
    const std::function<QString(const QString &)> &resolveDisplayName,
    const QString &ownUserId,
    const MentionStyle &mentionStyle)
{
    // The ordinary message: exactly one RichText segment whose text IS
    // sanitize()'s output. It is the SAME call, on the untouched input —
    // reproducing the sanitizer's bound or its scan here would make the two
    // free to drift, and the drift would be invisible until a body rendered
    // differently depending on which entry point read it.
    if (!containsCodeBlock(html)) {
        return QList<Segment>{
            Segment{SegmentKind::RichText,
                    sanitize(html, resolveDisplayName, ownUserId, mentionStyle),
                    QString()}};
    }

    // Bounds. A Matrix event is capped at 65536 bytes by the spec, so none of
    // these is reachable from a well-formed server; they exist so a hostile or
    // broken body degrades into "fewer segments" rather than into unbounded
    // work. The input bound is deliberately ABOVE the code-text bound so the
    // code-text bound is the binding one and can actually be proven.
    static constexpr qsizetype kMaxSegmentInput = 1024 * 1024;
    static constexpr qsizetype kMaxSegments = 64;
    // Counted in QChar (UTF-16) units, which is what bounds the memory the
    // renderer will hold.
    static constexpr qsizetype kMaxCodeChars = 256 * 1024;

    const QString in = html.size() > kMaxSegmentInput
        ? html.left(kMaxSegmentInput) : html;

    QList<Segment> out;
    QString richSource;   // raw source of the current RichText run
    QString codeText;     // decoded text of the current code block
    QString codeLanguage;
    qsizetype codeCharsEmitted = 0;
    int dropDepth = 0;    // inside a dropped-content element
    int preDepth = 0;     // inside a code block
    bool exhausted = false; // a bound was reached: stop emitting entirely

    auto flushRich = [&]() {
        const QString source = richSource;
        richSource.clear();
        if (exhausted || source.isEmpty())
            return;
        // The run is handed to the sanitizer VERBATIM: it owns the allowlist,
        // the href policy and the mention rewriting, and a second copy of any
        // of that here would be a second sanitizer to keep in step.
        const QString rich =
            sanitize(source, resolveDisplayName, ownUserId, mentionStyle);
        if (!richTextCarriesContent(rich))
            return;
        if (out.size() >= kMaxSegments) {
            exhausted = true;
            return;
        }
        out.append(Segment{SegmentKind::RichText, rich, QString()});
    };

    auto flushCode = [&]() {
        const QString text = normalizeCodeText(codeText);
        const QString language = codeLanguage;
        codeText.clear();
        codeLanguage.clear();
        if (exhausted || text.trimmed().isEmpty())
            return;
        if (out.size() >= kMaxSegments
            || codeCharsEmitted + text.size() > kMaxCodeChars) {
            exhausted = true;
            return;
        }
        codeCharsEmitted += text.size();
        out.append(Segment{SegmentKind::CodeBlock, text, language});
    };

    const qsizetype n = in.size();
    qsizetype i = 0;
    while (i < n && !exhausted) {
        if (in[i] != QLatin1Char('<')) {
            qsizetype lt = in.indexOf(QLatin1Char('<'), i);
            if (lt < 0)
                lt = n;
            if (dropDepth == 0) {
                const QString run = in.mid(i, lt - i);
                if (preDepth > 0)
                    codeText += decodeEntities(run);
                else
                    richSource += run;
            }
            i = lt;
            continue;
        }

        const qsizetype ltPos = i;
        qsizetype end = 0;
        const ParsedTag t = parseTag(in, ltPos, end);
        if (!t.valid) {
            if (dropDepth == 0) {
                // Inside a code block a stray '<' is a literal character of
                // the program. Outside, the raw '<' goes to the sanitizer,
                // which escapes exactly this character and keeps scanning.
                if (preDepth > 0)
                    codeText += QLatin1Char('<');
                else
                    richSource += QLatin1Char('<');
            }
            i = ltPos + 1;
            continue;
        }
        i = end;

        const QString &name = t.name;

        if (dropContentTags().contains(name)) {
            // Drop-with-content still drops INSIDE a code block: a <script>
            // body is not source the sender asked us to display.
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

        if (name == QLatin1String("pre")) {
            if (t.closing) {
                if (preDepth > 0) {
                    --preDepth;
                    if (preDepth == 0)
                        flushCode();
                }
                // A stray </pre> outside a block is markup noise: dropped.
                continue;
            }
            if (preDepth == 0) {
                flushRich();
                codeText.clear();
                codeLanguage = languageFromClass(t.raw);
            }
            // A nested <pre> is NOT a second block. The outer one owns the
            // text; the inner tag is dropped like any other tag inside.
            ++preDepth;
            continue;
        }

        if (preDepth > 0) {
            // Inside a code block <br> is the only markup with meaning
            // (senders emit one per line); every other tag is dropped and its
            // text keeps flowing.
            if (name == QLatin1String("br") && !t.closing)
                codeText += QLatin1Char('\n');
            else if (name == QLatin1String("code") && !t.closing
                     && codeLanguage.isEmpty())
                codeLanguage = languageFromClass(t.raw);
            continue;
        }

        richSource += QLatin1Char('<') + t.raw + QLatin1Char('>');
    }

    // An unclosed <pre> still describes one block — the sender's markup ran
    // out, the code did not.
    if (preDepth > 0)
        flushCode();
    flushRich();
    return out;
}
