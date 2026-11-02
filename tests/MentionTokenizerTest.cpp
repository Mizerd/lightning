
#include <QtTest/QtTest>

#include "models/MentionTokenizer.h"

using namespace mention;

class MentionTokenizerTest : public QObject
{
    Q_OBJECT

private slots:
    void triggersAtStartAfterSpaceParenAndQuote()
    {
        {
            const Token t = activeToken(QStringLiteral("@ali"), 4);
            QVERIFY(t.active);
            QCOMPARE(t.start, 0);
            QCOMPARE(t.query, QStringLiteral("ali"));
        }
        {
            const Token t = activeToken(QStringLiteral("hi @bob"), 7);
            QVERIFY(t.active);
            QCOMPARE(t.start, 3);
            QCOMPARE(t.query, QStringLiteral("bob"));
        }
        QVERIFY(activeToken(QStringLiteral("(@c"), 3).active);
        QVERIFY(activeToken(QStringLiteral(">@d"), 3).active);
        {
            const Token t = activeToken(QStringLiteral("@"), 1);
            QVERIFY(t.active);
            QCOMPARE(t.query, QString());
        }
    }

    void rejectsMidWordAndEmail()
    {
        QVERIFY(!activeToken(QStringLiteral("mail@server"), 11).active);
        QVERIFY(!activeToken(QStringLiteral("a@b"), 3).active);
        QVERIFY(!activeToken(QStringLiteral("x@name"), 6).active);
    }

    void rejectsInlineCodeAndFencedBlocks()
    {
        QVERIFY(!activeToken(QStringLiteral("`@bob"), 5).active);
        QVERIFY(activeToken(QStringLiteral("`x` @bob"), 8).active);
        QVERIFY(activeToken(QStringLiteral("\\` @bob"), 7).active);
        QVERIFY(!activeToken(QStringLiteral("```\n@bob"), 8).active);
        QVERIFY(activeToken(QStringLiteral("```\ncode\n```\n@bob"), 17).active);
    }

    void capturesQueryWithSpacesAndCaps()
    {
        const Token t = activeToken(QStringLiteral("@John Sm"), 8);
        QVERIFY(t.active);
        QCOMPARE(t.query, QStringLiteral("John Sm"));

        QVERIFY(!activeToken(QStringLiteral("@SpongeMan as a"), 15).active);
        QVERIFY(!activeToken(QStringLiteral("@SpongeMan as a true profesional"), 32).active);
        QVERIFY(activeToken(QStringLiteral("@SpongeMan as"), 13).active);

        const QString longQuery = QStringLiteral("@") + QString(41, QChar('a'));
        QVERIFY(!activeToken(longQuery, longQuery.length()).active);
        const QString maxQuery = QStringLiteral("@") + QString(40, QChar('a'));
        QVERIFY(activeToken(maxQuery, maxQuery.length()).active);
    }

    void cursorOutsideTokenIsInactive()
    {
        QVERIFY(!activeToken(QStringLiteral("@bob, hi"), 8).active);
        QVERIFY(!activeToken(QStringLiteral("@bob"), 0).active);
    }

    void buildsInsertionAndRecordsRef()
    {
        const InsertResult r = buildInsertion(
            QStringLiteral("hi @al"), 3, 6, QStringLiteral("@alice:hs"),
            QStringLiteral("Alice"));
        QCOMPARE(r.text, QStringLiteral("hi @Alice "));
        QCOMPARE(r.cursorPos, 10);
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
        ref.start = 3;
        ref.length = 6;
        const QString base = QStringLiteral("hi @Alice ");

        {
            const auto out = shiftRefs({ ref }, base,
                                       QStringLiteral("hi @Alice world"));
            QCOMPARE(out.size(), 1);
            QCOMPARE(out.first().start, 3);
        }
        {
            const auto out = shiftRefs({ ref }, base,
                                       QStringLiteral("yo hi @Alice "));
            QCOMPARE(out.size(), 1);
            QCOMPARE(out.first().start, 6);
            QCOMPARE(out.first().displayText, QStringLiteral("@Alice"));
        }
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
        const QString text = QStringLiteral("@Alice and @Bob");
        const Expansion e = expand(text, { b, a });
        QCOMPARE(e.userIds,
                 (QStringList{ QStringLiteral("@alice:hs"),
                               QStringLiteral("@bob:hs") }));
        QVERIFY(e.body.contains(
            QStringLiteral("[@Alice](https://matrix.to/#/%40alice%3Ahs)")));
        QVERIFY(e.body.contains(
            QStringLiteral("[@Bob](https://matrix.to/#/%40bob%3Ahs)")));

        const Expansion plain = expand(QStringLiteral("just text"), {});
        QCOMPARE(plain.body, QStringLiteral("just text"));
        QVERIFY(plain.userIds.isEmpty());
    }

