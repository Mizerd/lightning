// Where a window opens. Two rules, one test each:
//
//  1. centring uses the screen the window opens on, not the whole virtual
//     desktop (which aims two monitors at the seam between them);
//  2. an unreachable rect is refused.
//
// Behavioural: they ask where a window would go. The offscreen platform gives
// one screen, which is enough for "inside its own screen" and the
// reachability rule.

#include "app/AppController.h"

#include <QGuiApplication>
#include <QRect>
#include <QScreen>
#include <QtTest/QtTest>

class WindowPlacementTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // A fresh window lands wholly inside the screen it opens on. The old
    // code could not satisfy this with more than one monitor attached: it
    // centred a 1100px window inside the width of every screen COMBINED.
    void aCentredWindowLandsInsideItsOwnScreen()
    {
        const QScreen *screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        const QRect available = screen->availableGeometry();
        QVERIFY(!available.isEmpty());

        // Sized to FIT the screen under test: an oversized window
        // legitimately overflows, and that case is pinned separately below.
        const QSize want(available.width() / 2, available.height() / 2);
        const QRect placed =
            AppController::centredWindowRect(want.width(), want.height());
        QVERIFY(!placed.isEmpty());
        QCOMPARE(placed.size(), want);
        QVERIFY2(available.contains(placed),
                 qPrintable(QStringLiteral("%1,%2 %3x%4 not inside %5,%6 %7x%8")
                                .arg(placed.x()).arg(placed.y())
                                .arg(placed.width()).arg(placed.height())
                                .arg(available.x()).arg(available.y())
                                .arg(available.width())
                                .arg(available.height())));

        // ...and actually centred in it, not merely inside.
        QCOMPARE(placed.center().x(), available.center().x());
    }

    // A rect nobody could reach is refused, and the caller then leaves
    // placement to the window manager.
    void anUnreachableRectIsRefused()
    {
        const QScreen *screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        const QRect available = screen->availableGeometry();

        // Far past the right edge of every screen — the shape of the real
        // defect, which put the frame 90px beyond the desktop.
        const QRect beyondRight(available.right() + 4000, available.y(),
                                1100, 720);
        QVERIFY(!AppController::windowGeometryIsReachable(beyondRight));

        // Above the top edge: the title bar is what the user has to grab, so
        // a window whose frame starts above every screen is unreachable even
        // though its body would overlap.
        const QRect aboveTop(available.x(), available.top() - 4000, 1100, 720);
        QVERIFY(!AppController::windowGeometryIsReachable(aboveTop));

        QVERIFY(!AppController::windowGeometryIsReachable(QRect()));
    }

    // The guard must not refuse a window whose grab band meets a screen even
    // if the rest hangs off, which is how a window spanning two monitors
    // looks.
    void aReachableWindowIsAccepted()
    {
        const QScreen *screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        const QRect available = screen->availableGeometry();

        QVERIFY(AppController::windowGeometryIsReachable(
            QRect(available.x() + 10, available.y() + 10, 800, 600)));

        // Hanging off the bottom: only the top band has to be reachable.
        QVERIFY(AppController::windowGeometryIsReachable(
            QRect(available.x() + 10, available.bottom() - 40, 800, 600)));
    }

    // A window bigger than the screen still gets a usable answer rather than
    // a negative-origin rect nobody can drag: the grab band has to remain on
    // a screen, so an oversized window is either placed reachably or refused.
    void anOversizedWindowIsNeverPlacedOutOfReach()
    {
        const QScreen *screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        const QRect available = screen->availableGeometry();

        const QRect placed = AppController::centredWindowRect(
            available.width() * 2, available.height() * 2);
        if (!placed.isEmpty())
            QVERIFY(AppController::windowGeometryIsReachable(placed));
    }

    void aDegenerateSizeIsRefusedRatherThanGuessed()
    {
        QVERIFY(AppController::centredWindowRect(0, 720).isEmpty());
        QVERIFY(AppController::centredWindowRect(1100, 0).isEmpty());
        QVERIFY(AppController::centredWindowRect(-5, -5).isEmpty());
    }
};

QTEST_MAIN(WindowPlacementTest)
#include "WindowPlacementTest.moc"
