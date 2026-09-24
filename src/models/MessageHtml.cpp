#include "models/MessageHtml.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringView>
#include <QTextBoundaryFinder>
#include <QUrl>

#include <algorithm>

namespace {

// Formatting tags allowed through (attributes stripped): the intersection of
// the Matrix suggested subset and Qt's RichText subset.
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

// Tags dropped together with their content.
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

// Tag parser for one left-to-right pass. It owns the cursor so the '>'
// search always resumes where the last one stopped: the pass stays linear
// even for inputs like "<a" repeated (remote input parsed on the GUI thread),
// and no caller can reintroduce the quadratic scan.
class TagScanner
{
public:
    explicit TagScanner(const QString &s) : m_s(s) {}
    TagScanner(QString &&) = delete; // the scanner does not own the string

    // Parse a tag at m_s[pos] == '<'. endOut receives the index just past '>'.
    // valid is false for a stray '<'. `pos` only moves forward; if it ever
    // moves back, the memo is rebuilt.
    ParsedTag tagAt(qsizetype pos, qsizetype &endOut)
    {
        ParsedTag t;
        endOut = pos + 1;
        const qsizetype n = m_s.size();
        // A '<' only starts a tag when followed by a letter or '/' (HTML rule);
        // "a < b" is literal text.
        if (pos + 1 >= n)
            return t;
        const QChar after = m_s[pos + 1];
        if (!after.isLetter() && after != QLatin1Char('/'))
            return t;
        const qsizetype gt = gtAtOrAfter(pos + 1);
        if (gt < 0) {
            endOut = n;
            return t;
        }
        endOut = gt + 1;
        // Views, not copies: an interior is materialized only for a tag that
        // parses, so stray '<' characters cost no copying of the remainder.
        const QStringView raw =
            QStringView(m_s).mid(pos + 1, gt - pos - 1).trimmed();
        if (raw.isEmpty())
            return t;
        QStringView inside = raw;
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
        t.name = inside.left(i).toString().toLower();
        t.raw = raw.toString();
        t.valid = true;
        return t;
    }

private:
    // Index of the first '>' at or after `from`, or -1. Memoized: m_gt is the
    // first '>' at or after m_from, so any later `from` <= m_gt reuses it and
    // rescans start past the previous answer. Total scanning is linear.
    qsizetype gtAtOrAfter(qsizetype from)
    {
        if (from >= m_from) {
            if (m_gt >= from)
                return m_gt;
            if (m_exhausted)
                return -1;
        }
        m_from = from;
        m_gt = m_s.indexOf(QLatin1Char('>'), from);
        m_exhausted = (m_gt < 0);
        return m_gt;
    }

