#include "models/RichComposition.h"

#include <QFont>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextList>
#include <QUrl>

namespace RichComposition {

namespace {

// Inline formats one run can carry, in the fixed nesting order the serializer
// emits; the document stores properties, not tag order, so a fixed order
// keeps output deterministic.
struct InlineFlags {
    bool anchor = false;
    QString href;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strike = false;
    bool code = false;

    bool any() const
    {
        return anchor || bold || italic || underline || strike || code;
    }
};

InlineFlags flagsFor(const QTextCharFormat &format)
{
    InlineFlags f;
    if (format.isAnchor() && !format.anchorHref().isEmpty()
        && isSafeLinkTarget(format.anchorHref())) {
        f.anchor = true;
        f.href = format.anchorHref();
    }
    f.bold = format.fontWeight() > QFont::Medium;
    f.italic = format.fontItalic();
    // Qt's HTML import (a mention pill) and the link toggle both underline an
    // anchor's text; that is how a link is drawn, not formatting the user
    // chose, and serializing it put a <u> inside every <a>.
    f.underline = format.fontUnderline() && !f.anchor;
    f.strike = format.fontStrikeOut();
    f.code = format.fontFixedPitch();
    return f;
}

// A matrix.to user permalink -> "@user:server", else empty. Duplicates
// MessageHtml's private helper rather than adding a shared header.
QString matrixToUserId(const QString &href)
{
    const QUrl u(href);
    if (u.host().compare(QLatin1String("matrix.to"), Qt::CaseInsensitive) != 0)
        return {};
    QString frag = u.fragment(QUrl::FullyDecoded);
    if (frag.startsWith(QLatin1Char('/')))
        frag = frag.mid(1);
    const int slash = frag.indexOf(QLatin1Char('/'));
    if (slash >= 0)
        frag = frag.left(slash);
    if (frag.startsWith(QLatin1Char('@')) && frag.contains(QLatin1Char(':')))
        return frag;
    return {};
}

QString escaped(QString text)
{
    // toHtmlEscaped covers & < > "; the apostrophe needs no escape in text
    // content or double-quoted attributes, which is all this file emits.
    return text.toHtmlEscaped();
}

// One block's inline content as Matrix HTML. Appends found mention MXIDs.
QString inlineHtml(const QTextBlock &block, QStringList *mentionIds,
                   bool *sawFormatting)
{
    QString out;
    for (auto it = block.begin(); it != block.end(); ++it) {
        const QTextFragment fragment = it.fragment();
        if (!fragment.isValid())
            continue;
        QString text = fragment.text();
        // Inline objects serialize to nothing: the composer never inserts them
        // and pasted ones have no Matrix representation.
        text.remove(QChar::ObjectReplacementCharacter);
        if (text.isEmpty())
            continue;
        const InlineFlags f = flagsFor(fragment.charFormat());
        if (f.any())
            *sawFormatting = true;

        QString open, close;
        if (f.anchor) {
            open += QStringLiteral("<a href=\"") + escaped(f.href)
                + QStringLiteral("\">");
            close.prepend(QStringLiteral("</a>"));
            const QString mxid = matrixToUserId(f.href);
            if (!mxid.isEmpty() && !mentionIds->contains(mxid))
                mentionIds->append(mxid);
        }
        if (f.bold) {
            open += QStringLiteral("<strong>");
            close.prepend(QStringLiteral("</strong>"));
        }
        if (f.italic) {
            open += QStringLiteral("<em>");
            close.prepend(QStringLiteral("</em>"));
        }
        if (f.underline) {
            open += QStringLiteral("<u>");
            close.prepend(QStringLiteral("</u>"));
        }
        if (f.strike) {
            open += QStringLiteral("<del>");
            close.prepend(QStringLiteral("</del>"));
        }
        if (f.code) {
            open += QStringLiteral("<code>");
            close.prepend(QStringLiteral("</code>"));
        }

        QString body = escaped(text);
        // Shift+Enter inside a block is U+2028 in the document model.
        body.replace(QChar::LineSeparator, QLatin1String("<br/>"));
        out += open + body + close;
    }
    return out;
}

// One block's inline content as plain text.
QString inlinePlain(const QTextBlock &block)
{
    QString text = block.text();
    text.remove(QChar::ObjectReplacementCharacter);
    text.replace(QChar::LineSeparator, QLatin1Char('\n'));
    return text;
}

int quoteLevelOf(const QTextBlock &block)
{
    return block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel);
}

bool isCodeBlock(const QTextBlock &block)
{
    // Qt's markdown importer stamps fenced code with BlockCodeLanguage /
    // BlockCodeFence (possibly empty strings — hasProperty is the test).
    const QTextBlockFormat bf = block.blockFormat();
    return bf.hasProperty(QTextFormat::BlockCodeLanguage)
        || bf.hasProperty(QTextFormat::BlockCodeFence);
}

} // namespace

