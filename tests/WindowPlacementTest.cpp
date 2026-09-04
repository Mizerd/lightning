// Where a window OPENS, which is a question this application got wrong in a
// way nobody could report as a bug: the window opened off the edge of the
// desktop and simply was not there.
//
// Measured on the maintainer's two-monitor layout before the fix:
//
//   window placement "centred" applied=[6490,360 1100x720]
//   screens="DP-3[3840,0 2560x1440] DP-1[0,0 2560x1440]"
//
// with the virtual desktop ending at 6400. Two causes, and the tests below
// pin one each:
//
//  1. the centring divided the whole VIRTUAL DESKTOP, not the screen the
//     window opens on, so two monitors aimed it at the seam between them;
//  2. nothing checked the answer, so an unreachable rect was used anyway.
//
// These are deliberately BEHAVIOURAL — they ask where a window would go, not
// whether some symbol exists. The offscreen platform gives one screen, which
// is enough to pin "inside its own screen" and "an unreachable rect is
// refused"; the two-monitor arithmetic is pinned by the reachability rule,
// which is what actually made the bad rect unusable.

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

    // The guard that makes the whole thing safe: a rect nobody could reach is
    // refused, and the caller then leaves placement to the window manager.
    // Without this, arithmetic that goes wrong on a layout the developer does
    // not have opens the window where it cannot be seen.
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

    // The legitimate cases the guard must NOT refuse, because refusing them
    // would be a regression of its own: a window whose grab band meets a
    // screen is reachable even when the rest of it hangs off, and that is how
    // a window spanned across two monitors looks.
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
