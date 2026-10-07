// The tray icon's right-click menu (GitHub #17). With close-to-tray on, the icon
// had no menu and every click only raised the window, so a user whose window was
// closed to the tray had no way to quit from it. A QMenu is a widget, hence a
// QApplication (QTEST_MAIN) rather than composer-spell-test's guiless main.

#include <QAction>
#include <QImage>
#include <QMenu>
#include <QPixmap>
#include <QSignalSpy>
#include <QSystemTrayIcon>
#include <QTest>

#include "app/TrayIcon.h"

class TrayMenuTest : public QObject
{
    Q_OBJECT

private slots:
    // GitHub #17: with close-to-tray on, the tray icon had no menu and every
    // click only raised the window, so there was no way to quit from it.
    void theTrayMenuShowsTheWindowAndQuits()
    {
        TrayIcon tray;
        QMenu *menu = tray.contextMenu();
        QVERIFY(menu);
        QCOMPARE(tray.contextMenu(), menu); // one menu, built once
        QAction *show = nullptr;
        QAction *quit = nullptr;
        for (QAction *action : menu->actions()) {
            if (action->text() == QStringLiteral("Show Lightning"))
                show = action;
            else if (action->text() == QStringLiteral("Quit Lightning"))
                quit = action;
        }
        QVERIFY2(show && quit, "the tray menu offers Show and Quit");
        QSignalSpy shown(&tray, &TrayIcon::showRequested);
        QSignalSpy quitting(&tray, &TrayIcon::quitRequested);
        quit->trigger();
        QCOMPARE(quitting.count(), 1);
        QCOMPARE(shown.count(), 0);
        show->trigger();
        QCOMPARE(shown.count(), 1);
        QCOMPARE(quitting.count(), 1);
    }

    // The right click opens the menu; it must not also raise the window
    // (Windows reports the Context activation alongside the menu).
    void aRightClickOpensTheMenuWithoutRaisingTheWindow()
    {
        QVERIFY(!TrayIcon::activationShowsWindow(QSystemTrayIcon::Context));
        QVERIFY(TrayIcon::activationShowsWindow(QSystemTrayIcon::Trigger));
        QVERIFY(TrayIcon::activationShowsWindow(QSystemTrayIcon::DoubleClick));
        QVERIFY(TrayIcon::activationShowsWindow(QSystemTrayIcon::MiddleClick));
    }

