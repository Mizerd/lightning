#include "models/MentionTokenizer.h"

#include <QRegularExpression>
#include <QUrl>

#include <algorithm>

namespace mention {
namespace {

constexpr int kMaxQuery = 40;

bool isQueryChar(QChar c)
{
    if (c.isLetterOrNumber())
        return true;
    switch (c.unicode()) {
    case u'.':
    case u'_':
    case u'=':
    case u'/':
    case u'+':
    case u'-':
    case u' ':
        return true;
    default:
        return false;
    }
}

// A '@' only opens a mention at the start of the line, after whitespace, or
// after an opening paren / blockquote marker. Any other preceding character
// (crucially an alphanumeric, i.e. a user@host address) is rejected.
bool isBoundaryBefore(QChar c)
{
    return c.isSpace() || c == QLatin1Char('(') || c == QLatin1Char('>');
}

// True when `atPos` (the '@') sits inside inline code (odd count of unescaped
// backticks earlier on the same logical line) or inside a fenced ``` block
// (odd number of fence lines strictly above the current line).
bool insideCode(const QString &text, int atPos)
{
    int lineStart = 0;
    if (atPos > 0) {
        const int nl = text.lastIndexOf(QLatin1Char('\n'), atPos - 1);
        lineStart = nl < 0 ? 0 : nl + 1;
    }

    const QString above = text.left(lineStart);
    int fenceCount = 0;
    const auto lines = above.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.trimmed().startsWith(QLatin1String("```")))
            ++fenceCount;
    }
    if (fenceCount % 2 == 1)
        return true; // inside a fenced code block

    int ticks = 0;
    for (int i = lineStart; i < atPos && i < text.length(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('\\')) {
            ++i; // the next character is escaped
            continue;
        }
        if (c == QLatin1Char('`'))
            ++ticks;
    }
    return (ticks % 2) == 1;
}

QString escapeLinkLabel(const QString &label)
{
    QString out;
    out.reserve(label.size());
    for (const QChar c : label) {
        if (c == QLatin1Char('\\') || c == QLatin1Char(']'))
            out += QLatin1Char('\\');
        out += c;
    }
    return out;
}

QString matrixToUrl(const QString &userId)
{
    // Percent-encode the whole MXID; matrix.to accepts the encoded form and
    // Lightning's incoming sanitizer decodes it FullyDecoded, so it round-trips
    // back to a mention pill.
    const QByteArray encoded = QUrl::toPercentEncoding(userId);
    return QLatin1String("https://matrix.to/#/") + QString::fromLatin1(encoded);
}

} // namespace

Token activeToken(const QString &text, int cursorPos)
{
    Token tok;
    if (cursorPos < 0)
        cursorPos = 0;
    if (cursorPos > text.length())
        cursorPos = text.length();

    int at = -1;
    for (int i = cursorPos - 1; i >= 0; --i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('@')) {
            at = i;
            break;
        }
        if (!isQueryChar(c))
            return tok; // an invalid character before the cursor: no token
        if (cursorPos - i > kMaxQuery)
            return tok; // query would exceed the cap
    }
    if (at < 0)
        return tok;
    if (at > 0 && !isBoundaryBefore(text.at(at - 1)))
        return tok; // mid-word or user@host: not a mention trigger

    const QString query = text.mid(at + 1, cursorPos - at - 1);
    if (query.length() > kMaxQuery)
        return tok;
    if (insideCode(text, at))
        return tok;

    tok.active = true;
    tok.start = at;
    tok.query = query;
    return tok;
}

InsertResult buildInsertion(const QString &text, int tokenStart, int cursorPos,
                            const QString &userId, const QString &displayName)
{
    InsertResult r;
    const int start = qBound(0, tokenStart, text.length());
    const int end = qBound(start, cursorPos, text.length());
    const QString label = QLatin1Char('@') + displayName;
    r.text = text.left(start) + label + QLatin1Char(' ') + text.mid(end);
    r.cursorPos = start + label.length() + 1; // just past the trailing space
    r.ref.userId = userId;
    r.ref.displayText = label;
    r.ref.start = start;
    r.ref.length = label.length();
    return r;
}