bool isSafeLinkTarget(const QString &url)
{
    const QUrl u(url);
    if (!u.isValid())
        return false;
    const QString scheme = u.scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
        return !u.host().isEmpty() && u.userInfo().isEmpty();
    if (scheme == QLatin1String("mailto"))
        return !u.path().isEmpty();
    if (scheme == QLatin1String("matrix"))
        return true;
    return false;
}

Composed compose(const QTextDocument &document)
{
    Composed result;
    QStringList plainLines;
    bool sawFormatting = false;

    // Structural context carried between blocks.
    int openQuoteLevel = 0;
    QTextList *openList = nullptr;
    bool listOrdered = false;
    int listCounter = 0;
    bool inPre = false;
    QString structural; // the streamed structural HTML

    const auto closeList = [&] {
        if (openList) {
            structural += listOrdered ? QStringLiteral("</ol>")
                                      : QStringLiteral("</ul>");
            openList = nullptr;
        }
    };
    const auto closePre = [&] {
        if (inPre) {
            structural += QStringLiteral("</code></pre>");
            inPre = false;
        }
    };
    const auto setQuoteLevel = [&](int level) {
        while (openQuoteLevel < level) {
            structural += QStringLiteral("<blockquote>");
            ++openQuoteLevel;
        }
        while (openQuoteLevel > level) {
            structural += QStringLiteral("</blockquote>");
            --openQuoteLevel;
        }
    };

    for (QTextBlock block = document.begin(); block != document.end();
         block = block.next()) {
        const int quote = quoteLevelOf(block);
        QTextList *list = block.textList();
        const bool code = isCodeBlock(block);
        const int heading = block.blockFormat().headingLevel();

        // Every non-default structure is formatting.
        if (quote > 0 || list || code || heading > 0)
            sawFormatting = true;

        // ---- plain body line.
        QString plain = inlinePlain(block);
        if (list) {
            if (list != openList)
                listCounter = 0;
            ++listCounter;
            const QTextListFormat::Style style = list->format().style();
            const bool ordered = style == QTextListFormat::ListDecimal
                || style == QTextListFormat::ListLowerAlpha
                || style == QTextListFormat::ListUpperAlpha
                || style == QTextListFormat::ListLowerRoman
                || style == QTextListFormat::ListUpperRoman;
            plain = (ordered
                         ? QStringLiteral("%1. ").arg(listCounter)
                         : QStringLiteral("- "))
                + plain;
        }
        for (int i = 0; i < quote; ++i)
            plain.prepend(QStringLiteral("> "));
        plainLines.append(plain);

        // ---- structural HTML.
        if (code) {
            closeList();
            setQuoteLevel(0);
            if (!inPre) {
                structural += QStringLiteral("<pre><code>");
                inPre = true;
            } else {
                structural += QLatin1Char('\n');
            }
            // Code text is literal: escaped, no inline formatting.
            structural += escaped(inlinePlain(block));
            continue;
        }
        closePre();
        setQuoteLevel(quote);

        if (list) {
            const QTextListFormat::Style style = list->format().style();
            const bool ordered = style == QTextListFormat::ListDecimal
                || style == QTextListFormat::ListLowerAlpha
                || style == QTextListFormat::ListUpperAlpha
                || style == QTextListFormat::ListLowerRoman
                || style == QTextListFormat::ListUpperRoman;
            if (list != openList) {
                closeList();
                structural += ordered ? QStringLiteral("<ol>")
                                      : QStringLiteral("<ul>");
                openList = list;
                listOrdered = ordered;
            }
            structural += QStringLiteral("<li>")
                + inlineHtml(block, &result.mentionUserIds, &sawFormatting)
                + QStringLiteral("</li>");
            continue;
        }
        closeList();

        const QString inner =
            inlineHtml(block, &result.mentionUserIds, &sawFormatting);
        if (heading >= 1 && heading <= 6) {
            structural += QStringLiteral("<h%1>").arg(heading) + inner
                + QStringLiteral("</h%1>").arg(heading);
        } else {
            structural += QStringLiteral("<p>") + inner
                + QStringLiteral("</p>");
        }
    }
    closePre();
    closeList();
    setQuoteLevel(0);

    result.plainBody = plainLines.join(QLatin1Char('\n'));
    // Formatted only when the document carries formatting, matching the
    // markdown path. Multiple plain paragraphs are newlines in the plain body,
    // not markup.
    if (sawFormatting) {
        result.html = structural;
        // A single paragraph reads better unwrapped, and matches what the
        // markdown converter emits for one-line input.
        if (result.html.startsWith(QLatin1String("<p>"))
            && result.html.endsWith(QLatin1String("</p>"))
            && result.html.indexOf(QLatin1String("<p>"), 1) < 0) {
            result.html = result.html.mid(3, result.html.size() - 7);
        }
    }
    return result;
}

