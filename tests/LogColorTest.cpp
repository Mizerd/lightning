#include <QtTest>

#include "app/LogColor.h"

class LogColorTest : public QObject
{
    Q_OBJECT

private slots:
    void onlyAPlainTerminalGetsColour()
    {
        QVERIFY(lightning::logColorWanted(true, {}, "xterm", {}));
        QVERIFY(!lightning::logColorWanted(false, {}, "xterm", {}));
        QVERIFY(!lightning::logColorWanted(true, "1", "xterm", {}));
        QVERIFY(!lightning::logColorWanted(true, {}, "dumb", {}));
        QVERIFY(!lightning::logColorWanted(true, {}, "xterm", "%{message}"));
    }

    void levelsReadDifferently()
    {
        const QtMsgType types[] = {QtDebugMsg, QtInfoMsg, QtWarningMsg,
                                   QtCriticalMsg, QtFatalMsg};
        QSet<QByteArray> seen;
        for (QtMsgType type : types)
            seen.insert(QByteArray(lightning::logColorFor(type)));
        QCOMPARE(seen.size(), 5);
    }
};

QTEST_APPLESS_MAIN(LogColorTest)
#include "LogColorTest.moc"
