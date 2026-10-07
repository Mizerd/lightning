// savenaming: the name an attachment is saved under, its extension, the
// "name (1).ext" numbering of Element's download flow, and which files are
// never opened from Lightning.
//
// Reported 2026-10-06: "downloading files doesn't prefill their names or file
// formats". The save dialog was handed "file:///<leaf>" with no folder and no
// type, and a name the sender sent without an extension ("photo", a voice
// message's "Voice message") was offered without one.

#include "app/SaveNaming.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace savenaming;

class SaveNamingTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void theMimeTableGivesTheUsualExtension_data()
    {
        QTest::addColumn<QString>("mime");
        QTest::addColumn<QString>("ext");
        QTest::newRow("jpeg") << "image/jpeg" << "jpg";
        QTest::newRow("png") << "image/png" << "png";
        QTest::newRow("webp") << "image/webp" << "webp";
        QTest::newRow("svg") << "image/svg+xml" << "svg";
        QTest::newRow("mp4") << "video/mp4" << "mp4";
        QTest::newRow("quicktime") << "video/quicktime" << "mov";
        QTest::newRow("voice") << "audio/ogg" << "ogg";
        QTest::newRow("mpeg audio") << "audio/mpeg" << "mp3";
        QTest::newRow("pdf") << "application/pdf" << "pdf";
        QTest::newRow("docx")
            << "application/vnd.openxmlformats-officedocument."
               "wordprocessingml.document"
            << "docx";
        QTest::newRow("text with charset") << "text/plain; charset=utf-8"
                                           << "txt";
        QTest::newRow("upper case") << "IMAGE/PNG" << "png";
        QTest::newRow("generic") << "application/octet-stream" << "";
        QTest::newRow("empty") << "" << "";
    }
    void theMimeTableGivesTheUsualExtension()
    {
        QFETCH(QString, mime);
        QFETCH(QString, ext);
        QCOMPARE(extensionForMime(mime), ext);
    }

    void onlyRealExtensionsCount()
    {
        QVERIFY(looksLikeExtension(QStringLiteral("pdf")));
        QVERIFY(looksLikeExtension(QStringLiteral("7z")));
        QVERIFY(!looksLikeExtension(QStringLiteral("06")));
        QVERIFY(!looksLikeExtension(QStringLiteral("final draft")));
        QVERIFY(!looksLikeExtension(QString()));
        QCOMPARE(extensionOf(QStringLiteral("report.PDF")), QStringLiteral("pdf"));
        QCOMPARE(extensionOf(QStringLiteral("logs.tar.gz")),
                 QStringLiteral("tar.gz"));
        // A date is not an extension.
        QCOMPARE(extensionOf(QStringLiteral("Screenshot 2026.10.06")), QString());
        QCOMPARE(extensionOf(QStringLiteral("photo")), QString());
    }

    // The prefill: the sender's name, sanitized, with the type's extension
    // when it has none.
    void theSuggestedNameKeepsTheNameAndAddsTheMissingExtension_data()
    {
        QTest::addColumn<QString>("raw");
        QTest::addColumn<QString>("mime");
        QTest::addColumn<QString>("expected");
        QTest::newRow("named") << "report.pdf" << "application/pdf"
                               << "report.pdf";
        QTest::newRow("no extension") << "photo" << "image/jpeg" << "photo.jpg";
        QTest::newRow("voice message") << "Voice message" << "audio/ogg"
                                       << "Voice message.ogg";
        QTest::newRow("dotted date") << "Screenshot 2026.10.06" << "image/png"
                                     << "Screenshot 2026.10.06.png";
        // A sender's own extension is kept even when the type disagrees: the
        // name is what they chose, and the type is no more trustworthy.
        QTest::newRow("sender extension kept") << "scan.png" << "image/jpeg"
                                               << "scan.png";
        QTest::newRow("no name, image") << "" << "image/webp" << "image.webp";
        QTest::newRow("no name, video") << "" << "video/mp4" << "video.mp4";
        QTest::newRow("no name, unknown") << "" << "" << "file";
        QTest::newRow("path stripped") << "../../etc/passwd" << "text/plain"
                                       << "passwd.txt";
        QTest::newRow("backslashes") << "..\\..\\evil.exe" << ""
                                     << "evil.exe";
        QTest::newRow("hidden file") << ".bashrc" << "" << "bashrc";
        QTest::newRow("trailing dot") << "setup.exe." << "" << "setup.exe";
        QTest::newRow("windows reserved chars") << "a<b>c:d|e?f*.txt" << ""
                                                << "a_b_c_d_e_f_.txt";
        QTest::newRow("device name") << "con.txt" << "" << "file-con.txt";
    }
    void theSuggestedNameKeepsTheNameAndAddsTheMissingExtension()
    {
        QFETCH(QString, raw);
        QFETCH(QString, mime);
        QFETCH(QString, expected);
        QCOMPARE(suggestedFileName(raw, mime), expected);
    }

    void aLongNameKeepsItsExtension()
    {
        const QString name =
            suggestedFileName(QString(400, QLatin1Char('x')) + QStringLiteral(".pdf"),
                              QStringLiteral("application/pdf"));
        QVERIFY(name.size() <= 120);
        QVERIFY(name.endsWith(QStringLiteral(".pdf")));
        const QString bare = suggestedFileName(QString(400, QLatin1Char('y')),
                                               QStringLiteral("image/png"));
        QVERIFY(bare.size() <= 120);
        QVERIFY(bare.endsWith(QStringLiteral(".png")));
    }

    // "never let the user lose the extension silently"
    void anExtensionTheUserDeletesComesBack()
    {
        QCOMPARE(ensureExtension(QStringLiteral("holiday"), QStringLiteral("jpg")),
                 QStringLiteral("holiday.jpg"));
        QCOMPARE(ensureExtension(QStringLiteral("holiday."), QStringLiteral("jpg")),
                 QStringLiteral("holiday.jpg"));
        // A different extension is a choice, not a loss.
        QCOMPARE(ensureExtension(QStringLiteral("holiday.png"),
                                 QStringLiteral("jpg")),
                 QStringLiteral("holiday.png"));
        QCOMPARE(ensureExtension(QStringLiteral("holiday.jpg"),
                                 QStringLiteral("jpg")),
                 QStringLiteral("holiday.jpg"));
        QCOMPARE(ensureExtension(QStringLiteral("notes"), QString()),
                 QStringLiteral("notes"));
    }

    void duplicatesAreNumberedTheWayBrowsersDo()
    {
        QCOMPARE(numberedName(QStringLiteral("report.pdf"), 0),
                 QStringLiteral("report.pdf"));
        QCOMPARE(numberedName(QStringLiteral("report.pdf"), 1),
                 QStringLiteral("report (1).pdf"));
        QCOMPARE(numberedName(QStringLiteral("report.pdf"), 12),
                 QStringLiteral("report (12).pdf"));
        QCOMPARE(numberedName(QStringLiteral("logs.tar.gz"), 2),
                 QStringLiteral("logs (2).tar.gz"));
        QCOMPARE(numberedName(QStringLiteral("README"), 1),
                 QStringLiteral("README (1)"));

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QCOMPARE(uniqueFileName(dir.path(), QStringLiteral("a.png")),
                 QStringLiteral("a.png"));
        for (const char *taken : {"a.png", "a (1).png"}) {
            QFile f(QDir(dir.path()).filePath(QString::fromLatin1(taken)));
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
        QCOMPARE(uniqueFileName(dir.path(), QStringLiteral("a.png")),
                 QStringLiteral("a (2).png"));
    }

    void programsAndScriptsAreRiskyToOpen_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("mime");
        QTest::addColumn<bool>("risky");
        for (const char *n : {"setup.exe", "INSTALL.MSI", "run.bat", "x.cmd",
                              "a.ps1", "b.vbs", "c.js", "d.jar", "e.sh",
                              "app.desktop", "Tool.AppImage", "f.run", "g.bin",
                              "h.app", "i.dmg", "j.pkg", "k.deb", "l.rpm",
                              "m.apk", "n.scr", "o.com", "p.lnk", "q.py",
                              "r.command", "invoice.pdf.exe", "evil.exe.",
                              // active content in a browser
                              "page.html", "page.HTM", "a.xhtml", "b.mht",
                              "c.mhtml", "logo.svg",
                              // containers that shed mark-of-the-web
                              "disk.iso", "disk.img", "disk.vhd", "disk.vhdx"})
            QTest::newRow(n) << QString::fromLatin1(n) << QString() << true;
        QTest::newRow("no extension") << "README" << "" << true;
        QTest::newRow("exe by type") << "harmless.txt"
                                     << "application/x-msdownload" << true;
        QTest::newRow("script by type") << "notes.txt"
                                        << "application/x-shellscript" << true;
        QTest::newRow("html by type") << "notes.txt" << "text/html" << true;
        QTest::newRow("svg by type") << "picture.png" << "image/svg+xml"
                                     << true;
        for (const char *n : {"photo.jpg", "report.pdf", "song.mp3",
                              "clip.webm", "archive.zip", "notes.txt",
                              "sheet.xlsx", "logs.tar.gz"})
            QTest::newRow(n) << QString::fromLatin1(n) << QString() << false;
    }
    void programsAndScriptsAreRiskyToOpen()
    {
        QFETCH(QString, name);
        QFETCH(QString, mime);
        QFETCH(bool, risky);
        QCOMPARE(isRiskyToOpen(name, mime), risky);
    }

    // The Windows mark-of-the-web: Internet zone, and no URL (an
    // authenticated media URL must never reach the disk, CLAUDE.md §6).
    void theMarkOfTheWebIsTheInternetZoneWithNoUrl()
    {
        QCOMPARE(zoneIdentifierContent(),
                 QByteArray("[ZoneTransfer]\r\nZoneId=3\r\n"));
        QVERIFY(!zoneIdentifierContent().contains("Url"));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile file(QDir(dir.path()).filePath(QStringLiteral("a.zip")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("PK");
        file.close();
#if defined(Q_OS_WIN)
        // Best effort: a temp dir on a volume without streams may refuse.
        if (markAsDownloaded(file.fileName())) {
            QFile stream(file.fileName() + QStringLiteral(":Zone.Identifier"));
            QVERIFY(stream.open(QIODevice::ReadOnly));
            QCOMPARE(stream.readAll(), zoneIdentifierContent());
        }
#else
        QVERIFY(!markAsDownloaded(file.fileName()));
#endif
        // The file itself is never altered.
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("PK"));
    }

    // Element's desktop save dialog says what the file is, then offers all
    // files.
    void theSaveDialogFiltersNameTheType()
    {
        QCOMPARE(saveDialogFilters(QStringLiteral("report.pdf")),
                 (QStringList{QStringLiteral("PDF file (*.pdf)"),
                              QStringLiteral("All files (*)")}));
        QCOMPARE(saveDialogFilters(QStringLiteral("README")),
                 QStringList{QStringLiteral("All files (*)")});
    }

    // Reported 2026-10-07 from the Ubuntu 26 snap: a download saved through
    // the portal's Save dialog said "Saved to fd37b80a", the document
    // portal's id for the file, and Show in folder opened nothing. The
    // Flatpak takes the same path.
    void documentPortalPathsAreRecognised_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("runtimeDir");
        QTest::addColumn<bool>("portal");
        QTest::addColumn<QString>("docId");
        // The folder named with no host path, and with the real one
        // ("/home/someone/Downloads/report.pdf").
        QTest::addColumn<QString>("folder");
        QTest::addColumn<QString>("folderWithHost");

        // A Snap's runtime dir is its own; the doc mount is not under it.
        QTest::newRow("snap")
            << "/run/user/1000/doc/fd37b80a/report.pdf"
            << "/run/user/1000/snap.lightning" << true << "fd37b80a" << ""
            << "Downloads";
        QTest::newRow("flatpak")
            << "/run/user/1000/doc/fd37b80a/report.pdf" << "/run/user/1000"
            << true << "fd37b80a" << "" << "Downloads";
        // MediaBridge canonicalizes the parent, and in a Flatpak
        // /run/user/<uid>/doc is a symlink to this.
        QTest::newRow("flatpak, resolved")
            << "/run/flatpak/doc/fd37b80a/report.pdf" << "/run/user/1000"
            << true << "fd37b80a" << "" << "Downloads";
        QTest::newRow("host view, by app")
            << "/run/user/1000/doc/by-app/snap.lightning/fd37b80a/report.pdf"
            << "/run/user/1000" << true << "fd37b80a" << "" << "Downloads";
        QTest::newRow("runtime dir elsewhere")
            << "/tmp/rt/doc/abc123/notes.txt" << "/tmp/rt/" << true
            << "abc123" << "" << "Downloads";
        QTest::newRow("no runtime dir")
            << "/run/user/1000/doc/fd37b80a/report.pdf" << "" << true
            << "fd37b80a" << "" << "Downloads";
        QTest::newRow("the document's folder")
            << "/run/user/1000/doc/fd37b80a" << "" << true << "fd37b80a"
            << "" << "Downloads";
        QTest::newRow("the mount itself")
            << "/run/user/1000/doc" << "" << true << "" << "" << "Downloads";
        // Not the portal: the parent folder, whatever host path is given.
        QTest::newRow("ordinary file")
            << "/home/someone/Downloads/report.pdf" << "/run/user/1000"
            << false << "" << "Downloads" << "Downloads";
        QTest::newRow("docs, not doc")
            << "/run/user/1000/docs/report.pdf" << "/run/user/1000" << false
            << "" << "docs" << "docs";
        // The old test ("/run/user/" and "/doc/" anywhere) took this one.
        QTest::newRow("a share with a doc folder")
            << "/run/user/1000/gvfs/smb-share:server=nas,share=files/doc/a.pdf"
            << "/run/user/1000" << false << "" << "doc" << "doc";
        QTest::newRow("not a uid")
            << "/run/user/someone/doc/x/a.pdf" << "" << false << "" << "x"
            << "x";
        QTest::newRow("dot-dot out of the mount")
            << "/run/user/1000/doc/../Downloads/a.pdf" << "" << false << ""
            << "Downloads" << "Downloads";
        QTest::newRow("empty") << "" << "/run/user/1000" << false << "" << ""
                               << "";
    }
    void documentPortalPathsAreRecognised()
    {
        QFETCH(QString, path);
        QFETCH(QString, runtimeDir);
        QFETCH(bool, portal);
        QFETCH(QString, docId);
        QFETCH(QString, folder);
        QFETCH(QString, folderWithHost);
        QCOMPARE(isDocumentPortalPath(path, runtimeDir), portal);
        QCOMPARE(documentPortalId(path, runtimeDir), docId);
        QCOMPARE(savedFolderName(path, QString(), runtimeDir), folder);
        QCOMPARE(savedFolderName(path,
                                 QStringLiteral(
                                     "/home/someone/Downloads/report.pdf"),
                                 runtimeDir),
                 folderWithHost);
    }

    // A "host path" that is itself in the mount, or relative, names nothing
    // better than the id did: still no folder.
    void aHostPathInsideTheMountIsNotARealFolder()
    {
        const QString path = QStringLiteral("/run/user/1000/doc/fd37b80a/a.pdf");
        QCOMPARE(savedFolderName(path,
                                 QStringLiteral("/run/user/1000/doc/fd37b80a/a.pdf"),
                                 QStringLiteral("/run/user/1000")),
                 QString());
        QCOMPARE(savedFolderName(path, QStringLiteral("/run/flatpak/doc/x/a.pdf"),
                                 QString()),
                 QString());
        QCOMPARE(savedFolderName(path, QStringLiteral("Downloads/a.pdf"),
                                 QString()),
                 QString());
    }
};

QTEST_GUILESS_MAIN(SaveNamingTest)
#include "SaveNamingTest.moc"
