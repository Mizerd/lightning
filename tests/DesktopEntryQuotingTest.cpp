#include "app/DesktopEntryQuoting.h"

#include <QtTest/QtTest>

using lightning::desktop_entry::quoteExecArgument;

class DesktopEntryQuotingTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void anOrdinaryPathIsJustQuoted()
    {
        QCOMPARE(quoteExecArgument(QStringLiteral("/opt/Lightning.AppImage")),
                 QStringLiteral("\"/opt/Lightning.AppImage\""));
        QCOMPARE(quoteExecArgument(QStringLiteral("/home/a b/L.AppImage")),
                 QStringLiteral("\"/home/a b/L.AppImage\""));
    }

    void aBackslashSurvivesBothEscapingLayers()
    {
        const QString out = quoteExecArgument(QStringLiteral("/a\\b"));
        QCOMPARE(out, QStringLiteral("\"/a\\\\\\\\b\""));
        QVERIFY2(out.count(QLatin1Char('\\')) == 4,
                 "a literal backslash was not written as four: the reader "
                 "unescapes first, so anything less collapses to an Exec "
                 "escape of the following character");
    }

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
