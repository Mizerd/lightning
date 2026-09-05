#include "models/MessageHtml.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringView>
#include <QTextBoundaryFinder>
#include <QUrl>

#include <algorithm>

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

/// One attribute's value out of a raw tag interior, quoted either way.
/// Mirrors extractHref, which is the same job for the one attribute that
/// already had a reader.
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

/// True when the raw tag interior carries `data-mx-emoticon` as a bare
/// attribute name. MSC2545 says the value is IGNORED — "some libraries may
/// automatically add an empty value" — so presence is the whole test, and a
/// value must not be required or a conformant sender is refused.
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


// ---- Inline emoji sizing (markEmoji) ------------------------------------

struct CodepointRange {
    char32_t lo;
    char32_t hi;
};

// Emoji_Presentation=Yes: the codepoints a conforming shaper draws as a
// PICTURE with no variation selector. This is the set that may be enlarged,
// and it is deliberately the narrow one — "1" and "#" and "(c)" are Emoji=Yes
// too, and blowing a digit up to 1.5x because a keycap exists somewhere in
// Unicode would be a rendering bug, not a feature.
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

// The BROAD test, used only to decide whether a body is "nothing but emoji"
// (see markEmoji). It must be a superset of everything the narrow test
// accepts AND of everything EmojiCatalog's catalogue lookup accepts, because
// a cluster this misses while the catalogue counts it would let a big-emoji
// body be enlarged twice. It covers Emoji=Yes characters that carry no
// default emoji presentation — a bare U+2764 HEAVY BLACK HEART is in the
// catalogue and is not in the table above. CJK and kana are deliberately
// OUTSIDE the band: they are letters, and a three-character Japanese message
// must not read as an emoji-only one.
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

// Does this grapheme cluster render as a picture? Narrow by design: it is
// what gets enlarged.
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
    // An explicit U+FE0F asks for emoji presentation on a base that does not
    // default to it (U+2764 U+FE0F, U+2708 U+FE0F). Qualified on the base
    // being a symbol so a selector after a letter cannot resize a word.
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

// One O(n) gate over the whole body. An ASCII message — most of them — leaves
// here without allocating anything or building a boundary finder.
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

