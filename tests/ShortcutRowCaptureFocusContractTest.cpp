
#include <QFile>
#include <QtTest>

namespace {
QString readRow()
{
    QFile f(QStringLiteral(QML_DIR "/ShortcutRow.qml"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}
}

class ShortcutRowCaptureFocusContractTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void focusAndCaptureScrollTheRowIntoView()
    {
        const QString r = readRow();
        QVERIFY(!r.isEmpty());
        QVERIFY(r.contains(QStringLiteral("function revealInFlickable(")));
        QVERIFY(r.contains(QStringLiteral("onActiveFocusChanged: if (activeFocus) row.revealInFlickable(changeButton)")));
        QVERIFY(r.contains(QStringLiteral("row.revealInFlickable(chip)")));
    }
    void aCaptureEndsWhenThePageIsLeftOrAClickLandsElsewhere()
    {
        const QString r = readRow();
        QVERIFY(r.contains(QStringLiteral("onVisibleChanged: if (!visible && row.capturing) row.endCapture()")));
        QVERIFY(!r.contains(QStringLiteral("TapHandler {")));
        QVERIFY(r.contains(QStringLiteral("MouseArea {")));
    }
    void aFinishedCaptureHandsFocusBackToTheChangeButton()
    {
        const QString r = readRow();
        const int fn = r.indexOf(QStringLiteral("function finishCapture()"));
        QVERIFY(fn > 0);
        const int end = r.indexOf(QStringLiteral("changeButton.forceActiveFocus()"), fn);
        QVERIFY(end > fn && end - fn < 200);
        QVERIFY(r.contains(QStringLiteral("row.finishCapture()\n                    return")));
    }
    void enterActivatesTheButtons()
    {
        const QString r = readRow();
        for (const char *id : { "id: changeButton", "id: resetButton" }) {
            const int at = r.indexOf(QLatin1String(id));
            QVERIFY2(at > 0, id);
            const int open = r.lastIndexOf(QStringLiteral("AppButton {"), at);
            QVERIFY2(open > 0 && at - open < 300, id);
        }
        QVERIFY(!r.contains(QStringLiteral("activateOnReturn: false")));
        for (const char *k : { "Keys.onReturnPressed", "Keys.onEnterPressed" })
            QCOMPARE(r.count(QLatin1String(k)), 0);
    }
};

QTEST_APPLESS_MAIN(ShortcutRowCaptureFocusContractTest)
#include "ShortcutRowCaptureFocusContractTest.moc"
