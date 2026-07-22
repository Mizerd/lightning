// v0.7 outgoing @-mentions: pure tokenizer behaviour. Trigger boundaries,
// user@host / mid-word rejection, inline-code and fenced-block suppression,
// query capture (spaces included) and the 40-char cap, cursor-outside
// inactivity, single-edit ref reconciliation, and matrix.to link expansion.

#include <QtTest/QtTest>

#include "models/MentionTokenizer.h"

using namespace mention;

class MentionTokenizerTest : public QObject
{
    Q_OBJECT

private slots:
    void triggersAtStartAfterSpaceParenAndQuote()
    {
        // Start of text.
        {
            const Token t = activeToken(QStringLiteral("@ali"), 4);
            QVERIFY(t.active);
            QCOMPARE(t.start, 0);
            QCOMPARE(t.query, QStringLiteral("ali"));
        }
        // After whitespace.
        {
            const Token t = activeToken(QStringLiteral("hi @bob"), 7);
            QVERIFY(t.active);
            QCOMPARE(t.start, 3);
            QCOMPARE(t.query, QStringLiteral("bob"));
        }
        // After '(' and after '>' (quote marker).
        QVERIFY(activeToken(QStringLiteral("(@c"), 3).active);
        QVERIFY(activeToken(QStringLiteral(">@d"), 3).active);
        // A bare '@' with the caret right after it is active (empty query).
        {
            const Token t = activeToken(QStringLiteral("@"), 1);
            QVERIFY(t.active);
            QCOMPARE(t.query, QString());
        }
    }

    void rejectsMidWordAndEmail()
    {
        // user@host: the char before '@' is alphanumeric.
        QVERIFY(!activeToken(QStringLiteral("mail@server"), 11).active);
        QVERIFY(!activeToken(QStringLiteral("a@b"), 3).active);
        // Mid-word '@' (after a letter) never triggers.
        QVERIFY(!activeToken(QStringLiteral("x@name"), 6).active);
    }

    void rejectsInlineCodeAndFencedBlocks()
    {
        // Odd number of backticks before the '@' on the same line: inline code.
        QVERIFY(!activeToken(QStringLiteral("`@bob"), 5).active);
        // Even backticks: closed inline code, back to normal text.
        QVERIFY(activeToken(QStringLiteral("`x` @bob"), 8).active);
        // Escaped backtick does not open inline code.
        QVERIFY(activeToken(QStringLiteral("\\` @bob"), 7).active);
        // Inside a fenced block (one opening ``` line above).
        QVERIFY(!activeToken(QStringLiteral("```\n@bob"), 8).active);
        // After a closed fence (two fence lines above), normal again.
        QVERIFY(activeToken(QStringLiteral("```\ncode\n```\n@bob"), 17).active);
    }

    void capturesQueryWithSpacesAndCaps()
    {
        const Token t = activeToken(QStringLiteral("@John Sm"), 8);
        QVERIFY(t.active);
        QCOMPARE(t.query, QStringLiteral("John Sm"));

        // A query longer than 40 characters is not an active token.
        const QString longQuery = QStringLiteral("@") + QString(41, QChar('a'));
        QVERIFY(!activeToken(longQuery, longQuery.length()).active);
        // Exactly 40 is still fine.
        const QString maxQuery = QStringLiteral("@") + QString(40, QChar('a'));
        QVERIFY(activeToken(maxQuery, maxQuery.length()).active);
    }

    void cursorOutsideTokenIsInactive()
    {
        // A non-query char (comma) between '@' and the caret breaks the token.
        QVERIFY(!activeToken(QStringLiteral("@bob, hi"), 8).active);
        // Caret before the '@' — nothing to complete.
        QVERIFY(!activeToken(QStringLiteral("@bob"), 0).active);
    }

    void buildsInsertionAndRecordsRef()
    {
        const InsertResult r = buildInsertion(
            QStringLiteral("hi @al"), 3, 6, QStringLiteral("@alice:hs"),
            QStringLiteral("Alice"));
        QCOMPARE(r.text, QStringLiteral("hi @Alice "));
        QCOMPARE(r.cursorPos, 10); // just past the trailing space
        QCOMPARE(r.ref.displayText, QStringLiteral("@Alice"));
        QCOMPARE(r.ref.start, 3);
        QCOMPARE(r.ref.length, 6);
        QCOMPARE(r.ref.userId, QStringLiteral("@alice:hs"));
    }

