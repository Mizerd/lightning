
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
    void bothDialogsHandFocusBackToTheirOpener()
    {
        for (const char *name : {"AddWidgetDialog.qml", "WidgetOpenSheet.qml"}) {
            const QString s = read(QString::fromLatin1(name));
            QVERIFY2(!s.isEmpty(), name);
            QVERIFY2(s.contains(QStringLiteral("property Item returnFocusItem")), name);
            const int c = s.indexOf(QStringLiteral("onClosed:"));
            QVERIFY2(c > 0, name);
            const QString h = s.mid(c, 700);
            QVERIFY2(h.contains(QStringLiteral("Qt.callLater")), name);
            QVERIFY2(h.contains(QStringLiteral("returnFocusItem.forceActiveFocus")), name);
        }
    }

    void roomInfoPanelNamesTheOpenerOfEveryWidgetDialog()
    {
        const QString s = read(QStringLiteral("RoomInfoPanel.qml"));
        QVERIFY(!s.isEmpty());
        QVERIFY(s.contains(QStringLiteral("widgetSheet.returnFocusItem = ")));
        QVERIFY(s.contains(QStringLiteral("removeWidgetConfirm.returnFocusItem = ")));
        const int at = s.indexOf(QStringLiteral("id: addWidgetDialog"));
        QVERIFY(at > 0);
        QVERIFY(s.mid(at, 300).contains(QStringLiteral("returnFocusItem:")));
    }

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
