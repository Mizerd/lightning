// Source-scan contracts for the C68 round-1 fixes in ShortcutRow.qml. The row
// needs the whole `app` context to load, so each case asserts the property
// the fix introduced; removing the fix fails the case.

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
    // C68-F01: Tab to a Change button below the fold must scroll it in, and
    // arming a capture must reveal the chip.
    void focusAndCaptureScrollTheRowIntoView()
    {
        const QString r = readRow();
        QVERIFY(!r.isEmpty());
        QVERIFY(r.contains(QStringLiteral("function revealInFlickable(")));
        QVERIFY(r.contains(QStringLiteral("onActiveFocusChanged: if (activeFocus) row.revealInFlickable(changeButton)")));
        QVERIFY(r.contains(QStringLiteral("row.revealInFlickable(chip)")));
    }
    // C68-F02: a capture must end when the page is left or a click lands
    // outside the controls (neither moves focus).
    void aCaptureEndsWhenThePageIsLeftOrAClickLandsElsewhere()
    {
        const QString r = readRow();
        QVERIFY(r.contains(QStringLiteral("onVisibleChanged: if (!visible && row.capturing) row.endCapture()")));
        // The click catcher must be a MouseArea: a window TapHandler never
        // fires over an item that accepts the press. The behaviour itself is
        // ShortcutRowCaptureQmlTest's job.
        QVERIFY(!r.contains(QStringLiteral("TapHandler {")));
        QVERIFY(r.contains(QStringLiteral("MouseArea {")));
    }
    // C68-F07: Escape and a stored chord return focus to the Change button.
    void aFinishedCaptureHandsFocusBackToTheChangeButton()
    {
        const QString r = readRow();
        const int fn = r.indexOf(QStringLiteral("function finishCapture()"));
        QVERIFY(fn > 0);
        const int end = r.indexOf(QStringLiteral("changeButton.forceActiveFocus()"), fn);
        QVERIFY(end > fn && end - fn < 200);
        QVERIFY(r.contains(QStringLiteral("row.finishCapture()\n                    return")));
    }
    // C68-F06: Enter and keypad Enter activate both buttons. They are
    // AppButtons, which answer both keys at the type (C02); a handler of
    // their own would fire each press twice (MASTER-1 H1), so none remains.
    // ButtonKeyActivationQmlTest drives the type.
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
