#include "models/RichComposition.h"

#include <QFont>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextList>
#include <QUrl>

#include <memory>

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

namespace {

// A newline the reader meant as a line, as a CommonMark hard break. Fenced and
// indented code, a setext underline and a table's lines are left exactly as
// they are, and so is a line that already ends in a hard break.
QString hardBreaksForNewlines(const QString &markdown)
{
    static const QRegularExpression fence(QStringLiteral("^ {0,3}(?:> ?)*(`{3,}|~{3,})"));
    static const QRegularExpression underline(
        QStringLiteral("^ {0,3}(?:> ?)*(?:=+|-+|(?:[-*_] *){3,})\\s*$"));
    const QStringList lines = markdown.split(QLatin1Char('\n'));
    QString out;
    out.reserve(markdown.size() + lines.size() * 2);
    QChar openFence;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        out += line;
        const QRegularExpressionMatch marker = fence.match(line);
        if (!openFence.isNull()) {
            if (marker.hasMatch() && marker.captured(1).at(0) == openFence)
                openFence = QChar();
        } else if (marker.hasMatch()) {
            openFence = marker.captured(1).at(0);
        } else if (i + 1 < lines.size()) {
            const QString &next = lines.at(i + 1);
            const bool breakable = !line.trimmed().isEmpty()
                && !next.trimmed().isEmpty()
                && !line.startsWith(QLatin1String("    "))
                && !line.startsWith(QLatin1Char('\t'))
                && !line.endsWith(QLatin1String("  "))
                && !line.endsWith(QLatin1Char('\\'))
                && !line.contains(QLatin1Char('|'))
                && !next.contains(QLatin1Char('|'))
                && !underline.match(line).hasMatch()
                && !underline.match(next).hasMatch();
            if (breakable)
                out += QStringLiteral("  ");
        }
        if (i + 1 < lines.size())
            out += QLatin1Char('\n');
    }
    return out;
}

// What survives of text once markup is set aside, for comparing text with
// what a load made of it. Link targets and fence lines are markup; a language
// tag is not text the reader wrote.
QString comparableText(QString text)
{
    static const QRegularExpression linkTarget(
        QStringLiteral("\\]\\((?:[^()\\s]|\\([^()\\s]*\\))*\\)"));
    static const QRegularExpression fenceLine(
        QStringLiteral("^ {0,3}(?:> ?)*(?:`{3,}|~{3,})[ \\t]*\\S*[ \\t]*$"),
        QRegularExpression::MultilineOption);
    static const QRegularExpression markup(QStringLiteral("[\\s*_~`#>\\-+=|\\[\\]\\\\]"));
    text.remove(fenceLine);
    text.replace(linkTarget, QStringLiteral("]"));
    text.remove(markup);
    return text;
}

// True when the document holds something compose() cannot send, or lost text:
// images, rules, tables, task marks, nested lists, or a list that does not
// start at 1 all reach the wire differently from what the box said.
bool loadLostSomething(const QTextDocument &document, const QString &text)
{
    if (!document.rootFrame()->childFrames().isEmpty())
        return true;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        const QTextBlockFormat format = block.blockFormat();
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            // compose() would send the words without an unsafe target.
            const QTextCharFormat run = it.fragment().charFormat();
            if (run.isAnchor() && !RichComposition::isSafeLinkTarget(run.anchorHref()))
                return true;
        }
        if (block.text().contains(QChar::ObjectReplacementCharacter)
            || format.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth)
            || format.marker() != QTextBlockFormat::MarkerType::NoMarker)
            return true;
        if (QTextList *list = block.textList()) {
            if (list->format().indent() > 1 || list->format().start() != 1)
                return true;
        }
    }
    const QString plain = RichComposition::compose(document).plainBody;
    if (plain.trimmed().isEmpty() != text.trimmed().isEmpty())
        return true;
    return comparableText(plain) != comparableText(text);
}

