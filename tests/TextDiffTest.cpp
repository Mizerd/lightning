#include "models/TextDiff.h"

#include <QRegularExpression>
#include <QtTest/QtTest>

using textdiff::Op;
using textdiff::Segment;

class TextDiffTest : public QObject
{
    Q_OBJECT

    static QString join(const QList<Segment> &segs, Op skip)
    {
        QString out;
        for (const Segment &s : segs)
            if (s.op != skip)
                out += s.text;
        return out;
    }
    static void checkRoundTrip(const QString &a, const QString &b)
    {
        bool ok = false;
        const QList<Segment> d = textdiff::diff(a, b, &ok);
        QVERIFY(ok);
        QCOMPARE(join(d, Op::Added), a);
        QCOMPARE(join(d, Op::Removed), b);
    }

private Q_SLOTS:
    void identicalIsOneSameSegment()
    {
        const auto d = textdiff::diff(QStringLiteral("hello world"), QStringLiteral("hello world"));
        QCOMPARE(d, (QList<Segment>{ { Op::Same, QStringLiteral("hello world") } }));
        QVERIFY(textdiff::diff(QString(), QString()).isEmpty());
    }

    void insertion()
    {
        const auto d = textdiff::diff(QStringLiteral("hello world"),
                                      QStringLiteral("hello big world"));
        QCOMPARE(d, (QList<Segment>{ { Op::Same, QStringLiteral("hello ") },
                                     { Op::Added, QStringLiteral("big ") },
                                     { Op::Same, QStringLiteral("world") } }));
    }

    void deletion()
    {
        const auto d = textdiff::diff(QStringLiteral("a quick fox."), QStringLiteral("a fox."));
        QCOMPARE(d, (QList<Segment>{ { Op::Same, QStringLiteral("a ") },
                                     { Op::Removed, QStringLiteral("quick ") },
                                     { Op::Same, QStringLiteral("fox.") } }));
    }

    void replacementIsWordLevel()
    {
        const auto d = textdiff::diff(QStringLiteral("see you tomorrow!"),
                                      QStringLiteral("see you today!"));
        QCOMPARE(d.size(), 4);
        QCOMPARE(d[0], (Segment{ Op::Same, QStringLiteral("see you ") }));
        QCOMPARE(d[3], (Segment{ Op::Same, QStringLiteral("!") }));
        QVERIFY(d.contains(Segment{ Op::Removed, QStringLiteral("tomorrow") }));
        QVERIFY(d.contains(Segment{ Op::Added, QStringLiteral("today") }));
    }

    void unicodeAndEmojiStayWhole()
    {
        const QString family = QStringLiteral("\U0001F468‍\U0001F469‍\U0001F467");
        const QString a = QStringLiteral("Café ") + family + QStringLiteral(" ok");
        const QString b = QStringLiteral("Café \U0001F600 ok");
        checkRoundTrip(a, b);
        const auto d = textdiff::diff(a, b);
        QVERIFY(d.contains(Segment{ Op::Removed, family }));
        QVERIFY(d.contains(Segment{ Op::Added, QStringLiteral("\U0001F600") }));
        checkRoundTrip(QStringLiteral("Zürich straße"), QStringLiteral("Zurich strasse"));
    }

    void whitespaceOnlyChange()
    {
        const auto d = textdiff::diff(QStringLiteral("one two"), QStringLiteral("one  two"));
        QVERIFY(!d.isEmpty());
        QVERIFY(std::any_of(d.begin(), d.end(), [](const Segment &s) {
            return s.op != Op::Same && s.text.trimmed().isEmpty();
        }));
        checkRoundTrip(QStringLiteral("one two"), QStringLiteral("one\ntwo"));
    }

    void roundTripsArbitraryEdits()
    {
        checkRoundTrip(QString(), QStringLiteral("new text"));
        checkRoundTrip(QStringLiteral("old text"), QString());
        checkRoundTrip(QStringLiteral("a b c d e f"), QStringLiteral("f e d c b a"));
    }