void toggleFormat(QTextDocument *document, int selectionStart,
                  int selectionEnd, const QString &format,
                  const QString &argument)
{
    if (!document)
        return;
    QTextCursor cursor(document);
    cursor.setPosition(qMax(0, qMin(selectionStart, selectionEnd)));
    cursor.setPosition(qMax(selectionStart, selectionEnd),
                       QTextCursor::KeepAnchor);
    // An empty selection formats the word under the caret; QML's TextEdit has
    // no per-keystroke format hook for "start typing bold".
    if (!cursor.hasSelection()) {
        cursor.select(QTextCursor::WordUnderCursor);
        if (!cursor.hasSelection() && format != QLatin1String("quote")
            && format != QLatin1String("list")
            && format != QLatin1String("orderedlist"))
            return;
    }

    const QVariantMap state =
        formatState(*document, cursor.selectionStart(), cursor.selectionEnd());
    QTextCharFormat charDelta;
    if (format == QLatin1String("bold")) {
        charDelta.setFontWeight(state.value(format).toBool() ? QFont::Normal
                                                             : QFont::Bold);
    } else if (format == QLatin1String("italic")) {
        charDelta.setFontItalic(!state.value(format).toBool());
    } else if (format == QLatin1String("underline")) {
        charDelta.setFontUnderline(!state.value(format).toBool());
    } else if (format == QLatin1String("strike")) {
        charDelta.setFontStrikeOut(!state.value(format).toBool());
    } else if (format == QLatin1String("code")) {
        const bool on = !state.value(format).toBool();
        charDelta.setFontFixedPitch(on);
        // A visible family change makes inline code read as code in the editor;
        // the serializer keys on fixed pitch alone.
        if (on)
            charDelta.setFontFamilies({ QStringLiteral("monospace") });
        else
            charDelta.setFontFamilies(QStringList{});
    } else if (format == QLatin1String("link")) {
        if (state.value(format).toBool()) {
            charDelta.setAnchor(false);
            charDelta.setAnchorHref(QString());
            charDelta.setFontUnderline(false);
        } else {
            if (!isSafeLinkTarget(argument))
                return; // refused: unsafe scheme never enters the document
            charDelta.setAnchor(true);
            charDelta.setAnchorHref(argument);
            charDelta.setFontUnderline(true);
        }
    } else if (format == QLatin1String("quote")) {
        QTextBlockFormat bf;
        const int level = state.value(format).toBool() ? 0 : 1;
        bf.setProperty(QTextFormat::BlockQuoteLevel, level);
        bf.setLeftMargin(level > 0 ? 24 : 0);
        cursor.mergeBlockFormat(bf);
        return;
    } else if (format == QLatin1String("list")
               || format == QLatin1String("orderedlist")) {
        const bool ordered = format == QLatin1String("orderedlist");
        if (state.value(format).toBool()) {
            // Detach every selected block from its list.
            QTextCursor walker(document);
            walker.setPosition(cursor.selectionStart());
            while (true) {
                QTextBlock block = walker.block();
                if (QTextList *list = block.textList())
                    list->remove(block);
                QTextBlockFormat bf = block.blockFormat();
                bf.setIndent(0);
                walker.mergeBlockFormat(bf);
                if (block.position() + block.length()
                    > cursor.selectionEnd())
                    break;
                walker.setPosition(block.position() + block.length());
                if (walker.atEnd())
                    break;
            }
        } else {
            cursor.createList(ordered ? QTextListFormat::ListDecimal
                                      : QTextListFormat::ListDisc);
        }
        return;
    } else {
        return; // unknown format: no-op, never a guess
    }
    cursor.mergeCharFormat(charDelta);
}

