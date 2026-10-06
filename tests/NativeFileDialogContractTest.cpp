// Every file chooser in the QML tree is the desktop's own (app.files through
// NativeFileDialog.qml), never QtQuick.Dialogs' FileDialog / FolderDialog,
// which draws a non-native imitation whenever the Qt platform theme has no
// native dialog — what Rokas photographed on 2026-10-06 ("this menu super
// sucks"). And the download notice offers Open only where it is allowed.
//
// A source contract, because a new chooser written the old way looks fine on
// a desktop whose Qt plugin happens to supply a native dialog. Every sweep
// asserts how much it scanned.

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

/// Comments removed, so prose about the old dialog neither trips nor
/// satisfies a check.
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

} // namespace

class NativeFileDialogContractTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void noQtQuickDialogsChooserIsLeft()
    {
        const QRegularExpression importDialogs(
            QStringLiteral("(?m)^\\s*import\\s+QtQuick\\.Dialogs\\b"));
        // An element declaration, not NativeFileDialog: no letter before it.
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
        // A QtObject, so it never takes part in the layout it sits in.
        QVERIFY(wrapper.contains(QRegularExpression(
            QStringLiteral("(?m)^QtObject\\s*\\{"))));

        // The choosers this tree has: attach (composer x3, thread), avatars
        // and banners (x10), sticker, background, font, app icon, key import,
        // room export. Counted so a deletion is a decision, not a drift.
        const QRegularExpression use(QStringLiteral("\\bNativeFileDialog\\s*\\{"));
        int uses = 0;
        for (const QString &path : qmlFiles()) {
            const QString source = withoutComments(read(path));
            uses += int(source.count(use));
        }
        QVERIFY2(uses >= 19, qPrintable(QStringLiteral("only %1 choosers").arg(uses)));
    }

    // The old save dialog was seeded with "file:///<leaf>": no folder, no
    // type, and the native dialogs ignored it. app.downloads owns saving now.
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

    // Element's "Download completed" toast, with the one difference that a
    // file able to run code is never opened from here.
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

        // It lives in the chat column's safe area, not in the window's
        // bottom-left corner over the account and settings buttons.
        const QString main = withoutComments(read(QStringLiteral(QML_DIR "/Main.qml")));
        QVERIFY(!main.isEmpty());
        QVERIFY2(!main.contains(QStringLiteral("activeDownloadsCard")),
                 "the window-corner Downloading card is back");
        const QString pane =
            withoutComments(read(QStringLiteral(QML_DIR "/TimelinePane.qml")));
        QVERIFY(pane.contains(QRegularExpression(
            QStringLiteral("DownloadsCard\\s*\\{[^}]*safeArea:\\s*root\\.chatSafeArea"))));
    }
};

QTEST_GUILESS_MAIN(NativeFileDialogContractTest)
#include "NativeFileDialogContractTest.moc"
