#include "app/DesktopEntryQuoting.h"

#include <QtTest/QtTest>

#include <QTemporaryDir>

using lightning::desktop_entry::quoteExecArgument;
using lightning::desktop_entry::claimDefaultHandler;
using lightning::desktop_entry::withDefaultHandler;

class DesktopEntryQuotingTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aMimeappsListThatCannotBeReadIsNeverReplaced()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString mime = QStringLiteral("x-scheme-handler/matrix");
        const QString id = QStringLiteral("lightning.desktop");

        const QString asDir = dir.filePath(QStringLiteral("dir.list"));
        QVERIFY(QDir().mkpath(asDir));
        QCOMPARE(claimDefaultHandler(asDir, mime, id, {}),
                 QStringLiteral("skipped: mimeapps.list unreadable"));
        QVERIFY(QFileInfo(asDir).isDir());

        const QString locked = dir.filePath(QStringLiteral("mimeapps.list"));
        const QByteArray mine("[Default Applications]\ntext/html=firefox.desktop\n");
        {
            QFile f(locked);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(mine);
        }
        QFile::setPermissions(locked, QFileDevice::Permissions());
        if (QFile probe(locked); probe.open(QIODevice::ReadOnly))
            QSKIP("running with privileges that read a mode-000 file");
        QCOMPARE(claimDefaultHandler(locked, mime, id, {}),
                 QStringLiteral("skipped: mimeapps.list unreadable"));
        QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QFile f(locked);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), mine);
    }

    void anotherHandlerOrAnExistingDefaultIsKept()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString mime = QStringLiteral("x-scheme-handler/matrix");
        const QString id = QStringLiteral("lightning.desktop");
        const QString path = dir.filePath(QStringLiteral("cfg/mimeapps.list"));

        int asked = 0;
        QCOMPARE(claimDefaultHandler(path, mime, id, [&asked] { ++asked; return true; }),
                 QStringLiteral("kept another handler"));
        QCOMPARE(asked, 1);
        QVERIFY(!QFileInfo::exists(path));

        QCOMPARE(claimDefaultHandler(path, mime, id, [&asked] { ++asked; return false; }),
                 QStringLiteral("registered"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(),
                 QByteArray("[Default Applications]\nx-scheme-handler/matrix=lightning.desktop;\n"));
        f.close();

        QCOMPARE(claimDefaultHandler(path, mime, id, [&asked] { ++asked; return true; }),
                 QStringLiteral("kept the existing default"));
        QCOMPARE(asked, 2);
    }

    void theMatrixSchemeDefaultIsAddedOnlyWhenNoneIsSet()
    {
        const QString mime = QStringLiteral("x-scheme-handler/matrix");
        const QString id = QStringLiteral("lightning.desktop");
        QCOMPARE(withDefaultHandler(QString(), mime, id),
                 QStringLiteral("[Default Applications]\nx-scheme-handler/matrix=lightning.desktop;\n"));
        QCOMPARE(withDefaultHandler(QStringLiteral("[Added Associations]\ntext/plain=a.desktop;\n"), mime, id),
                 QStringLiteral("[Added Associations]\ntext/plain=a.desktop;\n\n"
                                "[Default Applications]\nx-scheme-handler/matrix=lightning.desktop;\n"));
        QCOMPARE(withDefaultHandler(QStringLiteral("[Default Applications]\ntext/html=f.desktop\n"), mime, id),
                 QStringLiteral("[Default Applications]\nx-scheme-handler/matrix=lightning.desktop;\ntext/html=f.desktop\n"));
        QVERIFY(withDefaultHandler(QStringLiteral("[Default Applications]\nx-scheme-handler/matrix = element.desktop\n"),
                                   mime, id).isEmpty());
        // Only the Default group counts; an association elsewhere is not a choice of default.
        QVERIFY(!withDefaultHandler(QStringLiteral("[Added Associations]\nx-scheme-handler/matrix=e.desktop;\n"),
                                    mime, id).isEmpty());
    }

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
