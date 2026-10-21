// Source pins for the quick switcher's keyboard and pointer reach (C09 round 1).
// Each case fails on the tree before the fix: the strings it asks for are the
// fix itself. Behaviour was additionally checked with the prebuilt
// quick-switcher-modes-qml-test over a filesystem copy of the module.

#include <QtTest/QtTest>

#include <QFile>

class QuickSwitcherKeyboardSourceTest : public QObject
{
    Q_OBJECT

    QString m_src;

private slots:
    void initTestCase()
    {
        QFile f(QStringLiteral(QML_DIR "/QuickSwitcher.qml"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        m_src = QString::fromUtf8(f.readAll());
    }

    // C09-F01: both lists carry a MiddleClickScroller as a SIBLING.
    void bothListsHaveAMiddleClickScroller()
    {
        QCOMPARE(m_src.count(QStringLiteral("MiddleClickScroller {")), 2);
        QVERIFY(m_src.contains(QStringLiteral("view: resultList")));
        QVERIFY(m_src.contains(QStringLiteral("view: commandList")));
    }

    // C09-F02: any selection change scrolls the row into view, so the Tab
    // wrap-around (which sets currentIndex directly) is covered.
    void selectionChangeScrollsIntoView()
    {
        QCOMPARE(m_src.count(QStringLiteral("positionViewAtIndex(currentIndex, ListView.Contain)")), 2);
        QCOMPARE(m_src.count(QStringLiteral("onCurrentIndexChanged:")), 2);
    }

    // C09-F03: paging and ctrl+Home/End move the selection.
    void pagingKeysMoveTheSelection()
    {
        QVERIFY(m_src.contains(QStringLiteral("Qt.Key_PageDown")));
        QVERIFY(m_src.contains(QStringLiteral("Qt.Key_PageUp")));
        QVERIFY(m_src.contains(QStringLiteral("Qt.ControlModifier")));
    }

    // C09-F04: Enter with no match reaches the two fallback buttons.
    void enterWithNoMatchUsesTheFallbacks()
    {
        QVERIFY(m_src.contains(QStringLiteral("function acceptCurrent()")));
        QVERIFY(m_src.contains(QStringLiteral("searchMessagesButton.clicked()")));
        QVERIFY(m_src.contains(QStringLiteral("exploreRoomsButton.clicked()")));
        QCOMPARE(m_src.count(QStringLiteral("switcher.acceptCurrent()")), 2);
    }

    // C09-F05: elided names get a tooltip; the footer names "switch to" only
    // when another account exists.
    void tooltipAndHonestFooter()
    {
        // Names are attacker-chosen: the hover text goes through a plain-text
        // Label, never the attached ToolTip.text (auto-detects rich text and
        // fetches remote <img>).
        QVERIFY(!m_src.contains(QStringLiteral("ToolTip.text")));
        QCOMPARE(m_src.count(QStringLiteral("PlainTip {")), 2);
        const int comp = m_src.indexOf(QStringLiteral("component PlainTip: ToolTip"));
        QVERIFY(comp > 0);
        const QString body = m_src.mid(comp, 1100);
        QVERIFY(body.contains(QStringLiteral("textFormat: Text.PlainText")));
        QVERIFY(body.contains(QStringLiteral("contentItem: Label")));
        // A standalone ToolTip ignores `delay`; the hover delay is a Timer.
        QVERIFY(body.contains(QStringLiteral("Timer {")));
        QVERIFY(!body.contains(QStringLiteral("visible: shown")));
        QVERIFY(!body.contains(QStringLiteral("delay: 600")));
        QVERIFY(m_src.contains(QStringLiteral("hasOtherAccount")));
        // One shared height per list; the paging arithmetic mirrors the delegates.
        QVERIFY(m_src.contains(QStringLiteral("height: switcher.resultRowHeight")));
        QVERIFY(m_src.contains(QStringLiteral("height: switcher.commandRowHeight")));
    }
};

QTEST_GUILESS_MAIN(QuickSwitcherKeyboardSourceTest)
#include "QuickSwitcherKeyboardSourceTest.moc"
