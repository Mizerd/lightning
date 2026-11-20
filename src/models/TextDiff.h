#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QTextBoundaryFinder>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace textdiff {

enum class Op { Same, Added, Removed };

struct Segment
{
    Op op;
    QString text;
    bool operator==(const Segment &o) const { return op == o.op && text == o.text; }
};

// Bounds the LCS table (uint16 cells) after the common prefix/suffix is trimmed.
inline constexpr qsizetype kMaxCells = 1'000'000;

inline QStringList tokenize(const QString &text)
{
    QStringList out;
    QTextBoundaryFinder f(QTextBoundaryFinder::Word, text);
    qsizetype start = 0;
    while (f.toNextBoundary() != -1) {
        out << text.mid(start, f.position() - start);
        start = f.position();
    }
    return out;
}

inline void push(QList<Segment> &out, Op op, const QString &text)
{
    if (text.isEmpty())
        return;
    if (!out.isEmpty() && out.last().op == op)
        out.last().text += text;
    else
        out.append({ op, text });
}

// *ok is false (and the result empty) when the texts are too large to diff.
inline QList<Segment> diff(const QString &before, const QString &after, bool *ok = nullptr)
{
    if (ok)
        *ok = true;
    const QStringList a = tokenize(before);
    const QStringList b = tokenize(after);
    qsizetype pre = 0;
    while (pre < a.size() && pre < b.size() && a[pre] == b[pre])
        ++pre;
    qsizetype suf = 0;
    while (suf < a.size() - pre && suf < b.size() - pre
           && a[a.size() - 1 - suf] == b[b.size() - 1 - suf])
        ++suf;
    const qsizetype n = a.size() - pre - suf;
    const qsizetype m = b.size() - pre - suf;
    if (n > 0 && m > 0 && (m >= kMaxCells || n >= kMaxCells / (m + 1))) {
        if (ok)
            *ok = false;
        return {};
    }

    QList<Segment> out;
    for (qsizetype i = 0; i < pre; ++i)
        push(out, Op::Same, a[i]);

    // lcs[i][j] = LCS length of a[pre+i..pre+n) and b[pre+j..pre+m).
    const qsizetype w = m + 1;
    // A one-sided change never reads the table.
    std::vector<std::uint16_t> lcs(n && m ? size_t((n + 1) * w) : 0, 0);
    for (qsizetype i = n - 1; i >= 0; --i)
        for (qsizetype j = m - 1; j >= 0; --j)
            lcs[i * w + j] = a[pre + i] == b[pre + j]
                ? std::uint16_t(lcs[(i + 1) * w + j + 1] + 1)
                : std::max(lcs[(i + 1) * w + j], lcs[i * w + j + 1]);
    qsizetype i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && a[pre + i] == b[pre + j]) {
            push(out, Op::Same, a[pre + i]);
            ++i, ++j;
        } else if (i < n && (j == m || lcs[(i + 1) * w + j] >= lcs[i * w + j + 1])) {
            push(out, Op::Removed, a[pre + i++]);
        } else {
            push(out, Op::Added, b[pre + j++]);
        }
    }

    for (qsizetype k = a.size() - suf; k < a.size(); ++k)
        push(out, Op::Same, a[k]);
    return out;
}

inline QString cssColor(const QString &c)
{
    if (c.size() != 7 || !c.startsWith(QLatin1Char('#')))
        return QStringLiteral("inherit");
    for (qsizetype i = 1; i < 7; ++i)
        if (!QStringLiteral("0123456789abcdefABCDEF").contains(c[i]))
            return QStringLiteral("inherit");
    return c;
}

// Every fragment is escaped; the result is safe for a RichText Text. Colours
// must be #rrggbb. Empty when the texts are too large to diff.
inline QString html(const QString &before, const QString &after,
                    const QString &added, const QString &removed)
{
    bool ok = false;
    const QList<Segment> segs = diff(before, after, &ok);
    if (!ok)
        return {};
    const auto escape = [](const QString &t) {
        return t.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    };
    QString out = QStringLiteral("<div style=\"white-space:pre-wrap\">");
    Op prev = Op::Same;
    for (const Segment &s : segs) {
        if (s.op == Op::Same) {
            out += escape(s.text);
            prev = Op::Same;
            continue;
        }
        // Spaces at a changed run's edges stay unmarked so the line or underline
        // covers words only; a whitespace-only change keeps its mark.
        qsizetype lead = 0;
        while (lead < s.text.size() && s.text[lead].isSpace())
            ++lead;
        qsizetype end = s.text.size();
        while (end > lead && s.text[end - 1].isSpace())
            --end;
        if (lead == end) {
            lead = 0;
            end = s.text.size();
        }
        // A thin space keeps a removed word and its replacement from reading as one.
        if (prev != Op::Same && lead == 0)
            out += QChar(0x2009);
        out += escape(s.text.left(lead));
        out += QStringLiteral("<span style=\"color:%1;text-decoration:%2\">%3</span>")
                   .arg(cssColor(s.op == Op::Added ? added : removed),
                        s.op == Op::Added ? QStringLiteral("underline")
                                          : QStringLiteral("line-through"),
                        escape(s.text.mid(lead, end - lead)));
        out += escape(s.text.mid(end));
        prev = end < s.text.size() ? Op::Same : s.op;
    }
    return out + QStringLiteral("</div>");
}

} // namespace textdiff
