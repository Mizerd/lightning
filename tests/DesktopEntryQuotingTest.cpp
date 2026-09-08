#include "app/DesktopEntryQuoting.h"

#include <QtTest/QtTest>

using lightning::desktop_entry::quoteExecArgument;

// The Exec quoting, CALLED rather than scanned for.
//
// The original guard was a source scan over src/main.cpp asserting that a
// literal appeared somewhere in a 1,900-line file. A review pointed out that
// several such contracts pass on broken code, and this one had concrete right
// and wrong answers going begging, so the function moved into its own unit
// and these call it.
//
// Every case below FAILS on the escaping that shipped, which escaped `"`,
// `` ` ``, `$` and `\` with a single backslash and left `%` alone.
class DesktopEntryQuotingTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void anOrdinaryPathIsJustQuoted()
    {
        QCOMPARE(quoteExecArgument(QStringLiteral("/opt/Lightning.AppImage")),
                 QStringLiteral("\"/opt/Lightning.AppImage\""));
        // A space is the whole reason the argument is quoted at all.
        QCOMPARE(quoteExecArgument(QStringLiteral("/home/a b/L.AppImage")),
                 QStringLiteral("\"/home/a b/L.AppImage\""));
    }

    // A LITERAL BACKSLASH NEEDS FOUR. One for the Exec rule, and then both of
    // those doubled by the string escape the reader undoes first.
    void aBackslashSurvivesBothEscapingLayers()
    {
        const QString out = quoteExecArgument(QStringLiteral("/a\\b"));
        QCOMPARE(out, QStringLiteral("\"/a\\\\\\\\b\""));
        QVERIFY2(out.count(QLatin1Char('\\')) == 4,
                 "a literal backslash was not written as four: the reader "
                 "unescapes first, so anything less collapses to an Exec "
                 "escape of the following character");
    }

    // `\$` IS NOT A VALID STRING ESCAPE. GKeyFile reports an invalid escape
    // sequence and returns NULL for the whole value, so the entry ends up
    // with no Exec at all and the menu item silently does nothing.
    void aDollarIsEscapedAtBothLayersNotOne()
    {
        const QString out = quoteExecArgument(QStringLiteral("/a$b"));
        QCOMPARE(out, QStringLiteral("\"/a\\\\$b\""));
        QVERIFY2(!out.contains(QStringLiteral("\\$"))
                     || out.contains(QStringLiteral("\\\\$")),
                 "the file would carry a bare \\$, which is an invalid string "
                 "escape: the reader returns NULL for the value and the entry "
                 "loses its Exec line entirely");
    }

    void aBacktickAndAQuoteGetTheSameTreatment()
    {
        QCOMPARE(quoteExecArgument(QStringLiteral("/a`b")),
                 QStringLiteral("\"/a\\\\`b\""));
        QCOMPARE(quoteExecArgument(QStringLiteral("/a\"b")),
                 QStringLiteral("\"/a\\\\\"b\""));
    }

    // `%` INTRODUCES A FIELD CODE. A browser-downloaded AppImage very often
    // sits in a percent-encoded path, and an undoubled `%20` is dropped by
    // the expander: `…/Down%20loads/…` launches from `…/Down0loads/…`, which
    // does not exist.
    void aPercentIsDoubledSoTheFieldCodeExpanderLeavesItAlone()
    {
        const QString out =
            quoteExecArgument(QStringLiteral("/home/a/Down%20loads/L.AppImage"));
        QVERIFY2(out.contains(QStringLiteral("Down%%20loads")),
                 "a literal % was left single, so the field-code expander "
                 "consumes it and the entry launches a path that does not "
                 "exist");
        QCOMPARE(out.count(QLatin1Char('%')), 2);
    }

    // REFUSED, NOT ENCODED. A newline would inject a key into the file and
    // there is no correct quoting for it; the caller treats empty as "do not
    // publish", which is the only safe answer.
    void aControlCharacterIsRefusedRatherThanEncoded()
    {
        QVERIFY2(quoteExecArgument(QStringLiteral("/a\nExec=/bin/sh")).isEmpty(),
                 "a newline in the path was encoded instead of refused, so the "
                 "written entry would carry an attacker-chosen extra key");
        QVERIFY(quoteExecArgument(QStringLiteral("/a\tb")).isEmpty());
        QVERIFY(quoteExecArgument(QStringLiteral("/a\rb")).isEmpty());
    }

    void anEmptyPathIsStillWellFormed()
    {
        QCOMPARE(quoteExecArgument(QString{}), QStringLiteral("\"\""));
    }
};

QTEST_MAIN(DesktopEntryQuotingTest)
#include "DesktopEntryQuotingTest.moc"
