// Source-scan contracts for the widget dialogs (C29-F01, C29-F03). The dialogs
// need the whole `app` context so they are scanned, not loaded.

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
}

class WidgetDialogFocusContractTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    // F01: AppButton is TabFocus, so a mouse click leaves nothing for Popup to
    // restore; the opener must be able to name itself and get focus on close.
    void bothDialogsHandFocusBackToTheirOpener()
    {
        for (const char *name : {"AddWidgetDialog.qml", "WidgetOpenSheet.qml"}) {
            const QString s = read(QString::fromLatin1(name));
            QVERIFY2(!s.isEmpty(), name);
            QVERIFY2(s.contains(QStringLiteral("property Item returnFocusItem")), name);
            const int c = s.indexOf(QStringLiteral("onClosed:"));
            QVERIFY2(c > 0, name);
            // Deferred, so Popup's own restore cannot overwrite it.
            const QString h = s.mid(c, 700);
            QVERIFY2(h.contains(QStringLiteral("Qt.callLater")), name);
            QVERIFY2(h.contains(QStringLiteral("returnFocusItem.forceActiveFocus")), name);
        }
    }

    // F01 wiring: the properties above do nothing until the panel names each
    // opener. This FAILS until the RoomInfoPanel hunk (NEEDS FROM OTHERS)
    // lands, which is the point: it is the half that makes the fix real.
    void roomInfoPanelNamesTheOpenerOfEveryWidgetDialog()
    {
        const QString s = read(QStringLiteral("RoomInfoPanel.qml"));
        QVERIFY(!s.isEmpty());
        // Row buttons live in a Repeater, so they assign themselves
        // (`widgetSheet.returnFocusItem = this`); the Add dialog is declared
        // with a binding to its single button.
        QVERIFY(s.contains(QStringLiteral("widgetSheet.returnFocusItem = ")));
        QVERIFY(s.contains(QStringLiteral("removeWidgetConfirm.returnFocusItem = ")));
        const int at = s.indexOf(QStringLiteral("id: addWidgetDialog"));
        QVERIFY(at > 0);
        QVERIFY(s.mid(at, 300).contains(QStringLiteral("returnFocusItem:")));
    }

    // F03: Return in either field is the Add button's action, through one
    // function so the button's enabled conditions still apply.
    void returnInTheFieldsSubmitsThroughTheSameGate()
    {
        const QString s = read(QStringLiteral("AddWidgetDialog.qml"));
        QVERIFY(!s.isEmpty());
        QCOMPARE(s.count(QStringLiteral("Keys.onReturnPressed: root.submit()")), 2);
        QCOMPARE(s.count(QStringLiteral("Keys.onEnterPressed: root.submit()")), 2);
        const int fn = s.indexOf(QStringLiteral("function submit()"));
        QVERIFY(fn > 0);
        const int end = s.indexOf(QStringLiteral("function openDialog"), fn);
        QVERIFY(end > fn);
        const QString body = s.mid(fn, end - fn);
        QVERIFY(body.contains(QStringLiteral("root.urlOk")));
        QVERIFY(body.contains(QStringLiteral("canManage")));
        QVERIFY(s.contains(QStringLiteral("onClicked: root.submit()")));
    }
};

QTEST_APPLESS_MAIN(WidgetDialogFocusContractTest)
#include "WidgetDialogFocusContractTest.moc"
