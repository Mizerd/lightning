// Inline emoji sizing: MessageHtml::markEmoji.
//
// Qt's rich-text engine ignores `em` and `%` in font-size, and MessageHtml
// does not know its pixel size, so the only scale-relative lever is the CSS
// keyword ladder (0.7/0.8/1.0/1.2/1.5/2.0/2.4); `x-large` is its 1.5 rung.
//
// These cases assert output for a given body, not source text.
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

const QString kSmile = u(0x1F60A);
const QString kThumb = u(0x1F44D);
const QString kZwj = QStringLiteral("‍");
const QString kVs16 = QStringLiteral("️");

}

class MessageEmojiSizeTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:

    void emojiInASentenceIsEnlarged()
    {
        const QString in = QStringLiteral("i dont have any downloaded ") + kSmile
            + QStringLiteral(", I try to get away");
        const QString out = MessageHtml::markEmoji(in);
        QCOMPARE(spanCount(out), 1);
        QCOMPARE(wrapped(out), kSmile);
        QVERIFY(out.contains(QStringLiteral("i dont have any downloaded ")
                             + kOpen));
        QVERIFY(out.contains(kClose + QStringLiteral(", I try to get away")));
    }

    void theSizeIsTheScaleRelativeKeywordNotAPixelValue()
    {
        const QString out =
            MessageHtml::markEmoji(QStringLiteral("hi ") + kSmile);
        QVERIFY(out.contains(QStringLiteral("font-size:x-large")));
        QVERIFY(!out.contains(QStringLiteral("px")));
        QVERIFY(!out.contains(QStringLiteral("em\"")));
        QVERIFY(!out.contains(QStringLiteral("%")));
    }

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

    void fourEmojiIsNotABigEmojiRowAndIsEnlarged()
    {
        const QString body = kSmile + kSmile + kSmile + kSmile;
        const QString out = MessageHtml::markEmoji(body);
        QVERIFY(out != body);
        QCOMPARE(spanCount(out), 1);
        QCOMPARE(wrapped(out), body);
    }

    void aShortNonLatinMessageIsStillEnlarged()
    {
        const QString body = QStringLiteral("こん") + kSmile;
        const QString out = MessageHtml::markEmoji(body);
        QCOMPARE(spanCount(out), 1);
        QCOMPARE(wrapped(out), kSmile);
    }

    void emojiInsideCodeKeepsItsSize()
    {
        const QString in = QStringLiteral("<code>print(\"") + kSmile
            + QStringLiteral("\")</code> and ") + kSmile;
        const QString out = MessageHtml::markEmoji(in);
        QCOMPARE(spanCount(out), 1);
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

    void aVariationSelectorQualifiesAnEmoji()
    {
        const QString heart = u(0x2764);
        const QString qualified = heart + kVs16;
        const QString out = MessageHtml::markEmoji(QStringLiteral("a ")
                                                   + qualified
                                                   + QStringLiteral(" b"));
        QCOMPARE(spanCount(out), 1);
        QCOMPARE(wrapped(out), qualified);

        const QString bare =
            MessageHtml::markEmoji(QStringLiteral("a ") + heart
                                   + QStringLiteral(" b"));
        QCOMPARE(spanCount(bare), 0);
    }

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

    void anEntityEncodedEmojiIsLeftAlone()
    {
        const QString in = QStringLiteral("a &#128522; b");
        QCOMPARE(MessageHtml::markEmoji(in), in);
    }

    void nothingFromTheBodyReachesTheStyle()
    {
        const QString hostile = QStringLiteral("x\" onload=\"alert(1)\" y=\"")
            + kSmile + QStringLiteral(" ;color:red;");
        const QString out = MessageHtml::markEmoji(hostile);
        QCOMPARE(spanCount(out), 1);
        QCOMPARE(out.count(QStringLiteral("style=")), 1);
        QVERIFY(out.contains(kOpen));
        QVERIFY(!out.contains(QStringLiteral("style=\"color:red")));
        QVERIFY(out.contains(QStringLiteral("onload=\"alert(1)\"")));
    }

    void theNumberOfRunsIsBounded()
    {
        QString body;
        for (int i = 0; i < 900; ++i)
            body += kSmile + QStringLiteral(" ");
        const QString out = MessageHtml::markEmoji(body);
        QVERIFY(spanCount(out) <= 256);
        QVERIFY(out.size() <= body.size() + 256 * (kOpen.size() + kClose.size()));
    }

    void anAsciiBodyIsReturnedUnchanged()
    {
        const QString body =
            QStringLiteral("<b>hello</b> world, see https://example.com/x?a=1&amp;b=2");
        QCOMPARE(MessageHtml::markEmoji(body), body);
        QCOMPARE(MessageHtml::markEmoji(QString()), QString());
    }

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

    void aMentionAnchorSurvivesBesideAnEnlargedEmoji()
    {
        const QString out = MessageHtml::sanitize(
            QStringLiteral("<a href=\"https://matrix.to/#/@bob:example.org\">"
                           "Bob</a> hi ")
                + kSmile,
            nullptr, QString(), {});
        QVERIFY(out.contains(QStringLiteral(
            "<a href=\"mention:@bob:example.org\">@bob</a>")));
        QCOMPARE(spanCount(out), 1);
        QCOMPARE(wrapped(out), kSmile);
    }
};

QTEST_MAIN(MessageEmojiSizeTest)
#include "MessageEmojiSizeTest.moc"
