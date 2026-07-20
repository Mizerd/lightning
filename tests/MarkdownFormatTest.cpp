// Unit coverage for the composer formatting toolbar's markdown transforms:
// wrap/unwrap for the inline styles, line prefixes for list/quote, the link
// scaffold, selection integrity, and the active-state detection that drives
// the toolbar's accent-chip states.

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
        // Selection is the inner word of an already-wrapped range.
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
        // Toggling again from the caret between the markers removes them.
        const auto undone = toggle(QStringLiteral("bold"), r.text, 5, 5);
        QCOMPARE(undone.text, QStringLiteral("hi "));
    }

    void listPrefixesEveryCoveredLine()
    {
        const QString text = QStringLiteral("one\ntwo\nthree");
        const auto r = toggle(QStringLiteral("list"), text, 0, text.size());
        QCOMPARE(r.text, QStringLiteral("- one\n- two\n- three"));
        // Toggling again removes the prefixes.
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
        // Selecting the label of an existing link removes it.
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
