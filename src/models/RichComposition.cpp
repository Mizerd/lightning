#include "models/RichComposition.h"

#include <QFont>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextList>
#include <QUrl>

namespace RichComposition {

namespace {

// The inline formats one text run can carry, in the fixed nesting order the
// serializer emits. A fixed order is what keeps output deterministic across
// Qt versions — the document model stores properties, not tag order.
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
    f.underline = format.fontUnderline();
    f.strike = format.fontStrikeOut();
    f.code = format.fontFixedPitch();
    return f;
}

// A matrix.to user permalink -> the "@user:server" id, else empty. Same
// shape as MessageHtml's — duplicated deliberately: that one lives behind
// an anonymous namespace, and a two-line parse is cheaper than a new
// shared header.
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
        // Inline objects (images etc.) serialize to nothing: the composer
        // never inserts them and a pasted one has no Matrix representation
        // the attachment pipeline does not do better.
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
    // Formatted only when the document actually carries formatting — an
    // unformatted message stays a plain m.text event, matching the markdown
    // path's plain-text behaviour. A single unformatted paragraph is the
    // ordinary message; multiple plain paragraphs are newlines in the plain
    // body, not markup.
    if (sawFormatting) {
        result.html = structural;
        // The single-paragraph case reads better unwrapped ("<strong>x"
        // rather than "<p><strong>x</strong></p>"), and it is what the
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
    // An empty selection formats the word under the caret — the closest
    // honest equivalent of "start typing bold", which QML's TextEdit gives
    // us no per-keystroke format hook for.
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
        // A visible family change is what makes inline code READ as code in
        // the editor; the serializer keys on fixed pitch alone.
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
