
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QtTest/QtTest>

namespace {

QString read(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

QString withoutComments(const QString &source)
{
    QString out = source;
    out.remove(QRegularExpression(QStringLiteral("/\\*.*?\\*/"),
                                  QRegularExpression::DotMatchesEverythingOption));
    out.remove(QRegularExpression(QStringLiteral("(?m)^\\s*//.*$")));
    out.remove(QRegularExpression(QStringLiteral("(?m)\\s//[^\"'\\n]*$")));
    return out;
}

QStringList qmlFiles()
{
    QStringList files;
    QDirIterator it(QStringLiteral(QML_DIR), {QStringLiteral("*.qml")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files << it.next();
    files.sort();
    return files;
}

}

class NativeFileDialogContractTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void noQtQuickDialogsChooserIsLeft()
    {
        const QRegularExpression importDialogs(
            QStringLiteral("(?m)^\\s*import\\s+QtQuick\\.Dialogs\\b"));
        const QRegularExpression element(
            QStringLiteral("(?<![A-Za-z_.])(FileDialog|FolderDialog)\\s*\\{"));
        const QStringList files = qmlFiles();
        QVERIFY2(files.size() > 50, "the QML tree was not found");
        QStringList offenders;
        for (const QString &path : files) {
            const QString source = withoutComments(read(path));
            if (importDialogs.match(source).hasMatch()
                || element.match(source).hasMatch())
                offenders << QFileInfo(path).fileName();
        }
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral("QtQuick.Dialogs chooser in: %1 — "
                                           "use NativeFileDialog (app.files)")
                                .arg(offenders.join(QStringLiteral(", ")))));
    }

    void everyChooserGoesThroughAppFiles()
    {
        const QString wrapper =
            withoutComments(read(QStringLiteral(QML_DIR "/NativeFileDialog.qml")));
        QVERIFY2(!wrapper.isEmpty(), "NativeFileDialog.qml is missing");
        QVERIFY(wrapper.contains(QStringLiteral("app.files.open(")));
        QVERIFY(wrapper.contains(QRegularExpression(
            QStringLiteral("(?m)^QtObject\\s*\\{"))));

        const QRegularExpression use(QStringLiteral("\\bNativeFileDialog\\s*\\{"));
        int uses = 0;
        for (const QString &path : qmlFiles()) {
            const QString source = withoutComments(read(path));
            uses += int(source.count(use));
        }
        QVERIFY2(uses >= 19, qPrintable(QStringLiteral("only %1 choosers").arg(uses)));
    }

    void noSaveDialogIsSeededWithAFolderlessUrl()
    {
        int scanned = 0;
        for (const QString &path : qmlFiles()) {
            const QString source = withoutComments(read(path));
            ++scanned;
            QVERIFY2(!source.contains(QStringLiteral("\"file:///\" + encodeURIComponent")),
                     qPrintable(QFileInfo(path).fileName()));
            QVERIFY2(!source.contains(QStringLiteral("mediaBridge.saveAs(")),
                     qPrintable(QStringLiteral("%1 saves around app.downloads")
                                    .arg(QFileInfo(path).fileName())));
        }
        QVERIFY(scanned > 50);
    }

    void theDownloadsCardOffersOpenOnlyWhereAllowed()
    {
        const QString card =
            withoutComments(read(QStringLiteral(QML_DIR "/DownloadsCard.qml")));
        QVERIFY2(!card.isEmpty(), "DownloadsCard.qml is missing");
        const int open = card.indexOf(QStringLiteral("\"downloadOpenButton\""));
        QVERIFY(open > 0);
        const QString openBlock = card.mid(open, 400);
        QVERIFY2(openBlock.contains(QStringLiteral("modelData.canOpen")),
                 "Open must be offered only when the controller allows it");
        QVERIFY(card.contains(QStringLiteral("\"downloadShowInFolderButton\"")));
        QVERIFY(card.contains(QStringLiteral("\"downloadRiskNotice\"")));
        QVERIFY(card.contains(QStringLiteral("app.downloads.showInFolder(")));

        const QString pane =
            withoutComments(read(QStringLiteral(QML_DIR "/TimelinePane.qml")));
        QVERIFY(pane.contains(QRegularExpression(
            QStringLiteral("PaneDownloadsCard\\s*\\{[^}]*safeArea:\\s*root\\.chatSafeArea"))));
        QVERIFY(pane.contains(QStringLiteral("DownloadNotices.begin(")));
        const QString fallback = withoutComments(
            read(QStringLiteral(QML_DIR "/PaneDownloadsCard.qml")));
        QVERIFY(fallback.contains(QStringLiteral("DownloadNotices.unclaimed(")));
        const QString row =
            withoutComments(read(QStringLiteral(QML_DIR "/MessageDelegate.qml")));
        QVERIFY(row.contains(QStringLiteral("item.mediaKey === model.mediaKey")));
        QVERIFY(row.contains(QStringLiteral("AttachmentDownloadNotice {")));
        QVERIFY(row.contains(QStringLiteral("eligible: root.rowOnScreen")));
        const QString notice = withoutComments(
            read(QStringLiteral(QML_DIR "/AttachmentDownloadNotice.qml")));
        QVERIFY(notice.contains(QStringLiteral("objectName: \"attachmentDownloadNotice\"")));
        QVERIFY(notice.contains(QStringLiteral("DownloadNotices.owner(")));
        QVERIFY(!row.contains(QStringLiteral("downloadRailWidth")));
        QVERIFY(!notice.contains(QStringLiteral("property bool beside")));
        QVERIFY(notice.contains(QStringLiteral("eligible && fits")));
        QVERIFY(row.contains(QStringLiteral(
            "ownBubble: root.bubbleMode && model.isOwn === true")));
        const int boxStart = row.indexOf(QStringLiteral("id: mediaBox"));
        QVERIFY(boxStart > 0);
        const QString box = row.mid(boxStart, row.indexOf(
            QStringLiteral("id: mediaLoader"), boxStart) - boxStart);
        QVERIFY2(!box.contains(QStringLiteral("attachmentNotice.height"))
                     && !box.contains(QStringLiteral("attachmentNotice.y"))
                     && !box.contains(QStringLiteral("attachmentNotice.items")),
                 "the download notice feeds the row's height again");
        const QString app = withoutComments(
            read(QStringLiteral(SRC_DIR "/app/AppController.cpp")));
        QVERIFY(app.count(QStringLiteral("m_downloads->clear()")) >= 2);
    }
};

QTEST_GUILESS_MAIN(NativeFileDialogContractTest)
#include "NativeFileDialogContractTest.moc"
