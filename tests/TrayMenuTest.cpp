// The tray icon's right-click menu (GitHub #17). With close-to-tray on, the icon
// had no menu and every click only raised the window, so a user whose window was
// closed to the tray had no way to quit from it. A QMenu is a widget, hence a
// QApplication (QTEST_MAIN) rather than composer-spell-test's guiless main.

#include <QAction>
#include <QMenu>
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
};

QTEST_MAIN(TrayMenuTest)
#include "TrayMenuTest.moc"