    void sizeCapSkipsTheDiff()
    {
        QString a, b;
        for (int i = 0; i < 1500; ++i) {
            a += QStringLiteral("a%1 ").arg(i);
            b += QStringLiteral("b%1 ").arg(i);
        }
        bool ok = true;
        QVERIFY(textdiff::diff(a, b, &ok).isEmpty());
        QVERIFY(!ok);
        QVERIFY(textdiff::html(a, b, QStringLiteral("#00ff00"), QStringLiteral("#ff0000")).isEmpty());
        // A small edit to a long text is still diffed: the shared ends are trimmed first.
        QVERIFY(!textdiff::html(a, a + QStringLiteral("x"), QStringLiteral("#00ff00"),
                                QStringLiteral("#ff0000")).isEmpty());
    }

    void cellCountBeyond32BitsIsRejected()
    {
        const QString before = QStringLiteral("a ").repeated(32768) + QLatin1Char('a');
        const QString after = QStringLiteral("b ").repeated(32768) + QLatin1Char('b');
        bool ok = true;
        QVERIFY(textdiff::diff(before, after, &ok).isEmpty());
        QVERIFY(!ok);
        QCOMPARE(textdiff::diff({}, after), (QList<Segment>{{Op::Added, after}}));
        QCOMPARE(textdiff::diff(before, {}), (QList<Segment>{{Op::Removed, before}}));
    }

    void htmlEscapesEveryFragment()
    {
        const QString h = textdiff::html(QStringLiteral("<b>x</b> & \"q\""),
                                         QStringLiteral("<img src=a onerror=b> & \"q\"\nz"),
                                         QStringLiteral("#00aa00"), QStringLiteral("#aa0000"));
        QString bare = h;
        bare.remove(QRegularExpression(QStringLiteral(
            R"re(<div style="white-space:pre-wrap">|</div>|<span style="color:#[0-9a-f]{6};text-decoration:(underline|line-through)">|</span>|<br>)re")));
        QVERIFY2(!bare.contains(QLatin1Char('<')) && !bare.contains(QLatin1Char('>')),
                 qPrintable(bare));
        QVERIFY(bare.contains(QStringLiteral("&lt;")) && bare.contains(QStringLiteral("img")));
        QVERIFY(h.contains(QStringLiteral("&amp;")));
        QVERIFY(h.contains(QStringLiteral("&quot;q&quot;")));
        QVERIFY(h.contains(QStringLiteral("<br>")));
        QVERIFY(h.contains(QStringLiteral("color:#aa0000;text-decoration:line-through")));
        QVERIFY(h.contains(QStringLiteral("color:#00aa00;text-decoration:underline")));
    }

    void htmlMarksWordsNotTheirSpaces()
    {
        const QString added = QStringLiteral("#00aa00");
        const QString removed = QStringLiteral("#aa0000");
        const QString ins = QStringLiteral("<span style=\"color:#00aa00;text-decoration:underline\">");
        const QString del = QStringLiteral("<span style=\"color:#aa0000;text-decoration:line-through\">");
        QCOMPARE(textdiff::html(QStringLiteral("a quick fox."), QStringLiteral("a fox."), added, removed),
                 QStringLiteral("<div style=\"white-space:pre-wrap\">a %1quick</span> fox.</div>").arg(del));
        const QString swap = textdiff::html(QStringLiteral("see you tomorrow!"),
                                            QStringLiteral("see you today!"), added, removed);
        QVERIFY2(swap.contains(QStringLiteral("tomorrow</span>") + QChar(0x2009) + ins
                               + QStringLiteral("today</span>")),
                 qPrintable(swap));
        const QString ws = textdiff::html(QStringLiteral("one two"), QStringLiteral("one  two"),
                                          added, removed);
        QVERIFY2(ws.contains(QRegularExpression(QRegularExpression::escape(ins)
                                                + QStringLiteral(" +</span>"))),
                 qPrintable(ws));
    }

    void htmlRejectsAnythingButAHexColour()
    {
        const QString h = textdiff::html(QStringLiteral("a"), QStringLiteral("b"),
                                         QStringLiteral("red\"><script>"),
                                         QStringLiteral("#12345g"));
        QVERIFY(!h.contains(QStringLiteral("script")));
        QCOMPARE(h.count(QStringLiteral("color:inherit")), 2);
    }
};

QTEST_GUILESS_MAIN(TextDiffTest)
#include "TextDiffTest.moc"