// Is html[i] the start of a character entity? Returns its end (past the ';')
// or -1. Entities are ATOMIC here for the same reason they are in
// markRoomMention: splitting "&amp;" corrupts the markup, and can even
// manufacture a tag.
qsizetype entityEnd(const QString &html, qsizetype i)
{
    const qsizetype semi = html.indexOf(QLatin1Char(';'), i + 1);
    if (semi < 0 || semi - i > 10)
        return -1;
    return semi + 1;
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
    // Per open <span>: was it emitted as a spoiler (anchor-wrapped)? The
    // stack is what keeps a plain </span> from closing a spoiler anchor and
    // vice versa when spans nest.
    QList<bool> spanIsSpoiler;

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

        // ── Inline custom emoji (MSC2545) ────────────────────────────────
        //
        // The ONLY <img> this sanitizer emits, and it is deliberately not a
        // general image permission. `<img>` used to be dropped outright,
        // documented as "a formatted body can never make the client fetch a
        // remote/tracking image", and that property is kept exactly: an image
        // is rendered only when it is MARKED as an emoticon and addressed by
        // `mxc:`, which cannot be fetched without going through Lightning's
        // authenticated media path.
        //
        // What is emitted is still the MXC form, NOT a resolved local source.
        // sanitize()'s output has one non-render consumer —
        // TimelineModel::sanitizedHtmlForEvent -> MessageComposer::beginEdit —
        // and rewriting the src here would put a local `image://` URL into an
        // edited message's outgoing formatted_body. Resolution happens at
        // render time instead (resolveInlineImages).
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
            // An mxc URI has no query, no fragment and no credentials; a
            // value carrying any of those is not one, whatever it claims.
            if (src.contains(QLatin1Char('"')) || src.contains(QLatin1Char('<'))
                || src.contains(QLatin1Char('>')) || src.contains(QLatin1Char(' '))) {
                continue;
            }
            // The shortcode is remote text shown to the reader on hover and
            // read by assistive technology, so it is escaped and bounded.
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
            // The sender's own `height` is NOT honoured. It is remote input
            // that decides how much of the reader's message list one glyph
            // occupies, and "SHOULD be 32" is not a constraint anybody has to
            // obey. A fixed inline size is what makes an emoticon an
            // emoticon rather than a picture.
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
            // Inside a COVERED spoiler nothing is a link: the whole slab is
            // the reveal toggle, and a nested anchor would otherwise take the
            // click the user meant as "show me" and open a browser or a
            // profile. Revealed, the links come back.
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
                // The resolver answers the room's member name, or a global
                // profile name for a user the snapshot cannot place; the
                // localpart is the last resort. The label the SENDER wrote
                // inside the anchor is deliberately NOT a source (2026-09-05,
                // considered and refused): a pill that reads "@admin" while
                // linking to @attacker:evil is exactly the spoof the localpart
                // fallback prevents, and Element ignores the anchor text for
                // the same reason. A sender who is not in the room resolves
                // through the profile resolver instead.
                if (disp.isEmpty())
                    disp = localpart(mentionUser);
                const bool self =
                    !ownUserId.isEmpty() && mentionUser == ownUserId;
                // The accent is reserved for a mention of YOU; everyone else
                // takes the link ink (see MentionStyle). Either ink standing
                // in for a missing other, so a theme that pushes only one
                // still styles both cases rather than half of them.
                const QString &preferred = self ? mentionStyle.accentColor
                                                : mentionStyle.linkColor;
                const QString &alternate = self ? mentionStyle.linkColor
                                                : mentionStyle.accentColor;
                const QString ink =
                    preferred.isEmpty() ? alternate : preferred;
                out += QStringLiteral("<a href=\"mention:")
                    + mentionUser.toHtmlEscaped() + QStringLiteral("\"");
                if (!ink.isEmpty()) {
                    // Ink and weight only — no surface. Qt paints an inline
                    // background as an unroundable full-line-height slab, and
                    // `text-decoration:none` is required or the anchor keeps
                    // Qt's default underline (both verified against 6.11).
                    // The colors are model-validated hex literals, escaped
                    // again here so a style break-out is impossible.
                    out += QStringLiteral(" style=\"color:")
                        + ink.toHtmlEscaped()
                        + QStringLiteral(";font-weight:600")
                        + QStringLiteral(";text-decoration:none\"");
                }
                out += QStringLiteral(">");
                // <b> stays the self-mention marker rather than a heavier
                // font-weight in the style: it is the ONE signal that also
                // survives an unstyled body, so the two paths agree and
                // MentionTokenizer's recovery keeps matching one shape.
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
                // Nothing ever gave message links a colour, so Qt painted
                // them in its built-in link blue (#0000ff) — a hard primary
                // blue that is close to unreadable on the dark timeline
                // grounds. The underline stays: it is what separates a URL
                // from a mention now that both carry the theme's link ink.
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
            // v0.9 spoilers (spec §11.36): a data-mx-spoiler span becomes a
            // click-to-reveal run. Covered = a solid codeBackground slab
            // (background AND text in the same ink — Qt paints an inline
            // background as a full-line-height slab, which is exactly the
            // cover a spoiler wants); revealed keeps the slab as background
            // only. Both states wrap the run in the internal spoiler:toggle
            // anchor the delegate routes back to the model, never to a
            // browser. The optional reason value is deliberately ignored;
            // every other span stays attribute-stripped as before.
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
            } else if (!mentionStyle.linkColor.isEmpty()
                       && name == QLatin1String("blockquote")) {
                // A QUOTE BAR. Qt renders a bare <blockquote> as an indent and
                // nothing else, so a quoted line read as an accidentally
                // indented one — no bar, no tint, nothing saying "somebody
                // else said this". Qt 6 does honour block border properties
                // (QTextBlockFormat gained them in 5.14), so a left rule in
                // the theme's own link ink is the same treatment every other
                // client uses. The colour is a validated theme QColor, escaped
                // again here so a style break-out is impossible.
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
    // Inline emoji sizing is the LAST step, over output this function has
    // already made safe. It is applied here rather than at each render site
    // so the formatted path and the segmented code-block path cannot drift;
    // the one non-render consumer of this output
    // (TimelineModel::sanitizedHtmlForEvent -> MessageComposer::beginEdit ->
    // mention::refsFromSanitizedHtml) matches only mention anchors and strips
    // inner tags, so the span never reaches an outgoing formatted_body.
    return markEmoji(out);
}