    void shiftRefsFollowsEditsAndDropsBrokenSlices()
    {
        MentionRef ref;
        ref.userId = QStringLiteral("@alice:hs");
        ref.displayText = QStringLiteral("@Alice");
        ref.start = 3; // "hi @Alice "
        ref.length = 6;
        const QString base = QStringLiteral("hi @Alice ");

        // Insert text AFTER the ref: it stays put.
        {
            const auto out = shiftRefs({ ref }, base,
                                       QStringLiteral("hi @Alice world"));
            QCOMPARE(out.size(), 1);
            QCOMPARE(out.first().start, 3);
        }
        // Insert text BEFORE the ref: it shifts right.
        {
            const auto out = shiftRefs({ ref }, base,
                                       QStringLiteral("yo hi @Alice "));
            QCOMPARE(out.size(), 1);
            QCOMPARE(out.first().start, 6);
            QCOMPARE(out.first().displayText, QStringLiteral("@Alice"));
        }
        // Delete a char INSIDE the name: slice no longer matches → dropped.
        {
            const auto out = shiftRefs({ ref }, base,
                                       QStringLiteral("hi @Alce "));
            QVERIFY(out.isEmpty());
        }
    }

    void expandRewritesRangesAndDedupsIds()
    {
        MentionRef a;
        a.userId = QStringLiteral("@alice:hs");
        a.displayText = QStringLiteral("@Alice");
        a.start = 0;
        a.length = 6;
        MentionRef b;
        b.userId = QStringLiteral("@bob:hs");
        b.displayText = QStringLiteral("@Bob");
        b.start = 11;
        b.length = 4;
        // "@Alice and @Bob"
        const QString text = QStringLiteral("@Alice and @Bob");
        const Expansion e = expand(text, { b, a }); // order-insensitive input
        QCOMPARE(e.userIds,
                 (QStringList{ QStringLiteral("@alice:hs"),
                               QStringLiteral("@bob:hs") }));
        QVERIFY(e.body.contains(
            QStringLiteral("[@Alice](https://matrix.to/#/%40alice%3Ahs)")));
        QVERIFY(e.body.contains(
            QStringLiteral("[@Bob](https://matrix.to/#/%40bob%3Ahs)")));

        // No refs → body unchanged, no ids.
        const Expansion plain = expand(QStringLiteral("just text"), {});
        QCOMPARE(plain.body, QStringLiteral("just text"));
        QVERIFY(plain.userIds.isEmpty());
    }

    void recoverRoundTripsExpandedBodies()
    {
        // Edit flow: a raw markdown body parses back into display text +
        // refs, so expand(recover(x)) round-trips and keeps m.mentions.
        const QString body = QStringLiteral(
            "[@Alice](https://matrix.to/#/%40alice%3Ahs) and "
            "[@Bob](https://matrix.to/#/%40bob%3Ahs) hi");
        const Recovery r = recoverFromBody(body);
        QCOMPARE(r.text, QStringLiteral("@Alice and @Bob hi"));
        QCOMPARE(r.refs.size(), 2);
        QCOMPARE(r.refs.at(0).userId, QStringLiteral("@alice:hs"));
        QCOMPARE(r.refs.at(0).start, 0);
        QCOMPARE(r.refs.at(0).length, 6);
        QCOMPARE(r.refs.at(1).userId, QStringLiteral("@bob:hs"));
        QCOMPARE(r.refs.at(1).start, 11);
        QCOMPARE(r.refs.at(1).length, 4);
        const Expansion round = expand(r.text, r.refs);
        QCOMPARE(round.body, QStringLiteral(
            "[@Alice](https://matrix.to/#/%40alice%3Ahs) and "
            "[@Bob](https://matrix.to/#/%40bob%3Ahs) hi"));
        QCOMPARE(round.userIds,
                 (QStringList{ QStringLiteral("@alice:hs"),
                               QStringLiteral("@bob:hs") }));
    }

    void recoverLeavesNonMentionMarkdownAlone()
    {
        // Room/event permalinks and ordinary links are NOT mentions.
        const QString roomLink = QStringLiteral(
            "join [room](https://matrix.to/#/%23room%3Ahs) or "
            "[site](https://example.org/x)");
        const Recovery r = recoverFromBody(roomLink);
        QCOMPARE(r.text, roomLink);
        QVERIFY(r.refs.isEmpty());

        // Escaped label characters unescape ("\]" -> "]").
        const Recovery esc = recoverFromBody(QStringLiteral(
            "[@A\\]B](https://matrix.to/#/%40a%3Ahs)"));
        QCOMPARE(esc.text, QStringLiteral("@A]B"));
        QCOMPARE(esc.refs.size(), 1);
        QCOMPARE(esc.refs.at(0).displayText, QStringLiteral("@A]B"));

        // Plain text round-trips untouched.
        const Recovery plain = recoverFromBody(QStringLiteral("no links"));
        QCOMPARE(plain.text, QStringLiteral("no links"));
        QVERIFY(plain.refs.isEmpty());
    }
};

QTEST_APPLESS_MAIN(MentionTokenizerTest)
#include "MentionTokenizerTest.moc"
