// Inline emoji sizing: MessageHtml::markEmoji.
//
// The report was "an emoji in a sentence reads as a character, not a picture".
// Measured against the reference client's screenshot, its inline emoji is
// ~1.7x its text where Lightning's was ~1.0x. Qt's rich-text engine ignores
// `em` and `%` in a font-size, and MessageHtml is not told what pixel size it
// is writing for, so the only scale-RELATIVE lever is the CSS keyword ladder
// (0.7/0.8/1.0/1.2/1.5/2.0/2.4) and `x-large` is its 1.5 rung.
//
// These cases assert BEHAVIOUR — what comes out for a given body — not the
// presence of source text. Each one was checked against a deliberately broken
// build of the same tree (see the round notes): removing the suppression
// fails emojiOnlyBodyIsLeftToTheBigEmojiPath, removing the code-span tracking
// fails emojiInsideCodeKeepsItsSize, and dropping the whitespace break fails
// whitespaceEndsARunRatherThanJoiningIt.
#include "models/MessageHtml.h"

#include <QObject>
#include <QString>
#include <QTest>

namespace {

const QString kOpen = QStringLiteral("<span style=\"font-size:x-large\">");
const QString kClose = QStringLiteral("</span>");

int spanCount(const QString &html)
{
    int n = 0;
    qsizetype at = 0;
    while ((at = html.indexOf(kOpen, at)) >= 0) {
        ++n;
        at += kOpen.size();
    }
    return n;
}

// The text a span wraps, for span number `index` (0-based).
QString wrapped(const QString &html, int index = 0)
{
    qsizetype at = -1;
    for (int i = 0; i <= index; ++i) {
        at = html.indexOf(kOpen, at + 1);
        if (at < 0)
            return {};
    }
    const qsizetype from = at + kOpen.size();
    const qsizetype to = html.indexOf(kClose, from);
    return to < 0 ? QString() : html.mid(from, to - from);
}

QString u(char32_t cp) { return QString::fromUcs4(&cp, 1); }

const QString kSmile = u(0x1F60A);      // U+1F60A SMILING FACE
const QString kThumb = u(0x1F44D);      // U+1F44D THUMBS UP
const QString kZwj = QStringLiteral("‍");
const QString kVs16 = QStringLiteral("️");

} // namespace

class MessageEmojiSizeTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:

    // The report itself: an emoji inside a sentence comes out enlarged, and
    // ONLY the emoji does.
    void emojiInASentenceIsEnlarged()
    {
        const QString in = QStringLiteral("i dont have any downloaded ") + kSmile
            + QStringLiteral(", I try to get away");
        const QString out = MessageHtml::markEmoji(in);
        QCOMPARE(spanCount(out), 1);
        QCOMPARE(wrapped(out), kSmile);
        // The words are untouched, in order, exactly once.
        QVERIFY(out.contains(QStringLiteral("i dont have any downloaded ")
                             + kOpen));
        QVERIFY(out.contains(kClose + QStringLiteral(", I try to get away")));
    }

    // The size is a constant of ours and it is the 1.5 rung. If this changes,
    // the change was deliberate and the header's measurement has to change
    // with it.
    void theSizeIsTheScaleRelativeKeywordNotAPixelValue()
    {
        const QString out =
            MessageHtml::markEmoji(QStringLiteral("hi ") + kSmile);
        QVERIFY(out.contains(QStringLiteral("font-size:x-large")));
        // A pixel size would freeze the emoji against the text-size slider,
        // and `em`/`%` are silently ignored by Qt's CSS parser.
        QVERIFY(!out.contains(QStringLiteral("px")));
        QVERIFY(!out.contains(QStringLiteral("em\"")));
        QVERIFY(!out.contains(QStringLiteral("%")));
    }

    // A body of 1-3 emoji and nothing else is the big-emoji row: the delegate
    // already renders it at 48/60 px. Enlarging it again would take it past
    // 90 px, which is the "do not double-apply" rule.
    void emojiOnlyBodyIsLeftToTheBigEmojiPath()
    {
        const QStringList bodies = {
            kSmile,
            kSmile + kSmile,
            kSmile + kSmile + kSmile,
            QStringLiteral("  ") + kSmile + QStringLiteral(" ") + kThumb
                + QStringLiteral("  "),
        };
        for (const QString &body : bodies)
            QCOMPARE(MessageHtml::markEmoji(body), body);
    }

    // Four is past the big-emoji window, so the delegate renders it at body
    // size and this is the only thing that will enlarge it.
    void fourEmojiIsNotABigEmojiRowAndIsEnlarged()
    {
        const QString body = kSmile + kSmile + kSmile + kSmile;
        const QString out = MessageHtml::markEmoji(body);
        QVERIFY(out != body);
        QCOMPARE(spanCount(out), 1); // one run, four adjacent clusters
        QCOMPARE(wrapped(out), body);
    }

    // Non-Latin text is not emoji. Kana and CJK sit deliberately outside the
    // "could this body be emoji-only" band, or a three-character Japanese
    // message would be mistaken for a big-emoji row and never enlarged.
    void aShortNonLatinMessageIsStillEnlarged()
    {
        const QString body = QStringLiteral("こん") + kSmile;
        const QString out = MessageHtml::markEmoji(body);
        QCOMPARE(spanCount(out), 1);
        QCOMPARE(wrapped(out), kSmile);
    }

    // An emoji in a code sample is a character in a string.
    void emojiInsideCodeKeepsItsSize()
    {
        const QString in = QStringLiteral("<code>print(\"") + kSmile
            + QStringLiteral("\")</code> and ") + kSmile;
        const QString out = MessageHtml::markEmoji(in);
        QCOMPARE(spanCount(out), 1);
        // The one span is the trailing emoji, not the one inside the code.
        QVERIFY(out.indexOf(kOpen) > out.indexOf(QStringLiteral("</code>")));
        QVERIFY(out.contains(QStringLiteral("<code>print(\"") + kSmile
                             + QStringLiteral("\")</code>")));
    }

    void emojiInsideAPreBlockKeepsItsSize()
    {
        const QString in =
            QStringLiteral("<pre>a ") + kSmile + QStringLiteral("</pre>");
        QCOMPARE(MessageHtml::markEmoji(in), in);
    }

    // One picture is one run: a ZWJ family, a flag, a tone variant and a
    // keycap must each be wrapped whole, or half a glyph changes size.
    void oneUserPerceivedEmojiIsOneRun()
    {
        struct Case { QString sequence; const char *what; };
        const QList<Case> cases = {
            { u(0x1F468) + kZwj + u(0x1F469) + kZwj + u(0x1F467), "ZWJ family" },
            { u(0x1F1F1) + u(0x1F1F9), "regional-indicator flag" },
            { u(0x1F44D) + u(0x1F3FD), "skin-tone variant" },
            { QStringLiteral("1") + kVs16 + QStringLiteral("⃣"), "keycap" },
        };
        for (const Case &c : cases) {
            const QString out =
                MessageHtml::markEmoji(QStringLiteral("x ") + c.sequence
                                       + QStringLiteral(" y"));
            QVERIFY2(spanCount(out) == 1, c.what);
            QVERIFY2(wrapped(out) == c.sequence, c.what);
        }
    }

    // U+FE0F is the sender asking for emoji presentation on a base that has
    // none by default. Honour it; do not honour a bare one.
    void aVariationSelectorQualifiesAnEmoji()
    {
        const QString heart = u(0x2764);
        const QString qualified = heart + kVs16;
        const QString out = MessageHtml::markEmoji(QStringLiteral("a ")
                                                   + qualified
                                                   + QStringLiteral(" b"));
        QCOMPARE(spanCount(out), 1);
        QCOMPARE(wrapped(out), qualified);

        // Unqualified: Emoji_Presentation=No, so it is text by default and is
        // deliberately left at text size.
        const QString bare =
            MessageHtml::markEmoji(QStringLiteral("a ") + heart
                                   + QStringLiteral(" b"));
        QCOMPARE(spanCount(bare), 0);
    }

    // Emoji=Yes is not Emoji_Presentation=Yes. Enlarging a digit, a hash or a
    // (c) because a keycap or a legacy symbol exists would be a rendering bug.
    void textPresentationCharactersAreNeverEnlarged()
    {
        const QStringList bodies = {
            QStringLiteral("issue #1 * 2 (c) © ® ™ — x"),
            QStringLiteral("1 2 3 4 5"),
            QStringLiteral("a … b • c"),
        };
        for (const QString &body : bodies)
            QCOMPARE(MessageHtml::markEmoji(body), body);
    }

    // A space at 1.5x is a wider space; the gap between two pictures belongs
    // to neither of them.
    void whitespaceEndsARunRatherThanJoiningIt()
    {
        const QString body = QStringLiteral("hey ") + kSmile
            + QStringLiteral(" ") + kThumb + QStringLiteral(" ok");
        const QString out = MessageHtml::markEmoji(body);
        QCOMPARE(spanCount(out), 2);
        QCOMPARE(wrapped(out, 0), kSmile);
        QCOMPARE(wrapped(out, 1), kThumb);
        QVERIFY(out.contains(kClose + QStringLiteral(" ") + kOpen));
    }

    void adjacentEmojiShareOneRun()
    {
        const QString body =
            QStringLiteral("hey ") + kSmile + kThumb + QStringLiteral(" ok");
        const QString out = MessageHtml::markEmoji(body);
        QCOMPARE(spanCount(out), 1);
        QCOMPARE(wrapped(out), kSmile + kThumb);
    }

    // The input is already-safe markup. Tags are copied verbatim and an
    // entity is atomic: splitting "&amp;" corrupts the markup and can even
    // manufacture a tag.
    void markupAndEntitiesSurviveUntouched()
    {
        const QString in = QStringLiteral("<b>bold ") + kSmile
            + QStringLiteral("</b> &amp; <i>x</i> &lt;script&gt; ") + kThumb;
        const QString out = MessageHtml::markEmoji(in);
        QVERIFY(out.contains(QStringLiteral("<b>bold ")));
        QVERIFY(out.contains(QStringLiteral("</b> &amp; <i>x</i> &lt;script&gt; ")));
        QCOMPARE(spanCount(out), 2);
        QCOMPARE(wrapped(out, 0), kSmile);
        QCOMPARE(wrapped(out, 1), kThumb);
    }

    // A numeric entity is markup for an emoji, not the character; re-encoding
    // it would mean decoding sender text and writing it back out.
    void anEntityEncodedEmojiIsLeftAlone()
    {
        const QString in = QStringLiteral("a &#128522; b");
        QCOMPARE(MessageHtml::markEmoji(in), in);
    }

    // Nothing from the input reaches the style: every byte emitted is one of
    // ours.
    void nothingFromTheBodyReachesTheStyle()
    {
        const QString hostile = QStringLiteral("x\" onload=\"alert(1)\" y=\"")
            + kSmile + QStringLiteral(" ;color:red;");
        const QString out = MessageHtml::markEmoji(hostile);
        QCOMPARE(spanCount(out), 1);
        // The only style declaration in the output is the constant.
        QCOMPARE(out.count(QStringLiteral("style=")), 1);
        QVERIFY(out.contains(kOpen));
        QVERIFY(!out.contains(QStringLiteral("style=\"color:red")));
        // The hostile text is still there, still inert, still escaped exactly
        // as it arrived.
        QVERIFY(out.contains(QStringLiteral("onload=\"alert(1)\"")));
    }

    // A pathological body cannot make the output grow without bound.
    void theNumberOfRunsIsBounded()
    {
        QString body;
        for (int i = 0; i < 900; ++i)
            body += kSmile + QStringLiteral(" ");
        const QString out = MessageHtml::markEmoji(body);
        QVERIFY(spanCount(out) <= 256);
        QVERIFY(out.size() <= body.size() + 256 * (kOpen.size() + kClose.size()));
    }

    // An ASCII body leaves without building a boundary finder, and comes back
    // byte-identical.
    void anAsciiBodyIsReturnedUnchanged()
    {
        const QString body =
            QStringLiteral("<b>hello</b> world, see https://example.com/x?a=1&amp;b=2");
        QCOMPARE(MessageHtml::markEmoji(body), body);
        QCOMPARE(MessageHtml::markEmoji(QString()), QString());
    }

    // Integration: the formatted-body path applies it, so the timeline gets
    // it without a second call site to keep in step.
    void sanitizeAppliesTheSizingToAFormattedBody()
    {
        const QString out = MessageHtml::sanitize(
            QStringLiteral("<b>hi</b> ") + kSmile + QStringLiteral(" there"),
            nullptr, QString(), {});
        QCOMPARE(spanCount(out), 1);
        QCOMPARE(wrapped(out), kSmile);
        QVERIFY(out.contains(QStringLiteral("<b>hi</b> ")));
    }

    void sanitizeLeavesAnEmojiOnlyFormattedBodyAlone()
    {
        const QString out = MessageHtml::sanitize(
            QStringLiteral("<p>") + kSmile + kThumb + QStringLiteral("</p>"),
            nullptr, QString(), {});
        QCOMPARE(spanCount(out), 0);
        QCOMPARE(out, QStringLiteral("<p>") + kSmile + kThumb
                          + QStringLiteral("</p>"));
    }

    // A mention anchor is markup we built; the emoji beside it is content.
    // The anchor must come through whole, because MentionTokenizer recovers
    // the edit's mention refs by matching exactly this shape.
    void aMentionAnchorSurvivesBesideAnEnlargedEmoji()
    {
        const QString out = MessageHtml::sanitize(
            QStringLiteral("<a href=\"https://matrix.to/#/@bob:example.org\">"
                           "Bob</a> hi ")
                + kSmile,
            nullptr, QString(), {});
        // No resolver here, so the label falls back to the localpart — the
        // ANCHOR shape is what MentionTokenizer matches on, and that is what
        // must survive.
        QVERIFY(out.contains(QStringLiteral(
            "<a href=\"mention:@bob:example.org\">@bob</a>")));
        QCOMPARE(spanCount(out), 1);
        QCOMPARE(wrapped(out), kSmile);
    }
};

QTEST_MAIN(MessageEmojiSizeTest)
#include "MessageEmojiSizeTest.moc"