QString MessageHtml::resolveInlineImages(
    const QString &safeHtml,
    const std::function<QString(const QString &)> &resolve)
{
    // Nothing to do for the overwhelming majority of messages, and this runs
    // on every read of a formatted body.
    if (!resolve || !safeHtml.contains(QLatin1String("data-mx-emoticon")))
        return safeHtml;

    // Operates on sanitize()'s OWN output, which is why this can be a
    // targeted rewrite rather than another parser: the only `<img>` that can
    // be here is the one the sanitizer emitted, in the exact shape it emits.
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
            // NOT CACHED YET. A broken-image glyph in the middle of a
            // sentence is worse than the shortcode it stands for, and the
            // fetch this call started will bring the reader back here.
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
    // The ordinary message: exactly one RichText segment whose text IS
    // sanitize()'s output. It is the SAME call, on the untouched input —
    // reproducing the sanitizer's bound or its scan here would make the two
    // free to drift, and the drift would be invisible until a body rendered
    // differently depending on which entry point read it.
    if (!containsCodeBlock(html)) {
        return QList<Segment>{
            Segment{SegmentKind::RichText,
                    sanitize(html, resolveDisplayName, ownUserId, mentionStyle,
                             revealSpoilers),
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

QString MessageHtml::markRoomMention(const QString &safeHtml,
                                     const QString &color)
{
    static const QString kNeedle = QStringLiteral("@room");
    if (safeHtml.isEmpty() || color.isEmpty() || !safeHtml.contains(kNeedle))
        return safeHtml;

    // A match must stand alone. Without this "@roomba" and "user@room.example"
    // would both light up as whole-room pings.
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
            // Copy the whole tag through untouched, and track code spans so a
            // literal @room inside one is left as the string it is.
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
            // Entities are atomic: splitting "&amp;" would corrupt the markup
            // and could even manufacture a tag.
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
    // The style is a compile-time constant of ours. `x-large` is Qt's
    // FontSizeAdjustment +2, i.e. the 1.5 rung of its 0.7/0.8/1.0/1.2/1.5/
    // 2.0/2.4 ladder — the one scale-RELATIVE lever this renderer offers
    // (`em` and `%` are silently ignored by Qt's CSS parser; see the header).
    static const QString kOpen =
        QStringLiteral("<span style=\"font-size:x-large\">");
    static const QString kClose = QStringLiteral("</span>");
    // A hostile body cannot make this grow without bound: each run costs a
    // fixed 43 characters and there are at most this many of them.
    static constexpr int kMaxRuns = 256;
    // A body of 1-3 emoji sequences is the big-emoji row, already rendered at
    // 48/60 px by the delegate. Enlarging it again would take it past 90.
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
            // Copy-through territory: track code spans exactly as
            // markRoomMention does, so an emoji in a code sample stays the
            // size of the characters around it.
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
        // Walk this text run by GRAPHEME cluster: one ZWJ family, one flag,
        // one keycap and one tone variant are each a single picture, and
        // splitting them would wrap half a glyph.
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme,
                                   safeHtml.constData() + i, lt - i);
        qsizetype cursor = i;
        qsizetype runStart = -1;
        const auto closeRun = [&](qsizetype at) {
            if (runStart < 0)
                return;
            // Past the cap the run is dropped rather than merged into a
            // neighbour: a body that hits it is pathological, and a wrong
            // span is worse than a missing one.
            if (runs.size() < kMaxRuns)
                runs.append(Run{runStart, at});
            runStart = -1;
        };
        while (cursor < lt) {
            // An entity is atomic and is never an emoji here: "&#128522;" is
            // markup for one, not the character itself, and re-encoding it
            // would mean decoding sender text and writing it back out.
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
                // Whitespace ENDS a run rather than joining it: a space at
                // 1.5x is a wider space, and the gap between two emoji is
                // not part of either picture.
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
    // The big-emoji suppression. `emojiish` is the BROAD test on purpose (see
    // couldCarryEmoji): it is a superset of both this file's narrow test and
    // EmojiCatalog's catalogue lookup, so where the two detectors disagree
    // the disagreement can only suppress — never enlarge a 60 px glyph again.
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
