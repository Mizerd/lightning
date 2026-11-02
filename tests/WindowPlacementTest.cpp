
#include "app/AppController.h"

#include <QGuiApplication>
#include <QRect>
#include <QScreen>
#include <QtTest/QtTest>

class WindowPlacementTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aCentredWindowLandsInsideItsOwnScreen()
    {
        const QScreen *screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        const QRect available = screen->availableGeometry();
        QVERIFY(!available.isEmpty());

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

        QCOMPARE(placed.center().x(), available.center().x());
    }

    void anUnreachableRectIsRefused()
    {
        const QScreen *screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        const QRect available = screen->availableGeometry();

        const QRect beyondRight(available.right() + 4000, available.y(),
                                1100, 720);
        QVERIFY(!AppController::windowGeometryIsReachable(beyondRight));

        const QRect aboveTop(available.x(), available.top() - 4000, 1100, 720);
        QVERIFY(!AppController::windowGeometryIsReachable(aboveTop));

        QVERIFY(!AppController::windowGeometryIsReachable(QRect()));
    }

    void aReachableWindowIsAccepted()
    {
        const QScreen *screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        const QRect available = screen->availableGeometry();

        QVERIFY(AppController::windowGeometryIsReachable(
            QRect(available.x() + 10, available.y() + 10, 800, 600)));

        QVERIFY(AppController::windowGeometryIsReachable(
            QRect(available.x() + 10, available.bottom() - 40, 800, 600)));
    }

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

    void aRestoredRectIsFittedToItsScreen()
    {
        const QRect zoomed(0, 0, 1185, 741);
        QCOMPARE(AppController::fitWindowGeometry(QRect(50, 25, 1500, 950),
                                                  {zoomed}),
                 zoomed);

        const QRect fits(50, 25, 900, 600);
        QCOMPARE(AppController::fitWindowGeometry(fits, {zoomed}), fits);

        QCOMPARE(AppController::fitWindowGeometry(QRect(600, 400, 900, 600),
                                                  {zoomed}),
                 QRect(285, 141, 900, 600));

        const QRect tiny(0, 0, 600, 400);
        QCOMPARE(AppController::fitWindowGeometry(QRect(10, 10, 1100, 720),
                                                  {tiny}),
                 QRect(0, 0, 640, 420));

        const QRect left(0, 0, 1920, 1080);
        const QRect right(1920, 0, 1280, 800);
        QCOMPARE(AppController::fitWindowGeometry(QRect(2000, 100, 1500, 950),
                                                  {left, right}),
                 QRect(1920, 0, 1280, 800));

        const QRect away(9000, 9000, 800, 600);
        QCOMPARE(AppController::fitWindowGeometry(away, {zoomed}), away);
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