QVariantMap formatState(const QTextDocument &document, int selectionStart,
                        int selectionEnd)
{
    QVariantMap state;
    QTextCursor cursor(const_cast<QTextDocument *>(&document));
    const int start = qMax(0, qMin(selectionStart, selectionEnd));
    const int end = qMax(selectionStart, selectionEnd);
    // charFormat() reports the character BEFORE the position, so probe one
    // past the start (bounded by the end for a non-empty selection).
    cursor.setPosition(end > start ? qMin(start + 1, end) : start);
    const QTextCharFormat cf = cursor.charFormat();
    const InlineFlags f = flagsFor(cf);
    state.insert(QStringLiteral("bold"), f.bold);
    state.insert(QStringLiteral("italic"), f.italic);
    state.insert(QStringLiteral("underline"), f.underline);
    state.insert(QStringLiteral("strike"), f.strike);
    state.insert(QStringLiteral("code"), f.code);
    state.insert(QStringLiteral("link"),
                 cf.isAnchor() && !cf.anchorHref().isEmpty());

    cursor.setPosition(start);
    // Whether an inline format would have anything to act on: toggleFormat
    // formats the selection, or the word under the caret, and does nothing for
    // a bare caret between words. The toolbar greys B/I/S/code/U on false
    // rather than offering a button that silently does nothing.
    bool inlineTarget = end > start;
    if (!inlineTarget) {
        QTextCursor probe(cursor);
        probe.select(QTextCursor::WordUnderCursor);
        inlineTarget = probe.hasSelection();
    }
    state.insert(QStringLiteral("inlineTarget"), inlineTarget);
    const QTextBlock block = cursor.block();
    state.insert(QStringLiteral("quote"), quoteLevelOf(block) > 0);
    QTextList *list = block.textList();
    const bool ordered = list
        && (list->format().style() == QTextListFormat::ListDecimal
            || list->format().style() == QTextListFormat::ListLowerAlpha
            || list->format().style() == QTextListFormat::ListUpperAlpha
            || list->format().style() == QTextListFormat::ListLowerRoman
            || list->format().style() == QTextListFormat::ListUpperRoman);
    state.insert(QStringLiteral("list"), list != nullptr && !ordered);
    state.insert(QStringLiteral("orderedlist"), list != nullptr && ordered);
    return state;
}

} // namespace RichComposition

namespace RichComposition {
namespace {

constexpr auto kMarkdownDialect = QTextDocument::MarkdownDialectGitHub;

// The quote markers a line of markdown opens with, as far as they matter here.
QString quotePrefixOf(const QString &line)
{
    static const QRegularExpression prefix(QStringLiteral("^ {0,3}(?:> ?)+"));
    return prefix.match(line).captured();
}

// A line that opens a new block rather than continuing a paragraph.
bool opensBlock(const QString &content)
{
    static const QRegularExpression opener(QStringLiteral(
        "^ {0,3}(?:[-+*]\\s|\\d{1,9}[.)]\\s|#{1,6}(?:\\s|$)|```|~~~|\\|"
        "|[-=_*:|\\s]{3,}$)"));
    return opener.match(content).hasMatch();
}

// Makes each newline that merely continues a paragraph a line break, dropping
// the next line's quote markers and indentation. Fenced code, blank lines and
// whatever opens a new block keep their newlines.
QString lineBreaksForNewlines(const QString &markdown)
{
    static const QRegularExpression fence(QStringLiteral("^ {0,3}(`{3,}|~{3,})"));
    const QStringList lines = markdown.split(QLatin1Char('\n'));
    QString out;
    out.reserve(markdown.size());
    QChar openFence;
    bool joined = false;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        const QString prefix = quotePrefixOf(line);
        const QString content = line.mid(prefix.size());
        QString emitted = joined ? content.trimmed() : line;
        joined = false;
        const QRegularExpressionMatch marker = fence.match(content);
        if (!openFence.isNull()) {
            if (marker.hasMatch() && marker.captured(1).at(0) == openFence)
                openFence = QChar();
        } else if (marker.hasMatch()) {
            openFence = marker.captured(1).at(0);
        } else if (i + 1 < lines.size() && !content.trimmed().isEmpty()
                   && !content.startsWith(QLatin1String("    "))
                   && !content.startsWith(QLatin1Char('\t'))
                   && !content.trimmed().startsWith(QLatin1Char('#'))) {
            const QString &next = lines.at(i + 1);
            const QString nextContent = next.mid(prefix.size());
            joined = quotePrefixOf(next) == prefix
                && !nextContent.trimmed().isEmpty()
                && !opensBlock(nextContent);
        }
        if (joined) {
            // A hard break's own trailing spaces go with the newline.
            while (emitted.endsWith(QLatin1Char(' ')))
                emitted.chop(1);
        }
        out += emitted;
        if (joined)
            out += QChar::LineSeparator;
        else if (i + 1 < lines.size())
            out += QLatin1Char('\n');
    }
    return out;
}

// Backslashes what plain text would otherwise mean to markdown. An underscore
// inside a word means nothing, so a name like @bob_b stays as it reads.
QString escapedMarkdown(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        bool escape = c == QLatin1Char('\\') || c == QLatin1Char('*')
            || c == QLatin1Char('`') || c == QLatin1Char('[')
            || c == QLatin1Char(']') || c == QLatin1Char('<')
            || c == QLatin1Char('~');
        if (c == QLatin1Char('_')) {
            const bool before = i > 0 && text.at(i - 1).isLetterOrNumber();
            const bool after =
                i + 1 < text.size() && text.at(i + 1).isLetterOrNumber();
            escape = !(before && after);
        }
        if (escape)
            out += QLatin1Char('\\');
        out += c;
    }
    return out;
}