// Set on a document holding text the reader wrote as markdown and saw as
// characters. Qt's writer escapes those characters for a reader; that reader
// is the plain box, where the same text was never escaped.
const char kOpenedLiterally[] = "lightningOpenedLiterally";

// A load of user text never drops any of it: what Qt's reader would lose or
// compose() could not send opens as the literal text instead.
void keepTextIfLoadLostIt(QTextDocument *document, const QString &text)
{
    const bool literal = loadLostSomething(*document, text);
    if (literal)
        document->setPlainText(text);
    document->setProperty(kOpenedLiterally, literal);
}

QString withoutWriterEscapes(QString markdown)
{
    static const QRegularExpression escape(
        QStringLiteral("\\\\([!-/:-@\\[-`{-~])"));
    markdown.replace(escape, QStringLiteral("\\1"));
    return markdown;
}

// The quote markers a line of markdown opens with.
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

// Joins the lines of each paragraph of Qt's writer output: inside one, a single
// newline is a fold at 80 columns. Code, tables and block openers stay.
QString unfolded(const QString &markdown)
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
        out += joined ? content.trimmed() : line;
        joined = false;
        const QRegularExpressionMatch marker = fence.match(content);
        if (!openFence.isNull()) {
            if (marker.hasMatch() && marker.captured(1).at(0) == openFence)
                openFence = QChar();
        } else if (marker.hasMatch()) {
            openFence = marker.captured(1).at(0);
        } else if (i + 1 < lines.size() && !content.trimmed().isEmpty()
                   && !content.startsWith(QLatin1Char('|'))
                   && !content.startsWith(QLatin1String("    "))
                   && !content.trimmed().startsWith(QLatin1Char('#'))) {
            const QString &next = lines.at(i + 1);
            const QString nextContent = next.mid(prefix.size());
            joined = quotePrefixOf(next) == prefix
                && !nextContent.trimmed().isEmpty()
                && !opensBlock(nextContent);
        }
        if (joined) {
            while (out.endsWith(QLatin1Char(' ')))
                out.chop(1);
            out += QLatin1Char(' ');
        } else if (i + 1 < lines.size()) {
            out += QLatin1Char('\n');
        }
    }
    return out;
}

// One run of a seed: text with the inline formats markdown can say exactly.
struct SeedRun {
    QString text;
    QString href;
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool code = false;
    bool operator==(const SeedRun &) const = default;
};

// The text of a run, or nothing for an entity or a character that would have
// to be escaped: an escape shows in the box, so such a message is left alone.
// What markdown makes of the rest is the read-back's question.
bool seedTextFrom(QString text, QString *out)
{
    text.replace(QLatin1String("&lt;"), QLatin1String("<"));
    text.replace(QLatin1String("&gt;"), QLatin1String(">"));
    text.replace(QLatin1String("&quot;"), QLatin1String("\""));
    text.replace(QLatin1String("&#39;"), QLatin1String("'"));
    text.replace(QLatin1String("&amp;"), QLatin1String("&"));
    static const QRegularExpression active(
        QStringLiteral("[\\\\*`\\[\\]<~&\\n\\x{2028}\\x{a0}]"));
    if (active.match(text).hasMatch())
        return false;
    *out = text;
    return true;
}