    // GitHub #23: at an interface zoom != 100% QIcon::pixmap() hands back
    // size*dpr physical pixels with devicePixelRatio set, and the badge was
    // laid out from the PHYSICAL size inside a painter that scales by the
    // ratio again, so the disc landed past the corner and was clipped. The
    // red disc must be the same circle at every ratio, inside the pixmap.
    void theBadgeDiscStaysInsideThePixmapAtAnyDevicePixelRatio_data()
    {
        QTest::addColumn<qreal>("dpr");
        QTest::newRow("1.0") << 1.0;
        QTest::newRow("1.1") << 1.1;
        QTest::newRow("1.25") << 1.25;
        QTest::newRow("2.0") << 2.0;
    }
    void theBadgeDiscStaysInsideThePixmapAtAnyDevicePixelRatio()
    {
        QFETCH(qreal, dpr);
        const int logicalSide = 128;
        const int physical = qRound(logicalSide * dpr);
        QPixmap base(physical, physical);
        base.setDevicePixelRatio(dpr);
        base.fill(Qt::white);
        const QPixmap out = TrayIcon::badged(base, QStringLiteral("\u2022"));
        QCOMPARE(out.size(), base.size());
        QCOMPARE(out.devicePixelRatio(), dpr);

        const QImage image = out.toImage();
        int minX = image.width(), minY = image.height(), maxX = -1, maxY = -1;
        qint64 red = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor c = image.pixelColor(x, y);
                if (c.red() > 200 && c.green() < 100 && c.blue() < 100) {
                    ++red;
                    minX = qMin(minX, x);
                    minY = qMin(minY, y);
                    maxX = qMax(maxX, x);
                    maxY = qMax(maxY, y);
                }
            }
        }
        QVERIFY2(red > 0, "a red disc is drawn");
        // dot diameter is 0.42 of the logical side; expect it tangent to the
        // bottom-right corner, in physical pixels.
        const qreal diameter = logicalSide * 0.42 * dpr;
        const qreal edge = physical;
        QVERIFY2(qAbs(minX - (edge - diameter)) <= 2.0,
                 qPrintable(QStringLiteral("left %1 expected %2")
                                .arg(minX).arg(edge - diameter)));
        QVERIFY2(qAbs(minY - (edge - diameter)) <= 2.0,
                 qPrintable(QStringLiteral("top %1 expected %2")
                                .arg(minY).arg(edge - diameter)));
        QVERIFY2(edge - 1 - maxX <= 2.0 && edge - 1 - maxY <= 2.0,
                 "the disc reaches the corner");
        // Not clipped: the whole disc area is there.
        const qreal expectedArea = 3.14159265 * diameter * diameter / 4.0;
        QVERIFY2(qAbs(red - expectedArea) / expectedArea < 0.06,
                 qPrintable(QStringLiteral("red area %1 expected %2")
                                .arg(red).arg(expectedArea)));
    }

    // Windows, 2026-10-07: every balloon also played Windows' "Notify System
    // Generic", because Qt 6.11.2's QWindowsSystemTrayIcon::showMessage sets
    // dwInfoFlags to NIIF_INFO or NIIF_USER | NIIF_LARGE_ICON and never
    // NIIF_NOSOUND. The values are shellapi.h's; TrayIcon.cpp static_asserts
    // the mirrored constants against the SDK on Windows.
    void aWindowsBalloonCarriesNoSoundUnlessThePlatformSoundWasAskedFor()
    {
        constexpr unsigned niifInfo = 0x01, niifUser = 0x04,
                           niifNoSound = 0x10, niifLargeIcon = 0x20;
        QCOMPARE(TrayIcon::kBalloonInfo, niifInfo);
        QCOMPARE(TrayIcon::kBalloonUser, niifUser);
        QCOMPARE(TrayIcon::kBalloonNoSound, niifNoSound);
        QCOMPARE(TrayIcon::kBalloonLargeIcon, niifLargeIcon);

        // Lightning's chime or silence: Windows must add nothing.
        QCOMPARE(TrayIcon::balloonInfoFlags(/*hasImage=*/false,
                                            /*platformSound=*/false),
                 niifInfo | niifNoSound);
        QCOMPARE(TrayIcon::balloonInfoFlags(true, false),
                 niifUser | niifLargeIcon | niifNoSound);
        // System default chosen: exactly Qt's own composition, so the icon
        // and size the balloon showed before are unchanged.
        QCOMPARE(TrayIcon::balloonInfoFlags(false, true), niifInfo);
        QCOMPARE(TrayIcon::balloonInfoFlags(true, true),
                 niifUser | niifLargeIcon);
    }

    // The balloon image is what Qt's icon.actualSize(QSize(256, 256)) gave:
    // scaled down to fit, aspect kept, never scaled up.
    void theBalloonImageFitsQtsBoundWithoutUpscaling()
    {
        QVERIFY(TrayIcon::balloonImage(QImage()).isNull());
        QImage small(64, 48, QImage::Format_ARGB32_Premultiplied);
        small.fill(Qt::red);
        QCOMPARE(TrayIcon::balloonImage(small).size(), QSize(64, 48));
        QImage wide(1024, 512, QImage::Format_ARGB32_Premultiplied);
        wide.fill(Qt::blue);
        QCOMPARE(TrayIcon::balloonImage(wide).size(), QSize(256, 128));
        QImage tall(300, 600, QImage::Format_ARGB32_Premultiplied);
        tall.fill(Qt::green);
        QCOMPARE(TrayIcon::balloonImage(tall).size(), QSize(128, 256));
    }

    // Without an icon there is no balloon, and the caller falls back to its
    // own log line; the sound argument changes nothing about that.
    void noIconMeansNoBalloonWhateverTheSound()
    {
        TrayIcon tray;
        QVERIFY(!tray.showMessage(QStringLiteral("t"), QStringLiteral("b"),
                                  QImage(), false));
        QVERIFY(!tray.showMessage(QStringLiteral("t"), QStringLiteral("b"),
                                  QImage(), true));
    }
};

QTEST_MAIN(TrayMenuTest)
#include "TrayMenuTest.moc"
