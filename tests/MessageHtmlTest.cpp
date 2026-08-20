// v0.7.1: sanitizer for incoming Matrix formatted bodies. Proves the
// allowlist is fail-closed (dangerous tags/attributes/schemes are stripped),
// that matrix.to mentions become internal "mention:" links with resolved
// display names and self-mention emphasis, and that ordinary formatting and
// http(s) links survive.
//
// v0.7.4 adds the code-block segmentation half (MessageHtml::segments). The
// cases below are chosen from the shapes that actually broke the timeline or
// that a hostile body can construct: a terminal transcript, one line far
// wider than any pane, hundreds of lines, whitespace that IS the program,
// non-Latin and emoji inside code, HTML-looking text that must stay literal,
// blocks interleaved with prose, the segment/byte bounds, and the language
// token grammar. The last of these is a security boundary, not cosmetics:
// `class` is sender-chosen text and only a validated token may ever reach a
// label or an accessible name.

#include "models/MessageHtml.h"

#include <QtTest/QtTest>

namespace {
QString sanitize(const QString &html)
{
    return MessageHtml::sanitize(html, nullptr, QString());
}

QList<MessageHtml::Segment> segments(const QString &html)
{
    return MessageHtml::segments(html, nullptr, QString());
}

// The text of the n-th CodeBlock segment, or a null string when there is no
// such segment (so a failing assertion reports "empty" rather than aborting
// on an out-of-range index).
QString codeAt(const QList<MessageHtml::Segment> &segs, int nth)
{
    int seen = 0;
    for (const auto &s : segs) {
        if (s.kind != MessageHtml::SegmentKind::CodeBlock)
            continue;
        if (seen++ == nth)
            return s.text;
    }
    return {};
}

QString languageAt(const QList<MessageHtml::Segment> &segs, int nth)
{
    int seen = 0;
    for (const auto &s : segs) {
        if (s.kind != MessageHtml::SegmentKind::CodeBlock)
            continue;
        if (seen++ == nth)
            return s.language;
    }
    return {};
}

int countOf(const QList<MessageHtml::Segment> &segs,
            MessageHtml::SegmentKind kind)
{
    int n = 0;
    for (const auto &s : segs)
        if (s.kind == kind)
            ++n;
    return n;
}
} // namespace

class MessageHtmlTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void allowedFormattingSurvives()
    {
        QCOMPARE(sanitize(QStringLiteral("<strong>hi</strong> <em>there</em>")),
                 QStringLiteral("<strong>hi</strong> <em>there</em>"));
        QCOMPARE(sanitize(QStringLiteral("a<br>b")),
                 QStringLiteral("a<br>b"));
        QCOMPARE(sanitize(QStringLiteral("<code>x = 1</code>")),
                 QStringLiteral("<code>x = 1</code>"));
    }

    void attributesAreStripped()
    {
        // style / class / data-* and every other attribute are dropped.
        QCOMPARE(sanitize(QStringLiteral(
                     "<span data-mx-color=\"#ff0000\" style=\"color:red\">x</span>")),
                 QStringLiteral("<span>x</span>"));
    }

    void scriptAndStyleContentIsDropped()
    {
        QCOMPARE(sanitize(QStringLiteral("a<script>alert(1)</script>b")),
                 QStringLiteral("ab"));
        QCOMPARE(sanitize(QStringLiteral("a<style>body{}</style>b")),
                 QStringLiteral("ab"));
    }

    void imagesAreDropped()
    {
        // No formatted body may cause a remote image fetch.
        QCOMPARE(sanitize(QStringLiteral(
                     "hi <img src=\"https://tracker/x.png\" onerror=\"e()\">")),
                 QStringLiteral("hi "));
    }

    void mxReplyFallbackIsDropped()
    {
        QCOMPARE(sanitize(QStringLiteral(
                     "<mx-reply><blockquote>old</blockquote></mx-reply>real")),
                 QStringLiteral("real"));
    }

    void unknownTagsAreDroppedButTextKept()
    {
        QCOMPARE(sanitize(QStringLiteral("<marquee>hello</marquee>")),
                 QStringLiteral("hello"));
    }

    void strayLessThanIsEscaped()
    {
        QCOMPARE(sanitize(QStringLiteral("a < b and c")),
                 QStringLiteral("a &lt; b and c"));
    }

    void javascriptLinksBecomePlainText()
    {
        const QString out = sanitize(QStringLiteral(
            "<a href=\"javascript:alert(1)\">click</a>"));
        QVERIFY(!out.contains(QStringLiteral("<a")));
        QVERIFY(!out.contains(QStringLiteral("javascript")));
        QCOMPARE(out, QStringLiteral("click"));
    }

    void httpLinksSurvive()
    {
        QCOMPARE(sanitize(QStringLiteral(
                     "<a href=\"https://example.com/x\">site</a>")),
                 QStringLiteral("<a href=\"https://example.com/x\">site</a>"));
    }

    void matrixToMentionBecomesInternalLinkWithLocalpart()
    {
        // No resolver -> localpart fallback, never a bare MXID.
        const QString out = sanitize(QStringLiteral(
            "<a href=\"https://matrix.to/#/@bob:example.org\">"
            "@bob:example.org</a> hey"));
        QCOMPARE(out, QStringLiteral(
            "<a href=\"mention:@bob:example.org\">@bob</a> hey"));
    }

    void mentionUsesResolvedDisplayName()
    {
        const QString out = MessageHtml::sanitize(
            QStringLiteral(
                "<a href=\"https://matrix.to/#/@bob:example.org\">bob</a>"),
            [](const QString &id) {
                return id == QStringLiteral("@bob:example.org")
                    ? QStringLiteral("Bob Builder") : QString();
            },
            QString());
        QCOMPARE(out, QStringLiteral(
            "<a href=\"mention:@bob:example.org\">@Bob Builder</a>"));
    }

    void selfMentionIsEmphasised()
    {
        const QString out = MessageHtml::sanitize(
            QStringLiteral(
                "<a href=\"https://matrix.to/#/@me:example.org\">me</a>"),
            nullptr,
            QStringLiteral("@me:example.org"));
        QCOMPARE(out, QStringLiteral(
            "<a href=\"mention:@me:example.org\"><b>@me</b></a>"));
    }

    void mentionChipStyleIsApplied()
    {
        // With theme ink supplied, the mention renders as an inline chip:
        // accent ink on a soft surface, no underline, padded with nbsp.
        const QString out = MessageHtml::sanitize(
            QStringLiteral(
                "<a href=\"https://matrix.to/#/@bob:example.org\">bob</a>"),
            nullptr, QString(),
            MessageHtml::MentionStyle{QStringLiteral("#7c7ff2"),
                                      QStringLiteral("#25253d")});
        QCOMPARE(out, QStringLiteral(
            "<a href=\"mention:@bob:example.org\" "
            "style=\"color:#7c7ff2;background-color:#25253d;"
            "text-decoration:none\">&nbsp;@bob&nbsp;</a>"));
    }

    void mentionChipStyleCannotBreakOutOfTheAttribute()
    {
        // Hostile "colors" are escaped; the model additionally validates
        // hex literals before they get here.
        const QString out = MessageHtml::sanitize(
            QStringLiteral(
                "<a href=\"https://matrix.to/#/@bob:example.org\">bob</a>"),
            nullptr, QString(),
            MessageHtml::MentionStyle{
                QStringLiteral("\"><script>bad</script>"),
                QStringLiteral("#25253d")});
        QVERIFY(!out.contains(QStringLiteral("<script")));
    }

    void malformedInputDoesNotCrashAndFailsClosed()
    {
        // Unterminated tag: nothing after the '<' is emitted as markup.
        QVERIFY(!sanitize(QStringLiteral("text <b unclosed")).contains(
            QStringLiteral("<b")));
        // Empty and plain inputs round-trip safely.
        QCOMPARE(sanitize(QString()), QString());
        QCOMPARE(sanitize(QStringLiteral("just text")),
                 QStringLiteral("just text"));
    }

    // ---- v0.7.4 code-block segmentation -------------------------------

    // The guard the whole design rests on: an ordinary message must take the
    // fast path and come back byte-identical to sanitize(). If this ever
    // drifts, every existing message renders differently depending on which
    // entry point the model happened to read.
    void bodyWithoutACodeBlockIsExactlyOneSanitizedRichTextSegment()
    {
        const QStringList bodies = {
            QString(),
            QStringLiteral("just text"),
            QStringLiteral("<strong>hi</strong> <em>there</em>"),
            QStringLiteral("run <code>ls -la</code> please"),
            QStringLiteral("<blockquote>quoted</blockquote><p>after</p>"),
            QStringLiteral("<a href=\"https://matrix.to/#/@bob:example.org\">"
                           "bob</a> hey"),
            QStringLiteral("a < b and c"),
        };
        for (const QString &body : bodies) {
            const auto segs = segments(body);
            QCOMPARE(segs.size(), 1);
            QVERIFY(segs.at(0).kind == MessageHtml::SegmentKind::RichText);
            QCOMPARE(segs.at(0).text, sanitize(body));
            QVERIFY(segs.at(0).language.isEmpty());
        }
    }

    // The report that started this: a `dig` transcript. Every line survives,
    // in order, as one plain-text block — no markup, no re-escaping.
    void terminalTranscriptBecomesOnePlainTextBlock()
    {
        const QString html = QStringLiteral(
            "<pre><code>; &lt;&lt;&gt;&gt; DiG 9.18.24 &lt;&lt;&gt;&gt; "
            "example.com A\n"
            ";; global options: +cmd\n"
            ";; Got answer:\n"
            ";; -&gt;&gt;HEADER&lt;&lt;- opcode: QUERY, status: NOERROR, "
            "id: 4242\n"
            ";; flags: qr rd ra; QUERY: 1, ANSWER: 1\n"
            "\n"
            ";; ANSWER SECTION:\n"
            "example.com.\t\t3600\tIN\tA\t93.184.216.34\n"
            "</code></pre>");
        const auto segs = segments(html);
        QCOMPARE(countOf(segs, MessageHtml::SegmentKind::CodeBlock), 1);
        const QString code = codeAt(segs, 0);
        const QStringList lines = code.split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 8);
        // Entities decoded exactly once: the angle brackets are literal
        // characters of the transcript and never markup again.
        QVERIFY(lines.at(0).startsWith(QStringLiteral("; <<>> DiG")));
        QCOMPARE(lines.at(3),
                 QStringLiteral(";; ->>HEADER<<- opcode: QUERY, status: "
                                "NOERROR, id: 4242"));
        // The blank separator line is part of the transcript.
        QCOMPARE(lines.at(5), QString());
        // Tabs are columns here, not decoration.
        QCOMPARE(lines.at(7),
                 QStringLiteral("example.com.\t\t3600\tIN\tA\t93.184.216.34"));
        // No trailing empty line from the closing markup.
        QVERIFY(!code.endsWith(QLatin1Char('\n')));
    }

    // The exact shape that escaped the timeline: one line far wider than any
    // pane. It must arrive whole (the renderer scrolls it), not truncated and
    // not wrapped by the parser.
    void oneVeryLongLineSurvivesWholeAndUnbroken()
    {
        const QString line(5000, QLatin1Char('x'));
        const auto segs = segments(
            QStringLiteral("<pre><code>") + line
            + QStringLiteral("</code></pre>"));
        const QString code = codeAt(segs, 0);
        QCOMPARE(code.size(), 5000);
        QVERIFY(!code.contains(QLatin1Char('\n')));
    }

    void hundredsOfLinesAllSurviveInOrder()
    {
        QStringList lines;
        for (int i = 0; i < 150; ++i)
            lines << QStringLiteral("line %1").arg(i);
        const auto segs = segments(QStringLiteral("<pre><code>")
                                   + lines.join(QLatin1Char('\n'))
                                   + QStringLiteral("</code></pre>"));
        const QString code = codeAt(segs, 0);
        const QStringList out = code.split(QLatin1Char('\n'));
        QCOMPARE(out.size(), 150);
        QCOMPARE(out.first(), QStringLiteral("line 0"));
        QCOMPARE(out.last(), QStringLiteral("line 149"));
    }

    // Indentation IS the program in Python/YAML. Tabs, runs of spaces and
    // &nbsp; padding all have to reach the renderer unchanged.
    void whitespaceIsPreservedExactly()
    {
        const QString html = QStringLiteral(
            "<pre><code class=\"language-python\">\n"
            "def f(x):\n"
            "\tif x:\n"
            "        return    x\n"
            "&nbsp;&nbsp;# padded\n"
            "</code></pre>");
        const auto segs = segments(html);
        const QStringList lines = codeAt(segs, 0).split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 4);
        QCOMPARE(lines.at(0), QStringLiteral("def f(x):"));
        QCOMPARE(lines.at(1), QStringLiteral("\tif x:"));
        QCOMPARE(lines.at(2), QStringLiteral("        return    x"));
        // &nbsp; becomes a real space: a code block wants columns, not a
        // non-breaking glyph the monospace layout would treat differently.
        QCOMPARE(lines.at(3), QStringLiteral("  # padded"));
        QCOMPARE(languageAt(segs, 0), QStringLiteral("python"));
    }

    void nonLatinTextAndEmojiSurviveInsideCode()
    {
        const QString html = QStringLiteral(
            "<pre><code># \u30b3\u30e1\u30f3\u30c8 \U0001F389 \u2014 "
            "\u043f\u0440\u0438\u0432\u0435\u0442\n"
            "print(\"h\u00e9llo \U0001F60A\")</code></pre>");
        const QStringList lines =
            codeAt(segments(html), 0).split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines.at(0),
                 QStringLiteral("# \u30b3\u30e1\u30f3\u30c8 \U0001F389 "
                                "\u2014 \u043f\u0440\u0438\u0432\u0435\u0442"));
        QCOMPARE(lines.at(1),
                 QStringLiteral("print(\"h\u00e9llo \U0001F60A\")"));
    }

    // A numeric entity for an astral code point must decode to the one
    // character, not to a lone surrogate or a replacement glyph.
    void numericEntitiesDecodeToRealCharacters()
    {
        const QString code = codeAt(
            segments(QStringLiteral(
                "<pre><code>a&#128512;b&#x41;c</code></pre>")), 0);
        QCOMPARE(code, QStringLiteral("a\U0001F600bAc"));
    }

    // HTML-shaped text inside a code block is TEXT. It arrives decoded, and
    // the renderer shows it with Text.PlainText, so it can never become
    // markup a second time.
    void htmlLookingCodeArrivesAsLiteralText()
    {
        const QString code = codeAt(
            segments(QStringLiteral(
                "<pre><code>&lt;div class=\"x\"&gt;hi&lt;/div&gt;\n"
                "&lt;script&gt;alert(1)&lt;/script&gt;\n"
                "a &amp;&amp; b</code></pre>")), 0);
        QCOMPARE(code,
                 QStringLiteral("<div class=\"x\">hi</div>\n"
                                "<script>alert(1)</script>\n"
                                "a && b"));
    }

    // Decoding is ONE pass. "&amp;lt;" was written to display the literal
    // string "&lt;" and must not become "<" — a second pass would resurrect
    // markup out of text the sender deliberately escaped.
    void entityDecodingDoesNotRunTwice()
    {
        QCOMPARE(codeAt(segments(QStringLiteral(
                            "<pre><code>&amp;lt;b&amp;gt;</code></pre>")), 0),
                 QStringLiteral("&lt;b&gt;"));
    }

    // A REAL (unescaped) drop-with-content element inside a <pre> is still
    // dropped with its content: a <script> body is not source the sender
    // asked us to display.
    void dropWithContentStillDropsInsideACodeBlock()
    {
        QCOMPARE(codeAt(segments(QStringLiteral(
                            "<pre>a<script>bad()</script>b</pre>")), 0),
                 QStringLiteral("ab"));
    }

    // Senders emit one <br> per line inside a <pre>; every other tag in
    // there is markup noise whose text still flows.
    void brInsideACodeBlockBecomesANewlineAndOtherTagsAreDropped()
    {
        QCOMPARE(codeAt(segments(QStringLiteral(
                            "<pre>one<br>t<b>w</b>o<br/>three</pre>")), 0),
                 QStringLiteral("one\ntwo\nthree"));
    }

    void textBeforeBetweenAndAfterBlocksBecomesRichTextInOrder()
    {
        const auto segs = segments(QStringLiteral(
            "before<pre><code>one</code></pre>"
            "between<pre><code>two</code></pre>after"));
        QCOMPARE(segs.size(), 5);
        QVERIFY(segs.at(0).kind == MessageHtml::SegmentKind::RichText);
        QCOMPARE(segs.at(0).text, QStringLiteral("before"));
        QVERIFY(segs.at(1).kind == MessageHtml::SegmentKind::CodeBlock);
        QCOMPARE(segs.at(1).text, QStringLiteral("one"));
        QCOMPARE(segs.at(2).text, QStringLiteral("between"));
        QVERIFY(segs.at(3).kind == MessageHtml::SegmentKind::CodeBlock);
        QCOMPARE(segs.at(3).text, QStringLiteral("two"));
        QCOMPARE(segs.at(4).text, QStringLiteral("after"));
    }

    // Two blocks with nothing but layout markup between them must NOT
    // produce an empty rich-text segment (which would render as a blank line
    // between the two cards).
    void adjacentBlocksDoNotEmitAnEmptyRichTextSegmentBetweenThem()
    {
        const auto segs = segments(QStringLiteral(
            "<pre><code>one</code></pre>\n<p> </p>\n"
            "<pre><code>two</code></pre>"));
        QCOMPARE(segs.size(), 2);
        QCOMPARE(countOf(segs, MessageHtml::SegmentKind::CodeBlock), 2);
    }

    // Inline <code> that is NOT inside a <pre> stays inline in the rich-text
    // segment and keeps its existing rendering, even when a fenced block sits
    // immediately next to it.
    void inlineCodeAdjacentToAFencedBlockStaysInline()
    {
        const auto segs = segments(QStringLiteral(
            "try <code>ls -la</code> then:"
            "<pre><code>ls -la /tmp</code></pre>"
            "and <code>cd</code> back"));
        QCOMPARE(segs.size(), 3);
        QVERIFY(segs.at(0).kind == MessageHtml::SegmentKind::RichText);
        QVERIFY(segs.at(0).text.contains(QStringLiteral("<code>ls -la</code>")));
        QVERIFY(segs.at(1).kind == MessageHtml::SegmentKind::CodeBlock);
        QCOMPARE(segs.at(1).text, QStringLiteral("ls -la /tmp"));
        QVERIFY(segs.at(2).text.contains(QStringLiteral("<code>cd</code>")));
    }

    // A bare <pre> with no <code> child is still exactly one block.
    void barePreIsOneBlock()
    {
        const auto segs = segments(QStringLiteral("<pre>plain\nblock</pre>"));
        QCOMPARE(segs.size(), 1);
        QVERIFY(segs.at(0).kind == MessageHtml::SegmentKind::CodeBlock);
        QCOMPARE(segs.at(0).text, QStringLiteral("plain\nblock"));
        QVERIFY(segs.at(0).language.isEmpty());
    }

    // A <pre> inside dropped content is not a code block at all — nothing in
    // there is rendered, so the fast path must stay on the fast path.
    void preInsideDroppedContentIsNotACodeBlock()
    {
        const QString html = QStringLiteral(
            "<mx-reply><blockquote><pre>old code</pre></blockquote>"
            "</mx-reply>real");
        const auto segs = segments(html);
        QCOMPARE(segs.size(), 1);
        QVERIFY(segs.at(0).kind == MessageHtml::SegmentKind::RichText);
        QCOMPARE(segs.at(0).text, sanitize(html));
        QVERIFY(!segs.at(0).text.contains(QStringLiteral("old code")));
    }

    // ---- language token grammar (a security boundary) -----------------

    void validLanguageTokensAreAccepted()
    {
        QCOMPARE(languageAt(segments(QStringLiteral(
                     "<pre><code class=\"language-rust\">fn main(){}"
                     "</code></pre>")), 0),
                 QStringLiteral("rust"));
        // The class may sit on the <pre> itself.
        QCOMPARE(languageAt(segments(QStringLiteral(
                     "<pre class=\"language-go\">package main</pre>")), 0),
                 QStringLiteral("go"));
        // `lang-` prefix, and the punctuation the grammar allows.
        QCOMPARE(languageAt(segments(QStringLiteral(
                     "<pre><code class=\"lang-c++\">x</code></pre>")), 0),
                 QStringLiteral("c++"));
        QCOMPARE(languageAt(segments(QStringLiteral(
                     "<pre><code class=\"highlight language-objective-c\">x"
                     "</code></pre>")), 0),
                 QStringLiteral("objective-c"));
    }

    void hostileOrOversizedClassesYieldNoLanguage()
    {
        // An event-handler-shaped class: refused twice over — it carries no
        // language- prefix, and '=' is not in the token grammar.
        QVERIFY(languageAt(segments(QStringLiteral(
                    "<pre><code class=\"onclick=x\">x</code></pre>")), 0)
                    .isEmpty());
        QVERIFY(languageAt(segments(QStringLiteral(
                    "<pre><code class=\"language-onclick=x\">x</code></pre>")),
                    0).isEmpty());
        // 40 characters: past the 24-char bound.
        const QString long40(40, QLatin1Char('a'));
        QVERIFY(languageAt(segments(
                    QStringLiteral("<pre><code class=\"language-") + long40
                    + QStringLiteral("\">x</code></pre>")), 0).isEmpty());
        // A quote inside the value (single-quoted attribute) must not escape
        // into the label.
        QVERIFY(languageAt(segments(QStringLiteral(
                    "<pre><code class='language-ru\"st'>x</code></pre>")), 0)
                    .isEmpty());
        // A suffix attribute must not be read as the class.
        QVERIFY(languageAt(segments(QStringLiteral(
                    "<pre><code data-class=\"language-rust\">x</code></pre>")),
                    0).isEmpty());
        // No class at all.
        QVERIFY(languageAt(segments(QStringLiteral(
                    "<pre><code>x</code></pre>")), 0).isEmpty());
    }

    void theRawClassStringNeverLeavesTheParser()
    {
        const auto segs = segments(QStringLiteral(
            "<pre><code class=\"language-rust theme-dracula tabindex\">x"
            "</code></pre>"));
        QCOMPARE(languageAt(segs, 0), QStringLiteral("rust"));
        for (const auto &s : segs) {
            QVERIFY(!s.language.contains(QStringLiteral("theme-dracula")));
            QVERIFY(!s.text.contains(QStringLiteral("theme-dracula")));
        }
    }

    // ---- bounds -------------------------------------------------------

    // At most 64 segments. Past that the parser simply stops emitting; it
    // never grows without bound and never crashes.
    void segmentCountIsBounded()
    {
        QString html;
        for (int i = 0; i < 100; ++i) {
            html += QStringLiteral("t%1").arg(i);
            html += QStringLiteral("<pre><code>c%1</code></pre>").arg(i);
        }
        const auto segs = segments(html);
        QCOMPARE(segs.size(), 64);
    }

    // At most 256 KiB of code text per message. The second block here would
    // cross the line, so it is dropped whole rather than truncated into a
    // program that is not the one that was sent.
    void codeTextVolumeIsBounded()
    {
        const QString first(200000, QLatin1Char('a'));
        const QString second(100000, QLatin1Char('b'));
        const auto segs = segments(
            QStringLiteral("<pre><code>") + first
            + QStringLiteral("</code></pre><pre><code>") + second
            + QStringLiteral("</code></pre>"));
        QCOMPARE(segs.size(), 1);
        QCOMPARE(codeAt(segs, 0).size(), 200000);
    }

    // Malformed markup degrades into fewer segments, never into a crash or
    // an unbounded loop.
    void malformedCodeMarkupFailsClosed()
    {
        // Unclosed <pre>: the sender's markup ran out, the code did not.
        QCOMPARE(codeAt(segments(QStringLiteral("<pre><code>one\ntwo")), 0),
                 QStringLiteral("one\ntwo"));
        // A stray </pre> outside any block is noise.
        const auto stray = segments(QStringLiteral("hi</pre>there"));
        QCOMPARE(stray.size(), 1);
        QVERIFY(stray.at(0).kind == MessageHtml::SegmentKind::RichText);
        // A nested <pre> is not a second block.
        const auto nested =
            segments(QStringLiteral("<pre>a<pre>b</pre>c</pre>"));
        QCOMPARE(countOf(nested, MessageHtml::SegmentKind::CodeBlock), 1);
        QCOMPARE(codeAt(nested, 0), QStringLiteral("abc"));
        // An empty block emits nothing at all.
        QCOMPARE(segments(QStringLiteral("<pre><code>  </code></pre>")).size(),
                 0);
    }
};

QTEST_APPLESS_MAIN(MessageHtmlTest)
#include "MessageHtmlTest.moc"