// Sanitized html -> runs, refusing anything but one optional <p> around
// bold, italic, strike, inline code and http(s) links.
bool seedRunsFrom(const QString &html, QList<SeedRun> *runs)
{
    static const QRegularExpression tag(QStringLiteral("<(/?)([a-zA-Z0-9]+)([^>]*)>"));
    static const QRegularExpression href(
        QStringLiteral("^\\s+href=\"(https?://[^\"\\s()<>\\\\]+)\"(?:\\s+style=\"[^\"]*\")?\\s*$"));
    SeedRun state;
    int paragraphs = 0;
    bool paragraphClosed = false;
    qsizetype cursor = 0;
    const auto addText = [&](const QString &raw) {
        if (raw.isEmpty())
            return true;
        if (paragraphClosed)
            return false;
        SeedRun run = state;
        if (!seedTextFrom(raw, &run.text))
            return false;
        if (run.code && run.text.contains(QLatin1Char('`')))
            return false;
        if (!runs->isEmpty()) {
            SeedRun last = runs->constLast();
            last.text = run.text;
            if (last == run) {
                runs->last().text += run.text;
                return true;
            }
        }
        runs->append(run);
        return true;
    };
    for (auto it = tag.globalMatch(html); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        if (!addText(html.mid(cursor, m.capturedStart() - cursor)))
            return false;
        cursor = m.capturedEnd();
        const bool closing = !m.captured(1).isEmpty();
        const QString name = m.captured(2).toLower();
        const QString attributes = m.captured(3);
        if (name == QLatin1String("p")) {
            if (closing) {
                paragraphClosed = true;
            } else if (++paragraphs > 1 || !runs->isEmpty()) {
                return false;
            }
            continue;
        }
        bool *flag = nullptr;
        if (name == QLatin1String("strong") || name == QLatin1String("b"))
            flag = &state.bold;
        else if (name == QLatin1String("em") || name == QLatin1String("i"))
            flag = &state.italic;
        else if (name == QLatin1String("del") || name == QLatin1String("s"))
            flag = &state.strike;
        else if (name == QLatin1String("code"))
            flag = &state.code;
        if (flag) {
            // sanitize() styles <code>; the other tags carry no attribute.
            if (name != QLatin1String("code") && !attributes.isEmpty())
                return false;
            if (*flag == !closing)
                return false;
            *flag = !closing;
            continue;
        }
        if (name != QLatin1String("a"))
            return false;
        if (closing) {
            if (state.href.isEmpty())
                return false;
            state.href.clear();
            continue;
        }
        const QRegularExpressionMatch link = href.match(attributes);
        if (!link.hasMatch() || !state.href.isEmpty())
            return false;
        state.href = link.captured(1);
    }
    if (!addText(html.mid(cursor)))
        return false;
    return !state.bold && !state.italic && !state.strike && !state.code
        && state.href.isEmpty();
}

QString seedMarkdownFrom(const QList<SeedRun> &runs)
{
    QString out;
    for (const SeedRun &run : runs) {
        QString text = run.code
            ? QLatin1Char('`') + run.text + QLatin1Char('`')
            : run.text;
        if (run.strike)
            text = QStringLiteral("~~") + text + QStringLiteral("~~");
        if (run.italic)
            text = QLatin1Char('*') + text + QLatin1Char('*');
        if (run.bold)
            text = QStringLiteral("**") + text + QStringLiteral("**");
        if (!run.href.isEmpty())
            text = QLatin1Char('[') + text + QStringLiteral("](") + run.href
                + QLatin1Char(')');
        out += text;
    }
    return out;
}

} // namespace

void RichComposition::loadMarkdown(QTextDocument *document,
                                   const QString &markdown)
{
    if (!document)
        return;
    document->setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
    const bool literal = loadLostSomething(*document, markdown);
    // The mirror's own escapes are not the reader's text.
    if (literal)
        document->setPlainText(withoutWriterEscapes(markdown).trimmed());
    document->setProperty(kOpenedLiterally, literal);
}

void RichComposition::loadMarkdownLines(QTextDocument *document,
                                        const QString &markdown)
{
    if (!document)
        return;
    document->setMarkdown(hardBreaksForNewlines(markdown),
                          QTextDocument::MarkdownDialectGitHub);
    keepTextIfLoadLostIt(document, markdown);
}

