// Rich composer: the QTextDocument -> Matrix serializer. Both wire bodies come
// from one document walk, so these cases pin (a) every formatting type's HTML
// shape, (b) the plain fallback derived beside it, (c) the whitelist (nothing
// an input can carry makes the serializer emit a tag this file does not name)
// and (d) link-target safety.

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

} // namespace

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
        // No formatting -> no formatted body, matching the markdown path's
        // behaviour for plain text.
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
        // The plain body keeps the literal characters untouched.
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
        // An explicit plain format: insertText(text) without one inherits
        // the cursor's current (anchor) format and Qt merges the fragments.
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

    // Qt's HTML import underlines an anchor, and so does the toolbar's link
    // toggle; neither is the user's underline, so no <u> goes inside an <a>.
    // A real underline beside the link is untouched.
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

    // toggleFormat formats the selection or the word at the caret and does
    // nothing otherwise; formatState reports which, so the toolbar can grey
    // B/I/S/code/U instead of offering a button that does nothing (the empty
    // composer is the case a user meets: "I clicked B and nothing happened").
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
        // http with embedded credentials is a phishing shape, refused.
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
        // A hostile paste after Qt ingests it: QTextDocument stores formatting,
        // not markup, and whatever survives can only leave through the
        // serializer's whitelist.
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
        // Draft-only mode switch: markdown in, document, markdown out.
        // Qt's own converters do this half; the case pins that our chosen
        // dialect keeps bold/italic/strike/code/lists/links intact.
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
    // ── Spell checking in rich mode ─────────────────────────────────

    // A document holding only structure (an empty list item) is not blank,
    // so the placeholder must not draw under it.
    void structureWithNoTextIsNotABlankDocument()
    {
        QTextDocument empty;
        QVERIFY(RichComposition::documentIsBlank(empty));

        QTextDocument typed;
        QTextCursor tc(&typed);
        tc.insertText(QStringLiteral("a"));
        QVERIFY(!RichComposition::documentIsBlank(typed));

        // An ordered list with nothing typed into it: Qt draws "1.".
        QTextDocument list;
        QTextCursor lc(&list);
        lc.insertList(QTextListFormat::ListDecimal);
        QVERIFY(!RichComposition::documentIsBlank(list));

        QTextDocument bullets;
        QTextCursor bc(&bullets);
        bc.insertList(QTextListFormat::ListDisc);
        QVERIFY(!RichComposition::documentIsBlank(bullets));

        // A quote bar and a code slab draw too.
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

        // Blank paragraphs are still blank: a document the user emptied by
        // pressing Return twice must get its placeholder back.
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

    // QA T-3: a mode switch flattened a four-line draft to one line, because
    // setMarkdown() reads a lone newline as a soft break.
    void markdownLinesSurviveAModeSwitch_data()
    {
        QTest::addColumn<QString>("markdown");
        QTest::newRow("lines") << QStringLiteral("hello world\nsecond line\nthird\nfourth");
        QTest::newRow("paragraphs") << QStringLiteral("one\n\ntwo");
        QTest::newRow("bold across lines")
            << QStringLiteral("**one**\nand **two**");
        QTest::newRow("list") << QStringLiteral("- one\n- two");
        QTest::newRow("fence") << QStringLiteral("```\ncode\nmore\n```");
    }
    void markdownLinesSurviveAModeSwitch()
    {
        QFETCH(QString, markdown);
        QTextDocument document;
        RichComposition::loadMarkdown(&document, markdown);
        const auto normal = [](QString text) {
            text.replace(QLatin1String("\n  ```"), QLatin1String("\n```"));
            return text.trimmed();
        };
        QCOMPARE(normal(RichComposition::toMarkdown(document)), markdown);
    }

    void aLineOfTheDraftIsALineOfTheDocument()
    {
        QTextDocument document;
        RichComposition::loadMarkdown(
            &document, QStringLiteral("a  \nb\n> c\n> d\n\n- e\n  f"));
        const auto composed = RichComposition::compose(document);
        QCOMPARE(composed.plainBody.trimmed(),
                 QStringLiteral("a\nb\n> c\nd\n- e\nf"));
        QVERIFY(composed.html.contains(QStringLiteral("c<br/>d")));
        QVERIFY(composed.html.contains(QStringLiteral("e<br/>f")));
    }

    // Qt's writer folds at 80 columns. Read back with T-3's rule every fold
    // would become a line of its own.
    void aLongLineIsNotFolded()
    {
        const QString words = QStringLiteral("word ").repeated(40).trimmed();
        QObject owner;
        QTextDocument *document = docWith(
            words + QStringLiteral(" **bold words here** ") + words, &owner);
        const QString markdown = RichComposition::toMarkdown(*document);
        QVERIFY2(!markdown.trimmed().contains(QLatin1Char('\n')),
                 qPrintable(markdown));
        QVERIFY(markdown.contains(QStringLiteral(" **bold words here** ")));
        // The caller's own document is not touched.
        QVERIFY(!document->toRawText().contains(QChar(0xE000)));

        QTextDocument again;
        RichComposition::loadMarkdown(&again, markdown);
        QCOMPARE(again.toRawText().trimmed(), document->toRawText().trimmed());
    }

    void aLongLineInsideAStructureIsNotFolded_data()
    {
        QTest::addColumn<QString>("prefix");
        QTest::newRow("list") << QStringLiteral("- ");
        QTest::newRow("quote") << QStringLiteral("> ");
    }
    void aLongLineInsideAStructureIsNotFolded()
    {
        QFETCH(QString, prefix);
        const QString words = QStringLiteral("word ").repeated(40).trimmed();
        QObject owner;
        QTextDocument *document = docWith(prefix + words + QStringLiteral(" **b ")
                                              + words + QStringLiteral("**"),
                                          &owner);
        const QString markdown = RichComposition::toMarkdown(*document).trimmed();
        QVERIFY2(!markdown.contains(QLatin1Char('\n')), qPrintable(markdown));
        QVERIFY(markdown.startsWith(prefix + QStringLiteral("word")));
    }

    void aLongWordKeepsEveryCharacter()
    {
        const QString token = QStringLiteral("abcdefghij").repeated(30);
        QObject owner;
        QTextDocument *document = docWith(
            QStringLiteral("see ") + token + QStringLiteral(" and **") + token
                + QStringLiteral("** end"), &owner);
        QTextDocument again;
        RichComposition::loadMarkdown(&again, RichComposition::toMarkdown(*document));
        QCOMPARE(again.toRawText().trimmed(), document->toRawText().trimmed());
    }

    void aLiteralMarkerStaysLiteral_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("dash") << QStringLiteral("- not a list");
        QTest::newRow("number") << QStringLiteral("1. not a list");
        QTest::newRow("hash") << QStringLiteral("## not a heading");
    }
    void aLiteralMarkerStaysLiteral()
    {
        QFETCH(QString, text);
        QTextDocument document;
        QTextCursor(&document).insertText(text);
        QTextDocument again;
        RichComposition::loadMarkdown(&again, RichComposition::toMarkdown(document));
        QVERIFY(!again.begin().textList());
        QCOMPARE(again.toPlainText(), text);
    }

    // QA T-7: a rich-mode send has a plain body, so the edit box showed plain
    // text and saving stripped the message.
    void anEditSeedKeepsTheFormattingARichBodyDoesNotCarry_data()
    {
        QTest::addColumn<QString>("html");
        QTest::addColumn<QString>("body");
        QTest::addColumn<QString>("seed");
        QTest::newRow("bold") << QStringLiteral("<strong>hello</strong> there")
                              << QStringLiteral("hello there")
                              << QStringLiteral("**hello** there");
        QTest::newRow("break") << QStringLiteral("<em>a</em><br/>b")
                               << QStringLiteral("a\nb")
                               << QStringLiteral("*a*\nb");
        QTest::newRow("list") << QStringLiteral("<ul><li><strong>a</strong></li><li>b</li></ul>")
                              << QStringLiteral("- a\n- b")
                              << QStringLiteral("- **a**\n- b");
        QTest::newRow("mention")
            << QStringLiteral("<a href=\"mention:@bob:x\" style=\"color:#fff\">"
                              "&nbsp;<b>@bob_b</b>&nbsp;</a> <em>hi</em>")
            << QStringLiteral("@bob_b hi")
            << QStringLiteral("@bob_b *hi*");
        QTest::newRow("emoji")
            << QStringLiteral("<strong>hi</strong> <img src=\"mxc://x/y\" "
                              "alt=\":blob:\" height=\"20\"/>")
            << QStringLiteral("hi :blob:")
            << QStringLiteral("**hi** :blob:");
        QTest::newRow("code block")
            << QStringLiteral("<strong>x</strong><pre><code>a\nb\n</code></pre>")
            << QStringLiteral("x\n```\na\nb\n```")
            << QStringLiteral("**x**\n\n```\na\nb\n```");
    }
    void anEditSeedKeepsTheFormattingARichBodyDoesNotCarry()
    {
        QFETCH(QString, html);
        QFETCH(QString, body);
        QFETCH(QString, seed);
        QString got = RichComposition::markdownFromSanitizedHtml(html, body);
        got.replace(QLatin1String("\n  ```"), QLatin1String("\n```"));
        QCOMPARE(got, seed);
    }

    void aBodyThatIsItsOwnMarkdownStaysTheSeed()
    {
        // Sent from the markdown box: the body is the source and shows it.
        QVERIFY(RichComposition::markdownFromSanitizedHtml(
                    QStringLiteral("<strong>bold</strong>"),
                    QStringLiteral("**bold**")).isEmpty());
        // Nothing formatted at all.
        QVERIFY(RichComposition::markdownFromSanitizedHtml(
                    QStringLiteral("plain"), QStringLiteral("plain")).isEmpty());
        QVERIFY(RichComposition::markdownFromSanitizedHtml(
                    QString(), QStringLiteral("plain")).isEmpty());
    }
};

// QTEST_MAIN, not GUILESS: toMarkdown() consults QFontDatabase, which
// qFatals without a QGuiApplication (offscreen platform via CMake).
QTEST_MAIN(RichCompositionTest)
#include "RichCompositionTest.moc"
