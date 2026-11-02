
#include <QAction>
#include <QCoreApplication>
#include <QEvent>
#include <QFile>
#include <QImage>
#include <QMenu>
#include <QPixmap>
#include <QSignalSpy>
#include <QSystemTrayIcon>
#include <QTest>

#include "app/CloseBehavior.h"
#include "app/TrayIcon.h"

class TrayMenuTest : public QObject
{
    Q_OBJECT

private slots:
    void theTrayMenuShowsTheWindowAndQuits()
    {
        TrayIcon tray;
        QMenu *menu = tray.contextMenu();
        QVERIFY(menu);
        QCOMPARE(tray.contextMenu(), menu);
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

    void theTooltipSaysUnreadInTheDotOnlyState()
    {
        const QString dotOnly = TrayIcon::tooltipText(QString(), 0, true);
        QVERIFY2(dotOnly.contains(QLatin1Char('\n')), qPrintable(dotOnly));
        QVERIFY(dotOnly.contains(QStringLiteral("Unread messages")));
        QCOMPARE(TrayIcon::tooltipText(QString(), 0, false),
                 QStringLiteral("Lightning"));
        QVERIFY(TrayIcon::tooltipText(QStringLiteral("@a:b"), 3, true)
                    .contains(QStringLiteral("3")));
    }

    void aLanguageChangeRetranslatesTheCachedMenu()
    {
        TrayIcon tray;
        QMenu *menu = tray.contextMenu();
        QList<QAction *> acts;
        for (QAction *a : menu->actions())
            if (!a->isSeparator())
                acts << a;
        QCOMPARE(acts.size(), 2);
        acts[0]->setText(QStringLiteral("stale"));
        acts[1]->setText(QStringLiteral("stale"));
        QEvent change(QEvent::LanguageChange);
        QCoreApplication::sendEvent(qApp, &change);
        QCOMPARE(acts[0]->text(), QStringLiteral("Show Lightning"));
        QCOMPARE(acts[1]->text(), QStringLiteral("Quit Lightning"));
    }

    void aMacControlClickIsASecondaryClick()
    {
        QCOMPARE(TrayIcon::effectiveActivationReason(QSystemTrayIcon::Trigger, true, true),
                 int(QSystemTrayIcon::Context));
        QCOMPARE(TrayIcon::effectiveActivationReason(QSystemTrayIcon::Trigger, true, false),
                 int(QSystemTrayIcon::Trigger));
        QCOMPARE(TrayIcon::effectiveActivationReason(QSystemTrayIcon::Trigger, false, true),
                 int(QSystemTrayIcon::Trigger));
        QCOMPARE(TrayIcon::effectiveActivationReason(QSystemTrayIcon::DoubleClick, true, true),
                 int(QSystemTrayIcon::DoubleClick));
    }

    void macOSBalloonsAreNeverTimedOut()
    {
        QCOMPARE(TrayIcon::balloonTimeoutMs(true), 0);
        QVERIFY(TrayIcon::balloonTimeoutMs(false) > 0);
    }

    void aRightClickOpensTheMenuWithoutRaisingTheWindow()
    {
        QVERIFY(!TrayIcon::activationShowsWindow(QSystemTrayIcon::Context));
        QVERIFY(TrayIcon::activationShowsWindow(QSystemTrayIcon::Trigger));
        QVERIFY(TrayIcon::activationShowsWindow(QSystemTrayIcon::DoubleClick));
        QVERIFY(TrayIcon::activationShowsWindow(QSystemTrayIcon::MiddleClick));
    }

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
        const qreal expectedArea = 3.14159265 * diameter * diameter / 4.0;
        QVERIFY2(qAbs(red - expectedArea) / expectedArea < 0.06,
                 qPrintable(QStringLiteral("red area %1 expected %2")
                                .arg(red).arg(expectedArea)));
    }

    void aWindowsBalloonCarriesNoSoundUnlessThePlatformSoundWasAskedFor()
    {
        constexpr unsigned niifInfo = 0x01, niifUser = 0x04,
                           niifNoSound = 0x10, niifLargeIcon = 0x20;
        QCOMPARE(TrayIcon::kBalloonInfo, niifInfo);
        QCOMPARE(TrayIcon::kBalloonUser, niifUser);
        QCOMPARE(TrayIcon::kBalloonNoSound, niifNoSound);
        QCOMPARE(TrayIcon::kBalloonLargeIcon, niifLargeIcon);

        QCOMPARE(TrayIcon::balloonInfoFlags(false,
false),
                 niifInfo | niifNoSound);
        QCOMPARE(TrayIcon::balloonInfoFlags(true, false),
                 niifUser | niifLargeIcon | niifNoSound);
        QCOMPARE(TrayIcon::balloonInfoFlags(false, true), niifInfo);
        QCOMPARE(TrayIcon::balloonInfoFlags(true, true),
                 niifUser | niifLargeIcon);
    }

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