// What opens and closes a run with these flags, innermost last.
QPair<QString, QString> markdownMarkers(const InlineFlags &f)
{
    QString open, close;
    const auto wrap = [&](const QString &before, const QString &after) {
        open += before;
        close.prepend(after);
    };
    if (f.anchor) {
        QString href = f.href;
        href.replace(QLatin1Char(')'), QLatin1String("%29"));
        wrap(QStringLiteral("["), QStringLiteral("](") + href + QLatin1Char(')'));
    }
    if (f.bold)
        wrap(QStringLiteral("**"), QStringLiteral("**"));
    if (f.italic)
        wrap(QStringLiteral("*"), QStringLiteral("*"));
    if (f.strike)
        wrap(QStringLiteral("~~"), QStringLiteral("~~"));
    if (f.underline)
        wrap(QStringLiteral("<u>"), QStringLiteral("</u>"));
    return { open, close };
}

// One run of equal formatting. Spaces at its ends stay outside the markers,
// where CommonMark needs them; inline code is literal.
QString markdownRun(const QString &text, const InlineFlags &flags)
{
    qsizetype start = 0;
    while (start < text.size() && text.at(start).isSpace())
        ++start;
    qsizetype end = text.size();
    while (end > start && text.at(end - 1).isSpace())
        --end;
    if (start == end)
        return text;
    const QString core = text.mid(start, end - start);
    QString body;
    if (flags.code) {
        QString ticks(QStringLiteral("`"));
        while (core.contains(ticks))
            ticks += QLatin1Char('`');
        body = ticks + core + ticks;
    } else {
        body = escapedMarkdown(core);
    }
    const auto markers = markdownMarkers(flags);
    return text.left(start) + markers.first + body + markers.second
        + text.mid(end);
}

