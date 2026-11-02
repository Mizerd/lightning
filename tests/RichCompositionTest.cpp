
#include "models/RichComposition.h"

#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QtTest/QtTest>

using RichComposition::Composed;
using RichComposition::compose;

namespace {

QTextDocument *docWith(const QString &markdown, QObject *parent)
{
    auto *doc = new QTextDocument(parent);
    doc->setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
    return doc;
}

}

class RichCompositionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void plainTextComposesWithNoHtmlAtAll()
    {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("just words"));
        const Composed c = compose(doc);
        QCOMPARE(c.plainBody, QStringLiteral("just words"));
        QVERIFY(c.html.isEmpty());
    }

    void multiParagraphPlainTextStaysPlain()
    {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("one\ntwo"));
        const Composed c = compose(doc);
        QCOMPARE(c.plainBody, QStringLiteral("one\ntwo"));
        QVERIFY(c.html.isEmpty());
    }

    void everyInlineFormatSerializesToItsMatrixTag()
    {
        QTextDocument doc;
        QTextCursor cursor(&doc);
        QTextCharFormat bold;
        bold.setFontWeight(QFont::Bold);
        cursor.insertText(QStringLiteral("b"), bold);
        QTextCharFormat italic;
        italic.setFontItalic(true);
        cursor.insertText(QStringLiteral("i"), italic);
        QTextCharFormat underline;
        underline.setFontUnderline(true);
        cursor.insertText(QStringLiteral("u"), underline);
        QTextCharFormat strike;
        strike.setFontStrikeOut(true);
        cursor.insertText(QStringLiteral("s"), strike);
        QTextCharFormat code;
        code.setFontFixedPitch(true);
        cursor.insertText(QStringLiteral("c"), code);

        const Composed c = compose(doc);
        QCOMPARE(c.plainBody, QStringLiteral("biusc"));
        QCOMPARE(c.html,
                 QStringLiteral("<strong>b</strong><em>i</em><u>u</u>"
                                "<del>s</del><code>c</code>"));
    }

    void combinedFormatsNestInAFixedOrder()
    {
        QTextDocument doc;
        QTextCursor cursor(&doc);
        QTextCharFormat f;
        f.setFontWeight(QFont::Bold);
        f.setFontItalic(true);
        cursor.insertText(QStringLiteral("x"), f);
        const Composed c = compose(doc);
        QCOMPARE(c.html, QStringLiteral("<strong><em>x</em></strong>"));
    }

    void markupCharactersInTextAreContentNotTags()
    {
        QTextDocument doc;
        QTextCursor cursor(&doc);
        QTextCharFormat bold;
        bold.setFontWeight(QFont::Bold);
        cursor.insertText(QStringLiteral("<script>alert(1)</script> & <b>"),
                          bold);
        const Composed c = compose(doc);
        QVERIFY(!c.html.contains(QStringLiteral("<script")));
        QVERIFY(c.html.contains(
            QStringLiteral("&lt;script&gt;alert(1)&lt;/script&gt;")));
        QVERIFY(c.html.contains(QStringLiteral("&amp;")));
        QCOMPARE(c.plainBody,
                 QStringLiteral("<script>alert(1)</script> & <b>"));
    }

    void listsSerializeWithPlainMarkersBesideTheHtml()
    {
        QObject owner;
        QTextDocument *doc =
            docWith(QStringLiteral("- alpha\n- beta"), &owner);
        const Composed c = compose(*doc);
        QCOMPARE(c.plainBody, QStringLiteral("- alpha\n- beta"));
        QCOMPARE(c.html,
                 QStringLiteral("<ul><li>alpha</li><li>beta</li></ul>"));

        QTextDocument *ordered =
            docWith(QStringLiteral("1. one\n2. two"), &owner);
        const Composed o = compose(*ordered);
        QCOMPARE(o.plainBody, QStringLiteral("1. one\n2. two"));
        QCOMPARE(o.html,
                 QStringLiteral("<ol><li>one</li><li>two</li></ol>"));
    }

    void blockQuotesNestAndPrefixThePlainBody()
    {
        QObject owner;
        QTextDocument *doc = docWith(QStringLiteral("> quoted line"), &owner);
        const Composed c = compose(*doc);
        QCOMPARE(c.plainBody, QStringLiteral("> quoted line"));
        QVERIFY(c.html.startsWith(QStringLiteral("<blockquote>")));
        QVERIFY(c.html.endsWith(QStringLiteral("</blockquote>")));
        QVERIFY(c.html.contains(QStringLiteral("quoted line")));
    }

    void fencedCodeBecomesPreCodeWithLiteralText()
    {
        QObject owner;
        QTextDocument *doc = docWith(
            QStringLiteral("```\nint x = 1 < 2;\nreturn x;\n```"), &owner);
        const Composed c = compose(*doc);
        QVERIFY(c.html.contains(QStringLiteral("<pre><code>")));
        QVERIFY(c.html.contains(QStringLiteral("int x = 1 &lt; 2;")));
        QVERIFY(c.plainBody.contains(QStringLiteral("int x = 1 < 2;")));
    }

    void safeLinksSurviveAndTheirMentionIdsAreCollected()
    {
        QTextDocument doc;
        QTextCursor cursor(&doc);
        QTextCharFormat link;
        link.setAnchor(true);
        link.setAnchorHref(QStringLiteral("https://example.org/a?b=1"));
        cursor.insertText(QStringLiteral("site"), link);
        cursor.insertText(QStringLiteral(" and "), QTextCharFormat());
        QTextCharFormat mention;
        mention.setAnchor(true);
        mention.setAnchorHref(
            QStringLiteral("https://matrix.to/#/@alice:example.org"));
        cursor.insertText(QStringLiteral("@Alice"), mention);

        const Composed c = compose(doc);
        QVERIFY(c.html.contains(
            QStringLiteral("<a href=\"https://example.org/a?b=1\">site</a>")));
        QVERIFY(c.html.contains(QStringLiteral("matrix.to/#/@alice:example.org")));
        QCOMPARE(c.mentionUserIds,
                 QStringList{ QStringLiteral("@alice:example.org") });
        QCOMPARE(c.plainBody, QStringLiteral("site and @Alice"));
    }

    void aLinkOrPillIsNeverWrappedInUnderline()
    {
        QTextDocument doc;
        QTextCursor cursor(&doc);
        QTextCharFormat pill;
        pill.setAnchor(true);
        pill.setAnchorHref(QStringLiteral("https://matrix.to/#/@alex:example.org"));
        pill.setFontUnderline(true);
        cursor.insertText(QStringLiteral("@Alex"), pill);
        cursor.insertText(QStringLiteral(" "), QTextCharFormat());
        QTextCharFormat underlined;
        underlined.setFontUnderline(true);
        cursor.insertText(QStringLiteral("real"), underlined);

        const Composed c = compose(doc);
        QVERIFY2(!c.html.contains(QStringLiteral("<a href=\"https://matrix.to/#/@alex:example.org\"><u>")),
                 qPrintable(c.html));
        QVERIFY(c.html.contains(QStringLiteral("<u>real</u>")));

        QTextDocument viaToolbar;
        viaToolbar.setPlainText(QStringLiteral("site"));
        RichComposition::toggleFormat(&viaToolbar, 0, 4, QStringLiteral("link"),
                                      QStringLiteral("https://example.org/"));
        QVERIFY2(!compose(viaToolbar).html.contains(QStringLiteral("<u>")),
                 qPrintable(compose(viaToolbar).html));
    }

    void formatStateReportsWhetherAnInlineFormatHasATarget()
    {
        using RichComposition::formatState;
        QTextDocument empty;
        QVERIFY(!formatState(empty, 0, 0)
                     .value(QStringLiteral("inlineTarget")).toBool());

        QTextDocument doc;
        doc.setPlainText(QStringLiteral("one  two"));
        QVERIFY(formatState(doc, 0, 3)
                    .value(QStringLiteral("inlineTarget")).toBool());
        QVERIFY(formatState(doc, 1, 1)
                    .value(QStringLiteral("inlineTarget")).toBool());
    }

    void unsafeLinkTargetsSerializeAsPlainText()
    {
        QTextDocument doc;
        QTextCursor cursor(&doc);
        QTextCharFormat link;
        link.setAnchor(true);
        link.setAnchorHref(QStringLiteral("javascript:alert(1)"));
        cursor.insertText(QStringLiteral("click me"), link);
        const Composed c = compose(doc);
        QVERIFY(!c.html.contains(QStringLiteral("<a ")));
        QVERIFY(!c.html.contains(QStringLiteral("javascript")));
        QCOMPARE(c.plainBody, QStringLiteral("click me"));
    }

    void linkTargetPolicyIsExactlyTheEmittableSet()
    {
        using RichComposition::isSafeLinkTarget;
        QVERIFY(isSafeLinkTarget(QStringLiteral("https://example.org/x")));
        QVERIFY(isSafeLinkTarget(QStringLiteral("http://example.org")));
        QVERIFY(isSafeLinkTarget(QStringLiteral("mailto:a@b.c")));
        QVERIFY(isSafeLinkTarget(QStringLiteral("matrix:r/room:example.org")));
        QVERIFY(!isSafeLinkTarget(QStringLiteral("javascript:x()")));
        QVERIFY(!isSafeLinkTarget(QStringLiteral("data:text/html,x")));
        QVERIFY(!isSafeLinkTarget(QStringLiteral("file:///etc/passwd")));
        QVERIFY(!isSafeLinkTarget(
            QStringLiteral("https://user:pw@example.org")));
    }

    void shiftEnterBecomesABreakNotAParagraph()
    {
        QTextDocument doc;
        QTextCursor cursor(&doc);
        QTextCharFormat bold;
        bold.setFontWeight(QFont::Bold);
        cursor.insertText(QStringLiteral("a"), bold);
        cursor.insertText(QString(QChar::LineSeparator), bold);
        cursor.insertText(QStringLiteral("b"), bold);
        const Composed c = compose(doc);
        QCOMPARE(c.plainBody, QStringLiteral("a\nb"));
        QCOMPARE(c.html, QStringLiteral("<strong>a<br/>b</strong>"));
    }

    void toggleFormatRoundTripsOnASelection()
    {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("make me bold"));
        RichComposition::toggleFormat(&doc, 0, 12, QStringLiteral("bold"));
        QVERIFY(compose(doc).html.contains(QStringLiteral("<strong>")));
        QVERIFY(RichComposition::formatState(doc, 0, 12)
                    .value(QStringLiteral("bold")).toBool());
        RichComposition::toggleFormat(&doc, 0, 12, QStringLiteral("bold"));
        QVERIFY(compose(doc).html.isEmpty());
    }

    void toggleLinkRefusesAnUnsafeTarget()
    {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("target"));
        RichComposition::toggleFormat(&doc, 0, 6, QStringLiteral("link"),
                                      QStringLiteral("javascript:evil()"));
        QVERIFY(compose(doc).html.isEmpty());
        RichComposition::toggleFormat(&doc, 0, 6, QStringLiteral("link"),
                                      QStringLiteral("https://example.org"));
        QVERIFY(compose(doc).html.contains(
            QStringLiteral("<a href=\"https://example.org\">")));
    }

    void quoteAndListTogglesShapeTheBlocks()
    {
        QTextDocument doc;
        doc.setPlainText(QStringLiteral("line"));
        RichComposition::toggleFormat(&doc, 0, 4, QStringLiteral("quote"));
        QVERIFY(compose(doc).html.startsWith(QStringLiteral("<blockquote>")));
        RichComposition::toggleFormat(&doc, 0, 4, QStringLiteral("quote"));
        QVERIFY(compose(doc).html.isEmpty());

        RichComposition::toggleFormat(&doc, 0, 4, QStringLiteral("list"));
        QVERIFY(compose(doc).html.contains(QStringLiteral("<ul><li>")));
        RichComposition::toggleFormat(&doc, 0, 4, QStringLiteral("list"));
        QVERIFY(compose(doc).html.isEmpty());
    }

    void pastedRichContentCannotSmuggleActiveMarkup()
    {
        QTextDocument doc;
        doc.setHtml(QStringLiteral(
            "<p onmouseover=\"evil()\">hi <b>there</b></p>"
            "<script>evil()</script>"
            "<iframe src=\"https://evil.example\"></iframe>"
            "<a href=\"javascript:evil()\">link</a>"));
        const Composed c = compose(doc);
        QVERIFY(!c.html.contains(QStringLiteral("script")));
        QVERIFY(!c.html.contains(QStringLiteral("iframe")));
        QVERIFY(!c.html.contains(QStringLiteral("onmouseover")));
        QVERIFY(!c.html.contains(QStringLiteral("javascript:")));
        QVERIFY(c.html.contains(QStringLiteral("<strong>there</strong>")));
        QVERIFY(c.plainBody.contains(QStringLiteral("hi there")));
    }

    void markdownRoundTripPreservesTheCommonFormats()
    {
        QObject owner;
        const QString source = QStringLiteral(
            "**bold** *it* ~~gone~~ `code` [l](https://example.org)");
        QTextDocument *doc = docWith(source, &owner);
        const QString back =
            doc->toMarkdown(QTextDocument::MarkdownDialectGitHub);
        QVERIFY(back.contains(QStringLiteral("**bold**")));
        QVERIFY(back.contains(QStringLiteral("*it*")));
        QVERIFY(back.contains(QStringLiteral("~~gone~~")));
        QVERIFY(back.contains(QStringLiteral("`code`")));
        QVERIFY(back.contains(QStringLiteral("https://example.org")));
    }

    void structureWithNoTextIsNotABlankDocument()
    {
        QTextDocument empty;
        QVERIFY(RichComposition::documentIsBlank(empty));

        QTextDocument typed;
        QTextCursor tc(&typed);
        tc.insertText(QStringLiteral("a"));
        QVERIFY(!RichComposition::documentIsBlank(typed));

        QTextDocument list;
        QTextCursor lc(&list);
        lc.insertList(QTextListFormat::ListDecimal);
        QVERIFY(!RichComposition::documentIsBlank(list));

        QTextDocument bullets;
        QTextCursor bc(&bullets);
        bc.insertList(QTextListFormat::ListDisc);
        QVERIFY(!RichComposition::documentIsBlank(bullets));

        QTextDocument quote;
        QTextCursor qc(&quote);
        QTextBlockFormat qf = qc.blockFormat();
        qf.setProperty(QTextFormat::BlockQuoteLevel, 1);
        qc.setBlockFormat(qf);
        QVERIFY(!RichComposition::documentIsBlank(quote));

        QTextDocument code;
        QTextCursor cc(&code);
        QTextBlockFormat cf = cc.blockFormat();
        cf.setProperty(QTextFormat::BlockCodeFence, QStringLiteral("```"));
        cc.setBlockFormat(cf);
        QVERIFY(!RichComposition::documentIsBlank(code));

        QTextDocument paragraphs;
        QTextCursor pc(&paragraphs);
        pc.insertBlock();
        pc.insertBlock();
        QVERIFY(RichComposition::documentIsBlank(paragraphs));
    }

    void spellSkipRangesCoverCodeAndMentionsButNotLinkText()
    {
        QTextDocument doc;
        QTextCursor c(&doc);
        c.insertText(QStringLiteral("see "));
        QTextCharFormat code;
        code.setFontFixedPitch(true);
        c.insertText(QStringLiteral("teh_var"), code);
        c.insertText(QStringLiteral(" and "), QTextCharFormat());
        QTextCharFormat mention;
        mention.setAnchor(true);
        mention.setAnchorHref(QStringLiteral("https://matrix.to/#/@teh:example.org"));
        c.insertText(QStringLiteral("@Teh"), mention);
        c.insertText(QStringLiteral(" and "), QTextCharFormat());
        QTextCharFormat link;
        link.setAnchor(true);
        link.setAnchorHref(QStringLiteral("https://example.org/teh"));
        c.insertText(QStringLiteral("teh docs"), link);
        c.insertText(QStringLiteral(" teh"), QTextCharFormat());
        const QVariantList skip = RichComposition::spellSkipRanges(doc);
        QCOMPARE(skip.size(), 2);
        const QString plain = doc.toRawText();
        QCOMPARE(plain.mid(skip.at(0).toMap().value(QStringLiteral("start")).toInt(),
                           skip.at(0).toMap().value(QStringLiteral("length")).toInt()),
                 QStringLiteral("teh_var"));
        QCOMPARE(plain.mid(skip.at(1).toMap().value(QStringLiteral("start")).toInt(),
                           skip.at(1).toMap().value(QStringLiteral("length")).toInt()),
                 QStringLiteral("@Teh"));
    }

    void spellSkipRangesCoverAWholeCodeBlock()
    {
        QTextDocument doc;
        doc.setMarkdown(QStringLiteral("hello\n\n```\nteh code\n```\n\nteh"));
        const QVariantList skip = RichComposition::spellSkipRanges(doc);
        QVERIFY(!skip.isEmpty());
        const QString plain = doc.toRawText();
        bool coversCode = false;
        for (const QVariant &v : skip) {
            const QVariantMap r = v.toMap();
            const QString slice = plain.mid(r.value(QStringLiteral("start")).toInt(),
                                            r.value(QStringLiteral("length")).toInt());
            if (slice.contains(QStringLiteral("teh code")))
                coversCode = true;
            QVERIFY(!slice.startsWith(QStringLiteral("hello")));
        }
        QVERIFY(coversCode);
    }

    void replacingASuggestionKeepsTheWordsFormatting()
    {
        QTextDocument doc;
        QTextCursor c(&doc);
        c.insertText(QStringLiteral("a "));
        QTextCharFormat bold;
        bold.setFontWeight(QFont::Bold);
        bold.setAnchor(true);
        bold.setAnchorHref(QStringLiteral("https://example.org/"));
        c.insertText(QStringLiteral("teh"), bold);
        c.insertText(QStringLiteral(" end"), QTextCharFormat());
        RichComposition::replaceRange(&doc, 2, 3, QStringLiteral("the"));
        QCOMPARE(doc.toRawText(), QStringLiteral("a the end"));
        QTextCursor probe(&doc);
        probe.setPosition(3);
        QCOMPARE(probe.charFormat().fontWeight(), int(QFont::Bold));
        QVERIFY(probe.charFormat().isAnchor());
        QCOMPARE(probe.charFormat().anchorHref(), QStringLiteral("https://example.org/"));
        probe.setPosition(7);
        QVERIFY(!probe.charFormat().isAnchor());
        const RichComposition::Composed composed = RichComposition::compose(doc);
        QVERIFY(!composed.html.contains(QStringLiteral("SpellCheck")));
        QVERIFY(composed.html.contains(QStringLiteral("the")));
        RichComposition::replaceRange(&doc, 100, 3, QStringLiteral("x"));
        QCOMPARE(doc.toRawText(), QStringLiteral("a the end"));
    }

    void everyLineOfTheBoxIsALineOfTheDocument_data()
    {
        QTest::addColumn<QString>("markdown");
        QTest::addColumn<QString>("plain");
        QTest::newRow("lines") << QStringLiteral("hello world\nsecond\nthird\nfourth")
                               << QStringLiteral("hello world\nsecond\nthird\nfourth");
        QTest::newRow("bold then line") << QStringLiteral("**Note:**\nnext line")
                                        << QStringLiteral("Note:\nnext line");
        QTest::newRow("line then bold") << QStringLiteral("hello\n**(x)** there")
                                        << QStringLiteral("hello\n(x) there");
        QTest::newRow("italic") << QStringLiteral("*wow!*\nmore")
                                << QStringLiteral("wow!\nmore");
        QTest::newRow("strike") << QStringLiteral("~~gone.~~\nmore")
                                << QStringLiteral("gone.\nmore");
    }
    void everyLineOfTheBoxIsALineOfTheDocument()
    {
        QFETCH(QString, markdown);
        QFETCH(QString, plain);
        QTextDocument document;
        RichComposition::loadMarkdownLines(&document, markdown);
        const auto composed = compose(document);
        QCOMPARE(composed.plainBody.trimmed(), plain);
        QVERIFY(!composed.html.contains(QLatin1Char('*')));
        QVERIFY(!composed.html.contains(QLatin1Char('~')));
    }

    void structureIsNotTouchedByLineBreaks_data()
    {
        QTest::addColumn<QString>("markdown");
        QTest::newRow("fence") << QStringLiteral("```\ncode\nmore\n```");
        QTest::newRow("tilde fence") << QStringLiteral("~~~\ncode\nmore\n~~~");
        QTest::newRow("setext") << QStringLiteral("Title\n=====\nnext");
        QTest::newRow("indented") << QStringLiteral("    code\n    more");
        QTest::newRow("backslash") << QStringLiteral("a\\\nb");
        QTest::newRow("hard") << QStringLiteral("a  \nb");
    }
    void structureIsNotTouchedByLineBreaks()
    {
        QFETCH(QString, markdown);
        QTextDocument lines;
        RichComposition::loadMarkdownLines(&lines, markdown);
        QTextDocument plain;
        plain.setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
        QCOMPARE(lines.toMarkdown(QTextDocument::MarkdownDialectGitHub),
                 plain.toMarkdown(QTextDocument::MarkdownDialectGitHub));
        QCOMPARE(lines.blockCount(), plain.blockCount());
    }

    void aLoadThatWouldLoseTextOpensItLiterally_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("tag") << QStringLiteral("use <foo> now");
        QTest::newRow("fence words") << QStringLiteral("a\n\n~~~ note to self\nb\n~~~\nc");
        QTest::newRow("info string") << QStringLiteral("```python secret words\nprint(1)\n```");
        QTest::newRow("br") << QStringLiteral("a<br>b");
        QTest::newRow("comment") << QStringLiteral("a <!-- b --> c");
        QTest::newRow("html") << QStringLiteral("a <b>x</b> c");
        QTest::newRow("div") << QStringLiteral("<div>x</div>");
        QTest::newRow("typed html") << QStringLiteral("<b>x</b> <foo>");
        QTest::newRow("entity") << QStringLiteral("AT&amp;T &copy; &#65;");
        QTest::newRow("image") << QStringLiteral("![alt](https://x.y/a.png) text");
        QTest::newRow("image only") << QStringLiteral("![alt](https://x.y/a.png)");
        QTest::newRow("typed image") << QStringLiteral("![a](b)");
        QTest::newRow("rule") << QStringLiteral("above\n\n---\n\nbelow");
        QTest::newRow("rule only") << QStringLiteral("---");
        QTest::newRow("star rule") << QStringLiteral("a\n\n***\n\nb");
        QTest::newRow("task") << QStringLiteral("- [ ] todo\n- [x] done");
        QTest::newRow("table") << QStringLiteral("| a | b |\n|---|---|\n| 1 | 2 |");
        QTest::newRow("start") << QStringLiteral("5. a\n6. b");
        QTest::newRow("nested") << QStringLiteral("- a\n  - b\n- c");
        QTest::newRow("reference") << QStringLiteral("[a][r]\n\n[r]: https://x.y");
        QTest::newRow("code with tag") << QStringLiteral("<foo> x\n---\ny");
    }
    void aLoadThatWouldLoseTextOpensItLiterally()
    {
        QFETCH(QString, text);
        QTextDocument lines;
        RichComposition::loadMarkdownLines(&lines, text);
        QCOMPARE(lines.toPlainText(), text);
        QTextDocument draft;
        RichComposition::loadMarkdown(&draft, text);
        QCOMPARE(draft.toPlainText(), text);
        QCOMPARE(compose(lines).plainBody, text);
    }

    void aLiteralDocumentGoesBackToThePlainBoxUnescaped()
    {
        QTextDocument document;
        const QString text = QStringLiteral("x **bold** y use <foo> now");
        RichComposition::loadMarkdownLines(&document, text);
        QCOMPARE(document.toPlainText(), text);
        RichComposition::toggleFormat(&document, 0, 0, QStringLiteral("orderedlist"));
        QVERIFY(document.begin().textList());
        QCOMPARE(RichComposition::markdownForPlainBox(document).trimmed(),
                 QStringLiteral("1. ") + text);
    }

    void theBoxDoesNotEndInBlankLines()
    {
        QTextDocument document;
        QTextCursor(&document).insertText(QStringLiteral("one line"));
        const QString box = RichComposition::markdownForPlainBox(document);
        QCOMPARE(box, QStringLiteral("one line"));
    }

    void markerPaddingSurvivesInsideCodeBlocks_data()
    {
        QTest::addColumn<QString>("markdown");
        QTest::addColumn<QString>("kept");
        QTest::newRow("fenced") << QStringLiteral("```\n1.    keep\n2.    pad\n```\n")
                                << QStringLiteral("1.    keep");
        QTest::newRow("tilde") << QStringLiteral("~~~\n1.    keep\n~~~\n")
                               << QStringLiteral("1.    keep");
        QTest::newRow("indented")
            << QStringLiteral("para\n\n    1.    keep\n")
            << QStringLiteral("1.    keep");
    }
    void markerPaddingSurvivesInsideCodeBlocks()
    {
        QFETCH(QString, markdown);
        QFETCH(QString, kept);
        QTextDocument document;
        RichComposition::loadMarkdownLines(&document, markdown);
        const QString box = RichComposition::markdownForPlainBox(document);
        QVERIFY2(box.contains(kept), qPrintable(box));
    }
    void aRealOrderedListStillLosesItsPadding()
    {
        QTextDocument document;
        RichComposition::loadMarkdownLines(&document, QStringLiteral("a\nb"));
        QTextCursor c(&document);
        c.select(QTextCursor::Document);
        c.createList(QTextListFormat::ListDecimal);
        const QString box = RichComposition::markdownForPlainBox(document);
        QVERIFY2(box.contains(QStringLiteral("1. a")), qPrintable(box));
        QVERIFY2(!box.contains(QStringLiteral("1.  a")), qPrintable(box));
    }

    void textTypedInRichStaysEscapedForThePlainBox()
    {
        QTextDocument document;
        QTextCursor(&document).insertText(QStringLiteral("2 *3* 4"));
        QVERIFY(RichComposition::markdownForPlainBox(document).contains(
            QStringLiteral("\\*")));
    }

    void aRestoredDraftOpensAsTyped_data()
    {
        QTest::addColumn<QString>("mirror");
        QTest::addColumn<QString>("typed");
        QTest::newRow("html") << QStringLiteral("\\<b>x</b> \\<foo>\n\n")
                              << QStringLiteral("<b>x</b> <foo>");
        QTest::newRow("image") << QStringLiteral("\\![a](b) \\[c](d)\n\n")
                               << QStringLiteral("![a](b) [c](d)");
    }
    void aRestoredDraftOpensAsTyped()
    {
        QFETCH(QString, mirror);
        QFETCH(QString, typed);
        QTextDocument document;
        RichComposition::loadMarkdown(&document, mirror);
        QCOMPARE(document.toPlainText(), typed);
    }

    void aLoadThatKeepsEverythingStaysFormatted_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("fragment");
        QTest::newRow("bold") << QStringLiteral("**a** b") << QStringLiteral("<strong>");
        QTest::newRow("link") << QStringLiteral("[a](https://x.y/z) b")
                              << QStringLiteral("<a href");
        QTest::newRow("paren link") << QStringLiteral("[a](https://x.y/a_(b)) z")
                                    << QStringLiteral("<a href");
        QTest::newRow("list") << QStringLiteral("- a\n- b") << QStringLiteral("<ul>");
        QTest::newRow("ordered") << QStringLiteral("1. a\n2. b") << QStringLiteral("<ol>");
        QTest::newRow("quote") << QStringLiteral("> a") << QStringLiteral("<blockquote>");
        QTest::newRow("fence") << QStringLiteral("```cpp\nint a;\n```")
                               << QStringLiteral("<pre>");
        QTest::newRow("heading") << QStringLiteral("# h\ntext") << QStringLiteral("<h1>");
    }
    void aLoadThatKeepsEverythingStaysFormatted()
    {
        QFETCH(QString, text);
        QFETCH(QString, fragment);
        QTextDocument lines;
        RichComposition::loadMarkdownLines(&lines, text);
        QVERIFY2(compose(lines).html.contains(fragment),
                 qPrintable(compose(lines).html));
    }

    void theMirrorIsUnfoldedForThePlainBox_data()
    {
        QTest::addColumn<QString>("markdown");
        const QString words = QStringLiteral("word ").repeated(40).trimmed();
        QTest::newRow("paragraph") << words;
        QTest::newRow("bold") << words + QStringLiteral(" **b** ") + words;
        QTest::newRow("list") << QStringLiteral("- ") + words;
        QTest::newRow("quote") << QStringLiteral("> ") + words;
    }
    void theMirrorIsUnfoldedForThePlainBox()
    {
        QFETCH(QString, markdown);
        QTextDocument document;
        document.setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
        QString box = RichComposition::markdownForPlainBox(document).trimmed();
        QVERIFY2(!box.contains(QLatin1Char('\n')), qPrintable(box));
        box.append(QLatin1Char('x'));
        QTextDocument back;
        RichComposition::loadMarkdownLines(&back, box);
        QCOMPARE(back.blockCount(), document.blockCount());
    }

    void aLineTheReaderMadeStaysALineInThePlainBox()
    {
        QTextDocument document;
        QTextCursor cursor(&document);
        cursor.insertText(QStringLiteral("one"));
        cursor.insertText(QString(QChar::LineSeparator));
        cursor.insertText(QStringLiteral("two"));
        QCOMPARE(RichComposition::markdownForPlainBox(document).trimmed(),
                 QStringLiteral("one\ntwo"));
    }

    void anEditSeedSaysExactlyWhatInlineFormattingSaid_data()
    {
        QTest::addColumn<QString>("html");
        QTest::addColumn<QString>("body");
        QTest::addColumn<QString>("seed");
        QTest::newRow("bold") << QStringLiteral("<strong>hello</strong> there")
                              << QStringLiteral("hello there")
                              << QStringLiteral("**hello** there");
        QTest::newRow("paragraph") << QStringLiteral("<p><em>a</em> b</p>")
                                   << QStringLiteral("a b")
                                   << QStringLiteral("*a* b");
        QTest::newRow("code") << QStringLiteral("use <code style=\"background-color:#222\">b()</code>")
                              << QStringLiteral("use b()")
                              << QStringLiteral("use `b()`");
        QTest::newRow("strike") << QStringLiteral("<del>x</del> y")
                                << QStringLiteral("x y")
                                << QStringLiteral("~~x~~ y");
        QTest::newRow("link") << QStringLiteral("<a href=\"https://x.y/z\">wiki</a> x")
                              << QStringLiteral("wiki x")
                              << QStringLiteral("[wiki](https://x.y/z) x");
        QTest::newRow("snake") << QStringLiteral("<b>a</b> snake_case")
                               << QStringLiteral("a snake_case")
                               << QStringLiteral("**a** snake_case");
    }
    void anEditSeedSaysExactlyWhatInlineFormattingSaid()
    {
        QFETCH(QString, html);
        QFETCH(QString, body);
        QFETCH(QString, seed);
        QCOMPARE(RichComposition::markdownFromSanitizedHtml(html, body), seed);
    }

    void anEditSeedRefusesWhatItCannotSayExactly_data()
    {
        QTest::addColumn<QString>("html");
        QTest::addColumn<QString>("body");
        QTest::newRow("mention") << QStringLiteral("<a href=\"mention:@b:x\"><b>@b</b></a> <em>hi</em>")
                                 << QStringLiteral("@b hi");
        QTest::newRow("emoji") << QStringLiteral("<b>hi</b> <img src=\"mxc://x/y\" alt=\":b:\"/>")
                               << QStringLiteral("hi :b:");
        QTest::newRow("list") << QStringLiteral("<ul><li><b>a</b></li></ul>")
                              << QStringLiteral("- a");
        QTest::newRow("table") << QStringLiteral("<table><tr><td><b>a</b></td></tr></table>")
                               << QStringLiteral("a");
        QTest::newRow("spoiler") << QStringLiteral("<b>a</b> <a href=\"spoiler:toggle\">s</a>")
                                 << QStringLiteral("a s");
        QTest::newRow("sub") << QStringLiteral("<b>a</b> x<sub>2</sub>")
                             << QStringLiteral("a x2");
        QTest::newRow("break") << QStringLiteral("<b>a</b><br/>b")
                               << QStringLiteral("a\nb");
        QTest::newRow("two paragraphs") << QStringLiteral("<p><b>a</b></p><p>b</p>")
                                        << QStringLiteral("a\nb");
        QTest::newRow("pre") << QStringLiteral("<b>x</b><pre><code>a\n</code></pre>")
                             << QStringLiteral("x\n```\na\n```");
        QTest::newRow("punctuation flank") << QStringLiteral("<b>Note:</b>next")
                                           << QStringLiteral("Note:next");
        QTest::newRow("star") << QStringLiteral("<b>a</b> 2*3*4")
                              << QStringLiteral("a 2*3*4");
        QTest::newRow("entity") << QStringLiteral("<b>a</b> &amp;copy;")
                                << QStringLiteral("a &copy;");
        QTest::newRow("paren href") << QStringLiteral("<a href=\"https://x.y/a_(b)\">w</a>")
                                    << QStringLiteral("w");
        QTest::newRow("tick in code") << QStringLiteral("<code>a`b</code>")
                                      << QStringLiteral("a`b");
        QTest::newRow("line opener") << QStringLiteral("- <b>a</b>")
                                     << QStringLiteral("- a");
        QTest::newRow("spaces in bold") << QStringLiteral("<b>a </b>b")
                                        << QStringLiteral("a b");
    }
    void anEditSeedRefusesWhatItCannotSayExactly()
    {
        QFETCH(QString, html);
        QFETCH(QString, body);
        QCOMPARE(RichComposition::markdownFromSanitizedHtml(html, body), QString());
    }

    void aBodyThatIsItsOwnMarkdownStaysTheSeed()
    {
        QVERIFY(RichComposition::markdownFromSanitizedHtml(
                    QStringLiteral("<strong>bold</strong>"),
                    QStringLiteral("**bold**")).isEmpty());
        QVERIFY(RichComposition::markdownFromSanitizedHtml(
                    QStringLiteral("plain"), QStringLiteral("plain")).isEmpty());
        QVERIFY(RichComposition::markdownFromSanitizedHtml(
                    QString(), QStringLiteral("plain")).isEmpty());
    }
};

QTEST_MAIN(RichCompositionTest)
#include "RichCompositionTest.moc"
