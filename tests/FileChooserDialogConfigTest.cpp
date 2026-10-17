// The QFileDialog the Qt route shows (FileChooser::configureQtDialog), asked
// for real: a save request must tell the dialog the suggested name's own
// extension as its default suffix, so a name typed without one still gets it.
//
// FileChooserTest injects a runner and so never reaches the real
// configuration; this builds a real widget dialog (offscreen, never shown,
// never opened) and reads back what was configured. A guiless test cannot
// build a QFileDialog, hence its own executable.

#include "app/FileChooser.h"

#include <QFileDialog>
#include <QtTest/QtTest>

class FileChooserDialogConfigTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aSaveDialogGetsTheSuggestedNamesExtensionAsItsDefaultSuffix_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("suffix");
        QTest::newRow("pdf") << "report.pdf" << "pdf";
        QTest::newRow("compound") << "backup.tar.gz" << "tar.gz";
        QTest::newRow("unknown suffix: none forced") << "weird.xyz" << "";
        QTest::newRow("no extension") << "file" << "";
        QTest::newRow("empty name") << "" << "";
    }
    void aSaveDialogGetsTheSuggestedNamesExtensionAsItsDefaultSuffix()
    {
        QFETCH(QString, name);
        QFETCH(QString, suffix);
        QFileDialog dialog;
        FileChooser::Request request;
        request.mode = FileChooser::Mode::SaveFile;
        request.currentName = name;
        FileChooser::configureQtDialog(dialog, request, true);
        QCOMPARE(dialog.acceptMode(), QFileDialog::AcceptSave);
        QCOMPARE(dialog.fileMode(), QFileDialog::AnyFile);
        QCOMPARE(dialog.defaultSuffix(), suffix);
        QVERIFY(dialog.testOption(QFileDialog::DontUseNativeDialog));
    }
    void anOpenDialogForcesNoSuffix()
    {
        QFileDialog dialog;
        FileChooser::Request request;
        request.mode = FileChooser::Mode::OpenFile;
        request.currentName = QStringLiteral("report.pdf");
        FileChooser::configureQtDialog(dialog, request, false);
        QCOMPARE(dialog.acceptMode(), QFileDialog::AcceptOpen);
        QVERIFY(dialog.defaultSuffix().isEmpty());
        QVERIFY(!dialog.testOption(QFileDialog::DontUseNativeDialog));
    }
};

QTEST_MAIN(FileChooserDialogConfigTest)
#include "FileChooserDialogConfigTest.moc"
