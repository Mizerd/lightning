#include "app/DesktopEntryQuoting.h"

#include <QtTest/QtTest>

using lightning::desktop_entry::quoteExecArgument;

// Desktop-entry Exec argument quoting, tested by calling the function rather
// than scanning main.cpp for a literal.
class DesktopEntryQuotingTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void anOrdinaryPathIsJustQuoted()
    {
        QCOMPARE(quoteExecArgument(QStringLiteral("/opt/Lightning.AppImage")),
                 QStringLiteral("\"/opt/Lightning.AppImage\""));
        // A space is why the argument is quoted at all.
        QCOMPARE(quoteExecArgument(QStringLiteral("/home/a b/L.AppImage")),
                 QStringLiteral("\"/home/a b/L.AppImage\""));
    }

    // A literal backslash needs four: one for the Exec rule, both doubled by
    // the string escape the reader undoes first.
    void aBackslashSurvivesBothEscapingLayers()
    {
        const QString out = quoteExecArgument(QStringLiteral("/a\\b"));
        QCOMPARE(out, QStringLiteral("\"/a\\\\\\\\b\""));
        QVERIFY2(out.count(QLatin1Char('\\')) == 4,
                 "a literal backslash was not written as four: the reader "
                 "unescapes first, so anything less collapses to an Exec "
                 "escape of the following character");
    }

    // `\$` is not a valid string escape: GKeyFile rejects the whole value and
    // the entry ends up with no Exec.
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

    // `%` introduces a field code: an undoubled `%20` in a percent-encoded
    // download path is mangled by the expander.
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

    // Refused, not encoded: a newline would inject a key and has no correct
    // quoting. Empty means "do not publish".
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