    void noIconMeansNoBalloonWhateverTheSound()
    {
        TrayIcon tray;
        QVERIFY(!tray.showMessage(QStringLiteral("t"), QStringLiteral("b"),
                                  QImage(), false));
        QVERIFY(!tray.showMessage(QStringLiteral("t"), QStringLiteral("b"),
                                  QImage(), true));
    }

    void macOsTrayMenuIsOursNotTheStatusItems()
    {
        using closebehavior::trayUsesNativeMenu;
        QVERIFY(!trayUsesNativeMenu(true));
        QVERIFY(trayUsesNativeMenu(false));
        QVERIFY(TrayIcon::activationOpensOwnMenu(QSystemTrayIcon::Context, false));
        QVERIFY(!TrayIcon::activationOpensOwnMenu(QSystemTrayIcon::Context, true));
        QVERIFY(!TrayIcon::activationOpensOwnMenu(QSystemTrayIcon::Trigger, false));
        QVERIFY(TrayIcon::activationShowsWindow(QSystemTrayIcon::Trigger));
    }

    void closeHidesWindowFollowsThePlatformConvention()
    {
        using closebehavior::closeHidesWindow;
        QVERIFY(closeHidesWindow(true, false, false, false));
        QVERIFY(closeHidesWindow(true, true, true, false));
        QVERIFY(!closeHidesWindow(true, true, true, true));
        QVERIFY(!closeHidesWindow(false, false, true, false));
        QVERIFY(!closeHidesWindow(false, true, false, false));
        QVERIFY(closeHidesWindow(false, true, true, false));
        QVERIFY(!closeHidesWindow(false, true, true, true));
    }

    void dockActivationRestoresOnlyAWindowThatACloseHid()
    {
        using closebehavior::activationRestoresWindow;
        QVERIFY(activationRestoresWindow(true, true, true));
        QVERIFY(!activationRestoresWindow(true, false, true));
        QVERIFY(!activationRestoresWindow(true, true, false));
        QVERIFY(!activationRestoresWindow(false, true, true));
    }

    void theMacCloseContractIsInSourceAndGuardedToMacOs()
    {
        auto read = [](const QString &relative) {
            QFile file(QStringLiteral(REPO_ROOT "/") + relative);
            return file.open(QIODevice::ReadOnly | QIODevice::Text)
                       ? QString::fromUtf8(file.readAll())
                       : QString();
        };
        const QString main = read(QStringLiteral("qml/Main.qml"));
        QVERIFY(!main.isEmpty());
        const int shortcut = main.indexOf(QStringLiteral("macCloseWindowShortcut"));
        QVERIFY2(shortcut >= 0, "Cmd+W must close (hide) the window on macOS");
        const QString block = main.mid(shortcut, 400);
        QVERIFY(block.contains(QStringLiteral("StandardKey.Close")));
        QVERIFY(block.contains(QStringLiteral("Qt.WindowShortcut")));
        QVERIFY(block.contains(QStringLiteral("Qt.platform.os === \"osx\"")));
        QVERIFY(block.contains(QStringLiteral("window.close()")));

        const QString controller = read(QStringLiteral("src/app/AppController.cpp"));
        QVERIFY(!controller.isEmpty());
        const int quitOnLast = controller.indexOf(
            QStringLiteral("QGuiApplication::setQuitOnLastWindowClosed(false)"));
        QVERIFY2(quitOnLast >= 0, "a hidden last window must not quit on macOS");
        const int guardOpen = controller.lastIndexOf(
            QStringLiteral("#ifdef Q_OS_MACOS"), quitOnLast);
        const int guardClose = controller.indexOf(QStringLiteral("#endif"), guardOpen);
        QVERIFY(guardOpen >= 0 && guardClose > quitOnLast);
        QCOMPARE(controller.count(QStringLiteral("setQuitOnLastWindowClosed")), 1);
    }
};

QTEST_MAIN(TrayMenuTest)
#include "TrayMenuTest.moc"
