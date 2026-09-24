// MessageHtml, the sanitizer for incoming Matrix formatted bodies: the
// allowlist is fail-closed (dangerous tags, attributes and schemes are
// stripped), matrix.to mentions become internal "mention:" links with resolved
// names, and ordinary formatting and http(s) links survive.
//
// Also code-block segmentation (MessageHtml::segments): terminal transcripts,
// very long lines, many lines, significant whitespace, non-Latin and emoji,
// HTML-looking text that must stay literal, interleaved prose, the
// segment/byte bounds, and the language token grammar. The last is a security
// boundary: `class` is sender-chosen, so only a validated token may reach a
// label or an accessible name.

#include "models/MessageHtml.h"

#include <QElapsedTimer>
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

// The text of the n-th CodeBlock segment, or a null string when there is none
// (so a failure reports "empty" instead of aborting on the index).
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

    // Spoilers (data-mx-spoiler) render covered by default.

    void aCoveredSpoilerIsASolidSlabBehindItsOwnToggleAnchor()
    {
        const MessageHtml::MentionStyle style{
            QString(), QString(), QStringLiteral("#22262e")};
        const QString out = MessageHtml::sanitize(
            QStringLiteral("<span data-mx-spoiler>the twist</span>"),
            nullptr, QString(), style, /*revealSpoilers=*/false);
        // The internal toggle anchor, never a browser target.
        QVERIFY(out.contains(QStringLiteral("href=\"spoiler:toggle\"")));
        // Cover = background and text in the same ink: the run is a slab.
        QVERIFY(out.contains(
            QStringLiteral("background-color:#22262e;color:#22262e")));
        // The text is still present (selection copies it, as in Element);
        // the ink hides it.
        QVERIFY(out.contains(QStringLiteral("the twist")));
        QVERIFY(out.contains(QStringLiteral("</span></a>")));
    }

    void aRevealedSpoilerKeepsTheBackgroundButShowsTheText()
    {
        const MessageHtml::MentionStyle style{
            QString(), QString(), QStringLiteral("#22262e")};
        const QString out = MessageHtml::sanitize(
            QStringLiteral("<span data-mx-spoiler=\"reason\">t</span>"),
            nullptr, QString(), style, /*revealSpoilers=*/true);
        QVERIFY(out.contains(QStringLiteral("href=\"spoiler:toggle\"")));
        QVERIFY(out.contains(QStringLiteral("background-color:#22262e")));
        // Revealed: the text ink is not the cover ink.
        QVERIFY(!out.contains(
            QStringLiteral("background-color:#22262e;color:#22262e")));
    }

    void aPlainSpanBesideASpoilerCLosesItsOwnTagOnly()
    {
        const MessageHtml::MentionStyle style{
            QString(), QString(), QStringLiteral("#22262e")};
        const QString out = MessageHtml::sanitize(
            QStringLiteral(
                "<span>a</span><span data-mx-spoiler>b</span><span>c</span>"),
            nullptr, QString(), style, false);
        // Exactly one anchor pair, wrapped around the middle span only.
        QCOMPARE(out.count(QStringLiteral("<a ")), 1);
        QCOMPARE(out.count(QStringLiteral("</a>")), 1);
        QVERIFY(out.indexOf(QStringLiteral("</a>"))
                < out.indexOf(QStringLiteral("<span>c")));
    }

    void anUnclosedSpoilerSpanIsClosedAtTheEnd()
    {
        const MessageHtml::MentionStyle style{
            QString(), QString(), QStringLiteral("#22262e")};
        const QString out = MessageHtml::sanitize(
            QStringLiteral("<span data-mx-spoiler>never closed"),
            nullptr, QString(), style, false);
        QVERIFY(out.endsWith(QStringLiteral("</span></a>")));
    }

    void spoilerAttributeVariantsAreRecognisedAndFakesAreNot()
    {
        const MessageHtml::MentionStyle style{
            QString(), QString(), QStringLiteral("#22262e")};
        const auto covered = [&](const QString &html) {
            return MessageHtml::sanitize(html, nullptr, QString(), style,
                                         false)
                .contains(QStringLiteral("spoiler:toggle"));
        };
        QVERIFY(covered(QStringLiteral("<span data-mx-spoiler>x</span>")));
        QVERIFY(covered(
            QStringLiteral("<span data-mx-spoiler=\"why\">x</span>")));
        QVERIFY(covered(
            QStringLiteral("<span DATA-MX-SPOILER>x</span>")));
        // A lookalike attribute must not become a spoiler.
        QVERIFY(!covered(
            QStringLiteral("<span data-mx-spoilerish>x</span>")));
        QVERIFY(!covered(
            QStringLiteral("<span notdata-mx-spoiler=\"1\">x</span>")));
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

    // MSC2545 inline custom emoji are the one image this sanitizer emits: only
    // when marked as an emoticon and addressed by `mxc:`, which can only be
    // fetched through the authenticated media path.
    void onlyMarkedMxcEmoticonsSurvive()
    {
        // Self-closing, as MSC2545's example and real senders use.
        const QString kept = sanitize(QStringLiteral(
            "a <img data-mx-emoticon src=\"mxc://e.org/blob\" "
            "alt=\":blob:\" title=\":blob:\" height=\"32\" /> b"));
        QVERIFY2(kept.contains(QStringLiteral("data-mx-emoticon")),
                 qPrintable(kept));
        QVERIFY2(kept.contains(QStringLiteral("mxc://e.org/blob")),
                 qPrintable(kept));
        QVERIFY2(kept.contains(QStringLiteral("alt=\":blob:\"")),
                 qPrintable(kept));
        // The sender's height is not honoured: remote input must not decide
        // how much space one glyph takes.
        QVERIFY2(!kept.contains(QStringLiteral("height=\"32\"")),
                 qPrintable(kept));

        for (const QString &hostile : {
                 // Unmarked: not an emoticon, not rendered.
                 QStringLiteral("<img src=\"mxc://e.org/blob\">"),
                 // Marked but remote: a tracking pixel.
                 QStringLiteral("<img data-mx-emoticon src=\"https://t/x.png\">"),
                 QStringLiteral("<img data-mx-emoticon src=\"http://t/x.png\">"),
                 QStringLiteral("<img data-mx-emoticon src=\"//t/x.png\">"),
                 QStringLiteral("<img data-mx-emoticon src=\"javascript:e()\">"),
                 QStringLiteral("<img data-mx-emoticon src=\"data:image/png;base64,AA\">"),
                 QStringLiteral("<img data-mx-emoticon src=\"file:///etc/passwd\">"),
                 // An mxc with no media id addresses nothing.
                 QStringLiteral("<img data-mx-emoticon src=\"mxc://\">"),
                 // No source at all.
                 QStringLiteral("<img data-mx-emoticon alt=\":x:\">"),
             }) {
            const QString out = sanitize(hostile);
            QVERIFY2(!out.contains(QStringLiteral("<img")), qPrintable(out));
        }

        // Event handlers and stray attributes never survive: the tag is
        // rebuilt from validated parts, not filtered.
        const QString rebuilt = sanitize(QStringLiteral(
            "<img data-mx-emoticon src=\"mxc://e.org/b\" onerror=\"e()\" "
            "onload=\"e()\" style=\"position:fixed\" class=\"x\">"));
        QVERIFY2(!rebuilt.contains(QStringLiteral("onerror")), qPrintable(rebuilt));
        QVERIFY2(!rebuilt.contains(QStringLiteral("onload")), qPrintable(rebuilt));
        QVERIFY2(!rebuilt.contains(QStringLiteral("style")), qPrintable(rebuilt));
        QVERIFY2(!rebuilt.contains(QStringLiteral("class")), qPrintable(rebuilt));
    }

    // resolveInlineImages() is a separate render-only step: sanitize() keeps
    // the mxc, so the edit path never sends a local image:// URL.
    void resolvingAnEmojiNeverChangesWhatAnEditWouldSend()
    {
        const QString safe = sanitize(QStringLiteral(
            "x <img data-mx-emoticon src=\"mxc://e.org/b\" alt=\":b:\"> y"));
        QVERIFY(safe.contains(QStringLiteral("mxc://e.org/b")));

        const QString rendered = MessageHtml::resolveInlineImages(
            safe, [](const QString &mxc) {
                return QStringLiteral("image://lightning-media/") + mxc;
            });
        QVERIFY(rendered.contains(QStringLiteral("image://lightning-media/")));
        QVERIFY2(!rendered.contains(QStringLiteral("data-mx-emoticon src=\"mxc")),
                 qPrintable(rendered));
        // The input is untouched, so the memo the edit path reads keeps the
        // mxc form.
        QVERIFY(safe.contains(QStringLiteral("mxc://e.org/b")));

        // Media not cached yet: the shortcode is shown, not a broken image.
        const QString pending = MessageHtml::resolveInlineImages(
            safe, [](const QString &) { return QString(); });
        QVERIFY2(!pending.contains(QStringLiteral("<img")), qPrintable(pending));
        QVERIFY2(pending.contains(QStringLiteral(":b:")), qPrintable(pending));
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

    // The label the sender wrote inside a mention anchor never names the pill:
    // "@admin" linking to @attacker:evil must read as @attacker. Names come
    // from the room/profile resolver.
    void theSendersAnchorLabelNeverNamesThePill()
    {
        const QString out = sanitize(QStringLiteral(
            "<a href=\"https://matrix.to/#/@attacker:evil.org\">@admin</a> hi"));
        QCOMPARE(out, QStringLiteral(
            "<a href=\"mention:@attacker:evil.org\">@attacker</a> hi"));
    }

    void resolvedNameWinsOverTheAnchorText()
    {
        const QString out = MessageHtml::sanitize(
            QStringLiteral(
                "<a href=\"https://matrix.to/#/@bob:example.org\">@old name</a>"),
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

    void mentionStyleIsInkAndWeightNotABox()
    {
        // With theme ink the mention is coloured and semibold, with the anchor
        // underline suppressed and no background: Qt paints an inline
        // background as an unrounded full-line-height slab.
        const QString out = MessageHtml::sanitize(
            QStringLiteral(
                "<a href=\"https://matrix.to/#/@bob:example.org\">bob</a>"),
            nullptr, QString(),
            MessageHtml::MentionStyle{QStringLiteral("#7c7ff2"),
                                      QStringLiteral("#9295f5")});
        QCOMPARE(out, QStringLiteral(
            "<a href=\"mention:@bob:example.org\" "
            "style=\"color:#9295f5;font-weight:600;"
            "text-decoration:none\">@bob</a>"));
        QVERIFY(!out.contains(QStringLiteral("background-color")));
    }

    void theAccentIsSpentOnlyOnAMentionOfYou()
    {
        // Other users' mentions take the link ink, keeping the accent
        // meaningful.
        const MessageHtml::MentionStyle style{QStringLiteral("#ffd447"),
                                              QStringLiteral("#9295f5")};
        const QString me = MessageHtml::sanitize(
            QStringLiteral(
                "<a href=\"https://matrix.to/#/@me:example.org\">me</a>"),
            nullptr, QStringLiteral("@me:example.org"), style);
        QVERIFY(me.contains(QStringLiteral("color:#ffd447")));
        QVERIFY(me.contains(QStringLiteral("<b>@me</b>")));

        const QString them = MessageHtml::sanitize(
            QStringLiteral(
                "<a href=\"https://matrix.to/#/@bob:example.org\">bob</a>"),
            nullptr, QStringLiteral("@me:example.org"), style);
        QVERIFY(them.contains(QStringLiteral("color:#9295f5")));
        QVERIFY(!them.contains(QStringLiteral("#ffd447")));
    }

    void mentionInkFallsBackToTheAccentWhenNoLinkInkIsPushed()
    {
        // A theme pushing only one ink still renders legibly, not in Qt's
        // default link blue.
        const QString out = MessageHtml::sanitize(
            QStringLiteral(
                "<a href=\"https://matrix.to/#/@bob:example.org\">bob</a>"),
            nullptr, QString(),
            MessageHtml::MentionStyle{QStringLiteral("#7c7ff2")});
        QVERIFY(out.contains(QStringLiteral("color:#7c7ff2")));
    }

    void externalLinksCarryTheThemeInk()
    {
        // Message links get the link ink instead of Qt's default #0000ff; the
        // underline stays, since it separates a URL from a mention.
        const QString out = MessageHtml::sanitize(
            QStringLiteral("see <a href=\"https://matrix.org/\">spec</a>"),
            nullptr, QString(),
            MessageHtml::MentionStyle{QStringLiteral("#ffd447"),
                                      QStringLiteral("#9295f5")});
        QCOMPARE(out, QStringLiteral(
            "see <a href=\"https://matrix.org/\" "
            "style=\"color:#9295f5\">spec</a>"));
        QVERIFY(!out.contains(QStringLiteral("text-decoration")));
    }

    void unstyledBodiesAreUnchangedByTheInkPath()
    {
        // No theme pushed yet (cold start): anchors come back unchanged, with
        // no empty style attribute.
        const QString out = MessageHtml::sanitize(
            QStringLiteral("see <a href=\"https://matrix.org/\">spec</a> and "
                           "<a href=\"https://matrix.to/#/@bob:e.org\">b</a>"),
            nullptr, QString());
        QCOMPARE(out, QStringLiteral(
            "see <a href=\"https://matrix.org/\">spec</a> and "
            "<a href=\"mention:@bob:e.org\">@bob</a>"));
    }

    void mentionStyleCannotBreakOutOfTheAttribute()
    {
        // Hostile "colors" are escaped (the model also validates opaque hex
        // first), on both inks.
        const QString mention = MessageHtml::sanitize(
            QStringLiteral(
                "<a href=\"https://matrix.to/#/@bob:example.org\">bob</a>"),
            nullptr, QString(),
            MessageHtml::MentionStyle{
                QStringLiteral("\"><script>bad</script>"),
                QStringLiteral("\"><script>bad</script>")});
        QVERIFY(!mention.contains(QStringLiteral("<script")));

        const QString link = MessageHtml::sanitize(
            QStringLiteral("<a href=\"https://matrix.org/\">spec</a>"),
            nullptr, QString(),
            MessageHtml::MentionStyle{
                QStringLiteral("#ffd447"),
                QStringLiteral("\"><script>bad</script>")});
        QVERIFY(!link.contains(QStringLiteral("<script")));
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

    // Code-block segmentation.
    //
    // An ordinary message takes the fast path and is byte-identical to
    // sanitize(), so rendering never depends on which entry point was used.
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

    // A `dig` transcript: every line survives, in order, as one plain-text
    // block, with no markup and no re-escaping.
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
        // Entities decoded exactly once: the angle brackets are literal.
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

    // One line far wider than any pane arrives whole (the renderer scrolls
    // it), neither truncated nor wrapped by the parser.
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

    // Indentation is significant (Python/YAML): tabs, space runs and &nbsp;
    // padding reach the renderer unchanged.
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
        // &nbsp; becomes a real space, which a monospace layout treats as a
        // column.
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

    // A numeric entity for an astral code point decodes to one character, not
    // a lone surrogate or replacement glyph.
    void numericEntitiesDecodeToRealCharacters()
    {
        const QString code = codeAt(
            segments(QStringLiteral(
                "<pre><code>a&#128512;b&#x41;c</code></pre>")), 0);
        QCOMPARE(code, QStringLiteral("a\U0001F600bAc"));
    }

    // HTML-shaped text inside a code block arrives decoded and is rendered as
    // PlainText, so it never becomes markup again.
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

    // Decoding is one pass: "&amp;lt;" displays "&lt;", never "<".
    void entityDecodingDoesNotRunTwice()
    {
        QCOMPARE(codeAt(segments(QStringLiteral(
                            "<pre><code>&amp;lt;b&amp;gt;</code></pre>")), 0),
                 QStringLiteral("&lt;b&gt;"));
    }

    // A real drop-with-content element inside a <pre> (e.g. <script>) is still
    // dropped with its content.
    void dropWithContentStillDropsInsideACodeBlock()
    {
        QCOMPARE(codeAt(segments(QStringLiteral(
                            "<pre>a<script>bad()</script>b</pre>")), 0),
                 QStringLiteral("ab"));
    }

    // Senders emit one <br> per line inside a <pre>; other tags there are
    // noise whose text still flows.
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

    // Two blocks separated only by layout markup produce no empty rich-text
    // segment (a blank line between the cards).
    void adjacentBlocksDoNotEmitAnEmptyRichTextSegmentBetweenThem()
    {
        const auto segs = segments(QStringLiteral(
            "<pre><code>one</code></pre>\n<p> </p>\n"
            "<pre><code>two</code></pre>"));
        QCOMPARE(segs.size(), 2);
        QCOMPARE(countOf(segs, MessageHtml::SegmentKind::CodeBlock), 2);
    }

    // Inline <code> outside a <pre> stays inline in the rich-text segment,
    // even next to a fenced block.
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

    // A <pre> inside dropped content is not a code block, so the fast path
    // applies.
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

    // Language token grammar (a security boundary).

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
        // An event-handler-shaped class: no language- prefix, and '=' is not
        // in the grammar.
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
        // A quote inside a single-quoted value must not escape into the label.
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

    // Bounds.
    //
    // At most 64 segments; past that the parser stops emitting.
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

    // At most 256 KiB of code text per message: the block that would cross the
    // limit is dropped whole, not truncated into a different program.
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

    // Malformed markup degrades into fewer segments, never a crash or an
    // unbounded loop.
    void malformedCodeMarkupFailsClosed()
    {
        // Unclosed <pre>: the markup ran out, the code did not.
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

    // Tag scanning is linear: a body of "<a" repeated used to cost O(N^2)
    // character reads on the GUI thread (in TimelineModel::data()), from a
    // message anyone can send. The output never changed, so the case measures
    // time. Measured: unfixed ~24.7 s at -O2, fixed ~60 ms at -O0; the 1500 ms
    // budget is ~24x above the fixed cost and ~16x below the unfixed one.
    void aTagStormWithNoClosingBracketIsScannedInLinearTime()
    {
        // "<pre" passes containsCodeBlock's cheap reject so the real scan
        // runs; with no '>' anywhere, every later '<' is a literal.
        const QString hostile =
            QStringLiteral("<pre") + QStringLiteral("<a").repeated(600000);
        QElapsedTimer clock;
        clock.start();
        const auto segs = segments(hostile);
        const qint64 ms = clock.elapsed();
        QVERIFY2(ms < 1500,
                 qPrintable(QStringLiteral("tag storm took %1 ms — the scan "
                                           "is quadratic again").arg(ms)));
        // Still a message: one RichText segment, no code block.
        QCOMPARE(countOf(segs, MessageHtml::SegmentKind::CodeBlock), 0);
    }

    // The other shape: a '>' that exists but is far away. "</" passes the
    // tag-start rule and then fails to parse, so each attempt must not rescan
    // (or copy) to that distant '>'.
    void aTagStormWithADistantClosingBracketIsAlsoLinear()
    {
        const QString hostile = QStringLiteral("<script><pre></script>")
            + QStringLiteral("</").repeated(300000) + QStringLiteral(">");
        QElapsedTimer clock;
        clock.start();
        const auto segs = segments(hostile);
        const qint64 ms = clock.elapsed();
        QVERIFY2(ms < 1500,
                 qPrintable(QStringLiteral("closing-tag storm took %1 ms — "
                                           "the scan is quadratic again")
                                .arg(ms)));
        // The only <pre> is inside dropped content, so there is no block.
        QCOMPARE(countOf(segs, MessageHtml::SegmentKind::CodeBlock), 0);
    }

    // The linear scan renders these shapes exactly as before: each literal '<'
    // is escaped and scanning continues.
    void malformedTagShapesKeepTheirExactTextOutput()
    {
        QCOMPARE(sanitize(QStringLiteral("<a<a<a")),
                 QStringLiteral("&lt;a&lt;a&lt;a"));
        QCOMPARE(sanitize(QStringLiteral("</ >")), QStringLiteral("&lt;/ >"));
        QCOMPARE(sanitize(QStringLiteral("</></>")),
                 QStringLiteral("&lt;/>&lt;/>"));
        // A later '>' does not make an earlier '<' a tag retroactively; the
        // whole run is one tag once one exists.
        QCOMPARE(sanitize(QStringLiteral("<a<b>x")), QStringLiteral("x"));
        // "< " is text, not a broken tag.
        QCOMPARE(sanitize(QStringLiteral("a < b > c")),
                 QStringLiteral("a &lt; b > c"));
        // A real tag after a storm of literals is still parsed.
        QCOMPARE(sanitize(QStringLiteral("</</<b>hi</b>")),
                 QStringLiteral("&lt;/&lt;/<b>hi</b>"));
    }

    // An ordered list keeps its `start` number: markdown generators continue a
    // list split by nested bullets with `start`, and stripping it restarts
    // every continuation at 1.
    void anOrderedListKeepsTheNumberItContinuesFrom()
    {
        const QString out = sanitize(QStringLiteral(
            "<ol><li>one</li></ol><ul><li>sub</li></ul>"
            "<ol start=\"2\"><li>two</li></ol>"));
        QVERIFY2(out.contains(QStringLiteral("<ol start=\"2\">")),
                 qPrintable(QStringLiteral(
                     "the continuation lost its start, so it renders as 1 "
                     "again: %1").arg(out)));
        // A list starting at one carries no attribute.
        QVERIFY(out.startsWith(QStringLiteral("<ol>")));

        // Not a plain bounded positive number: the attribute is dropped, never
        // echoed.
        for (const QString &bad : { QStringLiteral("-4"),
                                    QStringLiteral("0"),
                                    QStringLiteral("99999999999"),
                                    QStringLiteral("1e3"),
                                    QStringLiteral(""),
                                    QStringLiteral("abc") }) {
            const QString got = sanitize(
                QStringLiteral("<ol start=\"%1\"><li>x</li></ol>").arg(bad));
            QVERIFY2(got.startsWith(QStringLiteral("<ol>"))
                         && !got.contains(QStringLiteral("start=")),
                     qPrintable(QStringLiteral("accepted start=%1 -> %2")
                                    .arg(bad, got)));
        }

        // A valid value is re-emitted from the parsed integer, so nothing the
        // sender attached to it survives.
        const QString hostile = sanitize(QStringLiteral(
            "<ol start=\"2\" onclick=\"evil()\" style=\"x\"><li>x</li></ol>"));
        QCOMPARE(hostile, QStringLiteral("<ol start=\"2\"><li>x</li></ol>"));
    }

    // @room: a whole-room mention has no matrix.to link, so it is inked as a
    // mention without becoming an anchor.
    void roomMentionIsInkedWithoutBecomingALink()
    {
        const QString ink = QStringLiteral("#ff8800");
        const QString out =
            MessageHtml::markRoomMention(QStringLiteral("hey @room look"), ink);
        QVERIFY(out.contains(QStringLiteral("color:#ff8800")));
        QVERIFY(out.contains(QStringLiteral("font-weight:600")));
        QVERIFY(out.contains(QStringLiteral("@room")));
        // A span, never an anchor: there is no profile behind @room.
        QVERIFY(!out.contains(QStringLiteral("<a ")));
        QVERIFY(!out.contains(QStringLiteral("href")));
        // The surrounding text survives intact.
        QVERIFY(out.startsWith(QStringLiteral("hey ")));
        QVERIFY(out.endsWith(QStringLiteral(" look")));
        // No ink configured means no styling, not a broken span.
        QCOMPARE(MessageHtml::markRoomMention(QStringLiteral("@room"),
                                              QString()),
                 QStringLiteral("@room"));
    }

    void roomMentionNeedsWordBoundariesAndSkipsMarkupAndCode()
    {
        const QString ink = QStringLiteral("#ff8800");
        const auto mark = [&ink](const QString &s) {
            return MessageHtml::markRoomMention(s, ink);
        };
        // Not a mention: a longer word, or an address that merely ends there.
        QCOMPARE(mark(QStringLiteral("@roomba")), QStringLiteral("@roomba"));
        QCOMPARE(mark(QStringLiteral("bot@room.example")),
                 QStringLiteral("bot@room.example"));
        // Never inside a tag: a naive replace would hit this attribute.
        const QString tag =
            QStringLiteral("<a href=\"https://x/@room\">click</a> @room");
        const QString marked = mark(tag);
        QVERIFY(marked.contains(QStringLiteral("href=\"https://x/@room\"")));
        QCOMPARE(marked.count(QStringLiteral("<span")), 1);
        // Entities stay atomic.
        QVERIFY(mark(QStringLiteral("&amp; @room")).contains(
            QStringLiteral("&amp;")));
        // A literal @room in code is a string, not a ping.
        QCOMPARE(mark(QStringLiteral("<code>@room</code>")),
                 QStringLiteral("<code>@room</code>"));
        QCOMPARE(mark(QStringLiteral("<pre>@room</pre>")),
                 QStringLiteral("<pre>@room</pre>"));
        // ...but one after the block closes still counts.
        QVERIFY(mark(QStringLiteral("<code>@room</code> @room"))
                    .contains(QStringLiteral("<span")));
    }
};

QTEST_APPLESS_MAIN(MessageHtmlTest)
#include "MessageHtmlTest.moc"
