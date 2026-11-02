#include <QtTest>

#include "app/StartupChecks.h"

using lightning::startup::shouldRejectForNoDisplay;

class StartupChecksTest : public QObject
{
    Q_OBJECT
private slots:
    void linuxNoDisplayNoForce_rejected()
    {
        QVERIFY(shouldRejectForNoDisplay( true,
 false,
 false));
    }

    void linuxWithX11Display_accepted()
    {
        QVERIFY(!shouldRejectForNoDisplay(true, true, false));
    }

    void linuxWithWaylandDisplay_accepted()
    {
        QVERIFY(!shouldRejectForNoDisplay(true, true, false));
    }

    void linuxOffscreenForced_accepted()
    {
        QVERIFY(!shouldRejectForNoDisplay(true, false, true));
    }

    void windowsNoDisplay_accepted()
    {
        QVERIFY(!shouldRejectForNoDisplay( false, false, false));
    }

    void windowsWithForcedPlatform_accepted()
    {
        QVERIFY(!shouldRejectForNoDisplay(false, false, true));
    }

    void nonDisplayPlatformNeverRejects_data()
    {
        QTest::addColumn<bool>("hasDisplay");
        QTest::addColumn<bool>("platformForced");
        QTest::newRow("no-display/no-force")   << false << false;
        QTest::newRow("display/no-force")      << true  << false;
        QTest::newRow("no-display/forced")     << false << true;
        QTest::newRow("display/forced")        << true  << true;
    }

    void nonDisplayPlatformNeverRejects()
    {
        QFETCH(bool, hasDisplay);
        QFETCH(bool, platformForced);
        QVERIFY(!shouldRejectForNoDisplay(false, hasDisplay, platformForced));
    }
};

QTEST_APPLESS_MAIN(StartupChecksTest)
#include "StartupChecksTest.moc"