    // Only a PICKED member becomes a ref. Typed "@name" text, even one that
    // equals a member's display name, carries no ref, so it expands to itself:
    // no matrix.to link and no m.mentions id (Element's behaviour).
    void typedAtTextWithoutAPickSendsAsPlainText()
    {
        const QString typed = QStringLiteral("hello @nobody and @Alice");
        const Expansion e = expand(typed, {});
        QCOMPARE(e.body, typed);
        QVERIFY(e.userIds.isEmpty());
        QVERIFY(!e.body.contains(QStringLiteral("matrix.to")));

        const InsertResult r = buildInsertion(
            QStringLiteral("hello @nobody and @Al"), 18, 21,
            QStringLiteral("@alice:hs"), QStringLiteral("Alice"));
        const Expansion picked = expand(r.text, { r.ref });
        QCOMPARE(picked.userIds, QStringList{ QStringLiteral("@alice:hs") });
        QVERIFY(picked.body.contains(QStringLiteral("hello @nobody and ")));
    }

    void recoverRoundTripsExpandedBodies()
    {
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

    void refsRecoverFromSanitizedAnchorsForDisplayTextBodies()
    {
        const QString body = QStringLiteral("@Alice and @Alice hi");
        const QString html = QStringLiteral(
            "<a href=\"mention:@alice:hs\" style=\"color:#7c7ff2;"
            "background-color:#25253d;text-decoration:none\">"
            "&nbsp;@Alice&nbsp;</a> and "
            "<a href=\"mention:@alice2:hs\"><b>@Alice</b></a> hi");
        const QList<MentionRef> refs = refsFromSanitizedHtml(body, html);
        QCOMPARE(refs.size(), 2);
        QCOMPARE(refs.at(0).userId, QStringLiteral("@alice:hs"));
        QCOMPARE(refs.at(0).start, 0);
        QCOMPARE(refs.at(0).length, 6);
        QCOMPARE(refs.at(1).userId, QStringLiteral("@alice2:hs"));
        QCOMPARE(refs.at(1).start, 11);

        const Expansion e = expand(body, refs);
        QCOMPARE(e.userIds,
                 (QStringList{ QStringLiteral("@alice:hs"),
                               QStringLiteral("@alice2:hs") }));

        const QList<MentionRef> miss = refsFromSanitizedHtml(
            QStringLiteral("something else"),
            QStringLiteral("<a href=\"mention:@x:hs\">@Gone</a>"));
        QVERIFY(miss.isEmpty());
    }

    void recoverLeavesNonMentionMarkdownAlone()
    {
        const QString roomLink = QStringLiteral(
            "join [room](https://matrix.to/#/%23room%3Ahs) or "
            "[site](https://example.org/x)");
        const Recovery r = recoverFromBody(roomLink);
        QCOMPARE(r.text, roomLink);
        QVERIFY(r.refs.isEmpty());

        const Recovery esc = recoverFromBody(QStringLiteral(
            "[@A\\]B](https://matrix.to/#/%40a%3Ahs)"));
        QCOMPARE(esc.text, QStringLiteral("@A]B"));
        QCOMPARE(esc.refs.size(), 1);
        QCOMPARE(esc.refs.at(0).displayText, QStringLiteral("@A]B"));

        const Recovery plain = recoverFromBody(QStringLiteral("no links"));
        QCOMPARE(plain.text, QStringLiteral("no links"));
        QVERIFY(plain.refs.isEmpty());
    }
};

QTEST_APPLESS_MAIN(MentionTokenizerTest)
#include "MentionTokenizerTest.moc"