// One block's inline content as markdown, with a line break as a newline.
QString inlineMarkdown(const QTextBlock &block)
{
    QString out;
    QString pending;
    InlineFlags pendingFlags;
    const auto sameMarks = [](const InlineFlags &a, const InlineFlags &b) {
        return a.anchor == b.anchor && a.href == b.href && a.bold == b.bold
            && a.italic == b.italic && a.underline == b.underline
            && a.strike == b.strike && a.code == b.code;
    };
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        QString text = fragment.text();
        text.remove(QChar::ObjectReplacementCharacter);
        if (text.isEmpty())
            continue;
        const InlineFlags flags = flagsFor(fragment.charFormat());
        if (!pending.isEmpty() && !sameMarks(flags, pendingFlags)) {
            out += markdownRun(pending, pendingFlags);
            pending.clear();
        }
        pending += text;
        pendingFlags = flags;
    }
    if (!pending.isEmpty())
        out += markdownRun(pending, pendingFlags);
    out.replace(QChar::LineSeparator, QLatin1Char('\n'));
    // A literal marker at the start of a line stays literal.
    static const QRegularExpression literalMarker(
        QStringLiteral("^(\\s*)([-+>]|#{1,6}|\\d{1,9}[.)])(?=\\s|$)"),
        QRegularExpression::MultilineOption);
    QString escaped;
    qsizetype cursor = 0;
    for (auto m = literalMarker.globalMatch(out); m.hasNext();) {
        const QRegularExpressionMatch match = m.next();
        QString marker = match.captured(2);
        marker.insert(marker.at(0).isDigit() ? marker.size() - 1 : 0,
                      QLatin1Char('\\'));
        escaped += out.mid(cursor, match.capturedStart(2) - cursor) + marker;
        cursor = match.capturedEnd();
    }
    return escaped + out.mid(cursor);
}

} // namespace
} // namespace RichComposition

void RichComposition::loadMarkdown(QTextDocument *document,
                                   const QString &markdown)
{
    if (document)
        document->setMarkdown(lineBreaksForNewlines(markdown), kMarkdownDialect);
}

QString RichComposition::toMarkdown(const QTextDocument &document)
{
    // Walked here, in compose()'s shape, and not through
    // QTextDocument::toMarkdown(): QTextMarkdownWriter folds at 80 columns with
    // no switch for it, puts markers on lines of their own at a fold, and past
    // a fold with no space ahead drops one character per line.
    QString out;
    QTextList *lastList = nullptr;
    bool inFence = false;
    int lastQuote = 0;
    for (QTextBlock block = document.begin(); block != document.end();
         block = block.next()) {
        const bool code = isCodeBlock(block);
        if (code) {
            QString line = inlinePlain(block);
            if (inFence) {
                out += QLatin1Char('\n') + line;
            } else {
                if (!out.isEmpty())
                    out += QStringLiteral("\n\n");
                out += QStringLiteral("```")
                    + block.blockFormat().stringProperty(
                        QTextFormat::BlockCodeLanguage)
                    + QLatin1Char('\n') + line;
                inFence = true;
            }
            lastList = nullptr;
            lastQuote = 0;
            continue;
        }
        if (inFence) {
            out += QStringLiteral("\n```");
            inFence = false;
        }
        const int quote = quoteLevelOf(block);
        const QString prefix = QStringLiteral("> ").repeated(quote);
        QTextList *list = block.textList();
        QString text = inlineMarkdown(block);
        if (text.isEmpty() && !list)
            continue;
        QString marker;
        if (list) {
            const QTextListFormat::Style style = list->format().style();
            marker = style == QTextListFormat::ListDisc
                    || style == QTextListFormat::ListCircle
                    || style == QTextListFormat::ListSquare
                ? QStringLiteral("- ")
                : QStringLiteral("%1. ").arg(list->itemNumber(block) + 1);
        } else if (const int heading = block.blockFormat().headingLevel()) {
            marker = QStringLiteral("#").repeated(heading) + QLatin1Char(' ');
        }
        text.replace(QLatin1Char('\n'),
                     QLatin1Char('\n') + prefix
                         + QString(list ? marker.size() : 0, QLatin1Char(' ')));
        if (!out.isEmpty()) {
            if (list && list == lastList && quote == lastQuote)
                out += QLatin1Char('\n');
            else if (quote > 0 && quote == lastQuote)
                out += QLatin1Char('\n') + prefix.trimmed() + QLatin1Char('\n');
            else
                out += QStringLiteral("\n\n");
        }
        out += prefix + marker + text;
        lastList = list;
        lastQuote = quote;
    }
    if (inFence)
        out += QStringLiteral("\n```");
    return out;
}

