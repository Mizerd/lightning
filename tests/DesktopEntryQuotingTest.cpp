#include "app/DesktopEntryQuoting.h"

#include <QtTest/QtTest>

#include <QTemporaryDir>

using lightning::desktop_entry::quoteExecArgument;
using lightning::desktop_entry::claimDefaultHandler;
using lightning::desktop_entry::queryNamesAnotherHandler;
using lightning::desktop_entry::withDefaultHandler;
using lightning::desktop_entry::withGeneratedMarker;
using lightning::desktop_entry::isUntouchedGeneratedEntry;

namespace {

// What 0.10.5 wrote, captured from the published AppImage.
QString releasedEntry(const QString &comment, const QString &exec,
                      const QString &tryExecLine)
{
    return QStringLiteral("[Desktop Entry]\n"
                          "Type=Application\n"
                          "Name=Lightning\n"
                          "GenericName=Matrix Client\n"
                          "Comment=%1\n"
                          "Terminal=false\n"
                          "Categories=Network;Chat;InstantMessaging;\n"
                          "Keywords=Matrix;Chat;Messaging;\n"
                          "StartupNotify=true\n"
                          "Exec=%2 --backend=rust\n"
                          "%3"
                          "Icon=lightning\n"
                          "StartupWMClass=lightning-matrix\n"
                          "X-AppImage-Version=0.10.5\n"
                          "X-Lightning-Generated=true\n")
        .arg(comment, exec, tryExecLine);
}

}

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

    void aFailedXdgMimeQueryFailsClosed()
    {
        const QString ours = QStringLiteral("lightning.desktop");
        QVERIFY(queryNamesAnotherHandler(false, 0, QString(), ours));
        QVERIFY(queryNamesAnotherHandler(true, 1, QString(), ours));
        QVERIFY(queryNamesAnotherHandler(true, 127, ours, ours));
        QVERIFY(queryNamesAnotherHandler(true, 0, QStringLiteral("element.desktop\n"), ours));
        QVERIFY(!queryNamesAnotherHandler(true, 0, QString(), ours));
        QVERIFY(!queryNamesAnotherHandler(true, 0, QStringLiteral("lightning.desktop\n"), ours));
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

    void aMarkedEntryIsOursOnlyUntilTheUserEditsIt()
    {
        const QString body = QStringLiteral(
            "[Desktop Entry]\nType=Application\nName=Lightning\n"
            "Exec=\"/opt/Lightning.AppImage\" --backend=rust %u\n"
            "Icon=lightning\nStartupWMClass=matrix-client\n");
        const QString entry = withGeneratedMarker(body);
        QVERIFY(entry.startsWith(body));
        QVERIFY(entry.endsWith(QLatin1Char('\n')));
        QVERIFY(isUntouchedGeneratedEntry(entry));
        QCOMPARE(withGeneratedMarker(body), entry);

        QString wmClass = entry;
        wmClass.replace(QStringLiteral("StartupWMClass=matrix-client"),
                        QStringLiteral("StartupWMClass=lightning"));
        QVERIFY(!isUntouchedGeneratedEntry(wmClass));
        QVERIFY(!isUntouchedGeneratedEntry(
            entry + QStringLiteral("NoDisplay=true\n")));
        QVERIFY(!isUntouchedGeneratedEntry(
            QStringLiteral("# mine\n") + entry));
        QVERIFY(!isUntouchedGeneratedEntry(entry.chopped(1)));
        QString trueMarker = body + QStringLiteral("X-Lightning-Generated=true\n");
        QVERIFY(!isUntouchedGeneratedEntry(trueMarker));
        QVERIFY(!isUntouchedGeneratedEntry(body));
        QVERIFY(!isUntouchedGeneratedEntry(QString()));
    }

    void anEntryAnEarlierReleaseWroteIsStillOurs()
    {
        const QString path = QStringLiteral("/home/u/Apps/Lightning-0.10.5-x86_64.AppImage");
        const QString tryExec = QStringLiteral("TryExec=%1\n").arg(path);
        QVERIFY(isUntouchedGeneratedEntry(releasedEntry(
            QStringLiteral("Chat and calls on Matrix"),
            QStringLiteral("\"%1\"").arg(path), tryExec)));
        QVERIFY(isUntouchedGeneratedEntry(releasedEntry(
            QStringLiteral("Native Qt Matrix chat client"),
            QStringLiteral("\"%1\"").arg(path), tryExec)));
        QVERIFY(isUntouchedGeneratedEntry(releasedEntry(
            QStringLiteral("Chat and calls on Matrix"),
            QStringLiteral("\"/home/u/My Apps/L.AppImage\""), QString())));
        QVERIFY(isUntouchedGeneratedEntry(releasedEntry(
            QStringLiteral("Chat and calls on Matrix"),
            QStringLiteral("\"/opt/a\\\\\\\\b/100%%/L.AppImage\""),
            QStringLiteral("TryExec=/opt/a\\\\b/100%/L.AppImage\n"))));
    }

    void anEarlierReleasesEntryTheUserEditedIsTheirs()
    {
        const QString path = QStringLiteral("/home/u/Apps/Lightning.AppImage");
        const QString exec = QStringLiteral("\"%1\"").arg(path);
        const QString tryExec = QStringLiteral("TryExec=%1\n").arg(path);
        const QString original = releasedEntry(
            QStringLiteral("Chat and calls on Matrix"), exec, tryExec);
        QVERIFY(isUntouchedGeneratedEntry(original));

        QString wmClass = original;
        wmClass.replace(QStringLiteral("StartupWMClass=lightning-matrix"),
                        QStringLiteral("StartupWMClass=matrix-client"));
        QVERIFY(!isUntouchedGeneratedEntry(wmClass));
        QString args = original;
        args.replace(QStringLiteral("--backend=rust\n"),
                     QStringLiteral("--backend=rust --console\n"));
        QVERIFY(!isUntouchedGeneratedEntry(args));
        QString name = original;
        name.replace(QStringLiteral("Name=Lightning"), QStringLiteral("Name=Chat"));
        QVERIFY(!isUntouchedGeneratedEntry(name));
        QVERIFY(!isUntouchedGeneratedEntry(releasedEntry(
            QStringLiteral("Chat and calls on Matrix"), exec, QString())));
        QVERIFY(!isUntouchedGeneratedEntry(releasedEntry(
            QStringLiteral("Chat and calls on Matrix"), exec,
            QStringLiteral("TryExec=/elsewhere\n"))));
        QVERIFY(!isUntouchedGeneratedEntry(releasedEntry(
            QStringLiteral("My comment"), exec, tryExec)));
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
