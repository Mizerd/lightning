#include <QtTest>

#include "app/StartupChecks.h"

using lightning::startup::shouldRejectForNoDisplay;

// The DISPLAY / WAYLAND_DISPLAY preflight must fire ONLY on X11/Wayland
// platforms (Unix, excluding macOS) and only when no display is reachable and
// the caller has not forced a QPA platform. On Windows and macOS the native
// plugin needs neither variable, so startup must never be rejected there — that
// was the bug that made a normal double-click on Windows exit with
// "no graphical display available".
class StartupChecksTest : public QObject
{
    Q_OBJECT
private slots:
    // --- Unix-like, non-macOS (X11/Wayland) ---------------------------------
    void linuxNoDisplayNoForce_rejected()
    {
        // Linux, DISPLAY & WAYLAND_DISPLAY unset, QT_QPA_PLATFORM unset.
        QVERIFY(shouldRejectForNoDisplay(/*requires*/ true,
                                         /*hasDisplay*/ false,
                                         /*platformForced*/ false));
    }

    void linuxWithX11Display_accepted()
    {
        // Linux, DISPLAY present.
        QVERIFY(!shouldRejectForNoDisplay(true, true, false));
    }

    void linuxWithWaylandDisplay_accepted()
    {
        // hasDisplay already folds DISPLAY || WAYLAND_DISPLAY, so a Wayland
        // session is represented the same way.
        QVERIFY(!shouldRejectForNoDisplay(true, true, false));
    }

    void linuxOffscreenForced_accepted()
    {
        // Linux headless smoke test: QT_QPA_PLATFORM=offscreen, no display.
        QVERIFY(!shouldRejectForNoDisplay(true, false, true));
    }

    // --- Windows / macOS (native plugin, no display server concept) ---------
    void windowsNoDisplay_accepted()
    {
        // Windows: DISPLAY / WAYLAND_DISPLAY absent (the normal case) must be
        // accepted — this is the regression the guard fixes.
        QVERIFY(!shouldRejectForNoDisplay(/*requires*/ false, false, false));
    }

    void windowsWithForcedPlatform_accepted()
    {
        // e.g. QT_QPA_PLATFORM=windows explicitly set — still accepted.
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
        // Whatever the env, a platform that does not require a display server
        // (Windows/macOS) must never be rejected.
        QVERIFY(!shouldRejectForNoDisplay(false, hasDisplay, platformForced));
    }
};

QTEST_APPLESS_MAIN(StartupChecksTest)
#include "StartupChecksTest.moc"