QString RichComposition::markdownFromSanitizedHtml(const QString &sanitizedHtml,
                                                   const QString &plainBody)
{
    if (sanitizedHtml.isEmpty())
        return {};
    QString html = sanitizedHtml;
    // What sanitize() renders and markdown cannot say. A custom emoji is its
    // shortcode to the composer. Mention and spoiler anchors are internal
    // schemes that must not become links, and a mention's padding and bold
    // are its pill, not the sender's formatting.
    static const QRegularExpression image(
        QStringLiteral("<img\\b[^>]*?\\balt=\"([^\"]*)\"[^>]*>"));
    html.replace(image, QStringLiteral("\\1"));
    static const QRegularExpression internalAnchor(
        QStringLiteral("<a href=\"(mention|spoiler):[^\"]*\"[^>]*>(.*?)</a>"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression tag(QStringLiteral("<[^>]*>"));
    QString unwrapped;
    qsizetype cursor = 0;
    for (auto it = internalAnchor.globalMatch(html); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        unwrapped += html.mid(cursor, m.capturedStart() - cursor);
        QString inner = m.captured(2);
        if (m.captured(1) == QLatin1String("mention")) {
            inner.remove(tag);
            inner.replace(QLatin1String("&nbsp;"), QLatin1String(" "));
            inner = inner.trimmed();
        }
        unwrapped += inner;
        cursor = m.capturedEnd();
    }
    unwrapped += html.mid(cursor);
    QTextDocument document;
    // The newline that ends a <pre> is not a line of code.
    unwrapped.replace(QLatin1String("\n</code></pre>"),
                      QLatin1String("</code></pre>"));
    document.setHtml(unwrapped);
    // Qt's HTML import marks a <pre> only as unbreakable lines.
    QTextCursor blocks(&document);
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        if (!block.blockFormat().nonBreakableLines())
            continue;
        blocks.setPosition(block.position());
        QTextBlockFormat fence;
        fence.setProperty(QTextFormat::BlockCodeFence, QStringLiteral("`"));
        blocks.mergeBlockFormat(fence);
    }
    if (compose(document).html.isEmpty())
        return {};
    // A body that reads differently from the html is the sender's own markdown
    // source, and the better seed. Structure markers do not count: the rich
    // composer writes "- " and "> " into the plain body.
    static const QRegularExpression structure(
        QStringLiteral("^\\s*(?:[-*+]|\\d+[.)]|>|```\\S*)(?:\\s+|$)"),
        QRegularExpression::MultilineOption);
    const auto words = [](QString text) {
        text.replace(QChar::Nbsp, QLatin1Char(' '));
        text.replace(QChar::LineSeparator, QLatin1Char(' '));
        text.replace(QChar::ObjectReplacementCharacter, QLatin1Char(' '));
        return text.simplified();
    };
    if (words(document.toPlainText()) != words(QString(plainBody).remove(structure)))
        return {};
    return toMarkdown(document);
}

QVariantList RichComposition::spellSkipRanges(const QTextDocument &document)
{
    QVariantList out;
    auto push = [&out](int start, int length) {
        if (length > 0)
            out.append(QVariantMap{ { QStringLiteral("start"), start },
                                    { QStringLiteral("length"), length } });
    };
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        if (isCodeBlock(block)) {
            push(block.position(), block.length());
            continue;
        }
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            const QTextCharFormat cf = fragment.charFormat();
            const bool code = cf.fontFixedPitch();
            const bool mention = cf.isAnchor()
                && !matrixToUserId(cf.anchorHref()).isEmpty();
            if (code || mention)
                push(fragment.position(), fragment.length());
        }
    }
    return out;
}

void RichComposition::replaceRange(QTextDocument *document, int start, int length,
                                   const QString &replacement)
{
    if (!document || start < 0 || length <= 0
        || start + length > document->characterCount() - 1) {
        return;
    }
    QTextCursor cursor(document);
    cursor.setPosition(start);
    cursor.setPosition(start + length, QTextCursor::KeepAnchor);
    // Take the range's first character format before replacing, so a bold or
    // linked word stays bold or linked.
    QTextCursor probe(document);
    probe.setPosition(start + 1);
    const QTextCharFormat keep = probe.charFormat();
    cursor.beginEditBlock();
    cursor.insertText(replacement, keep);
    cursor.endEditBlock();
}

bool RichComposition::documentIsBlank(const QTextDocument &document)
{
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        if (!block.text().isEmpty())
            return false;
        // Structure without text is still visible: a list marker, a quote bar,
        // a code slab, a heading's metrics.
        if (block.textList() != nullptr)
            return false;
        const QTextBlockFormat bf = block.blockFormat();
        if (bf.intProperty(QTextFormat::BlockQuoteLevel) > 0
            || bf.headingLevel() > 0
            || bf.hasProperty(QTextFormat::BlockCodeLanguage)
            || bf.hasProperty(QTextFormat::BlockCodeFence)) {
            return false;
        }
    }
    return true;
}
