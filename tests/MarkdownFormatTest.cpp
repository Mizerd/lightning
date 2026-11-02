
#include <QtTest/QtTest>

#include "models/MarkdownFormat.h"

using MarkdownFormat::toggle;
using MarkdownFormat::state;

class MarkdownFormatTest : public QObject
{
    Q_OBJECT

private slots:
    void boldWrapsSelection()
    {
        const auto r = toggle(QStringLiteral("bold"),
                              QStringLiteral("hello world"), 0, 5);
        QCOMPARE(r.text, QStringLiteral("**hello** world"));
        QCOMPARE(r.selectionStart, 2);
        QCOMPARE(r.selectionEnd, 7);
    }

    void boldUnwrapsWrappedSelection()
    {
        const auto r = toggle(QStringLiteral("bold"),
                              QStringLiteral("**hello** world"), 2, 7);
        QCOMPARE(r.text, QStringLiteral("hello world"));
        QCOMPARE(r.selectionStart, 0);
        QCOMPARE(r.selectionEnd, 5);
    }

    void boldUnwrapsWhenMarkersInsideSelection()
    {
        const auto r = toggle(QStringLiteral("bold"),
                              QStringLiteral("**hello** world"), 0, 9);
        QCOMPARE(r.text, QStringLiteral("hello world"));
        QCOMPARE(r.selectionStart, 0);
        QCOMPARE(r.selectionEnd, 5);
    }

    void italicUsesUnderscoreAndDoesNotCollideWithBold()
    {
        const auto r = toggle(QStringLiteral("italic"),
                              QStringLiteral("**hi**"), 2, 4);
        QCOMPARE(r.text, QStringLiteral("**_hi_**"));
        const QVariantMap flags = state(r.text, r.selectionStart, r.selectionEnd);
        QVERIFY(flags.value(QStringLiteral("italic")).toBool());
        QVERIFY(!flags.value(QStringLiteral("bold")).toBool());
    }

    void strikeAndCodeRoundTrip()
    {
        auto r = toggle(QStringLiteral("strike"), QStringLiteral("abc"), 0, 3);
        QCOMPARE(r.text, QStringLiteral("~~abc~~"));
        r = toggle(QStringLiteral("strike"), r.text, r.selectionStart,
                   r.selectionEnd);
        QCOMPARE(r.text, QStringLiteral("abc"));

        r = toggle(QStringLiteral("code"), QStringLiteral("abc"), 0, 3);
        QCOMPARE(r.text, QStringLiteral("`abc`"));
        r = toggle(QStringLiteral("code"), r.text, r.selectionStart,
                   r.selectionEnd);
        QCOMPARE(r.text, QStringLiteral("abc"));
    }

    void emptySelectionInsertsMarkerPairWithCaretBetween()
    {
        const auto r = toggle(QStringLiteral("bold"), QStringLiteral("hi "), 3, 3);
        QCOMPARE(r.text, QStringLiteral("hi ****"));
        QCOMPARE(r.selectionStart, 5);
        QCOMPARE(r.selectionEnd, 5);
        const auto undone = toggle(QStringLiteral("bold"), r.text, 5, 5);
        QCOMPARE(undone.text, QStringLiteral("hi "));
    }

    void listPrefixesEveryCoveredLine()
    {
        const QString text = QStringLiteral("one\ntwo\nthree");
        const auto r = toggle(QStringLiteral("list"), text, 0, text.size());
        QCOMPARE(r.text, QStringLiteral("- one\n- two\n- three"));
        const auto undone = toggle(QStringLiteral("list"), r.text,
                                   r.selectionStart, r.selectionEnd);
        QCOMPARE(undone.text, text);
    }

    void quotePrefixesCurrentLineForCaretSelection()
    {
        const auto r = toggle(QStringLiteral("quote"),
                              QStringLiteral("first\nsecond"), 8, 8);
        QCOMPARE(r.text, QStringLiteral("first\n> second"));
        QCOMPARE(r.selectionStart, 10);
    }

    void mixedListSelectionAddsMissingPrefixesOnly()
    {
        const QString text = QStringLiteral("- one\ntwo");
        const auto r = toggle(QStringLiteral("list"), text, 0, text.size());
        QCOMPARE(r.text, QStringLiteral("- one\n- two"));
    }

