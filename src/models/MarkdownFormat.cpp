#include "MarkdownFormat.h"

#include <algorithm>

namespace MarkdownFormat {
namespace {

struct Selection {
    int start = 0;
    int end = 0;
};

Selection normalized(const QString &text, int start, int end)
{
    Selection sel;
    sel.start = std::clamp(std::min(start, end), 0, int(text.size()));
    sel.end = std::clamp(std::max(start, end), 0, int(text.size()));
    return sel;
}

QString inlineMarker(const QString &format)
{
    if (format == QLatin1String("bold")) return QStringLiteral("**");
    if (format == QLatin1String("italic")) return QStringLiteral("_");
    if (format == QLatin1String("strike")) return QStringLiteral("~~");
    if (format == QLatin1String("code")) return QStringLiteral("`");
    return {};
}

QString linePrefix(const QString &format)
{
    if (format == QLatin1String("list")) return QStringLiteral("- ");
    if (format == QLatin1String("quote")) return QStringLiteral("> ");
    return {};
}

bool wrappedBy(const QString &text, const Selection &sel, const QString &marker)
{
    const int len = marker.size();
    return sel.start >= len && sel.end + len <= text.size()
        && text.mid(sel.start - len, len) == marker
        && text.mid(sel.end, len) == marker;
}

int lineStart(const QString &text, int pos)
{
    const int idx = text.lastIndexOf(QLatin1Char('\n'),
                                     std::max(0, pos - 1));
    return (pos <= 0 || idx < 0) ? 0 : idx + 1;
}

bool caretSpan(const QString &format, const QString &text, const Selection &sel,
               int *open, int *close)
{
    if (sel.start != sel.end || format == QLatin1String("italic"))
        return false;
    const QString marker = inlineMarker(format);
    int lineEnd = text.indexOf(QLatin1Char('\n'), sel.start);
    if (lineEnd < 0)
        lineEnd = text.size();
    int from = lineStart(text, sel.start);
    while (true) {
        const int first = text.indexOf(marker, from);
        if (first < 0 || first >= lineEnd)
            return false;
        const int second = text.indexOf(marker, first + marker.size());
        if (second < 0 || second >= lineEnd)
            return false;
        // Emphasis markers pair only when the opener is followed, and the
        // closer preceded, by something that is not whitespace (CommonMark's
        // flanking rule): in "2 ** 3 ** 4" the stars are literal, not a span.
        // A code span has no such rule.
        const bool flanking = format == QLatin1String("code")
            || (first + marker.size() < second
                && !text.at(first + marker.size()).isSpace()
                && !text.at(second - 1).isSpace());
        if (!flanking) {
            from = first + marker.size();
            continue;
        }
        if (sel.start > first && sel.start <= second + marker.size()) {
            *open = first;
            *close = second;
            return true;
        }
        from = second + marker.size();
    }
}

Result toggleInline(const QString &format, QString text, Selection sel)
{
    const QString marker = inlineMarker(format);
    const int len = marker.size();
    Result out;
    int open = 0;
    int close = 0;
    if (caretSpan(format, text, sel, &open, &close)) {
        const int caret = sel.start - std::min(sel.start - open, len)
            - std::max(0, std::min(sel.start - close, len));
        text.remove(close, len);
        text.remove(open, len);
        out.text = text;
        out.selectionStart = caret;
        out.selectionEnd = caret;
        return out;
    }
    if (wrappedBy(text, sel, marker)) {
        text.remove(sel.end, len);
        text.remove(sel.start - len, len);
        out.text = text;
        out.selectionStart = sel.start - len;
        out.selectionEnd = sel.end - len;
        return out;
    }
    const QString selected = text.mid(sel.start, sel.end - sel.start);
    if (selected.size() >= len * 2 && selected.startsWith(marker)
        && selected.endsWith(marker)) {
        text.remove(sel.end - len, len);
        text.remove(sel.start, len);
        out.text = text;
        out.selectionStart = sel.start;
        out.selectionEnd = sel.end - len * 2;
        return out;
    }
    text.insert(sel.end, marker);
    text.insert(sel.start, marker);
    out.text = text;
    out.selectionStart = sel.start + len;
    out.selectionEnd = sel.end + len;
    return out;
}

QList<int> coveredLineStarts(const QString &text, const Selection &sel)
{
    QList<int> starts;
    int pos = lineStart(text, sel.start);
    const int last = sel.end > sel.start ? sel.end - 1 : sel.start;
    while (true) {
        starts.append(pos);
        const int next = text.indexOf(QLatin1Char('\n'), pos);
        if (next < 0 || next >= last)
            break;
        pos = next + 1;
    }
    return starts;
}

bool allLinesPrefixed(const QString &text, const QList<int> &starts,
                      const QString &prefix)
{
    for (int s : starts) {
        if (text.mid(s, prefix.size()) != prefix)
            return false;
    }
    return !starts.isEmpty();
}

Result toggleLines(const QString &format, QString text, Selection sel)
{
    const QString prefix = linePrefix(format);
    const QList<int> starts = coveredLineStarts(text, sel);
    const bool removing = allLinesPrefixed(text, starts, prefix);
    Result out;
    int start = sel.start;
    int end = sel.end;
    for (int i = starts.size() - 1; i >= 0; --i) {
        const int s = starts.at(i);
        if (removing) {
            text.remove(s, prefix.size());
            if (start > s) start = std::max(s, start - int(prefix.size()));
            if (end > s) end = std::max(s, end - int(prefix.size()));
        } else if (text.mid(s, prefix.size()) != prefix) {
            text.insert(s, prefix);
            if (start >= s) start += prefix.size();
            if (end >= s) end += prefix.size();
        }
    }
    out.text = text;
    out.selectionStart = start;
    out.selectionEnd = end;
    return out;
}

bool linkAround(const QString &text, const Selection &sel, int *closeParen)
{
    if (sel.start < 1 || text.at(sel.start - 1) != QLatin1Char('['))
        return false;
    if (text.mid(sel.end, 2) != QLatin1String("]("))
        return false;
    const int close = text.indexOf(QLatin1Char(')'), sel.end + 2);
    if (close < 0)
        return false;
    if (closeParen)
        *closeParen = close;
    return true;
}

Result toggleLink(QString text, Selection sel)
{
    Result out;
    int close = -1;
    if (linkAround(text, sel, &close)) {
        text.remove(sel.end, close - sel.end + 1);
        text.remove(sel.start - 1, 1);
        out.text = text;
        out.selectionStart = sel.start - 1;
        out.selectionEnd = sel.end - 1;
        return out;
    }
    if (sel.start == sel.end) {
        text.insert(sel.start, QStringLiteral("[text](url)"));
        out.text = text;
        out.selectionStart = sel.start + 1;
        out.selectionEnd = sel.start + 5;
        return out;
    }
    text.insert(sel.end, QStringLiteral("](url)"));
    text.insert(sel.start, QLatin1Char('['));
    out.text = text;
    out.selectionStart = sel.end + 3;
    out.selectionEnd = sel.end + 6;
    return out;
}

}

Result toggle(const QString &format, const QString &text,
              int selectionStart, int selectionEnd)
{
    const Selection sel = normalized(text, selectionStart, selectionEnd);
    if (!inlineMarker(format).isEmpty())
        return toggleInline(format, text, sel);
    if (!linePrefix(format).isEmpty())
        return toggleLines(format, text, sel);
    if (format == QLatin1String("link"))
        return toggleLink(text, sel);
    Result out;
    out.text = text;
    out.selectionStart = sel.start;
    out.selectionEnd = sel.end;
    return out;
}

QVariantMap state(const QString &text, int selectionStart, int selectionEnd)
{
    const Selection sel = normalized(text, selectionStart, selectionEnd);
    QVariantMap flags;
    for (const auto &format : { QStringLiteral("bold"), QStringLiteral("italic"),
                                QStringLiteral("strike"), QStringLiteral("code") })
    {
        int open = 0;
        int close = 0;
        flags.insert(format, wrappedBy(text, sel, inlineMarker(format))
                                 || caretSpan(format, text, sel, &open, &close));
    }
    for (const auto &format : { QStringLiteral("list"), QStringLiteral("quote") })
        flags.insert(format,
                     allLinesPrefixed(text, coveredLineStarts(text, sel),
                                      linePrefix(format)));
    flags.insert(QStringLiteral("link"), linkAround(text, sel, nullptr));
    return flags;
}

}
