
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

    void bothListsHaveAMiddleClickScroller()
    {
        QCOMPARE(m_src.count(QStringLiteral("MiddleClickScroller {")), 2);
        QVERIFY(m_src.contains(QStringLiteral("view: resultList")));
        QVERIFY(m_src.contains(QStringLiteral("view: commandList")));
    }

    void selectionChangeScrollsIntoView()
    {
        QCOMPARE(m_src.count(QStringLiteral("positionViewAtIndex(currentIndex, ListView.Contain)")), 2);
        QCOMPARE(m_src.count(QStringLiteral("onCurrentIndexChanged:")), 2);
    }

    void pagingKeysMoveTheSelection()
    {
        QVERIFY(m_src.contains(QStringLiteral("Qt.Key_PageDown")));
        QVERIFY(m_src.contains(QStringLiteral("Qt.Key_PageUp")));
        QVERIFY(m_src.contains(QStringLiteral("Qt.ControlModifier")));
    }

    void enterWithNoMatchUsesTheFallbacks()
    {
        QVERIFY(m_src.contains(QStringLiteral("function acceptCurrent()")));
        QVERIFY(m_src.contains(QStringLiteral("searchMessagesButton.clicked()")));
        QVERIFY(m_src.contains(QStringLiteral("exploreRoomsButton.clicked()")));
        QCOMPARE(m_src.count(QStringLiteral("switcher.acceptCurrent()")), 2);
    }

    void tooltipAndHonestFooter()
    {
        QVERIFY(!m_src.contains(QStringLiteral("ToolTip.text")));
        QCOMPARE(m_src.count(QStringLiteral("PlainTip {")), 2);
        const int comp = m_src.indexOf(QStringLiteral("component PlainTip: ToolTip"));
        QVERIFY(comp > 0);
        const QString body = m_src.mid(comp, 1100);
        QVERIFY(body.contains(QStringLiteral("textFormat: Text.PlainText")));
        QVERIFY(body.contains(QStringLiteral("contentItem: Label")));
        QVERIFY(body.contains(QStringLiteral("Timer {")));
        QVERIFY(!body.contains(QStringLiteral("visible: shown")));
        QVERIFY(!body.contains(QStringLiteral("delay: 600")));
        QVERIFY(m_src.contains(QStringLiteral("hasOtherAccount")));
        QVERIFY(m_src.contains(QStringLiteral("height: switcher.resultRowHeight")));
        QVERIFY(m_src.contains(QStringLiteral("height: switcher.commandRowHeight")));
    }
};

QTEST_GUILESS_MAIN(QuickSwitcherKeyboardSourceTest)
#include "QuickSwitcherKeyboardSourceTest.moc"