QString RichComposition::markdownForPlainBox(const QTextDocument &document)
{
    constexpr auto dialect = QTextDocument::MarkdownDialectGitHub;
    const QString mirror = document.toMarkdown(dialect);
    // A line break the reader made must not read as a fold: carried through
    // the writer as a private-use character.
    const QString raw = document.toRawText();
    QChar lineBreak(0xE000);
    while (raw.contains(lineBreak))
        lineBreak = QChar(ushort(lineBreak.unicode() + 1));
    std::unique_ptr<QTextDocument> copy(document.clone());
    copy->setUndoRedoEnabled(false);
    QTextCursor cursor(copy.get());
    while (!(cursor = copy->find(QString(QChar::LineSeparator))).isNull())
        cursor.insertText(QString(lineBreak));
    QString out = unfolded(copy->toMarkdown(dialect));
    out.replace(lineBreak, QLatin1Char('\n'));
    // Should unfolding ever cost text, the folded mirror is still every word.
    QTextDocument check;
    check.setMarkdown(out, dialect);
    QString box = comparableText(compose(check).plainBody)
            == comparableText(compose(document).plainBody)
        ? out : mirror;
    // Qt pads an ordered marker to a column; the box reads as typed.
    static const QRegularExpression paddedMarker(
        QStringLiteral("^(\\s*\\d{1,9}[.)]) {2,}"),
        QRegularExpression::MultilineOption);
    box.replace(paddedMarker, QStringLiteral("\\1 "));
    // Text the reader never saw as formatted comes back as they wrote it. As
    // in Element's composer, what they applied here (a list) stays applied.
    return document.property(kOpenedLiterally).toBool()
        ? withoutWriterEscapes(box) : box;
}

QString RichComposition::plainTextOfSanitizedHtml(const QString &sanitizedHtml)
{
    // sanitize() emits only mxc: images, which a text document cannot load.
    static const QRegularExpression image(
        QStringLiteral("<img\\b[^>]*?\\balt=\"([^\"]*)\"[^>]*>"));
    QString html = sanitizedHtml;
    html.replace(image, QStringLiteral("\\1"));
    QTextDocument document;
    document.setHtml(html);
    QString text = document.toPlainText();
    text.replace(QChar::Nbsp, QLatin1Char(' '));
    text.remove(QChar::ObjectReplacementCharacter);
    return text.trimmed();
}

QString RichComposition::markdownFromSanitizedHtml(const QString &sanitizedHtml,
                                                   const QString &plainBody)
{
    QList<SeedRun> runs;
    if (sanitizedHtml.isEmpty() || !seedRunsFrom(sanitizedHtml, &runs))
        return {};
    QString text;
    bool formatted = false;
    for (const SeedRun &run : std::as_const(runs)) {
        text += run.text;
        formatted = formatted || run.bold || run.italic || run.strike
            || run.code || !run.href.isEmpty();
    }
    // A body that reads differently is the sender's own markdown source and
    // the better seed.
    if (!formatted || text != plainBody)
        return {};
    // The proof: read back, the seed must say exactly what the html said.
    // Anything the reader treats differently (emphasis against punctuation, a
    // line-opening marker) fails here and the message is left alone.
    const QString seed = seedMarkdownFrom(runs);
    QTextDocument document;
    document.setMarkdown(seed, QTextDocument::MarkdownDialectGitHub);
    if (document.blockCount() != 1)
        return {};
    QList<SeedRun> readBack;
    const QTextBlock block = document.begin();
    if (block.textList() || isCodeBlock(block)
        || block.blockFormat().headingLevel() > 0 || quoteLevelOf(block) > 0)
        return {};
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        const QTextCharFormat format = fragment.charFormat();
        const InlineFlags flags = flagsFor(format);
        SeedRun run;
        run.text = fragment.text();
        run.href = flags.href;
        run.bold = flags.bold;
        run.italic = flags.italic;
        run.strike = flags.strike;
        run.code = flags.code;
        const auto sameMarks = [&run](SeedRun last) {
            last.text = run.text;
            return last == run;
        };
        if (!readBack.isEmpty() && sameMarks(readBack.constLast()))
            readBack.last().text += run.text;
        else
            readBack.append(run);
    }
    return readBack == runs ? seed : QString();
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