    void linkWrapsSelectionAndSelectsPlaceholderUrl()
    {
        const auto r = toggle(QStringLiteral("link"),
                              QStringLiteral("see docs"), 4, 8);
        QCOMPARE(r.text, QStringLiteral("see [docs](url)"));
        QCOMPARE(r.text.mid(r.selectionStart,
                            r.selectionEnd - r.selectionStart),
                 QStringLiteral("url"));
        const auto undone = toggle(QStringLiteral("link"), r.text, 5, 9);
        QCOMPARE(undone.text, QStringLiteral("see docs"));
    }

    void linkWithEmptySelectionInsertsScaffold()
    {
        const auto r = toggle(QStringLiteral("link"), QString(), 0, 0);
        QCOMPARE(r.text, QStringLiteral("[text](url)"));
        QCOMPARE(r.text.mid(r.selectionStart,
                            r.selectionEnd - r.selectionStart),
                 QStringLiteral("text"));
    }

    void stateReportsActiveFormats()
    {
        const QVariantMap flags =
            state(QStringLiteral("**bold**"), 2, 6);
        QVERIFY(flags.value(QStringLiteral("bold")).toBool());
        QVERIFY(!flags.value(QStringLiteral("italic")).toBool());
        QVERIFY(!flags.value(QStringLiteral("code")).toBool());

        const QVariantMap list =
            state(QStringLiteral("- a\n- b"), 0, 7);
        QVERIFY(list.value(QStringLiteral("list")).toBool());
        QVERIFY(!list.value(QStringLiteral("quote")).toBool());

        const QVariantMap link =
            state(QStringLiteral("[docs](https://x)"), 1, 5);
        QVERIFY(link.value(QStringLiteral("link")).toBool());
    }

    void aBareCaretInASpanTogglesItOff_data()
    {
        QTest::addColumn<QString>("format");
        QTest::addColumn<int>("caret");
        QTest::addColumn<int>("after");
        QTest::newRow("inside") << QStringLiteral("code") << 6 << 5;
        QTest::newRow("just inside end") << QStringLiteral("code") << 16 << 15;
        QTest::newRow("after the span") << QStringLiteral("code") << 17 << 15;
    }
    void aBareCaretInASpanTogglesItOff()
    {
        QFETCH(QString, format);
        QFETCH(int, caret);
        QFETCH(int, after);
        const QString text = QStringLiteral("`run <foo> ok xx`");
        QVERIFY(MarkdownFormat::state(text, caret, caret).value(format).toBool());
        const auto result = MarkdownFormat::toggle(format, text, caret, caret);
        QCOMPARE(result.text, QStringLiteral("run <foo> ok xx"));
        QCOMPARE(result.selectionStart, after);
        QCOMPARE(result.selectionEnd, after);
    }

    void aBareCaretOutsideASpanStillInsertsAPair()
    {
        const QString text = QStringLiteral("`a` b");
        const auto result = MarkdownFormat::toggle(QStringLiteral("code"), text, 5, 5);
        QCOMPARE(result.text, QStringLiteral("`a` b``"));
        QVERIFY(!MarkdownFormat::state(text, 5, 5).value(QStringLiteral("code")).toBool());
    }

    void literalMarkersWithSpacesAroundThemAreNoSpan()
    {
        const QString text = QStringLiteral("2 ** 3 ** 4");
        for (int caret : {3, 6, 8}) {
            QVERIFY2(!MarkdownFormat::state(text, caret, caret)
                          .value(QStringLiteral("bold")).toBool(),
                     qPrintable(QString::number(caret)));
        }
        const auto result =
            MarkdownFormat::toggle(QStringLiteral("bold"), text, 6, 6);
        QVERIFY(result.text.size() == text.size() + 4);
        QVERIFY(MarkdownFormat::state(QStringLiteral("a **b** c"), 5, 5)
                    .value(QStringLiteral("bold")).toBool());
        QVERIFY(MarkdownFormat::state(QStringLiteral("`a b ` c"), 3, 3)
                    .value(QStringLiteral("code")).toBool());
    }

    void unknownFormatAndOutOfRangeSelectionAreSafe()
    {
        const auto r = toggle(QStringLiteral("sparkle"),
                              QStringLiteral("abc"), -4, 99);
        QCOMPARE(r.text, QStringLiteral("abc"));
        QCOMPARE(r.selectionStart, 0);
        QCOMPARE(r.selectionEnd, 3);
        const QVariantMap flags = state(QString(), 5, -1);
        QVERIFY(!flags.value(QStringLiteral("bold")).toBool());
    }
};

QTEST_GUILESS_MAIN(MarkdownFormatTest)
#include "MarkdownFormatTest.moc"
