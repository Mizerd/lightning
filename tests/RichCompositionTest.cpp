// v0.9 rich composer: the QTextDocument -> Matrix serializer. Both wire
// bodies come from one document walk, so these cases pin (a) every
// formatting type's HTML shape, (b) the plain fallback derived beside it,
// (c) the whitelist property — nothing an input can carry makes the
// serializer emit a tag this file does not name — and (d) link-target
// safety.

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
        // What a hostile paste becomes AFTER Qt ingests it: QTextDocument
        // stores formatting, not markup, so script/iframe/event handlers do
        // not survive ingestion — and whatever DOES survive can only leave
        // through this serializer's whitelist.
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
};

// QTEST_MAIN, not GUILESS: toMarkdown() consults QFontDatabase, which
// qFatals without a QGuiApplication (offscreen platform via CMake).
QTEST_MAIN(RichCompositionTest)
#include "RichCompositionTest.moc"
