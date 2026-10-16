// Source-scan contracts for three small break-it fixes whose components cannot
// be loaded standalone (they need the whole `app` context): the shortcut row's
// stale conflict error (B093), the rooms column's saved width (B115) and the
// event-source dialog's long lines (B136). Each case asserts the property the
// fix introduced, so removing it fails the case.

#include <QFile>
#include <QtTest>

namespace {
QString read(const QString &name)
{
    QFile f(QStringLiteral(QML_DIR "/") + name);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

// Body of the first `{ ... }` opening at or after `from`.
QString bracedBody(const QString &s, int from)
{
    const int open = s.indexOf(QLatin1Char('{'), from);
    if (open < 0)
        return {};
    int depth = 0;
    for (int i = open; i < s.size(); ++i) {
        if (s.at(i) == QLatin1Char('{')) ++depth;
        else if (s.at(i) == QLatin1Char('}') && --depth == 0)
            return s.mid(open, i - open + 1);
    }
    return {};
}
}

class KeyboardFocusWidgetsContractTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    // B093: Escape, Cancel and focus loss all end a capture through
    // endCapture(); the rejected chord's message must go with it.
    void endingACaptureClearsTheConflictError()
    {
        const QString row = read(QStringLiteral("ShortcutRow.qml"));
        QVERIFY(!row.isEmpty());
        const int fn = row.indexOf(QStringLiteral("function endCapture()"));
        QVERIFY(fn > 0);
        const QString body = bracedBody(row, fn);
        QVERIFY(body.contains(QStringLiteral("row.capturing = false")));
        QVERIFY2(body.contains(QStringLiteral("row.captureError = \"\"")),
                 "endCapture() must clear captureError");
    }

    // B115: the rooms column is persisted only after a real divider drag. A
    // width the layout forced (window squeezed) is not a choice.
    void theRoomListWidthIsSavedOnlyAfterARealDrag()
    {
        const QString shell = read(QStringLiteral("MainScreen.qml"));
        QVERIFY(!shell.isEmpty());
        const int panel = shell.indexOf(QStringLiteral("id: roomsPanel"));
        QVERIFY(panel > 0);
        const QString from = shell.mid(panel, 3500);
        QVERIFY2(from.contains(QStringLiteral(
            "onWidthChanged: if (!SplitView.view.resizing && widthSaver.dragged)")),
            "a layout-forced width change must not arm the saver");
        QVERIFY(from.contains(QStringLiteral("property bool dragged: false")));
        // `dragged` is raised only from the divider's own resizing edge.
        const int conn = from.indexOf(QStringLiteral("function onResizingChanged()"));
        QVERIFY(conn > 0);
        const QString handler = bracedBody(from, conn);
        QVERIFY(handler.contains(QStringLiteral("widthSaver.dragged = true")));
        // Spent after one save, and the binding restored (SplitView breaks it
        // while dragging).
        const int timer = from.indexOf(QStringLiteral("id: widthSaver"));
        QVERIFY(timer > 0);
        const QString saver = bracedBody(from, timer - 20);
        QVERIFY(saver.contains(QStringLiteral("dragged = false")));
        QVERIFY(saver.contains(QStringLiteral("Qt.binding")));
    }

    // B136: JSON lines wrap at the viewport instead of being clipped behind a
    // faded horizontal bar.
    void eventSourceJsonWrapsInsteadOfClipping()
    {
        const QString dlg = read(QStringLiteral("EventSourceDialog.qml"));
        QVERIFY(!dlg.isEmpty());
        const int te = dlg.indexOf(QStringLiteral("id: sourceText"));
        QVERIFY(te > 0);
        const QString body = bracedBody(dlg, dlg.lastIndexOf(QStringLiteral("TextEdit"), te));
        QVERIFY2(body.contains(QStringLiteral("wrapMode: TextEdit.WrapAnywhere")),
                 "long JSON lines must wrap");
        QVERIFY(body.contains(QStringLiteral("width: parent.width")));
        QVERIFY(body.contains(QStringLiteral("textFormat: TextEdit.PlainText")));
        QVERIFY(!dlg.contains(QStringLiteral("contentWidth: sourceText.paintedWidth")));
    }
};

QTEST_GUILESS_MAIN(KeyboardFocusWidgetsContractTest)
#include "KeyboardFocusWidgetsContractTest.moc"