QList<MentionRef> shiftRefs(const QList<MentionRef> &refs, const QString &oldText,
                            const QString &newText)
{
    if (refs.isEmpty())
        return {};

    const int oldLen = oldText.length();
    const int newLen = newText.length();

    int p = 0;
    const int maxP = qMin(oldLen, newLen);
    while (p < maxP && oldText.at(p) == newText.at(p))
        ++p;

    int s = 0;
    const int maxS = qMin(oldLen - p, newLen - p);
    while (s < maxS && oldText.at(oldLen - 1 - s) == newText.at(newLen - 1 - s))
        ++s;

    const int oldChangeEnd = oldLen - s;
    const int delta = newLen - oldLen;

    QList<MentionRef> out;
    for (const MentionRef &ref : refs) {
        const int rs = ref.start;
        const int re = ref.start + ref.length;
        int newStart;
        if (re <= p)
            newStart = rs; // fully before the edit
        else if (rs >= oldChangeEnd)
            newStart = rs + delta; // fully after the edit
        else
            continue; // overlaps the edit: drop (fail-closed)
        if (newStart < 0 || newStart + ref.length > newLen)
            continue;
        if (newText.mid(newStart, ref.length) != ref.displayText)
            continue; // slice no longer matches: drop
        MentionRef moved = ref;
        moved.start = newStart;
        out.append(moved);
    }
    return out;
}

Expansion expand(const QString &text, const QList<MentionRef> &refs)
{
    Expansion result;

    QList<MentionRef> valid;
    valid.reserve(refs.size());
    for (const MentionRef &ref : refs) {
        if (ref.userId.isEmpty() || ref.length <= 0 || ref.start < 0
            || ref.start + ref.length > text.length())
            continue;
        if (text.mid(ref.start, ref.length) != ref.displayText)
            continue; // fail-closed: a degraded mention is simply not expanded
        valid.append(ref);
    }
    std::sort(valid.begin(), valid.end(),
              [](const MentionRef &a, const MentionRef &b) {
                  return a.start < b.start;
              });

    for (const MentionRef &ref : valid) {
        if (!result.userIds.contains(ref.userId))
            result.userIds.append(ref.userId);
    }

    QString body = text;
    for (int i = valid.size() - 1; i >= 0; --i) {
        const MentionRef &ref = valid.at(i);
        const QString link = QLatin1Char('[') + escapeLinkLabel(ref.displayText)
            + QLatin1String("](") + matrixToUrl(ref.userId) + QLatin1Char(')');
        body.replace(ref.start, ref.length, link);
    }
    result.body = body;
    return result;
}

Recovery recoverFromBody(const QString &body)
{
    Recovery out;
    static const QRegularExpression link(QStringLiteral(
        "\\[((?:[^\\]\\\\\\n]|\\\\.){1,120})\\]"
        "\\((https://matrix\\.to/#/[^)\\s]{1,512})\\)"));

    QString text;
    text.reserve(body.size());
    qsizetype cursor = 0;
    auto it = link.globalMatch(body);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();

        // Only matrix.to USER permalinks are mentions; room/event links (and
        // any unparseable target) stay as literal markdown.
        const QUrl url(m.captured(2));
        QString frag = url.fragment(QUrl::FullyDecoded);
        if (frag.startsWith(QLatin1Char('/')))
            frag = frag.mid(1);
        const qsizetype slash = frag.indexOf(QLatin1Char('/'));
        if (slash >= 0)
            frag = frag.left(slash);
        const bool isUser = frag.startsWith(QLatin1Char('@'))
            && frag.contains(QLatin1Char(':'));
        if (!isUser
            || url.host().compare(QLatin1String("matrix.to"),
                                  Qt::CaseInsensitive) != 0) {
            text += body.mid(cursor, m.capturedEnd() - cursor);
            cursor = m.capturedEnd();
            continue;
        }

        // Unescape the label ("\]" and "\\" per escapeLinkLabel).
        QString label = m.captured(1);
        label.replace(QLatin1String("\\]"), QLatin1String("]"));
        label.replace(QLatin1String("\\\\"), QLatin1String("\\"));

        text += body.mid(cursor, m.capturedStart() - cursor);
        MentionRef ref;
        ref.userId = frag;
        ref.displayText = label;
        ref.start = static_cast<int>(text.length());
        ref.length = static_cast<int>(label.length());
        out.refs.append(ref);
        text += label;
        cursor = m.capturedEnd();
    }
    text += body.mid(cursor);
    out.text = text;
    return out;
}

} // namespace mention