    const QString &m_s;
    qsizetype m_from = 0;
    qsizetype m_gt = -1;
    bool m_exhausted = false;
};

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

/// One attribute value from a raw tag interior, quoted either way (like
/// extractHref).
QString extractAttr(const QString &rawInside, const QString &name)
{
    const QRegularExpression re(
        QStringLiteral("(?:^|\\s)") + QRegularExpression::escape(name)
            + QStringLiteral("\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)')"),
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

/// True when the tag carries `data-mx-emoticon` as an attribute name. MSC2545
/// says the value is ignored, so presence alone is the test.
bool hasEmoticonMarker(const QString &rawInside)
{
    static const QRegularExpression re(
        QStringLiteral("(?:^|\\s)data-mx-emoticon(?:\\s|=|/|$)"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(rawInside).hasMatch();
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

// ---- Code-block segmentation helpers --------------------------------------

// Decode entities into literal characters. Code-block text is rendered as
// plain text, so "&lt;script&gt;" stays literal. One pass only: a second would
// turn "&amp;lt;" into "<".
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
        // An unterminated or overlong "&…" is not an entity; keep the
        // ampersand.
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
            // Reject surrogates and out-of-range code points.
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

// The validated language token from `class="language-rust"` / `lang-rust`.
// Fails closed: anything other than a short identifier yields empty. The raw
// class string is sender text and is never returned.
QString languageFromClass(const QString &rawInside)
{
    static const QRegularExpression attr(
        // The tag name comes first, so a real class attribute follows
        // whitespace; this keeps `data-class=` from matching.
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

// Tags with no content of their own; a run of only these is spacing, and
// dropping it avoids an empty line between adjacent code blocks. <hr>, lists
// and headings are content and are not here.
bool isLayoutOnlyTag(const QString &name)
{
    return name == QLatin1String("p") || name == QLatin1String("br")
        || name == QLatin1String("span") || name == QLatin1String("div");
}

// Does a sanitized RichText run carry anything a reader would see?
bool richTextCarriesContent(const QString &rich)
{
    const qsizetype n = rich.size();
    TagScanner scanner(rich);
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
        const ParsedTag t = scanner.tagAt(i, end);
        if (!t.valid) {
            // A stray '<' would have been escaped, so this is malformed output;
            // treat it as content rather than dropping the run.
            return true;
        }
        if (!isLayoutOnlyTag(t.name))
            return true;
        i = end;
    }
    return false;
}

// Normalize line endings, drop the leading newline `<pre><code>` usually
// carries, and drop trailing newlines.
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

// Cheap reject first: every formatted body runs through this. A <pre> inside
// dropped content (<script>, <mx-reply>) is not a code block.
bool containsCodeBlock(const QString &html)
{
    if (!html.contains(QLatin1String("<pre"), Qt::CaseInsensitive))
        return false;
    int dropDepth = 0;
    const qsizetype n = html.size();
    TagScanner scanner(html);
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
        const ParsedTag t = scanner.tagAt(i, end);
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


// ---- Inline emoji sizing (markEmoji) ------------------------------------

struct CodepointRange {
    char32_t lo;
    char32_t hi;
};

// Emoji_Presentation=Yes: code points drawn as a picture without a variation
// selector. The narrow set that may be enlarged; digits, "#" and "(c)" are
// Emoji=Yes too and must not be.
constexpr CodepointRange kEmojiPresentation[] = {
    {0x231Au, 0x231Bu},   {0x23E9u, 0x23ECu},   {0x23F0u, 0x23F0u},
    {0x23F3u, 0x23F3u},   {0x25FDu, 0x25FEu},   {0x2614u, 0x2615u},
    {0x2648u, 0x2653u},   {0x267Fu, 0x267Fu},   {0x2693u, 0x2693u},
    {0x26A1u, 0x26A1u},   {0x26AAu, 0x26ABu},   {0x26BDu, 0x26BEu},
    {0x26C4u, 0x26C5u},   {0x26CEu, 0x26CEu},   {0x26D4u, 0x26D4u},
    {0x26EAu, 0x26EAu},   {0x26F2u, 0x26F3u},   {0x26F5u, 0x26F5u},
    {0x26FAu, 0x26FAu},   {0x26FDu, 0x26FDu},   {0x2705u, 0x2705u},
    {0x270Au, 0x270Bu},   {0x2728u, 0x2728u},   {0x274Cu, 0x274Cu},
    {0x274Eu, 0x274Eu},   {0x2753u, 0x2755u},   {0x2757u, 0x2757u},
    {0x2795u, 0x2797u},   {0x27B0u, 0x27B0u},   {0x27BFu, 0x27BFu},
    {0x2B1Bu, 0x2B1Cu},   {0x2B50u, 0x2B50u},   {0x2B55u, 0x2B55u},
    {0x1F004u, 0x1F004u}, {0x1F0CFu, 0x1F0CFu}, {0x1F18Eu, 0x1F18Eu},
    {0x1F191u, 0x1F19Au}, {0x1F1E6u, 0x1F1FFu}, {0x1F201u, 0x1F201u},
    {0x1F21Au, 0x1F21Au}, {0x1F22Fu, 0x1F22Fu}, {0x1F232u, 0x1F236u},
    {0x1F238u, 0x1F23Au}, {0x1F250u, 0x1F251u}, {0x1F300u, 0x1F320u},
    {0x1F32Du, 0x1F335u}, {0x1F337u, 0x1F37Cu}, {0x1F37Eu, 0x1F393u},
    {0x1F3A0u, 0x1F3CAu}, {0x1F3CFu, 0x1F3D3u}, {0x1F3E0u, 0x1F3F0u},
    {0x1F3F4u, 0x1F3F4u}, {0x1F3F8u, 0x1F43Eu}, {0x1F440u, 0x1F440u},
    {0x1F442u, 0x1F4FCu}, {0x1F4FFu, 0x1F53Du}, {0x1F54Bu, 0x1F54Eu},
    {0x1F550u, 0x1F567u}, {0x1F57Au, 0x1F57Au}, {0x1F595u, 0x1F596u},
    {0x1F5A4u, 0x1F5A4u}, {0x1F5FBu, 0x1F64Fu}, {0x1F680u, 0x1F6C5u},
    {0x1F6CCu, 0x1F6CCu}, {0x1F6D0u, 0x1F6D2u}, {0x1F6D5u, 0x1F6D7u},
    {0x1F6DCu, 0x1F6DFu}, {0x1F6EBu, 0x1F6ECu}, {0x1F6F4u, 0x1F6FCu},
    {0x1F7E0u, 0x1F7EBu}, {0x1F7F0u, 0x1F7F0u}, {0x1F90Cu, 0x1F93Au},
    {0x1F93Cu, 0x1F945u}, {0x1F947u, 0x1F9FFu}, {0x1FA70u, 0x1FA7Cu},
    {0x1FA80u, 0x1FA89u}, {0x1FA8Fu, 0x1FAC6u}, {0x1FACEu, 0x1FADCu},
    {0x1FADFu, 0x1FAE9u}, {0x1FAF0u, 0x1FAF8u},
};

constexpr char32_t kZwj        = 0x200Du;
constexpr char32_t kVs16       = 0xFE0Fu;
constexpr char32_t kKeycap     = 0x20E3u;

bool inRanges(const CodepointRange *ranges, size_t count, char32_t cp)
{
    const auto *end = ranges + count;
    const auto *hit = std::lower_bound(
        ranges, end, cp,
        [](const CodepointRange &r, char32_t v) { return r.hi < v; });
    return hit != end && cp >= hit->lo;
}

bool isEmojiPresentation(char32_t cp)
{
    return inRanges(kEmojiPresentation, std::size(kEmojiPresentation), cp);
}

bool isSkinToneModifier(char32_t cp) { return cp >= 0x1F3FBu && cp <= 0x1F3FFu; }
bool isRegionalIndicator(char32_t cp) { return cp >= 0x1F1E6u && cp <= 0x1F1FFu; }
bool isTagCharacter(char32_t cp) { return cp >= 0xE0020u && cp <= 0xE007Fu; }

// The broad test, used only to decide whether a body is emoji-only (see
// markEmoji). It must be a superset of the narrow test and of EmojiCatalog's
// lookup, or a big-emoji body could be enlarged twice; it covers Emoji=Yes
// characters without default emoji presentation (a bare U+2764). CJK and kana
// are letters and deliberately outside it.
bool couldCarryEmoji(char32_t cp)
{
    return cp == 0x00A9u || cp == 0x00AEu
        || (cp >= 0x2000u && cp <= 0x2BFFu)
        || cp == 0x3030u || cp == 0x303Du
        || cp == 0x3297u || cp == 0x3299u
        || (cp >= 0xFE00u && cp <= 0xFE0Fu)
        || (cp >= 0x1F000u && cp <= 0x1FAFFu)
        || isTagCharacter(cp);
}

// Does this grapheme cluster render as a picture? Narrow by design: it
// decides what is enlarged.
bool clusterIsEmoji(const QList<char32_t> &cps)
{
    if (cps.isEmpty())
        return false;
    bool sawVs16 = false;
    for (char32_t cp : cps) {
        if (cp == kKeycap || isRegionalIndicator(cp) || isSkinToneModifier(cp)
            || isEmojiPresentation(cp))
            return true;
        if (cp == kVs16)
            sawVs16 = true;
    }
    // U+FE0F requests emoji presentation for a non-default base (U+2764
    // U+FE0F). The base must be a symbol so a selector after a letter cannot
    // resize a word.
    return sawVs16 && couldCarryEmoji(cps.first());
}

bool clusterCouldBeEmoji(const QList<char32_t> &cps)
{
    for (char32_t cp : cps) {
        if (couldCarryEmoji(cp) || cp == kZwj)
            return true;
    }
    return false;
}

bool clusterIsWhitespace(const QList<char32_t> &cps)
{
    for (char32_t cp : cps) {
        if (cp > 0x10FFFFu)
            return false;
        if (!QChar::isSpace(static_cast<char32_t>(cp)))
            return false;
    }
    return !cps.isEmpty();
}

// One O(n) gate; an ASCII body leaves without allocating anything.
bool mayHoldEmoji(const QString &html)
{
    for (const QChar c : html) {
        const char16_t u = c.unicode();
        if (u >= 0xD800u && u <= 0xDBFFu) // a high surrogate: any SMP char
            return true;
        if (u == 0x00A9u || u == 0x00AEu)
            return true;
        if (u >= 0x2000u && u <= 0x3299u)
            return true;
        if (u >= 0xFE00u && u <= 0xFE0Fu)
            return true;
    }
    return false;
}

// Is html[i] the start of a character entity? Returns its end (past ';') or
// -1. Entities are atomic: splitting "&amp;" corrupts markup and can even
// manufacture a tag.
qsizetype entityEnd(const QString &html, qsizetype i)
{
    // Bound the search rather than scanning the whole rest and rejecting after:
    // a body of repeated "&" would otherwise be quadratic. No entity is longer
    // than ten characters.
    const qsizetype limit = qMin(i + 11, html.size());
    const qsizetype semi = QStringView(html).mid(i + 1, limit - (i + 1))
                               .indexOf(QLatin1Char(';'));
    if (semi < 0)
        return -1;
    return i + 1 + semi + 1;
}

} // namespace

QString MessageHtml::sanitize(
    const QString &html,
    const std::function<QString(const QString &)> &resolveDisplayName,
    const QString &ownUserId,
    const MentionStyle &mentionStyle,
    bool revealSpoilers)
{
    // Defensive bound: never process an unreasonably large formatted body.
    static constexpr qsizetype kMaxInput = 64 * 1024;
    const QString in = html.size() > kMaxInput ? html.left(kMaxInput) : html;

    QString out;
    out.reserve(in.size());
    int dropDepth = 0;      // inside a dropped-content element
    int mentionSwallow = 0; // inside a mention whose text we already replaced
    QList<bool> anchorEmitted; // did each open <a> emit an <a> we must close?
    // Per open <span>: was it emitted as a spoiler anchor? Keeps a plain
    // </span> from closing a spoiler anchor and vice versa.
    QList<bool> spanIsSpoiler;

    const qsizetype n = in.size();
    TagScanner scanner(in);
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
        const ParsedTag t = scanner.tagAt(ltPos, end);
        if (!t.valid) {
            // Literal '<' (stray or unterminated tag): escape just this
            // character and keep scanning.
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

        // ── Inline custom emoji (MSC2545) ────────────────────────────────
        //
        // The only <img> this sanitizer emits; not a general image permission.
        // A formatted body can never make the client fetch a remote/tracking
        // image: an image is rendered only when marked as an emoticon and
        // addressed by `mxc:`, which goes through the authenticated media path.
        //
        // The mxc form is emitted, not a resolved local source: sanitize()
        // output is also used by MessageComposer::beginEdit, and an `image://`
        // URL must not end up in an outgoing formatted_body. Resolution happens
        // at render time (resolveInlineImages).
        if (name == QLatin1String("img")) {
            if (t.closing)
                continue;
            if (!hasEmoticonMarker(t.raw))
                continue;   // an image that does not claim to be an emoticon
            const QString src = extractAttr(t.raw, QStringLiteral("src"));
            if (!src.startsWith(QLatin1String("mxc://"))
                || src.length() <= int(sizeof("mxc://") - 1)) {
                continue;   // unaddressable, or not ours to fetch
            }
            // An mxc URI has no query, fragment or credentials; a value with
            // any of those is not one.
            if (src.contains(QLatin1Char('"')) || src.contains(QLatin1Char('<'))
                || src.contains(QLatin1Char('>')) || src.contains(QLatin1Char(' '))) {
                continue;
            }
            // The shortcode is remote text shown on hover and read by assistive
            // technology: escaped and bounded.
            QString alt = extractAttr(t.raw, QStringLiteral("alt"));
            if (alt.isEmpty())
                alt = extractAttr(t.raw, QStringLiteral("title"));
            alt.truncate(64);
            out += QStringLiteral("<img data-mx-emoticon src=\"")
                + src.toHtmlEscaped() + QStringLiteral("\"");
            if (!alt.isEmpty()) {
                out += QStringLiteral(" alt=\"") + alt.toHtmlEscaped()
                    + QStringLiteral("\" title=\"") + alt.toHtmlEscaped()
                    + QStringLiteral("\"");
            }
            // The sender's `height` is not honoured: a fixed inline size keeps
            // a remote emoticon from taking over the reader's message list.
            out += QStringLiteral(" height=\"20\" width=\"20\">");
            continue;
        }

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
            // Inside a covered spoiler nothing is a link: the whole slab is the
            // reveal toggle. Links return once revealed.
            const bool coveredSpoiler = !revealSpoilers && spanIsSpoiler.contains(true);
            const QString mentionUser = matrixToUserId(href);
            if (!mentionUser.isEmpty() && coveredSpoiler) {
                QString disp =
                    resolveDisplayName ? resolveDisplayName(mentionUser) : QString();
                if (disp == mentionUser)
                    disp.clear();
                if (disp.startsWith(QLatin1Char('@')))
                    disp = disp.mid(1);
                if (disp.isEmpty())
                    disp = localpart(mentionUser);
                out += (QStringLiteral("@") + disp).toHtmlEscaped();
                ++mentionSwallow; // drop the sender's original inner text + </a>
                continue;
            }
            if (coveredSpoiler) {
                anchorEmitted.append(false); // inner text flows, no href
                continue;
            }
            if (!mentionUser.isEmpty()) {
                QString disp =
                    resolveDisplayName ? resolveDisplayName(mentionUser) : QString();
                if (disp == mentionUser)
                    disp.clear();
                if (disp.startsWith(QLatin1Char('@')))
                    disp = disp.mid(1);
                // The resolver answers the member name, else a global profile
                // name, else the localpart. The sender's anchor text is
                // deliberately never used: a pill reading "@admin" that links
                // to @attacker:evil is the spoof this prevents (Element ignores
                // it too).
                if (disp.isEmpty())
                    disp = localpart(mentionUser);
                const bool self =
                    !ownUserId.isEmpty() && mentionUser == ownUserId;
                // The accent is reserved for a mention of you; others take the
                // link ink (see MentionStyle). Each ink falls back to the
                // other, so a theme that sets only one still styles both.
                const QString &preferred = self ? mentionStyle.accentColor
                                                : mentionStyle.linkColor;
                const QString &alternate = self ? mentionStyle.linkColor
                                                : mentionStyle.accentColor;
                const QString ink =
                    preferred.isEmpty() ? alternate : preferred;
                out += QStringLiteral("<a href=\"mention:")
                    + mentionUser.toHtmlEscaped() + QStringLiteral("\"");
                if (!ink.isEmpty()) {
                    // Ink and weight only, no surface: Qt paints an inline
                    // background as a full-line-height slab, and
                    // `text-decoration:none` is needed to drop the default
                    // underline. Colors are model-validated hex, escaped again
                    // so a style break-out is impossible.
                    out += QStringLiteral(" style=\"color:")
                        + ink.toHtmlEscaped()
                        + QStringLiteral(";font-weight:600")
                        + QStringLiteral(";text-decoration:none\"");
                }
                out += QStringLiteral(">");
                // <b> stays the self-mention marker because it survives an
                // unstyled body, so both paths agree and MentionTokenizer's
                // recovery matches one shape.
                if (self)
                    out += QLatin1String("<b>");
                out += (QStringLiteral("@") + disp).toHtmlEscaped();
                if (self)
                    out += QLatin1String("</b>");
                out += QLatin1String("</a>");
                ++mentionSwallow; // drop the sender's original inner text + </a>
                continue;
            }
            const QUrl u(href);
            if (isSafeHttp(u)) {
                // Give links the theme's link ink instead of Qt's default
                // #0000ff, which is unreadable on dark themes. The underline
                // separates a URL from a mention.
                const QString linkInk = mentionStyle.linkColor.isEmpty()
                    ? mentionStyle.accentColor
                    : mentionStyle.linkColor;
                out += QStringLiteral("<a href=\"") + href.toHtmlEscaped()
                    + QStringLiteral("\"");
                if (!linkInk.isEmpty())
                    out += QStringLiteral(" style=\"color:")
                        + linkInk.toHtmlEscaped() + QStringLiteral("\"");
                out += QStringLiteral(">");
                anchorEmitted.append(true);
            } else {
                anchorEmitted.append(false); // drop the link, keep its text
            }
            continue;
        }

        if (mentionSwallow > 0)
            continue; // drop any other markup inside a replaced mention

        if (name == QLatin1String("span")) {
            // Spoilers: a data-mx-spoiler span becomes a click-to-reveal run.
            // Covered is a solid codeBackground slab (background and text in
            // one ink); revealed keeps only the background. Both wrap the run
            // in the internal spoiler:toggle anchor, routed to the model, never
            // a browser. The optional reason is ignored; other spans stay
            // attribute-stripped.
            static const QRegularExpression spoilerAttr(
                QStringLiteral("(?:\\A|\\s)data-mx-spoiler(?:\\s*=|\\s|\\z)"),
                QRegularExpression::CaseInsensitiveOption);
            if (t.closing) {
                if (!spanIsSpoiler.isEmpty() && spanIsSpoiler.takeLast())
                    out += QLatin1String("</span></a>");
                else
                    out += QLatin1String("</span>");
                continue;
            }
            const bool spoiler = spoilerAttr.match(t.raw).hasMatch();
            const QString &cover = mentionStyle.codeBackground;
            if (spoiler) {
                out += QStringLiteral(
                    "<a href=\"spoiler:toggle\" "
                    "style=\"text-decoration:none\">");
                if (!revealSpoilers && !cover.isEmpty()) {
                    out += QStringLiteral("<span style=\"background-color:")
                        + cover.toHtmlEscaped() + QStringLiteral(";color:")
                        + cover.toHtmlEscaped() + QStringLiteral("\">");
                } else if (!cover.isEmpty()) {
                    out += QStringLiteral("<span style=\"background-color:")
                        + cover.toHtmlEscaped() + QStringLiteral("\">");
                } else {
                    out += QStringLiteral("<span>");
                }
                spanIsSpoiler.append(true);
            } else {
                out += QStringLiteral("<span>");
                spanIsSpoiler.append(false);
            }
            continue;
        }

        if (allowedTags().contains(name)) {
            if (t.closing) {
                out += QStringLiteral("</") + name + QStringLiteral(">");
            } else if (name == QLatin1String("ol")) {
                // `start` survives on <ol>. Markdown splits a numbered list
                // interrupted by nested bullets into several <ol start="N">
                // lists; without it every one restarts at 1. Qt 6 honours it
                // (QTextListFormat::start()), although its documented subset
                // does not say so. Digits only and bounded, re-emitted from a
                // parsed integer so no sender text survives.
                const QString rawStart =
                    extractAttr(t.raw, QStringLiteral("start"));
                bool ok = false;
                const int startAt = rawStart.toInt(&ok);
                if (ok && startAt >= 1 && startAt <= 1000000) {
                    out += QStringLiteral("<ol start=\"")
                        + QString::number(startAt) + QStringLiteral("\">");
                } else {
                    out += QStringLiteral("<ol>");
                }
            } else if (!mentionStyle.codeBackground.isEmpty()
                       && (name == QLatin1String("code")
                           || name == QLatin1String("pre"))) {
                // Inline code and code blocks get a subtle boxed background.
                // The colour is a validated theme QColor, escaped again; the
                // plain </tag> above closes it.
                out += QStringLiteral("<") + name
                    + QStringLiteral(" style=\"background-color:")
                    + mentionStyle.codeBackground.toHtmlEscaped()
                    + QStringLiteral("\">");
            } else if (!mentionStyle.linkColor.isEmpty()
                       && name == QLatin1String("blockquote")) {
                // A quote bar: Qt renders a bare <blockquote> as an indent
                // only. Qt 6 honours block border properties, so draw a left
                // rule in the theme's link ink. The colour is a validated theme
                // QColor, escaped again.
                out += QStringLiteral("<blockquote style=\"border-left:3px "
                                      "solid ")
                    + mentionStyle.linkColor.toHtmlEscaped()
                    + QStringLiteral("; padding-left:10px; margin-left:2px\">");
            } else {
                out += QStringLiteral("<") + name + QStringLiteral(">");
            }
        }
        // Unknown tag: dropped; its text content still flows through.
    }

    // Close any anchors/spans left open by malformed input.
    while (!spanIsSpoiler.isEmpty()) {
        out += spanIsSpoiler.takeLast() ? QLatin1String("</span></a>")
                                        : QLatin1String("</span>");
    }
    while (!anchorEmitted.isEmpty()) {
        if (anchorEmitted.takeLast())
            out += QLatin1String("</a>");
    }
    // Emoji sizing runs last, over already-safe output, so the formatted and
    // code-block paths share it. The one non-render consumer (beginEdit ->
    // mention::refsFromSanitizedHtml) matches only mention anchors and strips
    // inner tags, so the span never reaches an outgoing formatted_body.
    return markEmoji(out);
}

QString MessageHtml::resolveInlineImages(
    const QString &safeHtml,
    const std::function<QString(const QString &)> &resolve)
{
    // Nothing to do for most messages, and this runs on every read.
    if (!resolve || !safeHtml.contains(QLatin1String("data-mx-emoticon")))
        return safeHtml;

    // Operates on sanitize()'s own output, so the only possible <img> is the
    // one it emitted, in its exact shape.
    static const QRegularExpression re(
        QStringLiteral("<img data-mx-emoticon src=\"(mxc://[^\"]+)\"([^>]*)>"),
        QRegularExpression::CaseInsensitiveOption);

    QString out;
    out.reserve(safeHtml.size());
    int last = 0;
    auto it = re.globalMatch(safeHtml);
    while (it.hasNext()) {
        const auto m = it.next();
        out += safeHtml.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        const QString mxc = m.captured(1);
        const QString source = resolve(mxc);
        if (source.isEmpty()) {
            // Not cached yet: show the shortcode rather than a broken-image
            // glyph; the fetch just started will re-render this.
            static const QRegularExpression altRe(
                QStringLiteral("alt=\"([^\"]*)\""),
                QRegularExpression::CaseInsensitiveOption);
            const auto alt = altRe.match(m.captured(2));
            out += alt.hasMatch() ? alt.captured(1) : QString();
            continue;
        }
        out += QStringLiteral("<img src=\"") + source.toHtmlEscaped()
            + QStringLiteral("\"") + m.captured(2) + QStringLiteral(">");
    }
    out += safeHtml.mid(last);
    return out;
}

QList<MessageHtml::Segment> MessageHtml::segments(
    const QString &html,
    const std::function<QString(const QString &)> &resolveDisplayName,
    const QString &ownUserId,
    const MentionStyle &mentionStyle,
    bool revealSpoilers)
{
    // Bounds. Well-formed events are capped at 64 KiB by the spec; these make a
    // hostile body degrade into fewer segments rather than unbounded work. The
    // input bound is above the code-text bound so the latter binds. Applied
    // before containsCodeBlock(), since formatted_body arrives uncapped and
    // that scan runs for every message.
    static constexpr qsizetype kMaxSegmentInput = 1024 * 1024;
    const QString in = html.size() > kMaxSegmentInput
        ? html.left(kMaxSegmentInput) : html;

    // The ordinary message: one RichText segment from the same sanitize() call
    // on the untouched input, so the two entry points cannot drift.
    if (!containsCodeBlock(in)) {
        return QList<Segment>{
            Segment{SegmentKind::RichText,
                    sanitize(html, resolveDisplayName, ownUserId, mentionStyle,
                             revealSpoilers),
                    QString()}};
    }

    static constexpr qsizetype kMaxSegments = 64;
    // In QChar (UTF-16) units, which bounds the renderer's memory.
    static constexpr qsizetype kMaxCodeChars = 256 * 1024;

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
        // Handed to the sanitizer verbatim; it owns the allowlist, href policy
        // and mention rewriting.
        const QString rich =
            sanitize(source, resolveDisplayName, ownUserId, mentionStyle,
                     revealSpoilers);
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
    TagScanner scanner(in);
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
        const ParsedTag t = scanner.tagAt(ltPos, end);
        if (!t.valid) {
            if (dropDepth == 0) {
                // Inside a code block a stray '<' is part of the program.
                // Outside, the sanitizer escapes it.
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
            // Drop-with-content still applies inside a code block.
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
                // A stray </pre> outside a block is noise: dropped.
                continue;
            }
            if (preDepth == 0) {
                flushRich();
                codeText.clear();
                codeLanguage = languageFromClass(t.raw);
            }
            // A nested <pre> is not a second block; the inner tag is dropped.
            ++preDepth;
            continue;
        }

        if (preDepth > 0) {
            // Inside a code block only <br> has meaning (one per line); other
            // tags are dropped and their text flows.
            if (name == QLatin1String("br") && !t.closing)
                codeText += QLatin1Char('\n');
            else if (name == QLatin1String("code") && !t.closing
                     && codeLanguage.isEmpty())
                codeLanguage = languageFromClass(t.raw);
            continue;
        }

        richSource += QLatin1Char('<') + t.raw + QLatin1Char('>');
    }

    // An unclosed <pre> still describes one block.
    if (preDepth > 0)
        flushCode();
    flushRich();
    return out;
}

QString MessageHtml::markRoomMention(const QString &safeHtml,
                                     const QString &color)
{
    static const QString kNeedle = QStringLiteral("@room");
    if (safeHtml.isEmpty() || color.isEmpty() || !safeHtml.contains(kNeedle))
        return safeHtml;

    // A match must stand alone, so "@roomba" and "user@room.example" are not
    // room pings.
    const auto boundaryBefore = [](QChar c) {
        return !(c.isLetterOrNumber() || c == QLatin1Char('@')
                 || c == QLatin1Char('_') || c == QLatin1Char('-')
                 || c == QLatin1Char('.') || c == QLatin1Char('/'));
    };
    const auto boundaryAfter = [](QChar c) {
        return !(c.isLetterOrNumber() || c == QLatin1Char('_')
                 || c == QLatin1Char('-') || c == QLatin1Char('.')
                 || c == QLatin1Char(':'));
    };

    const QString open = QStringLiteral("<span style=\"color:")
        + color.toHtmlEscaped() + QStringLiteral(";font-weight:600\"><b>");
    const QString close = QStringLiteral("</b></span>");

    QString out;
    out.reserve(safeHtml.size() + 64);
    int codeDepth = 0;
    qsizetype i = 0;
    const qsizetype n = safeHtml.size();
    while (i < n) {
        const QChar ch = safeHtml.at(i);
        if (ch == QLatin1Char('<')) {
            // Copy the tag through untouched and track code spans, so a literal
            // @room in code is left alone.
            const qsizetype gt = safeHtml.indexOf(QLatin1Char('>'), i);
            const qsizetype end = gt < 0 ? n : gt + 1;
            const QString tag = safeHtml.mid(i, end - i);
            const QString lower = tag.toLower();
            if (lower.startsWith(QLatin1String("<code"))
                || lower.startsWith(QLatin1String("<pre")))
                ++codeDepth;
            else if (lower.startsWith(QLatin1String("</code"))
                     || lower.startsWith(QLatin1String("</pre")))
                codeDepth = qMax(0, codeDepth - 1);
            out += tag;
            i = end;
            continue;
        }
        if (ch == QLatin1Char('&')) {
            // Entities are atomic: splitting "&amp;" would corrupt markup.
            const qsizetype semi = safeHtml.indexOf(QLatin1Char(';'), i);
            if (semi > i && semi - i <= 10) {
                out += safeHtml.mid(i, semi - i + 1);
                i = semi + 1;
                continue;
            }
            out += ch;
            ++i;
            continue;
        }
        if (codeDepth == 0 && ch == QLatin1Char('@')
            && QStringView(safeHtml).mid(i, kNeedle.size()) == kNeedle) {
            const QChar before = i > 0 ? safeHtml.at(i - 1) : QLatin1Char(' ');
            const qsizetype after = i + kNeedle.size();
            const QChar next = after < n ? safeHtml.at(after) : QLatin1Char(' ');
            if (boundaryBefore(before) && boundaryAfter(next)) {
                out += open + kNeedle + close;
                i = after;
                continue;
            }
        }
        out += ch;
        ++i;
    }
    return out;
}

QString MessageHtml::markEmoji(const QString &safeHtml)
{
    // Constant style. `x-large` is Qt's FontSizeAdjustment +2, the 1.5 rung of
    // its size ladder and the only scale-relative option (`em` and `%` are
    // ignored by Qt's CSS parser; see the header).
    static const QString kOpen =
        QStringLiteral("<span style=\"font-size:x-large\">");
    static const QString kClose = QStringLiteral("</span>");
    // Each run costs a fixed 43 characters and there are at most this many, so
    // output growth is bounded.
    static constexpr int kMaxRuns = 256;
    // A body of 1-3 emoji is the big-emoji row, already enlarged by the
    // delegate.
    static constexpr int kBigEmojiMaxSequences = 3;

    if (safeHtml.isEmpty() || !mayHoldEmoji(safeHtml))
        return safeHtml;

    struct Run {
        qsizetype start = 0;
        qsizetype end = 0;
    };
    QList<Run> runs;
    int emojiish = 0;      // clusters that could be part of an emoji-only body
    int nonEmojiish = 0;   // anything else that is not whitespace

    const qsizetype n = safeHtml.size();
    qsizetype i = 0;
    int codeDepth = 0;
    QList<char32_t> cps;

    while (i < n) {
        if (safeHtml.at(i) == QLatin1Char('<')) {
            // Track code spans as markRoomMention does, so emoji in code stay
            // text-sized.
            const qsizetype gt = safeHtml.indexOf(QLatin1Char('>'), i);
            const qsizetype end = gt < 0 ? n : gt + 1;
            const QString lower = safeHtml.mid(i, end - i).toLower();
            if (lower.startsWith(QLatin1String("<code"))
                || lower.startsWith(QLatin1String("<pre")))
                ++codeDepth;
            else if (lower.startsWith(QLatin1String("</code"))
                     || lower.startsWith(QLatin1String("</pre")))
                codeDepth = qMax(0, codeDepth - 1);
            i = end;
            continue;
        }

        qsizetype lt = safeHtml.indexOf(QLatin1Char('<'), i);
        if (lt < 0)
            lt = n;
        // Walk by grapheme cluster so ZWJ families, flags, keycaps and tone
        // variants are never split.
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme,
                                   safeHtml.constData() + i, lt - i);
        qsizetype cursor = i;
        qsizetype runStart = -1;
        const auto closeRun = [&](qsizetype at) {
            if (runStart < 0)
                return;
            // Past the cap runs are dropped, not merged: a wrong span is worse
            // than a missing one.
            if (runs.size() < kMaxRuns)
                runs.append(Run{runStart, at});
            runStart = -1;
        };
        while (cursor < lt) {
            // An entity is never an emoji here; re-encoding it would mean
            // decoding and rewriting sender text.
            if (safeHtml.at(cursor) == QLatin1Char('&')) {
                const qsizetype ee = entityEnd(safeHtml, cursor);
                if (ee > 0 && ee <= lt) {
                    closeRun(cursor);
                    ++nonEmojiish;
                    cursor = ee;
                    continue;
                }
            }
            finder.setPosition(cursor - i);
            qsizetype next = finder.toNextBoundary();
            next = next < 0 ? lt : i + next;
            if (next <= cursor)
                next = cursor + 1;
            if (next > lt)
                next = lt;

            cps.clear();
            for (qsizetype k = cursor; k < next; ++k) {
                const QChar c = safeHtml.at(k);
                if (c.isHighSurrogate() && k + 1 < next
                    && safeHtml.at(k + 1).isLowSurrogate()) {
                    cps.append(QChar::surrogateToUcs4(c, safeHtml.at(k + 1)));
                    ++k;
                } else {
                    cps.append(char32_t(c.unicode()));
                }
            }

            if (clusterIsWhitespace(cps)) {
                // Whitespace ends a run: a 1.5x space is just a wider gap.
                closeRun(cursor);
            } else if (clusterCouldBeEmoji(cps)) {
                ++emojiish;
                if (codeDepth == 0 && clusterIsEmoji(cps)) {
                    if (runStart < 0)
                        runStart = cursor;
                } else {
                    closeRun(cursor);
                }
            } else {
                ++nonEmojiish;
                closeRun(cursor);
            }
            cursor = next;
        }
        closeRun(lt);
        i = lt;
    }

    if (runs.isEmpty())
        return safeHtml;
    // Big-emoji suppression. `emojiish` uses the broad test (see
    // couldCarryEmoji), a superset of both detectors, so disagreement can only
    // suppress, never enlarge twice.
    if (nonEmojiish == 0 && emojiish >= 1 && emojiish <= kBigEmojiMaxSequences)
        return safeHtml;

    QString out;
    out.reserve(safeHtml.size() + runs.size() * (kOpen.size() + kClose.size()));
    qsizetype copied = 0;
    for (const Run &run : std::as_const(runs)) {
        out += QStringView(safeHtml).mid(copied, run.start - copied);
        out += kOpen;
        out += QStringView(safeHtml).mid(run.start, run.end - run.start);
        out += kClose;
        copied = run.end;
    }
    out += QStringView(safeHtml).mid(copied);
    return out;
}
